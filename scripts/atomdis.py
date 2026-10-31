#!/usr/bin/env python3
"""AtomBIOS command-table disassembler (follows drivers/gpu/drm/radeon/atom.c).

Usage: atomdis.py ROM [table-name-or-index-or-hexoffset ...]
       atomdis.py ROM --all
       atomdis.py ROM --iio          (dump the indirect IO programs)

Offsets printed on the left are absolute ROM offsets, as in the kernel's
"atombios stuck executing XXXX ... @ 0xYYYY" message, so `atomdis.py rom 0xCA76`
disassembles the table that starts at 0xCA76.
"""
import struct, sys

CMD_NAMES = ["ASIC_Init", "GetDisplaySurfaceSize", "ASIC_RegistersInit",
"VRAM_BlockVenderDetection", "SetClocksRatio", "MemoryControllerInit",
"GPIO_PinInit", "MemoryParamAdjust", "DVOEncoderControl",
"GPIOPinControl", "SetEngineClock", "SetMemoryClock", "SetPixelClock",
"DynamicClockGating", "ResetMemoryDLL", "ResetMemoryDevice",
"MemoryPLLInit", "EnableMemorySelfRefresh", "AdjustMemoryController",
"EnableASIC_StaticPwrMgt", "ASIC_StaticPwrMgtStatusChange",
"DAC_LoadDetection", "TMDS2EncoderControl", "LCD1OutputControl",
"DAC1EncoderControl", "DAC2EncoderControl", "DVOOutputControl",
"CV1OutputControl", "SetCRTC_DPM_State", "TVEncoderControl",
"TMDS1EncoderControl", "LVDSEncoderControl", "TV1OutputControl",
"EnableScaler", "BlankCRTC", "EnableCRTC", "GetPixelClock",
"EnableVGA_Render", "EnableVGA_Access", "SetCRTC_Timing",
"SetCRTC_OverScan", "SetCRTC_Replication", "SelectCRTC_Source",
"EnableGraphSurfaces", "UpdateCRTC_DoubleBufferRegisters",
"LUT_AutoFill", "EnableHW_IconCursor", "GetMemoryClock",
"GetEngineClock", "SetCRTC_UsingDTDTiming", "TVBootUpStdPinDetection",
"DFP2OutputControl", "VRAM_BlockDetectionByStrap", "MemoryCleanUp",
"ReadEDIDFromHWAssistedI2C", "WriteOneByteToHWAssistedI2C",
"ReadHWAssistedI2CStatus", "SpeedFanControl", "PowerConnectorDetection",
"MC_Synchronization", "ComputeMemoryEnginePLL", "MemoryRefreshConversion",
"VRAM_GetCurrentInfoBlock", "DynamicMemorySettings", "MemoryTraining",
"EnableLVDS_SS", "DFP1OutputControl", "SetVoltage", "CRT1OutputControl",
"CRT2OutputControl", "SetupHWAssistedI2CStatus", "ClockSource",
"MemoryDeviceInit", "EnableYUV"]
DATA_NAMES = ["UtilityPipeLine", "MultimediaCapabilityInfo", "MultimediaConfigInfo",
"StandardVESA_Timing", "FirmwareInfo", "PaletteData", "LCD_Info", "DIGTransmitterInfo",
"AnalogTV_Info", "SupportedDevicesInfo", "GPIO_I2C_Info", "VRAM_UsageByFirmware",
"GPIO_Pin_LUT", "VESA_ToInternalModeLUT", "ComponentVideoInfo", "PowerPlayInfo",
"CompassionateData", "SaveRestoreInfo", "PPLL_SS_Info", "OemInfo", "XTMDS_Info",
"MclkSS_Info", "Object_Header", "IndirectIOAccess", "MC_InitParameter",
"ASIC_VDDC_Info", "ASIC_InternalSS_Info", "TV_VideoMode", "VRAM_Info",
"MemoryTrainingInfo", "IntegratedSystemInfo", "ASIC_ProfilingInfo",
"VoltageObjectInfo", "PowerSourceInfo"]
IO_NAMES = ["MM", "PLL", "MC", "PCIE", "PCIE PORT"]

ARG_REG, ARG_PS, ARG_WS, ARG_FB, ARG_ID, ARG_IMM, ARG_PLL, ARG_MC = range(8)
ARGN = ["REG", "PS", "WS", "FB", "ID", "IMM", "PLL", "MC"]
ALIGN = ["[31:0]", "[15:0]", "[23:8]", "[31:16]", "[7:0]", "[15:8]", "[23:16]", "[31:24]"]
DST_TO_SRC = [[0, 0, 0, 0], [1, 2, 3, 0], [1, 2, 3, 0], [1, 2, 3, 0],
              [4, 5, 6, 7], [4, 5, 6, 7], [4, 5, 6, 7], [4, 5, 6, 7]]
DEF_DST = [0, 0, 1, 2, 0, 1, 2, 3]
WS_NAMES = {0x40: "QUOTIENT", 0x41: "REMAINDER", 0x42: "DATAPTR", 0x43: "SHIFT",
            0x44: "OR_MASK", 0x45: "AND_MASK", 0x46: "FB_WINDOW", 0x47: "ATTRIBUTES",
            0x48: "REGPTR"}
COND = ["", "EQ", "B", "A", "BE", "AE", "NE"]

# opcode table: (mnemonic, kind, arg)
OPS = [None]
for m in ["MOVE", "AND", "OR", "SHIFT_LEFT", "SHIFT_RIGHT", "MUL", "DIV", "ADD", "SUB"]:
    for a in range(6):
        OPS.append((m, "dstsrc" if m not in ("SHIFT_LEFT", "SHIFT_RIGHT") else "shift",
                    [ARG_REG, ARG_PS, ARG_WS, ARG_FB, ARG_PLL, ARG_MC][a]))
OPS += [("SET_ATI_PORT", "port", 0), ("SET_PCI_PORT", "port", 1), ("SET_SYSIO_PORT", "port", 2),
        ("SET_REG_BLOCK", "regblock", 0), ("SET_FB_BASE", "src", 0)]
for a in range(6):
    OPS.append(("COMPARE", "cmp", [ARG_REG, ARG_PS, ARG_WS, ARG_FB, ARG_PLL, ARG_MC][a]))
OPS.append(("SWITCH", "switch", 0))
for c in range(7):
    OPS.append(("JUMP" + ("_" + COND[c] if c else ""), "jump", c))
for a in range(6):
    OPS.append(("TEST", "cmp", [ARG_REG, ARG_PS, ARG_WS, ARG_FB, ARG_PLL, ARG_MC][a]))
OPS += [("DELAY_MS", "delay", 0), ("DELAY_US", "delay", 1), ("CALL_TABLE", "call", 0),
        ("REPEAT", "none", 0)]
for a in range(6):
    OPS.append(("CLEAR", "clear", [ARG_REG, ARG_PS, ARG_WS, ARG_FB, ARG_PLL, ARG_MC][a]))
OPS += [("NOP", "none", 0), ("EOT", "eot", 0)]
for a in range(6):
    OPS.append(("MASK", "mask", [ARG_REG, ARG_PS, ARG_WS, ARG_FB, ARG_PLL, ARG_MC][a]))
OPS += [("POSTCARD", "byte", 0), ("BEEP", "none", 0), ("SAVE_REG", "none", 0),
        ("RESTORE_REG", "none", 0), ("SET_DATA_BLOCK", "byte", 0)]
for m in ["XOR", "SHL", "SHR"]:
    for a in range(6):
        OPS.append((m, "dstsrc", [ARG_REG, ARG_PS, ARG_WS, ARG_FB, ARG_PLL, ARG_MC][a]))
OPS.append(("DEBUG", "none", 0))
assert len(OPS) == 122, len(OPS)

try:
    sys.path.insert(0, __import__("os").path.dirname(__file__))
    import regnames  # optional: {offset: name}
    REGNAMES = regnames.REGNAMES
except Exception:
    REGNAMES = {}


class Dis:
    def __init__(self, rom):
        self.d = rom
        hdr = struct.unpack_from('<H', rom, 0x48)[0]
        self.cmd_table = struct.unpack_from('<H', rom, hdr + 0x1e)[0]
        self.data_table = struct.unpack_from('<H', rom, hdr + 0x20)[0]
        n = (struct.unpack_from('<H', rom, self.cmd_table)[0] - 4) // 2
        self.cmds = [struct.unpack_from('<H', rom, self.cmd_table + 4 + 2 * i)[0] for i in range(n)]
        n = (struct.unpack_from('<H', rom, self.data_table)[0] - 4) // 2
        self.datas = [struct.unpack_from('<H', rom, self.data_table + 4 + 2 * i)[0] for i in range(n)]
        self.reg_block = 0
        self.io_mode = 0

    def u8(self, p): return self.d[p]
    def u16(self, p): return struct.unpack_from('<H', self.d, p)[0]
    def u32(self, p): return struct.unpack_from('<I', self.d, p)[0]

    def regname(self, idx):
        off = (idx + self.reg_block) * 4
        if self.io_mode == 0:
            n = REGNAMES.get(off)
            s = "REG[0x%04x]" % off
            return s + (" " + n if n else "")
        return "%s[0x%04x]" % (IO_NAMES[self.io_mode & 0x7f] if (self.io_mode & 0x7f) < len(IO_NAMES) else "IIO%d" % (self.io_mode & 0x7f), off)

    def src(self, attr, p, as_dst=False, arg=None):
        """decode a source operand; returns (text, newp)"""
        if arg is None:
            arg = attr & 7
        align = (attr >> 3) & 7
        if arg == ARG_REG:
            idx = self.u16(p); p += 2
            return self.regname(idx) + ALIGN[align], p
        if arg == ARG_PS:
            return "PS[%d]%s" % (self.u8(p), ALIGN[align]), p + 1
        if arg == ARG_WS:
            i = self.u8(p)
            return "WS[%s]%s" % (WS_NAMES.get(i, str(i)), ALIGN[align]), p + 1
        if arg == ARG_FB:
            return "FB[%d]%s" % (self.u8(p), ALIGN[align]), p + 1
        if arg == ARG_ID:
            idx = self.u16(p); p += 2
            return "ID[0x%04x]%s" % (idx, ALIGN[align]), p
        if arg == ARG_IMM:
            if align == 0:
                return "0x%08x" % self.u32(p), p + 4
            if align in (1, 2, 3):
                return "0x%04x" % self.u16(p), p + 2
            return "0x%02x" % self.u8(p), p + 1
        if arg == ARG_PLL:
            return "PLL[0x%02x]%s" % (self.u8(p), ALIGN[align]), p + 1
        if arg == ARG_MC:
            return "MC[0x%02x]%s" % (self.u8(p), ALIGN[align]), p + 1
        return "?", p

    def dst(self, arg, attr, p):
        align = DST_TO_SRC[(attr >> 3) & 7][(attr >> 6) & 3]
        return self.src((align << 3) | arg, p, arg=arg)

    def direct(self, align, p):
        if align == 0:
            return "0x%08x" % self.u32(p), p + 4
        if align in (1, 2, 3):
            return "0x%04x" % self.u16(p), p + 2
        return "0x%02x" % self.u8(p), p + 1

    def table(self, base, out):
        size = self.u16(base)
        ws = self.u8(base + 4)
        ps = self.u8(base + 5) & 0x7f
        end = base + size
        p = base + 6
        self.reg_block = 0
        self.io_mode = 0
        out.append("  ; table @0x%04x len %d ws %d ps %d" % (base, size, ws, ps))
        while p < end:
            start = p
            op = self.u8(p); p += 1
            if op == 0 or op >= len(OPS):
                out.append("  %04x: .db 0x%02x" % (start, op))
                continue
            name, kind, arg = OPS[op]
            txt = name
            if kind == "dstsrc":
                attr = self.u8(p); p += 1
                dtxt, p = self.dst(arg, attr, p)
                stxt, p = self.src(attr, p)
                txt = "%s %s, %s" % (name, dtxt, stxt)
            elif kind == "shift":
                attr = self.u8(p); p += 1
                attr = (attr & 0x38) | (DEF_DST[(attr & 0x38) >> 3] << 6)
                dtxt, p = self.dst(arg, attr, p)
                txt = "%s %s, %d" % (name, dtxt, self.u8(p)); p += 1
            elif kind == "cmp":
                attr = self.u8(p); p += 1
                dtxt, p = self.dst(arg, attr, p)
                stxt, p = self.src(attr, p)
                txt = "%s %s, %s" % (name, dtxt, stxt)
            elif kind == "mask":
                attr = self.u8(p); p += 1
                dtxt, p = self.dst(arg, attr, p)
                mtxt, p = self.direct((attr >> 3) & 7, p)
                stxt, p = self.src(attr, p)
                txt = "MASK %s, and=%s, or=%s" % (dtxt, mtxt, stxt)
            elif kind == "clear":
                attr = self.u8(p); p += 1
                attr = (attr & 0x38) | (DEF_DST[(attr & 0x38) >> 3] << 6)
                dtxt, p = self.dst(arg, attr, p)
                txt = "CLEAR %s" % dtxt
            elif kind == "port":
                if arg == 0:
                    port = self.u16(p); p += 2
                    self.io_mode = 0 if port == 0 else 0x80 | port
                    txt = "SET_ATI_PORT %d (%s)" % (port, IO_NAMES[port] if port < len(IO_NAMES) else "?")
                else:
                    p += 1
                    self.io_mode = arg
            elif kind == "regblock":
                self.reg_block = self.u16(p); p += 2
                txt = "SET_REG_BLOCK 0x%04x" % self.reg_block
            elif kind == "src":
                attr = self.u8(p); p += 1
                stxt, p = self.src(attr, p)
                txt = "%s %s" % (name, stxt)
            elif kind == "switch":
                attr = self.u8(p); p += 1
                stxt, p = self.src(attr, p)
                cases = []
                while self.u16(p) != 0x5A5A:
                    if self.u8(p) != 0x63:
                        cases.append("BAD")
                        break
                    p += 1
                    v, p = self.src((attr & 0x38) | ARG_IMM, p)
                    cases.append("%s->%04x" % (v, base + self.u16(p)))
                    p += 2
                p += 2
                txt = "SWITCH %s { %s }" % (stxt, " ".join(cases))
            elif kind == "jump":
                tgt = self.u16(p); p += 2
                txt = "%s 0x%04x" % (name, base + tgt)
            elif kind == "delay":
                txt = "%s %d" % (name, self.u8(p)); p += 1
            elif kind == "call":
                i = self.u8(p); p += 1
                txt = "CALL_TABLE %d (%s) @0x%04x" % (i, CMD_NAMES[i] if i < len(CMD_NAMES) else "?",
                                                     self.cmds[i] if i < len(self.cmds) else 0)
            elif kind == "byte":
                v = self.u8(p); p += 1
                txt = "%s %d" % (name, v)
                if name == "SET_DATA_BLOCK":
                    txt += " (%s)" % (DATA_NAMES[v] if v < len(DATA_NAMES) else "self" if v == 255 else "?")
            elif kind == "eot":
                pass
            out.append("  %04x: %-8s %s" % (start, self.d[start:p].hex()[:16], txt))
            if kind == "eot":
                break

    def iio(self, out):
        for i, o in enumerate(self.datas):
            if i == 23 and o:  # IndirectIOAccess
                base = o + 4
                IIO_LEN = [1, 2, 3, 3, 3, 3, 4, 4, 4, 3]
                while self.u8(base) == 1:
                    port = self.u8(base + 1)
                    out.append("IIO port %d (%s):" % (port, IO_NAMES[port] if port < len(IO_NAMES) else "?"))
                    base += 2
                    while self.u8(base) != 9:
                        op = self.u8(base)
                        a = self.d[base + 1:base + IIO_LEN[op]]
                        n = ["NOP", "START", "READ", "WRITE", "CLEAR", "SET", "MOVE_INDEX", "MOVE_ATTR", "MOVE_DATA", "END"][op]
                        if op in (2, 3):
                            out.append("   %s REG[0x%04x]" % (n, struct.unpack('<H', a)[0] * 4))
                        elif op in (4, 5):
                            out.append("   %s bits %d<<%d" % (n, a[0], a[1]))
                        elif op in (6, 7, 8):
                            out.append("   %s %d bits from src>>%d to <<%d" % (n, a[0], a[1], a[2]))
                        base += IIO_LEN[op]
                    base += 3


def main():
    rom = open(sys.argv[1], 'rb').read()
    dis = Dis(rom)
    args = sys.argv[2:]
    out = []
    if not args or args == ["--list"]:
        for i, o in enumerate(dis.cmds):
            if o:
                print("%2d %-34s @0x%04x len %d" % (i, CMD_NAMES[i] if i < len(CMD_NAMES) else "?", o, dis.u16(o)))
        return
    if args[0] == "--iio":
        dis.iio(out)
    elif args[0] == "--all":
        for i, o in enumerate(dis.cmds):
            if o:
                out.append("== %d %s" % (i, CMD_NAMES[i] if i < len(CMD_NAMES) else "?"))
                dis.table(o, out)
    else:
        for a in args:
            if a.lower().startswith("0x"):
                base = int(a, 16)
                names = [CMD_NAMES[i] for i, o in enumerate(dis.cmds) if o == base]
                out.append("== @0x%04x %s" % (base, names))
            elif a.isdigit():
                base = dis.cmds[int(a)]
                out.append("== %s %s" % (a, CMD_NAMES[int(a)]))
            else:
                i = CMD_NAMES.index(a)
                base = dis.cmds[i]
                out.append("== %d %s" % (i, a))
            dis.table(base, out)
    print("\n".join(out))


if __name__ == "__main__":
    main()
