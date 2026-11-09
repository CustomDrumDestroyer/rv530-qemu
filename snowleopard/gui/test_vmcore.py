#!/usr/bin/env python3
"""Tests of vmcore.py (no display needed):

    python3 snowleopard/gui/test_vmcore.py

The QMP and launch tests run sl-vm.sh with fake_qemu.py as QEMU.
"""
import json
import os
import plistlib
import shutil
import socket
import subprocess
import sys
import tempfile
import time
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import vmcore as core  # noqa: E402

FAKE = os.path.join(HERE, "fake_qemu.py")
POSIX = not core.IS_WINDOWS


class Tmp(unittest.TestCase):
    def setUp(self):
        # short: a Unix socket path has a limit of about 100 bytes
        self.tmp = tempfile.mkdtemp(prefix="slgui")
        self.addCleanup(shutil.rmtree, self.tmp, True)
        self._default = core.DEFAULT_WORK
        core.DEFAULT_WORK = os.path.join(self.tmp, "work")
        self.addCleanup(setattr, core, "DEFAULT_WORK", self._default)

    def mkdir(self, *p):
        d = os.path.join(self.tmp, *p)
        os.makedirs(d, exist_ok=True)
        return d

    def touch(self, *p, data=b""):
        f = os.path.join(self.tmp, *p)
        os.makedirs(os.path.dirname(f), exist_ok=True)
        with open(f, "wb") as fh:
            fh.write(data)
        return f


class TestSettings(Tmp):
    def test_osk_env(self):
        for text, want in (("export OSK='a b\"c'\n", 'a b"c'), ('OSK="xyz" # key\n', "xyz"),
                           ("# OSK=no\nexport OSK=plain\n", "plain"), ("FOO=1\n", "")):
            p = self.touch("osk.env", data=text.encode())
            self.assertEqual(core.read_osk_env(p), want, text)
        self.assertEqual(core.read_osk_env(os.path.join(self.tmp, "none")), "")

    def test_find_osk_order(self):
        vm = core.VM(self.mkdir("work", "lion"))
        self.assertEqual(core.find_osk(vm, {}), ("", ""))
        self.assertEqual(core.find_osk(vm, {"OSK": "env"})[0], "env")
        self.touch("work", "osk.env", data=b"OSK=shared\n")
        self.assertEqual(core.find_osk(vm, {"OSK": "env"})[0], "shared")
        self.touch("work", "lion", "osk.env", data=b"OSK=own\n")
        self.assertEqual(core.find_osk(vm, {})[0], "own")
        vm.env["OSK"] = "settings"
        self.assertEqual(core.find_osk(vm, {}), ("settings", "VM settings"))

    def test_save_load_defaults(self):
        w = self.mkdir("work", "lion")
        vm = core.VM(w, "Lion", {"DISPLAY_MODE": "rv530", "MEM": "", "SSHPORT": 2226},
                     {"SL_KCACHE": "Auto"})
        vm.save()
        with open(os.path.join(w, "vm.json")) as f:
            d = json.load(f)
        self.assertEqual(d["env"], {"DISPLAY_MODE": "rv530", "SSHPORT": "2226"})
        back = core.VM.load(w)
        self.assertEqual((back.name, back.env, back.opencore), (vm.name, vm.env, vm.opencore))
        self.assertEqual(back.value("SYSDISK"), os.path.join(w, "snowleopard.qcow2"))
        self.assertEqual(back.value("VNC"), ":2")
        self.assertEqual(back.value("SSHPORT"), "2226")
        self.assertEqual(core.VM.load(self.mkdir("new")).env, {})
        self.touch("bad", "vm.json", data=b"{")
        with self.assertRaises(core.VMError):
            core.VM.load(os.path.join(self.tmp, "bad"))

    def test_discover(self):
        base = self.mkdir("work")
        self.mkdir("work", "fw")                        # the OVMF build: not a VM
        self.mkdir("work", "empty")
        self.touch("work", "lion", "lion.qcow2")
        core.VM(self.mkdir("work", "leopard"), "Leopard").save()
        other = self.mkdir("elsewhere")
        reg = core.Registry(os.path.join(self.tmp, "reg.json"))
        names = lambda: [v.name for v in core.discover(reg, base)]  # noqa: E731
        self.assertEqual(names(), ["Leopard", "lion"])
        self.mkdir("work", "esp")                       # now work/ is a VM too
        self.assertEqual(names(), ["Snow Leopard", "Leopard", "lion"])
        reg.add(other)
        reg.remove(os.path.join(base, "lion"))
        reg.save()
        reg = core.Registry(reg.path)
        self.assertEqual(names(), ["Snow Leopard", "Leopard", "elsewhere"])
        reg.add(os.path.join(base, "lion"))
        self.assertIn("lion", names())

    def test_free_ports(self):
        a = core.VM(self.mkdir("a"))
        b = core.VM(self.mkdir("b"), env={"SSHPORT": "2223", "VNC": ":3"})
        self.assertEqual(core.free_ports([a, b])[:2], ("2224", ":4"))
        self.assertEqual(core.free_ports([])[:2], ("2222", ":2"))

    def test_new_vm(self):
        vm = core.new_vm("My Lion", os.path.join(self.tmp, "work", "lion"),
                         "Mac OS X 10.7 Lion", others=[core.VM(self.mkdir("a"))])
        self.assertEqual(vm.opencore, {"SL_KCACHE": "Auto"})
        self.assertEqual((vm.env["SSHPORT"], vm.env["VNC"], vm.env["INSTALL"]), ("2223", ":3", "1"))
        self.assertTrue(vm.saved)
        with self.assertRaises(core.VMError):
            core.new_vm("again", vm.work, "Other (Linux guest, ...)", others=[])

    def test_vnc_port(self):
        self.assertEqual(core.vnc_port(":2"), 5902)
        self.assertEqual(core.vnc_port("127.0.0.1:10"), 5910)
        self.assertIsNone(core.vnc_port("none"))

    def test_launch_env(self):
        w = self.mkdir("work", "lion")
        self.touch("work", "fw", "OVMF_CODE.fd")
        self.touch("work", "fw", "OVMF_VARS.fd")
        self.touch("work", "osk.env", data=b"export OSK='k'\n")
        vm = core.VM(w, env={"DISPLAY_MODE": "rv530", "EXTRA": "-s -S"})
        env = core.launch_env(vm, {"PATH": "/bin", "DISPLAY_MODE": "both", "INSTALL": "1",
                                   "QEMU": "/q"})
        self.assertEqual(env["DISPLAY_MODE"], "rv530")
        self.assertNotIn("INSTALL", env)                # leaked from the GUI's shell
        self.assertEqual((env["QEMU"], env["PATH"], env["OSK"]), ("/q", "/bin", "k"))
        self.assertEqual(env["WORK"], w)
        # sl-vm.sh looks in WORK/fw only: the GUI gives it the shared build
        self.assertEqual(env["OVMF_CODE"], os.path.join(self.tmp, "work", "fw", "OVMF_CODE.fd"))
        cmd = core.launch_command(vm)
        self.assertTrue(cmd.startswith(". "), cmd)
        self.assertIn("EXTRA='-s -S'", cmd)
        self.assertIn("OVMF_CODE=", cmd)
        self.assertNotIn("OSK=", cmd.split("&&")[1])

    def test_preflight(self):
        w = self.mkdir("work", "a")
        vm = core.VM(w, env={"MEM": "lots", "INSTALL": "1"})
        msgs = core.preflight(vm, environ={"PATH": ""}, check_ports=False)
        errors = " ".join(m for l, m in msgs if l == "error")
        for want in ("SMC key", "OpenCore boot volume", "Memory", "installer"):
            self.assertIn(want, errors)
        other = core.VM(self.mkdir("work", "b"), "B")
        msgs = core.preflight(vm, running=[other], environ={}, check_ports=False)
        self.assertIn('The ssh port 2222 is used by the running VM "B".',
                      [m for l, m in msgs if l == "error"])
        code = self.touch("code.fd")
        vm.env["OVMF_CODE"] = code
        msgs = core.preflight(vm, environ={}, check_ports=False)
        self.assertTrue(any("vars template" in m for l, m in msgs if l == "error"))
        vm.env["OVMF_VARS_TEMPLATE"] = code
        msgs = core.preflight(vm, environ={}, check_ports=False)
        self.assertFalse(any("OVMF" in m for l, m in msgs if l == "error"))

    def test_port_check(self):
        s = socket.socket()
        s.bind(("127.0.0.1", 0))
        s.listen(1)
        port = s.getsockname()[1]
        try:
            self.assertFalse(core.port_free(port))
            vm = core.VM(self.mkdir("p"), env={"SSHPORT": str(port)})
            msgs = core.preflight(vm, environ={})
            self.assertTrue(any("Port %d (ssh)" % port in m for l, m in msgs if l == "warning"))
        finally:
            s.close()

    def test_opencore_job(self):
        esp = self.mkdir("work", "a", "esp", "EFI", "OC")
        with open(os.path.join(esp, "config.plist"), "wb") as f:
            plistlib.dump({"PlatformInfo": {"Generic": {"SystemUUID": "OLD-UUID"}}}, f)
        vm = core.VM(os.path.join(self.tmp, "work", "a"), env={"ACCEL": "tcg"})
        argv, env, opts = core.opencore_job(vm, {"SL_PICKER": "1", "SL_KARCH": ""})
        self.assertEqual(opts, {"SL_PICKER": "1", "SL_UUID": "OLD-UUID", "SL_TCG": "1",
                                "SL_SLTO": "2000000"})
        self.assertEqual(argv[-2:], ["", os.path.join(vm.work, "esp")])
        self.assertEqual(env["SL_UUID"], "OLD-UUID")
        _, _, opts = core.opencore_job(vm, {"SL_UUID": ""})
        self.assertNotIn(opts["SL_UUID"], ("", "OLD-UUID"))


def start_fake(work, qmp):
    p = subprocess.Popen([sys.executable, FAKE, "-name", "t", "-qmp", "unix:%s,server=on" % qmp,
                          "-D", os.path.join(work, "qemu.log")],
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    for _ in range(100):
        if os.path.exists(qmp):
            return p
        time.sleep(0.05)
    p.kill()
    raise RuntimeError("fake_qemu did not start")


@unittest.skipUnless(POSIX, "Unix sockets")
class TestQMP(Tmp):
    def test_probe_and_commands(self):
        qmp = os.path.join(self.tmp, "q.sock")
        self.assertEqual(core.probe(qmp), core.STOPPED)
        p = start_fake(self.tmp, qmp)
        self.addCleanup(p.kill)
        self.assertEqual(core.probe(qmp), "running")
        with core.QMP(qmp) as q:
            q.cmd("stop")
            shot = os.path.join(self.tmp, "s.png")
            core.screendump(q, shot)
            with self.assertRaises(core.QMPError):
                q.cmd("no-such-command")
        with open(shot, "rb") as f:
            self.assertEqual(f.read(8), b"\x89PNG\r\n\x1a\n")
        self.assertEqual(core.probe(qmp), "paused")
        # QEMU serves one client at a time: the second one waits
        with core.QMP(qmp):
            self.assertEqual(core.probe(qmp, timeout=0.5), core.BUSY)
        with core.QMP(qmp) as q:
            q.cmd("cont")
            core.type_text(q, "Hi!", gap=0)
            q.cmd("quit")
        p.wait(5)
        self.assertEqual(core.probe(qmp), core.STOPPED)
        with open(os.path.join(self.tmp, "qemu.log")) as f:
            log = f.read()
        self.assertIn("keys shift+h", log)
        self.assertIn("keys shift+1", log)


@unittest.skipUnless(POSIX and core.find_bash(), "bash")
class TestLaunch(Tmp):
    def test_sl_vm_with_fake_qemu(self):
        """Runner starts sl-vm.sh, which runs fake_qemu.py with the full
        command line; the clean shutdown over QMP ends it."""
        w = self.mkdir("work", "vm")
        self.mkdir("work", "vm", "esp")
        f = {n: self.touch("fw", n) for n in ("code.fd", "vars.fd", "rom", "disk.qcow2")}
        vm = core.VM(w, "T", {"QEMU": FAKE, "OSK": "test", "OVMF_CODE": f["code.fd"],
                              "OVMF_VARS_TEMPLATE": f["vars.fd"], "ROM": f["rom"],
                              "ROM5770": f["rom"], "SYSDISK": f["disk.qcow2"],
                              "DISPLAY_MODE": "rv530", "VNC": "none", "SSHPORT": "0"})
        self.assertEqual([m for l, m in core.preflight(vm, check_ports=False) if l == "error"], [])
        r = core.Runner()
        r.start(vm)
        for _ in range(100):
            if core.probe(vm.qmp_addr()) == "running":
                break
            time.sleep(0.1)
        else:
            self.fail("not running:\n" + core.tail(os.path.join(w, core.CONSOLE_LOG)))
        self.assertTrue(r.alive(w))
        with open(os.path.join(w, "qemu.log")) as fh:
            cmdline = fh.read()
        self.assertIn("-device rv530,id=gpu", cmdline)
        self.assertIn("isa-applesmc,osk=test", cmdline)
        with core.QMP(vm.qmp_addr()) as q:
            core.clean_shutdown(q, confirm=True)
        for _ in range(100):
            ended = r.reap()
            if ended:
                break
            time.sleep(0.1)
        self.assertEqual([(e[0], e[1]) for e in ended], [(w, 0)])
        self.assertIn("fake-qemu: snowleopard, QMP on unix:", core.tail(os.path.join(w, core.CONSOLE_LOG)))


if __name__ == "__main__":
    unittest.main()
