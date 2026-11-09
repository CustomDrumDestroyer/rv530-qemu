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

ROM=$PKG/build-rom/rv530-atombios.rom
# Windows (MSYS2): a native program needs a Windows path inside an option
command -v cygpath >/dev/null && ROM=$(cygpath -m "$ROM")
QMP=$(mktemp); export QMP
printf '%s\n' '{"execute":"qmp_capabilities"}' \
    '{"execute":"qom-get","arguments":{"path":"/machine/peripheral/gpu","property":"vram_size_mb"}}' \
    '{"execute":"quit"}' >"$QMP"
set -- "$PKG/bin/qemu-system-x86_64" -M q35 -nodefaults -accel tcg -m 512 -S -display none \
    -device pci-bridge,id=rp1,bus=pcie.0,addr=0x1,chassis_nr=1,shpc=off \
    -device rv530,id=gpu,bus=rp1,romfile="$ROM"
if command -v cygpath >/dev/null; then
    # Windows: QEMU's stdio chardev loses input from a file, so send the
    # commands over a TCP socket
    PY=$(mktemp)
    cat >"$PY" <<'PY'
import socket, subprocess, sys, time
p = subprocess.Popen(sys.argv[2:])
for _ in range(100):
    try:
        s = socket.create_connection(("127.0.0.1", 44556))
        break
    except OSError:
        time.sleep(0.2)
s.sendall(open(sys.argv[1], "rb").read())
data = b""
while True:
    try:
        chunk = s.recv(4096)
    except ConnectionResetError:    # Windows, after QEMU quits
        break
    if not chunk:
        break
    data += chunk
print(data.decode())
sys.exit(p.wait())
PY
    limited python3 "$PY" "$QMP" "$@" -qmp tcp:127.0.0.1:44556,server=on,wait=on
    rm -f "$PY"
else
    limited sh -c 'exec "$0" "$@" <"$QMP"' "$@" -qmp stdio
fi
grep -q '"return": 256' "$LOG" || { echo "smoke test: the rv530 did not realize" >&2; exit 1; }
rm -f "$LOG" "$QMP"
echo "smoke test passed"
