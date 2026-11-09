/*
 * Standalone tests for the R300/R500 vertex program module (r5xx_pvs.c).
 *
 *  1. Corpus round-trip: machine code compiled by Mesa's r300 compiler
 *     (tools/mesa-rc/rc_compile output, checked in as corpus/<name>.out)
 *     is disassembled and the listing must be byte-identical to Mesa's own
 *     r300_vertex_program_dump() output contained in the same file.
 *  2. Corpus execution: each corpus program has an independent C reference
 *     written from the *source* program (corpus/<name>.rc); the
 *     interpreter is run on pseudo-random inputs/constants and compared.
 *  3. Hand-encoded programs for everything Mesa never emits (R500 vector
 *     ops, alternate temporaries, output-replicate-X, JUMP/JSR, constant
 *     base offset, ...), plus decoder error paths and API semantics.
 *  4. Every program also goes through r5xx_pvs_to_glsl() and the result is
 *     validated with glslangValidator (-V --target-env vulkan1.3).
 *
 * usage: pvs_test [-c corpus_dir] [-o build_dir] [-g glslangValidator]
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "r5xx_pvs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <float.h>
#include <dirent.h>
#include <sys/stat.h>

static int g_fail, g_pass;
static const char *g_corpus_dir = "corpus";
static const char *g_build_dir = "build";
static const char *g_glslang = "/usr/bin/glslangValidator";
static bool g_verbose;

#define CHECK(cond, ...) do { \
    if (cond) { g_pass++; } else { \
        g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
        printf(__VA_ARGS__); printf("\n"); } } while (0)

/* ------------------------------------------------------------------ */
/* Assembler macros (mirror Mesa's PVS_OP_DST_OPERAND / PVS_SRC_OPERAND) */
/* ------------------------------------------------------------------ */

enum { T = 0, A0 = 1, OUT = 2, OUTX = 3, ALT = 4, INP = 5 };     /* dst */
enum { ST = 0, SI = 1, SC = 2, SA = 3 };                          /* src */
enum { X = 0, Y = 1, Z = 2, W = 3, K0 = 4, K1 = 5, U = 7 };       /* swz */

#define OP(opc, math, macro, idx, mask, cls, sat) \
    ((uint32_t)(((opc) & 0x3f) | ((math) << 6) | ((macro) << 7) | \
     (((cls) & 0xf) << 8) | (((idx) & 0x7f) << 13) | (((mask) & 0xf) << 20) | \
     ((math) ? ((sat) << 25) : ((sat) << 24))))
#define PRED(sense) ((1u << 26) | ((uint32_t)(sense) << 27))
#define DST_REL(sel) ((1u << 31) | ((uint32_t)(sel) << 29))

#define SRC_(idx, x, y, z, w, cls, neg) \
    ((uint32_t)((((idx) & 0xff) << 5) | ((x) << 13) | ((y) << 16) | \
     ((z) << 19) | ((w) << 22) | (((neg) & 0xf) << 25) | ((cls) & 3)))
/* two-level expansion so that XYZW / ZERO4 can stand for four selects */
#define SRC(...) SRC_(__VA_ARGS__)
#define SRC_ABS   (1u << 3)
#define SRC_REL   (1u << 4)
#define SRC_SEL(s) ((uint32_t)(s) << 29)
#define XYZW X, Y, Z, W
#define ZERO4 K0, K0, K0, K0

/* VE / ME opcodes used by the hand-written tests */
#define VE_DOT_PRODUCT 1
#define VE_MULTIPLY 2
#define VE_ADD 3
#define VE_MULTIPLY_ADD 4
#define VE_DISTANCE_VECTOR 5
#define VE_FRACTION 6
#define VE_MAXIMUM 7
#define VE_MINIMUM 8
#define VE_SET_GREATER_THAN_EQUAL 9
#define VE_SET_LESS_THAN 10
#define VE_MULTIPLYX2_ADD 11
#define VE_MULTIPLY_CLAMP 12
#define VE_FLT2FIX_DX 13
#define VE_FLT2FIX_DX_RND 14
#define VE_PRED_SET_EQ_PUSH 15
#define VE_PRED_SET_GT_PUSH 16
#define VE_PRED_SET_GTE_PUSH 17
#define VE_PRED_SET_NEQ_PUSH 18
#define VE_COND_WRITE_EQ 19
#define VE_COND_WRITE_GT 20
#define VE_COND_WRITE_GTE 21
#define VE_COND_WRITE_NEQ 22
#define VE_COND_MUX_EQ 23
#define VE_COND_MUX_GT 24
#define VE_COND_MUX_GTE 25
#define VE_SET_GREATER_THAN 26
#define VE_SET_EQUAL 27
#define VE_SET_NOT_EQUAL 28
#define ME_EXP_BASE2_DX 1
#define ME_LOG_BASE2_DX 2
#define ME_EXP_BASEE_FF 3
#define ME_LIGHT_COEFF_DX 4
#define ME_POWER_FUNC_FF 5
#define ME_RECIP_DX 6
#define ME_RECIP_FF 7
#define ME_RECIP_SQRT_DX 8
#define ME_RECIP_SQRT_FF 9
#define ME_MULTIPLY 10
#define ME_EXP_BASE2_FULL_DX 11
#define ME_LOG_BASE2_FULL_DX 12
#define ME_POWER_FUNC_FF_CLAMP_B 13
#define ME_POWER_FUNC_FF_CLAMP_B1 14
#define ME_POWER_FUNC_FF_CLAMP_01 15
#define ME_SIN 16
#define ME_COS 17
#define ME_LOG_BASE2_IEEE 18
#define ME_RECIP_IEEE 19
#define ME_RECIP_SQRT_IEEE 20
#define ME_PRED_SET_EQ 21
#define ME_PRED_SET_GT 22
#define ME_PRED_SET_GTE 23
#define ME_PRED_SET_NEQ 24
#define ME_PRED_SET_CLR 25
#define ME_PRED_SET_INV 26
#define ME_PRED_SET_POP 27
#define ME_PRED_SET_RESTORE 28

#define CNTL0(first, pos, last) ((first) | ((pos) << 10) | ((last) << 20))
#define CONST_CNTL(base, max) ((base) | ((max) << 16))
#define FC_LOOP(slot) (2u << (2 * (slot)))
#define FC_JUMP(slot) (1u << (2 * (slot)))
#define FC_JSR(slot)  (3u << (2 * (slot)))

typedef struct Asm {
    R5xxPvsProgram p;
    int n;
} Asm;

static void asm_init(Asm *a, bool r500)
{
    memset(a, 0, sizeof(*a));
    a->p.is_r500 = r500;
}

static void asm_emit(Asm *a, uint32_t op, uint32_t s0, uint32_t s1, uint32_t s2)
{
    uint32_t *w = &a->p.code[a->n * 4];
    w[0] = op; w[1] = s0; w[2] = s1; w[3] = s2;
    a->n++;
}

static void asm_finish(Asm *a)
{
    a->p.code_cntl0 = CNTL0(0, 0, a->n - 1);
    a->p.code_cntl1 = a->n - 1;
}

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static uint32_t g_seed = 12345;

static float frand(float lo, float hi)
{
    g_seed = g_seed * 1664525u + 1013904223u;
    return lo + (hi - lo) * ((g_seed >> 8) & 0xffff) / 65535.0f;
}

static r5xx_vec4 vrand(float lo, float hi)
{
    return r5xx_vec4_make(frand(lo, hi), frand(lo, hi), frand(lo, hi),
                          frand(lo, hi));
}

static float dot4(r5xx_vec4 a, r5xx_vec4 b)
{
    return a.f[0] * b.f[0] + a.f[1] * b.f[1] + a.f[2] * b.f[2] + a.f[3] * b.f[3];
}

static bool feq(float a, float b, float tol)
{
    if (a == b) {
        return true;
    }
    if (isnan(a) || isnan(b) || isinf(a) || isinf(b)) {
        return false;
    }
    return fabsf(a - b) <= tol * (1.0f + fabsf(a) + fabsf(b));
}

static bool veq(const r5xx_vec4 *a, const r5xx_vec4 *b, float tol)
{
    int c;
    for (c = 0; c < 4; c++) {
        if (!feq(a->f[c], b->f[c], tol)) {
            return false;
        }
    }
    return true;
}

static const r5xx_vec4 SENTINEL = { { 12345.0f, -12345.0f, 777.0f, -777.0f } };

static void fill_sentinel(r5xx_vec4 out[R5XX_PVS_MAX_OUTPUTS])
{
    int i;
    for (i = 0; i < R5XX_PVS_MAX_OUTPUTS; i++) {
        out[i] = SENTINEL;
    }
}

static void print_vec(const char *tag, const r5xx_vec4 *v)
{
    printf("  %s = (%g, %g, %g, %g)\n", tag, v->f[0], v->f[1], v->f[2], v->f[3]);
}

/* Compare the interpreter's output vector 'i' with the expectation. */
static void check_out(const char *name, int trial, int i,
                      const r5xx_vec4 out[], const r5xx_vec4 *exp, float tol)
{
    bool ok = veq(&out[i], exp, tol);
    CHECK(ok, "%s trial %d output[%d] mismatch", name, trial, i);
    if (!ok) {
        print_vec("got     ", &out[i]);
        print_vec("expected", exp);
    }
}

static void check_untouched(const char *name, const r5xx_vec4 out[],
                            uint32_t written)
{
    int i;
    for (i = 0; i < R5XX_PVS_MAX_OUTPUTS; i++) {
        if (!(written & (1u << i))) {
            CHECK(memcmp(&out[i], &SENTINEL, sizeof(SENTINEL)) == 0,
                  "%s: unwritten output[%d] was modified", name, i);
        }
    }
}

/* ------------------------------------------------------------------ */
/* GLSL validation                                                     */
/* ------------------------------------------------------------------ */

static void validate_glsl(const char *name, const R5xxPvsDecoded *d)
{
    char path[512], cmd[1200];
    char *glsl = r5xx_pvs_to_glsl(d);
    FILE *f;
    int rc;

    CHECK(glsl != NULL, "%s: r5xx_pvs_to_glsl returned NULL", name);
    if (!glsl) {
        return;
    }
    CHECK(strncmp(glsl, "#version 450", 12) == 0, "%s: missing #version", name);
#ifdef _WIN32
    mkdir(g_build_dir);
#else
    mkdir(g_build_dir, 0777);
#endif
    snprintf(path, sizeof(path), "%s/%s.vert", g_build_dir, name);
    f = fopen(path, "w");
    CHECK(f != NULL, "cannot write %s", path);
    if (f) {
        fputs(glsl, f);
        fclose(f);
        snprintf(cmd, sizeof(cmd),
                 "%s -V --target-env vulkan1.3 -S vert -o %s/%s.spv %s >%s/%s.log 2>&1",
                 g_glslang, g_build_dir, name, path, g_build_dir, name);
        rc = system(cmd);
        CHECK(rc == 0, "%s: GLSL validation failed (see %s/%s.log)", name,
              g_build_dir, name);
        if (rc != 0 || g_verbose) {
            snprintf(cmd, sizeof(cmd), "cat %s/%s.log", g_build_dir, name);
            if (system(cmd) != 0) {
                /* ignore */
            }
        }
    }
    free(glsl);
}

/* ------------------------------------------------------------------ */
/* Corpus file parser (rc_compile output)                              */
/* ------------------------------------------------------------------ */

typedef struct Corpus {
    char name[128];
    R5xxPvsProgram prog;
    int ninst;
    r5xx_vec4 consts[R5XX_PVS_MAX_CONST];
    bool const_imm[R5XX_PVS_MAX_CONST];     /* immediates from Mesa */
    char *dump;                             /* Mesa's listing */
    bool have_regs;
} Corpus;

static bool parse_corpus(const char *path, Corpus *c)
{
    FILE *f = fopen(path, "r");
    char line[1024];
    enum { S_NONE, S_CODE, S_CONST, S_REGS, S_DUMP } sec = S_NONE;
    size_t dump_cap = 0, dump_len = 0;
    const char *base;

    if (!f) {
        return false;
    }
    memset(c, 0, sizeof(*c));
    base = strrchr(path, '/');
    base = base ? base + 1 : path;
    snprintf(c->name, sizeof(c->name), "%s", base);
    if (strlen(c->name) > 4 && !strcmp(c->name + strlen(c->name) - 4, ".out")) {
        c->name[strlen(c->name) - 4] = '\0';
    }
    c->dump = malloc(1);
    c->dump[0] = '\0';

    while (fgets(line, sizeof(line), f)) {
        if (!strncmp(line, "=== ", 4)) {
            if (!strncmp(line, "=== CODE ===", 12)) sec = S_CODE;
            else if (!strncmp(line, "=== CONSTANTS ===", 17)) sec = S_CONST;
            else if (!strncmp(line, "=== REGS ===", 12)) sec = S_REGS;
            else if (!strncmp(line, "=== MESA DUMP ===", 17)) sec = S_DUMP;
            else sec = S_NONE;
            continue;
        }
        switch (sec) {
        case S_CODE: {
            unsigned n, w0, w1, w2, w3;
            if (sscanf(line, "inst %u: %x %x %x %x", &n, &w0, &w1, &w2, &w3) == 5 &&
                n < R5XX_PVS_MAX_INSTR) {
                c->prog.code[n * 4] = w0;
                c->prog.code[n * 4 + 1] = w1;
                c->prog.code[n * 4 + 2] = w2;
                c->prog.code[n * 4 + 3] = w3;
                if ((int)n + 1 > c->ninst) {
                    c->ninst = n + 1;
                }
            } else if (strstr(line, "# target r500-vs")) {
                c->prog.is_r500 = true;
            } else if (strstr(line, "# target r300-vs")) {
                c->prog.is_r500 = false;
            }
            break;
        }
        case S_CONST: {
            unsigned n, h[4];
            if (sscanf(line, "const %u: %*f %*f %*f %*f | %x %x %x %x", &n,
                       &h[0], &h[1], &h[2], &h[3]) == 5 && n < R5XX_PVS_MAX_CONST) {
                int k;
                for (k = 0; k < 4; k++) {
                    float fv;
                    uint32_t u = h[k];
                    memcpy(&fv, &u, 4);
                    c->consts[n].f[k] = fv;
                }
                c->const_imm[n] = true;
            }
            break;
        }
        case S_REGS: {
            char reg[80];
            unsigned v, idx;
            if (line[0] == '#') {
                break;
            }
            if (sscanf(line, "%79s %*s = 0x%x", reg, &v) != 2 &&
                sscanf(line, "%79s %*s %*s = 0x%x", reg, &v) != 2) {
                break;
            }
            c->have_regs = true;
            if (!strcmp(reg, "R300_VAP_PVS_CODE_CNTL_0")) c->prog.code_cntl0 = v;
            else if (!strcmp(reg, "R300_VAP_PVS_CODE_CNTL_1")) c->prog.code_cntl1 = v;
            else if (!strcmp(reg, "R300_VAP_PVS_CONST_CNTL")) c->prog.const_cntl = v;
            else if (!strcmp(reg, "R300_VAP_PVS_FLOW_CNTL_OPC")) c->prog.fc_opc = v;
            else if (sscanf(reg, "R500_VAP_PVS_FLOW_CNTL_ADDRS_LW_%u", &idx) == 1 && idx < 16)
                c->prog.fc_addrs_lw[idx] = v;
            else if (sscanf(reg, "R500_VAP_PVS_FLOW_CNTL_ADDRS_UW_%u", &idx) == 1 && idx < 16)
                c->prog.fc_addrs_uw[idx] = v;
            else if (sscanf(reg, "R300_VAP_PVS_FLOW_CNTL_ADDRS_%u", &idx) == 1 && idx < 16)
                c->prog.fc_addrs[idx] = v;
            else if (sscanf(reg, "R300_VAP_PVS_FLOW_CNTL_LOOP_INDEX_%u", &idx) == 1 && idx < 16)
                c->prog.fc_loop_index[idx] = v;
            break;
        }
        case S_DUMP: {
            size_t l = strlen(line);
            if (dump_len + l + 1 > dump_cap) {
                dump_cap = (dump_len + l + 1) * 2;
                c->dump = realloc(c->dump, dump_cap);
            }
            memcpy(c->dump + dump_len, line, l + 1);
            dump_len += l;
            break;
        }
        default:
            break;
        }
    }
    fclose(f);
    return c->ninst > 0;
}

/* ------------------------------------------------------------------ */
/* Independent references for the corpus programs (from the .rc source) */
/* ------------------------------------------------------------------ */

typedef struct Trial {
    r5xx_vec4 in[R5XX_PVS_MAX_INPUTS];
    r5xx_vec4 consts[R5XX_PVS_MAX_CONST];
    r5xx_vec4 exp[R5XX_PVS_MAX_OUTPUTS];
    uint32_t written;
    float tol;
} Trial;

/* basic_r500.rc / basic_r300.rc */
static void ref_basic(Trial *t, int trial)
{
    const r5xx_vec4 *in = t->in, *c = t->consts;
    r5xx_vec4 t0, t1, o;
    int k;
    (void)trial;
    o = r5xx_vec4_make(dot4(in[0], c[0]), dot4(in[0], c[1]),
                       dot4(in[0], c[2]), dot4(in[0], c[3]));
    t->exp[0] = o;
    for (k = 0; k < 4; k++) t0.f[k] = in[1].f[k] * c[4].f[k];
    for (k = 0; k < 4; k++) t1.f[k] = t0.f[k] - in[2].f[3 - k];
    for (k = 0; k < 4; k++) t->exp[1].f[k] = t1.f[k] * c[5].f[0] + in[3].f[k];
    for (k = 0; k < 2; k++) t->exp[2].f[k] = fminf(t0.f[k], in[1].f[k]);
    for (k = 2; k < 4; k++) t->exp[2].f[k] = fmaxf(t0.f[k], in[1].f[k]);
    for (k = 0; k < 2; k++) t->exp[3].f[k] = in[1].f[k] < c[4].f[k] ? 1.0f : 0.0f;
    for (k = 2; k < 4; k++) t->exp[3].f[k] = in[1].f[k] >= c[4].f[k] ? 1.0f : 0.0f;
    for (k = 0; k < 4; k++) {
        float a = fabsf(in[2].f[k]);
        t->exp[4].f[k] = a - floorf(a);
    }
    t->exp[5] = r5xx_vec4_make(1.0f, in[1].f[1] * in[2].f[1], in[1].f[2], in[2].f[3]);
    t->exp[6].f[0] = in[1].f[0] * in[2].f[0] + in[1].f[1] * in[2].f[1] +
                     in[1].f[2] * in[2].f[2];
    for (k = 1; k < 4; k++) t->exp[6].f[k] = in[1].f[k] - in[2].f[k];
    t->exp[7] = r5xx_vec4_make(in[3].f[1], in[3].f[0], in[3].f[3], in[3].f[2]);
    t->written = 0xff;
}

/* math_r500.rc */
static void ref_math(Trial *t, int trial)
{
    r5xx_vec4 *in = t->in;
    const r5xx_vec4 *c = t->consts;
    float nl, nh, p, x, fl, l;

    /* shape the inputs: LIT needs N.L, N.H and a power */
    in[1] = r5xx_vec4_make(frand(-1.0f, 1.0f), frand(0.05f, 1.0f),
                           frand(-5.0f, 5.0f), frand(-20.0f, 20.0f));
    if (trial == 0) {
        in[1].f[0] = -0.5f;              /* N.L < 0 path */
    }
    in[2] = r5xx_vec4_make(frand(0.1f, 8.0f), frand(-3.14159f, 3.14159f),
                           frand(-2.0f, 2.0f), frand(-2.0f, 2.0f));
    in[3] = vrand(0.1f, 4.0f);

    t->exp[0] = in[0];
    nl = in[1].f[0]; nh = fmaxf(in[1].f[1], 1e-19f); p = in[1].f[3];
    p = fminf(fmaxf(p, -128.0f), 128.0f);
    t->exp[1] = r5xx_vec4_make(1.0f, fmaxf(nl, 0.0f),
                               nl > 0.0f ? powf(nh, p) : 0.0f, 1.0f);
    x = in[2].f[0];
    fl = floorf(x);
    t->exp[2] = r5xx_vec4_make(exp2f(fl), x - fl, exp2f(x), 1.0f);
    l = log2f(x);
    fl = floorf(l);
    t->exp[3] = r5xx_vec4_make(fl, x / exp2f(fl), l, 1.0f);
    t->exp[4] = r5xx_vec4_make(1.0f / in[3].f[0], 1.0f / sqrtf(in[3].f[1]),
                               exp2f(in[2].f[0]), log2f(in[3].f[2]));
    t->exp[5] = r5xx_vec4_make(sinf(in[2].f[1]), cosf(in[2].f[1]),
                               powf(in[3].f[0], in[3].f[3]), c[0].f[0]);
    t->written = 0x3f;
    t->tol = 2e-5f;
}

/* rel_r500.rc */
static void ref_rel(Trial *t, int trial)
{
    r5xx_vec4 *in = t->in;
    const r5xx_vec4 *c = t->consts;
    int a;
    static const float sel[] = { 0.0f, 4.0f, 8.7f, 1.2f, 12.99f, 20.0f };
    in[1].f[0] = sel[trial % 6];
    a = (int)floorf(in[1].f[0]);
    t->exp[0] = r5xx_vec4_make(dot4(in[0], c[2 + a]), dot4(in[0], c[3 + a]),
                               dot4(in[0], c[4 + a]), dot4(in[0], c[5 + a]));
    t->exp[1] = c[1 + a];
    t->exp[2] = c[0];
    t->written = 0x7;
}

/* loop_r500.rc: counter = c0.x; while (counter < c1.x) { acc += in1;
 * counter += c1.y } (the hardware loop runs at most 128 times) */
static void ref_loop(Trial *t, int trial)
{
    r5xx_vec4 *in = t->in, *c = t->consts;
    r5xx_vec4 acc = in[0];
    float counter;
    int n = 0, k;
    static const float limits[] = { 5.0f, 0.0f, 1.0f, 300.0f, 7.5f, 128.0f };

    c[0] = r5xx_vec4_make(0.0f, frand(-1, 1), frand(-1, 1), frand(-1, 1));
    c[1] = r5xx_vec4_make(limits[trial % 6], 1.0f, 0, 0);
    counter = c[0].f[0];
    while (n < 128 && !(counter >= c[1].f[0])) {
        for (k = 0; k < 4; k++) acc.f[k] += in[1].f[k];
        counter += c[1].f[1];
        n++;
    }
    t->exp[0] = acc;
    t->exp[1] = r5xx_vec4_make(counter, c[0].f[1], c[0].f[2], c[0].f[3]);
    t->written = 0x3;
}

/* loop_r300.rc: unconditional hardware loop, count 0xff */
static void ref_loop_r300(Trial *t, int trial)
{
    int k;
    (void)trial;
    for (k = 0; k < 4; k++) {
        t->exp[0].f[k] = t->in[0].f[k] + 255.0f * t->in[1].f[k];
    }
    t->written = 0x1;
    t->tol = 1e-4f;
}

/* branch_r500.rc */
static void ref_branch(Trial *t, int trial)
{
    r5xx_vec4 *in = t->in, *c = t->consts;
    r5xx_vec4 tmp = c[0];
    int k;
    static const float cond[][2] = { {0, 0}, {1, 0}, {0, 1}, {1, 1},
                                     {-2.5f, 3.0f}, {0.0f, -7.0f} };
    in[1] = r5xx_vec4_make(cond[trial % 6][0], cond[trial % 6][1],
                           (trial & 1) ? 1.0f : -1.0f, 2.0f);
    c[2] = r5xx_vec4_make(1.0f, 0.0f, 1.0f, 2.0f);
    in[3] = vrand(-2.0f, 2.0f);
    in[2] = vrand(-2.0f, 2.0f);
    if (trial == 2) {
        in[2].f[1] = 0.0f;        /* CMP with exactly zero: not < 0 */
    }

    t->exp[0] = in[0];
    if (in[1].f[0] != 0.0f) {
        for (k = 0; k < 4; k++) tmp.f[k] += in[2].f[k];
        if (in[1].f[1] != 0.0f) {
            for (k = 0; k < 4; k++) tmp.f[k] *= c[1].f[k];
        }
    } else {
        for (k = 0; k < 4; k++) tmp.f[k] -= in[2].f[k];
    }
    t->exp[1] = tmp;
    for (k = 0; k < 4; k++) {
        t->exp[2].f[k] = in[2].f[k] < 0.0f ? in[3].f[k] : c[1].f[k];
    }
    for (k = 0; k < 2; k++) t->exp[3].f[k] = in[1].f[k] == c[2].f[k] ? 1.0f : 0.0f;
    for (k = 2; k < 4; k++) t->exp[3].f[k] = in[1].f[k] != c[2].f[k] ? 1.0f : 0.0f;
    for (k = 0; k < 2; k++) t->exp[4].f[k] = in[1].f[k] > c[2].f[k] ? 1.0f : 0.0f;
    for (k = 2; k < 4; k++) t->exp[4].f[k] = in[1].f[k] <= c[2].f[k] ? 1.0f : 0.0f;
    for (k = 0; k < 4; k++) t->exp[5].f[k] = r5xx_clampf(in[3].f[k], 0.0f, 1.0f);
    t->written = 0x3f;
}

/* nested_loop_r500.rc */
static void ref_nested_loop(Trial *t, int trial)
{
    r5xx_vec4 *in = t->in, *c = t->consts;
    r5xx_vec4 acc = in[0];
    float i, j;
    int ni = 0, k;
    static const float lim[][2] = { {3, 4}, {0, 5}, {5, 0}, {1, 1}, {2, 7}, {4, 3} };

    c[0] = r5xx_vec4_make(lim[trial % 6][0], lim[trial % 6][1], 0, 0);
    c[1] = r5xx_vec4_make(0.0f, 1.0f, 0, 0);
    for (i = c[1].f[0]; !(i >= c[0].f[0]) && ni < 128; i += c[1].f[1], ni++) {
        int nj = 0;
        for (j = c[1].f[0]; !(j >= c[0].f[1]) && nj < 128; j += c[1].f[1], nj++) {
            for (k = 0; k < 4; k++) acc.f[k] += in[1].f[k];
        }
    }
    t->exp[0] = acc;
    t->written = 0x1;
}

/* mad_macro_r500.rc */
static void ref_mad_macro(Trial *t, int trial)
{
    const r5xx_vec4 *in = t->in;
    int k;
    (void)trial;
    for (k = 0; k < 4; k++) {
        t->exp[0].f[k] = in[0].f[k] * in[1].f[k] + in[2].f[k];
        t->exp[1].f[k] = in[0].f[3 - k] * -in[1].f[k] + fabsf(in[2].f[k]);
    }
    t->written = 0x3;
}

typedef struct RefEntry {
    const char *name;
    void (*ref)(Trial *, int);
    uint32_t const_cntl_override;   /* 0 = use the register from the file */
    bool expect_fc;
} RefEntry;

static const RefEntry refs[] = {
    { "basic_r500", ref_basic, 0, false },
    { "basic_r300", ref_basic, 0, false },
    { "math_r500", ref_math, 0, false },
    /* rc_compile declares no external constants, so Mesa programs
     * MAX_CONST_ADDR = 0; a real driver sets it to the constant count. */
    { "rel_r500", ref_rel, CONST_CNTL(0, 255), false },
    { "loop_r500", ref_loop, 0, true },
    { "loop_r300", ref_loop_r300, 0, true },
    { "branch_r500", ref_branch, 0, false },
    { "nested_loop_r500", ref_nested_loop, 0, true },
    { "mad_macro_r500", ref_mad_macro, 0, false },
};

static const RefEntry *find_ref(const char *name)
{
    size_t i;
    for (i = 0; i < sizeof(refs) / sizeof(refs[0]); i++) {
        if (!strcmp(refs[i].name, name)) {
            return &refs[i];
        }
    }
    return NULL;
}

static void run_corpus_file(const char *path)
{
    Corpus c;
    R5xxPvsDecoded *d;
    char err[256] = "";
    const RefEntry *re;
    size_t need;
    char *buf;
    int trial;

    if (!parse_corpus(path, &c)) {
        CHECK(false, "cannot parse corpus file %s", path);
        return;
    }
    printf("corpus %-20s %3d inst %s\n", c.name, c.ninst,
           c.prog.is_r500 ? "r500" : "r300");
    CHECK(c.have_regs, "%s: no REGS section", c.name);
    CHECK(c.dump[0] != '\0', "%s: no MESA DUMP section", c.name);

    /* 1. disassembly round trip */
    need = r5xx_pvs_disasm(&c.prog, NULL, 0);
    buf = malloc(need + 1);
    r5xx_pvs_disasm(&c.prog, buf, need + 1);
    CHECK(strlen(buf) == need, "%s: disasm length %zu != %zu", c.name,
          strlen(buf), need);
    if (strcmp(buf, c.dump) != 0) {
        CHECK(false, "%s: disassembly differs from Mesa's listing", c.name);
        printf("---- ours ----\n%s---- mesa ----\n%s----\n", buf, c.dump);
    } else {
        g_pass++;
    }
    free(buf);

    /* 2. decode + execute */
    re = find_ref(c.name);
    if (re && re->const_cntl_override) {
        c.prog.const_cntl = re->const_cntl_override;
    }
    d = r5xx_pvs_decode(&c.prog, err, sizeof(err));
    CHECK(d != NULL, "%s: decode failed: %s", c.name, err);
    if (!d) {
        free(c.dump);
        return;
    }
    CHECK(r5xx_pvs_num_instructions(d) == c.ninst, "%s: %d instructions decoded, %d expected",
          c.name, r5xx_pvs_num_instructions(d), c.ninst);
    CHECK(!re || r5xx_pvs_uses_flow_control(d) == re->expect_fc,
          "%s: uses_flow_control mismatch", c.name);
    CHECK(re != NULL, "%s: no execution reference for this corpus program", c.name);

    for (trial = 0; re && trial < 8; trial++) {
        Trial t;
        r5xx_vec4 out[R5XX_PVS_MAX_OUTPUTS];
        int i;
        memset(&t, 0, sizeof(t));
        t.tol = 1e-5f;
        g_seed = 1000 + trial * 7919 + (uint32_t)strlen(c.name);
        for (i = 0; i < R5XX_PVS_MAX_INPUTS; i++) {
            t.in[i] = vrand(-3.0f, 3.0f);
        }
        for (i = 0; i < 64; i++) {
            t.consts[i] = vrand(-2.0f, 2.0f);
        }
        for (i = 0; i < R5XX_PVS_MAX_CONST; i++) {
            if (c.const_imm[i]) {
                t.consts[i] = c.consts[i];
            }
        }
        re->ref(&t, trial);
        fill_sentinel(out);
        r5xx_pvs_run(d, t.consts, t.in, out);
        for (i = 0; i < R5XX_PVS_MAX_OUTPUTS; i++) {
            if (t.written & (1u << i)) {
                check_out(c.name, trial, i, out, &t.exp[i], t.tol);
            }
        }
        check_untouched(c.name, out, t.written);
        CHECK(r5xx_pvs_outputs_written(d) == t.written,
              "%s: outputs_written 0x%x, expected 0x%x", c.name,
              r5xx_pvs_outputs_written(d), t.written);
    }

    /* 3. GLSL */
    validate_glsl(c.name, d);
    r5xx_pvs_free(d);
    free(c.dump);
}

static int cmp_str(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

static void run_corpus(void)
{
    DIR *dir = opendir(g_corpus_dir);
    struct dirent *de;
    char *names[256];
    int n = 0, i;

    CHECK(dir != NULL, "cannot open corpus dir %s", g_corpus_dir);
    if (!dir) {
        return;
    }
    while ((de = readdir(dir)) && n < 256) {
        size_t l = strlen(de->d_name);
        if (l > 4 && !strcmp(de->d_name + l - 4, ".out")) {
            names[n++] = strdup(de->d_name);
        }
    }
    closedir(dir);
    qsort(names, n, sizeof(names[0]), cmp_str);
    CHECK(n >= 9, "expected at least 9 corpus files, found %d", n);
    for (i = 0; i < n; i++) {
        char path[600];
        snprintf(path, sizeof(path), "%s/%s", g_corpus_dir, names[i]);
        run_corpus_file(path);
        free(names[i]);
    }
}

/* ------------------------------------------------------------------ */
/* Hand-encoded programs                                               */
/* ------------------------------------------------------------------ */

typedef struct HandCtx {
    const char *name;
    Asm a;
    R5xxPvsDecoded *d;
    r5xx_vec4 in[R5XX_PVS_MAX_INPUTS];
    r5xx_vec4 consts[R5XX_PVS_MAX_CONST];
    r5xx_vec4 out[R5XX_PVS_MAX_OUTPUTS];
} HandCtx;

static HandCtx *hand_begin(const char *name, bool r500)
{
    HandCtx *h = calloc(1, sizeof(*h));
    int i;
    h->name = name;
    asm_init(&h->a, r500);
    g_seed = 42;
    for (i = 0; i < R5XX_PVS_MAX_INPUTS; i++) {
        h->in[i] = vrand(-4.0f, 4.0f);
    }
    for (i = 0; i < R5XX_PVS_MAX_CONST; i++) {
        h->consts[i] = r5xx_vec4_make(i, i + 0.25f, -i, 100 + i);
    }
    return h;
}

static bool hand_run(HandCtx *h)
{
    char err[256] = "";
    asm_finish(&h->a);
    h->d = r5xx_pvs_decode(&h->a.p, err, sizeof(err));
    CHECK(h->d != NULL, "%s: decode failed: %s", h->name, err);
    if (!h->d) {
        return false;
    }
    fill_sentinel(h->out);
    r5xx_pvs_run(h->d, h->consts, h->in, h->out);
    return true;
}

static void hand_end(HandCtx *h)
{
    if (h->d) {
        validate_glsl(h->name, h->d);
        r5xx_pvs_free(h->d);
    }
    free(h);
}

#define HEXP(h, i, x, y, z, w) do { \
    r5xx_vec4 e_ = r5xx_vec4_make(x, y, z, w); \
    check_out((h)->name, 0, i, (h)->out, &e_, 1e-5f); } while (0)

/* R500 vector ops and modifiers that Mesa does not emit */
static void test_hand_ve_ops(void)
{
    HandCtx *h = hand_begin("hand_ve_ops", true);
    const r5xx_vec4 *in = h->in;
    int k;

    h->in[0] = r5xx_vec4_make(1.0f, -2.0f, 0.0f, 3.5f);
    h->in[1] = r5xx_vec4_make(1.0f, 2.0f, 0.0f, -3.5f);
    h->in[2] = r5xx_vec4_make(0.5f, 0.25f, -0.75f, 2.0f);
    /* o0 = SET_GT(i0, i1) */
    asm_emit(&h->a, OP(VE_SET_GREATER_THAN, 0, 0, 0, 0xf, OUT, 0),
             SRC(0, XYZW, SI, 0), SRC(1, XYZW, SI, 0), SRC(1, ZERO4, SI, 0));
    /* o1 = SET_EQ(i0, i1) ; o2 = SET_NE(i0, i1) */
    asm_emit(&h->a, OP(VE_SET_EQUAL, 0, 0, 1, 0xf, OUT, 0),
             SRC(0, XYZW, SI, 0), SRC(1, XYZW, SI, 0), SRC(1, ZERO4, SI, 0));
    asm_emit(&h->a, OP(VE_SET_NOT_EQUAL, 0, 0, 2, 0xf, OUT, 0),
             SRC(0, XYZW, SI, 0), SRC(1, XYZW, SI, 0), SRC(1, ZERO4, SI, 0));
    /* o3 = 2*i0*i1 + i2 (MULTIPLYX2_ADD); o4 = macro M2X_ADD on temps */
    asm_emit(&h->a, OP(VE_MULTIPLYX2_ADD, 0, 0, 3, 0xf, OUT, 0),
             SRC(0, XYZW, SI, 0), SRC(1, XYZW, SI, 0), SRC(2, XYZW, SI, 0));
    asm_emit(&h->a, OP(VE_ADD, 0, 0, 0, 0xf, T, 0), SRC(0, XYZW, SI, 0),
             SRC(0, ZERO4, SI, 0), SRC(0, ZERO4, SI, 0));
    asm_emit(&h->a, OP(VE_ADD, 0, 0, 1, 0xf, T, 0), SRC(1, XYZW, SI, 0),
             SRC(1, ZERO4, SI, 0), SRC(1, ZERO4, SI, 0));
    asm_emit(&h->a, OP(VE_ADD, 0, 0, 2, 0xf, T, 0), SRC(2, XYZW, SI, 0),
             SRC(2, ZERO4, SI, 0), SRC(2, ZERO4, SI, 0));
    asm_emit(&h->a, OP(1, 0, 1, 4, 0xf, OUT, 0),         /* macro M2X_ADD */
             SRC(0, XYZW, ST, 0), SRC(1, XYZW, ST, 0), SRC(2, XYZW, ST, 0));
    /* o5 = MULTIPLY_CLAMP(i0, i1) -> clamp01(i0*i1) (approximation) */
    asm_emit(&h->a, OP(VE_MULTIPLY_CLAMP, 0, 0, 5, 0xf, OUT, 0),
             SRC(0, XYZW, SI, 0), SRC(1, XYZW, SI, 0), SRC(1, ZERO4, SI, 0));
    /* o6: COND_WRITE_GT: where i0 > 0 write i2, else leave (sentinel) */
    asm_emit(&h->a, OP(VE_COND_WRITE_GT, 0, 0, 6, 0xf, OUT, 0),
             SRC(0, XYZW, SI, 0), SRC(2, XYZW, SI, 0), SRC(2, ZERO4, SI, 0));
    /* o7: COND_MUX_EQ: i0 == 0 ? i1 : i2 */
    asm_emit(&h->a, OP(VE_COND_MUX_EQ, 0, 0, 7, 0xf, OUT, 0),
             SRC(0, XYZW, SI, 0), SRC(1, XYZW, SI, 0), SRC(2, XYZW, SI, 0));
    /* o8: VE saturate on ADD: sat(i0 + i1) ; o9: abs + per-lane negate */
    asm_emit(&h->a, OP(VE_ADD, 0, 0, 8, 0xf, OUT, 1),
             SRC(0, XYZW, SI, 0), SRC(2, XYZW, SI, 0), SRC(2, ZERO4, SI, 0));
    asm_emit(&h->a, OP(VE_ADD, 0, 0, 9, 0xf, OUT, 0),
             SRC(0, XYZW, SI, 0x5) | SRC_ABS, SRC(0, ZERO4, SI, 0), SRC(0, ZERO4, SI, 0));
    /* o10: swizzle constants 0/1 with negate, and "unused" selects read 0 */
    asm_emit(&h->a, OP(VE_ADD, 0, 0, 10, 0xf, OUT, 0),
             SRC(0, K0, K1, U, 6, SI, 0xa), SRC(0, ZERO4, SI, 0), SRC(0, ZERO4, SI, 0));
    /* o11: FLT2FIX_DX_RND -> a0, then relative const read c[1 + a0.x] */
    asm_emit(&h->a, OP(VE_FLT2FIX_DX_RND, 0, 0, 0, 0x1, A0, 0),
             SRC(0, W, W, W, W, SI, 0), SRC(0, ZERO4, SI, 0), SRC(0, ZERO4, SI, 0));
    asm_emit(&h->a, OP(VE_ADD, 0, 0, 11, 0xf, OUT, 0),
             SRC(1, XYZW, SC, 0) | SRC_REL, SRC(0, ZERO4, SI, 0), SRC(0, ZERO4, SI, 0));
    /* o12: DISTANCE_VECTOR, FRACTION on negative, MIN/MAX, DP4 */
    asm_emit(&h->a, OP(VE_DISTANCE_VECTOR, 0, 0, 12, 0xf, OUT, 0),
             SRC(0, XYZW, SI, 0), SRC(2, XYZW, SI, 0), SRC(2, ZERO4, SI, 0));
    asm_emit(&h->a, OP(VE_FRACTION, 0, 0, 13, 0xf, OUT, 0),
             SRC(0, XYZW, SI, 0xf), SRC(0, ZERO4, SI, 0), SRC(0, ZERO4, SI, 0));
    asm_emit(&h->a, OP(VE_DOT_PRODUCT, 0, 0, 14, 0x3, OUT, 0),
             SRC(0, XYZW, SI, 0), SRC(2, XYZW, SI, 0), SRC(2, ZERO4, SI, 0));
    asm_emit(&h->a, OP(VE_MINIMUM, 0, 0, 14, 0x4, OUT, 0),
             SRC(0, XYZW, SI, 0), SRC(1, XYZW, SI, 0), SRC(2, ZERO4, SI, 0));
    asm_emit(&h->a, OP(VE_MAXIMUM, 0, 0, 14, 0x8, OUT, 0),
             SRC(0, XYZW, SI, 0), SRC(1, XYZW, SI, 0), SRC(2, ZERO4, SI, 0));
    h->a.p.const_cntl = CONST_CNTL(0, 255);
    if (!hand_run(h)) {
        hand_end(h);
        return;
    }
    HEXP(h, 0, 0, 0, 0, 1);
    HEXP(h, 1, 1, 0, 1, 0);
    HEXP(h, 2, 0, 1, 0, 1);
    HEXP(h, 3, 2 * 1 + 0.5f, 2 * -4 + 0.25f, -0.75f, 2 * -12.25f + 2.0f);
    HEXP(h, 4, 2 * 1 + 0.5f, 2 * -4 + 0.25f, -0.75f, 2 * -12.25f + 2.0f);
    HEXP(h, 5, 1, 0, 0, 0);
    {
        r5xx_vec4 e = r5xx_vec4_make(0.5f, SENTINEL.f[1], SENTINEL.f[2], 2.0f);
        check_out(h->name, 0, 6, h->out, &e, 1e-5f);
    }
    HEXP(h, 7, 0.5f, 0.25f, 0.0f, 2.0f);
    HEXP(h, 8, 1.0f, 0.0f, 0.0f, 1.0f);
    HEXP(h, 9, -1.0f, 2.0f, -0.0f, 3.5f);
    HEXP(h, 10, 0.0f, -1.0f, 0.0f, -0.0f);
    /* a0.x = round(3.5) = 4 -> c[5] */
    HEXP(h, 11, 5.0f, 5.25f, -5.0f, 105.0f);
    HEXP(h, 12, 1.0f, -2.0f * 0.25f, 0.0f, 2.0f);
    for (k = 0; k < 4; k++) {
        float v = -in[0].f[k];
        CHECK(feq(h->out[13].f[k], v - floorf(v), 1e-6f), "FRC lane %d", k);
    }
    HEXP(h, 14, dot4(in[0], in[2]), dot4(in[0], in[2]), 0.0f, 3.5f);
    CHECK(r5xx_pvs_inputs_used(h->d) == 0x7, "inputs_used = 0x%x",
          r5xx_pvs_inputs_used(h->d));
    CHECK(r5xx_pvs_outputs_written(h->d) == 0x7fff, "outputs_written = 0x%x",
          r5xx_pvs_outputs_written(h->d));
    hand_end(h);
}

/* register classes: alternate temporaries, output replicate-X, input as
 * destination, relative temporary destination via a0.y */
static void test_hand_regclasses(void)
{
    HandCtx *h = hand_begin("hand_regclasses", true);

    h->in[0] = r5xx_vec4_make(1.0f, 2.0f, 3.0f, 4.0f);
    h->in[1] = r5xx_vec4_make(10.0f, 20.0f, 30.0f, 40.0f);
    /* alt[3] = i0 + i1 ; o0 = alt[3] * 2 (via K1+K1) */
    asm_emit(&h->a, OP(VE_ADD, 0, 0, 3, 0xf, ALT, 0),
             SRC(0, XYZW, SI, 0), SRC(1, XYZW, SI, 0), SRC(1, ZERO4, SI, 0));
    asm_emit(&h->a, OP(VE_ADD, 0, 0, 0, 0xf, OUT, 0),
             SRC(3, XYZW, SA, 0), SRC(3, XYZW, SA, 0), SRC(3, ZERO4, SA, 0));
    /* o1 = replicate-X of i1.wzyx -> (40,40,40,40), only .xz written */
    asm_emit(&h->a, OP(VE_ADD, 0, 0, 1, 0x5, OUTX, 0),
             SRC(1, W, Z, Y, X, SI, 0), SRC(1, ZERO4, SI, 0), SRC(1, ZERO4, SI, 0));
    /* input class as destination: i2 = i0 * i1, then o2 = i2 */
    asm_emit(&h->a, OP(VE_MULTIPLY, 0, 0, 2, 0xf, INP, 0),
             SRC(0, XYZW, SI, 0), SRC(1, XYZW, SI, 0), SRC(1, ZERO4, SI, 0));
    asm_emit(&h->a, OP(VE_ADD, 0, 0, 2, 0xf, OUT, 0),
             SRC(2, XYZW, SI, 0), SRC(2, ZERO4, SI, 0), SRC(2, ZERO4, SI, 0));
    /* a0 = floor(i0) = (1,2,3,4); t[5 + a0.y] = t[7] = i1 ; o3 = t[7] */
    asm_emit(&h->a, OP(VE_FLT2FIX_DX, 0, 0, 0, 0xf, A0, 0),
             SRC(0, XYZW, SI, 0), SRC(0, ZERO4, SI, 0), SRC(0, ZERO4, SI, 0));
    asm_emit(&h->a, OP(VE_ADD, 0, 0, 5, 0xf, T, 0) | DST_REL(1),
             SRC(1, XYZW, SI, 0), SRC(1, ZERO4, SI, 0), SRC(1, ZERO4, SI, 0));
    asm_emit(&h->a, OP(VE_ADD, 0, 0, 3, 0xf, OUT, 0),
             SRC(7, XYZW, ST, 0), SRC(7, ZERO4, ST, 0), SRC(7, ZERO4, ST, 0));
    /* o4 = t[4 + a0.z] = t[7] via relative source with ADDR_SEL = z */
    asm_emit(&h->a, OP(VE_ADD, 0, 0, 4, 0xf, OUT, 0),
             SRC(4, XYZW, ST, 0) | SRC_REL | SRC_SEL(2), SRC(7, ZERO4, ST, 0),
             SRC(7, ZERO4, ST, 0));
    /* o[5 + a0.w] = o[9] = i0 (relative output destination) */
    asm_emit(&h->a, OP(VE_ADD, 0, 0, 5, 0xf, OUT, 0) | DST_REL(3),
             SRC(0, XYZW, SI, 0), SRC(0, ZERO4, SI, 0), SRC(0, ZERO4, SI, 0));
    if (!hand_run(h)) {
        hand_end(h);
        return;
    }
    HEXP(h, 0, 22, 44, 66, 88);
    {
        r5xx_vec4 e = r5xx_vec4_make(40.0f, SENTINEL.f[1], 40.0f, SENTINEL.f[3]);
        check_out(h->name, 0, 1, h->out, &e, 1e-5f);
    }
    HEXP(h, 2, 10, 40, 90, 160);
    HEXP(h, 3, 10, 20, 30, 40);
    HEXP(h, 4, 10, 20, 30, 40);
    HEXP(h, 9, 1, 2, 3, 4);
    check_untouched(h->name, h->out, 0x21f);
    hand_end(h);
}

/* constant base offset and relative-address range checks */
static void test_hand_consts(void)
{
    HandCtx *h = hand_begin("hand_consts", true);

    h->in[0] = r5xx_vec4_make(2.0f, -3.0f, 9.0f, 0.0f);
    h->a.p.const_cntl = CONST_CNTL(4, 6);   /* base 4, max relative addr 6 */
    /* o0 = c[1] (absolute -> slot 5) */
    asm_emit(&h->a, OP(VE_ADD, 0, 0, 0, 0xf, OUT, 0),
             SRC(1, XYZW, SC, 0), SRC(1, ZERO4, SC, 0), SRC(1, ZERO4, SC, 0));
    asm_emit(&h->a, OP(VE_FLT2FIX_DX, 0, 0, 0, 0xf, A0, 0),
             SRC(0, XYZW, SI, 0), SRC(0, ZERO4, SI, 0), SRC(0, ZERO4, SI, 0));
    /* o1 = c[1 + a0.x] = c[3] -> slot 7 (in range) */
    asm_emit(&h->a, OP(VE_ADD, 0, 0, 1, 0xf, OUT, 0),
             SRC(1, XYZW, SC, 0) | SRC_REL, SRC(1, ZERO4, SC, 0), SRC(1, ZERO4, SC, 0));
    /* o2 = c[1 + a0.y] = c[-2] -> out of range -> 0 */
    asm_emit(&h->a, OP(VE_ADD, 0, 0, 2, 0xf, OUT, 0),
             SRC(1, XYZW, SC, 0) | SRC_REL | SRC_SEL(1), SRC(1, ZERO4, SC, 0),
             SRC(1, ZERO4, SC, 0));
    /* o3 = c[1 + a0.z] = c[10] > max 6 -> 0 */
    asm_emit(&h->a, OP(VE_ADD, 0, 0, 3, 0xf, OUT, 0),
             SRC(1, XYZW, SC, 0) | SRC_REL | SRC_SEL(2), SRC(1, ZERO4, SC, 0),
             SRC(1, ZERO4, SC, 0));
    /* o4 = c[6 + a0.w] = c[6] -> slot 10 (boundary, in range) */
    asm_emit(&h->a, OP(VE_ADD, 0, 0, 4, 0xf, OUT, 0),
             SRC(6, XYZW, SC, 0) | SRC_REL | SRC_SEL(3), SRC(1, ZERO4, SC, 0),
             SRC(1, ZERO4, SC, 0));
    if (!hand_run(h)) {
        hand_end(h);
        return;
    }
    HEXP(h, 0, 5, 5.25f, -5, 105);
    HEXP(h, 1, 7, 7.25f, -7, 107);
    HEXP(h, 2, 0, 0, 0, 0);
    HEXP(h, 3, 0, 0, 0, 0);
    HEXP(h, 4, 10, 10.25f, -10, 110);
    hand_end(h);
}

/* math engine ops not covered by the corpus */
static void test_hand_me_ops(void)
{
    HandCtx *h = hand_begin("hand_me_ops", true);
    float x = 2.5f, y = -0.25f;

    h->in[0] = r5xx_vec4_make(x, y, 0.0f, 3.0f);
    h->in[1] = r5xx_vec4_make(4.0f, 0.5f, 2.0f, -1.0f);
    asm_emit(&h->a, OP(ME_EXP_BASEE_FF, 1, 0, 0, 0x1, OUT, 0),
             SRC(0, X, X, X, X, SI, 0), SRC(0, ZERO4, SI, 0), SRC(0, ZERO4, SI, 0));
    asm_emit(&h->a, OP(ME_RECIP_IEEE, 1, 0, 0, 0x2, OUT, 0),
             SRC(0, X, X, X, X, SI, 0), SRC(0, ZERO4, SI, 0), SRC(0, ZERO4, SI, 0));
    asm_emit(&h->a, OP(ME_RECIP_SQRT_IEEE, 1, 0, 0, 0x4, OUT, 0),
             SRC(1, X, X, X, X, SI, 0), SRC(0, ZERO4, SI, 0), SRC(0, ZERO4, SI, 0));
    asm_emit(&h->a, OP(ME_LOG_BASE2_IEEE, 1, 0, 0, 0x8, OUT, 0),
             SRC(1, X, X, X, X, SI, 0), SRC(0, ZERO4, SI, 0), SRC(0, ZERO4, SI, 0));
    /* o1: RSQ_DX(|y|), LOG2_FULL(|y|), RECIP_FF(x), ME_MULTIPLY(x, 4) */
    asm_emit(&h->a, OP(ME_RECIP_SQRT_DX, 1, 0, 1, 0x1, OUT, 0),
             SRC(0, Y, Y, Y, Y, SI, 0), SRC(0, ZERO4, SI, 0), SRC(0, ZERO4, SI, 0));
    asm_emit(&h->a, OP(ME_LOG_BASE2_FULL_DX, 1, 0, 1, 0x2, OUT, 0),
             SRC(0, Y, Y, Y, Y, SI, 0), SRC(0, ZERO4, SI, 0), SRC(0, ZERO4, SI, 0));
    asm_emit(&h->a, OP(ME_RECIP_FF, 1, 0, 1, 0x4, OUT, 0),
             SRC(0, X, X, X, X, SI, 0), SRC(0, ZERO4, SI, 0), SRC(0, ZERO4, SI, 0));
    asm_emit(&h->a, OP(ME_MULTIPLY, 1, 0, 1, 0x8, OUT, 0),
             SRC(0, X, X, X, X, SI, 0), SRC(1, X, X, X, X, SI, 0), SRC(0, ZERO4, SI, 0));
    /* o2: POW variants with base y (negative) and exponent i1.z = 2:
     *   FF: |y|^2 ; CLAMP_B: max(y,0)^2 = 0 ; CLAMP_B1: clamp01(y)^2 = 0;
     *   CLAMP_01: clamp01(|y|^2) */
    asm_emit(&h->a, OP(ME_POWER_FUNC_FF, 1, 0, 2, 0x1, OUT, 0),
             SRC(0, Y, Y, Y, Y, SI, 0), SRC(0, ZERO4, SI, 0), SRC(1, Z, Z, Z, Z, SI, 0));
    asm_emit(&h->a, OP(ME_POWER_FUNC_FF_CLAMP_B, 1, 0, 2, 0x2, OUT, 0),
             SRC(0, Y, Y, Y, Y, SI, 0), SRC(0, ZERO4, SI, 0), SRC(1, Z, Z, Z, Z, SI, 0));
    asm_emit(&h->a, OP(ME_POWER_FUNC_FF_CLAMP_B1, 1, 0, 2, 0x4, OUT, 0),
             SRC(0, Y, Y, Y, Y, SI, 0), SRC(0, ZERO4, SI, 0), SRC(1, Z, Z, Z, Z, SI, 0));
    asm_emit(&h->a, OP(ME_POWER_FUNC_FF_CLAMP_01, 1, 0, 2, 0x8, OUT, 0),
             SRC(0, W, W, W, W, SI, 0), SRC(0, ZERO4, SI, 0), SRC(1, Z, Z, Z, Z, SI, 0));
    /* o3: ME saturate: sat(RECIP(0.5)) = 1 ; LOG_DX(0) = (-inf, 1, -inf, 1) */
    asm_emit(&h->a, OP(ME_RECIP_DX, 1, 0, 3, 0x1, OUT, 1),
             SRC(1, Y, Y, Y, Y, SI, 0), SRC(0, ZERO4, SI, 0), SRC(0, ZERO4, SI, 0));
    asm_emit(&h->a, OP(ME_LOG_BASE2_DX, 1, 0, 4, 0xf, OUT, 0),
             SRC(0, Z, Z, Z, Z, SI, 0), SRC(0, ZERO4, SI, 0), SRC(0, ZERO4, SI, 0));
    /* o5: EXP_DX of a negative value */
    asm_emit(&h->a, OP(ME_EXP_BASE2_DX, 1, 0, 5, 0xf, OUT, 0),
             SRC(0, Y, Y, Y, Y, SI, 0), SRC(0, ZERO4, SI, 0), SRC(0, ZERO4, SI, 0));
    /* o6: LIT with Mesa's operand layout, N.L=x, N.H=i1.y, power=i1.z */
    asm_emit(&h->a, OP(ME_LIGHT_COEFF_DX, 1, 0, 6, 0xf, OUT, 0),
             SRC(0, X, K0, K0, K0, SI, 0), SRC(1, Y, K0, K0, K0, SI, 0),
             SRC(1, K0, K0, K0, Z, SI, 0));
    if (!hand_run(h)) {
        hand_end(h);
        return;
    }
    HEXP(h, 0, expf(x), 1.0f / x, 0.5f, 2.0f);
    HEXP(h, 1, 2.0f, -2.0f, 1.0f / x, 10.0f);
    HEXP(h, 2, 0.0625f, 0.0f, 0.0f, 1.0f);
    CHECK(h->out[3].f[0] == 1.0f, "ME saturate");
    CHECK(isinf(h->out[4].f[0]) && h->out[4].f[0] < 0 && h->out[4].f[1] == 1.0f &&
          isinf(h->out[4].f[2]) && h->out[4].f[3] == 1.0f, "LOG_DX(0)");
    HEXP(h, 5, 0.5f, 0.75f, exp2f(y), 1.0f);
    HEXP(h, 6, 1.0f, x, powf(0.5f, 2.0f), 1.0f);
    hand_end(h);
}

/*
 * R500 dual math, as in Apple's fixed-function lighting programs: a vector op
 * whose third source word carries a scalar RSQ into an alt temp.
 *   t4.w = dot(L, L)                        (L = i0 = (3, 4, 0), |L|^2 = 25)
 *   t0.w = dot(N, N)  ||  a0.x = rsq(t4.w)  (N = i1 = (0, 2, 0))
 *   t2   = c0 * 1     ||  a0.w = rsq(t0.w)
 *   o0   = a0.x * L   (normalized L)        o1 = a0.w * N (normalized N)
 */
#define DUAL_SRC_ALT(idx, sel, cls, op, alt, comp) \
    ((uint32_t)(((idx) & 0xff) << 5) | ((uint32_t)(sel) << 13) | \
     ((uint32_t)(alt) << 19) | ((uint32_t)(op) << 21) | ((uint32_t)(comp) << 27) | \
     ((cls) & 3))
#define DUAL_SRC(idx, sel, cls, op, comp) DUAL_SRC_ALT(idx, sel, cls, op, 0, comp)
static void test_hand_dual_math(void)
{
    HandCtx *h = hand_begin("hand_dual_math", true);

    h->in[0] = r5xx_vec4_make(3.0f, 4.0f, 0.0f, 1.0f);
    h->in[1] = r5xx_vec4_make(0.0f, 2.0f, 0.0f, 0.0f);
    asm_emit(&h->a, OP(VE_DOT_PRODUCT, 0, 0, 4, 0x8, T, 0),
             SRC(0, X, Y, Z, K0, SI, 0), SRC(0, X, Y, Z, K0, SI, 0), SRC(0, ZERO4, SI, 0));
    asm_emit(&h->a, OP(VE_DOT_PRODUCT, 0, 0, 0, 0x8, T, 0) | (2u << 27),
             SRC(1, X, Y, Z, K0, SI, 0), SRC(1, X, Y, Z, K0, SI, 0),
             DUAL_SRC(4, W, ST, ME_RECIP_SQRT_FF, 0));
    asm_emit(&h->a, OP(VE_MULTIPLY, 0, 0, 2, 0x7, T, 0) | (2u << 27),
             SRC(0, XYZW, SC, 0), SRC(0, K1, K1, K1, K1, SC, 0),
             DUAL_SRC(0, W, ST, ME_RECIP_SQRT_FF, 3));
    asm_emit(&h->a, OP(VE_MULTIPLY, 0, 0, 0, 0xf, OUT, 0),
             SRC(0, X, X, X, X, SA, 0), SRC(0, XYZW, SI, 0), SRC(0, ZERO4, SI, 0));
    asm_emit(&h->a, OP(VE_MULTIPLY, 0, 0, 1, 0xf, OUT, 0),
             SRC(0, W, W, W, W, SA, 0), SRC(1, XYZW, SI, 0), SRC(0, ZERO4, SI, 0));
    if (!hand_run(h)) {
        hand_end(h);
        return;
    }
    HEXP(h, 0, 0.6f, 0.8f, 0.0f, 0.2f);
    HEXP(h, 1, 0.0f, 1.0f, 0.0f, 0.0f);
    CHECK(r5xx_pvs_to_glsl(h->d) == NULL, "dual math programs stay on the interpreter");
    r5xx_pvs_free(h->d);
    h->d = NULL;
    hand_end(h);
}

/*
 * Dual math into alt temp 1, as in Grapher's GL_NORMALIZE lighting program
 * (third source word 0x19286000): two RSQs into a0.w and a1.w, each read
 * back by its own MUL.  Decoding bits 16-20 as the index put the second one
 * in a8 and left a1 zero: unlit, ambient-only surfaces.
 *   t4.w = dot(L, L)                         (L = (3, 4, 0), 25)
 *   t0.w = dot(N, N)  ||  a0.w = rsq(t4.w)   (N = (0, 2, 0), 4)
 *   t2   = c0 * 1     ||  a1.w = rsq(t0.w)
 *   o0   = a0.w * L   o1 = a1.w * N
 */
static void test_hand_dual_math_alt1(void)
{
    HandCtx *h = hand_begin("hand_dual_math_alt1", true);

    h->in[0] = r5xx_vec4_make(3.0f, 4.0f, 0.0f, 1.0f);
    h->in[1] = r5xx_vec4_make(0.0f, 2.0f, 0.0f, 0.0f);
    asm_emit(&h->a, OP(VE_DOT_PRODUCT, 0, 0, 4, 0x8, T, 0),
             SRC(0, X, Y, Z, K0, SI, 0), SRC(0, X, Y, Z, K0, SI, 0), SRC(0, ZERO4, SI, 0));
    asm_emit(&h->a, OP(VE_DOT_PRODUCT, 0, 0, 0, 0x8, T, 0) | (2u << 27),
             SRC(1, X, Y, Z, K0, SI, 0), SRC(1, X, Y, Z, K0, SI, 0),
             DUAL_SRC_ALT(4, W, ST, ME_RECIP_SQRT_FF, 0, 3));
    asm_emit(&h->a, OP(VE_MULTIPLY, 0, 0, 2, 0x7, T, 0) | (2u << 27),
             SRC(0, XYZW, SC, 0), SRC(0, K1, K1, K1, K1, SC, 0),
             DUAL_SRC_ALT(0, W, ST, ME_RECIP_SQRT_FF, 1, 3));
    asm_emit(&h->a, OP(VE_MULTIPLY, 0, 0, 0, 0xf, OUT, 0),
             SRC(0, W, W, W, W, SA, 0), SRC(0, XYZW, SI, 0), SRC(0, ZERO4, SI, 0));
    asm_emit(&h->a, OP(VE_MULTIPLY, 0, 0, 1, 0xf, OUT, 0),
             SRC(1, W, W, W, W, SA, 0), SRC(1, XYZW, SI, 0), SRC(0, ZERO4, SI, 0));
    if (!hand_run(h)) {
        hand_end(h);
        return;
    }
    HEXP(h, 0, 0.6f, 0.8f, 0.0f, 0.2f);
    HEXP(h, 1, 0.0f, 1.0f, 0.0f, 0.0f);
    r5xx_pvs_free(h->d);
    h->d = NULL;
    hand_end(h);
}

/* predicate ops driven directly, with both predicate senses */
static void test_hand_pred(void)
{
    HandCtx *h = hand_begin("hand_pred", true);

    h->in[0] = r5xx_vec4_make(0.0f, 1.0f, -1.0f, 2.0f);
    /* PRED_SET_GT(i0.y = 1) -> pred = true, t0.w = 0 */
    asm_emit(&h->a, OP(ME_PRED_SET_GT, 1, 0, 0, 0x8, T, 0),
             SRC(0, Y, Y, Y, Y, SI, 0), SRC(0, ZERO4, SI, 0), SRC(0, ZERO4, SI, 0));
    /* o0 = 1 if pred (sense 1); o1 = 1 if !pred (sense 0) */
    asm_emit(&h->a, OP(VE_ADD, 0, 0, 0, 0xf, OUT, 0) | PRED(1),
             SRC(0, K1, K1, K1, K1, SI, 0), SRC(0, ZERO4, SI, 0), SRC(0, ZERO4, SI, 0));
    asm_emit(&h->a, OP(VE_ADD, 0, 0, 1, 0xf, OUT, 0) | PRED(0),
             SRC(0, K1, K1, K1, K1, SI, 0), SRC(0, ZERO4, SI, 0), SRC(0, ZERO4, SI, 0));
    /* PRED_SET_GTE_PUSH: t0.w = (t0.w == 0 && i0.z >= 0) ? 0 : t0.w + 1 -> 1, pred false */
    asm_emit(&h->a, OP(VE_PRED_SET_GTE_PUSH, 0, 0, 0, 0x8, T, 0),
             SRC(0, U, U, U, W, ST, 0), SRC(0, U, U, U, Z, SI, 0), SRC(0, ZERO4, SI, 0));
    asm_emit(&h->a, OP(VE_ADD, 0, 0, 2, 0xf, OUT, 0) | PRED(1),
             SRC(0, K1, K1, K1, K1, SI, 0), SRC(0, ZERO4, SI, 0), SRC(0, ZERO4, SI, 0));
    /* PRED_SET_INV: 1 -> 0, pred true ; o3 written */
    asm_emit(&h->a, OP(ME_PRED_SET_INV, 1, 0, 0, 0x8, T, 0),
             SRC(0, W, W, W, W, ST, 0), SRC(0, ZERO4, SI, 0), SRC(0, ZERO4, SI, 0));
    asm_emit(&h->a, OP(VE_ADD, 0, 0, 3, 0xf, OUT, 0) | PRED(1),
             SRC(0, W, W, W, W, ST, 0), SRC(0, K1, K1, K1, K1, SI, 0), SRC(0, ZERO4, SI, 0));
    /* PRED_SET_CLR -> pred false, t0.w = FLT_MAX; o4 must not be written */
    asm_emit(&h->a, OP(ME_PRED_SET_CLR, 1, 0, 0, 0xf, T, 0),
             SRC(0, ZERO4, SI, 0), SRC(0, ZERO4, SI, 0), SRC(0, ZERO4, SI, 0));
    asm_emit(&h->a, OP(VE_ADD, 0, 0, 4, 0xf, OUT, 0) | PRED(1),
             SRC(0, K1, K1, K1, K1, SI, 0), SRC(0, ZERO4, SI, 0), SRC(0, ZERO4, SI, 0));
    /* PRED_SET_RESTORE from a zero -> pred true; PRED_SET_EQ_PUSH keeps it */
    asm_emit(&h->a, OP(ME_PRED_SET_RESTORE, 1, 0, 0, 0x8, T, 0),
             SRC(0, X, X, X, X, SI, 0), SRC(0, ZERO4, SI, 0), SRC(0, ZERO4, SI, 0));
    asm_emit(&h->a, OP(VE_PRED_SET_EQ_PUSH, 0, 0, 0, 0x8, T, 0),
             SRC(0, U, U, U, W, ST, 0), SRC(0, U, U, U, X, SI, 0), SRC(0, ZERO4, SI, 0));
    asm_emit(&h->a, OP(VE_ADD, 0, 0, 5, 0xf, OUT, 0) | PRED(1),
             SRC(0, W, W, W, W, ST, 0), SRC(0, K1, K1, K1, K1, SI, 0), SRC(0, ZERO4, SI, 0));
    /* PRED_SET_NEQ(0) -> false ; PRED_SET_POP(2) -> 1, pred false */
    asm_emit(&h->a, OP(ME_PRED_SET_NEQ, 1, 0, 1, 0x1, T, 0),
             SRC(0, X, X, X, X, SI, 0), SRC(0, ZERO4, SI, 0), SRC(0, ZERO4, SI, 0));
    asm_emit(&h->a, OP(ME_PRED_SET_POP, 1, 0, 1, 0x2, T, 0),
             SRC(0, W, W, W, W, SI, 0), SRC(0, ZERO4, SI, 0), SRC(0, ZERO4, SI, 0));
    asm_emit(&h->a, OP(VE_ADD, 0, 0, 6, 0xf, OUT, 0) | PRED(0),
             SRC(1, XYZW, ST, 0), SRC(1, ZERO4, ST, 0), SRC(1, ZERO4, ST, 0));
    if (!hand_run(h)) {
        hand_end(h);
        return;
    }
    HEXP(h, 0, 1, 1, 1, 1);
    HEXP(h, 3, 1, 1, 1, 1);
    HEXP(h, 5, 1, 1, 1, 1);
    HEXP(h, 6, 1, 1, 0, 0);
    check_untouched(h->name, h->out, 0x69);
    CHECK(r5xx_pvs_uses_flow_control(h->d) == false, "pred-only program has no FC");
    hand_end(h);
}

/* flow control: JUMP, JSR, LOOP with a count, FIRST_INST != 0 */
static void test_hand_fc(void)
{
    HandCtx *h = hand_begin("hand_fc", true);
    R5xxPvsProgram *p = &h->a.p;

    h->in[0] = r5xx_vec4_make(1.0f, 2.0f, 3.0f, 4.0f);
    /* two garbage instructions before FIRST_INST (would write o0) */
    asm_emit(&h->a, OP(VE_ADD, 0, 0, 0, 0xf, OUT, 0),
             SRC(0, K1, K1, K1, K1, SI, 0), SRC(0, ZERO4, SI, 0), SRC(0, ZERO4, SI, 0));
    asm_emit(&h->a, OP(VE_ADD, 0, 0, 0, 0xf, OUT, 0),
             SRC(0, K1, K1, K1, K1, SI, 0), SRC(0, ZERO4, SI, 0), SRC(0, ZERO4, SI, 0));
    /* 2: t0 = 0 */
    asm_emit(&h->a, OP(VE_ADD, 0, 0, 0, 0xf, T, 0),
             SRC(0, ZERO4, SI, 0), SRC(0, ZERO4, SI, 0), SRC(0, ZERO4, SI, 0));
    /* 3: loop activation point (NOP); 4..5 body: t0 += i0, t1 = t0 */
    asm_emit(&h->a, 0, 0, 0, 0);
    asm_emit(&h->a, OP(VE_ADD, 0, 0, 0, 0xf, T, 0),
             SRC(0, XYZW, ST, 0), SRC(0, XYZW, SI, 0), SRC(0, ZERO4, SI, 0));
    asm_emit(&h->a, OP(VE_ADD, 0, 0, 1, 0xf, T, 0),
             SRC(0, XYZW, ST, 0), SRC(0, ZERO4, ST, 0), SRC(0, ZERO4, ST, 0));
    /* 6: JUMP activation: after this instruction jump to 8 (skipping 7) */
    asm_emit(&h->a, OP(VE_ADD, 0, 0, 1, 0xf, OUT, 0),
             SRC(1, XYZW, ST, 0), SRC(1, ZERO4, ST, 0), SRC(1, ZERO4, ST, 0));
    asm_emit(&h->a, OP(VE_ADD, 0, 0, 1, 0xf, OUT, 0),          /* 7: skipped */
             SRC(0, K1, K1, K1, K1, SI, 0), SRC(0, ZERO4, SI, 0), SRC(0, ZERO4, SI, 0));
    /* 8: JSR activation: call 11..12 then return to 9 */
    asm_emit(&h->a, OP(VE_ADD, 0, 0, 2, 0xf, T, 0),
             SRC(0, K1, K1, K1, K1, SI, 0), SRC(0, ZERO4, SI, 0), SRC(0, ZERO4, SI, 0));
    /* 9: o2 = t2 ; 10: JUMP over the subroutine to 13 */
    asm_emit(&h->a, OP(VE_ADD, 0, 0, 2, 0xf, OUT, 0),
             SRC(2, XYZW, ST, 0), SRC(2, ZERO4, ST, 0), SRC(2, ZERO4, ST, 0));
    asm_emit(&h->a, 0, 0, 0, 0);
    /* 11..12: subroutine: t2 = t2 + i0 ; t2 = t2 * 2 */
    asm_emit(&h->a, OP(VE_ADD, 0, 0, 2, 0xf, T, 0),
             SRC(2, XYZW, ST, 0), SRC(0, XYZW, SI, 0), SRC(0, ZERO4, SI, 0));
    asm_emit(&h->a, OP(VE_ADD, 0, 0, 2, 0xf, T, 0),
             SRC(2, XYZW, ST, 0), SRC(2, XYZW, ST, 0), SRC(2, ZERO4, ST, 0));
    /* 13: o3 = i0 (end) */
    asm_emit(&h->a, OP(VE_ADD, 0, 0, 3, 0xf, OUT, 0),
             SRC(0, XYZW, SI, 0), SRC(0, ZERO4, SI, 0), SRC(0, ZERO4, SI, 0));
    asm_finish(&h->a);
    p->code_cntl0 = CNTL0(2, 2, 13);
    /* slot 0: LOOP act 3, count 5, body 4..5 */
    p->fc_opc = FC_LOOP(0) | FC_JUMP(1) | FC_JSR(2) | FC_JUMP(3);
    p->fc_addrs_lw[0] = 3 | (5u << 16);
    p->fc_addrs_uw[0] = 5 | (4u << 16);
    p->fc_loop_index[0] = 0 | (1u << 8);
    /* slot 1: JUMP at 6 -> 8 */
    p->fc_addrs_lw[1] = 6 | (8u << 16);
    /* slot 2: JSR at 8 -> 11, last 12, rtn 9 */
    p->fc_addrs_lw[2] = 8 | (11u << 16);
    p->fc_addrs_uw[2] = 12 | (9u << 16);
    /* slot 3: JUMP at 10 -> 13 */
    p->fc_addrs_lw[3] = 10 | (13u << 16);
    {
        char err[256] = "";
        h->d = r5xx_pvs_decode(p, err, sizeof(err));
        CHECK(h->d != NULL, "%s: decode failed: %s", h->name, err);
        if (!h->d) {
            hand_end(h);
            return;
        }
        fill_sentinel(h->out);
        r5xx_pvs_run(h->d, h->consts, h->in, h->out);
    }
    CHECK(r5xx_pvs_num_instructions(h->d) == 12, "num_instructions %d",
          r5xx_pvs_num_instructions(h->d));
    CHECK(r5xx_pvs_uses_flow_control(h->d), "uses_flow_control");
    HEXP(h, 1, 5, 10, 15, 20);              /* loop ran 5 times */
    HEXP(h, 2, 4, 6, 8, 10);                /* (1 + i0) * 2 */
    HEXP(h, 3, 1, 2, 3, 4);
    check_untouched(h->name, h->out, 0xe);
    {
        /* disassembly of a program with all FC kinds, snprintf semantics */
        char small[64];
        size_t full = r5xx_pvs_disasm(p, NULL, 0);
        size_t got = r5xx_pvs_disasm(p, small, sizeof(small));
        CHECK(full > 500 && got == full, "disasm sizes %zu %zu", full, got);
        CHECK(strlen(small) == sizeof(small) - 1, "disasm truncation");
        CHECK(strncmp(small, "Final vertex program code:\n0: op:", 33) == 0,
              "disasm header");
    }
    hand_end(h);
}

/* R300 flavour: fc register layout decode + no-saturate bits */
static void test_hand_r300(void)
{
    HandCtx *h = hand_begin("hand_r300", false);
    R5xxPvsProgram *p = &h->a.p;
    char *buf;

    h->in[0] = r5xx_vec4_make(1.0f, 2.0f, 3.0f, 4.0f);
    asm_emit(&h->a, OP(VE_ADD, 0, 0, 0, 0xf, T, 0),
             SRC(0, ZERO4, SI, 0), SRC(0, ZERO4, SI, 0), SRC(0, ZERO4, SI, 0));
    asm_emit(&h->a, 0, 0, 0, 0);
    asm_emit(&h->a, OP(VE_ADD, 0, 0, 0, 0xf, T, 0),
             SRC(0, XYZW, ST, 0), SRC(0, XYZW, SI, 0), SRC(0, ZERO4, SI, 0));
    asm_emit(&h->a, OP(VE_ADD, 0, 0, 0, 0xf, OUT, 0),
             SRC(0, XYZW, ST, 0), SRC(0, ZERO4, ST, 0), SRC(0, ZERO4, ST, 0));
    asm_finish(&h->a);
    p->fc_opc = FC_LOOP(0);
    /* act 1, count 7, last 2, rtn 2 */
    p->fc_addrs[0] = 1 | (7u << 8) | (2u << 16) | (2u << 24);
    p->fc_loop_index[0] = 0x100;
    {
        char err[256] = "";
        h->d = r5xx_pvs_decode(p, err, sizeof(err));
        CHECK(h->d != NULL, "%s: decode failed: %s", h->name, err);
        if (!h->d) {
            hand_end(h);
            return;
        }
        fill_sentinel(h->out);
        r5xx_pvs_run(h->d, h->consts, h->in, h->out);
    }
    HEXP(h, 0, 7, 14, 21, 28);
    buf = malloc(8192);
    r5xx_pvs_disasm(p, buf, 8192);
    CHECK(strstr(buf, "LOOP: 0x02020701\n") != NULL, "r300 FC listing: %s",
          strstr(buf, "Flow Control"));
    free(buf);
    hand_end(h);
}

/* decoder error paths and misc API behavior */
static void test_decode_errors(void)
{
    R5xxPvsProgram p;
    R5xxPvsDecoded *d;
    char err[128];

    memset(&p, 0, sizeof(p));
    p.is_r500 = true;
    p.code_cntl0 = CNTL0(5, 0, 3);
    d = r5xx_pvs_decode(&p, err, sizeof(err));
    CHECK(d == NULL && strstr(err, "last") != NULL, "last < first must fail");

    memset(&p, 0, sizeof(p));
    p.is_r500 = false;
    p.code_cntl0 = CNTL0(0, 0, 300);
    d = r5xx_pvs_decode(&p, err, sizeof(err));
    CHECK(d == NULL, "R300 program beyond 256 lines must fail");
    p.is_r500 = true;
    d = r5xx_pvs_decode(&p, err, sizeof(err));
    CHECK(d != NULL, "R500 program of 301 lines must decode");
    r5xx_pvs_free(d);

    memset(&p, 0, sizeof(p));
    p.is_r500 = true;
    p.code[0] = OP(30, 0, 0, 0, 0xf, OUT, 0);       /* reserved VE opcode */
    d = r5xx_pvs_decode(&p, err, sizeof(err));
    CHECK(d == NULL && strstr(err, "reserved") != NULL, "reserved VE opcode");
    p.code[0] = OP(29, 1, 0, 0, 0xf, OUT, 0);       /* reserved ME opcode */
    d = r5xx_pvs_decode(&p, err, sizeof(err));
    CHECK(d == NULL, "reserved ME opcode");
    p.code[0] = OP(VE_ADD, 0, 0, 0, 0xf, 7, 0);     /* bad dst class */
    d = r5xx_pvs_decode(&p, err, sizeof(err));
    CHECK(d == NULL, "bad destination class");
    p.code[0] = OP(VE_ADD, 0, 0, 0, 0xf, OUT, 0);
    d = r5xx_pvs_decode(&p, NULL, 0);               /* err may be NULL */
    CHECK(d != NULL, "decode with NULL err");
    CHECK(r5xx_pvs_num_instructions(d) == 1, "single instruction");
    CHECK(r5xx_pvs_inputs_used(d) == 0 && r5xx_pvs_outputs_written(d) == 1,
          "masks of a single MOV to o0");
    {
        /* run with NULL inputs/consts must not crash; output written as 0 */
        r5xx_vec4 out[R5XX_PVS_MAX_OUTPUTS];
        fill_sentinel(out);
        r5xx_pvs_run(d, NULL, NULL, out);
        CHECK(out[0].f[0] == 0.0f && out[0].f[3] == 0.0f, "NULL operands read 0");
        check_untouched("null-run", out, 1);
    }
    r5xx_pvs_free(d);
    r5xx_pvs_free(NULL);
    CHECK(r5xx_pvs_decode(NULL, err, sizeof(err)) == NULL, "NULL program");
    CHECK(r5xx_pvs_num_instructions(NULL) == 0, "NULL decoded");
    CHECK(r5xx_pvs_to_glsl(NULL) == NULL, "NULL glsl");
    CHECK(r5xx_pvs_disasm(NULL, NULL, 0) == 0, "NULL disasm");
}

/* A runaway program (loop whose count never ends through a JUMP cycle)
 * must terminate thanks to the step guard. */
static void test_runaway(void)
{
    HandCtx *h = hand_begin("hand_runaway", true);
    R5xxPvsProgram *p = &h->a.p;

    asm_emit(&h->a, OP(VE_ADD, 0, 0, 0, 0xf, OUT, 0),
             SRC(0, XYZW, SI, 0), SRC(0, ZERO4, SI, 0), SRC(0, ZERO4, SI, 0));
    asm_emit(&h->a, 0, 0, 0, 0);
    asm_finish(&h->a);
    p->fc_opc = FC_JUMP(0);
    p->fc_addrs_lw[0] = 1 | (0u << 16);   /* at 1 jump back to 0 forever */
    {
        char err[256] = "";
        h->d = r5xx_pvs_decode(p, err, sizeof(err));
        CHECK(h->d != NULL, "%s: decode failed: %s", h->name, err);
        if (h->d) {
            fill_sentinel(h->out);
            r5xx_pvs_run(h->d, h->consts, h->in, h->out);
            CHECK(veq(&h->out[0], &h->in[0], 0), "runaway program still produced o0");
        }
    }
    hand_end(h);
}

/* ------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    int i;
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-c") && i + 1 < argc) g_corpus_dir = argv[++i];
        else if (!strcmp(argv[i], "-o") && i + 1 < argc) g_build_dir = argv[++i];
        else if (!strcmp(argv[i], "-g") && i + 1 < argc) g_glslang = argv[++i];
        else if (!strcmp(argv[i], "-v")) g_verbose = true;
        else {
            fprintf(stderr, "usage: %s [-c corpus_dir] [-o build_dir] [-g glslangValidator] [-v]\n",
                    argv[0]);
            return 2;
        }
    }

    run_corpus();
    test_hand_ve_ops();
    test_hand_regclasses();
    test_hand_consts();
    test_hand_me_ops();
    test_hand_dual_math();
    test_hand_dual_math_alt1();
    test_hand_pred();
    test_hand_fc();
    test_hand_r300();
    test_decode_errors();
    test_runaway();

    printf("pvs_test: %d checks passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
