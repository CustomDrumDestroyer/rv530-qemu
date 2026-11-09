#!/usr/bin/env python3
"""slctl.py: drive a running VM over QMP (screenshots, keyboard, tablet).

  slctl.py shot FILE.png [head] [device]   screendump (PNG)
  slctl.py key KEY [KEY...]                press keys together (QEMU qcode names,
                                           e.g. ret, tab, spc, esc, meta_l, a)
  slctl.py type "text"                     type ASCII text
  slctl.py click X Y [WIDTH HEIGHT]        move the tablet to pixel X,Y of a
                                           WIDTHxHEIGHT screen (default 1024x768)
                                           and click the left button
  slctl.py move X Y [W H]                  move only
  slctl.py aim X Y [W H [click]]         closed-loop move (guest accelerates
                                           tablet input), optionally click
  slctl.py hmp "command"                   human monitor command
  slctl.py shutdown [--confirm] [SECONDS]  clean shutdown: the keyboard Power key
                                           brings up 10.6's "shut down your computer
                                           now?" dialog; --confirm also presses
                                           Return (Shut Down) 3 s later.  Waits up
                                           to SECONDS (120) for QEMU to exit.
                                           (system_powerdown, the ACPI button, asks
                                           10.6 to sleep, which this VM cannot)
  slctl.py quit                            stop QEMU (no guest shutdown)
Socket: $QMP (a path, or HOST:PORT for TCP) or snowleopard/work/qmp.sock.
"""
import json, os, socket, sys, time

SOCK = os.environ.get("QMP", os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                          "work", "qmp.sock"))


def qmp_connect():
    """QMP is a Unix socket path, or HOST:PORT (TCP, as sl-vm.sh uses on
    Windows hosts, where Python has no AF_UNIX)."""
    host, sep, port = SOCK.rpartition(":")
    if sep and port.isdigit() and "/" not in SOCK and "\\" not in SOCK:
        return socket.create_connection((host or "127.0.0.1", int(port)), timeout=60)
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(60)
    s.connect(SOCK)
    return s


def qmp(*cmds):
    s = qmp_connect()
    f = s.makefile("rw")
    f.readline()
    out = []
    for c in ({"execute": "qmp_capabilities"},) + cmds:
        f.write(json.dumps(c) + "\n")
        f.flush()
        while True:
            r = json.loads(f.readline())
            if "event" not in r:
                out.append(r)
                break
    s.close()
    return out[1:]


SHIFT = set('~!@#$%^&*()_+{}|:"<>?ABCDEFGHIJKLMNOPQRSTUVWXYZ')
NAMES = {' ': 'spc', '\n': 'ret', '\t': 'tab', '-': 'minus', '=': 'equal', '[': 'bracket_left',
         ']': 'bracket_right', '\\': 'backslash', ';': 'semicolon', "'": 'apostrophe',
         ',': 'comma', '.': 'dot', '/': 'slash', '`': 'grave_accent',
         '~': 'grave_accent', '!': '1', '@': '2', '#': '3', '$': '4', '%': '5', '^': '6',
         '&': '7', '*': '8', '(': '9', ')': '0', '_': 'minus', '+': 'equal', '{': 'bracket_left',
         '}': 'bracket_right', '|': 'backslash', ':': 'semicolon', '"': 'apostrophe',
         '<': 'comma', '>': 'dot', '?': 'slash'}


def keys(names, hold=100):
    return qmp({"execute": "send-key", "arguments": {
        "keys": [{"type": "qcode", "data": k} for k in names], "hold-time": hold}})


def type_text(t, hold=60, gap=0.14):
    """Type ASCII text over one QMP connection.  send-key returns before the
    key is released, so pace the keys (gap > hold) or the guest drops some."""
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(60)
    s.connect(SOCK)
    f = s.makefile("rw")
    f.readline()
    def cmd(c):
        f.write(json.dumps(c) + "\n"); f.flush()
        while True:
            r = json.loads(f.readline())
            if "event" not in r:
                return r
    cmd({"execute": "qmp_capabilities"})
    for ch in t:
        k = NAMES.get(ch, ch.lower())
        names = (["shift"] if ch in SHIFT else []) + [k]
        cmd({"execute": "send-key", "arguments": {
            "keys": [{"type": "qcode", "data": n} for n in names], "hold-time": hold}})
        time.sleep(gap)
    s.close()


def tablet(x, y, w, h, click):
    ev = [{"type": "abs", "data": {"axis": "x", "value": int(x * 32767 / max(w - 1, 1))}},
          {"type": "abs", "data": {"axis": "y", "value": int(y * 32767 / max(h - 1, 1))}}]
    qmp({"execute": "input-send-event", "arguments": {"events": ev}})
    if click:
        time.sleep(0.1)
        for down in (True, False):
            qmp({"execute": "input-send-event", "arguments": {"events": [
                {"type": "btn", "data": {"down": down, "button": "left"}}]}})
            time.sleep(0.1)


def png_read(fn):
    """Minimal PNG decoder (8-bit RGB/RGBA, non-interlaced) -> (w, h, rows)."""
    import struct as st, zlib
    d = open(fn, "rb").read()
    pos, idat, w = 8, b"", 0
    while pos < len(d):
        ln, typ = st.unpack(">I4s", d[pos:pos + 8])
        body = d[pos + 8:pos + 8 + ln]
        if typ == b"IHDR":
            w, h, bd, ct = st.unpack(">IIBB", body[:10])
            bpp = 4 if ct == 6 else 3
        elif typ == b"IDAT":
            idat += body
        pos += 12 + ln
    raw = zlib.decompress(idat)
    stride = w * bpp
    rows, prev, i = [], bytearray(stride), 0
    for y in range(h):
        f = raw[i]; line = bytearray(raw[i + 1:i + 1 + stride]); i += 1 + stride
        for x in range(stride):
            a = line[x - bpp] if x >= bpp else 0
            b = prev[x]
            c = prev[x - bpp] if x >= bpp else 0
            if f == 1: line[x] = (line[x] + a) & 255
            elif f == 2: line[x] = (line[x] + b) & 255
            elif f == 3: line[x] = (line[x] + ((a + b) >> 1)) & 255
            elif f == 4:
                p = a + b - c; pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                pr = a if pa <= pb and pa <= pc else (b if pb <= pc else c)
                line[x] = (line[x] + pr) & 255
        rows.append(bytes(line)); prev = line
    return w, h, bpp, rows


def find_cursor(before, after):
    """Top-left of the region that changed between two screenshots (the cursor
    tip, after it moved there from somewhere far away)."""
    w, h, bpp, r1 = png_read(before)
    _, _, _, r2 = png_read(after)
    pts = [(x, y) for y in range(h) for x in range(0, w)
           if r1[y][x * bpp:x * bpp + 3] != r2[y][x * bpp:x * bpp + 3]]
    return pts


def goto(x, y, w, h, tries=6):
    """Closed-loop pointer placement: the guest accelerates tablet input, so
    move, find where the cursor actually is, and correct."""
    tmp = os.path.join(os.path.dirname(SOCK), "_cur")
    tx, ty = x, y
    for _ in range(tries):
        # park far away (bottom right), then move to the target
        tablet(w - 1, h - 1, w, h, False); time.sleep(0.4)
        qmp({"execute": "screendump", "arguments": {"filename": tmp + "a.png", "format": "png"}})
        tablet(tx, ty, w, h, False); time.sleep(0.4)
        qmp({"execute": "screendump", "arguments": {"filename": tmp + "b.png", "format": "png"}})
        pts = [p for p in find_cursor(tmp + "a.png", tmp + "b.png") if p[0] < w - 40 or p[1] < h - 40]
        # group changed pixels into 24x24 cells; the arrow is the biggest
        # blob, a blinking text caret is a thin one
        cells = {}
        for p in pts:
            cells.setdefault((p[0] // 24, p[1] // 24), []).append(p)
        best = None
        for (gx, gy), _ in cells.items():
            blob = [q for (cx2, cy2), qs in cells.items()
                    if abs(cx2 - gx) <= 1 and abs(cy2 - gy) <= 1 for q in qs]
            xs = [q[0] for q in blob]
            if max(xs) - min(xs) < 5:
                continue
            if best is None or len(blob) > len(best):
                best = blob
        if not best:
            return tx, ty
        cx = min(p[0] for p in best); cy = min(p[1] for p in best)
        if abs(cx - x) <= 2 and abs(cy - y) <= 2:
            return tx, ty
        tx = max(0, min(w - 1, tx + (x - cx))); ty = max(0, min(h - 1, ty + (y - cy)))
    return tx, ty


def main(a):
    if a[0] == "shot":
        args = {"filename": os.path.abspath(a[1]), "format": "png"}
        if len(a) > 2:
            args["head"] = int(a[2])
        if len(a) > 3:
            args["device"] = a[3]
        print(qmp({"execute": "screendump", "arguments": args}))
    elif a[0] == "key":
        print(keys(a[1:]))
    elif a[0] == "type":
        type_text(a[1])
    elif a[0] == "clickhere":
        for down in (True, False):
            qmp({"execute": "input-send-event", "arguments": {"events": [
                {"type": "btn", "data": {"down": down, "button": "left"}}]}})
            time.sleep(0.1)
    elif a[0] == "aim":
        w, h = (int(a[3]), int(a[4])) if len(a) > 4 else (1440, 900)
        tx, ty = goto(int(a[1]), int(a[2]), w, h)
        if len(a) > 5 and a[5] == "click":
            tablet(tx, ty, w, h, True)
        print("tablet", tx, ty)
    elif a[0] in ("click", "move"):
        w, h = (int(a[3]), int(a[4])) if len(a) > 4 else (1024, 768)
        tablet(int(a[1]), int(a[2]), w, h, a[0] == "click")
    elif a[0] == "hmp":
        r = qmp({"execute": "human-monitor-command", "arguments": {"command-line": a[1]}})
        print(r[0].get("return", r))
    elif a[0] == "shutdown":
        confirm = "--confirm" in a[1:]
        rest = [x for x in a[1:] if x != "--confirm"]
        limit = float(rest[0]) if rest else 120.0
        keys(["power"])
        if confirm:
            time.sleep(3)
            keys(["ret"])
        t0 = time.time()
        while time.time() - t0 < limit:
            try:
                qmp({"execute": "query-status"})
            except (OSError, ValueError):
                # QMP goes away at power-off; the process exits just after
                while os.path.exists(SOCK) and time.time() - t0 < limit:
                    time.sleep(0.2)
                print("VM shut down after %.0f s" % (time.time() - t0))
                return
            time.sleep(1)
        print("VM still running after %.0f s (an app may be asking to save)" % limit)
        sys.exit(1)
    elif a[0] == "quit":
        print(qmp({"execute": "quit"}))
    else:
        print(__doc__)


if __name__ == "__main__":
    main(sys.argv[1:] or ["help"])
