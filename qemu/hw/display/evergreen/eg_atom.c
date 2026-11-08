/*
 * Radeon HD 5770 AtomBIOS mailbox: the device side of the synthetic,
 * clean-room VBIOS that scripts/atombios/mkrom-evergreen.py builds.
 *
 * Every command table of that ROM copies its parameter block into
 * EG_ATOM_MAILBOX (16 dwords) and writes "table index | dwords << 8" to
 * EG_ATOM_DOORBELL.  Tables that return values copy the mailbox back to
 * their parameter space afterwards.  The operation itself happens here.
 * Parameter layouts are the atombios.h structs (little endian).
 *
 * The Apple driver programs most display engine registers itself.  The
 * tables that set clocks, voltages, encoders and transmitters have no
 * state in this model.  They do nothing, and the eg_atom_call trace logs
 * them.  EnableCRTC sets the master enable bit of the CRTC.  Without it,
 * there is no scanout and no vertical blank interrupt.  The other tables
 * below return values.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "evergreen.h"
#include "trace.h"

static const char *const cmd_names[81] = {
    "ASIC_Init", "GetDisplaySurfaceSize", "ASIC_RegistersInit",
    "VRAM_BlockVenderDetection", "DIGxEncoderControl", "MemoryControllerInit",
    "EnableCRTCMemReq", "MemoryParamAdjust", "DVOEncoderControl",
    "GPIOPinControl", "SetEngineClock", "SetMemoryClock", "SetPixelClock",
    "EnableDispPowerGating", "ResetMemoryDLL", "ResetMemoryDevice",
    "MemoryPLLInit", "AdjustDisplayPll", "AdjustMemoryController",
    "EnableASIC_StaticPwrMgt", "SetUniphyInstance", "DAC_LoadDetection",
    "LVTMAEncoderControl", "HW_Misc_Operation", "DAC1EncoderControl",
    "DAC2EncoderControl", "DVOOutputControl", "CV1OutputControl",
    "GetConditionalGoldenSetting", "TVEncoderControl", "TMDSAEncoderControl",
    "LVDSEncoderControl", "Gfx_Harvesting", "EnableScaler", "BlankCRTC",
    "EnableCRTC", "GetPixelClock", "EnableVGA_Render", "GetSCLKOverMCLKRatio",
    "SetCRTC_Timing", "SetCRTC_OverScan", "SetCRTC_Replication",
    "SelectCRTC_Source", "EnableGraphSurfaces", "UpdateCRTC_DoubleBufferRegisters",
    "LUT_AutoFill", "EnableHW_IconCursor", "GetMemoryClock", "GetEngineClock",
    "SetCRTC_UsingDTDTiming", "ExternalEncoderControl", "LVTMAOutputControl",
    "VRAM_BlockDetectionByStrap", "MemoryCleanUp", "ProcessI2cChannelTransaction",
    "WriteOneByteToHWAssistedI2C", "ReadHWAssistedI2CStatus", "SpeedFanControl",
    "PowerConnectorDetection", "MC_Synchronization", "ComputeMemoryEnginePLL",
    "MemoryRefreshConversion", "VRAM_GetCurrentInfoBlock", "DynamicMemorySettings",
    "MemoryTraining", "EnableSpreadSpectrumOnPPLL", "TMDSAOutputControl",
    "SetVoltage", "DAC1OutputControl", "DAC2OutputControl",
    "ComputeMemoryClockParam", "ClockSource", "MemoryDeviceInit",
    "GetDispObjectInfo", "DIG1EncoderControl", "DIG2EncoderControl",
    "DIG1TransmitterControl", "DIG2TransmitterControl",
    "ProcessAuxChannelTransaction", "DPEncoderService", "GetVoltageInfo",
};

/* command table indices (atombios.h ATOM_MASTER_LIST_OF_COMMAND_TABLES) */
enum {
    ATOM_AdjustDisplayPll = 17,
    ATOM_EnableCRTC = 35,
    ATOM_GetMemoryClock = 47,
    ATOM_GetEngineClock = 48,
    ATOM_PowerConnectorDetection = 58,
    ATOM_ComputeMemoryEnginePLL = 60,
    ATOM_ProcessAuxChannelTransaction = 78,
    ATOM_DPEncoderService = 79,
};

/* the clocks of the ROM's FirmwareInfo and PowerPlay tables, 10 kHz units */
#define ATOM_ENGINE_CLOCK   85000
#define ATOM_MEMORY_CLOCK   120000
#define ATOM_REF_CLOCK      2700

#define ATOM_AUX_TIMEOUT    1       /* ucReplyStatus: no sink answered */

static void eg_atom_call(EvergreenState *s, uint32_t doorbell)
{
    uint32_t *mb = &s->regs[EG_ATOM_MAILBOX >> 2];
    uint32_t idx = doorbell & 0xff;
    const char *name = idx < ARRAY_SIZE(cmd_names) ? cmd_names[idx] : "?";

    trace_eg_atom_call(idx, name, mb[0], mb[1], mb[2], mb[3]);
    switch (idx) {
    case ATOM_EnableCRTC:
        /* ENABLE_CRTC_PARAMETERS: ucCRTC, ucEnable */
        eg_dce_crtc_enable(s, mb[0] & 0xff, (mb[0] >> 8) & 0xff);
        break;
    case ATOM_GetEngineClock:
        mb[0] = ATOM_ENGINE_CLOCK;
        break;
    case ATOM_GetMemoryClock:
        mb[0] = ATOM_MEMORY_CLOCK;
        break;
    case ATOM_AdjustDisplayPll:
        /*
         * v3: the PLL runs at the pixel clock, with no reference or post
         * divider
         */
        mb[0] = mb[0] & 0xffff;
        mb[1] = 0;
        break;
    case ATOM_ComputeMemoryEnginePLL: {
        /* v2: the clock is exact, with reference and post divider 1 */
        uint32_t clk = mb[0] & 0xffffff;
        mb[0] = clk;
        mb[1] = 1 | (clk / ATOM_REF_CLOCK) << 8 | 1u << 24;
        break;
    }
    case ATOM_PowerConnectorDetection:
        mb[0] = 0;                  /* detected */
        break;
    case ATOM_ProcessAuxChannelTransaction:
        /* no DisplayPort sink: byte 5 ucReplyStatus, byte 6 ucDataOutLen */
        mb[1] = (mb[1] & 0xff0000ffu) | ATOM_AUX_TIMEOUT << 8;
        break;
    case ATOM_DPEncoderService:
        mb[1] &= ~0xffu;            /* ucStatus: no sink */
        break;
    default:
        break;
    }
}

bool eg_atom_reg_write(EvergreenState *s, uint32_t off, uint32_t val)
{
    if (off >= EG_ATOM_MAILBOX &&
        off < EG_ATOM_MAILBOX + 4 * EG_ATOM_MAILBOX_DWORDS) {
        s->regs[off >> 2] = val;
        return true;
    }
    if (off == EG_ATOM_DOORBELL) {
        eg_atom_call(s, val);
        return true;
    }
    return false;
}
