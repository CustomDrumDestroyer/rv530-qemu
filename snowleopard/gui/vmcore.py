"""vmcore.py: VM definitions, launch and QMP control for slgui.py.

This module has no GUI code: slgui.py is the Tk front end, and
test_vmcore.py tests this module without a display.

A VM is a directory: the WORK directory of sl-vm.sh, which holds the system
disk, the NVRAM, the boot volume, the sockets and the logs.  The GUI keeps
the settings of a VM in WORK/vm.json:

    {"name": "Lion", "env": {"DISPLAY_MODE": "rv530", "SSHPORT": "2226", ...},
     "opencore": {"SL_KCACHE": "Auto", "SL_UUID": "..."}}

"env" holds the sl-vm.sh environment variables (see the top of sl-vm.sh),
and "opencore" the make-opencore.sh variables of the boot volume of the VM.
The GUI starts a VM with exactly these variables, so a VM that the GUI made
also starts from a terminal (launch_command() prints the command).

A VM that runs is found through its QMP socket, so the GUI also sees VMs
that were started from a terminal.
"""
import json
import os
import plistlib
import re
import shlex
import shutil
import socket
import subprocess
import sys
import time
import uuid

HERE = os.path.dirname(os.path.abspath(__file__))
SL = os.path.dirname(HERE)                  # snowleopard/
REPO = os.path.dirname(SL)
DEFAULT_WORK = os.path.join(SL, "work")
VM_FILE = "vm.json"
CONSOLE_LOG = "vm.out"                      # the output of sl-vm.sh
IS_WINDOWS = os.name == "nt"
IS_MAC = sys.platform == "darwin"

sys.path.insert(0, SL)
from slctl import NAMES as KEY_NAMES, SHIFT as KEY_SHIFT  # noqa: E402


# ---------------------------------------------------------------- settings

class Field:
    """One sl-vm.sh environment variable, as the settings form shows it."""
    __slots__ = ("key", "label", "kind", "choices", "default", "help")

    def __init__(self, key, label, kind="text", choices=(), default="", help=""):
        self.key, self.label, self.kind = key, label, kind
        self.choices, self.default, self.help = tuple(choices), default, help


def host_accel():
    """The accelerator that snowleopard/host.sh selects on this host."""
    if sys.platform.startswith("linux") and os.uname().machine == "x86_64" \
            and os.access("/dev/kvm", os.W_OK):
        return "kvm"
    if IS_MAC:
        def sysctl(name):
            try:
                return subprocess.run(["sysctl", "-n", name], capture_output=True,
                                      text=True, timeout=5).stdout.strip()
            except (OSError, subprocess.SubprocessError):
                return "0"
        if sysctl("hw.optional.arm64") != "1" and sysctl("kern.hv_support") == "1":
            return "hvf"
    return "tcg"


GROUPS = [
    ("Machine", [
        Field("DISPLAY_MODE", "Graphics", "choice", ("std", "rv530", "both", "hd5770"), "std",
              "rv530: the emulated Radeon X1600 is the only GPU. std: standard VGA, "
              "for the installation. both: VGA for the firmware and the X1600. "
              "hd5770: the experimental Radeon HD 5770 (it needs its own boot volume)."),
        Field("QDISPLAY", "Window", "choice", ("gtk", "cocoa", "sdl", "none"), "none",
              "The QEMU window: gtk on Linux and Windows, cocoa on a Mac. "
              "none: no window, use a VNC viewer."),
        Field("MEM", "Memory (MB)", "int", (), "4096 (2048 under TCG)"),
        Field("SMP", "vCPUs", "text", (), "4",
              "More vCPUs make the GPU faster: the command processor runs in the "
              "vCPU that sends the work."),
        Field("ACCEL", "Accelerator", "choice", ("kvm", "hvf", "tcg", "whpx"), "auto",
              "Empty: host.sh selects (KVM on x86_64 Linux, Hypervisor.framework on "
              "an Intel Mac, else TCG)."),
        Field("TSC_HZ", "TSC (Hz)", "int", (), "1200000000",
              "TCG only. At 1 GHz, 10.6 panics when it builds its commpage."),
    ]),
    ("Display", [
        Field("XRES", "Width", "int", (), "1440", "The preferred mode of the EDID."),
        Field("YRES", "Height", "int", (), "900"),
        Field("XMAX", "Max width", "int", (), "2560", "The largest mode of the EDID (rv530)."),
        Field("YMAX", "Max height", "int", (), "1600"),
        Field("REFRESH", "Refresh (Hz)", "int", (), "60", "Up to 240."),
        Field("AUTORESIZE", "Resize guest with window", "bool", (), "off",
              "Needs the guest agent. View > Resize Guest with Window changes it "
              "while the VM runs."),
        Field("RVOPTS", "rv530 options", "text", (), "",
              "More rv530 device properties, e.g. vulkan=off."),
        Field("EGOPTS", "HD 5770 options", "text", (), "",
              "More radeon-hd5770 device properties."),
    ]),
    ("Storage", [
        Field("SYSDISK", "System disk", "file", (), "WORK/snowleopard.qcow2",
              "Made at 40 GB on the first start if it does not exist."),
        Field("SYSDISK_FORMAT", "Disk format", "choice", ("qcow2", "raw"), "qcow2",
              r"raw for a raw image or, on Windows, \\.\PhysicalDriveN."),
        Field("ESP", "Boot volume", "dir", (), "WORK/esp",
              "The OpenCore folder from make-opencore.sh (Tools menu). "
              "sl-vm.sh uses ESP.img when it exists."),
        Field("INSTALL", "Attach installer", "bool", (), "off"),
        Field("INSTALLER", "Installer image", "file", (), "WORK/installer.img",
              "A raw HFS+ disk from make-installer.py, or the DVD .iso."),
    ]),
    ("Integration", [
        Field("AUDIO", "Audio", "choice", ("none", "usb"), "none",
              "usb: a USB audio output that AppleUSBAudio drives."),
        Field("AUDIO_BACKEND", "Audio backend", "choice",
              ("pipewire", "pa", "alsa", "sdl", "coreaudio", "dsound", "wav"), "auto"),
        Field("CLIPBOARD", "Clipboard sync", "bool", (), "off",
              "Needs the guest agent and a gtk or cocoa window. Not on Windows yet."),
        Field("DND", "Drag and drop", "bool", (), "off",
              "Needs the guest agent and a gtk or cocoa window. Not on Windows yet."),
        Field("SSHPORT", "ssh port", "int", (), "2222",
              "Host port (on 127.0.0.1) that forwards to ssh in the guest. "
              "Each VM that runs at the same time needs its own."),
        Field("VNC", "VNC display", "text", (), ":2",
              ":N listens on port 5900+N. Each VM that runs at the same time "
              "needs its own."),
    ]),
    ("Advanced", [
        Field("OSK", "SMC key (OSK)", "secret", (), "from osk.env",
              "Empty: read from WORK/osk.env, then snowleopard/work/osk.env, then "
              "the environment of the GUI."),
        Field("QEMU", "QEMU binary", "file", (), "auto"),
        Field("OVMF_CODE", "OVMF code", "file", (), "auto"),
        Field("OVMF_VARS_TEMPLATE", "OVMF vars template", "file", (), "auto"),
        Field("ROM", "rv530 option ROM", "file", (), "synthetic AtomBIOS"),
        Field("ROM5770", "HD 5770 option ROM", "file", (), "synthetic AtomBIOS"),
        Field("QMP", "QMP socket", "text", (), "WORK/qmp.sock",
              "A socket path, or HOST:PORT for TCP (the default on Windows: "
              "127.0.0.1:4444)."),
        Field("SERIAL", "Serial port", "text", (), "file:WORK/serial.log"),
        Field("QDEBUG", "QEMU -d", "text", (), "guest_errors,unimp"),
        Field("EXTRA", "Extra QEMU args", "text", (), ""),
    ]),
]
FIELDS = {f.key: f for _, group in GROUPS for f in group}
# Variables that the GUI environment may give to every VM when the VM leaves
# them empty.  sl-vm.sh reads the others too, so a value that is exported in
# the shell that started the GUI must not leak into each VM.
HOST_KEYS = {"QEMU", "QEMU_IMG", "OSK", "OVMF_CODE", "OVMF_VARS_TEMPLATE"}
VM_KEYS = (set(FIELDS) | {"WORK", "RP", "DNDFWD", "AUDIO_WAV"}) - HOST_KEYS
DISPLAY_MODES = FIELDS["DISPLAY_MODE"].choices

# make-opencore.sh variables for presets of the new-VM dialog
PRESETS = {
    "Mac OS X 10.6 Snow Leopard": {},
    "Mac OS X 10.7 Lion": {"SL_KCACHE": "Auto"},
    "Mac OS X 10.5 Leopard": {"SL_KARCH": "i386"},
    "Other (Linux guest, ...)": {},
}


def slug(name):
    s = re.sub(r"[^A-Za-z0-9._-]+", "-", name.strip()).strip("-.").lower()
    return s or "vm"


def bash_path(p):
    """Forward slashes on Windows: MSYS2 bash and cygpath take C:/x/y."""
    return p.replace("\\", "/") if IS_WINDOWS else p


class VM:
    def __init__(self, work, name="", env=None, opencore=None):
        self.work = os.path.abspath(work)
        self.name = name or default_name(self.work)
        self.env = {k: str(v) for k, v in (env or {}).items() if str(v) != ""}
        self.opencore = dict(opencore or {})

    @property
    def path(self):
        return os.path.join(self.work, VM_FILE)

    @property
    def saved(self):
        return os.path.isfile(self.path)

    @classmethod
    def load(cls, work):
        try:
            with open(os.path.join(work, VM_FILE), encoding="utf-8") as f:
                d = json.load(f)
        except FileNotFoundError:
            return cls(work)
        except (OSError, ValueError) as e:
            raise VMError("%s: %s" % (os.path.join(work, VM_FILE), e))
        return cls(work, d.get("name", ""), d.get("env"), d.get("opencore"))

    def save(self):
        os.makedirs(self.work, exist_ok=True)
        d = {"name": self.name, "env": {k: v for k, v in self.env.items() if v != ""},
             "opencore": self.opencore}
        tmp = self.path + ".tmp"
        with open(tmp, "w", encoding="utf-8") as f:
            json.dump(d, f, indent=2, sort_keys=True)
            f.write("\n")
        os.replace(tmp, self.path)

    # the value that sl-vm.sh uses when the variable is empty
    def default(self, key):
        w = self.work
        return {
            "SYSDISK": os.path.join(w, "snowleopard.qcow2"),
            "INSTALLER": os.path.join(w, "installer.img"),
            "ESP": os.path.join(w, "esp"),
            "SSHPORT": "2222", "VNC": ":2",
            "QMP": "127.0.0.1:4444" if IS_WINDOWS else os.path.join(w, "qmp.sock"),
            "SERIAL": "file:" + os.path.join(w, "serial.log"),
            "DISPLAY_MODE": "std", "QDISPLAY": "none", "AUDIO": "none",
            "SYSDISK_FORMAT": "qcow2", "SMP": "4",
        }.get(key, "")

    def value(self, key):
        return self.env.get(key) or self.default(key)

    def flag(self, key):
        return self.env.get(key, "") not in ("", "0")

    def qmp_addr(self):
        return self.value("QMP")

    def logs(self):
        """(label, path) of the log files of the VM."""
        out = [("Launcher output", os.path.join(self.work, CONSOLE_LOG)),
               ("QEMU log", os.path.join(self.work, "qemu.log"))]
        ser = self.value("SERIAL")
        if ser.startswith("file:"):
            out.append(("Serial port", ser[5:]))
        return out

    def esp_present(self):
        esp = self.value("ESP")
        return os.path.isdir(esp) or os.path.isfile(esp + ".img")


def default_name(work):
    if os.path.normcase(os.path.abspath(work)) == os.path.normcase(DEFAULT_WORK):
        return "Snow Leopard"
    return os.path.basename(os.path.normpath(work)) or work


class VMError(Exception):
    pass


# ---------------------------------------------------------------- registry

def config_dir():
    if IS_WINDOWS:
        base = os.environ.get("APPDATA") or os.path.expanduser("~")
    elif IS_MAC:
        base = os.path.expanduser("~/Library/Application Support")
    else:
        base = os.environ.get("XDG_CONFIG_HOME") or os.path.expanduser("~/.config")
    return os.path.join(base, "rv530-qemu")


class Registry:
    """The GUI's own settings: VM folders outside snowleopard/work, and the
    folders that the user removed from the list."""

    def __init__(self, path=None):
        self.path = path or os.path.join(config_dir(), "slgui.json")
        self.extra, self.hidden, self.prefs = [], [], {}
        try:
            with open(self.path, encoding="utf-8") as f:
                d = json.load(f)
            self.extra = list(d.get("extra", []))
            self.hidden = list(d.get("hidden", []))
            self.prefs = dict(d.get("prefs", {}))
        except (OSError, ValueError):
            pass

    def save(self):
        os.makedirs(os.path.dirname(self.path), exist_ok=True)
        with open(self.path, "w", encoding="utf-8") as f:
            json.dump({"extra": self.extra, "hidden": self.hidden, "prefs": self.prefs},
                      f, indent=2)
            f.write("\n")

    def add(self, work):
        work = os.path.abspath(work)
        if work in self.hidden:
            self.hidden.remove(work)
        if work not in self.extra and not _auto_dir(work):
            self.extra.append(work)

    def remove(self, work):
        work = os.path.abspath(work)
        if work in self.extra:
            self.extra.remove(work)
        if _auto_dir(work) and work not in self.hidden:
            self.hidden.append(work)


def _same(a, b):
    return os.path.normcase(os.path.realpath(a)) == os.path.normcase(os.path.realpath(b))


def _auto_dir(work):
    """Folders that discover() finds without the registry."""
    return _same(work, DEFAULT_WORK) or _same(os.path.dirname(os.path.abspath(work)), DEFAULT_WORK)


def looks_like_vm(d):
    """A folder with vm.json, a disk, an installer or a boot volume.  (The
    top-level work/ folder also holds the shared OVMF build, fw/, and the
    OpenCore zips, which alone do not make it a VM.)"""
    if not os.path.isdir(d):
        return False
    try:
        names = os.listdir(d)
    except OSError:
        return False
    if VM_FILE in names or "esp" in names or "esp.img" in names or "installer.img" in names:
        return True
    return any(n.endswith(".qcow2") for n in names)


def discover(registry=None, base=None):
    """The known VMs: snowleopard/work itself, its subfolders that look like
    a VM, and the folders of the registry."""
    base = base or DEFAULT_WORK
    cand = []
    if looks_like_vm(base):
        cand.append(base)
    if os.path.isdir(base):
        for n in sorted(os.listdir(base)):
            d = os.path.join(base, n)
            if n not in ("fw",) and not n.startswith(".") and looks_like_vm(d):
                cand.append(d)
    if registry:
        cand += registry.extra
    hidden = [os.path.normcase(os.path.realpath(h)) for h in (registry.hidden if registry else [])]
    seen, vms = set(), []
    for d in cand:
        key = os.path.normcase(os.path.realpath(d))
        if key in seen or key in hidden:
            continue
        seen.add(key)
        try:
            vms.append(VM.load(d))
        except VMError:
            vms.append(VM(d, os.path.basename(d) + " (bad vm.json)"))
    return vms


def free_ports(vms):
    """An SSH port, a VNC display and (Windows) a QMP port that no known VM uses."""
    ssh = {int(v.value("SSHPORT")) for v in vms if v.value("SSHPORT").isdigit()}
    vnc = {vnc_port(v.value("VNC")) for v in vms}
    qmp = {v.qmp_addr() for v in vms}
    p = 2222
    while p in ssh:
        p += 1
    d = 2
    while 5900 + d in vnc:
        d += 1
    q = 4444
    while "127.0.0.1:%d" % q in qmp:
        q += 1
    return str(p), ":%d" % d, "127.0.0.1:%d" % q


def new_vm(name, work, preset, guest_env=None, others=None):
    """A new VM folder with its vm.json; ports that no known VM uses."""
    work = os.path.abspath(work)
    if os.path.isfile(os.path.join(work, VM_FILE)):
        raise VMError("%s already holds a VM" % work)
    if others is None:
        others = discover()
    ssh, vnc, qmp = free_ports(others)
    env = {"DISPLAY_MODE": "std", "INSTALL": "1",
           "QDISPLAY": "cocoa" if IS_MAC else "gtk",
           "SSHPORT": ssh, "VNC": vnc}
    if IS_WINDOWS:
        env["QMP"] = qmp
    env.update(guest_env or {})
    vm = VM(work, name, env, dict(PRESETS.get(preset, {})))
    vm.save()
    return vm


# ---------------------------------------------------------------- the SMC key

def read_osk_env(path):
    """OSK from a shell file such as work/osk.env (`export OSK='...'`)."""
    try:
        with open(path, encoding="utf-8") as f:
            lines = f.readlines()
    except (OSError, UnicodeDecodeError):
        return ""
    for line in lines:
        line = line.strip()
        if line.startswith("export "):
            line = line[7:].strip()
        if not line.startswith("OSK="):
            continue
        try:
            parts = shlex.split(line[4:], comments=True)
        except ValueError:
            continue
        return parts[0] if parts else ""
    return ""


def find_osk(vm, environ=None):
    """(key, where it came from), or ("", "")."""
    environ = os.environ if environ is None else environ
    if vm.env.get("OSK"):
        return vm.env["OSK"], "VM settings"
    for p in (os.path.join(vm.work, "osk.env"), os.path.join(DEFAULT_WORK, "osk.env")):
        k = read_osk_env(p)
        if k:
            return k, p
    if environ.get("OSK"):
        return environ["OSK"], "environment"
    return "", ""


# ---------------------------------------------------------------- binaries

def _exe(p):
    for c in (p, p + ".exe") if IS_WINDOWS else (p,):
        if os.path.isfile(c) and os.access(c, os.X_OK):
            return c
    return ""


def find_qemu(vm, environ=None):
    """The QEMU that sl-vm.sh runs: QEMU, else the build of setup-qemu.sh,
    else bin/ of a release package."""
    environ = os.environ if environ is None else environ
    q = vm.env.get("QEMU") or environ.get("QEMU")
    if q:
        return _exe(q) or (q if os.path.isfile(q) else "")
    return (_exe(os.path.join(REPO, "build-qemu", "qemu-11.1.3", "build", "qemu-system-x86_64"))
            or _exe(os.path.join(REPO, "bin", "qemu-system-x86_64")))


def find_qemu_img(vm, environ=None):
    q = find_qemu(vm, environ)
    return (q and _exe(os.path.join(os.path.dirname(q), "qemu-img"))) or shutil.which("qemu-img") or ""


DISTRO_OVMF = ("/usr/share/OVMF/OVMF_%s_4M.fd", "/usr/share/edk2/x64/OVMF_%s.4m.fd",
               "/usr/share/edk2-ovmf/x64/OVMF_%s.4m.fd")


def find_ovmf(vm, environ=None):
    """(code, vars template, where): the firmware that the VM starts with.

    sl-vm.sh looks for the build of build-ovmf.sh in WORK/fw only.  That
    build (with the GOP driver of the card) is in snowleopard/work/fw, so a
    VM in another folder would get the firmware of the distribution.  The
    GUI then gives sl-vm.sh the shared build (launch_env())."""
    environ = os.environ if environ is None else environ
    code = vm.env.get("OVMF_CODE") or environ.get("OVMF_CODE")
    if code:
        tmpl = vm.env.get("OVMF_VARS_TEMPLATE") or environ.get("OVMF_VARS_TEMPLATE") or ""
        return code, tmpl, "settings"
    for d, where in ((os.path.join(vm.work, "fw"), "VM folder"),
                     (os.path.join(DEFAULT_WORK, "fw"), "shared build"),
                     (os.path.join(REPO, "firmware"), "release package")):
        if os.path.isfile(os.path.join(d, "OVMF_CODE.fd")):
            return os.path.join(d, "OVMF_CODE.fd"), os.path.join(d, "OVMF_VARS.fd"), where
    for pat in DISTRO_OVMF:
        if os.path.isfile(pat % "CODE"):
            return pat % "CODE", pat % "VARS", "distribution (no rv530 GOP driver)"
    return "", "", ""


def find_bash(environ=None):
    """bash for the scripts.  On Windows, the bash of MSYS2: not the WSL
    bash.exe in System32, which runs Linux and cannot run Windows QEMU."""
    environ = os.environ if environ is None else environ
    if not IS_WINDOWS:
        return shutil.which("bash") or ("/bin/bash" if os.path.exists("/bin/bash") else "")
    cand = [environ.get("SLGUI_BASH", "")]
    if environ.get("MSYSTEM"):
        cand.append(shutil.which("bash") or "")
    for root in (environ.get("MSYS2_ROOT", ""), r"C:\msys64", r"C:\tools\msys64"):
        if root:
            cand.append(os.path.join(root, "usr", "bin", "bash.exe"))
    for c in cand:
        if c and os.path.isfile(c) and "system32" not in c.lower():
            return c
    return ""


class LaunchError(Exception):
    pass


def script_argv(script, *args, environ=None):
    bash = find_bash(environ)
    if not bash:
        raise LaunchError("bash not found. On Windows, install MSYS2 (README.md, "
                          "\"Windows hosts\") or set SLGUI_BASH to its usr\\bin\\bash.exe.")
    # MSYS2: a login shell puts the UCRT64 tools (python3, cygpath, mtools)
    # on PATH when the GUI was not started from the UCRT64 shell
    return [bash] + (["-l"] if IS_WINDOWS else []) + [bash_path(script)] + list(args)


def base_env(environ=None):
    environ = os.environ if environ is None else environ
    env = {k: v for k, v in environ.items() if k not in VM_KEYS}
    if IS_WINDOWS:
        env.setdefault("MSYSTEM", "UCRT64")
        env["CHERE_INVOKING"] = "1"         # bash -l stays in the cwd
    return env


def launch_env(vm, environ=None):
    """The environment of sl-vm.sh for this VM."""
    environ = os.environ if environ is None else environ
    env = base_env(environ)
    env.update({k: v for k, v in vm.env.items() if v != ""})
    env["WORK"] = bash_path(vm.work)
    osk, _ = find_osk(vm, environ)
    if osk:
        env["OSK"] = osk
    code, tmpl, where = find_ovmf(vm, environ)
    if where == "shared build":
        env["OVMF_CODE"], env["OVMF_VARS_TEMPLATE"] = bash_path(code), bash_path(tmpl)
    return env


def launch_command(vm):
    """A shell command that starts the VM like the GUI does (without the SMC
    key: it sources osk.env when there is one)."""
    q = shlex.quote
    parts = ["WORK=%s" % q(vm.work)]
    for _, group in GROUPS:
        for f in group:
            v = vm.env.get(f.key, "")
            if v and f.key != "OSK":
                parts.append("%s=%s" % (f.key, q(v)))
    code, tmpl, where = find_ovmf(vm)
    if where == "shared build":
        parts += ["OVMF_CODE=%s" % q(code), "OVMF_VARS_TEMPLATE=%s" % q(tmpl)]
    cmd = " ".join(parts + [q(os.path.join(SL, "sl-vm.sh"))])
    for p in (os.path.join(vm.work, "osk.env"), os.path.join(DEFAULT_WORK, "osk.env")):
        if read_osk_env(p):
            return ". %s && %s" % (q(p), cmd)
    return cmd


def ssh_command(vm):
    return "SSHPORT=%s %s" % (shlex.quote(vm.value("SSHPORT")),
                              shlex.quote(os.path.join(SL, "sl-ssh.sh")))


def vnc_port(vnc):
    """TCP port of a -vnc display (':2' -> 5902), or None."""
    m = re.match(r"^(?:[^:]*):(\d+)", vnc or "")
    return 5900 + int(m.group(1)) if m else None


def port_free(port, host="127.0.0.1"):
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    try:
        s.bind((host, port))
        return True
    except OSError:
        return False
    finally:
        s.close()


# ---------------------------------------------------------------- checks

def preflight(vm, running=(), environ=None, check_ports=True):
    """Problems that stop or spoil a start: a list of (level, message), with
    level "error" (sl-vm.sh or QEMU would fail), "warning" or "info".
    running: the other VMs that run now."""
    environ = os.environ if environ is None else environ
    out = []
    err = lambda m: out.append(("error", m))      # noqa: E731
    warn = lambda m: out.append(("warning", m))   # noqa: E731
    info = lambda m: out.append(("info", m))      # noqa: E731

    if IS_WINDOWS and not find_bash(environ):
        err("bash not found: install MSYS2 (README.md, \"Windows hosts\").")
    qemu = find_qemu(vm, environ)
    if not qemu:
        err("QEMU not found. Run scripts/setup-qemu.sh, use a release package, "
            "or set Advanced > QEMU binary.")
    code, tmpl, where = find_ovmf(vm, environ)
    if not code:
        err("No OVMF firmware. Run Tools > Build OVMF, install the OVMF package, "
            "or set Advanced > OVMF code.")
    elif not os.path.isfile(code):
        err("The OVMF code %s does not exist." % code)
    elif not os.path.isfile(os.path.join(vm.work, "OVMF_VARS.fd")) and not os.path.isfile(tmpl):
        err("No OVMF vars template for the first start: set Advanced > OVMF vars "
            "template too.")
    elif "distribution" in where and vm.value("DISPLAY_MODE") != "std":
        warn("The OVMF of the distribution has no rv530 GOP driver: the screen stays "
             "black until the ATI driver loads. Tools > Build OVMF builds one with it.")
    osk, src = find_osk(vm, environ)
    if not osk:
        err("No SMC key. Set Advanced > SMC key, or put OSK='...' in %s."
            % os.path.join(vm.work, "osk.env"))
    elif src == "VM settings" and _inside(vm.work, REPO) and not _inside(vm.work, DEFAULT_WORK):
        warn("The SMC key is in %s, which is in the repository but not in the ignored "
             "snowleopard/work/. Do not commit it." % vm.path)
    mode = vm.value("DISPLAY_MODE")
    if mode not in DISPLAY_MODES:
        err("Graphics must be one of %s." % ", ".join(DISPLAY_MODES))
    if vm.value("AUDIO") not in ("none", "usb"):
        err("Audio must be none or usb.")
    if not vm.esp_present():
        err("No OpenCore boot volume at %s. Use Tools > Build OpenCore boot volume."
            % vm.value("ESP"))
    elif mode == "hd5770" and not vm.opencore.get("SL_FBNAME"):
        warn("hd5770 needs a boot volume made for the HD 5770 (Tools > Build OpenCore "
             "boot volume, card HD 5770).")
    if vm.flag("INSTALL") and not os.path.isfile(vm.value("INSTALLER")):
        err("Attach installer is on, but %s does not exist. Use Tools > Make installer "
            "disk, or set Storage > Installer image." % vm.value("INSTALLER"))
    disk = vm.value("SYSDISK")
    if not disk.startswith("\\") and not os.path.exists(disk):
        info("%s does not exist: sl-vm.sh makes it (40 GB) at the start." % disk)
    for key in ("MEM", "SSHPORT", "XRES", "YRES", "XMAX", "YMAX", "REFRESH", "TSC_HZ"):
        v = vm.env.get(key, "")
        if v and not v.isdigit():
            err("%s must be a number." % FIELDS[key].label)
    if IS_WINDOWS and (vm.flag("CLIPBOARD") or vm.flag("DND")):
        warn("Clipboard sync and drag and drop do not work on Windows hosts yet; "
             "sl-vm.sh turns them off.")
    if (vm.flag("CLIPBOARD") or vm.flag("DND")) and vm.value("QDISPLAY").split(",")[0] not in ("gtk", "cocoa"):
        warn("Clipboard sync and drag and drop need a gtk or cocoa window.")

    # resources that two VMs cannot share
    for other in running:
        if _same(other.work, vm.work):
            continue
        for what, key in (("ssh port", "SSHPORT"), ("VNC display", "VNC"), ("QMP socket", "QMP")):
            val = other.value(key)
            if val and val != "none" and vm.value(key) == val:
                err("The %s %s is used by the running VM \"%s\"." % (what, val, other.name))
    if check_ports:
        ssh = vm.value("SSHPORT")
        if ssh.isdigit() and not port_free(int(ssh)):
            warn("Port %s (ssh) is in use on this host: QEMU cannot start." % ssh)
        vp = vnc_port(vm.value("VNC"))
        if vp and not port_free(vp):
            warn("Port %d (VNC %s) is in use on this host: QEMU cannot start."
                 % (vp, vm.value("VNC")))
        qa = vm.qmp_addr()
        host, sep, port = qa.rpartition(":")
        if sep and port.isdigit() and "/" not in qa and "\\" not in qa \
                and not port_free(int(port), host or "127.0.0.1"):
            warn("Port %s (QMP) is in use on this host." % port)
    if vm.value("QDISPLAY").split(",")[0] == "none":
        vp = vnc_port(vm.value("VNC"))
        info("No window: connect a VNC viewer to 127.0.0.1:%s." % (vp or "?"))
    return out


def _inside(path, parent):
    path, parent = os.path.realpath(path), os.path.realpath(parent)
    return path == parent or path.startswith(parent + os.sep)


# ---------------------------------------------------------------- QMP

class QMPError(Exception):
    pass


def qmp_socket(addr, timeout):
    """addr is a Unix socket path, or HOST:PORT (TCP), as for slctl.py."""
    host, sep, port = addr.rpartition(":")
    if sep and port.isdigit() and "/" not in addr and "\\" not in addr:
        return socket.create_connection((host or "127.0.0.1", int(port)), timeout=timeout)
    if not hasattr(socket, "AF_UNIX"):
        raise QMPError("this Python has no Unix sockets: set QMP to HOST:PORT")
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(timeout)
    try:
        s.connect(addr)
    except OSError:
        s.close()
        raise
    return s


class QMP:
    """A QMP connection.  QEMU serves one QMP client at a time: keep it
    short, and do not hold two at once to the same VM."""

    def __init__(self, addr, timeout=5.0, greeting_timeout=None, sock=None):
        self.sock = sock or qmp_socket(addr, timeout)
        self.events = []
        try:
            if greeting_timeout is not None:
                self.sock.settimeout(greeting_timeout)
            self.f = self.sock.makefile("rw", encoding="utf-8", newline="\n")
            hello = self._read()
            if "QMP" not in hello:
                raise QMPError("not a QMP greeting: %r" % hello)
            self.sock.settimeout(timeout)
            self.cmd("qmp_capabilities")
        except BaseException:
            self.sock.close()
            raise

    def _read(self):
        line = self.f.readline()
        if not line:
            raise ConnectionError("QMP connection closed")
        return json.loads(line)

    def cmd(self, execute, args=None):
        msg = {"execute": execute}
        if args:
            msg["arguments"] = args
        self.f.write(json.dumps(msg) + "\n")
        self.f.flush()
        while True:
            r = self._read()
            if "event" in r:
                self.events.append(r)
                continue
            if "error" in r:
                raise QMPError(r["error"].get("desc", str(r["error"])))
            return r.get("return")

    def close(self):
        try:
            self.f.close()
        except OSError:
            pass
        self.sock.close()

    def __enter__(self):
        return self

    def __exit__(self, *a):
        self.close()


# the states that probe() returns besides QEMU's own RunState names
STOPPED, BUSY = "stopped", "busy"


def probe(addr, timeout=1.0):
    """The QEMU run state of the VM at addr ("running", "paused", ...),
    STOPPED when nothing listens, BUSY when another client holds QMP."""
    try:
        s = qmp_socket(addr, timeout)
    except (OSError, QMPError):
        return STOPPED
    try:
        with QMP(addr, timeout=timeout, greeting_timeout=timeout, sock=s) as q:
            return q.cmd("query-status").get("status", "running")
    except socket.timeout:
        return BUSY
    except (OSError, ValueError, QMPError):
        return STOPPED


def send_keys(q, names, hold=100):
    q.cmd("send-key", {"keys": [{"type": "qcode", "data": k} for k in names],
                       "hold-time": hold})


def type_text(q, text, hold=60, gap=0.14):
    """Type ASCII text (as slctl.py type): send-key returns before the key
    is released, so the keys are paced or the guest drops some."""
    for ch in text:
        k = KEY_NAMES.get(ch, ch.lower())
        send_keys(q, (["shift"] if ch in KEY_SHIFT else []) + [k], hold)
        time.sleep(gap)


def screendump(q, path):
    q.cmd("screendump", {"filename": os.path.abspath(path), "format": "png"})


def clean_shutdown(q, confirm=True):
    """The Power key brings up 10.6's "shut down your computer now?" dialog,
    and Return confirms it (as slctl.py shutdown --confirm).  The ACPI
    button (system_powerdown) asks 10.6 to sleep, which this VM cannot."""
    send_keys(q, ["power"])
    if confirm:
        time.sleep(3)
        send_keys(q, ["ret"])


# ---------------------------------------------------------------- processes

def spawn(argv, env, cwd, stdout):
    kw = {}
    if IS_WINDOWS:
        kw["creationflags"] = (getattr(subprocess, "CREATE_NEW_PROCESS_GROUP", 0)
                               | getattr(subprocess, "CREATE_NO_WINDOW", 0))
    else:
        # the VM keeps running when the GUI quits, and a Ctrl-C in the
        # terminal of the GUI does not reach it
        kw["start_new_session"] = True
    return subprocess.Popen(argv, env=env, cwd=cwd, stdin=subprocess.DEVNULL,
                            stdout=stdout, stderr=subprocess.STDOUT, **kw)


class Runner:
    """The sl-vm.sh processes that this GUI started.  Their output goes to
    WORK/vm.out, not to a pipe: a VM outlives the GUI, and QEMU would get
    SIGPIPE on a pipe that nobody reads."""

    def __init__(self):
        self.procs = {}             # work -> (Popen, start time)

    def start(self, vm, environ=None):
        if self.alive(vm.work):
            raise LaunchError("%s is already running" % vm.name)
        argv = script_argv(os.path.join(SL, "sl-vm.sh"), environ=environ)
        env = launch_env(vm, environ)
        os.makedirs(vm.work, exist_ok=True)
        log = os.path.join(vm.work, CONSOLE_LOG)
        with open(log, "wb") as out:
            out.write(("# %s: started by slgui.py\n" % time.strftime("%Y-%m-%d %H:%M:%S")).encode())
            out.flush()
            p = spawn(argv, env, SL, out)
        self.procs[vm.work] = (p, time.time())
        return p

    def alive(self, work):
        e = self.procs.get(work)
        return bool(e) and e[0].poll() is None

    def reap(self):
        """[(work, exit code, seconds it ran)] of the processes that ended."""
        out = []
        for work, (p, t0) in list(self.procs.items()):
            rc = p.poll()
            if rc is not None:
                out.append((work, rc, time.time() - t0))
                del self.procs[work]
        return out

    def kill(self, work):
        e = self.procs.get(work)
        if not e or e[0].poll() is not None:
            return False
        p = e[0]
        p.terminate()
        try:
            p.wait(3)
        except subprocess.TimeoutExpired:
            p.kill()
        return True


def tail(path, n=20):
    try:
        with open(path, "rb") as f:
            f.seek(0, 2)
            f.seek(max(0, f.tell() - 8192))
            lines = f.read().decode("utf-8", "replace").splitlines()
    except OSError:
        return ""
    return "\n".join(lines[-n:])


# ---------------------------------------------------------------- tools

def esp_uuid(esp):
    """The SystemUUID of a boot volume that make-opencore.sh made before.
    The guest must keep its UUID when the boot volume is made again."""
    try:
        with open(os.path.join(esp, "EFI", "OC", "config.plist"), "rb") as f:
            return plistlib.load(f)["PlatformInfo"]["Generic"]["SystemUUID"]
    except (OSError, KeyError, ValueError, TypeError, plistlib.InvalidFileException):
        return ""


def opencore_job(vm, opts, zip_path="", environ=None):
    """argv and env of make-opencore.sh for the boot volume of the VM.
    opts: SL_* variables.  Without SL_UUID, the UUID of vm.json or of the
    old boot volume is kept; SL_UUID="" makes a new one."""
    u = opts.get("SL_UUID")
    if u is None:
        u = vm.opencore.get("SL_UUID") or esp_uuid(vm.value("ESP"))
    opts = {k: v for k, v in opts.items() if v not in ("", None)}
    opts["SL_UUID"] = u or str(uuid.uuid4()).upper()
    # make-opencore.sh sets the lock timeouts for the accelerator of
    # host.sh; a VM with its own ACCEL needs the ones of that accelerator
    accel = vm.env.get("ACCEL", "")
    if accel and "SL_TCG" not in opts:
        opts["SL_TCG"] = "1" if accel.startswith("tcg") else "0"
        opts.setdefault("SL_SLTO", "2000000" if accel.startswith("tcg") else "1000000")
    env = base_env(environ)
    env.update(opts)
    argv = script_argv(os.path.join(SL, "make-opencore.sh"),
                       bash_path(zip_path) if zip_path else "", bash_path(vm.value("ESP")),
                       environ=environ)
    return argv, env, opts


def installer_job(src, dst, environ=None):
    return [sys.executable, os.path.join(SL, "make-installer.py"), src, dst], base_env(environ)


def ovmf_job(environ=None):
    return script_argv(os.path.join(SL, "build-ovmf.sh"), environ=environ), base_env(environ)


def create_disk(vm, size_gb, environ=None):
    """Make the system disk now (else sl-vm.sh makes a 40 GB one)."""
    img = find_qemu_img(vm, environ)
    if not img:
        raise LaunchError("qemu-img not found")
    disk = vm.value("SYSDISK")
    if os.path.exists(disk):
        raise LaunchError("%s already exists" % disk)
    r = subprocess.run([img, "create", "-f", vm.value("SYSDISK_FORMAT"), disk, "%dG" % size_gb],
                       capture_output=True, text=True)
    if r.returncode:
        raise LaunchError((r.stderr or r.stdout).strip())


def open_path(target):
    """Open a folder, a file or a URL with the desktop of the host."""
    if IS_WINDOWS:
        os.startfile(target)            # noqa: (Windows only)
    elif IS_MAC:
        subprocess.Popen(["open", target])
    else:
        subprocess.Popen(["xdg-open", target], stdout=subprocess.DEVNULL,
                         stderr=subprocess.DEVNULL)
