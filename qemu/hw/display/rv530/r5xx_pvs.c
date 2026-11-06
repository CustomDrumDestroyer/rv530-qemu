/*
 * R300/R500 vertex program (PVS) decoder, CPU interpreter, disassembler
 * and GLSL emitter.
 *
 * Encoding reference: Mesa src/gallium/drivers/r300/compiler/r3xx_vertprog.c
 * (encoder), r3xx_vertprog_dump.c (listing format), r300_reg.h (bit
 * definitions) and radeon_vert_fc.c (how the R500 predicate stack is used).
 * The instruction/field #defines below are copied from Mesa's r300_reg.h
 * (Copyright 2005 Nicolai Haehnle et.al., MIT license) with attribution.
 *
 * Semantics that Mesa's encoder does not pin down (what each math-engine
 * opcode computes exactly, loop/jump behavior) are documented in
 * docs/pvs_NOTES.md; the choices made here follow the ARB_vertex_program /
 * DirectX definitions of the corresponding instructions and the R600 ISA
 * description of the predicate-stack ops, which R5xx introduced.
 *
 * Pure C, no QEMU dependencies.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "r5xx_pvs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <float.h>

/* ------------------------------------------------------------------ */
/* Register / instruction bit definitions (from Mesa r300_reg.h)       */
/* ------------------------------------------------------------------ */

/* R300_VAP_PVS_CODE_CNTL_0 (0x22D0) */
#define R300_PVS_FIRST_INST_SHIFT         0
#define R300_PVS_XYZW_VALID_INST_SHIFT    10
#define R300_PVS_LAST_INST_SHIFT          20
/* R300_VAP_PVS_CONST_CNTL (0x22D4) */
#define R300_PVS_CONST_BASE_OFFSET_SHIFT  0
#define R300_PVS_MAX_CONST_ADDR_SHIFT     16
/* R300_VAP_PVS_CODE_CNTL_1 (0x22D8) */
#define R300_PVS_LAST_VTX_SRC_INST_SHIFT  0
/* R300_VAP_PVS_FLOW_CNTL_OPC (0x22DC): 2 bits per slot */
#define PVS_FC_OPC_NOP   0
#define PVS_FC_OPC_JUMP  1
#define PVS_FC_OPC_LOOP  2
#define PVS_FC_OPC_JSR   3
/* R300_VAP_PVS_FLOW_CNTL_ADDRS_n (0x2230..): R300 packing */
#define R300_PVS_FC_ACT_ADRS_SHIFT          0
#define R300_PVS_FC_LOOP_CNT_JMP_INST_SHIFT 8
#define R300_PVS_FC_LAST_INST_SHIFT         16
#define R300_PVS_FC_RTN_INST_SHIFT          24
/* R300_VAP_PVS_FLOW_CNTL_LOOP_INDEX_n (0x2290..) */
#define R300_PVS_FC_LOOP_INIT_VAL_SHIFT     0
#define R300_PVS_FC_LOOP_STEP_VAL_SHIFT     8
/* R500_VAP_PVS_FLOW_CNTL_ADDRS_LW_n (0x2500..) / UW_n (0x2504..) */
#define R500_PVS_FC_ACT_ADRS_SHIFT          0
#define R500_PVS_FC_LOOP_CNT_JMP_INST_SHIFT 16
#define R500_PVS_FC_LAST_INST_SHIFT         0
#define R500_PVS_FC_RTN_INST_SHIFT          16

/* Vector engine opcodes */
enum {
    VECTOR_NO_OP                = 0,
    VE_DOT_PRODUCT              = 1,
    VE_MULTIPLY                 = 2,
    VE_ADD                      = 3,
    VE_MULTIPLY_ADD             = 4,
    VE_DISTANCE_VECTOR          = 5,
    VE_FRACTION                 = 6,
    VE_MAXIMUM                  = 7,
    VE_MINIMUM                  = 8,
    VE_SET_GREATER_THAN_EQUAL   = 9,
    VE_SET_LESS_THAN            = 10,
    VE_MULTIPLYX2_ADD           = 11,
    VE_MULTIPLY_CLAMP           = 12,
    VE_FLT2FIX_DX               = 13,
    VE_FLT2FIX_DX_RND           = 14,
    /* R5XX */
    VE_PRED_SET_EQ_PUSH         = 15,
    VE_PRED_SET_GT_PUSH         = 16,
    VE_PRED_SET_GTE_PUSH        = 17,
    VE_PRED_SET_NEQ_PUSH        = 18,
    VE_COND_WRITE_EQ            = 19,
    VE_COND_WRITE_GT            = 20,
    VE_COND_WRITE_GTE           = 21,
    VE_COND_WRITE_NEQ           = 22,
    VE_COND_MUX_EQ              = 23,
    VE_COND_MUX_GT              = 24,
    VE_COND_MUX_GTE             = 25,
    VE_SET_GREATER_THAN         = 26,
    VE_SET_EQUAL                = 27,
    VE_SET_NOT_EQUAL            = 28,
    VE_LAST_VALID               = 28
};

/* Math engine opcodes */
enum {
    MATH_NO_OP                  = 0,
    ME_EXP_BASE2_DX             = 1,
    ME_LOG_BASE2_DX             = 2,
    ME_EXP_BASEE_FF             = 3,
    ME_LIGHT_COEFF_DX           = 4,
    ME_POWER_FUNC_FF            = 5,
    ME_RECIP_DX                 = 6,
    ME_RECIP_FF                 = 7,
    ME_RECIP_SQRT_DX            = 8,
    ME_RECIP_SQRT_FF            = 9,
    ME_MULTIPLY                 = 10,
    ME_EXP_BASE2_FULL_DX        = 11,
    ME_LOG_BASE2_FULL_DX        = 12,
    ME_POWER_FUNC_FF_CLAMP_B    = 13,
    ME_POWER_FUNC_FF_CLAMP_B1   = 14,
    ME_POWER_FUNC_FF_CLAMP_01   = 15,
    ME_SIN                      = 16,
    ME_COS                      = 17,
    /* R5XX */
    ME_LOG_BASE2_IEEE           = 18,
    ME_RECIP_IEEE               = 19,
    ME_RECIP_SQRT_IEEE          = 20,
    ME_PRED_SET_EQ              = 21,
    ME_PRED_SET_GT              = 22,
    ME_PRED_SET_GTE             = 23,
    ME_PRED_SET_NEQ             = 24,
    ME_PRED_SET_CLR             = 25,
    ME_PRED_SET_INV             = 26,
    ME_PRED_SET_POP             = 27,
    ME_PRED_SET_RESTORE         = 28,
    ME_LAST_VALID               = 28
};

/* Macro (2-clock) ops; opcode bit 0 selects */
enum {
    PVS_MACRO_OP_2CLK_MADD      = 0,
    PVS_MACRO_OP_2CLK_M2X_ADD   = 1
};

enum {
    PVS_SRC_REG_TEMPORARY       = 0,
    PVS_SRC_REG_INPUT           = 1,
    PVS_SRC_REG_CONSTANT        = 2,
    PVS_SRC_REG_ALT_TEMPORARY   = 3
};

enum {
    PVS_DST_REG_TEMPORARY       = 0,
    PVS_DST_REG_A0              = 1,
    PVS_DST_REG_OUT             = 2,
    PVS_DST_REG_OUT_REPL_X      = 3,
    PVS_DST_REG_ALT_TEMPORARY   = 4,
    PVS_DST_REG_INPUT           = 5
};

enum {
    PVS_SRC_SELECT_X            = 0,
    PVS_SRC_SELECT_Y            = 1,
    PVS_SRC_SELECT_Z            = 2,
    PVS_SRC_SELECT_W            = 3,
    PVS_SRC_SELECT_FORCE_0      = 4,
    PVS_SRC_SELECT_FORCE_1      = 5
};

/* Opcode / destination dword */
#define PVS_DST_OPCODE_MASK          0x3f
#define PVS_DST_OPCODE_SHIFT         0
#define PVS_DST_MATH_INST_SHIFT      6
#define PVS_DST_MACRO_INST_SHIFT     7
#define PVS_DST_REG_TYPE_MASK        0xf
#define PVS_DST_REG_TYPE_SHIFT       8
#define PVS_DST_ADDR_MODE_1_SHIFT    12
#define PVS_DST_OFFSET_MASK          0x7f
#define PVS_DST_OFFSET_SHIFT         13
#define PVS_DST_WE_X_SHIFT           20
#define PVS_DST_VE_SAT_SHIFT         24
#define PVS_DST_ME_SAT_SHIFT         25
#define PVS_DST_PRED_ENABLE_SHIFT    26
#define PVS_DST_PRED_SENSE_SHIFT     27
#define PVS_DST_ADDR_SEL_MASK        0x3
#define PVS_DST_ADDR_SEL_SHIFT       29
#define PVS_DST_ADDR_MODE_0_SHIFT    31

/* Source dword */
#define PVS_SRC_REG_TYPE_MASK        0x3
#define PVS_SRC_REG_TYPE_SHIFT       0
#define PVS_SRC_ABS_XYZW_SHIFT       3
#define PVS_SRC_ADDR_MODE_0_SHIFT    4
#define PVS_SRC_OFFSET_MASK          0xff
#define PVS_SRC_OFFSET_SHIFT         5
#define PVS_SRC_SWIZZLE_X_SHIFT      13
#define PVS_SRC_SWIZZLE_Y_SHIFT      16
#define PVS_SRC_SWIZZLE_Z_SHIFT      19
#define PVS_SRC_SWIZZLE_W_SHIFT      22
#define PVS_SRC_MODIFIER_X_SHIFT     25
#define PVS_SRC_ADDR_SEL_MASK        0x3
#define PVS_SRC_ADDR_SEL_SHIFT       29

#define R300_MAX_PVS_CODE_LINES      256
#define R500_MAX_PVS_CODE_LINES      1024

/* ------------------------------------------------------------------ */
/* Decoded representation                                              */
/* ------------------------------------------------------------------ */

typedef struct PvsSrc {
    uint8_t cls;        /* PVS_SRC_REG_* */
    uint8_t idx;        /* 8-bit register offset */
    uint8_t swz[4];     /* PVS_SRC_SELECT_*; 6/7 are "unused" */
    uint8_t neg;        /* per-component negate, bit 0 = x */
    uint8_t abs;        /* absolute value (whole vector) */
    uint8_t rel;        /* relative addressing through address register */
    uint8_t addr_sel;   /* address register component */
} PvsSrc;

typedef struct PvsInst {
    uint32_t raw[4];
    uint8_t opcode;     /* 6-bit opcode field */
    uint8_t math;       /* math engine instruction */
    uint8_t macro;      /* 2-clock macro instruction */
    uint8_t dst_cls;    /* PVS_DST_REG_* */
    uint8_t dst_idx;    /* 7-bit register offset */
    uint8_t wmask;      /* bit 0 = x .. bit 3 = w */
    uint8_t sat;        /* saturate [0,1] */
    uint8_t pred_en;
    uint8_t pred_sense;
    uint8_t dst_rel;
    uint8_t dst_addr_sel;
    PvsSrc src[3];
    /*
     * R500 dual math: PVS_DST_DUAL_MATH_OP (bits 27-28, only without
     * predication, where bit 27 is PRED_SENSE) pairs the vector op with a
     * math-engine op described by the third source word: register type,
     * offset and X swizzle select the scalar source, bits 21-25 hold the ME
     * opcode, bits 27-28 the destination component and bits 19-20 the
     * alt-temp destination.  Worked out from Apple's fixed-function lighting
     * programs: Chess (DP3 t0.w = N.N with RSQ a0.x = 1/|L|, read back as
     * a0.x/a0.w) and Grapher's GL_NORMALIZE path, whose second RSQ word
     * 0x19286000 (bit 19 set) feeds a1.w and is read back as a1.w.
     * Bits 16-18 have always been 0.  Only RSQ (opcode 9, which also reads
     * as "Z/W swizzle = 0") has been seen; other opcodes are an inference.
     */
    uint8_t dual;
    uint8_t dual_op;
    uint8_t dual_comp;
    uint8_t dual_dst_idx;
    uint8_t dual_sat;
    PvsSrc dual_src;
} PvsInst;

typedef struct PvsFc {
    uint8_t op;         /* PVS_FC_OPC_* */
    uint16_t act;       /* activation address */
    uint16_t cnt_jmp;   /* loop count or jump target */
    uint16_t last;      /* last instruction (loop body / subroutine) */
    uint16_t rtn;       /* return / loop-start address */
    uint8_t init;       /* loop index initial value */
    uint8_t step;       /* loop index step */
} PvsFc;

struct R5xxPvsDecoded {
    bool is_r500;
    int first;              /* absolute index of first instruction */
    int last;               /* absolute index of last instruction */
    int n;                  /* last - first + 1 */
    PvsInst *inst;          /* n entries; inst[i] == code line first+i */
    uint32_t inputs_used;
    uint32_t outputs_written;
    int num_temps;          /* highest temp index referenced + 1 */
    int num_alt_temps;
    uint32_t const_base;    /* R300_PVS_CONST_BASE_OFFSET */
    uint32_t const_max;     /* R300_PVS_MAX_CONST_ADDR */
    PvsFc fc[R5XX_PVS_MAX_FC];
    int num_fc;             /* highest used FC slot + 1 */
    bool uses_fc;
    bool uses_pred;
    bool uses_rel;
    bool uses_rel_out;      /* relative addressing of an output destination */
    bool uses_dual;         /* R500 dual math instructions */
    uint32_t code_cntl0, code_cntl1, const_cntl, vap_cntl;
};

/* ------------------------------------------------------------------ */
/* Decoder                                                             */
/* ------------------------------------------------------------------ */

static void decode_src(PvsSrc *s, uint32_t v)
{
    s->cls = (v >> PVS_SRC_REG_TYPE_SHIFT) & PVS_SRC_REG_TYPE_MASK;
    s->idx = (v >> PVS_SRC_OFFSET_SHIFT) & PVS_SRC_OFFSET_MASK;
    s->swz[0] = (v >> PVS_SRC_SWIZZLE_X_SHIFT) & 7;
    s->swz[1] = (v >> PVS_SRC_SWIZZLE_Y_SHIFT) & 7;
    s->swz[2] = (v >> PVS_SRC_SWIZZLE_Z_SHIFT) & 7;
    s->swz[3] = (v >> PVS_SRC_SWIZZLE_W_SHIFT) & 7;
    s->neg = (v >> PVS_SRC_MODIFIER_X_SHIFT) & 0xf;
    s->abs = (v >> PVS_SRC_ABS_XYZW_SHIFT) & 1;
    s->rel = (v >> PVS_SRC_ADDR_MODE_0_SHIFT) & 1;
    s->addr_sel = (v >> PVS_SRC_ADDR_SEL_SHIFT) & PVS_SRC_ADDR_SEL_MASK;
}

static void decode_inst(PvsInst *in, const uint32_t *w)
{
    uint32_t op = w[0];
    memcpy(in->raw, w, sizeof(in->raw));
    in->opcode = (op >> PVS_DST_OPCODE_SHIFT) & PVS_DST_OPCODE_MASK;
    in->math = (op >> PVS_DST_MATH_INST_SHIFT) & 1;
    in->macro = (op >> PVS_DST_MACRO_INST_SHIFT) & 1;
    in->dst_cls = (op >> PVS_DST_REG_TYPE_SHIFT) & PVS_DST_REG_TYPE_MASK;
    in->dst_idx = (op >> PVS_DST_OFFSET_SHIFT) & PVS_DST_OFFSET_MASK;
    in->wmask = (op >> PVS_DST_WE_X_SHIFT) & 0xf;
    in->sat = in->math ? (op >> PVS_DST_ME_SAT_SHIFT) & 1
                       : (op >> PVS_DST_VE_SAT_SHIFT) & 1;
    in->pred_en = (op >> PVS_DST_PRED_ENABLE_SHIFT) & 1;
    in->pred_sense = in->pred_en ? (op >> PVS_DST_PRED_SENSE_SHIFT) & 1 : 0;
    in->dual = in->pred_en || in->math || in->macro ? 0 : (op >> 27) & 3;
    in->dst_rel = (op >> PVS_DST_ADDR_MODE_0_SHIFT) & 1;
    in->dst_addr_sel = (op >> PVS_DST_ADDR_SEL_SHIFT) & PVS_DST_ADDR_SEL_MASK;
    decode_src(&in->src[0], w[1]);
    decode_src(&in->src[1], w[2]);
    decode_src(&in->src[2], w[3]);
    if (in->dual) {
        decode_src(&in->dual_src, w[3]);
        in->dual_src.neg &= 1;          /* modifier bits 27-28 are the component */
        in->dual_op = (w[3] >> 21) & 0x1f;
        in->dual_comp = (w[3] >> 27) & 3;
        in->dual_dst_idx = (w[3] >> 19) & 3;
        in->dual_sat = (op >> PVS_DST_ME_SAT_SHIFT) & 1;
    }
}

static void set_err(char *err, size_t errlen, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

static void set_err(char *err, size_t errlen, const char *fmt, ...)
{
    va_list ap;
    if (!err || !errlen) {
        return;
    }
    va_start(ap, fmt);
    vsnprintf(err, errlen, fmt, ap);
    va_end(ap);
}

static bool inst_is_pred_op(const PvsInst *in)
{
    if (in->macro) {
        return false;
    }
    if (in->math) {
        return in->opcode >= ME_PRED_SET_EQ && in->opcode <= ME_PRED_SET_RESTORE;
    }
    return in->opcode >= VE_PRED_SET_EQ_PUSH && in->opcode <= VE_PRED_SET_NEQ_PUSH;
}

R5xxPvsDecoded *r5xx_pvs_decode(const R5xxPvsProgram *p, char *err, size_t errlen)
{
    R5xxPvsDecoded *d;
    int first, last, max_lines, i;

    if (!p) {
        set_err(err, errlen, "null program");
        return NULL;
    }
    max_lines = p->is_r500 ? R500_MAX_PVS_CODE_LINES : R300_MAX_PVS_CODE_LINES;
    first = (p->code_cntl0 >> R300_PVS_FIRST_INST_SHIFT) & 0x3ff;
    last = (p->code_cntl0 >> R300_PVS_LAST_INST_SHIFT) & 0x3ff;
    if (last < first) {
        set_err(err, errlen, "PVS_CODE_CNTL_0: last instruction %d < first %d",
                last, first);
        return NULL;
    }
    if (last >= max_lines) {
        set_err(err, errlen, "PVS_CODE_CNTL_0: last instruction %d beyond "
                "code store (%d lines)", last, max_lines);
        return NULL;
    }

    d = calloc(1, sizeof(*d));
    if (!d) {
        set_err(err, errlen, "out of memory");
        return NULL;
    }
    d->is_r500 = p->is_r500;
    d->first = first;
    d->last = last;
    d->n = last - first + 1;
    d->code_cntl0 = p->code_cntl0;
    d->code_cntl1 = p->code_cntl1;
    d->const_cntl = p->const_cntl;
    d->vap_cntl = p->vap_cntl;
    d->const_base = (p->const_cntl >> R300_PVS_CONST_BASE_OFFSET_SHIFT) & 0xff;
    d->const_max = (p->const_cntl >> R300_PVS_MAX_CONST_ADDR_SHIFT) & 0xff;
    d->inst = calloc(d->n, sizeof(PvsInst));
    if (!d->inst) {
        free(d);
        set_err(err, errlen, "out of memory");
        return NULL;
    }

    for (i = 0; i < d->n; i++) {
        PvsInst *in = &d->inst[i];
        int s;
        decode_inst(in, &p->code[(first + i) * 4]);

        if (!in->macro) {
            int lim = in->math ? ME_LAST_VALID : VE_LAST_VALID;
            if (in->opcode > lim) {
                set_err(err, errlen, "instruction %d: reserved %s opcode %u",
                        first + i, in->math ? "ME" : "VE", in->opcode);
                r5xx_pvs_free(d);
                return NULL;
            }
        }
        if (in->dst_cls > PVS_DST_REG_INPUT) {
            set_err(err, errlen, "instruction %d: bad destination class %u",
                    first + i, in->dst_cls);
            r5xx_pvs_free(d);
            return NULL;
        }

        if (in->pred_en || inst_is_pred_op(in)) {
            d->uses_pred = true;
        }
        if (in->dst_rel) {
            d->uses_rel = true;
            if (in->dst_cls == PVS_DST_REG_OUT ||
                in->dst_cls == PVS_DST_REG_OUT_REPL_X) {
                d->uses_rel_out = true;
            }
        }
        switch (in->dst_cls) {
        case PVS_DST_REG_OUT:
        case PVS_DST_REG_OUT_REPL_X:
            if (in->dst_idx < R5XX_PVS_MAX_OUTPUTS && in->wmask) {
                d->outputs_written |= 1u << in->dst_idx;
            }
            break;
        case PVS_DST_REG_TEMPORARY:
            if (in->dst_idx + 1 > d->num_temps) {
                d->num_temps = in->dst_idx + 1;
            }
            break;
        case PVS_DST_REG_ALT_TEMPORARY:
            if (in->dst_idx + 1 > d->num_alt_temps) {
                d->num_alt_temps = in->dst_idx + 1;
            }
            break;
        default:
            break;
        }
        if (in->dual) {
            d->uses_dual = true;
            if (in->dual_dst_idx + 1 > d->num_alt_temps) {
                d->num_alt_temps = in->dual_dst_idx + 1;
            }
        }
        for (s = 0; s < 3; s++) {
            const PvsSrc *src = &in->src[s];
            if (src->rel) {
                d->uses_rel = true;
            }
            switch (src->cls) {
            case PVS_SRC_REG_INPUT:
                if (src->idx < 32) {
                    d->inputs_used |= 1u << src->idx;
                }
                break;
            case PVS_SRC_REG_TEMPORARY:
                if (src->idx + 1 > d->num_temps) {
                    d->num_temps = src->idx + 1;
                }
                break;
            case PVS_SRC_REG_ALT_TEMPORARY:
                if (src->idx + 1 > d->num_alt_temps) {
                    d->num_alt_temps = src->idx + 1;
                }
                break;
            default:
                break;
            }
        }
    }

    /* Flow control slots */
    for (i = 0; i < R5XX_PVS_MAX_FC; i++) {
        PvsFc *fc = &d->fc[i];
        fc->op = (p->fc_opc >> (2 * i)) & 3;
        if (p->is_r500) {
            uint32_t lw = p->fc_addrs_lw[i], uw = p->fc_addrs_uw[i];
            fc->act = (lw >> R500_PVS_FC_ACT_ADRS_SHIFT) & 0xffff;
            fc->cnt_jmp = (lw >> R500_PVS_FC_LOOP_CNT_JMP_INST_SHIFT) & 0xffff;
            fc->last = (uw >> R500_PVS_FC_LAST_INST_SHIFT) & 0xffff;
            fc->rtn = (uw >> R500_PVS_FC_RTN_INST_SHIFT) & 0xffff;
        } else {
            uint32_t a = p->fc_addrs[i];
            fc->act = (a >> R300_PVS_FC_ACT_ADRS_SHIFT) & 0xff;
            fc->cnt_jmp = (a >> R300_PVS_FC_LOOP_CNT_JMP_INST_SHIFT) & 0xff;
            fc->last = (a >> R300_PVS_FC_LAST_INST_SHIFT) & 0xff;
            fc->rtn = (a >> R300_PVS_FC_RTN_INST_SHIFT) & 0xff;
        }
        fc->init = (p->fc_loop_index[i] >> R300_PVS_FC_LOOP_INIT_VAL_SHIFT) & 0xff;
        fc->step = (p->fc_loop_index[i] >> R300_PVS_FC_LOOP_STEP_VAL_SHIFT) & 0xff;
        if (fc->op != PVS_FC_OPC_NOP) {
            d->num_fc = i + 1;
            d->uses_fc = true;
        }
    }
    return d;
}

void r5xx_pvs_free(R5xxPvsDecoded *d)
{
    if (d) {
        free(d->inst);
        free(d);
    }
}

int r5xx_pvs_num_instructions(const R5xxPvsDecoded *d)
{
    return d ? d->n : 0;
}

uint32_t r5xx_pvs_inputs_used(const R5xxPvsDecoded *d)
{
    return d ? d->inputs_used : 0;
}

uint32_t r5xx_pvs_outputs_written(const R5xxPvsDecoded *d)
{
    return d ? d->outputs_written : 0;
}

bool r5xx_pvs_uses_flow_control(const R5xxPvsDecoded *d)
{
    return d ? d->uses_fc : false;
}

/* ------------------------------------------------------------------ */
/* Interpreter                                                         */
/* ------------------------------------------------------------------ */

#define PVS_EXEC_MAX_STEPS   (1 << 20)   /* runaway guard */

typedef struct PvsExec {
    const R5xxPvsDecoded *d;
    const r5xx_vec4 *consts;
    r5xx_vec4 in[R5XX_PVS_MAX_INPUTS];
    r5xx_vec4 *out;
    r5xx_vec4 temp[R5XX_PVS_MAX_TEMPS];
    r5xx_vec4 alt[R5XX_PVS_MAX_TEMPS];
    int32_t a0[4];
    bool pred;
    int32_t loop_cnt[R5XX_PVS_MAX_FC];
    int32_t loop_idx[R5XX_PVS_MAX_FC];
    int32_t call_ret;
} PvsExec;

static inline int32_t clamp_addr(float f)
{
    if (!(f == f)) {              /* NaN */
        return 0;
    }
    if (f > 1048576.0f) {
        return 1048576;
    }
    if (f < -1048576.0f) {
        return -1048576;
    }
    return (int32_t)floorf(f);
}

static r5xx_vec4 exec_fetch_reg(const PvsExec *ex, const PvsSrc *s)
{
    const R5xxPvsDecoded *d = ex->d;
    int32_t idx = s->idx;
    r5xx_vec4 zero = { { 0, 0, 0, 0 } };

    if (s->rel) {
        idx += ex->a0[s->addr_sel];
    }
    switch (s->cls) {
    case PVS_SRC_REG_TEMPORARY:
        if (idx < 0 || idx >= R5XX_PVS_MAX_TEMPS) {
            return zero;
        }
        return ex->temp[idx];
    case PVS_SRC_REG_ALT_TEMPORARY:
        if (idx < 0 || idx >= R5XX_PVS_MAX_TEMPS) {
            return zero;
        }
        return ex->alt[idx];
    case PVS_SRC_REG_INPUT:
        if (idx < 0 || idx >= R5XX_PVS_MAX_INPUTS) {
            return zero;
        }
        return ex->in[idx];
    case PVS_SRC_REG_CONSTANT:
        /* Relative accesses outside [0, MAX_CONST_ADDR] read as zero
         * (DirectX semantics); absolute accesses are only bounded by the
         * constant store. */
        if (s->rel && (idx < 0 || (uint32_t)idx > d->const_max)) {
            return zero;
        }
        idx += (int32_t)d->const_base;
        if (idx < 0 || idx >= R5XX_PVS_MAX_CONST || !ex->consts) {
            return zero;
        }
        return ex->consts[idx];
    default:
        return zero;
    }
}

static r5xx_vec4 exec_fetch_src(const PvsExec *ex, const PvsSrc *s)
{
    r5xx_vec4 reg = exec_fetch_reg(ex, s);
    r5xx_vec4 r;
    int c;
    for (c = 0; c < 4; c++) {
        float v;
        switch (s->swz[c]) {
        case PVS_SRC_SELECT_X: case PVS_SRC_SELECT_Y:
        case PVS_SRC_SELECT_Z: case PVS_SRC_SELECT_W:
            v = reg.f[s->swz[c]];
            break;
        case PVS_SRC_SELECT_FORCE_1:
            v = 1.0f;
            break;
        default:
            v = 0.0f;
            break;
        }
        if (s->abs) {
            v = fabsf(v);
        }
        if (s->neg & (1 << c)) {
            v = -v;
        }
        r.f[c] = v;
    }
    return r;
}

/* ---- scalar helpers shared with the GLSL semantics ---- */

static inline float f_floor(float x) { return floorf(x); }
static inline float f_rsq_abs(float x) { return 1.0f / sqrtf(fabsf(x)); }

static float f_pow(float a, float b)
{
    /* The hardware evaluates pow through log2/exp2 of |a|; powf(0,0)
     * yields 1 which is what DX/ARB expect. */
    return powf(fabsf(a), b);
}

static r5xx_vec4 me_exp_dx(float x)
{
    float fl = f_floor(x);
    return r5xx_vec4_make(exp2f(fl), x - fl, exp2f(x), 1.0f);
}

static r5xx_vec4 me_log_dx(float x)
{
    float a = fabsf(x), l, fl;
    if (a == 0.0f) {
        return r5xx_vec4_make(-INFINITY, 1.0f, -INFINITY, 1.0f);
    }
    l = log2f(a);
    fl = f_floor(l);
    return r5xx_vec4_make(fl, a / exp2f(fl), l, 1.0f);
}

static r5xx_vec4 me_lit(float nl, float nh, float p)
{
    float z = 0.0f;
    if (p < -128.0f) {
        p = -128.0f;
    } else if (p > 128.0f) {
        p = 128.0f;
    }
    if (nl > 0.0f) {
        if (nh < 0.0f) {
            nh = 0.0f;
        }
        z = powf(nh, p);
    }
    return r5xx_vec4_make(1.0f, nl > 0.0f ? nl : 0.0f, z, 1.0f);
}

static inline float sat01(float v)
{
    return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

static inline bool cond_cc(int cc, float v)
{
    switch (cc) {
    case 0:  return v == 0.0f;      /* EQ  */
    case 1:  return v > 0.0f;       /* GT  */
    case 2:  return v >= 0.0f;      /* GTE */
    default: return v != 0.0f;      /* NEQ */
    }
}

/* Write a result vector to the instruction's destination honoring the
 * write mask (bits of 'mask', already ANDed with the instruction mask). */
static void exec_write(PvsExec *ex, const PvsInst *in, const r5xx_vec4 *r,
                       unsigned mask)
{
    int32_t idx = in->dst_idx;
    r5xx_vec4 *dst = NULL;
    int c;

    if (!mask) {
        return;
    }
    if (in->dst_rel) {
        idx += ex->a0[in->dst_addr_sel];
    }
    switch (in->dst_cls) {
    case PVS_DST_REG_TEMPORARY:
        if (idx >= 0 && idx < R5XX_PVS_MAX_TEMPS) {
            dst = &ex->temp[idx];
        }
        break;
    case PVS_DST_REG_ALT_TEMPORARY:
        if (idx >= 0 && idx < R5XX_PVS_MAX_TEMPS) {
            dst = &ex->alt[idx];
        }
        break;
    case PVS_DST_REG_OUT:
        if (idx >= 0 && idx < R5XX_PVS_MAX_OUTPUTS) {
            dst = &ex->out[idx];
        }
        break;
    case PVS_DST_REG_OUT_REPL_X:
        if (idx >= 0 && idx < R5XX_PVS_MAX_OUTPUTS) {
            r5xx_vec4 *o = &ex->out[idx];
            for (c = 0; c < 4; c++) {
                if (mask & (1 << c)) {
                    o->f[c] = r->f[0];
                }
            }
        }
        return;
    case PVS_DST_REG_INPUT:
        if (idx >= 0 && idx < R5XX_PVS_MAX_INPUTS) {
            dst = &ex->in[idx];
        }
        break;
    case PVS_DST_REG_A0:
        if (idx == 0) {
            for (c = 0; c < 4; c++) {
                if (mask & (1 << c)) {
                    ex->a0[c] = clamp_addr(r->f[c]);
                }
            }
        }
        return;
    default:
        return;
    }
    if (!dst) {
        return;
    }
    for (c = 0; c < 4; c++) {
        if (mask & (1 << c)) {
            dst->f[c] = r->f[c];
        }
    }
}

static void exec_dual(PvsExec *ex, const PvsInst *in, float x)
{
    float v;

    switch (in->dual_op) {
    case ME_RECIP_SQRT_DX:
    case ME_RECIP_SQRT_FF:
        v = f_rsq_abs(x);
        break;
    case ME_RECIP_SQRT_IEEE:
        v = 1.0f / sqrtf(x);
        break;
    case ME_RECIP_DX:
    case ME_RECIP_FF:
    case ME_RECIP_IEEE:
        v = 1.0f / x;
        break;
    case ME_EXP_BASE2_FULL_DX:
        v = exp2f(x);
        break;
    case ME_LOG_BASE2_FULL_DX:
        v = fabsf(x) == 0.0f ? -INFINITY : log2f(fabsf(x));
        break;
    case ME_LOG_BASE2_IEEE:
        v = log2f(x);
        break;
    case ME_EXP_BASEE_FF:
        v = expf(x);
        break;
    case ME_SIN:
        v = sinf(x);
        break;
    case ME_COS:
        v = cosf(x);
        break;
    default:
        v = 0.0f;                       /* not seen yet; see the decoder note */
        break;
    }
    if (in->dual_sat) {
        v = sat01(v);
    }
    ex->alt[in->dual_dst_idx].f[in->dual_comp] = v;   /* idx < 32 */
}

static void exec_inst(PvsExec *ex, const PvsInst *in)
{
    r5xx_vec4 s0, s1, s2, r = { { 0, 0, 0, 0 } };
    unsigned mask = in->wmask;
    bool set_pred = false, new_pred = false;
    int c;

    if (in->pred_en && ex->pred != (bool)in->pred_sense) {
        return;
    }

    s0 = exec_fetch_src(ex, &in->src[0]);
    s1 = exec_fetch_src(ex, &in->src[1]);
    s2 = exec_fetch_src(ex, &in->src[2]);
    if (in->dual) {
        /* both halves have read their operands: the scalar half can write */
        r5xx_vec4 ds = exec_fetch_src(ex, &in->dual_src);
        exec_dual(ex, in, ds.f[0]);
    }

    if (in->macro) {
        float k = (in->opcode & 1) ? 2.0f : 1.0f;
        for (c = 0; c < 4; c++) {
            r.f[c] = k * s0.f[c] * s1.f[c] + s2.f[c];
        }
    } else if (!in->math) {
        switch (in->opcode) {
        case VECTOR_NO_OP:
            return;
        case VE_DOT_PRODUCT: {
            float dp = s0.f[0] * s1.f[0] + s0.f[1] * s1.f[1] +
                       s0.f[2] * s1.f[2] + s0.f[3] * s1.f[3];
            r = r5xx_vec4_make(dp, dp, dp, dp);
            break;
        }
        case VE_MULTIPLY:
            for (c = 0; c < 4; c++) r.f[c] = s0.f[c] * s1.f[c];
            break;
        case VE_ADD:
            for (c = 0; c < 4; c++) r.f[c] = s0.f[c] + s1.f[c];
            break;
        case VE_MULTIPLY_ADD:
            for (c = 0; c < 4; c++) r.f[c] = s0.f[c] * s1.f[c] + s2.f[c];
            break;
        case VE_DISTANCE_VECTOR:
            r = r5xx_vec4_make(1.0f, s0.f[1] * s1.f[1], s0.f[2], s1.f[3]);
            break;
        case VE_FRACTION:
            for (c = 0; c < 4; c++) r.f[c] = s0.f[c] - f_floor(s0.f[c]);
            break;
        case VE_MAXIMUM:
            for (c = 0; c < 4; c++) r.f[c] = s0.f[c] > s1.f[c] ? s0.f[c] : s1.f[c];
            break;
        case VE_MINIMUM:
            for (c = 0; c < 4; c++) r.f[c] = s0.f[c] < s1.f[c] ? s0.f[c] : s1.f[c];
            break;
        case VE_SET_GREATER_THAN_EQUAL:
            for (c = 0; c < 4; c++) r.f[c] = s0.f[c] >= s1.f[c] ? 1.0f : 0.0f;
            break;
        case VE_SET_LESS_THAN:
            for (c = 0; c < 4; c++) r.f[c] = s0.f[c] < s1.f[c] ? 1.0f : 0.0f;
            break;
        case VE_MULTIPLYX2_ADD:
            for (c = 0; c < 4; c++) r.f[c] = 2.0f * s0.f[c] * s1.f[c] + s2.f[c];
            break;
        case VE_MULTIPLY_CLAMP:
            /* Approximation: product clamped to [0,1] (see NOTES). */
            for (c = 0; c < 4; c++) r.f[c] = sat01(s0.f[c] * s1.f[c]);
            break;
        case VE_FLT2FIX_DX:
            for (c = 0; c < 4; c++) r.f[c] = f_floor(s0.f[c]);
            break;
        case VE_FLT2FIX_DX_RND:
            for (c = 0; c < 4; c++) r.f[c] = f_floor(s0.f[c] + 0.5f);
            break;
        case VE_PRED_SET_EQ_PUSH:
        case VE_PRED_SET_GT_PUSH:
        case VE_PRED_SET_GTE_PUSH:
        case VE_PRED_SET_NEQ_PUSH: {
            int cc = in->opcode - VE_PRED_SET_EQ_PUSH;
            for (c = 0; c < 4; c++) {
                if (s0.f[c] == 0.0f && cond_cc(cc, s1.f[c])) {
                    r.f[c] = 0.0f;
                } else {
                    r.f[c] = s0.f[c] + 1.0f;
                }
            }
            set_pred = true;
            new_pred = r.f[3] == 0.0f;
            break;
        }
        case VE_COND_WRITE_EQ:
        case VE_COND_WRITE_GT:
        case VE_COND_WRITE_GTE:
        case VE_COND_WRITE_NEQ: {
            int cc = in->opcode - VE_COND_WRITE_EQ;
            for (c = 0; c < 4; c++) {
                if (cond_cc(cc, s0.f[c])) {
                    r.f[c] = s1.f[c];
                } else {
                    mask &= ~(1u << c);
                }
            }
            break;
        }
        case VE_COND_MUX_EQ:
        case VE_COND_MUX_GT:
        case VE_COND_MUX_GTE: {
            int cc = in->opcode - VE_COND_MUX_EQ;
            for (c = 0; c < 4; c++) {
                r.f[c] = cond_cc(cc, s0.f[c]) ? s1.f[c] : s2.f[c];
            }
            break;
        }
        case VE_SET_GREATER_THAN:
            for (c = 0; c < 4; c++) r.f[c] = s0.f[c] > s1.f[c] ? 1.0f : 0.0f;
            break;
        case VE_SET_EQUAL:
            for (c = 0; c < 4; c++) r.f[c] = s0.f[c] == s1.f[c] ? 1.0f : 0.0f;
            break;
        case VE_SET_NOT_EQUAL:
            for (c = 0; c < 4; c++) r.f[c] = s0.f[c] != s1.f[c] ? 1.0f : 0.0f;
            break;
        default:
            return;
        }
    } else {
        /* Math engine: scalar input is the (swizzled) X lane; the result
         * is replicated to all lanes unless noted. */
        float x = s0.f[0], v = 0.0f;
        bool replicate = true;
        switch (in->opcode) {
        case MATH_NO_OP:
            return;
        case ME_EXP_BASE2_DX:
            r = me_exp_dx(x);
            replicate = false;
            break;
        case ME_LOG_BASE2_DX:
            r = me_log_dx(x);
            replicate = false;
            break;
        case ME_EXP_BASEE_FF:
            v = expf(x);
            break;
        case ME_LIGHT_COEFF_DX:
            /* Operand layout as produced by Mesa's ei_lit():
             * src0 = (x, w, 0, y), src1 = (y, w, 0, x), src2 = (y, x, 0, w)
             * -> N.L = src0.x, N.H = src1.x, power = src2.w */
            r = me_lit(s0.f[0], s1.f[0], s2.f[3]);
            replicate = false;
            break;
        case ME_POWER_FUNC_FF:
            v = f_pow(x, s2.f[0]);
            break;
        case ME_POWER_FUNC_FF_CLAMP_B:
            v = f_pow(x < 0.0f ? 0.0f : x, s2.f[0]);
            break;
        case ME_POWER_FUNC_FF_CLAMP_B1:
            v = f_pow(sat01(x), s2.f[0]);
            break;
        case ME_POWER_FUNC_FF_CLAMP_01:
            v = sat01(f_pow(x, s2.f[0]));
            break;
        case ME_RECIP_DX:
        case ME_RECIP_FF:
        case ME_RECIP_IEEE:
            v = 1.0f / x;
            break;
        case ME_RECIP_SQRT_DX:
        case ME_RECIP_SQRT_FF:
            v = f_rsq_abs(x);
            break;
        case ME_RECIP_SQRT_IEEE:
            v = 1.0f / sqrtf(x);
            break;
        case ME_MULTIPLY:
            v = x * s1.f[0];
            break;
        case ME_EXP_BASE2_FULL_DX:
            v = exp2f(x);
            break;
        case ME_LOG_BASE2_FULL_DX:
            v = fabsf(x) == 0.0f ? -INFINITY : log2f(fabsf(x));
            break;
        case ME_LOG_BASE2_IEEE:
            v = log2f(x);
            break;
        case ME_SIN:
            v = sinf(x);
            break;
        case ME_COS:
            v = cosf(x);
            break;
        case ME_PRED_SET_EQ:
        case ME_PRED_SET_GT:
        case ME_PRED_SET_GTE:
        case ME_PRED_SET_NEQ:
            new_pred = cond_cc(in->opcode - ME_PRED_SET_EQ, x);
            v = new_pred ? 0.0f : 1.0f;
            set_pred = true;
            break;
        case ME_PRED_SET_CLR:
            v = FLT_MAX;
            new_pred = false;
            set_pred = true;
            break;
        case ME_PRED_SET_INV:
            if (x == 1.0f) {
                v = 0.0f;
                new_pred = true;
            } else if (x == 0.0f) {
                v = 1.0f;
                new_pred = false;
            } else {
                v = x;
                new_pred = false;
            }
            set_pred = true;
            break;
        case ME_PRED_SET_POP:
            v = x - 1.0f;
            if (v <= 0.0f) {
                v = 0.0f;
                new_pred = true;
            } else {
                new_pred = false;
            }
            set_pred = true;
            break;
        case ME_PRED_SET_RESTORE:
            v = x;
            new_pred = x == 0.0f;
            set_pred = true;
            break;
        default:
            return;
        }
        if (replicate) {
            r = r5xx_vec4_make(v, v, v, v);
        }
    }

    if (in->sat) {
        for (c = 0; c < 4; c++) {
            r.f[c] = sat01(r.f[c]);
        }
    }
    if (set_pred) {
        ex->pred = new_pred;
    }
    exec_write(ex, in, &r, mask);
}

void r5xx_pvs_run(const R5xxPvsDecoded *d, const r5xx_vec4 *consts,
                  const r5xx_vec4 in[R5XX_PVS_MAX_INPUTS],
                  r5xx_vec4 out[R5XX_PVS_MAX_OUTPUTS])
{
    /*
     * The context lives on the stack, and only the registers that the
     * program can reach are cleared: [0, num_temps) and [0, num_alt_temps),
     * or all of them with relative addressing.  The other registers are
     * never read.  This runs once for each vertex, so clearing all 8 KB
     * (and a calloc) was a large part of the CPU vertex time.
     */
    PvsExec exs, *ex = &exs;
    int pc, steps = 0, i, nt, na;

    if (!d || !out) {
        return;
    }
    nt = d->uses_rel ? R5XX_PVS_MAX_TEMPS : (d->num_temps < R5XX_PVS_MAX_TEMPS ? d->num_temps : R5XX_PVS_MAX_TEMPS);
    na = d->uses_rel ? R5XX_PVS_MAX_TEMPS : (d->num_alt_temps < R5XX_PVS_MAX_TEMPS ? d->num_alt_temps : R5XX_PVS_MAX_TEMPS);
    memset(ex->temp, 0, nt * sizeof(ex->temp[0]));
    memset(ex->alt, 0, na * sizeof(ex->alt[0]));
    memset(ex->a0, 0, sizeof(ex->a0));
    memset(ex->loop_cnt, 0, sizeof(ex->loop_cnt));
    memset(ex->loop_idx, 0, sizeof(ex->loop_idx));
    ex->pred = false;
    ex->d = d;
    ex->consts = consts;
    ex->out = out;
    if (in) {
        memcpy(ex->in, in, sizeof(ex->in));
    } else {
        memset(ex->in, 0, sizeof(ex->in));
    }
    ex->call_ret = -1;

    pc = d->first;
    while (pc >= d->first && pc <= d->last) {
        bool redirected = false;

        if (++steps > PVS_EXEC_MAX_STEPS) {
            break;
        }
        exec_inst(ex, &d->inst[pc - d->first]);

        /* Flow control: slots are examined in order after the instruction
         * at the relevant address has executed. */
        for (i = 0; i < d->num_fc && !redirected; i++) {
            const PvsFc *fc = &d->fc[i];
            switch (fc->op) {
            case PVS_FC_OPC_LOOP:
                if (pc == fc->act) {
                    ex->loop_cnt[i] = fc->cnt_jmp;
                    ex->loop_idx[i] = fc->init;
                }
                if (pc == fc->last) {
                    if (--ex->loop_cnt[i] > 0) {
                        ex->loop_idx[i] += fc->step;
                        pc = fc->rtn;
                        redirected = true;
                    }
                }
                break;
            case PVS_FC_OPC_JUMP:
                if (pc == fc->act) {
                    pc = fc->cnt_jmp;
                    redirected = true;
                }
                break;
            case PVS_FC_OPC_JSR:
                if (pc == fc->act) {
                    ex->call_ret = pc + 1;
                    pc = fc->cnt_jmp;
                    redirected = true;
                } else if (pc == fc->last) {
                    pc = ex->call_ret >= 0 ? ex->call_ret : fc->rtn;
                    ex->call_ret = -1;
                    redirected = true;
                }
                break;
            default:
                break;
            }
        }
        if (!redirected) {
            pc++;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Disassembler (Mesa r3xx_vertprog_dump.c format)                      */
/* ------------------------------------------------------------------ */

static const char *const pvs_ve_names[32] = {
    "                 VE_NO_OP",
    "           VE_DOT_PRODUCT",
    "              VE_MULTIPLY",
    "                   VE_ADD",
    "          VE_MULTIPLY_ADD",
    "       VE_DISTANCE_FACTOR",
    "              VE_FRACTION",
    "               VE_MAXIMUM",
    "               VE_MINIMUM",
    "VE_SET_GREATER_THAN_EQUAL",
    "         VE_SET_LESS_THAN",
    "        VE_MULTIPLYX2_ADD",
    "        VE_MULTIPLY_CLAMP",
    "            VE_FLT2FIX_DX",
    "        VE_FLT2FIX_DX_RND",
    "      VE_PRED_SET_EQ_PUSH",
    "      VE_PRED_SET_GT_PUSH",
    "     VE_PRED_SET_GTE_PUSH",
    "     VE_PRED_SET_NEQ_PUSH",
    "         VE_COND_WRITE_EQ",
    "         VE_COND_WRITE_GT",
    "        VE_COND_WRITE_GTE",
    "        VE_COND_WRITE_NEQ",
    "           VE_COND_MUX_EQ",
    "           VE_COND_MUX_GT",
    "          VE_COND_MUX_GTE",
    "      VE_SET_GREATER_THAN",
    "             VE_SET_EQUAL",
    "         VE_SET_NOT_EQUAL",
    "               (reserved)",
    "               (reserved)",
    "               (reserved)",
};

static const char *const pvs_me_names[32] = {
    "                 ME_NO_OP",
    "          ME_EXP_BASE2_DX",
    "          ME_LOG_BASE2_DX",
    "          ME_EXP_BASEE_FF",
    "        ME_LIGHT_COEFF_DX",
    "         ME_POWER_FUNC_FF",
    "              ME_RECIP_DX",
    "              ME_RECIP_FF",
    "         ME_RECIP_SQRT_DX",
    "         ME_RECIP_SQRT_FF",
    "              ME_MULTIPLY",
    "     ME_EXP_BASE2_FULL_DX",
    "     ME_LOG_BASE2_FULL_DX",
    " ME_POWER_FUNC_FF_CLAMP_B",
    "ME_POWER_FUNC_FF_CLAMP_B1",
    "ME_POWER_FUNC_FF_CLAMP_01",
    "                   ME_SIN",
    "                   ME_COS",
    "        ME_LOG_BASE2_IEEE",
    "            ME_RECIP_IEEE",
    "       ME_RECIP_SQRT_IEEE",
    "           ME_PRED_SET_EQ",
    "           ME_PRED_SET_GT",
    "          ME_PRED_SET_GTE",
    "          ME_PRED_SET_NEQ",
    "          ME_PRED_SET_CLR",
    "          ME_PRED_SET_INV",
    "          ME_PRED_SET_POP",
    "      ME_PRED_SET_RESTORE",
    "               (reserved)",
    "               (reserved)",
    "               (reserved)",
};

static const char *const pvs_src_cls_names[4] = { "t", "i", "c", "a" };
static const char *const pvs_dst_cls_names[8] = {
    "t", "a0", "o", "ox", "a", "i", "u", "u"
};
static const char *const pvs_swz_names[8] = {
    "X", "Y", "Z", "W", "0", "1", "U", "U"
};

/* Bounded append helper with snprintf semantics. */
typedef struct StrBuf {
    char *buf;
    size_t len;     /* capacity */
    size_t pos;     /* bytes that would have been written */
} StrBuf;

static void sb_printf(StrBuf *sb, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

static void sb_printf(StrBuf *sb, const char *fmt, ...)
{
    va_list ap;
    int n;
    va_start(ap, fmt);
    if (sb->buf && sb->pos < sb->len) {
        n = vsnprintf(sb->buf + sb->pos, sb->len - sb->pos, fmt, ap);
    } else {
        n = vsnprintf(NULL, 0, fmt, ap);
    }
    va_end(ap);
    if (n > 0) {
        sb->pos += (size_t)n;
    }
}

static void disasm_op(StrBuf *sb, uint32_t op)
{
    sb_printf(sb, " dst: %d%s op: ", (op >> 13) & 0x7f,
              pvs_dst_cls_names[(op >> 8) & 0x7]);
    if ((op >> PVS_DST_PRED_ENABLE_SHIFT) & 0x1) {
        sb_printf(sb, "PRED %u", (op >> PVS_DST_PRED_SENSE_SHIFT) & 0x1);
    }
    if (op & 0x80) {
        if (op & 0x1) {
            sb_printf(sb, "PVS_MACRO_OP_2CLK_M2X_ADD\n");
        } else {
            sb_printf(sb, "   PVS_MACRO_OP_2CLK_MADD\n");
        }
    } else if (op & 0x40) {
        sb_printf(sb, "%s\n", pvs_me_names[op & 0x1f]);
    } else {
        sb_printf(sb, "%s\n", pvs_ve_names[op & 0x1f]);
    }
}

static void disasm_src(StrBuf *sb, uint32_t src)
{
    sb_printf(sb, " reg: %d%s swiz: %s%s/%s%s/%s%s/%s%s\n",
              (src >> 5) & 0xff, pvs_src_cls_names[src & 0x3],
              src & (1u << 25) ? "-" : " ", pvs_swz_names[(src >> 13) & 0x7],
              src & (1u << 26) ? "-" : " ", pvs_swz_names[(src >> 16) & 0x7],
              src & (1u << 27) ? "-" : " ", pvs_swz_names[(src >> 19) & 0x7],
              src & (1u << 28) ? "-" : " ", pvs_swz_names[(src >> 22) & 0x7]);
}

size_t r5xx_pvs_disasm(const R5xxPvsProgram *p, char *buf, size_t len)
{
    StrBuf sb = { buf, len, 0 };
    int first, last, i, num_fc = 0;

    if (buf && len) {
        buf[0] = '\0';
    }
    if (!p) {
        return 0;
    }
    first = (p->code_cntl0 >> R300_PVS_FIRST_INST_SHIFT) & 0x3ff;
    last = (p->code_cntl0 >> R300_PVS_LAST_INST_SHIFT) & 0x3ff;
    if (last >= R5XX_PVS_MAX_INSTR) {
        last = R5XX_PVS_MAX_INSTR - 1;
    }

    sb_printf(&sb, "Final vertex program code:\n");
    for (i = first; i <= last; i++) {
        const uint32_t *w = &p->code[i * 4];
        int s;
        /* Mesa numbers instructions from the start of the code body;
         * FIRST_INST is normally 0. */
        sb_printf(&sb, "%d: op: 0x%08x", i - first, w[0]);
        disasm_op(&sb, w[0]);
        for (s = 0; s < 3; s++) {
            sb_printf(&sb, " src%i: 0x%08x", s, w[1 + s]);
            disasm_src(&sb, w[1 + s]);
        }
        if (!((w[0] >> PVS_DST_PRED_ENABLE_SHIFT) & 1) && !((w[0] >> 6) & 3) &&
            ((w[0] >> 27) & 3)) {
            static const char comp[] = "xyzw";
            unsigned op = (w[3] >> 21) & 0x1f;
            sb_printf(&sb, " dual: %s a%u.%c = op(%s%u.%c)\n",
                      op < 32 && pvs_me_names[op] ? pvs_me_names[op] : "ME_?",
                      (w[3] >> 16) & 0x1f, comp[(w[3] >> 27) & 3],
                      (const char *[]){ "t", "i", "c", "a" }[w[3] & 3],
                      (w[3] >> PVS_SRC_OFFSET_SHIFT) & PVS_SRC_OFFSET_MASK,
                      "xyzw01__"[(w[3] >> PVS_SRC_SWIZZLE_X_SHIFT) & 7]);
        }
    }

    for (i = 0; i < R5XX_PVS_MAX_FC; i++) {
        if ((p->fc_opc >> (2 * i)) & 3) {
            num_fc = i + 1;
        }
    }
    sb_printf(&sb, "Flow Control Ops: 0x%08x\n", p->fc_opc);
    for (i = 0; i < num_fc; i++) {
        bool is_loop = false;
        switch ((p->fc_opc >> (i * 2)) & 0x3) {
        case 0: sb_printf(&sb, "NOP"); break;
        case 1: sb_printf(&sb, "JUMP"); break;
        case 2: sb_printf(&sb, "LOOP"); is_loop = true; break;
        case 3: sb_printf(&sb, "JSR"); break;
        }
        if (p->is_r500) {
            sb_printf(&sb, ": uw-> 0x%08x lw-> 0x%08x loop data->0x%08x\n",
                      p->fc_addrs_uw[i], p->fc_addrs_lw[i],
                      p->fc_loop_index[i]);
            if (is_loop) {
                sb_printf(&sb, "Before = %u First = %u Last = %u\n",
                          p->fc_addrs_lw[i] & 0xffff,
                          (p->fc_addrs_uw[i] >> 16) & 0xffff,
                          p->fc_addrs_uw[i] & 0xffff);
            }
        } else {
            sb_printf(&sb, ": 0x%08x\n", p->fc_addrs[i]);
        }
    }
    return sb.pos;
}

/* ------------------------------------------------------------------ */
/* GLSL emitter                                                        */
/* ------------------------------------------------------------------ */

typedef struct GlslBuf {
    char *s;
    size_t len, cap;
    bool oom;
} GlslBuf;

static void __attribute__((format(printf, 2, 0)))
gb_vprintf(GlslBuf *g, const char *fmt, va_list ap)
{
    va_list ap2;
    int n;
    va_copy(ap2, ap);
    n = vsnprintf(NULL, 0, fmt, ap2);
    va_end(ap2);
    if (n < 0) {
        return;
    }
    if (g->len + (size_t)n + 1 > g->cap) {
        size_t ncap = g->cap ? g->cap : 4096;
        char *ns;
        while (g->len + (size_t)n + 1 > ncap) {
            ncap *= 2;
        }
        ns = realloc(g->s, ncap);
        if (!ns) {
            g->oom = true;
            return;
        }
        g->s = ns;
        g->cap = ncap;
    }
    vsnprintf(g->s + g->len, g->cap - g->len, fmt, ap);
    g->len += (size_t)n;
}

static void gb_printf(GlslBuf *g, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

static void gb_printf(GlslBuf *g, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    gb_vprintf(g, fmt, ap);
    va_end(ap);
}

static const char glsl_swz_chars[4] = { 'x', 'y', 'z', 'w' };

/* Expression for the register a source refers to (before swizzling). */
static void glsl_src_reg(GlslBuf *g, const R5xxPvsDecoded *d, const PvsSrc *s)
{
    const char *sel = "xyzw";
    char selc = sel[s->addr_sel];
    switch (s->cls) {
    case PVS_SRC_REG_TEMPORARY:
        if (s->rel) {
            gb_printf(g, "t[clamp(%d + a0.%c, 0, %d)]", s->idx, selc,
                      R5XX_PVS_MAX_TEMPS - 1);
        } else {
            gb_printf(g, "t[%d]", s->idx);
        }
        break;
    case PVS_SRC_REG_ALT_TEMPORARY:
        if (s->rel) {
            gb_printf(g, "at[clamp(%d + a0.%c, 0, %d)]", s->idx, selc,
                      R5XX_PVS_MAX_TEMPS - 1);
        } else {
            gb_printf(g, "at[%d]", s->idx);
        }
        break;
    case PVS_SRC_REG_INPUT:
        if (s->rel) {
            gb_printf(g, "vi[clamp(%d + a0.%c, 0, %d)]", s->idx, selc,
                      R5XX_PVS_MAX_INPUTS - 1);
        } else if (s->idx < R5XX_PVS_MAX_INPUTS) {
            gb_printf(g, "vi[%d]", s->idx);
        } else {
            gb_printf(g, "vec4(0.0)");
        }
        break;
    case PVS_SRC_REG_CONSTANT:
        if (s->rel) {
            gb_printf(g, "pvs_relc(%d + a0.%c)", s->idx, selc);
        } else {
            uint32_t idx = s->idx + d->const_base;
            if (idx < R5XX_PVS_MAX_CONST) {
                gb_printf(g, "c[%u]", idx);
            } else {
                gb_printf(g, "vec4(0.0)");
            }
        }
        break;
    default:
        gb_printf(g, "vec4(0.0)");
        break;
    }
}

/* Emit "vec4 sN = ...;" for a source operand. */
static void glsl_src(GlslBuf *g, const R5xxPvsDecoded *d, const PvsSrc *s,
                     int n)
{
    bool plain = true;
    int c;
    for (c = 0; c < 4; c++) {
        if (s->swz[c] > PVS_SRC_SELECT_W) {
            plain = false;
        }
    }
    gb_printf(g, "    vec4 s%d = ", n);
    if (plain) {
        bool all_neg = s->neg == 0xf, any_neg = s->neg != 0;
        if (all_neg) {
            gb_printf(g, "-");
        }
        if (s->abs) {
            gb_printf(g, "abs(");
        }
        gb_printf(g, "(");
        glsl_src_reg(g, d, s);
        gb_printf(g, ").%c%c%c%c", glsl_swz_chars[s->swz[0]],
                  glsl_swz_chars[s->swz[1]], glsl_swz_chars[s->swz[2]],
                  glsl_swz_chars[s->swz[3]]);
        if (s->abs) {
            gb_printf(g, ")");
        }
        if (any_neg && !all_neg) {
            gb_printf(g, " * vec4(%s, %s, %s, %s)",
                      (s->neg & 1) ? "-1.0" : "1.0",
                      (s->neg & 2) ? "-1.0" : "1.0",
                      (s->neg & 4) ? "-1.0" : "1.0",
                      (s->neg & 8) ? "-1.0" : "1.0");
        }
        gb_printf(g, ";\n");
        return;
    }
    /* Mixed register / constant selects: build component-wise. */
    gb_printf(g, "vec4(");
    for (c = 0; c < 4; c++) {
        bool neg = (s->neg >> c) & 1;
        gb_printf(g, "%s%s", c ? ", " : "", neg ? "-" : "");
        if (s->swz[c] <= PVS_SRC_SELECT_W) {
            if (s->abs) {
                gb_printf(g, "abs(");
            }
            gb_printf(g, "(");
            glsl_src_reg(g, d, s);
            gb_printf(g, ").%c", glsl_swz_chars[s->swz[c]]);
            if (s->abs) {
                gb_printf(g, ")");
            }
        } else if (s->swz[c] == PVS_SRC_SELECT_FORCE_1) {
            gb_printf(g, "1.0");
        } else {
            gb_printf(g, "0.0");
        }
    }
    gb_printf(g, ");\n");
}

static void glsl_mask_str(char *out, unsigned mask)
{
    int c, n = 0;
    for (c = 0; c < 4; c++) {
        if (mask & (1 << c)) {
            out[n++] = glsl_swz_chars[c];
        }
    }
    out[n] = '\0';
}

/* Destination l-value (without component selection). Returns false when
 * the destination is unwritable (e.g. input class, out of range). */
static bool glsl_dst_lvalue(GlslBuf *g, const PvsInst *in)
{
    const char *sel = "xyzw";
    char selc = sel[in->dst_addr_sel];
    switch (in->dst_cls) {
    case PVS_DST_REG_TEMPORARY:
        if (in->dst_rel) {
            gb_printf(g, "t[clamp(%d + a0.%c, 0, %d)]", in->dst_idx, selc,
                      R5XX_PVS_MAX_TEMPS - 1);
        } else {
            gb_printf(g, "t[%d]", in->dst_idx);
        }
        return true;
    case PVS_DST_REG_ALT_TEMPORARY:
        if (in->dst_rel) {
            gb_printf(g, "at[clamp(%d + a0.%c, 0, %d)]", in->dst_idx, selc,
                      R5XX_PVS_MAX_TEMPS - 1);
        } else {
            gb_printf(g, "at[%d]", in->dst_idx);
        }
        return true;
    case PVS_DST_REG_OUT:
    case PVS_DST_REG_OUT_REPL_X:
        if (in->dst_rel) {
            gb_printf(g, "o[clamp(%d + a0.%c, 0, %d)]", in->dst_idx, selc,
                      R5XX_PVS_MAX_OUTPUTS - 1);
        } else if (in->dst_idx < R5XX_PVS_MAX_OUTPUTS) {
            gb_printf(g, "o[%d]", in->dst_idx);
        } else {
            return false;
        }
        return true;
    case PVS_DST_REG_A0:
        gb_printf(g, "a0");
        return true;
    case PVS_DST_REG_INPUT:
        if (in->dst_rel) {
            gb_printf(g, "vi[clamp(%d + a0.%c, 0, %d)]", in->dst_idx, selc,
                      R5XX_PVS_MAX_INPUTS - 1);
        } else if (in->dst_idx < R5XX_PVS_MAX_INPUTS) {
            gb_printf(g, "vi[%d]", in->dst_idx);
        } else {
            return false;
        }
        return true;
    default:
        return false;
    }
}

static const char *glsl_cc_op(int cc)
{
    switch (cc) {
    case 0:  return "==";
    case 1:  return ">";
    case 2:  return ">=";
    default: return "!=";
    }
}

static void glsl_emit_write(GlslBuf *g, const PvsInst *in, unsigned mask,
                            const char *rexpr)
{
    char m[5];
    if (!mask) {
        return;
    }
    glsl_mask_str(m, mask);
    gb_printf(g, "    ");
    if (in->dst_cls == PVS_DST_REG_A0) {
        if (!glsl_dst_lvalue(g, in)) {
            return;
        }
        gb_printf(g, ".%s = ivec4(floor(clamp(%s, -1048576.0, 1048576.0))).%s;\n",
                  m, rexpr, m);
        return;
    }
    if (!glsl_dst_lvalue(g, in)) {
        gb_printf(g, "/* unwritable destination */;\n");
        return;
    }
    if (in->dst_cls == PVS_DST_REG_OUT_REPL_X) {
        gb_printf(g, ".%s = (%s).xxxx.%s;\n", m, rexpr, m);
    } else {
        gb_printf(g, ".%s = (%s).%s;\n", m, rexpr, m);
    }
}

static void glsl_emit_inst(GlslBuf *g, const R5xxPvsDecoded *d,
                           const PvsInst *in, int index)
{
    unsigned mask = in->wmask;
    bool pred_wrap = in->pred_en;
    int c;

    gb_printf(g, "    /* %d: 0x%08x 0x%08x 0x%08x 0x%08x */\n", index,
              in->raw[0], in->raw[1], in->raw[2], in->raw[3]);
    if (!in->macro && ((in->math && in->opcode == MATH_NO_OP) ||
                       (!in->math && in->opcode == VECTOR_NO_OP))) {
        return;
    }
    gb_printf(g, "    %s{\n", pred_wrap ? (in->pred_sense ? "if (pred) " : "if (!pred) ") : "");
    glsl_src(g, d, &in->src[0], 0);
    glsl_src(g, d, &in->src[1], 1);
    glsl_src(g, d, &in->src[2], 2);
    gb_printf(g, "    vec4 r;\n");

    if (in->macro) {
        gb_printf(g, "    r = %ss0 * s1 + s2;\n",
                  (in->opcode & 1) ? "2.0 * " : "");
    } else if (!in->math) {
        switch (in->opcode) {
        case VE_DOT_PRODUCT:
            gb_printf(g, "    r = vec4(dot(s0, s1));\n");
            break;
        case VE_MULTIPLY:
            gb_printf(g, "    r = s0 * s1;\n");
            break;
        case VE_ADD:
            gb_printf(g, "    r = s0 + s1;\n");
            break;
        case VE_MULTIPLY_ADD:
            gb_printf(g, "    r = s0 * s1 + s2;\n");
            break;
        case VE_DISTANCE_VECTOR:
            gb_printf(g, "    r = vec4(1.0, s0.y * s1.y, s0.z, s1.w);\n");
            break;
        case VE_FRACTION:
            gb_printf(g, "    r = s0 - floor(s0);\n");
            break;
        case VE_MAXIMUM:
            gb_printf(g, "    r = max(s0, s1);\n");
            break;
        case VE_MINIMUM:
            gb_printf(g, "    r = min(s0, s1);\n");
            break;
        case VE_SET_GREATER_THAN_EQUAL:
            gb_printf(g, "    r = vec4(greaterThanEqual(s0, s1));\n");
            break;
        case VE_SET_LESS_THAN:
            gb_printf(g, "    r = vec4(lessThan(s0, s1));\n");
            break;
        case VE_MULTIPLYX2_ADD:
            gb_printf(g, "    r = 2.0 * s0 * s1 + s2;\n");
            break;
        case VE_MULTIPLY_CLAMP:
            gb_printf(g, "    r = clamp(s0 * s1, 0.0, 1.0);\n");
            break;
        case VE_FLT2FIX_DX:
            gb_printf(g, "    r = floor(s0);\n");
            break;
        case VE_FLT2FIX_DX_RND:
            gb_printf(g, "    r = floor(s0 + 0.5);\n");
            break;
        case VE_PRED_SET_EQ_PUSH:
        case VE_PRED_SET_GT_PUSH:
        case VE_PRED_SET_GTE_PUSH:
        case VE_PRED_SET_NEQ_PUSH: {
            const char *op = glsl_cc_op(in->opcode - VE_PRED_SET_EQ_PUSH);
            gb_printf(g, "    r = s0 + 1.0;\n");
            for (c = 0; c < 4; c++) {
                gb_printf(g, "    if (s0.%c == 0.0 && s1.%c %s 0.0) r.%c = 0.0;\n",
                          glsl_swz_chars[c], glsl_swz_chars[c], op,
                          glsl_swz_chars[c]);
            }
            gb_printf(g, "    pred = (r.w == 0.0);\n");
            break;
        }
        case VE_COND_WRITE_EQ:
        case VE_COND_WRITE_GT:
        case VE_COND_WRITE_GTE:
        case VE_COND_WRITE_NEQ: {
            const char *op = glsl_cc_op(in->opcode - VE_COND_WRITE_EQ);
            gb_printf(g, "    r = s1;\n");
            if (in->sat) {
                gb_printf(g, "    r = clamp(r, 0.0, 1.0);\n");
            }
            for (c = 0; c < 4; c++) {
                char cm[5];
                if (!(mask & (1 << c))) {
                    continue;
                }
                glsl_mask_str(cm, 1u << c);
                gb_printf(g, "    if (s0.%c %s 0.0) ", glsl_swz_chars[c], op);
                if (in->dst_cls == PVS_DST_REG_A0) {
                    gb_printf(g, "a0.%s = int(floor(clamp(r.%s, -1048576.0, 1048576.0)));\n", cm, cm);
                } else if (glsl_dst_lvalue(g, in)) {
                    gb_printf(g, ".%s = r.%s;\n", cm,
                              in->dst_cls == PVS_DST_REG_OUT_REPL_X ? "x" : cm);
                } else {
                    gb_printf(g, ";\n");
                }
            }
            gb_printf(g, "    }\n");
            return;
        }
        case VE_COND_MUX_EQ:
        case VE_COND_MUX_GT:
        case VE_COND_MUX_GTE: {
            const char *op = glsl_cc_op(in->opcode - VE_COND_MUX_EQ);
            gb_printf(g, "    r = s2;\n");
            for (c = 0; c < 4; c++) {
                gb_printf(g, "    if (s0.%c %s 0.0) r.%c = s1.%c;\n",
                          glsl_swz_chars[c], op, glsl_swz_chars[c],
                          glsl_swz_chars[c]);
            }
            break;
        }
        case VE_SET_GREATER_THAN:
            gb_printf(g, "    r = vec4(greaterThan(s0, s1));\n");
            break;
        case VE_SET_EQUAL:
            gb_printf(g, "    r = vec4(equal(s0, s1));\n");
            break;
        case VE_SET_NOT_EQUAL:
            gb_printf(g, "    r = vec4(notEqual(s0, s1));\n");
            break;
        default:
            gb_printf(g, "    r = vec4(0.0);\n");
            break;
        }
    } else {
        switch (in->opcode) {
        case ME_EXP_BASE2_DX:
            gb_printf(g, "    r = pvs_exp_dx(s0.x);\n");
            break;
        case ME_LOG_BASE2_DX:
            gb_printf(g, "    r = pvs_log_dx(s0.x);\n");
            break;
        case ME_EXP_BASEE_FF:
            gb_printf(g, "    r = vec4(exp(s0.x));\n");
            break;
        case ME_LIGHT_COEFF_DX:
            gb_printf(g, "    r = pvs_lit(s0.x, s1.x, s2.w);\n");
            break;
        case ME_POWER_FUNC_FF:
            gb_printf(g, "    r = vec4(pvs_pow(s0.x, s2.x));\n");
            break;
        case ME_POWER_FUNC_FF_CLAMP_B:
            gb_printf(g, "    r = vec4(pvs_pow(max(s0.x, 0.0), s2.x));\n");
            break;
        case ME_POWER_FUNC_FF_CLAMP_B1:
            gb_printf(g, "    r = vec4(pvs_pow(clamp(s0.x, 0.0, 1.0), s2.x));\n");
            break;
        case ME_POWER_FUNC_FF_CLAMP_01:
            gb_printf(g, "    r = vec4(clamp(pvs_pow(s0.x, s2.x), 0.0, 1.0));\n");
            break;
        case ME_RECIP_DX:
        case ME_RECIP_FF:
        case ME_RECIP_IEEE:
            gb_printf(g, "    r = vec4(1.0 / s0.x);\n");
            break;
        case ME_RECIP_SQRT_DX:
        case ME_RECIP_SQRT_FF:
            gb_printf(g, "    r = vec4(inversesqrt(abs(s0.x)));\n");
            break;
        case ME_RECIP_SQRT_IEEE:
            gb_printf(g, "    r = vec4(inversesqrt(s0.x));\n");
            break;
        case ME_MULTIPLY:
            gb_printf(g, "    r = vec4(s0.x * s1.x);\n");
            break;
        case ME_EXP_BASE2_FULL_DX:
            gb_printf(g, "    r = vec4(exp2(s0.x));\n");
            break;
        case ME_LOG_BASE2_FULL_DX:
            gb_printf(g, "    r = vec4(pvs_log2_abs(s0.x));\n");
            break;
        case ME_LOG_BASE2_IEEE:
            gb_printf(g, "    r = vec4(log2(s0.x));\n");
            break;
        case ME_SIN:
            gb_printf(g, "    r = vec4(sin(s0.x));\n");
            break;
        case ME_COS:
            gb_printf(g, "    r = vec4(cos(s0.x));\n");
            break;
        case ME_PRED_SET_EQ:
        case ME_PRED_SET_GT:
        case ME_PRED_SET_GTE:
        case ME_PRED_SET_NEQ:
            gb_printf(g, "    pred = (s0.x %s 0.0);\n",
                      glsl_cc_op(in->opcode - ME_PRED_SET_EQ));
            gb_printf(g, "    r = vec4(pred ? 0.0 : 1.0);\n");
            break;
        case ME_PRED_SET_CLR:
            gb_printf(g, "    pred = false;\n    r = vec4(3.402823466e+38);\n");
            break;
        case ME_PRED_SET_INV:
            gb_printf(g, "    if (s0.x == 1.0) { r = vec4(0.0); pred = true; }\n"
                         "    else if (s0.x == 0.0) { r = vec4(1.0); pred = false; }\n"
                         "    else { r = vec4(s0.x); pred = false; }\n");
            break;
        case ME_PRED_SET_POP:
            gb_printf(g, "    r = vec4(max(s0.x - 1.0, 0.0));\n"
                         "    pred = (r.x == 0.0);\n");
            break;
        case ME_PRED_SET_RESTORE:
            gb_printf(g, "    r = vec4(s0.x);\n    pred = (s0.x == 0.0);\n");
            break;
        default:
            gb_printf(g, "    r = vec4(0.0);\n");
            break;
        }
    }
    if (in->sat) {
        gb_printf(g, "    r = clamp(r, 0.0, 1.0);\n");
    }
    glsl_emit_write(g, in, mask, "r");
    gb_printf(g, "    }\n");
}

/* Check whether the FC configuration is a set of properly nested LOOPs
 * so that it can be emitted as structured for() loops. */
static bool glsl_fc_is_structured(const R5xxPvsDecoded *d)
{
    int i, j;
    for (i = 0; i < d->num_fc; i++) {
        const PvsFc *a = &d->fc[i];
        if (a->op == PVS_FC_OPC_NOP) {
            continue;
        }
        if (a->op != PVS_FC_OPC_LOOP) {
            return false;
        }
        if (a->rtn != a->act + 1 || a->last < a->rtn ||
            a->act < d->first || a->last > d->last) {
            return false;
        }
        for (j = 0; j < d->num_fc; j++) {
            const PvsFc *b = &d->fc[j];
            if (i == j || b->op != PVS_FC_OPC_LOOP) {
                continue;
            }
            /* either disjoint or properly nested */
            if (b->act == a->act || b->last == a->last) {
                return false;
            }
            if (!(b->last < a->act || b->act > a->last ||
                  (b->act > a->act && b->last < a->last) ||
                  (b->act < a->act && b->last > a->last))) {
                return false;
            }
        }
    }
    return true;
}

static void glsl_emit_fc_pc_machine(GlslBuf *g, const R5xxPvsDecoded *d)
{
    int i;
    gb_printf(g, "    int pc = %d;\n    int call_ret = -1;\n", d->first);
    gb_printf(g, "    int lc[%d];\n    int li[%d];\n", R5XX_PVS_MAX_FC,
              R5XX_PVS_MAX_FC);
    gb_printf(g, "    for (int guard = 0; guard < %d; ++guard) {\n",
              PVS_EXEC_MAX_STEPS);
    gb_printf(g, "    if (pc < %d || pc > %d) break;\n", d->first, d->last);
    gb_printf(g, "    switch (pc) {\n");
    for (i = 0; i < d->n; i++) {
        gb_printf(g, "    case %d:\n", d->first + i);
        glsl_emit_inst(g, d, &d->inst[i], d->first + i);
        gb_printf(g, "    break;\n");
    }
    gb_printf(g, "    default: break;\n    }\n");
    for (i = 0; i < d->num_fc; i++) {
        const PvsFc *fc = &d->fc[i];
        switch (fc->op) {
        case PVS_FC_OPC_LOOP:
            gb_printf(g, "    if (pc == %u) { lc[%d] = %u; li[%d] = %u; }\n",
                      fc->act, i, fc->cnt_jmp, i, fc->init);
            gb_printf(g, "    if (pc == %u) { lc[%d]--; if (lc[%d] > 0) { li[%d] += %u; pc = %u; continue; } }\n",
                      fc->last, i, i, i, fc->step, fc->rtn);
            break;
        case PVS_FC_OPC_JUMP:
            gb_printf(g, "    if (pc == %u) { pc = %u; continue; }\n",
                      fc->act, fc->cnt_jmp);
            break;
        case PVS_FC_OPC_JSR:
            gb_printf(g, "    if (pc == %u) { call_ret = pc + 1; pc = %u; continue; }\n",
                      fc->act, fc->cnt_jmp);
            gb_printf(g, "    if (pc == %u) { pc = call_ret >= 0 ? call_ret : %u; call_ret = -1; continue; }\n",
                      fc->last, fc->rtn);
            break;
        default:
            break;
        }
    }
    gb_printf(g, "    pc++;\n    }\n");
}

static void glsl_emit_structured(GlslBuf *g, const R5xxPvsDecoded *d)
{
    int pc, i;
    for (pc = d->first; pc <= d->last; pc++) {
        glsl_emit_inst(g, d, &d->inst[pc - d->first], pc);
        /* loops starting after this instruction (outermost first) */
        for (;;) {
            int best = -1;
            for (i = 0; i < d->num_fc; i++) {
                const PvsFc *fc = &d->fc[i];
                if (fc->op == PVS_FC_OPC_LOOP && fc->act == pc &&
                    (best < 0 || fc->last > d->fc[best].last)) {
                    best = i;
                }
            }
            if (best < 0) {
                break;
            }
            gb_printf(g, "    for (int lc%d = 0, li%d = %u; lc%d < %u; ++lc%d, li%d += %u) {\n",
                      best, best, d->fc[best].init, best,
                      d->fc[best].cnt_jmp ? d->fc[best].cnt_jmp : 1u,
                      best, best, d->fc[best].step);
            /* mark as emitted: temporarily poison act so it is not reopened */
            ((PvsFc *)&d->fc[best])->act = 0xffff;
        }
        /* loops ending at this instruction (innermost first) */
        for (;;) {
            int best = -1;
            for (i = 0; i < d->num_fc; i++) {
                const PvsFc *fc = &d->fc[i];
                if (fc->op == PVS_FC_OPC_LOOP && fc->last == pc &&
                    fc->act == 0xffff &&
                    (best < 0 || fc->rtn > d->fc[best].rtn)) {
                    best = i;
                }
            }
            if (best < 0) {
                break;
            }
            gb_printf(g, "    }\n");
            ((PvsFc *)&d->fc[best])->last = 0xffff;
        }
    }
}

char *r5xx_pvs_to_glsl(const R5xxPvsDecoded *d)
{
    GlslBuf g = { NULL, 0, 0, false };
    R5xxPvsDecoded *copy;
    uint32_t m;
    int i;

    if (!d || d->uses_dual) {         /* dual math: interpreter only */
        return NULL;
    }
    /* Work on a copy: structured emission scribbles on the FC table. */
    copy = malloc(sizeof(*copy));
    if (!copy) {
        return NULL;
    }
    *copy = *d;

    gb_printf(&g, "#version 450\n");
    gb_printf(&g, "/* R5xx PVS program: %d instructions, const base %u */\n",
              d->n, d->const_base);
    for (i = 0; i < R5XX_PVS_MAX_INPUTS; i++) {
        if (d->inputs_used & (1u << i)) {
            gb_printf(&g, "layout(location = %d) in vec4 vin%d;\n", i, i);
        }
    }
    gb_printf(&g, "layout(std140, set = 0, binding = 0) uniform PvsConsts { vec4 c[%d]; };\n",
              R5XX_PVS_MAX_CONST);
    m = d->outputs_written;
    if (d->uses_rel_out) {
        m = 0xffffffffu;   /* relative output writes: declare them all */
    }
    for (i = 0; i < R5XX_PVS_MAX_OUTPUTS; i++) {
        if (m & (1u << i)) {
            gb_printf(&g, "layout(location = %d) out vec4 vout%d;\n", i, i);
        }
    }
    gb_printf(&g,
        "float pvs_neg_inf() { return uintBitsToFloat(0xff800000u); }\n"
        "vec4 pvs_relc(int i) { return (i < 0 || i > %u) ? vec4(0.0) : c[i + %u]; }\n"
        "vec4 pvs_exp_dx(float x) { float f = floor(x); return vec4(exp2(f), x - f, exp2(x), 1.0); }\n"
        "vec4 pvs_log_dx(float x) { float a = abs(x); if (a == 0.0) return vec4(pvs_neg_inf(), 1.0, pvs_neg_inf(), 1.0);\n"
        "    float l = log2(a); float f = floor(l); return vec4(f, a / exp2(f), l, 1.0); }\n"
        "float pvs_log2_abs(float x) { float a = abs(x); return a == 0.0 ? pvs_neg_inf() : log2(a); }\n"
        "float pvs_pow(float a, float b) { a = abs(a); if (a == 0.0) return b == 0.0 ? 1.0 : (b > 0.0 ? 0.0 : uintBitsToFloat(0x7f800000u)); return pow(a, b); }\n"
        "vec4 pvs_lit(float nl, float nh, float p) { p = clamp(p, -128.0, 128.0); float z = 0.0;\n"
        "    if (nl > 0.0) { nh = max(nh, 0.0); z = (nh == 0.0) ? (p == 0.0 ? 1.0 : (p > 0.0 ? 0.0 : uintBitsToFloat(0x7f800000u))) : pow(nh, p); }\n"
        "    return vec4(1.0, max(nl, 0.0), z, 1.0); }\n",
        d->const_max, d->const_base);

    gb_printf(&g, "void main() {\n");
    gb_printf(&g, "    vec4 t[%d];\n    vec4 at[%d];\n    vec4 o[%d];\n    vec4 vi[%d];\n",
              R5XX_PVS_MAX_TEMPS, R5XX_PVS_MAX_TEMPS, R5XX_PVS_MAX_OUTPUTS,
              R5XX_PVS_MAX_INPUTS);
    gb_printf(&g, "    for (int i = 0; i < %d; ++i) { t[i] = vec4(0.0); at[i] = vec4(0.0); }\n",
              R5XX_PVS_MAX_TEMPS);
    gb_printf(&g, "    for (int i = 0; i < %d; ++i) o[i] = vec4(0.0);\n",
              R5XX_PVS_MAX_OUTPUTS);
    gb_printf(&g, "    for (int i = 0; i < %d; ++i) vi[i] = vec4(0.0);\n",
              R5XX_PVS_MAX_INPUTS);
    for (i = 0; i < R5XX_PVS_MAX_INPUTS; i++) {
        if (d->inputs_used & (1u << i)) {
            gb_printf(&g, "    vi[%d] = vin%d;\n", i, i);
        }
    }
    gb_printf(&g, "    ivec4 a0 = ivec4(0);\n    bool pred = false;\n");

    if (!d->uses_fc) {
        for (i = 0; i < d->n; i++) {
            glsl_emit_inst(&g, d, &d->inst[i], d->first + i);
        }
    } else if (glsl_fc_is_structured(copy)) {
        glsl_emit_structured(&g, copy);
    } else {
        glsl_emit_fc_pc_machine(&g, d);
    }

    for (i = 0; i < R5XX_PVS_MAX_OUTPUTS; i++) {
        if (m & (1u << i)) {
            gb_printf(&g, "    vout%d = o[%d];\n", i, i);
        }
    }
    gb_printf(&g, "    gl_Position = o[0];\n}\n");
    free(copy);

    if (g.oom) {
        free(g.s);
        return NULL;
    }
    return g.s;
}
