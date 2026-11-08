#!/usr/bin/env python3
"""Inspect a running radeon-hd5770 guest over QMP.

Reads GPU addresses (VRAM through BAR0, GART pages through the page table)
with HMP xp, so it works without any device support.  Run from
snowleopard/ (it uses slctl.py's QMP socket).

    egdump.py ring [NDW]          decode the last NDW dwords of the CP ring, with IBs
    egdump.py ib ADDR NDW         decode an indirect buffer
    egdump.py shader ADDR [NDW]   disassemble a shader (ADDR = GPU byte address)
    egdump.py read ADDR NDW       raw dwords
    egdump.py regs OFF [N]        MMIO registers
    egdump.py surface ADDR W H PITCH MODE OUT.png [BANKS SPLIT]
                                  32 bpp surface at VRAM offset ADDR,
                                  detiled (MODE 0 linear, 2 1D, 4 2D tiled)
"""
import os
import struct
import sys

sys.path.insert(0, os.getcwd())
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import slctl  # noqa: E402
import egdis  # noqa: E402

_bars = None


def hmp(cmd):
    return slctl.qmp({"execute": "human-monitor-command",
                      "arguments": {"command-line": cmd}})[0]["return"]


def bars():
    global _bars
    if _bars is None:
        txt = hmp('info pci')
        blk = txt[txt.index('1002:68b8'):]
        b0 = int(blk.split('BAR0:')[1].split(' at ')[1].split()[0], 16)
        b2 = int(blk.split('BAR2:')[1].split(' at ')[1].split()[0], 16)
        _bars = (b0, b2)
    return _bars


def phys(addr, n):
    out = []
    while n > 0:
        k = min(n, 256)
        for line in hmp('xp /%dwx %#x' % (k, addr)).splitlines():
            if ':' in line:
                out += [int(x, 16) for x in line.split(':')[1].split()]
        addr += 4 * k
        n -= k
    return out


def reg(off, n=1):
    return phys(bars()[1] + off, n)


def gpu_read(addr, n):
    """GPU MC address -> dwords (VRAM in the BAR0 window, or GART)."""
    fbl = reg(0x2024)[0]
    fb_start, fb_end = (fbl & 0xffff) << 24, ((fbl >> 16) << 24) | 0xffffff
    pt_start, pt_end = reg(0x155c)[0] << 12, (reg(0x157c)[0] << 12) | 0xfff
    table = reg(0x153c)[0] << 12
    out = []
    while n > 0:
        if fb_start <= addr <= fb_end:
            k = n
            out += phys(bars()[0] + addr - fb_start, k)
        elif pt_start <= addr <= pt_end:
            pte_a = table + ((addr - pt_start) >> 12) * 8
            lo, hi = phys(bars()[0] + pte_a - fb_start, 2)
            pte = lo | hi << 32
            pa = (pte & 0xfffffffff000) | (addr & 0xfff)
            k = min(n, (0x1000 - (addr & 0xfff)) // 4)
            if not pte & 1:
                out += [0xdeadbeef] * k
            elif pte & 2:
                out += phys(pa, k)
            else:
                out += phys(bars()[0] + pa - fb_start, k)
        else:
            raise SystemExit('address %#x not mapped' % addr)
        addr += 4 * k
        n -= k
    return out


def ib(addr, ndw, depth=1):
    print('%s--- IB %#x, %d dwords' % ('  ' * depth, addr, ndw))
    egdis.pm4(gpu_read(addr, ndw), out=lambda s: print('  ' * depth + s),
              ib=lambda a, n: ib(a, n, depth + 1) if depth < 3 else None)


def tile_offset(x, y, pitch, height, mode, bpp=4, pipes=4, interleave=256, banks=4,
                split=1024, bw=1, bh=1, aspect=1, display=True):
    """Byte offset of pixel (x, y): a copy of eg_tile_offset() in eg_tile.c."""
    b = lambda v, n: (v >> n) & 1
    if display and bpp == 4:
        pix = [b(x, 0), b(x, 1), b(y, 0), b(x, 2), b(y, 1), b(y, 2)]
    else:
        pix = [b(x, 0), b(y, 0), b(x, 1), b(y, 1), b(x, 2), b(y, 2)]
    elem = sum(v << i for i, v in enumerate(pix)) * bpp
    micro = 64 * bpp
    if mode == 2:
        return ((y // 8) * (pitch // 8) + x // 8) * micro + elem
    if mode != 4:
        return (y * pitch + x) * bpp
    split_slice = 0
    if micro > split:
        split_slice = elem // split
        elem %= split
        micro = split
    mtp, mth = 8 * bw * pipes * aspect, 8 * bh * banks // aspect
    mtb = micro * (mtp // 8) * (mth // 8) // (pipes * banks)
    per_row = pitch // mtp
    slice_bytes = per_row * (height // mth) * mtb
    tile_index = ((y // 8) % bh) * bw + ((x // 8) // pipes) % bw
    total = slice_bytes * split_slice + ((y // mth) * per_row + x // mtp) * mtb + elem + tile_index * micro
    tx, ty = x // 8, y // 8
    if pipes == 4:
        pipe = (b(tx, 0) ^ b(ty, 0) ^ b(tx, 1)) | (b(tx, 1) ^ b(ty, 1)) << 1
    elif pipes == 2:
        pipe = b(tx, 0) ^ b(ty, 0)
    else:
        pipe = 0
    tx, ty = x // 8 // (bw * pipes), y // 8 // bh
    if banks == 4:
        bank = (b(tx, 0) ^ b(ty, 1)) | (b(tx, 1) ^ b(ty, 0)) << 1
    elif banks == 8:
        bank = (b(tx, 0) ^ b(ty, 2)) | (b(tx, 1) ^ b(ty, 1) ^ b(ty, 2)) << 1 | (b(tx, 2) ^ b(ty, 0)) << 2
    else:
        bank = b(tx, 0) ^ b(ty, 0)
    bank ^= (banks // 2 + 1) * split_slice
    bank &= banks - 1
    pi, pb, bb = interleave.bit_length() - 1, pipes.bit_length() - 1, banks.bit_length() - 1
    return (total & (interleave - 1)) | pipe << pi | bank << (pi + pb) | (total >> pi) << (pi + pb + bb)


def main(a):
    if a[0] == 'ring':
        ndw = int(a[1], 0) if len(a) > 1 else 0x100
        base = reg(0xc100)[0] << 8
        size = 8 << (reg(0xc104)[0] & 0x3f)        # bytes (RB_BUFSZ: log2 of qwords)
        wptr = reg(0xc114)[0]
        start = (wptr - ndw) % (size // 4)
        w = gpu_read(base + start * 4, min(ndw, size // 4 - start))
        egdis.pm4(w, ib=lambda x, n: ib(x, n))
    elif a[0] == 'ib':
        ib(int(a[1], 16), int(a[2], 0))
    elif a[0] == 'shader':
        egdis.shader(gpu_read(int(a[1], 16), int(a[2], 0) if len(a) > 2 else 512))
    elif a[0] == 'read':
        w = gpu_read(int(a[1], 16), int(a[2], 0))
        for i in range(0, len(w), 8):
            print('%5x: %s' % (i, ' '.join('%08x' % x for x in w[i:i + 8])))
    elif a[0] == 'regs':
        off = int(a[1], 16)
        for k, v in enumerate(reg(off, int(a[2], 0) if len(a) > 2 else 1)):
            print('%05x %-30s %08x' % (off + 4 * k, egdis.regname(off + 4 * k), v))
    elif a[0] == 'surface':
        import tempfile, zlib
        addr, w, h, pitch, mode = int(a[1], 16), int(a[2]), int(a[3]), int(a[4]), int(a[5])
        banks = int(a[7]) if len(a) > 7 else 4
        split = int(a[8]) if len(a) > 8 else 1024
        hh = (h + 31) // 32 * 32
        size = pitch * hh * 4
        fd, tmp = tempfile.mkstemp()
        os.close(fd)
        # the device's dump property reads all of VRAM (BAR0 shows 256 MiB)
        r = slctl.qmp({"execute": "qom-set", "arguments": {
            "path": "/machine/peripheral/gpu", "property": "dump",
            "value": "%#x %d %s" % (addr, size, tmp)}})[0]
        if 'error' in r:
            raise SystemExit(r['error']['desc'])
        raw = open(tmp, 'rb').read()
        os.unlink(tmp)
        rows = []
        for y in range(h):
            row = bytearray()
            for x in range(w):
                o = tile_offset(x, y, pitch, hh, mode, banks=banks, split=split)
                px = raw[o:o + 4] if o + 4 <= len(raw) else b'\0\0\0\0'
                row += bytes((px[2], px[1], px[0]))
            rows.append(b'\0' + bytes(row))
        def chunk(t, d):
            return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
        png = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) + \
            chunk(b'IDAT', zlib.compress(b''.join(rows))) + chunk(b'IEND', b'')
        open(a[6], 'wb').write(png)
    else:
        print(__doc__)


if __name__ == '__main__':
    main(sys.argv[1:] or ['help'])
