/*
 * HDP tiling apertures ("surfaces"): CPU accesses through BAR0 to an
 * address range covered by an active aperture see the tiled surface
 * behind it as a linear image.  Apple's ATIRadeonX3000 programs one for
 * every CPU copy into or out of a tiled buffer (GL swaps into window
 * surfaces, texture uploads: ATIRadeonX3000AMDR8xxHWMemory::prepare).
 *
 * Registers, per aperture n at 0x2c14 + 0x18 * n (Linux clears 32 of
 * them in evergreen_gpu_init; the field layout is from prepare()):
 *   +0x00 LOWER, +0x04 UPPER (inclusive), +0x08 BASE: GPU address >> 8
 *   +0x0c INFO: ARRAY_MODE 4:1, MICRO_TILE 6:5, BPP 9:7 (1-4: 16-128 bit),
 *         TILE_SPLIT 19:17, NUM_BANKS 21:20, BANK_WIDTH 23:22,
 *         BANK_HEIGHT 25:24, MACRO_TILE_ASPECT 27:26
 *   +0x10 SIZE: PITCH_TILE_MAX 10:0, SLICE_TILE_MAX 31:11
 *
 * Model: while an aperture is active, a RAM region holding the linear
 * image overlays BAR0 (filled by detiling VRAM when it is set up), so the
 * CPU copies at memory speed; it is tiled back into VRAM on an HDP flush
 * (HDP_MEM_COHERENCY_FLUSH_CNTL, which the driver writes before the GPU
 * uses the data) and when the aperture is torn down.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qapi/error.h"
#include "qemu/memalign.h"
#include "evergreen.h"
#include "eg_gfx.h"

#define HDP_SURF0               0x2c14
#define HDP_SURF_STRIDE         0x18
#define HDP_MEM_COHERENCY_FLUSH 0x5480

static uint64_t fb_start(EvergreenState *s)
{
    return (uint64_t)(s->regs[EG_MC_VM_FB_LOCATION >> 2] & 0xffff) << 24;
}

/*
 * Copy between the linear shadow and the tiled VRAM.  Towards VRAM only
 * the pages the CPU wrote (dirty log of the overlay) are copied, so GPU
 * rendering into the surface meanwhile is not overwritten.
 */
static void hdp_copy(EvergreenState *s, EgHdpSurf *h, bool to_vram)
{
    DirtyBitmapSnapshot *snap = NULL;
    const EgTiling *t = &h->tile;
    uint64_t vbase = h->base - fb_start(s);
    uint64_t lin0 = h->lo - h->base;            /* linear offset of the aperture start */
    uint8_t *shadow = h->shadow + (h->lo - h->map_lo);
    uint32_t bpp = t->bpp, x, y, j, i;
    uint32_t idx[64];
    bool fast = 64 * bpp <= t->pipe_interleave;

    eg_gfx_flush(s);                    /* the GPU writes the same VRAM */
    if (to_vram) {
        snap = memory_region_snapshot_and_clear_dirty(&h->mr, 0, h->map_len, DIRTY_MEMORY_VGA);
        if (s->debug & 32) {
            uint64_t a, nd = 0;
            for (a = 0; a < h->map_len; a += 4096) {
                nd += memory_region_snapshot_get_dirty(&h->mr, snap, a, MIN(4096, h->map_len - a));
            }
            qemu_log("evergreen: hdp flush %" PRIx64 "-%" PRIx64 " %" PRIu64 " dirty pages\n",
                     h->lo, h->hi, nd);
        }
        if (!memory_region_snapshot_get_dirty(&h->mr, snap, 0, h->map_len)) {
            g_free(snap);
            return;
        }
    }
    for (i = 0; i < 64; i++) {
        idx[i] = eg_tile_offset(t, i & 7, i >> 3) - eg_tile_offset(t, 0, 0);
    }
    for (y = 0; y < t->height; y += 8) {
        for (x = 0; x < t->pitch; x += 8) {
            uint64_t tbase = fast ? eg_tile_offset(t, x, y) : 0;
            for (j = 0; j < 8 && y + j < t->height; j++) {
                uint64_t lin = ((uint64_t)(y + j) * t->pitch + x) * bpp;
                if (lin + 8 * bpp <= lin0 || lin >= lin0 + h->len) {
                    continue;
                }
                if (snap) {
                    uint64_t a = h->lo - h->map_lo + MAX(lin, lin0) - lin0;
                    if (a >= h->map_len ||
                        !memory_region_snapshot_get_dirty(&h->mr, snap, a,
                                                          MIN(8 * bpp, h->map_len - a))) {
                        continue;
                    }
                }
                for (i = 0; i < 8; i++, lin += bpp) {
                    uint64_t v = vbase + (fast ? tbase + idx[j * 8 + i] :
                                                 eg_tile_offset(t, x + i, y + j));
                    if (lin < lin0 || lin + bpp > lin0 + h->len || v + bpp > s->vram_size) {
                        continue;
                    }
                    if (to_vram) {
                        memcpy(s->vram_ptr + v, shadow + (lin - lin0), bpp);
                    } else {
                        memcpy(shadow + (lin - lin0), s->vram_ptr + v, bpp);
                    }
                }
            }
        }
    }
    /* the page-alignment padding around the aperture is plain VRAM */
    {
        uint64_t mv = h->map_lo - fb_start(s), head = h->lo - h->map_lo;
        uint64_t tail = h->map_lo + h->map_len - (h->hi + 1);
        if (to_vram && snap) {
            if (head && memory_region_snapshot_get_dirty(&h->mr, snap, 0, head)) {
                memcpy(s->vram_ptr + mv, h->shadow, head);
            }
            if (tail && memory_region_snapshot_get_dirty(&h->mr, snap, h->map_len - tail, tail)) {
                memcpy(s->vram_ptr + mv + h->map_len - tail, h->shadow + h->map_len - tail,
                       tail);
            }
        } else if (to_vram) {
            memcpy(s->vram_ptr + mv, h->shadow, head);
            memcpy(s->vram_ptr + mv + h->map_len - tail, h->shadow + h->map_len - tail, tail);
        } else {
            memcpy(h->shadow, s->vram_ptr + mv, head);
            memcpy(h->shadow + h->map_len - tail, s->vram_ptr + mv + h->map_len - tail, tail);
        }
    }
    if (to_vram) {
        memory_region_set_dirty(&s->vram, vbase, MIN(eg_tile_size(t), s->vram_size - vbase));
    }
    g_free(snap);
}

static void hdp_activate(EvergreenState *s, int n)
{
    EgHdpSurf *h = &s->hdp[n];
    uint32_t *r = &s->regs[(HDP_SURF0 + HDP_SURF_STRIDE * n) >> 2];
    uint32_t info = r[3], size = r[4];
    uint32_t bppc = (info >> 7) & 7, pitch = ((size & 0x7ff) + 1) * 8;
    uint64_t slice = (uint64_t)((size >> 11) + 1) * 64;
    uint64_t off;
    char name[32];

    h->lo = (uint64_t)r[0] << 8;
    h->hi = ((uint64_t)r[1] << 8) | 0xff;
    h->base = (uint64_t)r[2] << 8;
    h->len = h->hi - h->lo + 1;
    /* KVM maps whole host pages */
    h->map_lo = h->lo & ~(uint64_t)(qemu_real_host_page_size() - 1);
    h->map_len = ROUND_UP(h->hi + 1, qemu_real_host_page_size()) - h->map_lo;
    off = h->map_lo - fb_start(s);
    if (h->lo < fb_start(s) || h->hi < h->lo || h->lo < h->base || h->map_lo < fb_start(s) ||
        off + h->map_len > memory_region_size(&s->aperture) || !bppc || bppc > 4) {
        qemu_log_mask(LOG_GUEST_ERROR, "evergreen: HDP aperture %d out of range\n", n);
        return;
    }
    eg_tiling_init(s, &h->tile, (info >> 1) & 0xf, 1u << bppc, pitch,
                   (uint32_t)(slice / pitch), info >> 20, info >> 22, info >> 24,
                   info >> 26, info >> 17, ((info >> 5) & 3) == 0);
    h->shadow = qemu_memalign(qemu_real_host_page_size(), h->map_len);
    hdp_copy(s, h, false);
    snprintf(name, sizeof(name), "evergreen.hdp%d", n);
    memory_region_init_ram_ptr(&h->mr, OBJECT(s), name, h->map_len, h->shadow);
    memory_region_set_log(&h->mr, true, DIRTY_MEMORY_VGA);
    memory_region_add_subregion_overlap(&s->bar0, off, &h->mr, 1);
    h->active = true;
    if (s->debug & 32) {
        qemu_log("evergreen: hdp%d on %" PRIx64 "-%" PRIx64 " base %" PRIx64 " info %08x size %08x"
                 " %ux%u bpp %u mode %u\n", n, h->lo, h->hi, h->base, info, size,
                 h->tile.pitch, h->tile.height, h->tile.bpp, h->tile.array_mode);
    }
}

static void hdp_deactivate(EvergreenState *s, int n)
{
    EgHdpSurf *h = &s->hdp[n];

    hdp_copy(s, h, true);
    if (s->debug & 32) {
        qemu_log("evergreen: hdp%d off\n", n);
    }
    memory_region_set_log(&h->mr, false, DIRTY_MEMORY_VGA);
    memory_region_del_subregion(&s->bar0, &h->mr);
    object_unparent(OBJECT(&h->mr));
    /* the KVM slot is gone once the transaction above has committed */
    qemu_vfree(h->shadow);
    h->shadow = NULL;
    h->active = false;
}

void eg_hdp_flush(EvergreenState *s)
{
    for (int n = 0; n < EG_NUM_HDP; n++) {
        if (s->hdp[n].active) {
            hdp_copy(s, &s->hdp[n], true);
        }
    }
}

bool eg_hdp_write(EvergreenState *s, uint32_t off, uint32_t val)
{
    int n;
    uint32_t *r;
    bool want;

    if (off == HDP_MEM_COHERENCY_FLUSH) {
        s->regs[off >> 2] = val;
        eg_hdp_flush(s);
        return true;
    }
    if (off < HDP_SURF0 || off >= HDP_SURF0 + HDP_SURF_STRIDE * EG_NUM_HDP) {
        return false;
    }
    s->regs[off >> 2] = val;
    n = (off - HDP_SURF0) / HDP_SURF_STRIDE;
    r = &s->regs[(HDP_SURF0 + HDP_SURF_STRIDE * n) >> 2];
    /* tiled, and a non-empty range: LOWER/UPPER are written last on set
     * up and UPPER is cleared first on tear down */
    want = ((r[3] >> 1) & 0xf) >= EG_ARRAY_1D_TILED_THIN1 && r[1] && r[1] >= r[0];
    if (s->hdp[n].active) {
        hdp_deactivate(s, n);       /* reprogrammed: flush the old mapping */
    }
    if (want) {
        hdp_activate(s, n);
    }
    return true;
}

void eg_hdp_reset(EvergreenState *s)
{
    for (int n = 0; n < EG_NUM_HDP; n++) {
        if (s->hdp[n].active) {
            hdp_deactivate(s, n);
        }
    }
}
