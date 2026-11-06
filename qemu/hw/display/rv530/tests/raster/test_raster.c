/*
 * Unit tests for the CPU rasterizer (r5xx_raster.c).
 *
 * Renders into a flat memory buffer through R5xxFlatMem and checks the
 * result pixel-exactly.  On failure an ASCII dump of the framebuffer
 * region is printed ('#' = got & expected, 'G' = got only, 'E' =
 * expected only, '.' = neither).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "r5xx_raster.h"
#include "stubs.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- framebuffer layout ------------------------------------------ */

#define FB_W      64
#define FB_H      64
#define VRAM_SIZE (1u << 20)
#define GPU_BASE  0x00100000u
#define CB0_OFF   0x00000u            /* 64x64 ARGB8888 = 16 KiB */
#define CB1_OFF   0x04000u
#define ZB_OFF    0x08000u            /* 64x64 Z24S8 = 16 KiB */
#define FP_OFF    0x0C000u            /* 64x64 FP16 = 32 KiB */

static uint8_t vram[VRAM_SIZE];
static R5xxFlatMem fm;

/* register bits used by the tests (Mesa r300_reg.h) */
#define COLOR_FORMAT_ARGB8888     (6u << 21)
#define COLOR_FORMAT_RGB565       (4u << 21)
#define COLOR_FORMAT_ARGB16161616 (10u << 21)
#define COLOR_MICROTILE_ENABLE    (1u << 17)
#define OUT_FMT_BGRA  ((3u << 8) | (2u << 10) | (1u << 12) | (0u << 14))
#define OUT_FMT_RGBA  ((1u << 8) | (2u << 10) | (3u << 12) | (0u << 14))
#define OUT_FMT_UNUSED 15u
#define OUT_FMT_C4_16_FP 18u
#define ZB_STENCIL_ENABLE  1u
#define ZB_Z_ENABLE        2u
#define ZB_Z_WRITE_ENABLE  4u
#define ZB_STENCIL_FRONT_BACK (1u << 4)
#define ZB_STENCIL_REFMASK_FRONT_BACK (1u << 6)
#define ZS_NEVER 0
#define ZS_LESS 1
#define ZS_LEQUAL 2
#define ZS_EQUAL 3
#define ZS_GEQUAL 4
#define ZS_GREATER 5
#define ZS_NOTEQUAL 6
#define ZS_ALWAYS 7
#define ZS_KEEP 0
#define ZS_ZERO 1
#define ZS_REPLACE 2
#define ZS_INCR 3
#define ZS_DECR 4
#define ZS_INVERT 5
#define ZS_INCR_WRAP 6
#define ZS_DECR_WRAP 7
#define ZSC(zf, sf, sfail, zpass, zfail) \
    ((zf) | ((sf) << 3) | ((sfail) << 6) | ((zpass) << 9) | ((zfail) << 12))
#define ZSC_BACK(sf, sfail, zpass, zfail) \
    (((sf) << 15) | ((sfail) << 18) | ((zpass) << 21) | ((zfail) << 24))
#define SREFMASK(ref, mask, wmask) ((ref) | ((mask) << 8) | ((wmask) << 16))
#define CULL_FRONT 1u
#define CULL_BACK 2u
#define FRONT_FACE_CW 4u
#define BLEND_ENABLE 1u
#define BLEND_SEPARATE_ALPHA 2u
#define BLEND_FCN(f) ((f) << 12)
#define BLEND_SRC(f) ((f) << 16)
#define BLEND_DST(f) ((f) << 24)
#define FCN_ADD_CLAMP 0
#define FCN_ADD_NOCLAMP 1
#define FCN_SUB_CLAMP 2
#define FCN_MIN 4
#define FCN_MAX 5
#define FCN_RSUB_CLAMP 6
#define BF_ZERO 32
#define BF_ONE 33
#define BF_SRC_COLOR 34
#define BF_ONE_MINUS_SRC_COLOR 35
#define BF_DST_COLOR 36
#define BF_ONE_MINUS_DST_COLOR 37
#define BF_SRC_ALPHA 38
#define BF_ONE_MINUS_SRC_ALPHA 39
#define BF_DST_ALPHA 40
#define BF_ONE_MINUS_DST_ALPHA 41
#define BF_SRC_ALPHA_SATURATE 42
#define BF_CONST_COLOR 43
#define BF_ONE_MINUS_CONST_COLOR 44
#define BF_CONST_ALPHA 45
#define BF_ONE_MINUS_CONST_ALPHA 46
#define DISCARD_SRC_ALPHA_0 (1u << 3)
#define ROP_ENABLE 4u
#define ROP(op) ((op) << 8)
#define ALPHA_FUNC_ENABLE (1u << 11)
#define ALPHA_8BIT (1u << 12)
#define ALPHA_FP16 (1u << 28)
#define FOG_ENABLE 1u
#define FOG_LINEAR (0u << 1)
#define FOG_EXP (1u << 1)
#define POLY_MODE_DUAL 1u
#define POLY_FRONT_LINE (1u << 4)
#define POLY_FRONT_POINT (0u << 4)
#define POLY_BACK_LINE (1u << 7)
#define POLY_BACK_POINT (0u << 7)
#define POLY_FRONT_TRI (2u << 4)
#define POLY_BACK_TRI (2u << 7)
#define CCTL_INDEPENDENT_CMASK (1u << 12)
#define CCTL_MULTIWRITES(n) (((n) - 1) << 5)
#define GB_SUBPIXEL_1_16 (1u << 16)

/* ---- test bookkeeping -------------------------------------------- */

static int failures, checks;
static const char *cur_test = "";

#define CHECK(cond, ...) do {                                           \
        checks++;                                                       \
        if (!(cond)) {                                                  \
            failures++;                                                 \
            printf("FAIL [%s] %s:%d: ", cur_test, __FILE__, __LINE__);  \
            printf(__VA_ARGS__);                                        \
            printf("\n");                                               \
        }                                                               \
    } while (0)

static uint32_t f2u(float f)
{
    uint32_t u;
    memcpy(&u, &f, 4);
    return u;
}

/* ---- framebuffer access ------------------------------------------ */

static uint32_t rd32(uint32_t off)
{
    return vram[off] | (vram[off + 1] << 8) | (vram[off + 2] << 16) |
           ((uint32_t)vram[off + 3] << 24);
}

static void wr32(uint32_t off, uint32_t v)
{
    vram[off] = v & 0xff;
    vram[off + 1] = (v >> 8) & 0xff;
    vram[off + 2] = (v >> 16) & 0xff;
    vram[off + 3] = v >> 24;
}

static uint32_t cb_pixel(int x, int y)
{
    return rd32(CB0_OFF + y * FB_W * 4 + x * 4);
}

static uint32_t cb1_pixel(int x, int y)
{
    return rd32(CB1_OFF + y * FB_W * 4 + x * 4);
}

static uint32_t zb_raw(int x, int y)
{
    return rd32(ZB_OFF + y * FB_W * 4 + x * 4);
}

static uint32_t zb_z24(int x, int y) { return zb_raw(x, y) >> 8; }
static uint8_t zb_s8(int x, int y) { return zb_raw(x, y) & 0xff; }

static uint32_t zb_z16(int x, int y)
{
    uint32_t off = ZB_OFF + y * FB_W * 2 + x * 2;
    return vram[off] | (vram[off + 1] << 8);
}

static void fill_cb(uint32_t v)
{
    int i;
    for (i = 0; i < FB_W * FB_H; i++) {
        wr32(CB0_OFF + i * 4, v);
        wr32(CB1_OFF + i * 4, v);
    }
}

static void fill_zb(uint32_t v)
{
    int i;
    for (i = 0; i < FB_W * FB_H; i++) {
        wr32(ZB_OFF + i * 4, v);
    }
}

static void clear_all(void)
{
    memset(vram, 0, sizeof(vram));
}

/* ---- rasterizer defaults ----------------------------------------- */

static R5xxRaster R;

static void setup_defaults(bool r500)
{
    R5xxRasterRegs *g = &R.regs;
    int off = r500 ? 0 : 1440;

    memset(&R, 0, sizeof(R));
    R.mem = &fm.mem;
    R.num_fs_inputs = 2;
    g->is_r500 = r500;
    g->rb3d_cctl = 0;
    g->rb3d_colorpitch[0] = FB_W | COLOR_FORMAT_ARGB8888;
    g->rb3d_coloroffset[0] = GPU_BASE + CB0_OFF;
    g->rb3d_color_channel_mask = 0xf;
    g->us_out_fmt[0] = OUT_FMT_BGRA;
    g->us_out_fmt[1] = g->us_out_fmt[2] = g->us_out_fmt[3] = OUT_FMT_UNUSED;
    g->zb_format = 2;                     /* Z24S8 */
    g->zb_depthpitch = FB_W;
    g->zb_depthoffset = GPU_BASE + ZB_OFF;
    g->sc_scissors_tl = (uint32_t)off | ((uint32_t)off << 13);
    g->sc_scissors_br = (uint32_t)(FB_W - 1 + off) |
                        ((uint32_t)(FB_H - 1 + off) << 13);
    g->sc_clip_rule = 0xffff;
    g->su_cull_mode = 0;                  /* FRONT_FACE_CCW, no culling */
    g->su_depth_scale = 0x4B7FFFFF;       /* 2^24 - 1 */
    g->ga_point_size = (6u << 16) | 6u;   /* 1 pixel */
    g->ga_point_minmax = (6u << 16) | 6u;
    g->ga_line_cntl = 6u | (2u << 16);    /* width 1, END_TYPE_SQR */
    g->ga_color_control = 3u << 16;       /* provoking = last */
}

static bool do_setup(void)
{
    char err[128];
    bool ok = r5xx_raster_setup(&R, err, sizeof(err));
    CHECK(ok, "setup failed: %s", err);
    return ok;
}

static R5xxVertex vtx(float x, float y, float z, float w,
                      float r, float g, float b, float a)
{
    R5xxVertex v;
    memset(&v, 0, sizeof(v));
    v.x = x;
    v.y = y;
    v.z = z;
    v.w = w;
    v.attr[0] = r5xx_vec4_make(r, g, b, a);
    v.attr[1] = r5xx_vec4_make(0, 0, 0, 1);
    return v;
}

static void tri(const R5xxVertex *a, const R5xxVertex *b, const R5xxVertex *c)
{
    r5xx_raster_triangle(&R, a, b, c);
}

/* Draw one pixel at (x, y) with the given color through a 1x1 point. */
static void draw_pixel(int x, int y, float r, float g, float b, float a)
{
    R5xxVertex v = vtx(x + 0.5f, y + 0.5f, 0.5f, 1, r, g, b, a);
    r5xx_raster_point(&R, &v);
}

/* ---- ASCII dumps ------------------------------------------------- */

static void dump_maps(const uint8_t *got, const uint8_t *exp, int w, int h)
{
    int x, y;
    printf("    legend: '#' both, 'G' got only, 'E' expected only, '.' none\n");
    for (y = 0; y < h; y++) {
        printf("    %2d ", y);
        for (x = 0; x < w; x++) {
            int g = got[y * w + x] != 0, e = exp[y * w + x] != 0;
            putchar(g && e ? '#' : g ? 'G' : e ? 'E' : '.');
        }
        putchar('\n');
    }
}

static void got_map_cb(uint8_t *map)
{
    int x, y;
    for (y = 0; y < FB_H; y++) {
        for (x = 0; x < FB_W; x++) {
            map[y * FB_W + x] = cb_pixel(x, y) != 0;
        }
    }
}

static bool compare_maps(const uint8_t *exp)
{
    uint8_t got[FB_W * FB_H];
    got_map_cb(got);
    if (memcmp(got, exp, sizeof(got)) != 0) {
        dump_maps(got, exp, FB_W, FB_H);
        return false;
    }
    return true;
}

/* ---- brute-force reference coverage ------------------------------ */

/* Independent implementation of the fill rule: coordinates snapped to
 * the subpixel grid, pixel center test against the three edges with the
 * top-left rule, evaluated for every pixel of the framebuffer. */
static bool ref_tri_covers(int S, const R5xxVertex *v0, const R5xxVertex *v1,
                           const R5xxVertex *v2, int px, int py)
{
    long long X[3], Y[3], area, cx, cy;
    const R5xxVertex *v[3] = { v0, v1, v2 };
    int i;

    for (i = 0; i < 3; i++) {
        X[i] = llrintf(v[i]->x * S);
        Y[i] = llrintf(v[i]->y * S);
    }
    area = (X[1] - X[0]) * (Y[2] - Y[0]) - (X[2] - X[0]) * (Y[1] - Y[0]);
    if (area == 0) {
        return false;
    }
    cx = (long long)px * S + S / 2;
    cy = (long long)py * S + S / 2;
    for (i = 0; i < 3; i++) {
        /* edge from v[i] to v[i+1]; make it clockwise (area > 0) */
        long long ax = X[i], ay = Y[i], bx = X[(i + 1) % 3], by = Y[(i + 1) % 3];
        long long dx, dy, e;
        bool topleft;
        if (area < 0) {
            long long t;
            t = ax; ax = bx; bx = t;
            t = ay; ay = by; by = t;
        }
        dx = bx - ax;
        dy = by - ay;
        e = dx * (cy - ay) - dy * (cx - ax);
        topleft = (dy == 0 && dx > 0) || dy < 0;
        if (e < 0 || (e == 0 && !topleft)) {
            return false;
        }
    }
    return true;
}

static void ref_tri_map(int S, const R5xxVertex *v0, const R5xxVertex *v1,
                        const R5xxVertex *v2, uint8_t *map)
{
    int x, y;
    for (y = 0; y < FB_H; y++) {
        for (x = 0; x < FB_W; x++) {
            map[y * FB_W + x] = ref_tri_covers(S, v0, v1, v2, x, y);
        }
    }
}

static float frand(float lo, float hi)
{
    return lo + (hi - lo) * (float)rand() / (float)RAND_MAX;
}

/* ================================================================== */
/* Tests                                                              */
/* ================================================================== */

static void test_setup_decode(void)
{
    cur_test = "setup_decode";
    clear_all();
    setup_defaults(true);
    R.regs.su_poly_offset_front_scale = f2u(12.0f);
    R.regs.su_poly_offset_front_offset = f2u(2.0f);
    R.regs.su_poly_offset_enable = 1;
    R.regs.rb3d_blendcntl = BLEND_ENABLE | BLEND_SEPARATE_ALPHA |
                            BLEND_FCN(FCN_MIN) | BLEND_SRC(BF_SRC_ALPHA) |
                            BLEND_DST(BF_ONE_MINUS_SRC_ALPHA);
    R.regs.rb3d_ablendcntl = BLEND_FCN(FCN_MAX) | BLEND_SRC(BF_ONE) |
                             BLEND_DST(BF_ZERO);
    R.regs.zb_cntl = ZB_Z_ENABLE | ZB_Z_WRITE_ENABLE | ZB_STENCIL_ENABLE;
    R.regs.zb_zstencilcntl = ZSC(ZS_LEQUAL, ZS_EQUAL, ZS_KEEP, ZS_INCR, ZS_DECR);
    R.regs.zb_stencilrefmask = SREFMASK(0x12, 0x34, 0x56);
    R.regs.ga_line_cntl = 18 | (2u << 16);
    R.regs.ga_point_size = (30u << 16) | 18u;
    R.regs.fg_alpha_func = ALPHA_FUNC_ENABLE | (4u << 8) | ALPHA_FP16;
    R.regs.fg_alpha_value = r5xx_float_to_half(0.75f);
    R.regs.rb3d_constant_color_ar = 1023u | (0u << 16);
    R.regs.rb3d_constant_color_gb = 511u | (255u << 16);
    if (!do_setup()) return;

    CHECK(R.d.subpix == 12, "subpix %d", R.d.subpix);
    CHECK(R.d.sc_x0 == 0 && R.d.sc_y0 == 0 && R.d.sc_x1 == 63 && R.d.sc_y1 == 63,
          "scissor %d %d %d %d", R.d.sc_x0, R.d.sc_y0, R.d.sc_x1, R.d.sc_y1);
    CHECK(R.rt.num_cbufs == 1 && R.rt.cbuf_enabled[0] && !R.rt.cbuf_enabled[1],
          "cbufs");
    CHECK(R.rt.cbuf[0].pitch_bytes == FB_W * 4 && R.rt.cbuf[0].bpp == 4 &&
          R.rt.cbuf[0].gpu_addr == GPU_BASE, "cbuf layout");
    CHECK(R.rt.cbuf_format[0] == R5XX_COLOR_ARGB8888, "cbuf format");
    CHECK(R.rt.zbuf_enabled && !R.rt.z_is_16 && R.rt.has_stencil &&
          R.rt.zbuf.bpp == 4, "zbuf");
    CHECK(R.d.po_front && !R.d.po_back && R.d.po_front_scale == 12.0f &&
          R.d.po_front_offset == 2.0f, "poly offset");
    CHECK(R.d.depth_scale == 16777215.0f, "depth scale %f", R.d.depth_scale);
    CHECK(R.d.blend_enable && R.d.separate_alpha && R.d.rgb_eq == FCN_MIN &&
          R.d.rgb_src == BF_SRC_ALPHA && R.d.rgb_dst == BF_ONE_MINUS_SRC_ALPHA &&
          R.d.a_eq == FCN_MAX && R.d.a_src == BF_ONE && R.d.a_dst == BF_ZERO,
          "blend decode");
    CHECK(R.d.z_enable && R.d.z_write && R.d.z_func == ZS_LEQUAL, "z decode");
    CHECK(R.d.stencil_enable && R.d.s_func[0] == ZS_EQUAL &&
          R.d.s_fail[0] == ZS_KEEP && R.d.s_zpass[0] == ZS_INCR &&
          R.d.s_zfail[0] == ZS_DECR, "stencil decode");
    CHECK(R.d.s_ref[0] == 0x12 && R.d.s_mask[0] == 0x34 && R.d.s_wmask[0] == 0x56,
          "stencil refmask");
    CHECK(R.d.s_ref[1] == 0x12 && R.d.s_func[1] == ZS_EQUAL, "back = front");
    CHECK(fabsf(R.d.line_half_w - 1.5f) < 1e-6f, "line width %f", R.d.line_half_w);
    CHECK(fabsf(R.d.point_half_w - 2.5f) < 1e-6f &&
          fabsf(R.d.point_half_h - 1.5f) < 1e-6f, "point size");
    CHECK(R.d.alpha_test && R.d.alpha_func == 4 && R.d.alpha_ref == 0.75f,
          "alpha test %d %f", R.d.alpha_func, R.d.alpha_ref);
    CHECK(fabsf(R.d.blend_const.f[0] - 1.0f) < 1e-6f &&
          fabsf(R.d.blend_const.f[1] - 255.0f / 1023.0f) < 1e-6f &&
          fabsf(R.d.blend_const.f[2] - 511.0f / 1023.0f) < 1e-6f &&
          R.d.blend_const.f[3] == 0.0f, "r500 const color");
    CHECK(R.d.provoking == 3, "provoking");

    /* R300 variant: scissor offset, 8-bit alpha ref, ARGB const color */
    setup_defaults(false);
    R.regs.fg_alpha_func = ALPHA_FUNC_ENABLE | (1u << 8) | 0x80;
    R.regs.rb3d_blend_color = 0x80FF4000;
    R.regs.gb_tile_config = GB_SUBPIXEL_1_16;
    if (!do_setup()) return;
    CHECK(R.d.subpix == 16, "subpix16");
    CHECK(R.d.sc_x0 == 0 && R.d.sc_x1 == 63 && R.d.sc_y1 == 63,
          "r300 scissor %d %d", R.d.sc_x0, R.d.sc_x1);
    CHECK(fabsf(R.d.alpha_ref - 128.0f / 255.0f) < 1e-6f, "r300 alpha ref");
    CHECK(fabsf(R.d.blend_const.f[0] - 1.0f) < 1e-6f &&
          fabsf(R.d.blend_const.f[1] - 64.0f / 255.0f) < 1e-6f &&
          R.d.blend_const.f[2] == 0.0f &&
          fabsf(R.d.blend_const.f[3] - 128.0f / 255.0f) < 1e-6f,
          "r300 const color");

    /* unsupported color format must be refused */
    setup_defaults(true);
    R.regs.rb3d_colorpitch[0] = FB_W | (11u << 21);
    {
        char err[64];
        CHECK(!r5xx_raster_setup(&R, err, sizeof(err)), "bad format accepted");
    }
}

static void test_triangle_coverage(void)
{
    int iter, mode;
    cur_test = "triangle_coverage";
    for (mode = 0; mode < 2; mode++) {
        int S = mode ? 16 : 12;
        for (iter = 0; iter < 150; iter++) {
            R5xxVertex v0, v1, v2;
            uint8_t exp[FB_W * FB_H], got[FB_W * FB_H];
            int i;
            clear_all();
            setup_defaults(true);
            R.regs.gb_tile_config = mode ? GB_SUBPIXEL_1_16 : 0;
            if (!do_setup()) return;
            /* mix of arbitrary floats and grid-aligned coordinates */
            v0 = vtx(frand(-8, 72), frand(-8, 72), 0.5f, 1, 1, 1, 1, 1);
            v1 = vtx(frand(-8, 72), frand(-8, 72), 0.5f, 1, 1, 1, 1, 1);
            v2 = vtx(frand(-8, 72), frand(-8, 72), 0.5f, 1, 1, 1, 1, 1);
            if (iter & 1) {
                v0.x = floorf(v0.x); v1.y = floorf(v1.y) + 0.5f;
                v2.x = floorf(v2.x) + 0.5f; v2.y = floorf(v2.y);
            }
            tri(&v0, &v1, &v2);
            ref_tri_map(S, &v0, &v1, &v2, exp);
            got_map_cb(got);
            for (i = 0; i < FB_W * FB_H; i++) {
                if (got[i] != exp[i]) break;
            }
            CHECK(i == FB_W * FB_H, "coverage mismatch S=%d iter %d "
                  "(%g,%g) (%g,%g) (%g,%g)", S, iter, v0.x, v0.y, v1.x, v1.y,
                  v2.x, v2.y);
            if (i != FB_W * FB_H) {
                dump_maps(got, exp, FB_W, FB_H);
                return;
            }
            /* every covered pixel must carry the attribute color */
            for (i = 0; i < FB_W * FB_H; i++) {
                if (exp[i] && rd32(CB0_OFF + i * 4) != 0xffffffffu) break;
            }
            CHECK(i == FB_W * FB_H, "color mismatch at %d", i);
        }
    }
    /* statistics: a 2x2 quad fully covered */
    {
        R5xxVertex a = vtx(0, 0, 0.5f, 1, 1, 0, 0, 1);
        R5xxVertex b = vtx(2, 0, 0.5f, 1, 1, 0, 0, 1);
        R5xxVertex c = vtx(2, 2, 0.5f, 1, 1, 0, 0, 1);
        R5xxVertex d = vtx(0, 2, 0.5f, 1, 1, 0, 0, 1);
        clear_all();
        setup_defaults(true);
        if (!do_setup()) return;
        tri(&a, &b, &c);
        tri(&a, &c, &d);
        CHECK(R.pixels_in == 4 && R.pixels_out == 4 && R.quads == 2 &&
              R.zpass_count == 4, "stats in=%llu out=%llu quads=%llu zp=%llu",
              (unsigned long long)R.pixels_in, (unsigned long long)R.pixels_out,
              (unsigned long long)R.quads, (unsigned long long)R.zpass_count);
    }
}

static float snap12(float v)
{
    return roundf(v * 12.0f) / 12.0f;
}

static void test_shared_edges(void)
{
    int iter;
    cur_test = "shared_edges";
    for (iter = 0; iter < 200; iter++) {
        R5xxVertex a, b, c, d;
        float side_b, side_d;
        uint8_t exp[FB_W * FB_H], m1[FB_W * FB_H], m2[FB_W * FB_H];
        int i, bad = -1;

        clear_all();
        setup_defaults(true);
        R.regs.rb3d_blendcntl = BLEND_ENABLE | BLEND_FCN(FCN_ADD_CLAMP) |
                                BLEND_SRC(BF_ONE) | BLEND_DST(BF_ONE);
        if (!do_setup()) return;
        a = vtx(frand(0, 64), frand(0, 64), 0.5f, 1, 1 / 255.0f, 0, 0, 1);
        c = vtx(frand(0, 64), frand(0, 64), 0.5f, 1, 1 / 255.0f, 0, 0, 1);
        if (iter & 1) {           /* grid aligned edge to stress ties */
            a.x = floorf(a.x); a.y = floorf(a.y) + 0.5f;
            c.x = floorf(c.x) + 0.5f; c.y = floorf(c.y);
        }
        /* b and d strictly on opposite sides of a-c after snapping to the
         * 1/12 subpixel grid, or the two triangles really overlap */
        do {
            b = vtx(frand(0, 64), frand(0, 64), 0.5f, 1, 1 / 255.0f, 0, 0, 1);
            d = vtx(frand(0, 64), frand(0, 64), 0.5f, 1, 1 / 255.0f, 0, 0, 1);
            side_b = (snap12(c.x) - snap12(a.x)) * (snap12(b.y) - snap12(a.y)) -
                     (snap12(c.y) - snap12(a.y)) * (snap12(b.x) - snap12(a.x));
            side_d = (snap12(c.x) - snap12(a.x)) * (snap12(d.y) - snap12(a.y)) -
                     (snap12(c.y) - snap12(a.y)) * (snap12(d.x) - snap12(a.x));
        } while (side_b * side_d >= 0);
        tri(&a, &b, &c);
        tri(&a, &c, &d);
        ref_tri_map(12, &a, &b, &c, m1);
        ref_tri_map(12, &a, &c, &d, m2);
        for (i = 0; i < FB_W * FB_H; i++) {
            uint32_t red = (rd32(CB0_OFF + i * 4) >> 16) & 0xff;
            exp[i] = m1[i] || m2[i];
            if (m1[i] && m2[i]) { bad = i; break; }       /* ref overlap */
            if (red > 1) { bad = i; break; }              /* drawn twice */
            if ((red == 1) != (exp[i] != 0)) { bad = i; break; }
        }
        CHECK(bad < 0, "shared edge iter %d: pixel (%d,%d) count=%u ref1=%d ref2=%d",
              iter, bad % FB_W, bad / FB_W,
              bad >= 0 ? (rd32(CB0_OFF + bad * 4) >> 16) & 0xff : 0,
              bad >= 0 ? m1[bad] : 0, bad >= 0 ? m2[bad] : 0);
        if (bad >= 0) {
            compare_maps(exp);
            return;
        }
    }
}

static void fullscreen_tri(float r, float g, float b, float a)
{
    R5xxVertex v0 = vtx(-1, -1, 0.5f, 1, r, g, b, a);
    R5xxVertex v1 = vtx(200, -1, 0.5f, 1, r, g, b, a);
    R5xxVertex v2 = vtx(-1, 200, 0.5f, 1, r, g, b, a);
    tri(&v0, &v1, &v2);
}

static void expect_rect(uint8_t *exp, int x0, int y0, int x1, int y1)
{
    int x, y;
    memset(exp, 0, FB_W * FB_H);
    for (y = y0; y <= y1; y++)
        for (x = x0; x <= x1; x++)
            exp[y * FB_W + x] = 1;
}

static void test_scissor(void)
{
    uint8_t exp[FB_W * FB_H];
    int r500;
    cur_test = "scissor";
    for (r500 = 0; r500 < 2; r500++) {
        int off = r500 ? 0 : 1440;
        clear_all();
        setup_defaults(r500);
        R.regs.sc_scissors_tl = (8 + off) | ((8 + off) << 13);
        R.regs.sc_scissors_br = (15 + off) | ((20 + off) << 13);
        if (!do_setup()) return;
        fullscreen_tri(1, 1, 1, 1);
        expect_rect(exp, 8, 8, 15, 20);
        CHECK(compare_maps(exp), "scissor r500=%d", r500);

        /* clip rect 0 with the "inside rect 0" rule (Mesa: 0xAAAA) */
        clear_all();
        R.regs.sc_clip_a[0] = (4 + off) | ((10 + off) << 13);
        R.regs.sc_clip_b[0] = (11 + off) | ((30 + off) << 13);
        R.regs.sc_clip_rule = 0xAAAA;
        if (!do_setup()) return;
        fullscreen_tri(1, 1, 1, 1);
        expect_rect(exp, 8, 10, 11, 20);
        CHECK(compare_maps(exp), "cliprect r500=%d", r500);

        /* rule 0x5555: only pixels OUTSIDE rect 0 */
        clear_all();
        R.regs.sc_clip_rule = 0x5555;
        if (!do_setup()) return;
        fullscreen_tri(1, 1, 1, 1);
        expect_rect(exp, 8, 8, 15, 20);
        {
            int x, y;
            for (y = 10; y <= 20; y++)
                for (x = 8; x <= 11; x++)
                    exp[y * FB_W + x] = 0;
        }
        CHECK(compare_maps(exp), "cliprect inverse r500=%d", r500);
    }
}

static void test_cull(void)
{
    /* CW on screen (y down): (0,0) -> (20,0) -> (0,20); CCW: reversed */
    R5xxVertex a = vtx(0, 0, 0.5f, 1, 1, 1, 1, 1);
    R5xxVertex b = vtx(20, 0, 0.5f, 1, 1, 1, 1, 1);
    R5xxVertex c = vtx(0, 20, 0.5f, 1, 1, 1, 1, 1);
    struct { uint32_t mode; bool cw_drawn, ccw_drawn; } cases[] = {
        { 0,                          true,  true  },
        { CULL_FRONT,                 true,  false },  /* front = CCW */
        { CULL_BACK,                  false, true  },
        { CULL_FRONT | CULL_BACK,     false, false },
        { FRONT_FACE_CW | CULL_FRONT, false, true  },  /* front = CW */
        { FRONT_FACE_CW | CULL_BACK,  true,  false },
    };
    unsigned i;
    cur_test = "cull";
    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        clear_all();
        setup_defaults(true);
        R.regs.su_cull_mode = cases[i].mode;
        if (!do_setup()) return;
        tri(&a, &b, &c);
        CHECK((cb_pixel(2, 2) != 0) == cases[i].cw_drawn, "cull mode %x CW",
              cases[i].mode);
        clear_all();
        tri(&a, &c, &b);
        CHECK((cb_pixel(2, 2) != 0) == cases[i].ccw_drawn, "cull mode %x CCW",
              cases[i].mode);
    }
}

static void test_depth(void)
{
    R5xxVertex a, b, c, d;
    int order;
    cur_test = "depth";
    for (order = 0; order < 2; order++) {
        clear_all();
        fill_zb(0xffffff00u);             /* far, stencil 0 */
        setup_defaults(true);
        R.regs.zb_cntl = ZB_Z_ENABLE | ZB_Z_WRITE_ENABLE;
        R.regs.zb_zstencilcntl = ZSC(ZS_LESS, 0, 0, 0, 0);
        if (!do_setup()) return;
        a = vtx(0, 0, 0.5f, 1, 1, 0, 0, 1);
        b = vtx(32, 0, 0.5f, 1, 1, 0, 0, 1);
        c = vtx(0, 32, 0.5f, 1, 1, 0, 0, 1);
        d = vtx(0, 0, 0.25f, 1, 0, 1, 0, 1);
        if (order == 0) {
            tri(&a, &b, &c);
            tri(&d, &b, &c);               /* nearer, drawn second */
            d.z = 0.25f;
        } else {
            tri(&d, &b, &c);
            tri(&a, &b, &c);               /* farther, drawn second: rejected */
        }
        /* pixel (1,1): nearer triangle visible, z interpolated close to
         * 0.25.  Its color is Gouraud shaded from green d to red b/c: at
         * the pixel center (1.5,1.5) red = 3/32 -> 0x18, green 0xe7. */
        CHECK(cb_pixel(1, 1) == 0xff18e700u, "order %d color %08x", order,
              cb_pixel(1, 1));
        {
            /* exact depth: d.z plane: z = 0.25 + (0.25/32)*(x_c) ... vertex
             * d at (0,0) z=.25, b at (32,0) z=.5, c at (0,32) z=.5 */
            double zc = 0.25 + 0.25 * (1.5 / 32.0) + 0.25 * (1.5 / 32.0);
            uint32_t ez = (uint32_t)(zc * 16777215.0 + 0.5);
            CHECK(zb_z24(1, 1) == ez || zb_z24(1, 1) + 1 == ez ||
                  zb_z24(1, 1) == ez + 1, "z24 %06x expected %06x",
                  zb_z24(1, 1), ez);
            CHECK(zb_s8(1, 1) == 0, "stencil touched");
        }
        CHECK(zb_z24(40, 40) == 0xffffff, "untouched depth");
    }
    /* equal depth, LEQUAL vs LESS */
    clear_all();
    fill_zb(0x800000ffu);                  /* z = 0x800000 = 0.5 */
    R.regs.zb_zstencilcntl = ZSC(ZS_LESS, 0, 0, 0, 0);
    if (!do_setup()) return;
    tri(&a, &b, &c);
    CHECK(cb_pixel(1, 1) == 0, "LESS with equal depth drew");
    R.regs.zb_zstencilcntl = ZSC(ZS_LEQUAL, 0, 0, 0, 0);
    if (!do_setup()) return;
    tri(&a, &b, &c);
    CHECK(cb_pixel(1, 1) == 0xffff0000u, "LEQUAL with equal depth");
    CHECK(zb_s8(1, 1) == 0xff, "stencil byte preserved");
    /* z write disabled */
    clear_all();
    fill_zb(0xffffff00u);
    R.regs.zb_cntl = ZB_Z_ENABLE;
    if (!do_setup()) return;
    tri(&a, &b, &c);
    CHECK(cb_pixel(1, 1) == 0xffff0000u && zb_z24(1, 1) == 0xffffff,
          "z write disabled");
    /* Z_WRITE_ENABLE without Z_ENABLE: no write (Mesa relies on this) */
    R.regs.zb_cntl = ZB_Z_WRITE_ENABLE;
    if (!do_setup()) return;
    tri(&a, &b, &c);
    CHECK(zb_z24(1, 1) == 0xffffff, "write without test");
    /* Z16 buffer */
    clear_all();
    memset(vram + ZB_OFF, 0xff, FB_W * FB_H * 2);
    setup_defaults(true);
    R.regs.zb_format = 0;
    R.regs.zb_cntl = ZB_Z_ENABLE | ZB_Z_WRITE_ENABLE;
    R.regs.zb_zstencilcntl = ZSC(ZS_LESS, 0, 0, 0, 0);
    if (!do_setup()) return;
    CHECK(R.rt.z_is_16 && !R.rt.has_stencil && R.rt.zbuf.bpp == 2, "z16 layout");
    {
        R5xxVertex p = vtx(5.5f, 5.5f, 0.25f, 1, 1, 1, 1, 1);
        r5xx_raster_point(&R, &p);
        CHECK(zb_z16(5, 5) == 0x4000, "z16 value %04x", zb_z16(5, 5));
        p.z = 0.5f;
        r5xx_raster_point(&R, &p);
        CHECK(zb_z16(5, 5) == 0x4000, "z16 LESS rejected");
        p.z = 1.0f;
        p.x = 6.5f;
        r5xx_raster_point(&R, &p);
        CHECK(zb_z16(6, 5) == 0xffff, "z16 max %04x", zb_z16(6, 5));
    }
}

static void test_stencil(void)
{
    R5xxVertex a = vtx(0, 0, 0.5f, 1, 1, 1, 1, 1);
    R5xxVertex b = vtx(16, 0, 0.5f, 1, 1, 1, 1, 1);
    R5xxVertex c = vtx(0, 16, 0.5f, 1, 1, 1, 1, 1);
    cur_test = "stencil";
    clear_all();
    setup_defaults(true);
    R.regs.zb_cntl = ZB_STENCIL_ENABLE;
    R.regs.zb_zstencilcntl = ZSC(ZS_ALWAYS, ZS_ALWAYS, ZS_KEEP, ZS_INCR, ZS_KEEP);
    R.regs.zb_stencilrefmask = SREFMASK(0, 0xff, 0xff);
    if (!do_setup()) return;
    tri(&a, &b, &c);
    tri(&a, &b, &c);
    CHECK(zb_s8(1, 1) == 2 && zb_s8(14, 14) == 0, "INCR twice: %d", zb_s8(1, 1));
    CHECK(zb_z24(1, 1) == 0, "depth touched without Z_ENABLE");
    /* EQUAL 2 passes only where s == 2 */
    clear_all();
    wr32(ZB_OFF + (1 * FB_W + 1) * 4, 0x00000002u);
    R.regs.zb_zstencilcntl = ZSC(ZS_ALWAYS, ZS_EQUAL, ZS_KEEP, ZS_KEEP, ZS_KEEP);
    R.regs.zb_stencilrefmask = SREFMASK(2, 0xff, 0xff);
    if (!do_setup()) return;
    tri(&a, &b, &c);
    CHECK(cb_pixel(1, 1) != 0 && cb_pixel(2, 2) == 0, "EQUAL");
    /* mask: ref 0x12, mask 0x0f -> compares 2 with s & 0x0f */
    clear_all();
    wr32(ZB_OFF + (1 * FB_W + 1) * 4, 0x000000f2u);
    R.regs.zb_stencilrefmask = SREFMASK(0x12, 0x0f, 0xff);
    if (!do_setup()) return;
    tri(&a, &b, &c);
    CHECK(cb_pixel(1, 1) != 0 && cb_pixel(2, 2) == 0, "masked compare");
    /* sfail REPLACE with NEVER, write mask 0x0f */
    clear_all();
    fill_zb(0x000000A0u);
    R.regs.zb_zstencilcntl = ZSC(ZS_ALWAYS, ZS_NEVER, ZS_REPLACE, ZS_KEEP, ZS_KEEP);
    R.regs.zb_stencilrefmask = SREFMASK(0xff, 0xff, 0x0f);
    if (!do_setup()) return;
    tri(&a, &b, &c);
    CHECK(zb_s8(1, 1) == 0xAF && cb_pixel(1, 1) == 0, "sfail REPLACE wmask: %02x",
          zb_s8(1, 1));
    CHECK(zb_s8(20, 20) == 0xA0, "outside untouched");
    /* zfail DECR (z NEVER), zpass unaffected */
    clear_all();
    fill_zb(0x00000002u);
    R.regs.zb_cntl = ZB_STENCIL_ENABLE | ZB_Z_ENABLE;
    R.regs.zb_zstencilcntl = ZSC(ZS_NEVER, ZS_ALWAYS, ZS_KEEP, ZS_INCR, ZS_DECR);
    R.regs.zb_stencilrefmask = SREFMASK(0, 0xff, 0xff);
    if (!do_setup()) return;
    tri(&a, &b, &c);
    CHECK(zb_s8(1, 1) == 1 && cb_pixel(1, 1) == 0, "zfail DECR: %d", zb_s8(1, 1));
    /* clamping and wrapping */
    {
        struct { int op; uint8_t in, out; } ops[] = {
            { ZS_INCR, 255, 255 }, { ZS_DECR, 0, 0 }, { ZS_INCR_WRAP, 255, 0 },
            { ZS_DECR_WRAP, 0, 255 }, { ZS_INVERT, 0x0f, 0xf0 }, { ZS_ZERO, 7, 0 },
            { ZS_REPLACE, 7, 0x33 }, { ZS_KEEP, 9, 9 },
        };
        unsigned i;
        R.regs.zb_cntl = ZB_STENCIL_ENABLE;
        R.regs.zb_stencilrefmask = SREFMASK(0x33, 0xff, 0xff);
        for (i = 0; i < sizeof(ops) / sizeof(ops[0]); i++) {
            clear_all();
            fill_zb(ops[i].in);
            R.regs.zb_zstencilcntl = ZSC(ZS_ALWAYS, ZS_ALWAYS, ZS_KEEP, ops[i].op, ZS_KEEP);
            if (!do_setup()) return;
            tri(&a, &b, &c);
            CHECK(zb_s8(1, 1) == ops[i].out, "op %d: %d -> %d (want %d)",
                  ops[i].op, ops[i].in, zb_s8(1, 1), ops[i].out);
        }
    }
    /* two-sided: back faces (CW with FRONT_FACE_CCW) use the back ops and,
     * on R500 with REFMASK_FRONT_BACK, the BF ref */
    clear_all();
    R.regs.zb_cntl = ZB_STENCIL_ENABLE | ZB_STENCIL_FRONT_BACK |
                     ZB_STENCIL_REFMASK_FRONT_BACK;
    R.regs.zb_zstencilcntl = ZSC(ZS_ALWAYS, ZS_ALWAYS, ZS_KEEP, ZS_REPLACE, ZS_KEEP) |
                             ZSC_BACK(ZS_ALWAYS, ZS_KEEP, ZS_REPLACE, ZS_KEEP);
    R.regs.zb_stencilrefmask = SREFMASK(0x11, 0xff, 0xff);
    R.regs.zb_stencilrefmask_bf = SREFMASK(0x55, 0xff, 0xff);
    if (!do_setup()) return;
    tri(&a, &b, &c);                       /* CW = back */
    CHECK(zb_s8(1, 1) == 0x55, "back ref %02x", zb_s8(1, 1));
    tri(&a, &c, &b);                       /* CCW = front */
    CHECK(zb_s8(1, 1) == 0x11, "front ref %02x", zb_s8(1, 1));
    /* R300: no BF register, both faces use the front ref */
    clear_all();
    setup_defaults(false);
    R.regs.zb_cntl = ZB_STENCIL_ENABLE | ZB_STENCIL_FRONT_BACK |
                     ZB_STENCIL_REFMASK_FRONT_BACK;
    R.regs.zb_zstencilcntl = ZSC(ZS_ALWAYS, ZS_ALWAYS, ZS_KEEP, ZS_REPLACE, ZS_KEEP) |
                             ZSC_BACK(ZS_ALWAYS, ZS_KEEP, ZS_INCR, ZS_KEEP);
    R.regs.zb_stencilrefmask = SREFMASK(0x11, 0xff, 0xff);
    R.regs.zb_stencilrefmask_bf = SREFMASK(0x55, 0xff, 0xff);
    if (!do_setup()) return;
    tri(&a, &b, &c);
    CHECK(zb_s8(1, 1) == 1, "r300 back op INCR: %02x", zb_s8(1, 1));
    tri(&a, &c, &b);
    CHECK(zb_s8(1, 1) == 0x11, "r300 front REPLACE: %02x", zb_s8(1, 1));
}

/* independent float reference of the GL blend equation */
static float ref_factor(int f, int ch, const float *s, const float *d,
                        const float *k)
{
    switch (f) {
    case BF_ZERO: return 0;
    case BF_ONE: return 1;
    case BF_SRC_COLOR: return s[ch];
    case BF_ONE_MINUS_SRC_COLOR: return 1 - s[ch];
    case BF_DST_COLOR: return d[ch];
    case BF_ONE_MINUS_DST_COLOR: return 1 - d[ch];
    case BF_SRC_ALPHA: return s[3];
    case BF_ONE_MINUS_SRC_ALPHA: return 1 - s[3];
    case BF_DST_ALPHA: return d[3];
    case BF_ONE_MINUS_DST_ALPHA: return 1 - d[3];
    case BF_SRC_ALPHA_SATURATE: return ch == 3 ? 1 : (s[3] < 1 - d[3] ? s[3] : 1 - d[3]);
    case BF_CONST_COLOR: return k[ch];
    case BF_ONE_MINUS_CONST_COLOR: return 1 - k[ch];
    case BF_CONST_ALPHA: return k[3];
    case BF_ONE_MINUS_CONST_ALPHA: return 1 - k[3];
    }
    return 1;
}

static void ref_blend(int eq, int sf, int df, const float *s, const float *d,
                      const float *k, float *out)
{
    int ch;
    for (ch = 0; ch < 4; ch++) {
        float a = s[ch] * ref_factor(sf, ch, s, d, k);
        float b = d[ch] * ref_factor(df, ch, s, d, k);
        float v;
        switch (eq) {
        case FCN_ADD_CLAMP: v = a + b; break;
        case FCN_SUB_CLAMP: v = a - b; break;
        case FCN_RSUB_CLAMP: v = b - a; break;
        case FCN_MIN: v = s[ch] < d[ch] ? s[ch] : d[ch]; break;
        default: v = s[ch] > d[ch] ? s[ch] : d[ch]; break;
        }
        out[ch] = v;
    }
}

static int u8(float v)
{
    v = v < 0 ? 0 : v > 1 ? 1 : v;
    return (int)(v * 255.0f + 0.5f);
}

static void test_blend(void)
{
    static const int eqs[] = { FCN_ADD_CLAMP, FCN_SUB_CLAMP, FCN_RSUB_CLAMP,
                               FCN_MIN, FCN_MAX };
    unsigned e;
    int sf, df, bad = 0;
    cur_test = "blend";
    srand(1234);
    for (e = 0; e < 5 && !bad; e++) {
        for (sf = BF_ZERO; sf <= BF_ONE_MINUS_CONST_ALPHA && !bad; sf++) {
            for (df = BF_ZERO; df <= BF_ONE_MINUS_CONST_ALPHA && !bad; df++) {
                float s[4], d[4], k[4], ref[4];
                uint32_t dst, got;
                int ch, i;
                for (i = 0; i < 4; i++) {
                    s[i] = u8(frand(0, 1)) / 255.0f;
                    d[i] = u8(frand(0, 1)) / 255.0f;
                    k[i] = u8(frand(0, 1)) / 255.0f;
                }
                dst = (u8(d[3]) << 24) | (u8(d[0]) << 16) | (u8(d[1]) << 8) | u8(d[2]);
                clear_all();
                fill_cb(dst);
                setup_defaults(false);
                R.regs.rb3d_blendcntl = BLEND_ENABLE | BLEND_FCN(eqs[e]) |
                                        BLEND_SRC(sf) | BLEND_DST(df);
                R.regs.rb3d_blend_color = (u8(k[3]) << 24) | (u8(k[0]) << 16) |
                                          (u8(k[1]) << 8) | u8(k[2]);
                if (!do_setup()) return;
                draw_pixel(3, 4, s[0], s[1], s[2], s[3]);
                got = cb_pixel(3, 4);
                ref_blend(eqs[e], sf, df, s, d, k, ref);
                for (ch = 0; ch < 4; ch++) {
                    int shift = ch == 3 ? 24 : 16 - 8 * ch;
                    int g = (got >> shift) & 0xff, r = u8(ref[ch]);
                    if (abs(g - r) > 1) {
                        bad = 1;
                        CHECK(0, "eq %d src %d dst %d ch %d: got %d want %d "
                              "(s=%g d=%g k=%g)", eqs[e], sf, df, ch, g, r,
                              s[ch], d[ch], k[ch]);
                    }
                }
                CHECK(cb_pixel(4, 4) == dst, "neighbor modified");
            }
        }
    }
    /* separate alpha: rgb ADD ONE/ONE, alpha SUB? use alpha = src*0 + dst*1 */
    clear_all();
    fill_cb(0x40202020u);
    setup_defaults(false);
    R.regs.rb3d_blendcntl = BLEND_ENABLE | BLEND_SEPARATE_ALPHA |
                            BLEND_FCN(FCN_ADD_CLAMP) | BLEND_SRC(BF_ONE) |
                            BLEND_DST(BF_ONE);
    R.regs.rb3d_ablendcntl = BLEND_FCN(FCN_ADD_CLAMP) | BLEND_SRC(BF_ZERO) |
                             BLEND_DST(BF_ONE);
    if (!do_setup()) return;
    draw_pixel(1, 1, 32 / 255.0f, 64 / 255.0f, 0xe0 / 255.0f, 1.0f);
    CHECK(cb_pixel(1, 1) == 0x404060ffu, "separate alpha %08x", cb_pixel(1, 1));
    /* blending disabled: straight copy */
    R.regs.rb3d_blendcntl = 0;
    if (!do_setup()) return;
    draw_pixel(1, 1, 32 / 255.0f, 64 / 255.0f, 0xe0 / 255.0f, 0.5f);
    CHECK(cb_pixel(1, 1) == 0x802040e0u, "no blend %08x", cb_pixel(1, 1));
    /* discard: SRC_ALPHA_0 must not touch the buffer even with mask on */
    R.regs.rb3d_blendcntl = BLEND_ENABLE | DISCARD_SRC_ALPHA_0 |
                            BLEND_FCN(FCN_ADD_CLAMP) | BLEND_SRC(BF_ONE) |
                            BLEND_DST(BF_ONE);
    if (!do_setup()) return;
    draw_pixel(1, 1, 1, 1, 1, 0.0f);
    CHECK(cb_pixel(1, 1) == 0x802040e0u, "discard alpha 0 %08x", cb_pixel(1, 1));
    draw_pixel(1, 1, 0, 0, 0x10 / 255.0f, 0.0f);
    CHECK(cb_pixel(1, 1) == 0x802040e0u, "discard alpha 0 (2)");
    /* R500 10-bit constant color: CONST_COLOR * src(1) + 0 */
    clear_all();
    setup_defaults(true);
    R.regs.rb3d_blendcntl = BLEND_ENABLE | BLEND_FCN(FCN_ADD_CLAMP) |
                            BLEND_SRC(BF_CONST_COLOR) | BLEND_DST(BF_ZERO);
    R.regs.rb3d_constant_color_ar = 1023u | (0u << 16);
    R.regs.rb3d_constant_color_gb = 0u | (511u << 16);
    if (!do_setup()) return;
    draw_pixel(2, 2, 1, 1, 1, 1);
    /* green 511/1023 * 255 = 127.4 -> 0x7f */
    CHECK(cb_pixel(2, 2) == 0x00ff7f00u, "r500 const %08x", cb_pixel(2, 2));
}

static void test_fp16_target(void)
{
    uint16_t h[4];
    int i;
    cur_test = "fp16_target";
    clear_all();
    setup_defaults(true);
    R.regs.rb3d_colorpitch[0] = FB_W | COLOR_FORMAT_ARGB16161616;
    R.regs.rb3d_coloroffset[0] = GPU_BASE + FP_OFF;
    R.regs.us_out_fmt[0] = OUT_FMT_C4_16_FP | OUT_FMT_RGBA;
    if (!do_setup()) return;
    CHECK(R.rt.cbuf_format[0] == R5XX_COLOR_ARGB16161616F && R.rt.cbuf[0].bpp == 8,
          "fp16 format");
    draw_pixel(3, 2, 1.5f, -0.5f, 0.25f, 2.0f);
    for (i = 0; i < 4; i++) {
        uint32_t off = FP_OFF + (2 * FB_W + 3) * 8 + i * 2;
        h[i] = vram[off] | (vram[off + 1] << 8);
    }
    CHECK(h[0] == r5xx_float_to_half(1.5f) && h[1] == r5xx_float_to_half(-0.5f) &&
          h[2] == r5xx_float_to_half(0.25f) && h[3] == r5xx_float_to_half(2.0f),
          "fp16 unclamped write %04x %04x %04x %04x", h[0], h[1], h[2], h[3]);
    /* ADD_NOCLAMP ONE/ONE: 1.5 + 1.5 = 3 */
    R.regs.rb3d_blendcntl = BLEND_ENABLE | BLEND_FCN(FCN_ADD_NOCLAMP) |
                            BLEND_SRC(BF_ONE) | BLEND_DST(BF_ONE);
    if (!do_setup()) return;
    draw_pixel(3, 2, 1.5f, -0.5f, 0.25f, 2.0f);
    {
        uint32_t off = FP_OFF + (2 * FB_W + 3) * 8;
        CHECK((vram[off] | (vram[off + 1] << 8)) == r5xx_float_to_half(3.0f),
              "noclamp add");
        CHECK((vram[off + 2] | (vram[off + 3] << 8)) == r5xx_float_to_half(-1.0f),
              "noclamp add neg");
    }
    /* ADD_CLAMP clamps to [0,1] */
    R.regs.rb3d_blendcntl = BLEND_ENABLE | BLEND_FCN(FCN_ADD_CLAMP) |
                            BLEND_SRC(BF_ONE) | BLEND_DST(BF_ONE);
    if (!do_setup()) return;
    draw_pixel(3, 2, 1.5f, -0.5f, 0.25f, 2.0f);
    {
        uint32_t off = FP_OFF + (2 * FB_W + 3) * 8;
        CHECK((vram[off] | (vram[off + 1] << 8)) == r5xx_float_to_half(1.0f),
              "clamp add");
        CHECK((vram[off + 2] | (vram[off + 3] << 8)) == r5xx_float_to_half(0.0f),
              "clamp add neg");
    }
    /* fp16 constant color and alpha ref */
    R.regs.rb3d_blendcntl = BLEND_ENABLE | BLEND_FCN(FCN_ADD_NOCLAMP) |
                            BLEND_SRC(BF_CONST_COLOR) | BLEND_DST(BF_ZERO);
    R.regs.rb3d_constant_color_ar = r5xx_float_to_half(4.0f) |
                                    ((uint32_t)r5xx_float_to_half(0.5f) << 16);
    R.regs.rb3d_constant_color_gb = r5xx_float_to_half(-2.0f) |
                                    ((uint32_t)r5xx_float_to_half(1.0f) << 16);
    if (!do_setup()) return;
    CHECK(R.d.blend_const.f[0] == 4.0f && R.d.blend_const.f[1] == 1.0f &&
          R.d.blend_const.f[2] == -2.0f && R.d.blend_const.f[3] == 0.5f,
          "fp16 const color");
    draw_pixel(3, 2, 1.0f, 1.0f, 1.0f, 1.0f);
    {
        /* The constant's fields address memory slots by ARGB name (AR's
         * low half = component 2, GB's low half = component 0), not the
         * channels the output selects put there: Mesa swaps R and B in
         * the constant for R16G16B16A16_FLOAT for exactly this reason. */
        uint32_t off = FP_OFF + (2 * FB_W + 3) * 8;
        CHECK((vram[off + 4] | (vram[off + 5] << 8)) == r5xx_float_to_half(4.0f) &&
              (vram[off] | (vram[off + 1] << 8)) == r5xx_float_to_half(-2.0f),
              "fp16 const blend");
    }
    /* logic ops are not applied to float targets */
    R.regs.rb3d_blendcntl = 0;
    R.regs.rb3d_ropcntl = ROP_ENABLE | ROP(0);     /* CLEAR */
    if (!do_setup()) return;
    draw_pixel(3, 2, 1.0f, 1.0f, 1.0f, 1.0f);
    {
        uint32_t off = FP_OFF + (2 * FB_W + 3) * 8;
        CHECK((vram[off] | (vram[off + 1] << 8)) == r5xx_float_to_half(1.0f),
              "rop on float target");
    }
}

static void test_color_mask(void)
{
    int m;
    cur_test = "color_mask";
    for (m = 0; m < 16; m++) {
        uint32_t got, exp = 0;
        int ch;
        clear_all();
        fill_cb(0x80808080u);
        setup_defaults(true);
        R.regs.rb3d_color_channel_mask = m;
        if (!do_setup()) return;
        draw_pixel(5, 5, 1.0f, 0.0f, 0.5f, 0.25f);
        got = cb_pixel(5, 5);
        /* hw bits: 0 = blue, 1 = green, 2 = red, 3 = alpha */
        for (ch = 0; ch < 4; ch++) {
            static const uint32_t src[4] = { 0x80, 0x00, 0xff, 0x40 }; /* B,G,R,A */
            exp |= ((m >> ch) & 1 ? src[ch] : 0x80u) << (8 * ch);
        }
        CHECK(got == exp, "mask %x: got %08x want %08x", m, got, exp);
        if (m == 0) {
            CHECK(R.pixels_out == 0, "mask 0 counted as written");
        }
    }
    /* RGB565: mask bits B,G,R only */
    clear_all();
    setup_defaults(true);
    R.regs.rb3d_colorpitch[0] = FB_W | COLOR_FORMAT_RGB565;
    R.regs.rb3d_color_channel_mask = 0x5;          /* B and R */
    if (!do_setup()) return;
    draw_pixel(5, 5, 1.0f, 1.0f, 1.0f, 1.0f);
    CHECK((vram[(5 * FB_W + 5) * 2] | (vram[(5 * FB_W + 5) * 2 + 1] << 8)) == 0xF81F,
          "565 mask %04x", vram[(5 * FB_W + 5) * 2] | (vram[(5 * FB_W + 5) * 2 + 1] << 8));
    /* independent masks (R500): buffer 1 only red */
    clear_all();
    fill_cb(0x80808080u);
    setup_defaults(true);
    R.regs.rb3d_cctl = CCTL_INDEPENDENT_CMASK;
    R.regs.rb3d_colorpitch[1] = FB_W | COLOR_FORMAT_ARGB8888;
    R.regs.rb3d_coloroffset[1] = GPU_BASE + CB1_OFF;
    R.regs.us_out_fmt[1] = OUT_FMT_BGRA;
    R.regs.rb3d_color_channel_mask = 0xf | (0x4 << 4);
    stub_us_mode = STUB_US_MRT;
    R.us.prog = (const R500UsDecoded *)&R;   /* any non-NULL: stub ignores it */
    if (!do_setup()) return;
    CHECK(R.rt.num_cbufs == 2 && R.rt.cbuf_enabled[1], "two cbufs");
    {
        R5xxVertex v = vtx(5.5f, 5.5f, 0.5f, 1, 1.0f, 0.0f, 0.5f, 0.25f);
        v.attr[1] = r5xx_vec4_make(0.0f, 1.0f, 1.0f, 1.0f);
        r5xx_raster_point(&R, &v);
    }
    CHECK(cb_pixel(5, 5) == 0x40ff0080u, "cb0 %08x", cb_pixel(5, 5));
    CHECK(cb1_pixel(5, 5) == 0x80008080u, "cb1 %08x", cb1_pixel(5, 5));
    stub_us_mode = STUB_US_PASSTHROUGH;
    /* without the independent bit, mask 0 applies to all */
    R.regs.rb3d_cctl = 0;
    R.us.prog = NULL;
    if (!do_setup()) return;
    fill_cb(0x80808080u);
    draw_pixel(5, 5, 1.0f, 0.0f, 0.5f, 0.25f);
    CHECK(cb1_pixel(5, 5) == 0x40ff0080u, "cb1 shared mask %08x", cb1_pixel(5, 5));
}

static void test_alpha_test(void)
{
    int f;
    cur_test = "alpha_test";
    for (f = 0; f < 8; f++) {
        /* R300: 8 bit ref 128 */
        float vals[3] = { 0.25f, 128.0f / 255.0f, 0.75f };
        int i;
        for (i = 0; i < 3; i++) {
            bool exp;
            float a = vals[i], ref = 128.0f / 255.0f;
            switch (f) {
            case 0: exp = false; break;
            case 1: exp = a < ref; break;
            case 2: exp = a == ref; break;
            case 3: exp = a <= ref; break;
            case 4: exp = a > ref; break;
            case 5: exp = a != ref; break;
            case 6: exp = a >= ref; break;
            default: exp = true; break;
            }
            clear_all();
            setup_defaults(false);
            R.regs.fg_alpha_func = ALPHA_FUNC_ENABLE | (f << 8) | 128;
            if (!do_setup()) return;
            draw_pixel(1, 1, 1, 1, 1, a);
            CHECK((cb_pixel(1, 1) != 0) == exp, "func %d alpha %g", f, a);
        }
    }
    /* R500 fp16 ref */
    clear_all();
    setup_defaults(true);
    R.regs.fg_alpha_func = ALPHA_FUNC_ENABLE | (4u << 8) | ALPHA_FP16;
    R.regs.fg_alpha_value = r5xx_float_to_half(0.5f);
    if (!do_setup()) return;
    draw_pixel(1, 1, 1, 1, 1, 0.6f);
    draw_pixel(2, 1, 1, 1, 1, 0.4f);
    CHECK(cb_pixel(1, 1) != 0 && cb_pixel(2, 1) == 0, "r500 fp16 alpha ref");
    /* R500 8-bit mode uses the FG_ALPHA_FUNC value */
    R.regs.fg_alpha_func = ALPHA_FUNC_ENABLE | (4u << 8) | ALPHA_8BIT | 200;
    if (!do_setup()) return;
    draw_pixel(1, 2, 1, 1, 1, 0.6f);
    draw_pixel(2, 2, 1, 1, 1, 0.9f);
    CHECK(cb_pixel(1, 2) == 0 && cb_pixel(2, 2) != 0, "r500 8bit alpha ref");
    /* alpha test failure must not write depth either */
    fill_zb(0xffffff00u);
    R.regs.zb_cntl = ZB_Z_ENABLE | ZB_Z_WRITE_ENABLE;
    R.regs.zb_zstencilcntl = ZSC(ZS_ALWAYS, 0, 0, 0, 0);
    if (!do_setup()) return;
    R.zpass_count = 0;                     /* earlier pixels passed */
    draw_pixel(3, 3, 1, 1, 1, 0.1f);
    CHECK(zb_z24(3, 3) == 0xffffff && R.zpass_count == 0, "alpha fail wrote z: z %06x zpass %llu",
          zb_z24(3, 3), (unsigned long long)R.zpass_count);
}

static void test_logic_op(void)
{
    int op;
    uint32_t s = 0xFF3C96C3u, d = 0xA5A5A5A5u;
    cur_test = "logic_op";
    for (op = 0; op < 16; op++) {
        uint32_t exp;
        switch (op) {
        case 0: exp = 0; break;
        case 1: exp = ~(s | d); break;
        case 2: exp = ~s & d; break;
        case 3: exp = ~s; break;
        case 4: exp = s & ~d; break;
        case 5: exp = ~d; break;
        case 6: exp = s ^ d; break;
        case 7: exp = ~(s & d); break;
        case 8: exp = s & d; break;
        case 9: exp = ~(s ^ d); break;
        case 10: exp = d; break;
        case 11: exp = ~s | d; break;
        case 12: exp = s; break;
        case 13: exp = s | ~d; break;
        case 14: exp = s | d; break;
        default: exp = 0xffffffffu; break;
        }
        clear_all();
        fill_cb(d);
        setup_defaults(true);
        R.regs.rb3d_ropcntl = ROP_ENABLE | ROP(op);
        /* blending is also enabled; logic op must win */
        R.regs.rb3d_blendcntl = BLEND_ENABLE | BLEND_FCN(FCN_ADD_CLAMP) |
                                BLEND_SRC(BF_ZERO) | BLEND_DST(BF_ZERO);
        if (!do_setup()) return;
        draw_pixel(7, 7, 0x3C / 255.0f, 0x96 / 255.0f, 0xC3 / 255.0f, 1.0f);
        CHECK(cb_pixel(7, 7) == exp, "rop %d: got %08x want %08x", op,
              cb_pixel(7, 7), exp);
    }
    /* logic op + color mask (only blue) */
    clear_all();
    fill_cb(d);
    R.regs.rb3d_ropcntl = ROP_ENABLE | ROP(6);          /* XOR */
    R.regs.rb3d_color_channel_mask = 1;
    if (!do_setup()) return;
    draw_pixel(7, 7, 0x3C / 255.0f, 0x96 / 255.0f, 0xC3 / 255.0f, 1.0f);
    CHECK(cb_pixel(7, 7) == ((d & 0xffffff00u) | ((s ^ d) & 0xff)),
          "rop + mask %08x", cb_pixel(7, 7));
}

static void test_tiled(void)
{
    R5xxSurfaceLayout l;
    uint32_t off, i;
    int count = 0;
    cur_test = "tiled";
    clear_all();
    setup_defaults(true);
    R.regs.rb3d_colorpitch[0] = FB_W | COLOR_FORMAT_ARGB8888 | COLOR_MICROTILE_ENABLE;
    R.regs.zb_depthpitch = FB_W | (1u << 17);
    R.regs.zb_cntl = ZB_Z_ENABLE | ZB_Z_WRITE_ENABLE;
    R.regs.zb_zstencilcntl = ZSC(ZS_ALWAYS, 0, 0, 0, 0);
    if (!do_setup()) return;
    CHECK(R.rt.cbuf[0].tiling == R5XX_TILE_MICRO && R.rt.cbuf[0].micro_mode == 1,
          "cbuf tiling decode");
    CHECK(R.rt.zbuf.tiling == R5XX_TILE_MICRO, "zbuf tiling decode");
    draw_pixel(5, 6, 1, 0, 0, 1);
    l = R.rt.cbuf[0];
    off = r5xx_surface_pixel_offset(&l, 5, 6);
    CHECK(off != 6 * FB_W * 4 + 5 * 4, "stub tiling is linear?");
    CHECK(rd32(CB0_OFF + off) == 0xffff0000u, "tiled pixel at %x: %08x", off,
          rd32(CB0_OFF + off));
    for (i = 0; i < FB_W * FB_H; i++) {
        if (rd32(CB0_OFF + i * 4) != 0) count++;
    }
    CHECK(count == 1, "%d pixels written", count);
    off = r5xx_surface_pixel_offset(&R.rt.zbuf, 5, 6);
    CHECK(rd32(ZB_OFF + off) == 0x80000000u, "tiled depth %08x", rd32(ZB_OFF + off));
    /* macro + micro combination maps to MICRO_MACRO */
    R.regs.rb3d_colorpitch[0] = FB_W | COLOR_FORMAT_ARGB8888 | COLOR_MICROTILE_ENABLE |
                                (1u << 16);
    if (!do_setup()) return;
    CHECK(R.rt.cbuf[0].tiling == R5XX_TILE_MICRO_MACRO, "micro+macro");
    R.regs.rb3d_colorpitch[0] = FB_W | COLOR_FORMAT_ARGB8888 | (1u << 16);
    if (!do_setup()) return;
    CHECK(R.rt.cbuf[0].tiling == R5XX_TILE_MACRO, "macro");
}

static void test_clear(void)
{
    r5xx_vec4 c = r5xx_vec4_make(0.5f, 0.25f, 1.0f, 0.0f);
    int x, y, bad = 0;
    cur_test = "clear";
    clear_all();
    setup_defaults(true);
    R.regs.rb3d_color_channel_mask = 0;     /* clears ignore the mask */
    if (!do_setup()) return;
    fill_zb(0x12345678u);
    r5xx_raster_clear(&R, 4, 5, 10, 9, true, &c, true, 0.5f, false, 0);
    for (y = 0; y < FB_H; y++) {
        for (x = 0; x < FB_W; x++) {
            bool in = x >= 4 && x < 10 && y >= 5 && y < 9;
            uint32_t ec = in ? 0x008040ffu : 0;
            uint32_t ez = in ? 0x80000078u : 0x12345678u;
            if (cb_pixel(x, y) != ec || zb_raw(x, y) != ez) bad++;
        }
    }
    CHECK(bad == 0, "%d bad pixels after color+depth clear", bad);
    r5xx_raster_clear(&R, 4, 5, 10, 9, false, &c, false, 0, true, 0xAB);
    CHECK(zb_raw(5, 6) == 0x800000ABu, "stencil-only clear %08x", zb_raw(5, 6));
    r5xx_raster_clear(&R, 0, 0, 64, 64, false, &c, true, 1.0f, true, 1);
    CHECK(zb_raw(63, 63) == 0xffffff01u && zb_raw(0, 0) == 0xffffff01u, "full clear %08x %08x",
          zb_raw(63, 63), zb_raw(0, 0));
    /* rect clamped to the pitch */
    r5xx_raster_clear(&R, -5, 0, 1000, 1, true, &c, false, 0, false, 0);
    CHECK(cb_pixel(63, 0) == 0x008040ffu && cb_pixel(0, 1) == 0, "clamped clear");
    /* Z16 clear */
    setup_defaults(true);
    R.regs.zb_format = 0;
    if (!do_setup()) return;
    r5xx_raster_clear(&R, 0, 0, 2, 1, false, &c, true, 0.25f, false, 0);
    CHECK(zb_z16(0, 0) == 0x4000 && zb_z16(1, 0) == 0x4000, "z16 clear %04x",
          zb_z16(0, 0));
}

static void test_lines(void)
{
    uint8_t exp[FB_W * FB_H];
    int i;
    cur_test = "lines";
    /* horizontal, width 1: pixels x=2..9 at y=5 */
    clear_all();
    setup_defaults(true);
    if (!do_setup()) return;
    {
        R5xxVertex a = vtx(2.5f, 5.5f, 0.5f, 1, 1, 1, 1, 1);
        R5xxVertex b = vtx(10.5f, 5.5f, 0.5f, 1, 1, 1, 1, 1);
        r5xx_raster_line(&R, &a, &b);
        expect_rect(exp, 2, 5, 9, 5);
        CHECK(compare_maps(exp), "horizontal line");
        /* reversed direction: x = 3..10 */
        clear_all();
        r5xx_raster_line(&R, &b, &a);
        expect_rect(exp, 3, 5, 10, 5);
        CHECK(compare_maps(exp), "horizontal line reversed");
    }
    /* vertical */
    clear_all();
    {
        R5xxVertex a = vtx(7.5f, 2.5f, 0.5f, 1, 1, 1, 1, 1);
        R5xxVertex b = vtx(7.5f, 12.5f, 0.5f, 1, 1, 1, 1, 1);
        r5xx_raster_line(&R, &a, &b);
        expect_rect(exp, 7, 2, 7, 11);
        CHECK(compare_maps(exp), "vertical line");
    }
    /* diagonal */
    clear_all();
    {
        R5xxVertex a = vtx(0.5f, 0.5f, 0.5f, 1, 1, 1, 1, 1);
        R5xxVertex b = vtx(8.5f, 8.5f, 0.5f, 1, 1, 1, 1, 1);
        r5xx_raster_line(&R, &a, &b);
        memset(exp, 0, sizeof(exp));
        for (i = 0; i < 8; i++) exp[i * FB_W + i] = 1;
        CHECK(compare_maps(exp), "diagonal line");
    }
    /* shallow slope: x-major, one pixel per column */
    clear_all();
    {
        R5xxVertex a = vtx(0.5f, 0.5f, 0.5f, 1, 1, 1, 1, 1);
        R5xxVertex b = vtx(16.5f, 4.5f, 0.5f, 1, 1, 1, 1, 1);
        int x, count = 0;
        r5xx_raster_line(&R, &a, &b);
        for (x = 0; x < 16; x++) {
            int y, n = 0;
            for (y = 0; y < FB_H; y++) if (cb_pixel(x, y)) n++;
            count += n;
            CHECK(n == 1, "column %d has %d pixels", x, n);
        }
        CHECK(count == 16 && cb_pixel(16, 4) == 0, "shallow line count %d", count);
        CHECK(cb_pixel(0, 0) != 0 && cb_pixel(15, 4) != 0, "shallow line ends");
    }
    /* wide horizontal line (width 3) covers rows 4..6 */
    clear_all();
    R.regs.ga_line_cntl = 18 | (2u << 16);
    if (!do_setup()) return;
    {
        R5xxVertex a = vtx(2.5f, 5.5f, 0.5f, 1, 1, 1, 1, 1);
        R5xxVertex b = vtx(10.5f, 5.5f, 0.5f, 1, 1, 1, 1, 1);
        r5xx_raster_line(&R, &a, &b);
        expect_rect(exp, 2, 4, 9, 6);
        CHECK(compare_maps(exp), "wide line");
    }
    /* color interpolation along the line (provoking irrelevant) */
    clear_all();
    R.regs.ga_line_cntl = 6 | (2u << 16);
    if (!do_setup()) return;
    {
        R5xxVertex a = vtx(0.5f, 3.5f, 0.5f, 1, 0, 0, 0, 1);
        R5xxVertex b = vtx(16.5f, 3.5f, 0.5f, 1, 1, 0, 0, 1);
        uint32_t p8 = cb_pixel(8, 3);
        r5xx_raster_line(&R, &a, &b);
        p8 = (cb_pixel(8, 3) >> 16) & 0xff;
        CHECK(p8 == 128 || p8 == 127, "line interpolation %u", p8);
        /* flat: color of the last (provoking) vertex */
        clear_all();
        R.flat_inputs = 1;
        r5xx_raster_line(&R, &a, &b);
        CHECK(((cb_pixel(8, 3) >> 16) & 0xff) == 255, "flat line");
        R.flat_inputs = 0;
    }
    /* scissored line */
    clear_all();
    R.regs.sc_scissors_tl = 4 | (0 << 13);
    R.regs.sc_scissors_br = 6 | (63 << 13);
    if (!do_setup()) return;
    {
        R5xxVertex a = vtx(0.5f, 3.5f, 0.5f, 1, 1, 1, 1, 1);
        R5xxVertex b = vtx(16.5f, 3.5f, 0.5f, 1, 1, 1, 1, 1);
        r5xx_raster_line(&R, &a, &b);
        expect_rect(exp, 4, 3, 6, 3);
        CHECK(compare_maps(exp), "scissored line");
    }
}

static void test_points(void)
{
    uint8_t exp[FB_W * FB_H];
    cur_test = "points";
    clear_all();
    setup_defaults(true);
    if (!do_setup()) return;
    {
        R5xxVertex v = vtx(10.5f, 10.5f, 0.5f, 1, 1, 1, 1, 1);
        r5xx_raster_point(&R, &v);
        expect_rect(exp, 10, 10, 10, 10);
        CHECK(compare_maps(exp), "1x1 point");
        /* size 1 at an integer position: center (10,10), half 0.5 covers
         * centers in [9.5, 10.5) -> pixel 9 only?  9.5 is pixel 9's center */
        clear_all();
        v.x = 10.0f; v.y = 10.0f;
        r5xx_raster_point(&R, &v);
        expect_rect(exp, 9, 9, 9, 9);
        CHECK(compare_maps(exp), "1x1 point on pixel corner");
    }
    /* size 2 at corner: 2x2 */
    clear_all();
    R.regs.ga_point_size = (12u << 16) | 12u;
    if (!do_setup()) return;
    {
        R5xxVertex v = vtx(10.0f, 10.0f, 0.5f, 1, 1, 1, 1, 1);
        r5xx_raster_point(&R, &v);
        expect_rect(exp, 9, 9, 10, 10);
        CHECK(compare_maps(exp), "2x2 point");
    }
    /* size 3 at center: 3x3; non-square 5x3 */
    clear_all();
    R.regs.ga_point_size = (18u << 16) | 18u;
    if (!do_setup()) return;
    {
        R5xxVertex v = vtx(10.5f, 10.5f, 0.5f, 1, 1, 1, 1, 1);
        r5xx_raster_point(&R, &v);
        expect_rect(exp, 9, 9, 11, 11);
        CHECK(compare_maps(exp), "3x3 point");
    }
    clear_all();
    R.regs.ga_point_size = (30u << 16) | 18u;
    if (!do_setup()) return;
    {
        R5xxVertex v = vtx(10.5f, 10.5f, 0.5f, 1, 1, 1, 1, 1);
        r5xx_raster_point(&R, &v);
        expect_rect(exp, 8, 9, 12, 11);
        CHECK(compare_maps(exp), "5x3 point");
    }
    /* per-vertex size clamped by GA_POINT_MINMAX (min 1, max 4) */
    clear_all();
    R.regs.ga_point_minmax = (24u << 16) | 6u;
    if (!do_setup()) return;
    {
        R5xxVertex v = vtx(10.0f, 10.0f, 0.5f, 1, 1, 1, 1, 1);
        r5xx_raster_point_sized(&R, &v, 8.0f);
        expect_rect(exp, 8, 8, 11, 11);
        CHECK(compare_maps(exp), "clamped vertex point size");
        clear_all();
        r5xx_raster_point_sized(&R, &v, 0.0f);
        expect_rect(exp, 9, 9, 9, 9);
        CHECK(compare_maps(exp), "min vertex point size");
        clear_all();
        r5xx_raster_point_sized(&R, &v, 2.0f);
        expect_rect(exp, 9, 9, 10, 10);
        CHECK(compare_maps(exp), "vertex point size 2");
    }
    /* point partially off screen / scissored */
    clear_all();
    R.regs.ga_point_size = (18u << 16) | 18u;
    if (!do_setup()) return;
    {
        R5xxVertex v = vtx(0.5f, 0.5f, 0.5f, 1, 1, 1, 1, 1);
        r5xx_raster_point(&R, &v);
        expect_rect(exp, 0, 0, 1, 1);
        CHECK(compare_maps(exp), "clipped point");
    }
}

static void test_interpolation(void)
{
    cur_test = "interpolation";
    clear_all();
    setup_defaults(true);
    if (!do_setup()) return;
    {
        /* perspective: v2 has w = 4; attribute red 0,0,1 */
        R5xxVertex a = vtx(0, 0, 0.2f, 1, 0, 0, 0, 1);
        R5xxVertex b = vtx(40, 0, 0.4f, 1, 0, 0, 0, 1);
        R5xxVertex c = vtx(0, 40, 0.8f, 4, 1, 0, 0, 1);
        double px = 10.5, py = 10.5;
        double l0 = 1 - px / 40 - py / 40, l1 = px / 40, l2 = py / 40;
        double num = l0 * 0 / 1 + l1 * 0 / 1 + l2 * 1 / 4.0;
        double den = l0 / 1 + l1 / 1 + l2 / 4.0;
        double red = num / den, z = l0 * 0.2 + l1 * 0.4 + l2 * 0.8;
        int got;
        R.regs.zb_cntl = ZB_Z_ENABLE | ZB_Z_WRITE_ENABLE;
        R.regs.zb_zstencilcntl = ZSC(ZS_ALWAYS, 0, 0, 0, 0);
        if (!do_setup()) return;
        tri(&a, &b, &c);
        got = (cb_pixel(10, 10) >> 16) & 0xff;
        CHECK(abs(got - u8((float)red)) <= 1, "perspective red %d want %d", got,
              u8((float)red));
        CHECK(labs((long)zb_z24(10, 10) - (long)(z * 16777215.0 + 0.5)) <= 1,
              "linear z %06x want %06x", zb_z24(10, 10),
              (unsigned)(z * 16777215.0 + 0.5));
        /* flat shading: provoking vertex LAST (default), then FIRST */
        clear_all();
        R.flat_inputs = 1;
        tri(&a, &b, &c);
        CHECK(((cb_pixel(10, 10) >> 16) & 0xff) == 255, "flat last");
        R.regs.ga_color_control = 0;
        if (!do_setup()) return;
        tri(&a, &b, &c);
        CHECK(((cb_pixel(10, 10) >> 16) & 0xff) == 0, "flat first");
        R.regs.ga_color_control = 1u << 16;
        a.attr[0].f[0] = 0.5f;
        b.attr[0].f[0] = 0.25f;
        if (!do_setup()) return;
        tri(&a, &b, &c);
        CHECK(((cb_pixel(10, 10) >> 16) & 0xff) == 64, "flat second %08x",
              cb_pixel(10, 10));
        R.flat_inputs = 0;
    }
    /* the quad input for the stub shader: window coords and 1/w */
    {
        R5xxVertex a = vtx(0, 0, 0.5f, 2, 1, 1, 1, 1);
        R5xxVertex b = vtx(4, 0, 0.5f, 2, 1, 1, 1, 1);
        R5xxVertex c = vtx(0, 4, 0.5f, 2, 1, 1, 1, 1);
        R.us.prog = (const R500UsDecoded *)&R;
        stub_us_calls = 0;
        tri(&a, &b, &c);
        /* pixel centers inside x + y < 4: quads (0,0), (2,0) and (0,2);
         * the (2,2) quad has no covered pixel and is not shaded */
        CHECK(stub_us_calls == 3, "quads run through the shader: %d", stub_us_calls);
        R.us.prog = NULL;
    }
}

static void test_polygon_offset(void)
{
    R5xxVertex a = vtx(0, 0, 0.5f, 1, 1, 1, 1, 1);
    R5xxVertex b = vtx(16, 0, 0.5f, 1, 1, 1, 1, 1);
    R5xxVertex c = vtx(0, 16, 0.5f, 1, 1, 1, 1, 1);
    uint32_t base;
    cur_test = "polygon_offset";
    clear_all();
    setup_defaults(true);
    R.regs.zb_cntl = ZB_Z_ENABLE | ZB_Z_WRITE_ENABLE;
    R.regs.zb_zstencilcntl = ZSC(ZS_ALWAYS, 0, 0, 0, 0);
    if (!do_setup()) return;
    tri(&a, &b, &c);
    base = zb_z24(1, 1);
    CHECK(base == 0x800000, "base depth %06x", base);
    /* constant offset: Mesa programs units*2 for Z24 -> units = 1 -> +1 lsb */
    R.regs.su_poly_offset_enable = 1;      /* front only */
    R.regs.su_poly_offset_front_offset = f2u(2.0f);
    R.regs.su_poly_offset_back_offset = f2u(200.0f);
    if (!do_setup()) return;
    tri(&a, &c, &b);                        /* CCW = front */
    CHECK(zb_z24(1, 1) == base + 1, "front offset %06x", zb_z24(1, 1));
    tri(&a, &b, &c);                        /* back: offset not enabled */
    CHECK(zb_z24(1, 1) == base, "back no offset %06x", zb_z24(1, 1));
    R.regs.su_poly_offset_enable = 2;      /* back only */
    if (!do_setup()) return;
    tri(&a, &b, &c);
    CHECK(zb_z24(1, 1) == base + 100, "back offset %06x", zb_z24(1, 1));
    /* slope: dz/dx = 0.5/16 per pixel, factor 1 (scale = 12) */
    {
        R5xxVertex d = vtx(0, 0, 0.0f, 1, 1, 1, 1, 1);
        R5xxVertex e = vtx(16, 0, 0.5f, 1, 1, 1, 1, 1);
        R5xxVertex f = vtx(0, 16, 0.0f, 1, 1, 1, 1, 1);
        double z = 0.5 * 1.5 / 16.0 + 0.5 / 16.0;
        R.regs.su_poly_offset_enable = 1;
        R.regs.su_poly_offset_front_scale = f2u(12.0f);
        R.regs.su_poly_offset_front_offset = 0;
        if (!do_setup()) return;
        tri(&d, &f, &e);
        CHECK(labs((long)zb_z24(1, 1) - (long)(z * 16777215.0 + 0.5)) <= 1,
              "slope offset %06x want %06x", zb_z24(1, 1),
              (unsigned)(z * 16777215.0 + 0.5));
    }
    /* Z16: units*4 */
    clear_all();
    setup_defaults(true);
    R.regs.zb_format = 0;
    R.regs.zb_cntl = ZB_Z_ENABLE | ZB_Z_WRITE_ENABLE;
    R.regs.zb_zstencilcntl = ZSC(ZS_ALWAYS, 0, 0, 0, 0);
    R.regs.su_poly_offset_enable = 1;
    R.regs.su_poly_offset_front_offset = f2u(4.0f);
    if (!do_setup()) return;
    tri(&a, &c, &b);
    CHECK(zb_z16(1, 1) == 0x8001, "z16 offset %04x", zb_z16(1, 1));
}

static void test_shader_hooks(void)
{
    R5xxVertex a = vtx(0, 0, 0.1f, 1, 1, 1, 1, 1);
    R5xxVertex b = vtx(16, 0, 0.1f, 1, 1, 1, 1, 1);
    R5xxVertex c = vtx(0, 16, 0.1f, 1, 1, 1, 1, 1);
    cur_test = "shader_hooks";
    clear_all();
    setup_defaults(true);
    R.us.prog = (const R500UsDecoded *)&R;
    R.regs.zb_cntl = ZB_Z_ENABLE | ZB_Z_WRITE_ENABLE | ZB_STENCIL_ENABLE;
    R.regs.zb_zstencilcntl = ZSC(ZS_LESS, ZS_ALWAYS, ZS_KEEP, ZS_INCR, ZS_KEEP);
    R.regs.zb_stencilrefmask = SREFMASK(0, 0xff, 0xff);
    fill_zb(0x80000000u);                  /* z = 0.5 */
    if (!do_setup()) return;
    /* kill: input1.x > 0.5 -> nothing, not even stencil/depth */
    stub_us_mode = STUB_US_KILL;
    a.attr[1].f[0] = b.attr[1].f[0] = c.attr[1].f[0] = 1.0f;
    tri(&a, &b, &c);
    CHECK(cb_pixel(1, 1) == 0 && zb_raw(1, 1) == 0x80000000u && R.zpass_count == 0,
          "killed pixel wrote something");
    CHECK(R.pixels_in > 0 && R.pixels_out == 0, "kill stats");
    a.attr[1].f[0] = b.attr[1].f[0] = c.attr[1].f[0] = 0.0f;
    tri(&a, &b, &c);
    CHECK(cb_pixel(1, 1) != 0 && zb_s8(1, 1) == 1, "not killed");
    /* shader depth: 0.75 fails LESS against 0.5; 0.25 passes */
    clear_all();
    fill_zb(0x80000000u);
    stub_us_mode = STUB_US_DEPTH;
    a.attr[1].f[1] = b.attr[1].f[1] = c.attr[1].f[1] = 0.75f;
    tri(&a, &b, &c);
    CHECK(cb_pixel(1, 1) == 0 && zb_z24(1, 1) == 0x800000, "shader depth fail");
    a.attr[1].f[1] = b.attr[1].f[1] = c.attr[1].f[1] = 0.25f;
    tri(&a, &b, &c);
    CHECK(cb_pixel(1, 1) != 0 && zb_z24(1, 1) == 0x400000, "shader depth %06x",
          zb_z24(1, 1));
    stub_us_mode = STUB_US_PASSTHROUGH;
    R.us.prog = NULL;
    /* multiwrite: color 0 replicated to two buffers */
    clear_all();
    setup_defaults(true);
    R.regs.rb3d_cctl = CCTL_MULTIWRITES(2);
    R.regs.rb3d_colorpitch[1] = FB_W | COLOR_FORMAT_ARGB8888;
    R.regs.rb3d_coloroffset[1] = GPU_BASE + CB1_OFF;
    if (!do_setup()) return;
    CHECK(R.rt.num_cbufs == 2, "multiwrite cbufs %d", R.rt.num_cbufs);
    draw_pixel(2, 3, 1, 0, 0, 1);
    CHECK(cb_pixel(2, 3) == 0xffff0000u && cb1_pixel(2, 3) == 0xffff0000u,
          "multiwrite %08x %08x", cb_pixel(2, 3), cb1_pixel(2, 3));
    /* US_OUT_FMT swizzle: RGBA order into a BGRA buffer swaps R and B */
    clear_all();
    setup_defaults(true);
    R.regs.us_out_fmt[0] = OUT_FMT_RGBA;
    if (!do_setup()) return;
    draw_pixel(2, 3, 1, 0.5f, 0, 1);
    CHECK(cb_pixel(2, 3) == 0xff0080ffu, "swizzled write %08x", cb_pixel(2, 3));
    /* UNUSED on buffer 0: nothing bound */
    R.regs.us_out_fmt[0] = OUT_FMT_UNUSED;
    if (!do_setup()) return;
    CHECK(R.rt.num_cbufs == 0, "unused cbuf");
    draw_pixel(4, 4, 1, 1, 1, 1);
    CHECK(cb_pixel(4, 4) == 0, "wrote unused cbuf");
}

static void test_polygon_mode(void)
{
    R5xxVertex a = vtx(2.5f, 2.5f, 0.5f, 1, 1, 1, 1, 1);
    R5xxVertex b = vtx(18.5f, 2.5f, 0.5f, 1, 1, 1, 1, 1);
    R5xxVertex c = vtx(2.5f, 18.5f, 0.5f, 1, 1, 1, 1, 1);
    uint8_t exp[FB_W * FB_H];
    int i;
    cur_test = "polygon_mode";
    clear_all();
    setup_defaults(true);
    R.regs.ga_poly_mode = POLY_MODE_DUAL | POLY_FRONT_LINE | POLY_BACK_LINE;
    if (!do_setup()) return;
    tri(&a, &b, &c);
    CHECK(cb_pixel(6, 6) == 0 && cb_pixel(5, 2) != 0 && cb_pixel(2, 5) != 0,
          "line mode");
    /* hypotenuse pixels */
    CHECK(cb_pixel(10, 10) != 0 || cb_pixel(10, 11) != 0 || cb_pixel(11, 10) != 0,
          "line mode hypotenuse");
    /* front tri, back point: CW is back here */
    clear_all();
    R.regs.ga_poly_mode = POLY_MODE_DUAL | POLY_FRONT_TRI | POLY_BACK_POINT;
    if (!do_setup()) return;
    tri(&a, &b, &c);
    memset(exp, 0, sizeof(exp));
    exp[2 * FB_W + 2] = exp[2 * FB_W + 18] = exp[18 * FB_W + 2] = 1;
    CHECK(compare_maps(exp), "point mode (back)");
    clear_all();
    tri(&a, &c, &b);
    CHECK(cb_pixel(6, 6) != 0, "fill mode (front)");
    /* DUAL bit clear: types ignored */
    clear_all();
    R.regs.ga_poly_mode = POLY_FRONT_POINT | POLY_BACK_POINT;
    if (!do_setup()) return;
    tri(&a, &b, &c);
    CHECK(cb_pixel(6, 6) != 0, "poly mode disabled");
    for (i = 0; i < 1; i++) { }
}

static void test_fog(void)
{
    R5xxVertex a = vtx(0, 0, 0.25f, 1, 1, 1, 1, 1);
    R5xxVertex b = vtx(16, 0, 0.25f, 1, 1, 1, 1, 1);
    R5xxVertex c = vtx(0, 16, 0.25f, 1, 1, 1, 1, 1);
    cur_test = "fog";
    clear_all();
    setup_defaults(true);
    R.regs.fg_fog_blend = FOG_ENABLE | FOG_LINEAR;
    R.regs.ga_fog_scale = f2u(1.0f);
    R.regs.ga_fog_offset = f2u(0.0f);
    R.regs.gb_select = 5;                     /* fog coordinate = Z */
    R.regs.fg_fog_color_r = 0;
    R.regs.fg_fog_color_g = 1023;
    R.regs.fg_fog_color_b = 0;
    if (!do_setup()) return;
    tri(&a, &b, &c);
    /* f = 1 - z = 0.75: color = 0.75*white + 0.25*green */
    CHECK(cb_pixel(1, 1) == 0xffbfffbfu, "linear fog %08x", cb_pixel(1, 1));
    /* fog from color 1 alpha (GB_SELECT = 1), EXP with density 1 */
    clear_all();
    R.regs.gb_select = 1;
    R.regs.fg_fog_blend = FOG_ENABLE | FOG_EXP;
    a.attr[1].f[3] = b.attr[1].f[3] = c.attr[1].f[3] = 2.0f;
    if (!do_setup()) return;
    tri(&a, &b, &c);
    {
        int e = u8(expf(-2.0f));
        uint32_t want = 0xff000000u | (e << 16) | 0xff00 | e;
        CHECK(cb_pixel(1, 1) == want, "exp fog %08x want %08x", cb_pixel(1, 1), want);
    }
    /* fog disabled by default */
    clear_all();
    R.regs.fg_fog_blend = 0;
    if (!do_setup()) return;
    tri(&a, &b, &c);
    CHECK(cb_pixel(1, 1) == 0xffffffffu, "fog off");
}

/*
 * ZMASK depth clear: the clear fills runs of pixels that are contiguous in
 * memory.  Compare it, byte for byte, with the per-pixel clear that a
 * memory without map() gets, for linear, micro, macro, and micro+macro
 * tiling, Z16 and Z24S8, and pitch and scissor sizes that end inside tiles.
 */
static void test_zmask_clear(void)
{
    static uint8_t ref[VRAM_SIZE];
    static const uint32_t pitches[] = { 64, 96, 128, 200, 256 };
    static const int widths[] = { 64, 37, 200, 1 };
    static const int heights[] = { 64, 13, 3 };
    R5xxMem saved = fm.mem;
    unsigned fmt, macro, micro, pi, wi, hi;
    int configs = 0;

    cur_test = "zmask_clear";
    for (fmt = 0; fmt <= 2; fmt += 2) {
        for (macro = 0; macro < 2; macro++) {
            for (micro = 0; micro < 3; micro++) {
                for (pi = 0; pi < sizeof(pitches) / sizeof(pitches[0]); pi++) {
                    for (wi = 0; wi < sizeof(widths) / sizeof(widths[0]); wi++) {
                        for (hi = 0; hi < sizeof(heights) / sizeof(heights[0]); hi++) {
                            uint32_t pitch = pitches[pi];
                            int w = widths[wi], h = heights[hi];
                            int off = 0;
                            size_t span = (size_t)pitch * 256 * 4;
                            setup_defaults(true);
                            R.regs.zb_format = fmt;
                            R.regs.zb_depthpitch = pitch | (macro << 16) | (micro << 17);
                            R.regs.zb_depthoffset = GPU_BASE + ZB_OFF;
                            R.regs.sc_scissors_tl = (uint32_t)off | ((uint32_t)off << 13);
                            R.regs.sc_scissors_br = (uint32_t)(w - 1 + off) |
                                                    ((uint32_t)(h - 1 + off) << 13);
                            if (ZB_OFF + span > VRAM_SIZE) {
                                span = VRAM_SIZE - ZB_OFF;
                            }
                            fm.mem = saved;
                            fm.mem.map = NULL;
                            memset(vram, 0x5a, sizeof(vram));
                            r5xx_raster_zmask_clear(&R, 1000, 0x11223344u);
                            memcpy(ref, vram, sizeof(vram));
                            fm.mem = saved;
                            memset(vram, 0x5a, sizeof(vram));
                            r5xx_raster_zmask_clear(&R, 1000, 0x11223344u);
                            CHECK(memcmp(ref, vram, sizeof(vram)) == 0,
                                  "fmt %u macro %u micro %u pitch %u %dx%d differs",
                                  fmt, macro, micro, pitch, w, h);
                            configs++;
                        }
                    }
                }
            }
        }
    }
    fm.mem = saved;
    CHECK(configs == 2 * 2 * 3 * 5 * 4 * 3, "configs %d", configs);
}

static void test_mem_fallback(void)
{
    /* No map(): everything goes through read()/write(). */
    R5xxMem saved = fm.mem;
    cur_test = "mem_fallback";
    clear_all();
    fm.mem.map = NULL;
    setup_defaults(true);
    R.regs.rb3d_blendcntl = BLEND_ENABLE | BLEND_FCN(FCN_ADD_CLAMP) |
                            BLEND_SRC(BF_ONE) | BLEND_DST(BF_ONE);
    R.regs.zb_cntl = ZB_Z_ENABLE | ZB_Z_WRITE_ENABLE;
    R.regs.zb_zstencilcntl = ZSC(ZS_ALWAYS, 0, 0, 0, 0);
    if (!do_setup()) { fm.mem = saved; return; }
    fill_cb(0x00100000u);
    draw_pixel(3, 3, 0.5f, 0, 0, 1);
    CHECK(cb_pixel(3, 3) == 0xff900000u, "read/write fallback %08x", cb_pixel(3, 3));
    CHECK(zb_z24(3, 3) == 0x800000, "zbuf fallback");
    fm.mem = saved;
}

int main(void)
{
    r5xx_flatmem_init(&fm, vram, VRAM_SIZE, GPU_BASE);
    fm.log_level = 1;
    srand(42);

    test_setup_decode();
    test_triangle_coverage();
    test_shared_edges();
    test_scissor();
    test_cull();
    test_depth();
    test_stencil();
    test_blend();
    test_fp16_target();
    test_color_mask();
    test_alpha_test();
    test_logic_op();
    test_tiled();
    test_clear();
    test_lines();
    test_points();
    test_interpolation();
    test_polygon_offset();
    test_shader_hooks();
    test_polygon_mode();
    test_fog();
    test_mem_fallback();
    test_zmask_clear();

    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
