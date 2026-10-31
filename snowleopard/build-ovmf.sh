#!/bin/bash
# Build nasm, iasl and OVMF (X64) with edk2 memory protections disabled
# (Mac OS X 10.6 boot.efi writes into pages modern edk2 marks read-only),
# with the rv530 GOP driver (snowleopard/Rv530GopDxe) built in and without
# the TianoCore boot logo, so the VM goes from a black screen straight to
# the Apple logo (OVMF_LOGO=1 keeps the logo).
#
# macOS: needs x86_64-elf-gcc and x86_64-elf-binutils (edk2's GCC toolchain
# with GCC_BIN=x86_64-elf-), from Homebrew on Apple silicon or MacPorts on an
# Intel Mac.  edk2 cannot build in a path with
# spaces; then work/fw becomes a symlink to ~/.cache/rv530-fw.
set -euxo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$HERE/work"
case "$HERE" in
*\ *)
  REAL=${XDG_CACHE_HOME:-$HOME/.cache}/rv530-fw
  mkdir -p "$REAL"
  [ -e "$HERE/work/fw" ] || ln -s "$REAL" "$HERE/work/fw" ;;
esac
mkdir -p "$HERE/work/fw" && cd "$(cd "$HERE/work/fw" && pwd -P)"
if [ "$(uname -s)" = Darwin ]; then
  JOBS=$(sysctl -n hw.ncpu)
  export GCC_BIN=${GCC_BIN:-x86_64-elf-}
  command -v "${GCC_BIN}gcc" >/dev/null || { echo "need ${GCC_BIN}gcc: brew install x86_64-elf-gcc x86_64-elf-binutils (Apple silicon), or sudo port install x86_64-elf-gcc x86_64-elf-binutils (Intel)" >&2; exit 1; }
else
  JOBS=$(nproc)
fi
P=$PWD/tools; mkdir -p $P/bin; export PATH=$P/bin:$PATH
if ! command -v nasm; then
  curl -fLO https://www.nasm.us/pub/nasm/releasebuilds/2.16.03/nasm-2.16.03.tar.xz
  tar xf nasm-2.16.03.tar.xz && (cd nasm-2.16.03 && ./configure --prefix=$P && make -j$JOBS && make install)
fi
if ! command -v iasl; then
  curl -fL -o acpica.tar.gz https://github.com/acpica/acpica/archive/refs/tags/R2024_12_12.tar.gz
  tar xf acpica.tar.gz && (cd acpica-R2024_12_12 && make -j$JOBS iasl && cp generate/unix/bin/iasl $P/bin/)
fi
[ -d edk2 ] || git clone --depth 1 -b edk2-stable202608 --recurse-submodules --shallow-submodules https://github.com/tianocore/edk2.git
cd edk2
# GOP driver for the emulated Radeon X1600 (rv530), built into OVMF
rm -rf OvmfPkg/Rv530GopDxe && cp -r "$HERE/Rv530GopDxe" OvmfPkg/
python3 - <<'PY'
for p, key in (('OvmfPkg/OvmfPkgX64.dsc', '  OvmfPkg/QemuVideoDxe/QemuVideoDxe.inf'),
               ('OvmfPkg/OvmfPkgX64.fdf', 'INF  OvmfPkg/QemuVideoDxe/QemuVideoDxe.inf')):
    s = open(p, newline='').read()
    if 'Rv530GopDxe' not in s:
        s = s.replace(key + '\r\n', key + '\r\n' + key.replace('QemuVideoDxe/QemuVideoDxe', 'Rv530GopDxe/Rv530GopDxe') + '\r\n', 1)
        open(p, 'w', newline='').write(s)
# the boot logo: LogoDxe provides it, the boot manager shows it when present
import os
for p in ('OvmfPkg/OvmfPkgX64.dsc', 'OvmfPkg/OvmfPkgX64.fdf'):
    s = open(p, newline='').read()
    lines = s.split('\r\n')
    keep = os.environ.get('OVMF_LOGO') == '1'
    for i, l in enumerate(lines):
        if l.strip().endswith('MdeModulePkg/Logo/LogoDxe.inf'):
            l = l.lstrip('#')
            lines[i] = l if keep else '#' + l
    open(p, 'w', newline='').write('\r\n'.join(lines))
PY
make -C BaseTools -j$JOBS
set +u; . ./edksetup.sh; set -u
build -a X64 -t GCC -b RELEASE -p OvmfPkg/OvmfPkgX64.dsc -n $JOBS \
  -D SMM_REQUIRE=FALSE -D SECURE_BOOT_ENABLE=FALSE \
  --pcd gEfiMdeModulePkgTokenSpaceGuid.PcdImageProtectionPolicy=0x0 \
  --pcd gEfiMdeModulePkgTokenSpaceGuid.PcdDxeNxMemoryProtectionPolicy=0x0 \
  --pcd gEfiMdeModulePkgTokenSpaceGuid.PcdSetNxForStack=FALSE \
  --pcd gEfiMdeModulePkgTokenSpaceGuid.PcdNullPointerDetectionPropertyMask=0x0 \
  --pcd gEfiMdeModulePkgTokenSpaceGuid.PcdHeapGuardPropertyMask=0x0
ls -la Build/OvmfX64/RELEASE_GCC/FV/OVMF_CODE.fd Build/OvmfX64/RELEASE_GCC/FV/OVMF_VARS.fd
cp Build/OvmfX64/RELEASE_GCC/FV/OVMF_CODE.fd Build/OvmfX64/RELEASE_GCC/FV/OVMF_VARS.fd ..
echo OVMF-BUILD-DONE
