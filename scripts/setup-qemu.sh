#!/bin/bash
# Fetch QEMU 11.1.3, drop in the rv530 device, apply the patches, build QEMU
# and the synthetic AtomBIOS option ROM.
# Usage: scripts/setup-qemu.sh [workdir]   (default: ./build-qemu)
# CONFIGURE_EXTRA adds options to QEMU's configure.
#
# Linux: a GTK window.  macOS: Cocoa and SDL windows; vulkan-loader,
# MoltenVK and shaderc from Homebrew (Apple silicon) or MacPorts (Intel Macs,
# which Homebrew no longer installs on) give the Vulkan backend (MoltenVK).
# QEMU refuses to build in a path with spaces or colons; in that case the
# tree goes to ~/.cache/rv530-qemu and the work dir becomes a symlink to it.
set -euo pipefail
HERE=$(cd "$(dirname "$0")/.." && pwd)
WORK=${1:-$HERE/build-qemu}
case "$WORK" in
*[\ :]*)
    REAL=${XDG_CACHE_HOME:-$HOME/.cache}/rv530-qemu
    mkdir -p "$REAL"
    [ -L "$WORK" ] || [ -e "$WORK" ] || ln -s "$REAL" "$WORK"
    echo "note: '$WORK' contains a space or colon; building in $REAL"
    WORK=$REAL ;;
esac
mkdir -p "$WORK"; cd "$WORK"
if [ ! -f qemu-11.1.3.tar.xz ]; then
    curl -fLO https://download.qemu.org/qemu-11.1.3.tar.xz
fi
if [ ! -d qemu-11.1.3 ]; then
    tar -xJf qemu-11.1.3.tar.xz
fi
cd qemu-11.1.3
rm -rf hw/display/rv530
cp -r "$HERE/qemu/hw/display/rv530" hw/display/rv530
rm -rf hw/display/evergreen
cp -r "$HERE/qemu/hw/display/evergreen" hw/display/evergreen
if ! grep -q "hw/display/rv530" meson.build; then
    patch -p1 < "$HERE/qemu/patches/0001-build-glue-rv530.patch"
fi
if ! grep -q "qemu_input_get_last_abs" include/ui/input.h; then
    patch -p1 < "$HERE/qemu/patches/0002-ui-input-last-abs.patch"
fi
if ! grep -q "RV530_DND_SEND" ui/gtk.c; then
    patch -p1 < "$HERE/qemu/patches/0003-ui-gtk-dnd.patch"
fi
if ! grep -q "gd_resize_dev" ui/gtk.c; then
    patch -p1 < "$HERE/qemu/patches/0004-ui-gtk-resize-toggle.patch"
fi
if ! grep -q "CLOCK_UPTIME_RAW" include/qemu/timer.h; then
    patch -p1 < "$HERE/qemu/patches/0005-tcg-tsc-macos.patch"
fi
if ! grep -q "RV530_DND_SEND" ui/cocoa.m; then
    patch -p1 < "$HERE/qemu/patches/0006-ui-cocoa-rv530.patch"
fi
if ! grep -q "r5xx_vk_mac.m" hw/display/meson.build; then
    patch -p1 < "$HERE/qemu/patches/0007-rv530-metal-check-macos.patch"
fi
if ! grep -q "KVM's clear does" accel/hvf/hvf-all.c; then
    patch -p1 < "$HERE/qemu/patches/0008-hvf-log-clear-page-align.patch"
fi

if ! grep -q "check_clean" accel/tcg/cputlb.c; then
    patch -p1 < "$HERE/qemu/patches/0009-tcg-notdirty-race.patch"
fi
if ! grep -q "hw/display/evergreen" meson.build; then
    patch -p1 < "$HERE/qemu/patches/0010-build-glue-evergreen.patch"
fi

case "$(uname -s)" in
Darwin)
    UI=(--enable-cocoa --enable-sdl)
    # MacPorts (/opt/local, Intel Macs): its pkg-config and Pythons are in
    # its bin, which a login shell may not have on PATH
    if [ -x /opt/local/bin/port ]; then
        export PATH="/opt/local/bin:/opt/local/sbin:$PATH"
    fi
    # QEMU's configure needs Python >= 3.11 (tomllib); macOS ships 3.9
    PY=${PYTHON:-}
    for p in python3.13 python3.12 python3.14 python3.15 python3.11; do
        [ -n "$PY" ] || PY=$(command -v $p || true)
    done
    PY=(${PY:+--python="$PY"})
    JOBS=$(sysctl -n hw.ncpu)
    # Homebrew's keg-only pkg-config files (vulkan-loader, shaderc); not
    # when MacPorts is installed, which then provides the libraries (an Intel
    # Mac may have both, and QEMU must not link a mix of the two)
    if [ ! -x /opt/local/bin/port ] && command -v brew >/dev/null; then
        export PKG_CONFIG_PATH="$(brew --prefix)/lib/pkgconfig:${PKG_CONFIG_PATH:-}"
    fi ;;
*)
    UI=(--enable-gtk)
    PY=()
    JOBS=$(nproc) ;;
esac
mkdir -p build && cd build
# shellcheck disable=SC2086
../configure "${PY[@]}" --target-list=x86_64-softmmu --enable-slirp "${UI[@]}" \
    --enable-vnc --enable-png --disable-docs --disable-werror \
    --enable-trace-backends=log ${CONFIGURE_EXTRA:-}
ninja -j"$JOBS"
if grep -q "r5xx_vk.c" build.ninja; then
    echo "rv530: Vulkan backend built"
else
    echo "rv530: Vulkan/shaderc not found, CPU path only (stub backend)"
fi
mkdir -p "$HERE/build-rom"
python3 "$HERE/scripts/atombios/mkrom.py" "$HERE/build-rom/rv530-atombios.rom"
echo "built: $WORK/qemu-11.1.3/build/qemu-system-x86_64"
echo "ROM:   $HERE/build-rom/rv530-atombios.rom"
