/*
 * R500 fragment program ("US", unified shader) decoder, CPU interpreter
 * and GLSL emitter.
 *
 * Programs are uploaded through R500_GA_US_VECTOR_INDEX (0x4250) /
 * R500_GA_US_VECTOR_DATA (0x4254): index type INSTR selects code
 * (6 dwords per instruction, up to 512), type CONST selects constants
 * (4 floats, up to 256).  Encodings: Mesa
 * src/gallium/drivers/r300/compiler/r500_fragprog_emit.c,
 * r500_fragprog.c (r500FragmentProgramDump) and r300_reg.h (R500_INST_*,
 * R500_RGB_*, R500_ALPHA_*, R500_TEX_*, R500_FC_*).
 *
 * Pure C, no QEMU dependencies.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef R500_US_H
#define R500_US_H

#include "r5xx_core.h"
#include "r5xx_tex.h"

#define R500_US_MAX_INSTR    512
#define R500_US_MAX_CONST    256
#define R500_US_MAX_TEMPS    128
#define R500_US_MAX_INPUTS   12      /* interpolated vec4 inputs (RS_INST count) */
#define R500_US_MAX_OUTPUTS  4       /* color render targets */
#define R500_US_MAX_FC       16      /* flow-control slots */

/* Raw state as written by the guest.  Filled by the 3D state module. */
typedef struct R500UsProgram {
    uint32_t code[R500_US_MAX_INSTR * 6];
    uint32_t config;        /* R500_US_CONFIG      0x4600 */
    uint32_t pixsize;       /* R500_US_PIXSIZE     0x4604 */
    uint32_t code_addr;     /* R500_US_CODE_ADDR   0x4630: start/end */
    uint32_t code_range;    /* R500_US_CODE_RANGE  0x4634 */
    uint32_t code_offset;   /* R500_US_CODE_OFFSET 0x4638 */
    uint32_t fc_ctrl;       /* R500_US_FC_CTRL     0x4624 */
    uint32_t fc_inst[R500_US_MAX_FC];   /* R500_US_FC_INST_n 0x9800 + 4n */
    uint32_t fc_addr[R500_US_MAX_FC];   /* R500_US_FC_ADDR_n 0xA000 + 4n */
    uint32_t out_fmt[4];    /* R300_US_OUT_FMT_n   0x46A4 + 4n */
    uint32_t w_fmt;         /* R300_US_W_FMT       0x46B4 */
    /* Added (see docs/us_NOTES.md): loop/boolean constants used by the
     * flow-control unit.  Zero when the guest never wrote them. */
    uint32_t fc_int_const[32];  /* R500_US_FC_INT_CONST_n 0x4C00 + 4n: KR iterations, KG aL init, KB aL step */
    uint32_t fc_bool_const;     /* R500_US_FC_BOOL_CONST  0x4620: 32 booleans */
} R500UsProgram;

typedef struct R500UsDecoded R500UsDecoded;

R500UsDecoded *r500_us_decode(const R500UsProgram *p, char *err, size_t errlen);
void r500_us_free(R500UsDecoded *d);

int r500_us_num_instructions(const R500UsDecoded *d);
uint32_t r500_us_inputs_used(const R500UsDecoded *d);     /* bitmask */
uint32_t r500_us_tex_units_used(const R500UsDecoded *d);  /* bitmask */
uint32_t r500_us_outputs_written(const R500UsDecoded *d); /* bitmask of color outputs */
bool r500_us_writes_depth(const R500UsDecoded *d);
bool r500_us_uses_kill(const R500UsDecoded *d);
bool r500_us_uses_flow_control(const R500UsDecoded *d);
/* Hash of the code range and decode flags (Vulkan pipeline cache key). */
uint64_t r500_us_hash(const R500UsDecoded *d);
const char *r500_us_glsl_build_id(void);

/* Texture sampling callback: the interpreter never touches memory itself. */
typedef void (*r500_us_tex_fn)(void *opaque, const R5xxTexRequest *req,
                               r5xx_vec4 out[4]);

typedef struct R500UsExec {
    const R500UsDecoded *prog;
    const r5xx_vec4 *consts;        /* R500_US_MAX_CONST entries */
    r500_us_tex_fn tex;
    void *tex_opaque;
} R500UsExec;

/* Per-quad input: 4 pixels, each with its interpolated input vectors.
 * Pixel order: 0 = top-left, 1 = top-right, 2 = bottom-left,
 * 3 = bottom-right.  inputs[p][i] is input register i of pixel p.  The
 * shader also sees the window position (for R500_INST_* face/position
 * sources) and front-facing flag. */
typedef struct R500UsQuadIn {
    r5xx_vec4 inputs[4][R500_US_MAX_INPUTS];
    float     x[4], y[4], z[4], w[4];   /* window position, depth, 1/w */
    bool      front_facing;
    uint8_t   coverage;                 /* bit p set: pixel p inside primitive */
} R500UsQuadIn;

typedef struct R500UsQuadOut {
    r5xx_vec4 color[4][R500_US_MAX_OUTPUTS];  /* [pixel][render target] */
    float     depth[4];
    uint8_t   killed;                   /* bit p set: pixel p discarded */
    bool      depth_written;
} R500UsQuadOut;

/* Run the program on one quad.  All four pixels execute in lock-step so
 * texture LOD can be derived; non-covered pixels still execute but are
 * never written back by the caller. */
void r500_us_run_quad(const R500UsExec *e, const R500UsQuadIn *in,
                      R500UsQuadOut *out);

/* Listing in the style of Mesa's r500FragmentProgramDump. */
size_t r500_us_disasm(const R500UsProgram *p, char *buf, size_t len);

/*
 * GLSL (Vulkan dialect, #version 450) fragment shader equivalent.
 *   inputs:   layout(location = i) in vec4 fin[i]
 *   consts:   layout(std140, set = 0, binding = 1) uniform UsConsts
 *             { vec4 c[256]; }
 *   textures: layout(set = 1, binding = n) uniform sampler2D tex[n]
 *             (samplerCube / sampler3D chosen from tex_kind[n])
 *   outputs:  layout(location = i) out vec4 fout[i]; gl_FragDepth
 * tex_kind[n]: 0 = 2D, 1 = cube, 2 = 3D, 3 = 2D shadow.
 * Returns a malloc()ed string the caller frees, or NULL.
 */
char *r500_us_to_glsl(const R500UsDecoded *d, const uint8_t tex_kind[16]);

#endif /* R500_US_H */
