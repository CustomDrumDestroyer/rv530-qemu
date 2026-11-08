#!/usr/bin/env python3
"""Build a synthetic AtomBIOS option ROM for the emulated Radeon HD 5770.

    mkrom-evergreen.py OUT.rom [--device-id 0x68b8] [--subsys 106b:00cf]

Nothing here comes from a real VBIOS.  The layouts are those of the public
atombios.h and ObjectID.h (Linux drivers/gpu/drm/radeon, MIT licensed).  The
values are chosen for the device model (qemu/hw/display/evergreen).

Command tables are paravirtual, as in mkrom.py.  Each table copies its
parameter block (PS) into the AtomBIOS mailbox registers of the device and
writes the table index to the doorbell.  Tables that return values then
copy the mailbox back to PS.  The device does the operation in C
(eg_atom.c).

Data tables:
- FirmwareInfo 2.1: clocks, PLL limits, 27 MHz reference clock.
- GPIO_I2C_Info: the eight DDC GPIO pairs of the device (eg_dce.c ddc_base).
- GPIO_Pin_LUT: the six HPD sense bits of DC_GPIO_HPD_A.
- Object_Header 1.3: two DisplayPort connectors and one dual-link DVI-I
  connector, with the DDC lines and HPD pins of the device model.  This is
  the board wiring that the Hoolock framebuffer of the Apple driver expects.
- PowerPlayInfo: a boot state and a performance state at the default clocks.
- VRAM_UsageByFirmware (none) and an empty IndirectIOAccess.
"""
import argparse, os, struct, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mkrom import CMD_NAMES, DATA_NAMES, OP_MOVE_REG, OP_MOVE_PS, OP_EOT, \
    ARG_PS, ARG_REG, ARG_IMM, hdr  # noqa: E402

# device mailbox (keep in sync with evergreen.h EG_ATOM_*)
MAILBOX = 0x1ff00           # 16 dwords of parameters / results
DOORBELL = 0x1ff7c          # write: table index | dword count << 8

# name: (parameter bytes the device reads, bytes it returns, format rev, content rev)
# The table set and the revisions are the interface of an Evergreen (DCE4)
# board: drivers check the revision before they select a parameter layout
# (Linux atombios_crtc.c, atombios_encoders.c).  A table that is not listed
# is absent (offset 0).  Parameter sizes are the atombios.h structs of that
# revision, never the larger *_PS_ALLOCATION unions.
P = 32                      # tables without a handler: log 8 dwords
CMDS = {
    "ASIC_Init":                        (8, 0, 1, 1),
    "GetDisplaySurfaceSize":            (P, 0, 1, 1),
    "ASIC_RegistersInit":               (0, 0, 1, 1),
    "VRAM_BlockVenderDetection":        (P, 0, 1, 1),
    "DIGxEncoderControl":               (16, 0, 1, 2),
    "MemoryControllerInit":             (P, 0, 1, 1),
    "EnableCRTCMemReq":                 (4, 0, 2, 1),
    "MemoryParamAdjust":                (P, 0, 1, 1),
    "GPIOPinControl":                   (8, 0, 2, 1),
    "SetEngineClock":                   (4, 0, 1, 1),
    "SetMemoryClock":                   (4, 0, 1, 1),
    "SetPixelClock":                    (20, 0, 1, 5),
    "ResetMemoryDLL":                   (P, 0, 1, 1),
    "ResetMemoryDevice":                (P, 0, 1, 1),
    "MemoryPLLInit":                    (P, 0, 1, 2),
    "AdjustDisplayPll":                 (8, 8, 1, 3),
    "AdjustMemoryController":           (P, 0, 1, 1),
    "EnableASIC_StaticPwrMgt":          (4, 0, 1, 1),
    "SetUniphyInstance":                (4, 0, 1, 2),
    "DAC_LoadDetection":                (4, 0, 1, 3),
    "DAC1EncoderControl":               (4, 0, 1, 1),
    "DAC2EncoderControl":               (4, 0, 1, 1),
    "DVOOutputControl":                 (P, 0, 1, 3),
    "GetConditionalGoldenSetting":      (P, 0, 1, 1),
    "EnableScaler":                     (4, 0, 1, 1),
    "BlankCRTC":                        (8, 0, 1, 1),
    "EnableCRTC":                       (4, 0, 1, 1),
    "GetPixelClock":                    (8, 0, 1, 2),
    "EnableVGA_Render":                 (4, 0, 2, 1),
    "GetSCLKOverMCLKRatio":             (P, 0, 1, 1),
    "SetCRTC_OverScan":                 (12, 0, 1, 1),
    "SelectCRTC_Source":                (4, 0, 1, 2),
    "EnableGraphSurfaces":              (4, 0, 1, 3),
    "UpdateCRTC_DoubleBufferRegisters": (4, 0, 1, 1),
    "LUT_AutoFill":                     (4, 0, 1, 2),
    "GetMemoryClock":                   (0, 4, 1, 1),
    "GetEngineClock":                   (0, 4, 1, 1),
    "SetCRTC_UsingDTDTiming":           (24, 0, 2, 3),
    "VRAM_BlockDetectionByStrap":       (P, 0, 1, 1),
    "MemoryCleanUp":                    (P, 0, 1, 1),
    "ProcessI2cChannelTransaction":     (8, 0, 1, 1),
    "SpeedFanControl":                  (P, 0, 1, 2),
    "PowerConnectorDetection":          (4, 4, 1, 1),
    "MC_Synchronization":               (P, 0, 1, 1),
    "ComputeMemoryEnginePLL":           (8, 8, 1, 2),
    "MemoryRefreshConversion":          (P, 0, 1, 1),
    "VRAM_GetCurrentInfoBlock":         (P, 0, 1, 1),
    "DynamicMemorySettings":            (P, 0, 1, 1),
    "MemoryTraining":                   (P, 0, 1, 1),
    "EnableSpreadSpectrumOnPPLL":       (12, 0, 1, 2),
    "SetVoltage":                       (4, 0, 1, 2),
    "DAC1OutputControl":                (4, 0, 1, 1),
    "DAC2OutputControl":                (4, 0, 1, 1),
    "ClockSource":                      (P, 0, 1, 1),
    "MemoryDeviceInit":                 (P, 0, 1, 1),
    "DIG1TransmitterControl":           (12, 0, 1, 3),
    "ProcessAuxChannelTransaction":     (8, 8, 1, 2),
    "DPEncoderService":                 (8, 8, 1, 2),
}


def cmd_table(index, ps_in, ps_out, frev, crev):
    code = bytearray()
    for i in range(ps_in // 4):                      # MOVE REG[mb + i], PS[i]
        code += struct.pack('<BBHB', OP_MOVE_REG, ARG_PS, (MAILBOX >> 2) + i, i)
    code += struct.pack('<BBHI', OP_MOVE_REG, ARG_IMM, DOORBELL >> 2,
                        index | (ps_in // 4) << 8)
    for i in range(ps_out // 4):                     # MOVE PS[i], REG[mb + i]
        code += struct.pack('<BBBH', OP_MOVE_PS, ARG_REG, i, (MAILBOX >> 2) + i)
    code.append(OP_EOT)
    # header: size, revisions, WS bytes, PS bytes
    return struct.pack('<HBBBB', 6 + len(code), frev, crev, 0, max(ps_in, ps_out)) + code


# --- clocks (10 kHz units) ---
ENGINE_CLOCK = 85000        # 850 MHz
MEMORY_CLOCK = 120000       # 1200 MHz
REF_CLOCK = 2700            # 27 MHz


def firmware_info():
    """ATOM_FIRMWARE_INFO_V2_1 (92 bytes)."""
    b = struct.pack('<IIIIIIIIII', 0x00010000,      # firmware revision
                    ENGINE_CLOCK, MEMORY_CLOCK,      # default engine/memory clock
                    0, 0,                            # reserved
                    120000, 140000,                  # max engine/memory PLL output
                    120000,                          # max pixel PLL output (1.2 GHz)
                    0,                               # binary altered info
                    60000)                           # default display engine clock
    b += struct.pack('<BBHHH', 0, 0, 1000, 0, 0)    # BL level, boot VDDC mV, LCD PLL
    b += struct.pack('<II', 0,                       # reserved
                     60000)                          # min pixel PLL output (600 MHz)
    b += struct.pack('<HHHHHHHHHH', 100, 1350, 20000,  # engine PLL input min/max, out min
                     100, 1350, 20000,               # memory PLL input min/max, out min
                     40000,                          # max pixel clock (400 MHz)
                     100, 1350,                      # pixel PLL input min/max
                     60000 & 0xffff)                 # min pixel PLL output, low 16 bits
    b += struct.pack('<HHHHB3x', 0x0000,            # firmware capability
                     REF_CLOCK, REF_CLOCK,           # core and memory reference clock
                     0,                              # DP: PPLL gives the UNIPHY clock
                     0)                              # memory module ID
    t = hdr(b, 2, 1)
    assert len(t) == 92, len(t)
    return t


# DDC GPIO pairs, in the order of eg_dce.c ddc_base[]: GPIO_I2C_Info entry n
# is I2C line n.  Clock on bit 0, data on bit 8.
DDC_BASE = (0x6430, 0x6440, 0x6450, 0x6460, 0x6470, 0x6480, 0x6560, 0x64d8)


def i2c_id(line):
    return 0x90 | line      # ATOM_I2C_ID_CONFIG: HW capable, engine 1, line


def gpio_i2c_info():
    """ATOM_GPIO_I2C_INFO: one ATOM_GPIO_I2C_ASSIGMENT for each DDC pair
    (MASK, A, EN, Y at +0, +4, +8, +0xc)."""
    out = b''
    for line, base in enumerate(DDC_BASE):
        mask, a, en, y = base >> 2, (base + 4) >> 2, (base + 8) >> 2, (base + 0xc) >> 2
        out += struct.pack('<HHHHHHHH', mask, en, y, a, mask, en, y, a)
        out += struct.pack('<B', i2c_id(line))
        out += struct.pack('<BBBBBBBBBB', 0, 0, 0, 0, 8, 8, 8, 8, 0, 0)
    return hdr(out, 1, 1)


# HPD sense bits of DC_GPIO_HPD_A (Linux radeon_atom_get_hpd_info_from_gpio)
HPD_A = 0x64b4
HPD_SHIFT = (0, 8, 16, 24, 26, 28)
HPD_GPIO_ID = 1             # GPIO id of HPDn (n from 0) is HPD_GPIO_ID + n


def gpio_pin_lut():
    """ATOM_GPIO_PIN_LUT: one ATOM_GPIO_PIN_ASSIGNMENT for each HPD pin."""
    b = b''.join(struct.pack('<HBB', HPD_A >> 2, sh, HPD_GPIO_ID + n)
                 for n, sh in enumerate(HPD_SHIFT))
    return hdr(b, 1, 1)


# ObjectID.h: object id = type << 12 | enum << 8 | id
def obj(t, i, enum=1):
    return t << 12 | enum << 8 | i


GPU, ENCODER, CONNECTOR = 1, 2, 3
ENC_DAC1, ENC_UNIPHY1, ENC_UNIPHY2 = 0x15, 0x20, 0x21
CONN_DVI_I_DL, CONN_DP = 0x02, 0x13
DEV_CRT1, DEV_DFP1, DEV_DFP2, DEV_DFP3 = 0x0001, 0x0008, 0x0080, 0x0200
REC_I2C, REC_HPD, REC_DEVICE_TAG, REC_END = 1, 2, 4, 0xff
# ACPI display output IDs (ACPI _DOD): CRT, then the digital outputs
ACPI_ID = {DEV_CRT1: 0x0100, DEV_DFP1: 0x0210, DEV_DFP2: 0x0220, DEV_DFP3: 0x0230}

# The board wiring of the Mac Pro HD 5770 (one dual-link DVI-I and two
# Mini DisplayPort connectors), which the Hoolock framebuffer of the Apple
# driver expects.  Each entry: connector object, device tags (in order),
# encoders (in device-tag order), I2C line, HPD pin (from 0).  The DDC
# lines and HPD pins are those of the device model (eg_dce.c, monitor-ddc
# and monitor-hpd).
CONNECTORS = (
    (obj(CONNECTOR, CONN_DP, 1), (DEV_DFP1,), (obj(ENCODER, ENC_UNIPHY1),), 1, 3),
    (obj(CONNECTOR, CONN_DP, 2), (DEV_DFP2,), (obj(ENCODER, ENC_UNIPHY1, 2),), 0, 4),
    (obj(CONNECTOR, CONN_DVI_I_DL), (DEV_DFP3, DEV_CRT1),
     (obj(ENCODER, ENC_UNIPHY2), obj(ENCODER, ENC_DAC1)), 2, 0),
)


def object_header():
    """ATOM_OBJECT_HEADER_V3 with connector, encoder and display path tables.
    All offsets are from the start of the table."""
    encoders = []
    for c in CONNECTORS:
        for e in c[2]:
            if e not in encoders:
                encoders.append(e)
    support = 0
    for c in CONNECTORS:
        for t in c[1]:
            support |= t

    body = bytearray()
    base = 4 + 14                                    # common header + V3 fields

    def here():
        return base + len(body)

    # display paths: one for each device tag
    paths = bytearray()
    npaths = 0
    for conn, tags, encs, _, _ in CONNECTORS:
        for tag, enc in zip(tags, encs):
            paths += struct.pack('<HHHHH', tag, 10, conn, obj(GPU, 0), enc)
            npaths += 1
    path_off = here()
    body += struct.pack('<BBxx', npaths, 1) + paths

    # connector object table, then each connector's source list and records
    conn_off = here()
    ctab_len = 4 + 8 * len(CONNECTORS)
    tail = bytearray()
    entries = bytearray()
    for conn, tags, encs, line, hpd in CONNECTORS:
        sd = here() + ctab_len + len(tail)
        tail += struct.pack('<B', len(encs)) + b''.join(struct.pack('<H', e) for e in encs)
        tail += struct.pack('<B', 0)                 # no destinations
        rec = here() + ctab_len + len(tail)
        # ATOM_CONNECTOR_DEVICE_TAG_RECORD
        tail += struct.pack('<BBBB', REC_DEVICE_TAG, 4 + 8 * len(tags), len(tags), 0)
        tail += b''.join(struct.pack('<IHH', ACPI_ID[t], t, 0) for t in tags)
        tail += struct.pack('<BBBB', REC_I2C, 4, i2c_id(line), 0)
        tail += struct.pack('<BBBB', REC_HPD, 4, HPD_GPIO_ID + hpd, 0)
        tail += struct.pack('<B', REC_END)
        entries += struct.pack('<HHHH', conn, sd, rec, 0)
    body += struct.pack('<Bxxx', len(CONNECTORS)) + entries + tail

    # encoder object table: the GPU is the source of each encoder, and its
    # connector is the destination
    dests = {e: c[0] for c in CONNECTORS for e in c[2]}
    enc_off = here()
    etab_len = 4 + 8 * len(encoders)
    tail = bytearray()
    entries = bytearray()
    for e in encoders:
        sd = here() + etab_len + len(tail)
        tail += struct.pack('<BHBH', 1, obj(GPU, 0), 1, dests[e])
        rec = here() + etab_len + len(tail)
        tail += struct.pack('<B', REC_END)
        entries += struct.pack('<HHHH', e, sd, rec, 0)
    body += struct.pack('<Bxxx', len(encoders)) + entries + tail

    # empty router and misc tables
    empty_off = here()
    body += struct.pack('<Bxxx', 0)

    v3 = struct.pack('<HHHHHHH', support, conn_off, empty_off, enc_off, 0,
                     path_off, empty_off)
    return hdr(v3 + bytes(body), 1, 3)


# PowerPlay (Linux pptable.h, ATOM_PPLIB_POWERPLAYTABLE3).  The Apple
# driver needs this table: without it, the driver reads other ROM bytes as
# the table and the kernel panics.
PP_LEVELS = 3               # clock levels in each state (low, medium, high)
PP_CLASS_BOOT, PP_UI_PERFORMANCE = 0x0008, 5
PP_THERMAL_EVERGREEN = 12
VDDC_MV = 1000


def powerplay():
    """ATOM_PPLIB_POWERPLAYTABLE3 with two states (boot, performance).  Each
    state has PP_LEVELS ATOM_PPLIB_EVERGREEN_CLOCK_INFO entries at the
    default clocks, and one ATOM_PPLIB_NONCLOCK_INFO (version 2, 24 bytes)."""
    states = ((PP_CLASS_BOOT, 0), (PP_UI_PERFORMANCE, 1))
    head = 46                                        # POWERPLAYTABLE3 (packed), header included
    entry = 1 + PP_LEVELS
    state_off = head
    clock_off = state_off + entry * len(states)
    clock = struct.pack('<HBHBHHHI', ENGINE_CLOCK & 0xffff, ENGINE_CLOCK >> 16,
                        MEMORY_CLOCK & 0xffff, MEMORY_CLOCK >> 16,
                        VDDC_MV, 0, 0, 0)
    assert len(clock) == 16
    nonclock_off = clock_off + len(clock)
    nonclock = b''.join(struct.pack('<HBBIBHII5x', cls, 0, 0, 0, 0, 0, 0, 0)
                        for cls, _ in states)
    assert len(nonclock) == 24 * len(states)
    # every level of every state uses clock info 0
    state_arr = b''.join(struct.pack('<B', i) + bytes(PP_LEVELS) for _, i in states)
    thermal = struct.pack('<BBBBBBBB', PP_THERMAL_EVERGREEN, 0, 0, 0, 0, 0, 0, 0)
    t = struct.pack('<BBBBBHHHHHHI', 0, len(states), entry, len(clock), 24,
                    state_off, clock_off, nonclock_off, 0, 0, head, 0)
    t += thermal + struct.pack('<HH', clock_off, nonclock_off)
    t += struct.pack('<BH', 0, 0)                    # no custom thermal policies
    t += struct.pack('<HHH', 0, 0, 0)                # format ID, fan table, extended header
    t = hdr(t + state_arr + clock + nonclock, 5, 1)
    assert struct.unpack_from('<H', t, 4 + 5 + 10)[0] == head
    return t


def vram_usage():
    """ATOM_VRAM_USAGE_BY_FIRMWARE: firmware reserves nothing."""
    return hdr(struct.pack('<IHH', 0, 0, 0), 1, 4)


def iio():
    """IndirectIOAccess with no programs (atom_index_iio stops at once)."""
    return hdr(b'\x00\x00\x00\x00', 1, 1)


# The DCE4 master command table has 80 entries (it ends at DPEncoderService).
# The Apple driver rejects a ROM with a longer table.
NUM_CMDS = 80
IMAGE_BLOCKS = 120          # 60 KiB, the size of the flash image of the card


# Identity strings in the layout of AMD option ROMs, as amdgpu's atom.c reads
# them.  The number of strings is at 0x2f.  A pointer to the first string
# (part number, chip, bus, description) is at 0x6e.  The build date is at
# 0x50.  The Apple driver rejects a ROM without them.
STRINGS = (b'113-QEMU5770-001', b'JUNIPER', b'PCI_EXPRESS',
           b'HD5770_QEMU synthetic AtomBIOS (clean-room, paravirtual)')
DATE = b'11/08/26 00:00'
VERSION = b'ATOMBIOSBK-AMD VER012.020.000.000.000000'


def build(device_id, ss_vendor, ss_id):
    rom = bytearray(0x10000)
    # --- PCI option ROM header ---
    rom[0:3] = b'\x55\xaa\x00'
    rom[3:6] = b'\xcb\x90\x90'                       # legacy init: RETF
    rom[0x2f] = len(STRINGS)
    rom[0x30:0x3a] = b' 761295520'                   # AtomBIOS signature (atom.h)
    rom[0x50:0x50 + len(DATE)] = DATE
    p = 0x80                                         # PCIR
    struct.pack_into('<H', rom, 0x18, p)
    rom[p:p + 4] = b'PCIR'
    struct.pack_into('<HHHHB', rom, p + 4, 0x1002, device_id, 0, 0x18, 0)
    rom[p + 0xd:p + 0x10] = b'\x00\x00\x03'          # class 030000
    rom[p + 0x14] = 0                                # code type x86
    rom[p + 0x15] = 0x80                             # last image

    pos = 0xa0

    def put(blob, align=2):
        nonlocal pos
        pos = (pos + align - 1) & ~(align - 1)
        o = pos
        rom[o:o + len(blob)] = blob
        pos += len(blob)
        return o

    first = put(b''.join(t + b'\x00' for t in STRINGS), 1)
    struct.pack_into('<H', rom, 0x6e, first)
    msg = first + sum(len(t) + 1 for t in STRINGS[:3])
    put(VERSION + b'\x00', 1)
    atom_hdr = put(bytes(36), 4)
    struct.pack_into('<H', rom, 0x48, atom_hdr)      # ATOM_ROM_HEADER
    cmd_off = []
    for i, name in enumerate(CMD_NAMES[:NUM_CMDS]):
        if name in CMDS:
            cmd_off.append(put(cmd_table(i, *CMDS[name])))
        else:
            cmd_off.append(0)
    data = {"FirmwareInfo": firmware_info(), "GPIO_I2C_Info": gpio_i2c_info(),
            "GPIO_Pin_LUT": gpio_pin_lut(), "Object_Header": object_header(),
            "VRAM_UsageByFirmware": vram_usage(), "IndirectIOAccess": iio(),
            "PowerPlayInfo": powerplay()}
    data_off = [put(data[n]) if n in data else 0 for n in DATA_NAMES]
    cmd_master = put(hdr(b''.join(struct.pack('<H', o) for o in cmd_off), 1, 1))
    data_master = put(hdr(b''.join(struct.pack('<H', o) for o in data_off), 1, 1))

    # --- ATOM_ROM_HEADER (36 bytes) ---
    h = struct.pack('<HBB', 36, 1, 1) + b'ATOM'
    h += struct.pack('<HHHHHHHHHHHHHBB', 0xc000, 0, 0, 0, msg, 0, 0, 0, ss_vendor, ss_id,
                     p, cmd_master, data_master, 0, 0)
    assert len(h) == 36
    rom[atom_hdr:atom_hdr + 36] = h

    blocks = max((pos + 511) // 512, IMAGE_BLOCKS)
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
    ap.add_argument('--device-id', type=lambda v: int(v, 0), default=0x68b8)
    ap.add_argument('--subsys', default='106b:00cf')
    a = ap.parse_args()
    sv, si = (int(x, 16) for x in a.subsys.split(':'))
    rom = build(a.device_id, sv, si)
    open(a.out, 'wb').write(rom)
    print('%s: %d bytes' % (a.out, len(rom)))


if __name__ == '__main__':
    main()
