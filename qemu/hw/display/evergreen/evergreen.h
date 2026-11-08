/*
 * ATI Radeon HD 5770 (Juniper, Evergreen) device model.
 *
 * Register offsets are those of Linux drivers/gpu/drm/radeon/evergreend.h,
 * evergreen_reg.h and r600d.h.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_DISPLAY_EVERGREEN_H
#define HW_DISPLAY_EVERGREEN_H

#include "qemu/osdep.h"
#include "qemu/timer.h"
#include "hw/pci/pci_device.h"
#include "hw/i2c/i2c.h"
#include "hw/i2c/bitbang_i2c.h"
#include "ui/console.h"
#include "qom/object.h"
#include "../rv530/r5xx_edid.h"

#define TYPE_EVERGREEN "radeon-hd5770"
OBJECT_DECLARE_SIMPLE_TYPE(EvergreenState, EVERGREEN)

#define EG_MMIO_SIZE            0x20000         /* BAR2: 128 KiB */
#define EG_NUM_REGS             (EG_MMIO_SIZE / 4)
/* SET_CONTEXT_REG space 0x28000-0x29000 and the constant/resource spaces
 * that only the CP writes; kept outside the MMIO register file */
#define EG_CTX_BASE             0x28000
#define EG_CTX_SIZE             0x1000

/* --- bus interface, indirect access --- */
#define EG_MM_INDEX             0x0000
#define EG_MM_DATA              0x0004
#define EG_PCIE_PORT_INDEX      0x0038
#define EG_PCIE_PORT_DATA       0x003c
#define EG_ROM_INDEX            0x00a8          /* VBIOS flash, byte offset */
#define EG_ROM_DATA             0x00ac

/* the paravirtual AtomBIOS of mkrom-evergreen.py (eg_atom.c) */
#define EG_ATOM_MAILBOX         0x1ff00
#define EG_ATOM_MAILBOX_DWORDS  16
#define EG_ATOM_DOORBELL        0x1ff7c

/* --- SRBM / GRBM / status --- */
#define EG_SRBM_STATUS          0x0e50
#define EG_SRBM_SOFT_RESET      0x0e60
#define EG_SRBM_STATUS2         0x0ec4
#define EG_GRBM_STATUS          0x8010
#define EG_GRBM_STATUS_SE0      0x8014
#define EG_GRBM_STATUS_SE1      0x8018
#define EG_GRBM_GUI_ACTIVE      (1u << 31)
#define EG_GRBM_CB_BUSY         (1u << 30)
#define EG_GRBM_DB_BUSY         (1u << 26)
#define EG_GRBM_SOFT_RESET      0x8020
#define EG_CP_STAT              0x8680
#define EG_CP_ME_CNTL           0x86d8
#define EG_CG_SPLL_STATUS       0x060c
#define EG_PPLL1_CNTL           0x0420
#define EG_PPLL2_CNTL           0x04a0
#define EG_PPLL_LOCKED          (3u << 20)

/* --- memory controller / VM --- */
#define EG_VM_L2_CNTL           0x1400
#define EG_VM_L2_STATUS         0x140c
#define EG_VM_CONTEXT0_CNTL     0x1410
#define EG_VM_CONTEXT0_REQUEST_RESPONSE 0x1470
#define EG_VM_CONTEXT0_PROTECTION_FAULT_DEFAULT_ADDR 0x1518
#define EG_VM_CONTEXT0_PAGE_TABLE_BASE_ADDR  0x153c
#define EG_VM_CONTEXT0_PAGE_TABLE_START_ADDR 0x155c
#define EG_VM_CONTEXT0_PAGE_TABLE_END_ADDR   0x157c
#define EG_MC_SHARED_CHMAP      0x2004
#define EG_MC_VM_FB_LOCATION    0x2024
#define EG_MC_ARB_RAMCFG        0x2760
#define EG_GB_ADDR_CONFIG       0x98f8
#define EG_CONFIG_MEMSIZE       0x5428
#define EG_HFS_STATUS           0x55d8          /* DRM crypto engine status */
#define EG_HFS_DONE             (1u << 16)
#define EG_CONFIG_APER_SIZE     0x5430          /* MB; Apple puts its GART table at the top */

/* --- interrupts --- */
#define EG_IH_RB_CNTL           0x3e00
#define EG_IH_RB_BASE           0x3e04
#define EG_IH_RB_RPTR           0x3e08
#define EG_IH_RB_WPTR           0x3e0c
#define EG_IH_RB_WPTR_ADDR_HI   0x3e10
#define EG_IH_RB_WPTR_ADDR_LO   0x3e14
#define EG_IH_CNTL              0x3e18
#define EG_IH_RB_ENABLE         (1u << 0)
#define EG_IH_WPTR_WRITEBACK_ENABLE (1u << 8)
#define EG_IH_ENABLE_INTR       (1u << 0)       /* IH_CNTL */

/* interrupt source ids (evergreen_irq_process) */
#define EG_SRC_VBLANK_D1        1               /* D1..D6: 1..6 */
#define EG_SRC_HPD              42
#define EG_SRC_CP_EOP           181
#define EG_SRC_DMA_TRAP         224

/* --- command processor --- */
#define EG_SCRATCH_REG0         0x8500
#define EG_SCRATCH_UMSK         0x8540
#define EG_SCRATCH_ADDR         0x8544
#define EG_CP_RB_RPTR           0x8700
#define EG_CP_RB_BASE           0xc100
#define EG_CP_RB_CNTL           0xc104
#define EG_CP_RB_RPTR_WR        0xc108
#define EG_CP_RB_RPTR_ADDR      0xc10c
#define EG_CP_RB_RPTR_ADDR_HI   0xc110
#define EG_CP_RB_WPTR           0xc114
#define EG_CP_INT_CNTL          0xc124
#define EG_CP_INT_STATUS        0xc128
#define EG_RB_RPTR_WR_ENA       (1u << 31)
#define EG_RB_NO_UPDATE         (1u << 27)
#define EG_TIME_STAMP_INT_ENABLE (1u << 26)

/* --- async DMA engine --- */
#define EG_DMA_RB_CNTL          0xd000
#define EG_DMA_RB_BASE          0xd004
#define EG_DMA_RB_RPTR          0xd008
#define EG_DMA_RB_WPTR          0xd00c
#define EG_DMA_RB_RPTR_ADDR_HI  0xd01c
#define EG_DMA_RB_RPTR_ADDR_LO  0xd020
#define EG_DMA_CNTL             0xd02c
#define EG_DMA_STATUS_REG       0xd034
#define EG_DMA_RB_ENABLE        (1u << 0)
#define EG_DMA_RPTR_WRITEBACK_ENABLE (1u << 12)
#define EG_DMA_TRAP_ENABLE      (1u << 0)

/* --- DCE4 display: CRTC n registers at + eg_crtc_offset[n] (evergreen_reg.h) --- */
#define EG_NUM_CRTC             6
#define EG_GRPH_ENABLE          0x6800
#define EG_GRPH_CONTROL         0x6804
#define EG_GRPH_SWAP_CONTROL    0x680c
#define EG_GRPH_PRIMARY_SURFACE_ADDRESS 0x6810
#define EG_GRPH_PITCH           0x6818
#define EG_GRPH_PRIMARY_SURFACE_ADDRESS_HIGH 0x681c
#define EG_GRPH_SURFACE_OFFSET_X 0x6824
#define EG_GRPH_SURFACE_OFFSET_Y 0x6828
#define EG_GRPH_X_START         0x682c
#define EG_GRPH_Y_START         0x6830
#define EG_GRPH_X_END           0x6834
#define EG_GRPH_Y_END           0x6838
#define EG_GRPH_UPDATE          0x6844
#define EG_CUR_CONTROL          0x6998      /* EN bit 0, MODE 9:8 */
#define EG_CUR_SURFACE_ADDRESS  0x699c
#define EG_CUR_SIZE             0x69a0
#define EG_CUR_SURFACE_ADDRESS_HIGH 0x69a4
#define EG_CUR_POSITION         0x69a8
#define EG_CUR_HOT_SPOT         0x69ac
#define EG_CUR_UPDATE           0x69b8      /* UPDATE_LOCK bit 16 */
#define EG_DC_LUT_RW_INDEX      0x69e4      /* auto-increments on each colour write */
#define EG_DC_LUT_PWL_DATA      0x69ec
#define EG_DC_LUT_30_COLOR      0x69f0
#define EG_INT_MASK             0x6b40      /* VBLANK_INT_MASK bit 0 */
#define EG_VLINE_STATUS         0x6bb8
#define EG_VBLANK_STATUS        0x6bbc      /* OCCURRED bit 0, ACK bit 4 */
#define EG_VIEWPORT_START       0x6d70
#define EG_VIEWPORT_SIZE        0x6d74
#define EG_CRTC_V_BLANK_START_END 0x6e34
#define EG_CRTC_CONTROL         0x6e70
#define EG_CRTC_STATUS          0x6e8c
#define EG_CRTC_STATUS_POSITION 0x6e90
#define EG_CRTC_STATUS_HV_COUNT 0x6ea0
#define EG_CRTC_STATUS_FRAME_COUNT 0x6e98
#define EG_CRTC_MASTER_EN       (1u << 0)
#define EG_DC_HPD1_INT_STATUS   0x601c      /* HPDn: + 0xc * n */
#define EG_DC_HPD1_INT_CONTROL  0x6020
#define EG_DC_HPD_SENSE         (1u << 1)
#define EG_DC_HPD_INT_STATUS    (1u << 0)
#define EG_DC_HPD_INT_ACK       (1u << 0)
#define EG_DC_HPD_INT_EN        (1u << 16)
#define EG_DISP_INTERRUPT_STATUS 0x60f4     /* ..._CONTINUE: 0x60f8, 0x60fc, 0x6100, 0x614c */

/* DDC GPIO pairs (MASK, A, EN, Y at +0, +4, +8, +0xc), clock bit 0 and
 * data bit 8, in the order of the VBIOS GPIO_I2C_Info (eg_dce.c ddc_base,
 * scripts/atombios/mkrom-evergreen.py) */
#define EG_NUM_DDC              8

/* surface tiling (eg_tile.c) */
#define EG_ARRAY_LINEAR_GENERAL  0
#define EG_ARRAY_LINEAR_ALIGNED  1
#define EG_ARRAY_1D_TILED_THIN1  2
#define EG_ARRAY_2D_TILED_THIN1  4

typedef struct EgTiling {
    uint32_t array_mode, bpp, pitch, height;    /* bpp in bytes, pitch in pixels */
    uint32_t pipes, pipe_interleave, banks, bank_width, bank_height;
    uint32_t macro_aspect, tile_split;
    bool display;                               /* displayable micro tile order */
    /* derived by eg_tiling_update() */
    bool split;
    uint16_t micro_elem[64];                    /* byte offset in a micro tile */
    uint32_t micro_bytes, tiles_per_row, mt_pitch, mt_height, mt_bytes, mt_per_row;
    uint32_t mt_pitch_shift, mt_height_shift, pi_bits, pipe_bits, bank_bits;
    uint32_t bank_x_shift, bank_y_shift;
    uint64_t slice_bytes;
} EgTiling;

#define EG_NUM_HDP              32

typedef struct EgHdpSurf {
    bool active;
    uint64_t lo, hi, base, len;         /* GPU addresses, bytes */
    uint64_t map_lo, map_len;           /* the page-aligned overlay */
    EgTiling tile;
    uint8_t *shadow;                    /* the linear image the CPU sees */
    MemoryRegion mr;
} EgHdpSurf;

typedef struct EgCrtc {
    struct EvergreenState *s;
    int index;
    uint32_t base;                  /* register offset of this CRTC */
    QemuConsole *con;
    bool vblank_pending;
    int last_w, last_h, last_bpp;
    uint64_t last_off;
    uint32_t last_pitch;
    uint32_t lut[256];              /* 10:10:10 gamma, not applied yet */
    uint8_t *shadow;                /* detiled copy of a tiled scanout surface */
    QEMUCursor *cursor;             /* hardware cursor, shown as the host's */
    bool cursor_visible;
    int hot_x, hot_y, cand_x, cand_y, cand_n;
    size_t shadow_size;
} EgCrtc;

struct EvergreenState {
    PCIDevice parent_obj;

    /* properties */
    uint32_t vram_size_mb;
    uint32_t device_id, subsys_vendor, subsys_id, revision;
    uint32_t debug;
    bool vulkan;                        /* property: 3D on the host GPU (eg_vk.h) */
    /* the CP waits for the GPU or a compile without the BQL (eg_cp_wait_cb) */
    bool cp_unlocked;
    QemuThread unlock_thread;           /* the thread that released the BQL */
    int unlock_nest, mmio_depth, mmio_reentered;
    uint64_t stat_mmio_reads, stat_busy_reads, stat_scratch_flushes;   /* EG_PROFILE */
    QemuCond cp_idle;
    /* the graphics ring runs in a bottom half, not in the vCPU (eg_cp_async) */
    QEMUBH *cp_bh;
    bool cp_async;
    bool no_lockless;           /* EG_MMIO_BQL: the register BAR under the BQL */

    MemoryRegion vram, aperture, bar0, mmio, io;
    uint8_t *vram_ptr;
    uint64_t vram_size;

    uint32_t regs[EG_NUM_REGS];
    uint32_t ctx[EG_CTX_SIZE / 4];
    uint32_t pcie_port[256];
    uint32_t mm_index;
    uint8_t unknown_logged[EG_NUM_REGS / 8];

    /* interrupts */
    uint32_t ih_wptr;               /* bytes */
    bool irq_level;

    /* command processor */
    uint32_t cp_rptr;               /* dwords */
    uint32_t dma_rptr;              /* bytes */
    int ib_depth;

    /* display */
    EgCrtc crtc[EG_NUM_CRTC];
    QEMUTimer vblank_timer;
    bool hpd_pending[6];

    /* DDC (EDID) on the GPIO pairs of the VBIOS GPIO_I2C_Info table */
    struct {
        bitbang_i2c_interface bbi2c;
        I2CBus *bus;
        RV530EdidState edid;
        uint32_t y;
    } ddc[EG_NUM_DDC];
    uint32_t xres, yres;
    uint32_t monitor_ddc, monitor_hpd;  /* where the monitor is plugged in */

    struct EgGfx *gfx;                  /* 3D engine state (eg_gfx.h) */
    EgHdpSurf hdp[EG_NUM_HDP];          /* HDP tiling apertures */
};

/* evergreen.c */
uint32_t eg_reg_read(EvergreenState *s, uint32_t off);
void eg_reg_write(EvergreenState *s, uint32_t off, uint32_t val);
bool eg_gpu_read(EvergreenState *s, uint64_t addr, void *buf, size_t len);
uint8_t *eg_vram_ptr(EvergreenState *s, uint64_t addr, uint64_t len);
bool eg_gpu_write(EvergreenState *s, uint64_t addr, const void *buf, size_t len);
/* GART (VM context 0) range, and one page's entry for the Vulkan backend's
 * table: guest RAM page within 'ram', or bit 31 | VRAM page */
bool eg_gart_range(EvergreenState *s, uint64_t *start, uint64_t *end);
const uint64_t *eg_gart_ptes(EvergreenState *s, uint64_t *n);
bool eg_gart_vk_entry(EvergreenState *s, uint64_t addr, MemoryRegion *ram, uint32_t *entry);
void eg_ih_push(EvergreenState *s, uint32_t src_id, uint32_t src_data);
void eg_cp_wait_cb(void *opaque, bool begin);

/* eg_cp.c */
void eg_cp_kick(EvergreenState *s);
bool eg_cp_pending(EvergreenState *s);
void eg_dma_kick(EvergreenState *s);

/* eg_tile.c */
uint64_t eg_tile_offset(const EgTiling *t, uint32_t x, uint32_t y);
uint64_t eg_tile_size(const EgTiling *t);
void eg_tiling_update(EgTiling *t);
void eg_tiling_init(EvergreenState *s, EgTiling *t, uint32_t array_mode,
                    uint32_t bpp, uint32_t pitch, uint32_t height,
                    uint32_t banks_field, uint32_t bank_w_field,
                    uint32_t bank_h_field, uint32_t aspect_field,
                    uint32_t split_field, bool display);

/* eg_hdp.c */
bool eg_hdp_write(EvergreenState *s, uint32_t off, uint32_t val);
void eg_hdp_flush(EvergreenState *s);
void eg_hdp_reset(EvergreenState *s);

/* eg_dce.c */
void eg_dce_init(EvergreenState *s);
void eg_dce_reset(EvergreenState *s);
void eg_dce_fini(EvergreenState *s);
bool eg_dce_read(EvergreenState *s, uint32_t off, uint32_t *val);
bool eg_dce_write(EvergreenState *s, uint32_t off, uint32_t val);
void eg_dce_crtc_enable(EvergreenState *s, int i, bool enable);

/* eg_atom.c: the AtomBIOS mailbox of the synthetic VBIOS */
bool eg_atom_reg_write(EvergreenState *s, uint32_t off, uint32_t val);

#endif
