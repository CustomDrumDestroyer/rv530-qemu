#!/usr/bin/env python3
"""Build a synthetic AtomBIOS option ROM for the emulated RV530.

    mkrom.py OUT.rom [--device-id 0x71c5] [--subsys 106b:0080]

Nothing here comes from a real VBIOS: the layouts are those of the public
atombios.h (Linux drivers/gpu/drm/radeon, MIT licensed) and the values are
chosen for the device model.

Command tables are "paravirtual": each copies its parameter block (PS) into
the device's AtomBIOS mailbox registers, writes the table index to the
doorbell, and copies results back to PS for the tables that return some.
The device performs the operation in C (r5xx_atom.c).  Tables whose
parameters are not listed below are logged by the device and do nothing.

Data tables: FirmwareInfo 1.4 (27 MHz reference, pixel PLL limits, clocks),
SupportedDevicesInfo 2.1 (one DVI-I connector: CRT1 + DFP1 on DDC1),
GPIO_I2C_Info (the three AVIVO DDC GPIO pairs), GPIO_Pin_LUT (HPD1/2),
VRAM_UsageByFirmware (none) and an empty IndirectIOAccess.
"""
import argparse, struct, sys

# device mailbox (keep in sync with r5xx_atom.c / rv530.h)
MAILBOX = 0xff00            # 16 dwords of parameters / results
DOORBELL = 0xff7c           # write: table index | dword count << 8

# command table index (atombios.h ATOM_MASTER_LIST_OF_COMMAND_TABLES)
CMD_NAMES = """ASIC_Init GetDisplaySurfaceSize ASIC_RegistersInit VRAM_BlockVenderDetection
DIGxEncoderControl MemoryControllerInit EnableCRTCMemReq MemoryParamAdjust DVOEncoderControl
GPIOPinControl SetEngineClock SetMemoryClock SetPixelClock EnableDispPowerGating ResetMemoryDLL
ResetMemoryDevice MemoryPLLInit AdjustDisplayPll AdjustMemoryController EnableASIC_StaticPwrMgt
SetUniphyInstance DAC_LoadDetection LVTMAEncoderControl HW_Misc_Operation DAC1EncoderControl
DAC2EncoderControl DVOOutputControl CV1OutputControl GetConditionalGoldenSetting TVEncoderControl
TMDSAEncoderControl LVDSEncoderControl Gfx_Harvesting EnableScaler BlankCRTC EnableCRTC
GetPixelClock EnableVGA_Render GetSCLKOverMCLKRatio SetCRTC_Timing SetCRTC_OverScan
SetCRTC_Replication SelectCRTC_Source EnableGraphSurfaces UpdateCRTC_DoubleBufferRegisters
LUT_AutoFill EnableHW_IconCursor GetMemoryClock GetEngineClock SetCRTC_UsingDTDTiming
ExternalEncoderControl LVTMAOutputControl VRAM_BlockDetectionByStrap MemoryCleanUp
ProcessI2cChannelTransaction WriteOneByteToHWAssistedI2C ReadHWAssistedI2CStatus SpeedFanControl
PowerConnectorDetection MC_Synchronization ComputeMemoryEnginePLL MemoryRefreshConversion
VRAM_GetCurrentInfoBlock DynamicMemorySettings MemoryTraining EnableSpreadSpectrumOnPPLL
TMDSAOutputControl SetVoltage DAC1OutputControl DAC2OutputControl ComputeMemoryClockParam
ClockSource MemoryDeviceInit GetDispObjectInfo DIG1EncoderControl DIG2EncoderControl
DIG1TransmitterControl DIG2TransmitterControl ProcessAuxChannelTransaction DPEncoderService
GetVoltageInfo""".split()
assert len(CMD_NAMES) == 81

DATA_NAMES = """UtilityPipeLine MultimediaCapabilityInfo MultimediaConfigInfo StandardVESA_Timing
FirmwareInfo PaletteData LCD_Info DIGTransmitterInfo AnalogTV_Info SupportedDevicesInfo
GPIO_I2C_Info VRAM_UsageByFirmware GPIO_Pin_LUT VESA_ToInternalModeLUT ComponentVideoInfo
PowerPlayInfo CompassionateData SaveRestoreInfo PPLL_SS_Info OemInfo XTMDS_Info MclkSS_Info
Object_Header IndirectIOAccess MC_InitParameter ASIC_VDDC_Info ASIC_InternalSS_Info
TV_VideoMode VRAM_Info MemoryTrainingInfo IntegratedSystemInfo ASIC_ProfilingInfo
VoltageObjectInfo PowerSourceInfo""".split()
assert len(DATA_NAMES) == 34

# name: (parameter bytes the device reads, bytes it returns, format rev, content rev)
# Sizes are the parameter structs of atombios.h, never the larger
# *_PS_ALLOCATION unions: copying past the caller's struct would read or
# overwrite its stack.  The revisions are the R5xx-era interface versions
# drivers check before choosing a parameter layout.
CMDS = {
    "ASIC_Init":                        (0, 0, 1, 1),
    "EnableCRTCMemReq":                 (4, 0, 1, 1),
    "SetEngineClock":                   (4, 0, 1, 1),
    "SetMemoryClock":                   (4, 0, 1, 1),
    "SetPixelClock":                    (12, 0, 1, 2),
    "EnableASIC_StaticPwrMgt":          (4, 0, 1, 1),
    "DAC_LoadDetection":                (4, 0, 1, 2),
    "LVTMAEncoderControl":              (4, 0, 1, 1),
    "DAC1EncoderControl":               (4, 0, 1, 1),
    "DAC2EncoderControl":               (4, 0, 1, 1),
    "TMDSAEncoderControl":              (4, 0, 1, 1),
    "LVDSEncoderControl":               (4, 0, 1, 1),
    "EnableScaler":                     (4, 0, 1, 1),
    "BlankCRTC":                        (8, 0, 1, 1),
    "EnableCRTC":                       (4, 0, 1, 1),
    "GetPixelClock":                    (0, 0, 1, 1),
    "EnableVGA_Render":                 (4, 0, 1, 1),
    "SetCRTC_Timing":                   (24, 0, 1, 1),
    "SetCRTC_OverScan":                 (12, 0, 1, 1),
    "SelectCRTC_Source":                (4, 0, 1, 1),
    "EnableGraphSurfaces":              (4, 0, 1, 1),
    "UpdateCRTC_DoubleBufferRegisters": (4, 0, 1, 1),
    "LUT_AutoFill":                     (4, 0, 1, 1),
    "EnableHW_IconCursor":              (4, 0, 1, 1),
    "GetMemoryClock":                   (0, 4, 1, 1),
    "GetEngineClock":                   (0, 4, 1, 1),
    "SetCRTC_UsingDTDTiming":           (24, 0, 1, 1),
    "LVTMAOutputControl":               (4, 0, 1, 1),
    "ComputeMemoryEnginePLL":           (8, 8, 1, 1),
    "EnableSpreadSpectrumOnPPLL":       (8, 0, 1, 2),
    "TMDSAOutputControl":               (4, 0, 1, 1),
    "SetVoltage":                       (4, 0, 1, 1),
    "DAC1OutputControl":                (4, 0, 1, 1),
    "DAC2OutputControl":                (4, 0, 1, 1),
    "MemoryDeviceInit":                 (0, 0, 1, 1),
    "DynamicMemorySettings":            (0, 0, 1, 1),
}

# AtomBIOS opcodes (atom.c): MOVE to REG = 1, MOVE to PS = 2, EOT = 0x5b
OP_MOVE_REG, OP_MOVE_PS, OP_EOT = 0x01, 0x02, 0x5b
ARG_REG, ARG_PS, ARG_IMM = 0, 1, 5


def cmd_table(index, ps_in, ps_out, frev, crev):
    code = bytearray()
    for i in range(ps_in // 4):                      # MOVE REG[mb + i], PS[i]
        code += struct.pack('<BBHB', OP_MOVE_REG, ARG_PS, (MAILBOX >> 2) + i, i)
    # MOVE REG[doorbell], index | dwords << 8
    code += struct.pack('<BBHI', OP_MOVE_REG, ARG_IMM, DOORBELL >> 2,
                        index | (ps_in // 4) << 8)
    for i in range(ps_out // 4):                     # MOVE PS[i], REG[mb + i]
        code += struct.pack('<BBBH', OP_MOVE_PS, ARG_REG, i, (MAILBOX >> 2) + i)
    code.append(OP_EOT)
    # header: size, revisions, WS bytes, PS bytes
    return struct.pack('<HBBBB', 6 + len(code), frev, crev, 0, ps_in) + code


def hdr(body, frev, crev):
    return struct.pack('<HBB', 4 + len(body), frev, crev) + body


def firmware_info():
    """ATOM_FIRMWARE_INFO_V1_4 (89 bytes); clocks in 10 kHz units."""
    b = struct.pack('<IIIIIIIIII', 0x00010000,      # firmware revision
                    50000, 50000,                    # default engine/memory clock
                    50000, 50000,                    # driver target clocks
                    100000, 100000,                  # max engine/memory PLL output
                    110000,                          # max pixel PLL output (1.1 GHz)
                    60000, 60000)                    # ASIC max engine/memory clock
    b += struct.pack('<BBHHH', 90, 0, 1200, 0, 0)  # max temp, BL level, VDDC mV, LCD PLL
    b += struct.pack('<II', 50000,                   # 3D engine clock
                     64800)                          # min pixel PLL output (648 MHz)
    b += struct.pack('<HHHHHH', 100, 1350, 20000,    # engine PLL input min/max, output min
                     100, 1350, 20000)               # memory PLL input min/max, output min
    b += struct.pack('<HHHH', 40000,                 # max pixel clock 400 MHz
                     100, 1350,                      # pixel PLL input 1-13.5 MHz
                     64800 & 0xffff)                 # min pixel PLL output (low 16 bits)
    b += struct.pack('<HHHBBB', 0x0000,              # firmware capability
                     2700,                           # 27 MHz reference clock
                     0, 0, 0, 0)                     # PM RTS, design/memory module ID
    t = hdr(b, 1, 4)
    assert len(t) == 89, len(t)
    return t


# ATOM_DEVICE_*_INDEX order for asConnInfo[]
DEV_CRT1, DEV_LCD1, DEV_TV1, DEV_DFP1, DEV_CRT2, DEV_LCD2, DEV_TV2, DEV_DFP2, DEV_CV, DEV_DFP3 = range(10)
CONN_DVI_I = 2              # legacy connector type (supported_devices_connector_convert)
DAC_NONE, DAC_A = 0, 1
I2C_DDC1 = 0x90             # HW capable, engine 1, line 0: GPIO_I2C_Info entry 0


def supported_devices():
    """ATOM_SUPPORTED_DEVICES_INFO_2: CRT1 + DFP1 on one DVI-I connector, DDC1."""
    conn = [(0, 0)] * 16
    conn[DEV_CRT1] = ((CONN_DVI_I << 4) | DAC_A, I2C_DDC1)
    conn[DEV_DFP1] = ((CONN_DVI_I << 4) | DAC_NONE, I2C_DDC1)
    b = struct.pack('<H', (1 << DEV_CRT1) | (1 << DEV_DFP1))
    b += b''.join(struct.pack('<BB', c, i) for c, i in conn)
    b += bytes([0xff] * 16)                          # no interrupt sources mapped
    return hdr(b, 2, 1)


def gpio_i2c_info():
    """ATOM_GPIO_I2C_INFO: DC_GPIO_DDC1..3 (MASK, A, EN, Y at +0, +4, +8, +0xc);
    clock on bit 0, data on bit 8, as the device's GPIO model."""
    out = b''
    for line, base in enumerate((0x7e40, 0x7e50, 0x7e60)):
        mask, a, en, y = base >> 2, (base + 4) >> 2, (base + 8) >> 2, (base + 0xc) >> 2
        out += struct.pack('<HHHHHHHH', mask, en, y, a, mask, en, y, a)
        out += struct.pack('<B', 0x90 | line)        # HW capable, engine 1, line n
        out += struct.pack('<BBBBBBBBBB', 0, 0, 0, 0, 8, 8, 8, 8, 0, 0)
    return hdr(out, 1, 1)


def gpio_pin_lut():
    """ATOM_GPIO_PIN_LUT: HPD1/HPD2 sense bits (DC_HOT_PLUG_DETECTn_INT_STATUS
    bit 1), GPIO ids as the AVIVO HPD pins."""
    b = struct.pack('<HBB', 0x7d04 >> 2, 1, 0x14)
    b += struct.pack('<HBB', 0x7d14 >> 2, 1, 0x15)
    return hdr(b, 1, 1)


def vram_usage():
    """ATOM_VRAM_USAGE_BY_FIRMWARE: firmware reserves nothing."""
    return hdr(struct.pack('<IHH', 0, 0, 0), 1, 2)


def iio():
    """IndirectIOAccess with no programs (atom_index_iio stops at once)."""
    return hdr(b'\x00\x00\x00\x00', 1, 1)


def build(device_id, ss_vendor, ss_id):
    rom = bytearray(0x10000)
    # --- PCI option ROM header ---
    rom[0:3] = b'\x55\xaa\x00'
    rom[3:6] = b'\xcb\x90\x90'                       # legacy init: RETF
    struct.pack_into('<H', rom, 0x18, 0x60)          # PCIR (after 0x48)
    rom[0x30:0x3a] = b' 761295520'                   # AtomBIOS signature (atom.h)
    struct.pack_into('<H', rom, 0x48, 0x80)          # ATOM_ROM_HEADER
    # PCIR: vendor, device, vpd, len, rev, class, image len (blocks), code rev, type, last
    p = 0x60
    rom[p:p + 4] = b'PCIR'
    struct.pack_into('<HHHHB', rom, p + 4, 0x1002, device_id, 0, 0x18, 0)
    rom[p + 0xd:p + 0x10] = b'\x00\x00\x03'          # class 030000
    rom[p + 0x14] = 0                                # code type x86
    rom[p + 0x15] = 0x80                             # last image

    pos = 0xc0
    def put(blob, align=2):
        nonlocal pos
        pos = (pos + align - 1) & ~(align - 1)
        o = pos
        rom[o:o + len(blob)] = blob
        pos += len(blob)
        return o

    msg = put(b'\r\nRV530_QEMU synthetic AtomBIOS (clean-room, paravirtual)\r\n\x00')
    put(b'RV530-QEMU\x00')
    cmd_off = []
    for i, name in enumerate(CMD_NAMES):
        ps_in, ps_out, frev, crev = CMDS.get(name, (0, 0, 1, 1))
        cmd_off.append(put(cmd_table(i, ps_in, ps_out, frev, crev)))
    data = {"FirmwareInfo": firmware_info(), "SupportedDevicesInfo": supported_devices(),
            "GPIO_I2C_Info": gpio_i2c_info(), "GPIO_Pin_LUT": gpio_pin_lut(),
            "VRAM_UsageByFirmware": vram_usage(), "IndirectIOAccess": iio()}
    data_off = [put(data[n]) if n in data else 0 for n in DATA_NAMES]
    cmd_master = put(hdr(b''.join(struct.pack('<H', o) for o in cmd_off), 1, 1))
    data_master = put(hdr(b''.join(struct.pack('<H', o) for o in data_off), 1, 1))

    # --- ATOM_ROM_HEADER (36 bytes) at 0x80 ---
    h = struct.pack('<HBB', 36, 1, 1) + b'ATOM'
    h += struct.pack('<HHHHHHHHHHHHHBB', 0, 0, 0, 0, msg, 0, 0, 0, ss_vendor, ss_id,
                     0x60, cmd_master, data_master, 0, 0)
    assert len(h) == 36
    rom[0x80:0x80 + 36] = h

    blocks = (pos + 511) // 512
    size = blocks * 512
    rom = rom[:size]
    rom[2] = blocks
    struct.pack_into('<H', rom, p + 0x10, blocks)    # PCIR image length
    rom[size - 1] = 0
    rom[size - 1] = (-sum(rom)) & 0xff
    assert sum(rom) & 0xff == 0
    return bytes(rom)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('out')
    ap.add_argument('--device-id', type=lambda v: int(v, 0), default=0x71c5)
    ap.add_argument('--subsys', default='106b:0080')
    a = ap.parse_args()
    sv, si = (int(x, 16) for x in a.subsys.split(':'))
    rom = build(a.device_id, sv, si)
    open(a.out, 'wb').write(rom)
    print('%s: %d bytes' % (a.out, len(rom)))


if __name__ == '__main__':
    main()
