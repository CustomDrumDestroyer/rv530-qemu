#!/usr/bin/env python3
"""slgui.py: an experimental GUI to manage the VMs of rv530-qemu.

    python3 snowleopard/gui/slgui.py

It runs on Linux, macOS and Windows (MSYS2) with the Tk of Python and needs
nothing else.  It does not replace the scripts: it starts sl-vm.sh with the
settings of a VM, controls the VM over QMP (as slctl.py), and runs
make-opencore.sh, make-installer.py and build-ovmf.sh.  See README.md.
"""
import math
import os
import queue
import subprocess
import sys
import threading
import time

try:
    import tkinter as tk
    from tkinter import filedialog, messagebox, simpledialog, ttk
    from tkinter import font as tkfont
except ImportError:
    sys.exit("slgui.py needs Tk for Python: python3-tk (Debian, Ubuntu), python3-tkinter "
             "(Fedora), tk (Arch), python-tk (Homebrew), or mingw-w64-ucrt-x86_64-tk (MSYS2).")

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import vmcore as core  # noqa: E402

TITLE = "rv530-qemu VM Manager"
STATUS_TEXT = {"stopped": "Stopped", "starting": "Starting", "running": "Running",
               "paused": "Paused", "busy": "Running (QMP in use)",
               "shutdown": "Shut down", "guest-panicked": "Guest panicked",
               "internal-error": "Internal error", "io-error": "I/O error"}
POLL_S = 2.0           # status poll interval
LIVE_S = 3.0           # live preview interval
PREVIEW_MAX = (560, 350)
LOG_BACKLOG = 256 * 1024
LOG_MAX_LINES = 20000


def active(st):
    return st != core.STOPPED


class App:
    def __init__(self, root):
        self.root = root
        self.reg = core.Registry()
        self.runner = core.Runner()
        self.q = queue.Queue()
        self.vms = []
        self.status = {}              # work -> QMP state (from the poller)
        self.locks = {}               # work -> Lock: one QMP client at a time
        self.cur = None               # work of the selected VM
        self.loading = False
        self.dirty = False
        self.closing = False
        self.selecting = False
        self.vars = {}
        self.preview_busy = False
        self.preview_at = 0.0
        self.preview_img = None
        self.log_path, self.log_off, self.log_at = None, 0, 0.0
        self.tasks = {}
        self.host_accel = core.host_accel()

        root.title(TITLE + " (experimental)")
        root.geometry(self.reg.prefs.get("geometry", "1180x760"))
        root.minsize(900, 560)
        style = ttk.Style()
        if root.tk.call("tk", "windowingsystem") == "x11" and "clam" in style.theme_names():
            style.theme_use("clam")
        head = tkfont.nametofont("TkDefaultFont").copy()
        head.configure(size=max(14, head.cget("size") + 4), weight="bold")
        self.head_font = head
        style.configure("Head.TLabel", font=head)
        style.configure("Help.TLabel", foreground="#666666")
        self.build_menu()
        self.build_body()
        root.protocol("WM_DELETE_WINDOW", self.quit)
        self.refresh_list(select=self.reg.prefs.get("selected"))
        threading.Thread(target=self.poll_loop, daemon=True).start()
        root.after(100, self.pump)

    # ------------------------------------------------------------ layout

    def build_menu(self):
        m = tk.Menu(self.root)
        self.menus = {}

        def menu(label, items):
            sub = tk.Menu(m, tearoff=0)
            for it in items:
                if it is None:
                    sub.add_separator()
                else:
                    sub.add_command(label=it[0], command=it[1])
            m.add_cascade(label=label, menu=sub)
            self.menus[label] = sub

        menu("VM", [("New VM...", self.new_vm), ("Add Existing Folder...", self.add_folder),
                    ("Remove from List", self.remove_vm), None,
                    ("Open VM Folder", self.open_folder),
                    ("Copy Launch Command", self.copy_launch),
                    ("Copy ssh Command", self.copy_ssh), None, ("Quit", self.quit)])
        menu("Machine", [("Start", self.start), ("Shut Down", self.shutdown),
                         ("Pause", self.pause), ("Resume", self.resume),
                         ("Reset", self.reset), ("Force Stop", self.force_stop), None,
                         ("Save Screenshot...", self.screenshot),
                         ("Send Power Key", self.power_key), ("Send Keys...", self.send_keys),
                         ("Type Text...", self.type_text), None,
                         ("Open VNC Viewer", self.open_vnc)])
        menu("Tools", [("Build OpenCore Boot Volume...", self.build_opencore),
                       ("Make Installer Disk...", self.make_installer),
                       ("Create System Disk...", self.create_disk), None,
                       ("Build OVMF...", self.build_ovmf)])
        menu("Help", [("About", self.about), ("Open GUI README", lambda: self.open_doc(core.HERE)),
                      ("Open Snow Leopard README", lambda: self.open_doc(core.SL))])
        self.root.configure(menu=m)

    def build_body(self):
        bar = ttk.Frame(self.root, padding=(6, 6, 6, 0))
        bar.pack(fill="x")
        self.btn = {}
        for key, label, cmd in (("new", "New VM", self.new_vm), (None, None, None),
                                ("start", "Start", self.start),
                                ("shutdown", "Shut Down", self.shutdown),
                                ("pause", "Pause", self.toggle_pause),
                                ("reset", "Reset", self.reset),
                                ("stop", "Force Stop", self.force_stop), (None, None, None),
                                ("shot", "Screenshot", self.screenshot)):
            if key is None:
                ttk.Separator(bar, orient="vertical").pack(side="left", fill="y", padx=6)
                continue
            b = ttk.Button(bar, text=label, command=cmd)
            b.pack(side="left", padx=2)
            self.btn[key] = b

        self.statusbar = ttk.Label(self.root, anchor="w", padding=(8, 2))
        self.statusbar.pack(side="bottom", fill="x")

        pane = ttk.Panedwindow(self.root, orient="horizontal")
        pane.pack(fill="both", expand=True, padx=6, pady=6)
        left = ttk.Frame(pane)
        self.tree = ttk.Treeview(left, columns=("status",), selectmode="browse")
        self.tree.heading("#0", text="VM")
        self.tree.heading("status", text="Status")
        self.tree.column("#0", width=170)
        self.tree.column("status", width=120)
        self.tree.pack(fill="both", expand=True)
        self.tree.bind("<<TreeviewSelect>>", self.on_select)
        pane.add(left, weight=0)

        self.nb = ttk.Notebook(pane)
        pane.add(self.nb, weight=1)
        self.build_overview()
        self.build_settings()
        self.build_logs()

    def build_overview(self):
        f = ttk.Frame(self.nb, padding=10)
        self.nb.add(f, text="Overview")
        head = ttk.Frame(f)
        head.pack(fill="x")
        self.h_name = ttk.Label(head, style="Head.TLabel")
        self.h_name.pack(side="left")
        self.h_status = ttk.Label(head, padding=(12, 0))
        self.h_status.pack(side="left")

        mid = ttk.Frame(f)
        mid.pack(fill="both", expand=True, pady=(8, 0))
        info = ttk.Frame(mid)
        info.pack(side="left", fill="y", anchor="n")
        self.info = {}
        for i, (key, label) in enumerate((("folder", "Folder"), ("DISPLAY_MODE", "Graphics"),
                                          ("QDISPLAY", "Window"), ("MEM", "Memory (MB)"),
                                          ("SMP", "vCPUs"), ("ACCEL", "Accelerator"),
                                          ("SSHPORT", "ssh port"), ("VNC", "VNC"),
                                          ("QMP", "QMP"), ("ESP", "Boot volume"),
                                          ("osk", "SMC key"), ("firmware", "Firmware"))):
            ttk.Label(info, text=label + ":").grid(row=i, column=0, sticky="nw", padx=(0, 8), pady=1)
            v = ttk.Label(info, wraplength=300, justify="left")
            v.grid(row=i, column=1, sticky="nw", pady=1)
            self.info[key] = v

        scr = ttk.LabelFrame(mid, text="Screen", padding=6)
        scr.pack(side="right", fill="both", expand=True, padx=(12, 0))
        self.preview = ttk.Label(scr, anchor="center", text="(no preview)")
        self.preview.pack(fill="both", expand=True)
        row = ttk.Frame(scr)
        row.pack(fill="x")
        ttk.Button(row, text="Refresh", command=self.request_preview).pack(side="left")
        self.live = tk.BooleanVar(value=False)
        ttk.Checkbutton(row, text="Live (every %d s)" % LIVE_S, variable=self.live).pack(
            side="left", padx=8)

        chk = ttk.LabelFrame(f, text="Checks", padding=6)
        chk.pack(fill="x", pady=(10, 0))
        self.checks = tk.Text(chk, height=7, wrap="word", relief="flat",
                              background=ttk.Style().lookup("TFrame", "background") or "white",
                              font="TkDefaultFont")
        self.checks.pack(fill="x")
        self.checks.tag_configure("error", foreground="#c62828")
        self.checks.tag_configure("warning", foreground="#b26a00")
        self.checks.tag_configure("info", foreground="#555555")
        self.checks.tag_configure("ok", foreground="#2e7d32")
        ttk.Button(chk, text="Check Again", command=self.show_checks).pack(anchor="e")

    def build_settings(self):
        f = ttk.Frame(self.nb, padding=10)
        self.nb.add(f, text="Settings")
        top = ttk.Frame(f)
        top.pack(fill="x")
        ttk.Label(top, text="Name:").pack(side="left")
        self.v_name = tk.StringVar()
        self.v_name.trace_add("write", self.on_change)
        ttk.Entry(top, textvariable=self.v_name, width=30).pack(side="left", padx=6)
        self.dirty_lbl = ttk.Label(top, foreground="#b26a00")
        self.dirty_lbl.pack(side="left", padx=10)
        ttk.Button(top, text="Revert", command=self.revert).pack(side="right")
        ttk.Button(top, text="Save", command=self.save).pack(side="right", padx=4)

        groups = ttk.Notebook(f)
        groups.pack(fill="both", expand=True, pady=(8, 0))
        for gname, fields in core.GROUPS:
            g = ttk.Frame(groups, padding=8)
            groups.add(g, text=gname)
            g.columnconfigure(3, weight=1)
            for r, fd in enumerate(fields):
                var = tk.StringVar()
                var.trace_add("write", self.on_change)
                self.vars[fd.key] = var
                ttk.Label(g, text=fd.label + ":").grid(row=r, column=0, sticky="ne", padx=(0, 6), pady=3)
                if fd.kind == "bool":
                    w = ttk.Checkbutton(g, variable=var, onvalue="1", offvalue="")
                elif fd.kind == "choice":
                    w = ttk.Combobox(g, textvariable=var, values=fd.choices, width=34)
                else:
                    w = ttk.Entry(g, textvariable=var, width=36,
                                  show="*" if fd.kind == "secret" else "")
                w.grid(row=r, column=1, sticky="nw" if fd.kind == "bool" else "new", pady=3)
                if fd.kind in ("file", "dir"):
                    ttk.Button(g, text="...", width=3,
                               command=lambda fd=fd: self.browse(fd)).grid(row=r, column=2, padx=2)
                elif fd.kind == "secret":
                    sv = tk.BooleanVar(value=False)
                    ttk.Checkbutton(g, text="Show", variable=sv,
                                    command=lambda w=w, sv=sv: w.configure(show="" if sv.get() else "*")
                                    ).grid(row=r, column=2, padx=2)
                hint = fd.help
                if fd.default:
                    hint = ("Default: %s. " % fd.default) + hint
                ttk.Label(g, text=hint, style="Help.TLabel", wraplength=380,
                          justify="left").grid(row=r, column=3, sticky="nw", padx=(8, 0), pady=3)

    def build_logs(self):
        f = ttk.Frame(self.nb, padding=10)
        self.nb.add(f, text="Logs")
        top = ttk.Frame(f)
        top.pack(fill="x")
        self.v_log = tk.StringVar()
        self.log_box = ttk.Combobox(top, textvariable=self.v_log, state="readonly", width=24)
        self.log_box.pack(side="left")
        self.log_box.bind("<<ComboboxSelected>>", lambda e: self.open_log())
        self.follow = tk.BooleanVar(value=True)
        ttk.Checkbutton(top, text="Follow", variable=self.follow).pack(side="left", padx=8)
        self.log_file_lbl = ttk.Label(top, style="Help.TLabel")
        self.log_file_lbl.pack(side="left", padx=8)
        ttk.Button(top, text="Open File", command=lambda: self.log_path and core.open_path(self.log_path)
                   ).pack(side="right")
        body = ttk.Frame(f)
        body.pack(fill="both", expand=True, pady=(6, 0))
        self.log = tk.Text(body, wrap="none", font="TkFixedFont", state="disabled")
        ys = ttk.Scrollbar(body, orient="vertical", command=self.log.yview)
        xs = ttk.Scrollbar(body, orient="horizontal", command=self.log.xview)
        self.log.configure(yscrollcommand=ys.set, xscrollcommand=xs.set)
        ys.pack(side="right", fill="y")
        xs.pack(side="bottom", fill="x")
        self.log.pack(fill="both", expand=True)

    # ------------------------------------------------------------ VM list

    def vm(self, work=None):
        work = work or self.cur
        for v in self.vms:
            if v.work == work:
                return v
        return None

    def eff_status(self, work):
        st = self.status.get(work, core.STOPPED)
        if st == core.STOPPED and self.runner.alive(work):
            return "starting"
        return st

    def refresh_list(self, select=None):
        self.vms = core.discover(self.reg)
        self.tree.delete(*self.tree.get_children())
        for v in self.vms:
            self.tree.insert("", "end", iid=v.work, text=v.name,
                             values=(STATUS_TEXT.get(self.eff_status(v.work), "?"),))
        target = select if self.vm(select) else (self.vms[0].work if self.vms else None)
        if target:
            self.selecting = True
            self.tree.selection_set(target)
            self.tree.see(target)
            self.selecting = False
        self.load(target)

    def update_row(self, work):
        if self.tree.exists(work):
            st = self.eff_status(work)
            self.tree.set(work, "status", STATUS_TEXT.get(st, st))
        if work == self.cur:
            self.show_status()

    def on_select(self, _=None):
        if self.selecting:
            return
        sel = self.tree.selection()
        work = sel[0] if sel else None
        if work == self.cur:
            return
        if self.dirty and self.vm():
            ans = messagebox.askyesnocancel(TITLE, "Save the changes to \"%s\"?" % self.vm().name)
            if ans is None:
                self.selecting = True
                self.tree.selection_set(self.cur)
                self.selecting = False
                return
            if ans and not self.save():
                return
        self.load(work)

    def load(self, work):
        """Show a VM in the three tabs."""
        self.cur = work
        vm = self.vm()
        self.loading = True
        self.v_name.set(vm.name if vm else "")
        for k, var in self.vars.items():
            v = vm.env.get(k, "") if vm else ""
            if core.FIELDS[k].kind == "bool":
                v = "1" if v not in ("", "0") else ""
            var.set(v)
        self.loading = False
        self.set_dirty(False)
        self.preview_img = None
        self.preview.configure(image="", text="(no preview)")
        self.show_info()
        self.show_status()
        self.show_checks()
        logs = vm.logs() if vm else []
        self.log_box.configure(values=[l for l, _ in logs])
        self.v_log.set(logs[0][0] if logs else "")
        self.open_log()

    # ------------------------------------------------------------ overview

    def show_info(self):
        vm = self.vm()
        if not vm:
            self.h_name.configure(text="No VMs yet")
            for v in self.info.values():
                v.configure(text="")
            self.info["folder"].configure(
                text="VM > New VM makes one. VM > Add Existing Folder adds a folder that "
                     "sl-vm.sh used (its WORK).")
            return
        self.h_name.configure(text=vm.name)
        vals = {k: vm.value(k) for k in ("DISPLAY_MODE", "QDISPLAY", "SMP", "SSHPORT", "QMP", "ESP")}
        vals["folder"] = vm.work
        vals["MEM"] = vm.env.get("MEM") or ("2048" if self._tcg(vm) else "4096")
        vals["ACCEL"] = vm.env.get("ACCEL") or "auto (%s)" % self.host_accel
        port = core.vnc_port(vm.value("VNC"))
        vals["VNC"] = "%s (127.0.0.1:%s)" % (vm.value("VNC"), port) if port else vm.value("VNC")
        if not vm.esp_present():
            vals["ESP"] += " (missing)"
        osk, src = core.find_osk(vm)
        vals["osk"] = ("from " + src) if osk else "missing"
        code, _, where = core.find_ovmf(vm)
        vals["firmware"] = ("%s (%s)" % (code, where)) if code else "missing"
        for k, w in self.info.items():
            w.configure(text=vals.get(k, ""))

    def _tcg(self, vm):
        return (vm.env.get("ACCEL") or self.host_accel).startswith("tcg")

    def show_status(self):
        vm = self.vm()
        st = self.eff_status(self.cur) if vm else None
        self.h_status.configure(text=("[%s]" % STATUS_TEXT.get(st, st)) if vm else "")
        on = vm is not None and active(st)
        qmp_ok = on and st != "starting"

        def state(cond):
            return "normal" if cond else "disabled"
        self.btn["start"].configure(state=state(vm is not None and not on))
        for k in ("shutdown", "reset", "shot"):
            self.btn[k].configure(state=state(qmp_ok))
        self.btn["stop"].configure(state=state(on))
        self.btn["pause"].configure(state=state(qmp_ok),
                                    text="Resume" if st == "paused" else "Pause")
        mm = self.menus["Machine"]
        for label, cond in (("Start", vm is not None and not on), ("Shut Down", qmp_ok),
                            ("Pause", qmp_ok and st != "paused"), ("Resume", st == "paused"),
                            ("Reset", qmp_ok), ("Force Stop", on),
                            ("Save Screenshot...", qmp_ok), ("Send Power Key", qmp_ok),
                            ("Send Keys...", qmp_ok), ("Type Text...", qmp_ok),
                            ("Open VNC Viewer", qmp_ok)):
            mm.entryconfigure(label, state=state(cond))
        for menu, labels in (("VM", ("Remove from List", "Open VM Folder", "Copy Launch Command",
                                     "Copy ssh Command")),
                             ("Tools", ("Build OpenCore Boot Volume...", "Make Installer Disk...",
                                        "Create System Disk..."))):
            for label in labels:
                self.menus[menu].entryconfigure(label, state=state(vm is not None))

    def running_others(self, vm):
        return [v for v in self.vms if v.work != vm.work and active(self.eff_status(v.work))]

    def show_checks(self):
        vm = self.vm()
        t = self.checks
        t.configure(state="normal")
        t.delete("1.0", "end")
        if vm:
            on = active(self.eff_status(vm.work))
            issues = core.preflight(vm, self.running_others(vm), check_ports=not on)
            if on:
                t.insert("end", "The VM runs. The checks below are for its next start.\n", "ok")
            elif not any(l in ("error", "warning") for l, _ in issues):
                t.insert("end", "Ready to start.\n", "ok")
            for level, msg in issues:
                t.insert("end", "%s: %s\n" % (level.capitalize(), msg), level)
        t.configure(state="disabled")

    def request_preview(self):
        vm = self.vm()
        if not vm or self.preview_busy or self.eff_status(vm.work) in (core.STOPPED, "starting"):
            return
        self.preview_busy = True
        self.preview_at = time.time()
        path = os.path.join(vm.work, "slgui-preview.png")

        def run(q):
            core.screendump(q, path)
            return path
        self.qmp_action(vm, "Preview", run, kind="preview", quiet=True)

    def show_preview(self, work, path, err):
        self.preview_busy = False
        if work != self.cur:
            return
        if err:
            self.preview.configure(image="", text="(no preview: %s)" % err)
            return
        try:
            img = tk.PhotoImage(file=path)
        except tk.TclError as e:
            self.preview.configure(image="", text="(no preview: %s)" % e)
            return
        f = max(1, math.ceil(img.width() / PREVIEW_MAX[0]), math.ceil(img.height() / PREVIEW_MAX[1]))
        if f > 1:
            img = img.subsample(f)
        self.preview_img = img
        self.preview.configure(image=img, text="")

    # ------------------------------------------------------------ settings

    def on_change(self, *_):
        if not self.loading and self.vm():
            self.set_dirty(True)

    def set_dirty(self, d):
        self.dirty = d
        self.dirty_lbl.configure(text="Unsaved changes" if d else "")

    def browse(self, fd):
        vm = self.vm()
        start = os.path.dirname(self.vars[fd.key].get()) or (vm.work if vm else core.SL)
        if fd.kind == "dir":
            p = filedialog.askdirectory(parent=self.root, initialdir=start, title=fd.label)
        else:
            p = filedialog.askopenfilename(parent=self.root, initialdir=start, title=fd.label)
        if p:
            self.vars[fd.key].set(p)

    def save(self):
        vm = self.vm()
        if not vm:
            return False
        env = {k: v for k, v in vm.env.items() if k not in core.FIELDS}
        env.update({k: var.get().strip() for k, var in self.vars.items() if var.get().strip()})
        vm.name = self.v_name.get().strip() or core.default_name(vm.work)
        vm.env = env
        try:
            vm.save()
        except OSError as e:
            messagebox.showerror(TITLE, "Cannot save %s:\n%s" % (vm.path, e))
            return False
        self.set_dirty(False)
        self.tree.item(vm.work, text=vm.name)
        self.show_info()
        self.show_checks()
        self.say("Saved %s" % vm.path)
        return True

    def revert(self):
        work = self.cur
        try:
            fresh = core.VM.load(work)
        except core.VMError as e:
            messagebox.showerror(TITLE, str(e))
            return
        self.vms = [fresh if v.work == work else v for v in self.vms]
        self.load(work)

    def ensure_saved(self):
        if not self.dirty:
            return True
        ans = messagebox.askyesnocancel(TITLE, "Save the changes first?")
        if ans is None:
            return False
        return self.save() if ans else (self.revert() or True)

    # ------------------------------------------------------------ logs

    def open_log(self):
        vm = self.vm()
        path = dict(vm.logs()).get(self.v_log.get()) if vm else None
        self.log_path, self.log_off = path, 0
        self.log_file_lbl.configure(text=path or "")
        self.log.configure(state="normal")
        self.log.delete("1.0", "end")
        self.log.configure(state="disabled")
        self.read_log()

    def read_log(self):
        p = self.log_path
        if not p:
            return
        try:
            size = os.path.getsize(p)
        except OSError:
            return
        if size < self.log_off:             # a new start truncated it
            self.open_log()
            return
        if size == self.log_off:
            return
        start = self.log_off or max(0, size - LOG_BACKLOG)
        try:
            with open(p, "rb") as f:
                f.seek(start)
                data = f.read(size - start)
        except OSError:
            return
        self.log_off = size
        at_end = self.log.yview()[1] >= 0.999
        self.log.configure(state="normal")
        self.log.insert("end", data.decode("utf-8", "replace"))
        n = int(self.log.index("end-1c").split(".")[0])
        if n > LOG_MAX_LINES:
            self.log.delete("1.0", "%d.0" % (n - LOG_MAX_LINES))
        self.log.configure(state="disabled")
        if self.follow.get() and at_end:
            self.log.see("end")

    def show_log(self, label):
        self.v_log.set(label)
        self.open_log()
        self.nb.select(2)

    # ------------------------------------------------------------ background

    def lock(self, work):
        return self.locks.setdefault(work, threading.Lock())

    def poll_loop(self):
        while not self.closing:
            for vm in list(self.vms):
                lk = self.lock(vm.work)
                if not lk.acquire(blocking=False):
                    continue                # an action holds QMP
                try:
                    st = core.probe(vm.qmp_addr())
                finally:
                    lk.release()
                self.q.put(("status", vm.work, st))
            self.q.put(("tick",))
            time.sleep(POLL_S)

    def qmp_action(self, vm, label, fn, kind="done", quiet=False):
        """Run fn(QMP connection) in a thread; the result comes back in pump()."""
        addr, work = vm.qmp_addr(), vm.work
        if not quiet:
            self.say("%s: %s..." % (vm.name, label))

        def run():
            with self.lock(work):
                try:
                    with core.QMP(addr, timeout=15) as q:
                        res = fn(q)
                    self.q.put((kind, work, label, None, res))
                except Exception as e:      # noqa: BLE001 (shown to the user)
                    self.q.put((kind, work, label, str(e) or e.__class__.__name__, None))
        threading.Thread(target=run, daemon=True).start()

    def pump(self):
        try:
            while True:
                msg = self.q.get_nowait()
                kind = msg[0]
                if kind == "status":
                    _, work, st = msg
                    if self.status.get(work) != st:
                        self.status[work] = st
                        self.update_row(work)
                        if work == self.cur:
                            self.show_checks()
                elif kind == "tick":
                    self.reap()
                elif kind == "done":
                    _, work, label, err, res = msg
                    vm = self.vm(work)
                    name = vm.name if vm else work
                    if err:
                        self.say("%s: %s failed" % (name, label))
                        messagebox.showerror(TITLE, "%s: %s failed:\n%s" % (name, label, err))
                    else:
                        self.say("%s: %s done%s" % (name, label, (": %s" % res) if res else ""))
                elif kind == "preview":
                    _, work, _, err, res = msg
                    self.show_preview(work, res, err)
                elif kind in ("task", "taskend"):
                    t = self.tasks.get(msg[1])
                    if t:
                        t.handle(msg)
        except queue.Empty:
            pass
        now = time.time()
        tab = self.nb.index("current") if self.nb.tabs() else 0
        if tab == 2 and now - self.log_at > 0.5:
            self.log_at = now
            self.read_log()
        if self.live.get() and tab == 0 and now - self.preview_at > LIVE_S:
            self.request_preview()
        if not self.closing:
            self.root.after(100, self.pump)

    def reap(self):
        for work, rc, ran in self.runner.reap():
            vm = self.vm(work)
            name = vm.name if vm else work
            self.update_row(work)
            if rc and ran < 30:
                messagebox.showerror(TITLE, "%s stopped at the start (exit code %d):\n\n%s"
                                     % (name, rc, core.tail(os.path.join(work, core.CONSOLE_LOG), 15)))
                if work == self.cur:
                    self.show_log("Launcher output")
            else:
                self.say("%s exited (code %d)" % (name, rc))

    def say(self, text):
        self.statusbar.configure(text=text)

    # ------------------------------------------------------------ machine

    def start(self):
        vm = self.vm()
        if not vm or active(self.eff_status(vm.work)) or not self.ensure_saved():
            return
        vm = self.vm()
        issues = core.preflight(vm, self.running_others(vm))
        errors = [m for l, m in issues if l == "error"]
        warns = [m for l, m in issues if l == "warning"]
        if errors:
            self.show_checks()
            messagebox.showerror(TITLE, "%s cannot start:\n\n%s" % (vm.name, "\n\n".join(errors)))
            return
        if warns and not messagebox.askyesno(TITLE, "\n\n".join(warns) + "\n\nStart anyway?"):
            return
        try:
            self.runner.start(vm)
        except (core.LaunchError, OSError) as e:
            messagebox.showerror(TITLE, "Cannot start %s:\n%s" % (vm.name, e))
            return
        self.say("%s: starting" % vm.name)
        self.update_row(vm.work)
        self.show_log("Launcher output")

    def shutdown(self):
        vm = self.vm()
        if vm and messagebox.askokcancel(
                TITLE, "Shut down \"%s\"?\n\nThe GUI presses the Power key of the guest and "
                "confirms the dialog of Mac OS X with Return. An app with unsaved "
                "documents can stop the shutdown." % vm.name):
            self.qmp_action(vm, "Shut down", lambda q: core.clean_shutdown(q, True))

    def power_key(self):
        vm = self.vm()
        if vm:
            self.qmp_action(vm, "Power key", lambda q: core.send_keys(q, ["power"]))

    def toggle_pause(self):
        (self.resume if self.eff_status(self.cur) == "paused" else self.pause)()

    def pause(self):
        vm = self.vm()
        if vm:
            self.qmp_action(vm, "Pause", lambda q: q.cmd("stop"))
            self.status[vm.work] = "paused"
            self.update_row(vm.work)

    def resume(self):
        vm = self.vm()
        if vm:
            self.qmp_action(vm, "Resume", lambda q: q.cmd("cont"))
            self.status[vm.work] = "running"
            self.update_row(vm.work)

    def reset(self):
        vm = self.vm()
        if vm and messagebox.askokcancel(TITLE, "Reset \"%s\"? The guest restarts at once, "
                                         "without a shutdown." % vm.name, icon="warning"):
            self.qmp_action(vm, "Reset", lambda q: q.cmd("system_reset"))

    def force_stop(self):
        vm = self.vm()
        if not vm or not messagebox.askokcancel(
                TITLE, "Force \"%s\" to stop?\n\nThis is like pulling the power plug: the "
                "guest can lose data. Use Shut Down when you can." % vm.name, icon="warning"):
            return
        work, addr = vm.work, vm.qmp_addr()

        def run():
            try:
                with self.lock(work), core.QMP(addr, timeout=5) as q:
                    q.cmd("quit")
                self.q.put(("done", work, "Force stop", None, None))
            except Exception as e:          # noqa: BLE001
                # no QMP: kill the process if this GUI started it
                if self.runner.kill(work):
                    self.q.put(("done", work, "Force stop", None, "killed"))
                else:
                    self.q.put(("done", work, "Force stop", str(e), None))
        threading.Thread(target=run, daemon=True).start()

    def screenshot(self):
        vm = self.vm()
        if not vm:
            return
        p = filedialog.asksaveasfilename(
            parent=self.root, title="Save Screenshot", defaultextension=".png",
            initialdir=vm.work, filetypes=[("PNG image", "*.png")],
            initialfile="%s-%s.png" % (core.slug(vm.name), time.strftime("%Y%m%d-%H%M%S")))
        if p:
            self.qmp_action(vm, "Screenshot", lambda q: (core.screendump(q, p), p)[1])

    def send_keys(self):
        vm = self.vm()
        if not vm:
            return
        s = simpledialog.askstring(TITLE, "Keys to press together, as QEMU qcode names\n"
                                   "(for example: meta_l q, or ctrl alt delete):", parent=self.root)
        if s and s.strip():
            names = s.replace("+", " ").split()
            self.qmp_action(vm, "Send keys", lambda q: core.send_keys(q, names))

    def type_text(self):
        vm = self.vm()
        if not vm:
            return
        s = simpledialog.askstring(TITLE, "ASCII text to type into the guest\n(US keyboard layout):",
                                   parent=self.root)
        if s:
            self.qmp_action(vm, "Type text", lambda q: core.type_text(q, s))

    def open_vnc(self):
        vm = self.vm()
        port = core.vnc_port(vm.value("VNC")) if vm else None
        if not port:
            messagebox.showinfo(TITLE, "This VM has no VNC display.")
            return
        url = "vnc://127.0.0.1:%d" % port
        if core.IS_MAC:
            core.open_path(url)             # Screen Sharing
        else:
            self.copy("127.0.0.1:%d" % port)
            messagebox.showinfo(TITLE, "Connect a VNC viewer to 127.0.0.1:%d.\n"
                                "(The address is on the clipboard.)" % port)

    # ------------------------------------------------------------ VM menu

    def new_vm(self):
        if not self.ensure_saved():
            return
        d = NewVMDialog(self.root, self.vms)
        if not d.result:
            return
        name, work, preset, size, mkdisk = d.result
        try:
            vm = core.new_vm(name, work, preset, others=self.vms)
            if mkdisk:
                core.create_disk(vm, size)
        except (core.VMError, core.LaunchError, OSError) as e:
            messagebox.showerror(TITLE, "Cannot make the VM:\n%s" % e)
            return
        self.reg.add(vm.work)
        self.save_reg()
        self.refresh_list(select=vm.work)
        self.say("Made %s. Next: Tools > Build OpenCore Boot Volume, and an installer." % vm.work)

    def add_folder(self):
        if not self.ensure_saved():
            return
        p = filedialog.askdirectory(parent=self.root, initialdir=core.DEFAULT_WORK,
                                    title="A VM folder (the WORK of sl-vm.sh)")
        if not p:
            return
        if not core.looks_like_vm(p) and not messagebox.askyesno(
                TITLE, "%s has no vm.json, disk or boot volume. Add it anyway?" % p):
            return
        self.reg.add(p)
        self.save_reg()
        self.refresh_list(select=os.path.abspath(p))

    def remove_vm(self):
        vm = self.vm()
        if vm and messagebox.askokcancel(
                TITLE, "Remove \"%s\" from the list?\n\nThe GUI does not delete anything: the "
                "files stay in %s, and VM > Add Existing Folder adds it again." % (vm.name, vm.work)):
            self.reg.remove(vm.work)
            self.save_reg()
            self.dirty = False
            self.refresh_list()

    def open_folder(self):
        vm = self.vm()
        if vm:
            core.open_path(vm.work)

    def open_doc(self, d):
        core.open_path(os.path.join(d, "README.md"))

    def copy(self, text):
        self.root.clipboard_clear()
        self.root.clipboard_append(text)

    def copy_launch(self):
        vm = self.vm()
        if vm:
            self.copy(core.launch_command(vm))
            self.say("Copied the launch command of %s" % vm.name)

    def copy_ssh(self):
        vm = self.vm()
        if vm:
            self.copy(core.ssh_command(vm))
            self.say("Copied the ssh command of %s" % vm.name)

    def save_reg(self):
        try:
            self.reg.save()
        except OSError as e:
            messagebox.showerror(TITLE, "Cannot save %s:\n%s" % (self.reg.path, e))

    # ------------------------------------------------------------ tools

    def run_task(self, title, argv, env, cwd=None, on_ok=None):
        t = TaskWindow(self, title, argv, env, cwd or core.SL, on_ok)
        self.tasks[t.id] = t

    def build_opencore(self):
        vm = self.vm()
        if not vm or not self.ensure_saved():
            return
        vm = self.vm()
        d = OpenCoreDialog(self.root, vm)
        if not d.result:
            return
        opts, zip_path = d.result
        try:
            argv, env, opts = core.opencore_job(vm, opts, zip_path)
        except core.LaunchError as e:
            messagebox.showerror(TITLE, str(e))
            return

        def ok():
            keep = {k: v for k, v in opts.items() if k not in ("SL_PICKER", "SL_VERBOSE")}
            vm.opencore = keep
            vm.save()
            self.show_info()
            self.show_checks()
        self.run_task("OpenCore boot volume: %s" % vm.name, argv, env, on_ok=ok)

    def make_installer(self):
        vm = self.vm()
        if not vm or not self.ensure_saved():
            return
        vm = self.vm()
        src = filedialog.askopenfilename(
            parent=self.root, title="Install DVD image (.dmg, .iso or raw)",
            filetypes=[("Disk images", "*.dmg *.iso *.img *.cdr"), ("All files", "*")])
        if not src:
            return
        dst = vm.value("INSTALLER")
        if dst.endswith(".iso"):
            dst = os.path.join(vm.work, "installer.img")
        argv, env = core.installer_job(src, dst)

        def ok():
            if os.path.abspath(dst) != os.path.abspath(vm.default("INSTALLER")):
                vm.env["INSTALLER"] = dst
            vm.env["INSTALL"] = "1"
            vm.save()
            if vm.work == self.cur:
                self.load(vm.work)
            self.say("%s: the installer is attached at the next start (Storage > "
                     "Attach installer)" % vm.name)
        self.run_task("Installer disk: %s" % vm.name, argv, env, on_ok=ok)

    def create_disk(self):
        vm = self.vm()
        if not vm or not self.ensure_saved():
            return
        vm = self.vm()
        disk = vm.value("SYSDISK")
        if os.path.exists(disk):
            messagebox.showinfo(TITLE, "%s already exists." % disk)
            return
        size = simpledialog.askinteger(TITLE, "Size of %s in GB:" % disk, parent=self.root,
                                       initialvalue=40, minvalue=1, maxvalue=16384)
        if size:
            try:
                core.create_disk(vm, size)
            except (core.LaunchError, OSError) as e:
                messagebox.showerror(TITLE, "Cannot make the disk:\n%s" % e)
                return
            self.say("Made %s (%d GB)" % (disk, size))
            self.show_checks()

    def build_ovmf(self):
        if not messagebox.askokcancel(
                TITLE, "Build OVMF with the rv530 GOP driver into snowleopard/work/fw?\n\n"
                "build-ovmf.sh clones edk2 and takes some minutes. It needs git, nasm, "
                "acpica-tools and a C compiler (snowleopard/README.md, Setup)."):
            return
        try:
            argv, env = core.ovmf_job()
        except core.LaunchError as e:
            messagebox.showerror(TITLE, str(e))
            return
        self.run_task("Build OVMF", argv, env, on_ok=lambda: (self.show_info(), self.show_checks()))

    def about(self):
        messagebox.showinfo(
            TITLE, "%s (experimental)\n\nManages the virtual machines of rv530-qemu: it "
            "starts snowleopard/sl-vm.sh with the settings of each VM and controls the VM "
            "over QMP.\n\nHost accelerator: %s\nPython %s, Tk %s\nSettings: %s"
            % (TITLE, self.host_accel, sys.version.split()[0], tk.TkVersion, self.reg.path))

    def quit(self):
        if self.dirty and self.vm():
            ans = messagebox.askyesnocancel(TITLE, "Save the changes to \"%s\"?" % self.vm().name)
            if ans is None or (ans and not self.save()):
                return
        running = [v.name for v in self.vms if active(self.eff_status(v.work))]
        if running and not messagebox.askokcancel(
                TITLE, "These VMs keep running after the manager closes:\n\n%s\n\n"
                "Open the manager again to control them." % "\n".join(running)):
            return
        busy = [t for t in self.tasks.values() if t.proc and t.proc.poll() is None]
        if busy:
            if not messagebox.askokcancel(TITLE, "Stop the tasks that run (%s)?"
                                          % ", ".join(t.title for t in busy)):
                return
            for t in busy:
                t.stop()
        self.closing = True
        self.reg.prefs["geometry"] = self.root.geometry()
        self.reg.prefs["selected"] = self.cur
        try:
            self.reg.save()
        except OSError:
            pass
        self.root.destroy()


class TaskWindow:
    """A helper script in a window with its output."""
    n = 0

    def __init__(self, app, title, argv, env, cwd, on_ok):
        TaskWindow.n += 1
        self.id, self.app, self.title, self.on_ok = TaskWindow.n, app, title, on_ok
        self.w = tk.Toplevel(app.root)
        self.w.title(title)
        self.w.geometry("820x480")
        self.text = tk.Text(self.w, wrap="char", font="TkFixedFont")
        ys = ttk.Scrollbar(self.w, orient="vertical", command=self.text.yview)
        self.text.configure(yscrollcommand=ys.set)
        bottom = ttk.Frame(self.w, padding=6)
        bottom.pack(side="bottom", fill="x")
        self.lbl = ttk.Label(bottom, text="Running...")
        self.lbl.pack(side="left")
        self.btn = ttk.Button(bottom, text="Stop", command=self.stop)
        self.btn.pack(side="right")
        ys.pack(side="right", fill="y")
        self.text.pack(fill="both", expand=True)
        self.w.protocol("WM_DELETE_WINDOW", self.close)
        self.text.insert("end", "$ %s\n" % " ".join(argv))
        try:
            kw = {"start_new_session": True} if not core.IS_WINDOWS else {
                "creationflags": getattr(subprocess, "CREATE_NO_WINDOW", 0)}
            self.proc = subprocess.Popen(argv, env=env, cwd=cwd, stdin=subprocess.DEVNULL,
                                         stdout=subprocess.PIPE, stderr=subprocess.STDOUT, **kw)
        except OSError as e:
            self.proc = None
            self.text.insert("end", "cannot start: %s\n" % e)
            self.done(-1)
            return
        threading.Thread(target=self.reader, daemon=True).start()

    def reader(self):
        for line in iter(self.proc.stdout.readline, b""):
            self.app.q.put(("task", self.id, line.decode("utf-8", "replace")))
        self.app.q.put(("taskend", self.id, self.proc.wait()))

    def handle(self, msg):
        if msg[0] == "task":
            at_end = self.text.yview()[1] >= 0.999
            self.text.insert("end", msg[2])
            if at_end:
                self.text.see("end")
        else:
            self.done(msg[2])

    def done(self, rc):
        ok = rc == 0
        self.lbl.configure(text="Done." if ok else "Failed (exit code %s)." % rc,
                           foreground="#2e7d32" if ok else "#c62828")
        self.btn.configure(text="Close", command=self.close)
        self.text.see("end")
        self.app.say("%s: %s" % (self.title, "done" if ok else "failed"))
        if ok and self.on_ok:
            self.on_ok()

    def stop(self):
        if self.proc and self.proc.poll() is None:
            self.proc.terminate()

    def close(self):
        if self.proc and self.proc.poll() is None:
            if not messagebox.askokcancel(TITLE, "Stop %s?" % self.title, parent=self.w):
                return
            self.stop()
        self.app.tasks.pop(self.id, None)
        self.w.destroy()


class Dialog(tk.Toplevel):
    """A modal dialog: body() builds it, apply() sets self.result."""

    def __init__(self, parent, title):
        super().__init__(parent)
        self.title(title)
        self.transient(parent)
        self.result = None
        f = ttk.Frame(self, padding=12)
        f.pack(fill="both", expand=True)
        f.columnconfigure(1, weight=1)
        self.body(f)
        b = ttk.Frame(self, padding=(12, 0, 12, 12))
        b.pack(fill="x")
        ttk.Button(b, text="Cancel", command=self.destroy).pack(side="right")
        ttk.Button(b, text="OK", command=self.ok).pack(side="right", padx=6)
        self.bind("<Return>", lambda e: self.ok())
        self.bind("<Escape>", lambda e: self.destroy())
        self.protocol("WM_DELETE_WINDOW", self.destroy)
        self.update_idletasks()             # center on the main window
        x = parent.winfo_rootx() + (parent.winfo_width() - self.winfo_reqwidth()) // 2
        y = parent.winfo_rooty() + (parent.winfo_height() - self.winfo_reqheight()) // 3
        self.geometry("+%d+%d" % (max(0, x), max(0, y)))
        self.wait_visibility()
        self.grab_set()
        self.wait_window(self)

    def ok(self):
        if self.apply() is not False:
            self.destroy()

    def row(self, f, r, label, widget, help=""):
        ttk.Label(f, text=label).grid(row=r, column=0, sticky="ne", padx=(0, 8), pady=4)
        widget.grid(row=r, column=1, sticky="ew", pady=4)
        if help:
            ttk.Label(f, text=help, style="Help.TLabel", wraplength=420, justify="left").grid(
                row=r + 1, column=1, sticky="w")


class NewVMDialog(Dialog):
    def __init__(self, parent, vms):
        self.vms = vms
        super().__init__(parent, "New VM")

    def body(self, f):
        self.name = tk.StringVar(value="Snow Leopard" if not self.vms else "")
        self.preset = tk.StringVar(value=list(core.PRESETS)[0])
        self.folder = tk.StringVar()
        self.size = tk.StringVar(value="40")
        self.mkdisk = tk.BooleanVar(value=True)
        self.auto_folder = True
        e = ttk.Entry(f, textvariable=self.name, width=40)
        self.row(f, 0, "Name:", e)
        e.focus_set()
        self.row(f, 2, "Guest:", ttk.Combobox(f, textvariable=self.preset, state="readonly",
                                              values=list(core.PRESETS)),
                 "Sets the OpenCore options: 10.7 needs the Auto kernel cache, 10.5 the "
                 "32-bit kernel.")
        row = ttk.Frame(f)
        ent = ttk.Entry(row, textvariable=self.folder, width=48)
        ent.pack(side="left", fill="x", expand=True)
        ent.bind("<Key>", lambda e: setattr(self, "auto_folder", False))
        ttk.Button(row, text="...", width=3, command=self.browse).pack(side="left", padx=2)
        self.row(f, 4, "Folder:", row, "The WORK folder of sl-vm.sh: the disk, NVRAM, boot volume "
                 "and logs of the VM.")
        row2 = ttk.Frame(f)
        ttk.Spinbox(row2, from_=4, to=4096, textvariable=self.size, width=6).pack(side="left")
        ttk.Label(row2, text="GB").pack(side="left", padx=4)
        ttk.Checkbutton(row2, text="Make the disk now", variable=self.mkdisk).pack(side="left", padx=12)
        self.row(f, 6, "System disk:", row2, "Without qemu-img, sl-vm.sh makes a 40 GB disk at "
                 "the first start.")
        self.name.trace_add("write", self.update_folder)
        self.update_folder()

    def update_folder(self, *_):
        if not self.auto_folder:
            return
        if not self.vms and not os.path.isfile(os.path.join(core.DEFAULT_WORK, core.VM_FILE)) \
                and not core.looks_like_vm(core.DEFAULT_WORK):
            self.folder.set(core.DEFAULT_WORK)       # the first VM: sl-vm.sh's default
        else:
            self.folder.set(os.path.join(core.DEFAULT_WORK, core.slug(self.name.get())))

    def browse(self):
        p = filedialog.askdirectory(parent=self, initialdir=core.DEFAULT_WORK)
        if p:
            self.auto_folder = False
            self.folder.set(p)

    def apply(self):
        name, work = self.name.get().strip(), self.folder.get().strip()
        if not name or not work:
            messagebox.showerror(TITLE, "Give a name and a folder.", parent=self)
            return False
        try:
            size = int(self.size.get())
        except ValueError:
            messagebox.showerror(TITLE, "The disk size must be a number.", parent=self)
            return False
        if any(os.path.normcase(os.path.abspath(work)) == os.path.normcase(v.work) for v in self.vms):
            messagebox.showerror(TITLE, "%s is already in the list." % work, parent=self)
            return False
        mkdisk = self.mkdisk.get()
        if mkdisk and not core.find_qemu_img(core.VM(work)):
            mkdisk = False
        self.result = (name, work, self.preset.get(), size, mkdisk)


class OpenCoreDialog(Dialog):
    CARDS = {"Radeon X1600 (rv530)": {},
             "Radeon HD 5770 (hd5770)": {"SL_FBNAME": "Hoolock",
                                         "SL_GPUMODEL": "ATI Radeon HD 5770"}}

    def __init__(self, parent, vm):
        self.vm = vm
        super().__init__(parent, "Build OpenCore Boot Volume")

    def body(self, f):
        vm, oc = self.vm, self.vm.opencore
        ttk.Label(f, text="Boot volume of \"%s\":\n%s" % (vm.name, vm.value("ESP")),
                  justify="left").grid(row=0, column=0, columnspan=2, sticky="w", pady=(0, 8))
        card = list(self.CARDS)[1 if (oc.get("SL_FBNAME") == "Hoolock"
                                      or vm.value("DISPLAY_MODE") == "hd5770") else 0]
        self.card = tk.StringVar(value=card)
        self.row(f, 1, "Card:", ttk.Combobox(f, textvariable=self.card, state="readonly",
                                             values=list(self.CARDS)))
        self.picker = tk.BooleanVar(value=vm.flag("INSTALL"))
        self.verbose = tk.BooleanVar(value=False)
        self.i386 = tk.BooleanVar(value=oc.get("SL_KARCH") == "i386")
        self.kauto = tk.BooleanVar(value=oc.get("SL_KCACHE") == "Auto")
        opts = ttk.Frame(f)
        for text, var in (("Boot picker (needed to start the installer)", self.picker),
                          ("Verbose kernel boot (not with 10.5.0)", self.verbose),
                          ("32-bit kernel (Mac OS X 10.5)", self.i386),
                          ("Auto kernel cache (Mac OS X 10.7)", self.kauto)):
            ttk.Checkbutton(opts, text=text, variable=var).pack(anchor="w")
        self.row(f, 2, "Options:", opts)
        uuid_now = oc.get("SL_UUID") or core.esp_uuid(vm.value("ESP"))
        self.uuid = tk.StringVar(value=uuid_now)
        self.row(f, 3, "System UUID:", ttk.Entry(f, textvariable=self.uuid, width=40),
                 "Kept from the old boot volume. Empty: a new random UUID.")
        row = ttk.Frame(f)
        self.zip = tk.StringVar()
        ttk.Entry(row, textvariable=self.zip, width=40).pack(side="left", fill="x", expand=True)
        ttk.Button(row, text="...", width=3, command=self.browse).pack(side="left", padx=2)
        self.row(f, 5, "OpenCore zip:", row, "Empty: download OpenCore 1.0.8 (DEBUG) and check "
                 "its SHA-256.")

    def browse(self):
        p = filedialog.askopenfilename(parent=self, filetypes=[("OpenCore zip", "*.zip")])
        if p:
            self.zip.set(p)

    def apply(self):
        opts = dict(self.CARDS[self.card.get()])
        if self.picker.get():
            opts["SL_PICKER"] = "1"
        if self.verbose.get():
            opts["SL_VERBOSE"] = "1"
        if self.i386.get():
            opts["SL_KARCH"] = "i386"
        if self.kauto.get():
            opts["SL_KCACHE"] = "Auto"
        # keep the other SL_* options that vm.json has (SL_BOOTARGS, ...)
        for k, v in self.vm.opencore.items():
            if k not in ("SL_FBNAME", "SL_GPUMODEL", "SL_KARCH", "SL_KCACHE", "SL_UUID",
                         "SL_PICKER", "SL_VERBOSE"):
                opts.setdefault(k, v)
        opts["SL_UUID"] = self.uuid.get().strip().upper()      # "": a new one
        self.result = (opts, self.zip.get().strip())


def main():
    if core.IS_WINDOWS:
        try:
            import ctypes
            ctypes.windll.shcore.SetProcessDpiAwareness(1)
        except (AttributeError, OSError):
            pass
    root = tk.Tk()
    App(root)
    root.mainloop()


if __name__ == "__main__":
    main()
