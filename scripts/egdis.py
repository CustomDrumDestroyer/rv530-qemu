#!/usr/bin/env python3
"""Evergreen (Radeon HD 5000) PM4 and shader disassembler.

Library used by egdump.py; register names come from Mesa's evergreend.h
and Linux's evergreend.h/evergreen_reg.h when reference/ is populated
(scripts/fetch-references.sh), opcode numbers from Mesa r600_isa.c.

    egdis.py shader FILE [OFFSET]     disassemble raw shader dwords (LE)
"""
import os
import re
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REF = os.path.join(HERE, '..', 'reference')

_names = None


def regname(off):
    global _names
    if _names is None:
        _names = {}
        for f in ('mesa/src/gallium/drivers/r600/evergreend.h',
                  'linux-radeon/evergreend.h', 'linux-radeon/evergreen_reg.h'):
            try:
                txt = open(os.path.join(REF, f)).read()
            except OSError:
                continue
            for m in re.finditer(r'#define\s+(?:R_[0-9A-Fa-f]+_)?([A-Z][A-Z0-9_]+)\s+(0x[0-9a-fA-F]+)\b', txt):
                v = int(m.group(2), 16)
                if 0x100 <= v < 0x40000 and v % 4 == 0 and v not in _names:
                    _names[v] = m.group(1)
    return _names.get(off, '%05x' % off)


PKT3 = {0x10: 'NOP', 0x11: 'SET_BASE', 0x12: 'CLEAR_STATE', 0x13: 'INDEX_BUFFER_SIZE',
        0x15: 'DISPATCH_DIRECT', 0x16: 'DISPATCH_INDIRECT', 0x17: 'INDIRECT_BUFFER_END',
        0x18: 'MODE_CONTROL', 0x20: 'SET_PREDICATION', 0x21: 'REG_RMW', 0x22: 'COND_EXEC',
        0x23: 'PRED_EXEC', 0x24: 'DRAW_INDIRECT', 0x25: 'DRAW_INDEX_INDIRECT',
        0x26: 'INDEX_BASE', 0x27: 'DRAW_INDEX_2', 0x28: 'CONTEXT_CONTROL',
        0x29: 'DRAW_INDEX_OFFSET', 0x2a: 'INDEX_TYPE', 0x2b: 'DRAW_INDEX',
        0x2d: 'DRAW_INDEX_AUTO', 0x2e: 'DRAW_INDEX_IMMD', 0x2f: 'NUM_INSTANCES',
        0x30: 'DRAW_INDEX_MULTI_AUTO', 0x32: 'INDIRECT_BUFFER', 0x34: 'STRMOUT_BUFFER_UPDATE',
        0x35: 'DRAW_INDEX_OFFSET_2', 0x36: 'DRAW_INDEX_MULTI_ELEMENT', 0x39: 'MEM_SEMAPHORE',
        0x3a: 'MPEG_INDEX', 0x3b: 'COPY_DW', 0x3c: 'WAIT_REG_MEM', 0x3d: 'MEM_WRITE',
        0x41: 'CP_DMA', 0x42: 'PFP_SYNC_ME', 0x43: 'SURFACE_SYNC', 0x44: 'ME_INITIALIZE',
        0x45: 'COND_WRITE', 0x46: 'EVENT_WRITE', 0x47: 'EVENT_WRITE_EOP',
        0x48: 'EVENT_WRITE_EOS', 0x4a: 'PREAMBLE_CNTL', 0x4b: 'RB_OFFSET',
        0x4c: 'ALU_PS_CONST_BUFFER_COPY', 0x4d: 'ALU_VS_CONST_BUFFER_COPY',
        0x4e: 'ALU_PS_CONST_UPDATE', 0x4f: 'ALU_VS_CONST_UPDATE', 0x57: 'ONE_REG_WRITE',
        0x68: 'SET_CONFIG_REG', 0x69: 'SET_CONTEXT_REG', 0x6a: 'SET_ALU_CONST',
        0x6b: 'SET_BOOL_CONST', 0x6c: 'SET_LOOP_CONST', 0x6d: 'SET_RESOURCE',
        0x6e: 'SET_SAMPLER', 0x6f: 'SET_CTL_CONST', 0x70: 'SET_RESOURCE_OFFSET',
        0x71: 'SET_ALU_CONST_VS', 0x72: 'SET_ALU_CONST_DI', 0x73: 'SET_CONTEXT_REG_INDIRECT',
        0x74: 'SET_RESOURCE_INDIRECT', 0x75: 'SET_APPEND_CNT'}
SET_BASE = {0x68: 0x8000, 0x69: 0x28000, 0x6a: 0x30000, 0x6b: 0x3a500, 0x6c: 0x3a200,
            0x6d: 0x30000, 0x6e: 0x3c000, 0x6f: 0x3cff0}


def pm4(w, out=print, ib=None):
    """Decode a PM4 stream; ib(addr, ndw) is called for INDIRECT_BUFFER."""
    i = 0
    while i < len(w):
        h = w[i]
        t = h >> 30
        if t == 3:
            op = (h >> 8) & 0xff
            n = ((h >> 16) & 0x3fff) + 1
            body = w[i + 1:i + 1 + n]
            name = PKT3.get(op, 'PKT3_%02x' % op)
            if op in (0x68, 0x69) and body:
                base = SET_BASE[op] + body[0] * 4
                out('%5x %s' % (i, name))
                for k, v in enumerate(body[1:]):
                    out('        %-34s = %08x' % (regname(base + 4 * k), v))
            elif op in (0x6d, 0x6e) and body:
                per = 8 if op == 0x6d else 3
                out('%5x %s #%d..%d' % (i, name, body[0] // per, body[0] // per + (n - 1) // per - 1))
                for k in range(0, n - 1, per):
                    r = body[1 + k:1 + k + per]
                    if any(r):
                        out('        #%-4d %s' % (body[0] // per + k // per, ' '.join('%08x' % x for x in r)))
            elif op == 0x6a and body:
                vals = body[1:]
                fl = ' '.join('%g' % struct.unpack('<f', struct.pack('<I', x))[0] for x in vals[:16])
                out('%5x SET_ALU_CONST @%d (vec4 %d): %s' % (i, body[0], body[0] // 4, fl))
            else:
                out('%5x %s(%d) %s' % (i, name, n, ' '.join('%08x' % x for x in body[:12])))
            if op == 0x32 and ib and len(body) >= 3:
                ib(body[0] | (body[1] & 0xff) << 32, body[2] & 0xfffff)
            i += n + 1
        elif t == 0:
            reg = (h & 0xffff) << 2
            n = ((h >> 16) & 0x3fff) + 1
            for k in range(n):
                out('%5x PKT0 %-30s = %08x' % (i, regname(reg + 4 * k), w[i + 1 + k] if i + 1 + k < len(w) else 0))
            i += n + 1
        elif t == 2:
            i += 1
        else:
            out('%5x ?? %08x' % (i, h))
            i += 1


# ---------------------------------------------------------------- shaders

ALU2 = {0x00: 'ADD', 0x01: 'MUL', 0x02: 'MUL_IEEE', 0x03: 'MAX', 0x04: 'MIN', 0x05: 'MAX_DX10',
        0x06: 'MIN_DX10', 0x08: 'SETE', 0x09: 'SETGT', 0x0a: 'SETGE', 0x0b: 'SETNE',
        0x0c: 'SETE_DX10', 0x0d: 'SETGT_DX10', 0x0e: 'SETGE_DX10', 0x0f: 'SETNE_DX10',
        0x10: 'FRACT', 0x11: 'TRUNC', 0x12: 'CEIL', 0x13: 'RNDNE', 0x14: 'FLOOR',
        0x15: 'ASHR_INT', 0x16: 'LSHR_INT', 0x17: 'LSHL_INT', 0x19: 'MOV', 0x1a: 'NOP',
        0x1e: 'PRED_SETGT_UINT', 0x1f: 'PRED_SETGE_UINT', 0x20: 'PRED_SETE', 0x21: 'PRED_SETGT',
        0x22: 'PRED_SETGE', 0x23: 'PRED_SETNE', 0x24: 'PRED_SET_INV', 0x25: 'PRED_SET_POP',
        0x26: 'PRED_SET_CLR', 0x27: 'PRED_SET_RESTORE', 0x28: 'PRED_SETE_PUSH',
        0x29: 'PRED_SETGT_PUSH', 0x2a: 'PRED_SETGE_PUSH', 0x2b: 'PRED_SETNE_PUSH',
        0x2c: 'KILLE', 0x2d: 'KILLGT', 0x2e: 'KILLGE', 0x2f: 'KILLNE', 0x30: 'AND_INT',
        0x31: 'OR_INT', 0x32: 'XOR_INT', 0x33: 'NOT_INT', 0x34: 'ADD_INT', 0x35: 'SUB_INT',
        0x36: 'MAX_INT', 0x37: 'MIN_INT', 0x38: 'MAX_UINT', 0x39: 'MIN_UINT', 0x3a: 'SETE_INT',
        0x3b: 'SETGT_INT', 0x3c: 'SETGE_INT', 0x3d: 'SETNE_INT', 0x3e: 'SETGT_UINT',
        0x3f: 'SETGE_UINT', 0x40: 'KILLGT_UINT', 0x41: 'KILLGE_UINT', 0x42: 'PRED_SETE_INT',
        0x43: 'PRED_SETGT_INT', 0x44: 'PRED_SETGE_INT', 0x45: 'PRED_SETNE_INT',
        0x46: 'KILLE_INT', 0x47: 'KILLGT_INT', 0x48: 'KILLGE_INT', 0x49: 'KILLNE_INT',
        0x4a: 'PRED_SETE_PUSH_INT', 0x4b: 'PRED_SETGT_PUSH_INT', 0x4c: 'PRED_SETGE_PUSH_INT',
        0x4d: 'PRED_SETNE_PUSH_INT', 0x4e: 'PRED_SETLT_PUSH_INT', 0x4f: 'PRED_SETLE_PUSH_INT',
        0x50: 'FLT_TO_INT', 0x51: 'BFREV_INT', 0x52: 'ADDC_UINT', 0x53: 'SUBB_UINT',
        0x54: 'GROUP_BARRIER', 0x57: 'SET_MODE', 0x81: 'EXP_IEEE', 0x82: 'LOG_CLAMPED',
        0x83: 'LOG_IEEE', 0x84: 'RECIP_CLAMPED', 0x85: 'RECIP_FF', 0x86: 'RECIP_IEEE',
        0x87: 'RECIPSQRT_CLAMPED', 0x88: 'RECIPSQRT_FF', 0x89: 'RECIPSQRT_IEEE',
        0x8a: 'SQRT_IEEE', 0x8d: 'SIN', 0x8e: 'COS', 0x8f: 'MULLO_INT', 0x90: 'MULHI_INT',
        0x91: 'MULLO_UINT', 0x92: 'MULHI_UINT', 0x93: 'RECIP_INT', 0x94: 'RECIP_UINT',
        0x9a: 'FLT_TO_UINT', 0x9b: 'INT_TO_FLT', 0x9c: 'UINT_TO_FLT', 0xa0: 'BFM_INT',
        0xa2: 'FLT32_TO_FLT16', 0xa3: 'FLT16_TO_FLT32', 0xa4: 'UBYTE0_FLT', 0xa5: 'UBYTE1_FLT',
        0xa6: 'UBYTE2_FLT', 0xa7: 'UBYTE3_FLT', 0xaa: 'BCNT_INT', 0xab: 'FFBH_UINT',
        0xac: 'FFBL_INT', 0xad: 'FFBH_INT', 0xae: 'FLT_TO_UINT4', 0xaf: 'DOT_IEEE',
        0xb0: 'FLT_TO_INT_RPI', 0xb1: 'FLT_TO_INT_FLOOR', 0xb2: 'MULHI_UINT24',
        0xb5: 'MUL_UINT24', 0xbe: 'DOT4', 0xbf: 'DOT4_IEEE', 0xc0: 'CUBE', 0xc1: 'MAX4',
        0xcc: 'MOVA_INT', 0xd6: 'INTERP_XY', 0xd7: 'INTERP_ZW', 0xd8: 'INTERP_X',
        0xd9: 'INTERP_Z', 0xe0: 'INTERP_LOAD_P0', 0xe1: 'INTERP_LOAD_P10', 0xe2: 'INTERP_LOAD_P20'}
ALU3 = {0x04: 'BFE_UINT', 0x05: 'BFE_INT', 0x06: 'BFI_INT', 0x07: 'FMA', 0x0c: 'BIT_ALIGN_INT',
        0x0d: 'BYTE_ALIGN_INT', 0x10: 'MULADD_UINT24', 0x14: 'MULADD', 0x15: 'MULADD_M2',
        0x16: 'MULADD_M4', 0x17: 'MULADD_D2', 0x18: 'MULADD_IEEE', 0x19: 'CNDE', 0x1a: 'CNDGT',
        0x1b: 'CNDGE', 0x1c: 'CNDE_INT', 0x1d: 'CNDGT_INT', 0x1e: 'CNDGE_INT', 0x1f: 'MUL_LIT'}
# ops only the trans (t) unit runs
TRANS_ONLY = {0x81, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x8a, 0x8d, 0x8e, 0x8f,
              0x90, 0x91, 0x92, 0x93, 0x94, 0x9a, 0x9b, 0x9c}
TRANS_ONLY3 = {0x1f}
CF = {0x00: 'NOP', 0x01: 'TEX', 0x02: 'VTX', 0x03: 'GDS', 0x04: 'LOOP_START', 0x05: 'LOOP_END',
      0x06: 'LOOP_START_DX10', 0x07: 'LOOP_START_NO_AL', 0x08: 'LOOP_CONTINUE',
      0x09: 'LOOP_BREAK', 0x0a: 'JUMP', 0x0b: 'PUSH', 0x0d: 'ELSE', 0x0e: 'POP', 0x12: 'CALL',
      0x13: 'CALL_FS', 0x14: 'RET', 0x15: 'EMIT_VERTEX', 0x16: 'EMIT_CUT_VERTEX',
      0x17: 'CUT_VERTEX', 0x18: 'KILL', 0x1a: 'WAIT_ACK', 0x1b: 'TEX_ACK', 0x1c: 'VTX_ACK',
      0x1d: 'JUMPTABLE', 0x1e: 'WAVE_SYNC', 0x1f: 'HALT', 0x50: 'MEM_SCRATCH',
      0x52: 'MEM_RING', 0x53: 'EXPORT', 0x54: 'EXPORT_DONE', 0x55: 'MEM_EXPORT',
      0x56: 'MEM_RAT', 0x57: 'MEM_RAT_NOCACHE'}
CF_ALU = {8: 'ALU', 9: 'ALU_PUSH_BEFORE', 10: 'ALU_POP_AFTER', 11: 'ALU_POP2_AFTER',
          12: 'ALU_EXT', 13: 'ALU_CONTINUE', 14: 'ALU_BREAK', 15: 'ALU_ELSE_AFTER'}
TEX = {0x03: 'LD', 0x04: 'GET_TEXTURE_RESINFO', 0x05: 'GET_NUMBER_OF_SAMPLES', 0x06: 'GET_LOD',
       0x07: 'GET_GRADIENTS_H', 0x08: 'GET_GRADIENTS_V', 0x09: 'SET_TEXTURE_OFFSETS',
       0x0a: 'KEEP_GRADIENTS', 0x0b: 'SET_GRADIENTS_H', 0x0c: 'SET_GRADIENTS_V', 0x0d: 'PASS',
       0x10: 'SAMPLE', 0x11: 'SAMPLE_L', 0x12: 'SAMPLE_LB', 0x13: 'SAMPLE_LZ', 0x14: 'SAMPLE_G',
       0x15: 'GATHER4', 0x16: 'SAMPLE_G_LB', 0x17: 'GATHER4_O', 0x18: 'SAMPLE_C',
       0x19: 'SAMPLE_C_L', 0x1a: 'SAMPLE_C_LB', 0x1b: 'SAMPLE_C_LZ', 0x1c: 'SAMPLE_C_G',
       0x1d: 'GATHER4_C', 0x1e: 'SAMPLE_C_G_LB', 0x1f: 'GATHER4_C_O'}
CH = 'xyzw'
SEL = 'xyzw01?_'


def src(sel, rel, chan, neg, absv, lit):
    if sel < 128:
        s = 'R%d' % sel
    elif sel < 160:
        s = 'KC0[%d]' % (sel - 128)
    elif sel < 192:
        s = 'KC1[%d]' % (sel - 160)
    elif sel == 0xf8:
        s, chan = '0', None
    elif sel == 0xf9:
        s, chan = '1.0', None
    elif sel == 0xfa:
        s, chan = '1i', None
    elif sel == 0xfb:
        s, chan = '-1i', None
    elif sel == 0xfc:
        s, chan = '0.5', None
    elif sel == 0xfd:
        v = lit[chan] if chan < len(lit) else 0
        s, chan = '[%08x %g]' % (v, struct.unpack('<f', struct.pack('<I', v))[0]), None
    elif sel == 0xfe:
        s = 'PV'
    elif sel == 0xff:
        s, chan = 'PS', None
    elif 256 <= sel < 288:
        s = 'KC2[%d]' % (sel - 256)
    elif 288 <= sel < 320:
        s = 'KC3[%d]' % (sel - 288)
    elif 448 <= sel < 480:
        s = 'Param%d' % (sel - 448)
    elif sel >= 256:
        s = 'C%d' % (sel - 256)
    else:
        s = 'S%d' % sel
    if rel:
        s += '[AR]'
    if chan is not None:
        s += '.' + CH[chan]
    if absv:
        s = '|%s|' % s
    if neg:
        s = '-' + s
    return s


def alu_group(w, i):
    """Decode one ALU instruction group at dword i; returns (text lines, dwords used)."""
    insts = []
    j = i
    while True:
        w0, w1 = w[j], w[j + 1]
        insts.append((w0, w1))
        j += 2
        if w0 >> 31:
            break
        if len(insts) == 5:
            break
    nlit = 0
    for w0, w1 in insts:
        op3 = (w1 >> 15) & 7 != 0
        for sel, chan in (((w0 & 0x1ff), (w0 >> 10) & 3), (((w0 >> 13) & 0x1ff), (w0 >> 23) & 3)) + \
                ((((w1 & 0x1ff), (w1 >> 10) & 3),) if op3 else ()):
            if sel == 0xfd:
                nlit = max(nlit, chan + 1)
    nlit = (nlit + 1) & ~1
    lit = w[j:j + nlit]
    lines = []
    used = set()
    last = -1
    for w0, w1 in insts:
        op3 = (w1 >> 15) & 7 != 0
        dst, drel, dchan, clamp = (w1 >> 21) & 0x7f, (w1 >> 28) & 1, (w1 >> 29) & 3, w1 >> 31
        if op3:
            op = (w1 >> 13) & 0x1f
            name = ALU3.get(op, 'OP3_%02x' % op)
            trans = op in TRANS_ONLY3
            srcs = [src(w0 & 0x1ff, (w0 >> 9) & 1, (w0 >> 10) & 3, (w0 >> 12) & 1, 0, lit),
                    src((w0 >> 13) & 0x1ff, (w0 >> 22) & 1, (w0 >> 23) & 3, (w0 >> 25) & 1, 0, lit),
                    src(w1 & 0x1ff, (w1 >> 9) & 1, (w1 >> 10) & 3, (w1 >> 12) & 1, 0, lit)]
            wm, omod, extra = 1, 0, ''
        else:
            op = (w1 >> 7) & 0xff
            name = ALU2.get(op, 'OP2_%02x' % op)
            trans = op in TRANS_ONLY
            srcs = [src(w0 & 0x1ff, (w0 >> 9) & 1, (w0 >> 10) & 3, (w0 >> 12) & 1, w1 & 1, lit),
                    src((w0 >> 13) & 0x1ff, (w0 >> 22) & 1, (w0 >> 23) & 3, (w0 >> 25) & 1, (w1 >> 1) & 1, lit)]
            wm, omod = (w1 >> 4) & 1, (w1 >> 5) & 3
            extra = (' UPDATE_EXEC' if w1 & 4 else '') + (' UPDATE_PRED' if w1 & 8 else '')
        if trans or dchan in used or dchan <= last:
            slot = 't'
        else:
            slot = CH[dchan]
            used.add(dchan)
            last = dchan
        d = ('R%d%s.%s' % (dst, '[AR]' if drel else '', CH[dchan])) if wm else '____'
        mods = (' *2' if omod == 1 else ' *4' if omod == 2 else ' /2' if omod == 3 else '') + \
               (' CLAMP' if clamp else '')
        pred = (w0 >> 29) & 3
        im = (w0 >> 26) & 7
        lines.append('    %s: %-16s %s, %s%s%s%s%s' % (slot, name, d, ', '.join(srcs), mods, extra,
                                                    ' PRED_SEL%d' % pred if pred else '',
                                                    ' IDX_MODE%d' % im if im else ''))
    return lines, j - i + nlit


def fetch(w, i, kind):
    w0, w1, w2 = w[i], w[i + 1], w[i + 2]
    if kind == 'TEX':
        op = w0 & 0x1f
        dsel = ''.join(SEL[(w1 >> s) & 7] for s in (9, 12, 15, 18))
        ssel = ''.join(SEL[(w2 >> s) & 7] for s in (20, 23, 26, 29))
        ct = ''.join('n' if (w1 >> (28 + k)) & 1 else 'u' for k in range(4))
        offs = [((w2 >> s) & 0x1f) for s in (0, 5, 10)]
        offs = [(o - 32 if o & 16 else o) / 2 for o in offs]
        return '    %-20s R%d.%s, R%d.%s, RID:%d SID:%d CT:%s%s%s' % (
            TEX.get(op, 'TEX_%02x' % op), w1 & 0x7f, dsel, (w0 >> 16) & 0x7f, ssel,
            (w0 >> 8) & 0xff, (w2 >> 15) & 0x1f, ct,
            ' LOD_BIAS:%d' % ((w1 >> 21) & 0x7f) if (w1 >> 21) & 0x7f else '',
            ' OFFS:%s' % offs if any(offs) else '')
    op = w0 & 0x1f
    dsel = ''.join(SEL[(w1 >> s) & 7] for s in (9, 12, 15, 18))
    return '    %-8s R%d.%s, R%d.%s, BUF:%d FMT:%d NUM:%d COMP:%d SRF:%d UCF:%d OFF:%d ENDIAN:%d FT:%d MFC:%d' % (
        'VFETCH' if op == 0 else 'SEMFETCH' if op == 1 else 'VTX_%02x' % op,
        (w1 & 0x7f), dsel, (w0 >> 16) & 0x7f, CH[(w0 >> 24) & 3], (w0 >> 8) & 0xff,
        (w1 >> 22) & 0x3f, (w1 >> 28) & 3, (w1 >> 30) & 1, w1 >> 31, (w1 >> 21) & 1,
        w2 & 0xffff, (w2 >> 16) & 3, (w0 >> 5) & 3, ((w0 >> 26) & 0x3f) + 1)


def shader(w, out=print, base=0):
    """Disassemble an Evergreen shader program (list of dwords)."""
    pc = 0
    while pc * 2 + 1 < len(w):
        w0, w1 = w[pc * 2], w[pc * 2 + 1]
        if (w1 >> 29) & 1:      # ALU clause
            op = (w1 >> 26) & 0xf
            addr, cnt = w0 & 0x3fffff, ((w1 >> 18) & 0x7f) + 1
            kc = 'KC0[bank %d mode %d addr %d] KC1[bank %d mode %d addr %d]' % (
                (w0 >> 22) & 0xf, w0 >> 30, (w1 >> 2) & 0xff, (w0 >> 26) & 0xf, w1 & 3, (w1 >> 10) & 0xff)
            out('%04x %s @%d count %d %s%s' % (pc, CF_ALU.get(op), addr, cnt, kc,
                                               ' ALT_CONST' if (w1 >> 25) & 1 else ''))
            j, end = addr * 2, addr * 2 + cnt * 2
            while j < end and j + 1 < len(w):
                lines, n = alu_group(w, j)
                for ln in lines:
                    out(ln)
                j += n
            pc += 1
            continue
        op = (w1 >> 22) & 0xff
        name = CF.get(op, 'CF_%02x' % op)
        eop = ' EOP' if (w1 >> 21) & 1 else ''
        if op in (0x53, 0x54):
            typ = ('PIXEL', 'POS', 'PARAM', '?')[(w0 >> 13) & 3]
            sw = ''.join('xyzw01?_'[(w1 >> s) & 7] for s in (0, 3, 6, 9))
            bc = ((w1 >> 16) & 0xf) + 1
            out('%04x %s %s%d R%d.%s burst %d%s' % (pc, name, typ, w0 & 0x1fff, (w0 >> 15) & 0x7f, sw, bc, eop))
        elif op in (0x01, 0x02, 0x1b, 0x1c):
            addr, cnt = w0 & 0xffffff, ((w1 >> 10) & 0x3f) + 1
            out('%04x %s @%d count %d%s' % (pc, name, addr, cnt, eop))
            for k in range(cnt):
                j = addr * 2 + k * 4
                if j + 2 < len(w):
                    # a TC clause holds both: VC_INST 0-2 are vertex fetches
                    kind = 'TEX' if op in (0x01, 0x1b) and (w[j] & 0x1f) > 2 else 'VTX'
                    out(fetch(w, j, kind))
        else:
            out('%04x %s addr %d cond %d pop %d count %d cf_const %d%s' % (
                pc, name, w0 & 0xffffff, (w1 >> 8) & 3, w1 & 7, (w1 >> 10) & 0x3f, (w1 >> 3) & 0x1f, eop))
        pc += 1
        if eop or op in (0x14, 0x1f) and False:
            break


if __name__ == '__main__':
    if len(sys.argv) >= 3 and sys.argv[1] == 'shader':
        data = open(sys.argv[2], 'rb').read()[int(sys.argv[3], 0) if len(sys.argv) > 3 else 0:]
        shader(list(struct.unpack('<%dI' % (len(data) // 4), data[:len(data) // 4 * 4])))
    else:
        print(__doc__)
