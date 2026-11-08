#!/usr/bin/env python3
"""Keep only the legacy x86 image of a multi-image PCI option ROM.

Apple's HD 5770 VBIOS carries an EFI (EBC) driver after the x86 image that
crashes OVMF's EbcDxe; Snow Leopard only needs the x86 image.

    vbios-x86only.py AppleHD_5770_1024_MB.rom AppleHD_5770_1024_MB-x86only.rom
"""
import struct
import sys

rom = bytearray(open(sys.argv[1], 'rb').read())
assert rom[0:2] == b'\x55\xaa', 'not an option ROM'
pcir = struct.unpack_from('<H', rom, 0x18)[0]
assert rom[pcir:pcir + 4] == b'PCIR', 'no PCI data structure'
assert rom[pcir + 0x14] == 0, 'first image is not x86'
size = struct.unpack_from('<H', rom, pcir + 0x10)[0] * 512
img = rom[:size]
img[pcir + 0x15] |= 0x80                       # last image
img[size - 1] = (img[size - 1] - sum(img)) & 0xff  # checksum (last byte)
assert sum(img) & 0xff == 0
open(sys.argv[2], 'wb').write(img)
print(f'{sys.argv[2]}: {size} bytes')
