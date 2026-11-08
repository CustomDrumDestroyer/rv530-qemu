/*
 * R300/R500 texture units: register decoding, surface layout (mip chain,
 * cube/3D, micro/macro tiling), format conversion and sampling, plus the
 * render-target color pack/unpack helpers.
 *
 * References (read-only, see docs/tex_NOTES.md):
 *   Mesa src/gallium/drivers/r300/r300_reg.h          register fields
 *   Mesa src/gallium/drivers/r300/r300_texture.c      format translation,
 *                                                     r300_texture_setup_format_state
 *   Mesa src/gallium/drivers/r300/r300_texture_desc.c mip chain layout and
 *                                                     tiling alignment
 *   Mesa src/gallium/drivers/r300/r300_state.c        sampler state
 *   Mesa src/gallium/drivers/r300/r300_state_derived.c
 *                                                     border color, merge
 *
 * Pure C, no QEMU dependencies.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "r5xx_tex.h"
#include <math.h>
#include <string.h>
#include <stdio.h>

/* ------------------------------------------------------------------ */
/* Register fields (copied from Mesa r300_reg.h, MIT license,           */
/* Copyright 2008 Corbin Simpson, 2010 Marek Olšák and others)         */
/* ------------------------------------------------------------------ */

/* R300_TX_FILTER0_n */
#define R300_TX_REPEAT                    0
#define R300_TX_MIRRORED                  1
#define R300_TX_CLAMP_TO_EDGE             2
#define R300_TX_MIRROR_ONCE_TO_EDGE       3
#define R300_TX_CLAMP                     4
#define R300_TX_MIRROR_ONCE               5
#define R300_TX_CLAMP_TO_BORDER           6
#define R300_TX_MIRROR_ONCE_TO_BORDER     7
#define R300_TX_WRAP_S_SHIFT              0
#define R300_TX_WRAP_T_SHIFT              3
#define R300_TX_WRAP_R_SHIFT              6
#define R300_TX_MAG_FILTER_SHIFT          9     /* 0 = FILTER4, 1 = NEAREST, 2 = LINEAR, 3 = ANISO */
#define R300_TX_MIN_FILTER_SHIFT          11    /* 1 = NEAREST, 2 = LINEAR, 3 = ANISO */
#define R300_TX_MIP_FILTER_SHIFT          13    /* 0 = NONE, 1 = NEAREST, 2 = LINEAR */
#define R300_TX_MAX_MIP_LEVEL_SHIFT       17    /* 4 bits */
#define R300_TX_MAX_ANISO_SHIFT           21    /* 3 bits: 1,2,4,8,16 */

/* R300_TX_FILTER1_n */
#define R300_LOD_BIAS_SHIFT               3
#define R300_LOD_BIAS_MASK                0x1ff8   /* 10-bit signed, 1/32 units */
#define R400_DXTC_SWIZZLE_ENABLE          (1 << 21)
#define R500_MACRO_SWITCH                 (1 << 22)
#define R500_TX_MAX_ANISO_SHIFT           23
#define R500_TX_MAX_ANISO_MASK            (63 << 23)
#define R500_BORDER_FIX                   (1u << 31)

/* R300_TX_FORMAT0_n */
#define R300_TX_WIDTHMASK_MASK            (2047 << 0)
#define R300_TX_HEIGHTMASK_SHIFT          11
#define R300_TX_DEPTHMASK_SHIFT           22
#define R300_TX_NUM_LEVELS_SHIFT          26
#define R300_TX_SIZE_PROJECTED            (1 << 30)
#define R300_TX_PITCH_EN                  (1u << 31)

/* R300_TX_FORMAT1_n */
#define R300_TX_FORMAT_X8                 0x0
#define R300_TX_FORMAT_X16                0x1
#define R300_TX_FORMAT_Y4X4               0x2
#define R300_TX_FORMAT_Y8X8               0x3
#define R300_TX_FORMAT_Y16X16             0x4
#define R300_TX_FORMAT_Z3Y3X2             0x5
#define R300_TX_FORMAT_Z5Y6X5             0x6
#define R300_TX_FORMAT_Z6Y5X5             0x7
#define R300_TX_FORMAT_Z11Y11X10          0x8
#define R300_TX_FORMAT_Z10Y11X11          0x9
#define R300_TX_FORMAT_W4Z4Y4X4           0xA
#define R300_TX_FORMAT_W1Z5Y5X5           0xB
#define R300_TX_FORMAT_W8Z8Y8X8           0xC
#define R300_TX_FORMAT_W2Z10Y10X10        0xD
#define R300_TX_FORMAT_W16Z16Y16X16       0xE
#define R300_TX_FORMAT_DXT1               0xF
#define R300_TX_FORMAT_DXT3               0x10
#define R300_TX_FORMAT_DXT5               0x11
#define R300_TX_FORMAT_CxV8U8             0x12
#define R300_TX_FORMAT_AVYU444            0x13
#define R300_TX_FORMAT_VYUY422            0x14
#define R300_TX_FORMAT_YVYU422            0x15
#define R300_TX_FORMAT_16_MPEG            0x16
#define R300_TX_FORMAT_16_16_MPEG         0x17
#define R300_TX_FORMAT_16F                0x18
#define R300_TX_FORMAT_16F_16F            0x19
#define R300_TX_FORMAT_16F_16F_16F_16F    0x1A
#define R300_TX_FORMAT_32F                0x1B
#define R300_TX_FORMAT_32F_32F            0x1C
#define R300_TX_FORMAT_32F_32F_32F_32F    0x1D
#define R300_TX_FORMAT_W24_FP             0x1E
#define R400_TX_FORMAT_ATI2N              0x1F
/* with R500_TXFORMAT_MSB */
#define R500_TX_FORMAT_X1                 0x0
#define R500_TX_FORMAT_X1_REV             0x1
#define R500_TX_FORMAT_X10                0x2
#define R500_TX_FORMAT_Y10X10             0x3
#define R500_TX_FORMAT_W10Z10Y10X10       0x4
#define R500_TX_FORMAT_ATI1N              0x5
#define R500_TX_FORMAT_Y8X24              0x6

#define R300_TX_FORMAT_SIGNED_W           (1 << 5)
#define R300_TX_FORMAT_SIGNED_Z           (1 << 6)
#define R300_TX_FORMAT_SIGNED_Y           (1 << 7)
#define R300_TX_FORMAT_SIGNED_X           (1 << 8)
#define R300_TX_FORMAT_A_SHIFT            9
#define R300_TX_FORMAT_R_SHIFT            12
#define R300_TX_FORMAT_G_SHIFT            15
#define R300_TX_FORMAT_B_SHIFT            18
#define R300_TX_FORMAT_GAMMA              (1 << 21)
#define R300_TX_FORMAT_YUV_TO_RGB         (1 << 22)
#define R300_TX_FORMAT_TEX_COORD_SHIFT    25     /* 1 = 3D, 2 = CUBIC_MAP */
/* swizzle selects */
#define R300_TX_FORMAT_SEL_X              0
#define R300_TX_FORMAT_SEL_Y              1
#define R300_TX_FORMAT_SEL_Z              2
#define R300_TX_FORMAT_SEL_W              3
#define R300_TX_FORMAT_SEL_ZERO           4
#define R300_TX_FORMAT_SEL_ONE            5
#define R300_TX_FORMAT_SEL_CUT_Z          6
#define R300_TX_FORMAT_SEL_CUT_W          7

/* R300_TX_FORMAT2_n */
#define R300_TX_PITCHMASK_MASK            0x1fff
#define R500_TXFORMAT_MSB                 (1 << 14)
#define R500_TXWIDTH_BIT11                (1 << 15)
#define R500_TXHEIGHT_BIT11               (1 << 16)

/* R300_TX_OFFSET_n */
#define R300_TXO_MACRO_TILE_SHIFT         2
#define R300_TXO_MICRO_TILE_SHIFT         3
#define R300_TXO_OFFSET_MASK              0xffffffe0u

/* R300_RB3D_COLORPITCH format codes */
#define R300_COLOR_FORMAT_SHIFT           21
#define R300_COLOR_FORMAT_ARGB1555        3
#define R300_COLOR_FORMAT_RGB565          4
#define R500_COLOR_FORMAT_ARGB2101010     5
#define R300_COLOR_FORMAT_ARGB8888        6
#define R300_COLOR_FORMAT_ARGB32323232    7
#define R300_COLOR_FORMAT_I8              9
#define R300_COLOR_FORMAT_ARGB16161616    10
#define R300_COLOR_FORMAT_UV88            13
#define R300_COLOR_FORMAT_ARGB4444        15

/* R300_US_OUT_FMT_n low bits */
#define R300_US_OUT_FMT_C_16_FP           16
#define R300_US_OUT_FMT_C2_16_FP          17
#define R300_US_OUT_FMT_C4_16_FP          18

/* ------------------------------------------------------------------ */
/* Small helpers                                                       */
/* ------------------------------------------------------------------ */

static inline uint32_t align_u32(uint32_t v, uint32_t a)
{
    return (v + a - 1) / a * a;
}

static inline uint32_t next_pow2(uint32_t x)
{
    uint32_t p = 1;
    while (p < x) {
        p <<= 1;
    }
    return p;
}

static inline bool is_pow2(uint32_t x)
{
    return x && !(x & (x - 1));
}

static inline uint32_t log2_u32(uint32_t x)
{
    uint32_t r = 0;
    while (x > 1) {
        x >>= 1;
        r++;
    }
    return r;
}

static inline int ifloor(float f)
{
    return (int)floorf(f);
}

static inline float fracf(float f)
{
    return f - floorf(f);
}

static inline float lerpf(float a, float b, float t)
{
    return a + (b - a) * t;
}

static float srgb_to_linear(float c)
{
    if (c <= 0.04045f) {
        return c / 12.92f;
    }
    return powf((c + 0.055f) / 1.055f, 2.4f);
}

static inline uint32_t rd16(const uint8_t *p)
{
    return p[0] | (p[1] << 8);
}

static inline uint32_t rd32(const uint8_t *p)
{
    return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline void wr16(uint8_t *p, uint32_t v)
{
    p[0] = v & 0xff;
    p[1] = (v >> 8) & 0xff;
}

static inline void wr32(uint8_t *p, uint32_t v)
{
    p[0] = v & 0xff;
    p[1] = (v >> 8) & 0xff;
    p[2] = (v >> 16) & 0xff;
    p[3] = (v >> 24) & 0xff;
}

static inline float f32_from_bits(uint32_t b)
{
    float f;
    memcpy(&f, &b, 4);
    return f;
}

static inline uint32_t f32_to_bits(float f)
{
    uint32_t b;
    memcpy(&b, &f, 4);
    return b;
}

/* Round-to-nearest unorm quantisation of a clamped float. */
static inline uint32_t to_unorm(float v, uint32_t maxval)
{
    v = r5xx_clampf(v, 0.0f, 1.0f);
    return (uint32_t)(v * (float)maxval + 0.5f);
}

/* ------------------------------------------------------------------ */
/* Surface layout                                                      */
/* ------------------------------------------------------------------ */

/*
 * Pixel alignment table, verbatim from Mesa r300_texture_desc.c
 * (r300_get_pixel_alignment).  Index: [macrotile][log2 bpp][microtile]
 * [dim]; microtile 0 = linear, 1 = tiled, 2 = square-tiled; dim 0 =
 * width, 1 = height.  A micro tile is always 32 bytes, a macro tile
 * always 2 KiB (8 x 8 micro tiles, or 8 rows of 256 bytes when micro
 * tiling is off).
 */
static const uint32_t tile_table[2][5][3][2] = {
    {
        {{ 32, 1}, { 8,  4}, { 0,  0}}, /*   8 bits per pixel */
        {{ 16, 1}, { 8,  2}, { 4,  4}}, /*  16 bits per pixel */
        {{  8, 1}, { 4,  2}, { 0,  0}}, /*  32 bits per pixel */
        {{  4, 1}, { 2,  2}, { 0,  0}}, /*  64 bits per pixel */
        {{  2, 1}, { 0,  0}, { 0,  0}}  /* 128 bits per pixel */
    },
    {
        {{256, 8}, {64, 32}, { 0,  0}}, /*   8 bits per pixel */
        {{128, 8}, {64, 16}, {32, 32}}, /*  16 bits per pixel */
        {{ 64, 8}, {32, 16}, { 0,  0}}, /*  32 bits per pixel */
        {{ 32, 8}, {16, 16}, { 0,  0}}, /*  64 bits per pixel */
        {{ 16, 8}, { 0,  0}, { 0,  0}}  /* 128 bits per pixel */
    }
};

/* Resolve the micro tiling mode index (0/1/2) for a layout. */
static uint32_t layout_micro_index(uint32_t bpp, R5xxTiling tiling,
                                   uint8_t micro_mode)
{
    uint32_t idx;

    if (tiling != R5XX_TILE_MICRO && tiling != R5XX_TILE_MICRO_MACRO) {
        return 0;
    }
    idx = micro_mode ? micro_mode : (bpp == 2 ? 2 : 1);
    if (idx > 2) {
        idx = 2;
    }
    /* Modes without a table entry (128 bpp, square for non-16bpp) fall
     * back to the valid micro mode for that bpp, or to linear. */
    if (tile_table[0][log2_u32(bpp)][idx][0] == 0) {
        idx = 1;
        if (tile_table[0][log2_u32(bpp)][idx][0] == 0) {
            idx = 0;
        }
    }
    return idx;
}

/* Mesa's r300_get_pixel_alignment without the RS690/scanout special
 * cases (RV530 is neither). */
static uint32_t pixel_alignment(uint32_t bpp, uint32_t micro_idx, bool macro,
                                int dim)
{
    uint32_t l2 = log2_u32(bpp);
    uint32_t v;

    if (l2 > 4) {
        l2 = 4;
    }
    v = tile_table[macro ? 1 : 0][l2][micro_idx][dim];
    return v ? v : 1;
}

bool (*r5xx_linear_hook)(void *opaque, uint32_t gpu_addr);
void *r5xx_linear_hook_opaque;

uint32_t r5xx_surface_pixel_offset(const R5xxSurfaceLayout *l,
                                   uint32_t x, uint32_t y)
{
    if (r5xx_addr_is_linear(l->gpu_addr)) {
        return y * l->pitch_bytes + x * (l->bpp ? l->bpp : 1);
    }
    return r5xx_tiled_pixel_offset(l, x, y);
}

uint32_t r5xx_tiled_pixel_offset(const R5xxSurfaceLayout *l,
                                 uint32_t x, uint32_t y)
{
    uint32_t bpp = l->bpp ? l->bpp : 1;
    bool macro = l->tiling == R5XX_TILE_MACRO ||
                 l->tiling == R5XX_TILE_MICRO_MACRO;
    uint32_t micro = layout_micro_index(bpp, l->tiling, l->micro_mode);
    uint32_t mw, mh;        /* micro tile size in pixels */
    uint32_t off;

    if (l->compressed || (!macro && micro == 0)) {
        /* Linear.  Compressed surfaces are never tiled by Mesa; x,y are
         * block coordinates here. */
        return y * l->pitch_bytes + x * bpp;
    }

    if (micro) {
        mw = pixel_alignment(bpp, micro, false, 0);
        mh = pixel_alignment(bpp, micro, false, 1);
    } else {
        mw = mh = 1;
    }

    if (!macro) {
        /*
         * Micro tiling only: 32-byte tiles of mw x mh pixels, stored
         * row-major inside the tile; tiles run left to right along a
         * row of tiles, and a row of tiles occupies pitch * mh bytes
         * (Mesa classic radeon_tile.c layout).
         */
        uint32_t tx = x / mw, ty = y / mh;
        uint32_t ix = x % mw, iy = y % mh;
        return ty * l->pitch_bytes * mh + tx * 32 + (iy * mw + ix) * bpp;
    } else {
        /*
         * Macro tiling: 2 KiB macro tiles of Mw x Mh pixels, left to
         * right along a macro row of pitch * Mh bytes.  Inside a macro
         * tile: 8 x 8 micro tiles in row-major order when micro tiling is
         * on, otherwise 8 rows of 256 bytes.  ASSUMPTION, see
         * docs/tex_NOTES.md.
         */
        uint32_t Mw = pixel_alignment(bpp, micro, true, 0);
        uint32_t Mh = pixel_alignment(bpp, micro, true, 1);
        uint32_t Mx = x / Mw, My = y / Mh;
        uint32_t ix = x % Mw, iy = y % Mh;

        off = My * l->pitch_bytes * Mh + Mx * 2048;
        if (micro) {
            uint32_t tx = ix / mw, ty = iy / mh;
            uint32_t px = ix % mw, py = iy % mh;
            uint32_t tiles_per_row = Mw / mw;
            off += (ty * tiles_per_row + tx) * 32 + (py * mw + px) * bpp;
        } else {
            off += iy * (Mw * bpp) + ix * bpp;
        }
        return off;
    }
}

/*
 * Number of pixels from (x, y) to the right, in the same row, whose bytes
 * follow each other in memory: r5xx_surface_pixel_offset(l, x + k, y) ==
 * r5xx_surface_pixel_offset(l, x, y) + k * bpp for k < the result.
 * UINT32_MAX means to the end of the row.  Lets a fill handle a run with one
 * address calculation and one map.
 */
uint32_t r5xx_surface_row_run(const R5xxSurfaceLayout *l, uint32_t x, uint32_t y)
{
    uint32_t bpp = l->bpp ? l->bpp : 1;
    bool macro = l->tiling == R5XX_TILE_MACRO ||
                 l->tiling == R5XX_TILE_MICRO_MACRO;
    uint32_t micro = layout_micro_index(bpp, l->tiling, l->micro_mode);
    uint32_t mw, mh, Mw, ix;

    if (l->compressed || (!macro && micro == 0) ||
        r5xx_addr_is_linear(l->gpu_addr)) {
        return UINT32_MAX;
    }
    if (micro) {
        mw = pixel_alignment(bpp, micro, false, 0);
        mh = pixel_alignment(bpp, micro, false, 1);
    } else {
        mw = mh = 1;
    }
    if (!macro) {
        /* micro tiles of one row follow each other in a row of tiles */
        return mh == 1 ? UINT32_MAX : mw - x % mw;
    }
    Mw = pixel_alignment(bpp, micro, true, 0);
    ix = x % Mw;
    if (!micro || mh == 1) {
        return Mw - ix;          /* to the end of this macro tile's row */
    }
    return mw - ix % mw;         /* to the end of this micro tile's row */
}

/* ------------------------------------------------------------------ */
/* Texture format descriptors                                          */
/* ------------------------------------------------------------------ */

typedef enum FmtKind {
    FK_UNORM,       /* packed little-endian integer channels, X at LSB */
    FK_F16,         /* half floats, X first */
    FK_F32,         /* floats, X first */
    FK_DXT1,
    FK_DXT3,
    FK_DXT5,
    FK_ATI1N,       /* BC4: one channel */
    FK_ATI2N,       /* BC5: two channels */
    FK_CXV8U8,      /* X8Y8 signed, Z = sqrt(1 - x^2 - y^2) */
    FK_YUV422,      /* 2 pixels per 4 bytes */
    FK_AYUV444,
    FK_Y8X24,       /* X = low 24 bits, Y = top byte */
    FK_W24_FP,      /* approximated as 24-bit unorm in the low bits */
    FK_UNSUPPORTED,
} FmtKind;

typedef struct FmtInfo {
    const char *name;
    uint8_t bpp;            /* bytes per texel or per block */
    uint8_t kind;
    uint8_t nchan;
    uint8_t bits[4];        /* FK_UNORM: bits per channel X,Y,Z,W */
} FmtInfo;

static const FmtInfo fmt_table[32] = {
    [R300_TX_FORMAT_X8]              = { "X8",              1, FK_UNORM, 1, { 8 } },
    [R300_TX_FORMAT_X16]             = { "X16",             2, FK_UNORM, 1, { 16 } },
    [R300_TX_FORMAT_Y4X4]            = { "Y4X4",            1, FK_UNORM, 2, { 4, 4 } },
    [R300_TX_FORMAT_Y8X8]            = { "Y8X8",            2, FK_UNORM, 2, { 8, 8 } },
    [R300_TX_FORMAT_Y16X16]          = { "Y16X16",          4, FK_UNORM, 2, { 16, 16 } },
    [R300_TX_FORMAT_Z3Y3X2]          = { "Z3Y3X2",          1, FK_UNORM, 3, { 2, 3, 3 } },
    [R300_TX_FORMAT_Z5Y6X5]          = { "Z5Y6X5",          2, FK_UNORM, 3, { 5, 6, 5 } },
    [R300_TX_FORMAT_Z6Y5X5]          = { "Z6Y5X5",          2, FK_UNORM, 3, { 5, 5, 6 } },
    [R300_TX_FORMAT_Z11Y11X10]       = { "Z11Y11X10",       4, FK_UNORM, 3, { 10, 11, 11 } },
    [R300_TX_FORMAT_Z10Y11X11]       = { "Z10Y11X11",       4, FK_UNORM, 3, { 11, 11, 10 } },
    [R300_TX_FORMAT_W4Z4Y4X4]        = { "W4Z4Y4X4",        2, FK_UNORM, 4, { 4, 4, 4, 4 } },
    [R300_TX_FORMAT_W1Z5Y5X5]        = { "W1Z5Y5X5",        2, FK_UNORM, 4, { 5, 5, 5, 1 } },
    [R300_TX_FORMAT_W8Z8Y8X8]        = { "W8Z8Y8X8",        4, FK_UNORM, 4, { 8, 8, 8, 8 } },
    [R300_TX_FORMAT_W2Z10Y10X10]     = { "W2Z10Y10X10",     4, FK_UNORM, 4, { 10, 10, 10, 2 } },
    [R300_TX_FORMAT_W16Z16Y16X16]    = { "W16Z16Y16X16",    8, FK_UNORM, 4, { 16, 16, 16, 16 } },
    [R300_TX_FORMAT_DXT1]            = { "DXT1",            8, FK_DXT1, 4, { 0 } },
    [R300_TX_FORMAT_DXT3]            = { "DXT3",           16, FK_DXT3, 4, { 0 } },
    [R300_TX_FORMAT_DXT5]            = { "DXT5",           16, FK_DXT5, 4, { 0 } },
    [R300_TX_FORMAT_CxV8U8]          = { "CxV8U8",          2, FK_CXV8U8, 3, { 8, 8 } },
    [R300_TX_FORMAT_AVYU444]         = { "AVYU444",         4, FK_AYUV444, 4, { 0 } },
    [R300_TX_FORMAT_VYUY422]         = { "VYUY422",         2, FK_YUV422, 3, { 0 } },
    [R300_TX_FORMAT_YVYU422]         = { "YVYU422",         2, FK_YUV422, 3, { 0 } },
    [R300_TX_FORMAT_16_MPEG]         = { "16_MPEG",         2, FK_UNORM, 1, { 16 } },
    [R300_TX_FORMAT_16_16_MPEG]      = { "16_16_MPEG",      4, FK_UNORM, 2, { 16, 16 } },
    [R300_TX_FORMAT_16F]             = { "16F",             2, FK_F16, 1, { 0 } },
    [R300_TX_FORMAT_16F_16F]         = { "16F_16F",         4, FK_F16, 2, { 0 } },
    [R300_TX_FORMAT_16F_16F_16F_16F] = { "16F_16F_16F_16F", 8, FK_F16, 4, { 0 } },
    [R300_TX_FORMAT_32F]             = { "32F",             4, FK_F32, 1, { 0 } },
    [R300_TX_FORMAT_32F_32F]         = { "32F_32F",         8, FK_F32, 2, { 0 } },
    [R300_TX_FORMAT_32F_32F_32F_32F] = { "32F_32F_32F_32F", 16, FK_F32, 4, { 0 } },
    [R300_TX_FORMAT_W24_FP]          = { "W24_FP",          4, FK_W24_FP, 1, { 0 } },
    [R400_TX_FORMAT_ATI2N]           = { "ATI2N",          16, FK_ATI2N, 2, { 0 } },
};

/* R500 formats selected with R500_TXFORMAT_MSB.  The 10-bit entries are
 * Mesa's guess (8-bit storage, 10-bit filtering); Mesa never emits them. */
static const FmtInfo fmt_table_msb[8] = {
    [R500_TX_FORMAT_X1]           = { "X1",           1, FK_UNSUPPORTED, 1, { 0 } },
    [R500_TX_FORMAT_X1_REV]       = { "X1_REV",       1, FK_UNSUPPORTED, 1, { 0 } },
    [R500_TX_FORMAT_X10]          = { "X10",          1, FK_UNORM, 1, { 8 } },
    [R500_TX_FORMAT_Y10X10]       = { "Y10X10",       2, FK_UNORM, 2, { 8, 8 } },
    [R500_TX_FORMAT_W10Z10Y10X10] = { "W10Z10Y10X10", 4, FK_UNORM, 4, { 8, 8, 8, 8 } },
    [R500_TX_FORMAT_ATI1N]        = { "ATI1N",        8, FK_ATI1N, 1, { 0 } },
    [R500_TX_FORMAT_Y8X24]        = { "Y8X24",        4, FK_Y8X24, 2, { 0 } },
    [7]                           = { "MSB_7",        1, FK_UNSUPPORTED, 0, { 0 } },
};

static const FmtInfo *fmt_info(uint32_t hw_format, bool msb)
{
    if (msb) {
        return &fmt_table_msb[hw_format & 7];
    }
    return &fmt_table[hw_format & 0x1f];
}

const char *r5xx_tex_format_name(uint32_t hw_format, bool msb)
{
    const FmtInfo *fi = fmt_info(hw_format, msb);
    return fi->name ? fi->name : "unknown";
}

static bool fmt_is_compressed(const FmtInfo *fi)
{
    return fi->kind == FK_DXT1 || fi->kind == FK_DXT3 || fi->kind == FK_DXT5 ||
           fi->kind == FK_ATI1N || fi->kind == FK_ATI2N;
}

/* Encode the texel format for the Vulkan backend (r5xx_vk.h RvkSampler.fmt):
 * kind | nchan << 4 | bits[i] << (8 + 5i).  False if the GPU path does not
 * implement it (compressed, packed special formats, cube maps); why, when
 * given, gets the reason. */
bool r5xx_tex_gpu_format(const R5xxTexSampler *s, uint32_t *fmt, char *why, size_t whylen)
{
    const FmtInfo *fi = fmt_info(s->hw_format, s->raw[4] & R500_TXFORMAT_MSB);
    uint32_t v, i, total = 0;
    bool yuv = fi->kind == FK_YUV422 || fi->kind == FK_AYUV444;
    const char *r = !s->valid ? "invalid" : s->is_cube ? "cube" :
                    s->compressed ? "compressed" : (s->yuv_to_rgb && !yuv) ? "YUV_TO_RGB" :
                    s->depth_compare ? "depth compare" :
                    (s->bpp == 0 || s->bpp > 16) ? "texel size" : NULL;

    if (!r) {
        switch (fi->kind) {
        case FK_UNORM: case FK_F16: case FK_F32: case FK_YUV422: case FK_AYUV444:
            break;
        default:
            r = "format";
            break;
        }
    }
    if (r) {
        if (why) {
            snprintf(why, whylen, "texture %s (format 0x%02x %s) not supported on the GPU path",
                     r, s->hw_format, fi->name ? fi->name : "?");
        }
        return false;
    }
    switch (fi->kind) {
    case FK_UNORM:
        v = 0;
        for (i = 0; i < 4; i++) {
            if (fi->bits[i] > 31) {
                return false;
            }
            total += fi->bits[i];
            v |= (uint32_t)fi->bits[i] << (8 + 5 * i);
        }
        if (total > 64) {
            return false;
        }
        break;
    case FK_F16:
        v = 1;
        break;
    case FK_F32:
        v = 2;
        break;
    case FK_YUV422:
        v = 3;
        break;
    case FK_AYUV444:
        v = 4;
        break;
    default:
        return false;
    }
    *fmt = v | ((uint32_t)fi->nchan << 4);
    return true;
}

/* ------------------------------------------------------------------ */
/* Block decompression                                                 */
/* ------------------------------------------------------------------ */

static void rgb565_to_f(uint32_t c, float *r, float *g, float *b)
{
    *r = ((c >> 11) & 31) / 31.0f;
    *g = ((c >> 5) & 63) / 63.0f;
    *b = (c & 31) / 31.0f;
}

/* Decode a DXT color block (8 bytes) at texel (sx, sy).  dxt1_mode
 * enables the 3-color + transparent mode when c0 <= c1. */
static void dxt_color_block(const uint8_t *p, int sx, int sy, bool dxt1_mode,
                            float out[4])
{
    uint32_t c0 = rd16(p), c1 = rd16(p + 2);
    float r0, g0, b0, r1, g1, b1;
    uint32_t idx = (p[4 + sy] >> (2 * sx)) & 3;

    rgb565_to_f(c0, &r0, &g0, &b0);
    rgb565_to_f(c1, &r1, &g1, &b1);
    out[3] = 1.0f;
    switch (idx) {
    case 0:
        out[0] = r0; out[1] = g0; out[2] = b0;
        break;
    case 1:
        out[0] = r1; out[1] = g1; out[2] = b1;
        break;
    case 2:
        if (!dxt1_mode || c0 > c1) {
            out[0] = (2 * r0 + r1) / 3; out[1] = (2 * g0 + g1) / 3;
            out[2] = (2 * b0 + b1) / 3;
        } else {
            out[0] = (r0 + r1) / 2; out[1] = (g0 + g1) / 2;
            out[2] = (b0 + b1) / 2;
        }
        break;
    default:
        if (!dxt1_mode || c0 > c1) {
            out[0] = (r0 + 2 * r1) / 3; out[1] = (g0 + 2 * g1) / 3;
            out[2] = (b0 + 2 * b1) / 3;
        } else {
            out[0] = out[1] = out[2] = 0.0f;
            out[3] = 0.0f;
        }
        break;
    }
}

/* DXT5 alpha / BC4 block (8 bytes), unorm result. */
static float dxt5_alpha_block(const uint8_t *p, int sx, int sy)
{
    uint32_t a0 = p[0], a1 = p[1];
    uint64_t bits = 0;
    int i;
    uint32_t idx;

    for (i = 0; i < 6; i++) {
        bits |= (uint64_t)p[2 + i] << (8 * i);
    }
    idx = (bits >> (3 * (sy * 4 + sx))) & 7;
    if (idx == 0) {
        return a0 / 255.0f;
    }
    if (idx == 1) {
        return a1 / 255.0f;
    }
    if (a0 > a1) {
        return ((8 - idx) * a0 + (idx - 1) * a1) / (7.0f * 255.0f);
    }
    if (idx == 6) {
        return 0.0f;
    }
    if (idx == 7) {
        return 1.0f;
    }
    return ((6 - idx) * a0 + (idx - 1) * a1) / (5.0f * 255.0f);
}

/* Signed variant (RGTC SNORM): bytes are int8, result in [-1, 1]. */
static float dxt5_alpha_block_snorm(const uint8_t *p, int sx, int sy)
{
    int a0 = (int8_t)p[0], a1 = (int8_t)p[1];
    uint64_t bits = 0;
    int i;
    uint32_t idx;
    float v;

    for (i = 0; i < 6; i++) {
        bits |= (uint64_t)p[2 + i] << (8 * i);
    }
    idx = (bits >> (3 * (sy * 4 + sx))) & 7;
    if (idx == 0) {
        v = a0;
    } else if (idx == 1) {
        v = a1;
    } else if (a0 > a1) {
        v = ((8 - (int)idx) * a0 + ((int)idx - 1) * a1) / 7.0f;
    } else if (idx == 6) {
        return -1.0f;
    } else if (idx == 7) {
        return 1.0f;
    } else {
        v = ((6 - (int)idx) * a0 + ((int)idx - 1) * a1) / 5.0f;
    }
    v /= 127.0f;
    return v < -1.0f ? -1.0f : v;
}

/* ------------------------------------------------------------------ */
/* Texel unpacking                                                     */
/* ------------------------------------------------------------------ */

static void yuv_to_rgb(float y, float u, float v, float *r, float *g, float *b)
{
    u -= 0.5f;
    v -= 0.5f;
    *r = r5xx_clampf(y + 1.402f * v, 0.0f, 1.0f);
    *g = r5xx_clampf(y - 0.344136f * u - 0.714136f * v, 0.0f, 1.0f);
    *b = r5xx_clampf(y + 1.772f * u, 0.0f, 1.0f);
}

/*
 * Unpack the raw X,Y,Z,W channels of one texel.  p points at the texel
 * (or its block / 422 pair); sx, sy are the position inside the block
 * (sx = x & 1 for 422).  Applies the sign bits and gamma, but not the
 * output swizzle.
 */
static void unpack_raw(const R5xxTexSampler *s, const FmtInfo *fi,
                       const uint8_t *p, int sx, int sy, float raw[4])
{
    int i;

    raw[0] = 0.0f;
    raw[1] = 0.0f;
    raw[2] = 0.0f;
    raw[3] = 1.0f;

    switch (fi->kind) {
    case FK_UNORM:
    case FK_CXV8U8: {
        uint64_t bits = 0;
        uint32_t shift = 0;
        for (i = 0; i < fi->bpp; i++) {
            bits |= (uint64_t)p[i] << (8 * i);
        }
        for (i = 0; i < fi->nchan && fi->bits[i]; i++) {
            uint32_t nb = fi->bits[i];
            uint32_t v = (uint32_t)(bits >> shift) & ((1u << nb) - 1);
            shift += nb;
            if ((s->signed_mask >> i) & 1) {
                int32_t sv = (int32_t)(v << (32 - nb)) >> (32 - nb);
                float f = (float)sv / (float)((1u << (nb - 1)) - 1);
                raw[i] = f < -1.0f ? -1.0f : f;
            } else {
                raw[i] = (float)v / (float)((1u << nb) - 1);
            }
        }
        if (fi->kind == FK_CXV8U8) {
            float d = 1.0f - raw[0] * raw[0] - raw[1] * raw[1];
            raw[2] = d > 0.0f ? sqrtf(d) : 0.0f;
        }
        break;
    }
    case FK_F16:
        for (i = 0; i < fi->nchan; i++) {
            raw[i] = r5xx_half_to_float((uint16_t)rd16(p + 2 * i));
        }
        break;
    case FK_F32:
        for (i = 0; i < fi->nchan; i++) {
            raw[i] = f32_from_bits(rd32(p + 4 * i));
        }
        break;
    case FK_DXT1:
        dxt_color_block(p, sx, sy, true, raw);
        break;
    case FK_DXT3:
        dxt_color_block(p + 8, sx, sy, false, raw);
        raw[3] = ((rd16(p + 2 * sy) >> (4 * sx)) & 0xf) / 15.0f;
        break;
    case FK_DXT5:
        dxt_color_block(p + 8, sx, sy, false, raw);
        raw[3] = dxt5_alpha_block(p, sx, sy);
        break;
    case FK_ATI1N:
        raw[0] = (s->signed_mask & 1) ? dxt5_alpha_block_snorm(p, sx, sy)
                                      : dxt5_alpha_block(p, sx, sy);
        break;
    case FK_ATI2N:
        raw[0] = (s->signed_mask & 1) ? dxt5_alpha_block_snorm(p, sx, sy)
                                      : dxt5_alpha_block(p, sx, sy);
        raw[1] = (s->signed_mask & 2) ? dxt5_alpha_block_snorm(p + 8, sx, sy)
                                      : dxt5_alpha_block(p + 8, sx, sy);
        break;
    case FK_YUV422: {
        /* YVYU422 (0x15): bytes U Y0 V Y1 ('2vuy', PIPE_FORMAT_UYVY):
         * QuickTime's frames in iMovie, which Apple's driver uploads as
         * GL_YCBCR_422_APPLE / UNSIGNED_SHORT_8_8_APPLE.
         * VYUY422 (0x14): bytes Y0 U Y1 V ('yuvs', PIPE_FORMAT_YUYV).
         * Raw channels: Z = U, X = V, Y = luma, so that Mesa's (B=X, G=Y,
         * R=Z) swizzle yields RGB.  With YUV_TO_RGB the converted color is
         * placed in the same slots. */
        float cu, cv, cy;
        if (s->hw_format == R300_TX_FORMAT_YVYU422) {
            cu = p[0] / 255.0f;
            cv = p[2] / 255.0f;
            cy = (sx ? p[3] : p[1]) / 255.0f;
        } else {
            cu = p[1] / 255.0f;
            cv = p[3] / 255.0f;
            cy = (sx ? p[2] : p[0]) / 255.0f;
        }
        if (s->yuv_to_rgb) {
            float r, g, b;
            yuv_to_rgb(cy, cu, cv, &r, &g, &b);
            raw[0] = b;
            raw[1] = g;
            raw[2] = r;
        } else {
            raw[0] = cv;
            raw[1] = cy;
            raw[2] = cu;
        }
        break;
    }
    case FK_AYUV444: {
        float cu = p[0] / 255.0f, cy = p[1] / 255.0f, cv = p[2] / 255.0f;
        raw[3] = p[3] / 255.0f;
        if (s->yuv_to_rgb) {
            float r, g, b;
            yuv_to_rgb(cy, cu, cv, &r, &g, &b);
            raw[0] = b;
            raw[1] = g;
            raw[2] = r;
        } else {
            raw[0] = cv;
            raw[1] = cy;
            raw[2] = cu;
        }
        break;
    }
    case FK_Y8X24: {
        uint32_t v = rd32(p);
        raw[0] = (v & 0xffffff) / 16777215.0f;
        raw[1] = (v >> 24) / 255.0f;
        break;
    }
    case FK_W24_FP: {
        uint32_t v = rd32(p);
        raw[0] = (v & 0xffffff) / 16777215.0f;
        break;
    }
    default:
        break;
    }

    if (s->gamma && (fi->kind == FK_UNORM || fi->kind == FK_DXT1 ||
                     fi->kind == FK_DXT3 || fi->kind == FK_DXT5)) {
        for (i = 0; i < 3; i++) {
            if (raw[i] > 0.0f) {
                raw[i] = srgb_to_linear(raw[i]);
            }
        }
    }
}

static void apply_swizzle(const R5xxTexSampler *s, const float raw_in[4],
                          r5xx_vec4 *out)
{
    float raw[4];
    int i;

    memcpy(raw, raw_in, sizeof(raw));
    if (s->dxtc_swizzle && (s->hw_format == R300_TX_FORMAT_DXT1 ||
                            s->hw_format == R300_TX_FORMAT_DXT3 ||
                            s->hw_format == R300_TX_FORMAT_DXT5) &&
        !(s->raw[4] & R500_TXFORMAT_MSB)) {
        float t = raw[0];
        raw[0] = raw[2];
        raw[2] = t;
    }
    for (i = 0; i < 4; i++) {
        uint32_t sel = s->swizzle[i];
        float v;
        switch (sel) {
        case R300_TX_FORMAT_SEL_X:
        case R300_TX_FORMAT_SEL_Y:
        case R300_TX_FORMAT_SEL_Z:
        case R300_TX_FORMAT_SEL_W:
            v = raw[sel];
            break;
        case R300_TX_FORMAT_SEL_ZERO:
            v = 0.0f;
            break;
        case R300_TX_FORMAT_SEL_ONE:
            v = 1.0f;
            break;
        case R300_TX_FORMAT_SEL_CUT_Z:
            v = 2.0f * raw[2];
            if (v > 1.0f) {
                v = 0.0f;
            }
            break;
        default: /* CUT_W */
            v = 2.0f * raw[3];
            if (v > 1.0f) {
                v = 0.0f;
            }
            break;
        }
        out->f[i] = v;
    }
}

/* ------------------------------------------------------------------ */
/* Mip chain layout (Mesa r300_texture_desc.c replica)                 */
/* ------------------------------------------------------------------ */

static R5xxTiling tiling_from_bits(bool macro, uint32_t micro_bits)
{
    if (macro) {
        return micro_bits ? R5XX_TILE_MICRO_MACRO : R5XX_TILE_MACRO;
    }
    return micro_bits ? R5XX_TILE_MICRO : R5XX_TILE_LINEAR;
}

static void setup_miptree(R5xxTexSampler *s)
{
    bool base_macro = s->layout.tiling == R5XX_TILE_MACRO ||
                      s->layout.tiling == R5XX_TILE_MICRO_MACRO;
    uint32_t micro_bits = s->layout.micro_mode;
    uint32_t micro_idx = layout_micro_index(s->bpp, s->layout.tiling,
                                            s->layout.micro_mode);
    bool pot_height = s->is_cube || s->is_3d || s->num_levels > 1;
    uint32_t size = 0;
    uint32_t i;

    for (i = 0; i < 16; i++) {
        uint32_t w = r5xx_minify(s->width, i);
        uint32_t h = r5xx_minify(s->height, i);
        uint32_t d = s->is_3d ? r5xx_minify(s->depth, i) : 1;
        bool macro;
        uint32_t stride, nblocksy, layer, lvl_size;

        /* r300_texture_macro_switch, rv350 mode (>=), for the mip levels:
         * the base level keeps the macro tiling TX_OFFSET asks for.  Mesa
         * never macro-tiles a base level smaller than a tile, but Apple's
         * driver does (the 7x22 text caret is rendered macro-tiled and then
         * sampled), and the render backend has no such switch. */
        macro = base_macro && !s->compressed &&
                (i == 0 ||
                 (w >= pixel_alignment(s->bpp, micro_idx, true, 0) &&
                  h >= pixel_alignment(s->bpp, micro_idx, true, 1)));

        if (!s->compressed) {
            uint32_t tw = pixel_alignment(s->bpp, micro_idx, macro, 0);
            uint32_t th = pixel_alignment(s->bpp, micro_idx, macro, 1);
            stride = align_u32(w, tw) * s->bpp;
            if (pot_height) {
                h = next_pow2(h);
            }
            nblocksy = align_u32(h, th);
        } else {
            stride = align_u32((w + s->block_w - 1) / s->block_w * s->bpp, 32);
            if (pot_height) {
                h = next_pow2(h);
            }
            nblocksy = (h + s->block_h - 1) / s->block_h;
        }
        if (i == 0 && s->pitch_en) {
            stride = s->layout.pitch_bytes;
        } else if (i == 0 && !s->compressed && !is_pow2(w) &&
                   s->layout.tiling != R5XX_TILE_LINEAR) {
            /* A tiled NPOT texture without TXPITCH_EN has the pitch of
             * the next power of two: Lion's driver blits the 160-wide
             * last column of the screen-size background (256-wide
             * tiles) micro-tiled at 256 pixels and samples it so.  Mesa
             * always sets TXPITCH_EN for NPOT textures. */
            if (next_pow2(w) * s->bpp > stride) {
                stride = next_pow2(w) * s->bpp;
            }
        }

        layer = stride * nblocksy;
        lvl_size = layer * (s->is_cube ? 6 : d);

        s->level_offset[i] = size;
        s->level_pitch[i] = stride;
        s->level_layer_size[i] = layer;
        s->level_tiling[i] = (uint8_t)tiling_from_bits(macro, micro_bits);
        size += lvl_size;
    }
    s->face_offset_stride = s->level_layer_size[0];
    s->layout.pitch_bytes = s->level_pitch[0];
    s->layout.tiling = (R5xxTiling)s->level_tiling[0];
}

void r5xx_tex_level_layout(const R5xxTexSampler *s, uint32_t level,
                           uint32_t layer, R5xxSurfaceLayout *l)
{
    if (level > 15) {
        level = 15;
    }
    *l = s->layout;
    l->gpu_addr = s->layout.gpu_addr + s->level_offset[level] +
                  layer * s->level_layer_size[level];
    l->pitch_bytes = s->level_pitch[level];
    l->width = r5xx_minify(s->width, level);
    l->height = r5xx_minify(s->height, level);
    l->tiling = (R5xxTiling)s->level_tiling[level];
}

/* ------------------------------------------------------------------ */
/* Register decoding                                                   */
/* ------------------------------------------------------------------ */

static void decode_border(R5xxTexSampler *s, const FmtInfo *fi, uint32_t reg)
{
    uint8_t bytes[16];
    float raw[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    R5xxTexSampler tmp = *s;     /* for the 8888 reinterpretation cases */
    const FmtInfo *use = fi;
    int i;

    memset(bytes, 0, sizeof(bytes));
    wr32(bytes, reg);

    switch (fi->kind) {
    case FK_DXT1:
    case FK_DXT3:
    case FK_DXT5:
        /* Mesa packs B8G8R8A8: byte 0 is X (B with the DXTC swizzle). */
        use = &fmt_table[R300_TX_FORMAT_W8Z8Y8X8];
        tmp.signed_mask = 0;
        unpack_raw(&tmp, use, bytes, 0, 0, raw);
        break;
    case FK_ATI1N:
        /* B4G4R4A4 with the value in the G nibble (Mesa quirk). */
        raw[0] = ((reg >> 4) & 0xf) / 15.0f;
        if (s->signed_mask & 1) {
            raw[0] = raw[0] * 2.0f - 1.0f;
        }
        break;
    case FK_ATI2N:
        use = &fmt_table[R300_TX_FORMAT_Y8X8];
        unpack_raw(&tmp, use, bytes, 0, 0, raw);
        break;
    case FK_F16:
    case FK_F32:
    case FK_UNORM:
    case FK_CXV8U8:
        if (fi->bpp > 4 || (fi->kind == FK_F32 && fi->nchan > 1)) {
            /* 4x16 and 2/4x32 formats: packed as R8G8B8A8 (U/SNORM). */
            use = &fmt_table[R300_TX_FORMAT_W8Z8Y8X8];
            if (fi->kind != FK_UNORM) {
                tmp.signed_mask = 0;
            }
        }
        unpack_raw(&tmp, use, bytes, 0, 0, raw);
        break;
    case FK_Y8X24:
    case FK_W24_FP:
        unpack_raw(&tmp, use, bytes, 0, 0, raw);
        break;
    case FK_YUV422:
    case FK_AYUV444:
        use = &fmt_table[R300_TX_FORMAT_W8Z8Y8X8];
        unpack_raw(&tmp, use, bytes, 0, 0, raw);
        break;
    default:
        break;
    }
    for (i = 0; i < 4; i++) {
        if (raw[i] != raw[i]) {
            raw[i] = 0.0f;
        }
    }
    apply_swizzle(s, raw, &s->border_color);
}

static void set_err(char *err, size_t errlen, const char *msg)
{
    if (err && errlen) {
        snprintf(err, errlen, "%s", msg);
    }
}

bool r5xx_tex_decode(const R5xxTexRegs *regs, bool is_r500,
                     R5xxTexSampler *out, char *err, size_t errlen)
{
    const FmtInfo *fi;
    uint32_t f0 = regs->format0, f1 = regs->format1, f2 = regs->format2;
    uint32_t fl0 = regs->filter0, fl1 = regs->filter1;
    uint32_t coord_type, micro_bits;
    bool msb, ok = true;
    int32_t lod_raw;
    char msg[96];

    memset(out, 0, sizeof(*out));
    out->raw[0] = regs->filter0;
    out->raw[1] = regs->filter1;
    out->raw[2] = regs->format0;
    out->raw[3] = regs->format1;
    out->raw[4] = regs->format2;
    out->raw[5] = regs->offset;
    out->raw[6] = regs->chroma_key;
    out->raw[7] = regs->border_color;
    if (err && errlen) {
        err[0] = 0;
    }

    /* --- geometry --- */
    out->width = (f0 & R300_TX_WIDTHMASK_MASK) + 1;
    out->height = ((f0 >> R300_TX_HEIGHTMASK_SHIFT) & 0x7ff) + 1;
    out->depth = 1u << ((f0 >> R300_TX_DEPTHMASK_SHIFT) & 0xf);
    out->num_levels = ((f0 >> R300_TX_NUM_LEVELS_SHIFT) & 0xf) + 1;
    out->pitch_en = (f0 & R300_TX_PITCH_EN) != 0;
    msb = is_r500 && (f2 & R500_TXFORMAT_MSB);
    if (is_r500) {
        if (f2 & R500_TXWIDTH_BIT11) {
            out->width += 2048;
        }
        if (f2 & R500_TXHEIGHT_BIT11) {
            out->height += 2048;
        }
    } else {
        out->raw[4] &= ~(uint32_t)R500_TXFORMAT_MSB;
    }
    coord_type = (f1 >> R300_TX_FORMAT_TEX_COORD_SHIFT) & 3;
    out->is_3d = coord_type == 1;
    out->is_cube = coord_type == 2;
    if (!out->is_3d) {
        out->depth = 1;
    }
    out->is_rect = false;   /* no sampler-level flag; R500_TEX_UNSCALED is per instruction */
    out->is_npot = out->pitch_en || !is_pow2(out->width) || !is_pow2(out->height);

    /* --- format --- */
    out->hw_format = f1 & 0x1f;
    fi = fmt_info(out->hw_format, msb);
    out->bpp = fi->bpp;
    out->compressed = fmt_is_compressed(fi);
    out->block_w = out->block_h = out->compressed ? 4 : 1;
    out->swizzle[0] = (f1 >> R300_TX_FORMAT_R_SHIFT) & 7;
    out->swizzle[1] = (f1 >> R300_TX_FORMAT_G_SHIFT) & 7;
    out->swizzle[2] = (f1 >> R300_TX_FORMAT_B_SHIFT) & 7;
    out->swizzle[3] = (f1 >> R300_TX_FORMAT_A_SHIFT) & 7;
    out->signed_mask = ((f1 & R300_TX_FORMAT_SIGNED_X) ? 1 : 0) |
                       ((f1 & R300_TX_FORMAT_SIGNED_Y) ? 2 : 0) |
                       ((f1 & R300_TX_FORMAT_SIGNED_Z) ? 4 : 0) |
                       ((f1 & R300_TX_FORMAT_SIGNED_W) ? 8 : 0);
    out->is_signed = out->signed_mask != 0;
    out->is_float = fi->kind == FK_F16 || fi->kind == FK_F32;
    out->is_depth = fi->kind == FK_Y8X24 || fi->kind == FK_W24_FP;
    out->gamma = (f1 & R300_TX_FORMAT_GAMMA) != 0;
    out->yuv_to_rgb = (f1 & R300_TX_FORMAT_YUV_TO_RGB) != 0;
    if (out->yuv_to_rgb && (fi->kind == FK_YUV422 || fi->kind == FK_AYUV444)) {
        /* the converted color bypasses the RGB swizzle: Apple's driver
         * programs X, X, X, ONE here and still gets color */
        out->swizzle[0] = R300_TX_FORMAT_SEL_Z;
        out->swizzle[1] = R300_TX_FORMAT_SEL_Y;
        out->swizzle[2] = R300_TX_FORMAT_SEL_X;
    }
    out->dxtc_swizzle = (fl1 & R400_DXTC_SWIZZLE_ENABLE) != 0;

    if (fi->kind == FK_UNSUPPORTED || !fi->name) {
        snprintf(msg, sizeof(msg), "unsupported texture format 0x%x%s",
                 out->hw_format, msb ? " (MSB)" : "");
        set_err(err, errlen, msg);
        ok = false;
    }

    /* --- layout --- */
    out->layout.gpu_addr = regs->offset & R300_TXO_OFFSET_MASK;
    out->endian = regs->offset & 3;
    out->layout.width = out->width;
    out->layout.height = out->height;
    out->layout.bpp = out->bpp;
    out->layout.compressed = out->compressed;
    out->layout.is_r500 = is_r500;
    micro_bits = (regs->offset >> R300_TXO_MICRO_TILE_SHIFT) & 3;
    if (micro_bits > 2) {
        micro_bits = 2;
    }
    out->layout.micro_mode = (uint8_t)micro_bits;
    out->layout.tiling = tiling_from_bits((regs->offset >> R300_TXO_MACRO_TILE_SHIFT) & 1,
                                          micro_bits);
    if (out->compressed) {
        /* Mesa never tiles compressed textures; the TX unit addresses
         * them linearly in blocks. */
        out->layout.tiling = R5XX_TILE_LINEAR;
    }
    if (out->pitch_en) {
        uint32_t pitch_px = (f2 & R300_TX_PITCHMASK_MASK) + 1;
        if (out->compressed) {
            out->layout.pitch_bytes = (pitch_px / out->block_w) * out->bpp;
        } else {
            out->layout.pitch_bytes = pitch_px * out->bpp;
        }
    }
    setup_miptree(out);

    /* --- sampling --- */
    out->wrap_s = (R5xxWrap)((fl0 >> R300_TX_WRAP_S_SHIFT) & 7);
    out->wrap_t = (R5xxWrap)((fl0 >> R300_TX_WRAP_T_SHIFT) & 7);
    out->wrap_r = (R5xxWrap)((fl0 >> R300_TX_WRAP_R_SHIFT) & 7);
    switch ((fl0 >> R300_TX_MAG_FILTER_SHIFT) & 3) {
    case 1:  out->mag_filter = R5XX_FILTER_NEAREST; break;
    case 2:  out->mag_filter = R5XX_FILTER_LINEAR; break;
    case 3:  out->mag_filter = R5XX_FILTER_LINEAR; break;  /* ANISO */
    default: out->mag_filter = R5XX_FILTER_NONE; break;    /* FILTER4 */
    }
    switch ((fl0 >> R300_TX_MIN_FILTER_SHIFT) & 3) {
    case 1:  out->min_filter = R5XX_FILTER_NEAREST; break;
    case 2:  out->min_filter = R5XX_FILTER_LINEAR; break;
    case 3:  out->min_filter = R5XX_FILTER_LINEAR; break;  /* ANISO */
    default: out->min_filter = R5XX_FILTER_NONE; break;
    }
    switch ((fl0 >> R300_TX_MIP_FILTER_SHIFT) & 3) {
    case 1:  out->mip_filter = R5XX_FILTER_NEAREST; break;
    case 2:  out->mip_filter = R5XX_FILTER_LINEAR; break;
    default: out->mip_filter = R5XX_FILTER_NONE; break;
    }
    out->min_lod = (float)((fl0 >> R300_TX_MAX_MIP_LEVEL_SHIFT) & 0xf);
    out->max_lod = (float)(out->num_levels - 1);
    if (out->min_lod > out->max_lod) {
        out->min_lod = out->max_lod;
    }
    out->anisotropy = 1u << ((fl0 >> R300_TX_MAX_ANISO_SHIFT) & 7);
    if (out->anisotropy > 16) {
        out->anisotropy = 16;
    }
    if (is_r500 && (fl1 & R500_TX_MAX_ANISO_MASK)) {
        /* r500_anisotropy(): (max_aniso - 1) * 4.2001, clamped to 63 */
        uint32_t a = (fl1 & R500_TX_MAX_ANISO_MASK) >> R500_TX_MAX_ANISO_SHIFT;
        uint32_t v = (uint32_t)((float)a / 4.2001f + 0.5f) + 1;
        if (v > out->anisotropy) {
            out->anisotropy = v > 16 ? 16 : v;
        }
    }
    lod_raw = (int32_t)(fl1 & R300_LOD_BIAS_MASK) >> R300_LOD_BIAS_SHIFT;
    if (lod_raw & 0x200) {
        lod_raw -= 0x400;
    }
    out->lod_bias = (float)lod_raw / 32.0f;
    out->depth_compare = false;    /* Mesa does the compare in the shader */
    out->depth_compare_func = 7;   /* R300_ZS_ALWAYS */

    decode_border(out, fi, regs->border_color);
    out->valid = ok;
    return ok;
}

/* ------------------------------------------------------------------ */
/* Texel fetch                                                         */
/* ------------------------------------------------------------------ */

static void fetch_raw(const R5xxTexSampler *s, const R5xxMem *mem,
                      uint32_t level, uint32_t layer, int32_t x, int32_t y,
                      float raw[4])
{
    const FmtInfo *fi = fmt_info(s->hw_format, s->raw[4] & R500_TXFORMAT_MSB);
    R5xxSurfaceLayout l;
    uint8_t buf[16];
    uint32_t off, len;
    int sx = 0, sy = 0;

    r5xx_tex_level_layout(s, level, layer, &l);
    if (s->compressed) {
        sx = x & 3;
        sy = y & 3;
        off = r5xx_surface_pixel_offset(&l, (uint32_t)x >> 2, (uint32_t)y >> 2);
        len = fi->bpp;
    } else if (fi->kind == FK_YUV422) {
        sx = x & 1;
        off = r5xx_surface_pixel_offset(&l, (uint32_t)x & ~1u, (uint32_t)y);
        len = 4;
    } else {
        off = r5xx_surface_pixel_offset(&l, (uint32_t)x, (uint32_t)y);
        len = fi->bpp;
    }
    memset(buf, 0, sizeof(buf));
    if (s->endian) {
        /* the swap works on whole dwords: read the ones holding the texel */
        uint32_t a = (l.gpu_addr + off) & ~3u, skip = (l.gpu_addr + off) & 3;
        uint32_t nw = (skip + len + 3) / 4, i;
        uint8_t w[20];
        if (nw > 5) {
            nw = 5;
        }
        mem->read(mem->opaque, a, w, 4 * nw);
        for (i = 0; i < nw; i++) {
            wr32(w + 4 * i, r5xx_endian_swap(s->endian, rd32(w + 4 * i)));
        }
        memcpy(buf, w + skip, len < sizeof(buf) ? len : sizeof(buf));
    } else {
        mem->read(mem->opaque, l.gpu_addr + off, buf, len);
    }
    unpack_raw(s, fi, buf, sx, sy, raw);
}

void r5xx_tex_fetch_texel(const R5xxTexSampler *s, const R5xxMem *mem,
                          uint32_t level, uint32_t face, uint32_t slice,
                          int32_t x, int32_t y, r5xx_vec4 *out)
{
    float raw[4];
    uint32_t lw, lh, ld, layer;

    if (!s->valid) {
        *out = s->border_color;
        return;
    }
    if (level >= s->num_levels) {
        level = s->num_levels - 1;
    }
    if (level > 15) {
        level = 15;
    }
    lw = r5xx_minify(s->width, level);
    lh = r5xx_minify(s->height, level);
    ld = s->is_3d ? r5xx_minify(s->depth, level) : 1;
    if (x < 0) {
        x = 0;
    }
    if ((uint32_t)x >= lw) {
        x = (int32_t)lw - 1;
    }
    if (y < 0) {
        y = 0;
    }
    if ((uint32_t)y >= lh) {
        y = (int32_t)lh - 1;
    }
    if (s->is_cube) {
        layer = face > 5 ? 5 : face;
    } else if (s->is_3d) {
        layer = slice >= ld ? ld - 1 : slice;
    } else {
        layer = 0;
    }
    fetch_raw(s, mem, level, layer, x, y, raw);
    apply_swizzle(s, raw, out);
}

/* ------------------------------------------------------------------ */
/* Wrap modes                                                          */
/* ------------------------------------------------------------------ */

/* Nearest: returns the texel index, or -1 / size for the border. */
static int wrap_nearest(R5xxWrap mode, float s, int size)
{
    float fsize = (float)size;
    int i;

    switch (mode) {
    case R5XX_WRAP_REPEAT:
        i = ifloor(s * fsize);
        i %= size;
        if (i < 0) {
            i += size;
        }
        return i;
    case R5XX_WRAP_MIRROR: {
        int flr = ifloor(s);
        float u = fracf(s);
        if (flr & 1) {
            u = 1.0f - u;
        }
        i = ifloor(u * fsize);
        if (i < 0) {
            i = 0;
        } else if (i >= size) {
            i = size - 1;
        }
        return i;
    }
    case R5XX_WRAP_MIRROR_CLAMP_LAST_TEXEL:
        s = fabsf(s);
        /* fall through */
    case R5XX_WRAP_CLAMP_LAST_TEXEL:
        i = ifloor(s * fsize);
        if (i < 0) {
            i = 0;
        } else if (i >= size) {
            i = size - 1;
        }
        return i;
    case R5XX_WRAP_MIRROR_CLAMP_HALF_BORDER:
        s = fabsf(s);
        /* fall through */
    case R5XX_WRAP_CLAMP_HALF_BORDER:
        /* GL_CLAMP: s in [0,1]; nearest never reaches the border. */
        s *= fsize;
        if (s <= 0.0f) {
            return 0;
        } else if (s >= fsize) {
            return size - 1;
        }
        return ifloor(s);
    case R5XX_WRAP_MIRROR_CLAMP_BORDER:
        s = fabsf(s);
        /* fall through */
    case R5XX_WRAP_CLAMP_BORDER:
    default: {
        float min = -1.0f / (2.0f * fsize);
        float max = 1.0f - min;
        if (s <= min) {
            return -1;
        } else if (s >= max) {
            return size;
        }
        return ifloor(s * fsize);
    }
    }
}

/* Linear: two texel indices (-1 / size mean border) and the weight of
 * the second. */
static void wrap_linear(R5xxWrap mode, float s, int size, int *i0, int *i1,
                        float *w)
{
    float fsize = (float)size;
    float u;

    switch (mode) {
    case R5XX_WRAP_REPEAT:
        u = s * fsize - 0.5f;
        *i0 = ifloor(u);
        *w = fracf(u);
        *i0 %= size;
        if (*i0 < 0) {
            *i0 += size;
        }
        *i1 = (*i0 + 1) % size;
        return;
    case R5XX_WRAP_MIRROR: {
        int flr = ifloor(s);
        u = fracf(s);
        if (flr & 1) {
            u = 1.0f - u;
        }
        u = u * fsize - 0.5f;
        *i0 = ifloor(u);
        *i1 = *i0 + 1;
        *w = fracf(u);
        if (*i0 < 0) {
            *i0 = 0;
        }
        if (*i1 >= size) {
            *i1 = size - 1;
        }
        return;
    }
    case R5XX_WRAP_MIRROR_CLAMP_LAST_TEXEL:
        s = fabsf(s);
        /* fall through */
    case R5XX_WRAP_CLAMP_LAST_TEXEL:
        s = r5xx_clampf(s, 0.0f, 1.0f);
        u = s * fsize - 0.5f;
        *i0 = ifloor(u);
        *i1 = *i0 + 1;
        *w = fracf(u);
        if (*i0 < 0) {
            *i0 = 0;
        }
        if (*i1 >= size) {
            *i1 = size - 1;
        }
        return;
    case R5XX_WRAP_MIRROR_CLAMP_HALF_BORDER:
        s = fabsf(s);
        /* fall through */
    case R5XX_WRAP_CLAMP_HALF_BORDER:
        s = r5xx_clampf(s, 0.0f, 1.0f);
        u = s * fsize - 0.5f;
        *i0 = ifloor(u);
        *i1 = *i0 + 1;
        *w = fracf(u);
        return;
    case R5XX_WRAP_MIRROR_CLAMP_BORDER:
        s = fabsf(s);
        /* fall through */
    case R5XX_WRAP_CLAMP_BORDER:
    default: {
        float min = -1.0f / (2.0f * fsize);
        float max = 1.0f - min;
        s = r5xx_clampf(s, min, max);
        u = s * fsize - 0.5f;
        *i0 = ifloor(u);
        *i1 = *i0 + 1;
        *w = fracf(u);
        return;
    }
    }
}

/* ------------------------------------------------------------------ */
/* Filtered sampling                                                   */
/* ------------------------------------------------------------------ */

static void fetch_or_border(const R5xxTexSampler *s, const R5xxMem *mem,
                            uint32_t level, uint32_t face, int slice,
                            int x, int y, int lw, int lh, int ld,
                            r5xx_vec4 *out)
{
    if (x < 0 || x >= lw || y < 0 || y >= lh || slice < 0 || slice >= ld) {
        *out = s->border_color;
        return;
    }
    r5xx_tex_fetch_texel(s, mem, level, face, (uint32_t)slice, x, y, out);
}

static void vec4_lerp(const r5xx_vec4 *a, const r5xx_vec4 *b, float t,
                      r5xx_vec4 *out)
{
    int i;
    for (i = 0; i < 4; i++) {
        out->f[i] = lerpf(a->f[i], b->f[i], t);
    }
}

/* Sample one level with the given filter. u, v, w are normalized. */
static void sample_level(const R5xxTexSampler *s, const R5xxMem *mem,
                         uint32_t level, uint32_t face,
                         float u, float v, float w, R5xxFilter filter,
                         r5xx_vec4 *out)
{
    int lw = (int)r5xx_minify(s->width, level);
    int lh = (int)r5xx_minify(s->height, level);
    int ld = s->is_3d ? (int)r5xx_minify(s->depth, level) : 1;
    R5xxWrap ws = s->wrap_s, wt = s->wrap_t, wr = s->wrap_r;

    if (filter == R5XX_FILTER_NEAREST) {
        int x = wrap_nearest(ws, u, lw);
        int y = wrap_nearest(wt, v, lh);
        int z = s->is_3d ? wrap_nearest(wr, w, ld) : 0;
        fetch_or_border(s, mem, level, face, z, x, y, lw, lh, ld, out);
    } else {
        int x0, x1, y0, y1, z0 = 0, z1 = 0;
        float fx, fy, fz = 0.0f;
        r5xx_vec4 t00, t10, t01, t11, a, b, c0;

        wrap_linear(ws, u, lw, &x0, &x1, &fx);
        wrap_linear(wt, v, lh, &y0, &y1, &fy);
        if (s->is_3d) {
            wrap_linear(wr, w, ld, &z0, &z1, &fz);
        }
        fetch_or_border(s, mem, level, face, z0, x0, y0, lw, lh, ld, &t00);
        fetch_or_border(s, mem, level, face, z0, x1, y0, lw, lh, ld, &t10);
        fetch_or_border(s, mem, level, face, z0, x0, y1, lw, lh, ld, &t01);
        fetch_or_border(s, mem, level, face, z0, x1, y1, lw, lh, ld, &t11);
        vec4_lerp(&t00, &t10, fx, &a);
        vec4_lerp(&t01, &t11, fx, &b);
        vec4_lerp(&a, &b, fy, &c0);
        if (s->is_3d && ld > 1) {
            r5xx_vec4 c1;
            fetch_or_border(s, mem, level, face, z1, x0, y0, lw, lh, ld, &t00);
            fetch_or_border(s, mem, level, face, z1, x1, y0, lw, lh, ld, &t10);
            fetch_or_border(s, mem, level, face, z1, x0, y1, lw, lh, ld, &t01);
            fetch_or_border(s, mem, level, face, z1, x1, y1, lw, lh, ld, &t11);
            vec4_lerp(&t00, &t10, fx, &a);
            vec4_lerp(&t01, &t11, fx, &b);
            vec4_lerp(&a, &b, fy, &c1);
            vec4_lerp(&c0, &c1, fz, out);
        } else {
            *out = c0;
        }
    }
}

/* Cube map face selection: standard major-axis rule, faces ordered
 * +X, -X, +Y, -Y, +Z, -Z. */
static int cube_select_face(const float r[3])
{
    float ax = fabsf(r[0]), ay = fabsf(r[1]), az = fabsf(r[2]);

    if (ax >= ay && ax >= az) {
        return r[0] >= 0.0f ? 0 : 1;
    }
    if (ay >= az) {
        return r[1] >= 0.0f ? 2 : 3;
    }
    return r[2] >= 0.0f ? 4 : 5;
}

/* Face coordinates of direction r projected onto the given face. */
static void cube_face_coords(int face, const float r[3], float *s, float *t)
{
    float sc, tc, ma;

    switch (face) {
    case 0:  sc = -r[2]; tc = -r[1]; ma = r[0]; break;
    case 1:  sc =  r[2]; tc = -r[1]; ma = -r[0]; break;
    case 2:  sc =  r[0]; tc =  r[2]; ma = r[1]; break;
    case 3:  sc =  r[0]; tc = -r[2]; ma = -r[1]; break;
    case 4:  sc =  r[0]; tc = -r[1]; ma = r[2]; break;
    default: sc = -r[0]; tc = -r[1]; ma = -r[2]; break;
    }
    ma = fabsf(ma);
    if (ma == 0.0f) {
        ma = 1e-20f;
    }
    *s = 0.5f * (sc / ma + 1.0f);
    *t = 0.5f * (tc / ma + 1.0f);
}

static float compare_func(uint32_t func, float ref, float depth)
{
    bool pass;
    switch (func & 7) {
    case 0:  pass = false; break;             /* NEVER */
    case 1:  pass = ref < depth; break;       /* LESS */
    case 2:  pass = ref == depth; break;      /* EQUAL */
    case 3:  pass = ref <= depth; break;      /* LEQUAL */
    case 4:  pass = ref > depth; break;       /* GREATER */
    case 5:  pass = ref != depth; break;      /* NOTEQUAL */
    case 6:  pass = ref >= depth; break;      /* GEQUAL */
    default: pass = true; break;              /* ALWAYS */
    }
    return pass ? 1.0f : 0.0f;
}

void r5xx_tex_sample_quad(const R5xxTexSampler *s, const R5xxMem *mem,
                          const R5xxTexRequest *req, r5xx_vec4 out[4])
{
    float c[4][3];          /* per pixel coords after projection */
    int face[4] = { 0, 0, 0, 0 };
    int p, q;
    bool unscaled = req->unscaled || s->is_rect;
    float fw = (float)s->width, fh = (float)s->height, fd = (float)s->depth;

    if (!s->valid) {
        for (p = 0; p < 4; p++) {
            out[p] = s->border_color;
        }
        return;
    }

    for (p = 0; p < 4; p++) {
        c[p][0] = req->coord[p].f[0];
        c[p][1] = req->coord[p].f[1];
        c[p][2] = req->coord[p].f[2];
        if (req->op == R5XX_TEXOP_PROJ) {
            float qq = req->coord[p].f[3];
            if (qq != 0.0f) {
                c[p][0] /= qq;
                c[p][1] /= qq;
                c[p][2] /= qq;
            }
        }
        if (s->is_cube) {
            face[p] = cube_select_face(c[p]);
        }
    }

    for (p = 0; p < 4; p++) {
        float u[4], v[4], w[4];
        float dudx, dvdx, dwdx = 0.0f, dudy, dvdy, dwdy = 0.0f;
        float lod, rho, lx, ly;
        R5xxFilter filter;
        r5xx_vec4 res;

        /* Coordinates of the whole quad in this pixel's frame. */
        for (q = 0; q < 4; q++) {
            if (s->is_cube) {
                cube_face_coords(face[p], c[q], &u[q], &v[q]);
                w[q] = 0.0f;
            } else {
                u[q] = c[q][0];
                v[q] = c[q][1];
                w[q] = c[q][2];
                if (unscaled) {
                    u[q] /= fw;
                    v[q] /= fh;
                }
            }
        }

        if (req->op == R5XX_TEXOP_GRAD) {
            if (s->is_cube) {
                float r2[3], su, sv;
                r2[0] = c[p][0] + req->ddx[p].f[0];
                r2[1] = c[p][1] + req->ddx[p].f[1];
                r2[2] = c[p][2] + req->ddx[p].f[2];
                cube_face_coords(face[p], r2, &su, &sv);
                dudx = su - u[p];
                dvdx = sv - v[p];
                r2[0] = c[p][0] + req->ddy[p].f[0];
                r2[1] = c[p][1] + req->ddy[p].f[1];
                r2[2] = c[p][2] + req->ddy[p].f[2];
                cube_face_coords(face[p], r2, &su, &sv);
                dudy = su - u[p];
                dvdy = sv - v[p];
            } else {
                dudx = req->ddx[p].f[0];
                dvdx = req->ddx[p].f[1];
                dwdx = req->ddx[p].f[2];
                dudy = req->ddy[p].f[0];
                dvdy = req->ddy[p].f[1];
                dwdy = req->ddy[p].f[2];
                if (unscaled) {
                    dudx /= fw; dvdx /= fh; dudy /= fw; dvdy /= fh;
                }
            }
        } else {
            int px = (p & 1) ? p - 1 : p + 1;   /* horizontal neighbor */
            int py = (p & 2) ? p - 2 : p + 2;   /* vertical neighbor */
            float sx = (p & 1) ? -1.0f : 1.0f;
            float sy = (p & 2) ? -1.0f : 1.0f;
            dudx = (u[px] - u[p]) * sx;
            dvdx = (v[px] - v[p]) * sx;
            dwdx = (w[px] - w[p]) * sx;
            dudy = (u[py] - u[p]) * sy;
            dvdy = (v[py] - v[p]) * sy;
            dwdy = (w[py] - w[p]) * sy;
        }

        if (req->op == R5XX_TEXOP_LOD) {
            lod = req->lod[p];
        } else {
            dudx *= fw; dvdx *= fh; dudy *= fw; dvdy *= fh;
            if (s->is_3d) {
                dwdx *= fd;
                dwdy *= fd;
            } else {
                dwdx = dwdy = 0.0f;
            }
            lx = sqrtf(dudx * dudx + dvdx * dvdx + dwdx * dwdx);
            ly = sqrtf(dudy * dudy + dvdy * dvdy + dwdy * dwdy);
            rho = lx > ly ? lx : ly;
            lod = rho > 0.0f ? log2f(rho) : -1000.0f;
            if (req->op == R5XX_TEXOP_BIAS) {
                lod += req->lod[p];
            }
        }
        lod += s->lod_bias;
        lod = r5xx_clampf(lod, s->min_lod, s->max_lod);

        filter = lod > 0.0f ? s->min_filter : s->mag_filter;
        if (filter == R5XX_FILTER_NONE) {
            /* FILTER4 (mag) treated as linear, undefined min as nearest */
            filter = lod > 0.0f ? R5XX_FILTER_NEAREST : R5XX_FILTER_LINEAR;
        }

        if (s->mip_filter == R5XX_FILTER_NONE || s->num_levels == 1) {
            uint32_t level = (uint32_t)s->min_lod;
            sample_level(s, mem, level, (uint32_t)face[p], u[p], v[p], w[p],
                         filter, &res);
        } else if (s->mip_filter == R5XX_FILTER_NEAREST) {
            int level = (int)ceilf(lod + 0.5f) - 1;
            if (level < (int)s->min_lod) {
                level = (int)s->min_lod;
            }
            if (level > (int)s->max_lod) {
                level = (int)s->max_lod;
            }
            sample_level(s, mem, (uint32_t)level, (uint32_t)face[p],
                         u[p], v[p], w[p], filter, &res);
        } else {
            int l0 = ifloor(lod);
            int l1 = l0 + 1;
            float f = lod - (float)l0;
            r5xx_vec4 r0, r1;
            if (l0 < (int)s->min_lod) {
                l0 = (int)s->min_lod;
            }
            if (l1 > (int)s->max_lod) {
                l1 = (int)s->max_lod;
            }
            if (l0 > (int)s->max_lod) {
                l0 = (int)s->max_lod;
            }
            if (l1 < l0) {
                l1 = l0;
            }
            sample_level(s, mem, (uint32_t)l0, (uint32_t)face[p],
                         u[p], v[p], w[p], filter, &r0);
            if (l1 != l0 && f > 0.0f) {
                sample_level(s, mem, (uint32_t)l1, (uint32_t)face[p],
                             u[p], v[p], w[p], filter, &r1);
                vec4_lerp(&r0, &r1, f, &res);
            } else {
                res = r0;
            }
        }

        if (s->depth_compare) {
            float ref = s->is_cube ? req->coord[p].f[3] : c[p][2];
            float d = compare_func(s->depth_compare_func,
                                   r5xx_clampf(ref, 0.0f, 1.0f), res.f[0]);
            res.f[0] = res.f[1] = res.f[2] = res.f[3] = d;
        }
        out[p] = res;
    }
}

/* ------------------------------------------------------------------ */
/* Render-target color formats                                        */
/* ------------------------------------------------------------------ */

uint32_t r5xx_color_format_bpp(R5xxColorFormat fmt)
{
    switch (fmt) {
    case R5XX_COLOR_ARGB8888:      return 4;
    case R5XX_COLOR_RGB565:        return 2;
    case R5XX_COLOR_ARGB1555:      return 2;
    case R5XX_COLOR_ARGB4444:      return 2;
    case R5XX_COLOR_ARGB2101010:   return 4;
    case R5XX_COLOR_I8:            return 1;
    case R5XX_COLOR_ARGB16161616:  return 8;
    case R5XX_COLOR_ARGB16161616F: return 8;
    case R5XX_COLOR_ARGB32323232F: return 16;
    case R5XX_COLOR_UV88:          return 2;
    default:                       return 0;
    }
}

uint32_t r5xx_color_format_from_hw(uint32_t colorpitch, bool is_r500,
                                   R5xxColorFormat *fmt)
{
    uint32_t code = (colorpitch >> R300_COLOR_FORMAT_SHIFT) & 0xf;
    R5xxColorFormat f;

    switch (code) {
    case R300_COLOR_FORMAT_ARGB8888:     f = R5XX_COLOR_ARGB8888; break;
    case R300_COLOR_FORMAT_RGB565:       f = R5XX_COLOR_RGB565; break;
    case R300_COLOR_FORMAT_ARGB1555:     f = R5XX_COLOR_ARGB1555; break;
    case R300_COLOR_FORMAT_ARGB4444:     f = R5XX_COLOR_ARGB4444; break;
    case R500_COLOR_FORMAT_ARGB2101010:  f = R5XX_COLOR_ARGB2101010; break;
    case R300_COLOR_FORMAT_I8:           f = R5XX_COLOR_I8; break;
    case R300_COLOR_FORMAT_ARGB16161616: f = R5XX_COLOR_ARGB16161616; break;
    case R300_COLOR_FORMAT_ARGB32323232: f = R5XX_COLOR_ARGB32323232F; break;
    case R300_COLOR_FORMAT_UV88:         f = R5XX_COLOR_UV88; break;
    default:
        return 0;
    }
    if (fmt) {
        *fmt = f;
    }
    return r5xx_color_format_bpp(f);
}

R5xxColorFormat r5xx_color_format_apply_out_fmt(R5xxColorFormat fmt,
                                                uint32_t us_out_fmt)
{
    uint32_t c = us_out_fmt & 0x1f;
    bool fp16 = c == R300_US_OUT_FMT_C_16_FP || c == R300_US_OUT_FMT_C2_16_FP ||
                c == R300_US_OUT_FMT_C4_16_FP;

    if (fmt == R5XX_COLOR_ARGB16161616 && fp16) {
        return R5XX_COLOR_ARGB16161616F;
    }
    if (fmt == R5XX_COLOR_ARGB16161616F && !fp16) {
        return R5XX_COLOR_ARGB16161616;
    }
    return fmt;
}

void r5xx_color_unpack(R5xxColorFormat fmt, const uint8_t *p, r5xx_vec4 *out)
{
    uint32_t v;

    switch (fmt) {
    case R5XX_COLOR_ARGB8888:
        r5xx_unpack_argb8888(rd32(p), out);
        break;
    case R5XX_COLOR_RGB565:
        v = rd16(p);
        out->f[0] = ((v >> 11) & 31) / 31.0f;
        out->f[1] = ((v >> 5) & 63) / 63.0f;
        out->f[2] = (v & 31) / 31.0f;
        out->f[3] = 1.0f;
        break;
    case R5XX_COLOR_ARGB1555:
        v = rd16(p);
        out->f[0] = ((v >> 10) & 31) / 31.0f;
        out->f[1] = ((v >> 5) & 31) / 31.0f;
        out->f[2] = (v & 31) / 31.0f;
        out->f[3] = (float)((v >> 15) & 1);
        break;
    case R5XX_COLOR_ARGB4444:
        v = rd16(p);
        out->f[0] = ((v >> 8) & 15) / 15.0f;
        out->f[1] = ((v >> 4) & 15) / 15.0f;
        out->f[2] = (v & 15) / 15.0f;
        out->f[3] = ((v >> 12) & 15) / 15.0f;
        break;
    case R5XX_COLOR_ARGB2101010:
        v = rd32(p);
        out->f[0] = ((v >> 20) & 1023) / 1023.0f;
        out->f[1] = ((v >> 10) & 1023) / 1023.0f;
        out->f[2] = (v & 1023) / 1023.0f;
        out->f[3] = ((v >> 30) & 3) / 3.0f;
        break;
    case R5XX_COLOR_I8:
        out->f[0] = out->f[1] = out->f[2] = out->f[3] = p[0] / 255.0f;
        break;
    case R5XX_COLOR_ARGB16161616:
        /* words: B, G, R, A (C0 at the lowest address) */
        out->f[2] = rd16(p) / 65535.0f;
        out->f[1] = rd16(p + 2) / 65535.0f;
        out->f[0] = rd16(p + 4) / 65535.0f;
        out->f[3] = rd16(p + 6) / 65535.0f;
        break;
    case R5XX_COLOR_ARGB16161616F:
        out->f[2] = r5xx_half_to_float((uint16_t)rd16(p));
        out->f[1] = r5xx_half_to_float((uint16_t)rd16(p + 2));
        out->f[0] = r5xx_half_to_float((uint16_t)rd16(p + 4));
        out->f[3] = r5xx_half_to_float((uint16_t)rd16(p + 6));
        break;
    case R5XX_COLOR_ARGB32323232F:
        out->f[2] = f32_from_bits(rd32(p));
        out->f[1] = f32_from_bits(rd32(p + 4));
        out->f[0] = f32_from_bits(rd32(p + 8));
        out->f[3] = f32_from_bits(rd32(p + 12));
        break;
    case R5XX_COLOR_UV88:
        /* byte 0 = C2 (R slot), byte 1 = C0 (B slot) */
        out->f[0] = p[0] / 255.0f;
        out->f[1] = 0.0f;
        out->f[2] = p[1] / 255.0f;
        out->f[3] = 1.0f;
        break;
    default:
        out->f[0] = out->f[1] = out->f[2] = 0.0f;
        out->f[3] = 1.0f;
        break;
    }
}

void r5xx_color_pack(R5xxColorFormat fmt, const r5xx_vec4 *c, uint8_t *p)
{
    switch (fmt) {
    case R5XX_COLOR_ARGB8888:
        wr32(p, r5xx_pack_argb8888(c));
        break;
    case R5XX_COLOR_RGB565:
        wr16(p, (to_unorm(c->f[0], 31) << 11) | (to_unorm(c->f[1], 63) << 5) |
                to_unorm(c->f[2], 31));
        break;
    case R5XX_COLOR_ARGB1555:
        wr16(p, (to_unorm(c->f[3], 1) << 15) | (to_unorm(c->f[0], 31) << 10) |
                (to_unorm(c->f[1], 31) << 5) | to_unorm(c->f[2], 31));
        break;
    case R5XX_COLOR_ARGB4444:
        wr16(p, (to_unorm(c->f[3], 15) << 12) | (to_unorm(c->f[0], 15) << 8) |
                (to_unorm(c->f[1], 15) << 4) | to_unorm(c->f[2], 15));
        break;
    case R5XX_COLOR_ARGB2101010:
        wr32(p, (to_unorm(c->f[3], 3) << 30) | (to_unorm(c->f[0], 1023) << 20) |
                (to_unorm(c->f[1], 1023) << 10) | to_unorm(c->f[2], 1023));
        break;
    case R5XX_COLOR_I8:
        p[0] = (uint8_t)to_unorm(c->f[0], 255);
        break;
    case R5XX_COLOR_ARGB16161616:
        wr16(p, to_unorm(c->f[2], 65535));
        wr16(p + 2, to_unorm(c->f[1], 65535));
        wr16(p + 4, to_unorm(c->f[0], 65535));
        wr16(p + 6, to_unorm(c->f[3], 65535));
        break;
    case R5XX_COLOR_ARGB16161616F:
        wr16(p, r5xx_float_to_half(c->f[2]));
        wr16(p + 2, r5xx_float_to_half(c->f[1]));
        wr16(p + 4, r5xx_float_to_half(c->f[0]));
        wr16(p + 6, r5xx_float_to_half(c->f[3]));
        break;
    case R5XX_COLOR_ARGB32323232F:
        wr32(p, f32_to_bits(c->f[2]));
        wr32(p + 4, f32_to_bits(c->f[1]));
        wr32(p + 8, f32_to_bits(c->f[0]));
        wr32(p + 12, f32_to_bits(c->f[3]));
        break;
    case R5XX_COLOR_UV88:
        p[0] = (uint8_t)to_unorm(c->f[0], 255);
        p[1] = (uint8_t)to_unorm(c->f[2], 255);
        break;
    default:
        break;
    }
}
