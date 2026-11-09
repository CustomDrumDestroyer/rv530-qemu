/*
 * Tests for r500_us.c: decoder/disassembler round trips against Mesa's
 * compiler output, hand-encoded execution tests, texture callback paths,
 * flow control, and GLSL emission validated with glslangValidator.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "r500_us.h"
#include "r5xx_vk.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/stat.h>

static int g_fail, g_pass;
static const char *g_rc, *g_corpus, *g_glslang;

#define CHECK(cond, ...) do { \
    if (cond) { g_pass++; } else { g_fail++; \
        printf("  FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } \
} while (0)

static bool feq(float a, float b, float tol)
{
    if (isnan(a) && isnan(b)) {
        return true;
    }
    if (isinf(a) || isinf(b)) {
        return a == b;
    }
    return fabsf(a - b) <= tol * (1.0f + fabsf(b));
}

#define CHECKF(got, exp) CHECK(feq((got), (exp), 1e-5f), "%s = %g, expected %g", #got, (double)(got), (double)(exp))

/* ------------------------------------------------------------------ */
/* Mini assembler (mirrors Mesa's r500_fragprog_emit.c field layout)   */
/* ------------------------------------------------------------------ */

enum { K_T = 0, K_C = 1, K_I = 2 };            /* temp, const, inline */
typedef struct Src { int kind, idx, rel; } Src;
#define T(i)   ((Src){ K_T, (i), 0 })
#define TR(i)  ((Src){ K_T, (i), 1 })
#define C(i)   ((Src){ K_C, (i), 0 })
#define IL(v)  ((Src){ K_I, (v), 0 })
#define IL_ONE IL(0x38)
#define IL_ZERO IL(0x00)  /* note: hardware reads 2^-10; use swizzle 0 for zero */

enum { SEL0, SEL1, SEL2, SELP };
enum { MOD_NOP, MOD_NEG, MOD_ABS, MOD_NAB };
enum { RGB_MAD, RGB_DP3, RGB_DP4, RGB_D2A, RGB_MIN, RGB_MAX, RGB_CND = 7, RGB_CMP, RGB_FRC, RGB_SOP, RGB_MDH, RGB_MDV };
enum { A_MAD, A_DP, A_MIN, A_MAX, A_CND = 5, A_CMP, A_FRC, A_EX2, A_LN2, A_RCP, A_RSQ, A_SIN, A_COS, A_MDH, A_MDV };
enum { OMOD_1, OMOD_X2, OMOD_X4, OMOD_X8, OMOD_D2, OMOD_D4, OMOD_D8, OMOD_OFF };
enum { CMP_EQ, CMP_LT, CMP_GE, CMP_NE };
enum { PS_NONE, PS_RGBA, PS_RRRR, PS_GGGG, PS_BBBB, PS_AAAA };
enum { SRCP_BIAS, SRCP_SUB, SRCP_ADD, SRCP_INV };
enum { TYPE_ALU, TYPE_OUT, TYPE_FC, TYPE_TEX };
enum { TEX_NOP, TEX_LD, TEX_KILL, TEX_PROJ, TEX_LODBIAS, TEX_LOD, TEX_DXDY };
enum { FC_JUMP, FC_LOOP, FC_ENDLOOP, FC_REP, FC_ENDREP, FC_BREAKLOOP, FC_BREAKREP, FC_CONTINUE };
enum { AOP_NONE, AOP_POP, AOP_PUSH };
enum { BOP_NONE, BOP_DECR, BOP_INCR };

static unsigned swz_char(char c)
{
    switch (c) {
    case 'r': case 'x': return 0;
    case 'g': case 'y': return 1;
    case 'b': case 'z': return 2;
    case 'a': case 'w': return 3;
    case '0': return 4;
    case 'h': return 5;
    case '1': return 6;
    default:  return 7;
    }
}

typedef struct Arg { int sel; const char *swz; int mod; } Arg;

typedef struct Alu {
    int type, wmask, omask, clamp_rgb, clamp_a;
    int rgb_ps, rgb_pinv, a_ps, a_pinv;
    int alu_wmask, alu_sel, alu_op;
    int nop, last, write_inactive, tex_wait;
    Src rgb_src[3], a_src[3];
    int rgb_srcp, a_srcp;
    Arg A, B, Cc, aA, aB, aC;
    int rgb_op, a_op, rgb_dst, a_dst, rgb_dst_rel, a_dst_rel;
    int rgb_omod, a_omod, rgb_target, a_target, w_omask;
} Alu;

static unsigned enc_src(Src s)
{
    switch (s.kind) {
    case K_C: return (s.idx & 0xff) | (1 << 8) | (s.rel ? 1 << 9 : 0);
    case K_I: return 0x80 | (s.idx & 0x7f);
    default:  return (s.idx & 0x7f) | (s.rel ? 1 << 9 : 0);
    }
}

static unsigned enc_rgb_arg(const Arg *a)
{
    unsigned t = a->sel & 3, i;
    const char *swz = a->swz ? a->swz : "rgb";
    for (i = 0; i < 3; i++) {
        t |= swz_char(swz[i]) << (2 + 3 * i);
    }
    t |= (a->mod & 3) << 11;
    return t;
}

static unsigned enc_alpha_arg(const Arg *a)
{
    unsigned t = a->sel & 3;
    const char *swz = a->swz ? a->swz : "a";
    t |= swz_char(swz[0]) << 2;
    t |= (a->mod & 3) << 5;
    return t;
}

static void enc_alu(const Alu *a, uint32_t *w)
{
    memset(w, 0, 6 * sizeof(uint32_t));
    w[0] = a->type | (a->tex_wait << 2) | (a->rgb_ps << 3) | (a->rgb_pinv << 6) |
           (a->write_inactive << 7) | (a->last << 8) | (a->nop << 9) |
           ((a->wmask & 7) << 11) | (((a->wmask >> 3) & 1) << 14) |
           ((a->omask & 7) << 15) | (((a->omask >> 3) & 1) << 18) |
           (a->clamp_rgb << 19) | (a->clamp_a << 20) | (a->alu_sel << 21) |
           (a->a_pinv << 22) | (a->alu_op << 23) | (a->a_ps << 25);
    w[1] = enc_src(a->rgb_src[0]) | (enc_src(a->rgb_src[1]) << 10) |
           (enc_src(a->rgb_src[2]) << 20) | ((unsigned)a->rgb_srcp << 30);
    w[2] = enc_src(a->a_src[0]) | (enc_src(a->a_src[1]) << 10) |
           (enc_src(a->a_src[2]) << 20) | ((unsigned)a->a_srcp << 30);
    w[3] = enc_rgb_arg(&a->A) | (enc_rgb_arg(&a->B) << 13) | (a->rgb_omod << 26) |
           (a->rgb_target << 29) | (a->alu_wmask ? 1u << 31 : 0);
    w[4] = a->a_op | (a->a_dst << 4) | (a->a_dst_rel << 11) |
           (enc_alpha_arg(&a->aA) << 12) | (enc_alpha_arg(&a->aB) << 19) |
           (a->a_omod << 26) | (a->a_target << 29) | (a->w_omask ? 1u << 31 : 0);
    w[5] = a->rgb_op | (a->rgb_dst << 4) | (a->rgb_dst_rel << 11) |
           (enc_rgb_arg(&a->Cc) << 12) | (enc_alpha_arg(&a->aC) << 25);
}

typedef struct Tex {
    int op, unit, src, dst, wmask, src_rel, dst_rel;
    const char *src_swz, *dst_swz, *dx_swz, *dy_swz;
    int dx, dy, unscaled, acq, tex_wait, ign_unc;
    int rgb_ps, rgb_pinv, a_ps, a_pinv, write_inactive, last;
} Tex;

static unsigned enc_swz4(const char *s, const char *def)
{
    unsigned v = 0, i;
    if (!s) {
        s = def;
    }
    for (i = 0; i < 4; i++) {
        v |= (swz_char(s[i]) & 3) << (2 * i);
    }
    return v;
}

static void enc_tex(const Tex *t, uint32_t *w)
{
    memset(w, 0, 6 * sizeof(uint32_t));
    w[0] = TYPE_TEX | (t->tex_wait << 2) | (t->rgb_ps << 3) | (t->rgb_pinv << 6) |
           (t->write_inactive << 7) | (t->last << 8) |
           ((t->wmask & 7) << 11) | (((t->wmask >> 3) & 1) << 14) |
           (t->a_pinv << 22) | (t->a_ps << 25);
    w[1] = (t->unit << 16) | (t->op << 22) | (t->acq << 25) | (t->ign_unc << 26) | (t->unscaled << 27);
    w[2] = t->src | (t->src_rel << 7) | (enc_swz4(t->src_swz, "rgba") << 8) |
           (t->dst << 16) | (t->dst_rel << 23) | (enc_swz4(t->dst_swz, "rgba") << 24);
    w[3] = t->dx | (enc_swz4(t->dx_swz, "rgba") << 8) | (t->dy << 16) | (enc_swz4(t->dy_swz, "rgba") << 24);
}

typedef struct Fc {
    int op, func, any, a_op, b_op0, b_op1, pop, b_else, ign_unc;
    int bool_addr, int_addr, addr, global, ps, pinv, last;
} Fc;

static void enc_fc(const Fc *f, uint32_t *w)
{
    memset(w, 0, 6 * sizeof(uint32_t));
    w[0] = TYPE_FC | (1 << 10) | (f->ps << 3) | (f->pinv << 6) | (f->last << 8);
    w[2] = f->op | (f->b_else << 4) | (f->any << 5) | (f->a_op << 6) | (f->func << 8) |
           (f->pop << 16) | (f->b_op0 << 24) | (f->b_op1 << 26) | (f->ign_unc << 28);
    w[3] = f->bool_addr | (f->int_addr << 8) | (f->addr << 16) | (f->global ? 1u << 31 : 0);
}

typedef struct Prog {
    R500UsProgram p;
    int n;
} Prog;

static void prog_init(Prog *pr)
{
    memset(pr, 0, sizeof(*pr));
    pr->p.config = 2;           /* ZERO_TIMES_ANYTHING_EQUALS_ZERO like Mesa */
    pr->p.fc_int_const[0] = 0xff;
}

static int prog_add(Prog *pr, const uint32_t *w)
{
    int i = pr->n++;
    memcpy(&pr->p.code[i * 6], w, 6 * sizeof(uint32_t));
    return i;
}

static int prog_alu(Prog *pr, const Alu *a)
{
    uint32_t w[6];
    enc_alu(a, w);
    return prog_add(pr, w);
}

static int prog_tex(Prog *pr, const Tex *t)
{
    uint32_t w[6];
    enc_tex(t, w);
    return prog_add(pr, w);
}

static int prog_fc(Prog *pr, const Fc *f)
{
    uint32_t w[6];
    enc_fc(f, w);
    return prog_add(pr, w);
}

static void prog_patch_fc_addr(Prog *pr, int idx, int addr)
{
    pr->p.code[idx * 6 + 3] = (pr->p.code[idx * 6 + 3] & ~(0x1ffu << 16)) | ((unsigned)addr << 16);
}

static void prog_finish(Prog *pr)
{
    pr->p.code_addr = (pr->n - 1) << 16;
    pr->p.code_range = (pr->n - 1) << 16;
    pr->p.code_offset = 0;
    pr->p.fc_ctrl = 1u << 31;
    pr->p.pixsize = 8;
    pr->p.code[(pr->n - 1) * 6] |= 1 << 2;    /* TEX_SEM_WAIT on last */
}

/* Common instruction shapes. */

/* temp dst = src (MAD src * 1 + 0, all channels) */
static Alu alu_mov(int dst, Src src)
{
    Alu a = { 0 };
    a.type = TYPE_ALU;
    a.wmask = 0xf;
    a.rgb_src[0] = src;
    a.a_src[0] = src;
    a.rgb_src[1] = IL_ONE; a.rgb_src[2] = IL_ONE;
    a.a_src[1] = IL_ONE; a.a_src[2] = IL_ONE;
    a.A = (Arg){ SEL0, "rgb", 0 };
    a.B = (Arg){ SEL0, "111", 0 };
    a.Cc = (Arg){ SEL0, "000", 0 };
    a.aA = (Arg){ SEL0, "a", 0 };
    a.aB = (Arg){ SEL0, "1", 0 };
    a.aC = (Arg){ SEL0, "0", 0 };
    a.rgb_dst = a.a_dst = dst;
    return a;
}

/* output target = src */
static Alu alu_out(int target, Src src)
{
    Alu a = alu_mov(0, src);
    a.type = TYPE_OUT;
    a.wmask = 0;
    a.omask = 0xf;
    a.rgb_target = a.a_target = target;
    return a;
}

/* Mesa-style IF/ELSE/ENDIF helpers (r500_fragprog_emit.c) */
static int emit_if(Prog *pr, int func)
{
    Fc f = { 0 };
    f.op = FC_JUMP; f.func = func; f.b_op0 = BOP_INCR; f.ign_unc = 1;
    return prog_fc(pr, &f);
}

static int emit_else(Prog *pr)
{
    Fc f = { 0 };
    f.op = FC_JUMP; f.b_else = 1; f.b_op1 = BOP_DECR; f.pop = 1;
    return prog_fc(pr, &f);
}

static void emit_endif(Prog *pr, int if_idx, int else_idx)
{
    Fc f = { 0 };
    int endif;
    f.op = FC_JUMP; f.any = 1; f.b_op0 = BOP_DECR; f.pop = 1;
    endif = prog_fc(pr, &f);
    prog_patch_fc_addr(pr, endif, endif + 1);
    if (else_idx >= 0) {
        pr->p.code[if_idx * 6 + 2] |= BOP_INCR << 26;
        prog_patch_fc_addr(pr, if_idx, else_idx + 1);
        prog_patch_fc_addr(pr, else_idx, endif + 1);
    } else {
        prog_patch_fc_addr(pr, if_idx, endif + 1);
    }
}

static int emit_loop(Prog *pr, int is_rep, int int_addr)
{
    Fc f = { 0 };
    f.op = is_rep ? FC_REP : FC_LOOP; f.ign_unc = 1; f.int_addr = int_addr;
    return prog_fc(pr, &f);
}

static void emit_endloop(Prog *pr, int loop_idx, int is_rep, const int *brks, int nbrk,
                         const int *conts, int ncont)
{
    Fc f = { 0 };
    int end, i;
    f.op = is_rep ? FC_ENDREP : FC_ENDLOOP; f.func = 0xff; f.any = 1; f.ign_unc = 1;
    f.int_addr = (pr->p.code[loop_idx * 6 + 3] >> 8) & 0x1f;
    end = prog_fc(pr, &f);
    prog_patch_fc_addr(pr, end, loop_idx + 1);
    prog_patch_fc_addr(pr, loop_idx, end);
    for (i = 0; i < nbrk; i++) {
        prog_patch_fc_addr(pr, brks[i], end + 1);
    }
    for (i = 0; i < ncont; i++) {
        prog_patch_fc_addr(pr, conts[i], end);
    }
}

static int emit_break(Prog *pr, int depth, int is_rep, int func)
{
    Fc f = { 0 };
    f.op = is_rep ? FC_BREAKREP : FC_BREAKLOOP; f.func = func; f.b_op1 = BOP_DECR; f.pop = depth; f.ign_unc = 1;
    return prog_fc(pr, &f);
}

static int emit_continue(Prog *pr, int depth, int func)
{
    Fc f = { 0 };
    f.op = FC_CONTINUE; f.func = func; f.b_op1 = BOP_DECR; f.pop = depth; f.ign_unc = 1;
    return prog_fc(pr, &f);
}

/* ALU instruction writing the ALU result: alu_result = (src.x OP 0) */
static Alu alu_result_x(Src src, int cmp)
{
    Alu a = alu_mov(0, src);
    a.wmask = 0;
    a.alu_wmask = 1;
    a.alu_sel = 0;
    a.alu_op = cmp;
    return a;
}

/* ------------------------------------------------------------------ */
/* Execution helpers                                                   */
/* ------------------------------------------------------------------ */

typedef struct FakeTex {
    R5xxTexRequest last;
    int calls;
} FakeTex;

/* Fake sampler: result is a known function of the request so tests can
 * verify the coordinate path. */
static void fake_tex(void *opaque, const R5xxTexRequest *req, r5xx_vec4 out[4])
{
    FakeTex *ft = opaque;
    int p;
    ft->last = *req;
    ft->calls++;
    for (p = 0; p < 4; p++) {
        const r5xx_vec4 *c = &req->coord[p];
        switch (req->op) {
        case R5XX_TEXOP_PROJ:
            out[p] = r5xx_vec4_make(c->f[0] / c->f[3], c->f[1] / c->f[3], c->f[2] / c->f[3], 1.0f + req->unit);
            break;
        case R5XX_TEXOP_BIAS:
            out[p] = r5xx_vec4_make(c->f[0], c->f[1], req->lod[p], 2.0f + req->unit);
            break;
        case R5XX_TEXOP_LOD:
            out[p] = r5xx_vec4_make(c->f[0], c->f[1], req->lod[p], 3.0f + req->unit);
            break;
        case R5XX_TEXOP_GRAD:
            out[p] = r5xx_vec4_make(req->ddx[p].f[0], req->ddy[p].f[1], c->f[0], 4.0f + req->unit);
            break;
        default:
            out[p] = r5xx_vec4_make(c->f[0] * 2.0f, c->f[1] * 2.0f, c->f[2], 0.0f + req->unit);
            break;
        }
    }
}

static R500UsQuadIn make_quad(void)
{
    R500UsQuadIn q;
    int p, i;
    memset(&q, 0, sizeof(q));
    q.coverage = 0xf;
    for (p = 0; p < 4; p++) {
        for (i = 0; i < R500_US_MAX_INPUTS; i++) {
            q.inputs[p][i] = r5xx_vec4_make(p + 1 + i * 10, 2 * p + i * 10, -(float)p + i * 10, 0.5f + i * 10);
        }
    }
    return q;
}

static bool run_prog(const R500UsProgram *p, const R500UsQuadIn *in, const r5xx_vec4 *consts,
                     r500_us_tex_fn tex, void *opaque, R500UsQuadOut *out)
{
    char err[256];
    R500UsDecoded *d = r500_us_decode(p, err, sizeof(err));
    R500UsExec e;
    if (!d) {
        printf("  decode failed: %s\n", err);
        return false;
    }
    e.prog = d;
    e.consts = consts;
    e.tex = tex;
    e.tex_opaque = opaque;
    r500_us_run_quad(&e, in, out);
    r500_us_free(d);
    return true;
}

/* ------------------------------------------------------------------ */
/* Tests                                                               */
/* ------------------------------------------------------------------ */

static void test_mov_mesa_words(void)
{
    /* rc_compile output for `MOV output[0], input[0];` */
    static const uint32_t w[6] = { 0x00078005, 0x08020000, 0x08020000, 0x00db0220, 0x00c0c000, 0x20490000 };
    Prog pr;
    R500UsQuadIn q = make_quad();
    R500UsQuadOut out;
    char err[128];
    R500UsDecoded *d;
    int p, c;

    printf("test_mov_mesa_words\n");
    prog_init(&pr);
    prog_add(&pr, w);
    prog_finish(&pr);
    d = r500_us_decode(&pr.p, err, sizeof(err));
    CHECK(d != NULL, "decode: %s", err);
    CHECK(r500_us_num_instructions(d) == 1, "num instructions");
    CHECK(r500_us_inputs_used(d) == 1, "inputs used %x", r500_us_inputs_used(d));
    CHECK(r500_us_outputs_written(d) == 1, "outputs written");
    CHECK(!r500_us_writes_depth(d) && !r500_us_uses_kill(d) && !r500_us_uses_flow_control(d), "flags");
    r500_us_free(d);
    CHECK(run_prog(&pr.p, &q, NULL, NULL, NULL, &out), "run");
    for (p = 0; p < 4; p++) {
        for (c = 0; c < 4; c++) {
            CHECKF(out.color[p][0].f[c], q.inputs[p][0].f[c]);
        }
    }
    CHECK(out.killed == 0 && !out.depth_written, "no kill/depth");
}

static void test_mad_swizzle_modifiers(void)
{
    Prog pr;
    Alu a;
    R500UsQuadIn q = make_quad();
    R500UsQuadOut out;
    r5xx_vec4 consts[256];
    int p;

    printf("test_mad_swizzle_modifiers\n");
    memset(consts, 0, sizeof(consts));
    consts[3] = r5xx_vec4_make(1, -2, 3, -4);
    for (p = 0; p < 4; p++) {
        q.inputs[p][0] = r5xx_vec4_make(0.5f, 0.25f, 2.0f, 8.0f);
    }
    prog_init(&pr);
    /* rgb: -|c3.gbr| * t0.aaa + 0.5 ; alpha: (-c3.a) * t0.r + 1 */
    a = alu_mov(1, C(3));
    a.rgb_src[1] = T(0);
    a.a_src[1] = T(0);
    a.A = (Arg){ SEL0, "gbr", MOD_NAB };
    a.B = (Arg){ SEL1, "aaa", MOD_NOP };
    a.Cc = (Arg){ SEL0, "hhh", MOD_NOP };
    a.aA = (Arg){ SEL0, "a", MOD_NEG };
    a.aB = (Arg){ SEL1, "r", MOD_NOP };
    a.aC = (Arg){ SEL0, "1", MOD_NOP };
    a.rgb_omod = OMOD_X2;
    a.a_omod = OMOD_D4;
    a.clamp_a = 1;
    prog_alu(&pr, &a);
    /* clamped copy to t2 */
    a = alu_mov(2, T(1));
    a.clamp_rgb = 1;
    a.a_omod = OMOD_X8;
    prog_alu(&pr, &a);
    prog_alu(&pr, &(Alu){ 0 }) ; /* placeholder replaced below */
    pr.n--;
    a = alu_out(0, T(1));
    prog_alu(&pr, &a);
    a = alu_out(1, T(2));
    prog_alu(&pr, &a);
    prog_finish(&pr);
    CHECK(run_prog(&pr.p, &q, consts, NULL, NULL, &out), "run");
    for (p = 0; p < 4; p++) {
        CHECKF(out.color[p][0].f[0], -31.0f);
        CHECKF(out.color[p][0].f[1], -47.0f);
        CHECKF(out.color[p][0].f[2], -15.0f);
        CHECKF(out.color[p][0].f[3], 0.75f);
        CHECKF(out.color[p][1].f[0], 0.0f);
        CHECKF(out.color[p][1].f[3], 6.0f);
    }
}

static void test_dot_products(void)
{
    Prog pr;
    Alu a;
    R500UsQuadIn q = make_quad();
    R500UsQuadOut out;
    int p;

    printf("test_dot_products\n");
    for (p = 0; p < 4; p++) {
        q.inputs[p][0] = r5xx_vec4_make(1, 2, 3, 7);
        q.inputs[p][1] = r5xx_vec4_make(4, 5, 6, 8);
    }
    prog_init(&pr);
    a = alu_mov(2, T(0)); a.rgb_src[1] = T(1); a.a_src[1] = T(1);
    a.rgb_op = RGB_DP3; a.a_op = A_DP;
    a.A = (Arg){ SEL0, "rgb", 0 }; a.B = (Arg){ SEL1, "rgb", 0 };
    prog_alu(&pr, &a);
    a.rgb_dst = a.a_dst = 3;
    a.rgb_op = RGB_DP4; a.aA = (Arg){ SEL0, "a", 0 }; a.aB = (Arg){ SEL1, "a", 0 };
    prog_alu(&pr, &a);
    a.rgb_dst = a.a_dst = 4;
    a.rgb_op = RGB_D2A; a.Cc = (Arg){ SEL1, "rgb", 0 };
    prog_alu(&pr, &a);
    a = alu_out(0, T(2)); prog_alu(&pr, &a);
    a = alu_out(1, T(3)); prog_alu(&pr, &a);
    a = alu_out(2, T(4)); prog_alu(&pr, &a);
    prog_finish(&pr);
    CHECK(run_prog(&pr.p, &q, NULL, NULL, NULL, &out), "run");
    for (p = 0; p < 4; p++) {
        int c;
        for (c = 0; c < 4; c++) {
            CHECKF(out.color[p][0].f[c], 32.0f);
            CHECKF(out.color[p][1].f[c], 88.0f);
            CHECKF(out.color[p][2].f[c], 20.0f);
        }
    }
}

static void test_min_max_cnd_cmp_frc(void)
{
    Prog pr;
    Alu a;
    R500UsQuadIn q = make_quad();
    R500UsQuadOut out;
    int p;

    printf("test_min_max_cnd_cmp_frc\n");
    for (p = 0; p < 4; p++) {
        q.inputs[p][0] = r5xx_vec4_make(1.5f, -2.25f, 3.75f, 0.6f);
        q.inputs[p][1] = r5xx_vec4_make(-1.0f, 0.0f, 5.0f, 0.4f);
    }
    prog_init(&pr);
    /* MIN -> t2, MAX -> t3, CND (C = t1) -> t4, CMP (C = t0) -> t5, FRC -> t6 */
    a = alu_mov(2, T(0)); a.rgb_src[1] = T(1); a.a_src[1] = T(1);
    a.A = (Arg){ SEL0, "rgb", 0 }; a.B = (Arg){ SEL1, "rgb", 0 };
    a.aA = (Arg){ SEL0, "a", 0 }; a.aB = (Arg){ SEL1, "a", 0 };
    a.rgb_op = RGB_MIN; a.a_op = A_MIN; prog_alu(&pr, &a);
    a.rgb_dst = a.a_dst = 3; a.rgb_op = RGB_MAX; a.a_op = A_MAX; prog_alu(&pr, &a);
    a.rgb_dst = a.a_dst = 4; a.rgb_op = RGB_CND; a.a_op = A_CND;
    a.Cc = (Arg){ SEL1, "rgb", 0 }; a.aC = (Arg){ SEL1, "a", 0 }; prog_alu(&pr, &a);
    a.rgb_dst = a.a_dst = 5; a.rgb_op = RGB_CMP; a.a_op = A_CMP;
    a.Cc = (Arg){ SEL0, "rgb", 0 }; a.aC = (Arg){ SEL0, "a", 0 }; prog_alu(&pr, &a);
    a.rgb_dst = a.a_dst = 6; a.rgb_op = RGB_FRC; a.a_op = A_FRC; prog_alu(&pr, &a);
    a = alu_out(0, T(2)); prog_alu(&pr, &a);
    a = alu_out(1, T(3)); prog_alu(&pr, &a);
    a = alu_out(2, T(4)); prog_alu(&pr, &a);
    a = alu_out(3, T(5)); prog_alu(&pr, &a);
    a = alu_mov(0, T(6)); a.type = TYPE_OUT; a.wmask = 0; a.omask = 0xf; a.rgb_target = a.a_target = 0;
    /* write FRC into target 0 alpha only (keep rgb of MIN): use omask A */
    a.omask = 8; a.a_target = 3; prog_alu(&pr, &a);
    prog_finish(&pr);
    CHECK(run_prog(&pr.p, &q, NULL, NULL, NULL, &out), "run");
    for (p = 0; p < 4; p++) {
        CHECKF(out.color[p][0].f[0], -1.0f); CHECKF(out.color[p][0].f[1], -2.25f); CHECKF(out.color[p][0].f[2], 3.75f);
        CHECKF(out.color[p][1].f[0], 1.5f); CHECKF(out.color[p][1].f[1], 0.0f); CHECKF(out.color[p][1].f[2], 5.0f); CHECKF(out.color[p][1].f[3], 0.6f);
        /* CND: C > 0.5 ? A : B with C = t1 */
        CHECKF(out.color[p][2].f[0], -1.0f); CHECKF(out.color[p][2].f[1], 0.0f); CHECKF(out.color[p][2].f[2], 3.75f); CHECKF(out.color[p][2].f[3], 0.4f);
        /* CMP: C >= 0 ? A : B with C = t0 */
        CHECKF(out.color[p][3].f[0], 1.5f); CHECKF(out.color[p][3].f[1], 0.0f); CHECKF(out.color[p][3].f[2], 3.75f);
        /* FRC alpha of t0 = 0.6 */
        CHECKF(out.color[p][3].f[3], 0.6f);
    }
}

static void test_transcendentals(void)
{
    Prog pr;
    Alu a;
    R500UsQuadIn q = make_quad();
    R500UsQuadOut out;
    int p;
    static const int ops[6] = { A_EX2, A_LN2, A_RCP, A_RSQ, A_SIN, A_COS };
    static const float in_a[6] = { 3.0f, 8.0f, 4.0f, 16.0f, 0.25f, 0.5f };
    static const float exp_a[6] = { 8.0f, 3.0f, 0.25f, 0.25f, 1.0f, -1.0f };
    int i;

    printf("test_transcendentals\n");
    for (i = 0; i < 6; i++) {
        for (p = 0; p < 4; p++) {
            q.inputs[p][0] = r5xx_vec4_make(0, 0, 0, in_a[i]);
        }
        prog_init(&pr);
        a = alu_mov(1, T(0));
        a.rgb_op = RGB_SOP;
        a.a_op = ops[i];
        prog_alu(&pr, &a);
        a = alu_out(0, T(1));
        prog_alu(&pr, &a);
        prog_finish(&pr);
        CHECK(run_prog(&pr.p, &q, NULL, NULL, NULL, &out), "run");
        for (p = 0; p < 4; p++) {
            int c;
            for (c = 0; c < 4; c++) {
                CHECK(feq(out.color[p][0].f[c], exp_a[i], 1e-5f), "op %d chan %d = %g expected %g",
                      ops[i], c, out.color[p][0].f[c], exp_a[i]);
            }
        }
    }
    /* special values: RCP(+0)=+inf, RCP(-0)=-inf, RSQ(-0)=+inf, LN2(0)=-inf, RSQ(-4)=NaN */
    {
        static const int sops[5] = { A_RCP, A_RCP, A_RSQ, A_LN2, A_RSQ };
        static const float sin_a[5] = { 0.0f, -0.0f, -0.0f, 0.0f, -4.0f };
        static const float sexp[5] = { INFINITY, -INFINITY, INFINITY, -INFINITY, NAN };
        for (i = 0; i < 5; i++) {
            for (p = 0; p < 4; p++) {
                q.inputs[p][0] = r5xx_vec4_make(0, 0, 0, sin_a[i]);
            }
            prog_init(&pr);
            a = alu_mov(1, T(0));
            a.rgb_op = RGB_SOP; a.a_op = sops[i];
            a.a_omod = OMOD_OFF;   /* keep inf/nan unmodified */
            prog_alu(&pr, &a);
            a = alu_out(0, T(1)); a.a_omod = OMOD_OFF; a.rgb_omod = OMOD_OFF;
            a.rgb_op = RGB_MAX; a.a_op = A_MAX; a.B = (Arg){ SEL0, "rgb", 0 }; a.aB = (Arg){ SEL0, "a", 0 };
            prog_alu(&pr, &a);
            prog_finish(&pr);
            CHECK(run_prog(&pr.p, &q, NULL, NULL, NULL, &out), "run");
            CHECK(feq(out.color[0][0].f[3], sexp[i], 0), "special %d: %g expected %g", i, out.color[0][0].f[3], sexp[i]);
        }
    }
}

static void test_presubtract_inline(void)
{
    Prog pr;
    Alu a;
    R500UsQuadIn q = make_quad();
    R500UsQuadOut out;
    int p;

    printf("test_presubtract_inline\n");
    for (p = 0; p < 4; p++) {
        q.inputs[p][0] = r5xx_vec4_make(0.25f, 0.5f, 2.0f, 4.0f);
        q.inputs[p][1] = r5xx_vec4_make(1.0f, 3.0f, 5.0f, 7.0f);
    }
    prog_init(&pr);
    /* t2 = srcp with BIAS (1-2*src0) rgb, SUB alpha (src1 - src0) */
    a = alu_mov(2, T(0)); a.rgb_src[1] = T(1); a.a_src[1] = T(1);
    a.A = (Arg){ SELP, "rgb", 0 }; a.aA = (Arg){ SELP, "a", 0 };
    a.rgb_srcp = SRCP_BIAS; a.a_srcp = SRCP_SUB;
    prog_alu(&pr, &a);
    a.rgb_dst = a.a_dst = 3; a.rgb_srcp = SRCP_ADD; a.a_srcp = SRCP_INV;
    prog_alu(&pr, &a);
    /* t4 = inline constants: rgb = 0x3b (1.375) * 1 + 0x28 (0.25); alpha = 0x00 (2^-10) + 0x04 (2^-7) */
    a = alu_mov(4, IL(0x3b));
    a.rgb_src[2] = IL(0x28); a.Cc = (Arg){ SEL2, "rgb", 0 };
    a.a_src[0] = IL(0x00); a.a_src[2] = IL(0x04); a.aC = (Arg){ SEL2, "a", 0 };
    prog_alu(&pr, &a);
    a = alu_out(0, T(2)); prog_alu(&pr, &a);
    a = alu_out(1, T(3)); prog_alu(&pr, &a);
    a = alu_out(2, T(4)); prog_alu(&pr, &a);
    prog_finish(&pr);
    CHECK(run_prog(&pr.p, &q, NULL, NULL, NULL, &out), "run");
    for (p = 0; p < 4; p++) {
        CHECKF(out.color[p][0].f[0], 0.5f); CHECKF(out.color[p][0].f[1], 0.0f); CHECKF(out.color[p][0].f[2], -3.0f);
        CHECKF(out.color[p][0].f[3], 3.0f);
        CHECKF(out.color[p][1].f[0], 1.25f); CHECKF(out.color[p][1].f[1], 3.5f); CHECKF(out.color[p][1].f[2], 7.0f);
        CHECKF(out.color[p][1].f[3], -3.0f);
        CHECKF(out.color[p][2].f[0], 1.625f); CHECKF(out.color[p][2].f[1], 1.625f);
        CHECKF(out.color[p][2].f[3], ldexpf(1, -10) + ldexpf(1, -7));
    }
}

static void test_predicates(void)
{
    Prog pr;
    Alu a;
    R500UsQuadIn q = make_quad();
    R500UsQuadOut out;
    int p;
    static const float xs[4] = { -1.0f, 2.0f, 0.0f, -0.5f };

    printf("test_predicates\n");
    for (p = 0; p < 4; p++) {
        q.inputs[p][0] = r5xx_vec4_make(xs[p], 1.0f, -3.0f, xs[p]);
    }
    prog_init(&pr);
    /* t1 = 9 everywhere */
    a = alu_mov(1, IL(0x3f)); a.rgb_src[2] = IL(0x3f); a.Cc = (Arg){ SEL2, "rgb", 0 };
    a.a_src[2] = IL(0x3f); a.aC = (Arg){ SEL2, "a", 0 };     /* 1.875*1 + 1.875 = 3.75 */
    prog_alu(&pr, &a);
    /* ALU: predicate bits from t0: pred.r = (t0.r < 0), pred.g = (t0.g == 0), pred.a = (t0.a >= 0) */
    a = alu_mov(0, T(0)); a.wmask = 0; a.omask = 0x8 | 0x3;
    a.rgb_target = CMP_LT; a.a_target = CMP_GE;
    prog_alu(&pr, &a);
    /* pred.g should be 0 (t0.g = 1 not < 0). Now predicated writes:
     * t1.rgb = t0.rgb if pred.r (RRRR), t1.a = t0.a if pred.a inverted */
    a = alu_mov(1, T(0));
    a.rgb_ps = PS_RRRR; a.a_ps = PS_AAAA; a.a_pinv = 1;
    prog_alu(&pr, &a);
    /* t2 = t0 with per-channel RGBA predication (r from pred.r, g from pred.g, b from pred.b) */
    a = alu_mov(2, IL(0x38)); prog_alu(&pr, &a);
    a = alu_mov(2, T(0)); a.rgb_ps = PS_RGBA; a.a_ps = PS_RGBA; prog_alu(&pr, &a);
    a = alu_out(0, T(1)); prog_alu(&pr, &a);
    a = alu_out(1, T(2)); prog_alu(&pr, &a);
    prog_finish(&pr);
    CHECK(run_prog(&pr.p, &q, NULL, NULL, NULL, &out), "run");
    for (p = 0; p < 4; p++) {
        bool neg = xs[p] < 0;
        CHECKF(out.color[p][0].f[0], neg ? xs[p] : 3.75f);
        CHECKF(out.color[p][0].f[1], neg ? 1.0f : 3.75f);
        CHECKF(out.color[p][0].f[2], neg ? -3.0f : 3.75f);
        /* alpha written when !(t0.a >= 0) i.e. negative */
        CHECKF(out.color[p][0].f[3], neg ? xs[p] : 3.75f);
        CHECKF(out.color[p][1].f[0], neg ? xs[p] : 1.0f);
        CHECKF(out.color[p][1].f[1], 1.0f);                 /* pred.g = 0 */
        CHECKF(out.color[p][1].f[2], 1.0f);                 /* pred.b untouched = 0 */
        CHECKF(out.color[p][1].f[3], neg ? 1.0f : xs[p]);   /* pred.a = t0.a >= 0 */
    }
}

static void test_if_else(void)
{
    Prog pr;
    Alu a;
    R500UsQuadIn q = make_quad();
    R500UsQuadOut out;
    int p, k, if0, else0, if1, else1;
    static const float xs[3][4] = { { -1, 2, -3, 4 }, { -1, -2, -3, -4 }, { 1, 2, 3, 4 } };

    printf("test_if_else\n");
    prog_init(&pr);
    /* if (t0.x < 0) { if (t0.y < 0) t1 = 1 else t1 = 2 } else t1 = 3 */
    a = alu_result_x(T(0), CMP_LT); prog_alu(&pr, &a);
    if0 = emit_if(&pr, 0x0f);
    a = alu_result_x(T(0), CMP_LT); a.A = (Arg){ SEL0, "ggg", 0 }; prog_alu(&pr, &a);
    if1 = emit_if(&pr, 0x0f);
    a = alu_mov(1, IL(0x38)); prog_alu(&pr, &a);
    else1 = emit_else(&pr);
    a = alu_mov(1, IL(0x40)); prog_alu(&pr, &a);           /* 2.0 */
    emit_endif(&pr, if1, else1);
    else0 = emit_else(&pr);
    a = alu_mov(1, IL(0x44)); prog_alu(&pr, &a);           /* 3.0 */
    emit_endif(&pr, if0, else0);
    a = alu_out(0, T(1)); prog_alu(&pr, &a);
    prog_finish(&pr);
    for (k = 0; k < 3; k++) {
        for (p = 0; p < 4; p++) {
            q.inputs[p][0] = r5xx_vec4_make(xs[k][p], (p & 1) ? -1.0f : 1.0f, 0, 0);
        }
        CHECK(run_prog(&pr.p, &q, NULL, NULL, NULL, &out), "run");
        for (p = 0; p < 4; p++) {
            float expect = xs[k][p] < 0 ? ((p & 1) ? 1.0f : 2.0f) : 3.0f;
            CHECK(feq(out.color[p][0].f[0], expect, 0), "case %d pixel %d: %g expected %g",
                  k, p, out.color[p][0].f[0], expect);
        }
    }
}

static void test_loop_mesa_words(void)
{
    /* rc_compile output for:
     *   MOV temp[0], input[0]; BGNLOOP; SLT temp[1].x, temp[0].x, input[1].x;
     *   IF temp[1].x; BRK; ENDIF; ADD temp[0].x, temp[0].x, input[2].x; ENDLOOP;
     *   MOV output[0], temp[0];                                            */
    static const uint32_t w[9][6] = {
        { 0x00007800, 0x08020000, 0x08020000, 0x00db0220, 0x00c0c000, 0x20490000 },
        { 0x00000402, 0x00000000, 0x10000001, 0x00070000, 0x00000000, 0x00000000 },
        { 0x00800000, 0x08000400, 0x08020080, 0x80db0480, 0x00000000, 0x00c81010 },
        { 0x00000402, 0x00000000, 0x12000f00, 0x00060000, 0x00000000, 0x00000000 },
        { 0x00000402, 0x00000000, 0x1401ff05, 0x00080000, 0x00000000, 0x00000000 },
        { 0x00000402, 0x00000000, 0x01010020, 0x00060000, 0x00000000, 0x00000000 },
        { 0x00000800, 0x08000800, 0x08020080, 0x00db0480, 0x00000000, 0x00481000 },
        { 0x00000402, 0x00000000, 0x1000ff22, 0x00020000, 0x00000000, 0x00000000 },
        { 0x00078005, 0x08020000, 0x08020000, 0x00db0220, 0x00c0c000, 0x20490000 },
    };
    Prog pr;
    R500UsQuadIn q = make_quad();
    R500UsQuadOut out;
    int p, i;
    static const float start[4] = { 10, 3, 20, 5 };
    static const float expect[4] = { 4, 3, 4, 3 };

    printf("test_loop_mesa_words\n");
    prog_init(&pr);
    for (i = 0; i < 9; i++) {
        prog_add(&pr, w[i]);
    }
    prog_finish(&pr);
    for (p = 0; p < 4; p++) {
        q.inputs[p][0] = r5xx_vec4_make(start[p], 0, 0, 0);
        q.inputs[p][1] = r5xx_vec4_make(5, 0, 0, 0);
        q.inputs[p][2] = r5xx_vec4_make(-2, 0, 0, 0);
    }
    CHECK(run_prog(&pr.p, &q, NULL, NULL, NULL, &out), "run");
    for (p = 0; p < 4; p++) {
        CHECK(feq(out.color[p][0].f[0], expect[p], 0), "pixel %d: %g expected %g", p, out.color[p][0].f[0], expect[p]);
    }
    /* iteration limit: input1 = -1000 never breaks; 0xff iterations of -2 */
    for (p = 0; p < 4; p++) {
        q.inputs[p][1] = r5xx_vec4_make(-1000, 0, 0, 0);
    }
    CHECK(run_prog(&pr.p, &q, NULL, NULL, NULL, &out), "run");
    for (p = 0; p < 4; p++) {
        CHECK(feq(out.color[p][0].f[0], start[p] - 2 * 255, 0), "limit pixel %d: %g", p, out.color[p][0].f[0]);
    }
}

static void test_loop_continue_rep_al(void)
{
    Prog pr;
    Alu a;
    R500UsQuadIn q = make_quad();
    R500UsQuadOut out;
    int p, loop, brk, cont, ifb, ifc;
    static const float start[4] = { 0, 2, 4, -3 };
    static const float limit[4] = { 5, 5, 5, 7 };
    static const float expect[4] = { 2, 2, 0, 4 };

    printf("test_loop_continue_rep_al\n");
    prog_init(&pr);
    pr.p.fc_int_const[1] = 3;                       /* REP: 3 iterations */
    pr.p.fc_int_const[2] = 0x010103;                /* LOOP: 3 iterations, aL = 1, step 1 */
    /* t0 = in0; t1 = 0; t3 = in1 (limit)
     * loop { t0.x += 1; if (t0.x >= limit) break; if (t0.x < 3) continue; t1.x += 1 } */
    a = alu_mov(1, IL(0x38)); a.B = (Arg){ SEL0, "000", 0 }; a.aB = (Arg){ SEL0, "0", 0 }; prog_alu(&pr, &a); /* t1 = 0 */
    loop = emit_loop(&pr, 0, 0);
    a = alu_mov(0, T(0)); a.Cc = (Arg){ SEL0, "111", 0 }; a.wmask = 1; prog_alu(&pr, &a);     /* t0.x += 1 */
    /* alu_result = (t0.x - limit) >= 0 : MAD t0.x*1 + (-in1.x) */
    a = alu_result_x(T(0), CMP_GE); a.rgb_src[2] = T(1 + 0); a.rgb_src[2] = T(1);
    a.rgb_src[2] = (Src){ K_T, 1, 0 };
    /* careful: in1 lives in temp 1, which we overwrote; use temp 3 copy below */
    pr.n--;
    a = alu_mov(3, T(1)); prog_alu(&pr, &a);         /* placeholder, fixed up: we need in1 before t1 is cleared */
    pr.n--;
    /* rebuild cleanly */
    pr.n = 0;
    a = alu_mov(3, T(1)); prog_alu(&pr, &a);                                               /* t3 = limit */
    a = alu_mov(1, IL(0x38)); a.B = (Arg){ SEL0, "000", 0 }; a.aB = (Arg){ SEL0, "0", 0 }; prog_alu(&pr, &a); /* t1 = 0 */
    loop = emit_loop(&pr, 0, 0);
    a = alu_mov(0, T(0)); a.Cc = (Arg){ SEL0, "111", 0 }; a.wmask = 1; prog_alu(&pr, &a);   /* t0.x += 1 */
    a = alu_result_x(T(0), CMP_GE); a.rgb_src[2] = T(3); a.Cc = (Arg){ SEL2, "rgb", MOD_NEG }; prog_alu(&pr, &a);
    ifb = emit_if(&pr, 0x0f);
    brk = emit_break(&pr, 1, 0, 0xff);
    emit_endif(&pr, ifb, -1);
    a = alu_result_x(T(0), CMP_LT); a.rgb_src[2] = IL(0x44); a.Cc = (Arg){ SEL2, "rgb", MOD_NEG }; prog_alu(&pr, &a); /* t0.x - 3 < 0 */
    ifc = emit_if(&pr, 0x0f);
    cont = emit_continue(&pr, 1, 0xff);
    emit_endif(&pr, ifc, -1);
    a = alu_mov(1, T(1)); a.Cc = (Arg){ SEL0, "111", 0 }; a.wmask = 1; prog_alu(&pr, &a);   /* t1.x += 1 */
    emit_endloop(&pr, loop, 0, &brk, 1, &cont, 1);
    /* REP 3 times: t1.y += 1 */
    loop = emit_loop(&pr, 1, 1);
    a = alu_mov(1, T(1)); a.Cc = (Arg){ SEL0, "111", 0 }; a.wmask = 2; prog_alu(&pr, &a);
    emit_endloop(&pr, loop, 1, NULL, 0, NULL, 0);
    /* LOOP with aL = 1..3: t[4 + aL].x = aL-th write: t[4+aL] = 1 */
    loop = emit_loop(&pr, 0, 2);
    a = alu_mov(4, IL(0x38)); a.rgb_dst_rel = 1; a.a_dst_rel = 1; prog_alu(&pr, &a);
    emit_endloop(&pr, loop, 0, NULL, 0, NULL, 0);
    a = alu_out(0, T(1)); prog_alu(&pr, &a);
    a = alu_out(1, T(5)); prog_alu(&pr, &a);
    a = alu_out(2, T(7)); prog_alu(&pr, &a);
    a = alu_out(3, T(8)); prog_alu(&pr, &a);
    prog_finish(&pr);
    for (p = 0; p < 4; p++) {
        q.inputs[p][0] = r5xx_vec4_make(start[p], 0, 0, 0);
        q.inputs[p][1] = r5xx_vec4_make(limit[p], 0, 0, 0);
        q.inputs[p][4] = q.inputs[p][5] = q.inputs[p][6] = q.inputs[p][7] = q.inputs[p][8] = r5xx_vec4_make(0, 0, 0, 0);
    }
    CHECK(run_prog(&pr.p, &q, NULL, NULL, NULL, &out), "run");
    for (p = 0; p < 4; p++) {
        CHECK(feq(out.color[p][0].f[0], expect[p], 0), "pixel %d: t1.x = %g expected %g", p, out.color[p][0].f[0], expect[p]);
        CHECK(feq(out.color[p][0].f[1], 3.0f, 0), "pixel %d: rep count %g", p, out.color[p][0].f[1]);
        CHECKF(out.color[p][1].f[0], 1.0f);     /* t5 */
        CHECKF(out.color[p][2].f[0], 1.0f);     /* t7 */
        CHECKF(out.color[p][3].f[0], 0.0f);     /* t8 untouched */
    }
}

static void test_call_ret(void)
{
    Prog pr;
    Alu a;
    Fc f;
    R500UsQuadIn q = make_quad();
    R500UsQuadOut out;
    int p, call;

    printf("test_call_ret\n");
    prog_init(&pr);
    /* 0: t1 = 1; 1: alu_result = in0.x < 0; 2: CALL c sub; 3: t1 += 10 ; 4: OUT  5: sub: t1 *= 2 ; 6: RET */
    a = alu_mov(1, IL(0x38)); prog_alu(&pr, &a);
    a = alu_result_x(T(0), CMP_LT); prog_alu(&pr, &a);
    memset(&f, 0, sizeof(f));
    f.op = FC_JUMP; f.func = 0xf0; f.any = 1; f.a_op = AOP_PUSH; f.b_op1 = BOP_INCR; f.addr = 5;
    call = prog_fc(&pr, &f);
    (void)call;
    a = alu_mov(1, T(1)); a.rgb_src[2] = IL(0x52); a.Cc = (Arg){ SEL2, "rgb", 0 }; prog_alu(&pr, &a); /* + 10: 0x52 = 1.25 * 2^(10-7) */
    a = alu_out(0, T(1)); prog_alu(&pr, &a);
    prog_finish(&pr);                                   /* end = 4 */
    a = alu_mov(1, T(1)); a.B = (Arg){ SEL0, "111", 0 }; a.rgb_src[1] = IL(0x40); a.B = (Arg){ SEL1, "rgb", 0 }; prog_alu(&pr, &a);
    memset(&f, 0, sizeof(f));
    f.op = FC_JUMP; f.func = 0xff; f.a_op = AOP_POP; f.b_op1 = BOP_DECR; f.pop = 1;
    prog_fc(&pr, &f);
    for (p = 0; p < 4; p++) {
        q.inputs[p][0] = r5xx_vec4_make((p & 1) ? -1.0f : 1.0f, 0, 0, 0);
    }
    CHECK(run_prog(&pr.p, &q, NULL, NULL, NULL, &out), "run");
    for (p = 0; p < 4; p++) {
        float expect = (p & 1) ? 12.0f : 11.0f;
        CHECK(feq(out.color[p][0].f[0], expect, 0), "pixel %d: %g expected %g", p, out.color[p][0].f[0], expect);
    }
    /* all pixels call */
    for (p = 0; p < 4; p++) {
        q.inputs[p][0] = r5xx_vec4_make(-1.0f, 0, 0, 0);
    }
    CHECK(run_prog(&pr.p, &q, NULL, NULL, NULL, &out), "run");
    for (p = 0; p < 4; p++) {
        CHECKF(out.color[p][0].f[0], 12.0f);
    }
    /* no pixel calls */
    for (p = 0; p < 4; p++) {
        q.inputs[p][0] = r5xx_vec4_make(1.0f, 0, 0, 0);
    }
    CHECK(run_prog(&pr.p, &q, NULL, NULL, NULL, &out), "run");
    for (p = 0; p < 4; p++) {
        CHECKF(out.color[p][0].f[0], 11.0f);
    }
}

static void test_textures(void)
{
    Prog pr;
    Alu a;
    Tex t;
    FakeTex ft;
    R500UsQuadIn q = make_quad();
    R500UsQuadOut out;
    int p;

    printf("test_textures\n");
    for (p = 0; p < 4; p++) {
        q.inputs[p][0] = r5xx_vec4_make(0.1f * (p + 1), 0.2f * (p + 1), 0.3f, 2.0f);
        q.inputs[p][1] = r5xx_vec4_make(0.01f, 0.02f, 0.03f, 0.04f);
        q.inputs[p][2] = r5xx_vec4_make(0.05f, 0.06f, 0.07f, 0.08f);
        q.inputs[p][3] = r5xx_vec4_make(1, 2, 3, 4);
    }
    prog_init(&pr);
    memset(&t, 0, sizeof(t));
    t.op = TEX_LD; t.unit = 2; t.src = 0; t.dst = 4; t.wmask = 0xf; t.src_swz = "grba"; t.dst_swz = "abgr";
    prog_tex(&pr, &t);
    t.op = TEX_PROJ; t.unit = 1; t.src = 0; t.dst = 5; t.src_swz = "rgba"; t.dst_swz = "rgba";
    prog_tex(&pr, &t);
    t.op = TEX_LODBIAS; t.unit = 3; t.dst = 6;
    prog_tex(&pr, &t);
    t.op = TEX_LOD; t.unit = 4; t.dst = 7; t.wmask = 0x5;      /* r and b only */
    prog_tex(&pr, &t);
    t.op = TEX_DXDY; t.unit = 5; t.dst = 8; t.wmask = 0xf; t.dx = 1; t.dy = 2; t.dx_swz = "gbar"; t.dy_swz = "rgba";
    prog_tex(&pr, &t);
    /* KILL on t3 (all positive -> no kill) and t9 = t0 - in3.x*? -> per pixel */
    t.op = TEX_KILL; t.src = 3; t.dst = 0; t.wmask = 0xf; t.dx = t.dy = 0; t.dx_swz = t.dy_swz = NULL;
    prog_tex(&pr, &t);
    a = alu_mov(9, T(0)); a.rgb_src[2] = IL(0x30); a.Cc = (Arg){ SEL2, "rgb", MOD_NEG };   /* t0 - 0.5 */
    prog_alu(&pr, &a);
    t.op = TEX_KILL; t.src = 9; t.wmask = 0x1;      /* examine only .r */
    prog_tex(&pr, &t);
    a = alu_out(0, T(4)); prog_alu(&pr, &a);
    a = alu_out(1, T(5)); prog_alu(&pr, &a);
    a = alu_out(2, T(6)); prog_alu(&pr, &a);
    a = alu_out(3, T(7)); prog_alu(&pr, &a);
    /* the DXDY result (t8) is checked by its own program below */
    prog_finish(&pr);
    memset(&ft, 0, sizeof(ft));
    CHECK(run_prog(&pr.p, &q, NULL, fake_tex, &ft, &out), "run");
    CHECK(ft.calls == 5, "tex calls %d", ft.calls);
    for (p = 0; p < 4; p++) {
        const r5xx_vec4 *i0 = &q.inputs[p][0];
        /* LD: coord = t0.grba -> out = (2g, 2r, b, 2); dst swizzle abgr -> t4 = (2, b, 2r, 2g) */
        CHECKF(out.color[p][0].f[0], 2.0f);
        CHECKF(out.color[p][0].f[1], i0->f[2]);
        CHECKF(out.color[p][0].f[2], 2.0f * i0->f[0]);
        CHECKF(out.color[p][0].f[3], 2.0f * i0->f[1]);
        /* PROJ: (r/q, g/q, b/q, 1 + unit) */
        CHECKF(out.color[p][1].f[0], i0->f[0] / 2.0f);
        CHECKF(out.color[p][1].f[1], i0->f[1] / 2.0f);
        CHECKF(out.color[p][1].f[3], 2.0f);
        /* LODBIAS: lod = q = 2 */
        CHECKF(out.color[p][2].f[2], 2.0f);
        CHECKF(out.color[p][2].f[3], 5.0f);
        /* LOD with wmask rb: g, a keep input value of temp 7 */
        CHECKF(out.color[p][3].f[0], i0->f[0]);
        CHECKF(out.color[p][3].f[1], q.inputs[p][7].f[1]);
        CHECKF(out.color[p][3].f[2], 2.0f);
        CHECKF(out.color[p][3].f[3], q.inputs[p][7].f[3]);
        /* kill: t9.r = t0.r - 0.5 < 0 for p = 0..3 (0.1..0.4) -> all killed */
    }
    CHECK(out.killed == 0xf, "killed %x", out.killed);
    /* DXDY request content */
    CHECK(ft.last.unit == 5 || ft.last.unit == 5, "unit");
    /* rerun to inspect the DXDY request: the last call before KILLs was DXDY */
    {
        Prog pr2;
        prog_init(&pr2);
        t.op = TEX_DXDY; t.unit = 5; t.src = 0; t.dst = 8; t.wmask = 0xf; t.dx = 1; t.dy = 2; t.dx_swz = "gbar"; t.dy_swz = "rgba";
        prog_tex(&pr2, &t);
        a = alu_out(0, T(8)); prog_alu(&pr2, &a);
        prog_finish(&pr2);
        memset(&ft, 0, sizeof(ft));
        CHECK(run_prog(&pr2.p, &q, NULL, fake_tex, &ft, &out), "run");
        CHECK(ft.last.op == R5XX_TEXOP_GRAD && ft.last.unit == 5, "grad request");
        for (p = 0; p < 4; p++) {
            CHECKF(ft.last.ddx[p].f[0], 0.02f);   /* in1.g */
            CHECKF(ft.last.ddx[p].f[3], 0.01f);   /* in1.r */
            CHECKF(ft.last.ddy[p].f[1], 0.06f);
            CHECKF(out.color[p][0].f[0], 0.02f);
            CHECKF(out.color[p][0].f[1], 0.06f);
            CHECKF(out.color[p][0].f[3], 9.0f);
        }
        CHECK(out.killed == 0, "no kill");
    }
    /* per-pixel kill with -0.0 treated as negative, and ignore_uncovered FC */
    {
        Prog pr3;
        static const float xs[4] = { 1.0f, -0.0f, 0.0f, -2.0f };
        prog_init(&pr3);
        for (p = 0; p < 4; p++) {
            q.inputs[p][0] = r5xx_vec4_make(5.0f, xs[p], 5.0f, 5.0f);
        }
        t.op = TEX_KILL; t.src = 0; t.wmask = 0xf;
        prog_tex(&pr3, &t);
        a = alu_out(0, T(0)); prog_alu(&pr3, &a);
        prog_finish(&pr3);
        CHECK(run_prog(&pr3.p, &q, NULL, fake_tex, &ft, &out), "run");
        CHECK(out.killed == 0xa, "killed %x expected a", out.killed);
    }
}

static void test_derivatives_mrt_depth(void)
{
    Prog pr;
    Alu a;
    R500UsQuadIn q = make_quad();
    R500UsQuadOut out;
    int p;
    static const float v[4] = { 1.0f, 4.0f, 9.0f, 16.0f };

    printf("test_derivatives_mrt_depth\n");
    for (p = 0; p < 4; p++) {
        q.inputs[p][0] = r5xx_vec4_make(v[p], 2 * v[p], 3 * v[p], 4 * v[p]);
    }
    prog_init(&pr);
    /* MDH: A*B + C with B = -1: topright - topleft */
    a = alu_mov(1, T(0));
    a.rgb_op = RGB_MDH; a.a_op = A_MDH;
    a.B = (Arg){ SEL0, "111", MOD_NEG }; a.aB = (Arg){ SEL0, "1", MOD_NEG };
    a.Cc = (Arg){ SEL0, "rgb", 0 }; a.aC = (Arg){ SEL0, "a", 0 };
    a.nop = 1;
    prog_alu(&pr, &a);
    a.rgb_dst = a.a_dst = 2; a.rgb_op = RGB_MDV; a.a_op = A_MDV;
    prog_alu(&pr, &a);
    a = alu_out(1, T(1)); prog_alu(&pr, &a);
    a = alu_out(2, T(2)); a.w_omask = 1; prog_alu(&pr, &a);     /* depth = t2.a */
    a = alu_out(3, T(0)); a.omask = 0x5; prog_alu(&pr, &a);     /* only r,b */
    prog_finish(&pr);
    CHECK(run_prog(&pr.p, &q, NULL, NULL, NULL, &out), "run");
    for (p = 0; p < 4; p++) {
        CHECKF(out.color[p][1].f[0], 3.0f);
        CHECKF(out.color[p][1].f[1], 6.0f);
        CHECKF(out.color[p][1].f[3], 12.0f);
        CHECKF(out.color[p][2].f[0], 8.0f);
        CHECKF(out.color[p][2].f[2], 24.0f);
        CHECKF(out.color[p][2].f[3], 32.0f);
        CHECKF(out.depth[p], 32.0f);
        CHECKF(out.color[p][3].f[0], v[p]);
        CHECKF(out.color[p][3].f[1], 0.0f);
        CHECKF(out.color[p][3].f[2], 3 * v[p]);
        CHECKF(out.color[p][0].f[0], 0.0f);
    }
    CHECK(out.depth_written, "depth written");
}

static void test_code_offset_wrap(void)
{
    Prog pr;
    Alu a;
    R500UsQuadIn q = make_quad();
    R500UsQuadOut out;
    int p, i;
    uint32_t w[6];

    printf("test_code_offset_wrap\n");
    /* program at addresses 510, 511, 0, 1 using code_offset = 510 */
    prog_init(&pr);
    a = alu_mov(1, IL(0x38)); prog_alu(&pr, &a);              /* t1 = 1 */
    a = alu_result_x(T(0), CMP_LT); prog_alu(&pr, &a);
    i = emit_if(&pr, 0x0f);                                   /* jump addr relative: 4 + offset */
    a = alu_mov(1, IL(0x40)); prog_alu(&pr, &a);              /* t1 = 2 if in0.x < 0 */
    emit_endif(&pr, i, -1);
    a = alu_out(0, T(1)); prog_alu(&pr, &a);
    prog_finish(&pr);                                        /* n = 6 */
    for (i = 0; i < pr.n; i++) {
        memcpy(w, &pr.p.code[i * 6], sizeof(w));
        memset(&pr.p.code[i * 6], 0, sizeof(w));
        memcpy(&pr.p.code[((i + 510) % 512) * 6], w, sizeof(w));
    }
    pr.p.code_offset = 510;
    pr.p.code_addr = (pr.n - 1) << 16;
    for (p = 0; p < 4; p++) {
        q.inputs[p][0] = r5xx_vec4_make((p & 1) ? -1.0f : 1.0f, 0, 0, 0);
    }
    CHECK(run_prog(&pr.p, &q, NULL, NULL, NULL, &out), "run");
    for (p = 0; p < 4; p++) {
        CHECKF(out.color[p][0].f[0], (p & 1) ? 2.0f : 1.0f);
    }
}

static void test_disasm_golden(void)
{
    /* Mesa's r500FragmentProgramDump output for the loop program above. */
    static const uint32_t w[3][6] = {
        { 0x00000402, 0x00000000, 0x10000001, 0x00070000, 0x00000000, 0x00000000 },
        { 0x00800000, 0x08000400, 0x08020080, 0x80db0480, 0x00000000, 0x00c81010 },
        { 0x00007807, 0x02800000, 0x0000e400, 0x00000000, 0x00000000, 0x00000000 },
    };
    static const char expect[] =
        "R500 Fragment Program:\n--------\n"
        "0\t0:CMN_INST   0x00000402:FC    ALU WAIT wmask: NONE omask: NONE\n"
        "\t2:FC_INST    0x10000001:0x00 0 LOOP NONE NONE NONE 0 0 7 IGN_UNC\n"
        "\t3:FC_ADDR    0x00070000:BOOL: 0x00, INT: 0x00, JUMP_ADDR: 7, JMP_GLBL: 0\n"
        "\n"
        "1\t0:CMN_INST   0x00800000:ALU     wmask: NONE omask: NONE\n"
        "\t1:RGB_ADDR   0x08000400:Addr0: 0t, Addr1: 1t, Addr2: 128t, srcp:0\n"
        "\t2:ALPHA_ADDR 0x08020080:Addr0: 128t, Addr1: 128t, Addr2: 128t, srcp:0\n"
        "\t3 RGB_INST:  0x80db0480:rgb_A_src:0 R/0/0 0 rgb_B_src:0 1/1/1 0 targ: 0\n"
        "\t4 ALPHA_INST:0x00000000:MAD dest:0 alp_A_src:0 R 0 alp_B_src:0 R 0 targ 0 w:0\n"
        "\t5 RGBA_INST: 0x00c81010:MAD dest:1 rgb_C_src:1 R/0/0 1 alp_C_src:0 R 0\n"
        "\n"
        "2\t0:CMN_INST   0x00007807:TEX TEX_WAIT    wmask: ARGB omask: NONE\n"
        "\t1:TEX_INST:  0x02800000: id: 0 op:TEXKILL, ACQ,  SCALED\n"
        "\t2:TEX_ADDR:  0x0000e400: src: 0 R/G/B/A dst: 0 R/R/R/R\n"
        "\t3:TEX_DXDY:  0x00000000\n"
        "\n";
    Prog pr;
    char buf[4096], small[40];
    size_t n, m;
    int i;

    printf("test_disasm_golden\n");
    prog_init(&pr);
    for (i = 0; i < 3; i++) {
        prog_add(&pr, w[i]);
    }
    pr.p.code_addr = 2 << 16;
    n = r500_us_disasm(&pr.p, buf, sizeof(buf));
    CHECK(n == strlen(expect), "length %zu vs %zu", n, strlen(expect));
    CHECK(strcmp(buf, expect) == 0, "listing differs:\n%s", buf);
    m = r500_us_disasm(&pr.p, small, sizeof(small));
    CHECK(m == n && strlen(small) == sizeof(small) - 1 && strncmp(small, expect, sizeof(small) - 1) == 0, "truncation");
    m = r500_us_disasm(&pr.p, NULL, 0);
    CHECK(m == n, "size query");
}

/* ------------------------------------------------------------------ */
/* GLSL validation                                                     */
/* ------------------------------------------------------------------ */

static bool validate_glsl(const char *glsl, const char *name)
{
    char path[512], cmd[1280];
    FILE *f;
    int rc;

    if (!g_glslang || !*g_glslang) {
        return true;
    }
    /* Windows (MSYS2): the native test has no /tmp and cmd.exe has no
     * /dev/null */
    snprintf(path, sizeof(path), "%s/r500_us_test_%d_%s.frag",
             getenv("TEMP") ? getenv("TEMP") : "/tmp", (int)getpid(), name);
    f = fopen(path, "w");
    if (!f) {
        return false;
    }
    fputs(glsl, f);
    fclose(f);
#ifdef _WIN32
    snprintf(cmd, sizeof(cmd), "%s -V --target-env vulkan1.3 -S frag -o NUL %s > %s.log 2>&1",
             g_glslang, path, path);
#else
    snprintf(cmd, sizeof(cmd), "%s -V --target-env vulkan1.3 -S frag -o /dev/null %s > %s.log 2>&1",
             g_glslang, path, path);
#endif
    rc = system(cmd);
    if (rc != 0) {
        char line[512];
        snprintf(cmd, sizeof(cmd), "%s.log", path);
        f = fopen(cmd, "r");
        printf("  glslangValidator failed for %s (%s):\n", name, path);
        if (f) {
            while (fgets(line, sizeof(line), f)) {
                printf("    %s", line);
            }
            fclose(f);
        }
        return false;
    }
    unlink(path);
    snprintf(cmd, sizeof(cmd), "%s.log", path);
    unlink(cmd);
    return true;
}

static void check_glsl(const R500UsProgram *p, const char *name)
{
    char err[128];
    R500UsDecoded *d = r500_us_decode(p, err, sizeof(err));
    uint8_t kinds[16] = { 0 };
    char *glsl;

    kinds[1] = 1;   /* cube */
    kinds[2] = 2;   /* 3D */
    kinds[3] = 3;   /* shadow */
    CHECK(d != NULL, "decode %s: %s", name, err);
    if (!d) {
        return;
    }
    glsl = r500_us_to_glsl(d, kinds);
    if (r500_us_uses_flow_control(d)) {
        /* not translated: the device draws these with the CPU interpreter */
        CHECK(glsl == NULL, "glsl emit %s: flow control should fall back", name);
    } else {
        CHECK(glsl != NULL, "glsl emit %s", name);
    }
    if (glsl) {
        /* the complete fragment shader, as the Vulkan backend compiles it,
         * with and without float64 (Apple GPUs through MoltenVK) */
        for (int f64 = 1; f64 >= 0; f64--) {
            char *prefix = rvk_frag_prefix(f64);
            char *src = rvk_frag_source(prefix, glsl);
            CHECK(validate_glsl(src, name), "glsl validate %s%s", name,
                  f64 ? "" : " (no float64)");
            free(src);
            free(prefix);
        }
        free(glsl);
    }
    r500_us_free(d);
}

/* ------------------------------------------------------------------ */
/* Corpus: programs compiled by Mesa's compiler via rc_compile          */
/* ------------------------------------------------------------------ */

static bool parse_rc_output(const char *text, R500UsProgram *p, r5xx_vec4 *consts,
                            char **dump_out)
{
    const char *line = text;
    const char *dump_start = NULL;
    int ninst = 0;

    memset(p, 0, sizeof(*p));
    memset(consts, 0, sizeof(r5xx_vec4) * 256);
    while (line && *line) {
        const char *nl = strchr(line, '\n');
        size_t len = nl ? (size_t)(nl - line) : strlen(line);
        char buf[600];
        unsigned n, w[6];
        if (len >= sizeof(buf)) {
            len = sizeof(buf) - 1;
        }
        memcpy(buf, line, len);
        buf[len] = 0;
        if (sscanf(buf, "inst %u: %x %x %x %x %x %x", &n, &w[0], &w[1], &w[2], &w[3], &w[4], &w[5]) == 7 && n < 512) {
            int i;
            for (i = 0; i < 6; i++) {
                p->code[n * 6 + i] = w[i];
            }
            ninst = n + 1;
        } else if (sscanf(buf, "int_const %u: %x", &n, &w[0]) == 2 && n < 32) {
            p->fc_int_const[n] = w[0];
        } else if (!strncmp(buf, "const ", 6)) {
            float x, y, z, wv;
            if (sscanf(buf, "const %u: %f %f %f %f", &n, &x, &y, &z, &wv) == 5 && n < 256) {
                consts[n] = r5xx_vec4_make(x, y, z, wv);
            }
        } else if (!strncmp(buf, "R500_US_CONFIG", 14)) {
            sscanf(buf, "R500_US_CONFIG %*s = %x", &p->config);
        } else if (!strncmp(buf, "R500_US_PIXSIZE", 15)) {
            sscanf(buf, "R500_US_PIXSIZE %*s = %x", &p->pixsize);
        } else if (!strncmp(buf, "R500_US_FC_CTRL", 15)) {
            sscanf(buf, "R500_US_FC_CTRL %*s = %x", &p->fc_ctrl);
        } else if (!strncmp(buf, "R500_US_CODE_RANGE", 18)) {
            sscanf(buf, "R500_US_CODE_RANGE %*s = %x", &p->code_range);
        } else if (!strncmp(buf, "R500_US_CODE_OFFSET", 19)) {
            sscanf(buf, "R500_US_CODE_OFFSET %*s = %x", &p->code_offset);
        } else if (!strncmp(buf, "R500_US_CODE_ADDR", 17)) {
            sscanf(buf, "R500_US_CODE_ADDR %*s = %x", &p->code_addr);
        } else if (!strcmp(buf, "=== MESA DUMP ===")) {
            dump_start = nl ? nl + 1 : NULL;
        } else if (!strcmp(buf, "=== FINAL PROGRAM ===") && dump_start) {
            size_t dl = line - dump_start;
            *dump_out = malloc(dl + 1);
            memcpy(*dump_out, dump_start, dl);
            (*dump_out)[dl] = 0;
        }
        line = nl ? nl + 1 : NULL;
    }
    if (!*dump_out || ninst == 0) {
        return false;
    }
    return true;
}

static char *read_cmd(const char *cmd)
{
    FILE *f = popen(cmd, "r");
    size_t cap = 65536, n = 0;
    char *buf;
    if (!f) {
        return NULL;
    }
    buf = malloc(cap);
    for (;;) {
        size_t r = fread(buf + n, 1, cap - n - 1, f);
        n += r;
        if (r == 0) {
            break;
        }
        if (n + 1 >= cap) {
            cap *= 2;
            buf = realloc(buf, cap);
        }
    }
    buf[n] = 0;
    pclose(f);
    return buf;
}

static void run_corpus_case(const char *path, const char *name)
{
    char cmd[1024], *text, *dump = NULL;
    R500UsProgram p;
    r5xx_vec4 consts[256];
    char *dis;
    size_t n;
    R500UsQuadIn q = make_quad();
    R500UsQuadOut out;
    FakeTex ft;

    printf("corpus %s\n", name);
    snprintf(cmd, sizeof(cmd), "%s %s 2>&1", g_rc, path);
    text = read_cmd(cmd);
    CHECK(text != NULL, "run rc_compile");
    if (!text) {
        return;
    }
    if (!parse_rc_output(text, &p, consts, &dump)) {
        CHECK(false, "parse rc_compile output for %s", name);
        free(text);
        return;
    }
    n = r500_us_disasm(&p, NULL, 0);
    dis = malloc(n + 1);
    r500_us_disasm(&p, dis, n + 1);
    if (strcmp(dis, dump) != 0) {
        /* show first differing line */
        size_t i = 0;
        while (dis[i] && dump[i] && dis[i] == dump[i]) {
            i++;
        }
        CHECK(false, "disassembly differs from Mesa at byte %zu:\n--- ours ---\n%.300s\n--- mesa ---\n%.300s",
              i, dis + (i > 80 ? i - 80 : 0), dump + (i > 80 ? i - 80 : 0));
    } else {
        g_pass++;
    }
    /* execute (no crash, terminates) and emit GLSL */
    memset(&ft, 0, sizeof(ft));
    CHECK(run_prog(&p, &q, consts, fake_tex, &ft, &out), "execute %s", name);
    check_glsl(&p, name);
    free(dis);
    free(dump);
    free(text);
}

static void run_corpus(void)
{
    DIR *d;
    struct dirent *de;
    char **names = NULL;
    int n = 0, i;

    if (!g_rc || !*g_rc) {
        printf("corpus: rc_compile not available, SKIPPED (build tools/mesa-rc)\n");
        return;
    }
    d = opendir(g_corpus);
    if (!d) {
        printf("corpus: no directory %s\n", g_corpus);
        return;
    }
    while ((de = readdir(d))) {
        size_t l = strlen(de->d_name);
        if (l > 5 && !strcmp(de->d_name + l - 5, ".test")) {
            names = realloc(names, sizeof(char *) * (n + 1));
            names[n++] = strdup(de->d_name);
        }
    }
    closedir(d);
    /* sort for stable output */
    for (i = 0; i < n; i++) {
        int j;
        for (j = i + 1; j < n; j++) {
            if (strcmp(names[i], names[j]) > 0) {
                char *t = names[i]; names[i] = names[j]; names[j] = t;
            }
        }
    }
    for (i = 0; i < n; i++) {
        char path[1024];
        snprintf(path, sizeof(path), "%s/%s", g_corpus, names[i]);
        run_corpus_case(path, names[i]);
        free(names[i]);
    }
    free(names);
    if (n == 0) {
        printf("corpus: no .test files found\n");
    }
}

/* GLSL emission for the hand-written programs: build a few and validate. */
static void test_glsl_handwritten(void)
{
    Prog pr;
    Alu a;
    Tex t;
    int if0, else0, loop, brk;

    printf("test_glsl_handwritten\n");
    /* straight-line ALU + textures of every kind + kill + MRT + depth */
    prog_init(&pr);
    memset(&t, 0, sizeof(t));
    t.op = TEX_LD; t.unit = 0; t.src = 0; t.dst = 4; t.wmask = 0xf; prog_tex(&pr, &t);
    t.op = TEX_PROJ; t.unit = 1; t.dst = 5; prog_tex(&pr, &t);
    t.op = TEX_LODBIAS; t.unit = 2; t.dst = 6; prog_tex(&pr, &t);
    t.op = TEX_LOD; t.unit = 3; t.dst = 7; t.wmask = 5; prog_tex(&pr, &t);
    t.op = TEX_DXDY; t.unit = 4; t.dst = 8; t.wmask = 0xf; t.dx = 1; t.dy = 2; prog_tex(&pr, &t);
    t.op = TEX_KILL; t.src = 1; prog_tex(&pr, &t);
    a = alu_mov(9, T(4)); a.rgb_op = RGB_DP3; a.a_op = A_DP; a.rgb_src[1] = T(5); a.B = (Arg){ SEL1, "rgb", 0 }; prog_alu(&pr, &a);
    a = alu_mov(9, T(4)); a.rgb_op = RGB_SOP; a.a_op = A_RSQ; a.aA = (Arg){ SEL0, "a", MOD_ABS }; a.wmask = 8; prog_alu(&pr, &a);
    a = alu_mov(10, T(6)); a.rgb_op = RGB_MDH; a.a_op = A_MDV; a.B = (Arg){ SEL0, "111", MOD_NEG }; a.aB = (Arg){ SEL0, "1", MOD_NEG }; prog_alu(&pr, &a);
    a = alu_mov(11, T(7)); a.wmask = 0; a.omask = 0xf; a.rgb_target = CMP_LT; a.a_target = CMP_NE; prog_alu(&pr, &a);
    a = alu_mov(11, T(8)); a.rgb_ps = PS_RGBA; a.a_ps = PS_AAAA; a.a_pinv = 1; a.rgb_omod = OMOD_X4; a.clamp_rgb = 1; prog_alu(&pr, &a);
    a = alu_mov(12, T(0)); a.A = (Arg){ SELP, "rgb", 0 }; a.rgb_src[1] = C(5); a.rgb_srcp = SRCP_SUB; a.a_srcp = SRCP_INV; a.aA = (Arg){ SELP, "a", 0 }; prog_alu(&pr, &a);
    a = alu_out(0, T(9)); prog_alu(&pr, &a);
    a = alu_out(1, T(10)); a.w_omask = 1; prog_alu(&pr, &a);
    a = alu_out(2, T(11)); prog_alu(&pr, &a);
    a = alu_out(3, T(12)); prog_alu(&pr, &a);
    prog_finish(&pr);
    check_glsl(&pr.p, "handwritten_alu_tex");

    /* structured control flow: if/else + loop with break */
    prog_init(&pr);
    a = alu_result_x(T(0), CMP_LT); prog_alu(&pr, &a);
    if0 = emit_if(&pr, 0x0f);
    a = alu_mov(1, IL(0x38)); prog_alu(&pr, &a);
    else0 = emit_else(&pr);
    a = alu_mov(1, IL(0x40)); prog_alu(&pr, &a);
    emit_endif(&pr, if0, else0);
    loop = emit_loop(&pr, 0, 0);
    a = alu_mov(1, T(1)); a.Cc = (Arg){ SEL0, "111", 0 }; prog_alu(&pr, &a);
    a = alu_result_x(T(1), CMP_GE); a.rgb_src[2] = IL(0x50); a.Cc = (Arg){ SEL2, "rgb", MOD_NEG }; prog_alu(&pr, &a);
    if0 = emit_if(&pr, 0x0f);
    brk = emit_break(&pr, 1, 0, 0xff);
    emit_endif(&pr, if0, -1);
    emit_endloop(&pr, loop, 0, &brk, 1, NULL, 0);
    a = alu_out(0, T(1)); prog_alu(&pr, &a);
    prog_finish(&pr);
    check_glsl(&pr.p, "handwritten_fc");

    /* unstructured: CALL/RET -> switch-loop fallback */
    prog_init(&pr);
    {
        Fc f;
        a = alu_mov(1, IL(0x38)); prog_alu(&pr, &a);
        memset(&f, 0, sizeof(f));
        f.op = FC_JUMP; f.func = 0xff; f.any = 1; f.a_op = AOP_PUSH; f.b_op1 = BOP_INCR; f.addr = 4;
        prog_fc(&pr, &f);
        a = alu_mov(1, T(1)); a.Cc = (Arg){ SEL0, "111", 0 }; prog_alu(&pr, &a);
        a = alu_out(0, T(1)); prog_alu(&pr, &a);
        prog_finish(&pr);
        a = alu_mov(1, T(1)); a.rgb_src[1] = IL(0x40); a.B = (Arg){ SEL1, "rgb", 0 }; prog_alu(&pr, &a);
        memset(&f, 0, sizeof(f));
        f.op = FC_JUMP; f.func = 0xff; f.a_op = AOP_POP; f.b_op1 = BOP_DECR; f.pop = 1;
        prog_fc(&pr, &f);
    }
    check_glsl(&pr.p, "handwritten_call");
}

int main(int argc, char **argv)
{
    int i;
    g_corpus = "corpus";
    g_glslang = "glslangValidator";
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--rc") && i + 1 < argc) {
            g_rc = argv[++i];
        } else if (!strcmp(argv[i], "--corpus") && i + 1 < argc) {
            g_corpus = argv[++i];
        } else if (!strcmp(argv[i], "--glslang") && i + 1 < argc) {
            g_glslang = argv[++i];
        }
    }
    test_mov_mesa_words();
    test_mad_swizzle_modifiers();
    test_dot_products();
    test_min_max_cnd_cmp_frc();
    test_transcendentals();
    test_presubtract_inline();
    test_predicates();
    test_if_else();
    test_loop_mesa_words();
    test_loop_continue_rep_al();
    test_call_ret();
    test_textures();
    test_derivatives_mrt_depth();
    test_code_offset_wrap();
    test_disasm_golden();
    test_glsl_handwritten();
    run_corpus();
    printf("%d checks passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
