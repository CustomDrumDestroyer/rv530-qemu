/*
 * CPU reference rasterizer for the R300/R500 3D pipeline.
 *
 * Takes primitives whose vertices are already in window coordinates with
 * per-vertex fragment-shader inputs, rasterizes them in 2x2 quads with
 * perspective-correct interpolation, runs the fragment shader through a
 * callback and applies the fixed-function per-pixel state (scissor,
 * clip rectangles, depth/stencil test, alpha test, blending, logic ops,
 * color mask, dithering ignored) exactly as the hardware registers
 * describe it.  Render targets are read and written through R5xxMem.
 *
 * Reference: Mesa src/gallium/drivers/r300/r300_state.c
 * (r300_create_blend_state, r300_create_dsa_state, r300_create_rs_state),
 * r300_state_inlines.h, r300_reg.h (R300_RB3D_*, R300_ZB_*, R300_SC_*,
 * R300_SU_*, R300_GA_*, R300_FG_*).
 *
 * Pure C, no QEMU dependencies.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef R5XX_RASTER_H
#define R5XX_RASTER_H

#include "r5xx_core.h"
#include "r5xx_tex.h"
#include "r500_us.h"

#define R5XX_MAX_FS_INPUTS  R500_US_MAX_INPUTS
#define R5XX_MAX_COLOR_BUFS 4

/* Raw register values the rasterizer decodes (copied from the register
 * file by the 3D state module at draw time). */
typedef struct R5xxRasterRegs {
    /* color buffers */
    uint32_t rb3d_cctl;                 /* 0x4E00 */
    uint32_t rb3d_blendcntl;            /* 0x4E04 */
    uint32_t rb3d_ablendcntl;           /* 0x4E08 */
    uint32_t rb3d_color_channel_mask;   /* 0x4E0C */
    uint32_t rb3d_blend_color;          /* 0x4E10 (R300 constant color) */
    uint32_t rb3d_ropcntl;              /* 0x4E18 */
    uint32_t rb3d_clrcmp_flipe, rb3d_clrcmp_clr, rb3d_clrcmp_msk; /* 0x4E1C.. */
    uint32_t rb3d_coloroffset[4];       /* 0x4E28 + 4n */
    uint32_t rb3d_colorpitch[4];        /* 0x4E38 + 4n */
    uint32_t rb3d_dither_ctl;           /* 0x4E50 */
    uint32_t rb3d_constant_color_ar;    /* 0x4EF8 (R500) */
    uint32_t rb3d_constant_color_gb;    /* 0x4EFC (R500) */
    /* depth / stencil */
    uint32_t zb_cntl;                   /* 0x4F00 */
    uint32_t zb_zstencilcntl;           /* 0x4F04 */
    uint32_t zb_stencilrefmask;         /* 0x4F08 */
    uint32_t zb_format;                 /* 0x4F10 */
    uint32_t zb_ztop;                   /* 0x4F14 */
    uint32_t zb_depthoffset;            /* 0x4F20 */
    uint32_t zb_depthpitch;             /* 0x4F24 */
    uint32_t zb_depthclearvalue;        /* 0x4F28 */
    uint32_t zb_stencilrefmask_bf;      /* 0x4FD4 (R500) */
    /* fog / alpha test */
    uint32_t fg_fog_blend;              /* 0x4BC0 */
    uint32_t fg_fog_factor;             /* 0x4BC4 */
    uint32_t fg_fog_color_r, fg_fog_color_g, fg_fog_color_b; /* 0x4BC8.. */
    uint32_t fg_alpha_func;             /* 0x4BD4 */
    uint32_t fg_depth_src;              /* 0x4BD8 */
    uint32_t fg_alpha_value;            /* 0x4BE0 (R500) */
    /* scissor / clip */
    uint32_t sc_scissors_tl, sc_scissors_br;   /* 0x43E0 / 0x43E4 */
    uint32_t sc_clip_rule;              /* 0x43D0 */
    uint32_t sc_clip_a[4], sc_clip_b[4];/* 0x43B0.. */
    uint32_t sc_edgerule;               /* 0x43A8 */
    /* setup */
    uint32_t su_cull_mode;              /* 0x42B8 */
    uint32_t su_poly_offset_front_scale, su_poly_offset_front_offset;
    uint32_t su_poly_offset_back_scale, su_poly_offset_back_offset;
    uint32_t su_poly_offset_enable;     /* 0x42B4 */
    uint32_t su_depth_scale, su_depth_offset;  /* 0x42C0 / 0x42C4 */
    uint32_t ga_poly_mode;              /* 0x4288 */
    uint32_t ga_color_control;          /* 0x4278 */
    uint32_t ga_point_size;             /* 0x421C */
    uint32_t ga_point_minmax;           /* 0x4230 */
    uint32_t ga_line_cntl;              /* 0x4234 */
    uint32_t ga_round_mode;             /* 0x428C */
    uint32_t ga_offset;                 /* 0x4290 */
    uint32_t ga_fog_scale, ga_fog_offset; /* 0x4294 / 0x4298 */
    uint32_t gb_tile_config;            /* 0x4018 */
    uint32_t gb_aa_config;              /* 0x4020 */
    uint32_t us_out_fmt[4];             /* 0x46A4.. output format/swizzle */
    bool     is_r500;
    /* added by the raster module (end of struct, see docs/raster_NOTES.md):
     * GB_SELECT 0x401C selects the fog coordinate source (bits 0-2:
     * 0-3 = alpha of color 0-3, 4 = 1/(1/W) i.e. W, 5 = Z).  Zero if the
     * 3D module does not track it. */
    uint32_t gb_select;
    /* RB3D_AARESOLVE_OFFSET/PITCH/CTL 0x4E80..0x4E88 */
    uint32_t rb3d_aaresolve_offset, rb3d_aaresolve_pitch, rb3d_aaresolve_ctl;
} R5xxRasterRegs;

/* Decoded render-target description. */
typedef struct R5xxRenderTarget {
    int num_cbufs;
    bool cbuf_enabled[R5XX_MAX_COLOR_BUFS];
    R5xxSurfaceLayout cbuf[R5XX_MAX_COLOR_BUFS];
    R5xxColorFormat cbuf_format[R5XX_MAX_COLOR_BUFS];
    bool zbuf_enabled;
    R5xxSurfaceLayout zbuf;
    bool z_is_16;             /* Z16 vs Z24 (+S8) */
    bool has_stencil;
    uint32_t max_width, max_height;  /* from colorpitch / scissor; clip */
} R5xxRenderTarget;

/* A vertex in window space. x, y in pixels (float, top-left origin as
 * produced by the VTE/viewport transform), z in [0,1] (or scaled per
 * SU_DEPTH_SCALE), w = clip-space w (> 0). attr[] are the fragment-shader
 * input vectors, already routed through the RS block. */
typedef struct R5xxVertex {
    float x, y, z, w;
    r5xx_vec4 attr[R5XX_MAX_FS_INPUTS];
} R5xxVertex;

typedef struct R5xxRaster {
    R5xxRasterRegs regs;
    R5xxRenderTarget rt;
    const R5xxMem *mem;
    R500UsExec us;                 /* fragment shader to run per quad */
    int num_fs_inputs;
    /* per-input interpolation: bit i set = flat (no perspective, use
     * provoking vertex); derived from GA_COLOR_CONTROL and RS setup */
    uint32_t flat_inputs;
    /* statistics */
    uint64_t pixels_in, pixels_out, quads;
    uint64_t zpass_count;          /* for occlusion queries */
    /* Derived state filled by r5xx_raster_setup() (added at the end of
     * the struct by the raster module; read-only for other modules). */
    struct R5xxRasterDerived {
        /* AA resolve draw: covered pixels copy color buffer 0 to resolve */
        bool aa_resolve;
        R5xxSurfaceLayout resolve;
        int subpix;                        /* 12 or 16 (GB_TILE_CONFIG) */
        int sc_x0, sc_y0, sc_x1, sc_y1;    /* scissor, inclusive */
        int cl_x0[4], cl_y0[4], cl_x1[4], cl_y1[4]; /* clip rects, incl. */
        uint32_t clip_rule;
        bool cull_front, cull_back, front_ccw;
        bool po_front, po_back;            /* polygon offset enables */
        float po_front_scale, po_front_offset, po_back_scale, po_back_offset;
        float depth_scale, depth_offset;   /* SU_DEPTH_SCALE / OFFSET */
        bool poly_mode_dual;
        int poly_front_type, poly_back_type; /* 0 point, 1 line, 2 tri */
        int provoking;                     /* 0,1,2 or 3 = last */
        float point_half_w, point_half_h;  /* pixels */
        float point_min, point_max;        /* GA_POINT_MINMAX, pixels */
        float line_half_w;                 /* pixels */
        int line_end_type;
        /* blending (raw hw codes, see r300_reg.h) */
        bool blend_enable, separate_alpha;
        int rgb_eq, rgb_src, rgb_dst, a_eq, a_src, a_dst;
        int discard_mode;
        r5xx_vec4 blend_const;
        uint32_t cmask[R5XX_MAX_COLOR_BUFS];
        bool cmask_independent;
        bool rop_enable;
        int rop;
        bool alpha_test;
        int alpha_func;                    /* R300_FG_ALPHA_FUNC_* code */
        float alpha_ref;
        bool z_enable, z_write;
        int z_func;                        /* R300_ZS_* */
        bool stencil_enable, stencil_two_sided, stencil_refmask_bf;
        int s_func[2], s_fail[2], s_zfail[2], s_zpass[2]; /* [front, back] */
        uint8_t s_ref[2], s_mask[2], s_wmask[2];
        bool ztop;
        bool fog_enable;
        int fog_mode, fog_select;
        float fog_scale, fog_offset, fog_factor;
        r5xx_vec4 fog_color;
        bool depth_from_shader;            /* FG_DEPTH_SRC */
        int num_multiwrites;               /* RB3D_CCTL, 1..4 */
        /* canonical channel c of buffer n is fed from shader output
         * channel chan_src[n][c] (US_OUT_FMT swizzle) */
        uint8_t chan_src[R5XX_MAX_COLOR_BUFS][4];
        bool cbuf_bgra[R5XX_MAX_COLOR_BUFS]; /* memory order BGRA vs RGBA */
        bool cbuf_float[R5XX_MAX_COLOR_BUFS];
    } d;
    /* polygon offset of the triangle being drawn, in [0,1] depth units;
     * kept apart from the float z, which cannot hold 1/2^24 near 0.5 */
    double prim_z_offset;
} R5xxRaster;

/* Point with a per-vertex size (diameter in pixels, clamped to
 * GA_POINT_MINMAX).  r5xx_raster_point() uses GA_POINT_SIZE instead.  The
 * 3D module calls this when the vertex program writes the point-size
 * output (VAP_OUTPUT_VTX_FMT_0 PSIZE). */
void r5xx_raster_point_sized(R5xxRaster *r, const R5xxVertex *v, float size);

/* Decode regs into rt / derived state; call after filling regs/mem/us. */
bool r5xx_raster_setup(R5xxRaster *r, char *err, size_t errlen);

/* PACKET3_3D_CLEAR_ZMASK: fill the depth buffer with ZB_DEPTHCLEARVALUE */
void r5xx_raster_zmask_clear(R5xxRaster *r, uint32_t ndw, uint32_t value);

void r5xx_raster_triangle(R5xxRaster *r, const R5xxVertex *v0,
                          const R5xxVertex *v1, const R5xxVertex *v2);
void r5xx_raster_line(R5xxRaster *r, const R5xxVertex *v0,
                      const R5xxVertex *v1);
void r5xx_raster_point(R5xxRaster *r, const R5xxVertex *v);

/* Direct color/depth buffer clears used by the CLEAR_* packets and the
 * replay tool.  rect is inclusive-exclusive. */
void r5xx_raster_clear(R5xxRaster *r, int x0, int y0, int x1, int y1,
                       bool clear_color, const r5xx_vec4 *color,
                       bool clear_depth, float depth,
                       bool clear_stencil, uint8_t stencil);

#endif /* R5XX_RASTER_H */
