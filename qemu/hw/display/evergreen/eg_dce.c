/*
 * Evergreen DCE4 display engine: six CRTC/graphics pipes (two shown as
 * QEMU consoles), vblank and hot-plug interrupts through the IH ring, and
 * the DDC GPIO pairs with the shared rv530 EDID monitor.
 *
 * Linear 16/32 bpp scanout and the hardware cursor; no tiled scanout.
 * The encoders and transmitters (DIG, UNIPHY) are register state only.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qapi/error.h"
#include "ui/pixel_ops.h"
#include "ui/input.h"
#include "evergreen.h"
#include "eg_gfx.h"
#include "trace.h"

#define EG_NUM_CONSOLES 2
#define FRAME_NS        (NANOSECONDS_PER_SECOND / 60)

/* EVERGREEN_CRTCn_REGISTER_OFFSET */
static const uint32_t crtc_offset[EG_NUM_CRTC] = {
    0x0000, 0x0c00, 0x9800, 0xa400, 0xb000, 0xbc00,
};

/* DISP_INTERRUPT_STATUS(_CONTINUE..5): one register per CRTC/HPD pair */
static const uint32_t disp_int_status[6] = {
    0x60f4, 0x60f8, 0x60fc, 0x6100, 0x614c, 0x6150,
};
#define LB_VBLANK_INTERRUPT     (1u << 3)
#define DC_HPD_INTERRUPT        (1u << 17)
#define VBLANK_OCCURRED         (1u << 0)
#define VBLANK_ACK              (1u << 4)
#define VBLANK_INT_MASK         (1u << 0)
#define GRPH_SURFACE_UPDATE_PENDING (1u << 2)

static const uint32_t ddc_base[EG_NUM_DDC] = {
    0x6430, 0x6440, 0x6450, 0x6460, 0x6470, 0x6480, 0x6560, 0x64d8,
};

#define CREG(s, i, r) ((s)->regs[((r) + crtc_offset[i]) >> 2])

/* which CRTC (and register relative to CRTC0) an offset belongs to */
static int eg_crtc_for(uint32_t off, uint32_t *rel)
{
    int i;

    for (i = EG_NUM_CRTC - 1; i >= 0; i--) {
        uint32_t r = off - crtc_offset[i];
        if (off >= crtc_offset[i] && r >= 0x6800 && r < 0x7000) {
            *rel = r;
            return i;
        }
    }
    return -1;
}

static bool eg_crtc_enabled(EvergreenState *s, int i)
{
    return CREG(s, i, EG_CRTC_CONTROL) & EG_CRTC_MASTER_EN;
}

/* ------------------------------------------------------------------ */
/* Timing                                                              */
/* ------------------------------------------------------------------ */

static uint32_t eg_frame_pos(EvergreenState *s)
{
    return qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) % FRAME_NS;
}

static void eg_vblank_timer(void *opaque)
{
    EvergreenState *s = opaque;
    int i;

    for (i = 0; i < EG_NUM_CRTC; i++) {
        if (!eg_crtc_enabled(s, i)) {
            continue;
        }
        CREG(s, i, EG_CRTC_STATUS_FRAME_COUNT)++;
        CREG(s, i, EG_VBLANK_STATUS) |= VBLANK_OCCURRED;
        if (CREG(s, i, EG_INT_MASK) & VBLANK_INT_MASK) {
            s->regs[disp_int_status[i] >> 2] |= LB_VBLANK_INTERRUPT;
            eg_ih_push(s, EG_SRC_VBLANK_D1 + i, 0);
        }
    }
    timer_mod(&s->vblank_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + FRAME_NS);
}

/* ------------------------------------------------------------------ */
/* Registers                                                           */
/* ------------------------------------------------------------------ */

static void eg_ddc_update(EvergreenState *s, int n)
{
    uint32_t a = s->regs[(ddc_base[n] + 4) >> 2];
    uint32_t en = s->regs[(ddc_base[n] + 8) >> 2];
    /* a pin driven as output low pulls the line low; otherwise pulled up */
    int scl = ((en & 1) && !(a & 1)) ? 0 : 1;
    int sda = ((en & 0x100) && !(a & 0x100)) ? 0 : 1;
    int sda_in;

    bitbang_i2c_set(&s->ddc[n].bbi2c, BITBANG_I2C_SCL, scl);
    sda_in = bitbang_i2c_set(&s->ddc[n].bbi2c, BITBANG_I2C_SDA, sda);
    s->ddc[n].y = (scl ? 1 : 0) | (sda_in ? 0x100 : 0);
}

static int eg_ddc_for(uint32_t off)
{
    int n;

    for (n = 0; n < EG_NUM_DDC; n++) {
        if (off >= ddc_base[n] && off < ddc_base[n] + 0x10) {
            return n;
        }
    }
    return -1;
}

bool eg_dce_read(EvergreenState *s, uint32_t off, uint32_t *val)
{
    uint32_t rel;
    int i = eg_crtc_for(off, &rel), n;

    if (i >= 0) {
        uint32_t lines = 806, pos = eg_frame_pos(s) * (uint64_t)lines / FRAME_NS;
        switch (rel) {
        case EG_CRTC_STATUS:
            /* vblank: the last 4% of the frame */
            *val = (eg_crtc_enabled(s, i) && pos >= lines - lines / 25) ? 1 : 0;
            return true;
        case EG_CRTC_STATUS_POSITION:
            *val = eg_crtc_enabled(s, i) ? (pos << 16) : 0;
            return true;
        case EG_CRTC_STATUS_HV_COUNT:
            *val = eg_crtc_enabled(s, i) ? pos : 0;
            return true;
        case EG_GRPH_UPDATE:
            /* surface flips complete at once */
            *val = s->regs[off >> 2] & ~GRPH_SURFACE_UPDATE_PENDING;
            return true;
        default:
            return false;
        }
    }
    n = eg_ddc_for(off);
    if (n >= 0 && off == ddc_base[n] + 0xc) {
        *val = s->ddc[n].y;
        return true;
    }
    if (off >= EG_DC_HPD1_INT_STATUS && off < EG_DC_HPD1_INT_STATUS + 6 * 0xc &&
        (off - EG_DC_HPD1_INT_STATUS) % 0xc == 0) {
        int h = (off - EG_DC_HPD1_INT_STATUS) / 0xc;
        /* one monitor, on the DVI connector's pin by default */
        *val = (h == s->monitor_hpd ? EG_DC_HPD_SENSE : 0) |
               (s->hpd_pending[h] ? EG_DC_HPD_INT_STATUS : 0);
        return true;
    }
    return false;
}

static void eg_cursor_update(EvergreenState *s, int i, bool shape);

bool eg_dce_write(EvergreenState *s, uint32_t off, uint32_t val)
{
    uint32_t rel;
    int i = eg_crtc_for(off, &rel), n;

    if (i >= 0) {
        switch (rel) {
        case EG_VBLANK_STATUS: {
            /* OCCURRED is status, ACK (write 1) clears it */
            uint32_t occurred = s->regs[off >> 2] & VBLANK_OCCURRED;
            if (val & VBLANK_ACK) {
                occurred = 0;
                s->regs[disp_int_status[i] >> 2] &= ~LB_VBLANK_INTERRUPT;
            }
            s->regs[off >> 2] = (val & ~(VBLANK_OCCURRED | VBLANK_ACK)) | occurred;
            return true;
        }
        case EG_DC_LUT_RW_INDEX:
            s->regs[off >> 2] = val & 0xff;
            return true;
        case EG_CUR_CONTROL:
        case EG_CUR_SURFACE_ADDRESS:
        case EG_CUR_SURFACE_ADDRESS_HIGH:
        case EG_CUR_SIZE:
        case EG_CUR_HOT_SPOT:
            s->regs[off >> 2] = val;
            if (i < EG_NUM_CONSOLES && !(CREG(s, i, EG_CUR_UPDATE) & (1u << 16))) {
                eg_cursor_update(s, i, true);
            }
            return true;
        case EG_CUR_POSITION:
            s->regs[off >> 2] = val;
            if (i < EG_NUM_CONSOLES) {
                eg_cursor_update(s, i, false);
            }
            return true;
        case EG_CUR_UPDATE:
            s->regs[off >> 2] = val;
            if (i < EG_NUM_CONSOLES && !(val & (1u << 16))) {
                eg_cursor_update(s, i, true);       /* unlock: latch the shape */
            }
            return true;
        case EG_DC_LUT_PWL_DATA:
        case EG_DC_LUT_30_COLOR: {
            /* Apple's driver polls RW_INDEX for the increment after each entry */
            uint32_t *idx = &CREG(s, i, EG_DC_LUT_RW_INDEX);
            s->regs[off >> 2] = val;
            s->crtc[i].lut[*idx & 0xff] = val;
            *idx = (*idx + 1) & 0xff;
            return true;
        }
        default:
            return false;
        }
    }
    n = eg_ddc_for(off);
    if (n >= 0) {
        s->regs[off >> 2] = val;
        if (off == ddc_base[n] + 4 || off == ddc_base[n] + 8) {
            eg_ddc_update(s, n);
        }
        return true;
    }
    if (off >= EG_DC_HPD1_INT_CONTROL && off < EG_DC_HPD1_INT_CONTROL + 6 * 0xc &&
        (off - EG_DC_HPD1_INT_CONTROL) % 0xc == 0) {
        int h = (off - EG_DC_HPD1_INT_CONTROL) / 0xc;
        s->regs[off >> 2] = val & ~EG_DC_HPD_INT_ACK;
        if (val & EG_DC_HPD_INT_ACK) {
            s->hpd_pending[h] = false;
            s->regs[disp_int_status[h] >> 2] &= ~DC_HPD_INTERRUPT;
        }
        return true;
    }
    return false;
}

/* ------------------------------------------------------------------ */
/* Hardware cursor                                                     */
/* ------------------------------------------------------------------ */

static int eg_crtc_width(EvergreenState *s, int i)
{
    return (int)(CREG(s, i, EG_GRPH_X_END) - CREG(s, i, EG_GRPH_X_START));
}

static int eg_crtc_height(EvergreenState *s, int i)
{
    return (int)(CREG(s, i, EG_GRPH_Y_END) - CREG(s, i, EG_GRPH_Y_START));
}

/*
 * The cursor goes to the host as a cursor image (as on the rv530): a
 * 64x64 ARGB8888 surface with a 256 byte pitch.  Apple's driver applies
 * the hot spot itself (HOT_SPOT stays 0), so it is recovered from the
 * absolute pointer position, see rv530_cursor_update().
 */
static void eg_cursor_update(EvergreenState *s, int i, bool shape)
{
    EgCrtc *c = &s->crtc[i];
    uint32_t ctl = CREG(s, i, EG_CUR_CONTROL), size = CREG(s, i, EG_CUR_SIZE);
    uint32_t pos = CREG(s, i, EG_CUR_POSITION), hot = CREG(s, i, EG_CUR_HOT_SPOT);
    uint64_t addr = ((uint64_t)(CREG(s, i, EG_CUR_SURFACE_ADDRESS_HIGH) & 0xff) << 32) |
                    CREG(s, i, EG_CUR_SURFACE_ADDRESS);
    uint32_t w = ((size >> 16) & 0x7f) + 1, h = (size & 0x7f) + 1, x, y;
    int left, top, ax = -1, ay = -1;

    if (!c->con) {
        return;
    }
    if (!(ctl & 1)) {
        if (c->cursor_visible) {
            c->cursor_visible = false;
            qemu_console_set_mouse(c->con, 0, 0, false);
        }
        return;
    }
    if (w > 64 || h > 64) {
        return;
    }
    if (shape || !c->cursor || c->cursor->width != (int)w || c->cursor->height != (int)h) {
        if (!c->cursor || c->cursor->width != (int)w || c->cursor->height != (int)h) {
            cursor_unref(c->cursor);
            c->cursor = cursor_alloc(w, h);
        }
        for (y = 0; y < h; y++) {
            uint32_t line[64];
            if (!eg_gpu_read(s, addr + y * 256, line, w * 4)) {
                memset(line, 0, sizeof(line));
            }
            for (x = 0; x < w; x++) {
                c->cursor->data[y * w + x] = le32_to_cpu(line[x]);
            }
        }
        c->cursor->hot_x = c->hot_x;
        c->cursor->hot_y = c->hot_y;
        qemu_console_set_cursor(c->con, c->cursor);
    }
    left = (int)((pos >> 16) & 0x3fff) - (int)((hot >> 16) & 0x3f);
    top = (int)(pos & 0x3fff) - (int)(hot & 0x3f);
    if (i == 0 && eg_crtc_width(s, i) > 1 && qemu_input_get_last_abs(&ax, &ay)) {
        int px = (int)(((int64_t)ax * (eg_crtc_width(s, i) - 1) + 16383) / 32767);
        int py = (int)(((int64_t)ay * (eg_crtc_height(s, i) - 1) + 16383) / 32767);
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
    qemu_console_set_mouse(c->con, left + c->hot_x, top + c->hot_y, true);
    c->cursor_visible = true;
}

/* ------------------------------------------------------------------ */
/* Scanout                                                             */
/* ------------------------------------------------------------------ */

/* copy a tiled surface into the linear shadow buffer, a micro tile at a time
 * (within one 8x8 tile the offset only varies in the low pipe-interleave
 * bits, as long as a micro tile fits in the interleave) */
static void eg_detile(EvergreenState *s, const EgTiling *t, uint64_t off,
                      uint8_t *dst, uint32_t w, uint32_t h)
{
    uint32_t idx[64], x, y, i, j;
    uint64_t size = eg_tile_size(t);
    bool fast = 64 * t->bpp <= t->pipe_interleave;

    if (off + size > s->vram_size) {
        return;
    }
    for (i = 0; i < 64; i++) {
        idx[i] = eg_tile_offset(t, i & 7, i >> 3) - eg_tile_offset(t, 0, 0);
    }
    for (y = 0; y < h; y += 8) {
        for (x = 0; x < w; x += 8) {
            const uint8_t *src = s->vram_ptr + off + (fast ? eg_tile_offset(t, x, y) : 0);
            for (j = 0; j < 8 && y + j < h; j++) {
                uint8_t *d = dst + ((uint64_t)(y + j) * w + x) * t->bpp;
                for (i = 0; i < 8 && x + i < w; i++, d += t->bpp) {
                    const uint8_t *p = fast ? src + idx[j * 8 + i]
                                            : s->vram_ptr + off + eg_tile_offset(t, x + i, y + j);
                    memcpy(d, p, t->bpp);
                }
            }
        }
    }
}

static bool eg_gfx_update(void *opaque)
{
    EgCrtc *c = opaque;
    EvergreenState *s = c->s;
    int i = c->index;
    uint32_t ctl = CREG(s, i, EG_GRPH_CONTROL);

    /* no wait for the GPU: completed batches mark what they drew dirty */
    eg_gfx_poll(s);
    if (i == 0 && !s->cp_unlocked) {
        /* idle time: start the mirror's uploads before the next draws */
        eg_gfx_mirror_collect(s);
        eg_vk_upload_kick(s->gfx->vk);
    }
    uint32_t depth = ctl & 3, bpp = depth == 1 ? 2 : depth == 2 ? 4 : 0;
    uint32_t mode = (ctl >> 20) & 0xf;
    uint32_t w = CREG(s, i, EG_GRPH_X_END) - CREG(s, i, EG_GRPH_X_START);
    uint32_t h = CREG(s, i, EG_GRPH_Y_END) - CREG(s, i, EG_GRPH_Y_START);
    uint32_t pitch_px = CREG(s, i, EG_GRPH_PITCH) & 0x7fff;
    uint32_t pitch = pitch_px * bpp;
    uint64_t addr = ((uint64_t)(CREG(s, i, EG_GRPH_PRIMARY_SURFACE_ADDRESS_HIGH) & 0xff) << 32) |
                    (CREG(s, i, EG_GRPH_PRIMARY_SURFACE_ADDRESS) & 0xffffff00);
    uint32_t fbl = s->regs[EG_MC_VM_FB_LOCATION >> 2];
    uint64_t fb_start = (uint64_t)(fbl & 0xffff) << 24, off = addr - fb_start;
    bool tiled = mode >= EG_ARRAY_1D_TILED_THIN1;
    DisplaySurface *ds;
    uint8_t *old;
    EgTiling t;

    if (!eg_crtc_enabled(s, i) || !(CREG(s, i, EG_GRPH_ENABLE) & 1) || !bpp ||
        !w || !h || w > 8192 || h > 8192 || addr < fb_start || pitch_px < w ||
        off + (uint64_t)pitch * (h - 1) + w * bpp > s->vram_size) {
        return true;
    }
    if (tiled) {
        /* GRPH_CONTROL: NUM_BANKS 3:2, BANK_WIDTH 7:6, BANK_HEIGHT 12:11,
         * TILE_SPLIT 15:13, MACRO_TILE_ASPECT 19:18 */
        eg_tiling_init(s, &t, mode, bpp, pitch_px, h, ctl >> 2, ctl >> 6, ctl >> 11,
                       ctl >> 18, ctl >> 13, true);
    }
    if (w != c->last_w || h != c->last_h || bpp != c->last_bpp ||
        off != c->last_off || pitch != c->last_pitch || tiled != !!c->shadow) {
        c->last_w = w;
        c->last_h = h;
        c->last_bpp = bpp;
        c->last_off = off;
        c->last_pitch = pitch;
        trace_eg_scanout(i, w, h, bpp, addr);
        old = c->shadow;
        c->shadow = tiled ? g_malloc0((size_t)w * h * bpp) : NULL;
        /* 32 bpp ARGB8888 or 16 bpp RGB565: linear surfaces are VRAM
         * itself, tiled ones a detiled copy */
        ds = qemu_create_displaysurface_from(w, h,
                                             bpp == 4 ? PIXMAN_x8r8g8b8 : PIXMAN_r5g6b5,
                                             tiled ? w * bpp : pitch,
                                             tiled ? c->shadow : s->vram_ptr + off);
        qemu_console_set_surface(c->con, ds);
        g_free(old);                /* the replaced surface pointed into it */
    }
    if (tiled) {
        eg_detile(s, &t, off, c->shadow, w, h);
    }
    qemu_console_update_full(c->con);
    return true;
}

static const GraphicHwOps eg_gfx_ops = {
    .gfx_update = eg_gfx_update,
};

/* ------------------------------------------------------------------ */

void eg_dce_reset(EvergreenState *s)
{
    int n;

    for (n = 0; n < EG_NUM_DDC; n++) {
        s->ddc[n].y = 0x101;
    }
    memset(s->hpd_pending, 0, sizeof(s->hpd_pending));
    for (n = 0; n < 6; n++) {
        /* hot-plug detection enabled, as the VBIOS leaves it */
        s->regs[(EG_DC_HPD1_INT_CONTROL + 4 + 0xc * n) >> 2] = 1u << 28;
    }
}

void eg_dce_init(EvergreenState *s)
{
    int i;

    for (i = 0; i < EG_NUM_CRTC; i++) {
        s->crtc[i].s = s;
        s->crtc[i].index = i;
        s->crtc[i].base = crtc_offset[i];
        if (i < EG_NUM_CONSOLES) {
            s->crtc[i].con = qemu_graphic_console_create(DEVICE(s), i, &eg_gfx_ops,
                                                         &s->crtc[i]);
        }
    }
    for (i = 0; i < EG_NUM_DDC; i++) {
        char name[32];
        snprintf(name, sizeof(name), "evergreen.ddc%d", i + 1);
        s->ddc[i].bus = i2c_init_bus(DEVICE(s), name);
        bitbang_i2c_init(&s->ddc[i].bbi2c, s->ddc[i].bus);
        if (i == s->monitor_ddc) {
            rv530_edid_build(s->ddc[i].edid.blob, s->xres, s->yres, 2560, 1600, 0, 60);
            i2c_slave_set_address(I2C_SLAVE(&s->ddc[i].edid), 0x50);
            qdev_realize(DEVICE(&s->ddc[i].edid), BUS(s->ddc[i].bus), &error_abort);
        } else {
            object_unparent(OBJECT(&s->ddc[i].edid));
        }
    }
    timer_init_ns(&s->vblank_timer, QEMU_CLOCK_VIRTUAL, eg_vblank_timer, s);
    timer_mod(&s->vblank_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + FRAME_NS);
}

void eg_dce_fini(EvergreenState *s)
{
    int i;

    timer_del(&s->vblank_timer);
    for (i = 0; i < EG_NUM_CONSOLES; i++) {
        if (s->crtc[i].con) {
            qemu_graphic_console_close(s->crtc[i].con);
        }
        g_free(s->crtc[i].shadow);
        s->crtc[i].shadow = NULL;
        cursor_unref(s->crtc[i].cursor);
        s->crtc[i].cursor = NULL;
    }
}
