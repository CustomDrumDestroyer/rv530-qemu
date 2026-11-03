#!/bin/bash
# build-release.sh NAME OUTDIR: build QEMU with the rv530 and pack a release
# package, OUTDIR/NAME.tar.gz, that runs without the build's toolchain or
# package manager.
#
#   NAME/bin/qemu-system-x86_64, bin/qemu-img   QEMU (on macOS the emulator
#                                                is libexec/, bin/ a wrapper
#                                                that points the Vulkan
#                                                loader at the bundled
#                                                MoltenVK)
#   NAME/lib/                                    bundled libraries
#                                                (bundle-libs.py)
#   NAME/share/qemu/                             QEMU's data files
#   NAME/firmware/                               OVMF with the rv530 GOP
#                                                driver, when OVMF_DIR holds
#                                                build-ovmf.sh's output
#   NAME/build-rom/rv530-atombios.rom            the synthetic option ROM
#   NAME/...                                     the repository's files
#                                                (scripts, snowleopard/,
#                                                docs, device sources)
#
# snowleopard/sl-vm.sh finds the emulator and the firmware in that layout.
# Environment: OVMF_DIR (OVMF_CODE.fd and OVMF_VARS.fd to include),
# WORK (QEMU's build tree, default build-qemu), CONFIGURE_EXTRA.
set -euo pipefail
HERE=$(cd "$(dirname "$0")/../.." && pwd)
NAME=${1:?package name}
OUT=$(mkdir -p "${2:?output directory}" && cd "$2" && pwd)
WORK=${WORK:-$HERE/build-qemu}
PREFIX=/rv530-qemu
PKG=$OUT/$NAME
# macOS packages use the Cocoa window and leave SDL out: Homebrew's sdl2 is
# sdl2-compat, which dlopens SDL3, so bundling cannot see it, and without it
# the library shows a modal error dialog from its initializer, before main().
# Linux packages leave out what the VM doesn't use and a distribution may not
# have under the same name: QEMU's OpenGL display (the device draws through
# Vulkan; this also drops libepoxy and libgbm, which is not always installed
# and has no configure option of its own), libjpeg (VNC's JPEG encoding; .so.8
# on Ubuntu, .so.62 on Debian and Fedora), and libgcrypt (QEMU has built-in
# crypto).  Audio and GTK are modules, loaded when used, so that a host without
# PulseAudio's or ALSA's library only loses that audio driver.
if [ "$(uname -s)" = Darwin ]; then
    CONFIGURE_EXTRA="--disable-sdl ${CONFIGURE_EXTRA:-}"
else
    CONFIGURE_EXTRA="--enable-modules --libdir=lib --disable-opengl --disable-vnc-jpeg --disable-gcrypt ${CONFIGURE_EXTRA:-}"
fi

CONFIGURE_EXTRA="--prefix=$PREFIX --disable-guest-agent ${CONFIGURE_EXTRA:-}" \
    "$HERE/scripts/setup-qemu.sh" "$WORK"
B=$WORK/qemu-11.1.2/build
grep -q "r5xx_vk.c" "$B/build.ninja" || { echo "no Vulkan backend in this build" >&2; exit 1; }

rm -rf "$PKG" "$OUT/stage"
DESTDIR=$OUT/stage ninja -C "$B" install >/dev/null
mkdir -p "$PKG/bin" "$PKG/share"
cp "$OUT/stage$PREFIX/bin/qemu-system-x86_64" "$OUT/stage$PREFIX/bin/qemu-img" "$PKG/bin/"
cp -R "$OUT/stage$PREFIX/share/qemu" "$PKG/share/"
if [ -d "$OUT/stage$PREFIX/lib/qemu" ]; then
    mkdir -p "$PKG/lib"
    cp -R "$OUT/stage$PREFIX/lib/qemu" "$PKG/lib/"
fi
rm -rf "$OUT/stage"

# the repository's files, as tracked, and the generated option ROM
(cd "$HERE" && git ls-files -z | xargs -0 tar -cf - --) | tar -xf - -C "$PKG"
mkdir -p "$PKG/build-rom"
cp "$HERE/build-rom/rv530-atombios.rom" "$PKG/build-rom/"
if [ -n "${OVMF_DIR:-}" ]; then
    mkdir -p "$PKG/firmware"
    cp "$OVMF_DIR/OVMF_CODE.fd" "$OVMF_DIR/OVMF_VARS.fd" "$PKG/firmware/"
fi

case "$(uname -s)" in
Darwin)
    # The emulator goes to libexec/ (QEMU finds share/qemu from there as
    # from bin/); bin/qemu-system-x86_64 sets the Vulkan loader's driver
    # list to the bundled MoltenVK unless the caller chose drivers itself.
    mkdir -p "$PKG/libexec" "$PKG/share/vulkan/icd.d"
    mv "$PKG/bin/qemu-system-x86_64" "$PKG/libexec/"
    if otool -L "$PKG/libexec/qemu-system-x86_64" | grep -i sdl; then
        echo "SDL is linked in a macOS package" >&2; exit 1
    fi
    MVK=
    # MacPorts first, as setup-qemu.sh builds against it when it is installed
    for d in /opt/local/lib "$(brew --prefix molten-vk 2>/dev/null || true)/lib"; do
        [ -f "$d/libMoltenVK.dylib" ] && { MVK=$d/libMoltenVK.dylib; break; }
    done
    [ -n "$MVK" ] || { echo "no libMoltenVK.dylib (Homebrew molten-vk or MacPorts MoltenVK)" >&2; exit 1; }
    mkdir -p "$PKG/lib"
    cp "$(python3 -c 'import os,sys; print(os.path.realpath(sys.argv[1]))' "$MVK")" "$PKG/lib/libMoltenVK.dylib"
    chmod 755 "$PKG/lib/libMoltenVK.dylib"
    # MoltenVK's own driver manifest, pointed at the bundled library
    # (the loader reads a relative library_path from the manifest's folder)
    JSON=
    for j in /opt/local/share/vulkan/icd.d/MoltenVK_icd.json \
             "$(brew --prefix 2>/dev/null || true)/etc/vulkan/icd.d/MoltenVK_icd.json"; do
        [ -f "$j" ] && { JSON=$j; break; }
    done
    [ -n "$JSON" ] || { echo "no MoltenVK_icd.json" >&2; exit 1; }
    python3 - "$JSON" "$PKG/share/vulkan/icd.d/MoltenVK_icd.json" <<'PY'
import json, sys
j = json.load(open(sys.argv[1]))
j["ICD"]["library_path"] = "../../../lib/libMoltenVK.dylib"
json.dump(j, open(sys.argv[2], "w"), indent=4)
PY
    cat > "$PKG/bin/qemu-system-x86_64" <<'EOF'
#!/bin/sh
# rv530-qemu: run the emulator with the bundled MoltenVK as the Vulkan driver
# (unless VK_DRIVER_FILES or VK_ICD_FILENAMES choose another).
d=$(cd "$(dirname "$0")/.." && pwd)
if [ -z "${VK_DRIVER_FILES:-}${VK_ICD_FILENAMES:-}" ]; then
    VK_DRIVER_FILES=$d/share/vulkan/icd.d/MoltenVK_icd.json
    export VK_DRIVER_FILES
fi
exec "$d/libexec/qemu-system-x86_64" "$@"
EOF
    chmod 755 "$PKG/bin/qemu-system-x86_64"
    BUNDLE_EXTRA_LIBS=$PKG/lib/libMoltenVK.dylib \
        python3 "$HERE/scripts/package/bundle-libs.py" "$PKG" libexec/qemu-system-x86_64 bin/qemu-img
    printf 'libMoltenVK.dylib\t%s\n' "$(python3 -c 'import os,sys; print(os.path.realpath(sys.argv[1]))' "$MVK")" \
        >> "$PKG/lib/bundled.txt"
    # Hypervisor.framework (Intel Macs) needs the entitlement QEMU's build
    # signs with; bundling rewrote the binary, so sign it again
    codesign --force --sign - --entitlements "$WORK/qemu-11.1.2/accel/hvf/entitlements.plist" \
        "$PKG/libexec/qemu-system-x86_64"
    codesign -d --entitlements - "$PKG/libexec/qemu-system-x86_64" 2>/dev/null |
        grep -q com.apple.security.hypervisor || { echo "entitlement missing" >&2; exit 1; }
    ;;
*)
    python3 "$HERE/scripts/package/bundle-libs.py" "$PKG" bin/qemu-system-x86_64 bin/qemu-img \
        $(cd "$PKG" && ls lib/qemu/*.so)
    ;;
esac
python3 "$HERE/scripts/package/notices.py" "$PKG"

# smoke test: the packaged emulator starts and realizes the device, paused
env -u LD_LIBRARY_PATH "$HERE/scripts/package/smoke-test.sh" "$PKG"

tar -C "$OUT" -czf "$OUT/$NAME.tar.gz" "$NAME"
echo "package: $OUT/$NAME.tar.gz"
