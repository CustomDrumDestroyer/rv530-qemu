/*
 * QEMU ATI Radeon X1600 (RV530, R5xx family) emulation.
 *
 * PCI device, register file, indirect register spaces, interrupts,
 * tracing and the polling-loop detector.  Subsystems:
 *   r5xx_mc.c     memory controller, VRAM aperture, PCIe GART
 *   r5xx_cp.c     command processor (ring, indirect buffers, PM4)
 *   r5xx_avivo.c  AVIVO display engine, cursor, vblank, DDC
 *   r5xx_3d.c     3D state, vertex fetch, draw dispatch (CPU path)
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/units.h"
#include "qemu/error-report.h"
#include "qemu/main-loop.h"
#include "qapi/error.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "hw/pci/pci.h"
#include "hw/pci/pcie.h"
#include "hw/display/edid.h"
#include "migration/vmstate.h"
#include "rv530.h"
#include "trace.h"

#define RV_DEBUG_UNKNOWN  (1u << 0)
#define RV_DEBUG_ALLREGS  (1u << 1)
#define RV_DEBUG_PM4      (1u << 2)
#define RV_DEBUG_3D       (1u << 3)
#define RV_DEBUG_POLL     (1u << 4)

#define PCI_VENDOR_ID_ATI_   0x1002
#define PCI_VENDOR_ID_APPLE_ 0x106b

/* ------------------------------------------------------------------ */
/* Logging helpers                                                     */
/* ------------------------------------------------------------------ */

static const char *regname_or_hex(uint32_t off, char *buf, size_t len)
{
    const char *n = r5xx_regname(off);
    if (n) {
        return n;
    }
    snprintf(buf, len, "0x%04x", off);
    return buf;
}

void rv530_log_unknown(RV530State *s, const char *what, uint32_t off,
                       uint32_t val, bool is_write)
{
    uint32_t idx = off >> 2;
    if (idx < R5XX_NUM_REGS) {
        if (s->unknown_logged[idx >> 3] & (1 << (idx & 7))) {
            return;
        }
        s->unknown_logged[idx >> 3] |= 1 << (idx & 7);
    }
    char buf[16];
    qemu_log_mask(LOG_UNIMP, "rv530: unhandled %s %s %s (0x%04x) val=0x%08x\n",
                  what, is_write ? "write" : "read",
                  regname_or_hex(off, buf, sizeof(buf)), off, val);
}

/* ------------------------------------------------------------------ */
/* Interrupts                                                          */
/* ------------------------------------------------------------------ */

static uint32_t rv530_gen_int_status(RV530State *s)
{
    uint32_t st = 0;
    if (s->sw_int_pending) {
        st |= RV_GEN_INT_SW_INT;
    }
    if (rv530_avivo_irq_pending(s)) {
        st |= RV_GEN_INT_DISPLAY_STAT;
    }
    return st;
}

void rv530_update_irq(RV530State *s)
{
    uint32_t cntl = s->regs[RV_GEN_INT_CNTL >> 2];
    bool level = false;

    if ((cntl & RV_GEN_INT_SW_INT_EN) && s->sw_int_pending) {
        level = true;
    }
    if (rv530_avivo_irq_pending(s)) {
        level = true;
    }
    if (level != s->irq_level) {
        s->irq_level = level;
        trace_rv530_irq(level, cntl, s->sw_int_pending);
    }
    pci_set_irq(PCI_DEVICE(s), level);
}

/* ------------------------------------------------------------------ */
/* Register file                                                       */
/* ------------------------------------------------------------------ */

static uint32_t rv530_rbbm_status(RV530State *s)
{
    uint32_t v = RV_RBBM_CMDFIFO_AVAIL;
    if (s->cp_busy) {
        v |= RV_RBBM_GUI_ACTIVE | RV_RBBM_CP_CMDSTRM_BUSY;
    }
    return v;
}

static void rv530_poll_detect(RV530State *s, uint32_t off, uint32_t val)
{
    if (off == s->poll_last_off && val == s->poll_last_val) {
        s->poll_count++;
        if (s->poll_count == 500 && !s->poll_reported) {
            char buf[16];
            s->poll_reported = true;
            qemu_log_mask(LOG_GUEST_ERROR,
                          "rv530: polling loop: %s (0x%04x) read %u times, "
                          "value 0x%08x unchanged\n",
                          regname_or_hex(off, buf, sizeof(buf)), off,
                          s->poll_count, val);
            trace_rv530_poll_loop(off, val, s->poll_count);
        }
    } else {
        s->poll_last_off = off;
        s->poll_last_val = val;
        s->poll_count = 1;
        s->poll_reported = false;
    }
}

uint32_t rv530_reg_read(RV530State *s, uint32_t off)
{
    uint32_t val;
    uint32_t idx = off >> 2;

    off &= ~3u;

    switch (off) {
    case RV_MM_INDEX:
        return s->mm_index;
    case RV_MM_DATA:
        return rv530_reg_read(s, s->mm_index & 0xfffc);
    case RV_CLOCK_CNTL_INDEX:
        return s->pll_index;
    case RV_CLOCK_CNTL_DATA:
        val = s->pll_regs[s->pll_index & 0x3f];
        if ((s->pll_index & 0x3f) == 0x34) {
            /* chip id strap: ATIRadeonX1000 keys its pipe/GART setup off it */
            val = (s->device_id & 0xffff) | ((s->revision & 0xf) << 24);
        }
        trace_rv530_pll_read(s->pll_index & 0x3f, val);
        return val;
    case RV_PCIE_INDEX:
        return s->pcie_index;
    case RV_PCIE_DATA:
        return rv530_pcie_reg_read(s, s->pcie_index & 0xff);
    case RV_MC_IND_INDEX:
        return s->mc_index;
    case RV_MC_IND_DATA:
        return rv530_mc_reg_read(s, s->mc_index & 0xff);
    case RV_P1PLL_CNTL:
    case RV_P2PLL_CNTL:
        /* the pixel PLLs lock instantly */
        return s->regs[idx] | RV_PPLL_LOCKED;
    case RV_GEN_INT_STATUS:
        return rv530_gen_int_status(s);
    case RV_WAIT_UNTIL:
        /* reads report engine-clean status (bits 16/17); Apple's driver
         * polls them in its idle loop.  The engines here are synchronous. */
        return s->regs[idx] | (3u << 16);
    case RV_RBBM_STATUS:
        if (s->cp_busy) {
            rv530_cp_run(s);
        }
        return rv530_rbbm_status(s);
    case RV_MC_STATUS_LEGACY:
        return 1u << 2;             /* RADEON_MC_IDLE */
    case RV_CONFIG_APER_SIZE:
        return 256 * MiB;
    case RV_CP_STAT:
        return 0;
    default:
        break;
    }

    if (rv530_cp_reg_read(s, off, &val)) {
        return val;
    }
    if (off >= 0x6000 && off < 0x8000) {
        if (rv530_avivo_reg_read(s, off, &val)) {
            return val;
        }
    }
    if (rv530_2d_reg_read(s, off, &val)) {
        return val;
    }
    if (rv530_gfx_reg_read(s, off, &val)) {
        return val;
    }
    if (off >= RV_SCRATCH_REG0 && off <= RV_SCRATCH_REG7 && s->cp_busy) {
        rv530_cp_run(s);
    }
    return s->regs[idx];
}

void rv530_reg_write(RV530State *s, uint32_t off, uint32_t val)
{
    uint32_t idx = off >> 2;

    off &= ~3u;

    switch (off) {
    case RV_MM_INDEX:
        s->mm_index = val;
        return;
    case RV_MM_DATA:
        rv530_reg_write(s, s->mm_index & 0xfffc, val);
        return;
    case RV_CLOCK_CNTL_INDEX:
        s->pll_index = val;
        return;
    case RV_CLOCK_CNTL_DATA:
        trace_rv530_pll_write(s->pll_index & 0x3f, val);
        s->pll_regs[s->pll_index & 0x3f] = val;
        return;
    case RV_PCIE_INDEX:
        s->pcie_index = val;
        return;
    case RV_PCIE_DATA:
        rv530_pcie_reg_write(s, s->pcie_index & 0xff, val);
        return;
    case RV_MC_IND_INDEX:
        s->mc_index = val;
        return;
    case RV_MC_IND_DATA:
        if (s->mc_index & RV_MC_IND_WR_EN) {
            rv530_mc_reg_write(s, s->mc_index & 0xff, val);
        }
        return;
    case RV_GEN_INT_CNTL:
        s->regs[idx] = val;
        rv530_update_irq(s);
        return;
    case RV_GEN_INT_STATUS:
        if (val & RV_GEN_INT_SW_INT_FIRE) {
            rv530_gfx_flush(s);
            s->sw_int_pending = true;
        }
        if (val & RV_GEN_INT_SW_INT) {
            s->sw_int_pending = false;
        }
        rv530_update_irq(s);
        return;
    case RV_RBBM_SOFT_RESET:
        s->regs[idx] = val;
        if (val & RV_SOFT_RESET_CP) {
            rv530_cp_reset(s);
        }
        return;
    case RV_CONFIG_MEMSIZE:
        s->regs[idx] = val;
        return;
    case RV_WAIT_UNTIL:
        return;
    default:
        break;
    }

    if (off >= RV_SCRATCH_REG0 && off <= RV_SCRATCH_REG7) {
        uint32_t n = (off - RV_SCRATCH_REG0) >> 2;
        /*
         * The fence must not pass the work, whether the guest reads it from
         * memory (SCRATCH_UMSK) or polls the register: another vCPU's
         * register read is served while a CP run waits unlocked, and saw
         * the new value while the batch's results were still only in the
         * GPU's copy of VRAM.  Flush first, then publish the value.
         */
        rv530_gfx_flush(s);
        s->regs[idx] = val;
        trace_rv530_scratch_write(n, val);
        if (s->regs[RV_SCRATCH_UMSK >> 2] & (1u << n)) {
            uint32_t addr = s->regs[RV_SCRATCH_ADDR >> 2] + n * 4;
            trace_rv530_scratch_wb(n, val, addr);
            rv530_gpu_write32(s, addr, val);
        }
        return;
    }
    if (rv530_cp_reg_write(s, off, val)) {
        return;
    }
    if (off >= RV_ATOM_MAILBOX && rv530_atom_reg_write(s, off, val)) {
        return;
    }
    if (rv530_2d_reg_write(s, off, val)) {
        return;
    }
    if (off >= 0x6000 && off < 0x8000) {
        if (rv530_avivo_reg_write(s, off, val)) {
            return;
        }
    } else if (off == RV_VGA_RENDER_CONTROL || off == RV_D1VGA_CONTROL ||
               off == RV_D2VGA_CONTROL) {
        rv530_avivo_reg_write(s, off, val);
    }
    if (rv530_gfx_reg_write(s, off, val)) {
        return;
    }
    if (off >= 0x0b00 && off <= 0x0b7c) {
        /* SURFACE_CNTL / SURFACEn_*: consulted by rv530_linear_surface() */
        s->regs[idx] = val;
        return;
    }
    if (s->debug & RV_DEBUG_UNKNOWN) {
        rv530_log_unknown(s, "register", off, val, true);
    }
    s->regs[idx] = val;
}

/* ------------------------------------------------------------------ */
/* MMIO                                                                */
/* ------------------------------------------------------------------ */

/*
 * Report device work that holds the BQL (and, for MMIO, keeps the vCPU out
 * of the guest) for long: an SMP Snow Leopard guest panics when a CPU does
 * not answer a TLB shootdown IPI within 250 ms.  RV530_STALL_MS sets the
 * threshold (default 100).
 */
static int64_t rv530_stall_ns = -1;

void rv530_stall_start(RV530State *s, RV530Stall *st)
{
    if (rv530_stall_ns < 0) {
        const char *e = getenv("RV530_STALL_MS");
        rv530_stall_ns = (e ? atoi(e) : 100) * 1000000ll;
    }
    memcpy(st->ns, s->prof_ns, sizeof(st->ns));
    memcpy(st->n, s->prof_n, sizeof(st->n));
    st->unlocked_ns = s->unlocked_ns;
    st->t0 = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
}

void rv530_stall_end(RV530State *s, const RV530Stall *st, const char *what,
                     uint32_t off)
{
    static const char *const names[RV_PROF_N] = {
        "vk-path", "vk-submit", "flush", "cpu-draw", "2d", "unlocked",
        "setup", "vk-build", "vk-prims", "dirty-log",
    };
    int64_t total = qemu_clock_get_ns(QEMU_CLOCK_REALTIME) - st->t0;
    int64_t unlocked = s->unlocked_ns - st->unlocked_ns;
    int64_t dt = total - unlocked;     /* time the BQL was actually held */

    if (rv530_stall_ns > 0 && dt >= rv530_stall_ns) {
        char buf[16], detail[512];
        size_t len = 0;
        int i;

        detail[0] = 0;
        for (i = 0; i < RV_PROF_N; i++) {
            if (s->prof_n[i] != st->n[i]) {
                len += snprintf(detail + len, sizeof(detail) - len, " %s %u/%.1fms",
                                names[i], s->prof_n[i] - st->n[i],
                                (s->prof_ns[i] - st->ns[i]) / 1e6);
            }
        }
        trace_rv530_stall(what, regname_or_hex(off, buf, sizeof(buf)),
                          (uint32_t)(dt / 1000000));
        if (unlocked) {
            len += snprintf(detail + len, sizeof(detail) - len,
                            " (plus %" PRId64 " ms without the BQL)", unlocked / 1000000);
        }
        warn_report("rv530: %s %s held the BQL for %" PRId64 " ms:%s", what,
                    regname_or_hex(off, buf, sizeof(buf)), dt / 1000000, detail);
    }
}

/*
 * A CP run waiting for a shader compile has released the BQL: keep other
 * vCPUs out of the device until it has taken it back.
 */
static void rv530_wait_cp_locked(RV530State *s)
{
    while (s->cp_unlocked) {
        qemu_cond_wait_bql(&s->cp_idle);
    }
}

/*
 * Interrupt status, acknowledge and display timing registers only touch
 * IRQ and CRTC state, which the vblank timer changes during a CP wait
 * anyway.  The guest's interrupt handler reads them from another vCPU
 * while a CP run waits for a shader compile; it need not wait too.
 */
static bool rv530_irq_reg(uint32_t off, bool write, uint32_t val)
{
    uint32_t crtc = off >= 0x6800 && off < 0x7000 ? off - 0x800 : off;

    switch (crtc) {
    case RV_D1MODE_VBLANK_STATUS:
        return true;
    case RV_D1CRTC_STATUS:
    case RV_D1CRTC_STATUS_POSITION:
    case RV_D1CRTC_FRAME_COUNT:
    case RV_D1CRTC_STATUS_HV_COUNT:
        return !write;
    }
    switch (off) {
    case RV_MSI_REARM_EN:           /* the handler's last write */
        return true;
    case RV_DISP_INTERRUPT_STATUS:
        return !write;
    case RV_GEN_INT_STATUS:
        return !write || !(val & RV_GEN_INT_SW_INT_FIRE);   /* FIRE flushes */
    }
    return false;
}

/*
 * Reads during a CP run that waits unlocked are served at once, without
 * waiting for the run: the CP thread touches no device state while it
 * waits, so the registers are consistent, and the reads that would run the
 * CP (RBBM_STATUS, CP_RB_RPTR) do nothing then (rv530_cp_run sees
 * cp_unlocked) and report the engine busy, as hardware does mid-run.  A
 * run that waits on shader compiles and GPU batches can last long enough
 * for a vCPU held here to miss TLB shootdowns ("Unresponsive processor"
 * panics); ATIRadeonX1000's GPUSensorFunc timer polls RBBM_STATUS.
 */

/*
 * Enter an MMIO handler: 1 to go ahead, 2 for a read or an interrupt register
 * served while a CP run waits unlocked (no mmio_depth accounting), 0 to refuse a
 * same-thread re-entry (see mmio_depth).
 */
static int rv530_mmio_enter(RV530State *s, uint32_t off, bool write, uint32_t val)
{
    if (s->cp_unlocked) {
        int64_t t0;
        if (!write || rv530_irq_reg(off, write, val)) {
            return 2;
        }
        t0 = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
        rv530_wait_cp_locked(s);
        trace_rv530_mmio_wait(off, write,
                              (qemu_clock_get_ns(QEMU_CLOCK_REALTIME) - t0) / 1000);
    }
    if (s->mmio_depth) {
        if (!s->mmio_reentered++) {
            warn_report("rv530: re-entrant MMIO %s of 0x%04x refused",
                        write ? "write" : "read", off);
        }
        return 0;
    }
    s->mmio_depth++;
    return 1;
}

/*
 * R5xxVkWaitFn: the CP thread is about to sleep without touching device
 * state, either on a pending pipeline (r5xx_vk.c) or on the CPU raster
 * workers (r5xx_3d.c).  Release the BQL meanwhile so other vCPUs and
 * devices keep running.
 */
void rv530_vk_wait_cb(void *opaque, bool begin)
{
    RV530State *s = opaque;
    static int no_unlock = -1;

    if (no_unlock < 0) {
        no_unlock = getenv("RV530_NO_UNLOCK") != NULL;  /* debug */
    }
    if (no_unlock) {
        return;
    }

    if (begin) {
        if (s->cp_unlocked) {
            s->unlock_nest++;       /* already released by an outer wait */
            return;
        }
        if (!bql_locked()) {
            return;
        }
        s->unlocked_t0 = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
        s->cp_unlocked = true;
        bql_unlock();
    } else if (s->unlock_nest) {
        s->unlock_nest--;
    } else if (s->cp_unlocked) {
        int64_t dt;

        bql_lock();
        s->cp_unlocked = false;
        dt = qemu_clock_get_ns(QEMU_CLOCK_REALTIME) - s->unlocked_t0;
        s->unlocked_ns += dt;
        s->prof_ns[RV_PROF_WAIT] += dt;
        s->prof_n[RV_PROF_WAIT]++;
        qemu_cond_broadcast(&s->cp_idle);
    }
}

static uint64_t rv530_mmio_read(void *opaque, hwaddr addr, unsigned size)
{
    RV530State *s = opaque;
    uint32_t off = addr & ~3u;
    RV530Stall st;
    uint32_t val;
    int how = rv530_mmio_enter(s, off, false, 0);

    if (!how) {
        return 0;
    }
    if (how == 2) {
        return (rv530_reg_read(s, off) >> ((addr & 3) * 8)) &
               (size < 4 ? (1ull << (size * 8)) - 1 : 0xffffffffull);
    }
    rv530_stall_start(s, &st);
    val = rv530_reg_read(s, off);
    uint32_t shift = (addr & 3) * 8;
    uint64_t ret = val >> shift;

    if (size < 4) {
        ret &= (1ull << (size * 8)) - 1;
    }
    if (trace_event_get_state_backends(TRACE_RV530_MMIO_READ)) {
        char buf[16];
        trace_rv530_mmio_read(size, addr, regname_or_hex(off, buf, sizeof(buf)),
                              ret);
    }
    rv530_poll_detect(s, off, val);
    rv530_stall_end(s, &st, "MMIO read", off);
    s->mmio_depth--;
    return ret;
}

static void rv530_mmio_write(void *opaque, hwaddr addr, uint64_t data,
                             unsigned size)
{
    RV530State *s = opaque;
    uint32_t off = addr & ~3u;
    uint32_t val = data;
    int how = rv530_mmio_enter(s, off, true, (uint32_t)(data << ((addr & 3) * 8)));

    if (!how) {
        return;
    }
    if (size < 4) {
        uint32_t shift = (addr & 3) * 8;
        uint32_t mask = ((1u << (size * 8)) - 1) << shift;
        uint32_t old = (off == RV_CLOCK_CNTL_INDEX) ? s->pll_index
                                                    : s->regs[off >> 2];
        val = (old & ~mask) | ((uint32_t)(data << shift) & mask);
    }
    if (how == 2) {
        rv530_reg_write(s, off, val);
        return;
    }
    if (trace_event_get_state_backends(TRACE_RV530_MMIO_WRITE)) {
        char buf[16];
        trace_rv530_mmio_write(size, addr, regname_or_hex(off, buf, sizeof(buf)),
                               data);
    }
    s->poll_count = 0;
    {
        RV530Stall st;
        rv530_stall_start(s, &st);
        rv530_reg_write(s, off, val);
        rv530_stall_end(s, &st, "MMIO write", off);
    }
    s->mmio_depth--;
}

static const MemoryRegionOps rv530_mmio_ops = {
    .read = rv530_mmio_read,
    .write = rv530_mmio_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .impl.min_access_size = 1,
    .impl.max_access_size = 4,
};

/* ------------------------------------------------------------------ */
/* Device                                                              */
/* ------------------------------------------------------------------ */

static void rv530_reset(DeviceState *dev)
{
    RV530State *s = RV530(dev);

    memset(s->regs, 0, sizeof(s->regs));
    /* DP_CNTL resets to left-to-right / top-to-bottom; Apple's driver
     * never programs it */
    s->regs[0x16c0 >> 2] = 0x3;
    s->r2d_legacy[0] = s->r2d_legacy[1] = false;
    memset(s->mc_regs, 0, sizeof(s->mc_regs));
    memset(s->pcie_regs, 0, sizeof(s->pcie_regs));
    memset(s->pll_regs, 0, sizeof(s->pll_regs));
    /* posted clocks (as left by the VBIOS ASIC_Init on real cards):
     * SPLL (PLL 0x00) and MPLL (PLL 0x04) feedback dividers in bits 5-12,
     * reference divider in bits 2-4, 27 MHz reference.  Apple's driver
     * derives SCLK = (fb+1)*27/(ref+1)/2 = 594 MHz and MCLK = 405 MHz from
     * them, and caps the display pixel clock at SCLK. */
    s->pll_regs[0x00] = 43 << 5;
    s->pll_regs[0x04] = 29 << 5;
    memset(s->unknown_logged, 0, sizeof(s->unknown_logged));
    s->mm_index = s->mc_index = s->pcie_index = s->pll_index = 0;
    s->sw_int_pending = false;
    s->poll_count = 0;

    /* A posted card: memory size known, CRTC enabled (r5xx_avivo). */
    s->regs[RV_CONFIG_MEMSIZE >> 2] = s->vram_size;
    s->regs[RV_HOST_PATH_CNTL >> 2] = 0;
    /* GB_PIPE_SELECT: RV530 has 1 quad pipe (bits 12-13 = pipes - 1) */
    s->regs[RV_GB_PIPE_SELECT >> 2] = 0x00000000;
    s->regs[RV_GB_PIPE_SELECT2 >> 2] = 0x00000003;   /* 2 z pipes */
    s->regs[RV_DST_PIPE_CONFIG >> 2] = 0;
    /* RBBM_SOFT_RESET reads back as 0 */

    rv530_mc_reset(s);
    rv530_cp_reset(s);
    rv530_avivo_reset(s);
    rv530_gfx_reset(s);
    rv530_update_irq(s);
}

static void rv530_realize(PCIDevice *dev, Error **errp)
{
    RV530State *s = RV530(dev);
    uint8_t *pci_conf = dev->config;

    if (s->vram_size_mb < 32 || s->vram_size_mb > 256 ||
        !is_power_of_2(s->vram_size_mb)) {
        error_setg(errp, "rv530: vram_size_mb must be a power of two "
                   "between 32 and 256");
        return;
    }
    s->vram_size = s->vram_size_mb * MiB;
    qemu_cond_init(&s->cp_idle);

    pci_set_word(pci_conf + PCI_DEVICE_ID, s->device_id);
    pci_set_word(pci_conf + PCI_SUBSYSTEM_VENDOR_ID, s->subsys_vendor);
    pci_set_word(pci_conf + PCI_SUBSYSTEM_ID, s->subsys_id);
    pci_set_byte(pci_conf + PCI_REVISION_ID, s->revision);
    pci_conf[PCI_INTERRUPT_PIN] = 1;

    /* BAR0: 256 MiB prefetchable aperture, VRAM at offset 0 */
    memory_region_init(&s->aperture, OBJECT(s), "rv530.aperture", 256 * MiB);
    memory_region_init_ram(&s->vram, OBJECT(s), "rv530.vram", s->vram_size,
                           &error_fatal);
    memory_region_add_subregion(&s->aperture, 0, &s->vram);
    s->vram_ptr = memory_region_get_ram_ptr(&s->vram);
    memory_region_set_log(&s->vram, true, DIRTY_MEMORY_VGA);
    /* Real X1600 layout: BAR0 32-bit prefetchable VRAM, BAR1 I/O, BAR2
     * registers.  Mac OS X 10.6's IOPCIFamily cannot map a VRAM BAR that
     * firmware placed above 4 GiB, which a 64-bit BAR allows. */
    pci_register_bar(dev, 0, PCI_BASE_ADDRESS_MEM_PREFETCH, &s->aperture);

    /* BAR2: 64 KiB register MMIO */
    memory_region_init_io(&s->mmio, OBJECT(s), &rv530_mmio_ops, s,
                          "rv530.mmio", R5XX_REG_SPACE_SIZE);
    s->mmio.disable_reentrancy_guard = true;     /* see mmio_depth */
    pci_register_bar(dev, 2, PCI_BASE_ADDRESS_SPACE_MEMORY, &s->mmio);

    /* BAR1: 256 byte I/O alias of the first registers (MM_INDEX/DATA) */
    memory_region_init_alias(&s->io, OBJECT(s), "rv530.io", &s->mmio, 0, 0x100);
    pci_register_bar(dev, 1, PCI_BASE_ADDRESS_SPACE_IO, &s->io);

    if (pci_bus_is_express(pci_get_bus(dev))) {
        int ret = pcie_endpoint_cap_init(dev, 0x80);
        assert(ret > 0);
    } else {
        dev->cap_present &= ~QEMU_PCI_CAP_EXPRESS;
    }

    rv530_mc_init(s);
    rv530_cp_init(s);
    rv530_surface_hook_init(s);
    rv530_avivo_init(s, errp);
    rv530_clip_init(s);
    rv530_gfx_init(s);
    rv530_gfx_vk_init(s);

    if (s->capture_file) {
        s->capture = fopen(s->capture_file, "wb");
        if (!s->capture) {
            warn_report("rv530: cannot open capture file %s", s->capture_file);
        }
    }
}

static void rv530_exit(PCIDevice *dev)
{
    RV530State *s = RV530(dev);

    rv530_gfx_vk_fini(s);
    rv530_gfx_free(s);
    rv530_gart_invalidate(s);
    if (s->capture) {
        fclose(s->capture);
    }
    rv530_avivo_fini(s);
    rv530_clip_fini(s);
}

static const Property rv530_properties[] = {
    DEFINE_PROP_UINT32("vram_size_mb", RV530State, vram_size_mb, 256),
    DEFINE_PROP_UINT32("device-id", RV530State, device_id, 0x71c5),
    DEFINE_PROP_UINT32("subsys-vendor-id", RV530State, subsys_vendor,
                       PCI_VENDOR_ID_APPLE_),
    DEFINE_PROP_UINT32("subsys-id", RV530State, subsys_id, 0x0080),
    DEFINE_PROP_UINT32("revision", RV530State, revision, 0x00),
    DEFINE_PROP_STRING("capture", RV530State, capture_file),
    DEFINE_PROP_UINT32("debug", RV530State, debug, 1),
    DEFINE_PROP_BOOL("cp-sync", RV530State, cp_sync, true),
    DEFINE_PROP_BOOL("vulkan", RV530State, vulkan, true),
    DEFINE_PROP_BOOL("vk-verify", RV530State, vk_verify, false),
    DEFINE_PROP_BOOL("vk-sync", RV530State, vk_sync, false),
    DEFINE_PROP_BOOL("no-3d", RV530State, no_3d, false),
    DEFINE_PROP_BOOL("resize", RV530State, resize, false),
    DEFINE_PROP_UINT32("refresh", RV530State, refresh, 60),
    DEFINE_PROP_CHR("clipboard", RV530State, clip_chr),
    DEFINE_EDID_PROPERTIES(RV530State, edid_info),
};

static void rv530_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);

    k->realize = rv530_realize;
    k->exit = rv530_exit;
    k->vendor_id = PCI_VENDOR_ID_ATI_;
    k->device_id = 0x71c5;
    k->class_id = PCI_CLASS_DISPLAY_VGA;
    k->romfile = NULL;
    dc->desc = "ATI Radeon X1600 (RV530)";
    device_class_set_legacy_reset(dc, rv530_reset);
    device_class_set_props(dc, rv530_properties);
    dc->hotpluggable = false;
    dc->vmsd = NULL;               /* not migratable yet */
    set_bit(DEVICE_CATEGORY_DISPLAY, dc->categories);
}

/* "resize-to" WxH (qom-set): ask the guest for a mode as a window resize would */
static void rv530_set_resize_to(Object *obj, const char *val, Error **errp)
{
    unsigned w, h;

    if (sscanf(val, "%ux%u", &w, &h) != 2) {
        error_setg(errp, "resize-to wants WIDTHxHEIGHT");
        return;
    }
    rv530_request_mode(RV530(obj), w, h);
}

static bool rv530_get_auto_resize(Object *obj, Error **errp)
{
    return RV530(obj)->resize;
}

static void rv530_set_auto_resize_prop(Object *obj, bool on, Error **errp)
{
    rv530_set_auto_resize(RV530(obj), on);
}

static void rv530_instance_init(Object *obj)
{
    RV530State *s = RV530(obj);
    int i;

    PCI_DEVICE(obj)->cap_present |= QEMU_PCI_CAP_EXPRESS;
    for (i = 0; i < RV_NUM_DDC; i++) {
        char name[16];
        snprintf(name, sizeof(name), "ddc%d", i + 1);
        object_initialize_child(obj, name, &s->ddc[i].ddc, TYPE_RV530_EDID);
    }
    object_property_add_str(obj, "resize-to", NULL, rv530_set_resize_to);
    /* "resize" sets the initial state; this one also works at run time */
    object_property_add_bool(obj, "auto-resize", rv530_get_auto_resize,
                             rv530_set_auto_resize_prop);
}

static const TypeInfo rv530_info = {
    .name = TYPE_RV530,
    .parent = TYPE_PCI_DEVICE,
    .instance_size = sizeof(RV530State),
    .instance_init = rv530_instance_init,
    .class_init = rv530_class_init,
    .interfaces = (const InterfaceInfo[]) {
        { INTERFACE_PCIE_DEVICE },
        { INTERFACE_CONVENTIONAL_PCI_DEVICE },
        { },
    },
};

static void rv530_register_types(void)
{
    type_register_static(&rv530_info);
}

type_init(rv530_register_types)
