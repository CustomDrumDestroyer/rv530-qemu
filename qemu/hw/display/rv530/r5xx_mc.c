/*
 * RV530 memory controller: MC address space, VRAM window, PCIe GART.
 *
 * Every GPU-side access goes through rv530_gpu_map/read/write which
 * turn a 32-bit GPU (MC) address into VRAM bytes or guest system memory
 * through the RV370-style PCIe GART (Linux rv370_pcie_gart_*).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "system/dma.h"
#include "hw/pci/pci.h"
#include "rv530.h"
#include "system/address-spaces.h"
#include "r5xx_tex.h"
#include "trace.h"

#define GPU_PAGE_SHIFT 12
#define GPU_PAGE_SIZE  (1u << GPU_PAGE_SHIFT)
#define GPU_PAGE_MASK  (GPU_PAGE_SIZE - 1)

#define R300_PTE_UNSNOOPED (1u << 0)
#define R300_PTE_WRITEABLE (1u << 2)
#define R300_PTE_READABLE  (1u << 3)

/* ------------------------------------------------------------------ */
/* MC indirect registers                                               */
/* ------------------------------------------------------------------ */

static void rv530_mc_update_windows(RV530State *s)
{
    uint32_t fb = s->mc_regs[RV_MC_FB_LOCATION];
    s->fb_start = (fb & 0xffff) << 16;
    s->fb_end = ((fb >> 16) & 0xffff) << 16 | 0xffff;
    trace_rv530_mc_fb_location(s->fb_start, s->fb_end);
}

uint32_t rv530_mc_reg_read(RV530State *s, uint32_t idx)
{
    uint32_t val;

    switch (idx) {
    case RV_MC_STATUS:
        val = RV_MC_IDLE | RV_MC_IDLE_RV515;
        break;
    default:
        val = s->mc_regs[idx];
        break;
    }
    trace_rv530_mc_read(idx, r5xx_mc_regname(idx) ?: "?", val);
    return val;
}

void rv530_mc_reg_write(RV530State *s, uint32_t idx, uint32_t val)
{
    trace_rv530_mc_write(idx, r5xx_mc_regname(idx) ?: "?", val);
    s->mc_regs[idx] = val;
    if (idx == RV_MC_FB_LOCATION) {
        rv530_mc_update_windows(s);
        rv530_gart_invalidate(s);
    }
}

/* ------------------------------------------------------------------ */
/* PCIe indirect registers / GART                                      */
/* ------------------------------------------------------------------ */

static void rv530_gart_update(RV530State *s)
{
    uint32_t cntl = s->pcie_regs[RV_PCIE_TX_GART_CNTL];
    s->gart_enabled = !!(cntl & RV_PCIE_TX_GART_EN);
    s->gart_start = s->pcie_regs[RV_PCIE_TX_GART_START_LO];
    s->gart_end = s->pcie_regs[RV_PCIE_TX_GART_END_LO] | GPU_PAGE_MASK;
    s->gart_base = s->pcie_regs[RV_PCIE_TX_GART_BASE];
    trace_rv530_gart_update(s->gart_enabled, s->gart_start, s->gart_end,
                            s->gart_base);
    rv530_gart_invalidate(s);
}

uint32_t rv530_pcie_reg_read(RV530State *s, uint32_t idx)
{
    uint32_t val = s->pcie_regs[idx];
    trace_rv530_pcie_read(idx, r5xx_pcie_regname(idx) ?: "?", val);
    return val;
}

void rv530_pcie_reg_write(RV530State *s, uint32_t idx, uint32_t val)
{
    trace_rv530_pcie_write(idx, r5xx_pcie_regname(idx) ?: "?", val);
    switch (idx) {
    case RV_PCIE_TX_GART_CNTL:
        /* the invalidate bit is self clearing */
        s->pcie_regs[idx] = val & ~RV_PCIE_TX_GART_INVALIDATE_TLB;
        rv530_gart_update(s);
        break;
    case RV_PCIE_TX_GART_BASE:
    case RV_PCIE_TX_GART_START_LO:
    case RV_PCIE_TX_GART_START_HI:
    case RV_PCIE_TX_GART_END_LO:
    case RV_PCIE_TX_GART_END_HI:
        s->pcie_regs[idx] = val;
        rv530_gart_update(s);
        break;
    case RV_PCIE_TX_GART_ERROR:
        s->pcie_regs[idx] = 0;
        break;
    default:
        s->pcie_regs[idx] = val;
        break;
    }
}

void rv530_gart_invalidate(RV530State *s)
{
    int i;
    if (s->gfx) {
        rv530_gfx_gart_changed(s);
    }
    for (i = 0; i < RV_GART_CACHE_SIZE; i++) {
        RV530GartEntry *e = &s->gart_cache[i];
        if (e->host) {
            dma_memory_unmap(pci_get_address_space(PCI_DEVICE(s)), e->host,
                             e->len, e->write ? DMA_DIRECTION_FROM_DEVICE
                                              : DMA_DIRECTION_TO_DEVICE,
                             e->len);
            e->host = NULL;
        }
        e->gpu_page = ~0u;
    }
}

/* Look up the page-table entry for a GPU page; returns false on fault. */
static bool rv530_gart_lookup(RV530State *s, uint32_t addr, uint64_t *phys,
                              bool write)
{
    uint32_t page = (addr - s->gart_start) >> GPU_PAGE_SHIFT;
    uint32_t pte_addr = s->gart_base + page * 4;
    uint32_t pte;
    uint8_t *p;

    /* the page table itself normally lives in VRAM */
    p = rv530_gpu_map(s, pte_addr, 4, false);
    if (!p) {
        return false;
    }
    pte = ldl_le_p(p);
    if (!(pte & (write ? R300_PTE_WRITEABLE : R300_PTE_READABLE))) {
        /* Linux sets both bits; treat a zero entry as unmapped */
        if (pte == 0) {
            return false;
        }
    }
    *phys = ((uint64_t)(pte & 0x0ffffff0) << 8) | ((uint64_t)(pte >> 24) << 32);
    return true;
}

/* Guest RAM page backing a GART page (Vulkan backend).  'ram' is the
 * machine RAM region the backend imported. */
bool rv530_gart_ram_page(RV530State *s, uint32_t addr, MemoryRegion *ram,
                         uint64_t *ram_off)
{
    uint64_t phys;
    MemoryRegionSection sec;
    bool ok;

    if (!s->gart_enabled || addr < s->gart_start || addr > s->gart_end ||
        !rv530_gart_lookup(s, addr & ~GPU_PAGE_MASK, &phys, true)) {
        return false;
    }
    sec = memory_region_find(get_system_memory(), phys, GPU_PAGE_SIZE);
    ok = sec.mr == ram && int128_ge(sec.size, int128_make64(GPU_PAGE_SIZE));
    if (ok) {
        *ram_off = sec.offset_within_region;
    }
    if (sec.mr) {
        memory_region_unref(sec.mr);
    }
    return ok;
}

static uint8_t *rv530_gart_map_page_locked(RV530State *s, uint32_t addr, bool write);

/* CPU-path draws may rasterize on several threads (r5xx_3d.c cpu pool) */
static uint8_t *rv530_gart_map_page(RV530State *s, uint32_t addr, bool write)
{
    uint8_t *p;

    qemu_mutex_lock(&s->gart_lock);
    p = rv530_gart_map_page_locked(s, addr, write);
    qemu_mutex_unlock(&s->gart_lock);
    return p;
}

static uint8_t *rv530_gart_map_page_locked(RV530State *s, uint32_t addr, bool write)
{
    uint32_t gpu_page = addr >> GPU_PAGE_SHIFT;
    uint32_t slot = gpu_page % RV_GART_CACHE_SIZE;
    RV530GartEntry *e = &s->gart_cache[slot];
    uint64_t phys;
    dma_addr_t len = GPU_PAGE_SIZE;
    void *host;

    if (e->gpu_page == gpu_page && e->host && (e->write || !write)) {
        return (uint8_t *)e->host + (addr & GPU_PAGE_MASK);
    }
    if (!rv530_gart_lookup(s, addr, &phys, write)) {
        return NULL;
    }
    host = dma_memory_map(pci_get_address_space(PCI_DEVICE(s)), phys, &len,
                          DMA_DIRECTION_FROM_DEVICE, MEMTXATTRS_UNSPECIFIED);
    if (!host || len != GPU_PAGE_SIZE) {
        if (host) {
            dma_memory_unmap(pci_get_address_space(PCI_DEVICE(s)), host, len,
                             DMA_DIRECTION_FROM_DEVICE, 0);
        }
        return NULL;
    }
    if (e->host) {
        dma_memory_unmap(pci_get_address_space(PCI_DEVICE(s)), e->host, e->len,
                         DMA_DIRECTION_FROM_DEVICE, e->len);
    }
    e->gpu_page = gpu_page;
    e->host = host;
    e->len = len;
    e->write = true;
    trace_rv530_gart_map(addr, phys);
    return (uint8_t *)host + (addr & GPU_PAGE_MASK);
}

/* ------------------------------------------------------------------ */
/* GPU address space access                                            */
/* ------------------------------------------------------------------ */

/*
 * RADEON_SURFACEn: aperture windows through which the CPU sees tiled VRAM
 * linearly.  The model cannot swizzle direct-mapped aperture accesses, so
 * memory under a set tiling surface is kept linear, and GPU-side tiled
 * accesses to it are treated as linear (see r5xx_linear_hook).  The memory
 * is converted when a window is set (tiled -> linear) and removed (linear
 * -> tiled), so the GPU's tiled view of it stays right across the window's
 * life.  Lion's driver needs that: it sets a window over a tiled texture,
 * fills it with the CPU, removes the window and samples the texture tiled.
 * 10.6 only keeps one window over the front buffer.
 */
#define RV_SURFACE_CNTL    0x0b00
#define RV_SURFACE0_LOWER  0x0b04
#define RV_SURFACE_STRIDE  0x10
#define RV_SURF_TILE_MASK  0x000f0000u
#define RV_SURF_TILE_MACRO (1u << 16)
#define RV_SURF_TILE_MICRO (2u << 16)
#define RV_SURF_PITCH_MASK 0x0000ffffu       /* in units of 8 bytes */

static bool rv530_surf_set(uint32_t lo, uint32_t hi, uint32_t info)
{
    return (info & RV_SURF_TILE_MASK) && hi > lo;
}

bool rv530_linear_surface(RV530State *s, uint32_t gpu_addr)
{
    uint32_t off;
    int i;

    if (gpu_addr < s->fb_start || gpu_addr > s->fb_end) {
        return false;
    }
    off = gpu_addr - s->fb_start;
    for (i = 0; i < RV_NUM_SURFACES; i++) {
        if (rv530_surf_set(s->surf_lo[i], s->surf_hi[i], s->surf_info[i]) &&
            off >= s->surf_lo[i] && off <= s->surf_hi[i]) {
            return true;
        }
    }
    return false;
}

/*
 * Convert VRAM [lo, hi] between linear and the surface's tiling.  Colour
 * tiling only (depth tiling has its own layout and stays linear, as
 * before); the layout is taken at 32 bpp, which for micro and macro tiles
 * is the same byte layout as 16 bpp.  Every tiled layout here keeps runs
 * of 16 bytes together, so those are moved whole.
 */
static void rv530_surf_convert(RV530State *s, int i, uint32_t lo, uint32_t hi,
                               uint32_t info, bool to_tiled)
{
    uint32_t pitch = (info & RV_SURF_PITCH_MASK) * 8;
    uint32_t mode = info & RV_SURF_TILE_MASK;
    R5xxSurfaceLayout l = { .bpp = 4, .micro_mode = 1, .is_r500 = true };
    uint64_t len;
    uint32_t rows, x, y;
    uint8_t *tmp, *v;

    if (mode == (RV_SURF_TILE_MACRO | RV_SURF_TILE_MICRO)) {
        l.tiling = R5XX_TILE_MICRO_MACRO;
    } else if (mode == RV_SURF_TILE_MACRO) {
        l.tiling = R5XX_TILE_MACRO;
    } else if (mode == RV_SURF_TILE_MICRO) {
        l.tiling = R5XX_TILE_MICRO;
    } else {
        return;
    }
    if (pitch < 16 || (pitch & 15) || hi >= s->vram_size) {
        return;
    }
    len = (uint64_t)hi - lo + 1;
    rows = len / pitch;
    if (!rows) {
        return;
    }
    l.pitch_bytes = pitch;
    l.width = pitch / 4;
    l.height = rows;
    trace_rv530_surface(i, lo, hi, info, to_tiled);
    rv530_gfx_flush(s);             /* the GPU's writes are in VRAM */
    v = s->vram_ptr + lo;
    tmp = g_malloc(len);
    memcpy(tmp, v, len);
    for (y = 0; y < rows; y++) {
        for (x = 0; x < l.width; x += 4) {
            uint64_t t = r5xx_tiled_pixel_offset(&l, x, y);
            uint64_t p = (uint64_t)y * pitch + x * 4;

            if (t + 16 > len) {
                continue;
            }
            if (to_tiled) {
                memcpy(v + t, tmp + p, 16);
            } else {
                memcpy(v + p, tmp + t, 16);
            }
        }
    }
    g_free(tmp);
    memory_region_set_dirty(&s->vram, lo, len);
}

/*
 * SURFACE_CNTL and SURFACEn_LOWER/UPPER/INFO.  A window takes effect when
 * its INFO is written (Apple writes LOWER, UPPER, INFO to set one and
 * clears them in the same order, so LOWER = 0 alone must not move it), or
 * on a LOWER/UPPER write while no window is set (Linux writes INFO first).
 */
void rv530_surface_write(RV530State *s, uint32_t off, uint32_t val)
{
    uint32_t rel = off - RV_SURFACE0_LOWER;
    int i = rel / RV_SURFACE_STRIDE;
    uint32_t base = RV_SURFACE0_LOWER + i * RV_SURFACE_STRIDE;
    uint32_t lo, hi, info;
    bool was;

    s->regs[off >> 2] = val;
    if (off < RV_SURFACE0_LOWER || rel % RV_SURFACE_STRIDE > 8 ||
        i >= RV_NUM_SURFACES) {
        return;
    }
    was = rv530_surf_set(s->surf_lo[i], s->surf_hi[i], s->surf_info[i]);
    if (was && rel % RV_SURFACE_STRIDE != 8) {
        return;
    }
    lo = s->regs[base >> 2];
    hi = s->regs[(base + 4) >> 2];
    info = s->regs[(base + 8) >> 2];
    if (lo == s->surf_lo[i] && hi == s->surf_hi[i] && info == s->surf_info[i]) {
        return;
    }
    if (was) {
        uint32_t olo = s->surf_lo[i], ohi = s->surf_hi[i], oinfo = s->surf_info[i];

        s->surf_info[i] = 0;        /* off the linear list first */
        rv530_surf_convert(s, i, olo, ohi, oinfo, true);
    }
    if (rv530_surf_set(lo, hi, info)) {
        rv530_surf_convert(s, i, lo, hi, info, false);
    }
    s->surf_lo[i] = lo;
    s->surf_hi[i] = hi;
    s->surf_info[i] = info;
}

static bool rv530_linear_hook(void *opaque, uint32_t gpu_addr)
{
    return rv530_linear_surface(opaque, gpu_addr);
}

void rv530_surface_hook_init(RV530State *s)
{
    r5xx_linear_hook = rv530_linear_hook;
    r5xx_linear_hook_opaque = s;
}

uint8_t *rv530_gpu_map(RV530State *s, uint32_t addr, uint32_t len, bool write)
{
    if (addr >= s->fb_start && addr <= s->fb_end) {
        uint64_t off = addr - s->fb_start;
        if (off + len <= s->vram_size) {
            return s->vram_ptr + off;
        }
        return NULL;
    }
    if (s->gart_enabled && addr >= s->gart_start && addr <= s->gart_end) {
        if (((addr + len - 1) >> GPU_PAGE_SHIFT) != (addr >> GPU_PAGE_SHIFT)) {
            return NULL;           /* crosses a page */
        }
        return rv530_gart_map_page(s, addr, write);
    }
    return NULL;
}

static void rv530_gpu_fault(RV530State *s, uint32_t addr, uint32_t len,
                            bool write)
{
    s->gart_faults++;
    if (s->gart_faults < 32) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "rv530: GPU %s fault addr=0x%08x len=%u "
                      "(fb 0x%08x-0x%08x gart %s 0x%08x-0x%08x)\n",
                      write ? "write" : "read", addr, len,
                      s->fb_start, s->fb_end, s->gart_enabled ? "on" : "off",
                      s->gart_start, s->gart_end);
    }
    trace_rv530_gpu_fault(addr, len, write);
}

void rv530_gpu_read(RV530State *s, uint32_t addr, void *buf, uint32_t len)
{
    uint8_t *dst = buf;
    while (len) {
        uint32_t chunk = GPU_PAGE_SIZE - (addr & GPU_PAGE_MASK);
        uint8_t *p;
        if (chunk > len) {
            chunk = len;
        }
        p = rv530_gpu_map(s, addr, chunk, false);
        if (p) {
            memcpy(dst, p, chunk);
        } else {
            rv530_gpu_fault(s, addr, chunk, false);
            memset(dst, 0, chunk);
        }
        addr += chunk;
        dst += chunk;
        len -= chunk;
    }
}

void rv530_gpu_write(RV530State *s, uint32_t addr, const void *buf, uint32_t len)
{
    const uint8_t *src = buf;
    while (len) {
        uint32_t chunk = GPU_PAGE_SIZE - (addr & GPU_PAGE_MASK);
        uint8_t *p;
        if (chunk > len) {
            chunk = len;
        }
        p = rv530_gpu_map(s, addr, chunk, true);
        if (p) {
            memcpy(p, src, chunk);
            if (p >= s->vram_ptr && p < s->vram_ptr + s->vram_size) {
                memory_region_set_dirty(&s->vram, p - s->vram_ptr, chunk);
            }
        } else {
            rv530_gpu_fault(s, addr, chunk, true);
        }
        addr += chunk;
        src += chunk;
        len -= chunk;
    }
}

/* R5xxMem glue for the QEMU-independent core */

static uint8_t *mem_map(void *opaque, uint32_t addr, uint32_t len, bool write)
{
    return rv530_gpu_map(opaque, addr, len, write);
}

/*
 * After the store, not at map time: raster workers write with the BQL
 * released, and a dirty-log collect between the two would clear the mark and
 * lose the write.
 */
static void mem_written(void *opaque, uint8_t *p, uint32_t len)
{
    RV530State *s = opaque;

    if (p >= s->vram_ptr && p < s->vram_ptr + s->vram_size) {
        memory_region_set_dirty(&s->vram, p - s->vram_ptr, len);
    }
}

static void mem_read(void *opaque, uint32_t addr, void *buf, uint32_t len)
{
    rv530_gpu_read(opaque, addr, buf, len);
}

static void mem_write(void *opaque, uint32_t addr, const void *buf, uint32_t len)
{
    rv530_gpu_write(opaque, addr, buf, len);
}

static void G_GNUC_PRINTF(3, 0) mem_log(void *opaque, int level, const char *fmt, va_list ap)
{
    RV530State *s = opaque;
    if (level <= 1 || (s->debug & 8)) {
        char buf[512];
        vsnprintf(buf, sizeof(buf), fmt, ap);
        qemu_log_mask(level == 0 ? LOG_GUEST_ERROR : LOG_UNIMP, "rv530-3d: %s\n", buf);
    }
}

void rv530_mc_init(RV530State *s)
{
    qemu_mutex_init(&s->gart_lock);
    s->mem.opaque = s;
    s->mem.map = mem_map;
    s->mem.read = mem_read;
    s->mem.write = mem_write;
    s->mem.written = mem_written;
    s->mem.log = mem_log;
}

void rv530_mc_reset(RV530State *s)
{
    rv530_gart_invalidate(s);
    memset(s->pcie_regs, 0, sizeof(s->pcie_regs));
    memset(s->mc_regs, 0, sizeof(s->mc_regs));
    /* VRAM at GPU address 0, as a BIOS would leave it */
    s->mc_regs[RV_MC_FB_LOCATION] = ((s->vram_size - 1) >> 16) << 16;
    s->mc_regs[RV_MC_AGP_LOCATION] = 0xffffffff;
    s->mc_regs[RV_MC_CNTL0] = RV_MC_CNTL0_128BIT;
    rv530_mc_update_windows(s);
    s->gart_enabled = false;
    s->gart_faults = 0;
    s->regs[RV_HDP_FB_LOCATION >> 2] = 0;
}
