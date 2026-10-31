#!/usr/bin/env python3
"""Generate r5xx_regnames.c from the Linux radeon driver headers.

Usage: gen_regnames.py <linux-radeon-dir> <mesa-r300-dir> > r5xx_regnames.c
"""
import re, sys, os

linux = sys.argv[1]
mesa = sys.argv[2] if len(sys.argv) > 2 else None

# (file, name-filter regex, priority) lower priority number wins on collision
sources = [
    ("r500_reg.h",  r"^(AVIVO_|R500_|R300_|RV530_|R400_|RS690_|RADEON_)", 0),
    ("avivod.h",    r"^D[12]|^DC_|^LB_", 1),
    ("rv515d.h",    r"^R_[0-9A-F]{6}_", 2),
    ("rs600d.h",    r"^R_[0-9A-F]{6}_", 2),
    ("r520d.h",     r"^R_[0-9A-F]{6}_", 2),
    ("r300d.h",     r"^R_[0-9A-F]{6}_", 2),
    ("r100d.h",     r"^R_[0-9A-F]{6}_", 2),
    ("r300_reg.h",  r"^(R300_|R500_|R400_)", 3),
    ("radeon_reg.h", r"^(RADEON_|R300_|R200_|RV350_|R420_|RS400_)", 4),
]

regs = {}   # offset -> (prio, name)

def add(off, name, prio):
    if off < 0 or off >= 0x10000 or off & 3:
        return
    cur = regs.get(off)
    if cur is None or prio < cur[0]:
        regs[off] = (prio, name)

pat = re.compile(r"^\s*#\s*define\s+([A-Za-z_][A-Za-z0-9_]*)\s+(0[xX][0-9A-Fa-f]+)\s*(?:/\*.*\*/)?\s*$")
for fname, filt, prio in sources:
    path = os.path.join(linux, fname)
    if not os.path.exists(path):
        continue
    ff = re.compile(filt)
    for line in open(path, errors="replace"):
        m = pat.match(line)
        if not m:
            continue
        name, val = m.group(1), int(m.group(2), 16)
        if not ff.match(name):
            continue
        if name.startswith(("C_", "S_", "G_")) and "_" in name[2:8]:
            continue
        # R_00XXXX_NAME: offset encoded in the name; value must agree
        mm = re.match(r"^R_([0-9A-F]{6})_(.*)$", name)
        if mm:
            off = int(mm.group(1), 16)
            if off != val:
                continue
            nm = mm.group(2)
            # rv515d.h MC indirect regs are tiny indices; skip them here
            if fname == "rv515d.h" and val < 0x100:
                continue
            add(off, nm, prio)
            continue
        # skip bit masks / small bit values that are not offsets
        if "MASK" in name or name.endswith("_SHIFT") or name.endswith("_EN") and val < 0x10:
            continue
        # radeon_reg.h mixes PLL indices and tiny values; keep low offsets
        # only if nothing better exists (prio already low).
        if fname == "radeon_reg.h" and val < 0x100 and not re.search(r"_(CNTL|INDEX|DATA|STATUS|BASE|CONFIG|SCRATCH|BIOS|MEMSIZE|TOM|APER|HDP|SEPROM|GPIO|VIPH|VGA|VIDEOMUX|AGP|MC_|PCIE|RBBM|PLL)", name):
            continue
        add(val, name, prio)

# Mesa's r300_reg.h has the most complete 3D register list
if mesa:
    path = os.path.join(mesa, "r300_reg.h")
    if os.path.exists(path):
        for line in open(path, errors="replace"):
            m = pat.match(line)
            if not m:
                continue
            name, val = m.group(1), int(m.group(2), 16)
            if not re.match(r"^(R300_|R500_|R400_|RV350_)", name):
                continue
            if "MASK" in name or "SHIFT" in name:
                continue
            if val < 0x2000 and not (0x1000 <= val < 0x2000):
                continue
            add(val, name, 1)

# explicit important ones
explicit = {
    0x0000: "MM_INDEX", 0x0004: "MM_DATA", 0x0008: "CLOCK_CNTL_INDEX", 0x000c: "CLOCK_CNTL_DATA",
    0x0030: "PCIE_INDEX", 0x0034: "PCIE_DATA", 0x0040: "GEN_INT_CNTL", 0x0044: "GEN_INT_STATUS",
    0x0070: "MC_IND_INDEX", 0x0074: "MC_IND_DATA", 0x00f0: "RBBM_SOFT_RESET", 0x00f8: "CONFIG_MEMSIZE",
    0x0108: "CONFIG_APER_SIZE", 0x0130: "HOST_PATH_CNTL", 0x0134: "HDP_FB_LOCATION",
    0x0300: "VGA_RENDER_CONTROL", 0x0310: "VGA_MEMORY_BASE_ADDRESS", 0x0328: "VGA_HDP_CONTROL",
    0x0704: "CP_RB_CNTL", 0x0700: "CP_RB_BASE", 0x0710: "CP_RB_RPTR", 0x0714: "CP_RB_WPTR",
    0x070c: "CP_RB_RPTR_ADDR", 0x071c: "CP_RB_RPTR_WR", 0x0718: "CP_RB_WPTR_DELAY",
    0x0738: "CP_IB_BASE", 0x073c: "CP_IB_BUFSZ", 0x0740: "CP_CSQ_CNTL", 0x0744: "CP_CSQ_MODE",
    0x0770: "SCRATCH_UMSK", 0x0774: "SCRATCH_ADDR", 0x07c0: "CP_STAT",
    0x07d4: "CP_ME_RAM_ADDR", 0x07d8: "CP_ME_RAM_RADDR", 0x07dc: "CP_ME_RAM_DATAH", 0x07e0: "CP_ME_RAM_DATAL",
    0x0e40: "RBBM_STATUS", 0x1720: "WAIT_UNTIL", 0x1724: "ISYNC_CNTL",
    0x15e0: "SCRATCH_REG0", 0x15e4: "SCRATCH_REG1", 0x15e8: "SCRATCH_REG2", 0x15ec: "SCRATCH_REG3",
    0x15f0: "SCRATCH_REG4", 0x15f4: "SCRATCH_REG5", 0x15f8: "SCRATCH_REG6", 0x15fc: "SCRATCH_REG7",
    0x402c: "GB_PIPE_SELECT", 0x4124: "GB_PIPE_SELECT2", 0x4ad4: "R300_DST_PIPE_CONFIG",
    0x6080: "D1CRTC_CONTROL", 0x6110: "D1GRPH_PRIMARY_SURFACE_ADDRESS", 0x6118: "D1GRPH_SECONDARY_SURFACE_ADDRESS",
    0x7e40: "DC_GPIO_DDC1_MASK", 0x7e44: "DC_GPIO_DDC1_A", 0x7e48: "DC_GPIO_DDC1_EN", 0x7e4c: "DC_GPIO_DDC1_Y",
    0x7e50: "DC_GPIO_DDC2_MASK", 0x7e54: "DC_GPIO_DDC2_A", 0x7e58: "DC_GPIO_DDC2_EN", 0x7e5c: "DC_GPIO_DDC2_Y",
    0x7e60: "DC_GPIO_DDC3_MASK", 0x7e64: "DC_GPIO_DDC3_A", 0x7e68: "DC_GPIO_DDC3_EN", 0x7e6c: "DC_GPIO_DDC3_Y",
    0x7e30: "DC_GPIO_GENERIC_MASK", 0x7e34: "DC_GPIO_GENERIC_A", 0x7e38: "DC_GPIO_GENERIC_EN", 0x7e3c: "DC_GPIO_GENERIC_Y",
    0x7edc: "DISP_INTERRUPT_STATUS",
}
for off, name in explicit.items():
    add(off, name, -1)

mc = {0x00: "MC_STATUS", 0x01: "MC_FB_LOCATION", 0x02: "MC_AGP_LOCATION", 0x03: "MC_AGP_BASE",
      0x04: "MC_AGP_BASE_2", 0x05: "MC_CNTL", 0x06: "MC_MISC_LAT_TIMER?", 0x08: "MC_MISC_UMA_CNTL",
      0x0a: "MC_FB_LOCATION2?", 0x53: "MC_RD_SHIFT?", 0x7a: "MC_CNTL?"}
pcie = {0x10: "PCIE_TX_GART_CNTL", 0x11: "PCIE_TX_GART_BASE", 0x12: "PCIE_TX_GART_START_LO",
        0x13: "PCIE_TX_GART_START_HI", 0x14: "PCIE_TX_GART_END_LO", 0x15: "PCIE_TX_GART_END_HI",
        0x16: "PCIE_TX_GART_ERROR", 0x17: "PCIE_TX_DISCARD_RD_ADDR_LO", 0x18: "PCIE_TX_DISCARD_RD_ADDR_HI",
        0xa2: "PCIE_LC_LINK_WIDTH_CNTL", 0xa4: "PCIE_LC_SPEED_CNTL"}
# PLL indices from radeon_reg.h (RADEON_* with value < 0x40 in the PLL section) – hand list
pll = {0x00: "CLK_PIN_CNTL", 0x01: "PPLL_CNTL", 0x02: "PPLL_REF_DIV", 0x03: "PPLL_DIV_0", 0x04: "PPLL_DIV_1",
       0x05: "PPLL_DIV_2", 0x06: "PPLL_DIV_3", 0x07: "VCLK_ECP_CNTL", 0x08: "HTOTAL_CNTL",
       0x09: "M_SPLL_REF_FB_DIV", 0x0a: "SPLL_CNTL", 0x0b: "SCLK_CNTL", 0x0c: "MPLL_CNTL", 0x0d: "MCLK_CNTL",
       0x0e: "AGP_PLL_CNTL", 0x0f: "PLL_TEST_CNTL", 0x10: "P2PLL_CNTL", 0x11: "P2PLL_REF_DIV",
       0x12: "P2PLL_DIV_0", 0x13: "HTOTAL2_CNTL", 0x14: "PIXCLKS_CNTL", 0x15: "MCLK_MISC",
       0x16: "SCLK_MORE_CNTL", 0x17: "PLL_PWRMGT_CNTL", 0x1c: "VCLK_ECP_CNTL?", 0x1f: "MDLL_CKO?",
       0x24: "MDLL_RDCKA", 0x25: "MDLL_RDCKB", 0x2d: "SCLK_MORE_CNTL?"}

out = sys.stdout
out.write("/* Generated by scripts/gen_regnames.py - do not edit. */\n")
out.write("#include \"r5xx_core.h\"\n#include <stdlib.h>\n\n")
out.write("typedef struct { uint32_t off; const char *name; } RegName;\n\n")
def table(name, d, keyfn=lambda k: k):
    out.write("static const RegName %s[] = {\n" % name)
    for k in sorted(d):
        out.write("    { 0x%04x, \"%s\" },\n" % (keyfn(k), d[k][1] if isinstance(d[k], tuple) else d[k]))
    out.write("};\n\n")
table("mmio_names", regs)
table("mc_names", mc)
table("pcie_names", pcie)
table("pll_names", pll)
out.write("""static int cmp_reg(const void *a, const void *b)
{
    uint32_t ka = *(const uint32_t *)a, kb = ((const RegName *)b)->off;
    return ka < kb ? -1 : (ka > kb ? 1 : 0);
}

static const char *lookup(const RegName *t, size_t n, uint32_t off)
{
    const RegName *r = bsearch(&off, t, n, sizeof(*t), cmp_reg);
    return r ? r->name : NULL;
}

#define N(t) (sizeof(t) / sizeof((t)[0]))

const char *r5xx_regname(uint32_t offset)
{
    return lookup(mmio_names, N(mmio_names), offset & ~3u);
}

const char *r5xx_mc_regname(uint32_t index)
{
    return lookup(mc_names, N(mc_names), index);
}

const char *r5xx_pcie_regname(uint32_t index)
{
    return lookup(pcie_names, N(pcie_names), index);
}

const char *r5xx_pll_regname(uint32_t index)
{
    return lookup(pll_names, N(pll_names), index);
}

static const struct { uint32_t op; const char *name; } pm4_names[] = {
    { 0x10, "NOP" }, { 0x19, "NEXT_CHAR" }, { 0x1d, "PLY_NEXTSCAN" }, { 0x1e, "SET_SCISSORS" },
    { 0x23, "3D_RNDR_GEN_INDX_PRIM" }, { 0x24, "LOAD_MICROCODE" }, { 0x26, "WAIT_FOR_IDLE" },
    { 0x28, "3D_DRAW_VBUF" }, { 0x29, "3D_DRAW_IMMD" }, { 0x2a, "3D_DRAW_INDX" },
    { 0x2c, "LOAD_PALETTE" }, { 0x2f, "3D_LOAD_VBPNTR" }, { 0x32, "3D_CLEAR_ZMASK" },
    { 0x33, "INDX_BUFFER" }, { 0x34, "3D_DRAW_VBUF_2" }, { 0x35, "3D_DRAW_IMMD_2" },
    { 0x36, "3D_DRAW_INDX_2" }, { 0x37, "3D_CLEAR_HIZ" }, { 0x38, "3D_CLEAR_CMASK" },
    { 0x3d, "3D_DRAW_128" }, { 0x91, "CNTL_PAINT" }, { 0x92, "CNTL_BITBLT" }, { 0x93, "CNTL_SMALLTEXT" },
    { 0x94, "CNTL_HOSTDATA_BLT" }, { 0x95, "CNTL_POLYLINE" }, { 0x97, "CNTL_POLYSCANLINES" },
    { 0x98, "CNTL_PAINT_MULTI" }, { 0x99, "CNTL_BITBLT_MULTI" }, { 0x9a, "CNTL_TRANS_BITBLT" },
    { 0x9b, "BITBLT_MULTI" }, { 0x9c, "CNTL_HOSTDATA_BLT?" },
};

const char *r5xx_pm4_opname(uint32_t opcode)
{
    for (size_t i = 0; i < N(pm4_names); i++) {
        if (pm4_names[i].op == opcode) {
            return pm4_names[i].name;
        }
    }
    return NULL;
}
""")
