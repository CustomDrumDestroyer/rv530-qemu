# VM Manager (experimental)

`slgui.py` is a desktop window to manage the virtual machines of rv530-qemu.
It runs on Linux, macOS, and Windows hosts.

```
python3 snowleopard/gui/slgui.py
```

The GUI does not replace the scripts. It uses them:

- It starts `snowleopard/sl-vm.sh` with the settings of a VM.
- It controls a VM that runs through QMP, as `slctl.py` does.
- It runs `make-opencore.sh`, `make-installer.py`, and `build-ovmf.sh`.

Thus a VM that you make in the GUI also starts from a terminal, and a VM that
you start from a terminal also shows in the GUI.

**Status:** this is a first version. We tested it on Linux with a stand-in for
QEMU (see [Tests](#tests)). We did not test it on macOS or Windows hosts yet.
Reports and fixes are welcome ([CONTRIBUTING.md](../../CONTRIBUTING.md)).

## Requirements

Python 3.8 or later with Tk. The GUI needs no other packages.

| Host | Tk |
| --- | --- |
| Debian, Ubuntu | `sudo apt install python3-tk` |
| Fedora | `sudo dnf install python3-tkinter` |
| Arch | `sudo pacman -S tk` |
| macOS, Python from python.org | Included |
| macOS, Homebrew | `brew install python-tk` |
| macOS, MacPorts | `sudo port install py313-tkinter` |
| Windows, MSYS2 UCRT64 | `pacman -S mingw-w64-ucrt-x86_64-tk` |

On Windows, run the GUI from the "MSYS2 UCRT64" shell. The GUI runs the
scripts with the `bash` of MSYS2. If MSYS2 is not in `C:\msys64`, set
`SLGUI_BASH` to its `usr\bin\bash.exe`. The GUI never uses the `bash.exe` of
WSL.

## What it does

- **VM list.** The list shows each VM and its state (Stopped, Starting,
  Running, Paused). The GUI finds the VMs itself (see
  [Where the VMs are](#where-the-vms-are)).
- **Start.** Before a start, the GUI checks the VM. It stops on a problem
  that makes `sl-vm.sh` or QEMU fail, and gives the fix:
  - QEMU, OVMF, the SMC key, or the boot volume is missing.
  - The installer is missing.
  - A number setting is not a number.
  - The ssh port, VNC display, or QMP socket is the same as that of a VM that
    runs.

  It warns when a port is in use on the host, or when the OVMF has no rv530
  GOP driver. The **Checks** box on the Overview tab shows the same checks.
- **Shut Down.** Presses the Power key of the guest, then Return to confirm
  the dialog of Mac OS X (as `slctl.py shutdown --confirm`). The ACPI power
  button is not used, because it asks 10.6 to sleep.
- **Pause, Resume, Reset, Force Stop.** Force Stop sends the QMP `quit`. If
  QMP does not answer, it stops the process that the GUI started.
- **Screen.** The Overview tab shows the guest screen (Refresh, or Live every
  3 s). **Screenshot** saves a PNG.
- **Keyboard.** Machine > Send Power Key, Send Keys (QEMU qcode names, for
  example `meta_l q`), and Type Text.
- **Settings.** The Settings tab edits the `sl-vm.sh` variables of the VM.
  Each field shows its default. An empty field uses the default of
  `sl-vm.sh`.
- **Logs.** The Logs tab follows the output of `sl-vm.sh` (`WORK/vm.out`),
  the QEMU log (`WORK/qemu.log`), and the serial log.
- **Tools.**
  - Build OpenCore Boot Volume: runs `make-opencore.sh` for the boot volume
    of the VM. You select the card (X1600 or HD 5770), the boot picker,
    verbose boot, the 32-bit kernel (10.5), and the Auto kernel cache (10.7).
    The GUI keeps the System UUID of the old boot volume. It also sets the
    lock timeouts for the accelerator of the VM, if the VM sets `ACCEL`.
  - Make Installer Disk: runs `make-installer.py`. Then the GUI attaches the
    disk at the next start.
  - Create System Disk: runs `qemu-img`.
  - Build OVMF: runs `build-ovmf.sh`.
- **VM menu.** New VM, Add Existing Folder, Remove from List (no files are
  deleted), Open VM Folder, Copy Launch Command (a shell command that starts
  the VM as the GUI does), and Copy ssh Command.

A VM keeps running when you close the GUI. When you open the GUI again, it
finds the VM through its QMP socket.

## Where the VMs are

A VM is a folder: the `WORK` folder of `sl-vm.sh`. It contains the disk,
NVRAM, boot volume, sockets, and logs. The GUI keeps the settings of a VM in
`WORK/vm.json`:

```json
{
  "name": "Lion",
  "env": {"DISPLAY_MODE": "rv530", "SSHPORT": "2226", "VNC": ":4"},
  "opencore": {"SL_KCACHE": "Auto", "SL_UUID": "..."}
}
```

`env` contains the `sl-vm.sh` variables (the list is at the top of
`sl-vm.sh`). `opencore` contains the `make-opencore.sh` variables of the boot
volume.

The GUI finds these VMs:

- `snowleopard/work/`, the default VM of `sl-vm.sh`, if it has a disk, a
  boot volume, an installer, or a `vm.json`.
- Each folder in `snowleopard/work/` that has one of these items, for
  example `work/lion`. `work/fw` (the OVMF build) is not a VM.
- The folders that you add with VM > Add Existing Folder.

The GUI keeps its own settings (the added folders and the window size) in
`slgui.json`:

- Linux: `~/.config/rv530-qemu/`
- macOS: `~/Library/Application Support/rv530-qemu/`
- Windows: `%APPDATA%\rv530-qemu\`

New VM gives each VM its own ssh port and VNC display. On Windows, it also
gives each VM its own QMP port. Thus more than one VM can run at the same
time.

### The SMC key

The GUI looks for the SMC key in this order:

1. Advanced > SMC key in the settings of the VM (in `vm.json`).
2. `OSK='...'` in `WORK/osk.env`.
3. `OSK='...'` in `snowleopard/work/osk.env`.
4. `OSK` in the environment of the GUI.

Use `osk.env`. Git ignores `snowleopard/work/`. The GUI warns if the key is
in a `vm.json` in the repository but outside `snowleopard/work/`.

### Differences from a start in a terminal

- The GUI does not give a VM the `sl-vm.sh` variables of its own
  environment. A `DISPLAY_MODE` that is exported in your shell does not change
  each VM. `QEMU`, `QEMU_IMG`, `OSK`, `OVMF_CODE`, and `OVMF_VARS_TEMPLATE`
  are the exceptions: they apply to each VM that does not set them.
- `sl-vm.sh` looks for the OVMF build of `build-ovmf.sh` only in `WORK/fw`.
  The build is in `snowleopard/work/fw`. For a VM in another folder, the GUI
  gives `sl-vm.sh` that build (`OVMF_CODE` and `OVMF_VARS_TEMPLATE`). Each VM
  keeps its own `OVMF_VARS.fd`.

Copy Launch Command shows these variables.

## Files

| File | Contents |
| --- | --- |
| `slgui.py` | The Tk window |
| `vmcore.py` | VM settings, discovery, checks, launch, and QMP. It has no GUI code |
| `test_vmcore.py` | Tests of `vmcore.py` |
| `fake_qemu.py` | A stand-in for `qemu-system-x86_64`, for the tests. It serves QMP and draws a test image. It runs no guest |

## Tests

```
python3 snowleopard/gui/test_vmcore.py
```

The tests need no display and no QEMU. They run `sl-vm.sh` with
`fake_qemu.py` as QEMU, check its command line, and shut it down through
QMP.

To try the GUI without QEMU, set Advanced > QEMU binary of a VM to
`snowleopard/gui/fake_qemu.py`. Then set dummy files for OVMF code, OVMF
vars template, and the system disk, and make an empty boot-volume folder.
