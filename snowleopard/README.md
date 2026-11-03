# Snow Leopard on the emulated X1600

Everything needed to run Mac OS X 10.6 (Snow Leopard) as a QEMU guest on the
`rv530` device, with Quartz Extreme and Core Image on the emulated GPU, on an
x86_64 Linux host (KVM), an Intel Mac (Hypervisor.framework), or an Apple
silicon Mac (TCG).

## What you provide

This repository contains no Apple software and no Apple keys. You need:

- **A Mac OS X 10.6 install DVD** image (`.iso`, or the raw HFS+ volume
  extracted from it), and optionally the **10.6.8 Combo Update** from Apple.
  The Mac OS X 10.6 client license does not permit running it in a virtual
  machine; Mac OS X Server 10.6's license does. Make sure your use is
  licensed.
- **The SMC key** (`OSK`) that QEMU's `isa-applesmc` presents to the guest.
  This is Apple's string; it is not included, and you must obtain it
  yourself (it can be read from a real Mac's SMC).
- A host: x86_64 Linux with KVM, or a Mac (the top-level `README.md`'s
  "macOS hosts"), and, for acceleration, a Vulkan 1.3 GPU (MoltenVK on a
  Mac).

## Files

| Path | What it is |
| --- | --- |
| `sl-vm.sh` | The VM launcher: q35 + OVMF + OpenCore, the rv530 behind a `pci-bridge` at 00:01.0, USB keyboard and tablet, e1000 with an ssh port forward. The accelerator comes from `host.sh`: KVM on x86_64 Linux, Hypervisor.framework on an Intel Mac, multi-threaded TCG elsewhere (explicit core topology and a 1.2 GHz TSC, 2 GB by default; `ACCEL`, `TSC_HZ`). `DISPLAY_MODE=rv530` (the emulated X1600 as the only GPU), `std` (`-vga std`, for installation), or `both`. All options are listed at the top of the script. |
| `host.sh` | Picks the accelerator for `sl-vm.sh` and `make-opencore.sh` (`kvm`, `hvf`, or `tcg`). |
| `build-ovmf.sh` | Builds OVMF (edk2) with the memory protections that Snow Leopard's `boot.efi` trips over turned off, the `Rv530GopDxe` driver built in, and no TianoCore logo (`OVMF_LOGO=1` keeps it). On macOS it cross-builds with `x86_64-elf-gcc` (Homebrew's on Apple silicon, MacPorts' on an Intel Mac). Output: `work/fw/OVMF_CODE.fd`, `work/fw/OVMF_VARS.fd`. |
| `Rv530GopDxe/` | Firmware graphics driver for the card: a single-mode GOP over the card's scanout, so the firmware, OpenCore, and `boot.efi` can draw before the ATI kexts load. |
| `make-opencore.sh` | Builds the OpenCore boot volume (downloads and verifies OpenCore 1.0.8 when not given a zip): the `ATY,Wormy` framebuffer properties for the card, iMac8,1 SMBIOS with placeholder serials and a random UUID, an ACPI patch for QEMU's RTC resource, and `QemuTabletFix.kext`. Where the guest runs under TCG it adds two kernel patches that make 10.6's lock waits unbounded and a 2 s `slto_us` (`SL_TCG`, `SL_SLTO`; at most 2147483, above which 10.6 panics with "clear_wait_internal: deadlock"). No boot picker unless `SL_PICKER=1`; verbose boot with `SL_VERBOSE=1`. Output: `work/esp/` and `work/esp.img`. |
| `make-installer.py` | Turns an install DVD image (`.dmg`, through `hdiutil` on macOS or `dmg2img` on Linux, or a raw image) into the disk `sl-vm.sh` attaches with `INSTALL=1`: a bare HFS+ volume is wrapped in a GPT disk, since XNU only mounts its root from a partition. |
| `kexts/QemuTabletFix.kext` | Codeless kext that makes 10.6's HID driver use the full usb-tablet range, so the guest pointer follows the host pointer. |
| `agent/` | `RV530Agent`, an optional guest-side helper: drag and drop between host and guest, text clipboard sync, and switching to the new resolution when the window is resized. `agent/install.sh` builds it with Xcode 3.2's gcc and installs it as a LaunchAgent. Without Xcode in the guest, `agent/build-host.sh` builds it on a macOS host (clang for 10.6; `unixthread.py` swaps the `LC_MAIN` today's ld writes for the `LC_UNIXTHREAD` 10.6's dyld needs) and `install.sh` installs that binary. |
| `slctl.py` | Drives a running VM over QMP: screenshots, keys, typing, clicks, HMP commands, quit. |
| `sl-ssh.sh` | ssh into the guest with the legacy algorithms its OpenSSH 5.2 needs (`SSHPORT`, `SL_USER`, `SL_KEY`). |
| `tests/` | In-guest OpenGL and Core Image checks (`tests/README.md`). |
| `work/` | Git-ignored: the system disk, the boot volume, OVMF builds, logs (`serial.log`, `qemu.log`, `vm.out`). Keep your SMC key here too, e.g., in `work/osk.env`, never in a tracked file. |

The host side of drag and drop and clipboard sync is in `../scripts/dnd/`,
and `../scripts/agent-update.sh` installs the agent over ssh.

## Setup

### 1. Build QEMU, OVMF, and the boot volume

```
scripts/setup-qemu.sh                   # QEMU with the rv530, and the option ROM
sudo apt install git uuid-dev nasm acpica-tools dosfstools mtools unzip ovmf   # for the next two
snowleopard/build-ovmf.sh               # -> snowleopard/work/fw/OVMF_CODE.fd
SL_PICKER=1 snowleopard/make-opencore.sh   # with the boot picker, for installing
```

`build-ovmf.sh` clones edk2 and takes a few minutes. It builds nasm and iasl
itself if they are missing.

With a pre-built package from the repository's releases (top-level
`README.md`, "Pre-built packages"), QEMU, OVMF and the option ROM are already
built: run only `make-opencore.sh`, from the unpacked package, which needs
python3, unzip, curl, dosfstools and mtools (on a Mac, Homebrew's or
MacPorts' dosfstools and mtools).

### 2. Install Mac OS X

Install on the standard VGA display first:

```
cd snowleopard
export OSK='...'                        # the SMC key, see above
INSTALL=1 INSTALLER=/path/to/SnowLeopard.iso DISPLAY_MODE=std QDISPLAY=gtk ./sl-vm.sh
```

From a `.dmg`, make the installer disk first: `./make-installer.py
/path/to/SnowLeopard.dmg` writes `work/installer.img`, which `INSTALL=1`
attaches by default. On a Mac, use `QDISPLAY=cocoa` instead of `gtk`.

The system disk `work/snowleopard.qcow2` (40 GB) is created on first start.
In the OpenCore picker, choose the installer. In the installer, use Disk
Utility to erase the QEMU disk as **Mac OS Extended (Journaled)** with a
**GUID** partition map, then install. When it is done, start again without
`INSTALL=1`, pick the installed system, and finish the setup assistant.
Apply the 10.6.8 Combo Update if you have it.

### 3. Switch to the emulated X1600

Rebuild the boot volume without the picker, then start with the rv530 as
the only GPU:

```
snowleopard/make-opencore.sh
cd snowleopard
DISPLAY_MODE=rv530 QDISPLAY=gtk SMP=4 MEM=4096 ./sl-vm.sh
```

The guest goes from a black screen to the Apple logo and the desktop.
Apple's ATI X1000 drivers bind to the card through the device properties
OpenCore injects; nothing is installed in the guest. System Profiler →
Graphics/Displays lists an ATI Radeon X1600 with 256 MB. To check that the
display is accelerated (Quartz Extreme), run this in Terminal; it prints `1`:

```
python -c 'import Quartz; print Quartz.CGDisplayUsesOpenGLAcceleration(Quartz.CGMainDisplayID())'
```

`snowleopard/tests/ci_blur_check.py` compares a Core Image blur on the GPU
with the software renderer (expect `max diff 0`).

### 4. Optional extras

| Option | What it does |
| --- | --- |
| `SMP=N`, `MEM=MB` | vCPUs (default 4) and memory (default 4096). More vCPUs make the GPU much faster, because the command processor runs in the vCPU that submits the work. |
| `XRES`, `YRES`, `XMAX`, `YMAX` | The EDID's preferred mode (default 1440x900) and largest mode (default 2560x1600) |
| `REFRESH=Hz` | Refresh rate of the preferred mode (default 60, up to 240) |
| `AUTORESIZE=1` | The guest resolution follows the QEMU window (with the agent installed). View → Resize Guest with Window (GTK or Cocoa) switches it at run time; with the Cocoa window it also turns on Zoom To Fit. |
| `AUDIO=usb` | A USB audio output that Apple's AppleUSBAudio drives; the host side is Core Audio on a Mac and, on Linux, the first of PipeWire, PulseAudio, and ALSA that QEMU can load (a release package has PulseAudio, which works on PipeWire desktops with the PulseAudio client library, and ALSA). `AUDIO_BACKEND` picks another (pipewire, pa, alsa, sdl, coreaudio, or wav with `AUDIO_WAV`) |
| `DND=1`, `CLIPBOARD=1` | Drag and drop and text clipboard sync with the agent (GTK window on Linux, Cocoa window on a Mac) |
| `SSHPORT` | Host port forwarded to the guest's ssh (default 2222, on 127.0.0.1 only) |
| `EXTRA` | Extra QEMU arguments |

**ssh into the guest.** Turn on Remote Login in System Preferences →
Sharing, and add your public key to `~/.ssh/authorized_keys` in the guest
(`sl-ssh.sh` uses `work/sl_key` by default). With more than one vCPU,
launchd's on-demand sshd may fail to bind early in boot. Running sshd as a
plain daemon instead is reliable. As root in the guest, disable the
on-demand service and add a LaunchDaemon:

```
launchctl unload -w /System/Library/LaunchDaemons/ssh.plist
cat > /Library/LaunchDaemons/local.sshd.plist <<'EOF'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>Label</key><string>local.sshd</string>
	<key>ProgramArguments</key>
	<array><string>/usr/sbin/sshd</string><string>-D</string></array>
	<key>RunAtLoad</key><true/>
	<key>KeepAlive</key><true/>
</dict>
</plist>
EOF
launchctl load -w /Library/LaunchDaemons/local.sshd.plist
```

**The guest agent** needs Xcode 3.2 (on the install DVD, under Optional
Installs) for its compiler. Then, from the host, run
`scripts/agent-update.sh`, or in the guest run `agent/install.sh`.

## Card identity

The device presents itself as the iMac's Mobility Radeon X1600 (1002:71C5,
subsystem 106B:0080) with one DVI connector. It uses the synthetic,
clean-room AtomBIOS from `scripts/atombios/mkrom.py`, so no VBIOS dump is
needed. Apple's driver takes the card's identity from the `ATY,*` properties
OpenCore injects. The SMBIOS model is iMac8,1, the oldest model whose
`boot.efi` starts the 64-bit kernel on 10.6.0.

## Known issues

- The command processor runs in the vCPU that submits the work. A long
  batch, or a shader compiled for the first time, keeps that vCPU out of the
  guest, and Mac OS X panics ("Unresponsive processor") when a vCPU misses a
  TLB shootdown for 250 ms. The device keeps its runs well below that and
  reports long ones (`RV530_STALL_MS`); a persistent shader cache
  (`~/.cache/rv530`) removes most compile waits after the first run.
- Brief audio dropouts can happen while applications launch, for the same
  reason.
- Sleep is disabled (S3/S4 off); shut down from the guest.
- No snapshots or migration.
