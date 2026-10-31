/*
 * RV530 AVIVO display engine: two display controllers (D1, D2) with
 * graphics surface scanout, CRTC timing and vblank, hardware cursor and
 * gamma LUT each, plus the shared display interrupt, hot plug and GPIO DDC
 * (EDID) logic.
 *
 * The D2 register block is the D1 block shifted by 0x800 (Linux
 * radeon_crtc->crtc_offset); every per-CRTC register below is named by its
 * D1 offset and accessed at c->base + offset.  Each CRTC is one QEMU
 * console head.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/bitmap.h"
#include "qapi/error.h"
#include "hw/display/edid.h"
#include "ui/pixel_ops.h"
#include "ui/input.h"
#include "rv530.h"
#include "r5xx_tex.h"
#include "trace.h"
#include "r5xx_vk.h"
#include "hw/core/cpu.h"
#include "system/physmem.h"
#include "system/tcg.h"

#define FRAME_HZ 60
#define DEFAULT_LINES 806          /* used until a mode is programmed */
#define VRAM_PAGE_SHIFT 12

/* per-CRTC registers that are not in rv530.h yet (D1 offsets) */
#define RV_D1CRTC_STATUS_HV       RV_D1CRTC_STATUS_HV_COUNT
#define RV_D1CRTC_CURRENT_MASTER_EN_STATE (1u << 16)
#define RV_D1GRPH_LUT_SEL_B       (1u << 0)
#define RV_DC_LUT_RW_MODE         0x6484
#define RV_DC_LUT_WRITE_EN_MASK   0x649c

/* register value of a per-CRTC register */
#define CREG(c, off) ((c)->s->regs[((c)->base + (off)) >> 2])

/* ------------------------------------------------------------------ */
/* Timing                                                              */
/* ------------------------------------------------------------------ */

static uint32_t rv530_crtc_lines(RV530Crtc *c)
{
    uint32_t vt = CREG(c, RV_D1CRTC_V_TOTAL) & 0x1fff;
    return vt ? vt + 1 : DEFAULT_LINES;
}

/* vblank window [start, end) in CRTC line coordinates; may wrap */
static void rv530_crtc_vbl(RV530Crtc *c, uint32_t *start, uint32_t *end)
{
    uint32_t vbl = CREG(c, RV_D1CRTC_V_BLANK_START_END);
    uint32_t lines = rv530_crtc_lines(c);

    *start = vbl & 0x1fff;
    *end = (vbl >> 16) & 0x1fff;
    if (!vbl || *start >= lines || *end >= lines) {
        /* not programmed: last 4% of the frame */
        *start = lines - lines / 25;
        *end = 0;
    }
}

/*
 * Both CRTCs share one frame timeline that starts at the vblank timer:
 * at frame_start_ns each CRTC is at its first vblank line, so the
 * vblank interrupt and the counters agree with what the position says.
 */
static uint32_t rv530_crtc_vcount(RV530Crtc *c)
{
    RV530State *s = c->s;
    int64_t t = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) - s->frame_start_ns;
    uint32_t lines = rv530_crtc_lines(c), start, end;

    if (t < 0) {
        t = 0;
    }
    if (t >= s->frame_period_ns) {
        t = s->frame_period_ns - 1;
    }
    rv530_crtc_vbl(c, &start, &end);
    return (start + (uint32_t)(t * lines / s->frame_period_ns)) % lines;
}

static bool rv530_crtc_in_vblank(RV530Crtc *c)
{
    uint32_t v, start, end;

    if (!(CREG(c, RV_D1CRTC_CONTROL) & RV_CRTC_EN)) {
        return true;               /* a disabled CRTC is permanently blank */
    }
    v = rv530_crtc_vcount(c);
    rv530_crtc_vbl(c, &start, &end);
    if (start <= end) {
        return v >= start && v < end;
    }
    return v >= start || v < end;
}

bool rv530_avivo_irq_pending(RV530State *s)
{
    uint32_t mask = s->regs[RV_DxMODE_INT_MASK >> 2];

    if (s->crtc[0].vblank_occurred && (mask & RV_D1MODE_VBLANK_INT_MASK)) {
        return true;
    }
    if (s->crtc[1].vblank_occurred && (mask & RV_D2MODE_VBLANK_INT_MASK)) {
        return true;
    }
    if (s->hpd1_pending &&
        (s->regs[RV_DC_HOT_PLUG_DETECT1_INT_CONTROL >> 2] & RV_HPD_INT_EN)) {
        return true;
    }
    return false;
}

/*
 * Frame period of the mode D1 runs: H_TOTAL x V_TOTAL / pixel clock, the
 * clock from the P1PLL dividers (27 MHz x fb.frac / (ref x post)) that the
 * driver or SetPixelClock programmed.  60 Hz until those look plausible
 * (24-240 Hz), e.g. during the driver's bring-up.
 */
#define RV_EXT1_PPLL_REF_DIV  0x0404
#define RV_EXT1_PPLL_FB_DIV   0x0430
#define RV_EXT1_PPLL_POST_DIV 0x043c
#define RV_REF_CLOCK_KHZ      27000

static int64_t rv530_crtc_period_ns(RV530State *s)
{
    uint32_t fb = s->regs[RV_EXT1_PPLL_FB_DIV >> 2];
    uint32_t ref = s->regs[RV_EXT1_PPLL_REF_DIV >> 2] & 0x3ff;
    uint32_t post = s->regs[RV_EXT1_PPLL_POST_DIV >> 2] & 0x7f;
    uint64_t ht = (s->regs[RV_D1CRTC_H_TOTAL >> 2] & 0x1fff) + 1;
    uint64_t vt = (s->regs[RV_D1CRTC_V_TOTAL >> 2] & 0x1fff) + 1;
    /* pixel clock in units of 100 Hz: 27 MHz x (fb + frac / 10) */
    uint64_t clk = ref && post ?
        (uint64_t)RV_REF_CLOCK_KHZ * (((fb >> 16) & 0x7ff) * 10 + (fb & 0xf)) /
        (ref * post) : 0;
    int64_t period;

    if (!clk || ht < 64 || vt < 64) {
        return NANOSECONDS_PER_SECOND / FRAME_HZ;
    }
    period = ht * vt * 10000000 / clk;    /* clk in 100 Hz units -> ns */
    if (period < NANOSECONDS_PER_SECOND / 240 || period > NANOSECONDS_PER_SECOND / 24) {
        return NANOSECONDS_PER_SECOND / FRAME_HZ;
    }
    return period;
}

static void rv530_vblank_timer(void *opaque)
{
    RV530State *s = opaque;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    int i;

    s->frame_count++;
    s->frame_start_ns = now;
    /* a new mode takes effect at the frame boundary */
    s->frame_period_ns = rv530_crtc_period_ns(s);
    for (i = 0; i < RV_NUM_CRTC; i++) {
        RV530Crtc *c = &s->crtc[i];
        if (CREG(c, RV_D1CRTC_CONTROL) & RV_CRTC_EN) {
            c->vblank_occurred = true;
        }
    }
    rv530_update_irq(s);
    timer_mod(&s->vblank_timer, now + s->frame_period_ns);
}

/* ------------------------------------------------------------------ */
/* DDC over GPIO                                                       */
/* ------------------------------------------------------------------ */

static void rv530_ddc_update(RV530State *s, RV530Ddc *d)
{
    uint32_t a = s->regs[(d->base + 4) >> 2];
    uint32_t en = s->regs[(d->base + 8) >> 2];
    /* a pin driven as output low pulls the line low; otherwise it floats
     * high (open drain with pull-ups) */
    int scl = ((en & 1) && !(a & 1)) ? 0 : 1;
    int sda = ((en & 0x100) && !(a & 0x100)) ? 0 : 1;
    int sda_in = 1;

    if (d->has_ddc) {
        bitbang_i2c_set(&d->bbi2c, BITBANG_I2C_SCL, scl);
        sda_in = bitbang_i2c_set(&d->bbi2c, BITBANG_I2C_SDA, sda);
    }
    d->y = (scl ? 1 : 0) | (sda_in ? 0x100 : 0);
}

/* ------------------------------------------------------------------ */
/* Scanout                                                             */
/* ------------------------------------------------------------------ */

static int rv530_crtc_lut_index(RV530Crtc *c)
{
    return (CREG(c, RV_D1GRPH_LUT_SEL) & RV_D1GRPH_LUT_SEL_B) ? 1 : 0;
}

static bool rv530_scanout_mode(RV530Crtc *c, RV530Scanout *m)
{
    uint32_t ctl = CREG(c, RV_D1GRPH_CONTROL);
    uint32_t depth = ctl & 3;
    uint32_t xs = CREG(c, RV_D1GRPH_X_START);
    uint32_t xe = CREG(c, RV_D1GRPH_X_END);
    uint32_t ys = CREG(c, RV_D1GRPH_Y_START);
    uint32_t ye = CREG(c, RV_D1GRPH_Y_END);
    uint32_t vp = CREG(c, RV_D1MODE_VIEWPORT_SIZE);

    memset(m, 0, sizeof(*m));
    if (!(CREG(c, RV_D1CRTC_CONTROL) & RV_CRTC_EN) ||
        (CREG(c, RV_D1CRTC_CONTROL) & RV_CRTC_DISP_READ_REQUEST_DISABLE) ||
        !(CREG(c, RV_D1GRPH_ENABLE) & 1)) {
        return false;
    }
    m->enabled = true;
    m->format = ctl;
    m->bpp = depth == 0 ? 1 : depth == 1 ? 2 : depth == 2 ? 4 : 8;
    m->width = xe > xs ? xe - xs : 0;
    m->height = ye > ys ? ye - ys : 0;
    if (vp) {
        uint32_t vw = (vp >> 16) & 0x3fff, vh = vp & 0x3fff;
        if (vw && vw < m->width) {
            m->width = vw;
        }
        if (vh && vh < m->height) {
            m->height = vh;
        }
    }
    if (!m->width || !m->height || m->width > 4096 || m->height > 4096) {
        return false;
    }
    m->pitch = (CREG(c, RV_D1GRPH_PITCH) & 0x3fff) * m->bpp;
    if (!m->pitch) {
        m->pitch = m->width * m->bpp;
    }
    /* The CRTC shows the viewport (VIEWPORT_START, used by Linux to place
     * each head inside a shared framebuffer) of the surface window that
     * starts at SURFACE_OFFSET + X/Y_START. */
    m->x0 = CREG(c, RV_D1GRPH_SURFACE_OFFSET_X) + xs +
            ((CREG(c, RV_D1MODE_VIEWPORT_START) >> 16) & 0x3fff);
    m->y0 = CREG(c, RV_D1GRPH_SURFACE_OFFSET_Y) + ys +
            (CREG(c, RV_D1MODE_VIEWPORT_START) & 0x3fff);
    m->addr = CREG(c, RV_D1GRPH_PRIMARY_SURFACE_ADDRESS);
    m->tiled = !!(ctl & (RV_D1GRPH_TILED | RV_D1GRPH_MACRO)) &&
               !rv530_linear_surface(c->s, m->addr);
    if (!m->tiled) {
        m->addr += m->y0 * m->pitch + m->x0 * m->bpp;
        m->x0 = m->y0 = 0;
    }
    /*
     * The DC LUT (gamma) applies to every direct-color format, not only to
     * 8 bpp indexed ones.  An identity LUT lets linear ARGB8888 scan out
     * zero-copy; anything else (ColorSync calibration, a game's hardware
     * gamma, fades) goes through a converted shadow.
     */
    {
        bool direct8888 = m->bpp == 4 && ((ctl >> 8) & 7) == 0 && !m->tiled;
        bool identity = !c->s->lut_nonid[rv530_crtc_lut_index(c)];
        m->zero_copy = direct8888 && identity;
        m->lut_direct = direct8888 && !identity;
    }
    return true;
}

static const uint32_t *rv530_crtc_lut(RV530Crtc *c)
{
    return c->s->lut[rv530_crtc_lut_index(c)];
}

/* gamma: each channel through its own column of the LUT (0x00RRGGBB) */
static inline uint32_t rv530_lut_apply(const uint32_t *lut, uint32_t pix)
{
    return (lut[(pix >> 16) & 0xff] & 0xff0000) | (lut[(pix >> 8) & 0xff] & 0xff00) |
           (lut[pix & 0xff] & 0xff);
}

static void rv530_convert_line(RV530Crtc *c, const RV530Scanout *m,
                               uint32_t y, uint32_t *dst)
{
    RV530State *s = c->s;
    const uint32_t *lut = rv530_crtc_lut(c);
    uint32_t x;
    uint32_t fmt = (m->format >> 8) & 7;

    for (x = 0; x < m->width; x++) {
        uint32_t off;
        uint8_t *p;
        uint32_t pix = 0;

        if (m->tiled) {
            R5xxSurfaceLayout l = {
                .gpu_addr = m->addr, .pitch_bytes = m->pitch,
                .width = m->pitch / m->bpp, .height = m->y0 + m->height,
                .bpp = m->bpp,
                .tiling = (m->format & RV_D1GRPH_MACRO) ?
                          ((m->format & RV_D1GRPH_TILED) ? R5XX_TILE_MICRO_MACRO
                                                         : R5XX_TILE_MACRO)
                          : R5XX_TILE_MICRO,
                .micro_mode = 1,
                .is_r500 = true,
            };
            off = r5xx_surface_pixel_offset(&l, m->x0 + x, m->y0 + y);
        } else {
            off = y * m->pitch + x * m->bpp;
        }
        p = rv530_gpu_map(s, m->addr + off, m->bpp, false);
        if (!p) {
            dst[x] = 0;
            continue;
        }
        switch (m->bpp) {
        case 1:
            pix = lut[p[0]];
            break;
        case 2: {
            uint16_t v = lduw_le_p(p);
            if (fmt == 1) {          /* RGB565 */
                pix = rgb_to_pixel32(((v >> 11) & 0x1f) << 3,
                                     ((v >> 5) & 0x3f) << 2, (v & 0x1f) << 3);
            } else if (fmt == 2) {   /* ARGB4444 */
                pix = rgb_to_pixel32(((v >> 8) & 0xf) * 17,
                                     ((v >> 4) & 0xf) * 17, (v & 0xf) * 17);
            } else {                 /* ARGB1555 */
                pix = rgb_to_pixel32(((v >> 10) & 0x1f) << 3,
                                     ((v >> 5) & 0x1f) << 3, (v & 0x1f) << 3);
            }
            break;
        }
        case 4: {
            uint32_t v = ldl_le_p(p);
            if (fmt == 1) {          /* ARGB2101010 */
                pix = rgb_to_pixel32((v >> 22) & 0xff, (v >> 12) & 0xff,
                                     (v >> 2) & 0xff);
            } else {
                pix = v;
            }
            break;
        }
        case 8: {                    /* ARGB16161616 */
            pix = rgb_to_pixel32(p[5], p[3], p[1]);
            break;
        }
        }
        if (m->bpp != 1 && s->lut_nonid[rv530_crtc_lut_index(c)]) {
            pix = rv530_lut_apply(lut, pix);
        }
        dst[x] = pix;
    }
}

/*
 * VRAM dirty tracking shared by both heads.  QEMU has one VGA dirty client
 * per region, and fbcon (and X with a spanning desktop) scans the same
 * buffer out on both CRTCs, so whichever head refreshes first would clear
 * the bits the other one needs.  Every refresh therefore moves all newly
 * dirty pages into each CRTC's private bitmap, and each head consumes only
 * its own.
 */
static void rv530_collect_dirty_untimed(RV530State *s);

/*
 * memory_region_snapshot_and_clear_dirty() ends in
 * memory_global_after_dirty_log_sync(), which under TCG is a run_on_cpu()
 * rendezvous with every vCPU.  From a vCPU's MMIO exit (a CP kick) that
 * deadlocks against another vCPU waiting in start_exclusive() (e.g.
 * cpu_exec_step_atomic) for this one to leave its exec region.  VRAM is
 * plain RAM: under TCG its dirty bits come from the TCG write path and the
 * snapshot resets the TLBs itself, so a vCPU caller skips the rendezvous.
 * KVM (and the main loop) keep the full path with its dirty-log sync.
 */
DirtyBitmapSnapshot *rv530_vram_snapshot_dirty(RV530State *s)
{
    if (tcg_enabled() && current_cpu) {
        return physical_memory_snapshot_and_clear_dirty(&s->vram, 0, s->vram_size,
                                                        DIRTY_MEMORY_VGA);
    }
    return memory_region_snapshot_and_clear_dirty(&s->vram, 0, s->vram_size,
                                                  DIRTY_MEMORY_VGA);
}

void rv530_collect_dirty(RV530State *s)
{
    int64_t t0 = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);

    rv530_collect_dirty_untimed(s);
    s->prof_ns[RV_PROF_COLLECT] += qemu_clock_get_ns(QEMU_CLOCK_REALTIME) - t0;
    s->prof_n[RV_PROF_COLLECT]++;
}

static void rv530_collect_dirty_untimed(RV530State *s)
{
    DirtyBitmapSnapshot *snap;
    uint64_t npages = s->vram_size >> VRAM_PAGE_SHIFT;
    uint64_t first[RV_NUM_CRTC], last[RV_NUM_CRTC];
    bool mirror = r5xx_vk_mirrored(s->vk);
    uint64_t pg, chunk, run = 0, run_len = 0;
    uint64_t *check;
    int i;

    /* RV530_VK_CHECK_MIRROR: pages that differ from the mirror's
     * copy before the snapshot and are not dirty in it were written
     * without the dirty log seeing it */
    check = mirror ? r5xx_vk_check_scan(s->vk) : NULL;
    snap = rv530_vram_snapshot_dirty(s);
    if (check) {
        for (pg = 0; pg < npages; pg++) {
            if (!(check[pg >> 6] & (1ull << (pg & 63)))) {
                if (!check[pg >> 6]) {
                    pg |= 63;
                }
                continue;
            }
            if (!memory_region_snapshot_get_dirty(&s->vram, snap, pg << VRAM_PAGE_SHIFT,
                                                  1u << VRAM_PAGE_SHIFT)) {
                r5xx_vk_check_missed(s->vk, pg);
            }
        }
    }
    for (i = 0; i < RV_NUM_CRTC; i++) {
        const RV530Scanout *m = &s->crtc[i].scanout;

        first[i] = 1;
        last[i] = 0;
        if (m->enabled && m->zero_copy && m->addr >= s->fb_start) {
            first[i] = (m->addr - s->fb_start) >> VRAM_PAGE_SHIFT;
            last[i] = ((uint64_t)m->addr - s->fb_start +
                       (uint64_t)m->pitch * m->height - 1) >> VRAM_PAGE_SHIFT;
        }
    }
    /* 64-page chunks first: most of VRAM is clean on any given call */
    for (chunk = 0; chunk < npages; chunk += 64) {
        uint64_t end = MIN(chunk + 64, npages);

        if (!memory_region_snapshot_get_dirty(&s->vram, snap,
                                              chunk << VRAM_PAGE_SHIFT,
                                              (end - chunk) << VRAM_PAGE_SHIFT)) {
            continue;
        }
        for (pg = chunk; pg < end; pg++) {
            if (!memory_region_snapshot_get_dirty(&s->vram, snap,
                                                  pg << VRAM_PAGE_SHIFT,
                                                  1u << VRAM_PAGE_SHIFT)) {
                continue;
            }
            for (i = 0; i < RV_NUM_CRTC; i++) {
                if (pg >= first[i] && pg <= last[i]) {
                    set_bit(pg, s->crtc[i].dirty);
                }
            }
            if (mirror) {
                if (run_len && run + run_len == pg) {
                    run_len++;
                } else {
                    if (run_len) {
                        r5xx_vk_upload(s->vk, run << VRAM_PAGE_SHIFT,
                                       run_len << VRAM_PAGE_SHIFT);
                    }
                    run = pg;
                    run_len = 1;
                }
            }
        }
    }
    if (run_len) {
        r5xx_vk_upload(s->vk, run << VRAM_PAGE_SHIFT, run_len << VRAM_PAGE_SHIFT);
    }
    g_free(snap);
}

/*
 * The GPU wrote [off, off+len) and it is now back in host VRAM: tell the
 * heads directly.  Going through the VGA dirty log would make the next
 * collection upload those pages to the GPU mirror again for nothing.
 */
void rv530_avivo_mark_dirty(RV530State *s, uint64_t off, uint64_t len)
{
    uint64_t pg, end;
    int i;

    if (!len || off >= s->vram_size) {
        return;
    }
    end = MIN(off + len, s->vram_size);
    for (i = 0; i < RV_NUM_CRTC; i++) {
        if (!s->crtc[i].dirty) {
            continue;
        }
        for (pg = off >> VRAM_PAGE_SHIFT; pg <= (end - 1) >> VRAM_PAGE_SHIFT; pg++) {
            set_bit(pg, s->crtc[i].dirty);
        }
    }
}

static bool rv530_crtc_line_dirty(RV530Crtc *c, ram_addr_t off, uint32_t len)
{
    unsigned long first = off >> VRAM_PAGE_SHIFT;
    unsigned long last = (off + len - 1) >> VRAM_PAGE_SHIFT;

    return find_next_bit(c->dirty, last + 1, first) <= last;
}

static bool rv530_gfx_update_crtc(void *opaque);

static bool rv530_gfx_update(void *opaque)
{
    RV530Crtc *c = opaque;
    RV530Stall st;
    bool ret;

    if (c->s->cp_unlocked) {
        /* a CP run has released the BQL mid-batch: try again next frame */
        return true;
    }
    rv530_stall_start(c->s, &st);
    if (c->index == 0 && r5xx_vk_mirrored(c->s->vk) && !r5xx_vk_pending(c->s->vk)) {
        /* keep the GPU's VRAM copy current even while nothing is scanned
         * out zero-copy (boot): see r5xx_vk_upload_kick */
        rv530_collect_dirty(c->s);
        r5xx_vk_upload_kick(c->s->vk);
    }
    ret = rv530_gfx_update_crtc(opaque);
    rv530_stall_end(c->s, &st, "display refresh", 0);
    return ret;
}

static bool rv530_gfx_update_crtc(void *opaque)
{
    RV530Crtc *c = opaque;
    RV530State *s = c->s;
    RV530Scanout m;
    bool mode_changed;

    if (!rv530_scanout_mode(c, &m)) {
        if (c->scanout.enabled) {
            memset(&c->scanout, 0, sizeof(c->scanout));
            qemu_console_resize(c->con, 640, 480);
            qemu_console_update_full(c->con);
        }
        return true;
    }
    mode_changed = memcmp(&m, &c->scanout, sizeof(m)) != 0;
    if (mode_changed) {
        c->scanout = m;
        trace_rv530_scanout_mode(c->index, m.width, m.height, m.bpp, m.pitch,
                                 m.addr, m.tiled, m.zero_copy);
    }

    if (m.zero_copy) {
        uint32_t len = m.pitch * (m.height - 1) + m.width * m.bpp;
        uint8_t *base = rv530_gpu_map(s, m.addr, len, false);

        if (base && base >= s->vram_ptr &&
            base + len <= s->vram_ptr + s->vram_size) {
            ram_addr_t off = base - s->vram_ptr;
            DisplaySurface *cur = qemu_console_surface(c->con);

            rv530_collect_dirty(s);
            r5xx_vk_upload_kick(s->vk);
            if (mode_changed || !cur || surface_data(cur) != base) {
                DisplaySurface *ds = qemu_create_displaysurface_from(
                    m.width, m.height, PIXMAN_x8r8g8b8, m.pitch, base);
                qemu_console_set_surface(c->con, ds);
                qemu_console_update_full(c->con);
            } else {
                int y, ys = -1;
                for (y = 0; y < (int)m.height; y++) {
                    bool dirty = rv530_crtc_line_dirty(c, off + m.pitch * y,
                                                       m.width * 4);
                    if (dirty && ys < 0) {
                        ys = y;
                    }
                    if (!dirty && ys >= 0) {
                        qemu_console_update(c->con, 0, ys, m.width, y - ys);
                        ys = -1;
                    }
                }
                if (ys >= 0) {
                    qemu_console_update(c->con, 0, ys, m.width, y - ys);
                }
            }
            bitmap_clear(c->dirty, off >> VRAM_PAGE_SHIFT,
                         ((off + len - 1) >> VRAM_PAGE_SHIFT) -
                         (off >> VRAM_PAGE_SHIFT) + 1);
            return true;
        }
    }

    if (m.lut_direct) {
        uint32_t len = m.pitch * (m.height - 1) + m.width * m.bpp;
        uint8_t *base = rv530_gpu_map(s, m.addr, len, false);

        if (base && base >= s->vram_ptr &&
            base + len <= s->vram_ptr + s->vram_size) {
            ram_addr_t off = base - s->vram_ptr;
            const uint32_t *lut = rv530_crtc_lut(c);
            size_t need = (size_t)m.width * m.height * 4;
            bool full = mode_changed || c->lut_gen_seen != s->lut_gen;
            int y, ys = -1;

            rv530_collect_dirty(s);
            r5xx_vk_upload_kick(s->vk);
            if (c->shadow_size != need || !qemu_console_surface(c->con) ||
                surface_data(qemu_console_surface(c->con)) != c->shadow) {
                uint8_t *old = c->shadow;
                c->shadow = g_malloc0(need);
                c->shadow_size = need;
                qemu_console_set_surface(c->con, qemu_create_displaysurface_from(
                    m.width, m.height, PIXMAN_x8r8g8b8, m.width * 4, c->shadow));
                g_free(old);
                full = true;
            }
            c->lut_gen_seen = s->lut_gen;
            for (y = 0; y < (int)m.height; y++) {
                bool dirty = full || rv530_crtc_line_dirty(c, off + m.pitch * y,
                                                           m.width * 4);
                if (dirty) {
                    const uint32_t *src = (const uint32_t *)(base + m.pitch * y);
                    uint32_t *dst = (uint32_t *)(c->shadow + (size_t)y * m.width * 4);
                    uint32_t x;
                    for (x = 0; x < m.width; x++) {
                        dst[x] = rv530_lut_apply(lut, le32_to_cpu(src[x]));
                    }
                    if (ys < 0) {
                        ys = y;
                    }
                } else if (ys >= 0) {
                    qemu_console_update(c->con, 0, ys, m.width, y - ys);
                    ys = -1;
                }
            }
            if (ys >= 0) {
                qemu_console_update(c->con, 0, ys, m.width, y - ys);
            }
            bitmap_clear(c->dirty, off >> VRAM_PAGE_SHIFT,
                         ((off + len - 1) >> VRAM_PAGE_SHIFT) -
                         (off >> VRAM_PAGE_SHIFT) + 1);
            return true;
        }
    }

    /* conversion path (8/16/64 bpp, 2101010, tiled, or not in VRAM) */
    {
        size_t need = (size_t)m.width * m.height * 4;
        uint32_t y;
        if (c->shadow_size != need || mode_changed ||
            !qemu_console_surface(c->con) ||
            surface_data(qemu_console_surface(c->con)) != c->shadow) {
            DisplaySurface *ds;
            /* the old surface may still reference the old shadow buffer */
            uint8_t *old = c->shadow;
            c->shadow = g_malloc0(need);
            c->shadow_size = need;
            ds = qemu_create_displaysurface_from(
                m.width, m.height, PIXMAN_x8r8g8b8, m.width * 4, c->shadow);
            qemu_console_set_surface(c->con, ds);
            g_free(old);
        }
        for (y = 0; y < m.height; y++) {
            rv530_convert_line(c, &m, y, (uint32_t *)(c->shadow + y * m.width * 4));
        }
        qemu_console_update_full(c->con);
    }
    return true;
}

/*
 * Window-size following.  Mac OS X has no resize request for an ATI
 * card, so the device swaps the monitor: a new EDID whose preferred mode
 * is the requested size (and whose serial makes it a different display to
 * WindowServer), HPD1 low, then high again.  The driver re-reads the EDID
 * on the hot-plug interrupts and the new mode becomes available.
 */
static void rv530_hpd_replug(void *opaque)
{
    RV530State *s = opaque;

    s->hpd1_unplugged = false;
    s->hpd1_pending = true;
    rv530_update_irq(s);
}

void rv530_request_mode(RV530State *s, uint32_t w, uint32_t h)
{
    RV530Ddc *d = &s->ddc[0];

    w = MIN(MAX(w, 640), MAX(s->edid_info.maxx, 640)) & ~7u;
    h = MIN(MAX(h, 480), MAX(s->edid_info.maxy, 480)) & ~1u;
    if (!d->has_ddc || (w == s->edid_info.prefx && h == s->edid_info.prefy)) {
        return;
    }
    /* only once the guest driver listens for hot plug (not in firmware) */
    if (!(s->regs[RV_DC_HOT_PLUG_DETECT1_INT_CONTROL >> 2] & RV_HPD_INT_EN)) {
        return;
    }
    trace_rv530_request_mode(w, h);
    s->edid_info.prefx = w;
    s->edid_info.prefy = h;
    rv530_edid_build(d->ddc.blob, w, h, s->edid_info.maxx, s->edid_info.maxy,
                     w * 10000 + h, s->refresh);
    s->hpd1_unplugged = true;
    s->hpd1_pending = true;
    rv530_update_irq(s);
    timer_mod(&s->hpd_timer, qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 300);
}

static void rv530_resize_timer(void *opaque)
{
    RV530State *s = opaque;

    rv530_request_mode(s, s->resize_w, s->resize_h);
}

static void rv530_ui_info(void *opaque, uint32_t head, QemuUIInfo *info)
{
    RV530Crtc *c = opaque;
    RV530State *s = c->s;

    trace_rv530_ui_info(head, info->width, info->height, s->resize);
    if (head != 0 || !info->width || !info->height) {
        return;
    }
    /*
     * Remembered while auto-resize is off too, for rv530_set_auto_resize;
     * but not the sizes a window reports before it is laid out (smaller
     * than the smallest mode) or while the firmware still drives the
     * display: re-enabling auto-resize applied such a size, 640x480.
     */
    if (info->width < 640 || info->height < 480 ||
        !(s->regs[RV_DC_HOT_PLUG_DETECT1_INT_CONTROL >> 2] & RV_HPD_INT_EN)) {
        return;
    }
    s->resize_w = info->width;
    s->resize_h = info->height;
    if (s->resize) {
        /* a window drag sends many sizes: act on the last one */
        timer_mod(&s->resize_timer, qemu_clock_get_ms(QEMU_CLOCK_REALTIME) + 500);
    }
}

/*
 * Switch window-size following at run time: the "auto-resize" QOM
 * property, set from the GTK View menu or with qom-set.  Turning it on
 * applies the window's last reported size at once; with none known (the
 * window has not changed since the guest driver started), the mode stays.
 */
void rv530_set_auto_resize(RV530State *s, bool on)
{
    s->resize = on;
    if (!on) {
        timer_del(&s->resize_timer);
    } else if (s->resize_w && s->resize_h) {
        timer_mod(&s->resize_timer, qemu_clock_get_ms(QEMU_CLOCK_REALTIME) + 100);
    }
}

static const GraphicHwOps rv530_gfx_ops = {
    .gfx_update = rv530_gfx_update,
    .ui_info = rv530_ui_info,
};

/* ------------------------------------------------------------------ */
/* Cursor                                                              */
/* ------------------------------------------------------------------ */

/* current mode size from the CRTC registers (the cached c->scanout is only
 * filled by display refreshes, which do not run without a UI client) */

static int m_width(RV530Crtc *c)
{
    RV530Scanout m;
    return rv530_scanout_mode(c, &m) ? (int)m.width : 0;
}

static int m_height(RV530Crtc *c)
{
    RV530Scanout m;
    return rv530_scanout_mode(c, &m) ? (int)m.height : 0;
}

static void rv530_cursor_update(RV530Crtc *c, bool shape)
{
    RV530State *s = c->s;
    uint32_t ctl = CREG(c, RV_D1CUR_CONTROL);
    uint32_t size = CREG(c, RV_D1CUR_SIZE);
    uint32_t pos = CREG(c, RV_D1CUR_POSITION);
    uint32_t hot = CREG(c, RV_D1CUR_HOT_SPOT);
    uint32_t addr = CREG(c, RV_D1CUR_SURFACE_ADDRESS);
    uint32_t w = ((size >> 16) & 0x7f) + 1, h = (size & 0x7f) + 1;
    uint32_t x, y;

    trace_rv530_cursor(c->index, ctl & RV_D1CURSOR_EN, (pos >> 16) & 0x3fff,
                       pos & 0x3fff, w, h, addr);
    if (!(ctl & RV_D1CURSOR_EN)) {
        if (c->cursor_visible) {
            c->cursor_visible = false;
            qemu_console_set_mouse(c->con, 0, 0, false);
        }
        return;
    }
    if (w > 64 || h > 64) {
        return;
    }
    if (shape || !c->cursor || c->cursor->width != w || c->cursor->height != h) {
        if (!c->cursor || c->cursor->width != w || c->cursor->height != h) {
            cursor_unref(c->cursor);
            c->cursor = cursor_alloc(w, h);
        }
        /* 64x64 ARGB8888 surface, 256 byte pitch, premultiplied alpha */
        for (y = 0; y < h; y++) {
            uint8_t *line = rv530_gpu_map(s, addr + y * 256, w * 4, false);
            for (x = 0; x < w; x++) {
                c->cursor->data[y * w + x] = line ? ldl_le_p(line + x * 4) : 0;
            }
        }
        c->cursor->hot_x = c->hot_x;
        c->cursor->hot_y = c->hot_y;
        qemu_console_set_cursor(c->con, c->cursor);
    }
    {
        /* top-left of the image on screen (hardware: POSITION - HOT_SPOT) */
        int left = (int)((pos >> 16) & 0x3fff) - (int)((hot >> 16) & 0x3f);
        int top = (int)(pos & 0x3fff) - (int)(hot & 0x3f);
        int ax, ay;

        /*
         * With an absolute pointing device the guest pointer is where QEMU
         * last put it, so pointer - top-left is the shape's hot spot.  Take
         * it once two updates agree (the host pointer may already have moved
         * on while the guest draws), and hand it to the UI, which anchors
         * the host cursor image at its hot spot.
         */
        ax = ay = -1;
        if (c->index == 0 && m_width(c) > 1 && qemu_input_get_last_abs(&ax, &ay)) {
            int px = (int)(((int64_t)ax * (m_width(c) - 1) + 16383) / 32767);
            int py = (int)(((int64_t)ay * (m_height(c) - 1) + 16383) / 32767);
            int cx = px - left, cy = py - top;

            if (cx >= 0 && cy >= 0 && cx < (int)w && cy < (int)h) {
                c->cand_n = (cx == c->cand_x && cy == c->cand_y) ? c->cand_n + 1 : 1;
                c->cand_x = cx;
                c->cand_y = cy;
                if (c->cand_n >= 2 && (cx != c->hot_x || cy != c->hot_y)) {
                    c->hot_x = c->cursor->hot_x = cx;
                    c->hot_y = c->cursor->hot_y = cy;
                    qemu_console_set_cursor(c->con, c->cursor);
                }
            }
        }
        trace_rv530_cursor_hot(c->index, c->hot_x, c->hot_y, left + c->hot_x,
                               top + c->hot_y, ax, ay, m_width(c), c->cand_n);
        qemu_console_set_mouse(c->con, left + c->hot_x, top + c->hot_y, true);
    }
    c->cursor_visible = true;
}

/* ------------------------------------------------------------------ */
/* Registers                                                           */
/* ------------------------------------------------------------------ */

static RV530Ddc *rv530_ddc_for(RV530State *s, uint32_t off)
{
    int i;
    for (i = 0; i < RV_NUM_DDC; i++) {
        if (off >= s->ddc[i].base && off < s->ddc[i].base + 0x10) {
            return &s->ddc[i];
        }
    }
    return NULL;
}

/* registers in 0x6000-0x6fff that are global, not per CRTC */
static bool rv530_is_global_dc(uint32_t off)
{
    return (off >= RV_DC_LUT_RW_SELECT && off <= RV_DC_LUT_AUTOFILL) ||
           off == RV_DxMODE_INT_MASK || off == RV_DC_CRTC_MASTER_EN ||
           off == 0x6520 /* DC_LB_MEMORY_SPLIT */;
}

static RV530Crtc *rv530_crtc_for(RV530State *s, uint32_t off, uint32_t *rel)
{
    RV530Crtc *c;

    if (off < 0x6000 || off >= 0x7000 || rv530_is_global_dc(off)) {
        return NULL;
    }
    c = &s->crtc[(off - 0x6000) / RV_CRTC_STRIDE];
    *rel = off - c->base;
    return c;
}

bool rv530_avivo_reg_read(RV530State *s, uint32_t off, uint32_t *val)
{
    RV530Ddc *d;
    RV530Crtc *c;
    uint32_t rel;

    switch (off) {
    case RV_DISP_INTERRUPT_STATUS: {
        uint32_t v = 0, mask = s->regs[RV_DxMODE_INT_MASK >> 2];
        if (s->crtc[0].vblank_occurred && (mask & RV_D1MODE_VBLANK_INT_MASK)) {
            v |= RV_DISP_LB_D1_VBLANK_INT;
        }
        if (s->crtc[1].vblank_occurred && (mask & RV_D2MODE_VBLANK_INT_MASK)) {
            v |= RV_DISP_LB_D2_VBLANK_INT;
        }
        if (s->hpd1_pending) {
            v |= RV_DISP_HPD1_INT;
        }
        *val = v;
        return true;
    }
    case RV_DC_HOT_PLUG_DETECT1_INT_STATUS:
        *val = (s->hpd1_unplugged ? 0 : RV_HPD_SENSE) |
               (s->hpd1_pending ? RV_HPD_INT_STATUS : 0);
        return true;
    case RV_DC_HOT_PLUG_DETECT2_INT_STATUS:
        *val = 0;
        return true;
    case RV_DC_LUT_AUTOFILL:
        *val = s->regs[off >> 2] | RV_DC_LUT_AUTOFILL_DONE;
        return true;
    case RV_DACA_COMPARATOR_OUTPUT:
    case RV_DACB_COMPARATOR_OUTPUT:
        *val = 0;                  /* nothing on the analog outputs */
        return true;
    default:
        break;
    }

    c = rv530_crtc_for(s, off, &rel);
    if (c) {
        switch (rel) {
        case RV_D1CRTC_CONTROL: {
            uint32_t v = CREG(c, RV_D1CRTC_CONTROL);
            v &= ~RV_D1CRTC_CURRENT_MASTER_EN_STATE;
            if (v & RV_CRTC_EN) {
                v |= RV_D1CRTC_CURRENT_MASTER_EN_STATE;
            }
            *val = v;
            return true;
        }
        case RV_D1CRTC_STATUS:
            *val = rv530_crtc_in_vblank(c) ? RV_D1CRTC_V_BLANK : 0;
            return true;
        case RV_D1CRTC_STATUS_POSITION:
        case RV_D1CRTC_STATUS_HV_COUNT:
            /* line in bits 0-12, pixel in bits 16-28 (radeon_display.c) */
            *val = rv530_crtc_vcount(c) & 0x1fff;
            return true;
        case RV_D1CRTC_FRAME_COUNT:
            *val = (CREG(c, RV_D1CRTC_CONTROL) & RV_CRTC_EN) ?
                   s->frame_count & 0xffffff : 0;
            return true;
        case RV_D1MODE_VBLANK_STATUS: {
            uint32_t v = 0;
            uint32_t bit = c->index ? RV_D2MODE_VBLANK_INT_MASK
                                    : RV_D1MODE_VBLANK_INT_MASK;
            if (c->vblank_occurred) {
                v |= RV_VBLANK_OCCURRED;
                if (s->regs[RV_DxMODE_INT_MASK >> 2] & bit) {
                    v |= RV_VBLANK_INTERRUPT;
                }
            }
            if (rv530_crtc_in_vblank(c)) {
                v |= RV_VBLANK_STAT;
            }
            *val = v;
            return true;
        }
        case RV_D1GRPH_UPDATE:
            /* updates are applied instantly: never pending */
            *val = CREG(c, RV_D1GRPH_UPDATE) &
                   ~(RV_D1GRPH_SURFACE_UPDATE_PENDING | 1u);
            return true;
        default:
            break;
        }
    }

    d = rv530_ddc_for(s, off);
    if (d && (off & 0xc) == 0xc) {
        *val = d->y;
        return true;
    }
    return false;
}

bool rv530_avivo_reg_write(RV530State *s, uint32_t off, uint32_t val)
{
    uint32_t idx = off >> 2;
    RV530Ddc *d;
    RV530Crtc *c;
    uint32_t rel;

    switch (off) {
    case RV_DxMODE_INT_MASK:
        s->regs[idx] = val;
        rv530_update_irq(s);
        return true;
    case RV_DC_HOT_PLUG_DETECT1_INT_CONTROL:
        s->regs[idx] = val & ~RV_HPD_INT_ACK;
        if (val & RV_HPD_INT_ACK) {
            s->hpd1_pending = false;
        }
        rv530_update_irq(s);
        return true;
    case RV_DC_LUT_AUTOFILL:
        s->regs[idx] = val;
        if (val & 1) {               /* AUTOFILL: load the identity ramp */
            uint32_t sel = s->regs[RV_DC_LUT_RW_SELECT >> 2] & 1, i;
            for (i = 0; i < 256; i++) {
                s->lut[sel][i] = rgb_to_pixel32(i, i, i);
            }
            s->lut_nonid[sel] = 0;
            s->lut_gen++;
        }
        return true;
    case RV_DC_LUT_RW_SELECT:
    case RV_DC_LUT_RW_MODE:
    case RV_DC_LUT_WRITE_EN_MASK:
        s->regs[idx] = val;
        return true;
    case RV_DC_LUT_RW_INDEX:
        s->regs[idx] = val & 0xff;
        return true;
    case RV_DC_LUT_30_COLOR: {
        uint32_t sel = s->regs[RV_DC_LUT_RW_SELECT >> 2] & 1;
        uint32_t i = s->regs[RV_DC_LUT_RW_INDEX >> 2] & 0xff;
        uint32_t ident = rgb_to_pixel32(i, i, i);
        uint32_t v = rgb_to_pixel32((val >> 22) & 0xff, (val >> 12) & 0xff,
                                    (val >> 2) & 0xff);
        if (v != s->lut[sel][i]) {
            s->lut_nonid[sel] += (v != ident) - (s->lut[sel][i] != ident);
            s->lut[sel][i] = v;
            s->lut_gen++;
        }
        s->regs[RV_DC_LUT_RW_INDEX >> 2] = (i + 1) & 0xff;
        return true;
    }
    default:
        break;
    }

    c = rv530_crtc_for(s, off, &rel);
    if (c) {
        switch (rel) {
        case RV_D1MODE_VBLANK_STATUS:
            if (val & RV_VBLANK_ACK) {
                c->vblank_occurred = false;
                rv530_update_irq(s);
            }
            return true;
        case RV_D1CRTC_CONTROL:
            s->regs[idx] = val & ~RV_D1CRTC_CURRENT_MASTER_EN_STATE;
            if (!(val & RV_CRTC_EN)) {
                c->vblank_occurred = false;
                rv530_update_irq(s);
            }
            trace_rv530_avivo_write(off, r5xx_regname(off) ?: "?", val);
            return true;
        case RV_D1CRTC_V_TOTAL:
        case RV_D1CRTC_V_BLANK_START_END:
        case RV_D1CRTC_H_TOTAL:
        case RV_D1CRTC_H_BLANK_START_END:
        case RV_D1GRPH_ENABLE:
        case RV_D1GRPH_CONTROL:
        case RV_D1GRPH_PRIMARY_SURFACE_ADDRESS:
        case RV_D1GRPH_SECONDARY_SURFACE_ADDRESS:
        case RV_D1GRPH_PITCH:
        case RV_D1GRPH_SURFACE_OFFSET_X:
        case RV_D1GRPH_SURFACE_OFFSET_Y:
        case RV_D1GRPH_X_START:
        case RV_D1GRPH_Y_START:
        case RV_D1GRPH_X_END:
        case RV_D1GRPH_Y_END:
        case RV_D1GRPH_LUT_SEL:
        case RV_D1GRPH_FLIP_CONTROL:
        case RV_D1MODE_VIEWPORT_START:
        case RV_D1MODE_VIEWPORT_SIZE:
        case RV_D1MODE_DESKTOP_HEIGHT:
        case RV_D1MODE_DATA_FORMAT:
        case RV_D1CRTC_BLANK_CONTROL:
        case RV_D1CRTC_UPDATE_LOCK:
        case RV_D1MODE_MASTER_UPDATE_LOCK:
        case RV_D1MODE_MASTER_UPDATE_MODE:
        case RV_D1GRPH_UPDATE:
            s->regs[idx] = val;
            trace_rv530_avivo_write(off, r5xx_regname(off) ?: "?", val);
            return true;
        case RV_D1CUR_SURFACE_ADDRESS:
        case RV_D1CUR_SIZE:
        case RV_D1CUR_HOT_SPOT:
        case RV_D1CUR_CONTROL:
            s->regs[idx] = val;
            rv530_cursor_update(c, true);
            return true;
        case RV_D1CUR_POSITION:
            s->regs[idx] = val;
            rv530_cursor_update(c, false);
            return true;
        case RV_D1CUR_UPDATE:
            s->regs[idx] = val;
            if (!(val & (1u << 16))) {         /* unlock: latch the shape */
                rv530_cursor_update(c, true);
            }
            return true;
        default:
            break;
        }
    }

    d = rv530_ddc_for(s, off);
    if (d) {
        if ((off & 0xc) == 0xc) {
            return true;           /* Y is read only */
        }
        s->regs[idx] = val;
        if ((off & 0xc) == 0x4 || (off & 0xc) == 0x8) {
            rv530_ddc_update(s, d);
        }
        return true;
    }
    return false;
}

/* ------------------------------------------------------------------ */
/* Init / reset                                                        */
/* ------------------------------------------------------------------ */

void rv530_avivo_reset(RV530State *s)
{
    uint32_t i, j;

    s->frame_count = 0;
    s->hpd1_pending = false;
    s->hpd1_unplugged = false;
    s->frame_period_ns = NANOSECONDS_PER_SECOND / FRAME_HZ;
    s->frame_start_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    timer_mod(&s->vblank_timer, s->frame_start_ns + s->frame_period_ns);
    for (i = 0; i < RV_NUM_CRTC; i++) {
        RV530Crtc *c = &s->crtc[i];
        c->vblank_occurred = false;
        memset(&c->scanout, 0, sizeof(c->scanout));
        if (c->dirty) {
            bitmap_zero(c->dirty, s->vram_size >> VRAM_PAGE_SHIFT);
        }
    }
    for (j = 0; j < 2; j++) {
        for (i = 0; i < 256; i++) {
            s->lut[j][i] = rgb_to_pixel32(i, i, i);
        }
        s->lut_nonid[j] = 0;
    }
    s->lut_gen++;
    for (i = 0; i < RV_NUM_DDC; i++) {
        s->ddc[i].y = 0x101;
    }

    /* Leave the card "posted": CRTC1 enabled with a linear 32bpp
     * surface at VRAM offset 0, as a firmware GOP driver would.  D2 off. */
    s->regs[RV_D1CRTC_CONTROL >> 2] = RV_CRTC_EN;
    s->regs[RV_D1GRPH_ENABLE >> 2] = 1;
    s->regs[RV_D1GRPH_CONTROL >> 2] = 2;              /* 32bpp ARGB8888 */
    s->regs[RV_D1GRPH_PRIMARY_SURFACE_ADDRESS >> 2] = 0;
    s->regs[RV_D1GRPH_SECONDARY_SURFACE_ADDRESS >> 2] = 0;
    s->regs[RV_D1GRPH_PITCH >> 2] = s->xres;
    s->regs[RV_D1GRPH_X_END >> 2] = s->xres;
    s->regs[RV_D1GRPH_Y_END >> 2] = s->yres;
    s->regs[RV_D1MODE_VIEWPORT_SIZE >> 2] = (s->xres << 16) | s->yres;
    s->regs[RV_D1MODE_DESKTOP_HEIGHT >> 2] = s->yres;
    s->regs[RV_D1CRTC_H_TOTAL >> 2] = s->xres + 320 - 1;
    s->regs[RV_D1CRTC_V_TOTAL >> 2] = s->yres + 38 - 1;
    s->regs[RV_D1CRTC_H_BLANK_START_END >> 2] = (s->xres + 296) << 16 | 160;
    s->regs[RV_D1CRTC_V_BLANK_START_END >> 2] = (s->yres + 33) << 16 | 29;
    s->regs[RV_DC_CRTC_MASTER_EN >> 2] = 1;
    s->regs[RV_TMDSA_CNTL >> 2] = 1;
    s->regs[RV_DACA_ENABLE >> 2] = 0;
    s->regs[RV_VGA_RENDER_CONTROL >> 2] = 0;
    s->regs[RV_D1VGA_CONTROL >> 2] = 0;
    s->regs[RV_D1MODE_MASTER_UPDATE_MODE >> 2] = 3;
    s->regs[(RV_D1MODE_MASTER_UPDATE_MODE + RV_CRTC_STRIDE) >> 2] = 3;
    s->regs[RV_DC_HOT_PLUG_DETECT1_CONTROL >> 2] = 1;
}

void rv530_avivo_init(RV530State *s, Error **errp)
{
    int i;
    static const uint32_t bases[RV_NUM_DDC] = {
        RV_DC_GPIO_DDC1_MASK, RV_DC_GPIO_DDC2_MASK, RV_DC_GPIO_DDC3_MASK
    };

    for (i = 0; i < RV_NUM_CRTC; i++) {
        RV530Crtc *c = &s->crtc[i];
        c->s = s;
        c->index = i;
        c->base = i * RV_CRTC_STRIDE;
        c->con = qemu_graphic_console_create(DEVICE(s), i, &rv530_gfx_ops, c);
        c->dirty = bitmap_new(s->vram_size >> VRAM_PAGE_SHIFT);
    }
    timer_init_ns(&s->vblank_timer, QEMU_CLOCK_VIRTUAL, rv530_vblank_timer, s);
    timer_init_ms(&s->resize_timer, QEMU_CLOCK_REALTIME, rv530_resize_timer, s);
    timer_init_ms(&s->hpd_timer, QEMU_CLOCK_VIRTUAL, rv530_hpd_replug, s);

    if (!s->edid_info.prefx || !s->edid_info.prefy) {
        s->edid_info.prefx = 1024;
        s->edid_info.prefy = 768;
    }
    s->xres = s->edid_info.prefx;
    s->yres = s->edid_info.prefy;
    if (!s->edid_info.maxx || !s->edid_info.maxy) {
        s->edid_info.maxx = 2560;
        s->edid_info.maxy = 1600;
    }

    for (i = 0; i < RV_NUM_DDC; i++) {
        RV530Ddc *d = &s->ddc[i];
        char name[32];
        d->base = bases[i];
        snprintf(name, sizeof(name), "rv530.ddc%d", i + 1);
        d->bus = i2c_init_bus(DEVICE(s), name);
        bitbang_i2c_init(&d->bbi2c, d->bus);
        /* EDID on DDC1 (the TMDS/DVI connector) only */
        if (i == 0) {
            rv530_edid_build(d->ddc.blob, s->edid_info.prefx, s->edid_info.prefy,
                             s->edid_info.maxx, s->edid_info.maxy, 0, s->refresh);
            i2c_slave_set_address(I2C_SLAVE(&d->ddc), 0x50);
            qdev_realize(DEVICE(&d->ddc), BUS(d->bus), &error_abort);
            d->has_ddc = true;
        } else {
            object_unparent(OBJECT(&d->ddc));
            d->has_ddc = false;
        }
    }
}

void rv530_avivo_fini(RV530State *s)
{
    int i;

    timer_del(&s->vblank_timer);
    timer_del(&s->resize_timer);
    timer_del(&s->hpd_timer);
    for (i = 0; i < RV_NUM_CRTC; i++) {
        RV530Crtc *c = &s->crtc[i];
        g_free(c->shadow);
        g_free(c->dirty);
        cursor_unref(c->cursor);
        if (c->con) {
            qemu_graphic_console_close(c->con);
        }
    }
}
