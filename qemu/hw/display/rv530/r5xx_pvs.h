/*
 * R300/R500 vertex program (PVS) decoder, CPU interpreter and GLSL emitter.
 *
 * The PVS is programmed by the guest through R300_VAP_PVS_VECTOR_INDX_REG
 * (0x2200) / R300_VAP_PVS_VECTOR_DATA_REG (0x2204): index < 1024 selects
 * code (4 dwords per instruction), index >= R500_PVS_CONST_START (1024)
 * selects constants (4 floats per vector).  The 3D state module latches
 * those writes into R5xxPvsProgram and calls into this module at draw
 * time.  Encodings: Mesa src/gallium/drivers/r300/compiler/r3xx_vertprog.c,
 * r3xx_vertprog_dump.c, radeon_code.h and r300_reg.h (R300_VPI_*).
 *
 * Pure C, no QEMU dependencies.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef R5XX_PVS_H
#define R5XX_PVS_H

#include "r5xx_core.h"

#define R5XX_PVS_MAX_INSTR     1024      /* R500_MAX_PVS_CODE_LINES */
#define R5XX_PVS_MAX_CONST     1024      /* R500_MAX_PVS_CONST_VECS */
#define R5XX_PVS_MAX_INPUTS    16        /* VAP input vector slots */
#define R5XX_PVS_MAX_OUTPUTS   32        /* result registers */
#define R5XX_PVS_MAX_TEMPS     256       /* generous; R500 has 128 */
#define R5XX_PVS_MAX_FC        16        /* flow-control slots */

/* Raw state as written by the guest.  Filled by the 3D state module. */
typedef struct R5xxPvsProgram {
    uint32_t code[R5XX_PVS_MAX_INSTR * 4];
    /* R300_VAP_PVS_CODE_CNTL_0 (0x22D0): first/last instruction,
     * first instruction of position-only pass etc. */
    uint32_t code_cntl0;
    /* R300_VAP_PVS_CODE_CNTL_1 (0x22D8) */
    uint32_t code_cntl1;
    /* R300_VAP_PVS_CONST_CNTL (0x22D4): constant base / count */
    uint32_t const_cntl;
    /* R300_VAP_CNTL (0x2080): PVS num slots, cntl etc. */
    uint32_t vap_cntl;
    /* flow control (R500 only) */
    uint32_t fc_opc;                            /* 0x22DC */
    uint32_t fc_addrs[R5XX_PVS_MAX_FC];         /* 0x2230.. */
    uint32_t fc_loop_index[R5XX_PVS_MAX_FC];    /* 0x2290.. */
    uint32_t fc_addrs_lw[R5XX_PVS_MAX_FC];      /* 0x2500.. (R500) */
    uint32_t fc_addrs_uw[R5XX_PVS_MAX_FC];      /* 0x2504.. (R500) */
    bool is_r500;
} R5xxPvsProgram;

/* Decoded program; opaque to callers. */
typedef struct R5xxPvsDecoded R5xxPvsDecoded;

/* Decode the instruction range selected by code_cntl0.  On failure
 * returns NULL and writes a message into err (may be NULL). */
R5xxPvsDecoded *r5xx_pvs_decode(const R5xxPvsProgram *p,
                                char *err, size_t errlen);
void r5xx_pvs_free(R5xxPvsDecoded *d);

/* Information the vertex-fetch/rasterizer glue needs. */
int r5xx_pvs_num_instructions(const R5xxPvsDecoded *d);
uint32_t r5xx_pvs_inputs_used(const R5xxPvsDecoded *d);   /* bitmask */
uint32_t r5xx_pvs_outputs_written(const R5xxPvsDecoded *d); /* bitmask */
bool r5xx_pvs_uses_flow_control(const R5xxPvsDecoded *d);

/*
 * Execute the program for one vertex.
 *   consts: the full constant file as uploaded (index 0 == PVS const 0,
 *           i.e. VECTOR_INDX 1024 on R500).  Constant addressing honors
 *           const_cntl (base) and address-register-relative access.
 *   in:     input vectors, index == input slot written by vertex fetch.
 *   out:    result registers; out[0] is the clip-space position.
 * Outputs not written by the program are left untouched.
 */
void r5xx_pvs_run(const R5xxPvsDecoded *d,
                  const r5xx_vec4 *consts,
                  const r5xx_vec4 in[R5XX_PVS_MAX_INPUTS],
                  r5xx_vec4 out[R5XX_PVS_MAX_OUTPUTS]);

/* Human-readable listing, Mesa r3xx_vertprog_dump style. Returns the
 * number of bytes that would have been written (snprintf semantics). */
size_t r5xx_pvs_disasm(const R5xxPvsProgram *p, char *buf, size_t len);

/*
 * GLSL (Vulkan dialect, #version 450) vertex shader equivalent.
 *   inputs:   layout(location = i) in vec4 vin[i]  (one per used slot)
 *   consts:   layout(std140, set = 0, binding = 0) uniform PvsConsts
 *             { vec4 c[1024]; }
 *   outputs:  layout(location = i) out vec4 vout[i]; gl_Position = vout[0]
 * Returns a malloc()ed string the caller frees, or NULL.
 */
char *r5xx_pvs_to_glsl(const R5xxPvsDecoded *d);

#endif /* R5XX_PVS_H */
