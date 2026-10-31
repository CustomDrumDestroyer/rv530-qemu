/*
 * Differential test: the same draws through the CPU reference path
 * (r5xx_raster + r500_us + r5xx_tex) and the Vulkan backend (r5xx_vk),
 * starting from identical VRAM, then compare the render targets.
 *
 * Scenarios use register values and fragment programs captured from Snow
 * Leopard's WindowServer where possible.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "r5xx_vk.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define VRAM_SIZE (64u << 20)
#define RT_OFF    0x00100000u
#define Z_OFF     0x00400000u
#define TEX_OFF   0x00800000u
#define TEX1_OFF  0x00c00000u
#define RT_W      256
#define RT_H      256

static uint8_t *vram, *backup, *cpu_out;
static R5xxFlatMem fm;
static R5xxVk *vk;
static R5xxRaster R;
static R500UsProgram prog;
static R500UsDecoded *dec;
static R5xxTexSampler samp[16];
static uint32_t tex_used;
static r5xx_vec4 consts[256];
static int failures;
static const RvkMem vram_only = { .fb_start = 0, .vram_size = VRAM_SIZE };

/* ---- helpers ----------------------------------------------------- */

static uint32_t rng = 12345;
static uint32_t rnd(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return rng;
}

static void fill_random(uint32_t off, uint32_t len)
{
    uint32_t i;
    for (i = 0; i < len; i += 4) {
        uint32_t v = rnd();
        memcpy(vram + off + i, &v, 4);
    }
}

static void fill_pattern_argb(uint32_t off, uint32_t len)
{
    /* smooth-ish content so that bilinear filtering differences are small */
    uint32_t i;
    for (i = 0; i < len; i += 4) {
        uint32_t p = i / 4, x = p % 97, y = p / 97;
        uint32_t v = ((x * 5) & 0xff) | (((y * 3) & 0xff) << 8) |
                     ((((x + y) * 2) & 0xff) << 16) | ((200 + (x & 31)) << 24);
        memcpy(vram + off + i, &v, 4);
    }
}

static void tex_cb(void *opaque, const R5xxTexRequest *req, r5xx_vec4 out[4])
{
    if (req->unit < 0 || req->unit >= 16 || !((tex_used >> req->unit) & 1)) {
        int i;
        for (i = 0; i < 4; i++) {
            out[i] = r5xx_vec4_make(0, 0, 0, 1);
        }
        return;
    }
    r5xx_tex_sample_quad(&samp[req->unit], &fm.mem, req, out);
}

static void load_prog(const uint32_t *words, int ninst)
{
    char err[128];
    memset(&prog, 0, sizeof(prog));
    memcpy(prog.code, words, (size_t)ninst * 6 * 4);
    prog.code_addr = 0 | ((uint32_t)(ninst - 1) << 16);
    prog.code_range = (uint32_t)(ninst - 1) << 16;
    if (dec) {
        r500_us_free(dec);
    }
    dec = r500_us_decode(&prog, err, sizeof(err));
    if (!dec) {
        printf("decode failed: %s\n", err);
        exit(1);
    }
}

static void set_tex(int unit, uint32_t offset, uint32_t f0, uint32_t f1, uint32_t f2,
                    uint32_t filt0, uint32_t filt1)
{
    R5xxTexRegs tr = {
        .filter0 = filt0, .filter1 = filt1, .format0 = f0, .format1 = f1,
        .format2 = f2, .offset = offset,
    };
    char err[128];
    if (!r5xx_tex_decode(&tr, true, &samp[unit], err, sizeof(err))) {
        printf("tex decode: %s\n", err);
    }
    tex_used |= 1u << unit;
}

static void raster_defaults(void)
{
    R5xxRasterRegs *g = &R.regs;

    memset(&R, 0, sizeof(R));
    R.mem = &fm.mem;
    R.num_fs_inputs = 2;
    g->is_r500 = true;
    g->rb3d_colorpitch[0] = RT_W | (6u << 21) | (1u << 16) | (1u << 17); /* ARGB8888, macro+micro */
    g->rb3d_coloroffset[0] = RT_OFF;
    g->rb3d_color_channel_mask = 0xf;
    g->us_out_fmt[0] = (3u << 8) | (2u << 10) | (1u << 12) | (0u << 14);  /* BGRA */
    g->us_out_fmt[1] = g->us_out_fmt[2] = g->us_out_fmt[3] = 15;
    g->sc_scissors_tl = 0;
    g->sc_scissors_br = (RT_W - 1) | ((uint32_t)(RT_H - 1) << 13);
    g->sc_clip_rule = 0xffff;
    g->su_depth_scale = 0x4B7FFFFF;
    g->ga_point_size = (6u << 16) | 6u;
    g->ga_point_minmax = (6u << 16) | 6u;
    g->ga_line_cntl = 6u | (2u << 16);
    g->ga_color_control = 3u << 16;
    R.us.prog = NULL;
    R.us.consts = consts;
    R.us.tex = tex_cb;
}

static R5xxVertex vtx(float x, float y, float z, float w, r5xx_vec4 a0, r5xx_vec4 a1)
{
    R5xxVertex v;
    memset(&v, 0, sizeof(v));
    v.x = x; v.y = y; v.z = z; v.w = w;
    v.attr[0] = a0;
    v.attr[1] = a1;
    return v;
}

#define MAXV 4096
static R5xxVertex tris[MAXV];
static int ntris;

static void add_tri(R5xxVertex a, R5xxVertex b, R5xxVertex c)
{
    tris[ntris * 3] = a;
    tris[ntris * 3 + 1] = b;
    tris[ntris * 3 + 2] = c;
    ntris++;
}

static void add_quad(float x0, float y0, float x1, float y1, float z,
                     r5xx_vec4 col, float s0, float t0, float s1, float t1)
{
    R5xxVertex a = vtx(x0, y0, z, 1, col, r5xx_vec4_make(s0, t0, 0, 1));
    R5xxVertex b = vtx(x1, y0, z, 1, col, r5xx_vec4_make(s1, t0, 0, 1));
    R5xxVertex c = vtx(x1, y1, z, 1, col, r5xx_vec4_make(s1, t1, 0, 1));
    R5xxVertex d = vtx(x0, y1, z, 1, col, r5xx_vec4_make(s0, t1, 0, 1));
    add_tri(a, b, c);
    add_tri(a, c, d);
}

/* Run the queued triangles through both paths and compare 'len' bytes of
 * the surface at 'off'.  tol: max per-byte difference; max_bad: allowed
 * differing bytes. */
static void run_and_compare(const char *name, uint32_t off, uint32_t len,
                            int tol, uint32_t max_bad)
{
    char err[256];
    static RvkVertex gv[MAXV * 2];
    RvkDraw *draw = calloc(1, sizeof(RvkDraw));
    R5xxVkDrawReq req;
    uint32_t i, bad = 0, maxd = 0, first = UINT32_MAX;
    int n = 0, t, k;
    struct timespec ts0, ts1, ts2;
    double cpu_ms, gpu_ms;

    R.us.prog = dec;
    if (!r5xx_raster_setup(&R, err, sizeof(err))) {
        printf("FAIL [%s] raster setup: %s\n", name, err);
        failures++;
        return;
    }
    memcpy(backup, vram, VRAM_SIZE);

    clock_gettime(CLOCK_MONOTONIC, &ts0);
    for (t = 0; t < ntris; t++) {
        r5xx_raster_triangle(&R, &tris[t * 3], &tris[t * 3 + 1], &tris[t * 3 + 2]);
    }
    clock_gettime(CLOCK_MONOTONIC, &ts1);
    memcpy(cpu_out, vram, VRAM_SIZE);
    memcpy(vram, backup, VRAM_SIZE);

    if (!rvk_build_draw(draw, &R, samp, tex_used, consts, &vram_only, err, sizeof(err))) {
        printf("FAIL [%s] build: %s\n", name, err);
        failures++;
        free(draw);
        return;
    }
    for (t = 0; t < ntris; t++) {
        k = rvk_triangle(&gv[n], &R, &tris[t * 3], &tris[t * 3 + 1], &tris[t * 3 + 2], false);
        if (k < 0) {
            printf("FAIL [%s] triangle needs CPU path\n", name);
            failures++;
            free(draw);
            return;
        }
        n += k;
    }
    memset(&req, 0, sizeof(req));
    req.draw = draw;
    req.verts = gv;
    req.nverts = (uint32_t)n;
    req.prog = dec;
    req.prog_hash = r500_us_hash(dec);
    req.sc_x0 = R.d.sc_x0; req.sc_y0 = R.d.sc_y0;
    req.sc_x1 = R.d.sc_x1; req.sc_y1 = R.d.sc_y1;
    rvk_dirty_ranges(&req, &R, &vram_only, req.sc_y0, req.sc_y1);
    r5xx_vk_upload(vk, 0, VRAM_SIZE);
    if (getenv("RVK_ASYNC_UPLOAD")) {
        r5xx_vk_upload_kick(vk);        /* copy runs before the draw's batch */
    }
    if (!r5xx_vk_draw(vk, &req, err, sizeof(err))) {
        printf("FAIL [%s] vk draw: %s\n", name, err);
        failures++;
        free(draw);
        return;
    }
    r5xx_vk_flush(vk);
    clock_gettime(CLOCK_MONOTONIC, &ts2);
    cpu_ms = (ts1.tv_sec - ts0.tv_sec) * 1e3 + (ts1.tv_nsec - ts0.tv_nsec) / 1e6;
    gpu_ms = (ts2.tv_sec - ts1.tv_sec) * 1e3 + (ts2.tv_nsec - ts1.tv_nsec) / 1e6;

    for (i = 0; i < len; i++) {
        int dlt = abs((int)vram[off + i] - (int)cpu_out[off + i]);
        if (dlt > tol) {
            bad++;
            if (first == UINT32_MAX) {
                first = i;
            }
        }
        if ((uint32_t)dlt > maxd) {
            maxd = (uint32_t)dlt;
        }
    }
    {
        uint32_t changed = 0;
        for (i = 0; i < len; i++) {
            changed += backup[off + i] != cpu_out[off + i];
        }
        printf("%s [%s] %u/%u bytes differ (tol %d, max diff %u), %u bytes changed by the draw; "
               "CPU %.2f ms, GPU %.2f ms\n", bad > max_bad ? "FAIL" : "ok  ", name,
               bad, len, tol, maxd, changed, cpu_ms, gpu_ms);
        if (bad > max_bad) {
            failures++;
            printf("     first at byte %u: cpu %02x gpu %02x (orig %02x)\n", first,
                   cpu_out[off + first], vram[off + first], backup[off + first]);
        }
        if (!changed) {
            printf("FAIL [%s] the draw changed nothing\n", name);
            failures++;
        }
    }
    free(draw);
}

/* ---- programs captured from WindowServer -------------------------- */

/* r1 = TXP(t1) unscaled; out = r0 * r1 */
static const uint32_t prog_composite[] = {
    0x00007807, 0x0ec00000, 0xe401e401, 0x00000000, 0, 0,
    0x00078105, 0x10000400, 0x10000400, 0x00442220, 0x0068c000, 0x20490000,
};
/* out = r0 * 1 */
static const uint32_t prog_color[] = {
    0x00078105, 0x10040000, 0x10040000, 0x00db0220, 0x00c0c000, 0x20490000,
};
/* r0 = TEX(t0) scaled; out = r0 * 1 */
static const uint32_t prog_tex[] = {
    0x00007803, 0x02400000, 0xe400e400, 0x00000000, 0, 0,
    0x00078105, 0x00000000, 0x00000000, 0x00db0220, 0x00c0c000, 0x20490000,
};

static void scenario_composite(void)
{
    raster_defaults();
    tex_used = 0;
    fill_random(RT_OFF, RT_W * RT_H * 4);
    fill_pattern_argb(TEX_OFF, 1 << 20);
    /* WindowServer window surface: 227x245 ARGB8888 macro+micro, pitch 256 */
    set_tex(0, TEX_OFF | 0xc, 0x8007a0e2, 0x0000a60c, 0x000000ff, 0x00000a92, 0);
    load_prog(prog_composite, 2);
    /* premultiplied over: ONE, ONE_MINUS_SRC_ALPHA */
    R.regs.rb3d_blendcntl = 0x27210007;
    R.regs.rb3d_ablendcntl = 0x27210000;
    ntris = 0;
    add_quad(10, 5, 237, 250, 0.5f, r5xx_vec4_make(1, 1, 1, 1), 0, 0, 227, 245);
    run_and_compare("composite tiled+blend", RT_OFF, RT_W * RT_H * 4, 1, 0);

    /* with a 0.45 alpha color (Dashboard dimming style, still textured) */
    fill_random(RT_OFF, RT_W * RT_H * 4);
    ntris = 0;
    add_quad(-20.3f, 30.7f, 200.2f, 300.1f, 0.5f, r5xx_vec4_make(0.45f, 0.45f, 0.45f, 0.45f),
             3.5f, 1.25f, 180.0f, 240.0f);
    run_and_compare("composite partial alpha, fractional", RT_OFF, RT_W * RT_H * 4, 1, 64);
}

static void scenario_bilinear(void)
{
    raster_defaults();
    tex_used = 0;
    fill_random(RT_OFF, RT_W * RT_H * 4);
    fill_pattern_argb(TEX_OFF, 1 << 20);
    /* 64x64 linear ARGB8888, bilinear, repeat */
    set_tex(0, TEX_OFF, (63u) | (63u << 11), 0x0000a60c, 0, (2u << 9) | (2u << 11), 0);
    load_prog(prog_tex, 2);
    ntris = 0;
    add_quad(0, 0, 256, 256, 0.5f, r5xx_vec4_make(1, 1, 1, 1), -0.3f, 0.2f, 2.7f, 3.4f);
    run_and_compare("bilinear repeat, magnified", RT_OFF, RT_W * RT_H * 4, 2, 600);
}

/* the same 64x64 texture stored byte-reversed per dword, read back through
 * TX_OFFSET endian 8IN32 (Apple client storage, GL_UNSIGNED_INT_8_8_8_8) */
static void scenario_endian(void)
{
    const uint32_t f0 = 63u | (63u << 11), filt = (2u << 9) | (2u << 11);
    r5xx_vec4 want, got;
    uint32_t i;

    raster_defaults();
    tex_used = 0;
    fill_pattern_argb(TEX_OFF, 1 << 20);
    set_tex(0, TEX_OFF, f0, 0x0000a60c, 0, filt, 0);
    r5xx_tex_fetch_texel(&samp[0], &fm.mem, 0, 0, 0, 13, 7, &want);
    for (i = 0; i < 64 * 64 * 4; i += 4) {
        uint8_t *p = vram + TEX_OFF + i, t;
        t = p[0]; p[0] = p[3]; p[3] = t;
        t = p[1]; p[1] = p[2]; p[2] = t;
    }
    set_tex(0, TEX_OFF | 2, f0, 0x0000a60c, 0, filt, 0);
    r5xx_tex_fetch_texel(&samp[0], &fm.mem, 0, 0, 0, 13, 7, &got);
    if (memcmp(&want, &got, sizeof(want))) {
        printf("FAIL [endian 8in32 texel] got %g %g %g %g want %g %g %g %g\n",
               got.f[0], got.f[1], got.f[2], got.f[3],
               want.f[0], want.f[1], want.f[2], want.f[3]);
        failures++;
    } else {
        printf("ok   [endian 8in32 texel] matches the unswapped texture\n");
    }
    fill_random(RT_OFF, RT_W * RT_H * 4);
    load_prog(prog_tex, 2);
    ntris = 0;
    add_quad(0, 0, 256, 256, 0.5f, r5xx_vec4_make(1, 1, 1, 1), -0.3f, 0.2f, 2.7f, 3.4f);
    run_and_compare("bilinear, endian 8in32", RT_OFF, RT_W * RT_H * 4, 2, 600);
}

static void scenario_depth_stencil(void)
{
    raster_defaults();
    tex_used = 0;
    fill_random(RT_OFF, RT_W * RT_H * 4);
    memset(vram + Z_OFF, 0xff, RT_W * RT_H * 4);
    {
        uint32_t i;
        for (i = 0; i < RT_W * RT_H; i++) {
            vram[Z_OFF + 4 * i] = (uint8_t)(i * 7);     /* stencil bytes */
        }
    }
    R.regs.zb_format = 2;                             /* Z24S8 */
    R.regs.zb_depthpitch = RT_W | (1u << 16);         /* macro tiled */
    R.regs.zb_depthoffset = Z_OFF;
    R.regs.zb_cntl = 1u | 2u | 4u;                    /* stencil, z, z write */
    /* z LESS, stencil ALWAYS, sfail KEEP, zpass INCR_WRAP, zfail DECR */
    R.regs.zb_zstencilcntl = 1 | (7 << 3) | (0 << 6) | (6 << 9) | (4 << 12);
    R.regs.zb_stencilrefmask = 5 | (0xff << 8) | (0xff << 16);
    load_prog(prog_color, 1);
    ntris = 0;
    add_quad(20, 20, 200, 200, 0.6f, r5xx_vec4_make(1, 0, 0, 1), 0, 0, 0, 0);
    add_quad(60, 60, 240, 240, 0.3f, r5xx_vec4_make(0, 1, 0, 1), 0, 0, 0, 0);
    add_quad(100, 10, 180, 250, 0.45f, r5xx_vec4_make(0, 0, 1, 0.5f), 0, 0, 0, 0);
    {
        R5xxVertex a = vtx(5, 250, 0.1f, 2.0f, r5xx_vec4_make(1, 1, 0, 1), r5xx_vec4_make(0, 0, 0, 1));
        R5xxVertex b = vtx(250, 240, 0.9f, 1.0f, r5xx_vec4_make(0, 1, 1, 1), r5xx_vec4_make(0, 0, 0, 1));
        R5xxVertex c = vtx(128, 3, 0.5f, 0.5f, r5xx_vec4_make(1, 0, 1, 1), r5xx_vec4_make(0, 0, 0, 1));
        add_tri(a, b, c);
    }
    run_and_compare("depth/stencil color", RT_OFF, RT_W * RT_H * 4, 1, 400);
    {
        /* compare the depth/stencil buffer of the same run */
        uint32_t i, bad = 0;
        for (i = 0; i < RT_W * RT_H * 4; i++) {
            bad += abs((int)vram[Z_OFF + i] - (int)cpu_out[Z_OFF + i]) > 1;
        }
        printf("%s [depth/stencil zbuf] %u bytes differ\n", bad > 400 ? "FAIL" : "ok  ", bad);
        if (bad > 400) {
            failures++;
        }
    }
}

static void scenario_565_mask(void)
{
    raster_defaults();
    tex_used = 0;
    fill_random(RT_OFF, RT_W * RT_H * 2);
    R.regs.rb3d_colorpitch[0] = RT_W | (4u << 21);    /* RGB565 linear */
    R.regs.rb3d_color_channel_mask = 0x5;             /* B and R only */
    load_prog(prog_color, 1);
    ntris = 0;
    {
        R5xxVertex a = vtx(0, 0, 0.5f, 1, r5xx_vec4_make(1, 0, 0, 1), r5xx_vec4_make(0, 0, 0, 1));
        R5xxVertex b = vtx(256, 0, 0.5f, 1, r5xx_vec4_make(0, 1, 0, 1), r5xx_vec4_make(0, 0, 0, 1));
        R5xxVertex c = vtx(128, 256, 0.5f, 1, r5xx_vec4_make(0, 0, 1, 1), r5xx_vec4_make(0, 0, 0, 1));
        add_tri(a, b, c);
    }
    run_and_compare("RGB565 gouraud, color mask", RT_OFF, RT_W * RT_H * 2, 1, 400);
}

/* UV88 (two 8-bit channels: R slot, B slot) render target */
static void scenario_uv88(void)
{
    raster_defaults();
    tex_used = 0;
    fill_random(RT_OFF, RT_W * RT_H * 2);
    R.regs.rb3d_colorpitch[0] = RT_W | (13u << 21);   /* R300_COLOR_FORMAT_UV88 */
    load_prog(prog_color, 1);
    ntris = 0;
    {
        R5xxVertex a = vtx(0, 0, 0.5f, 1, r5xx_vec4_make(1, 0, 0, 1), r5xx_vec4_make(0, 0, 0, 1));
        R5xxVertex b = vtx(256, 0, 0.5f, 1, r5xx_vec4_make(0, 1, 0, 1), r5xx_vec4_make(0, 0, 0, 1));
        R5xxVertex c = vtx(128, 256, 0.5f, 1, r5xx_vec4_make(0, 0, 1, 1), r5xx_vec4_make(0, 0, 0, 1));
        add_tri(a, b, c);
    }
    run_and_compare("UV88 gouraud", RT_OFF, RT_W * RT_H * 2, 1, 400);
}

static void scenario_fp16(void)
{
    uint32_t i;
    raster_defaults();
    tex_used = 0;
    /* random but finite halves in [0,1] */
    for (i = 0; i < RT_W * RT_H * 8; i += 2) {
        uint16_t h = (uint16_t)(rnd() % 0x3c00);
        memcpy(vram + RT_OFF + i, &h, 2);
    }
    fill_pattern_argb(TEX_OFF, 1 << 20);
    R.regs.rb3d_colorpitch[0] = RT_W | (10u << 21);            /* ARGB16161616, linear */
    R.regs.us_out_fmt[0] = 18u | (3u << 8) | (2u << 10) | (1u << 12) | (0u << 14); /* C4_16_FP */
    R.regs.rb3d_blendcntl = 0x27210007;                        /* ONE, ONE_MINUS_SRC_ALPHA */
    R.regs.rb3d_ablendcntl = 0x27210000;
    set_tex(0, TEX_OFF | 0xc, 0x8007a0e2, 0x0000a60c, 0x000000ff, 0x00000a92, 0);
    load_prog(prog_composite, 2);
    ntris = 0;
    add_quad(3.25f, 7.5f, 230.1f, 249.9f, 0.5f, r5xx_vec4_make(0.7f, 0.7f, 0.7f, 0.7f),
             0, 0, 227, 245);
    run_and_compare("fp16 render target, blend", RT_OFF, RT_W * RT_H * 8, 1, 64);
}

/*
 * Draw order within one batch: many overlapping opaque quads of
 * different colors to the same surface, all in one batch, no flush in
 * between.  Same surface, so the backend puts no barrier between them and
 * relies on the fragment shader interlock for order: every pixel must end
 * up with the color of the last quad covering it, as on the CPU path.
 * Optionally blended (RVK_ORDER_BLEND) so that a lost or reordered write
 * also shows.
 */
static void scenario_order(int trials, int ndraws)
{
    char err[256];
    static RvkVertex gv[64];
    RvkDraw *draw = calloc(1, sizeof(RvkDraw));
    R5xxVkDrawReq req;
    uint32_t bad_total = 0, bad_trials = 0, len = RT_W * RT_H * 4;
    int t, d, i, nv;

    raster_defaults();
    tex_used = 0;
    load_prog(prog_color, 1);
    if (getenv("RVK_ORDER_BLEND")) {
        R.regs.rb3d_blendcntl = 0x27210007;
        R.regs.rb3d_ablendcntl = 0x27210000;
    }
    R.us.prog = dec;
    for (t = 0; t < trials; t++) {
        float qx0[64], qy0[64], qx1[64], qy1[64];
        r5xx_vec4 col[64];
        uint32_t bad = 0, first = UINT32_MAX;

        fill_random(RT_OFF, len);
        memcpy(backup, vram, VRAM_SIZE);
        for (d = 0; d < ndraws; d++) {
            qx0[d] = rnd() % 128; qy0[d] = rnd() % 128;
            qx1[d] = qx0[d] + 64 + rnd() % 128; qy1[d] = qy0[d] + 64 + rnd() % 128;
            col[d] = r5xx_vec4_make((rnd() % 256) / 255.0f, (rnd() % 256) / 255.0f,
                                    (rnd() % 256) / 255.0f, 0.5f + (rnd() % 128) / 255.0f);
        }
        /* CPU reference */
        for (d = 0; d < ndraws; d++) {
            ntris = 0;
            add_quad(qx0[d], qy0[d], qx1[d], qy1[d], 0.5f, col[d], 0, 0, 1, 1);
            r5xx_raster_setup(&R, err, sizeof(err));
            for (i = 0; i < ntris; i++) {
                r5xx_raster_triangle(&R, &tris[i * 3], &tris[i * 3 + 1], &tris[i * 3 + 2]);
            }
        }
        memcpy(cpu_out, vram, VRAM_SIZE);
        memcpy(vram, backup, VRAM_SIZE);
        r5xx_vk_upload(vk, 0, VRAM_SIZE);
        /* the same draws, one batch */
        for (d = 0; d < ndraws; d++) {
            ntris = 0;
            add_quad(qx0[d], qy0[d], qx1[d], qy1[d], 0.5f, col[d], 0, 0, 1, 1);
            r5xx_raster_setup(&R, err, sizeof(err));
            if (!rvk_build_draw(draw, &R, samp, tex_used, consts, &vram_only, err, sizeof(err))) {
                printf("FAIL [order] build: %s\n", err);
                failures++;
                free(draw);
                return;
            }
            nv = 0;
            for (i = 0; i < ntris; i++) {
                nv += rvk_triangle(&gv[nv], &R, &tris[i * 3], &tris[i * 3 + 1],
                                   &tris[i * 3 + 2], false);
            }
            memset(&req, 0, sizeof(req));
            req.draw = draw; req.verts = gv; req.nverts = nv; req.prog = dec;
            req.prog_hash = r500_us_hash(dec);
            req.sc_x0 = 0; req.sc_y0 = 0; req.sc_x1 = RT_W - 1; req.sc_y1 = RT_H - 1;
            rvk_dirty_ranges(&req, &R, &vram_only, 0, RT_H - 1);
            if (!r5xx_vk_draw(vk, &req, err, sizeof(err))) {
                printf("FAIL [order] vk draw: %s\n", err);
                failures++;
                free(draw);
                return;
            }
        }
        r5xx_vk_flush(vk);
        for (i = 0; i < (int)len; i++) {
            if (abs((int)vram[RT_OFF + i] - (int)cpu_out[RT_OFF + i]) > 1) {
                if (first == UINT32_MAX) {
                    first = i;
                }
                bad++;
            }
        }
        if (bad) {
            bad_trials++;
            bad_total += bad;
            if (bad_trials <= 5) {
                printf("     trial %d: %u bytes differ, first at pixel (%u, %u)\n", t, bad,
                       (first / 4) % RT_W, (first / 4) / RT_W);
            }
        }
    }
    printf("%s [order: %d batches of %d overlapping draws%s] %u batches wrong, %u bytes\n",
           bad_trials ? "FAIL" : "ok  ", trials, ndraws,
           getenv("RVK_ORDER_BLEND") ? ", blended" : "", bad_trials, bad_total);
    if (bad_trials) {
        failures++;
    }
    free(draw);
}

/*
 * A guest CPU write while a batch is pending: another vCPU writes
 * VRAM inside the batch's render band (rows are rounded to 32 for the
 * write-back) but outside what the draw covers.  The write-back must not
 * revert it.  Before the merged write-back it copied the whole band back from
 * the mirror.
 */
static void scenario_cpu_write_in_band(void)
{
    char err[256];
    static RvkVertex gv[64];
    RvkDraw *draw = calloc(1, sizeof(RvkDraw));
    R5xxVkDrawReq req;
    uint32_t pitch = RT_W * 4, i, lost = 0, drawn = 0;
    int nv = 0;

    raster_defaults();
    R.regs.rb3d_colorpitch[0] = RT_W | (6u << 21);          /* linear */
    tex_used = 0;
    load_prog(prog_color, 1);
    R.us.prog = dec;
    memset(vram + RT_OFF, 0x11, RT_W * RT_H * 4);
    r5xx_vk_upload(vk, 0, VRAM_SIZE);
    ntris = 0;
    add_quad(0, 0, RT_W, 16, 0.5f, r5xx_vec4_make(1, 0, 0, 1), 0, 0, 1, 1);
    r5xx_raster_setup(&R, err, sizeof(err));
    rvk_build_draw(draw, &R, samp, tex_used, consts, &vram_only, err, sizeof(err));
    for (i = 0; i < (uint32_t)ntris; i++) {
        nv += rvk_triangle(&gv[nv], &R, &tris[i * 3], &tris[i * 3 + 1], &tris[i * 3 + 2], false);
    }
    memset(&req, 0, sizeof(req));
    req.draw = draw; req.verts = gv; req.nverts = nv; req.prog = dec;
    req.prog_hash = r500_us_hash(dec);
    req.sc_x0 = 0; req.sc_y0 = 0; req.sc_x1 = RT_W - 1; req.sc_y1 = 15;
    rvk_dirty_ranges(&req, &R, &vram_only, 0, 15);
    /* the first batch brings the band into the mirror */
    if (!r5xx_vk_draw(vk, &req, err, sizeof(err))) {
        printf("FAIL [cpu write in band] vk draw: %s\n", err);
        failures++;
        free(draw);
        return;
    }
    r5xx_vk_flush(vk);
    /* the second one draws again; meanwhile "another vCPU" writes row 20,
     * inside the band (rows 0-31) but not drawn, and the dirty log has not
     * been collected yet (no r5xx_vk_upload) */
    r5xx_vk_draw(vk, &req, err, sizeof(err));
    memset(vram + RT_OFF + 20 * pitch, 0x77, pitch);
    r5xx_vk_flush(vk);
    for (i = 0; i < pitch; i++) {
        lost += vram[RT_OFF + 20 * pitch + i] != 0x77;
    }
    for (i = 0; i < pitch; i += 4) {
        drawn += vram[RT_OFF + i + 2] == 0xff;              /* red, BGRA */
    }
    printf("%s [cpu write in band] %u/%u bytes of the CPU write lost, %u/%u pixels drawn\n",
           lost || drawn != RT_W ? "FAIL" : "ok  ", lost, pitch, drawn, RT_W);
    if (lost || drawn != RT_W) {
        failures++;
    }
    free(draw);
}

/* Throughput: N copies of the composite draw (256x256 RT, textured,
 * blended) in one batch, against the CPU path. */
static void bench(int n, bool blend)
{
    char err[256];
    static RvkVertex gv[64];
    RvkDraw *draw = calloc(1, sizeof(RvkDraw));
    R5xxVkDrawReq req;
    struct timespec t0, t1, t2;
    int i, k, nv = 0;

    raster_defaults();
    tex_used = 0;
    fill_pattern_argb(TEX_OFF, 1 << 20);
    set_tex(0, TEX_OFF | 0xc, 0x8007a0e2, 0x0000a60c, 0x000000ff, 0x00000a92, 0);
    load_prog(prog_composite, 2);
    if (blend) {
        R.regs.rb3d_blendcntl = 0x27210007;
        R.regs.rb3d_ablendcntl = 0x27210000;
    }
    ntris = 0;
    add_quad(0, 0, 256, 256, 0.5f, r5xx_vec4_make(1, 1, 1, 1), 0, 0, 227, 245);
    R.us.prog = dec;
    r5xx_raster_setup(&R, err, sizeof(err));
    rvk_build_draw(draw, &R, samp, tex_used, consts, &vram_only, err, sizeof(err));
    for (i = 0; i < ntris; i++) {
        nv += rvk_triangle(&gv[nv], &R, &tris[i * 3], &tris[i * 3 + 1], &tris[i * 3 + 2], false);
    }
    memset(&req, 0, sizeof(req));
    req.draw = draw; req.verts = gv; req.nverts = nv; req.prog = dec;
    req.prog_hash = r500_us_hash(dec);
    req.sc_x0 = 0; req.sc_y0 = 0; req.sc_x1 = 255; req.sc_y1 = 255;
    rvk_dirty_ranges(&req, &R, &vram_only, 0, 255);
    r5xx_vk_upload(vk, 0, VRAM_SIZE);
    r5xx_vk_draw(vk, &req, err, sizeof(err));     /* warm up */
    r5xx_vk_flush(vk);
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (i = 0; i < n; i++) {
        r5xx_vk_draw(vk, &req, err, sizeof(err));
    }
    r5xx_vk_flush(vk);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    for (i = 0; i < 5; i++) {
        for (k = 0; k < ntris; k++) {
            r5xx_raster_triangle(&R, &tris[k * 3], &tris[k * 3 + 1], &tris[k * 3 + 2]);
        }
    }
    clock_gettime(CLOCK_MONOTONIC, &t2);
    {
        struct timespec a, b;
        clock_gettime(CLOCK_MONOTONIC, &a);
        for (i = 0; i < 50; i++) {
            r5xx_vk_draw(vk, &req, err, sizeof(err));
            r5xx_vk_flush(vk);
        }
        clock_gettime(CLOCK_MONOTONIC, &b);
        printf("bench: draw+flush round trip %.3f ms\n",
               ((b.tv_sec - a.tv_sec) * 1e3 + (b.tv_nsec - a.tv_nsec) / 1e6) / 50);
    }
    printf("bench %s: GPU %.3f ms per 256x256 draw (%d in one batch), CPU %.3f ms per draw\n",
           blend ? "blend" : "opaque",
           ((t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6) / n, n,
           ((t2.tv_sec - t1.tv_sec) * 1e3 + (t2.tv_nsec - t1.tv_nsec) / 1e6) / 5);
    free(draw);
}

/* 1024x1024 bilinear-sampled draw of a 1024x1024 tiled texture, many
 * times in one batch: texture-fetch bound. */
static void bench_big(int n)
{
    char err[256];
    static RvkVertex gv[64];
    RvkDraw *draw = calloc(1, sizeof(RvkDraw));
    R5xxVkDrawReq req;
    struct timespec t0, t1;
    int i, nv = 0;

    raster_defaults();
    tex_used = 0;
    R.regs.rb3d_colorpitch[0] = 1024 | (6u << 21) | (1u << 16) | (1u << 17);
    R.regs.sc_scissors_br = 1023 | (1023u << 13);
    fill_pattern_argb(TEX_OFF, 4 << 20);
    /* 1024x1024 ARGB8888 macro+micro, bilinear, normalized coords */
    set_tex(0, TEX_OFF | 0xc, 1023u | (1023u << 11), 0x0000a60c, 0, (2u << 9) | (2u << 11), 0);
    load_prog(prog_tex, 2);
    ntris = 0;
    add_quad(0, 0, 1024, 1024, 0.5f, r5xx_vec4_make(1, 1, 1, 1), 0.001f, 0.002f, 1.001f, 1.002f);
    R.us.prog = dec;
    r5xx_raster_setup(&R, err, sizeof(err));
    rvk_build_draw(draw, &R, samp, tex_used, consts, &vram_only, err, sizeof(err));
    for (i = 0; i < ntris; i++) {
        nv += rvk_triangle(&gv[nv], &R, &tris[i * 3], &tris[i * 3 + 1], &tris[i * 3 + 2], false);
    }
    memset(&req, 0, sizeof(req));
    req.draw = draw; req.verts = gv; req.nverts = nv; req.prog = dec;
    req.prog_hash = r500_us_hash(dec);
    req.sc_x0 = 0; req.sc_y0 = 0; req.sc_x1 = 1023; req.sc_y1 = 1023;
    rvk_dirty_ranges(&req, &R, &vram_only, 0, 1023);
    r5xx_vk_upload(vk, 0, VRAM_SIZE);
    r5xx_vk_draw(vk, &req, err, sizeof(err));
    r5xx_vk_flush(vk);
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (i = 0; i < n; i++) {
        r5xx_vk_draw(vk, &req, err, sizeof(err));
    }
    r5xx_vk_flush(vk);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    printf("bench big: GPU %.3f ms per 1024x1024 bilinear draw (texture 1024x1024 tiled)\n",
           ((t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6) / n);
    free(draw);
}

/* A captured Core Image kernel (raw US words in $RVK_KERNEL) on a
 * 1100x564 16-bit RT sampling a 64-bpp texture with 8 texcoord inputs. */
static void bench_kernel(const char *path, int n)
{
    char err[256];
    static uint32_t words[512 * 6];
    static RvkVertex gv[64];
    RvkDraw *draw = calloc(1, sizeof(RvkDraw));
    R5xxVkDrawReq req;
    struct timespec t0, t1, t2;
    FILE *f = fopen(path, "r");
    int nw = 0, i, k, nv = 0;
    unsigned v;

    if (!f) {
        return;
    }
    while (nw < 512 * 6 && fscanf(f, "%x", &v) == 1) {
        words[nw++] = v;
    }
    fclose(f);
    raster_defaults();
    tex_used = 0;
    R.num_fs_inputs = 10;
    R.regs.rb3d_colorpitch[0] = 1104 | (10u << 21);
    R.regs.us_out_fmt[0] = 18u | (3u << 8) | (2u << 10) | (1u << 12) | (0u << 14);
    R.regs.sc_scissors_br = 1101 | (564u << 13);
    fill_pattern_argb(TEX_OFF, 8 << 20);
    set_tex(0, TEX_OFF, 0x80119c4c, 0x0008860e, 0x0000044f, 0x00001436, 0x20100000);
    load_prog(words, nw / 6);
    prog.config = 2;
    {
        char e2[128];
        r500_us_free(dec);
        dec = r500_us_decode(&prog, e2, sizeof(e2));
    }
    ntris = 0;
    {
        R5xxVertex q[4];
        float xs[4] = { 0, 1102, 1102, 0 }, ys[4] = { 0, 0, 565, 565 };
        for (i = 0; i < 4; i++) {
            memset(&q[i], 0, sizeof(q[i]));
            q[i].x = xs[i]; q[i].y = ys[i]; q[i].z = 0.5f; q[i].w = 1;
            for (k = 0; k < 10; k++) {
                q[i].attr[k] = r5xx_vec4_make(xs[i] + (k - 4) * 1.5f, ys[i], 0, 1);
            }
        }
        add_tri(q[0], q[1], q[2]);
        add_tri(q[0], q[2], q[3]);
    }
    R.us.prog = dec;
    if (!r5xx_raster_setup(&R, err, sizeof(err)) ||
        !rvk_build_draw(draw, &R, samp, tex_used, consts, &vram_only, err, sizeof(err))) {
        printf("bench kernel: %s\n", err);
        return;
    }
    for (i = 0; i < ntris; i++) {
        nv += rvk_triangle(&gv[nv], &R, &tris[i * 3], &tris[i * 3 + 1], &tris[i * 3 + 2], false);
    }
    memset(&req, 0, sizeof(req));
    req.draw = draw; req.verts = gv; req.nverts = nv; req.prog = dec;
    req.prog_hash = r500_us_hash(dec);
    req.sc_x0 = 0; req.sc_y0 = 0; req.sc_x1 = 1101; req.sc_y1 = 564;
    rvk_dirty_ranges(&req, &R, &vram_only, 0, 564);
    r5xx_vk_upload(vk, 0, VRAM_SIZE);
    if (!r5xx_vk_draw(vk, &req, err, sizeof(err))) {
        printf("bench kernel: %s\n", err);
        return;
    }
    r5xx_vk_flush(vk);
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (i = 0; i < n; i++) {
        r5xx_vk_draw(vk, &req, err, sizeof(err));
    }
    r5xx_vk_flush(vk);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    for (k = 0; k < ntris; k++) {
        r5xx_raster_triangle(&R, &tris[k * 3], &tris[k * 3 + 1], &tris[k * 3 + 2]);
    }
    clock_gettime(CLOCK_MONOTONIC, &t2);
    printf("bench kernel (%d instr): GPU %.3f ms, CPU %.1f ms per 1102x565 draw\n", nw / 6,
           ((t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6) / n,
           (t2.tv_sec - t1.tv_sec) * 1e3 + (t2.tv_nsec - t1.tv_nsec) / 1e6);
    free(draw);
}

int main(void)
{
    char err[256];

    if (posix_memalign((void **)&vram, 1 << 21, VRAM_SIZE) ||
        posix_memalign((void **)&backup, 64, VRAM_SIZE) ||
        posix_memalign((void **)&cpu_out, 64, VRAM_SIZE)) {
        return 1;
    }
    memset(vram, 0, VRAM_SIZE);
    setenv("RV530_VK_SYNC_COMPILE", "1", 1);
    r5xx_flatmem_init(&fm, vram, VRAM_SIZE, 0);
    vk = r5xx_vk_create(vram, VRAM_SIZE, NULL, NULL, err, sizeof(err));
    if (!vk) {
        printf("SKIP: no Vulkan backend: %s\n", err);
        return 0;
    }
    scenario_composite();
    scenario_bilinear();
    scenario_endian();
    scenario_depth_stencil();
    scenario_565_mask();
    scenario_uv88();
    scenario_fp16();
    scenario_cpu_write_in_band();
    scenario_order(getenv("RVK_ORDER_TRIALS") ? atoi(getenv("RVK_ORDER_TRIALS")) : 50, 32);
    if (getenv("RVK_BENCH")) {
        bench(200, false);
        bench(200, true);
        bench_big(20);
        if (getenv("RVK_KERNEL")) {
            bench_kernel(getenv("RVK_KERNEL"), 10);
        }
    }
    r5xx_vk_destroy(vk);
    printf("%s (%d failures)\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? 1 : 0;
}
