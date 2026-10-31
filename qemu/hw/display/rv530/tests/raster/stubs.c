/*
 * TEST STUBS for the rasterizer unit tests.
 *
 * r5xx_tex.c and r500_us.c are written by other modules; the raster tests
 * must not depend on them, so this file provides minimal stand-ins for
 * the functions r5xx_raster.c calls from r5xx_tex.h and r500_us.h.
 * Nothing here is production code.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "r5xx_tex.h"
#include "r500_us.h"
#include "stubs.h"
#include <string.h>

/* ---- surface layout stub ----------------------------------------- */

/* LINEAR is exact.  Any tiled mode uses a toy 2x2 tile swizzle so the
 * tests can verify the rasterizer routes every access through this
 * function; it has nothing to do with the real R300 tiling. */
uint32_t r5xx_surface_pixel_offset(const R5xxSurfaceLayout *l,
                                   uint32_t x, uint32_t y)
{
    if (l->tiling == R5XX_TILE_LINEAR) {
        return y * l->pitch_bytes + x * l->bpp;
    }
    return (y >> 1) * (l->pitch_bytes * 2) + (x >> 1) * (4 * l->bpp) +
           ((y & 1) * 2 + (x & 1)) * l->bpp;
}

/* ---- color formats stub ------------------------------------------ */

uint32_t r5xx_color_format_from_hw(uint32_t colorpitch, bool is_r500,
                                   R5xxColorFormat *fmt)
{
    switch ((colorpitch >> 21) & 15) {
    case 6:
        *fmt = R5XX_COLOR_ARGB8888;
        return 4;
    case 4:
        *fmt = R5XX_COLOR_RGB565;
        return 2;
    case 10:
        *fmt = R5XX_COLOR_ARGB16161616;
        return 8;
    default:
        return 0;
    }
}

static uint32_t rd32(const uint8_t *p)
{
    return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void wr32(uint8_t *p, uint32_t v)
{
    p[0] = v & 0xff;
    p[1] = (v >> 8) & 0xff;
    p[2] = (v >> 16) & 0xff;
    p[3] = v >> 24;
}

static uint32_t unorm(float v, uint32_t max)
{
    v = r5xx_clampf(v, 0.0f, 1.0f);
    return (uint32_t)(v * max + 0.5f);
}

void r5xx_color_unpack(R5xxColorFormat fmt, const uint8_t *p, r5xx_vec4 *out)
{
    switch (fmt) {
    case R5XX_COLOR_ARGB8888:
        r5xx_unpack_argb8888(rd32(p), out);
        break;
    case R5XX_COLOR_RGB565: {
        uint32_t v = p[0] | (p[1] << 8);
        out->f[0] = ((v >> 11) & 31) / 31.0f;
        out->f[1] = ((v >> 5) & 63) / 63.0f;
        out->f[2] = (v & 31) / 31.0f;
        out->f[3] = 1.0f;
        break;
    }
    /* wide formats: B, G, R, A from the lowest address, like r5xx_tex.c */
    case R5XX_COLOR_ARGB16161616F: {
        static const int ch[4] = { 2, 1, 0, 3 };
        int i;
        for (i = 0; i < 4; i++) {
            out->f[ch[i]] = r5xx_half_to_float(p[2 * i] | (p[2 * i + 1] << 8));
        }
        break;
    }
    case R5XX_COLOR_ARGB16161616: {
        static const int ch[4] = { 2, 1, 0, 3 };
        int i;
        for (i = 0; i < 4; i++) {
            out->f[ch[i]] = (p[2 * i] | (p[2 * i + 1] << 8)) / 65535.0f;
        }
        break;
    }
    default:
        *out = r5xx_vec4_make(0, 0, 0, 1);
        break;
    }
}

void r5xx_color_pack(R5xxColorFormat fmt, const r5xx_vec4 *c, uint8_t *p)
{
    switch (fmt) {
    case R5XX_COLOR_ARGB8888:
        wr32(p, r5xx_pack_argb8888(c));
        break;
    case R5XX_COLOR_RGB565: {
        uint32_t v = (unorm(c->f[0], 31) << 11) | (unorm(c->f[1], 63) << 5) |
                     unorm(c->f[2], 31);
        p[0] = v & 0xff;
        p[1] = v >> 8;
        break;
    }
    case R5XX_COLOR_ARGB16161616F: {
        static const int ch[4] = { 2, 1, 0, 3 };
        int i;
        for (i = 0; i < 4; i++) {
            uint16_t h = r5xx_float_to_half(c->f[ch[i]]);
            p[2 * i] = h & 0xff;
            p[2 * i + 1] = h >> 8;
        }
        break;
    }
    case R5XX_COLOR_ARGB16161616: {
        static const int ch[4] = { 2, 1, 0, 3 };
        int i;
        for (i = 0; i < 4; i++) {
            uint32_t v = unorm(c->f[ch[i]], 65535);
            p[2 * i] = v & 0xff;
            p[2 * i + 1] = v >> 8;
        }
        break;
    }
    default:
        memset(p, 0, 4);
        break;
    }
}

/* ---- fragment shader stub ---------------------------------------- */

int stub_us_mode = STUB_US_PASSTHROUGH;
int stub_us_calls;

/* The fake program: color = interpolated input 0 (all targets).  Mode
 * KILL discards pixels whose input 1 .x > 0.5; mode DEPTH writes input
 * 1 .y as the fragment depth. */
void r500_us_run_quad(const R500UsExec *e, const R500UsQuadIn *in,
                      R500UsQuadOut *out)
{
    int p, n;
    (void)e;
    stub_us_calls++;
    memset(out, 0, sizeof(*out));
    for (p = 0; p < 4; p++) {
        for (n = 0; n < R500_US_MAX_OUTPUTS; n++) {
            out->color[p][n] = in->inputs[p][0];
        }
        if (stub_us_mode == STUB_US_KILL && in->inputs[p][1].f[0] > 0.5f) {
            out->killed |= 1u << p;
        }
        if (stub_us_mode == STUB_US_DEPTH) {
            out->depth[p] = in->inputs[p][1].f[1];
            out->depth_written = true;
        }
        if (stub_us_mode == STUB_US_MRT) {
            /* target 1 gets input 1 so the MRT swizzle path is visible */
            out->color[p][1] = in->inputs[p][1];
        }
    }
}
