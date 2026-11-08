/*
 * ATI Radeon HD 5770 (Juniper, Evergreen / TeraScale 2) device model.
 *
 * Enough of the chip for Linux radeon's evergreen_init() and Apple's
 * ATI5000Controller / ATIRadeonX3000 (Quartz Extreme, Core Image, GL)
 * with the synthetic AtomBIOS of scripts/atombios/mkrom-evergreen.py
 * (eg_atom.c) or a real VBIOS: register file with the status and
 * identity values the drivers read, the memory controller (VRAM aperture,
 * PCIe GART), the interrupt handler ring (IH), the command processor and
 * async DMA rings (eg_cp.c), the DCE4 display engine (eg_dce.c) and the
 * 3D engine (eg_draw.c, with the pixel stage on the host GPU: eg_vk.c).
 *
 *   BAR0  256 MiB VRAM aperture (prefetchable)
 *   BAR2  128 KiB registers
 *   BAR4  256 bytes I/O (MM_INDEX / MM_DATA)
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/main-loop.h"
#include "qemu/error-report.h"
#include "qemu/units.h"
#include "qapi/error.h"
#include "hw/pci/msi.h"
#include "hw/pci/pcie.h"
#include "hw/core/qdev-properties.h"
#include "system/dma.h"
#include "system/address-spaces.h"
#include "evergreen.h"
#include "eg_gfx.h"
#include "trace.h"

/* Juniper identity, as Linux evergreen_gpu_init() expects */
#define JUNIPER_GB_ADDR_CONFIG  0x02010002
#define EG_MC_CHMAP_4CH         (2u << 12)      /* NOOFCHAN: 4 x 32-bit = 128-bit */

/* ------------------------------------------------------------------ */
/* Interrupts: IH ring in GPU memory, MSI (or INTx)                    */
/* ------------------------------------------------------------------ */

static void eg_update_irq(EvergreenState *s)
{
    PCIDevice *d = PCI_DEVICE(s);
    uint32_t rptr = s->regs[EG_IH_RB_RPTR >> 2];
    bool level = (s->regs[EG_IH_CNTL >> 2] & EG_IH_ENABLE_INTR) &&
                 (s->regs[EG_IH_RB_CNTL >> 2] & EG_IH_RB_ENABLE) &&
                 rptr != s->ih_wptr;

    if (msi_enabled(d)) {
        if (level && !s->irq_level) {
            msi_notify(d, 0);
        }
    } else {
        pci_set_irq(d, level);
    }
    s->irq_level = level;
}

static uint32_t eg_ih_size(EvergreenState *s)
{
    /* IH_RB_CNTL bits 5:1: log2 of the ring size in dwords */
    return 4u << ((s->regs[EG_IH_RB_CNTL >> 2] >> 1) & 0x1f);
}

void eg_ih_push(EvergreenState *s, uint32_t src_id, uint32_t src_data)
{
    uint32_t cntl = s->regs[EG_IH_RB_CNTL >> 2];
    uint64_t base = (uint64_t)s->regs[EG_IH_RB_BASE >> 2] << 8;
    uint32_t size = eg_ih_size(s);
    uint32_t v[4] = { cpu_to_le32(src_id), cpu_to_le32(src_data & 0xfffffff), 0, 0 };

    if (!(cntl & EG_IH_RB_ENABLE)) {
        return;
    }
    trace_eg_ih_push(src_id, src_data, s->ih_wptr);
    eg_gpu_write(s, base + s->ih_wptr, v, sizeof(v));
    s->ih_wptr = (s->ih_wptr + 16) & (size - 1);
    if (cntl & EG_IH_WPTR_WRITEBACK_ENABLE) {
        uint64_t wb = ((uint64_t)(s->regs[EG_IH_RB_WPTR_ADDR_HI >> 2] & 0xff) << 32) |
                      (s->regs[EG_IH_RB_WPTR_ADDR_LO >> 2] & ~3u);
        uint32_t w = cpu_to_le32(s->ih_wptr);
        eg_gpu_write(s, wb, &w, 4);
    }
    /* each new vector is an edge for MSI */
    s->irq_level = false;
    eg_update_irq(s);
}

/* ------------------------------------------------------------------ */
/* GPU address space: VRAM through MC_VM_FB_LOCATION, system memory    */
/* through the PCIe GART (VM context 0, 4 KiB pages, 64-bit PTEs)     */
/* ------------------------------------------------------------------ */

#define EG_PTE_VALID    (1ull << 0)
#define EG_PTE_SYSTEM   (1ull << 1)

typedef enum { EG_MEM_NONE, EG_MEM_VRAM, EG_MEM_SYS } EgMemKind;

/* translate one GPU address; *avail is how many bytes the result covers */
static EgMemKind eg_translate(EvergreenState *s, uint64_t addr, uint64_t *out,
                              uint64_t *avail)
{
    uint32_t fbl = s->regs[EG_MC_VM_FB_LOCATION >> 2];
    uint64_t fb_start = (uint64_t)(fbl & 0xffff) << 24;
    uint64_t fb_end = ((uint64_t)(fbl >> 16) << 24) | 0xffffff;
    uint64_t pt_start = (uint64_t)s->regs[EG_VM_CONTEXT0_PAGE_TABLE_START_ADDR >> 2] << 12;
    uint64_t pt_end = ((uint64_t)s->regs[EG_VM_CONTEXT0_PAGE_TABLE_END_ADDR >> 2] << 12) | 0xfff;

    if (fbl && addr >= fb_start && addr <= fb_end) {
        uint64_t off = addr - fb_start;
        if (off >= s->vram_size) {
            return EG_MEM_NONE;
        }
        *out = off;
        *avail = MIN(fb_end + 1 - addr, s->vram_size - off);
        return EG_MEM_VRAM;
    }
    if ((s->regs[EG_VM_CONTEXT0_CNTL >> 2] & 1) && addr >= pt_start && addr <= pt_end) {
        uint64_t table = (uint64_t)s->regs[EG_VM_CONTEXT0_PAGE_TABLE_BASE_ADDR >> 2] << 12;
        uint64_t pte_addr = table + ((addr - pt_start) >> 12) * 8;
        uint64_t pte = 0, pv;
        uint64_t ptk;

        /* the page table lives in VRAM */
        if (eg_translate(s, pte_addr, &pv, &ptk) != EG_MEM_VRAM || ptk < 8) {
            return EG_MEM_NONE;
        }
        memcpy(&pte, s->vram_ptr + pv, 8);
        pte = le64_to_cpu(pte);
        if (!(pte & EG_PTE_VALID)) {
            return EG_MEM_NONE;
        }
        *out = (pte & 0xfffffffff000ull) | (addr & 0xfff);
        *avail = 0x1000 - (addr & 0xfff);
        return (pte & EG_PTE_SYSTEM) ? EG_MEM_SYS : EG_MEM_VRAM;
    }
    return EG_MEM_NONE;
}

bool eg_gart_range(EvergreenState *s, uint64_t *start, uint64_t *end)
{
    if (!(s->regs[EG_VM_CONTEXT0_CNTL >> 2] & 1)) {
        return false;
    }
    *start = (uint64_t)s->regs[EG_VM_CONTEXT0_PAGE_TABLE_START_ADDR >> 2] << 12;
    *end = ((uint64_t)s->regs[EG_VM_CONTEXT0_PAGE_TABLE_END_ADDR >> 2] << 12) | 0xfff;
    return *end > *start;
}

/* host pointer to the 64-bit PTEs of the GART (in VRAM), *n of them; NULL if none */
const uint64_t *eg_gart_ptes(EvergreenState *s, uint64_t *n)
{
    uint64_t start, end, table, off, avail;

    if (!eg_gart_range(s, &start, &end)) {
        return NULL;
    }
    table = (uint64_t)s->regs[EG_VM_CONTEXT0_PAGE_TABLE_BASE_ADDR >> 2] << 12;
    if (eg_translate(s, table, &off, &avail) != EG_MEM_VRAM || (off & 7)) {
        return NULL;
    }
    *n = MIN((end - start + 1) >> 12, avail / 8);
    return (const uint64_t *)(s->vram_ptr + off);
}

bool eg_gart_vk_entry(EvergreenState *s, uint64_t addr, MemoryRegion *ram, uint32_t *entry)
{
    uint64_t a, avail;
    MemoryRegionSection sec;
    bool ok;

    switch (eg_translate(s, addr & ~0xfffull, &a, &avail)) {
    case EG_MEM_VRAM:
        if ((a >> 12) >= 0x80000000ull) {
            return false;
        }
        *entry = 0x80000000u | (uint32_t)(a >> 12);
        return true;
    case EG_MEM_SYS:
        if (!ram) {
            return false;
        }
        sec = memory_region_find(get_system_memory(), a, 0x1000);
        ok = sec.mr == ram && int128_ge(sec.size, int128_make64(0x1000)) &&
             (sec.offset_within_region >> 12) < 0x80000000ull;
        if (ok) {
            *entry = (uint32_t)(sec.offset_within_region >> 12);
        }
        if (sec.mr) {
            memory_region_unref(sec.mr);
        }
        return ok;
    default:
        return false;
    }
}

/* host pointer to [addr, addr + len) when it is all VRAM, else NULL */
uint8_t *eg_vram_ptr(EvergreenState *s, uint64_t addr, uint64_t len)
{
    uint32_t fbl = s->regs[EG_MC_VM_FB_LOCATION >> 2];
    uint64_t fb_start = (uint64_t)(fbl & 0xffff) << 24;
    uint64_t fb_end = ((uint64_t)(fbl >> 16) << 24) | 0xffffff;

    if (!fbl || addr < fb_start || addr + len - 1 > fb_end ||
        addr - fb_start + len > s->vram_size) {
        return NULL;
    }
    return s->vram_ptr + (addr - fb_start);
}

/*
 * Host pointers of the guest RAM pages that GART reads touch, per thread
 * (the CP and the raster workers read at the same time).  Vertex fetch
 * reads a few bytes at a time, often from GART buffers in guest RAM, and
 * pci_dma_read costs much more than the copy.  Guest RAM does not move
 * while the VM runs.  Writes keep pci_dma_write (dirty logging).
 */
#define EG_SYS_TLB 64

static uint8_t *eg_sys_host(uint64_t pa)
{
    static __thread struct {
        uint64_t page;
        uint8_t *host;
    } tlb[EG_SYS_TLB];
    uint64_t page = pa >> 12;
    __typeof__(tlb[0]) *e = &tlb[page % EG_SYS_TLB];
    MemoryRegionSection sec;

    if (e->host && e->page == page) {
        return e->host + (pa & 0xfff);
    }
    sec = memory_region_find(get_system_memory(), page << 12, 0x1000);
    if (!sec.mr) {
        return NULL;
    }
    if (memory_region_is_ram(sec.mr) && !memory_region_is_rom(sec.mr) &&
        int128_ge(sec.size, int128_make64(0x1000))) {
        e->page = page;
        e->host = (uint8_t *)memory_region_get_ram_ptr(sec.mr) + sec.offset_within_region;
    } else {
        e->host = NULL;
    }
    memory_region_unref(sec.mr);
    return e->host ? e->host + (pa & 0xfff) : NULL;
}

static bool eg_gpu_access(EvergreenState *s, uint64_t addr, void *buf, size_t len,
                          bool write)
{
    uint8_t *p = buf;

    while (len) {
        uint64_t a, avail;
        EgMemKind k = eg_translate(s, addr, &a, &avail);
        size_t n = MIN(len, avail);

        if (k == EG_MEM_NONE) {
            qemu_log_mask(LOG_GUEST_ERROR, "evergreen: GPU %s of unmapped 0x%010" PRIx64 "\n",
                          write ? "write" : "read", addr);
            if (!write) {
                memset(p, 0, len);
            }
            return false;
        }
        if (k == EG_MEM_VRAM) {
            if (write) {
                memcpy(s->vram_ptr + a, p, n);
                memory_region_set_dirty(&s->vram, a, n);
            } else {
                memcpy(p, s->vram_ptr + a, n);
            }
        } else if (write) {
            pci_dma_write(PCI_DEVICE(s), a, p, n);
        } else {
            uint8_t *h = (a & 0xfff) + n <= 0x1000 ? eg_sys_host(a) : NULL;
            if (h) {
                memcpy(p, h, n);
            } else {
                pci_dma_read(PCI_DEVICE(s), a, p, n);
            }
        }
        addr += n;
        p += n;
        len -= n;
    }
    return true;
}

bool eg_gpu_read(EvergreenState *s, uint64_t addr, void *buf, size_t len)
{
    return eg_gpu_access(s, addr, buf, len, false);
}

bool eg_gpu_write(EvergreenState *s, uint64_t addr, const void *buf, size_t len)
{
    return eg_gpu_access(s, addr, (void *)buf, len, true);
}

/* ------------------------------------------------------------------ */
/* Registers                                                           */
/* ------------------------------------------------------------------ */

static void eg_log_unknown(EvergreenState *s, uint32_t off, bool write, uint32_t val)
{
    uint32_t i = off >> 2;

    if ((s->debug & 1) && i < EG_NUM_REGS && !(s->unknown_logged[i / 8] & (1 << (i % 8)))) {
        s->unknown_logged[i / 8] |= 1 << (i % 8);
        qemu_log("evergreen: first %s of register 0x%05x (0x%08x)\n",
                 write ? "write" : "read", off, val);
    }
}

uint32_t eg_reg_read(EvergreenState *s, uint32_t off)
{
    uint32_t val;

    off &= ~3u;
    if (off >= EG_CTX_BASE && off < EG_CTX_BASE + EG_CTX_SIZE) {
        return s->ctx[(off - EG_CTX_BASE) >> 2];
    }
    if (off >= EG_MMIO_SIZE) {
        return 0;
    }
    if (eg_dce_read(s, off, &val)) {
        return val;
    }
    switch (off) {
    case EG_MM_INDEX:
        return s->mm_index;
    case EG_MM_DATA:
        return eg_reg_read(s, s->mm_index & 0x7ffffffc);
    case EG_PCIE_PORT_DATA:
        return s->pcie_port[s->regs[EG_PCIE_PORT_INDEX >> 2] & 0xff];
    case EG_ROM_DATA: {
        /* the VBIOS flash through registers (Apple's readRegRom): the
         * dword at byte offset ROM_INDEX of the option ROM image */
        PCIDevice *d = PCI_DEVICE(s);
        uint32_t i = s->regs[EG_ROM_INDEX >> 2];
        trace_eg_rom_read(i);
        if (d->has_rom && (uint64_t)i + 4 <= memory_region_size(&d->rom)) {
            return ldl_le_p((uint8_t *)memory_region_get_ram_ptr(&d->rom) + i);
        }
        return 0xffffffff;
    }
    /*
     * The 3D engine is busy while GPU batches run (the guest polls
     * GUI_ACTIVE to wait for them); everything else completes synchronously.
     */
    case EG_GRBM_STATUS:
    case EG_GRBM_STATUS_SE0:
    case EG_GRBM_STATUS_SE1:
        return eg_cp_pending(s) || eg_gfx_busy(s) ?
               EG_GRBM_GUI_ACTIVE | EG_GRBM_CB_BUSY | EG_GRBM_DB_BUSY : 0;
    case EG_CP_STAT:
        return eg_cp_pending(s) ? 1u << 31 : 0;     /* CP_BUSY */
    case EG_SRBM_STATUS:
    case EG_SRBM_STATUS2:
    case EG_VM_L2_STATUS:
        return 0;
    case EG_DMA_STATUS_REG:
        return 1;                               /* DMA_IDLE */
    case EG_PPLL1_CNTL:
    case EG_PPLL2_CNTL:
        /* the pixel PLLs lock at once (SetPixelClock polls bits 21:20) */
        return s->regs[off >> 2] | EG_PPLL_LOCKED;
    case EG_CONFIG_MEMSIZE:
        return s->vram_size / MiB;              /* MB on Evergreen */
    case EG_HFS_STATUS:
        /*
         * The DRM crypto engine at 0x55xx (ATIController::doHfsMode2, used
         * by AppleUpstreamUserClient for Front Row): the driver writes key
         * material and polls for DONE without a timeout.  The computation
         * needs a secret on-chip key; report it done (with zero results)
         * so the caller fails gracefully instead of hanging WindowServer.
         */
        return s->regs[off >> 2] | EG_HFS_DONE;
    case EG_CONFIG_APER_SIZE:
        return memory_region_size(&s->aperture) / MiB;
    case EG_CP_RB_RPTR:
        return s->cp_rptr;
    case EG_DMA_RB_RPTR:
        return s->dma_rptr;
    case EG_IH_RB_WPTR:
        return s->ih_wptr;
    default:
        break;
    }
    val = s->regs[off >> 2];
    eg_log_unknown(s, off, false, val);
    return val;
}

void eg_reg_write(EvergreenState *s, uint32_t off, uint32_t val)
{
    uint32_t idx = off >> 2;

    off &= ~3u;
    if (off >= EG_CTX_BASE && off < EG_CTX_BASE + EG_CTX_SIZE) {
        s->ctx[(off - EG_CTX_BASE) >> 2] = val;
        return;
    }
    if (off >= EG_MMIO_SIZE) {
        return;
    }
    if (eg_dce_write(s, off, val) || eg_hdp_write(s, off, val) ||
        eg_atom_reg_write(s, off, val)) {
        return;
    }
    switch (off) {
    case EG_MM_INDEX:
        s->mm_index = val;
        return;
    case EG_MM_DATA:
        eg_reg_write(s, s->mm_index & 0x7ffffffc, val);
        return;
    case EG_PCIE_PORT_DATA:
        s->pcie_port[s->regs[EG_PCIE_PORT_INDEX >> 2] & 0xff] = val;
        return;
    case EG_VM_CONTEXT0_REQUEST_RESPONSE:
        /* TLB flush request: answer at once (RESPONSE_TYPE 1 = done) */
        s->regs[idx] = (val & ~0xf0u) | (val & 0xf ? 1u << 4 : 0);
        return;
    case EG_CP_RB_WPTR:
        s->regs[idx] = val;
        if (s->cp_async) {
            qemu_bh_schedule(s->cp_bh);         /* the vCPU goes back to the guest */
        } else {
            eg_cp_kick(s);
        }
        return;
    case EG_CP_RB_RPTR_WR:
        s->regs[idx] = val;
        if (s->regs[EG_CP_RB_CNTL >> 2] & EG_RB_RPTR_WR_ENA) {
            s->cp_rptr = val;
        }
        return;
    case EG_CP_RB_CNTL:
        s->regs[idx] = val;
        if (val & EG_RB_RPTR_WR_ENA) {
            s->cp_rptr = s->regs[EG_CP_RB_RPTR_WR >> 2];
        }
        return;
    case EG_DMA_RB_WPTR:
        s->regs[idx] = val;
        eg_dma_kick(s);
        return;
    case EG_DMA_RB_RPTR:
        s->regs[idx] = val;
        s->dma_rptr = val;
        return;
    case EG_DMA_RB_CNTL:
        s->regs[idx] = val;
        if (!(val & EG_DMA_RB_ENABLE)) {
            s->dma_rptr = 0;
        }
        return;
    case EG_IH_RB_WPTR:
        s->ih_wptr = val & ~0xfu;
        s->regs[idx] = val;
        eg_update_irq(s);
        return;
    case EG_IH_RB_RPTR:
    case EG_IH_RB_CNTL:
    case EG_IH_CNTL:
        s->regs[idx] = val;
        eg_update_irq(s);
        return;
    case EG_GRBM_SOFT_RESET:
    case EG_SRBM_SOFT_RESET:
        s->regs[idx] = val;
        return;
    default:
        break;
    }
    eg_log_unknown(s, off, true, val);
    s->regs[idx] = val;
}

/*
 * The graphics ring, as on the real chip, runs while the guest goes on:
 * the WPTR write only schedules this bottom half, and the vCPU returns at
 * once.  The guest sees the CP work through RPTR, the RPTR write-back,
 * fences and the busy bits of GRBM_STATUS and CP_STAT, so the GL thread
 * prepares its next commands while the device runs the last ones.
 * EG_CP_SYNC=1: run the ring in the vCPU that writes WPTR (as before).
 */
static void eg_cp_bh(void *opaque)
{
    EvergreenState *s = opaque;
    int n;

    if (s->cp_unlocked) {
        /* a vCPU runs the CP (DMA ring, fence read) and waits unlocked */
        qemu_bh_schedule(s->cp_bh);
        return;
    }
    /* WPTR can move while the ring runs: go on until it is drained */
    for (n = 0; n < 64 && eg_cp_pending(s); n++) {
        eg_cp_kick(s);
    }
    if (eg_cp_pending(s)) {
        qemu_bh_schedule(s->cp_bh);
    }
}

/*
 * EgVkWaitFn: the CP is about to sleep without touching device state, on a
 * GPU batch or a pipeline compile.  Release the BQL meanwhile so other
 * vCPUs and devices keep running (as the rv530 does).  Until the CP has
 * the BQL again, eg_mmio_enter() keeps other vCPUs out of the device and
 * the device timers wait (eg_gfx_poll).
 */
void eg_cp_wait_cb(void *opaque, bool begin)
{
    EvergreenState *s = opaque;
    static int no_unlock = -1;

    if (no_unlock < 0) {
        no_unlock = getenv("EG_NO_UNLOCK") != NULL;     /* debug */
    }
    if (no_unlock) {
        return;
    }
    /* waits of a thread that did not release the BQL itself: no-ops */
    static __thread int skip;

    if (begin) {
        if (s->cp_unlocked) {
            if (qemu_thread_is_self(&s->unlock_thread)) {
                s->unlock_nest++;
            } else {
                skip++;
            }
            return;
        }
        if (!bql_locked()) {
            skip++;
            return;
        }
        qemu_thread_get_self(&s->unlock_thread);
        s->cp_unlocked = true;
        bql_unlock();
    } else if (skip) {
        skip--;
    } else if (s->unlock_nest) {
        s->unlock_nest--;
    } else if (s->cp_unlocked) {
        bql_lock();
        s->cp_unlocked = false;
        qemu_cond_broadcast(&s->cp_idle);
    }
}

/*
 * 1: go on; 2: a read the register file answers alone; 0: refused.  While
 * the CP waits unlocked, GRBM_STATUS reads show the GPU busy (the driver
 * polls them); other accesses wait for the CP.
 */
static int eg_mmio_enter(EvergreenState *s, uint32_t off, bool write)
{
    if (s->cp_unlocked) {
        if (!write && off >= EG_GRBM_STATUS && off <= EG_GRBM_STATUS_SE1) {
            return 2;
        }
        while (s->cp_unlocked) {
            qemu_cond_wait_bql(&s->cp_idle);
        }
    }
    if (s->mmio_depth) {
        if (!s->mmio_reentered++) {
            warn_report("radeon-hd5770: re-entrant MMIO %s of 0x%05x refused",
                        write ? "write" : "read", off);
        }
        return 0;
    }
    s->mmio_depth++;
    return 1;
}

static uint64_t eg_mmio_read(void *opaque, hwaddr addr, unsigned size)
{
    EvergreenState *s = opaque;
    uint32_t off = addr & ~3u, val;
    int how = eg_mmio_enter(s, off, false);

    if (!how) {
        return 0;
    }
    s->stat_mmio_reads++;
    if (how == 2) {
        val = EG_GRBM_GUI_ACTIVE | EG_GRBM_CB_BUSY | EG_GRBM_DB_BUSY;
        s->stat_busy_reads++;
    } else {
        /*
         * A scratch register is a fence: the rendering before it completes
         * before the guest reads it.  GRBM_STATUS shows running GPU work as
         * busy instead of waiting for it (a wait here holds the BQL), and
         * the other registers do not depend on the rendering.
         */
        if (off >= EG_SCRATCH_REG0 && off < EG_SCRATCH_REG0 + 32 && eg_gfx_pending(s)) {
            eg_gfx_flush(s);
            s->stat_scratch_flushes++;
        }
        val = eg_reg_read(s, off);
        if (off >= EG_GRBM_STATUS && off <= EG_GRBM_STATUS_SE1 && val) {
            s->stat_busy_reads++;
        }
        s->mmio_depth--;
    }
    trace_eg_mmio_read(addr, val);
    return (val >> ((addr & 3) * 8)) & (size == 4 ? 0xffffffffu : (1u << (size * 8)) - 1);
}

static void eg_mmio_write(void *opaque, hwaddr addr, uint64_t data, unsigned size)
{
    EvergreenState *s = opaque;
    uint32_t off = addr & ~3u;
    uint32_t val = data;

    if (!eg_mmio_enter(s, off, true)) {
        return;
    }
    if (size < 4) {
        uint32_t shift = (addr & 3) * 8;
        uint32_t mask = ((1u << (size * 8)) - 1) << shift;
        val = (s->regs[off >> 2] & ~mask) | ((uint32_t)(data << shift) & mask);
    }
    trace_eg_mmio_write(addr, val);
    eg_reg_write(s, off, val);
    s->mmio_depth--;
}

/*
 * The register BAR runs without the BQL (memory_region_clear_global_locking):
 * the CP holds the BQL for a whole ring kick in the main loop, and a vCPU
 * that rings the doorbell for its next command buffer waited all that time
 * to enter the device.  The WPTR write of the asynchronous CP only stores
 * the pointer and schedules the CP, which needs no lock.  Every other
 * access takes the BQL, as before.
 */
static uint64_t eg_mmio_read_bql(void *opaque, hwaddr addr, unsigned size)
{
    bool locked = bql_locked();
    uint64_t v;

    if (!locked) {
        bql_lock();
    }
    v = eg_mmio_read(opaque, addr, size);
    if (!locked) {
        bql_unlock();
    }
    return v;
}

static void eg_mmio_write_bql(void *opaque, hwaddr addr, uint64_t data, unsigned size)
{
    EvergreenState *s = opaque;
    bool locked = bql_locked();

    if (size == 4 && addr == EG_CP_RB_WPTR && s->cp_async && !s->no_lockless) {
        qatomic_set(&s->regs[EG_CP_RB_WPTR >> 2], (uint32_t)data);
        qemu_bh_schedule(s->cp_bh);
        return;
    }
    if (!locked) {
        bql_lock();
    }
    eg_mmio_write(opaque, addr, data, size);
    if (!locked) {
        bql_unlock();
    }
}

static const MemoryRegionOps eg_mmio_ops = {
    .read = eg_mmio_read_bql,
    .write = eg_mmio_write_bql,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
    .impl = { .min_access_size = 1, .max_access_size = 4 },
};

/* ------------------------------------------------------------------ */
/* Device                                                              */
/* ------------------------------------------------------------------ */

static void eg_reset(DeviceState *dev)
{
    EvergreenState *s = EVERGREEN(dev);

    memset(s->regs, 0, sizeof(s->regs));
    memset(s->ctx, 0, sizeof(s->ctx));
    memset(s->pcie_port, 0, sizeof(s->pcie_port));
    s->mm_index = 0;
    s->ih_wptr = 0;
    s->cp_rptr = 0;
    s->dma_rptr = 0;
    if (s->cp_bh) {
        qemu_bh_cancel(s->cp_bh);
    }
    s->irq_level = false;

    /* posted: the VBIOS has set up the memory controller */
    s->regs[EG_MC_SHARED_CHMAP >> 2] = EG_MC_CHMAP_4CH;
    s->regs[EG_MC_ARB_RAMCFG >> 2] = 0x00000010;    /* NOOFCOLS 1 */
    s->regs[0x98f8 >> 2] = JUNIPER_GB_ADDR_CONFIG;  /* GB_ADDR_CONFIG */
    s->regs[EG_CG_SPLL_STATUS >> 2] = 1u << 1;      /* SPLL_CHG_STATUS */
    s->regs[EG_MC_VM_FB_LOCATION >> 2] =
        (((s->vram_size - 1) >> 24) << 16) & 0xffff0000;
    eg_dce_reset(s);
    eg_gfx_reset(s);
    eg_hdp_reset(s);
}

static void eg_realize(PCIDevice *dev, Error **errp)
{
    EvergreenState *s = EVERGREEN(dev);
    uint8_t *conf = dev->config;

    if (s->vram_size_mb < 64 || s->vram_size_mb > 1024 ||
        !is_power_of_2(s->vram_size_mb)) {
        error_setg(errp, "radeon-hd5770: vram_size_mb must be a power of two "
                   "between 64 and 1024");
        return;
    }
    s->vram_size = (uint64_t)s->vram_size_mb * MiB;

    pci_set_word(conf + PCI_DEVICE_ID, s->device_id);
    pci_set_word(conf + PCI_SUBSYSTEM_VENDOR_ID, s->subsys_vendor);
    pci_set_word(conf + PCI_SUBSYSTEM_ID, s->subsys_id);
    pci_set_byte(conf + PCI_REVISION_ID, s->revision);
    conf[PCI_INTERRUPT_PIN] = 1;

    /* BAR0: VRAM aperture (the first 256 MiB of VRAM, as on the real card) */
    memory_region_init_ram(&s->vram, OBJECT(s), "evergreen.vram", s->vram_size,
                           &error_fatal);
    s->vram_ptr = memory_region_get_ram_ptr(&s->vram);
    memory_region_set_log(&s->vram, true, DIRTY_MEMORY_VGA);
    memory_region_init_alias(&s->aperture, OBJECT(s), "evergreen.aperture",
                             &s->vram, 0, MIN(s->vram_size, 256 * MiB));
    /* 32-bit BARs: firmware then places them below 4 GiB, which Mac OS X
     * 10.6's IOPCIFamily needs (the real card's BARs are 64-bit, but a Mac
     * Pro's EFI keeps them low too) */
    memory_region_init(&s->bar0, OBJECT(s), "evergreen.bar0", MIN(s->vram_size, 256 * MiB));
    memory_region_add_subregion(&s->bar0, 0, &s->aperture);    /* HDP apertures overlay it */
    pci_register_bar(dev, 0, PCI_BASE_ADDRESS_MEM_PREFETCH, &s->bar0);

    memory_region_init_io(&s->mmio, OBJECT(s), &eg_mmio_ops, s,
                          "evergreen.mmio", EG_MMIO_SIZE);
    s->mmio.disable_reentrancy_guard = true;    /* see eg_mmio_enter() */
    /* EG_MMIO_BQL=1: the whole BAR under the BQL (eg_mmio_write_bql) */
    s->no_lockless = getenv("EG_MMIO_BQL") != NULL;
    if (!s->no_lockless) {
        memory_region_enable_lockless_io(&s->mmio);
    }
    qemu_cond_init(&s->cp_idle);
    s->cp_bh = qemu_bh_new(eg_cp_bh, s);
    s->cp_async = !getenv("EG_CP_SYNC");
    pci_register_bar(dev, 2, PCI_BASE_ADDRESS_SPACE_MEMORY, &s->mmio);

    memory_region_init_alias(&s->io, OBJECT(s), "evergreen.io", &s->mmio, 0, 0x100);
    pci_register_bar(dev, 4, PCI_BASE_ADDRESS_SPACE_IO, &s->io);

    if (pci_bus_is_express(pci_get_bus(dev))) {
        int ret = pcie_endpoint_cap_init(dev, 0x58);
        assert(ret > 0);
    } else {
        dev->cap_present &= ~QEMU_PCI_CAP_EXPRESS;
    }
    if (msi_init(dev, 0xa0, 1, true, false, errp) < 0) {
        return;
    }
    eg_gfx_init(s);
    eg_dce_init(s);
    eg_gfx_vk_init(s);
}

static void eg_exit(PCIDevice *dev)
{
    EvergreenState *s = EVERGREEN(dev);

    eg_dce_fini(s);
    eg_gfx_vk_fini(s);
    g_free(s->gfx);
    s->gfx = NULL;
    msi_uninit(dev);
}

static const Property eg_properties[] = {
    DEFINE_PROP_UINT32("vram_size_mb", EvergreenState, vram_size_mb, 1024),
    DEFINE_PROP_UINT32("device-id", EvergreenState, device_id, 0x68b8),
    DEFINE_PROP_UINT32("subsys-vendor-id", EvergreenState, subsys_vendor, 0x106b),
    DEFINE_PROP_UINT32("subsys-id", EvergreenState, subsys_id, 0x00cf),
    DEFINE_PROP_UINT32("revision", EvergreenState, revision, 0x00),
    /* 1: first access of each register, 2: unhandled PM4, 4: every draw,
     * 8: the first vertices of each draw, 16: every PM4 packet, 32: HDP apertures */
    DEFINE_PROP_UINT32("debug", EvergreenState, debug, 1),
    /* 3D on the host GPU (falls back to the CPU path without Vulkan) */
    DEFINE_PROP_BOOL("vulkan", EvergreenState, vulkan, true),
    DEFINE_PROP_UINT32("xres", EvergreenState, xres, 1440),
    DEFINE_PROP_UINT32("yres", EvergreenState, yres, 900),
    /* the Apple HD 5770 VBIOS: DVI-I on DDC pair 3 (0x6450) and HPD1;
     * DisplayPort DP-1 on 0x6440/HPD4, DP-2 on 0x6430/HPD5 */
    DEFINE_PROP_UINT32("monitor-ddc", EvergreenState, monitor_ddc, 2),
    DEFINE_PROP_UINT32("monitor-hpd", EvergreenState, monitor_hpd, 0),
};

static void eg_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);

    k->realize = eg_realize;
    k->exit = eg_exit;
    k->vendor_id = 0x1002;
    k->device_id = 0x68b8;
    k->class_id = PCI_CLASS_DISPLAY_VGA;
    dc->desc = "ATI Radeon HD 5770 (Juniper, Evergreen)";
    device_class_set_legacy_reset(dc, eg_reset);
    device_class_set_props(dc, eg_properties);
    dc->hotpluggable = false;
    set_bit(DEVICE_CATEGORY_DISPLAY, dc->categories);
}

/* debug: qom-set <gpu> dump "VRAM_OFFSET LENGTH /host/path" writes a VRAM range */
static void eg_set_dump(Object *obj, const char *val, Error **errp)
{
    EvergreenState *s = EVERGREEN(obj);
    uint64_t off, len;
    char path[256];
    FILE *f;

    if (sscanf(val, "%" SCNi64 " %" SCNi64 " %255s", &off, &len, path) != 3 ||
        off > s->vram_size || len > s->vram_size - off) {
        error_setg(errp, "dump: \"OFFSET LENGTH PATH\" within VRAM");
        return;
    }
    f = fopen(path, "wb");
    if (!f) {
        error_setg_errno(errp, errno, "dump: %s", path);
        return;
    }
    if (fwrite(s->vram_ptr + off, 1, len, f) != len) {
        error_setg(errp, "dump: short write");
    }
    fclose(f);
}

static void eg_instance_init(Object *obj)
{
    EvergreenState *s = EVERGREEN(obj);
    int i;

    PCI_DEVICE(obj)->cap_present |= QEMU_PCI_CAP_EXPRESS;
    /* the debug flags, writable at run time: qom-set <gpu> dbg N */
    object_property_add_uint32_ptr(obj, "dbg", &s->debug, OBJ_PROP_FLAG_READWRITE);
    object_property_add_str(obj, "dump", NULL, eg_set_dump);
    for (i = 0; i < EG_NUM_DDC; i++) {
        char name[16];
        snprintf(name, sizeof(name), "ddc%d", i + 1);
        object_initialize_child(obj, name, &s->ddc[i].edid, TYPE_RV530_EDID);
    }
}

static const TypeInfo eg_info = {
    .name = TYPE_EVERGREEN,
    .parent = TYPE_PCI_DEVICE,
    .instance_size = sizeof(EvergreenState),
    .instance_init = eg_instance_init,
    .class_init = eg_class_init,
    .interfaces = (const InterfaceInfo[]) {
        { INTERFACE_PCIE_DEVICE },
        { INTERFACE_CONVENTIONAL_PCI_DEVICE },
        { },
    },
};

static void eg_register_types(void)
{
    type_register_static(&eg_info);
}

type_init(eg_register_types)
