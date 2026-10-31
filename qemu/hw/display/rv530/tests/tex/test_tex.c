/*
 * Unit tests for the R300/R500 texture module (r5xx_tex.c).
 *
 * Every case builds the TX_* registers as a driver would, decodes them
 * with r5xx_tex_decode(), writes texels into a flat memory and checks
 * what r5xx_tex_fetch_texel() / r5xx_tex_sample_quad() return against
 * values worked out by hand (not read back from the code under test).
 * Formats that broke in the guest have their own case: wide-format
 * channel order, the TX_OFFSET endian swap, Y'CbCr 4:2:2 byte
 * order and the YUV_TO_RGB swizzle, 3D textures.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "r5xx_core.h"
#include "r5xx_tex.h"

#define MEM_SIZE (4u << 20)

/* register fields (Mesa r300_reg.h; the same values r5xx_tex.c uses) */
#define F0_W(w)         ((uint32_t)((w) - 1))
#define F0_H(h)         ((uint32_t)((h) - 1) << 11)
#define F0_DEPTH_LOG2(d) ((uint32_t)(d) << 22)
#define F0_LEVELS(n)    ((uint32_t)((n) - 1) << 26)
#define F1_FMT(f)       ((uint32_t)(f))
#define F1_SWZ(r, g, b, a) (((uint32_t)(a) << 9) | ((uint32_t)(r) << 12) | \
                            ((uint32_t)(g) << 15) | ((uint32_t)(b) << 18))
#define F1_YUV_TO_RGB   (1u << 22)
#define F1_3D           (1u << 25)
#define SEL_X 0
#define SEL_Y 1
#define SEL_Z 2
#define SEL_W 3
#define SEL_0 4
#define SEL_1 5
#define IDENT F1_SWZ(SEL_X, SEL_Y, SEL_Z, SEL_W)
/* filter0: wrap s/t/r 0-2/3-5/6-8, mag 9-10, min 11-12, mip 13-14 */
#define FL_WRAP(s, t, r) ((uint32_t)(s) | ((uint32_t)(t) << 3) | ((uint32_t)(r) << 6))
#define FL_NEAREST      ((1u << 9) | (1u << 11))
#define FL_LINEAR       ((2u << 9) | (2u << 11))
#define FL_MIP_NEAREST  (1u << 13)
#define WRAP_REPEAT 0
#define WRAP_CLAMP_EDGE 2
#define WRAP_CLAMP_BORDER 6

/* format codes */
#define FMT_X8          0x00
#define FMT_Z5Y6X5      0x06
#define FMT_W1Z5Y5X5    0x0B
#define FMT_W8Z8Y8X8    0x0C
#define FMT_DXT1        0x0F
#define FMT_VYUY422     0x14
#define FMT_YVYU422     0x15
#define FMT_16F4        0x1A
#define FMT_32F         0x1B

static uint8_t *mem;
static R5xxFlatMem fm;
static int checks, failures;

static void check(const char *what, const r5xx_vec4 *got, float r, float g, float b, float a,
                  float tol)
{
    float want[4] = { r, g, b, a };
    int c, bad = 0;

    checks++;
    for (c = 0; c < 4; c++) {
        if (!(fabsf(got->f[c] - want[c]) <= tol)) {
            bad = 1;
        }
    }
    if (bad) {
        failures++;
        printf("FAIL %s: got (%.4f %.4f %.4f %.4f), want (%.4f %.4f %.4f %.4f)\n", what,
               got->f[0], got->f[1], got->f[2], got->f[3], r, g, b, a);
    }
}

static void check_true(const char *what, int cond)
{
    checks++;
    if (!cond) {
        failures++;
        printf("FAIL %s\n", what);
    }
}

static void decode(R5xxTexSampler *s, uint32_t offset, uint32_t f0, uint32_t f1,
                   uint32_t filter0)
{
    R5xxTexRegs tr = { .filter0 = filter0, .format0 = f0, .format1 = f1, .offset = offset };
    char err[128];

    if (!r5xx_tex_decode(&tr, true, s, err, sizeof(err))) {
        printf("FAIL decode: %s\n", err);
        failures++;
    }
}

/* write bpp bytes at texel (x, y) of level/layer of s */
static void put_texel(const R5xxTexSampler *s, uint32_t level, uint32_t layer, uint32_t x,
                      uint32_t y, const void *bytes, uint32_t n)
{
    R5xxSurfaceLayout l;

    r5xx_tex_level_layout(s, level, layer, &l);
    memcpy(mem + l.gpu_addr + r5xx_surface_pixel_offset(&l, x, y), bytes, n);
}

static void fill(const R5xxTexSampler *s, uint32_t w, uint32_t h, const void *bytes, uint32_t n)
{
    uint32_t x, y;
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            put_texel(s, 0, 0, x, y, bytes, n);
        }
    }
}

static r5xx_vec4 fetch(const R5xxTexSampler *s, uint32_t level, uint32_t slice, int x, int y)
{
    r5xx_vec4 v;
    r5xx_tex_fetch_texel(s, &fm.mem, level, 0, slice, x, y, &v);
    return v;
}

/* sample all four pixels of a quad at one coordinate, explicit LOD */
static r5xx_vec4 sample(const R5xxTexSampler *s, float u, float v, float r, float lod)
{
    R5xxTexRequest q;
    r5xx_vec4 out[4];
    int p;

    memset(&q, 0, sizeof(q));
    q.op = R5XX_TEXOP_LOD;
    for (p = 0; p < 4; p++) {
        q.coord[p] = r5xx_vec4_make(u, v, r, 1);
        q.lod[p] = lod;
    }
    r5xx_tex_sample_quad(s, &fm.mem, &q, out);
    return out[0];
}

/* ---- formats ---------------------------------------------------- */

static void test_formats(void)
{
    R5xxTexSampler s;
    r5xx_vec4 v;

    {   /* 8-bit channels, X in the lowest byte */
        static const uint8_t px[4] = { 0x10, 0x20, 0x30, 0x40 };
        decode(&s, 0x10000, F0_W(4) | F0_H(4), F1_FMT(FMT_W8Z8Y8X8) | IDENT, FL_NEAREST);
        fill(&s, 4, 4, px, 4);
        v = fetch(&s, 0, 0, 1, 2);
        check("W8Z8Y8X8", &v, 0x10 / 255.f, 0x20 / 255.f, 0x30 / 255.f, 0x40 / 255.f, 1e-6f);
    }
    {   /* the TX_OFFSET endian swap applies to the dwords read */
        static const uint8_t px[4] = { 0x10, 0x20, 0x30, 0x40 };
        decode(&s, 0x10000 | 2, F0_W(4) | F0_H(4), F1_FMT(FMT_W8Z8Y8X8) | IDENT, FL_NEAREST);
        fill(&s, 4, 4, px, 4);
        v = fetch(&s, 0, 0, 0, 0);
        check("W8Z8Y8X8 with 32-bit endian swap", &v, 0x40 / 255.f, 0x30 / 255.f,
              0x20 / 255.f, 0x10 / 255.f, 1e-6f);
    }
    {   /* 5:6:5, X in the low 5 bits */
        uint16_t px = (31u << 11) | (32u << 5) | 0;     /* Z=31 Y=32 X=0 */
        decode(&s, 0x10000, F0_W(4) | F0_H(4), F1_FMT(FMT_Z5Y6X5) | F1_SWZ(SEL_X, SEL_Y, SEL_Z, SEL_1),
               FL_NEAREST);
        fill(&s, 4, 4, &px, 2);
        v = fetch(&s, 0, 0, 3, 3);
        check("Z5Y6X5", &v, 0.0f, 32 / 63.f, 1.0f, 1.0f, 1e-6f);
    }
    {   /* 1:5:5:5 with the 1-bit W on top */
        uint16_t px = 0x8000 | (10u << 10) | (20u << 5) | 31;
        decode(&s, 0x10000, F0_W(4) | F0_H(4), F1_FMT(FMT_W1Z5Y5X5) | IDENT, FL_NEAREST);
        fill(&s, 4, 4, &px, 2);
        v = fetch(&s, 0, 0, 0, 0);
        check("W1Z5Y5X5", &v, 1.0f, 20 / 31.f, 10 / 31.f, 1.0f, 1e-6f);
    }
    {   /* one 8-bit channel, replicated by the swizzle */
        uint8_t px = 0x80;
        decode(&s, 0x10000, F0_W(4) | F0_H(4), F1_FMT(FMT_X8) | F1_SWZ(SEL_X, SEL_X, SEL_X, SEL_1),
               FL_NEAREST);
        fill(&s, 4, 4, &px, 1);
        v = fetch(&s, 0, 0, 2, 1);
        check("X8 replicated", &v, 128 / 255.f, 128 / 255.f, 128 / 255.f, 1.0f, 1e-6f);
    }
    {   /* half floats, X first in memory (wide formats) */
        static const uint16_t px[4] = { 0x3c00, 0x3800, 0x3400, 0x4000 };  /* 1 .5 .25 2 */
        decode(&s, 0x10000, F0_W(4) | F0_H(4), F1_FMT(FMT_16F4) | IDENT, FL_NEAREST);
        fill(&s, 4, 4, px, 8);
        v = fetch(&s, 0, 0, 1, 1);
        check("16F_16F_16F_16F", &v, 1.0f, 0.5f, 0.25f, 2.0f, 0.0f);
    }
    {   /* one float channel */
        float px = 0.75f;
        decode(&s, 0x10000, F0_W(4) | F0_H(4), F1_FMT(FMT_32F) | F1_SWZ(SEL_X, SEL_0, SEL_0, SEL_1),
               FL_NEAREST);
        fill(&s, 4, 4, &px, 4);
        v = fetch(&s, 0, 0, 0, 0);
        check("32F", &v, 0.75f, 0.0f, 0.0f, 1.0f, 0.0f);
    }
    {   /* DXT1, c0 > c1 (four colors): red, blue and the two blends */
        uint8_t blk[8] = { 0x00, 0xf8, 0x1f, 0x00,             /* c0 = red, c1 = blue */
                           0xe4, 0x00, 0x00, 0x00 };            /* row 0: idx 0 1 2 3 */
        decode(&s, 0x10000, F0_W(4) | F0_H(4), F1_FMT(FMT_DXT1) | IDENT, FL_NEAREST);
        memcpy(mem + 0x10000, blk, 8);
        v = fetch(&s, 0, 0, 0, 0);
        check("DXT1 c0", &v, 1, 0, 0, 1, 1e-6f);
        v = fetch(&s, 0, 0, 1, 0);
        check("DXT1 c1", &v, 0, 0, 1, 1, 1e-6f);
        v = fetch(&s, 0, 0, 2, 0);
        check("DXT1 2/3 c0 + 1/3 c1", &v, 2 / 3.f, 0, 1 / 3.f, 1, 1e-6f);
        v = fetch(&s, 0, 0, 3, 0);
        check("DXT1 1/3 c0 + 2/3 c1", &v, 1 / 3.f, 0, 2 / 3.f, 1, 1e-6f);
        v = fetch(&s, 0, 0, 0, 1);
        check("DXT1 row 1 (index 0)", &v, 1, 0, 0, 1, 1e-6f);
    }
    {   /*
         * Y'CbCr 4:2:2 with YUV_TO_RGB, as Apple's driver programs it:
         * swizzle X, X, X, ONE, and the converted color still
         * comes out.  Orange (ff 80 00) is Y' 0x97, Cb 0x2a, Cr 0xc9.
         * YVYU422 holds U Y0 V Y1 ('2vuy'), VYUY422 Y0 U Y1 V.
         */
        static const uint8_t yvyu[4] = { 0x2a, 0x97, 0xc9, 0x97 };
        static const uint8_t vyuy[4] = { 0x97, 0x2a, 0x97, 0xc9 };
        uint32_t f1 = F1_YUV_TO_RGB | F1_SWZ(SEL_X, SEL_X, SEL_X, SEL_1);
        int x;
        decode(&s, 0x10000, F0_W(4) | F0_H(4), F1_FMT(FMT_YVYU422) | f1, FL_NEAREST);
        for (x = 0; x < 4; x += 2) {
            put_texel(&s, 0, 0, x, 0, yvyu, 4);
        }
        v = fetch(&s, 0, 0, 0, 0);
        check("YVYU422 + YUV_TO_RGB (even texel)", &v, 1.0f, 0.5f, 0.0f, 1.0f, 0.02f);
        v = fetch(&s, 0, 0, 1, 0);
        check("YVYU422 + YUV_TO_RGB (odd texel)", &v, 1.0f, 0.5f, 0.0f, 1.0f, 0.02f);
        decode(&s, 0x10000, F0_W(4) | F0_H(4), F1_FMT(FMT_VYUY422) | f1, FL_NEAREST);
        for (x = 0; x < 4; x += 2) {
            put_texel(&s, 0, 0, x, 0, vyuy, 4);
        }
        v = fetch(&s, 0, 0, 1, 0);
        check("VYUY422 + YUV_TO_RGB", &v, 1.0f, 0.5f, 0.0f, 1.0f, 0.02f);
    }
}

/* ---- layout ------------------------------------------------------ */

static void test_layout(void)
{
    R5xxTexSampler s;
    r5xx_vec4 v;
    uint32_t l, z;
    char name[64];

    /* 3D: 4x4x4, a distinct value in every slice */
    decode(&s, 0x20000, F0_W(4) | F0_H(4) | F0_DEPTH_LOG2(2), F1_FMT(FMT_W8Z8Y8X8) | IDENT | F1_3D,
           FL_NEAREST);
    check_true("3D: depth 4", s.depth == 4 && s.is_3d);
    check_true("3D: a slice holds a whole 4x4 level", s.level_layer_size[0] >= 4 * 4 * 4);
    for (z = 0; z < 4; z++) {
        uint8_t px[4] = { (uint8_t)(z * 64), 0, 0, 255 };
        uint32_t x, y;
        for (y = 0; y < 4; y++) {
            for (x = 0; x < 4; x++) {
                put_texel(&s, 0, z, x, y, px, 4);
            }
        }
    }
    for (z = 0; z < 4; z++) {
        v = fetch(&s, 0, z, 2, 3);
        snprintf(name, sizeof(name), "3D slice %u", z);
        check(name, &v, z * 64 / 255.f, 0, 0, 1, 1e-6f);
    }

    /* mip chain: 8x8 with 4 levels; the levels must not overlap */
    decode(&s, 0x30000, F0_W(8) | F0_H(8) | F0_LEVELS(4), F1_FMT(FMT_W8Z8Y8X8) | IDENT,
           FL_NEAREST | FL_MIP_NEAREST);
    check_true("mips: 4 levels", s.num_levels == 4);
    for (l = 0; l + 1 < 4; l++) {
        R5xxSurfaceLayout a, b;
        r5xx_tex_level_layout(&s, l, 0, &a);
        r5xx_tex_level_layout(&s, l + 1, 0, &b);
        snprintf(name, sizeof(name), "mips: level %u ends before level %u", l, l + 1);
        check_true(name, b.gpu_addr >= a.gpu_addr + a.pitch_bytes * r5xx_minify(8, l));
        snprintf(name, sizeof(name), "mips: level %u pitch holds a row", l + 1);
        check_true(name, b.pitch_bytes >= r5xx_minify(8, l + 1) * 4);
    }
    for (l = 0; l < 4; l++) {
        uint32_t w = r5xx_minify(8, l), x, y;
        uint8_t px[4] = { 0, (uint8_t)(l * 80), 0, 255 };
        for (y = 0; y < w; y++) {
            for (x = 0; x < w; x++) {
                put_texel(&s, l, 0, x, y, px, 4);
            }
        }
    }
    for (l = 0; l < 4; l++) {
        uint32_t w = r5xx_minify(8, l);
        v = fetch(&s, l, 0, w - 1, w - 1);
        snprintf(name, sizeof(name), "mip level %u", l);
        check(name, &v, 0, l * 80 / 255.f, 0, 1, 1e-6f);
    }
}

/* ---- sampling ---------------------------------------------------- */

static void test_sampling(void)
{
    R5xxTexSampler s;
    r5xx_vec4 v;
    uint32_t x, y;

    /* 4x1 ramp of X: 0, 85, 170, 255 */
    decode(&s, 0x40000, F0_W(4) | F0_H(1), F1_FMT(FMT_W8Z8Y8X8) | IDENT,
           FL_NEAREST | FL_WRAP(WRAP_REPEAT, WRAP_REPEAT, WRAP_REPEAT));
    for (x = 0; x < 4; x++) {
        uint8_t px[4] = { (uint8_t)(x * 85), 0, 0, 255 };
        put_texel(&s, 0, 0, x, 0, px, 4);
    }
    v = sample(&s, 1.5f / 4, 0.5f, 0, 0);
    check("nearest at the center of texel 1", &v, 85 / 255.f, 0, 0, 1, 1e-6f);
    v = sample(&s, 1.0f + 0.5f / 4, 0.5f, 0, 0);
    check("repeat: u = 1 + half a texel is texel 0", &v, 0, 0, 0, 1, 1e-6f);

    decode(&s, 0x40000, F0_W(4) | F0_H(1), F1_FMT(FMT_W8Z8Y8X8) | IDENT,
           FL_LINEAR | FL_WRAP(WRAP_CLAMP_EDGE, WRAP_CLAMP_EDGE, WRAP_CLAMP_EDGE));
    v = sample(&s, 2.0f / 4, 0.5f, 0, 0);
    check("bilinear halfway between texels 1 and 2", &v, 127.5f / 255.f, 0, 0, 1, 1e-5f);
    v = sample(&s, -0.25f, 0.5f, 0, 0);
    check("clamp to edge left of the texture", &v, 0, 0, 0, 1, 1e-6f);
    v = sample(&s, 1.25f, 0.5f, 0, 0);
    check("clamp to edge right of the texture", &v, 1, 0, 0, 1, 1e-6f);

    {   /* clamp to border: outside the texture is the border color */
        R5xxTexRegs tr = {
            .filter0 = FL_NEAREST | FL_WRAP(WRAP_CLAMP_BORDER, WRAP_CLAMP_BORDER, WRAP_CLAMP_BORDER),
            .format0 = F0_W(4) | F0_H(1), .format1 = F1_FMT(FMT_W8Z8Y8X8) | IDENT,
            .offset = 0x40000, .border_color = 0x80402010u,     /* in the texture's format */
        };
        char err[128];
        r5xx_tex_decode(&tr, true, &s, err, sizeof(err));
        v = sample(&s, -0.5f, 0.5f, 0, 0);
        check("clamp to border", &v, 0x10 / 255.f, 0x20 / 255.f, 0x40 / 255.f, 0x80 / 255.f,
              1e-6f);
    }

    /* 3D: 2x2x2, slice 0 black, slice 1 white; linear halfway in r */
    decode(&s, 0x50000, F0_W(2) | F0_H(2) | F0_DEPTH_LOG2(1), F1_FMT(FMT_W8Z8Y8X8) | IDENT | F1_3D,
           FL_LINEAR | FL_WRAP(WRAP_CLAMP_EDGE, WRAP_CLAMP_EDGE, WRAP_CLAMP_EDGE));
    for (y = 0; y < 2; y++) {
        for (x = 0; x < 2; x++) {
            static const uint8_t black[4] = { 0, 0, 0, 255 }, white[4] = { 255, 255, 255, 255 };
            put_texel(&s, 0, 0, x, y, black, 4);
            put_texel(&s, 0, 1, x, y, white, 4);
        }
    }
    v = sample(&s, 0.5f, 0.5f, 0.5f, 0);
    check("3D trilinear halfway between slices", &v, 0.5f, 0.5f, 0.5f, 1, 1e-5f);
    v = sample(&s, 0.5f, 0.5f, 0.25f, 0);
    check("3D at the center of slice 0", &v, 0, 0, 0, 1, 1e-5f);

    /* mip selection with an explicit LOD (levels from test_layout's
     * pattern: level l has G = l * 80) */
    decode(&s, 0x30000, F0_W(8) | F0_H(8) | F0_LEVELS(4), F1_FMT(FMT_W8Z8Y8X8) | IDENT,
           FL_NEAREST | FL_MIP_NEAREST);
    v = sample(&s, 0.5f, 0.5f, 0, 2.0f);
    check("mip nearest: LOD 2 is level 2", &v, 0, 160 / 255.f, 0, 1, 1e-6f);
    v = sample(&s, 0.5f, 0.5f, 0, 0.0f);
    check("mip nearest: LOD 0 is level 0", &v, 0, 0, 0, 1, 1e-6f);
}

int main(void)
{
    mem = calloc(1, MEM_SIZE);
    if (!mem) {
        return 1;
    }
    r5xx_flatmem_init(&fm, mem, MEM_SIZE, 0);
    test_formats();
    test_layout();
    test_sampling();
    printf("tex_test: %d checks passed, %d failed\n", checks - failures, failures);
    return failures ? 1 : 0;
}
