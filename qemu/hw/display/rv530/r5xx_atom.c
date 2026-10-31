/*
 * RV530 AtomBIOS mailbox: the device side of the synthetic VBIOS built by
 * scripts/atombios/mkrom.py.
 *
 * Every command table of that ROM copies its parameter block into
 * RV_ATOM_MAILBOX (16 dwords) and writes "table index | dwords << 8" to
 * RV_ATOM_DOORBELL; tables that return values copy the mailbox back to
 * their parameter space afterwards.  The operation itself happens here, by
 * writing the same AVIVO registers a real table leaves behind (CRTC timing
 * in the AMD convention: counters start at sync, *_BLANK_START_END holds
 * blank start = active end in the low half and blank end = active start in
 * the high half, *_SYNC_A the sync width in the high half).  Parameter
 * layouts are the atombios.h structs (little endian).
 *
 * Tables without a handler (memory init, voltage, encoders and outputs,
 * which have no state in this model) succeed without doing anything; they
 * show up in the rv530_atom trace.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "rv530.h"
#include "trace.h"

/* command table indices (atombios.h ATOM_MASTER_LIST_OF_COMMAND_TABLES) */
enum {
    ATOM_ASIC_Init = 0,
    ATOM_EnableCRTCMemReq = 6,
    ATOM_SetEngineClock = 10,
    ATOM_SetMemoryClock = 11,
    ATOM_SetPixelClock = 12,
    ATOM_BlankCRTC = 34,
    ATOM_EnableCRTC = 35,
    ATOM_SetCRTC_Timing = 39,
    ATOM_GetMemoryClock = 47,
    ATOM_GetEngineClock = 48,
    ATOM_SetCRTC_UsingDTDTiming = 49,
    ATOM_ComputeMemoryEnginePLL = 60,
};

/* susModeMiscInfo (ATOM_MODE_MISC_INFO_ACCESS) */
#define ATOM_HSYNC_POLARITY  0x0002     /* set: negative */
#define ATOM_VSYNC_POLARITY  0x0004
#define ATOM_INTERLACE       0x0080

#define RV_D1CRTC_H_SYNC_A       0x6008
#define RV_D1CRTC_H_SYNC_A_CNTL  0x600c
#define RV_D1CRTC_V_SYNC_A       0x6028
#define RV_D1CRTC_V_SYNC_A_CNTL  0x602c
#define RV_D1CRTC_INTERLACE_CONTROL 0x6088
#define RV_CRTC_BLANK_DATA_EN    (1u << 8)

/* the clocks the ROM's FirmwareInfo reports, 10 kHz units */
#define ATOM_DEFAULT_CLOCK   50000

static uint32_t mb_u8(const uint8_t *p, int i)
{
    return p[i];
}

static uint32_t mb_u16(const uint8_t *p, int i)
{
    return p[i] | (p[i + 1] << 8);
}

static void crtc_write(RV530State *s, uint32_t crtc, uint32_t off, uint32_t val)
{
    rv530_reg_write(s, off + (crtc ? RV_CRTC_STRIDE : 0), val);
}

static uint32_t crtc_read(RV530State *s, uint32_t crtc, uint32_t off)
{
    return s->regs[(off + (crtc ? RV_CRTC_STRIDE : 0)) >> 2];
}

/* program one CRTC from totals, sync starts and widths (all in pixels/lines) */
static void set_timing(RV530State *s, uint32_t crtc,
                       uint32_t htotal, uint32_t hdisp, uint32_t hss, uint32_t hsw,
                       uint32_t vtotal, uint32_t vdisp, uint32_t vss, uint32_t vsw,
                       uint32_t misc)
{
    uint32_t hbe = htotal - hss, vbe = vtotal - vss;

    trace_rv530_atom_timing(crtc, hdisp, vdisp, htotal, vtotal);
    crtc_write(s, crtc, RV_D1CRTC_H_TOTAL, htotal - 1);
    crtc_write(s, crtc, RV_D1CRTC_H_BLANK_START_END, (hbe << 16) | (hbe + hdisp));
    crtc_write(s, crtc, RV_D1CRTC_H_SYNC_A, hsw << 16);
    crtc_write(s, crtc, RV_D1CRTC_H_SYNC_A_CNTL, (misc & ATOM_HSYNC_POLARITY) ? 1 : 0);
    crtc_write(s, crtc, RV_D1CRTC_V_TOTAL, vtotal - 1);
    crtc_write(s, crtc, RV_D1CRTC_V_BLANK_START_END, (vbe << 16) | (vbe + vdisp));
    crtc_write(s, crtc, RV_D1CRTC_V_SYNC_A, vsw << 16);
    crtc_write(s, crtc, RV_D1CRTC_V_SYNC_A_CNTL, (misc & ATOM_VSYNC_POLARITY) ? 1 : 0);
    crtc_write(s, crtc, RV_D1CRTC_INTERLACE_CONTROL, (misc & ATOM_INTERLACE) ? 1 : 0);
}

static void atom_call(RV530State *s, uint32_t table, uint32_t ndw)
{
    uint8_t p[RV_ATOM_MAILBOX_DWORDS * 4];
    uint32_t *mb = &s->regs[RV_ATOM_MAILBOX >> 2];
    uint32_t i, crtc;

    for (i = 0; i < RV_ATOM_MAILBOX_DWORDS; i++) {
        stl_le_p(p + 4 * i, i < ndw ? mb[i] : 0);
    }
    trace_rv530_atom_call(table, ndw, ldl_le_p(p), ldl_le_p(p + 4), ldl_le_p(p + 8));

    switch (table) {
    case ATOM_SetCRTC_Timing:
        /* SET_CRTC_TIMING_PARAMETERS: H total, disp, sync start, sync
         * width, the same for V, misc info, CRTC */
        set_timing(s, mb_u8(p, 18) & 1,
                   mb_u16(p, 0), mb_u16(p, 2), mb_u16(p, 4), mb_u16(p, 6),
                   mb_u16(p, 8), mb_u16(p, 10), mb_u16(p, 12), mb_u16(p, 14),
                   mb_u16(p, 16));
        break;
    case ATOM_SetCRTC_UsingDTDTiming: {
        /* SET_CRTC_USING_DTD_TIMING_PARAMETERS: H size, H blanking, V
         * size, V blanking, H sync offset, width, V sync offset, width,
         * misc info, H border, V border, CRTC */
        uint32_t hdisp = mb_u16(p, 0), hblank = mb_u16(p, 2);
        uint32_t vdisp = mb_u16(p, 4), vblank = mb_u16(p, 6);
        uint32_t hb = mb_u8(p, 18), vb = mb_u8(p, 19);
        set_timing(s, mb_u8(p, 20) & 1,
                   hdisp + hblank + 2 * hb, hdisp + 2 * hb,
                   hdisp + hb + mb_u16(p, 8), mb_u16(p, 10),
                   vdisp + vblank + 2 * vb, vdisp + 2 * vb,
                   vdisp + vb + mb_u16(p, 12), mb_u16(p, 14), mb_u16(p, 16));
        break;
    }
    case ATOM_EnableCRTC: {
        /* ENABLE_CRTC_PARAMETERS: CRTC, enable.  Display read requests
         * follow the CRTC (R5xx drivers never call EnableCRTCMemReq) */
        uint32_t v;
        crtc = mb_u8(p, 0) & 1;
        v = crtc_read(s, crtc, RV_D1CRTC_CONTROL);
        if (mb_u8(p, 1)) {
            v = (v | RV_CRTC_EN) & ~RV_CRTC_DISP_READ_REQUEST_DISABLE;
        } else {
            v = (v & ~RV_CRTC_EN) | RV_CRTC_DISP_READ_REQUEST_DISABLE;
        }
        crtc_write(s, crtc, RV_D1CRTC_CONTROL, v);
        break;
    }
    case ATOM_EnableCRTCMemReq:
        crtc = mb_u8(p, 0) & 1;
        crtc_write(s, crtc, RV_D1CRTC_CONTROL,
                   (crtc_read(s, crtc, RV_D1CRTC_CONTROL) &
                    ~RV_CRTC_DISP_READ_REQUEST_DISABLE) |
                   (mb_u8(p, 1) ? 0 : RV_CRTC_DISP_READ_REQUEST_DISABLE));
        break;
    case ATOM_BlankCRTC:
        /* BLANK_CRTC_PARAMETERS: CRTC, blanking, black color (not modelled) */
        crtc = mb_u8(p, 0) & 1;
        crtc_write(s, crtc, RV_D1CRTC_BLANK_CONTROL,
                   mb_u8(p, 1) ? RV_CRTC_BLANK_DATA_EN : 0);
        break;
    case ATOM_GetEngineClock:
    case ATOM_GetMemoryClock:
        mb[0] = ATOM_DEFAULT_CLOCK;
        break;
    case ATOM_ComputeMemoryEnginePLL:
        /* COMPUTE_MEMORY_ENGINE_PLL_PARAMETERS: clock in, post divider out */
        mb[1] = 1;
        break;
    case ATOM_SetPixelClock:
        /* the pixel PLLs report lock immediately; nothing else depends on
         * the dividers in this model */
    default:
        break;
    }
}

bool rv530_atom_reg_write(RV530State *s, uint32_t off, uint32_t val)
{
    if (off >= RV_ATOM_MAILBOX &&
        off < RV_ATOM_MAILBOX + 4 * RV_ATOM_MAILBOX_DWORDS) {
        s->regs[off >> 2] = val;
        return true;
    }
    if (off == RV_ATOM_DOORBELL) {
        atom_call(s, val & 0xff, MIN((val >> 8) & 0xff, RV_ATOM_MAILBOX_DWORDS));
        return true;
    }
    return false;
}
