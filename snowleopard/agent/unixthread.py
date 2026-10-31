#!/usr/bin/env python3
# Replace LC_MAIN with LC_UNIXTHREAD (rip = symbol "start") so a binary
# linked by a modern ld runs on Mac OS X 10.6, whose dyld predates LC_MAIN.
import struct, subprocess, sys
path = sys.argv[1]
start = None
for line in subprocess.run(['nm', path], capture_output=True, text=True).stdout.splitlines():
    p = line.split()
    if len(p) == 3 and p[2] == 'start':
        start = int(p[0], 16)
assert start, 'no start symbol'
b = bytearray(open(path, 'rb').read())
magic, cpu, sub, ft, ncmds, sizeofcmds, flags, res = struct.unpack_from('<8I', b, 0)
assert magic == 0xfeedfacf
off, cmds, first_sect = 32, [], None
for i in range(ncmds):
    cmd, size = struct.unpack_from('<2I', b, off)
    cmds.append((cmd, bytes(b[off:off + size])))
    if cmd == 0x19:  # LC_SEGMENT_64: lowest section file offset bounds the header
        nsects = struct.unpack_from('<I', b, off + 64)[0]
        for s in range(nsects):
            so = struct.unpack_from('<I', b, off + 72 + s * 80 + 48)[0]
            if so and (first_sect is None or so < first_sect):
                first_sect = so
    off += size
LC_MAIN, LC_UNIXTHREAD = 0x80000028, 0x5
regs = [0] * 21
regs[16] = start  # rip
thread = struct.pack('<4I', LC_UNIXTHREAD, 16 + 168, 4, 42) + struct.pack('<21Q', *regs)
new = [c for c in cmds if c[0] != LC_MAIN] + [(LC_UNIXTHREAD, thread)]
blob = b''.join(c[1] for c in new)
assert 32 + len(blob) <= first_sect, 'not enough header padding'
b[32:32 + sizeofcmds] = b'\0' * sizeofcmds
b[32:32 + len(blob)] = blob
struct.pack_into('<2I', b, 16, len(new), len(blob))
open(path, 'wb').write(b)
print('LC_UNIXTHREAD at 0x%x' % start)
