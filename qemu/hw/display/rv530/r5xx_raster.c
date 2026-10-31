/*
 * CPU reference rasterizer for the R300/R500 3D pipeline.
 *
 * See r5xx_raster.h for the interface and docs/raster_NOTES.md for what
 * is exact, what is approximated and why.
 *
 * Register bit definitions below are copied from Mesa 24.0.9
 * src/gallium/drivers/r300/r300_reg.h (MIT license, Copyright 2005
 * Nicolai Haehnle et al.) so that no Mesa header is included.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "r5xx_raster.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Register bits (Mesa r300_reg.h)                                     */
/* ------------------------------------------------------------------ */

/* R300_GB_TILE_CONFIG 0x4018 */
#define GB_SUBPIXEL_1_16                    (1u << 16)

/* R300_RB3D_CCTL 0x4E00 */
#define RB3D_CCTL_NUM_MULTIWRITES_SHIFT     5
#define RB3D_CCTL_NUM_MULTIWRITES_MASK      3u
#define RB3D_CCTL_INDEPENDENT_CMASK         (1u << 12)

/* R300_RB3D_CBLEND 0x4E04 / R300_RB3D_ABLEND 0x4E08 */
#define CBLEND_ALPHA_BLEND_ENABLE           (1u << 0)
#define CBLEND_SEPARATE_ALPHA_ENABLE        (1u << 1)
#define CBLEND_READ_ENABLE                  (1u << 2)
#define CBLEND_DISCARD_SHIFT                3
#define CBLEND_DISCARD_MASK                 7u
#define DISCARD_NONE                        0
#define DISCARD_SRC_ALPHA_0                 1
#define DISCARD_SRC_COLOR_0                 2
#define DISCARD_SRC_ALPHA_COLOR_0           3
#define DISCARD_SRC_ALPHA_1                 4
#define DISCARD_SRC_COLOR_1                 5
#define DISCARD_SRC_ALPHA_COLOR_1           6
#define BLEND_FCN_SHIFT                     12
#define BLEND_FCN_MASK                      7u
#define COMB_FCN_ADD_CLAMP                  0
#define COMB_FCN_ADD_NOCLAMP                1
#define COMB_FCN_SUB_CLAMP                  2
#define COMB_FCN_SUB_NOCLAMP                3
#define COMB_FCN_MIN                        4
#define COMB_FCN_MAX                        5
#define COMB_FCN_RSUB_CLAMP                 6
#define COMB_FCN_RSUB_NOCLAMP               7
#define BLEND_GL_ZERO                       32
#define BLEND_GL_ONE                        33
#define BLEND_GL_SRC_COLOR                  34
#define BLEND_GL_ONE_MINUS_SRC_COLOR        35
#define BLEND_GL_DST_COLOR                  36
#define BLEND_GL_ONE_MINUS_DST_COLOR        37
#define BLEND_GL_SRC_ALPHA                  38
#define BLEND_GL_ONE_MINUS_SRC_ALPHA        39
#define BLEND_GL_DST_ALPHA                  40
#define BLEND_GL_ONE_MINUS_DST_ALPHA        41
#define BLEND_GL_SRC_ALPHA_SATURATE         42
#define BLEND_GL_CONST_COLOR                43
#define BLEND_GL_ONE_MINUS_CONST_COLOR      44
#define BLEND_GL_CONST_ALPHA                45
#define BLEND_GL_ONE_MINUS_CONST_ALPHA      46
#define BLEND_MASK                          63u
#define SRC_BLEND_SHIFT                     16
#define DST_BLEND_SHIFT                     24

/* R300_RB3D_ROPCNTL 0x4E18 */
#define ROPCNTL_ROP_ENABLE                  0x4u
#define ROPCNTL_ROP_SHIFT                   8
#define ROPCNTL_ROP_MASK                    15u

/* R300_RB3D_COLOROFFSETn / COLORPITCHn */
#define COLOROFFSET_MASK                    0xFFFFFFE0u
#define COLORPITCH_MASK                     0x00003FFEu
#define COLOR_TILE_ENABLE                   (1u << 16)
#define COLOR_MICROTILE_SHIFT               17
#define COLOR_MICROTILE_MASK                3u
#define COLOR_FORMAT_SHIFT                  21
#define COLOR_FORMAT_MASK                   15u

/* R300_US_OUT_FMT_n 0x46A4 */
#define US_OUT_FMT_MASK                     31u
#define US_OUT_FMT_C4_16                    5
#define US_OUT_FMT_UNUSED                   15
#define US_OUT_FMT_C4_16_FP                 18
#define US_OUT_FMT_C4_32_FP                 21
#define US_OUT_SEL_SHIFT                    8      /* C0..C3, 2 bits each */

/* R300_ZB_CNTL 0x4F00 */
#define ZB_STENCIL_ENABLE                   (1u << 0)
#define ZB_Z_ENABLE                         (1u << 1)
#define ZB_Z_WRITE_ENABLE                   (1u << 2)
#define ZB_STENCIL_FRONT_BACK               (1u << 4)
#define ZB_STENCIL_REFMASK_FRONT_BACK       (1u << 6)    /* R500 */

/* R300_ZB_ZSTENCILCNTL 0x4F04 */
#define ZS_NEVER        0
#define ZS_LESS         1
#define ZS_LEQUAL       2
#define ZS_EQUAL        3
#define ZS_GEQUAL       4
#define ZS_GREATER      5
#define ZS_NOTEQUAL     6
#define ZS_ALWAYS       7
#define ZS_KEEP         0
#define ZS_ZERO         1
#define ZS_REPLACE      2
#define ZS_INCR         3
#define ZS_DECR         4
#define ZS_INVERT       5
#define ZS_INCR_WRAP    6
#define ZS_DECR_WRAP    7
#define Z_FUNC_SHIFT            0
#define S_FRONT_FUNC_SHIFT      3
#define S_FRONT_SFAIL_OP_SHIFT  6
#define S_FRONT_ZPASS_OP_SHIFT  9
#define S_FRONT_ZFAIL_OP_SHIFT  12
#define S_BACK_FUNC_SHIFT       15
#define S_BACK_SFAIL_OP_SHIFT   18
#define S_BACK_ZPASS_OP_SHIFT   21
#define S_BACK_ZFAIL_OP_SHIFT   24

/* R300_ZB_STENCILREFMASK 0x4F08 (and R500 _BF 0x4FD4) */
#define STENCILREF_SHIFT        0
#define STENCILMASK_SHIFT       8
#define STENCILWRITEMASK_SHIFT  16

/* R300_ZB_FORMAT 0x4F10 */
#define DEPTHFORMAT_16BIT_INT_Z             0
#define DEPTHFORMAT_16BIT_13E3              1
#define DEPTHFORMAT_24BIT_INT_Z_8BIT_STENCIL 2
#define DEPTHFORMAT_MASK                    15u

/* R300_ZB_DEPTHPITCH 0x4F24 */
#define DEPTHPITCH_MASK                     0x00003FFCu
#define DEPTHMACROTILE_ENABLE               (1u << 16)
#define DEPTHMICROTILE_SHIFT                17
#define DEPTHMICROTILE_MASK                 3u

/* R300_ZB_ZTOP 0x4F14 */
#define ZTOP_ENABLE                         1u

/* R300_FG_FOG_BLEND 0x4BC0 */
#define FG_FOG_BLEND_ENABLE                 (1u << 0)
#define FG_FOG_BLEND_FN_SHIFT               1
#define FG_FOG_BLEND_FN_MASK                3u
#define FG_FOG_FN_LINEAR    0
#define FG_FOG_FN_EXP       1
#define FG_FOG_FN_EXP2      2
#define FG_FOG_FN_CONSTANT  3
#define FG_FOG_COLOR_MASK                   0x3FFu
#define FG_FOG_FACTOR_MASK                  0x3FFu

/* R300_FG_ALPHA_FUNC 0x4BD4 */
#define FG_ALPHA_FUNC_VAL_MASK              0xFFu
#define FG_ALPHA_FUNC_SHIFT                 8
#define FG_ALPHA_FUNC_MASK                  7u
#define FG_ALPHA_NEVER      0
#define FG_ALPHA_LESS       1
#define FG_ALPHA_EQUAL      2
#define FG_ALPHA_LE         3
#define FG_ALPHA_GREATER    4
#define FG_ALPHA_NOTEQUAL   5
#define FG_ALPHA_GE         6
#define FG_ALPHA_ALWAYS     7
#define FG_ALPHA_FUNC_ENABLE                (1u << 11)
#define FG_ALPHA_FUNC_8BIT                  (1u << 12)   /* R500 */
#define FG_ALPHA_FUNC_FP16_ENABLE           (1u << 28)   /* R500 */
#define FG_ALPHA_VALUE_MASK                 0xFFFFu

/* R300_FG_DEPTH_SRC 0x4BD8 */
#define FG_DEPTH_SRC_SHADER                 1u

/* R300_SC_SCISSORS_TL/BR, SC_CLIPRECT_TL/BR_n */
#define SC_COORD_MASK                       0x1FFFu
#define SC_Y_SHIFT                          13
#define SC_R300_OFFSET                      1440

/* R300_SU_CULL_MODE 0x42B8 */
#define CULL_FRONT                          (1u << 0)
#define CULL_BACK                           (1u << 1)
#define FRONT_FACE_CW                       (1u << 2)

/* R300_SU_POLY_OFFSET_ENABLE 0x42B4 */
#define PO_FRONT_ENABLE                     (1u << 0)
#define PO_BACK_ENABLE                      (1u << 1)

/* R300_GA_POLY_MODE 0x4288 */
#define GA_POLY_MODE_DUAL                   (1u << 0)
#define GA_POLY_MODE_FRONT_SHIFT            4
#define GA_POLY_MODE_BACK_SHIFT             7
#define GA_POLY_MODE_PTYPE_MASK             3u
#define PTYPE_POINT 0
#define PTYPE_LINE  1
#define PTYPE_TRI   2

/* R300_GA_COLOR_CONTROL 0x4278 */
#define GA_COLOR_CONTROL_PROVOKING_SHIFT    16
#define GA_COLOR_CONTROL_PROVOKING_MASK     3u

/* R300_GA_POINT_SIZE 0x421C, GA_POINT_MINMAX 0x4230, GA_LINE_CNTL 0x4234 */
#define GA_POINTSIZE_Y_SHIFT                0
#define GA_POINTSIZE_X_SHIFT                16
#define GA_POINT_MINMAX_MIN_SHIFT           0
#define GA_POINT_MINMAX_MAX_SHIFT           16
#define GA_LINE_CNTL_WIDTH_MASK             0xFFFFu
#define GA_LINE_CNTL_END_TYPE_SHIFT         16
#define GA_LINE_CNTL_END_TYPE_MASK          3u

/* R300_GB_SELECT 0x401C (linux r300_reg.h) */
#define GB_FOG_SELECT_MASK                  7u
#define GB_FOG_SELECT_1_1_W                 4
#define GB_FOG_SELECT_Z                     5

/* ------------------------------------------------------------------ */
/* Small helpers                                                       */
/* ------------------------------------------------------------------ */

static float u32_to_float(uint32_t u)
{
    float f;
    memcpy(&f, &u, 4);
    return f;
}

static int64_t floordiv64(int64_t a, int64_t b)
{
    int64_t q = a / b;
    if ((a % b != 0) && ((a < 0) != (b < 0))) {
        q--;
    }
    return q;
}

static int imin(int a, int b) { return a < b ? a : b; }
static int imax(int a, int b) { return a > b ? a : b; }
static float fminf_(float a, float b) { return a < b ? a : b; }
static float fmaxf_(float a, float b) { return a > b ? a : b; }

/* Generic comparison on the R300_ZS_* function encoding. */
static bool zs_compare(int func, double a, double b)
{
    switch (func & 7) {
    case ZS_NEVER:    return false;
    case ZS_LESS:     return a < b;
    case ZS_LEQUAL:   return a <= b;
    case ZS_EQUAL:    return a == b;
    case ZS_GEQUAL:   return a >= b;
    case ZS_GREATER:  return a > b;
    case ZS_NOTEQUAL: return a != b;
    default:          return true;
    }
}

/* FG_ALPHA_FUNC uses a different encoding of the same eight functions. */
static bool alpha_compare(int func, float a, float ref)
{
    switch (func & 7) {
    case FG_ALPHA_NEVER:    return false;
    case FG_ALPHA_LESS:     return a < ref;
    case FG_ALPHA_EQUAL:    return a == ref;
    case FG_ALPHA_LE:       return a <= ref;
    case FG_ALPHA_GREATER:  return a > ref;
    case FG_ALPHA_NOTEQUAL: return a != ref;
    case FG_ALPHA_GE:       return a >= ref;
    default:                return true;
    }
}

static uint8_t stencil_op(int op, uint8_t s, uint8_t ref)
{
    switch (op & 7) {
    case ZS_KEEP:      return s;
    case ZS_ZERO:      return 0;
    case ZS_REPLACE:   return ref;
    case ZS_INCR:      return s == 255 ? 255 : s + 1;
    case ZS_DECR:      return s == 0 ? 0 : s - 1;
    case ZS_INVERT:    return ~s;
    case ZS_INCR_WRAP: return (uint8_t)(s + 1);
    default:           return (uint8_t)(s - 1);
    }
}

/* ------------------------------------------------------------------ */
/* Memory access                                                       */
/* ------------------------------------------------------------------ */

static void surf_read(const R5xxRaster *r, const R5xxSurfaceLayout *l,
                      uint32_t x, uint32_t y, void *buf)
{
    const R5xxMem *m = r->mem;
    uint32_t addr = l->gpu_addr + r5xx_surface_pixel_offset(l, x, y);
    uint8_t *p = m->map ? m->map(m->opaque, addr, l->bpp, false) : NULL;
    if (p) {
        memcpy(buf, p, l->bpp);
    } else {
        m->read(m->opaque, addr, buf, l->bpp);
    }
}

static void surf_write(const R5xxRaster *r, const R5xxSurfaceLayout *l,
                       uint32_t x, uint32_t y, const void *buf)
{
    const R5xxMem *m = r->mem;
    uint32_t addr = l->gpu_addr + r5xx_surface_pixel_offset(l, x, y);
    uint8_t *p = m->map ? m->map(m->opaque, addr, l->bpp, true) : NULL;
    if (p) {
        memcpy(p, buf, l->bpp);
    } else {
        m->write(m->opaque, addr, buf, l->bpp);
    }
}

/* Depth/stencil layout (Mesa: PIPE_FORMAT_S8_UINT_Z24_UNORM, util_pack_z):
 * Z24S8: depth in bits 8..31, stencil in bits 0..7.  Z16: 16-bit depth.
 * Values are little-endian in memory. */
static void zs_read(const R5xxRaster *r, uint32_t x, uint32_t y,
                    uint32_t *z, uint8_t *s)
{
    uint8_t buf[4] = { 0, 0, 0, 0 };
    surf_read(r, &r->rt.zbuf, x, y, buf);
    if (r->rt.z_is_16) {
        *z = buf[0] | (buf[1] << 8);
        *s = 0;
    } else {
        uint32_t v = buf[0] | (buf[1] << 8) | (buf[2] << 16) |
                     ((uint32_t)buf[3] << 24);
        *z = v >> 8;
        *s = v & 0xff;
    }
}

static void zs_write(const R5xxRaster *r, uint32_t x, uint32_t y,
                     uint32_t z, uint8_t s)
{
    uint8_t buf[4];
    if (r->rt.z_is_16) {
        buf[0] = z & 0xff;
        buf[1] = (z >> 8) & 0xff;
    } else {
        uint32_t v = (z << 8) | s;
        buf[0] = v & 0xff;
        buf[1] = (v >> 8) & 0xff;
        buf[2] = (v >> 16) & 0xff;
        buf[3] = v >> 24;
    }
    surf_write(r, &r->rt.zbuf, x, y, buf);
}

/* Float depth -> fixed point depth value of the bound buffer.  The hw
 * works in 24-bit fixed point (SU_DEPTH_SCALE, Mesa programs 2^24-1);
 * a 16-bit buffer stores the top 16 bits. */
static uint32_t depth_to_fixed(const R5xxRaster *r, float z)
{
    double zf = ((double)z + r->prim_z_offset) * r->d.depth_scale +
                r->d.depth_offset;
    if (r->rt.z_is_16) {
        zf = zf / 256.0;
        if (zf < 0) zf = 0;
        if (zf > 65535.0) zf = 65535.0;
    } else {
        if (zf < 0) zf = 0;
        if (zf > 16777215.0) zf = 16777215.0;
    }
    return (uint32_t)(zf + 0.5);
}

/* ------------------------------------------------------------------ */
/* Setup: decode the registers                                         */
/* ------------------------------------------------------------------ */

static R5xxTiling decode_tiling(bool macro, unsigned micro)
{
    if (macro) {
        return micro ? R5XX_TILE_MICRO_MACRO : R5XX_TILE_MACRO;
    }
    return micro ? R5XX_TILE_MICRO : R5XX_TILE_LINEAR;
}

/*
 * Order of the canonical channels in memory, lowest address first, as
 * r5xx_color_pack() (r5xx_tex.c) and color_pack_wide() (GLSL) store them:
 * B, G, R, A for every color format, the 16- and 32-bit-per-channel ones
 * included.  US_OUT_FMT's Cp_SEL then puts the selected shader channel in
 * memory component p, as on the hardware (Mesa programs R, G, B, A selects
 * for R16G16B16A16_FLOAT, i.e. red at the lowest address).  The wide
 * formats were once mapped as R, G, B, A here, which matched only the
 * raster test's stub pack and swapped red and blue on real draws (Core
 * Image's half-float intermediates: blue ladybug in Fun House).
 */
static bool format_is_bgra(R5xxColorFormat f)
{
    return true;
}

static bool format_is_float(R5xxColorFormat f)
{
    return f == R5XX_COLOR_ARGB16161616F || f == R5XX_COLOR_ARGB32323232F;
}

static void decode_rect(uint32_t tl, uint32_t br, int off,
                        int *x0, int *y0, int *x1, int *y1)
{
    *x0 = (int)(tl & SC_COORD_MASK) - off;
    *y0 = (int)((tl >> SC_Y_SHIFT) & SC_COORD_MASK) - off;
    *x1 = (int)(br & SC_COORD_MASK) - off;
    *y1 = (int)((br >> SC_Y_SHIFT) & SC_COORD_MASK) - off;
}

static bool setup_cbuf(R5xxRaster *r, int n, char *err, size_t errlen)
{
    const R5xxRasterRegs *g = &r->regs;
    struct R5xxRasterDerived *d = &r->d;
    uint32_t pitchreg = g->rb3d_colorpitch[n];
    uint32_t pitch = pitchreg & COLORPITCH_MASK;
    R5xxColorFormat fmt;
    uint32_t bpp, out_fmt, type;
    R5xxSurfaceLayout *l = &r->rt.cbuf[n];
    unsigned micro;
    int c;

    if (!pitch) {
        snprintf(err, errlen, "color buffer %d: zero pitch", n);
        return false;
    }
    bpp = r5xx_color_format_from_hw(pitchreg, g->is_r500, &fmt);
    if (!bpp) {
        snprintf(err, errlen, "color buffer %d: unsupported format %u", n,
                 (pitchreg >> COLOR_FORMAT_SHIFT) & COLOR_FORMAT_MASK);
        return false;
    }

    /* US_OUT_FMT refines the 16/32-bit formats (float vs integer) and
     * carries the shader -> memory swizzle.  With multiwrite replication
     * the extra buffers are marked UNUSED and take buffer 0's swizzle. */
    out_fmt = g->us_out_fmt[n];
    if (n > 0 && d->num_multiwrites > 1) {
        out_fmt = g->us_out_fmt[0];
    }
    type = out_fmt & US_OUT_FMT_MASK;
    if (fmt == R5XX_COLOR_ARGB16161616 && type == US_OUT_FMT_C4_16_FP) {
        fmt = R5XX_COLOR_ARGB16161616F;
    }
    if (fmt == R5XX_COLOR_ARGB16161616F && type == US_OUT_FMT_C4_16) {
        fmt = R5XX_COLOR_ARGB16161616;
    }
    bpp = (fmt == R5XX_COLOR_ARGB16161616F) ? 8 : bpp;

    memset(l, 0, sizeof(*l));
    l->gpu_addr = g->rb3d_coloroffset[n] & COLOROFFSET_MASK;
    l->pitch_bytes = pitch * bpp;
    l->width = pitch;
    l->height = r->rt.max_height;
    l->bpp = bpp;
    l->compressed = false;
    micro = (pitchreg >> COLOR_MICROTILE_SHIFT) & COLOR_MICROTILE_MASK;
    l->tiling = decode_tiling(pitchreg & COLOR_TILE_ENABLE, micro);
    l->micro_mode = (uint8_t)micro;
    l->is_r500 = g->is_r500;
    r->rt.cbuf_format[n] = fmt;
    d->cbuf_bgra[n] = format_is_bgra(fmt);
    d->cbuf_float[n] = format_is_float(fmt);

    /* Swizzle: Cp_SEL (0 = A, 1 = R, 2 = G, 3 = B) names the shader
     * channel stored in memory component p.  Memory component p holds
     * canonical channel pos[p]. */
    {
        static const uint8_t sel_to_chan[4] = { 3, 0, 1, 2 };
        static const uint8_t pos_bgra[4] = { 2, 1, 0, 3 };
        static const uint8_t pos_rgba[4] = { 0, 1, 2, 3 };
        const uint8_t *pos = d->cbuf_bgra[n] ? pos_bgra : pos_rgba;
        for (c = 0; c < 4; c++) {
            d->chan_src[n][c] = (uint8_t)c;          /* identity default */
        }
        if (out_fmt != 0) {
            int p;
            for (p = 0; p < 4; p++) {
                unsigned sel = (out_fmt >> (US_OUT_SEL_SHIFT + 2 * p)) & 3;
                d->chan_src[n][pos[p]] = sel_to_chan[sel];
            }
        }
    }
    return true;
}

bool r5xx_raster_setup(R5xxRaster *r, char *err, size_t errlen)
{
    const R5xxRasterRegs *g = &r->regs;
    struct R5xxRasterDerived *d = &r->d;
    int off = g->is_r500 ? 0 : SC_R300_OFFSET;
    int n;

    if (errlen) {
        err[0] = 0;
    }
    memset(d, 0, sizeof(*d));
    memset(&r->rt, 0, sizeof(r->rt));

    d->subpix = (g->gb_tile_config & GB_SUBPIXEL_1_16) ? 16 : 12;

    /* --- scissor and clip rectangles --- */
    decode_rect(g->sc_scissors_tl, g->sc_scissors_br, off,
                &d->sc_x0, &d->sc_y0, &d->sc_x1, &d->sc_y1);
    if (d->sc_x0 < 0 || d->sc_y0 < 0) {
        r5xx_logf(r->mem, 1, "raster: scissor TL below origin (%d,%d), "
                  "clamped", d->sc_x0, d->sc_y0);
        d->sc_x0 = imax(d->sc_x0, 0);
        d->sc_y0 = imax(d->sc_y0, 0);
    }
    for (n = 0; n < 4; n++) {
        decode_rect(g->sc_clip_a[n], g->sc_clip_b[n], off,
                    &d->cl_x0[n], &d->cl_y0[n], &d->cl_x1[n], &d->cl_y1[n]);
    }
    d->clip_rule = g->sc_clip_rule & 0xffff;
    r->rt.max_width = (uint32_t)imax(d->sc_x1 + 1, 0);
    r->rt.max_height = (uint32_t)imax(d->sc_y1 + 1, 0);

    /* --- setup unit --- */
    d->cull_front = !!(g->su_cull_mode & CULL_FRONT);
    d->cull_back = !!(g->su_cull_mode & CULL_BACK);
    d->front_ccw = !(g->su_cull_mode & FRONT_FACE_CW);
    d->po_front = !!(g->su_poly_offset_enable & PO_FRONT_ENABLE);
    d->po_back = !!(g->su_poly_offset_enable & PO_BACK_ENABLE);
    d->po_front_scale = u32_to_float(g->su_poly_offset_front_scale);
    d->po_front_offset = u32_to_float(g->su_poly_offset_front_offset);
    d->po_back_scale = u32_to_float(g->su_poly_offset_back_scale);
    d->po_back_offset = u32_to_float(g->su_poly_offset_back_offset);
    d->depth_scale = u32_to_float(g->su_depth_scale);
    d->depth_offset = u32_to_float(g->su_depth_offset);
    if (d->depth_scale == 0.0f) {
        /* Never programmed: assume Mesa's default (2^24 - 1). */
        d->depth_scale = 16777215.0f;
    }

    /* --- geometry assembly --- */
    d->poly_mode_dual = !!(g->ga_poly_mode & GA_POLY_MODE_DUAL);
    d->poly_front_type = (g->ga_poly_mode >> GA_POLY_MODE_FRONT_SHIFT) &
                         GA_POLY_MODE_PTYPE_MASK;
    d->poly_back_type = (g->ga_poly_mode >> GA_POLY_MODE_BACK_SHIFT) &
                        GA_POLY_MODE_PTYPE_MASK;
    d->provoking = (g->ga_color_control >> GA_COLOR_CONTROL_PROVOKING_SHIFT) &
                   GA_COLOR_CONTROL_PROVOKING_MASK;
    /* GA_POINT_SIZE / GA_LINE_CNTL hold the half size in subpixels. */
    d->point_half_w = (float)((g->ga_point_size >> GA_POINTSIZE_X_SHIFT) &
                              0xffff) / d->subpix;
    d->point_half_h = (float)((g->ga_point_size >> GA_POINTSIZE_Y_SHIFT) &
                              0xffff) / d->subpix;
    d->point_min = (float)((g->ga_point_minmax >> GA_POINT_MINMAX_MIN_SHIFT) &
                           0xffff) * 2.0f / d->subpix;
    d->point_max = (float)((g->ga_point_minmax >> GA_POINT_MINMAX_MAX_SHIFT) &
                           0xffff) * 2.0f / d->subpix;
    d->line_half_w = (float)(g->ga_line_cntl & GA_LINE_CNTL_WIDTH_MASK) /
                     d->subpix;
    d->line_end_type = (g->ga_line_cntl >> GA_LINE_CNTL_END_TYPE_SHIFT) &
                       GA_LINE_CNTL_END_TYPE_MASK;

    /* --- blending --- */
    d->blend_enable = !!(g->rb3d_blendcntl & CBLEND_ALPHA_BLEND_ENABLE);
    d->separate_alpha = !!(g->rb3d_blendcntl & CBLEND_SEPARATE_ALPHA_ENABLE);
    d->rgb_eq = (g->rb3d_blendcntl >> BLEND_FCN_SHIFT) & BLEND_FCN_MASK;
    d->rgb_src = (g->rb3d_blendcntl >> SRC_BLEND_SHIFT) & BLEND_MASK;
    d->rgb_dst = (g->rb3d_blendcntl >> DST_BLEND_SHIFT) & BLEND_MASK;
    if (d->separate_alpha) {
        d->a_eq = (g->rb3d_ablendcntl >> BLEND_FCN_SHIFT) & BLEND_FCN_MASK;
        d->a_src = (g->rb3d_ablendcntl >> SRC_BLEND_SHIFT) & BLEND_MASK;
        d->a_dst = (g->rb3d_ablendcntl >> DST_BLEND_SHIFT) & BLEND_MASK;
    } else {
        d->a_eq = d->rgb_eq;
        d->a_src = d->rgb_src;
        d->a_dst = d->rgb_dst;
    }
    d->discard_mode = (g->rb3d_blendcntl >> CBLEND_DISCARD_SHIFT) &
                      CBLEND_DISCARD_MASK;
    d->rop_enable = !!(g->rb3d_ropcntl & ROPCNTL_ROP_ENABLE);
    d->rop = (g->rb3d_ropcntl >> ROPCNTL_ROP_SHIFT) & ROPCNTL_ROP_MASK;
    d->cmask_independent = !!(g->rb3d_cctl & RB3D_CCTL_INDEPENDENT_CMASK);
    for (n = 0; n < R5XX_MAX_COLOR_BUFS; n++) {
        d->cmask[n] = (g->rb3d_color_channel_mask >> (4 * n)) & 0xf;
    }
    d->num_multiwrites = ((g->rb3d_cctl >> RB3D_CCTL_NUM_MULTIWRITES_SHIFT) &
                          RB3D_CCTL_NUM_MULTIWRITES_MASK) + 1;

    /* --- alpha test --- */
    d->alpha_test = !!(g->fg_alpha_func & FG_ALPHA_FUNC_ENABLE);
    d->alpha_func = (g->fg_alpha_func >> FG_ALPHA_FUNC_SHIFT) &
                    FG_ALPHA_FUNC_MASK;
    if (!g->is_r500 || (g->fg_alpha_func & FG_ALPHA_FUNC_8BIT)) {
        d->alpha_ref = (float)(g->fg_alpha_func & FG_ALPHA_FUNC_VAL_MASK) /
                       255.0f;
    } else if (g->fg_alpha_func & FG_ALPHA_FUNC_FP16_ENABLE) {
        d->alpha_ref = r5xx_half_to_float(g->fg_alpha_value &
                                          FG_ALPHA_VALUE_MASK);
    } else {
        d->alpha_ref = (float)(g->fg_alpha_value & 0x3ff) / 1023.0f;
    }
    d->depth_from_shader = !!(g->fg_depth_src & FG_DEPTH_SRC_SHADER);

    /* --- depth / stencil --- */
    d->z_enable = !!(g->zb_cntl & ZB_Z_ENABLE);
    d->z_write = !!(g->zb_cntl & ZB_Z_WRITE_ENABLE);
    d->z_func = (g->zb_zstencilcntl >> Z_FUNC_SHIFT) & 7;
    d->stencil_enable = !!(g->zb_cntl & ZB_STENCIL_ENABLE);
    d->stencil_two_sided = !!(g->zb_cntl & ZB_STENCIL_FRONT_BACK);
    d->stencil_refmask_bf = g->is_r500 &&
                            !!(g->zb_cntl & ZB_STENCIL_REFMASK_FRONT_BACK);
    d->s_func[0] = (g->zb_zstencilcntl >> S_FRONT_FUNC_SHIFT) & 7;
    d->s_fail[0] = (g->zb_zstencilcntl >> S_FRONT_SFAIL_OP_SHIFT) & 7;
    d->s_zpass[0] = (g->zb_zstencilcntl >> S_FRONT_ZPASS_OP_SHIFT) & 7;
    d->s_zfail[0] = (g->zb_zstencilcntl >> S_FRONT_ZFAIL_OP_SHIFT) & 7;
    if (d->stencil_two_sided) {
        d->s_func[1] = (g->zb_zstencilcntl >> S_BACK_FUNC_SHIFT) & 7;
        d->s_fail[1] = (g->zb_zstencilcntl >> S_BACK_SFAIL_OP_SHIFT) & 7;
        d->s_zpass[1] = (g->zb_zstencilcntl >> S_BACK_ZPASS_OP_SHIFT) & 7;
        d->s_zfail[1] = (g->zb_zstencilcntl >> S_BACK_ZFAIL_OP_SHIFT) & 7;
    } else {
        d->s_func[1] = d->s_func[0];
        d->s_fail[1] = d->s_fail[0];
        d->s_zpass[1] = d->s_zpass[0];
        d->s_zfail[1] = d->s_zfail[0];
    }
    d->s_ref[0] = (g->zb_stencilrefmask >> STENCILREF_SHIFT) & 0xff;
    d->s_mask[0] = (g->zb_stencilrefmask >> STENCILMASK_SHIFT) & 0xff;
    d->s_wmask[0] = (g->zb_stencilrefmask >> STENCILWRITEMASK_SHIFT) & 0xff;
    if (d->stencil_refmask_bf) {
        d->s_ref[1] = (g->zb_stencilrefmask_bf >> STENCILREF_SHIFT) & 0xff;
        d->s_mask[1] = (g->zb_stencilrefmask_bf >> STENCILMASK_SHIFT) & 0xff;
        d->s_wmask[1] = (g->zb_stencilrefmask_bf >> STENCILWRITEMASK_SHIFT) &
                        0xff;
    } else {
        d->s_ref[1] = d->s_ref[0];
        d->s_mask[1] = d->s_mask[0];
        d->s_wmask[1] = d->s_wmask[0];
    }
    d->ztop = !!(g->zb_ztop & ZTOP_ENABLE);

    /* --- fog --- */
    d->fog_enable = !!(g->fg_fog_blend & FG_FOG_BLEND_ENABLE);
    d->fog_mode = (g->fg_fog_blend >> FG_FOG_BLEND_FN_SHIFT) &
                  FG_FOG_BLEND_FN_MASK;
    d->fog_scale = u32_to_float(g->ga_fog_scale);
    d->fog_offset = u32_to_float(g->ga_fog_offset);
    d->fog_factor = (float)(g->fg_fog_factor & FG_FOG_FACTOR_MASK) / 1023.0f;
    d->fog_color = r5xx_vec4_make(
        (float)(g->fg_fog_color_r & FG_FOG_COLOR_MASK) / 1023.0f,
        (float)(g->fg_fog_color_g & FG_FOG_COLOR_MASK) / 1023.0f,
        (float)(g->fg_fog_color_b & FG_FOG_COLOR_MASK) / 1023.0f, 1.0f);
    d->fog_select = g->gb_select & GB_FOG_SELECT_MASK;

    /* --- color buffers --- */
    r->rt.num_cbufs = 0;
    for (n = 0; n < R5XX_MAX_COLOR_BUFS; n++) {
        bool enabled;
        if (d->num_multiwrites > 1) {
            enabled = n < d->num_multiwrites;
        } else if (n == 0) {
            enabled = (g->us_out_fmt[0] & US_OUT_FMT_MASK) != US_OUT_FMT_UNUSED;
        } else {
            enabled = g->us_out_fmt[n] != 0 &&
                      (g->us_out_fmt[n] & US_OUT_FMT_MASK) != US_OUT_FMT_UNUSED;
        }
        if (enabled && !(g->rb3d_colorpitch[n] & COLORPITCH_MASK)) {
            enabled = false;    /* nothing bound */
        }
        r->rt.cbuf_enabled[n] = enabled;
        if (enabled) {
            if (!setup_cbuf(r, n, err, errlen)) {
                return false;
            }
            r->rt.num_cbufs = n + 1;
        }
    }

    /* --- blend constant (depends on cbuf 0 format on R500) --- */
    if (g->is_r500) {
        uint32_t ar = g->rb3d_constant_color_ar, gb = g->rb3d_constant_color_gb;
        if (r->rt.cbuf_enabled[0] &&
            r->rt.cbuf_format[0] == R5XX_COLOR_ARGB16161616F) {
            d->blend_const = r5xx_vec4_make(r5xx_half_to_float(ar & 0xffff),
                                            r5xx_half_to_float(gb >> 16),
                                            r5xx_half_to_float(gb & 0xffff),
                                            r5xx_half_to_float(ar >> 16));
        } else {
            d->blend_const = r5xx_vec4_make((ar & 0x3ff) / 1023.0f,
                                            ((gb >> 16) & 0x3ff) / 1023.0f,
                                            (gb & 0x3ff) / 1023.0f,
                                            ((ar >> 16) & 0x3ff) / 1023.0f);
        }
    } else {
        r5xx_unpack_argb8888(g->rb3d_blend_color, &d->blend_const);
    }

    /* --- depth buffer --- */
    r->rt.zbuf_enabled = false;
    {
        uint32_t pitch = g->zb_depthpitch & DEPTHPITCH_MASK;
        uint32_t fmt = g->zb_format & DEPTHFORMAT_MASK;
        if (pitch) {
            R5xxSurfaceLayout *l = &r->rt.zbuf;
            unsigned micro;
            if (fmt == DEPTHFORMAT_16BIT_INT_Z) {
                r->rt.z_is_16 = true;
                r->rt.has_stencil = false;
            } else if (fmt == DEPTHFORMAT_24BIT_INT_Z_8BIT_STENCIL) {
                r->rt.z_is_16 = false;
                r->rt.has_stencil = true;
            } else {
                snprintf(err, errlen, "unsupported ZB_FORMAT %u", fmt);
                return false;
            }
            memset(l, 0, sizeof(*l));
            l->gpu_addr = g->zb_depthoffset & COLOROFFSET_MASK;
            l->bpp = r->rt.z_is_16 ? 2 : 4;
            l->pitch_bytes = pitch * l->bpp;
            l->width = pitch;
            l->height = r->rt.max_height;
            micro = (g->zb_depthpitch >> DEPTHMICROTILE_SHIFT) &
                    DEPTHMICROTILE_MASK;
            l->tiling = decode_tiling(g->zb_depthpitch & DEPTHMACROTILE_ENABLE,
                                      micro);
            l->micro_mode = (uint8_t)micro;
            l->is_r500 = g->is_r500;
            r->rt.zbuf_enabled = true;
        } else if (d->z_enable || d->stencil_enable) {
            r5xx_logf(r->mem, 1, "raster: depth/stencil test enabled "
                      "without a depth buffer (ZB_DEPTHPITCH = 0)");
        }
    }

    /*
     * Multisampling is emulated with one sample at the pixel center: an
     * MSAA color buffer is rendered like a plain one (the hardware's own
     * layout of the AA buffer is not visible to the driver, which only
     * renders into it and resolves it).  A resolve is a draw with
     * AARESOLVE_CTL bit 0 set and the AA buffer bound as color buffer 0;
     * COLORPITCH then also describes the resolve target's tiling (Mesa
     * r300_simple_msaa_resolve).  Covered pixels are copied, not shaded.
     */
    d->aa_resolve = (g->rb3d_aaresolve_ctl & 1) && r->rt.cbuf_enabled[0];
    if (d->aa_resolve) {
        uint32_t px = g->rb3d_aaresolve_pitch & COLORPITCH_MASK;

        d->resolve = r->rt.cbuf[0];
        d->resolve.gpu_addr = g->rb3d_aaresolve_offset & ~0x1fu;
        d->resolve.pitch_bytes = px * d->resolve.bpp;
        d->resolve.width = px;
        if (!px) {
            snprintf(err, errlen, "AA resolve: zero pitch");
            return false;
        }
    } else if (g->gb_aa_config & 1) {
        r5xx_logf(r->mem, 2, "raster: GB_AA_CONFIG multisampling as 1 sample");
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* Per-pixel back end                                                  */
/* ------------------------------------------------------------------ */

static bool pixel_visible(const R5xxRaster *r, int x, int y)
{
    const struct R5xxRasterDerived *d = &r->d;
    unsigned code = 0;
    int i;

    if (x < d->sc_x0 || x > d->sc_x1 || y < d->sc_y0 || y > d->sc_y1) {
        return false;
    }
    for (i = 0; i < 4; i++) {
        if (x >= d->cl_x0[i] && x <= d->cl_x1[i] &&
            y >= d->cl_y0[i] && y <= d->cl_y1[i]) {
            code |= 1u << i;
        }
    }
    return (d->clip_rule >> code) & 1;
}

static float blend_factor(int code, int ch, const r5xx_vec4 *s,
                          const r5xx_vec4 *dst, const r5xx_vec4 *k)
{
    switch (code) {
    case BLEND_GL_ZERO:                  return 0.0f;
    case BLEND_GL_ONE:                   return 1.0f;
    case BLEND_GL_SRC_COLOR:             return s->f[ch];
    case BLEND_GL_ONE_MINUS_SRC_COLOR:   return 1.0f - s->f[ch];
    case BLEND_GL_DST_COLOR:             return dst->f[ch];
    case BLEND_GL_ONE_MINUS_DST_COLOR:   return 1.0f - dst->f[ch];
    case BLEND_GL_SRC_ALPHA:             return s->f[3];
    case BLEND_GL_ONE_MINUS_SRC_ALPHA:   return 1.0f - s->f[3];
    case BLEND_GL_DST_ALPHA:             return dst->f[3];
    case BLEND_GL_ONE_MINUS_DST_ALPHA:   return 1.0f - dst->f[3];
    case BLEND_GL_SRC_ALPHA_SATURATE:
        return ch < 3 ? fminf_(s->f[3], 1.0f - dst->f[3]) : 1.0f;
    case BLEND_GL_CONST_COLOR:           return k->f[ch];
    case BLEND_GL_ONE_MINUS_CONST_COLOR: return 1.0f - k->f[ch];
    case BLEND_GL_CONST_ALPHA:           return k->f[3];
    case BLEND_GL_ONE_MINUS_CONST_ALPHA: return 1.0f - k->f[3];
    default:                             return 1.0f;
    }
}

static float blend_channel(int eq, int fs, int fd, int ch, const r5xx_vec4 *s,
                           const r5xx_vec4 *dst, const r5xx_vec4 *k,
                           bool is_float)
{
    float a = s->f[ch], b = dst->f[ch], v;
    if (eq == COMB_FCN_MIN) {
        return fminf_(a, b);
    }
    if (eq == COMB_FCN_MAX) {
        return fmaxf_(a, b);
    }
    a *= blend_factor(fs, ch, s, dst, k);
    b *= blend_factor(fd, ch, s, dst, k);
    switch (eq) {
    case COMB_FCN_SUB_CLAMP:
    case COMB_FCN_SUB_NOCLAMP:
        v = a - b;
        break;
    case COMB_FCN_RSUB_CLAMP:
    case COMB_FCN_RSUB_NOCLAMP:
        v = b - a;
        break;
    default:
        v = a + b;
        break;
    }
    if (is_float && !(eq & 1)) {
        v = r5xx_clampf(v, 0.0f, 1.0f);
    }
    return v;
}

static uint32_t rop_apply(int rop, uint32_t s, uint32_t d)
{
    uint32_t v = 0;
    if (rop & 1) v |= ~s & ~d;
    if (rop & 2) v |= ~s & d;
    if (rop & 4) v |= s & ~d;
    if (rop & 8) v |= s & d;
    return v;
}

static bool discard_pixel(int mode, const r5xx_vec4 *s)
{
    bool rgb0 = s->f[0] == 0.0f && s->f[1] == 0.0f && s->f[2] == 0.0f;
    bool rgb1 = s->f[0] >= 1.0f && s->f[1] >= 1.0f && s->f[2] >= 1.0f;
    bool a0 = s->f[3] == 0.0f, a1 = s->f[3] >= 1.0f;
    switch (mode) {
    case DISCARD_SRC_ALPHA_0:        return a0;
    case DISCARD_SRC_COLOR_0:        return rgb0;
    case DISCARD_SRC_ALPHA_COLOR_0:  return a0 && rgb0;
    case DISCARD_SRC_ALPHA_1:        return a1;
    case DISCARD_SRC_COLOR_1:        return rgb1;
    case DISCARD_SRC_ALPHA_COLOR_1:  return a1 && rgb1;
    default:                         return false;
    }
}

/* Blend / rop / mask and write one color buffer.  'shader' is the raw
 * shader output for this target. */
static bool write_color(R5xxRaster *r, int n, int x, int y,
                        const r5xx_vec4 *shader)
{
    const struct R5xxRasterDerived *d = &r->d;
    const R5xxSurfaceLayout *l = &r->rt.cbuf[n];
    R5xxColorFormat fmt = r->rt.cbuf_format[n];
    uint32_t hwmask = d->cmask_independent ? d->cmask[n] : d->cmask[0];
    static const uint8_t pos_bgra[4] = { 2, 1, 0, 3 };
    static const uint8_t pos_rgba[4] = { 0, 1, 2, 3 };
    const uint8_t *pos = d->cbuf_bgra[n] ? pos_bgra : pos_rgba;
    unsigned chan_mask = 0;
    r5xx_vec4 src, dst, res;
    uint8_t buf[16], dstbuf[16];
    bool need_dst;
    int c;

    for (c = 0; c < 4; c++) {
        if ((hwmask >> c) & 1) {
            chan_mask |= 1u << pos[c];
        }
    }
    if (!chan_mask) {
        return false;
    }
    for (c = 0; c < 4; c++) {
        src.f[c] = shader->f[d->chan_src[n][c]];
    }
    if (d->blend_enable && discard_pixel(d->discard_mode, &src)) {
        return false;
    }

    need_dst = d->blend_enable || d->rop_enable || chan_mask != 0xf;
    if (need_dst) {
        surf_read(r, l, x, y, dstbuf);
        r5xx_color_unpack(fmt, dstbuf, &dst);
    } else {
        memset(dstbuf, 0, sizeof(dstbuf));
        dst = r5xx_vec4_make(0, 0, 0, 1);
    }

    res = src;
    if (d->rop_enable && !d->cbuf_float[n] && l->bpp <= 4) {
        /* Logic ops work on the packed pixel bits; blending is bypassed. */
        uint32_t sbits = 0, dbits = 0, v;
        unsigned i;
        r5xx_color_pack(fmt, &src, buf);
        for (i = 0; i < l->bpp; i++) {
            sbits |= (uint32_t)buf[i] << (8 * i);
            dbits |= (uint32_t)dstbuf[i] << (8 * i);
        }
        v = rop_apply(d->rop, sbits, dbits);
        for (i = 0; i < l->bpp; i++) {
            buf[i] = (v >> (8 * i)) & 0xff;
        }
        r5xx_color_unpack(fmt, buf, &res);
    } else if (d->blend_enable) {
        for (c = 0; c < 3; c++) {
            res.f[c] = blend_channel(d->rgb_eq, d->rgb_src, d->rgb_dst, c,
                                     &src, &dst, &d->blend_const,
                                     d->cbuf_float[n]);
        }
        res.f[3] = blend_channel(d->a_eq, d->a_src, d->a_dst, 3,
                                 &src, &dst, &d->blend_const,
                                 d->cbuf_float[n]);
    }
    for (c = 0; c < 4; c++) {
        if (!((chan_mask >> c) & 1)) {
            res.f[c] = dst.f[c];
        }
    }
    r5xx_color_pack(fmt, &res, buf);
    surf_write(r, l, x, y, buf);
    return true;
}

static void apply_fog(const R5xxRaster *r, float fogc, r5xx_vec4 *c)
{
    const struct R5xxRasterDerived *d = &r->d;
    float f;
    int i;

    switch (d->fog_mode) {
    case FG_FOG_FN_LINEAR:
        f = 1.0f - r5xx_clampf((fogc - d->fog_offset) * d->fog_scale,
                               0.0f, 1.0f);
        break;
    case FG_FOG_FN_EXP:
        f = expf(-(fogc * d->fog_scale));
        break;
    case FG_FOG_FN_EXP2:
        f = fogc * d->fog_scale;
        f = expf(-(f * f));
        break;
    default:
        f = d->fog_factor;
        break;
    }
    f = r5xx_clampf(f, 0.0f, 1.0f);
    for (i = 0; i < 3; i++) {
        c->f[i] = f * c->f[i] + (1.0f - f) * d->fog_color.f[i];
    }
}

/* Fixed-function per-pixel stage after the fragment shader. */
static void shade_pixel(R5xxRaster *r, bool front, int x, int y, float z,
                        float fogc, const r5xx_vec4 *colors)
{
    const struct R5xxRasterDerived *d = &r->d;
    r5xx_vec4 col[R500_US_MAX_OUTPUTS];
    bool wrote = false;
    int n;

    if (d->aa_resolve) {
        uint8_t px[16];

        surf_read(r, &r->rt.cbuf[0], x, y, px);
        surf_write(r, &d->resolve, x, y, px);
        r->pixels_out++;
        return;
    }
    memcpy(col, colors, sizeof(col));
    if (d->fog_enable) {
        for (n = 0; n < R500_US_MAX_OUTPUTS; n++) {
            apply_fog(r, fogc, &col[n]);
        }
    }
    if (d->alpha_test && !alpha_compare(d->alpha_func, col[0].f[3],
                                        d->alpha_ref)) {
        return;
    }

    if (r->rt.zbuf_enabled && (d->z_enable || d->stencil_enable)) {
        bool do_stencil = d->stencil_enable && r->rt.has_stencil;
        int face = front ? 0 : 1;
        uint32_t zold, znew = 0;
        uint8_t sold, snew;
        bool zpass;

        zs_read(r, x, y, &zold, &sold);
        snew = sold;
        if (do_stencil) {
            uint8_t ref = d->s_ref[face], mask = d->s_mask[face];
            if (!zs_compare(d->s_func[face], ref & mask, sold & mask)) {
                snew = stencil_op(d->s_fail[face], sold, ref);
                snew = (sold & ~d->s_wmask[face]) | (snew & d->s_wmask[face]);
                if (snew != sold) {
                    zs_write(r, x, y, zold, snew);
                }
                return;
            }
        }
        if (d->z_enable) {
            znew = depth_to_fixed(r, z);
            zpass = zs_compare(d->z_func, znew, zold);
        } else {
            zpass = true;
        }
        if (do_stencil) {
            uint8_t ref = d->s_ref[face];
            snew = stencil_op(zpass ? d->s_zpass[face] : d->s_zfail[face],
                              sold, ref);
            snew = (sold & ~d->s_wmask[face]) | (snew & d->s_wmask[face]);
        }
        if (!zpass) {
            if (snew != sold) {
                zs_write(r, x, y, zold, snew);
            }
            return;
        }
        if (d->z_enable && d->z_write) {
            if (znew != zold || snew != sold) {
                zs_write(r, x, y, znew, snew);
                wrote = true;
            }
        } else if (snew != sold) {
            zs_write(r, x, y, zold, snew);
            wrote = true;
        }
    }
    r->zpass_count++;

    for (n = 0; n < R5XX_MAX_COLOR_BUFS; n++) {
        if (r->rt.cbuf_enabled[n]) {
            const r5xx_vec4 *c = &col[d->num_multiwrites > 1 ? 0 : n];
            if (write_color(r, n, x, y, c)) {
                wrote = true;
            }
        }
    }
    if (wrote) {
        r->pixels_out++;
    }
}

/* ------------------------------------------------------------------ */
/* Primitive setup and quad traversal                                  */
/* ------------------------------------------------------------------ */

typedef enum { PRIM_TRI, PRIM_LINE, PRIM_POINT } PrimKind;

typedef struct Prim {
    PrimKind kind;
    const R5xxVertex *v[3];
    bool front;
    double z_offset;            /* polygon offset in [0,1] depth units */
    /* triangle: edge functions E_i(px,py) = A_i*px + B_i*py + C_i in
     * subpixel units, oriented so that inside is >= 0; bias_i is the
     * top-left fill rule adjustment (0 or -1). */
    int64_t A[3], B[3], C[3], bias[3];
    int64_t area;
    /* line: window endpoints, major axis */
    float x0, y0, x1, y1;
    bool xmajor;
    float half_w;
    /* point */
    float half_px, half_py;
} Prim;

static int64_t snap(const R5xxRaster *r, float v)
{
    return (int64_t)llrintf(v * (float)r->d.subpix);
}

/* Window position of a pixel center in subpixel units. */
static int64_t center(const R5xxRaster *r, int px)
{
    return (int64_t)px * r->d.subpix + r->d.subpix / 2;
}

static bool prim_covers(const R5xxRaster *r, const Prim *p, int px, int py)
{
    if (!pixel_visible(r, px, py)) {
        return false;
    }
    if (p->kind == PRIM_TRI) {
        int64_t cx = center(r, px), cy = center(r, py);
        int i;
        for (i = 0; i < 3; i++) {
            if (p->A[i] * cx + p->B[i] * cy + p->C[i] + p->bias[i] < 0) {
                return false;
            }
        }
        return true;
    } else if (p->kind == PRIM_LINE) {
        float cx = px + 0.5f, cy = py + 0.5f;
        float maj, maj0, maj1, min0, min1, minc, t, ml;
        if (p->xmajor) {
            maj = cx; maj0 = p->x0; maj1 = p->x1;
            min0 = p->y0; min1 = p->y1; minc = cy;
        } else {
            maj = cy; maj0 = p->y0; maj1 = p->y1;
            min0 = p->x0; min1 = p->x1; minc = cx;
        }
        /* diamond-exit approximation: the start pixel is drawn, the end
         * pixel is not (half-open interval along the major axis) */
        if (maj1 > maj0) {
            if (maj < maj0 || maj >= maj1) return false;
        } else {
            if (maj <= maj1 || maj > maj0) return false;
        }
        t = (maj - maj0) / (maj1 - maj0);
        ml = min0 + t * (min1 - min0);
        return minc > ml - p->half_w && minc <= ml + p->half_w;
    } else {
        float cx = px + 0.5f, cy = py + 0.5f;
        return cx >= p->x0 - p->half_px && cx < p->x0 + p->half_px &&
               cy >= p->y0 - p->half_py && cy < p->y0 + p->half_py;
    }
}

/* Barycentric / parametric weights of a pixel center. */
static void prim_weights(const R5xxRaster *r, const Prim *p, int px, int py,
                         double b[3])
{
    if (p->kind == PRIM_TRI) {
        int64_t cx = center(r, px), cy = center(r, py);
        int i;
        for (i = 0; i < 3; i++) {
            b[i] = (double)(p->A[i] * cx + p->B[i] * cy + p->C[i]) /
                   (double)p->area;
        }
    } else if (p->kind == PRIM_LINE) {
        float maj = p->xmajor ? px + 0.5f : py + 0.5f;
        float maj0 = p->xmajor ? p->x0 : p->y0;
        float maj1 = p->xmajor ? p->x1 : p->y1;
        double t = (maj - maj0) / (maj1 - maj0);
        if (t < 0) t = 0;
        if (t > 1) t = 1;
        b[0] = 1.0 - t;
        b[1] = t;
        b[2] = 0;
    } else {
        b[0] = 1;
        b[1] = 0;
        b[2] = 0;
    }
}

static int provoking_index(const R5xxRaster *r, const Prim *p)
{
    int nv = p->kind == PRIM_TRI ? 3 : (p->kind == PRIM_LINE ? 2 : 1);
    int idx = r->d.provoking;
    if (idx == 3 || idx >= nv) {
        idx = nv - 1;
    }
    return idx;
}

static void emit_quad(R5xxRaster *r, const Prim *p, int qx, int qy,
                      uint8_t cov)
{
    R500UsQuadIn in;
    R500UsQuadOut out;
    float fogc[4];
    int nattr = imin(r->num_fs_inputs, R5XX_MAX_FS_INPUTS);
    int prov = provoking_index(r, p);
    int i;

    memset(&in, 0, sizeof(in));
    for (i = 0; i < 4; i++) {
        int px = qx + (i & 1), py = qy + (i >> 1);
        double b[3], invw[3], denom = 0, z = 0;
        int k, a;

        prim_weights(r, p, px, py, b);
        for (k = 0; k < 3; k++) {
            float w = p->v[k]->w;
            invw[k] = 1.0 / (w > 1e-30f ? w : 1e-30f);
            denom += b[k] * invw[k];
            z += b[k] * p->v[k]->z;
        }
        in.x[i] = px + 0.5f;
        in.y[i] = py + 0.5f;
        in.z[i] = (float)z;
        in.w[i] = (float)denom;
        for (a = 0; a < nattr; a++) {
            if ((r->flat_inputs >> a) & 1) {
                in.inputs[i][a] = p->v[prov]->attr[a];
            } else {
                int c;
                for (c = 0; c < 4; c++) {
                    double s = 0;
                    for (k = 0; k < 3; k++) {
                        s += b[k] * invw[k] * p->v[k]->attr[a].f[c];
                    }
                    in.inputs[i][a].f[c] = (float)(denom != 0 ? s / denom : s);
                }
            }
        }
        /* fog coordinate (GB_SELECT): alpha of color n, W or Z */
        if (r->d.fog_select == GB_FOG_SELECT_Z) {
            fogc[i] = in.z[i];
        } else if (r->d.fog_select == GB_FOG_SELECT_1_1_W) {
            fogc[i] = denom != 0 ? (float)(1.0 / denom) : 0.0f;
        } else {
            int a2 = imin(r->d.fog_select, R5XX_MAX_FS_INPUTS - 1);
            fogc[i] = in.inputs[i][a2].f[3];
        }
    }
    in.front_facing = p->front;
    in.coverage = cov;

    memset(&out, 0, sizeof(out));
    if (r->us.prog) {
        r500_us_run_quad(&r->us, &in, &out);
    } else {
        int n;
        for (i = 0; i < 4; i++) {
            for (n = 0; n < R500_US_MAX_OUTPUTS; n++) {
                out.color[i][n] = in.inputs[i][0];
            }
        }
    }
    r->quads++;

    for (i = 0; i < 4; i++) {
        float z;
        if (!((cov >> i) & 1)) {
            continue;
        }
        r->pixels_in++;
        if ((out.killed >> i) & 1) {
            continue;
        }
        z = in.z[i];
        if (out.depth_written) {
            z = out.depth[i];
        }
        shade_pixel(r, p->front, qx + (i & 1), qy + (i >> 1), z, fogc[i],
                    out.color[i]);
    }
}

/* Walk the bounding box [x0,x1]x[y0,y1] (inclusive) in 2x2 quads. */
static void traverse(R5xxRaster *r, const Prim *p, int x0, int y0,
                     int x1, int y1)
{
    int qx, qy;

    x0 = imax(x0, r->d.sc_x0);
    y0 = imax(y0, r->d.sc_y0);
    x1 = imin(x1, r->d.sc_x1);
    y1 = imin(y1, r->d.sc_y1);
    if (x0 > x1 || y0 > y1) {
        return;
    }
    for (qy = y0 & ~1; qy <= y1; qy += 2) {
        for (qx = x0 & ~1; qx <= x1; qx += 2) {
            uint8_t cov = 0;
            int i;
            for (i = 0; i < 4; i++) {
                if (prim_covers(r, p, qx + (i & 1), qy + (i >> 1))) {
                    cov |= 1u << i;
                }
            }
            if (cov) {
                emit_quad(r, p, qx, qy, cov);
            }
        }
    }
}

/* ---- triangles ---- */

static void raster_tri(R5xxRaster *r, Prim *p)
{
    int64_t X[3], Y[3], minX, maxX, minY, maxY;
    int64_t area, s;
    int i;

    for (i = 0; i < 3; i++) {
        X[i] = snap(r, p->v[i]->x);
        Y[i] = snap(r, p->v[i]->y);
    }
    area = (X[1] - X[0]) * (Y[2] - Y[0]) - (X[2] - X[0]) * (Y[1] - Y[0]);
    if (area == 0) {
        return;
    }
    s = area < 0 ? -1 : 1;
    p->area = area * s;
    for (i = 0; i < 3; i++) {
        /* edge opposite vertex i: from v[i+1] to v[i+2], oriented so the
         * inside is on the positive side */
        int a = (i + 1) % 3, b = (i + 2) % 3;
        int64_t dx = (X[b] - X[a]) * s, dy = (Y[b] - Y[a]) * s;
        p->A[i] = -dy;
        p->B[i] = dx;
        p->C[i] = dy * X[a] - dx * Y[a];
        /* top-left rule (y down, clockwise orientation): top edge is
         * horizontal pointing right, left edge points up */
        p->bias[i] = ((dy == 0 && dx > 0) || dy < 0) ? 0 : -1;
    }
    minX = maxX = X[0];
    minY = maxY = Y[0];
    for (i = 1; i < 3; i++) {
        if (X[i] < minX) minX = X[i];
        if (X[i] > maxX) maxX = X[i];
        if (Y[i] < minY) minY = Y[i];
        if (Y[i] > maxY) maxY = Y[i];
    }
    traverse(r, p,
             (int)floordiv64(minX - r->d.subpix / 2, r->d.subpix),
             (int)floordiv64(minY - r->d.subpix / 2, r->d.subpix),
             (int)floordiv64(maxX - r->d.subpix / 2, r->d.subpix) + 1,
             (int)floordiv64(maxY - r->d.subpix / 2, r->d.subpix) + 1);
}

/* Polygon offset in float depth units for a triangle with the given
 * window-space vertices: GL's m*factor + r*units.  Mesa programs
 * SCALE = factor*12 and OFFSET = units*4 (Z16) / units*2 (Z24). */
static double poly_offset(const R5xxRaster *r, const R5xxVertex *v0,
                         const R5xxVertex *v1, const R5xxVertex *v2,
                         float scale, float offset)
{
    double ax = v1->x - v0->x, ay = v1->y - v0->y, az = v1->z - v0->z;
    double bx = v2->x - v0->x, by = v2->y - v0->y, bz = v2->z - v0->z;
    double det = ax * by - ay * bx;
    double dzdx, dzdy, m, unit;
    if (det == 0) {
        return 0;
    }
    dzdx = (az * by - bz * ay) / det;
    dzdy = (ax * bz - bx * az) / det;
    m = fabs(dzdx) > fabs(dzdy) ? fabs(dzdx) : fabs(dzdy);
    unit = r->rt.z_is_16 ? (1.0 / 65535.0) / 4.0 : (1.0 / 16777215.0) / 2.0;
    return m * scale / r->d.subpix + offset * unit;
}

static void raster_line_prim(R5xxRaster *r, Prim *p);
static void raster_point_prim(R5xxRaster *r, Prim *p);

void r5xx_raster_triangle(R5xxRaster *r, const R5xxVertex *v0,
                          const R5xxVertex *v1, const R5xxVertex *v2)
{
    const struct R5xxRasterDerived *d = &r->d;
    Prim p;
    double area;
    bool ccw, front;
    int ptype;

    area = ((double)v1->x - v0->x) * ((double)v2->y - v0->y) -
           ((double)v2->x - v0->x) * ((double)v1->y - v0->y);
    if (area == 0) {
        /* snapped area decides below; use the float sign for facing */
        area = 1;
    }
    ccw = area < 0;                     /* y-down window coordinates */
    front = (ccw == d->front_ccw);
    if ((front && d->cull_front) || (!front && d->cull_back)) {
        return;
    }

    memset(&p, 0, sizeof(p));
    p.v[0] = v0;
    p.v[1] = v1;
    p.v[2] = v2;
    p.front = front;
    if (front ? d->po_front : d->po_back) {
        p.z_offset = poly_offset(r, v0, v1, v2,
                                 front ? d->po_front_scale : d->po_back_scale,
                                 front ? d->po_front_offset : d->po_back_offset);
    }

    ptype = PTYPE_TRI;
    if (d->poly_mode_dual) {
        ptype = front ? d->poly_front_type : d->poly_back_type;
    }
    if (ptype == PTYPE_TRI) {
        p.kind = PRIM_TRI;
        r->prim_z_offset = p.z_offset;
        raster_tri(r, &p);
        r->prim_z_offset = 0;
    } else if (ptype == PTYPE_LINE) {
        const R5xxVertex *e[3][2] = { { v0, v1 }, { v1, v2 }, { v2, v0 } };
        int i;
        for (i = 0; i < 3; i++) {
            Prim lp = p;
            lp.kind = PRIM_LINE;
            lp.v[0] = e[i][0];
            lp.v[1] = e[i][1];
            lp.v[2] = e[i][1];
            raster_line_prim(r, &lp);
        }
    } else {
        const R5xxVertex *pv[3] = { v0, v1, v2 };
        int i;
        for (i = 0; i < 3; i++) {
            Prim pp = p;
            pp.kind = PRIM_POINT;
            pp.v[0] = pv[i];
            pp.v[1] = pv[i];
            pp.v[2] = pv[i];
            pp.half_px = d->point_half_w;
            pp.half_py = d->point_half_h;
            raster_point_prim(r, &pp);
        }
    }
}

/* ---- lines ---- */

static void raster_line_prim(R5xxRaster *r, Prim *p)
{
    const R5xxVertex *v0 = p->v[0], *v1 = p->v[1];
    float dx, dy, hw;
    int x0, y0, x1, y1;

    p->x0 = (float)snap(r, v0->x) / r->d.subpix;
    p->y0 = (float)snap(r, v0->y) / r->d.subpix;
    p->x1 = (float)snap(r, v1->x) / r->d.subpix;
    p->y1 = (float)snap(r, v1->y) / r->d.subpix;
    dx = p->x1 - p->x0;
    dy = p->y1 - p->y0;
    if (dx == 0 && dy == 0) {
        return;
    }
    p->xmajor = fabsf(dx) >= fabsf(dy);
    hw = r->d.line_half_w;
    p->half_w = hw < 0.5f ? 0.5f : hw;

    x0 = (int)floorf(fminf_(p->x0, p->x1) - p->half_w) - 1;
    x1 = (int)ceilf(fmaxf_(p->x0, p->x1) + p->half_w) + 1;
    y0 = (int)floorf(fminf_(p->y0, p->y1) - p->half_w) - 1;
    y1 = (int)ceilf(fmaxf_(p->y0, p->y1) + p->half_w) + 1;
    traverse(r, p, x0, y0, x1, y1);
}

void r5xx_raster_line(R5xxRaster *r, const R5xxVertex *v0,
                      const R5xxVertex *v1)
{
    Prim p;
    memset(&p, 0, sizeof(p));
    p.kind = PRIM_LINE;
    p.v[0] = v0;
    p.v[1] = v1;
    p.v[2] = v1;
    p.front = true;
    raster_line_prim(r, &p);
}

/* ---- points ---- */

static void raster_point_prim(R5xxRaster *r, Prim *p)
{
    const R5xxVertex *v = p->v[0];
    int x0, y0, x1, y1;

    p->x0 = (float)snap(r, v->x) / r->d.subpix;
    p->y0 = (float)snap(r, v->y) / r->d.subpix;
    if (p->half_px < 0.5f) p->half_px = 0.5f;
    if (p->half_py < 0.5f) p->half_py = 0.5f;
    x0 = (int)floorf(p->x0 - p->half_px) - 1;
    x1 = (int)ceilf(p->x0 + p->half_px) + 1;
    y0 = (int)floorf(p->y0 - p->half_py) - 1;
    y1 = (int)ceilf(p->y0 + p->half_py) + 1;
    traverse(r, p, x0, y0, x1, y1);
}

void r5xx_raster_point(R5xxRaster *r, const R5xxVertex *v)
{
    Prim p;
    memset(&p, 0, sizeof(p));
    p.kind = PRIM_POINT;
    p.v[0] = p.v[1] = p.v[2] = v;
    p.front = true;
    p.half_px = r->d.point_half_w;
    p.half_py = r->d.point_half_h;
    raster_point_prim(r, &p);
}

void r5xx_raster_point_sized(R5xxRaster *r, const R5xxVertex *v, float size)
{
    Prim p;
    if (r->regs.ga_point_minmax != 0) {
        size = r5xx_clampf(size, r->d.point_min, r->d.point_max);
    }
    memset(&p, 0, sizeof(p));
    p.kind = PRIM_POINT;
    p.v[0] = p.v[1] = p.v[2] = v;
    p.front = true;
    /* snap the diameter to the subpixel grid like GA_POINT_SIZE does */
    p.half_px = p.half_py = (float)llrintf(size * 0.5f * r->d.subpix) /
                            r->d.subpix;
    raster_point_prim(r, &p);
}

/* ------------------------------------------------------------------ */
/* Clears                                                              */
/* ------------------------------------------------------------------ */

/*
 * PACKET3_3D_CLEAR_ZMASK marks every compressed Z tile "cleared": until a
 * tile is written, depth/stencil reads return ZB_DEPTHCLEARVALUE.  Z is
 * stored uncompressed here, so the equivalent is filling the depth buffer
 * with that word (the raw packed Z24S8 / Z16 value).  The area is the
 * scissor rectangle, limited to what ndw ZMASK dwords can describe at most
 * (64x64 pixels per dword: 4 pipes, 8x8 compression) so a misjudged clear
 * never runs past the buffer.  Apple's driver clears depth this way.
 */
void r5xx_raster_zmask_clear(R5xxRaster *r, uint32_t ndw, uint32_t value)
{
    const R5xxRasterRegs *g = &r->regs;
    uint32_t pitch = g->zb_depthpitch & DEPTHPITCH_MASK;
    uint32_t fmt = g->zb_format & DEPTHFORMAT_MASK;
    R5xxSurfaceLayout l;
    uint64_t max_px = (uint64_t)ndw * 64 * 64;
    int x1, y1, x, y;
    unsigned micro;

    if (!pitch || !ndw || (fmt != DEPTHFORMAT_16BIT_INT_Z &&
                           fmt != DEPTHFORMAT_24BIT_INT_Z_8BIT_STENCIL)) {
        return;
    }
    memset(&l, 0, sizeof(l));
    l.gpu_addr = g->zb_depthoffset & COLOROFFSET_MASK;
    l.bpp = fmt == DEPTHFORMAT_16BIT_INT_Z ? 2 : 4;
    l.pitch_bytes = pitch * l.bpp;
    l.width = pitch;
    micro = (g->zb_depthpitch >> DEPTHMICROTILE_SHIFT) & DEPTHMICROTILE_MASK;
    l.tiling = decode_tiling(g->zb_depthpitch & DEPTHMACROTILE_ENABLE, micro);
    l.micro_mode = (uint8_t)micro;
    l.is_r500 = g->is_r500;

    {
        int sx0, sy0, sx1, sy1;
        decode_rect(g->sc_scissors_tl, g->sc_scissors_br, g->is_r500 ? 0 : SC_R300_OFFSET,
                    &sx0, &sy0, &sx1, &sy1);
        x1 = imin(sx1 + 1, (int)pitch);
        y1 = sy1 + 1;
    }
    if ((uint64_t)y1 * pitch > max_px) {
        y1 = (int)(max_px / pitch);
    }
    l.height = y1;
    for (y = 0; y < y1; y++) {
        for (x = 0; x < x1; x++) {
            if (l.bpp == 2) {
                uint16_t v16 = (uint16_t)value;
                surf_write(r, &l, x, y, &v16);
            } else {
                surf_write(r, &l, x, y, &value);
            }
        }
    }
}

void r5xx_raster_clear(R5xxRaster *r, int x0, int y0, int x1, int y1,
                       bool clear_color, const r5xx_vec4 *color,
                       bool clear_depth, float depth,
                       bool clear_stencil, uint8_t stencil)
{
    int x, y, n;

    x0 = imax(x0, 0);
    y0 = imax(y0, 0);
    if (clear_color) {
        for (n = 0; n < R5XX_MAX_COLOR_BUFS; n++) {
            const R5xxSurfaceLayout *l = &r->rt.cbuf[n];
            uint8_t buf[16];
            int xe;
            if (!r->rt.cbuf_enabled[n]) {
                continue;
            }
            r5xx_color_pack(r->rt.cbuf_format[n], color, buf);
            xe = imin(x1, (int)l->width);
            for (y = y0; y < y1; y++) {
                for (x = x0; x < xe; x++) {
                    surf_write(r, l, x, y, buf);
                }
            }
        }
    }
    if ((clear_depth || clear_stencil) && r->rt.zbuf_enabled) {
        const R5xxSurfaceLayout *l = &r->rt.zbuf;
        uint32_t zv;
        int xe = imin(x1, (int)l->width);
        float dz = r5xx_clampf(depth, 0.0f, 1.0f);
        bool rmw;

        /* double: in float, 16777215.5 rounds to 2^24 and the Z24 value
         * of a clear to 1.0 overflowed to 0 */
        zv = r->rt.z_is_16 ? (uint32_t)(dz * 65535.0 + 0.5)
                           : (uint32_t)(dz * 16777215.0 + 0.5);
        if (!r->rt.has_stencil) {
            clear_stencil = false;
            stencil = 0;
        }
        rmw = r->rt.has_stencil && !(clear_depth && clear_stencil);
        for (y = y0; y < y1; y++) {
            for (x = x0; x < xe; x++) {
                if (rmw) {
                    uint32_t zo;
                    uint8_t so;
                    zs_read(r, x, y, &zo, &so);
                    zs_write(r, x, y, clear_depth ? zv : zo,
                             clear_stencil ? stencil : so);
                } else if (clear_depth) {
                    zs_write(r, x, y, zv, stencil);
                }
            }
        }
    }
}
