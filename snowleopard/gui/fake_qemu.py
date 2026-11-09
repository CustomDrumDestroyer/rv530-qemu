#!/usr/bin/env python3
"""fake_qemu.py: a stand-in for qemu-system-x86_64, for tests of slgui.py.

It takes the command line of sl-vm.sh, serves the QMP commands that the GUI
uses on the -qmp socket (one client at a time, as QEMU), writes the -D and
-serial logs, and draws a PNG for screendump.  It runs no guest.

  QEMU=snowleopard/gui/fake_qemu.py snowleopard/sl-vm.sh

Power then Return (the shutdown dialog of 10.6) and quit end it.
"""
import json, os, socket, struct, sys, threading, time, zlib


def opt(argv, name):
    for i, a in enumerate(argv[:-1]):
        if a == name:
            return argv[i + 1]
    return ""


def png(path, w, h, phase):
    rows = b"".join(b"\0" + bytes(
        v for x in range(w) for v in ((x * 255 // w + phase) & 255, y * 255 // h, 128))
        for y in range(h))

    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xffffffff)
    data = (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(rows)) + chunk(b"IEND", b""))
    with open(path, "wb") as f:
        f.write(data)


class Fake:
    def __init__(self, argv):
        self.argv = argv
        self.status = "running"
        self.power = 0.0
        self.quit = False
        self.t0 = time.time()
        log = opt(argv, "-D")
        self.log = open(log, "a") if log else sys.stderr
        ser = opt(argv, "-serial")
        self.serial = open(ser[5:], "a") if ser.startswith("file:") else None

    def say(self, msg):
        self.log.write("fake-qemu: %s\n" % msg)
        self.log.flush()

    def handle(self, c):
        ex, args = c.get("execute"), c.get("arguments", {})
        if ex in ("qmp_capabilities", "input-send-event", "system_reset", "human-monitor-command"):
            if ex == "system_reset":
                self.say("reset")
            return ""
        if ex == "query-status":
            return {"status": self.status, "running": self.status == "running"}
        if ex == "stop":
            self.status = "paused"
            return {}
        if ex == "cont":
            self.status = "running"
            return {}
        if ex == "send-key":
            names = [k["data"] for k in args.get("keys", [])]
            self.say("keys %s" % "+".join(names))
            if names == ["power"]:
                self.power = time.time()
            elif names == ["ret"] and time.time() - self.power < 10:
                self.say("guest shuts down")
                threading.Timer(1.0, self.stop).start()
            return {}
        if ex == "screendump":
            png(args["filename"], 640, 400, int((time.time() - self.t0) * 40))
            return {}
        if ex == "quit":
            threading.Timer(0.2, self.stop).start()
            return {}
        return None

    def stop(self):
        self.quit = True

    def serve(self, conn):
        f = conn.makefile("rw", encoding="utf-8", newline="\n")
        f.write(json.dumps({"QMP": {"version": {"qemu": {"major": 11, "minor": 1, "micro": 3}},
                                    "capabilities": []}}) + "\n")
        f.flush()
        for line in f:
            try:
                c = json.loads(line)
            except ValueError:
                continue
            r = self.handle(c)
            if r is None:
                out = {"error": {"class": "CommandNotFound",
                                 "desc": "The command %s has not been found" % c.get("execute")}}
            else:
                out = {"return": r}
            f.write(json.dumps(out) + "\n")
            f.flush()
        conn.close()

    def run(self):
        qmp = opt(self.argv, "-qmp").split(",")[0]
        if qmp.startswith("unix:"):
            path = qmp[5:]
            srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            srv.bind(path)
        else:
            host, _, port = qmp[4:].rpartition(":")
            srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            srv.bind((host or "127.0.0.1", int(port)))
            path = None
        srv.listen(4)
        srv.settimeout(0.2)
        print("fake-qemu: %s, QMP on %s" % (opt(self.argv, "-name"), qmp), flush=True)
        self.say("started: " + " ".join(self.argv[1:]))
        n = 0
        try:
            while not self.quit:
                try:
                    conn, _ = srv.accept()
                except socket.timeout:
                    n += 1
                    if self.serial and n % 10 == 0 and self.status == "running":
                        self.serial.write("fake guest: tick %d\n" % (n // 10))
                        self.serial.flush()
                    continue
                conn.settimeout(None)
                try:
                    self.serve(conn)
                except OSError:             # the client went away (as QEMU, go on)
                    conn.close()
        finally:
            srv.close()
            if path and os.path.exists(path):
                os.unlink(path)
            self.say("exit")


if __name__ == "__main__":
    if "-audiodev" in sys.argv and "help" in sys.argv:
        print("Available audio drivers:\nnone\nwav")
        sys.exit(0)
    Fake(sys.argv).run()
