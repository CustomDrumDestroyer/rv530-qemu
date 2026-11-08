#!/usr/bin/env python3
"""make-installer.py: turn a Snow Leopard install image into a bootable disk.

  make-installer.py INPUT [OUTPUT]     (OUTPUT defaults to work/installer.img)

INPUT is a .dmg (decompressed with hdiutil on macOS, dmg2img on Linux) or a
raw image.  Many install DMGs hold a bare HFS+ volume with no partition map;
XNU's AppleFileSystemDriver only matches partitions (Apple_HFS content), so
such a volume boots as far as "Still waiting for root device".  A bare
volume is therefore placed in a GPT disk with one Apple_HFS partition at
1 MiB.  So is the Apple_HFS volume of an Apple partition map with 2048-byte
blocks (Lion's InstallESD.dmg: XNU on a 512-byte-sector disk does not read
such a map either); an image with any other partition map is copied as it
is.  The result is attached by sl-vm.sh (INSTALL=1 INSTALLER=...).
"""
import os, shutil, struct, subprocess, sys, tempfile, uuid, zlib

HERE = os.path.dirname(os.path.abspath(__file__))
SECTOR = 512
HFS_GUID = uuid.UUID('48465300-0000-11AA-AA11-00306543ECAC')
START = 2048                  # first usable LBA of the partition (1 MiB)
ENTRIES, ENTRY_SIZE = 128, 128


def raw_from_dmg(dmg, tmpdir):
    out = os.path.join(tmpdir, 'raw.img')
    if sys.platform == 'darwin':
        subprocess.run(['hdiutil', 'convert', dmg, '-format', 'UDTO', '-o', out],
                       check=True, stdout=subprocess.DEVNULL)
        os.rename(out + '.cdr', out)
    else:
        if not shutil.which('dmg2img'):
            sys.exit('need dmg2img to decompress a .dmg (apt install dmg2img)')
        subprocess.run(['dmg2img', '-i', dmg, '-o', out], check=True)
    return out


def is_bare_hfs(path):
    with open(path, 'rb') as f:
        f.seek(1024)
        return f.read(2) in (b'H+', b'HX')


def apm_hfs_volume(path):
    """(offset, length) of the Apple_HFS partition of a 2048-byte-block Apple
    partition map, or None."""
    with open(path, 'rb') as f:
        if f.read(4) != b'ER\x08\x00':
            return None
        for i in range(1, 64):
            f.seek(i * 2048)
            e = f.read(512)
            if e[:2] != b'PM':
                return None
            start, count = struct.unpack('>II', e[8:16])
            if e[48:80].rstrip(b'\0') == b'Apple_HFS':
                return start * 2048, count * 2048
    return None


def extract(path, off, length, out):
    with open(path, 'rb') as f, open(out, 'wb') as o:
        f.seek(off)
        while length:
            buf = f.read(min(length, 16 << 20))
            if not buf:
                break
            o.write(buf)
            length -= len(buf)


def gpt_header(lba, alt, first, last, disk_guid, entries_lba, entries_crc):
    h = struct.pack('<8sIIIIQQQQ16sQIII', b'EFI PART', 0x10000, 92, 0, 0,
                    lba, alt, first, last, disk_guid.bytes_le,
                    entries_lba, ENTRIES, ENTRY_SIZE, entries_crc)
    h = h[:16] + struct.pack('<I', zlib.crc32(h)) + h[20:]
    return h.ljust(SECTOR, b'\0')


def wrap_gpt(vol, out):
    vsize = os.path.getsize(vol)
    vsec = (vsize + SECTOR - 1) // SECTOR
    tbl = ENTRIES * ENTRY_SIZE // SECTOR            # 32 sectors
    total = START + vsec + 2048                     # room for the backup GPT
    last_usable = total - 1 - tbl - 1
    name = 'Mac OS X Install DVD'.encode('utf-16-le').ljust(72, b'\0')
    entry = struct.pack('<16s16sQQQ72s', HFS_GUID.bytes_le, uuid.uuid4().bytes_le,
                        START, START + vsec - 1, 0, name)
    entries = entry.ljust(ENTRIES * ENTRY_SIZE, b'\0')
    ecrc = zlib.crc32(entries)
    dguid = uuid.uuid4()
    # protective MBR
    mbr = bytearray(SECTOR)
    mbr[446:462] = struct.pack('<BBBBBBBBII', 0, 0, 2, 0, 0xEE, 0xFF, 0xFF, 0xFF,
                               1, min(total - 1, 0xFFFFFFFF))
    mbr[510:512] = b'\x55\xaa'
    with open(out, 'wb') as f:
        f.truncate(total * SECTOR)
        f.write(mbr)
        f.write(gpt_header(1, total - 1, START, last_usable, dguid, 2, ecrc))
        f.write(entries)
        f.seek((total - 1 - tbl) * SECTOR)
        f.write(entries)
        f.write(gpt_header(total - 1, 1, START, last_usable, dguid, total - 1 - tbl, ecrc))
        f.seek(START * SECTOR)
        with open(vol, 'rb') as v:
            shutil.copyfileobj(v, f, 16 << 20)


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    src = sys.argv[1]
    out = sys.argv[2] if len(sys.argv) > 2 else os.path.join(HERE, 'work', 'installer.img')
    os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
    with tempfile.TemporaryDirectory(dir=os.path.dirname(os.path.abspath(out))) as tmp:
        raw = raw_from_dmg(src, tmp) if src.lower().endswith('.dmg') else src
        apm = apm_hfs_volume(raw)
        if apm:
            print('Apple partition map (2048-byte blocks): taking its HFS+ volume')
            vol = os.path.join(tmp, 'volume.img')
            extract(raw, apm[0], apm[1], vol)
            if raw != src:
                os.remove(raw)
            raw = vol
        if is_bare_hfs(raw):
            print('bare HFS+ volume: wrapping it in a GPT disk')
            wrap_gpt(raw, out)
        elif raw != src:
            os.rename(raw, out)
        else:
            shutil.copyfile(raw, out)
    print('installer image:', out)


if __name__ == '__main__':
    main()
