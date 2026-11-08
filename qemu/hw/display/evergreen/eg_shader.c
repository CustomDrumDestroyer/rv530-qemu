/*
 * Evergreen shader core: an interpreter for the CF / ALU (VLIW5) / fetch
 * clauses of the Evergreen ISA, run over up to four lanes (a 2x2 pixel
 * quad or a few vertices) at a time.
 *
 * Encodings: Mesa src/gallium/drivers/r600/eg_sq.h and r600_isa.c (the
 * Evergreen column of the opcode tables); semantics: the AMD Evergreen
 * ISA guide and Mesa's sfn back end.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "eg_gfx.h"
#include <math.h>

/* ------------------------------------------------------------------ */
/* Programs                                                            */
/* ------------------------------------------------------------------ */

#define PGM_CHUNK   1024                /* dwords fetched at a time */
#define PGM_MAX     (64 * 1024)

bool eg_program_load(EvergreenState *s, EgProgram *p, uint64_t addr, int stage)
{
    memset(p, 0, sizeof(*p));
    p->addr = addr;
    p->stage = stage;
    p->code = g_new0(uint32_t, PGM_CHUNK);
    p->ndw = PGM_CHUNK;
    return eg_gpu_read(s, addr, p->code, PGM_CHUNK * 4);
}

void eg_program_free(EgProgram *p)
{
    g_free(p->code);
    p->code = NULL;
    if (p->groups) {
        g_hash_table_destroy(p->groups);
        p->groups = NULL;
    }
}

/* make dwords [dw, dw + n) of the program available */
static const uint32_t *pgm(EgShader *sh, const EgProgram *cp, uint32_t dw, uint32_t n)
{
    EgProgram *p = (EgProgram *)cp;

    if (dw + n > p->ndw) {
        uint32_t want = ROUND_UP(dw + n, PGM_CHUNK);
        if (want > PGM_MAX) {
            return NULL;
        }
        p->code = g_renew(uint32_t, p->code, want);
        eg_gpu_read(sh->s, p->addr + p->ndw * 4, p->code + p->ndw, (want - p->ndw) * 4);
        p->ndw = want;
    }
    return p->code + dw;
}

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static inline float u2f(uint32_t u)
{
    float f;
    memcpy(&f, &u, 4);
    return f;
}

static inline uint32_t f2u(float f)
{
    uint32_t u;
    memcpy(&u, &f, 4);
    return u;
}

static float half_to_float(uint16_t h)
{
    uint32_t s = (h >> 15) & 1, e = (h >> 10) & 0x1f, m = h & 0x3ff;
    float v;

    if (e == 0) {
        v = ldexpf((float)m, -24);
    } else if (e == 31) {
        v = m ? NAN : INFINITY;
    } else {
        v = ldexpf((float)(m | 0x400), (int)e - 25);
    }
    return s ? -v : v;
}

static uint16_t float_to_half(float f)
{
    uint32_t u = f2u(f), s = (u >> 16) & 0x8000;
    int e = (int)((u >> 23) & 0xff) - 127 + 15;
    uint32_t m = u & 0x7fffff;

    if (isnan(f)) {
        return 0x7e00;
    }
    if (e >= 31) {
        return s | 0x7c00;
    }
    if (e <= 0) {
        if (e < -10) {
            return s;
        }
        m = (m | 0x800000) >> (1 - e);
        return s | ((m + 0x1000) >> 13);
    }
    return s | (e << 10) | ((m + 0x1000) >> 13);
}

/* DX9 rules: 0 * anything = 0 */
static inline float mul_dx9(float a, float b)
{
    return (a == 0.0f || b == 0.0f) ? 0.0f : a * b;
}

static inline int32_t f2i_sat(double v)
{
    if (isnan(v)) {
        return 0;
    }
    if (v >= 2147483647.0) {
        return INT32_MAX;
    }
    if (v <= -2147483648.0) {
        return INT32_MIN;
    }
    return (int32_t)v;
}

static inline uint32_t f2u_sat(double v)
{
    if (isnan(v) || v <= 0) {
        return 0;
    }
    if (v >= 4294967295.0) {
        return UINT32_MAX;
    }
    return (uint32_t)v;
}

/* ------------------------------------------------------------------ */
/* ALU                                                                 */
/* ------------------------------------------------------------------ */

/* OP2 opcodes (Evergreen) */
enum {
    OP2_ADD = 0x00, OP2_MUL = 0x01, OP2_MUL_IEEE = 0x02, OP2_MAX = 0x03, OP2_MIN = 0x04,
    OP2_MAX_DX10 = 0x05, OP2_MIN_DX10 = 0x06, OP2_SETE = 0x08, OP2_SETGT = 0x09,
    OP2_SETGE = 0x0a, OP2_SETNE = 0x0b, OP2_SETE_DX10 = 0x0c, OP2_SETGT_DX10 = 0x0d,
    OP2_SETGE_DX10 = 0x0e, OP2_SETNE_DX10 = 0x0f, OP2_FRACT = 0x10, OP2_TRUNC = 0x11,
    OP2_CEIL = 0x12, OP2_RNDNE = 0x13, OP2_FLOOR = 0x14, OP2_ASHR_INT = 0x15,
    OP2_LSHR_INT = 0x16, OP2_LSHL_INT = 0x17, OP2_MOV = 0x19, OP2_NOP = 0x1a,
    OP2_PRED_SETGT_UINT = 0x1e, OP2_PRED_SETGE_UINT = 0x1f, OP2_PRED_SETE = 0x20,
    OP2_PRED_SETGT = 0x21, OP2_PRED_SETGE = 0x22, OP2_PRED_SETNE = 0x23,
    OP2_PRED_SET_INV = 0x24, OP2_PRED_SET_POP = 0x25, OP2_PRED_SET_CLR = 0x26,
    OP2_PRED_SET_RESTORE = 0x27, OP2_PRED_SETE_PUSH = 0x28, OP2_PRED_SETGT_PUSH = 0x29,
    OP2_PRED_SETGE_PUSH = 0x2a, OP2_PRED_SETNE_PUSH = 0x2b, OP2_KILLE = 0x2c,
    OP2_KILLGT = 0x2d, OP2_KILLGE = 0x2e, OP2_KILLNE = 0x2f, OP2_AND_INT = 0x30,
    OP2_OR_INT = 0x31, OP2_XOR_INT = 0x32, OP2_NOT_INT = 0x33, OP2_ADD_INT = 0x34,
    OP2_SUB_INT = 0x35, OP2_MAX_INT = 0x36, OP2_MIN_INT = 0x37, OP2_MAX_UINT = 0x38,
    OP2_MIN_UINT = 0x39, OP2_SETE_INT = 0x3a, OP2_SETGT_INT = 0x3b, OP2_SETGE_INT = 0x3c,
    OP2_SETNE_INT = 0x3d, OP2_SETGT_UINT = 0x3e, OP2_SETGE_UINT = 0x3f,
    OP2_KILLGT_UINT = 0x40, OP2_KILLGE_UINT = 0x41, OP2_PRED_SETE_INT = 0x42,
    OP2_PRED_SETGT_INT = 0x43, OP2_PRED_SETGE_INT = 0x44, OP2_PRED_SETNE_INT = 0x45,
    OP2_KILLE_INT = 0x46, OP2_KILLGT_INT = 0x47, OP2_KILLGE_INT = 0x48,
    OP2_KILLNE_INT = 0x49, OP2_PRED_SETE_PUSH_INT = 0x4a, OP2_PRED_SETGT_PUSH_INT = 0x4b,
    OP2_PRED_SETGE_PUSH_INT = 0x4c, OP2_PRED_SETNE_PUSH_INT = 0x4d,
    OP2_PRED_SETLT_PUSH_INT = 0x4e, OP2_PRED_SETLE_PUSH_INT = 0x4f,
    OP2_FLT_TO_INT = 0x50, OP2_BFREV_INT = 0x51, OP2_ADDC_UINT = 0x52,
    OP2_SUBB_UINT = 0x53, OP2_GROUP_BARRIER = 0x54, OP2_SET_MODE = 0x57,
    OP2_SET_CF_IDX0 = 0x58, OP2_SET_CF_IDX1 = 0x59,
    OP2_EXP_IEEE = 0x81, OP2_LOG_CLAMPED = 0x82, OP2_LOG_IEEE = 0x83,
    OP2_RECIP_CLAMPED = 0x84, OP2_RECIP_FF = 0x85, OP2_RECIP_IEEE = 0x86,
    OP2_RECIPSQRT_CLAMPED = 0x87, OP2_RECIPSQRT_FF = 0x88, OP2_RECIPSQRT_IEEE = 0x89,
    OP2_SQRT_IEEE = 0x8a, OP2_SIN = 0x8d, OP2_COS = 0x8e, OP2_MULLO_INT = 0x8f,
    OP2_MULHI_INT = 0x90, OP2_MULLO_UINT = 0x91, OP2_MULHI_UINT = 0x92,
    OP2_RECIP_INT = 0x93, OP2_RECIP_UINT = 0x94, OP2_FLT_TO_UINT = 0x9a,
    OP2_INT_TO_FLT = 0x9b, OP2_UINT_TO_FLT = 0x9c, OP2_BFM_INT = 0xa0,
    OP2_FLT32_TO_FLT16 = 0xa2, OP2_FLT16_TO_FLT32 = 0xa3, OP2_UBYTE0_FLT = 0xa4,
    OP2_UBYTE1_FLT = 0xa5, OP2_UBYTE2_FLT = 0xa6, OP2_UBYTE3_FLT = 0xa7,
    OP2_BCNT_INT = 0xaa, OP2_FFBH_UINT = 0xab, OP2_FFBL_INT = 0xac, OP2_FFBH_INT = 0xad,
    OP2_FLT_TO_UINT4 = 0xae, OP2_DOT_IEEE = 0xaf, OP2_FLT_TO_INT_RPI = 0xb0,
    OP2_FLT_TO_INT_FLOOR = 0xb1, OP2_MULHI_UINT24 = 0xb2, OP2_MUL_UINT24 = 0xb5,
    OP2_DOT4 = 0xbe, OP2_DOT4_IEEE = 0xbf, OP2_CUBE = 0xc0, OP2_MAX4 = 0xc1,
    OP2_MOVA_INT = 0xcc, OP2_INTERP_XY = 0xd6, OP2_INTERP_ZW = 0xd7,
    OP2_INTERP_X = 0xd8, OP2_INTERP_Z = 0xd9, OP2_INTERP_LOAD_P0 = 0xe0,
    OP2_INTERP_LOAD_P10 = 0xe1, OP2_INTERP_LOAD_P20 = 0xe2,
};

/* OP3 opcodes */
enum {
    OP3_BFE_UINT = 0x04, OP3_BFE_INT = 0x05, OP3_BFI_INT = 0x06, OP3_FMA = 0x07,
    OP3_BIT_ALIGN_INT = 0x0c, OP3_BYTE_ALIGN_INT = 0x0d, OP3_MULADD_UINT24 = 0x10,
    OP3_MULADD = 0x14, OP3_MULADD_M2 = 0x15, OP3_MULADD_M4 = 0x16, OP3_MULADD_D2 = 0x17,
    OP3_MULADD_IEEE = 0x18, OP3_CNDE = 0x19, OP3_CNDGT = 0x1a, OP3_CNDGE = 0x1b,
    OP3_CNDE_INT = 0x1c, OP3_CNDGT_INT = 0x1d, OP3_CNDGE_INT = 0x1e, OP3_MUL_LIT = 0x1f,
};

static bool op2_trans_only(uint32_t op)
{
    return (op >= 0x81 && op <= 0x9c);
}

typedef struct AluInst {
    uint32_t w0, w1;
    bool op3;
    uint32_t op;
    int slot;                           /* 0-3 vector x..w, 4 trans */
    int nsrc;
    /* the source operands, decoded once (inst_src runs per lane) */
    uint16_t sel[3];
    uint8_t chan[3], rel[3], im;
    uint32_t neg_mask[3], abs_mask[3];  /* XOR / AND masks for the sign bit */
} AluInst;

/* KC0, KC1, KC2, KC3 (the last two from ALU_EXTENDED): EgShader.kc_cache */
typedef EgVec4 Kcache[4][32];

typedef struct GroupCtx {
    EgVec4 pv[EG_MAX_LANES];
    float ps[EG_MAX_LANES];
    uint32_t ps_u[EG_MAX_LANES];
} GroupCtx;

/*
 * Apple's GL engine sends its constants with SET_ALU_CONST and leaves the
 * constant buffer address at 0 for the CP microcode to fill: a kcache
 * window over that buffer reads the constant file of the stage.
 */
void eg_kcache_cfile(EvergreenState *s, bool ps, uint32_t addr, uint32_t n, void *dst)
{
    EgVec4 *d = dst;
    uint32_t i;

    for (i = 0; i < n; i++) {
        uint32_t c = addr * 16 + i + (ps ? 0 : 256);
        memcpy(&d[i], &s->gfx->alu_const[(c & (EG_NUM_ALU_CONSTS - 1)) * 4], 16);
    }
}

static void load_kcache_set(EgShader *sh, EgVec4 *dst, uint32_t bank, uint32_t mode,
                            uint32_t addr)
{
    EvergreenState *s = sh->s;
    /* the buffers do not change during a draw: reload only on a new key */
    uint32_t key = 0x80000000u | (bank & 15) << 24 | (mode & 3) << 16 | (addr & 0xff) |
                   (sh->stage == EG_STAGE_PS ? 0x100 : 0);
    int slot = (int)(((uint8_t *)dst - (uint8_t *)&sh->kc_cache[0]) / sizeof(sh->kc_cache[0]));

    if (slot >= 0 && slot < 4) {
        if (sh->kc_key[slot] == key) {
            return;
        }
        sh->kc_key[slot] = key;
    }
    uint32_t reg = sh->stage == EG_STAGE_PS ? 0x28940 : 0x28980;   /* ALU_CONST_CACHE_PS/VS_n */
    uint64_t base;
    uint32_t n;

    memset(dst, 0, sizeof(EgVec4) * 32);
    if (!mode) {
        return;
    }
    n = mode == 2 ? 32 : 16;
    if (!CTX(s, reg + 4 * (bank & 15))) {
        eg_kcache_cfile(s, sh->stage == EG_STAGE_PS, addr, n, dst);
        return;
    }
    base = ((uint64_t)CTX(s, reg + 4 * (bank & 15)) << 8) + (uint64_t)addr * 16 * 16;
    eg_gpu_read(s, base, dst, n * 16);
}

static uint32_t read_src(EgShader *sh, int lane, uint32_t sel, uint32_t chan, bool rel,
                         uint32_t index_mode, const uint32_t *lit, const Kcache *kc,
                         const GroupCtx *g)
{
    int32_t idx = 0;

    if (rel) {
        idx = index_mode < 4 ? sh->ar[lane][index_mode] :
              index_mode == 4 ? sh->loop_index : sh->ar[lane][0];
    }
    if (sel < 128) {
        int r = (int)sel + idx;
        return sh->gpr[lane][r & (EG_NUM_GPRS - 1)].u[chan];
    }
    if (sel < 192) {
        int set = sel >= 160, i = (int)(sel - (set ? 160 : 128)) + idx;
        return (*kc)[set][i & 31].u[chan];
    }
    switch (sel) {
    case 0xf8:
        return 0;
    case 0xf9:
        return f2u(1.0f);
    case 0xfa:
        return 1;
    case 0xfb:
        return (uint32_t)-1;
    case 0xfc:
        return f2u(0.5f);
    case 0xfd:
        return lit[chan];
    case 0xfe:
        return g->pv[lane].u[chan];
    case 0xff:
        return g->ps_u[lane];
    }
    if (sel >= 256 && sel < 320) {
        int set = sel >= 288, i = (int)(sel - (set ? 288 : 256)) + idx;
        return (*kc)[2 + set][i & 31].u[chan];
    }
    if (sel >= 448 && sel < 480) {
        return 0;                       /* interpolation params: INTERP_* only */
    }
    if (sel >= 320) {
        /* constant file (R600-style SET_ALU_CONST) */
        int c = (int)(sel - 256) + idx + (sh->stage == EG_STAGE_PS ? 0 : 256);
        return sh->s->gfx->alu_const[(c & (EG_NUM_ALU_CONSTS - 1)) * 4 + chan];
    }
    return 0;
}

/* source operand of instruction slot k (0-2), with neg/abs applied */
static uint32_t inst_src(EgShader *sh, int lane, const AluInst *in, int k,
                         const uint32_t *lit, const Kcache *kc, const GroupCtx *g)
{
    uint32_t sel = in->sel[k], chan = in->chan[k], v;

    /* the common case first: a GPR without relative addressing */
    if (sel < 128 && !in->rel[k]) {
        v = sh->gpr[lane][sel].u[chan];
    } else {
        v = read_src(sh, lane, sel, chan, in->rel[k], in->im, lit, kc, g);
    }
    return (v & in->abs_mask[k]) ^ in->neg_mask[k];
}

static uint32_t src_sel(const AluInst *in, int k)
{
    return k == 0 ? in->w0 & 0x1ff : k == 1 ? (in->w0 >> 13) & 0x1ff : in->w1 & 0x1ff;
}

static float do_cmp(int cc, float a, float b)
{
    switch (cc) {
    case 0: return a == b;
    case 1: return a > b;
    case 2: return a >= b;
    default: return a != b;
    }
}

/*
 * Execute one instruction group for one lane.  res[slot] receives the
 * result bits, did[slot] whether the slot executed.  Side effects on the
 * predicate / exec mask / kill are returned in *pred_out (-1 = none).
 */
typedef struct GroupOut {
    uint32_t res[5];
    bool did[5];
    int pred;                           /* -1 none, else 0/1 */
    bool exec_update, kill;
    int mova;                           /* -1, else AR value set */
} GroupOut;

static void exec_group_lane(EgShader *sh, int lane, const AluInst *ins, int n,
                            const uint32_t *lit, const Kcache *kc, const GroupCtx *g,
                            GroupOut *o)
{
    uint32_t sv[5][3];
    float dot = 0;
    int k, i;

    o->pred = -1;
    o->exec_update = o->kill = false;
    o->mova = INT32_MIN;
    for (i = 0; i < n; i++) {
        for (k = 0; k < ins[i].nsrc; k++) {
            sv[i][k] = inst_src(sh, lane, &ins[i], k, lit, kc, g);
        }
    }
    /* reductions over the four vector slots */
    for (i = 0; i < n; i++) {
        if (!ins[i].op3 && ins[i].slot < 4 &&
            (ins[i].op == OP2_DOT4 || ins[i].op == OP2_DOT4_IEEE || ins[i].op == OP2_MAX4)) {
            float t = ins[i].op == OP2_DOT4 ? mul_dx9(u2f(sv[i][0]), u2f(sv[i][1])) :
                      ins[i].op == OP2_DOT4_IEEE ? u2f(sv[i][0]) * u2f(sv[i][1]) :
                      u2f(sv[i][0]);
            if (ins[i].op == OP2_MAX4) {
                dot = (i == 0) ? t : fmaxf(dot, t);
            } else {
                dot += t;
            }
        }
    }

    for (i = 0; i < n; i++) {
        const AluInst *in = &ins[i];
        uint32_t w0 = in->w0, w1 = in->w1, r = 0, op = in->op;
        uint32_t ps = (w0 >> 29) & 3;
        float a = u2f(sv[i][0]), b = u2f(sv[i][1]), c = u2f(sv[i][2]), f = 0;
        uint32_t ua = sv[i][0], ub = sv[i][1], uc = sv[i][2];
        int32_t ia = (int32_t)ua, ib = (int32_t)ub;
        bool isf = true;
        int slot = in->slot;

        o->did[slot] = false;
        if ((ps == 2 && (sh->pred >> lane) & 1) || (ps == 3 && !((sh->pred >> lane) & 1))) {
            continue;
        }
        o->did[slot] = true;
        if (in->op3) {
            switch (op) {
            case OP3_MULADD: f = mul_dx9(a, b) + c; break;
            case OP3_MULADD_M2: f = (mul_dx9(a, b) + c) * 2; break;
            case OP3_MULADD_M4: f = (mul_dx9(a, b) + c) * 4; break;
            case OP3_MULADD_D2: f = (mul_dx9(a, b) + c) / 2; break;
            case OP3_MULADD_IEEE: case OP3_FMA: f = fmaf(a, b, c); break;
            case OP3_CNDE: f = a == 0.0f ? b : c; break;
            case OP3_CNDGT: f = a > 0.0f ? b : c; break;
            case OP3_CNDGE: f = a >= 0.0f ? b : c; break;
            case OP3_CNDE_INT: r = ia == 0 ? ub : uc; isf = false; break;
            case OP3_CNDGT_INT: r = ia > 0 ? ub : uc; isf = false; break;
            case OP3_CNDGE_INT: r = ia >= 0 ? ub : uc; isf = false; break;
            case OP3_MUL_LIT:
                /* LIT helper: src1 (y) and src0 are... dst = (b > 0) ? exp(log a * c) */
                f = (c > 0.0f || isnan(c)) ? mul_dx9(a, b) : 0.0f;
                break;
            case OP3_BFE_UINT: {
                uint32_t off = ub & 31, w = uc & 31;
                r = w ? (ua >> off) & (w == 32 ? ~0u : (1u << w) - 1) : 0;
                isf = false;
                break;
            }
            case OP3_BFE_INT: {
                uint32_t off = ub & 31, w = uc & 31;
                r = w ? (uint32_t)(((int32_t)(ua << (32 - off - w))) >> (32 - w)) : 0;
                isf = false;
                break;
            }
            case OP3_BFI_INT: r = (ua & ub) | (~ua & uc); isf = false; break;
            case OP3_BIT_ALIGN_INT:
                r = (uint32_t)((((uint64_t)ua << 32) | ub) >> (uc & 31)); isf = false; break;
            case OP3_BYTE_ALIGN_INT:
                r = (uint32_t)((((uint64_t)ua << 32) | ub) >> ((uc & 3) * 8)); isf = false; break;
            case OP3_MULADD_UINT24:
                r = (ua & 0xffffff) * (ub & 0xffffff) + uc; isf = false; break;
            default:
                eg_gfx_warn(sh->s, 0x80 + (op & 0x1f), "evergreen: ALU OP3 0x%02x\n", op);
                break;
            }
        } else {
            switch (op) {
            case OP2_ADD: f = a + b; break;
            case OP2_MUL: f = mul_dx9(a, b); break;
            case OP2_MUL_IEEE: f = a * b; break;
            case OP2_MAX: case OP2_MAX_DX10: f = fmaxf(a, b); break;
            case OP2_MIN: case OP2_MIN_DX10: f = fminf(a, b); break;
            case OP2_SETE: case OP2_SETGT: case OP2_SETGE: case OP2_SETNE:
                f = do_cmp(op - OP2_SETE, a, b) ? 1.0f : 0.0f; break;
            case OP2_SETE_DX10: case OP2_SETGT_DX10: case OP2_SETGE_DX10: case OP2_SETNE_DX10:
                r = do_cmp(op - OP2_SETE_DX10, a, b) ? ~0u : 0; isf = false; break;
            case OP2_FRACT: f = a - floorf(a); break;
            case OP2_TRUNC: f = truncf(a); break;
            case OP2_CEIL: f = ceilf(a); break;
            case OP2_RNDNE: f = rintf(a); break;
            case OP2_FLOOR: f = floorf(a); break;
            case OP2_ASHR_INT: r = (uint32_t)(ia >> (ub & 31)); isf = false; break;
            case OP2_LSHR_INT: r = ua >> (ub & 31); isf = false; break;
            case OP2_LSHL_INT: r = ua << (ub & 31); isf = false; break;
            case OP2_MOV: r = ua; isf = false; f = a;
                /* MOV keeps the bits, but clamp / omod still apply to floats */
                if ((w1 >> 31) || ((w1 >> 5) & 3)) {
                    isf = true;
                }
                break;
            case OP2_NOP: case OP2_GROUP_BARRIER: case OP2_SET_MODE:
            case OP2_SET_CF_IDX0: case OP2_SET_CF_IDX1:
                o->did[slot] = false;
                continue;
            case OP2_PRED_SETE: case OP2_PRED_SETGT: case OP2_PRED_SETGE: case OP2_PRED_SETNE:
            case OP2_PRED_SETE_PUSH: case OP2_PRED_SETGT_PUSH: case OP2_PRED_SETGE_PUSH:
            case OP2_PRED_SETNE_PUSH: {
                int cc = (op >= OP2_PRED_SETE_PUSH ? op - OP2_PRED_SETE_PUSH : op - OP2_PRED_SETE);
                bool t = do_cmp(cc, a, b);
                f = t ? 1.0f : 0.0f;
                o->pred = t;
                break;
            }
            case OP2_PRED_SETGT_UINT: case OP2_PRED_SETGE_UINT: {
                bool t = op == OP2_PRED_SETGT_UINT ? ua > ub : ua >= ub;
                f = t ? 1.0f : 0.0f;
                o->pred = t;
                break;
            }
            case OP2_PRED_SETE_INT: case OP2_PRED_SETGT_INT: case OP2_PRED_SETGE_INT:
            case OP2_PRED_SETNE_INT: case OP2_PRED_SETE_PUSH_INT: case OP2_PRED_SETGT_PUSH_INT:
            case OP2_PRED_SETGE_PUSH_INT: case OP2_PRED_SETNE_PUSH_INT:
            case OP2_PRED_SETLT_PUSH_INT: case OP2_PRED_SETLE_PUSH_INT: {
                bool t;
                switch (op) {
                case OP2_PRED_SETE_INT: case OP2_PRED_SETE_PUSH_INT: t = ia == ib; break;
                case OP2_PRED_SETGT_INT: case OP2_PRED_SETGT_PUSH_INT: t = ia > ib; break;
                case OP2_PRED_SETGE_INT: case OP2_PRED_SETGE_PUSH_INT: t = ia >= ib; break;
                case OP2_PRED_SETLT_PUSH_INT: t = ia < ib; break;
                case OP2_PRED_SETLE_PUSH_INT: t = ia <= ib; break;
                default: t = ia != ib; break;
                }
                f = t ? 1.0f : 0.0f;
                o->pred = t;
                break;
            }
            case OP2_PRED_SET_INV:
                f = a == 1.0f ? 0.0f : a + 1.0f;
                o->pred = (f == 0.0f);
                break;
            case OP2_PRED_SET_POP:
                f = a <= 0.0f ? 0.0f : a - 1.0f;
                o->pred = (f == 0.0f);
                break;
            case OP2_PRED_SET_CLR:
                f = INFINITY;
                o->pred = 0;
                break;
            case OP2_PRED_SET_RESTORE:
                f = a;
                o->pred = (a == 0.0f);
                break;
            case OP2_KILLE: case OP2_KILLGT: case OP2_KILLGE: case OP2_KILLNE: {
                bool t = do_cmp(op - OP2_KILLE, a, b);
                f = t ? 1.0f : 0.0f;
                o->kill |= t;
                break;
            }
            case OP2_KILLGT_UINT: case OP2_KILLGE_UINT: {
                bool t = op == OP2_KILLGT_UINT ? ua > ub : ua >= ub;
                f = t ? 1.0f : 0.0f;
                o->kill |= t;
                break;
            }
            case OP2_KILLE_INT: case OP2_KILLGT_INT: case OP2_KILLGE_INT: case OP2_KILLNE_INT: {
                bool t = op == OP2_KILLE_INT ? ia == ib : op == OP2_KILLGT_INT ? ia > ib :
                         op == OP2_KILLGE_INT ? ia >= ib : ia != ib;
                f = t ? 1.0f : 0.0f;
                o->kill |= t;
                break;
            }
            case OP2_AND_INT: r = ua & ub; isf = false; break;
            case OP2_OR_INT: r = ua | ub; isf = false; break;
            case OP2_XOR_INT: r = ua ^ ub; isf = false; break;
            case OP2_NOT_INT: r = ~ua; isf = false; break;
            case OP2_ADD_INT: r = ua + ub; isf = false; break;
            case OP2_SUB_INT: r = ua - ub; isf = false; break;
            case OP2_MAX_INT: r = (uint32_t)MAX(ia, ib); isf = false; break;
            case OP2_MIN_INT: r = (uint32_t)MIN(ia, ib); isf = false; break;
            case OP2_MAX_UINT: r = MAX(ua, ub); isf = false; break;
            case OP2_MIN_UINT: r = MIN(ua, ub); isf = false; break;
            case OP2_SETE_INT: r = ia == ib ? ~0u : 0; isf = false; break;
            case OP2_SETGT_INT: r = ia > ib ? ~0u : 0; isf = false; break;
            case OP2_SETGE_INT: r = ia >= ib ? ~0u : 0; isf = false; break;
            case OP2_SETNE_INT: r = ia != ib ? ~0u : 0; isf = false; break;
            case OP2_SETGT_UINT: r = ua > ub ? ~0u : 0; isf = false; break;
            case OP2_SETGE_UINT: r = ua >= ub ? ~0u : 0; isf = false; break;
            case OP2_FLT_TO_INT: r = (uint32_t)f2i_sat(truncf(a)); isf = false; break;
            case OP2_FLT_TO_INT_FLOOR: r = (uint32_t)f2i_sat(floorf(a)); isf = false; break;
            case OP2_FLT_TO_INT_RPI: r = (uint32_t)f2i_sat(floorf(a + 0.5f)); isf = false; break;
            case OP2_FLT_TO_UINT: r = f2u_sat(truncf(a)); isf = false; break;
            case OP2_INT_TO_FLT: f = (float)ia; break;
            case OP2_UINT_TO_FLT: f = (float)ua; break;
            case OP2_BFREV_INT: {
                uint32_t v = ua, x = 0;
                for (k = 0; k < 32; k++) {
                    x = (x << 1) | ((v >> k) & 1);
                }
                r = x; isf = false;
                break;
            }
            case OP2_ADDC_UINT: r = (uint64_t)ua + ub > UINT32_MAX; isf = false; break;
            case OP2_SUBB_UINT: r = ua < ub; isf = false; break;
            case OP2_EXP_IEEE: f = exp2f(a); break;
            case OP2_LOG_CLAMPED: f = a == 0.0f ? -FLT_MAX : log2f(a); break;
            case OP2_LOG_IEEE: f = log2f(a); break;
            case OP2_RECIP_CLAMPED: case OP2_RECIP_FF:
                f = 1.0f / a;
                if (isinf(f)) {
                    f = copysignf(op == OP2_RECIP_FF ? 0.0f : FLT_MAX, f);
                }
                break;
            case OP2_RECIP_IEEE: f = 1.0f / a; break;
            case OP2_RECIPSQRT_CLAMPED: case OP2_RECIPSQRT_FF:
                f = 1.0f / sqrtf(a);
                if (isinf(f)) {
                    f = op == OP2_RECIPSQRT_FF ? 0.0f : FLT_MAX;
                }
                break;
            case OP2_RECIPSQRT_IEEE: f = 1.0f / sqrtf(a); break;
            case OP2_SQRT_IEEE: f = sqrtf(a); break;
            case OP2_SIN: f = sinf(a * 2.0f * (float)M_PI); break;
            case OP2_COS: f = cosf(a * 2.0f * (float)M_PI); break;
            case OP2_MULLO_INT: case OP2_MULLO_UINT: r = ua * ub; isf = false; break;
            case OP2_MULHI_INT: r = (uint32_t)(((int64_t)ia * ib) >> 32); isf = false; break;
            case OP2_MULHI_UINT: r = (uint32_t)(((uint64_t)ua * ub) >> 32); isf = false; break;
            case OP2_RECIP_INT: r = ia ? (uint32_t)(int32_t)(4294967296.0 / ia) : 0; isf = false; break;
            case OP2_RECIP_UINT: r = ua ? (uint32_t)(4294967296.0 / ua) : 0; isf = false; break;
            case OP2_BFM_INT: r = ((1u << (ua & 31)) - 1) << (ub & 31); isf = false; break;
            case OP2_FLT32_TO_FLT16: r = float_to_half(a); isf = false; break;
            case OP2_FLT16_TO_FLT32: f = half_to_float(ua & 0xffff); break;
            case OP2_UBYTE0_FLT: f = (float)(ua & 0xff); break;
            case OP2_UBYTE1_FLT: f = (float)((ua >> 8) & 0xff); break;
            case OP2_UBYTE2_FLT: f = (float)((ua >> 16) & 0xff); break;
            case OP2_UBYTE3_FLT: f = (float)(ua >> 24); break;
            case OP2_BCNT_INT: r = ctpop32(ua); isf = false; break;
            case OP2_FFBH_UINT: r = ua ? clz32(ua) : ~0u; isf = false; break;
            case OP2_FFBL_INT: r = ua ? ctz32(ua) : ~0u; isf = false; break;
            case OP2_FFBH_INT: {
                uint32_t v = ia < 0 ? ~ua : ua;
                r = v ? clz32(v) : ~0u; isf = false;
                break;
            }
            case OP2_MUL_UINT24: r = (ua & 0xffffff) * (ub & 0xffffff); isf = false; break;
            case OP2_MULHI_UINT24:
                r = (uint32_t)(((uint64_t)(ua & 0xffffff) * (ub & 0xffffff)) >> 32); isf = false; break;
            case OP2_DOT4: case OP2_DOT4_IEEE: case OP2_MAX4:
                f = dot; break;
            case OP2_DOT_IEEE: f = a * b; break;
            case OP2_CUBE: {
                /* the four slots take (z,z,x,y), (y,x,z,z) pairs: see Mesa tgsi_tex
                 * cube setup: result x = tc, y = sc, z = 2*ma, w = face id */
                float vx = 0, vy = 0, vz = 0, ma, sc, tc, id;
                int j;
                for (j = 0; j < n; j++) {
                    if (!ins[j].op3 && ins[j].op == OP2_CUBE) {
                        switch (ins[j].slot) {
                        case 0: vz = u2f(sv[j][0]); vy = u2f(sv[j][1]); break;
                        case 1: vx = u2f(sv[j][1]); break;
                        case 2: vx = u2f(sv[j][0]); break;
                        case 3: vy = u2f(sv[j][0]); vz = u2f(sv[j][1]); break;
                        }
                    }
                }
                if (fabsf(vx) >= fabsf(vy) && fabsf(vx) >= fabsf(vz)) {
                    ma = vx; id = vx >= 0 ? 0 : 1; sc = vx >= 0 ? -vz : vz; tc = -vy;
                } else if (fabsf(vy) >= fabsf(vz)) {
                    ma = vy; id = vy >= 0 ? 2 : 3; sc = vx; tc = vy >= 0 ? vz : -vz;
                } else {
                    ma = vz; id = vz >= 0 ? 4 : 5; sc = vz >= 0 ? vx : -vx; tc = -vy;
                }
                f = slot == 0 ? tc : slot == 1 ? sc : slot == 2 ? 2.0f * fabsf(ma) : id;
                break;
            }
            case OP2_MOVA_INT:
                r = ua; isf = false;
                o->mova = ia;
                break;
            case OP2_INTERP_XY: case OP2_INTERP_ZW: case OP2_INTERP_X: case OP2_INTERP_Z: {
                /* the even slot of each pair takes j, the odd one i */
                int pi = (int)src_sel(in, 1) - 448, j, base = slot & 2;
                float vi = 0, vj = 0;
                const EgInterp *ip;
                int ch = (op == OP2_INTERP_XY || op == OP2_INTERP_X) ? (slot & 1) :
                         2 + (slot & 1);
                if (op == OP2_INTERP_XY || op == OP2_INTERP_ZW) {
                    ch = slot;
                }
                for (j = 0; j < n; j++) {
                    if (!ins[j].op3 && ins[j].op == op && (ins[j].slot & 2) == base) {
                        if (ins[j].slot & 1) {
                            vi = u2f(sv[j][0]);
                        } else {
                            vj = u2f(sv[j][0]);
                        }
                    }
                }
                if (pi < 0 || pi >= sh->ninterp || !sh->interp) {
                    f = 0;
                    break;
                }
                ip = &sh->interp[pi];
                f = ip->p0.f[ch] + vi * ip->p10.f[ch] + vj * ip->p20.f[ch];
                break;
            }
            case OP2_INTERP_LOAD_P0: case OP2_INTERP_LOAD_P10: case OP2_INTERP_LOAD_P20: {
                int pi = (int)src_sel(in, 0) - 448;
                const EgInterp *ip;
                if (pi < 0 || pi >= sh->ninterp || !sh->interp) {
                    f = 0;
                    break;
                }
                ip = &sh->interp[pi];
                r = op == OP2_INTERP_LOAD_P0 ? ip->p0.u[slot & 3] :
                    op == OP2_INTERP_LOAD_P10 ? ip->p10.u[slot & 3] : ip->p20.u[slot & 3];
                isf = false;
                break;
            }
            default:
                eg_gfx_warn(sh->s, op & 0xff, "evergreen: ALU OP2 0x%02x\n", op);
                break;
            }
        }
        if (isf) {
            uint32_t omod = in->op3 ? 0 : (w1 >> 5) & 3;
            if (op == OP2_MOV && !in->op3) {
                f = a;
            }
            if (omod == 1) {
                f *= 2;
            } else if (omod == 2) {
                f *= 4;
            } else if (omod == 3) {
                f /= 2;
            }
            if (w1 >> 31) {
                f = f > 1.0f ? 1.0f : (f >= 0.0f ? f : 0.0f);   /* NaN -> 0 */
            }
            r = f2u(f);
        } else if (op == OP2_MOV && !in->op3) {
            r = ua;
        }
        o->res[slot] = r;
        if (!in->op3 && (w1 & (1u << 2))) {
            o->exec_update = true;
        }
    }
}

/* decode a group at dword dw; returns the dwords it spans (with literals) */
static int decode_group(EgShader *sh, const EgProgram *p, uint32_t dw, AluInst *ins, int *n,
                        const uint32_t **lit)
{
    int i = 0, used = 0, last = -1, nlit = 0;
    const uint32_t *w;

    for (;;) {
        w = pgm(sh, p, dw + 2 * i, 2);
        if (!w) {
            *n = 0;
            return 2;
        }
        ins[i].w0 = w[0];
        ins[i].w1 = w[1];
        ins[i].op3 = ((w[1] >> 15) & 7) != 0;
        ins[i].op = ins[i].op3 ? (w[1] >> 13) & 0x1f : (w[1] >> 7) & 0xff;
        ins[i].nsrc = ins[i].op3 ? 3 : 2;
        {
            AluInst *in = &ins[i];
            uint32_t w0 = w[0], w1 = w[1];
            in->im = (w0 >> 26) & 7;
            in->sel[0] = w0 & 0x1ff;
            in->rel[0] = (w0 >> 9) & 1;
            in->chan[0] = (w0 >> 10) & 3;
            in->neg_mask[0] = (w0 >> 12) & 1 ? 0x80000000u : 0;
            in->abs_mask[0] = !in->op3 && (w1 & 1) ? 0x7fffffffu : 0xffffffffu;
            in->sel[1] = (w0 >> 13) & 0x1ff;
            in->rel[1] = (w0 >> 22) & 1;
            in->chan[1] = (w0 >> 23) & 3;
            in->neg_mask[1] = (w0 >> 25) & 1 ? 0x80000000u : 0;
            in->abs_mask[1] = !in->op3 && ((w1 >> 1) & 1) ? 0x7fffffffu : 0xffffffffu;
            in->sel[2] = w1 & 0x1ff;
            in->rel[2] = (w1 >> 9) & 1;
            in->chan[2] = (w1 >> 10) & 3;
            in->neg_mask[2] = (w1 >> 12) & 1 ? 0x80000000u : 0;
            in->abs_mask[2] = 0xffffffffu;
        }
        i++;
        if ((w[0] >> 31) || i == 5) {
            break;
        }
    }
    *n = i;
    for (int k = 0; k < i; k++) {
        AluInst *in = &ins[k];
        int ch = (in->w1 >> 29) & 3;
        bool trans = in->op3 ? in->op == OP3_MUL_LIT : op2_trans_only(in->op);
        for (int j = 0; j < in->nsrc; j++) {
            if (src_sel(in, j) == 0xfd) {
                uint32_t c = j == 0 ? (in->w0 >> 10) & 3 : j == 1 ? (in->w0 >> 23) & 3 :
                             (in->w1 >> 10) & 3;
                nlit = MAX(nlit, (int)c + 1);
            }
        }
        if (trans || (used >> ch) & 1 || ch <= last) {
            in->slot = 4;
        } else {
            in->slot = ch;
            used |= 1 << ch;
            last = ch;
        }
    }
    nlit = (nlit + 1) & ~1;
    *lit = pgm(sh, p, dw + 2 * i, MAX(nlit, 1));
    return 2 * i + nlit;
}

/* an ALU group, decoded once per program */
typedef struct DecodedGroup {
    AluInst ins[5];
    int n, span;
    uint32_t lit[4];
} DecodedGroup;

static const DecodedGroup *get_group(EgShader *sh, const EgProgram *cp, uint32_t dw)
{
    EgProgram *p = (EgProgram *)cp;
    DecodedGroup *g;
    const uint32_t *lit = NULL;

    if (!p->groups) {
        p->groups = g_hash_table_new_full(NULL, NULL, NULL, g_free);
    }
    g = g_hash_table_lookup(p->groups, GUINT_TO_POINTER(dw + 1));
    if (g) {
        return g;
    }
    g = g_new0(DecodedGroup, 1);
    g->span = decode_group(sh, p, dw, g->ins, &g->n, &lit);
    if (lit) {
        memcpy(g->lit, lit, MIN(4, g->span - 2 * g->n) * 4);
    }
    g_hash_table_insert(p->groups, GUINT_TO_POINTER(dw + 1), g);
    return g;
}

static void exec_alu_clause(EgShader *sh, const EgProgram *p, uint32_t addr, uint32_t count,
                            const Kcache *kc, uint32_t *mask_after)
{
    uint32_t dw = addr * 2, end = dw + count * 2;
    GroupCtx g;
    const AluInst *ins;
    GroupOut out[EG_MAX_LANES];
    int n, lane, k;

    memset(&g, 0, sizeof(g));
    while (dw < end) {
        const DecodedGroup *dg = get_group(sh, p, dw);
        const uint32_t *lit = dg->lit;
        uint32_t exec = sh->active & sh->live & ~sh->killed;
        int span = dg->span;
        uint32_t newpred = sh->pred, newactive = sh->active;
        bool exec_upd = false;

        n = dg->n;
        ins = dg->ins;
        if (!n) {
            break;
        }
        for (lane = 0; lane < sh->nlanes; lane++) {
            if ((exec >> lane) & 1) {
                exec_group_lane(sh, lane, ins, n, lit, kc, &g, &out[lane]);
            }
        }
        /* commit: GPRs, PV/PS, predicate, exec mask, kills */
        for (lane = 0; lane < sh->nlanes; lane++) {
            GroupOut *o = &out[lane];
            if (!((exec >> lane) & 1)) {
                continue;
            }
            for (k = 0; k < n; k++) {
                const AluInst *in = &ins[k];
                int slot = in->slot;
                uint32_t w1 = in->w1;
                bool wm = in->op3 || ((w1 >> 4) & 1);
                if (!o->did[slot]) {
                    continue;
                }
                if (wm) {
                    int dst = (w1 >> 21) & 0x7f, ch = (w1 >> 29) & 3;
                    if ((w1 >> 28) & 1) {
                        uint32_t im = (in->w0 >> 26) & 7;
                        dst += im < 4 ? sh->ar[lane][im] : im == 4 ? sh->loop_index : 0;
                    }
                    sh->gpr[lane][dst & (EG_NUM_GPRS - 1)].u[ch] = o->res[slot];
                }
                if (slot < 4) {
                    g.pv[lane].u[slot] = o->res[slot];
                } else {
                    g.ps_u[lane] = o->res[slot];
                }
            }
            if (o->mova != INT32_MIN) {
                sh->ar[lane][0] = sh->ar[lane][1] = sh->ar[lane][2] = sh->ar[lane][3] = o->mova;
            }
            if (o->pred >= 0) {
                for (k = 0; k < n; k++) {
                    if (!ins[k].op3 && (ins[k].w1 & (1u << 3))) {
                        newpred = (newpred & ~(1u << lane)) | ((uint32_t)o->pred << lane);
                    }
                }
                if (o->exec_update) {
                    exec_upd = true;
                    if (!o->pred) {
                        newactive &= ~(1u << lane);
                    }
                }
            }
            if (o->kill) {
                sh->killed |= 1u << lane;
            }
        }
        sh->pred = newpred;
        if (exec_upd) {
            sh->active = newactive;
        }
        dw += span;
        if (++sh->steps > 1000000) {
            break;
        }
    }
    if (mask_after) {
        *mask_after = sh->active;
    }
}

/* ------------------------------------------------------------------ */
/* Exports                                                             */
/* ------------------------------------------------------------------ */

static void do_export(EgShader *sh, uint32_t w0, uint32_t w1)
{
    uint32_t type = (w0 >> 13) & 3, base = w0 & 0x1fff, gpr = (w0 >> 15) & 0x7f;
    uint32_t burst = ((w1 >> 16) & 0xf) + 1, b, c, lane;
    uint32_t sel[4] = { w1 & 7, (w1 >> 3) & 7, (w1 >> 6) & 7, (w1 >> 9) & 7 };
    uint32_t live = sh->active & sh->live & ~sh->killed;

    for (b = 0; b < burst; b++) {
        for (lane = 0; lane < (uint32_t)sh->nlanes; lane++) {
            EgVec4 v, *dst = NULL;
            const EgVec4 *src = &sh->gpr[lane][(gpr + b) & (EG_NUM_GPRS - 1)];
            if (!((live >> lane) & 1)) {
                continue;
            }
            if (type == 1 && base + b >= 60 && base + b < 64) {
                dst = &sh->pos[lane][base + b - 60];
            } else if (type == 2 && base + b < EG_NUM_PARAMS) {
                dst = &sh->param[lane][base + b];
                sh->param_mask |= 1u << (base + b);
            } else if (type == 0 && base + b < 8) {
                dst = &sh->color[lane][base + b];
                sh->color_mask |= 1u << (base + b);
            } else if (type == 0 && base + b == 61) {
                dst = &sh->depth[lane];
                sh->depth_written = true;
            }
            if (!dst) {
                continue;
            }
            v = *dst;
            for (c = 0; c < 4; c++) {
                if (sel[c] < 4) {
                    v.u[c] = src->u[sel[c]];
                } else if (sel[c] == 4) {
                    v.f[c] = 0.0f;
                } else if (sel[c] == 5) {
                    v.f[c] = 1.0f;
                }
            }
            *dst = v;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Control flow                                                        */
/* ------------------------------------------------------------------ */

#define CF_STACK 64

typedef struct LoopState {
    uint32_t start_pc, end_pc;
    uint32_t saved_active, broken;
    int32_t count, saved_index, inc;
    bool dx10;
} LoopState;

static uint32_t cond_mask(EgShader *sh, uint32_t w1)
{
    uint32_t cond = (w1 >> 8) & 3, cf_const = (w1 >> 3) & 0x1f;
    uint32_t act = sh->active & sh->live & ~sh->killed;
    bool b;

    switch (cond) {
    case 0:
        return act;
    case 1:
        return 0;
    default:
        b = (sh->s->gfx->bool_const[(sh->stage == EG_STAGE_PS ? 0 : 1)] >> cf_const) & 1;
        return (cond == 2) == b ? act : 0;
    }
}

static void run_cf(EgShader *sh, const EgProgram *p, bool is_fetch)
{
    uint32_t stack[CF_STACK];
    int sp = 0, lsp = 0, csp = 0;
    LoopState loops[8];
    uint32_t callstack[8];
    uint32_t pc = 0;
    Kcache *kc = (Kcache *)sh->kc_cache;

    for (;;) {
        const uint32_t *w = pgm(sh, p, pc * 2, 2);
        uint32_t w0, w1, op;
        bool eop = false;

        if (!w || ++sh->steps > 1000000) {
            eg_gfx_warn(sh->s, 0xf0, "evergreen: runaway shader at 0x%" PRIx64 "\n", p->addr);
            return;
        }
        w0 = w[0];
        w1 = w[1];
        if ((w1 >> 29) & 1) {
            /* ALU clause */
            uint32_t aop = (w1 >> 26) & 0xf, addr = w0 & 0x3fffff, cnt = ((w1 >> 18) & 0x7f) + 1;
            uint32_t before = sh->active;
            if (aop == 12) {
                /* ALU_EXTENDED: this word sets KC2/KC3, the next is the clause */
                const uint32_t *x = pgm(sh, p, (pc + 1) * 2, 2);
                load_kcache_set(sh, (*kc)[2], (w0 >> 22) & 0xf, w0 >> 30, (w1 >> 2) & 0xff);
                load_kcache_set(sh, (*kc)[3], (w0 >> 26) & 0xf, w1 & 3, (w1 >> 10) & 0xff);
                pc++;
                if (!x) {
                    return;
                }
                w0 = x[0];
                w1 = x[1];
                aop = (w1 >> 26) & 0xf;
                addr = w0 & 0x3fffff;
                cnt = ((w1 >> 18) & 0x7f) + 1;
            } else {
                load_kcache_set(sh, (*kc)[2], 0, 0, 0);
                load_kcache_set(sh, (*kc)[3], 0, 0, 0);
            }
            load_kcache_set(sh, (*kc)[0], (w0 >> 22) & 0xf, w0 >> 30, (w1 >> 2) & 0xff);
            load_kcache_set(sh, (*kc)[1], (w0 >> 26) & 0xf, w1 & 3, (w1 >> 10) & 0xff);
            if (aop == 9 && sp < CF_STACK) {            /* ALU_PUSH_BEFORE */
                stack[sp++] = sh->active;
            }
            if (sh->active & sh->live & ~sh->killed) {
                exec_alu_clause(sh, p, addr, cnt, kc, NULL);
            }
            switch (aop) {
            case 10:                                    /* ALU_POP_AFTER */
                if (sp > 0) {
                    sh->active = stack[--sp];
                }
                break;
            case 11:                                    /* ALU_POP2_AFTER */
                if (sp > 1) {
                    sp -= 2;
                    sh->active = stack[sp];
                } else if (sp > 0) {
                    sh->active = stack[--sp];
                }
                break;
            case 13:                                    /* ALU_CONTINUE */
            case 14:                                    /* ALU_BREAK */
                if (lsp > 0) {
                    uint32_t gone = before & ~sh->active;
                    if (aop == 14) {
                        loops[lsp - 1].broken |= gone;
                    }
                }
                break;
            case 15:                                    /* ALU_ELSE_AFTER */
                if (sp > 0) {
                    sh->active = stack[sp - 1] & ~sh->active;
                }
                break;
            }
            pc++;
            continue;
        }

        op = (w1 >> 22) & 0xff;
        eop = (w1 >> 21) & 1;
        switch (op) {
        case 0x00:                                      /* NOP */
            break;
        case 0x01:                                      /* TC (TEX + VTX) */
        case 0x02:                                      /* VC */
        case 0x1b:                                      /* TEX_ACK */
        case 0x1c: {                                    /* VTX_ACK */
            uint32_t addr = w0 & 0xffffff, cnt = ((w1 >> 10) & 0x3f) + 1, i;
            uint32_t lanes = cond_mask(sh, w1);
            for (i = 0; i < cnt && lanes; i++) {
                const uint32_t *f = pgm(sh, p, addr * 2 + i * 4, 4);
                if (!f) {
                    break;
                }
                if (op == 0x02 || op == 0x1c || (f[0] & 0x1f) <= 2) {
                    eg_vtx_fetch(sh, f, lanes);
                } else {
                    eg_tex_fetch(sh, f, lanes);
                }
            }
            break;
        }
        case 0x04:                                      /* LOOP_START */
        case 0x06:                                      /* LOOP_START_DX10 */
        case 0x07: {                                    /* LOOP_START_NO_AL */
            uint32_t c = sh->s->gfx->loop_const[(sh->stage == EG_STAGE_PS ? 0 : 32) +
                                                 ((w1 >> 3) & 0x1f)];
            LoopState *l;
            if (lsp >= 8) {
                return;
            }
            l = &loops[lsp++];
            l->start_pc = pc;
            l->end_pc = w0 & 0xffffff;
            l->saved_active = sh->active;
            l->broken = 0;
            l->dx10 = op == 0x06;
            l->count = l->dx10 ? INT32_MAX : (int32_t)(c & 0xfff);
            l->saved_index = sh->loop_index;
            l->inc = (int8_t)(c >> 24);
            if (op != 0x07) {
                sh->loop_index = (int32_t)((c >> 12) & 0xff);
            }
            if (!l->count || !(cond_mask(sh, w1))) {
                sh->loop_index = l->saved_index;
                lsp--;
                pc = l->end_pc;
                continue;
            }
            break;
        }
        case 0x05: {                                    /* LOOP_END */
            LoopState *l;
            if (!lsp) {
                break;
            }
            l = &loops[lsp - 1];
            sh->active = (sh->active | (l->saved_active & ~l->broken)) & l->saved_active & ~l->broken;
            if (--l->count > 0 && (sh->active & sh->live & ~sh->killed)) {
                sh->loop_index += l->inc;
                pc = l->start_pc + 1;
                continue;
            }
            sh->active = l->saved_active;
            sh->loop_index = l->saved_index;
            lsp--;
            break;
        }
        case 0x08:                                      /* LOOP_CONTINUE */
        case 0x09:                                      /* LOOP_BREAK */
            if (lsp) {
                uint32_t m = cond_mask(sh, w1);
                if (op == 0x09) {
                    loops[lsp - 1].broken |= m;
                }
                sh->active &= ~m;
                if (!(sh->active & sh->live & ~sh->killed)) {
                    pc = w0 & 0xffffff;     /* the LOOP_END, which re-arms the lanes */
                    continue;
                }
            }
            break;
        case 0x0a: {                                    /* JUMP */
            uint32_t m = cond_mask(sh, w1);
            if (!m) {
                uint32_t pop = w1 & 7;
                while (pop-- && sp > 0) {
                    sh->active = stack[--sp];
                }
                pc = w0 & 0xffffff;
                continue;
            }
            break;
        }
        case 0x0b:                                      /* PUSH */
            if (sp < CF_STACK) {
                stack[sp++] = sh->active;
            }
            if (!cond_mask(sh, w1)) {
                uint32_t pop = w1 & 7;
                while (pop-- && sp > 0) {
                    sh->active = stack[--sp];
                }
                pc = w0 & 0xffffff;
                continue;
            }
            break;
        case 0x0d: {                                    /* ELSE */
            uint32_t top = sp > 0 ? stack[sp - 1] : sh->live;
            sh->active = top & ~sh->active;
            if (!(sh->active & sh->live & ~sh->killed)) {
                uint32_t pop = w1 & 7;
                while (pop-- && sp > 0) {
                    sh->active = stack[--sp];
                }
                pc = w0 & 0xffffff;
                continue;
            }
            break;
        }
        case 0x0e: {                                    /* POP */
            uint32_t pop = w1 & 7;
            while (pop-- && sp > 0) {
                sh->active = stack[--sp];
            }
            break;
        }
        case 0x12:                                      /* CALL */
            if (cond_mask(sh, w1) && csp < 8) {
                callstack[csp++] = pc + 1;
                pc = w0 & 0xffffff;
                continue;
            }
            break;
        case 0x13:                                      /* CALL_FS */
            if (sh->fetch && !is_fetch) {
                sh->in_fs = true;
                run_cf(sh, sh->fetch, true);
                sh->in_fs = false;
            }
            break;
        case 0x14:                                      /* RET */
            if (is_fetch && !csp) {
                return;
            }
            if (csp) {
                pc = callstack[--csp];
                continue;
            }
            return;
        case 0x18:                                      /* KILL */
            sh->killed |= cond_mask(sh, w1);
            break;
        case 0x1a:                                      /* WAIT_ACK */
        case 0x1e:                                      /* WAVE_SYNC */
        case 0x50: case 0x52: case 0x55: case 0x56: case 0x57:  /* memory exports */
        case 0x58: case 0x59: case 0x5a: case 0x5b: case 0x5c:
            if (op >= 0x50) {
                eg_gfx_warn(sh->s, 0xe0 + (op & 0xf), "evergreen: CF memory export 0x%02x\n", op);
            }
            break;
        case 0x1f:                                      /* HALT */
            return;
        case 0x53:                                      /* EXPORT */
        case 0x54:                                      /* EXPORT_DONE */
            do_export(sh, w0, w1);
            break;
        default:
            eg_gfx_warn(sh->s, 0xc0 + (op & 0x1f), "evergreen: CF op 0x%02x\n", op);
            break;
        }
        if (eop) {
            return;
        }
        pc++;
    }
}

void eg_shader_run(EgShader *sh)
{
    sh->active = sh->live;
    sh->steps = 0;
    run_cf(sh, sh->prog, false);
}
