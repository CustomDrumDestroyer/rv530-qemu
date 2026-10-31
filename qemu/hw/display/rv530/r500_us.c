/*
 * R500 fragment program ("US", unified shader) decoder, CPU interpreter,
 * disassembler and GLSL emitter.
 *
 * References:
 *   - AMD "R5xx Acceleration" v1.5, chapter 8 (Fragment Shaders): the
 *     instruction semantics, floating point rules, predicate/ALU-result
 *     handling and the flow-control stack/branch-counter model.
 *   - Mesa src/gallium/drivers/r300/compiler/r500_fragprog_emit.c (the
 *     encoder), r500_fragprog.c (r500FragmentProgramDump, whose listing
 *     format the disassembler reproduces) and r300_reg.h (bit layout).
 *
 * Register bit definitions below are copied from Mesa's r300_reg.h
 * (MIT license, Copyright 2005 Nicolai Haehnle et al.) so that this file
 * does not depend on Mesa headers.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "r500_us.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdarg.h>

/* ------------------------------------------------------------------ */
/* Register bit layout (from Mesa r300_reg.h / R5xx Acceleration 8.x)   */
/* ------------------------------------------------------------------ */

/* US_CMN_INST (dword 0) */
#define R500_INST_TYPE_MASK             (3 << 0)
#define R500_INST_TYPE_ALU              0
#define R500_INST_TYPE_OUT              1
#define R500_INST_TYPE_FC               2
#define R500_INST_TYPE_TEX              3
#define R500_INST_TEX_SEM_WAIT          (1 << 2)
#define R500_INST_RGB_PRED_SEL_SHIFT    3
#define R500_INST_RGB_PRED_INV          (1 << 6)
#define R500_INST_WRITE_INACTIVE        (1 << 7)
#define R500_INST_LAST                  (1 << 8)
#define R500_INST_NOP                   (1 << 9)
#define R500_INST_ALU_WAIT              (1 << 10)
#define R500_INST_RGB_WMASK_SHIFT       11
#define R500_INST_ALPHA_WMASK           (1 << 14)
#define R500_INST_RGB_OMASK_SHIFT       15
#define R500_INST_ALPHA_OMASK           (1 << 18)
#define R500_INST_RGB_CLAMP             (1 << 19)
#define R500_INST_ALPHA_CLAMP           (1 << 20)
#define R500_INST_ALU_RESULT_SEL        (1 << 21)
#define R500_INST_ALPHA_PRED_INV        (1 << 22)
#define R500_INST_ALU_RESULT_OP_SHIFT   23
#define R500_INST_ALPHA_PRED_SEL_SHIFT  25
#define R500_INST_STAT_WE_SHIFT         28

/* predicate select modes */
#define R500_PRED_SEL_NONE  0
#define R500_PRED_SEL_RGBA  1
#define R500_PRED_SEL_RRRR  2
#define R500_PRED_SEL_GGGG  3
#define R500_PRED_SEL_BBBB  4
#define R500_PRED_SEL_AAAA  5

/* compare ops (ALU_RESULT_OP, predicate TARGET) */
#define R500_CMP_EQ 0
#define R500_CMP_LT 1
#define R500_CMP_GE 2
#define R500_CMP_NE 3

/* US_ALU_RGB_ADDR / US_ALU_ALPHA_ADDR (dwords 1, 2) */
#define R500_ADDR0_SHIFT        0
#define R500_ADDR0_CONST        (1 << 8)
#define R500_ADDR0_REL          (1 << 9)
#define R500_ADDR1_SHIFT        10
#define R500_ADDR1_CONST        (1 << 18)
#define R500_ADDR1_REL          (1 << 19)
#define R500_ADDR2_SHIFT        20
#define R500_ADDR2_CONST        (1 << 28)
#define R500_ADDR2_REL          (1 << 29)
#define R500_SRCP_OP_SHIFT      30
#define R500_SRCP_OP_BIAS       0   /* 1 - 2*src0 */
#define R500_SRCP_OP_SUB        1   /* src1 - src0 */
#define R500_SRCP_OP_ADD        2   /* src1 + src0 */
#define R500_SRCP_OP_INV        3   /* 1 - src0 */

/* input modifiers */
#define R500_IMOD_NOP 0
#define R500_IMOD_NEG 1
#define R500_IMOD_ABS 2
#define R500_IMOD_NAB 3

/* US_ALU_RGB_INST (dword 3) */
#define R500_RGB_SEL_A_SHIFT    0
#define R500_RGB_SWZ_A_SHIFT    2       /* 3 x 3 bits: r, g, b */
#define R500_RGB_MOD_A_SHIFT    11
#define R500_RGB_SEL_B_SHIFT    13
#define R500_RGB_SWZ_B_SHIFT    15
#define R500_RGB_MOD_B_SHIFT    24
#define R500_RGB_OMOD_SHIFT     26
#define R500_RGB_TARGET_SHIFT   29
#define R500_RGB_ALU_WMASK      (1u << 31)

/* US_ALU_ALPHA_INST (dword 4) */
#define R500_ALPHA_OP_MASK      0xf
#define R500_ALPHA_ADDRD_SHIFT  4
#define R500_ALPHA_ADDRD_REL    (1 << 11)
#define R500_ALPHA_SEL_A_SHIFT  12
#define R500_ALPHA_SWZ_A_SHIFT  14
#define R500_ALPHA_MOD_A_SHIFT  17
#define R500_ALPHA_SEL_B_SHIFT  19
#define R500_ALPHA_SWZ_B_SHIFT  21
#define R500_ALPHA_MOD_B_SHIFT  24
#define R500_ALPHA_OMOD_SHIFT   26
#define R500_ALPHA_TARGET_SHIFT 29
#define R500_ALPHA_W_OMASK      (1u << 31)

/* US_ALU_RGBA_INST (dword 5) */
#define R500_RGBA_OP_MASK       0xf
#define R500_RGBA_ADDRD_SHIFT   4
#define R500_RGBA_ADDRD_REL     (1 << 11)
#define R500_RGBA_SEL_C_SHIFT   12
#define R500_RGBA_SWZ_C_SHIFT   14      /* 3 x 3 bits */
#define R500_RGBA_MOD_C_SHIFT   23
#define R500_RGBA_ALPHA_SEL_C_SHIFT 25
#define R500_RGBA_ALPHA_SWZ_C_SHIFT 27
#define R500_RGBA_ALPHA_MOD_C_SHIFT 30

/* RGB ops */
#define R500_RGB_OP_MAD 0
#define R500_RGB_OP_DP3 1
#define R500_RGB_OP_DP4 2
#define R500_RGB_OP_D2A 3
#define R500_RGB_OP_MIN 4
#define R500_RGB_OP_MAX 5
#define R500_RGB_OP_CND 7
#define R500_RGB_OP_CMP 8
#define R500_RGB_OP_FRC 9
#define R500_RGB_OP_SOP 10
#define R500_RGB_OP_MDH 11
#define R500_RGB_OP_MDV 12

/* alpha ops */
#define R500_ALPHA_OP_MAD 0
#define R500_ALPHA_OP_DP  1
#define R500_ALPHA_OP_MIN 2
#define R500_ALPHA_OP_MAX 3
#define R500_ALPHA_OP_CND 5
#define R500_ALPHA_OP_CMP 6
#define R500_ALPHA_OP_FRC 7
#define R500_ALPHA_OP_EX2 8
#define R500_ALPHA_OP_LN2 9
#define R500_ALPHA_OP_RCP 10
#define R500_ALPHA_OP_RSQ 11
#define R500_ALPHA_OP_SIN 12
#define R500_ALPHA_OP_COS 13
#define R500_ALPHA_OP_MDH 14
#define R500_ALPHA_OP_MDV 15

/* output modifiers */
#define R500_OMOD_IDENTITY 0
#define R500_OMOD_MUL_2    1
#define R500_OMOD_MUL_4    2
#define R500_OMOD_MUL_8    3
#define R500_OMOD_DIV_2    4
#define R500_OMOD_DIV_4    5
#define R500_OMOD_DIV_8    6
#define R500_OMOD_DISABLE  7

/* swizzle selects */
#define R500_SWZ_R    0
#define R500_SWZ_G    1
#define R500_SWZ_B    2
#define R500_SWZ_A    3
#define R500_SWZ_ZERO 4
#define R500_SWZ_HALF 5
#define R500_SWZ_ONE  6
#define R500_SWZ_UNUSED 7

/* US_TEX_INST (dword 1) */
#define R500_TEX_ID_SHIFT       16
#define R500_TEX_INST_SHIFT     22
#define R500_TEX_INST_NOP       0
#define R500_TEX_INST_LD        1
#define R500_TEX_INST_TEXKILL   2
#define R500_TEX_INST_PROJ      3
#define R500_TEX_INST_LODBIAS   4
#define R500_TEX_INST_LOD       5
#define R500_TEX_INST_DXDY      6
#define R500_TEX_SEM_ACQUIRE    (1 << 25)
#define R500_TEX_IGNORE_UNCOVERED (1 << 26)
#define R500_TEX_UNSCALED       (1 << 27)

/* US_TEX_ADDR (dword 2) */
#define R500_TEX_SRC_ADDR_SHIFT 0
#define R500_TEX_SRC_ADDR_REL   (1 << 7)
#define R500_TEX_SRC_SWZ_SHIFT  8       /* 4 x 2 bits: s, t, r, q */
#define R500_TEX_DST_ADDR_SHIFT 16
#define R500_TEX_DST_ADDR_REL   (1 << 23)
#define R500_TEX_DST_SWZ_SHIFT  24      /* 4 x 2 bits: r, g, b, a */

/* US_TEX_ADDR_DXDY (dword 3) */
#define R500_DX_ADDR_SHIFT      0
#define R500_DX_ADDR_REL        (1 << 7)
#define R500_DX_SWZ_SHIFT       8
#define R500_DY_ADDR_SHIFT      16
#define R500_DY_ADDR_REL        (1 << 23)
#define R500_DY_SWZ_SHIFT       24

/* US_FC_INST (dword 2) */
#define R500_FC_OP_MASK         7
#define R500_FC_OP_JUMP         0
#define R500_FC_OP_LOOP         1
#define R500_FC_OP_ENDLOOP      2
#define R500_FC_OP_REP          3
#define R500_FC_OP_ENDREP       4
#define R500_FC_OP_BREAKLOOP    5
#define R500_FC_OP_BREAKREP     6
#define R500_FC_OP_CONTINUE     7
#define R500_FC_B_ELSE          (1 << 4)
#define R500_FC_JUMP_ANY        (1 << 5)
#define R500_FC_A_OP_SHIFT      6
#define R500_FC_A_OP_NONE       0
#define R500_FC_A_OP_POP        1
#define R500_FC_A_OP_PUSH       2
#define R500_FC_JUMP_FUNC_SHIFT 8
#define R500_FC_B_POP_CNT_SHIFT 16
#define R500_FC_B_OP0_SHIFT     24
#define R500_FC_B_OP1_SHIFT     26
#define R500_FC_B_OP_NONE       0
#define R500_FC_B_OP_DECR       1
#define R500_FC_B_OP_INCR       2
#define R500_FC_IGNORE_UNCOVERED (1 << 28)

/* US_FC_ADDR (dword 3) */
#define R500_FC_BOOL_ADDR_SHIFT 0
#define R500_FC_INT_ADDR_SHIFT  8
#define R500_FC_JUMP_ADDR_SHIFT 16
#define R500_FC_JUMP_GLOBAL     (1u << 31)

/* US_CONFIG */
#define R500_ZERO_TIMES_ANYTHING_EQUALS_ZERO (1 << 1)
/* US_FC_CTRL */
#define R500_FC_FULL_FC_EN      (1u << 31)

#define US_NUM_INST 512

/* ------------------------------------------------------------------ */
/* Decoded representation                                              */
/* ------------------------------------------------------------------ */

enum { US_SRC_TEMP = 0, US_SRC_CONST = 1, US_SRC_INLINE = 2 };

typedef struct UsSrcAddr {
    uint8_t index;          /* temp 0..127, const 0..255, or 7-bit float */
    uint8_t kind;           /* US_SRC_* */
    bool    rel;            /* add aL */
} UsSrcAddr;

typedef struct UsRgbArg {
    uint8_t sel;            /* 0..2 = src0..2, 3 = srcp */
    uint8_t swz[3];         /* R500_SWZ_* per component */
    uint8_t mod;            /* R500_IMOD_* */
} UsRgbArg;

typedef struct UsAlphaArg {
    uint8_t sel;
    uint8_t swz;
    uint8_t mod;
} UsAlphaArg;

typedef struct UsAluInst {
    UsSrcAddr  rgb_addr[3], alpha_addr[3];
    uint8_t    rgb_srcp_op, alpha_srcp_op;
    UsRgbArg   rgb_arg[3];          /* A, B, C */
    UsAlphaArg alpha_arg[3];
    uint8_t    rgb_op, alpha_op;
    uint8_t    rgb_dest, alpha_dest;
    bool       rgb_dest_rel, alpha_dest_rel;
    uint8_t    rgb_omod, alpha_omod;
    uint8_t    rgb_target, alpha_target;  /* render target or predicate op */
    bool       alu_wmask;           /* write ALU result */
    bool       w_omask;             /* write depth */
} UsAluInst;

typedef struct UsTexInst {
    uint8_t unit, op;
    bool    sem_acquire, ignore_uncovered, unscaled;
    uint8_t src, dst;
    bool    src_rel, dst_rel;
    uint8_t src_swz[4], dst_swz[4];
    uint8_t dx, dy;
    bool    dx_rel, dy_rel;
    uint8_t dx_swz[4], dy_swz[4];
} UsTexInst;

typedef struct UsFcInst {
    uint8_t  op;
    bool     b_else, jump_any, ignore_uncovered, jump_global;
    uint8_t  a_op, b_op0, b_op1;
    uint8_t  jump_func, pop_cnt;
    uint8_t  bool_addr, int_addr;
    uint16_t jump_addr;
} UsFcInst;

typedef struct UsInst {
    uint32_t raw[6];
    uint8_t  type;
    bool     tex_sem_wait, write_inactive, last, nop, alu_wait;
    uint8_t  rgb_pred_sel, alpha_pred_sel;
    bool     rgb_pred_inv, alpha_pred_inv;
    uint8_t  rgb_wmask, alpha_wmask, rgb_omask, alpha_omask;
    bool     rgb_clamp, alpha_clamp;
    uint8_t  alu_result_sel, alu_result_op, stat_we;
    union {
        UsAluInst alu;
        UsTexInst tex;
        UsFcInst  fc;
    } u;
} UsInst;

struct R500UsDecoded {
    UsInst   inst[US_NUM_INST];     /* indexed by absolute address */
    int      start, end;            /* absolute addresses (offset applied) */
    int      offset;                /* US_CODE_OFFSET */
    int      count;                 /* instructions from start to end */
    bool     zero_times_anything;
    bool     full_fc;
    uint32_t fc_int_const[32];
    uint32_t fc_bool_const;
    uint32_t inputs_used, tex_units_used, outputs_written;
    bool     writes_depth, uses_kill, uses_fc;
    int      max_temp;
};

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static inline uint32_t bits(uint32_t v, int lo, int n)
{
    return (v >> lo) & ((1u << n) - 1);
}

static void decode_src_addr(uint32_t v, int shift, uint32_t const_bit,
                            uint32_t rel_bit, UsSrcAddr *s)
{
    uint32_t a = bits(v, shift, 8);
    s->rel = (v & rel_bit) != 0;
    if (v & const_bit) {
        s->kind = US_SRC_CONST;
        s->index = a;
    } else if (a & 0x80) {
        s->kind = US_SRC_INLINE;
        s->index = a & 0x7f;
    } else {
        s->kind = US_SRC_TEMP;
        s->index = a & 0x7f;
    }
}

/* 7-bit inline float: 4-bit exponent (bias 7), 3-bit mantissa, denormals
 * at exponent 0, bit pattern 0 means 2^-10 (R5xx Acceleration 8.4.1). */
static float us_inline_float(unsigned v)
{
    unsigned e = (v >> 3) & 0xf, m = v & 7;
    if (e == 0) {
        return m == 0 ? ldexpf(1.0f, -10) : ldexpf((float)m, -9);
    }
    return (1.0f + m / 8.0f) * ldexpf(1.0f, (int)e - 7);
}

static void decode_alu(const uint32_t *w, UsInst *in)
{
    UsAluInst *a = &in->u.alu;
    int i;

    decode_src_addr(w[1], R500_ADDR0_SHIFT, R500_ADDR0_CONST, R500_ADDR0_REL, &a->rgb_addr[0]);
    decode_src_addr(w[1], R500_ADDR1_SHIFT, R500_ADDR1_CONST, R500_ADDR1_REL, &a->rgb_addr[1]);
    decode_src_addr(w[1], R500_ADDR2_SHIFT, R500_ADDR2_CONST, R500_ADDR2_REL, &a->rgb_addr[2]);
    a->rgb_srcp_op = bits(w[1], R500_SRCP_OP_SHIFT, 2);
    decode_src_addr(w[2], R500_ADDR0_SHIFT, R500_ADDR0_CONST, R500_ADDR0_REL, &a->alpha_addr[0]);
    decode_src_addr(w[2], R500_ADDR1_SHIFT, R500_ADDR1_CONST, R500_ADDR1_REL, &a->alpha_addr[1]);
    decode_src_addr(w[2], R500_ADDR2_SHIFT, R500_ADDR2_CONST, R500_ADDR2_REL, &a->alpha_addr[2]);
    a->alpha_srcp_op = bits(w[2], R500_SRCP_OP_SHIFT, 2);

    a->rgb_arg[0].sel = bits(w[3], R500_RGB_SEL_A_SHIFT, 2);
    a->rgb_arg[0].mod = bits(w[3], R500_RGB_MOD_A_SHIFT, 2);
    a->rgb_arg[1].sel = bits(w[3], R500_RGB_SEL_B_SHIFT, 2);
    a->rgb_arg[1].mod = bits(w[3], R500_RGB_MOD_B_SHIFT, 2);
    a->rgb_arg[2].sel = bits(w[5], R500_RGBA_SEL_C_SHIFT, 2);
    a->rgb_arg[2].mod = bits(w[5], R500_RGBA_MOD_C_SHIFT, 2);
    for (i = 0; i < 3; i++) {
        a->rgb_arg[0].swz[i] = bits(w[3], R500_RGB_SWZ_A_SHIFT + 3 * i, 3);
        a->rgb_arg[1].swz[i] = bits(w[3], R500_RGB_SWZ_B_SHIFT + 3 * i, 3);
        a->rgb_arg[2].swz[i] = bits(w[5], R500_RGBA_SWZ_C_SHIFT + 3 * i, 3);
    }
    a->rgb_omod = bits(w[3], R500_RGB_OMOD_SHIFT, 3);
    a->rgb_target = bits(w[3], R500_RGB_TARGET_SHIFT, 2);
    a->alu_wmask = (w[3] & R500_RGB_ALU_WMASK) != 0;

    a->alpha_op = w[4] & R500_ALPHA_OP_MASK;
    a->alpha_dest = bits(w[4], R500_ALPHA_ADDRD_SHIFT, 7);
    a->alpha_dest_rel = (w[4] & R500_ALPHA_ADDRD_REL) != 0;
    a->alpha_arg[0].sel = bits(w[4], R500_ALPHA_SEL_A_SHIFT, 2);
    a->alpha_arg[0].swz = bits(w[4], R500_ALPHA_SWZ_A_SHIFT, 3);
    a->alpha_arg[0].mod = bits(w[4], R500_ALPHA_MOD_A_SHIFT, 2);
    a->alpha_arg[1].sel = bits(w[4], R500_ALPHA_SEL_B_SHIFT, 2);
    a->alpha_arg[1].swz = bits(w[4], R500_ALPHA_SWZ_B_SHIFT, 3);
    a->alpha_arg[1].mod = bits(w[4], R500_ALPHA_MOD_B_SHIFT, 2);
    a->alpha_arg[2].sel = bits(w[5], R500_RGBA_ALPHA_SEL_C_SHIFT, 2);
    a->alpha_arg[2].swz = bits(w[5], R500_RGBA_ALPHA_SWZ_C_SHIFT, 3);
    a->alpha_arg[2].mod = bits(w[5], R500_RGBA_ALPHA_MOD_C_SHIFT, 2);
    a->alpha_omod = bits(w[4], R500_ALPHA_OMOD_SHIFT, 3);
    a->alpha_target = bits(w[4], R500_ALPHA_TARGET_SHIFT, 2);
    a->w_omask = (w[4] & R500_ALPHA_W_OMASK) != 0;

    a->rgb_op = w[5] & R500_RGBA_OP_MASK;
    a->rgb_dest = bits(w[5], R500_RGBA_ADDRD_SHIFT, 7);
    a->rgb_dest_rel = (w[5] & R500_RGBA_ADDRD_REL) != 0;
}

static void decode_tex(const uint32_t *w, UsInst *in)
{
    UsTexInst *t = &in->u.tex;
    int i;

    t->unit = bits(w[1], R500_TEX_ID_SHIFT, 4);
    t->op = bits(w[1], R500_TEX_INST_SHIFT, 3);
    t->sem_acquire = (w[1] & R500_TEX_SEM_ACQUIRE) != 0;
    t->ignore_uncovered = (w[1] & R500_TEX_IGNORE_UNCOVERED) != 0;
    t->unscaled = (w[1] & R500_TEX_UNSCALED) != 0;
    t->src = bits(w[2], R500_TEX_SRC_ADDR_SHIFT, 7);
    t->src_rel = (w[2] & R500_TEX_SRC_ADDR_REL) != 0;
    t->dst = bits(w[2], R500_TEX_DST_ADDR_SHIFT, 7);
    t->dst_rel = (w[2] & R500_TEX_DST_ADDR_REL) != 0;
    t->dx = bits(w[3], R500_DX_ADDR_SHIFT, 7);
    t->dx_rel = (w[3] & R500_DX_ADDR_REL) != 0;
    t->dy = bits(w[3], R500_DY_ADDR_SHIFT, 7);
    t->dy_rel = (w[3] & R500_DY_ADDR_REL) != 0;
    for (i = 0; i < 4; i++) {
        t->src_swz[i] = bits(w[2], R500_TEX_SRC_SWZ_SHIFT + 2 * i, 2);
        t->dst_swz[i] = bits(w[2], R500_TEX_DST_SWZ_SHIFT + 2 * i, 2);
        t->dx_swz[i] = bits(w[3], R500_DX_SWZ_SHIFT + 2 * i, 2);
        t->dy_swz[i] = bits(w[3], R500_DY_SWZ_SHIFT + 2 * i, 2);
    }
}

static void decode_fc(const uint32_t *w, UsInst *in)
{
    UsFcInst *f = &in->u.fc;

    f->op = w[2] & R500_FC_OP_MASK;
    f->b_else = (w[2] & R500_FC_B_ELSE) != 0;
    f->jump_any = (w[2] & R500_FC_JUMP_ANY) != 0;
    f->a_op = bits(w[2], R500_FC_A_OP_SHIFT, 2);
    f->jump_func = bits(w[2], R500_FC_JUMP_FUNC_SHIFT, 8);
    f->pop_cnt = bits(w[2], R500_FC_B_POP_CNT_SHIFT, 5);
    f->b_op0 = bits(w[2], R500_FC_B_OP0_SHIFT, 2);
    f->b_op1 = bits(w[2], R500_FC_B_OP1_SHIFT, 2);
    f->ignore_uncovered = (w[2] & R500_FC_IGNORE_UNCOVERED) != 0;
    f->bool_addr = bits(w[3], R500_FC_BOOL_ADDR_SHIFT, 5);
    f->int_addr = bits(w[3], R500_FC_INT_ADDR_SHIFT, 5);
    f->jump_addr = bits(w[3], R500_FC_JUMP_ADDR_SHIFT, 9);
    f->jump_global = (w[3] & R500_FC_JUMP_GLOBAL) != 0;
}

static void decode_inst(const uint32_t *w, UsInst *in)
{
    memset(in, 0, sizeof(*in));
    memcpy(in->raw, w, sizeof(in->raw));
    in->type = w[0] & R500_INST_TYPE_MASK;
    in->tex_sem_wait = (w[0] & R500_INST_TEX_SEM_WAIT) != 0;
    in->rgb_pred_sel = bits(w[0], R500_INST_RGB_PRED_SEL_SHIFT, 3);
    in->rgb_pred_inv = (w[0] & R500_INST_RGB_PRED_INV) != 0;
    in->write_inactive = (w[0] & R500_INST_WRITE_INACTIVE) != 0;
    in->last = (w[0] & R500_INST_LAST) != 0;
    in->nop = (w[0] & R500_INST_NOP) != 0;
    in->alu_wait = (w[0] & R500_INST_ALU_WAIT) != 0;
    in->rgb_wmask = bits(w[0], R500_INST_RGB_WMASK_SHIFT, 3);
    in->alpha_wmask = (w[0] & R500_INST_ALPHA_WMASK) ? 1 : 0;
    in->rgb_omask = bits(w[0], R500_INST_RGB_OMASK_SHIFT, 3);
    in->alpha_omask = (w[0] & R500_INST_ALPHA_OMASK) ? 1 : 0;
    in->rgb_clamp = (w[0] & R500_INST_RGB_CLAMP) != 0;
    in->alpha_clamp = (w[0] & R500_INST_ALPHA_CLAMP) != 0;
    in->alu_result_sel = (w[0] & R500_INST_ALU_RESULT_SEL) ? 1 : 0;
    in->alpha_pred_inv = (w[0] & R500_INST_ALPHA_PRED_INV) != 0;
    in->alu_result_op = bits(w[0], R500_INST_ALU_RESULT_OP_SHIFT, 2);
    in->alpha_pred_sel = bits(w[0], R500_INST_ALPHA_PRED_SEL_SHIFT, 3);
    in->stat_we = bits(w[0], R500_INST_STAT_WE_SHIFT, 4);

    switch (in->type) {
    case R500_INST_TYPE_ALU:
    case R500_INST_TYPE_OUT:
        decode_alu(w, in);
        break;
    case R500_INST_TYPE_TEX:
        decode_tex(w, in);
        break;
    case R500_INST_TYPE_FC:
        decode_fc(w, in);
        break;
    }
}

/* ------------------------------------------------------------------ */
/* Program analysis                                                    */
/* ------------------------------------------------------------------ */

static void note_temp_read(R500UsDecoded *d, uint64_t *written, const UsSrcAddr *s)
{
    if (s->kind != US_SRC_TEMP || s->rel) {
        return;
    }
    if (s->index > d->max_temp) {
        d->max_temp = s->index;
    }
    if (s->index < R500_US_MAX_INPUTS && !(written[s->index >> 6] & (1ull << (s->index & 63)))) {
        d->inputs_used |= 1u << s->index;
    }
}

static void note_temp_read_idx(R500UsDecoded *d, uint64_t *written, unsigned idx, bool rel)
{
    UsSrcAddr s = { (uint8_t)idx, US_SRC_TEMP, rel };
    note_temp_read(d, written, &s);
}

static void note_temp_write(R500UsDecoded *d, uint64_t *written, unsigned idx)
{
    if ((int)idx > d->max_temp) {
        d->max_temp = idx;
    }
    written[idx >> 6] |= 1ull << (idx & 63);
}

/* Which of the ALU sources are actually referenced by an argument. */
static bool rgb_arg_uses_src(const UsRgbArg *a, int src)
{
    int i;
    if (a->sel == 3) {
        return src == 0 || src == 1;    /* srcp uses src0 and src1 */
    }
    if (a->sel != src) {
        return false;
    }
    for (i = 0; i < 3; i++) {
        if (a->swz[i] < 4) {
            return true;
        }
    }
    return false;
}

static bool alpha_arg_uses_src(const UsAlphaArg *a, int src)
{
    if (a->sel == 3) {
        return src == 0 || src == 1;
    }
    return a->sel == src && a->swz < 4;
}

static void analyse(R500UsDecoded *d)
{
    uint64_t written[2] = { 0, 0 };
    int n, pc;

    d->inputs_used = 0;
    d->tex_units_used = 0;
    d->outputs_written = 0;
    d->writes_depth = d->uses_kill = d->uses_fc = false;
    d->max_temp = 0;

    for (n = 0, pc = d->start; n < d->count; n++, pc = (pc + 1) % US_NUM_INST) {
        const UsInst *in = &d->inst[pc];
        int i, k;

        switch (in->type) {
        case R500_INST_TYPE_ALU:
        case R500_INST_TYPE_OUT: {
            const UsAluInst *a = &in->u.alu;
            bool rgb_dp4 = a->rgb_op == R500_RGB_OP_DP4;
            bool rgb_uses_alpha = (a->rgb_op == R500_RGB_OP_SOP);
            bool alpha_uses_rgb = (a->alpha_op == R500_ALPHA_OP_DP);
            bool rgb_active = in->rgb_wmask || (in->type == R500_INST_TYPE_OUT && in->rgb_omask) ||
                              (in->type == R500_INST_TYPE_ALU && in->rgb_omask) ||
                              (a->alu_wmask && in->alu_result_sel == 0) || alpha_uses_rgb;
            bool alpha_active = in->alpha_wmask || in->alpha_omask || a->w_omask ||
                                (a->alu_wmask && in->alu_result_sel == 1) || rgb_uses_alpha || rgb_dp4;
            for (i = 0; i < 3; i++) {
                for (k = 0; k < 3; k++) {
                    bool rgb_uses = rgb_arg_uses_src(&a->rgb_arg[k], i);
                    bool alpha_uses = alpha_arg_uses_src(&a->alpha_arg[k], i);
                    if (a->rgb_op == R500_RGB_OP_MDH || a->rgb_op == R500_RGB_OP_MDV) {
                        if ((k == 0 || k == 2) && i == 0) {
                            rgb_uses = true;
                        }
                    }
                    if (a->alpha_op == R500_ALPHA_OP_MDH || a->alpha_op == R500_ALPHA_OP_MDV) {
                        if ((k == 0 || k == 2) && i == 0) {
                            alpha_uses = true;
                        }
                    }
                    if (rgb_uses && rgb_active) {
                        note_temp_read(d, written, &a->rgb_addr[i]);
                        /* an rgb arg swizzling in .a reads alpha_addr */
                        note_temp_read(d, written, &a->alpha_addr[i]);
                    }
                    if (alpha_uses && alpha_active) {
                        note_temp_read(d, written, &a->alpha_addr[i]);
                        note_temp_read(d, written, &a->rgb_addr[i]);
                    }
                }
            }
            if (in->rgb_wmask) {
                note_temp_write(d, written, a->rgb_dest);
            }
            if (in->alpha_wmask) {
                note_temp_write(d, written, a->alpha_dest);
            }
            if (in->type == R500_INST_TYPE_OUT) {
                if (in->rgb_omask) {
                    d->outputs_written |= 1u << a->rgb_target;
                }
                if (in->alpha_omask) {
                    d->outputs_written |= 1u << a->alpha_target;
                }
                if (a->w_omask) {
                    d->writes_depth = true;
                }
            }
            break;
        }
        case R500_INST_TYPE_TEX: {
            const UsTexInst *t = &in->u.tex;
            if (t->op == R500_TEX_INST_NOP) {
                break;
            }
            note_temp_read_idx(d, written, t->src, t->src_rel);
            if (t->op == R500_TEX_INST_TEXKILL) {
                d->uses_kill = true;
                break;
            }
            d->tex_units_used |= 1u << t->unit;
            if (t->op == R500_TEX_INST_DXDY) {
                note_temp_read_idx(d, written, t->dx, t->dx_rel);
                note_temp_read_idx(d, written, t->dy, t->dy_rel);
            }
            if (in->rgb_wmask || in->alpha_wmask) {
                note_temp_write(d, written, t->dst);
            }
            break;
        }
        case R500_INST_TYPE_FC:
            d->uses_fc = true;
            break;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Public decode API                                                   */
/* ------------------------------------------------------------------ */

R500UsDecoded *r500_us_decode(const R500UsProgram *p, char *err, size_t errlen)
{
    R500UsDecoded *d;
    int start, end, offset, i, n, pc;

    if (err && errlen) {
        err[0] = 0;
    }
    if (!p) {
        if (err) {
            snprintf(err, errlen, "null program");
        }
        return NULL;
    }
    d = calloc(1, sizeof(*d));
    if (!d) {
        if (err) {
            snprintf(err, errlen, "out of memory");
        }
        return NULL;
    }
    offset = p->code_offset & 0x1ff;
    start = ((p->code_addr & 0x1ff) + offset) % US_NUM_INST;
    end = (((p->code_addr >> 16) & 0x1ff) + offset) % US_NUM_INST;
    d->start = start;
    d->end = end;
    d->offset = offset;
    d->count = ((end - start + US_NUM_INST) % US_NUM_INST) + 1;
    d->zero_times_anything = (p->config & R500_ZERO_TIMES_ANYTHING_EQUALS_ZERO) != 0;
    d->full_fc = (p->fc_ctrl & R500_FC_FULL_FC_EN) != 0;
    memcpy(d->fc_int_const, p->fc_int_const, sizeof(d->fc_int_const));
    d->fc_bool_const = p->fc_bool_const;

    for (i = 0; i < US_NUM_INST; i++) {
        uint32_t w[6];
        memcpy(w, &p->code[i * 6], sizeof(w));
        /* Direct writes to US_FC_INST_n / US_FC_ADDR_n (0x9800/0xA000)
         * alias dwords 2/3 of instruction n; honor them when the vector
         * upload left those dwords empty. */
        if (i < R500_US_MAX_FC && (w[0] & R500_INST_TYPE_MASK) == R500_INST_TYPE_FC &&
            w[2] == 0 && w[3] == 0 && (p->fc_inst[i] || p->fc_addr[i])) {
            w[2] = p->fc_inst[i];
            w[3] = p->fc_addr[i];
        }
        decode_inst(w, &d->inst[i]);
    }

    /* Validation of the active range. */
    for (n = 0, pc = start; n < d->count; n++, pc = (pc + 1) % US_NUM_INST) {
        const UsInst *in = &d->inst[pc];
        if (in->type == R500_INST_TYPE_ALU || in->type == R500_INST_TYPE_OUT) {
            const UsAluInst *a = &in->u.alu;
            if (a->rgb_op == 6 || a->rgb_op > R500_RGB_OP_MDV) {
                snprintf(err, errlen, "inst %d: reserved RGB opcode %u", pc, a->rgb_op);
                free(d);
                return NULL;
            }
            if (a->alpha_op == 4) {
                snprintf(err, errlen, "inst %d: reserved alpha opcode", pc);
                free(d);
                return NULL;
            }
        } else if (in->type == R500_INST_TYPE_TEX) {
            if (in->u.tex.op == 7) {
                snprintf(err, errlen, "inst %d: reserved TEX opcode", pc);
                free(d);
                return NULL;
            }
        }
    }
    if (d->inst[end].type != R500_INST_TYPE_OUT && err && errlen) {
        /* The hardware requires an OUTPUT at END_ADDR; we still run it. */
        snprintf(err, errlen, "warning: last instruction %d is not an OUTPUT", end);
    }
    analyse(d);
    return d;
}

void r500_us_free(R500UsDecoded *d)
{
    free(d);
}

int r500_us_num_instructions(const R500UsDecoded *d)
{
    return d ? d->count : 0;
}

uint32_t r500_us_inputs_used(const R500UsDecoded *d)
{
    return d ? d->inputs_used : 0;
}

uint32_t r500_us_tex_units_used(const R500UsDecoded *d)
{
    return d ? d->tex_units_used : 0;
}

uint32_t r500_us_outputs_written(const R500UsDecoded *d)
{
    return d ? d->outputs_written : 0;
}

bool r500_us_writes_depth(const R500UsDecoded *d)
{
    return d && d->writes_depth;
}

bool r500_us_uses_kill(const R500UsDecoded *d)
{
    return d && d->uses_kill;
}

bool r500_us_uses_flow_control(const R500UsDecoded *d)
{
    return d && d->uses_fc;
}

/* ------------------------------------------------------------------ */
/* Disassembler (format of Mesa's r500FragmentProgramDump)             */
/* ------------------------------------------------------------------ */

typedef struct UsOut {
    char *buf;
    size_t len;     /* capacity */
    size_t pos;     /* characters that would have been written */
} UsOut;

static void out_printf(UsOut *o, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

static void out_printf(UsOut *o, const char *fmt, ...)
{
    va_list ap;
    int n;
    char tmp[512];

    va_start(ap, fmt);
    n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n < 0) {
        return;
    }
    if (o->buf && o->len) {
        size_t avail = o->pos < o->len ? o->len - o->pos : 0;
        if (avail) {
            size_t c = (size_t)n < avail - 1 ? (size_t)n : avail - 1;
            memcpy(o->buf + o->pos, tmp, c);
            o->buf[o->pos + c] = 0;
        }
    }
    o->pos += n;
}

static const char *dis_swiz(unsigned v)
{
    static const char *const names[8] = { "R", "G", "B", "A", "0", "H", "1", "U" };
    return names[v & 7];
}

static const char *dis_rgb_op(unsigned v)
{
    static const char *const names[13] = {
        "MAD", "DP3", "DP4", "D2A", "MIN", "MAX", "Reserved", "CND", "CMP",
        "FRC", "SOP", "MDH", "MDV"
    };
    return v < 13 ? names[v] : "(null)";
}

static const char *dis_alpha_op(unsigned v)
{
    static const char *const names[16] = {
        "MAD", "DP", "MIN", "MAX", "Reserved", "CND", "CMP", "FRC", "EX2",
        "LN2", "RCP", "RSQ", "SIN", "COS", "MDH", "MDV"
    };
    return names[v & 15];
}

static const char *dis_mask(unsigned v)
{
    static const char *const names[16] = {
        "NONE", "R", "G", "RG", "B", "RB", "GB", "RGB", "A", "AR", "AG",
        "ARG", "AB", "ARB", "AGB", "ARGB"
    };
    return names[v & 15];
}

static const char *dis_texop(unsigned v)
{
    static const char *const names[7] = {
        "NOP", "LD", "TEXKILL", "PROJ", "LODBIAS", "LOD", "DXDY"
    };
    return v < 7 ? names[v] : "(null)";
}

static void disasm_inst(UsOut *o, int n, const uint32_t *w)
{
    uint32_t inst0 = w[0], inst;
    const char *str = "";
    int i;

    out_printf(o, "%d\t0:CMN_INST   0x%08x:", n, inst0);
    switch (inst0 & 0x3) {
    case R500_INST_TYPE_ALU: str = "ALU"; break;
    case R500_INST_TYPE_OUT: str = "OUT"; break;
    case R500_INST_TYPE_FC:  str = "FC"; break;
    case R500_INST_TYPE_TEX: str = "TEX"; break;
    }
    out_printf(o, "%s %s %s %s %s ", str,
               inst0 & R500_INST_TEX_SEM_WAIT ? "TEX_WAIT" : "",
               inst0 & R500_INST_LAST ? "LAST" : "",
               inst0 & R500_INST_NOP ? "NOP" : "",
               inst0 & R500_INST_ALU_WAIT ? "ALU WAIT" : "");
    out_printf(o, "wmask: %s omask: %s\n", dis_mask((inst0 >> 11) & 0xf),
               dis_mask((inst0 >> 15) & 0xf));

    switch (inst0 & 0x3) {
    case R500_INST_TYPE_ALU:
    case R500_INST_TYPE_OUT:
        inst = w[1];
        out_printf(o, "\t1:RGB_ADDR   0x%08x:", inst);
        out_printf(o, "Addr0: %d%c, Addr1: %d%c, Addr2: %d%c, srcp:%d\n",
                   inst & 0xff, (inst & (1 << 8)) ? 'c' : 't',
                   (inst >> 10) & 0xff, (inst & (1 << 18)) ? 'c' : 't',
                   (inst >> 20) & 0xff, (inst & (1 << 28)) ? 'c' : 't',
                   (inst >> 30));
        inst = w[2];
        out_printf(o, "\t2:ALPHA_ADDR 0x%08x:", inst);
        out_printf(o, "Addr0: %d%c, Addr1: %d%c, Addr2: %d%c, srcp:%d\n",
                   inst & 0xff, (inst & (1 << 8)) ? 'c' : 't',
                   (inst >> 10) & 0xff, (inst & (1 << 18)) ? 'c' : 't',
                   (inst >> 20) & 0xff, (inst & (1 << 28)) ? 'c' : 't',
                   (inst >> 30));
        inst = w[3];
        out_printf(o, "\t3 RGB_INST:  0x%08x:", inst);
        out_printf(o, "rgb_A_src:%d %s/%s/%s %d rgb_B_src:%d %s/%s/%s %d targ: %d\n",
                   (inst) & 0x3, dis_swiz((inst >> 2) & 0x7), dis_swiz((inst >> 5) & 0x7),
                   dis_swiz((inst >> 8) & 0x7), (inst >> 11) & 0x3,
                   (inst >> 13) & 0x3, dis_swiz((inst >> 15) & 0x7), dis_swiz((inst >> 18) & 0x7),
                   dis_swiz((inst >> 21) & 0x7), (inst >> 24) & 0x3, (inst >> 29) & 0x3);
        inst = w[4];
        out_printf(o, "\t4 ALPHA_INST:0x%08x:", inst);
        out_printf(o, "%s dest:%d%s alp_A_src:%d %s %d alp_B_src:%d %s %d targ %d w:%d\n",
                   dis_alpha_op(inst & 0xf), (inst >> 4) & 0x7f, inst & (1 << 11) ? "(rel)" : "",
                   (inst >> 12) & 0x3, dis_swiz((inst >> 14) & 0x7), (inst >> 17) & 0x3,
                   (inst >> 19) & 0x3, dis_swiz((inst >> 21) & 0x7), (inst >> 24) & 0x3,
                   (inst >> 29) & 0x3, (inst >> 31) & 0x1);
        inst = w[5];
        out_printf(o, "\t5 RGBA_INST: 0x%08x:", inst);
        out_printf(o, "%s dest:%d%s rgb_C_src:%d %s/%s/%s %d alp_C_src:%d %s %d\n",
                   dis_rgb_op(inst & 0xf), (inst >> 4) & 0x7f, inst & (1 << 11) ? "(rel)" : "",
                   (inst >> 12) & 0x3, dis_swiz((inst >> 14) & 0x7), dis_swiz((inst >> 17) & 0x7),
                   dis_swiz((inst >> 20) & 0x7), (inst >> 23) & 0x3,
                   (inst >> 25) & 0x3, dis_swiz((inst >> 27) & 0x7), (inst >> 30) & 0x3);
        break;
    case R500_INST_TYPE_FC:
        inst = w[2];
        out_printf(o, "\t2:FC_INST    0x%08x:", inst);
        out_printf(o, "0x%02x %1x ", inst >> 8 & 0xff, (inst & R500_FC_JUMP_ANY) >> 5);
        switch (inst & 0x7) {
        case R500_FC_OP_JUMP:      out_printf(o, "JUMP"); break;
        case R500_FC_OP_LOOP:      out_printf(o, "LOOP"); break;
        case R500_FC_OP_ENDLOOP:   out_printf(o, "ENDLOOP"); break;
        case R500_FC_OP_REP:       out_printf(o, "REP"); break;
        case R500_FC_OP_ENDREP:    out_printf(o, "ENDREP"); break;
        case R500_FC_OP_BREAKLOOP: out_printf(o, "BREAKLOOP"); break;
        case R500_FC_OP_BREAKREP:  out_printf(o, "BREAKREP"); break;
        case R500_FC_OP_CONTINUE:  out_printf(o, "CONTINUE"); break;
        }
        out_printf(o, " ");
        switch ((inst >> 6) & 3) {
        case R500_FC_A_OP_NONE: out_printf(o, "NONE"); break;
        case R500_FC_A_OP_POP:  out_printf(o, "POP"); break;
        case R500_FC_A_OP_PUSH: out_printf(o, "PUSH"); break;
        }
        for (i = 0; i < 2; i++) {
            out_printf(o, " ");
            switch ((inst >> (24 + i * 2)) & 3) {
            case R500_FC_B_OP_NONE: out_printf(o, "NONE"); break;
            case R500_FC_B_OP_DECR: out_printf(o, "DECR"); break;
            case R500_FC_B_OP_INCR: out_printf(o, "INCR"); break;
            }
        }
        out_printf(o, " %d %1x", (inst >> 16) & 0x1f, (inst & R500_FC_B_ELSE) >> 4);
        out_printf(o, " %d", w[3] >> 16);
        if (inst & R500_FC_IGNORE_UNCOVERED) {
            out_printf(o, " IGN_UNC");
        }
        inst = w[3];
        out_printf(o, "\n\t3:FC_ADDR    0x%08x:", inst);
        out_printf(o, "BOOL: 0x%02x, INT: 0x%02x, JUMP_ADDR: %d, JMP_GLBL: %1x\n",
                   inst & 0x1f, (inst >> 8) & 0x1f, (inst >> 16) & 0x1ff, inst >> 31);
        break;
    case R500_INST_TYPE_TEX:
        inst = w[1];
        out_printf(o, "\t1:TEX_INST:  0x%08x: id: %d op:%s, %s, %s %s\n", inst, (inst >> 16) & 0xf,
                   dis_texop((inst >> 22) & 0x7), (inst & (1 << 25)) ? "ACQ" : "",
                   (inst & (1 << 26)) ? "IGNUNC" : "", (inst & (1 << 27)) ? "UNSCALED" : "SCALED");
        inst = w[2];
        out_printf(o, "\t2:TEX_ADDR:  0x%08x: src: %d%s %s/%s/%s/%s dst: %d%s %s/%s/%s/%s\n", inst,
                   inst & 127, inst & (1 << 7) ? "(rel)" : "",
                   dis_swiz((inst >> 8) & 0x3), dis_swiz((inst >> 10) & 0x3),
                   dis_swiz((inst >> 12) & 0x3), dis_swiz((inst >> 14) & 0x3),
                   (inst >> 16) & 127, inst & (1 << 23) ? "(rel)" : "",
                   dis_swiz((inst >> 24) & 0x3), dis_swiz((inst >> 26) & 0x3),
                   dis_swiz((inst >> 28) & 0x3), dis_swiz((inst >> 30) & 0x3));
        out_printf(o, "\t3:TEX_DXDY:  0x%08x\n", w[3]);
        break;
    }
    out_printf(o, "\n");
}

size_t r500_us_disasm(const R500UsProgram *p, char *buf, size_t len)
{
    UsOut o = { buf, len, 0 };
    int offset, start, end, count, n, pc;

    if (buf && len) {
        buf[0] = 0;
    }
    if (!p) {
        return 0;
    }
    offset = p->code_offset & 0x1ff;
    start = ((p->code_addr & 0x1ff) + offset) % US_NUM_INST;
    end = (((p->code_addr >> 16) & 0x1ff) + offset) % US_NUM_INST;
    count = ((end - start + US_NUM_INST) % US_NUM_INST) + 1;

    out_printf(&o, "R500 Fragment Program:\n--------\n");
    for (n = 0, pc = start; n < count; n++, pc = (pc + 1) % US_NUM_INST) {
        disasm_inst(&o, pc, &p->code[pc * 6]);
    }
    return o.pos;
}

/* ------------------------------------------------------------------ */
/* Interpreter                                                         */
/* ------------------------------------------------------------------ */

#define US_MAX_LOOP_DEPTH   8
#define US_MAX_ADDR_DEPTH   8
#define US_MAX_STEPS        (1 << 22)

typedef struct UsLoop {
    int     count;              /* remaining iterations */
    int     aL, incr;
    bool    is_loop;            /* LOOP (has aL) vs REP */
    uint8_t brk_mask;           /* pixels disabled by BREAK in this loop */
    uint8_t cont_mask;          /* pixels disabled by CONTINUE in this loop */
} UsLoop;

typedef struct UsState {
    const R500UsDecoded *d;
    const R500UsExec *e;
    const R500UsQuadIn *in;
    R500UsQuadOut *out;
    r5xx_vec4 temps[4][R500_US_MAX_TEMPS];
    uint8_t pred[4];            /* bit 0 = R ... bit 3 = A */
    bool    alu_result[4];
    bool    active[4];          /* branch-active */
    bool    done[4];            /* LAST bit executed */
    int     bcnt[4];            /* branch counter (meaningful when inactive) */
    uint8_t loop_inactive;      /* bit p: disabled by BREAK/CONTINUE */
    uint8_t covered;            /* coverage minus killed pixels */
    UsLoop  loops[US_MAX_LOOP_DEPTH];
    int     nloops;
    int     astack[US_MAX_ADDR_DEPTH];
    int     nastack;
    int     pc;
} UsState;

static int us_cur_aL(const UsState *s)
{
    int i;
    for (i = s->nloops - 1; i >= 0; i--) {
        if (s->loops[i].is_loop) {
            return s->loops[i].aL;
        }
    }
    return 0;
}

static bool us_branch_inactive(const UsState *s, int p)
{
    return !s->active[p] && !s->done[p] && !(s->loop_inactive & (1 << p));
}

/* Fetch a full RGBA source: rgb from one address, alpha from another. */
static r5xx_vec4 us_fetch(const UsState *s, int p, const UsSrcAddr *rgb,
                          const UsSrcAddr *alpha)
{
    r5xx_vec4 v;
    const UsSrcAddr *src[2] = { rgb, alpha };
    int k, aL = us_cur_aL(s);

    for (k = 0; k < 2; k++) {
        const UsSrcAddr *a = src[k];
        r5xx_vec4 r;
        switch (a->kind) {
        case US_SRC_TEMP:
            r = s->temps[p][(a->index + (a->rel ? aL : 0)) & (R500_US_MAX_TEMPS - 1)];
            break;
        case US_SRC_CONST: {
            unsigned idx = (a->index + (a->rel ? aL : 0)) & (R500_US_MAX_CONST - 1);
            if (s->e->consts) {
                r = s->e->consts[idx];
            } else {
                r = r5xx_vec4_make(0, 0, 0, 0);
            }
            break;
        }
        default: {
            float f = us_inline_float(a->index);
            r = r5xx_vec4_make(f, f, f, f);
            break;
        }
        }
        if (k == 0) {
            v.f[0] = r.f[0];
            v.f[1] = r.f[1];
            v.f[2] = r.f[2];
        } else {
            v.f[3] = r.f[3];
        }
    }
    return v;
}

static float us_srcp(unsigned op, float s0, float s1)
{
    switch (op) {
    case R500_SRCP_OP_BIAS: return 1.0f - 2.0f * s0;
    case R500_SRCP_OP_SUB:  return s1 - s0;
    case R500_SRCP_OP_ADD:  return s1 + s0;
    default:                return 1.0f - s0;
    }
}

static float us_imod(float v, unsigned mod)
{
    switch (mod) {
    case R500_IMOD_NEG: return -v;
    case R500_IMOD_ABS: return fabsf(v);
    case R500_IMOD_NAB: return -fabsf(v);
    default:            return v;
    }
}

static float us_swz_value(const r5xx_vec4 *v, unsigned swz)
{
    switch (swz) {
    case R500_SWZ_R: case R500_SWZ_G: case R500_SWZ_B: case R500_SWZ_A:
        return v->f[swz];
    case R500_SWZ_HALF: return 0.5f;
    case R500_SWZ_ONE:  return 1.0f;
    default:            return 0.0f;
    }
}

static float us_mul(const UsState *s, float a, float b)
{
    if (s->d->zero_times_anything && (a == 0.0f || b == 0.0f)) {
        return 0.0f;
    }
    return a * b;
}

static float us_omod(float v, unsigned omod, bool clamp)
{
    switch (omod) {
    case R500_OMOD_MUL_2: v *= 2.0f; break;
    case R500_OMOD_MUL_4: v *= 4.0f; break;
    case R500_OMOD_MUL_8: v *= 8.0f; break;
    case R500_OMOD_DIV_2: v *= 0.5f; break;
    case R500_OMOD_DIV_4: v *= 0.25f; break;
    case R500_OMOD_DIV_8: v *= 0.125f; break;
    case R500_OMOD_DISABLE: return v;   /* no modification, no clamp */
    default: break;
    }
    if (fpclassify(v) == FP_SUBNORMAL) {
        v = copysignf(0.0f, v);
    }
    if (clamp) {
        if (!(v >= 0.0f)) {         /* also catches NaN */
            v = 0.0f;
        } else if (v > 1.0f) {
            v = 1.0f;
        }
    }
    return v;
}

static bool us_compare(float v, unsigned op)
{
    if (fpclassify(v) == FP_SUBNORMAL) {
        v = 0.0f;
    }
    switch (op) {
    case R500_CMP_EQ: return v == 0.0f;
    case R500_CMP_LT: return v < 0.0f;
    case R500_CMP_GE: return v >= 0.0f;
    default:          return v != 0.0f;
    }
}

/* 4-bit channel mask selected by a predicate select. */
static unsigned us_pred_mask(unsigned sel, bool inv, unsigned pred)
{
    unsigned m;
    switch (sel) {
    case R500_PRED_SEL_RGBA: m = pred & 0xf; break;
    case R500_PRED_SEL_RRRR: m = (pred & 1) ? 0xf : 0; break;
    case R500_PRED_SEL_GGGG: m = (pred & 2) ? 0xf : 0; break;
    case R500_PRED_SEL_BBBB: m = (pred & 4) ? 0xf : 0; break;
    case R500_PRED_SEL_AAAA: m = (pred & 8) ? 0xf : 0; break;
    default: return 0xf;
    }
    return inv ? (m ^ 0xf) : m;
}

static float us_transcendental(unsigned op, float a)
{
    switch (op) {
    case R500_ALPHA_OP_EX2: return exp2f(a);
    case R500_ALPHA_OP_LN2: return log2f(a);
    case R500_ALPHA_OP_RCP: return 1.0f / a;
    case R500_ALPHA_OP_RSQ:
        if (a == 0.0f) {
            return INFINITY;            /* RSQ(-0.0) = +Inf per 8.7.3 */
        }
        return 1.0f / sqrtf(a);
    case R500_ALPHA_OP_SIN: return sinf(a * 6.283185307179586f);
    case R500_ALPHA_OP_COS: return cosf(a * 6.283185307179586f);
    default: return a;
    }
}

static bool us_tex_coverage_ok(const UsState *s, int p, bool ignore_uncovered)
{
    return !ignore_uncovered || (s->covered & (1 << p));
}

static void us_exec_alu(UsState *s, const UsInst *in)
{
    const UsAluInst *a = &in->u.alu;
    r5xx_vec4 src[4][4];        /* [pixel][src0, src1, src2, srcp] */
    float argA[4][4], argB[4][4], argC[4][4];   /* [pixel][r,g,b,a] */
    float res_rgb[4][3], res_a[4];
    int p, c, k, aL = us_cur_aL(s);

    for (p = 0; p < 4; p++) {
        for (k = 0; k < 3; k++) {
            src[p][k] = us_fetch(s, p, &a->rgb_addr[k], &a->alpha_addr[k]);
        }
        for (c = 0; c < 3; c++) {
            src[p][3].f[c] = us_srcp(a->rgb_srcp_op, src[p][0].f[c], src[p][1].f[c]);
        }
        src[p][3].f[3] = us_srcp(a->alpha_srcp_op, src[p][0].f[3], src[p][1].f[3]);
    }

    /* Evaluate inputs A, B, C for the rgb and alpha units. */
    for (p = 0; p < 4; p++) {
        float *args[3] = { argA[p], argB[p], argC[p] };
        for (k = 0; k < 3; k++) {
            const r5xx_vec4 *sv = &src[p][a->rgb_arg[k].sel];
            const r5xx_vec4 *av = &src[p][a->alpha_arg[k].sel];
            for (c = 0; c < 3; c++) {
                args[k][c] = us_imod(us_swz_value(sv, a->rgb_arg[k].swz[c]), a->rgb_arg[k].mod);
            }
            args[k][3] = us_imod(us_swz_value(av, a->alpha_arg[k].swz), a->alpha_arg[k].mod);
        }
    }
    /* MDH/MDV: A forced to top-left src0, C to top-right/bottom-left src0
     * (selects and swizzles ignored, modifiers respected). */
    if (a->rgb_op == R500_RGB_OP_MDH || a->rgb_op == R500_RGB_OP_MDV) {
        int q = a->rgb_op == R500_RGB_OP_MDH ? 1 : 2;
        for (p = 0; p < 4; p++) {
            for (c = 0; c < 3; c++) {
                argA[p][c] = us_imod(src[0][0].f[c], a->rgb_arg[0].mod);
                argC[p][c] = us_imod(src[q][0].f[c], a->rgb_arg[2].mod);
            }
        }
    }
    if (a->alpha_op == R500_ALPHA_OP_MDH || a->alpha_op == R500_ALPHA_OP_MDV) {
        int q = a->alpha_op == R500_ALPHA_OP_MDH ? 1 : 2;
        for (p = 0; p < 4; p++) {
            argA[p][3] = us_imod(src[0][0].f[3], a->alpha_arg[0].mod);
            argC[p][3] = us_imod(src[q][0].f[3], a->alpha_arg[2].mod);
        }
    }

    for (p = 0; p < 4; p++) {
        const float *A = argA[p], *B = argB[p], *C = argC[p];
        float dot = 0.0f, alpha = 0.0f;
        bool rgb_scalar = false;

        /* RGB unit */
        switch (a->rgb_op) {
        case R500_RGB_OP_MAD:
        case R500_RGB_OP_MDH:
        case R500_RGB_OP_MDV:
            for (c = 0; c < 3; c++) {
                res_rgb[p][c] = us_mul(s, A[c], B[c]) + C[c];
            }
            break;
        case R500_RGB_OP_DP3:
            dot = us_mul(s, A[0], B[0]) + us_mul(s, A[1], B[1]) + us_mul(s, A[2], B[2]);
            rgb_scalar = true;
            break;
        case R500_RGB_OP_DP4:
            dot = us_mul(s, A[0], B[0]) + us_mul(s, A[1], B[1]) + us_mul(s, A[2], B[2]) +
                  us_mul(s, A[3], B[3]);
            rgb_scalar = true;
            break;
        case R500_RGB_OP_D2A:
            dot = us_mul(s, A[0], B[0]) + us_mul(s, A[1], B[1]) + C[2];
            rgb_scalar = true;
            break;
        case R500_RGB_OP_MIN:
            for (c = 0; c < 3; c++) {
                res_rgb[p][c] = A[c] < B[c] ? A[c] : B[c];
            }
            break;
        case R500_RGB_OP_MAX:
            for (c = 0; c < 3; c++) {
                res_rgb[p][c] = A[c] >= B[c] ? A[c] : B[c];
            }
            break;
        case R500_RGB_OP_CND:
            for (c = 0; c < 3; c++) {
                res_rgb[p][c] = C[c] > 0.5f ? A[c] : B[c];
            }
            break;
        case R500_RGB_OP_CMP:
            for (c = 0; c < 3; c++) {
                res_rgb[p][c] = C[c] >= 0.0f ? A[c] : B[c];
            }
            break;
        case R500_RGB_OP_FRC:
            for (c = 0; c < 3; c++) {
                res_rgb[p][c] = A[c] - floorf(A[c]);
            }
            break;
        case R500_RGB_OP_SOP:
        default:
            /* replicated transcendental result, filled in below */
            break;
        }
        if (rgb_scalar) {
            res_rgb[p][0] = res_rgb[p][1] = res_rgb[p][2] = dot;
        }

        /* alpha unit */
        switch (a->alpha_op) {
        case R500_ALPHA_OP_MAD:
        case R500_ALPHA_OP_MDH:
        case R500_ALPHA_OP_MDV:
            alpha = us_mul(s, A[3], B[3]) + C[3];
            break;
        case R500_ALPHA_OP_DP:
            alpha = rgb_scalar ? dot : res_rgb[p][0];
            break;
        case R500_ALPHA_OP_MIN:
            alpha = A[3] < B[3] ? A[3] : B[3];
            break;
        case R500_ALPHA_OP_MAX:
            alpha = A[3] >= B[3] ? A[3] : B[3];
            break;
        case R500_ALPHA_OP_CND:
            alpha = C[3] > 0.5f ? A[3] : B[3];
            break;
        case R500_ALPHA_OP_CMP:
            alpha = C[3] >= 0.0f ? A[3] : B[3];
            break;
        case R500_ALPHA_OP_FRC:
            alpha = A[3] - floorf(A[3]);
            break;
        default:
            alpha = us_transcendental(a->alpha_op, A[3]);
            break;
        }
        if (a->rgb_op == R500_RGB_OP_SOP) {
            res_rgb[p][0] = res_rgb[p][1] = res_rgb[p][2] = alpha;
        }
        res_a[p] = alpha;
    }

    /* Output modifiers, clamping, writes. */
    for (p = 0; p < 4; p++) {
        unsigned rgb_pm = us_pred_mask(in->rgb_pred_sel, in->rgb_pred_inv, s->pred[p]);
        unsigned a_pm = us_pred_mask(in->alpha_pred_sel, in->alpha_pred_inv, s->pred[p]);
        bool may_write = (s->active[p] && !s->done[p]) || in->write_inactive;
        float r[3], av;

        for (c = 0; c < 3; c++) {
            r[c] = us_omod(res_rgb[p][c], a->rgb_omod, in->rgb_clamp);
        }
        av = us_omod(res_a[p], a->alpha_omod, in->alpha_clamp);

        if (a->alu_wmask) {
            s->alu_result[p] = us_compare(in->alu_result_sel ? av : r[0], in->alu_result_op);
        }
        if (!may_write) {
            continue;
        }
        if (in->rgb_wmask) {
            r5xx_vec4 *dst = &s->temps[p][(a->rgb_dest + (a->rgb_dest_rel ? aL : 0)) & 127];
            for (c = 0; c < 3; c++) {
                if ((in->rgb_wmask >> c) & 1 && (rgb_pm >> c) & 1) {
                    dst->f[c] = r[c];
                }
            }
        }
        if (in->alpha_wmask && (a_pm & 8)) {
            r5xx_vec4 *dst = &s->temps[p][(a->alpha_dest + (a->alpha_dest_rel ? aL : 0)) & 127];
            dst->f[3] = av;
        }
        if (in->type == R500_INST_TYPE_OUT) {
            for (c = 0; c < 3; c++) {
                if ((in->rgb_omask >> c) & 1 && (rgb_pm >> c) & 1) {
                    s->out->color[p][a->rgb_target].f[c] = r[c];
                }
            }
            if (in->alpha_omask && (a_pm & 8)) {
                s->out->color[p][a->alpha_target].f[3] = av;
            }
            if (a->w_omask && (a_pm & 8)) {
                s->out->depth[p] = av;
                s->out->depth_written = true;
            }
        } else {
            /* ALU instruction: OMASK selects predicate bits to set. */
            for (c = 0; c < 3; c++) {
                if ((in->rgb_omask >> c) & 1) {
                    if (us_compare(r[c], a->rgb_target)) {
                        s->pred[p] |= 1 << c;
                    } else {
                        s->pred[p] &= ~(1 << c);
                    }
                }
            }
            if (in->alpha_omask) {
                if (us_compare(av, a->alpha_target)) {
                    s->pred[p] |= 8;
                } else {
                    s->pred[p] &= ~8;
                }
            }
        }
    }
}

static r5xx_vec4 us_tex_coord(const UsState *s, int p, unsigned addr, bool rel,
                              const uint8_t *swz)
{
    const r5xx_vec4 *v = &s->temps[p][(addr + (rel ? us_cur_aL(s) : 0)) & 127];
    r5xx_vec4 r;
    int c;
    for (c = 0; c < 4; c++) {
        r.f[c] = v->f[swz[c] & 3];
    }
    return r;
}

static void us_exec_tex(UsState *s, const UsInst *in)
{
    const UsTexInst *t = &in->u.tex;
    R5xxTexRequest req;
    r5xx_vec4 res[4];
    int p, c, aL = us_cur_aL(s);

    if (t->op == R500_TEX_INST_NOP) {
        return;
    }
    if (t->op == R500_TEX_INST_TEXKILL) {
        for (p = 0; p < 4; p++) {
            const r5xx_vec4 *v;
            unsigned mask, pm;
            if (!((s->active[p] && !s->done[p]) || in->write_inactive)) {
                continue;
            }
            /* source swizzle ignored; write masks and predication limit
             * the channels examined */
            v = &s->temps[p][(t->src + (t->src_rel ? aL : 0)) & 127];
            pm = (us_pred_mask(in->rgb_pred_sel, in->rgb_pred_inv, s->pred[p]) & 7) |
                 (us_pred_mask(in->alpha_pred_sel, in->alpha_pred_inv, s->pred[p]) & 8);
            mask = (in->rgb_wmask | (in->alpha_wmask << 3)) & pm;
            for (c = 0; c < 4; c++) {
                float f = v->f[c];
                /* R500 KIL treats -0.0 as negative (Mesa radeon_pair_translate.c) */
                if ((mask >> c) & 1 && !isnan(f) && signbit(f)) {
                    s->out->killed |= 1 << p;
                    s->covered &= ~(1 << p);
                    break;
                }
            }
        }
        return;
    }

    memset(&req, 0, sizeof(req));
    req.unit = t->unit;
    req.unscaled = t->unscaled;
    switch (t->op) {
    case R500_TEX_INST_PROJ:    req.op = R5XX_TEXOP_PROJ; break;
    case R500_TEX_INST_LODBIAS: req.op = R5XX_TEXOP_BIAS; break;
    case R500_TEX_INST_LOD:     req.op = R5XX_TEXOP_LOD; break;
    case R500_TEX_INST_DXDY:    req.op = R5XX_TEXOP_GRAD; break;
    default:                    req.op = R5XX_TEXOP_SAMPLE; break;
    }
    for (p = 0; p < 4; p++) {
        req.coord[p] = us_tex_coord(s, p, t->src, t->src_rel, t->src_swz);
        if (t->op == R500_TEX_INST_LODBIAS || t->op == R500_TEX_INST_LOD) {
            req.lod[p] = req.coord[p].f[3];
        }
        if (t->op == R500_TEX_INST_DXDY) {
            req.ddx[p] = us_tex_coord(s, p, t->dx, t->dx_rel, t->dx_swz);
            req.ddy[p] = us_tex_coord(s, p, t->dy, t->dy_rel, t->dy_swz);
        }
    }
    memset(res, 0, sizeof(res));
    if (s->e->tex) {
        s->e->tex(s->e->tex_opaque, &req, res);
    }
    for (p = 0; p < 4; p++) {
        r5xx_vec4 *dst;
        unsigned mask, pm;
        if (!((s->active[p] && !s->done[p]) || in->write_inactive)) {
            continue;
        }
        if (!us_tex_coverage_ok(s, p, t->ignore_uncovered)) {
            continue;
        }
        pm = (us_pred_mask(in->rgb_pred_sel, in->rgb_pred_inv, s->pred[p]) & 7) |
             (us_pred_mask(in->alpha_pred_sel, in->alpha_pred_inv, s->pred[p]) & 8);
        mask = (in->rgb_wmask | (in->alpha_wmask << 3)) & pm;
        dst = &s->temps[p][(t->dst + (t->dst_rel ? aL : 0)) & 127];
        for (c = 0; c < 4; c++) {
            if ((mask >> c) & 1) {
                dst->f[c] = res[p].f[t->dst_swz[c] & 3];
            }
        }
    }
}

/* Branch-counter operation on the pixels (R5xx Acceleration 8.6.3.2). */
static void us_branch_op(UsState *s, unsigned op, unsigned pop_cnt,
                         bool jump, const bool *want, uint8_t participants)
{
    int p;
    switch (op) {
    case R500_FC_B_OP_DECR:
        for (p = 0; p < 4; p++) {
            if (us_branch_inactive(s, p)) {
                s->bcnt[p] -= (int)pop_cnt;
                if (s->bcnt[p] < 0) {
                    s->bcnt[p] = 0;
                    s->active[p] = true;
                }
            }
        }
        break;
    case R500_FC_B_OP_INCR:
        for (p = 0; p < 4; p++) {
            if (us_branch_inactive(s, p)) {
                s->bcnt[p] += 1;
            } else if (s->active[p] && !s->done[p] && (participants & (1 << p)) &&
                       want[p] != jump) {
                s->active[p] = false;
                s->bcnt[p] = 0;
            }
        }
        break;
    default:
        break;
    }
}

/* Jump decision for the active pixels. */
static bool us_fc_decide(const UsState *s, const UsFcInst *f, const bool *want,
                         uint8_t participants, bool inhibited)
{
    int p;
    bool any = false, all = true, have = false;
    for (p = 0; p < 4; p++) {
        if (!(participants & (1 << p))) {
            continue;
        }
        have = true;
        any = any || want[p];
        all = all && want[p];
    }
    if (inhibited) {
        return false;
    }
    if (!have) {
        return !f->jump_any;
    }
    return f->jump_any ? any : all;
}

static void us_exec_fc(UsState *s, const UsInst *in)
{
    const UsFcInst *f = &in->u.fc;
    bool want[4], jump;
    uint8_t participants = 0;
    int p, target, next = s->pc + 1;
    bool boolean = (s->d->fc_bool_const >> f->bool_addr) & 1;
    UsLoop *top = s->nloops ? &s->loops[s->nloops - 1] : NULL;

    target = f->jump_global ? f->jump_addr : (f->jump_addr + s->d->offset) % US_NUM_INST;

    if (f->b_else) {
        /* Active pixels become branch-inactive with zero counter and
         * branch-inactive pixels with zero counter become active. */
        for (p = 0; p < 4; p++) {
            if (s->done[p] || (s->loop_inactive & (1 << p))) {
                continue;
            }
            if (s->active[p]) {
                s->active[p] = false;
                s->bcnt[p] = 0;
            } else if (s->bcnt[p] == 0) {
                s->active[p] = true;
            }
        }
    }

    if ((f->op == R500_FC_OP_ENDLOOP || f->op == R500_FC_OP_ENDREP) && top) {
        /* pixels that executed CONTINUE rejoin for the decision */
        for (p = 0; p < 4; p++) {
            if (top->cont_mask & (1 << p)) {
                s->loop_inactive &= ~(1 << p);
                s->active[p] = true;
            }
        }
        top->cont_mask = 0;
    }

    for (p = 0; p < 4; p++) {
        unsigned pred, idx;
        want[p] = false;
        if (!s->active[p] || s->done[p]) {
            continue;
        }
        if (f->ignore_uncovered && !(s->covered & (1 << p))) {
            continue;
        }
        participants |= 1 << p;
        pred = us_pred_mask(in->rgb_pred_sel, in->rgb_pred_inv, s->pred[p]) & 1;
        if (in->rgb_pred_sel == R500_PRED_SEL_NONE) {
            pred = in->rgb_pred_inv ? 1 : 0;
        }
        idx = (s->alu_result[p] ? 4 : 0) | (pred ? 2 : 0) | (boolean ? 1 : 0);
        want[p] = (f->jump_func >> idx) & 1;
    }

    switch (f->op) {
    case R500_FC_OP_LOOP:
    case R500_FC_OP_REP: {
        uint32_t k = s->d->fc_int_const[f->int_addr & 31];
        int count = k & 0xff;
        if (count == 0) {
            jump = true;
        } else {
            jump = us_fc_decide(s, f, want, participants, false);
        }
        us_branch_op(s, jump ? f->b_op1 : f->b_op0, f->pop_cnt, jump, want, participants);
        if (!jump) {
            if (s->nloops < US_MAX_LOOP_DEPTH) {
                UsLoop *l = &s->loops[s->nloops++];
                l->count = count;
                l->aL = (k >> 8) & 0xff;
                l->incr = (int8_t)((k >> 16) & 0xff);
                l->is_loop = f->op == R500_FC_OP_LOOP;
                l->brk_mask = l->cont_mask = 0;
            }
        }
        break;
    }
    case R500_FC_OP_ENDLOOP:
    case R500_FC_OP_ENDREP:
        if (top) {
            top->count--;
            if (top->count <= 0) {
                jump = false;
            } else {
                jump = us_fc_decide(s, f, want, participants, false);
            }
        } else {
            jump = false;
        }
        us_branch_op(s, jump ? f->b_op1 : f->b_op0, f->pop_cnt, jump, want, participants);
        if (top) {
            if (jump) {
                if (top->is_loop) {
                    top->aL += top->incr;
                }
            } else {
                for (p = 0; p < 4; p++) {
                    if (top->brk_mask & (1 << p)) {
                        s->loop_inactive &= ~(1 << p);
                        s->active[p] = true;
                    }
                }
                s->nloops--;
            }
        }
        break;
    case R500_FC_OP_BREAKLOOP:
    case R500_FC_OP_BREAKREP:
    case R500_FC_OP_CONTINUE: {
        bool inhibited = false;
        for (p = 0; p < 4; p++) {
            if (us_branch_inactive(s, p)) {
                inhibited = true;
            }
            if (f->op != R500_FC_OP_CONTINUE && top && (top->cont_mask & (1 << p))) {
                inhibited = true;
            }
        }
        jump = us_fc_decide(s, f, want, participants, inhibited);
        us_branch_op(s, jump ? f->b_op1 : f->b_op0, f->pop_cnt, jump, want, participants);
        if (jump) {
            if (f->op != R500_FC_OP_CONTINUE && top) {
                for (p = 0; p < 4; p++) {
                    if ((top->brk_mask | top->cont_mask) & (1 << p)) {
                        s->loop_inactive &= ~(1 << p);
                        s->active[p] = true;
                    }
                }
                s->nloops--;
            }
        } else {
            for (p = 0; p < 4; p++) {
                if ((participants & (1 << p)) && want[p] && s->active[p]) {
                    s->active[p] = false;
                    s->loop_inactive |= 1 << p;
                    if (top) {
                        if (f->op == R500_FC_OP_CONTINUE) {
                            top->cont_mask |= 1 << p;
                        } else {
                            top->brk_mask |= 1 << p;
                        }
                    }
                }
            }
        }
        break;
    }
    default: /* JUMP */
        jump = us_fc_decide(s, f, want, participants, false);
        us_branch_op(s, jump ? f->b_op1 : f->b_op0, f->pop_cnt, jump, want, participants);
        if (jump) {
            if (f->a_op == R500_FC_A_OP_PUSH) {
                if (s->nastack < US_MAX_ADDR_DEPTH) {
                    s->astack[s->nastack++] = next % US_NUM_INST;
                }
            } else if (f->a_op == R500_FC_A_OP_POP) {
                if (s->nastack > 0) {
                    target = s->astack[--s->nastack];
                }
            }
        }
        break;
    }
    s->pc = jump ? target : next;
}

void r500_us_run_quad(const R500UsExec *e, const R500UsQuadIn *in,
                      R500UsQuadOut *out)
{
    UsState *s;
    const R500UsDecoded *d;
    int p, i;
    long steps = 0;

    memset(out, 0, sizeof(*out));
    if (!e || !e->prog || !in) {
        return;
    }
    d = e->prog;
    /* one state per thread, reused (rasterizer worker threads run quads
     * concurrently); zeroed like the calloc it replaces */
    {
        static __thread UsState *tls_state;
        if (!tls_state) {
            tls_state = malloc(sizeof(*tls_state));
            if (!tls_state) {
                return;
            }
        }
        s = tls_state;
    }
    memset(s, 0, sizeof(*s));
    s->d = d;
    s->e = e;
    s->in = in;
    s->out = out;
    s->covered = in->coverage;
    for (p = 0; p < 4; p++) {
        s->active[p] = true;
        for (i = 0; i < R500_US_MAX_INPUTS; i++) {
            s->temps[p][i] = in->inputs[p][i];
        }
    }
    s->pc = d->start;

    for (;;) {
        const UsInst *inst = &d->inst[s->pc];
        int cur = s->pc;
        bool all_done;

        switch (inst->type) {
        case R500_INST_TYPE_ALU:
        case R500_INST_TYPE_OUT:
            us_exec_alu(s, inst);
            s->pc = (cur + 1) % US_NUM_INST;
            break;
        case R500_INST_TYPE_TEX:
            us_exec_tex(s, inst);
            s->pc = (cur + 1) % US_NUM_INST;
            break;
        case R500_INST_TYPE_FC:
            us_exec_fc(s, inst);
            s->pc %= US_NUM_INST;
            break;
        }
        if (inst->last) {
            for (p = 0; p < 4; p++) {
                if (s->active[p]) {
                    s->done[p] = true;
                }
            }
        }
        all_done = s->done[0] && s->done[1] && s->done[2] && s->done[3];
        if (cur == d->end || (all_done && inst->type == R500_INST_TYPE_OUT)) {
            break;
        }
        if (++steps > US_MAX_STEPS) {
            fprintf(stderr, "r500_us: program did not terminate after %ld steps\n", steps);
            break;
        }
    }
}

/* ------------------------------------------------------------------ */
/* GLSL emitter (Vulkan backend)                                        */
/* ------------------------------------------------------------------ */
/*
 * Straight-line translation of a program without flow control into a GLSL
 * function `void us_run()`.  One GPU invocation runs one pixel; the quad
 * semantics of the interpreter (derivatives, MDH/MDV) map onto helper
 * invocations and subgroup quad operations.  The surrounding fragment
 * shader (r5xx_vk.c) provides:
 *   vec4 us_input(int i), vec4 us_const(int i),
 *   vec4 us_tex(int unit, int op, bool unscaled, vec4 c, vec4 dx, vec4 dy),
 *   vec4 us_out[4]; float us_depth; bool us_depth_written; bool us_killed;
 *   bool us_cmp(float v, uint op), float us_om(float v), float us_flush(float),
 *   uint us_pm(uint pred, uint sel, bool inv)
 * Returns NULL (caller falls back to the CPU path) for programs using flow
 * control or relative addressing.
 */

typedef struct GlslOut {
    char *buf;
    size_t len, cap;
} GlslOut;

static void g_printf(GlslOut *o, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void g_printf(GlslOut *o, const char *fmt, ...)
{
    va_list ap;
    int n;

    for (;;) {
        va_start(ap, fmt);
        n = vsnprintf(o->buf ? o->buf + o->len : NULL,
                      o->buf ? o->cap - o->len : 0, fmt, ap);
        va_end(ap);
        if (n < 0) {
            return;
        }
        if (o->buf && o->len + (size_t)n < o->cap) {
            o->len += n;
            return;
        }
        o->cap = (o->cap ? o->cap * 2 : 4096) + n + 1;
        o->buf = realloc(o->buf, o->cap);
        if (!o->buf) {
            o->cap = o->len = 0;
            return;
        }
    }
}

static uint32_t f32_bits(float f)
{
    uint32_t u;
    memcpy(&u, &f, 4);
    return u;
}

static void glsl_float(char *b, size_t n, float f)
{
    if (isinf(f)) {
        snprintf(b, n, "%s(1.0/0.0)", f < 0 ? "-" : "");
    } else {
        snprintf(b, n, "uintBitsToFloat(0x%08xu)", f32_bits(f));
    }
}

/* rgb (k = 0) or alpha (k = 1) half of a source operand */
static void glsl_src(char *b, size_t n, const UsSrcAddr *a)
{
    switch (a->kind) {
    case US_SRC_TEMP:
        snprintf(b, n, "t%u", a->index & 127);
        break;
    case US_SRC_CONST:
        snprintf(b, n, "us_const(%u)", a->index);
        break;
    default: {
        char f[40];
        glsl_float(f, sizeof(f), us_inline_float(a->index));
        snprintf(b, n, "vec4(%s)", f);
        break;
    }
    }
}

static const char *glsl_swz(unsigned swz, const char *v, char *b, size_t n)
{
    static const char ch[4] = { 'x', 'y', 'z', 'w' };
    switch (swz) {
    case R500_SWZ_R: case R500_SWZ_G: case R500_SWZ_B: case R500_SWZ_A:
        snprintf(b, n, "%s.%c", v, ch[swz]);
        break;
    case R500_SWZ_HALF: snprintf(b, n, "0.5"); break;
    case R500_SWZ_ONE:  snprintf(b, n, "1.0"); break;
    default:            snprintf(b, n, "0.0"); break;
    }
    return b;
}

static const char *glsl_imod(unsigned mod, const char *v, char *b, size_t n)
{
    switch (mod) {
    case R500_IMOD_NEG: snprintf(b, n, "(-(%s))", v); break;
    case R500_IMOD_ABS: snprintf(b, n, "abs(%s)", v); break;
    case R500_IMOD_NAB: snprintf(b, n, "(-abs(%s))", v); break;
    default:            snprintf(b, n, "(%s)", v); break;
    }
    return b;
}

static const char *glsl_srcp(unsigned op, const char *s0, const char *s1,
                             char *b, size_t n)
{
    switch (op) {
    case R500_SRCP_OP_BIAS: snprintf(b, n, "(1.0 - 2.0 * %s)", s0); break;
    case R500_SRCP_OP_SUB:  snprintf(b, n, "(%s - %s)", s1, s0); break;
    case R500_SRCP_OP_ADD:  snprintf(b, n, "(%s + %s)", s1, s0); break;
    default:                snprintf(b, n, "(1.0 - %s)", s0); break;
    }
    return b;
}

/* output modifier + clamp expression on variable v */
static void glsl_omod(GlslOut *o, const char *v, unsigned omod, bool clamp)
{
    static const char *mul[7] = { "", " * 2.0", " * 4.0", " * 8.0",
                                  " * 0.5", " * 0.25", " * 0.125" };
    if (omod == R500_OMOD_DISABLE) {
        return;
    }
    g_printf(o, "    %s = us_flush(%s%s);\n", v, v, omod < 7 ? mul[omod] : "");
    if (clamp) {
        g_printf(o, "    %s = (%s >= 0.0) ? min(%s, 1.0) : 0.0;\n", v, v, v);
    }
}

static bool glsl_alu(GlslOut *o, const R500UsDecoded *d, const UsInst *in)
{
    const UsAluInst *a = &in->u.alu;
    static const char *chn[4] = { "x", "y", "z", "w" };
    const char *mul = d->zero_times_anything ? "us_mulz" : "us_mul";
    char sr[128], sa[128], e[160], e2[200], e3[200];
    int k, c;
    unsigned rgb_sel_pm = in->rgb_pred_sel, a_sel_pm = in->alpha_pred_sel;

    for (k = 0; k < 3; k++) {
        if (a->rgb_addr[k].rel || a->alpha_addr[k].rel) {
            return false;
        }
    }
    if (a->rgb_dest_rel || a->alpha_dest_rel) {
        return false;
    }
    g_printf(o, "  {\n    vec4 S0, S1, S2, SP, A, B, C; vec3 R; float AV;\n");
    for (k = 0; k < 3; k++) {
        glsl_src(sr, sizeof(sr), &a->rgb_addr[k]);
        glsl_src(sa, sizeof(sa), &a->alpha_addr[k]);
        g_printf(o, "    S%d = vec4((%s).xyz, (%s).w);\n", k, sr, sa);
    }
    for (c = 0; c < 3; c++) {
        char s0[16], s1[16];
        snprintf(s0, sizeof(s0), "S0.%s", chn[c]);
        snprintf(s1, sizeof(s1), "S1.%s", chn[c]);
        g_printf(o, "    SP.%s = %s;\n", chn[c],
                 glsl_srcp(a->rgb_srcp_op, s0, s1, e, sizeof(e)));
    }
    g_printf(o, "    SP.w = %s;\n", glsl_srcp(a->alpha_srcp_op, "S0.w", "S1.w", e, sizeof(e)));
    for (k = 0; k < 3; k++) {
        static const char *names[3] = { "A", "B", "C" };
        static const char *srcs[4] = { "S0", "S1", "S2", "SP" };
        for (c = 0; c < 3; c++) {
            glsl_swz(a->rgb_arg[k].swz[c], srcs[a->rgb_arg[k].sel], e, sizeof(e));
            g_printf(o, "    %s.%s = %s;\n", names[k], chn[c],
                     glsl_imod(a->rgb_arg[k].mod, e, e2, sizeof(e2)));
        }
        glsl_swz(a->alpha_arg[k].swz, srcs[a->alpha_arg[k].sel], e, sizeof(e));
        g_printf(o, "    %s.w = %s;\n", names[k],
                 glsl_imod(a->alpha_arg[k].mod, e, e2, sizeof(e2)));
    }
    if (a->rgb_op == R500_RGB_OP_MDH || a->rgb_op == R500_RGB_OP_MDV) {
        unsigned q = a->rgb_op == R500_RGB_OP_MDH ? 1 : 2;
        for (c = 0; c < 3; c++) {
            snprintf(e, sizeof(e), "subgroupQuadBroadcast(S0.%s, 0u)", chn[c]);
            g_printf(o, "    A.%s = %s;\n", chn[c], glsl_imod(a->rgb_arg[0].mod, e, e2, sizeof(e2)));
            snprintf(e, sizeof(e), "subgroupQuadBroadcast(S0.%s, %uu)", chn[c], q);
            g_printf(o, "    C.%s = %s;\n", chn[c], glsl_imod(a->rgb_arg[2].mod, e, e2, sizeof(e2)));
        }
    }
    if (a->alpha_op == R500_ALPHA_OP_MDH || a->alpha_op == R500_ALPHA_OP_MDV) {
        unsigned q = a->alpha_op == R500_ALPHA_OP_MDH ? 1 : 2;
        g_printf(o, "    A.w = %s;\n", glsl_imod(a->alpha_arg[0].mod,
                 "subgroupQuadBroadcast(S0.w, 0u)", e2, sizeof(e2)));
        snprintf(e, sizeof(e), "subgroupQuadBroadcast(S0.w, %uu)", q);
        g_printf(o, "    C.w = %s;\n", glsl_imod(a->alpha_arg[2].mod, e, e2, sizeof(e2)));
    }

    /* RGB unit */
    switch (a->rgb_op) {
    case R500_RGB_OP_MAD: case R500_RGB_OP_MDH: case R500_RGB_OP_MDV:
        for (c = 0; c < 3; c++) {
            g_printf(o, "    R.%s = %s(A.%s, B.%s) + C.%s;\n", chn[c], mul, chn[c], chn[c], chn[c]);
        }
        break;
    case R500_RGB_OP_DP3:
        g_printf(o, "    R = vec3(%s(A.x, B.x) + %s(A.y, B.y) + %s(A.z, B.z));\n", mul, mul, mul);
        break;
    case R500_RGB_OP_DP4:
        g_printf(o, "    R = vec3(%s(A.x, B.x) + %s(A.y, B.y) + %s(A.z, B.z) + %s(A.w, B.w));\n",
                 mul, mul, mul, mul);
        break;
    case R500_RGB_OP_D2A:
        g_printf(o, "    R = vec3(%s(A.x, B.x) + %s(A.y, B.y) + C.z);\n", mul, mul);
        break;
    case R500_RGB_OP_MIN:
        for (c = 0; c < 3; c++) {
            g_printf(o, "    R.%s = A.%s < B.%s ? A.%s : B.%s;\n", chn[c], chn[c], chn[c], chn[c], chn[c]);
        }
        break;
    case R500_RGB_OP_MAX:
        for (c = 0; c < 3; c++) {
            g_printf(o, "    R.%s = A.%s >= B.%s ? A.%s : B.%s;\n", chn[c], chn[c], chn[c], chn[c], chn[c]);
        }
        break;
    case R500_RGB_OP_CND:
        for (c = 0; c < 3; c++) {
            g_printf(o, "    R.%s = C.%s > 0.5 ? A.%s : B.%s;\n", chn[c], chn[c], chn[c], chn[c]);
        }
        break;
    case R500_RGB_OP_CMP:
        for (c = 0; c < 3; c++) {
            g_printf(o, "    R.%s = C.%s >= 0.0 ? A.%s : B.%s;\n", chn[c], chn[c], chn[c], chn[c]);
        }
        break;
    case R500_RGB_OP_FRC:
        g_printf(o, "    R = A.xyz - floor(A.xyz);\n");
        break;
    default:
        g_printf(o, "    R = vec3(0.0);\n");
        break;
    }
    /* alpha unit */
    switch (a->alpha_op) {
    case R500_ALPHA_OP_MAD: case R500_ALPHA_OP_MDH: case R500_ALPHA_OP_MDV:
        g_printf(o, "    AV = %s(A.w, B.w) + C.w;\n", mul);
        break;
    case R500_ALPHA_OP_DP:   g_printf(o, "    AV = R.x;\n"); break;
    case R500_ALPHA_OP_MIN:  g_printf(o, "    AV = A.w < B.w ? A.w : B.w;\n"); break;
    case R500_ALPHA_OP_MAX:  g_printf(o, "    AV = A.w >= B.w ? A.w : B.w;\n"); break;
    case R500_ALPHA_OP_CND:  g_printf(o, "    AV = C.w > 0.5 ? A.w : B.w;\n"); break;
    case R500_ALPHA_OP_CMP:  g_printf(o, "    AV = C.w >= 0.0 ? A.w : B.w;\n"); break;
    case R500_ALPHA_OP_FRC:  g_printf(o, "    AV = A.w - floor(A.w);\n"); break;
    case R500_ALPHA_OP_EX2:  g_printf(o, "    AV = exp2(A.w);\n"); break;
    case R500_ALPHA_OP_LN2:  g_printf(o, "    AV = log2(A.w);\n"); break;
    case R500_ALPHA_OP_RCP:  g_printf(o, "    AV = 1.0 / A.w;\n"); break;
    case R500_ALPHA_OP_RSQ:  g_printf(o, "    AV = A.w == 0.0 ? (1.0/0.0) : 1.0 / sqrt(A.w);\n"); break;
    case R500_ALPHA_OP_SIN:  g_printf(o, "    AV = sin(A.w * 6.283185307179586);\n"); break;
    case R500_ALPHA_OP_COS:  g_printf(o, "    AV = cos(A.w * 6.283185307179586);\n"); break;
    default:                 g_printf(o, "    AV = A.w;\n"); break;
    }
    if (a->rgb_op == R500_RGB_OP_SOP) {
        g_printf(o, "    R = vec3(AV);\n");
    }
    for (c = 0; c < 3; c++) {
        snprintf(e, sizeof(e), "R.%s", chn[c]);
        glsl_omod(o, e, a->rgb_omod, in->rgb_clamp);
    }
    glsl_omod(o, "AV", a->alpha_omod, in->alpha_clamp);
    if (a->alu_wmask) {
        g_printf(o, "    alu_result = us_cmp(%s, %uu);\n",
                 in->alu_result_sel ? "AV" : "R.x", in->alu_result_op);
    }
    g_printf(o, "    uint rpm = us_pm(pred, %uu, %s), apm = us_pm(pred, %uu, %s);\n",
             rgb_sel_pm, in->rgb_pred_inv ? "true" : "false",
             a_sel_pm, in->alpha_pred_inv ? "true" : "false");
    for (c = 0; c < 3; c++) {
        if ((in->rgb_wmask >> c) & 1) {
            g_printf(o, "    if ((rpm & %uu) != 0u) t%u.%s = R.%s;\n",
                     1u << c, a->rgb_dest & 127, chn[c], chn[c]);
        }
    }
    if (in->alpha_wmask) {
        g_printf(o, "    if ((apm & 8u) != 0u) t%u.w = AV;\n", a->alpha_dest & 127);
    }
    if (in->type == R500_INST_TYPE_OUT) {
        for (c = 0; c < 3; c++) {
            if ((in->rgb_omask >> c) & 1) {
                g_printf(o, "    if ((rpm & %uu) != 0u) us_out[%u].%s = R.%s;\n",
                         1u << c, a->rgb_target, chn[c], chn[c]);
            }
        }
        if (in->alpha_omask) {
            g_printf(o, "    if ((apm & 8u) != 0u) us_out[%u].w = AV;\n", a->alpha_target);
        }
        if (a->w_omask) {
            g_printf(o, "    if ((apm & 8u) != 0u) { us_depth = AV; us_depth_written = true; }\n");
        }
    } else {
        for (c = 0; c < 3; c++) {
            if ((in->rgb_omask >> c) & 1) {
                g_printf(o, "    pred = us_cmp(R.%s, %uu) ? (pred | %uu) : (pred & ~%uu);\n",
                         chn[c], a->rgb_target, 1u << c, 1u << c);
            }
        }
        if (in->alpha_omask) {
            g_printf(o, "    pred = us_cmp(AV, %uu) ? (pred | 8u) : (pred & ~8u);\n",
                     a->alpha_target);
        }
    }
    g_printf(o, "  }\n");
    (void)e3;
    return true;
}

static void glsl_texcoord(char *b, size_t n, unsigned addr, const uint8_t *swz)
{
    static const char ch[4] = { 'x', 'y', 'z', 'w' };
    snprintf(b, n, "t%u.%c%c%c%c", addr & 127, ch[swz[0] & 3], ch[swz[1] & 3],
             ch[swz[2] & 3], ch[swz[3] & 3]);
}

static bool glsl_tex(GlslOut *o, const UsInst *in)
{
    const UsTexInst *t = &in->u.tex;
    static const char ch[4] = { 'x', 'y', 'z', 'w' };
    char c[64], dx[64], dy[64];
    int k;

    if (t->src_rel || t->dst_rel || t->dx_rel || t->dy_rel) {
        return false;
    }
    if (t->op == R500_TEX_INST_NOP) {
        return true;
    }
    glsl_texcoord(c, sizeof(c), t->src, t->src_swz);
    g_printf(o, "  {\n    uint pm = (us_pm(pred, %uu, %s) & 7u) | (us_pm(pred, %uu, %s) & 8u);\n",
             in->rgb_pred_sel, in->rgb_pred_inv ? "true" : "false",
             in->alpha_pred_sel, in->alpha_pred_inv ? "true" : "false");
    g_printf(o, "    uint mask = %uu & pm;\n", in->rgb_wmask | (in->alpha_wmask << 3));
    if (t->op == R500_TEX_INST_TEXKILL) {
        g_printf(o, "    vec4 kv = t%u;\n", t->src & 127);
        g_printf(o, "    for (int c = 0; c < 4; c++) {\n"
                    "      if (((mask >> c) & 1u) != 0u && !isnan(kv[c]) &&\n"
                    "          (floatBitsToUint(kv[c]) & 0x80000000u) != 0u) { us_killed = true; break; }\n"
                    "    }\n  }\n");
        return true;
    }
    if (t->op == R500_TEX_INST_DXDY) {
        glsl_texcoord(dx, sizeof(dx), t->dx, t->dx_swz);
        glsl_texcoord(dy, sizeof(dy), t->dy, t->dy_swz);
    } else {
        snprintf(dx, sizeof(dx), "vec4(0.0)");
        snprintf(dy, sizeof(dy), "vec4(0.0)");
    }
    g_printf(o, "    vec4 r = us_tex(%u, %u, %s, %s, %s, %s);\n", t->unit, t->op,
             t->unscaled ? "true" : "false", c, dx, dy);
    if (t->ignore_uncovered) {
        g_printf(o, "    if (!gl_HelperInvocation && !us_killed) {\n");
    } else {
        g_printf(o, "    {\n");
    }
    for (k = 0; k < 4; k++) {
        g_printf(o, "      if ((mask & %uu) != 0u) t%u.%c = r.%c;\n", 1u << k,
                 t->dst & 127, ch[k], ch[t->dst_swz[k] & 3]);
    }
    g_printf(o, "    }\n  }\n");
    return true;
}

char *r500_us_to_glsl(const R500UsDecoded *d, const uint8_t tex_kind[16])
{
    GlslOut o = { 0 };
    int pc, steps = 0, i;

    (void)tex_kind;
    if (!d || d->uses_fc) {
        return NULL;
    }
    {
        /* declare only the temporaries the program touches */
        uint8_t used[128];
        memset(used, 0, sizeof(used));
        pc = d->start;
        for (steps = 0;; steps++) {
            const UsInst *in = &d->inst[pc];
            if (in->type == R500_INST_TYPE_ALU || in->type == R500_INST_TYPE_OUT) {
                for (i = 0; i < 3; i++) {
                    if (in->u.alu.rgb_addr[i].kind == US_SRC_TEMP) used[in->u.alu.rgb_addr[i].index & 127] = 1;
                    if (in->u.alu.alpha_addr[i].kind == US_SRC_TEMP) used[in->u.alu.alpha_addr[i].index & 127] = 1;
                }
                used[in->u.alu.rgb_dest & 127] = 1;
                used[in->u.alu.alpha_dest & 127] = 1;
            } else if (in->type == R500_INST_TYPE_TEX) {
                used[in->u.tex.src & 127] = used[in->u.tex.dst & 127] = 1;
                used[in->u.tex.dx & 127] = used[in->u.tex.dy & 127] = 1;
            }
            if (pc == d->end || (in->last && in->type == R500_INST_TYPE_OUT) ||
                steps > US_NUM_INST) {
                break;
            }
            pc = (pc + 1) % US_NUM_INST;
        }
        steps = 0;
        g_printf(&o, "void us_run()\n{\n  uint pred = 0u; bool alu_result = false;\n");
        for (i = 0; i < 128; i++) {
            if (!used[i]) {
                continue;
            }
            if (i < R500_US_MAX_INPUTS) {
                g_printf(&o, "  vec4 t%d = us_input(%d);\n", i, i);
            } else {
                g_printf(&o, "  vec4 t%d = vec4(0.0);\n", i);
            }
        }
    }
    pc = d->start;
    for (;;) {
        const UsInst *in = &d->inst[pc];
        bool ok = true;

        switch (in->type) {
        case R500_INST_TYPE_ALU:
        case R500_INST_TYPE_OUT:
            ok = glsl_alu(&o, d, in);
            break;
        case R500_INST_TYPE_TEX:
            ok = glsl_tex(&o, in);
            break;
        default:
            ok = false;
            break;
        }
        if (!ok || !o.buf) {
            free(o.buf);
            return NULL;
        }
        if (pc == d->end || (in->last && in->type == R500_INST_TYPE_OUT)) {
            break;
        }
        pc = (pc + 1) % US_NUM_INST;
        if (++steps > US_NUM_INST) {
            free(o.buf);
            return NULL;
        }
    }
    g_printf(&o, "  if (alu_result) {}\n}\n");
    return o.buf;
}

/* Version of the GLSL that r500_us_to_glsl() emits; part of the key of the
 * Vulkan backend's persistent SPIR-V cache.  Bump it whenever the emitted
 * code changes for the same program. */
const char *r500_us_glsl_build_id(void)
{
    return "r500-us-glsl-1";
}

uint64_t r500_us_hash(const R500UsDecoded *d)
{
    uint64_t h = 1469598103934665603ull;
    int pc = d->start, steps = 0, k;

    h = (h ^ (uint64_t)d->zero_times_anything) * 1099511628211ull;
    for (;;) {
        for (k = 0; k < 6; k++) {
            h = (h ^ d->inst[pc].raw[k]) * 1099511628211ull;
        }
        if (pc == d->end || ++steps > US_NUM_INST) {
            break;
        }
        pc = (pc + 1) % US_NUM_INST;
    }
    return h;
}
