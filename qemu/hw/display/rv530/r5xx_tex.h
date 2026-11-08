/*
 * R300/R500 texture units: register decoding, surface layout (mip chain,
 * cube/3D, micro/macro tiling), format conversion and sampling.
 *
 * Reference: Mesa src/gallium/drivers/r300/r300_texture.c,
 * r300_texture_desc.c, r300_state.c (r300_create_sampler_state /
 * r300_texture_setup_format_state) and r300_reg.h (R300_TX_*).
 *
 * Pure C, no QEMU dependencies.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef R5XX_TEX_H
#define R5XX_TEX_H

#include "r5xx_core.h"

#define R5XX_NUM_TEX_UNITS 16

/* ------------------------------------------------------------------ */
/* Surface layout: shared by textures, render targets and scanout       */
/* ------------------------------------------------------------------ */

typedef enum R5xxTiling {
    R5XX_TILE_LINEAR = 0,
    R5XX_TILE_MICRO,          /* micro tiling (R300_TXO_MICRO_TILE) */
    R5XX_TILE_MACRO,          /* macro tiling only */
    R5XX_TILE_MICRO_MACRO,    /* both */
} R5xxTiling;

typedef struct R5xxSurfaceLayout {
    uint32_t gpu_addr;        /* base address of level 0 */
    uint32_t pitch_bytes;     /* row pitch of level 0 in bytes */
    uint32_t width, height;   /* level 0 size in pixels */
    uint32_t bpp;             /* bytes per pixel (or per block for DXT) */
    bool     compressed;      /* block compressed (DXT) */
    R5xxTiling tiling;
    bool     is_r500;
    /* --- added by the tex module (appended, see docs/tex_NOTES.md) --- */
    uint8_t  micro_mode;      /* 0 = auto (square for 16bpp, else 8x4/4x2/2x2),
                               * 1 = R300_TXO_MICRO_TILE_TILED,
                               * 2 = R300_TXO_MICRO_TILE_TILED_SQUARE */
} R5xxSurfaceLayout;

/* Optional device hook: true when gpu_addr lies in memory the CPU sees
 * through a tiling "surface" aperture window (RADEON_SURFACEn).  While the
 * window is set, that memory is kept linear in the model (the aperture is
 * direct-mapped RAM), so tiled GPU accesses to it are treated as linear
 * too; the device converts it when the window is set and removed. */
extern bool (*r5xx_linear_hook)(void *opaque, uint32_t gpu_addr);
extern void *r5xx_linear_hook_opaque;
static inline bool r5xx_addr_is_linear(uint32_t gpu_addr)
{
    return r5xx_linear_hook && r5xx_linear_hook(r5xx_linear_hook_opaque, gpu_addr);
}

/* Byte offset of pixel (x, y) relative to gpu_addr, honoring tiling. */
uint32_t r5xx_surface_row_run(const R5xxSurfaceLayout *l, uint32_t x, uint32_t y);
uint32_t r5xx_surface_pixel_offset(const R5xxSurfaceLayout *l,
                                   uint32_t x, uint32_t y);
/* The same, ignoring r5xx_linear_hook: the tiled layout itself. */
uint32_t r5xx_tiled_pixel_offset(const R5xxSurfaceLayout *l,
                                 uint32_t x, uint32_t y);

/* ------------------------------------------------------------------ */
/* Texture formats                                                     */
/* ------------------------------------------------------------------ */

/* Decoded texture state for one unit.  Produced by r5xx_tex_decode()
 * from the raw registers; consumed by the sampler.  Fields are public so
 * the Vulkan backend can translate them too. */
typedef struct R5xxTexRegs {
    uint32_t filter0;        /* R300_TX_FILTER0_n   0x4400 + 4n */
    uint32_t filter1;        /* R300_TX_FILTER1_n   0x4440 + 4n */
    uint32_t format0;        /* R300_TX_FORMAT0_n   0x4480 + 4n */
    uint32_t format1;        /* R300_TX_FORMAT1_n   0x44C0 + 4n */
    uint32_t format2;        /* R300_TX_FORMAT2_n   0x4500 + 4n */
    uint32_t offset;         /* R300_TX_OFFSET_n    0x4540 + 4n */
    uint32_t chroma_key;     /* R300_TX_CHROMA_KEY_n 0x4580 + 4n */
    uint32_t border_color;   /* R300_TX_BORDER_COLOR_n 0x45C0 + 4n */
} R5xxTexRegs;

typedef enum R5xxWrap {
    R5XX_WRAP_REPEAT, R5XX_WRAP_MIRROR, R5XX_WRAP_CLAMP_LAST_TEXEL,
    R5XX_WRAP_MIRROR_CLAMP_LAST_TEXEL, R5XX_WRAP_CLAMP_HALF_BORDER,
    R5XX_WRAP_MIRROR_CLAMP_HALF_BORDER, R5XX_WRAP_CLAMP_BORDER,
    R5XX_WRAP_MIRROR_CLAMP_BORDER,
} R5xxWrap;

typedef enum R5xxFilter {
    R5XX_FILTER_NONE, R5XX_FILTER_NEAREST, R5XX_FILTER_LINEAR,
} R5xxFilter;

typedef struct R5xxTexSampler {
    /* geometry */
    uint32_t width, height, depth;    /* level 0 */
    uint32_t num_levels;              /* 1 + max mip level */
    bool     is_cube, is_3d, is_npot, is_rect;
    /* format */
    uint32_t hw_format;               /* R300_TX_FORMAT_* low bits */
    uint32_t bpp;                     /* bytes per texel / block */
    bool     compressed;              /* DXT1/3/5 */
    uint32_t block_w, block_h;        /* 4x4 for DXT, 1x1 otherwise */
    uint8_t  swizzle[4];              /* output channel sources 0-3 = RGBA, 4 = zero, 5 = one */
    bool     is_float, is_signed, is_depth;
    bool     gamma;                   /* sRGB decode */
    /* layout */
    R5xxSurfaceLayout layout;         /* level 0 */
    uint32_t level_offset[16];        /* bytes from base, per mip level */
    uint32_t level_pitch[16];         /* bytes */
    uint32_t face_offset_stride;      /* cube faces (bytes) */
    /* sampling */
    R5xxWrap wrap_s, wrap_t, wrap_r;
    R5xxFilter min_filter, mag_filter, mip_filter;
    float    lod_bias, min_lod, max_lod;
    uint32_t anisotropy;              /* 1, 2, 4, 8, 16 */
    r5xx_vec4 border_color;
    bool     depth_compare;           /* shadow sampler */
    uint32_t depth_compare_func;      /* R300_TX_FORMAT_* compare func */
    uint32_t raw[8];                  /* copy of the registers, for debug */
    /* --- added by the tex module (appended, see docs/tex_NOTES.md) --- */
    uint32_t level_layer_size[16];    /* bytes per cube face / 3D slice, per level */
    uint8_t  level_tiling[16];        /* R5xxTiling per level (hw macro switch) */
    bool     dxtc_swizzle;            /* R400_DXTC_SWIZZLE_ENABLE in FILTER1 */
    bool     yuv_to_rgb;              /* R300_TX_FORMAT_YUV_TO_RGB */
    bool     pitch_en;                /* R300_TX_PITCH_EN: level-0 pitch from FORMAT2 */
    bool     valid;                   /* r5xx_tex_decode() succeeded */
    uint8_t  signed_mask;             /* bit0 = X .. bit3 = W (R300_TX_FORMAT_SIGNED_*) */
    uint8_t  endian;                  /* TX_OFFSET bits 1:0, R5XX_ENDIAN_* */
} R5xxTexSampler;

/* Byte order of the texels in memory (R300_TXO_ENDIAN_*), undone per
 * dword before decoding.  Apple sets 8IN32 for client-storage textures in
 * big-endian packed types (GL_UNSIGNED_INT_8_8_8_8). */
enum {
    R5XX_ENDIAN_NONE, R5XX_ENDIAN_8IN16, R5XX_ENDIAN_8IN32, R5XX_ENDIAN_16IN32,
};

static inline uint32_t r5xx_endian_swap(uint32_t mode, uint32_t w)
{
    switch (mode) {
    case R5XX_ENDIAN_8IN16:
        return ((w & 0x00ff00ffu) << 8) | ((w >> 8) & 0x00ff00ffu);
    case R5XX_ENDIAN_8IN32:
        return (w << 24) | ((w & 0xff00u) << 8) | ((w >> 8) & 0xff00u) | (w >> 24);
    case R5XX_ENDIAN_16IN32:
        return (w << 16) | (w >> 16);
    default:
        return w;
    }
}

/* Decode the registers of one unit.  Returns false (and a message in
 * err) for unsupported formats; the sampler is still filled in well
 * enough to return the border color. */
bool r5xx_tex_decode(const R5xxTexRegs *regs, bool is_r500,
                     R5xxTexSampler *out, char *err, size_t errlen);

/* Read one texel (no filtering) of the given level/face/slice as float
 * RGBA, applying the format swizzle.  Coordinates are clamped. */
/* Vulkan backend format code (see r5xx_vk.h); false if unsupported. */
bool r5xx_tex_gpu_format(const R5xxTexSampler *s, uint32_t *fmt, char *why, size_t whylen);

void r5xx_tex_fetch_texel(const R5xxTexSampler *s, const R5xxMem *mem,
                          uint32_t level, uint32_t face, uint32_t slice,
                          int32_t x, int32_t y, r5xx_vec4 *out);

/* ------------------------------------------------------------------ */
/* Sampling requests from the fragment shader                          */
/* ------------------------------------------------------------------ */

typedef enum R5xxTexOp {
    R5XX_TEXOP_SAMPLE,      /* TEX: implicit LOD from quad derivatives */
    R5XX_TEXOP_PROJ,        /* TXP: divide s,t,r by q, then as SAMPLE */
    R5XX_TEXOP_BIAS,        /* TXB: implicit LOD + bias from .w / lod[] */
    R5XX_TEXOP_LOD,         /* explicit LOD in lod[] */
    R5XX_TEXOP_GRAD,        /* explicit derivatives in ddx[], ddy[] */
} R5xxTexOp;

/* One request covers a 2x2 quad so that implicit derivatives can be
 * computed.  Pixel order: 0 = top-left, 1 = top-right, 2 = bottom-left,
 * 3 = bottom-right. */
typedef struct R5xxTexRequest {
    int unit;
    R5xxTexOp op;
    r5xx_vec4 coord[4];     /* s, t, r (cube/3D), q (projection) */
    r5xx_vec4 ddx[4], ddy[4];
    float lod[4];           /* bias or explicit lod per pixel */
    /* --- added by the tex module (appended, see docs/tex_NOTES.md) --- */
    bool unscaled;          /* R500_TEX_UNSCALED: s,t are in texels (RECT) */
} R5xxTexRequest;

/* Filtered sampling of a quad. */
void r5xx_tex_sample_quad(const R5xxTexSampler *s, const R5xxMem *mem,
                          const R5xxTexRequest *req, r5xx_vec4 out[4]);

/* ------------------------------------------------------------------ */
/* Color / depth format conversion for render targets and scanout     */
/* ------------------------------------------------------------------ */

/* R300_RB3D_COLORPITCH format field values (R300_COLOR_FORMAT_*) */
typedef enum R5xxColorFormat {
    R5XX_COLOR_ARGB8888 = 0,
    R5XX_COLOR_RGB565,
    R5XX_COLOR_ARGB1555,
    R5XX_COLOR_ARGB4444,
    R5XX_COLOR_ARGB2101010,
    R5XX_COLOR_I8,
    R5XX_COLOR_ARGB16161616,      /* R500: ARGB16161616 / FP16 */
    R5XX_COLOR_ARGB16161616F,
    R5XX_COLOR_ARGB32323232F,
    /* --- added by the tex module (appended, see docs/tex_NOTES.md) --- */
    R5XX_COLOR_UV88,              /* R300_COLOR_FORMAT_UV88: 2 bytes (C2, C0) */
} R5xxColorFormat;

/* Map the RB3D_COLORPITCH hardware format code to the enum; returns
 * bytes per pixel or 0 for unsupported. */
uint32_t r5xx_color_format_from_hw(uint32_t colorpitch, bool is_r500,
                                   R5xxColorFormat *fmt);
void r5xx_color_unpack(R5xxColorFormat fmt, const uint8_t *p, r5xx_vec4 *out);
void r5xx_color_pack(R5xxColorFormat fmt, const r5xx_vec4 *c, uint8_t *p);

/* --- added by the tex module (see docs/tex_NOTES.md) --- */

/* The RB3D format code 10 (ARGB16161616) is shared by the UNORM16 and the
 * FP16 render targets; the distinction is R300_US_OUT_FMT_n (C4_16 vs
 * C4_16_FP).  Given the format from r5xx_color_format_from_hw() and the
 * raw US_OUT_FMT register, return the precise format. */
R5xxColorFormat r5xx_color_format_apply_out_fmt(R5xxColorFormat fmt,
                                                uint32_t us_out_fmt);

/* Bytes per pixel of a color format. */
uint32_t r5xx_color_format_bpp(R5xxColorFormat fmt);

/* Width/height/depth of a mip level (max(1, v >> level)). */
static inline uint32_t r5xx_minify(uint32_t v, uint32_t level)
{
    v >>= level;
    return v ? v : 1;
}

/* Layout of one level/layer of a texture: fills *l so that
 * r5xx_surface_pixel_offset(l, x, y) addresses texel (x, y) of that
 * level (block coordinates for compressed formats). */
void r5xx_tex_level_layout(const R5xxTexSampler *s, uint32_t level,
                           uint32_t layer, R5xxSurfaceLayout *l);

/* Human readable name of a texture format code (debug). */
const char *r5xx_tex_format_name(uint32_t hw_format, bool msb);

#endif /* R5XX_TEX_H */
