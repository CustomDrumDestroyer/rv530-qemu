/*
 * QEMU ATI Radeon X1600 (RV530, R5xx family) emulation - device state.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_DISPLAY_RV530_H
#define HW_DISPLAY_RV530_H

#include "qemu/osdep.h"
#include "qemu/timer.h"
#include "qemu/thread.h"
#include "hw/pci/pci_device.h"
#include "hw/i2c/i2c.h"
#include "hw/i2c/bitbang_i2c.h"
#include "hw/display/edid.h"
#include "ui/console.h"
#include "ui/clipboard.h"
#include "chardev/char-fe.h"
#include "qom/object.h"
#include "r5xx_core.h"

#define TYPE_RV530 "rv530"
OBJECT_DECLARE_SIMPLE_TYPE(RV530State, RV530)

/* ---- register offsets used by the device glue ---- */
#define RV_MM_INDEX              0x0000
#define RV_MM_DATA               0x0004
#define RV_CLOCK_CNTL_INDEX      0x0008
#define RV_CLOCK_CNTL_DATA       0x000c
#define RV_BIOS_0_SCRATCH        0x0010
#define RV_BUS_CNTL              0x0030   /* also PCIE_INDEX on R5xx */
#define RV_PCIE_INDEX            0x0030
#define RV_PCIE_DATA             0x0034
#define RV_GEN_INT_CNTL          0x0040
#define RV_GEN_INT_STATUS        0x0044
#define RV_MSI_REARM_EN          0x0160
#define RV_MC_IND_INDEX          0x0070
#define RV_MC_IND_DATA           0x0074
#define RV_RBBM_SOFT_RESET       0x00f0
#define RV_CONFIG_MEMSIZE        0x00f8
#define RV_CONFIG_APER_SIZE      0x0108
#define RV_HOST_PATH_CNTL        0x0130
#define RV_HDP_FB_LOCATION       0x0134
#define RV_MC_STATUS_LEGACY      0x0150
#define RV_P1PLL_CNTL            0x0450
#define RV_P2PLL_CNTL            0x0454
#define RV_PPLL_LOCKED           (0x30u << 16)  /* bits 20,21: what the VBIOS waits for */
#define RV_VGA_RENDER_CONTROL    0x0300
#define RV_VGA_MEMORY_BASE       0x0310
#define RV_VGA_HDP_CONTROL       0x0328
#define RV_D1VGA_CONTROL         0x0330
#define RV_D2VGA_CONTROL         0x0338
#define RV_CP_RB_BASE            0x0700
#define RV_CP_RB_CNTL            0x0704
#define RV_CP_RB_RPTR_ADDR       0x070c
#define RV_CP_RB_RPTR            0x0710
#define RV_CP_RB_WPTR            0x0714
#define RV_CP_RB_WPTR_DELAY      0x0718
#define RV_CP_RB_RPTR_WR         0x071c
#define RV_CP_IB_BASE            0x0738
#define RV_CP_IB_BUFSZ           0x073c
#define RV_CP_CSQ_CNTL           0x0740
#define RV_CP_CSQ_MODE           0x0744
#define RV_SCRATCH_UMSK          0x0770
#define RV_SCRATCH_ADDR          0x0774
#define RV_CP_ME_CNTL            0x07d0
#define RV_CP_ME_RAM_ADDR        0x07d4
#define RV_CP_ME_RAM_RADDR       0x07d8
#define RV_CP_ME_RAM_DATAH       0x07dc
#define RV_CP_ME_RAM_DATAL       0x07e0
#define RV_CP_STAT               0x07c0
#define RV_RBBM_STATUS           0x0e40
#define RV_SCRATCH_REG0          0x15e0
#define RV_SCRATCH_REG7          0x15fc
#define RV_WAIT_UNTIL            0x1720
#define RV_ISYNC_CNTL            0x1724
#define RV_VAP_PVS_VECTOR_INDX   0x2200
#define RV_VAP_PVS_VECTOR_DATA   0x2204
#define RV_VAP_PVS_UPLOAD_DATA   0x2208
#define RV_GB_PIPE_SELECT        0x402c
#define RV_GB_PIPE_SELECT2       0x4124
#define RV_GA_US_VECTOR_INDEX    0x4250
#define RV_GA_US_VECTOR_DATA     0x4254
#define RV_DST_PIPE_CONFIG       0x4ad4
#define RV_ZB_ZPASS_DATA         0x4f58
#define RV_ZB_ZPASS_ADDR         0x4f5c

/* AVIVO display controller 1 */
#define RV_D1CRTC_H_TOTAL        0x6000
#define RV_D1CRTC_H_BLANK_START_END 0x6004
#define RV_D1CRTC_V_TOTAL        0x6020
#define RV_D1CRTC_V_BLANK_START_END 0x6024
#define RV_D1CRTC_CONTROL        0x6080
#define RV_D1CRTC_BLANK_CONTROL  0x6084
#define RV_D1CRTC_STATUS         0x609c
#define RV_D1CRTC_STATUS_POSITION 0x60a0
#define RV_D1CRTC_FRAME_COUNT    0x60a4
#define RV_D1CRTC_STATUS_HV_COUNT 0x60ac
#define RV_D1MODE_MASTER_UPDATE_LOCK 0x60e0
#define RV_D1MODE_MASTER_UPDATE_MODE 0x60e4
#define RV_D1CRTC_UPDATE_LOCK    0x60e8
#define RV_DC_CRTC_MASTER_EN     0x60f8
#define RV_D1GRPH_ENABLE         0x6100
#define RV_D1GRPH_CONTROL        0x6104
#define RV_D1GRPH_LUT_SEL        0x6108
#define RV_D1GRPH_PRIMARY_SURFACE_ADDRESS 0x6110
#define RV_D1GRPH_SECONDARY_SURFACE_ADDRESS 0x6118
#define RV_D1GRPH_PITCH          0x6120
#define RV_D1GRPH_SURFACE_OFFSET_X 0x6124
#define RV_D1GRPH_SURFACE_OFFSET_Y 0x6128
#define RV_D1GRPH_X_START        0x612c
#define RV_D1GRPH_Y_START        0x6130
#define RV_D1GRPH_X_END          0x6134
#define RV_D1GRPH_Y_END          0x6138
#define RV_D1GRPH_UPDATE         0x6144
#define RV_D1GRPH_FLIP_CONTROL   0x6148
#define RV_D1CUR_CONTROL         0x6400
#define RV_D1CUR_SURFACE_ADDRESS 0x6408
#define RV_D1CUR_SIZE            0x6410
#define RV_D1CUR_POSITION        0x6414
#define RV_D1CUR_HOT_SPOT        0x6418
#define RV_D1CUR_UPDATE          0x6424
#define RV_DC_LUT_RW_SELECT      0x6480
#define RV_DC_LUT_RW_INDEX       0x6488
#define RV_DC_LUT_30_COLOR       0x6494
#define RV_DC_LUT_AUTOFILL       0x64a0
#define RV_DC_LUT_AUTOFILL_DONE  (1u << 1)
#define RV_CRTC_STRIDE           0x0800        /* D2 registers = D1 + 0x800 */
#define RV_D1MODE_DATA_FORMAT    0x6528
#define RV_D1MODE_DESKTOP_HEIGHT 0x652c
#define RV_D1MODE_VBLANK_STATUS  0x6534
#define RV_D1MODE_VLINE_START_END 0x6538
#define RV_D1MODE_VLINE_STATUS   0x653c
#define RV_DxMODE_INT_MASK       0x6540
#define RV_D1MODE_VIEWPORT_START 0x6580
#define RV_D1MODE_VIEWPORT_SIZE  0x6584
#define RV_D2CRTC_CONTROL        0x6880
#define RV_D2CRTC_STATUS         0x689c
#define RV_D2CRTC_FRAME_COUNT    0x68a4
#define RV_D2GRPH_ENABLE         0x6900
#define RV_D2GRPH_UPDATE         0x6944
#define RV_D2MODE_VBLANK_STATUS  0x6d34
#define RV_DACA_ENABLE           0x7800
#define RV_DACA_COMPARATOR_ENABLE 0x783c
#define RV_DACA_COMPARATOR_OUTPUT 0x7840
#define RV_TMDSA_CNTL            0x7880
#define RV_TMDSA_TRANSMITTER_CONTROL 0x7910
#define RV_DACB_COMPARATOR_OUTPUT 0x7a40
#define RV_LVTMA_CNTL            0x7a80
#define RV_DC_HOT_PLUG_DETECT1_CONTROL 0x7d00
#define RV_DC_HOT_PLUG_DETECT1_INT_STATUS 0x7d04
#define RV_DC_HOT_PLUG_DETECT1_INT_CONTROL 0x7d08
#define RV_DC_HOT_PLUG_DETECT2_CONTROL 0x7d10
#define RV_DC_HOT_PLUG_DETECT2_INT_STATUS 0x7d14
#define RV_DC_HOT_PLUG_DETECT2_INT_CONTROL 0x7d18
#define RV_DC_GPIO_GENERIC_MASK  0x7e30
#define RV_DC_GPIO_DDC1_MASK     0x7e40
#define RV_DC_GPIO_DDC1_A        0x7e44
#define RV_DC_GPIO_DDC1_EN       0x7e48
#define RV_DC_GPIO_DDC1_Y        0x7e4c
#define RV_DC_GPIO_DDC2_MASK     0x7e50
#define RV_DC_GPIO_DDC3_MASK     0x7e60
#define RV_DISP_INTERRUPT_STATUS 0x7edc

/* bits */
#define RV_GEN_INT_SW_INT_EN     (1u << 25)
#define RV_GEN_INT_SW_INT        (1u << 25)
#define RV_GEN_INT_SW_INT_FIRE   (1u << 26)
#define RV_GEN_INT_DISPLAY_STAT  (1u << 0)
#define RV_RBBM_GUI_ACTIVE       (1u << 31)
#define RV_RBBM_CP_CMDSTRM_BUSY  (1u << 16)
#define RV_RBBM_CMDFIFO_AVAIL    0x40
#define RV_CRTC_EN               (1u << 0)
#define RV_CRTC_DISP_READ_REQUEST_DISABLE (1u << 24)
#define RV_D1CRTC_V_BLANK        (1u << 0)
#define RV_VBLANK_OCCURRED       (1u << 0)
#define RV_VBLANK_ACK            (1u << 4)
#define RV_VBLANK_STAT           (1u << 12)
#define RV_VBLANK_INTERRUPT      (1u << 16)
#define RV_D1MODE_VBLANK_INT_MASK (1u << 0)
#define RV_D2MODE_VBLANK_INT_MASK (1u << 8)
#define RV_DISP_LB_D1_VBLANK_INT (1u << 4)
#define RV_DISP_LB_D2_VBLANK_INT (1u << 5)
#define RV_DISP_HPD1_INT         (1u << 18)
#define RV_HPD_INT_EN            (1u << 16)
#define RV_HPD_INT_ACK           (1u << 0)
#define RV_HPD_INT_STATUS        (1u << 0)
#define RV_HPD_SENSE             (1u << 1)
#define RV_D1CURSOR_EN           (1u << 0)
#define RV_D1GRPH_UPDATE_LOCK    (1u << 16)
#define RV_D1GRPH_SURFACE_UPDATE_PENDING (1u << 2)
#define RV_D1GRPH_TILED          (1u << 20)
#define RV_D1GRPH_MACRO          (1u << 21)
#define RV_RB_RPTR_WR_ENA        (1u << 31)
#define RV_RB_NO_UPDATE          (1u << 27)
#define RV_SOFT_RESET_CP         (1u << 0)
#define RV_PCIE_TX_GART_EN       (1u << 0)
#define RV_PCIE_TX_GART_INVALIDATE_TLB (1u << 8)

/* MC indirect register indices: R520/RV530/RV560/RV570 map (Linux r520.c,
 * r520d.h, r500_reg.h).  RV515 uses a different map (FB_LOCATION at 1). */
#define RV_MC_STATUS             0x00
#define RV_MC_IDLE               (1u << 1)      /* R520_MC_STATUS_IDLE */
#define RV_MC_IDLE_RV515         (1u << 4)      /* also set, harmless */
#define RV_MC_FB_LOCATION        0x04
#define RV_MC_AGP_LOCATION       0x05
#define RV_MC_AGP_BASE           0x06
#define RV_MC_AGP_BASE_2         0x07
#define RV_MC_CNTL0              0x08
#define RV_MC_CNTL0_128BIT       (2u << 24)     /* MEM_NUM_CHANNELS = 2 */
#define RV_MC_IND_WR_EN          ((1u << 23) | (1u << 24))

/* PCIE indirect register indices */
#define RV_PCIE_TX_GART_CNTL     0x10
#define RV_PCIE_TX_DISCARD_RD_ADDR_LO 0x11
#define RV_PCIE_TX_DISCARD_RD_ADDR_HI 0x12
#define RV_PCIE_TX_GART_BASE     0x13
#define RV_PCIE_TX_GART_START_LO 0x14
#define RV_PCIE_TX_GART_START_HI 0x15
#define RV_PCIE_TX_GART_END_LO   0x16
#define RV_PCIE_TX_GART_END_HI   0x17
#define RV_PCIE_TX_GART_ERROR    0x18

#define RV_GART_CACHE_SIZE       64
#define RV_NUM_DDC               3

typedef struct RV530GartEntry {
    uint32_t gpu_page;       /* GPU address >> 12, or ~0 if empty */
    void *host;              /* dma_memory_map() pointer */
    dma_addr_t len;
    bool write;
} RV530GartEntry;

/* the monitor's EDID on DDC (r5xx_edid.c): base block + CEA extension */
#define RV530_EDID_SIZE 256
#define TYPE_RV530_EDID "rv530-edid"
typedef struct RV530EdidState {
    I2CSlave i2c;
    bool firstbyte;
    uint8_t reg;
    uint8_t blob[RV530_EDID_SIZE];
} RV530EdidState;
OBJECT_DECLARE_SIMPLE_TYPE(RV530EdidState, RV530_EDID)

void rv530_edid_build(uint8_t edid[RV530_EDID_SIZE], uint32_t prefx,
                      uint32_t prefy, uint32_t maxx, uint32_t maxy, uint32_t serial,
                      uint32_t hz);

typedef struct RV530Ddc {
    bitbang_i2c_interface bbi2c;
    I2CBus *bus;
    RV530EdidState ddc;
    bool has_ddc;
    uint32_t base;           /* MASK register offset */
    uint32_t y;              /* last pin read-back value */
} RV530Ddc;

typedef struct RV530Scanout {
    bool enabled;
    uint32_t addr;           /* GPU address of the surface (tiled) or of the
                              * first visible pixel (linear) */
    uint32_t x0, y0;         /* first visible pixel inside the surface */
    uint32_t width, height;
    uint32_t pitch;          /* bytes */
    uint32_t bpp;
    uint32_t format;         /* D1GRPH_CONTROL */
    bool tiled;
    bool zero_copy;
    bool lut_direct;         /* linear ARGB8888 through a non-identity LUT */
} RV530Scanout;


#define RV_NUM_CRTC 2

struct RV530State;
typedef struct RV530Crtc {
    struct RV530State *s;
    int index;                        /* 0 = D1, 1 = D2 */
    uint32_t base;                    /* register offset of this CRTC: 0 or 0x800 */
    QemuConsole *con;                 /* one QEMU console head per CRTC */
    bool vblank_occurred;             /* DxMODE_VBLANK_STATUS.OCCURRED */
    RV530Scanout scanout;
    uint8_t *shadow;                  /* converted frame when not zero-copy */
    size_t shadow_size;
    uint32_t lut_gen_seen;            /* s->lut_gen the shadow was built with */
    QEMUCursor *cursor;
    bool cursor_visible;
    /* cursor hot spot recovered from the pointer position (the driver
     * applies the shape's hot spot in software, D1CUR_HOT_SPOT stays 0) */
    int hot_x, hot_y, cand_x, cand_y, cand_n;
    unsigned long *dirty;             /* per-CRTC copy of VRAM page dirty bits */
} RV530Crtc;

struct R5xxGfx;              /* 3D engine state, r5xx_3d.c */

/* phases timed for the stall report (rv530_stall_end) */
enum { RV_PROF_VKPREP, RV_PROF_VKDRAW, RV_PROF_FLUSH, RV_PROF_CPU, RV_PROF_2D, RV_PROF_WAIT,
       RV_PROF_SETUP, RV_PROF_VKBUILD, RV_PROF_VKPRIMS, RV_PROF_COLLECT, RV_PROF_N };

struct RV530State {
    PCIDevice parent_obj;

    /* properties */
    uint32_t vram_size_mb;
    uint32_t device_id;
    uint32_t subsys_vendor;
    uint32_t subsys_id;
    uint32_t revision;
    char *capture_file;
    uint32_t debug;          /* bit0: unknown regs, bit1: all regs, bit2: pm4, bit3: 3d */
    bool trace_3d;
    bool cp_sync;            /* run CP synchronously on WPTR write */
    uint32_t xres, yres;     /* initial (posted) mode and EDID preferred mode */
    bool no_3d;

    /* memory regions */
    MemoryRegion vram;
    MemoryRegion aperture;   /* BAR0 */
    MemoryRegion mmio;       /* BAR2 */
    MemoryRegion io;         /* BAR4 */
    uint8_t *vram_ptr;
    uint32_t vram_size;

    /* register file */
    uint32_t regs[R5XX_NUM_REGS];
    uint32_t mc_regs[256];
    uint32_t pcie_regs[256];
    uint32_t pll_regs[64];
    uint32_t mm_index;
    uint32_t mc_index;
    uint32_t pcie_index;
    uint32_t pll_index;
    uint8_t  unknown_logged[R5XX_NUM_REGS / 8];

    /* polling-loop detector */
    uint32_t poll_last_off;
    uint32_t poll_last_val;
    uint32_t poll_count;
    bool     poll_reported;

    /* MC / GART */
    uint32_t fb_start, fb_end;        /* GPU address window of VRAM */
    uint32_t gart_start, gart_end;    /* GPU address window of GART */
    uint32_t gart_base;               /* GPU address of the page table */
    bool gart_enabled;
    RV530GartEntry gart_cache[RV_GART_CACHE_SIZE];
    QemuMutex gart_lock;              /* parallel CPU rasterization */
    R5xxMem mem;                      /* GPU memory interface */
    uint64_t gart_faults;

    /* command processor */
    QEMUBH *cp_bh;
    bool cp_busy;
    bool cp_running;
    uint32_t cp_rptr;                 /* dwords */
    uint32_t cp_wptr;
    uint32_t cp_ring_mask;            /* dwords - 1 */
    uint32_t cp_ring_base;
    uint32_t cp_rptr_addr;            /* GPU addr (bytes) of rptr write-back or 0 */
    uint32_t cp_me_hash;
    uint32_t cp_me_words;
    int cp_ib_depth;
    uint64_t cp_packets, cp_draws;
    bool sw_int_pending;
    /* legacy 2D DST/SRC_OFFSET + _PITCH (exact byte values; the packed
     * *_PITCH_OFFSET form only holds 1 KiB offsets and 64-byte pitches) */
    bool r2d_legacy[2];               /* [0] = dst, [1] = src */
    /* Vulkan backend (r5xx_vk.c); NULL = CPU path only */
    struct R5xxVk *vk;
    MemoryRegion *vk_sysram;      /* guest RAM imported by the backend */
    bool vulkan, vk_verify, vk_sync;
    bool irq_level;                   /* last INTx level, for tracing */
    /* time spent per phase, for the stall report (rv530_stall_end) */
    int64_t prof_ns[RV_PROF_N];
    uint32_t prof_n[RV_PROF_N];
    /*
     * A CP run that waits for a shader compile releases the BQL
     * (rv530_vk_wait_cb).  Meanwhile other threads must not enter the
     * device: MMIO from other vCPUs waits on cp_idle, display refreshes
     * are skipped.
     */
    bool cp_unlocked;
    int unlock_nest;                  /* nested rv530_vk_wait_cb begins while unlocked */
    uint64_t cp_runs;                 /* rv530_cp_run calls, for per-run budgets */
    QemuCond cp_idle;
    /*
     * MMIO handlers active on the stack.  The rv530.mmio region opts out
     * of QEMU's per-device re-entrancy guard, which rejected the accesses
     * of other vCPUs made while a CP run waited with the BQL released;
     * those now wait on cp_idle instead.  An access that arrives
     * with mmio_depth > 0 after that wait can only be a re-entry on the
     * same thread (device DMA into its own BAR) and is refused.
     */
    uint32_t mmio_depth;
    uint32_t mmio_reentered;          /* refused accesses, for the warning */
    int64_t unlocked_t0, unlocked_ns;
    uint32_t pm4_buf[16384];          /* type-3 payload (count is 14 bits) */

    /* display */
    QEMUTimer vblank_timer;
    int64_t frame_start_ns;           /* virtual clock at start of current frame */
    int64_t frame_period_ns;
    uint32_t frame_count;             /* both CRTCs run off one 60 Hz timeline */
    bool hpd1_pending;
    bool hpd1_unplugged;              /* HPD1 sense low (monitor swap) */
    bool resize;                      /* follow the UI window size ("resize",
                                       * at run time "auto-resize") */
    uint32_t refresh;                 /* property: preferred mode refresh, Hz */

    /* clipboard sync with the guest agent (r5xx_clip.c) */
    CharFrontend clip_chr;
    QemuClipboardPeer clip_peer;
    GByteArray *clip_in;
    QemuClipboardInfo *clip_sent;     /* last host clipboard sent to the guest */
    bool clip_pending;
    uint32_t resize_w, resize_h;      /* last size the UI asked for */
    QEMUTimer resize_timer;           /* debounces UI resizes */
    QEMUTimer hpd_timer;              /* replugs the monitor after a swap */
    RV530Crtc crtc[RV_NUM_CRTC];
    RV530Ddc ddc[RV_NUM_DDC];
    qemu_edid_info edid_info;
    uint32_t lut[2][256];             /* DC_LUTA / DC_LUTB, selected by DxGRPH_LUT_SEL */
    uint32_t lut_nonid[2];            /* entries that differ from the identity ramp */
    uint32_t lut_gen;                 /* bumped on every LUT change */

    /* 3D */
    struct R5xxGfx *gfx;
    FILE *capture;
};

/* rv530.c */
uint32_t rv530_reg_read(RV530State *s, uint32_t off);
void rv530_reg_write(RV530State *s, uint32_t off, uint32_t val);
void rv530_update_irq(RV530State *s);
void rv530_log_unknown(RV530State *s, const char *what, uint32_t off,
                       uint32_t val, bool is_write);

/* r5xx_mc.c */
void rv530_mc_init(RV530State *s);
void rv530_mc_reset(RV530State *s);
uint32_t rv530_mc_reg_read(RV530State *s, uint32_t idx);
void rv530_mc_reg_write(RV530State *s, uint32_t idx, uint32_t val);
uint32_t rv530_pcie_reg_read(RV530State *s, uint32_t idx);
void rv530_pcie_reg_write(RV530State *s, uint32_t idx, uint32_t val);
void rv530_gart_invalidate(RV530State *s);
bool rv530_gart_ram_page(RV530State *s, uint32_t addr, MemoryRegion *ram,
                         uint64_t *ram_off);
void rv530_gfx_gart_changed(RV530State *s);
bool rv530_linear_surface(RV530State *s, uint32_t gpu_addr);
void rv530_surface_hook_init(RV530State *s);
uint8_t *rv530_gpu_map(RV530State *s, uint32_t addr, uint32_t len, bool write);
void rv530_gpu_read(RV530State *s, uint32_t addr, void *buf, uint32_t len);
void rv530_gpu_write(RV530State *s, uint32_t addr, const void *buf, uint32_t len);
static inline uint32_t rv530_gpu_read32(RV530State *s, uint32_t addr)
{
    uint32_t v;
    rv530_gpu_read(s, addr, &v, 4);
    return le32_to_cpu(v);
}
static inline void rv530_gpu_write32(RV530State *s, uint32_t addr, uint32_t v)
{
    v = cpu_to_le32(v);
    rv530_gpu_write(s, addr, &v, 4);
}

/* r5xx_cp.c */
void rv530_cp_init(RV530State *s);
void rv530_cp_reset(RV530State *s);
bool rv530_cp_reg_read(RV530State *s, uint32_t off, uint32_t *val);
bool rv530_cp_reg_write(RV530State *s, uint32_t off, uint32_t val);
void rv530_cp_run(RV530State *s);
void rv530_cp_execute_buffer(RV530State *s, uint32_t addr, uint32_t ndw);

/* r5xx_avivo.c */
void rv530_avivo_init(RV530State *s, Error **errp);
void rv530_avivo_fini(RV530State *s);
void rv530_avivo_reset(RV530State *s);
bool rv530_avivo_reg_read(RV530State *s, uint32_t off, uint32_t *val);
bool rv530_avivo_reg_write(RV530State *s, uint32_t off, uint32_t val);
void rv530_set_auto_resize(RV530State *s, bool on);
void rv530_request_mode(RV530State *s, uint32_t w, uint32_t h);
void rv530_clip_init(RV530State *s);
void rv530_clip_fini(RV530State *s);

/* r5xx_atom.c: mailbox of the synthetic AtomBIOS (scripts/atombios/mkrom.py),
 * in a register range the RV530 does not use */
#define RV_ATOM_MAILBOX          0xff00
#define RV_ATOM_MAILBOX_DWORDS   16
#define RV_ATOM_DOORBELL         0xff7c
bool rv530_atom_reg_write(RV530State *s, uint32_t off, uint32_t val);
bool rv530_avivo_irq_pending(RV530State *s);
void rv530_collect_dirty(RV530State *s);
DirtyBitmapSnapshot *rv530_vram_snapshot_dirty(RV530State *s);
typedef struct RV530Stall {
    int64_t t0;
    int64_t unlocked_ns;
    int64_t ns[RV_PROF_N];
    uint32_t n[RV_PROF_N];
} RV530Stall;
void rv530_stall_start(RV530State *s, RV530Stall *st);
void rv530_vk_wait_cb(void *opaque, bool begin);
void rv530_stall_end(RV530State *s, const RV530Stall *st, const char *what,
                     uint32_t off);
void rv530_avivo_mark_dirty(RV530State *s, uint64_t off, uint64_t len);

/* r5xx_2d.c */
bool rv530_2d_reg_read(RV530State *s, uint32_t off, uint32_t *val);
bool rv530_2d_reg_write(RV530State *s, uint32_t off, uint32_t val);
bool rv530_2d_pm4(RV530State *s, uint32_t op, const uint32_t *data, uint32_t n);

/* r5xx_3d.c */
void rv530_gfx_init(RV530State *s);
void rv530_gfx_reset(RV530State *s);
void rv530_gfx_free(RV530State *s);
bool rv530_gfx_reg_write(RV530State *s, uint32_t off, uint32_t val);
bool rv530_gfx_reg_read(RV530State *s, uint32_t off, uint32_t *val);
void rv530_gfx_pm4(RV530State *s, uint32_t opcode, const uint32_t *data,
                   uint32_t count);
void rv530_gfx_flush(RV530State *s);
void rv530_gfx_vk_init(RV530State *s);
void rv530_gfx_vk_fini(RV530State *s);

#endif /* HW_DISPLAY_RV530_H */
