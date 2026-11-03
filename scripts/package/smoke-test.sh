#!/bin/sh
# smoke-test.sh PKG: the release package's emulator starts, has the rv530,
# and realizes it (paused, no display, TCG, so it runs anywhere), and qemu-img
# runs.  POSIX sh: it also runs in minimal distribution containers, and macOS
# has no timeout(1).  A command that hangs fails the test, with a stack sample
# on macOS.
set -eu
PKG=${1:?package directory}
LIMIT=${SMOKE_TIMEOUT:-120}
LOG=$(mktemp)

limited() {
    "$@" >"$LOG" 2>&1 </dev/null &
    pid=$!
    (
        sleep "$LIMIT" >/dev/null 2>&1
        kill -0 "$pid" 2>/dev/null || exit 0
        echo "smoke test: still running after $LIMIT s: $*" >&2
        if command -v sample >/dev/null; then
            sudo -n sample "$pid" 5 >&2 2>&1 || sample "$pid" 5 >&2 2>&1 || true
        fi
        kill -9 "$pid" 2>/dev/null
    ) &
    dog=$!
    rc=0
    wait "$pid" || rc=$?
    kill "$dog" 2>/dev/null || true
    cat "$LOG"
    return "$rc"
}

limited "$PKG/bin/qemu-system-x86_64" -version
limited "$PKG/bin/qemu-img" --version
limited "$PKG/bin/qemu-system-x86_64" -device help
grep -q '"rv530"' "$LOG" || { echo "smoke test: no rv530 device" >&2; exit 1; }
if [ "$(uname -s)" = Linux ]; then
    # a module on Linux: the host's GTK must load it
    limited "$PKG/bin/qemu-system-x86_64" -display help
    grep -qx gtk "$LOG" || { echo "smoke test: no GTK display" >&2; exit 1; }
fi

QMP=$(mktemp); export QMP
printf '%s\n' '{"execute":"qmp_capabilities"}' \
    '{"execute":"qom-get","arguments":{"path":"/machine/peripheral/gpu","property":"vram_size_mb"}}' \
    '{"execute":"quit"}' >"$QMP"
limited sh -c 'exec "$0" "$@" <"$QMP"' "$PKG/bin/qemu-system-x86_64" -M q35 -nodefaults -accel tcg -m 512 -S -display none \
    -device pci-bridge,id=rp1,bus=pcie.0,addr=0x1,chassis_nr=1,shpc=off \
    -device rv530,id=gpu,bus=rp1,romfile="$PKG/build-rom/rv530-atombios.rom" \
    -qmp stdio
grep -q '"return": 256' "$LOG" || { echo "smoke test: the rv530 did not realize" >&2; exit 1; }
rm -f "$LOG" "$QMP"
echo "smoke test passed"
