#!/usr/bin/env python3
"""notices.py PKG: write PKG/THIRD-PARTY-NOTICES.md for a release package.

Lists QEMU, the firmware and every library bundle-libs.py put in PKG/lib
(from PKG/lib/bundled.txt, which it removes): the package it came from, its
version and license, as the build machine's package manager reports them
(dpkg on Linux, Homebrew or MacPorts on macOS), and copies the license and
copyright files that package ships into PKG/licenses/.
"""
import glob
import json
import os
import re
import shutil
import subprocess
import sys


def out(*cmd):
    r = subprocess.run(cmd, capture_output=True, text=True)
    return r.stdout.strip() if r.returncode == 0 else ''


def dpkg(path):
    owner = out('dpkg', '-S', path) or out('dpkg', '-S', path.replace('/usr/lib/', '/lib/', 1))
    if not owner:
        return None
    pkg = owner.split(':')[0] if owner.count(':') < 2 else ':'.join(owner.split(':')[:2])
    ver = out('dpkg-query', '-W', '-f', '${Version}', pkg)
    src = out('dpkg-query', '-W', '-f', '${source:Package}', pkg) or pkg
    copyright = f'/usr/share/doc/{pkg.split(":")[0]}/copyright'
    lic = ''
    if os.path.exists(copyright):
        m = re.findall(r'^License: (.+)$', open(copyright, errors='replace').read(), re.M)
        lic = ', '.join(dict.fromkeys(x.strip() for x in m))[:200]
    return dict(package=pkg, version=ver, license=lic or 'see licenses/',
                source=f'Debian/Ubuntu source package {src} {ver}',
                files=[copyright] if os.path.exists(copyright) else [])


def homebrew(path):
    m = re.search(r'/Cellar/([^/]+)/([^/]+)/', path)
    if not m:
        return None
    formula, ver = m.groups()
    info = json.loads(out('brew', 'info', '--json=v2', formula) or '{}').get('formulae', [{}])
    info = info[0] if info else {}
    keg = path[:m.end()]
    files = [f for f in glob.glob(keg + '*') if re.search(r'(COPYING|LICEN[CS]E|NOTICE)', os.path.basename(f), re.I)]
    return dict(package=formula, version=ver, license=info.get('license') or 'see licenses/',
                source=(info.get('urls', {}).get('stable', {}) or {}).get('url', info.get('homepage', '')),
                files=files)


def macports(path):
    owner = out('port', 'provides', path)
    m = re.search(r'is provided by: (\S+)', owner)
    if not m:
        return None
    port = m.group(1)
    info = out('port', 'info', '--license', '--version', '--homepage', port)
    field = lambda k: (re.search(rf'^{k}: (.+)$', info, re.M) or [None, ''])[1].strip()
    docs = glob.glob(f'/opt/local/share/doc/{port}/*')
    files = [f for f in docs if re.search(r'(COPYING|LICEN[CS]E|NOTICE)', os.path.basename(f), re.I)]
    return dict(package=port, version=field('version'), license=field('license') or 'see licenses/',
                source=f'MacPorts port {port} ({field("homepage")})', files=files)


def main():
    pkg = sys.argv[1]
    listing = os.path.join(pkg, 'lib', 'bundled.txt')
    libs = []
    if os.path.exists(listing):
        libs = [l.rstrip('\n').split('\t') for l in open(listing) if l.strip()]
        os.remove(listing)
    lic_dir = os.path.join(pkg, 'licenses')
    rows, by_pkg = [], {}
    for name, src in libs:
        info = dpkg(src) if sys.platform != 'darwin' else (homebrew(src) or macports(src))
        if not info:
            info = dict(package='?', version='', license='unknown', source=src, files=[])
        rows.append((name, info))
        by_pkg.setdefault(info['package'], info)
    for p, info in by_pkg.items():
        for f in info['files']:
            os.makedirs(lic_dir, exist_ok=True)
            shutil.copy2(f, os.path.join(lic_dir, f'{p}.{os.path.basename(f)}'))
    with open(os.path.join(pkg, 'THIRD-PARTY-NOTICES.md'), 'w') as f:
        f.write('# Third-party software in this package\n\n')
        f.write('- **QEMU 11.1.2** with the patches in `qemu/patches/` and the rv530 device in '
                '`qemu/hw/display/rv530/`: GNU GPL version 2 (`COPYING`). The QEMU source is '
                '`qemu-11.1.2.tar.xz` from https://download.qemu.org/, also attached to this '
                'release; the device and patches are in this package.\n')
        if os.path.isdir(os.path.join(pkg, 'firmware')):
            f.write('- **OVMF** (`firmware/`), built from edk2 tag edk2-stable202608 '
                    '(https://github.com/tianocore/edk2) with `snowleopard/Rv530GopDxe/` by '
                    '`snowleopard/build-ovmf.sh`: BSD-2-Clause-Patent.\n')
        f.write('\nBundled libraries (`lib/`), dynamically linked and replaceable; their '
                'license and copyright files, where the package manager ships them, are in '
                '`licenses/`:\n\n| Library | From | Version | License | Source |\n'
                '| --- | --- | --- | --- | --- |\n')
        for name, info in rows:
            f.write(f'| `{name}` | {info["package"]} | {info["version"]} | {info["license"]} | '
                    f'{info["source"]} |\n')
    print(f'notices: {len(rows)} libraries from {len(by_pkg)} packages')


main()
