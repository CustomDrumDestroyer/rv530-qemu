/*
 * RV530 legacy 2D engine (the R100-style "GUI" blitter that R5xx keeps).
 *
 * Two ways in:
 *  - register writes (PM4 type 0 or MMIO): DP_GUI_MASTER_CNTL, pitch/offset,
 *    colors, clip, coordinates; writing DST_HEIGHT_WIDTH or DST_WIDTH_HEIGHT
 *    starts the operation.  This is what Mesa/xf86-video-ati submit through
 *    the CS ioctl on R300-R500 (r300_cs checker allows only these).
 *  - PM4 type 3 CNTL_* packets (PAINT, BITBLT, PAINT_MULTI, BITBLT_MULTI),
 *    used by the kernel's r100_copy_blit for buffer moves and possibly by
 *    Apple's driver.
 *
 * Supported: solid fill and memory-source blits with any ROP3, write mask,
 * destination clipping, blit direction, 8/15/16/32 bpp, macro/micro tiled
 * surfaces (through r5xx_surface_pixel_offset).  Mono expansion, host-data
 * blits, lines and text are logged as unimplemented.
 *
 * Register definitions: Linux drivers/gpu/drm/radeon/radeon_reg.h.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "rv530.h"
#include "r5xx_tex.h"
#include "trace.h"

#define R2D_DST_OFFSET           0x1404
#define R2D_DST_PITCH            0x1408
#define R2D_SRC_PITCH_OFFSET     0x1428
#define R2D_DST_PITCH_OFFSET     0x142c
#define R2D_SRC_Y_X              0x1434
#define R2D_DST_Y_X              0x1438
#define R2D_DST_HEIGHT_WIDTH     0x143c
#define R2D_DP_GUI_MASTER_CNTL   0x146c
#define R2D_DP_BRUSH_BKGD_CLR    0x1478
#define R2D_DP_BRUSH_FRGD_CLR    0x147c
#define R2D_SRC_X_Y              0x1590
#define R2D_DST_X_Y              0x1594
#define R2D_DST_WIDTH_HEIGHT     0x1598
#define R2D_SRC_OFFSET           0x15ac
#define R2D_SRC_PITCH            0x15b0
/* byte swap of source / destination data in system memory (0 none,
 * 1 within 16 bits, 2 within 32 bits, 3 16-bit halves); not in the public
 * headers.  VRAM accesses are not swapped: the driver leaves both at 2
 * after a big-endian upload and the glyph blits that follow copy VRAM. */
#define R2D_DST_ENDIAN           0x15d0
#define R2D_SRC_ENDIAN           0x15d4
#define R2D_DP_SRC_FRGD_CLR      0x15d8
#define R2D_DP_SRC_BKGD_CLR      0x15dc
#define R2D_DP_CNTL              0x16c0
#define R2D_DP_DATATYPE          0x16c4
#define R2D_DP_MIX               0x16c8
#define R2D_DP_WRITE_MASK        0x16cc
#define R2D_DP_CNTL_XDIR_YDIR_YMAJOR 0x16d0
#define R2D_DEFAULT_PITCH_OFFSET 0x16e0
#define R2D_DEFAULT_SC_BOTTOM_RIGHT 0x16e8
#define R2D_SC_TOP_LEFT          0x16ec
#define R2D_SC_BOTTOM_RIGHT      0x16f0
#define R2D_SRC_SC_BOTTOM_RIGHT  0x16f4
#define R2D_DSTCACHE_CTLSTAT     0x1714
#define R2D_DST_TILE             0x1700
#define R2D_SRC_TILE             0x1704
#define R2D_HOST_DATA0           0x17c0
#define R2D_HOST_DATA_LAST       0x17e0

#define GMC_SRC_PITCH_OFFSET_CNTL (1u << 0)
#define GMC_DST_PITCH_OFFSET_CNTL (1u << 1)
#define GMC_SRC_CLIPPING          (1u << 2)
#define GMC_DST_CLIPPING          (1u << 3)
#define GMC_BRUSH_SHIFT           4
#define GMC_DST_DATATYPE_SHIFT    8
#define GMC_SRC_DATATYPE_SHIFT    12
#define GMC_ROP3_SHIFT            16
#define GMC_SRC_SOURCE_SHIFT      24
#define GMC_WR_MSK_DIS            (1u << 30)

#define BRUSH_SOLID_COLOR         13
#define BRUSH_NONE                15
#define SRC_SOURCE_MEMORY         2
#define SRC_SOURCE_HOST_DATA      3
#define SRC_DATATYPE_COLOR        3

#define DST_X_LEFT_TO_RIGHT       (1u << 0)
#define DST_Y_TOP_TO_BOTTOM       (1u << 1)
#define TILE_MACRO                (1u << 30)
#define TILE_MICRO                (2u << 30)

#define PM4_CNTL_PAINT            0x91
#define PM4_CNTL_BITBLT           0x92
#define PM4_CNTL_SMALLTEXT        0x93
#define PM4_CNTL_HOSTDATA_BLT     0x94
#define PM4_CNTL_POLYLINE         0x95
#define PM4_CNTL_POLYSCANLINES    0x98
#define PM4_CNTL_PAINT_MULTI      0x9a
#define PM4_CNTL_BITBLT_MULTI     0x9b
#define PM4_CNTL_TRANS_BITBLT     0x9c

#define REG(s, off) ((s)->regs[(off) >> 2])

typedef struct R2dSurface {
    uint32_t addr;          /* GPU address */
    uint32_t pitch;         /* bytes */
    uint32_t bpp;           /* bytes per pixel */
    uint32_t tile;          /* TILE_MACRO / TILE_MICRO bits */
    R5xxSurfaceLayout l;
} R2dSurface;

static uint32_t r2d_bpp(uint32_t datatype)
{
    switch (datatype) {
    case 2: case 7: case 8: return 1;        /* 8bpp CI, RGB332, Y8 */
    case 3: case 4: case 15: case 11: case 12: case 14: return 2;
    case 5: return 3;                        /* 24bpp, not used by drivers */
    case 6: return 4;
    default: return 4;
    }
}

static void r2d_surface(RV530State *s, R2dSurface *sf, uint32_t pitch_offset,
                        uint32_t bpp, int legacy)
{
    sf->addr = (pitch_offset & 0x3fffff) << 10;
    sf->pitch = ((pitch_offset >> 22) & 0xff) * 64;
    sf->tile = pitch_offset & (TILE_MACRO | TILE_MICRO);
    if (legacy >= 0 && s->r2d_legacy[legacy]) {
        /* legacy split registers: tiling from DST_TILE / SRC_TILE
         * (0x1700 / 0x1704), as programmed by Apple's ATIR500AtomicBltMgr.
         * Same 2-bit code as PITCH_OFFSET bits 31:30: 1 macro, 2 micro,
         * 3 both (Front Row uploads 2 into textures it samples micro-only) */
        uint32_t t = REG(s, legacy ? R2D_SRC_TILE : R2D_DST_TILE);
        sf->addr = REG(s, legacy ? R2D_SRC_OFFSET : R2D_DST_OFFSET);
        /* pitch in bytes, wider than 14 bits: Apple uploads vertex
         * buffers as 32736-byte rows (8184 pixels at 32 bpp) */
        sf->pitch = REG(s, legacy ? R2D_SRC_PITCH : R2D_DST_PITCH) & 0xffff;
        sf->tile = (t & 3) << 30;
    }
    sf->bpp = bpp;
    if (sf->tile && rv530_linear_surface(s, sf->addr)) {
        sf->tile = 0;
    }
    memset(&sf->l, 0, sizeof(sf->l));
    sf->l.gpu_addr = sf->addr;
    sf->l.pitch_bytes = sf->pitch;
    sf->l.width = bpp ? sf->pitch / bpp : 0;
    sf->l.height = 8192;
    sf->l.bpp = bpp;
    sf->l.is_r500 = true;
    sf->l.tiling = (sf->tile == (TILE_MACRO | TILE_MICRO)) ? R5XX_TILE_MICRO_MACRO :
                   (sf->tile == TILE_MACRO) ? R5XX_TILE_MACRO :
                   (sf->tile == TILE_MICRO) ? R5XX_TILE_MICRO : R5XX_TILE_LINEAR;
    /* the 2D engine micro-tiles the regular way at every depth (16 bpp
     * "square" micro tiles exist only for textures/color buffers that
     * ask for them) */
    sf->l.micro_mode = 1;
}

static inline uint32_t r2d_px_addr(const R2dSurface *sf, uint32_t x, uint32_t y)
{
    if (!sf->tile) {
        return sf->addr + y * sf->pitch + x * sf->bpp;
    }
    return sf->addr + r5xx_surface_pixel_offset(&sf->l, x, y);
}

/* read/write a horizontal run of pixels */
static void r2d_read_run(RV530State *s, const R2dSurface *sf, uint32_t x,
                         uint32_t y, uint32_t w, uint8_t *buf)
{
    uint32_t i;
    if (!sf->tile) {
        rv530_gpu_read(s, r2d_px_addr(sf, x, y), buf, w * sf->bpp);
        return;
    }
    for (i = 0; i < w; i++) {
        rv530_gpu_read(s, r2d_px_addr(sf, x + i, y), buf + i * sf->bpp, sf->bpp);
    }
}

static void r2d_write_run(RV530State *s, const R2dSurface *sf, uint32_t x,
                          uint32_t y, uint32_t w, const uint8_t *buf)
{
    uint32_t i;
    if (!sf->tile) {
        rv530_gpu_write(s, r2d_px_addr(sf, x, y), buf, w * sf->bpp);
        return;
    }
    for (i = 0; i < w; i++) {
        rv530_gpu_write(s, r2d_px_addr(sf, x + i, y), buf + i * sf->bpp, sf->bpp);
    }
}

static inline uint32_t rop3(uint32_t rop, uint32_t p, uint32_t sv, uint32_t d)
{
    uint32_t r = 0;
    int i;

    switch (rop) {
    case 0x00: return 0;
    case 0xff: return ~0u;
    case 0xcc: return sv;
    case 0xf0: return p;
    case 0xaa: return d;
    case 0x66: return sv ^ d;
    case 0x88: return sv & d;
    case 0xee: return sv | d;
    case 0x5a: return p ^ d;
    case 0x55: return ~d;
    case 0x33: return ~sv;
    default:
        break;
    }
    /* generic: bit i of the ROP is the result for (P,S,D) = (i>>2, i>>1, i)&1 */
    for (i = 0; i < 8; i++) {
        if (rop & (1u << i)) {
            r |= ((i & 4) ? p : ~p) & ((i & 2) ? sv : ~sv) & ((i & 1) ? d : ~d);
        }
    }
    return r;
}

static inline uint32_t ldpx(const uint8_t *p, uint32_t bpp)
{
    switch (bpp) {
    case 1: return p[0];
    case 2: return lduw_le_p(p);
    case 3: return p[0] | p[1] << 8 | p[2] << 16;
    default: return ldl_le_p(p);
    }
}

static inline void stpx(uint8_t *p, uint32_t bpp, uint32_t v)
{
    switch (bpp) {
    case 1: p[0] = v; break;
    case 2: stw_le_p(p, v); break;
    case 3: p[0] = v; p[1] = v >> 8; p[2] = v >> 16; break;
    default: stl_le_p(p, v); break;
    }
}

static void r2d_swap(uint8_t *buf, uint32_t len, uint32_t mode)
{
    uint32_t i;

    switch (mode & 3) {
    case 1:
        for (i = 0; i + 1 < len; i += 2) {
            stw_le_p(buf + i, bswap16(lduw_le_p(buf + i)));
        }
        break;
    case 2:
        for (i = 0; i + 3 < len; i += 4) {
            stl_le_p(buf + i, bswap32(ldl_le_p(buf + i)));
        }
        break;
    case 3:
        for (i = 0; i + 3 < len; i += 4) {
            stl_le_p(buf + i, ror32(ldl_le_p(buf + i), 16));
        }
        break;
    }
}

/*
 * Execute one rectangle.  (dx, dy) / (sx, sy) are the starting corner as
 * the hardware sees it: the right/bottom edge when the direction bits say
 * right-to-left / bottom-to-top.
 */
static void r2d_rect_body(RV530State *s, int32_t dx, int32_t dy, int32_t sx,
                          int32_t sy, uint32_t w, uint32_t h);

static void r2d_rect(RV530State *s, int32_t dx, int32_t dy, int32_t sx,
                     int32_t sy, uint32_t w, uint32_t h)
{
    int64_t t0 = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);

    r2d_rect_body(s, dx, dy, sx, sy, w, h);
    s->prof_ns[RV_PROF_2D] += qemu_clock_get_ns(QEMU_CLOCK_REALTIME) - t0;
    s->prof_n[RV_PROF_2D]++;
}

static void r2d_rect_body(RV530State *s, int32_t dx, int32_t dy, int32_t sx,
                          int32_t sy, uint32_t w, uint32_t h)
{
    uint32_t gmc = REG(s, R2D_DP_GUI_MASTER_CNTL);
    uint32_t dpcntl = REG(s, R2D_DP_CNTL);
    uint32_t brush = (gmc >> GMC_BRUSH_SHIFT) & 0xf;
    uint32_t bpp = r2d_bpp((gmc >> GMC_DST_DATATYPE_SHIFT) & 0xf);
    uint32_t rop = (gmc >> GMC_ROP3_SHIFT) & 0xff;
    uint32_t src_source = (gmc >> GMC_SRC_SOURCE_SHIFT) & 7;
    bool uses_src = ((rop >> 2) ^ rop) & 0x33;
    bool uses_dst = ((rop >> 1) ^ rop) & 0x55;
    uint32_t pmask = bpp == 4 ? ~0u : (1u << (bpp * 8)) - 1;
    uint32_t wmask = (gmc & GMC_WR_MSK_DIS) ? pmask
                                             : REG(s, R2D_DP_WRITE_MASK) & pmask;
    uint32_t pat = REG(s, R2D_DP_BRUSH_FRGD_CLR);
    /* Apple's driver uploads big-endian ARGB images from system memory
     * (the Desktop preferences preview) with the swap registers at 2 */
    uint32_t sswap = REG(s, R2D_SRC_ENDIAN) & 3, dswap = REG(s, R2D_DST_ENDIAN) & 3;
    int32_t cl, ct, cr, cb, x0, y0, x1, y1, ddx, ddy;
    R2dSurface dst, src;
    uint8_t *sbuf, *dbuf;
    uint32_t y, i;
    bool xltr = dpcntl & DST_X_LEFT_TO_RIGHT;
    bool yttb = dpcntl & DST_Y_TOP_TO_BOTTOM;

    if (!w || !h) {
        return;
    }
    rv530_gfx_flush(s);     /* order against queued GPU draws */
    if (uses_src && src_source != SRC_SOURCE_MEMORY) {
        qemu_log_mask(LOG_UNIMP, "rv530: 2D source %u (host data/mono) "
                      "not implemented, gmc 0x%08x\n", src_source, gmc);
        return;
    }
    if (brush != BRUSH_SOLID_COLOR && brush != BRUSH_NONE &&
        (((rop >> 4) ^ rop) & 0x0f)) {
        qemu_log_mask(LOG_UNIMP, "rv530: 2D brush type %u not implemented\n",
                      brush);
        pat = REG(s, R2D_DP_BRUSH_FRGD_CLR);
    }

    r2d_surface(s, &dst, (gmc & GMC_DST_PITCH_OFFSET_CNTL) ?
                REG(s, R2D_DST_PITCH_OFFSET) : REG(s, R2D_DEFAULT_PITCH_OFFSET),
                bpp, (gmc & GMC_DST_PITCH_OFFSET_CNTL) ? 0 : -1);
    r2d_surface(s, &src, (gmc & GMC_SRC_PITCH_OFFSET_CNTL) ?
                REG(s, R2D_SRC_PITCH_OFFSET) : REG(s, R2D_DEFAULT_PITCH_OFFSET),
                bpp, (gmc & GMC_SRC_PITCH_OFFSET_CNTL) ? 1 : -1);
    if (!dst.pitch || (uses_src && !src.pitch)) {
        qemu_log_mask(LOG_GUEST_ERROR, "rv530: 2D zero pitch (dst 0x%08x src "
                      "0x%08x)\n", REG(s, R2D_DST_PITCH_OFFSET),
                      REG(s, R2D_SRC_PITCH_OFFSET));
        return;
    }

    /* normalize to a top-left rectangle */
    x0 = xltr ? dx : dx - (int32_t)w + 1;
    y0 = yttb ? dy : dy - (int32_t)h + 1;
    if (!xltr) {
        sx -= (int32_t)w - 1;
    }
    if (!yttb) {
        sy -= (int32_t)h - 1;
    }
    x1 = x0 + w;
    y1 = y0 + h;

    /* destination clip: SC_TOP_LEFT/SC_BOTTOM_RIGHT, else the default */
    if (gmc & GMC_DST_CLIPPING) {
        cl = REG(s, R2D_SC_TOP_LEFT) & 0x3fff;
        ct = (REG(s, R2D_SC_TOP_LEFT) >> 16) & 0x3fff;
        cr = REG(s, R2D_SC_BOTTOM_RIGHT) & 0x3fff;
        cb = (REG(s, R2D_SC_BOTTOM_RIGHT) >> 16) & 0x3fff;
    } else {
        cl = ct = 0;
        cr = REG(s, R2D_DEFAULT_SC_BOTTOM_RIGHT) & 0x3fff;
        cb = (REG(s, R2D_DEFAULT_SC_BOTTOM_RIGHT) >> 16) & 0x3fff;
        if (!cr || !cb) {
            cr = cb = 0x3fff;
        }
    }
    /* the hardware clips to bottom/right inclusive of the value - 1:
     * drivers program the size (or 0x1fff) there */
    ddx = x0 < cl ? cl - x0 : 0;
    ddy = y0 < ct ? ct - y0 : 0;
    x0 += ddx; sx += ddx;
    y0 += ddy; sy += ddy;
    if (x1 > cr) {
        x1 = cr;
    }
    if (y1 > cb) {
        y1 = cb;
    }
    if (x0 >= x1 || y0 >= y1 || x0 < 0 || y0 < 0 || sx < 0 || sy < 0) {
        return;
    }
    w = x1 - x0;
    h = y1 - y0;
    trace_rv530_2d(gmc, dst.addr, dst.pitch, x0, y0, w, h,
                   uses_src ? src.addr : 0, sx, sy);

    trace_rv530_2d_state(rop, pat, wmask, dpcntl);
    if (src.addr >= s->fb_start && src.addr <= s->fb_end) {
        sswap = 0;
    }
    if (dst.addr >= s->fb_start && dst.addr <= s->fb_end) {
        dswap = 0;
    }
    sbuf = uses_src ? g_malloc(w * bpp) : NULL;
    dbuf = g_malloc(w * bpp);

    /* overlapping copies in the same surface must run in the right order;
     * reading a whole run before writing it handles the X direction */
    for (i = 0; i < h; i++) {
        bool overlap_down = uses_src && src.addr == dst.addr && sy < y0;
        y = overlap_down ? h - 1 - i : i;
        if (uses_src) {
            r2d_read_run(s, &src, sx, sy + y, w, sbuf);
            r2d_swap(sbuf, w * bpp, sswap);
        }
        if (uses_dst || wmask != pmask) {
            r2d_read_run(s, &dst, x0, y0 + y, w, dbuf);
            r2d_swap(dbuf, w * bpp, dswap);
        }
        if (rop == 0xcc && wmask == pmask) {
            memcpy(dbuf, sbuf, w * bpp);
        } else {
            uint32_t xx;
            for (xx = 0; xx < w; xx++) {
                uint32_t d = (uses_dst || wmask != pmask) ?
                             ldpx(dbuf + xx * bpp, bpp) : 0;
                uint32_t sv = uses_src ? ldpx(sbuf + xx * bpp, bpp) : 0;
                uint32_t r = rop3(rop, pat, sv, d);
                r = (r & wmask) | (d & ~wmask);
                stpx(dbuf + xx * bpp, bpp, r);
            }
        }
        r2d_swap(dbuf, w * bpp, dswap);
        r2d_write_run(s, &dst, x0, y0 + y, w, dbuf);
    }
    g_free(sbuf);
    g_free(dbuf);
}

static void r2d_trigger(RV530State *s, uint32_t w, uint32_t h)
{
    uint32_t dyx = REG(s, R2D_DST_Y_X), syx = REG(s, R2D_SRC_Y_X);
    int32_t dx = (int16_t)(dyx & 0xffff), dy = (int16_t)(dyx >> 16);
    int32_t sx = (int16_t)(syx & 0xffff), sy = (int16_t)(syx >> 16);

    r2d_rect(s, dx, dy, sx, sy, w, h);
    /* like the hardware, advance the destination below the rectangle so
     * that consecutive scanline fills work */
    if (REG(s, R2D_DP_CNTL) & DST_Y_TOP_TO_BOTTOM) {
        REG(s, R2D_DST_Y_X) = ((uint32_t)(dy + h) << 16) | (dx & 0xffff);
    }
}

bool rv530_2d_reg_read(RV530State *s, uint32_t off, uint32_t *val)
{
    switch (off) {
    case R2D_DSTCACHE_CTLSTAT:
        *val = 0;                  /* flushes complete immediately */
        return true;
    case R2D_DST_X_Y: {
        uint32_t v = REG(s, R2D_DST_Y_X);
        *val = (v << 16) | (v >> 16);
        return true;
    }
    default:
        return false;
    }
}

bool rv530_2d_reg_write(RV530State *s, uint32_t off, uint32_t val)
{
    if (off < 0x1400 || off >= 0x1800) {
        return false;
    }
    trace_rv530_2d_reg(off, val);
    switch (off) {
    case R2D_SRC_X_Y:
        REG(s, R2D_SRC_Y_X) = (val << 16) | (val >> 16);
        return true;
    case R2D_DST_X_Y:
        REG(s, R2D_DST_Y_X) = (val << 16) | (val >> 16);
        return true;
    case R2D_DST_HEIGHT_WIDTH:
        REG(s, off) = val;
        r2d_trigger(s, val & 0xffff, val >> 16);
        return true;
    case R2D_DST_WIDTH_HEIGHT:
        REG(s, off) = val;
        r2d_trigger(s, val >> 16, val & 0xffff);
        return true;
    case R2D_DST_OFFSET:
        /* legacy split form: keep DST_PITCH_OFFSET coherent */
        s->r2d_legacy[0] = true;
        REG(s, off) = val;
        REG(s, R2D_DST_PITCH_OFFSET) = (REG(s, R2D_DST_PITCH_OFFSET) & 0xffc00000) |
                                       ((val >> 10) & 0x3fffff);
        return true;
    case R2D_DST_PITCH:
        s->r2d_legacy[0] = true;
        REG(s, off) = val;
        REG(s, R2D_DST_PITCH_OFFSET) = (REG(s, R2D_DST_PITCH_OFFSET) & 0xc03fffff) |
                                       (((val / 64) & 0xff) << 22);
        return true;
    case R2D_SRC_OFFSET:
        s->r2d_legacy[1] = true;
        REG(s, off) = val;
        REG(s, R2D_SRC_PITCH_OFFSET) = (REG(s, R2D_SRC_PITCH_OFFSET) & 0xffc00000) |
                                       ((val >> 10) & 0x3fffff);
        return true;
    case R2D_SRC_PITCH:
        s->r2d_legacy[1] = true;
        REG(s, off) = val;
        REG(s, R2D_SRC_PITCH_OFFSET) = (REG(s, R2D_SRC_PITCH_OFFSET) & 0xc03fffff) |
                                       (((val / 64) & 0xff) << 22);
        return true;
    case R2D_DP_CNTL_XDIR_YDIR_YMAJOR:
        REG(s, off) = val;
        REG(s, R2D_DP_CNTL) = ((val >> 31) ? DST_X_LEFT_TO_RIGHT : 0) |
                              (((val >> 15) & 1) ? DST_Y_TOP_TO_BOTTOM : 0);
        return true;
    case R2D_SRC_PITCH_OFFSET:
    case R2D_DST_PITCH_OFFSET:
        s->r2d_legacy[off == R2D_SRC_PITCH_OFFSET] = false;
        REG(s, off) = val;
        return true;
    case R2D_DP_GUI_MASTER_CNTL:
    case R2D_SRC_Y_X:
    case R2D_DST_Y_X:
    case R2D_DP_BRUSH_BKGD_CLR:
    case R2D_DP_BRUSH_FRGD_CLR:
    case R2D_DP_SRC_FRGD_CLR:
    case R2D_DP_SRC_BKGD_CLR:
    case R2D_DP_CNTL:
    case R2D_DP_DATATYPE:
    case R2D_DP_MIX:
    case R2D_DP_WRITE_MASK:
    case R2D_DEFAULT_PITCH_OFFSET:
    case R2D_DEFAULT_SC_BOTTOM_RIGHT:
    case R2D_SC_TOP_LEFT:
    case R2D_SC_BOTTOM_RIGHT:
    case R2D_SRC_SC_BOTTOM_RIGHT:
    case R2D_DSTCACHE_CTLSTAT:
    case R2D_DST_TILE:
    case R2D_SRC_TILE:
        REG(s, off) = val;
        return true;
    default:
        if (off >= R2D_HOST_DATA0 && off <= R2D_HOST_DATA_LAST) {
            qemu_log_mask(LOG_UNIMP, "rv530: 2D host data blit not "
                          "implemented\n");
            return true;
        }
        return false;
    }
}

/*
 * CNTL_* packets.  Layout (R100 CP packet spec, as built by Linux
 * r100_copy_blit and the old DRM radeon_cp_dispatch_*):
 *   GUI_MASTER_CNTL
 *   [SRC_PITCH_OFFSET]                  if GMC_SRC_PITCH_OFFSET_CNTL
 *   [DST_PITCH_OFFSET]                  if GMC_DST_PITCH_OFFSET_CNTL
 *   [SRC_SC_BOTTOM_RIGHT]               if GMC_SRC_CLIPPING
 *   [SC_TOP_LEFT, SC_BOTTOM_RIGHT]      if GMC_DST_CLIPPING
 *   [brush data]                        1 dword for solid color
 *   [DP_SRC_FRGD_CLR, DP_SRC_BKGD_CLR]  for mono sources
 *   then per rectangle: [SRC_X_Y] DST_X_Y DST_WIDTH_HEIGHT
 */
bool rv530_2d_pm4(RV530State *s, uint32_t op, const uint32_t *d, uint32_t n)
{
    uint32_t i = 0, gmc, brush, srcdt, src_source;
    bool blit;

    switch (op) {
    case PM4_CNTL_PAINT:
    case PM4_CNTL_PAINT_MULTI:
        blit = false;
        break;
    case PM4_CNTL_BITBLT:
    case PM4_CNTL_BITBLT_MULTI:
        blit = true;
        break;
    case PM4_CNTL_SMALLTEXT:
    case PM4_CNTL_HOSTDATA_BLT:
    case PM4_CNTL_POLYLINE:
    case PM4_CNTL_POLYSCANLINES:
    case PM4_CNTL_TRANS_BITBLT:
        qemu_log_mask(LOG_UNIMP, "rv530: 2D packet 0x%02x not implemented\n",
                      op);
        return true;
    default:
        return false;
    }
    if (n < 1) {
        return true;
    }
    gmc = d[i++];
    REG(s, R2D_DP_GUI_MASTER_CNTL) = gmc;
    if ((gmc & GMC_SRC_PITCH_OFFSET_CNTL) && i < n) {
        REG(s, R2D_SRC_PITCH_OFFSET) = d[i++];
        s->r2d_legacy[1] = false;
    }
    if ((gmc & GMC_DST_PITCH_OFFSET_CNTL) && i < n) {
        REG(s, R2D_DST_PITCH_OFFSET) = d[i++];
        s->r2d_legacy[0] = false;
    }
    if ((gmc & GMC_SRC_CLIPPING) && i < n) {
        REG(s, R2D_SRC_SC_BOTTOM_RIGHT) = d[i++];
    }
    if ((gmc & GMC_DST_CLIPPING) && i + 1 < n) {
        REG(s, R2D_SC_TOP_LEFT) = d[i++];
        REG(s, R2D_SC_BOTTOM_RIGHT) = d[i++];
    }
    brush = (gmc >> GMC_BRUSH_SHIFT) & 0xf;
    switch (brush) {
    case BRUSH_NONE:
        break;
    case BRUSH_SOLID_COLOR:
        if (i < n) {
            REG(s, R2D_DP_BRUSH_FRGD_CLR) = d[i++];
        }
        break;
    case 0: case 2:                       /* 8x8 mono: fg, bg, 2 dw pattern */
        i += 4;
        break;
    case 1: case 3:                       /* 8x8 mono, transparent bg */
        i += 3;
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "rv530: 2D packet brush %u\n", brush);
        return true;
    }
    srcdt = (gmc >> GMC_SRC_DATATYPE_SHIFT) & 3;
    src_source = (gmc >> GMC_SRC_SOURCE_SHIFT) & 7;
    if (blit && srcdt != SRC_DATATYPE_COLOR) {
        i += 2;                           /* mono source colors */
    }
    (void)src_source;
    /* packets are always left-to-right, top-to-bottom */
    REG(s, R2D_DP_CNTL) = DST_X_LEFT_TO_RIGHT | DST_Y_TOP_TO_BOTTOM;
    while (i + (blit ? 3 : 2) <= n) {
        uint32_t sxy = blit ? d[i++] : 0;
        uint32_t dxy = d[i++];
        uint32_t wh = d[i++];
        r2d_rect(s, dxy >> 16, dxy & 0xffff, sxy >> 16, sxy & 0xffff,
                 wh >> 16, wh & 0xffff);
        if (op == PM4_CNTL_PAINT || op == PM4_CNTL_BITBLT) {
            break;
        }
    }
    return true;
}
