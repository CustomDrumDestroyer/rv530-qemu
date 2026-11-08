/*
 * Evergreen pixel shader -> GLSL, for the Vulkan backend (eg_vk.h).
 *
 * A static translation of what eg_shader.c interprets.  One fragment
 * shader invocation is one lane, so the exec mask, predicate, kill flag,
 * address register and loop index become per-invocation variables, and
 * the CF stack becomes bools whose depth is known at every point of the
 * program.
 *
 * Control flow: the CF program is translated in order.  JUMP, ELSE and
 * PUSH jump only when no lane of the quad would execute the code they skip,
 * and the stack pops they do then are the ones the skipped code would have
 * done, so running straight through (with the lanes inactive) computes
 * the same thing; that keeps all lanes of a quad together, as texture
 * derivatives need.  Loops become GLSL loops whose exit is decided for the
 * whole quad.  Branches on boolean constants and CALL are not translated
 * (the CPU path draws those).
 *
 * Pure C; the operation semantics mirror eg_shader.c exec_group_lane().
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdbool.h>
#include <ctype.h>
#include "eg_vk.h"

#define EVK_PS_BUILD_ID     4       /* bump when the generated code changes */
#define MAX_CF              4096
#define MAX_STACK           32
#define MAX_LOOPS           8

uint32_t evk_ps_build_id(void)
{
    return EVK_PS_BUILD_ID;
}

typedef struct Str {
    char *p;
    size_t n, cap;
} Str;

static void str_add(Str *s, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void str_add(Str *s, const char *fmt, ...)
{
    va_list ap;
    int n;

    for (;;) {
        size_t room = s->cap - s->n;
        va_start(ap, fmt);
        n = vsnprintf(s->p ? s->p + s->n : NULL, s->p ? room : 0, fmt, ap);
        va_end(ap);
        if (n < 0) {
            return;
        }
        if (s->p && (size_t)n < room) {
            s->n += n;
            return;
        }
        s->cap = (s->cap + n + 1) * 2;
        s->p = realloc(s->p, s->cap);
    }
}

typedef struct Tr {
    EvkFetchFn fetch;
    void *opaque;
    EvkProgInfo *info;
    Str body;
    char *why;
    size_t whylen;
    bool fail;
    int sp, maxsp;              /* CF stack depth here, and the deepest */
    int nloops, maxloops, loop_id;
    int loop_sp[MAX_LOOPS];
    int loop_ids[MAX_LOOPS];
    int maxreg;
    bool relgpr;
    int kc[4];                  /* the kcache window slot of KC0-KC3, -1: none */
    int tmp;                    /* unique names */
    bool vs;                    /* translating a vertex shader */
    bool in_fs;                 /* ... in its fetch shader (CALL_FS) */
    EvkFetchFn fetch_fs;
    void *fs_opaque;
} Tr;

static void fail(Tr *t, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void fail(Tr *t, const char *fmt, ...)
{
    va_list ap;

    if (t->fail) {
        return;
    }
    t->fail = true;
    va_start(ap, fmt);
    vsnprintf(t->why, t->whylen, fmt, ap);
    va_end(ap);
}

#define EMIT(...) str_add(&t->body, __VA_ARGS__)

static const uint32_t *pw(Tr *t, uint32_t dw, uint32_t n)
{
    const uint32_t *w = t->fetch(t->opaque, dw, n);

    if (!w) {
        fail(t, "program past its end (dword %u)", dw);
        return NULL;
    }
    if (dw + n > t->info->ndw) {
        t->info->ndw = dw + n;
    }
    return w;
}

static void use_reg(Tr *t, int r)
{
    if (r > t->maxreg) {
        t->maxreg = r;
    }
}

static const char CH[4] = { 'x', 'y', 'z', 'w' };

/* the slot of a resource / sampler / buffer id in the draw's tables */
static int slot_of(Tr *t, uint8_t *ids, int *n, int max, uint32_t id, const char *what)
{
    int i;

    for (i = 0; i < *n; i++) {
        if (ids[i] == id) {
            return i;
        }
    }
    if (*n >= max) {
        fail(t, "more than %d %s", max, what);
        return 0;
    }
    ids[*n] = (uint8_t)id;
    return (*n)++;
}

static int vbuf_slot(Tr *t, uint32_t id)
{
    EvkProgInfo *in = t->info;
    int i, max = t->vs ? EVK_MAX_VS_VBUF : EVK_MAX_VBUF;

    for (i = 0; i < in->nvbuf; i++) {
        if (in->vbuf_id[i] == id) {
            return i;
        }
    }
    if (in->nvbuf >= max) {
        fail(t, "more than %d fetch buffers", max);
        return 0;
    }
    in->vbuf_id[in->nvbuf] = (uint16_t)id;
    return in->nvbuf++;
}

static int kc_slot(Tr *t, uint32_t bank, uint32_t mode, uint32_t addr)
{
    EvkProgInfo *in = t->info;
    int i;

    if (!mode) {
        return -1;
    }
    for (i = 0; i < in->nkc; i++) {
        if (in->kc_bank[i] == bank && in->kc_mode[i] == mode && in->kc_addr[i] == addr) {
            return i;
        }
    }
    if (in->nkc >= EVK_MAX_KC) {
        fail(t, "more than %d kcache windows", EVK_MAX_KC);
        return -1;
    }
    in->kc_bank[in->nkc] = (uint8_t)bank;
    in->kc_mode[in->nkc] = (uint8_t)mode;
    in->kc_addr[in->nkc] = (uint8_t)addr;
    return in->nkc++;
}

/* index register of INDEX_MODE */
static const char *index_reg(uint32_t im)
{
    return im == 4 ? "aL" : "ar";
}

/* the CF condition (cond_mask) as a per-lane bool */
static void cond_expr(char *buf, size_t len, uint32_t w1)
{
    uint32_t cond = (w1 >> 8) & 3, cf_const = (w1 >> 3) & 0x1f;

    switch (cond) {
    case 0:
        snprintf(buf, len, "(act && alive)");
        break;
    case 1:
        snprintf(buf, len, "false");
        break;
    default:
        snprintf(buf, len, "(act && alive && ((MISC(EVK_M_BOOLC) >> %uu) & 1u) %s 0u)",
                 cf_const, cond == 2 ? "!=" : "==");
        break;
    }
}

/* ------------------------------------------------------------------ */
/* ALU                                                                 */
/* ------------------------------------------------------------------ */

typedef struct Inst {
    uint32_t w0, w1;
    bool op3;
    uint32_t op;
    int slot, nsrc;
} Inst;

static bool trans_only(const Inst *in)
{
    return in->op3 ? in->op == 0x1f : (in->op >= 0x81 && in->op <= 0x9c);
}

static uint32_t src_sel(const Inst *in, int k)
{
    return k == 0 ? in->w0 & 0x1ff : k == 1 ? (in->w0 >> 13) & 0x1ff : in->w1 & 0x1ff;
}

/* a source operand as a uint expression (read_src + neg/abs) */
static void src_expr(Tr *t, const Inst *in, int k, const uint32_t *lit, char *buf, size_t len)
{
    uint32_t w0 = in->w0, w1 = in->w1, sel, chan, im = (w0 >> 26) & 7;
    bool rel, neg, absv = false;
    char base[96];

    if (k == 0) {
        sel = w0 & 0x1ff; rel = (w0 >> 9) & 1; chan = (w0 >> 10) & 3; neg = (w0 >> 12) & 1;
        absv = !in->op3 && (w1 & 1);
    } else if (k == 1) {
        sel = (w0 >> 13) & 0x1ff; rel = (w0 >> 22) & 1; chan = (w0 >> 23) & 3;
        neg = (w0 >> 25) & 1;
        absv = !in->op3 && ((w1 >> 1) & 1);
    } else {
        sel = w1 & 0x1ff; rel = (w1 >> 9) & 1; chan = (w1 >> 10) & 3; neg = (w1 >> 12) & 1;
    }
    if (sel < 128) {
        if (rel) {
            t->relgpr = true;
            snprintf(base, sizeof(base), "R[(%u + %s) & 127].%c", sel, index_reg(im), CH[chan]);
        } else {
            use_reg(t, (int)sel);
            snprintf(base, sizeof(base), "R[%u].%c", sel, CH[chan]);
        }
    } else if (sel < 192 || (sel >= 256 && sel < 320)) {
        int set = sel < 192 ? (sel >= 160) : 2 + (sel >= 288);
        uint32_t i = sel - (sel < 192 ? (set ? 160 : 128) : (set == 3 ? 288 : 256));
        int s = t->kc[set];
        if (s < 0) {
            snprintf(base, sizeof(base), "0u");
        } else if (rel) {
            snprintf(base, sizeof(base), "D.kc[%d + ((%u + %s) & 31)].%c", s * 32, i,
                     index_reg(im), CH[chan]);
        } else {
            snprintf(base, sizeof(base), "D.kc[%u].%c", s * 32 + (i & 31), CH[chan]);
        }
    } else if (sel >= 248 && sel < 256) {
        switch (sel) {
        case 0xf8: snprintf(base, sizeof(base), "0u"); break;
        case 0xf9: snprintf(base, sizeof(base), "0x3f800000u"); break;
        case 0xfa: snprintf(base, sizeof(base), "1u"); break;
        case 0xfb: snprintf(base, sizeof(base), "0xffffffffu"); break;
        case 0xfc: snprintf(base, sizeof(base), "0x3f000000u"); break;
        case 0xfd: snprintf(base, sizeof(base), "0x%08xu", lit[chan]); break;
        case 0xfe: snprintf(base, sizeof(base), "pv.%c", CH[chan]); break;
        default: snprintf(base, sizeof(base), "psv"); break;
        }
    } else if (sel >= 448 && sel < 480) {
        snprintf(base, sizeof(base), "0u");             /* INTERP_* parameters */
    } else if (sel >= 320) {
        t->info->uses_cfile = true;
        if (rel) {
            snprintf(base, sizeof(base), "D.cfile[(%u + %s) & 255].%c", sel - 256,
                     index_reg(im), CH[chan]);
        } else {
            snprintf(base, sizeof(base), "D.cfile[%u].%c", (sel - 256) & 255, CH[chan]);
        }
    } else {
        snprintf(base, sizeof(base), "0u");
    }
    if (absv && neg) {
        snprintf(buf, len, "((%s & 0x7fffffffu) ^ 0x80000000u)", base);
    } else if (absv) {
        snprintf(buf, len, "(%s & 0x7fffffffu)", base);
    } else if (neg) {
        snprintf(buf, len, "(%s ^ 0x80000000u)", base);
    } else {
        snprintf(buf, len, "%s", base);
    }
}

/* decode the group at dword dw (decode_group); returns its span, 0 on error */
static int decode_group(Tr *t, uint32_t dw, Inst *ins, int *n, uint32_t lit[4])
{
    int i = 0, used = 0, last = -1, nlit = 0, k;
    const uint32_t *w;

    for (;;) {
        w = pw(t, dw + 2 * i, 2);
        if (!w) {
            return 0;
        }
        ins[i].w0 = w[0];
        ins[i].w1 = w[1];
        ins[i].op3 = ((w[1] >> 15) & 7) != 0;
        ins[i].op = ins[i].op3 ? (w[1] >> 13) & 0x1f : (w[1] >> 7) & 0xff;
        ins[i].nsrc = ins[i].op3 ? 3 : 2;
        i++;
        if ((w[0] >> 31) || i == 5) {
            break;
        }
    }
    *n = i;
    for (k = 0; k < i; k++) {
        Inst *in = &ins[k];
        int ch = (in->w1 >> 29) & 3, j;
        for (j = 0; j < in->nsrc; j++) {
            if (src_sel(in, j) == 0xfd) {
                int c = j == 0 ? (in->w0 >> 10) & 3 : j == 1 ? (in->w0 >> 23) & 3 :
                        (in->w1 >> 10) & 3;
                if (c + 1 > nlit) {
                    nlit = c + 1;
                }
            }
        }
        if (trans_only(in) || ((used >> ch) & 1) || ch <= last) {
            in->slot = 4;
        } else {
            in->slot = ch;
            used |= 1 << ch;
            last = ch;
        }
    }
    nlit = (nlit + 1) & ~1;
    memset(lit, 0, 16);
    if (nlit) {
        w = pw(t, dw + 2 * i, nlit);
        if (!w) {
            return 0;
        }
        memcpy(lit, w, nlit * 4);
    }
    return 2 * i + nlit;
}

static bool is_dot(const Inst *in)
{
    return !in->op3 && in->slot < 4 && (in->op == 0xbe || in->op == 0xbf || in->op == 0xc1);
}

/* PRED_SET* ops: the comparison producing the predicate, else NULL */
static bool pred_cmp(uint32_t op, char *buf, size_t len, int i)
{
    char a[16], b[16], ua[16], ub[16], ia[24], ib[24];

    snprintf(a, sizeof(a), "F(a%d_0)", i);
    snprintf(b, sizeof(b), "F(a%d_1)", i);
    snprintf(ua, sizeof(ua), "a%d_0", i);
    snprintf(ub, sizeof(ub), "a%d_1", i);
    snprintf(ia, sizeof(ia), "int(a%d_0)", i);
    snprintf(ib, sizeof(ib), "int(a%d_1)", i);
    switch (op) {
    case 0x20: case 0x28: case 0x2c: snprintf(buf, len, "%s == %s", a, b); return true;
    case 0x21: case 0x29: case 0x2d: snprintf(buf, len, "%s > %s", a, b); return true;
    case 0x22: case 0x2a: case 0x2e: snprintf(buf, len, "%s >= %s", a, b); return true;
    case 0x23: case 0x2b: case 0x2f: snprintf(buf, len, "%s != %s", a, b); return true;
    case 0x1e: case 0x40: snprintf(buf, len, "%s > %s", ua, ub); return true;
    case 0x1f: case 0x41: snprintf(buf, len, "%s >= %s", ua, ub); return true;
    case 0x42: case 0x4a: case 0x46: snprintf(buf, len, "%s == %s", ia, ib); return true;
    case 0x43: case 0x4b: case 0x47: snprintf(buf, len, "%s > %s", ia, ib); return true;
    case 0x44: case 0x4c: case 0x48: snprintf(buf, len, "%s >= %s", ia, ib); return true;
    case 0x45: case 0x4d: case 0x49: snprintf(buf, len, "%s != %s", ia, ib); return true;
    case 0x4e: snprintf(buf, len, "%s < %s", ia, ib); return true;
    case 0x4f: snprintf(buf, len, "%s <= %s", ia, ib); return true;
    }
    return false;
}

static bool is_kill(uint32_t op)
{
    return (op >= 0x2c && op <= 0x2f) || op == 0x40 || op == 0x41 || (op >= 0x46 && op <= 0x49);
}

static bool is_pred_set(uint32_t op)
{
    return (op >= 0x1e && op <= 0x2b) || (op >= 0x42 && op <= 0x45) || (op >= 0x4a && op <= 0x4f);
}

/*
 * One instruction's result into r<i> (and t<i> for predicate / kill
 * ops).  Returns false for an op the translator does not know.
 */
static bool emit_op(Tr *t, const Inst *ins, int n, int i, const char *dot)
{
    const Inst *in = &ins[i];
    uint32_t op = in->op, w1 = in->w1;
    char A[16], B[16], C[16], UA[16], UB[16], UC[16], IA[24], IB[24];
    char f[512] = "", r[512] = "", cmp[128];
    bool isf = true;

    snprintf(A, sizeof(A), "F(a%d_0)", i);
    snprintf(B, sizeof(B), "F(a%d_1)", i);
    snprintf(C, sizeof(C), "F(a%d_2)", i);
    snprintf(UA, sizeof(UA), "a%d_0", i);
    snprintf(UB, sizeof(UB), "a%d_1", i);
    snprintf(UC, sizeof(UC), "a%d_2", i);
    snprintf(IA, sizeof(IA), "int(a%d_0)", i);
    snprintf(IB, sizeof(IB), "int(a%d_1)", i);
#define FS(...) snprintf(f, sizeof(f), __VA_ARGS__)
#define RS(...) (snprintf(r, sizeof(r), __VA_ARGS__), isf = false)

    if (in->op3) {
        switch (op) {
        case 0x14: FS("mulz(%s, %s) + %s", A, B, C); break;
        case 0x15: FS("(mulz(%s, %s) + %s) * 2.0", A, B, C); break;
        case 0x16: FS("(mulz(%s, %s) + %s) * 4.0", A, B, C); break;
        case 0x17: FS("(mulz(%s, %s) + %s) / 2.0", A, B, C); break;
        case 0x18: case 0x07: FS("fma(%s, %s, %s)", A, B, C); break;
        case 0x19: FS("%s == 0.0 ? %s : %s", A, B, C); break;
        case 0x1a: FS("%s > 0.0 ? %s : %s", A, B, C); break;
        case 0x1b: FS("%s >= 0.0 ? %s : %s", A, B, C); break;
        case 0x1c: RS("%s == 0 ? %s : %s", IA, UB, UC); break;
        case 0x1d: RS("%s > 0 ? %s : %s", IA, UB, UC); break;
        case 0x1e: RS("%s >= 0 ? %s : %s", IA, UB, UC); break;
        case 0x1f: FS("(%s > 0.0 || isnan(%s)) ? mulz(%s, %s) : 0.0", C, C, A, B); break;
        case 0x04: RS("bfe_u(%s, %s, %s)", UA, UB, UC); break;
        case 0x05: RS("bfe_i(%s, %s, %s)", UA, UB, UC); break;
        case 0x06: RS("(%s & %s) | (~%s & %s)", UA, UB, UA, UC); break;
        case 0x0c:
            RS("(%s & 31u) == 0u ? %s : ((%s >> (%s & 31u)) | (%s << (32u - (%s & 31u))))",
               UC, UB, UB, UC, UA, UC);
            break;
        case 0x0d:
            RS("(%s & 3u) == 0u ? %s : ((%s >> ((%s & 3u) * 8u)) | (%s << (32u - (%s & 3u) * 8u)))",
               UC, UB, UB, UC, UA, UC);
            break;
        case 0x10: RS("(%s & 0xffffffu) * (%s & 0xffffffu) + %s", UA, UB, UC); break;
        default:
            return false;
        }
    } else if (pred_cmp(op, cmp, sizeof(cmp), i)) {
        EMIT("        bool t%d = %s;\n", i, cmp);
        FS("t%d ? 1.0 : 0.0", i);
    } else {
        switch (op) {
        case 0x00: FS("%s + %s", A, B); break;
        case 0x01: FS("mulz(%s, %s)", A, B); break;
        case 0x02: FS("%s * %s", A, B); break;
        case 0x03: case 0x05: FS("fmax_(%s, %s)", A, B); break;
        case 0x04: case 0x06: FS("fmin_(%s, %s)", A, B); break;
        case 0x08: FS("%s == %s ? 1.0 : 0.0", A, B); break;
        case 0x09: FS("%s > %s ? 1.0 : 0.0", A, B); break;
        case 0x0a: FS("%s >= %s ? 1.0 : 0.0", A, B); break;
        case 0x0b: FS("%s != %s ? 1.0 : 0.0", A, B); break;
        case 0x0c: RS("%s == %s ? 0xffffffffu : 0u", A, B); break;
        case 0x0d: RS("%s > %s ? 0xffffffffu : 0u", A, B); break;
        case 0x0e: RS("%s >= %s ? 0xffffffffu : 0u", A, B); break;
        case 0x0f: RS("%s != %s ? 0xffffffffu : 0u", A, B); break;
        case 0x10: FS("%s - floor(%s)", A, A); break;
        case 0x11: FS("trunc(%s)", A); break;
        case 0x12: FS("ceil(%s)", A); break;
        case 0x13: FS("roundEven(%s)", A); break;
        case 0x14: FS("floor(%s)", A); break;
        case 0x15: RS("uint(%s >> (%s & 31u))", IA, UB); break;
        case 0x16: RS("%s >> (%s & 31u)", UA, UB); break;
        case 0x17: RS("%s << (%s & 31u)", UA, UB); break;
        case 0x19:
            if ((w1 >> 31) || ((w1 >> 5) & 3)) {
                FS("%s", A);
            } else {
                RS("%s", UA);
            }
            break;
        case 0x24: FS("%s == 1.0 ? 0.0 : %s + 1.0", A, A); break;
        case 0x25: FS("%s <= 0.0 ? 0.0 : %s - 1.0", A, A); break;
        case 0x26: FS("uintBitsToFloat(0x7f800000u)"); break;
        case 0x27: FS("%s", A); break;
        case 0x30: RS("%s & %s", UA, UB); break;
        case 0x31: RS("%s | %s", UA, UB); break;
        case 0x32: RS("%s ^ %s", UA, UB); break;
        case 0x33: RS("~%s", UA); break;
        case 0x34: RS("%s + %s", UA, UB); break;
        case 0x35: RS("%s - %s", UA, UB); break;
        case 0x36: RS("uint(max(%s, %s))", IA, IB); break;
        case 0x37: RS("uint(min(%s, %s))", IA, IB); break;
        case 0x38: RS("max(%s, %s)", UA, UB); break;
        case 0x39: RS("min(%s, %s)", UA, UB); break;
        case 0x3a: RS("%s == %s ? 0xffffffffu : 0u", IA, IB); break;
        case 0x3b: RS("%s > %s ? 0xffffffffu : 0u", IA, IB); break;
        case 0x3c: RS("%s >= %s ? 0xffffffffu : 0u", IA, IB); break;
        case 0x3d: RS("%s != %s ? 0xffffffffu : 0u", IA, IB); break;
        case 0x3e: RS("%s > %s ? 0xffffffffu : 0u", UA, UB); break;
        case 0x3f: RS("%s >= %s ? 0xffffffffu : 0u", UA, UB); break;
        case 0x50: RS("f2i_sat(trunc(%s))", A); break;
        case 0xb1: RS("f2i_sat(floor(%s))", A); break;
        case 0xb0: RS("f2i_sat(floor(%s + 0.5))", A); break;
        case 0x9a: RS("f2u_sat(trunc(%s))", A); break;
        case 0x9b: FS("float(%s)", IA); break;
        case 0x9c: FS("float(%s)", UA); break;
        case 0x51: RS("bitfieldReverse(%s)", UA); break;
        case 0x52: RS("(%s + %s < %s) ? 1u : 0u", UA, UB, UA); break;
        case 0x53: RS("%s < %s ? 1u : 0u", UA, UB); break;
        case 0x81: FS("exp2(%s)", A); break;
        case 0x82: FS("%s == 0.0 ? -3.40282347e38 : log2(%s)", A, A); break;
        case 0x83: FS("log2(%s)", A); break;
        case 0x84: FS("recip_clamped(%s, false)", A); break;
        case 0x85: FS("recip_clamped(%s, true)", A); break;
        case 0x86: FS("1.0 / %s", A); break;
        case 0x87: FS("rsq_clamped(%s, false)", A); break;
        case 0x88: FS("rsq_clamped(%s, true)", A); break;
        case 0x89: FS("inversesqrt(%s)", A); break;
        case 0x8a: FS("sqrt(%s)", A); break;
        case 0x8d: FS("sin(%s * 2.0 * 3.14159265358979)", A); break;
        case 0x8e: FS("cos(%s * 2.0 * 3.14159265358979)", A); break;
        case 0x8f: case 0x91: RS("%s * %s", UA, UB); break;
        case 0x90: RS("mulhi_i(%s, %s)", UA, UB); break;
        case 0x92: RS("mulhi_u(%s, %s)", UA, UB); break;
        case 0x93: RS("recip_i(%s)", UA); break;
        case 0x94: RS("recip_u(%s)", UA); break;
        case 0xa0: RS("((1u << (%s & 31u)) - 1u) << (%s & 31u)", UA, UB); break;
        case 0xa2: RS("float_to_half(%s)", A); break;
        case 0xa3: FS("half_to_float(%s & 0xffffu)", UA); break;
        case 0xa4: FS("float(%s & 255u)", UA); break;
        case 0xa5: FS("float((%s >> 8) & 255u)", UA); break;
        case 0xa6: FS("float((%s >> 16) & 255u)", UA); break;
        case 0xa7: FS("float(%s >> 24)", UA); break;
        case 0xaa: RS("uint(bitCount(%s))", UA); break;
        case 0xab: RS("ffbh(%s)", UA); break;
        case 0xac: RS("%s != 0u ? uint(findLSB(%s)) : 0xffffffffu", UA, UA); break;
        case 0xad: RS("ffbh(%s < 0 ? ~%s : %s)", IA, UA, UA); break;
        case 0xb5: RS("(%s & 0xffffffu) * (%s & 0xffffffu)", UA, UB); break;
        case 0xb2: RS("mulhi_u24(%s, %s)", UA, UB); break;
        case 0xbe: case 0xbf: case 0xc1: FS("%s", dot); break;
        case 0xaf: FS("%s * %s", A, B); break;
        case 0xc0: {
            /* CUBE: the vector comes from the group's four CUBE slots */
            char vx[24] = "0.0", vy[24] = "0.0", vz[24] = "0.0";
            int j;
            if (in->slot > 3) {
                return false;
            }
            for (j = 0; j < n; j++) {
                if (ins[j].op3 || ins[j].op != 0xc0) {
                    continue;
                }
                switch (ins[j].slot) {
                case 0:
                    snprintf(vz, sizeof(vz), "F(a%d_0)", j);
                    snprintf(vy, sizeof(vy), "F(a%d_1)", j);
                    break;
                case 1: snprintf(vx, sizeof(vx), "F(a%d_1)", j); break;
                case 2: snprintf(vx, sizeof(vx), "F(a%d_0)", j); break;
                case 3:
                    snprintf(vy, sizeof(vy), "F(a%d_0)", j);
                    snprintf(vz, sizeof(vz), "F(a%d_1)", j);
                    break;
                }
            }
            FS("cube_res(%s, %s, %s, %d)", vx, vy, vz, in->slot);
            break;
        }
        case 0xcc: RS("%s", UA); break;
        case 0xd6: case 0xd7: case 0xd8: case 0xd9: {
            /* INTERP: the even slot of each pair takes j, the odd one i */
            int pi = (int)src_sel(in, 1) - 448, base = in->slot & 2, j, ch;
            char vi[24] = "0.0", vj[24] = "0.0";
            if (in->slot > 3) {
                return false;
            }
            ch = (op == 0xd8) ? (in->slot & 1) : (op == 0xd9) ? 2 + (in->slot & 1) : in->slot;
            for (j = 0; j < n; j++) {
                if (!ins[j].op3 && ins[j].op == op && ins[j].slot < 4 &&
                    (ins[j].slot & 2) == base) {
                    snprintf((ins[j].slot & 1) ? vi : vj, 24, "F(a%d_0)", j);
                }
            }
            if (pi < 0 || pi >= EVK_MAX_PARAMS) {
                fail(t, "interpolator %d", pi);
                return true;
            }
            FS("vparam(0, %d).%c + %s * (vparam(1, %d).%c - vparam(0, %d).%c) + "
               "%s * (vparam(2, %d).%c - vparam(0, %d).%c)",
               pi, CH[ch], vi, pi, CH[ch], pi, CH[ch], vj, pi, CH[ch], pi, CH[ch]);
            break;
        }
        case 0xe0: case 0xe1: case 0xe2: {
            int pi = (int)src_sel(in, 0) - 448, c = in->slot & 3;
            if (pi < 0 || pi >= EVK_MAX_PARAMS) {
                fail(t, "interpolator %d", pi);
                return true;
            }
            if (op == 0xe0) {
                RS("U(vparam(0, %d).%c)", pi, CH[c]);
            } else {
                RS("U(vparam(%d, %d).%c - vparam(0, %d).%c)", op == 0xe1 ? 1 : 2, pi, CH[c],
                   pi, CH[c]);
            }
            break;
        }
        default:
            return false;
        }
    }
    if (isf) {
        uint32_t omod = in->op3 ? 0 : (w1 >> 5) & 3;
        EMIT("        float f%d = %s;\n", i, f);
        if (omod == 1) {
            EMIT("        f%d *= 2.0;\n", i);
        } else if (omod == 2) {
            EMIT("        f%d *= 4.0;\n", i);
        } else if (omod == 3) {
            EMIT("        f%d /= 2.0;\n", i);
        }
        if (w1 >> 31) {
            EMIT("        f%d = clamp01(f%d);\n", i, i);
        }
        EMIT("        uint r%d = U(f%d);\n", i, i);
    } else {
        EMIT("        uint r%d = %s;\n", i, r);
    }
#undef FS
#undef RS
    return true;
}

static bool is_nop(const Inst *in)
{
    return !in->op3 && (in->op == 0x1a || in->op == 0x54 || in->op == 0x57 ||
                        in->op == 0x58 || in->op == 0x59);
}

static void emit_group(Tr *t, const Inst *ins, int n, const uint32_t *lit)
{
    char buf[160], dot[1024] = "0.0";
    int i, k;
    bool has_pred = false, upd_pred = false, any_dot = false;

    EMIT("    if (act && alive) {\n");
    for (i = 0; i < n; i++) {
        for (k = 0; k < ins[i].nsrc; k++) {
            src_expr(t, &ins[i], k, lit, buf, sizeof(buf));
            EMIT("        uint a%d_%d = %s;\n", i, k, buf);
        }
    }
    /* DOT4 / DOT4_IEEE / MAX4 reduce over the vector slots */
    for (i = 0; i < n; i++) {
        char term[64], prev[1024];
        if (!is_dot(&ins[i])) {
            continue;
        }
        if (ins[i].op == 0xbe) {
            snprintf(term, sizeof(term), "mulz(F(a%d_0), F(a%d_1))", i, i);
        } else if (ins[i].op == 0xbf) {
            snprintf(term, sizeof(term), "F(a%d_0) * F(a%d_1)", i, i);
        } else {
            snprintf(term, sizeof(term), "F(a%d_0)", i);
        }
        snprintf(prev, sizeof(prev), "%s", dot);
        if (ins[i].op == 0xc1) {
            if (i == 0) {
                snprintf(dot, sizeof(dot), "%s", term);
            } else {
                snprintf(dot, sizeof(dot), "fmax_(%s, %s)", prev, term);
            }
        } else if (!any_dot) {
            snprintf(dot, sizeof(dot), "(%s + %s)", prev, term);
        } else {
            snprintf(dot, sizeof(dot), "(%s + %s)", prev, term);
        }
        any_dot = true;
    }
    if (any_dot) {
        EMIT("        float dot_ = %s;\n", dot);
    }
    for (i = 0; i < n; i++) {
        uint32_t ps = (ins[i].w0 >> 29) & 3;
        if (is_nop(&ins[i])) {
            continue;
        }
        EMIT("        bool d%d = %s;\n", i, ps == 2 ? "!pred" : ps == 3 ? "pred" : "true");
        if (!emit_op(t, ins, n, i, "dot_")) {
            fail(t, "ALU %s 0x%02x", ins[i].op3 ? "OP3" : "OP2", ins[i].op);
            return;
        }
        if (!ins[i].op3 && is_pred_set(ins[i].op)) {
            has_pred = true;
        }
        if (!ins[i].op3 && (ins[i].op == 0x24 || ins[i].op == 0x25 || ins[i].op == 0x26 ||
                            ins[i].op == 0x27)) {
            has_pred = true;
        }
    }
    /* commit: GPRs and PV/PS in order, then AR, predicate, exec mask, kill */
    if (has_pred) {
        EMIT("        int opred = -1;\n");
    }
    for (i = 0; i < n; i++) {
        const Inst *in = &ins[i];
        uint32_t w1 = in->w1, op = in->op;
        bool wm = in->op3 || ((w1 >> 4) & 1);
        int dst = (w1 >> 21) & 0x7f, ch = (w1 >> 29) & 3;
        if (is_nop(in)) {
            continue;
        }
        EMIT("        if (d%d) {\n", i);
        if (wm) {
            if ((w1 >> 28) & 1) {
                t->relgpr = true;
                EMIT("            R[(%d + %s) & 127].%c = r%d;\n", dst,
                     index_reg((in->w0 >> 26) & 7), CH[ch], i);
            } else {
                use_reg(t, dst);
                EMIT("            R[%d].%c = r%d;\n", dst, CH[ch], i);
            }
        }
        if (in->slot < 4) {
            EMIT("            pv.%c = r%d;\n", CH[in->slot], i);
        } else {
            EMIT("            psv = r%d;\n", i);
        }
        if (!in->op3 && op == 0xcc) {
            EMIT("            ar = int(a%d_0);\n", i);
        }
        if (!in->op3 && is_pred_set(op)) {
            EMIT("            opred = t%d ? 1 : 0;\n", i);
        } else if (!in->op3 && (op == 0x24 || op == 0x25)) {
            EMIT("            opred = f%d == 0.0 ? 1 : 0;\n", i);
        } else if (!in->op3 && op == 0x26) {
            EMIT("            opred = 0;\n");
        } else if (!in->op3 && op == 0x27) {
            EMIT("            opred = F(a%d_0) == 0.0 ? 1 : 0;\n", i);
        }
        if (!in->op3 && is_kill(op)) {
            EMIT("            if (t%d) kill_ = true;\n", i);
        }
        EMIT("        }\n");
        if (!in->op3 && ((w1 >> 3) & 1)) {
            upd_pred = true;
        }
    }
    if (has_pred) {
        bool eu = false;
        EMIT("        if (opred >= 0) {\n");
        if (upd_pred) {
            EMIT("            pred = opred == 1;\n");
        }
        EMIT("            bool eu_ = false;\n");
        for (i = 0; i < n; i++) {
            if (!ins[i].op3 && !is_nop(&ins[i]) && ((ins[i].w1 >> 2) & 1)) {
                EMIT("            eu_ = eu_ || d%d;\n", i);
                eu = true;
            }
        }
        if (eu) {
            EMIT("            if (eu_ && opred == 0) act = false;\n");
        }
        EMIT("        }\n");
    }
    for (i = 0; i < n; i++) {
        if (!ins[i].op3 && is_kill(ins[i].op)) {
            EMIT("        if (kill_) alive = false;\n");
            break;
        }
    }
    EMIT("    }\n");
}

static void emit_alu_clause(Tr *t, uint32_t addr, uint32_t count)
{
    uint32_t dw = addr * 2, end = dw + count * 2;
    Inst ins[5];
    uint32_t lit[4];
    int n, span;

    EMIT("    pv = uvec4(0u); psv = 0u;\n");
    while (dw < end && !t->fail) {
        span = decode_group(t, dw, ins, &n, lit);
        if (!span || !n) {
            break;
        }
        emit_group(t, ins, n, lit);
        dw += span;
    }
}

/* ------------------------------------------------------------------ */
/* Fetch clauses                                                       */
/* ------------------------------------------------------------------ */

static void emit_dst_sel(Tr *t, int dst, const uint32_t sel[4], const char *v, const char *one)
{
    int c;

    use_reg(t, dst);
    for (c = 0; c < 4; c++) {
        if (sel[c] < 4) {
            EMIT("            R[%d].%c = %s.%c;\n", dst, CH[c], v, CH[sel[c]]);
        } else if (sel[c] == 4) {
            EMIT("            R[%d].%c = 0u;\n", dst, CH[c]);
        } else if (sel[c] == 5) {
            EMIT("            R[%d].%c = %s;\n", dst, CH[c], one);
        }
    }
}

static void emit_tex(Tr *t, const uint32_t *f, const char *cond)
{
    if (t->vs) {
        fail(t, "texture fetch in a vertex shader");
        return;
    }
    uint32_t w0 = f[0], w1 = f[1], w2 = f[2];
    uint32_t op = w0 & 0x1f, rid = (w0 >> 8) & 0xff, src = (w0 >> 16) & 0x7f;
    uint32_t dst = w1 & 0x7f, sid = (w2 >> 15) & 0x1f;
    uint32_t dsel[4] = { (w1 >> 9) & 7, (w1 >> 12) & 7, (w1 >> 15) & 7, (w1 >> 18) & 7 };
    uint32_t ssel[4] = { (w2 >> 20) & 7, (w2 >> 23) & 7, (w2 >> 26) & 7, (w2 >> 29) & 7 };
    uint32_t norm = (w1 >> 28) & 0xf;
    int ioff[3], c, ts, ss = 0;
    float ibias = (float)(int8_t)(((w1 >> 21) & 0x7f) << 1) / 32.0f;
    char comp[4][40];
    EvkProgInfo *in = t->info;

    for (c = 0; c < 3; c++) {
        int v = (w2 >> (5 * c)) & 0x1f;
        ioff[c] = (v & 0x10) ? v - 32 : v;
    }
    use_reg(t, (int)src);
    for (c = 0; c < 4; c++) {
        if (ssel[c] < 4) {
            snprintf(comp[c], sizeof(comp[c]), "R[%u].%c", src, CH[ssel[c]]);
        } else {
            snprintf(comp[c], sizeof(comp[c]), "%s", ssel[c] == 5 ? "0x3f800000u" : "0u");
        }
    }
    EMIT("    {\n        uvec4 c_ = uvec4(%s, %s, %s, %s);\n", comp[0], comp[1], comp[2],
         comp[3]);
    switch (op) {
    case 0x0b: case 0x0c:                               /* SET_GRADIENTS_H/V */
        EMIT("        grad_%c = F(c_);\n        has_grad = true;\n    }\n",
             op == 0x0b ? 'h' : 'v');
        return;
    case 0x09: case 0x0a:                               /* SET_OFFSETS, KEEP_GRAD */
        EMIT("    }\n");
        return;
    case 0x07: case 0x08:                               /* GET_GRADIENTS_H/V */
        EMIT("        uvec4 t_ = U(quad_grad(F(c_), %s));\n", op == 0x07 ? "true" : "false");
        break;
    case 0x03: case 0x04:
    case 0x10: case 0x11: case 0x12: case 0x13: case 0x14: case 0x15: case 0x16:
    case 0x18: case 0x19: case 0x1a: case 0x1b: case 0x1c: case 0x1e:
        ts = slot_of(t, in->tex_rid, &in->ntex, EVK_MAX_TEX, rid, "textures");
        if (op != 0x03 && op != 0x04) {
            ss = slot_of(t, in->smp_sid, &in->nsmp, EVK_MAX_SMP, sid, "samplers");
        }
        EMIT("        uvec4 t_ = tex_fetch(%du, %du, 0x%02xu, c_, ivec3(%d, %d, %d), %.9g, %uu);\n",
             ts, ss, op, ioff[0], ioff[1], ioff[2], ibias, norm);
        break;
    default:
        fail(t, "TEX op 0x%02x", op);
        return;
    }
    EMIT("        if (%s) {\n", cond);
    emit_dst_sel(t, (int)dst, dsel, "t_", "0x3f800000u");
    EMIT("        }\n    }\n");
}

static void emit_vtx(Tr *t, const uint32_t *f, const char *cond)
{
    uint32_t w0 = f[0], w1 = f[1], w2 = f[2];
    uint32_t buf = (w0 >> 8) & 0xff, src = (w0 >> 16) & 0x7f, ssel = (w0 >> 24) & 3;
    uint32_t dst = w1 & 0x7f;
    /* resource ids: PS relative; VS absolute (VS 176+, fetch shader 992+) */
    int slot = vbuf_slot(t, t->vs ? (t->in_fs ? 992u : 176u) + buf : buf);

    use_reg(t, (int)src);
    use_reg(t, (int)dst);
    if (buf >= 128) {
        t->info->uses_cfile = true;     /* an unset buffer 128+ reads it */
    }
    EMIT("    if (%s) R[%u] = vtx_fetch(%du, %s, R[%u].%c, 0x%08xu, 0x%08xu, R[%u]);\n",
         cond, dst, slot, buf >= 128 ? "true" : "false", src, CH[ssel], w1, w2, dst);
}

/* ------------------------------------------------------------------ */
/* Control flow                                                        */
/* ------------------------------------------------------------------ */

static void push(Tr *t)
{
    if (t->sp >= MAX_STACK) {
        fail(t, "CF stack deeper than %d", MAX_STACK);
        return;
    }
    EMIT("    stk[%d] = act;\n", t->sp);
    t->sp++;
    if (t->sp > t->maxsp) {
        t->maxsp = t->sp;
    }
}

static void pop(Tr *t, int n)
{
    if (n <= 0 || t->sp <= 0) {
        return;
    }
    t->sp = n >= t->sp ? 0 : t->sp - n;
    EMIT("    act = stk[%d];\n", t->sp);
}

static void emit_export(Tr *t, uint32_t w0, uint32_t w1)
{
    uint32_t type = (w0 >> 13) & 3, base = w0 & 0x1fff, gpr = (w0 >> 15) & 0x7f;
    uint32_t burst = ((w1 >> 16) & 0xf) + 1, b, c;
    uint32_t sel[4] = { w1 & 7, (w1 >> 3) & 7, (w1 >> 6) & 7, (w1 >> 9) & 7 };

    if (t->vs) {
        /* POS 60-63 -> EPOS[], PARAM 0-31 -> EPAR[] */
        if (type != 1 && type != 2) {
            return;
        }
        for (b = 0; b < burst; b++) {
            uint32_t g = (gpr + b) & 127, n = base + b;
            char arr[16];
            if (type == 1 ? (n < 60 || n > 63) : n > 31) {
                continue;
            }
            snprintf(arr, sizeof(arr), type == 1 ? "EPOS[%u]" : "EPAR[%u]",
                     type == 1 ? n - 60 : n);
            use_reg(t, (int)g);
            EMIT("    if (act) {\n");
            for (c = 0; c < 4; c++) {
                if (sel[c] < 4) {
                    EMIT("        %s.%c = R[%u].%c;\n", arr, CH[c], g, CH[sel[c]]);
                } else if (sel[c] == 4) {
                    EMIT("        %s.%c = 0u;\n", arr, CH[c]);
                } else if (sel[c] == 5) {
                    EMIT("        %s.%c = 0x3f800000u;\n", arr, CH[c]);
                }
            }
            EMIT("    }\n");
        }
        return;
    }
    if (type != 0) {
        return;                         /* POS / PARAM: not in a pixel shader */
    }
    for (b = 0; b < burst; b++) {
        uint32_t g = (gpr + b) & 127, n = base + b;
        if (n >= 8 && n != 61) {
            continue;
        }
        use_reg(t, (int)g);
        EMIT("    if (act && alive) {\n");
        for (c = 0; c < 4; c++) {
            const char *dv = n == 61 ? "edepth" : NULL;
            char lhs[32];
            if (n == 61 && c) {
                break;
            }
            if (dv) {
                snprintf(lhs, sizeof(lhs), "edepth");
            } else {
                snprintf(lhs, sizeof(lhs), "E[%u].%c", n, CH[c]);
            }
            if (sel[c] < 4) {
                EMIT("        %s = R[%u].%c;\n", lhs, g, CH[sel[c]]);
            } else if (sel[c] == 4) {
                EMIT("        %s = 0u;\n", lhs);
            } else if (sel[c] == 5) {
                EMIT("        %s = 0x3f800000u;\n", lhs);
            }
        }
        if (n == 61) {
            EMIT("#ifndef EVK_PS_WRITES_Z\n#define EVK_PS_WRITES_Z 1\n#endif\n");
            EMIT("        edepth_written = true;\n");
        } else {
            EMIT("        emask |= %uu;\n", 1u << n);
        }
        EMIT("    }\n");
    }
}

static void emit_setup(Str *s, uint32_t baryc, uint32_t in0, uint32_t in1, Tr *t)
{
    static const int shift[6] = { 8, 0, 4, 24, 16, 20 };
    int k, reg = 0, ch = 0;

    for (k = 0; k < 6; k++) {
        const char *b = k < 3 ? "gl_BaryCoordEXT" : "gl_BaryCoordNoPerspEXT";
        if (!((baryc >> shift[k]) & 3)) {
            continue;
        }
        use_reg(t, reg);
        str_add(s, "    R[%d].%c = U(%s.y);\n    R[%d].%c = U(%s.z);\n",
                reg, CH[ch], b, reg, CH[ch + 1], b);
        ch += 2;
        if (ch == 4) {
            ch = 0;
            reg++;
        }
    }
    if ((in0 >> 8) & 1) {
        int r = (in0 >> 10) & 0x1f;
        use_reg(t, r);
        str_add(s, "    R[%d] = U(vec4(floor(gl_FragCoord.xy) + MISCF(EVK_M_PIX_CENTER), v_z, "
                "gl_FragCoord.w));\n", r);
    }
    if ((in1 >> 8) & 1) {
        int r = (in1 >> 12) & 0x1f, c = (in1 >> 9) & 3;
        use_reg(t, r);
        if ((in1 >> 11) & 1) {
            str_add(s, "    R[%d].%c = is_front() ? 0xffffffffu : 0u;\n", r, CH[c]);
        } else {
            str_add(s, "    R[%d].%c = U(is_front() ? 1.0 : -1.0);\n", r, CH[c]);
        }
    }
    if ((in1 >> 24) & 1) {
        int r = (in1 >> 25) & 0x1f;
        use_reg(t, r);
        str_add(s, "    R[%d] = uvec4(uvec2(ivec2(floor(gl_FragCoord.xy))) << 4, 0u, 0u);\n", r);
    }
}

/*
 * Number the GPRs the program uses 0..n-1 (only literal "R[n]" indices
 * exist without relative addressing).  The clause temporaries R122-R127
 * would otherwise make every fragment hold 128 vec4s, which the host
 * compiler cannot keep in registers.  Returns n.
 */
static void renumber(char *p, const int *map)
{
    char *q;

    for (q = p ? strstr(p, "R[") : NULL; q; q = strstr(q + 2, "R[")) {
        char *e, buf[8];
        long r;
        int w;

        if (q > p && (isalnum((unsigned char)q[-1]) || q[-1] == '_')) {
            continue;
        }
        r = strtol(q + 2, &e, 10);
        if (e == q + 2 || *e != ']' || r < 0 || r > 127) {
            continue;
        }
        /* the new number is never longer: pad with spaces inside the brackets */
        w = (int)(e - (q + 2));
        snprintf(buf, sizeof(buf), "%-*d", w, map[r]);
        memcpy(q + 2, buf, w);
    }
}

static int compact_regs(char *setup, char *body)
{
    int map[128], used[128] = { 0 }, n = 0, r;
    char *parts[2] = { setup, body }, *q;

    for (int k = 0; k < 2; k++) {
        for (q = parts[k] ? strstr(parts[k], "R[") : NULL; q; q = strstr(q + 2, "R[")) {
            char *e;

            if (q > parts[k] && (isalnum((unsigned char)q[-1]) || q[-1] == '_')) {
                continue;
            }
            r = (int)strtol(q + 2, &e, 10);
            if (e != q + 2 && *e == ']' && r >= 0 && r < 128) {
                used[r] = 1;
            }
        }
    }
    for (r = 0; r < 128; r++) {
        map[r] = used[r] ? n++ : 0;
    }
    renumber(setup, map);
    renumber(body, map);
    return n ? n : 1;
}

/* translate the CF program from dword 0 to its end (EOP, RET or HALT) */
static void tr_program(Tr *t)
{
    uint32_t pc = 0;
    int steps;
    bool done = false;
    char cond[128];

    for (steps = 0; steps < MAX_CF && !t->fail && !done; steps++) {
        const uint32_t *w = pw(t, pc * 2, 2);
        uint32_t w0, w1, op;
        bool eop;

        if (!w) {
            break;
        }
        w0 = w[0];
        w1 = w[1];
        if ((w1 >> 29) & 1) {
            uint32_t aop = (w1 >> 26) & 0xf, addr, cnt;
            if (aop == 12) {
                /* ALU_EXTENDED: this word sets KC2/KC3, the next is the clause */
                t->kc[2] = kc_slot(t, (w0 >> 22) & 0xf, w0 >> 30, (w1 >> 2) & 0xff);
                t->kc[3] = kc_slot(t, (w0 >> 26) & 0xf, w1 & 3, (w1 >> 10) & 0xff);
                pc++;
                w = pw(t, pc * 2, 2);
                if (!w) {
                    break;
                }
                w0 = w[0];
                w1 = w[1];
                aop = (w1 >> 26) & 0xf;
            } else {
                t->kc[2] = t->kc[3] = -1;
            }
            t->kc[0] = kc_slot(t, (w0 >> 22) & 0xf, w0 >> 30, (w1 >> 2) & 0xff);
            t->kc[1] = kc_slot(t, (w0 >> 26) & 0xf, w1 & 3, (w1 >> 10) & 0xff);
            addr = w0 & 0x3fffff;
            cnt = ((w1 >> 18) & 0x7f) + 1;
            if (aop == 9) {
                push(t);                                /* ALU_PUSH_BEFORE */
            }
            if (aop == 14) {
                EMIT("    before_ = act;\n");
            }
            if (aop < 8 || aop == 12) {
                fail(t, "ALU clause type %u", aop);
                break;
            }
            emit_alu_clause(t, addr, cnt);
            switch (aop) {
            case 10:                                    /* ALU_POP_AFTER */
                pop(t, 1);
                break;
            case 11:                                    /* ALU_POP2_AFTER */
                pop(t, 2);
                break;
            case 14:                                    /* ALU_BREAK */
                if (t->nloops) {
                    EMIT("    lbrk%d = lbrk%d || (before_ && !act);\n",
                         t->loop_ids[t->nloops - 1], t->loop_ids[t->nloops - 1]);
                }
                break;
            case 15:                                    /* ALU_ELSE_AFTER */
                if (t->sp > 0) {
                    EMIT("    act = stk[%d] && !act;\n", t->sp - 1);
                }
                break;
            }
            pc++;
            continue;
        }

        op = (w1 >> 22) & 0xff;
        eop = (w1 >> 21) & 1;
        cond_expr(cond, sizeof(cond), w1);
        switch (op) {
        case 0x13:                                      /* CALL_FS */
            if (t->vs && t->fetch_fs && !t->in_fs) {
                EvkFetchFn f = t->fetch;
                void *o = t->opaque;
                t->fetch = t->fetch_fs;
                t->opaque = t->fs_opaque;
                t->in_fs = true;
                tr_program(t);
                t->in_fs = false;
                t->fetch = f;
                t->opaque = o;
            }
            break;
        case 0x00:                                      /* NOP */
        case 0x1a: case 0x1e:                           /* WAIT_ACK, WAVE_SYNC */
        case 0x50: case 0x52: case 0x55: case 0x56: case 0x57:
        case 0x58: case 0x59: case 0x5a: case 0x5b: case 0x5c:
            break;
        case 0x01: case 0x02: case 0x1b: case 0x1c: {   /* TC, VC, TEX_ACK, VTX_ACK */
            uint32_t addr = w0 & 0xffffff, cnt = ((w1 >> 10) & 0x3f) + 1, k;
            for (k = 0; k < cnt && !t->fail; k++) {
                const uint32_t *f = pw(t, addr * 2 + k * 4, 4);
                if (!f) {
                    break;
                }
                if (op == 0x02 || op == 0x1c || (f[0] & 0x1f) <= 2) {
                    emit_vtx(t, f, cond);
                } else {
                    emit_tex(t, f, cond);
                }
            }
            break;
        }
        case 0x04: case 0x06: case 0x07: {              /* LOOP_START(_DX10, _NO_AL) */
            int id = t->loop_id++;
            uint32_t ci = (w1 >> 3) & 0x1f;
            if (t->nloops >= MAX_LOOPS) {
                fail(t, "loops nested deeper than %d", MAX_LOOPS);
                break;
            }
            t->loop_sp[t->nloops] = t->sp;
            t->loop_ids[t->nloops++] = id;
            EMIT("    {\n    bool lsv%d = act, lbrk%d = false;\n", id, id);
            EMIT("    uint lc%d = D.loopc[%u].%c;\n", id, ci >> 2, CH[ci & 3]);
            if (op == 0x06) {
                EMIT("    int lcnt%d = 0x7fffffff;\n", id);
            } else {
                EMIT("    int lcnt%d = int(lc%d & 0xfffu);\n", id, id);
            }
            EMIT("    int lsi%d = aL, linc%d = int(lc%d) >> 24;\n", id, id, id);
            if (op != 0x07) {
                EMIT("    aL = int((lc%d >> 12) & 0xffu);\n", id);
            }
            EMIT("    bool lrun%d = lcnt%d != 0 && quad_any(%s);\n", id, id, cond);
            EMIT("    [[dont_unroll]] for (int it%d = 0; lrun%d && it%d < 4096; it%d++) {\n",
                 id, id, id, id);
            break;
        }
        case 0x05: {                                    /* LOOP_END */
            int id;
            if (!t->nloops) {
                break;
            }
            id = t->loop_ids[--t->nloops];
            if (t->sp != t->loop_sp[t->nloops]) {
                fail(t, "unbalanced CF stack in a loop");
                break;
            }
            EMIT("    act = lsv%d && !lbrk%d;\n", id, id);
            EMIT("    lcnt%d--;\n", id);
            EMIT("    if (lcnt%d > 0 && quad_any(act && alive)) aL += linc%d; else lrun%d = false;\n",
                 id, id, id);
            EMIT("    }\n    act = lsv%d;\n    aL = lsi%d;\n    }\n", id, id);
            break;
        }
        case 0x08: case 0x09:                           /* LOOP_CONTINUE, LOOP_BREAK */
            if (t->nloops) {
                int id = t->loop_ids[t->nloops - 1];
                EMIT("    {\n        bool m_ = %s;\n", cond);
                if (op == 0x09) {
                    EMIT("        lbrk%d = lbrk%d || m_;\n", id, id);
                }
                EMIT("        act = act && !m_;\n    }\n");
            }
            break;
        case 0x0a:                                      /* JUMP */
            if ((w1 >> 8) & 3) {
                fail(t, "JUMP on condition %u", (w1 >> 8) & 3);
            }
            break;
        case 0x0b:                                      /* PUSH */
            if ((w1 >> 8) & 3) {
                fail(t, "PUSH on condition %u", (w1 >> 8) & 3);
                break;
            }
            push(t);
            break;
        case 0x0d:                                      /* ELSE */
            if (t->sp > 0) {
                EMIT("    act = stk[%d] && !act;\n", t->sp - 1);
            } else {
                EMIT("    act = !act;\n");
            }
            break;
        case 0x0e:                                      /* POP */
            pop(t, w1 & 7);
            break;
        case 0x12:                                      /* CALL */
            fail(t, "CALL");
            break;
        case 0x14:                                      /* RET */
        case 0x1f:                                      /* HALT */
            done = true;
            break;
        case 0x18:                                      /* KILL */
            EMIT("    if (%s) alive = false;\n", cond);
            break;
        case 0x53: case 0x54:                           /* EXPORT, EXPORT_DONE */
            emit_export(t, w0, w1);
            break;
        default:
            fail(t, "CF op 0x%02x", op);
            break;
        }
        if (eop) {
            break;
        }
        pc++;
    }
    if (!t->fail && steps >= MAX_CF) {
        fail(t, "program longer than %d CF instructions", MAX_CF);
    }
}

static char *tr_finish(Tr *t, Str *setup_s, const char *name);

char *evk_ps_to_glsl(EvkFetchFn fetch, void *opaque, uint32_t baryc, uint32_t in0,
                     uint32_t in1, EvkProgInfo *info, char *why, size_t whylen)
{
    Tr tr, *t = &tr;
    Str setup = { 0 };
    int i;

    memset(t, 0, sizeof(*t));
    memset(info, 0, sizeof(*info));
    t->fetch = fetch;
    t->opaque = opaque;
    t->info = info;
    t->why = why;
    t->whylen = whylen;
    t->maxreg = 0;
    for (i = 0; i < 4; i++) {
        t->kc[i] = -1;
    }
    emit_setup(&setup, baryc, in0, in1, t);
    tr_program(t);
    return tr_finish(t, &setup, "ps_run");
}

/* a copy of 'in' with every 'from' replaced by 'to' (frees 'in') */
static char *replace_all(char *in, const char *from, const char *to)
{
    Str out = { 0 };
    const char *p = in, *q;
    size_t nf = strlen(from);

    while ((q = strstr(p, from))) {
        str_add(&out, "%.*s%s", (int)(q - p), p, to);
        p = q + nf;
    }
    str_add(&out, "%s", p);
    free(in);
    return out.p;
}

char *evk_vs_to_glsl(EvkFetchFn fetch, void *opaque, EvkFetchFn fetch_fs, void *fs_opaque,
                     EvkProgInfo *info, char *why, size_t whylen)
{
    Tr tr, *t = &tr;
    Str setup = { 0 };
    char *out;
    int i;

    memset(t, 0, sizeof(*t));
    memset(info, 0, sizeof(*info));
    t->fetch = fetch;
    t->opaque = opaque;
    t->fetch_fs = fetch_fs;
    t->fs_opaque = fs_opaque;
    t->vs = true;
    t->info = info;
    t->why = why;
    t->whylen = whylen;
    for (i = 0; i < 4; i++) {
        t->kc[i] = -1;
    }
    /* VGT: R0.x the vertex index, R0.w the instance */
    use_reg(t, 0);
    str_add(&setup, "    R[0].x = uint(gl_VertexIndex);\n    R[0].w = uint(gl_InstanceIndex);\n");
    tr_program(t);
    out = tr_finish(t, &setup, "vs_run");
    if (out) {
        /* the VS banks of the constants */
        out = replace_all(out, "D.kc[", "D.vs_kc[");
        out = replace_all(out, "D.cfile[", "D.vs_cfile[");
        out = replace_all(out, "D.loopc[", "D.vs_loopc[");
        out = replace_all(out, "EVK_M_BOOLC", "EVK_M_VS_BOOLC");
    }
    return out;
}

/* the function 'name' around the translated program; NULL if it failed */
static char *tr_finish(Tr *t, Str *setup_s, const char *name)
{
    Str out = { 0 }, setup = *setup_s;
    int nreg;

    if (!t->fail && t->nloops) {
        fail(t, "LOOP_START without LOOP_END");
    }
    if (t->fail) {
        free(t->body.p);
        free(setup.p);
        return NULL;
    }
    if (t->relgpr) {
        nreg = 128;
    } else {
        nreg = compact_regs(setup.p, t->body.p);
    }
    str_add(&out, "void %s()\n{\n    uvec4 R[%d];\n", name, nreg);
    str_add(&out, "    for (int i = 0; i < %d; i++) R[i] = uvec4(0u);\n", nreg);
    str_add(&out, "    uvec4 pv = uvec4(0u);\n    uint psv = 0u;\n");
    str_add(&out, "    bool act = true, pred = false, before_ = false, kill_ = false;\n");
    str_add(&out, "    int ar = 0, aL = 0;\n");
    str_add(&out, "    bool stk[%d];\n", t->maxsp ? t->maxsp : 1);
    str_add(&out, "%s%s}\n", setup.p ? setup.p : "", t->body.p ? t->body.p : "");
    free(setup.p);
    free(t->body.p);
    return out.p;
}

/* the fragment shader template, with "@PS_PROGRAM@" where ps_run() goes */
#include "eg_vk_glsl.h"

const char *evk_frag_template(void)
{
    return evk_frag_glsl;
}
