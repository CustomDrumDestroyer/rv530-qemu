# Snow Leopard on the emulated X1600

This directory contains all the parts that you need to run Mac OS X 10.6
(Snow Leopard) as a QEMU guest on the `rv530` device. Quartz Extreme and Core
Image run on the emulated GPU. The host can be:

- x86_64 Linux (KVM)
- An Intel Mac (Hypervisor.framework)
- An Apple silicon Mac (TCG)

## What you supply

This repository does not contain Apple software or Apple keys. You need these
items:

- **A Mac OS X 10.6 install DVD** image (`.iso`, or the raw HFS+ volume from
  the DVD). Optionally, the **10.6.8 Combo Update** from Apple.
  - Apple's license for the Mac OS X 10.6 client does not permit running it
    in a virtual machine. Its license for Mac OS X Server 10.6 does. Make sure
    that you have a license that covers your use.
- **The SMC key** (`OSK`) that the QEMU `isa-applesmc` device gives to the
  guest. This is a string from Apple. The repository does not contain it. You
  must get it yourself. You can read it from the SMC of a real Mac.
- A host: x86_64 Linux with KVM, or a Mac (see "macOS hosts" in the top-level
  `README.md`). For acceleration, a Vulkan 1.3 GPU (MoltenVK on a Mac).

## Files

| Path | What it is |
| --- | --- |
| `sl-vm.sh` | The VM launcher: q35, OVMF, and OpenCore, with the rv530 behind a `pci-bridge` at 00:01.0, a USB keyboard and tablet, and an e1000 with an ssh port forward. `host.sh` selects the accelerator: KVM on x86_64 Linux, Hypervisor.framework on an Intel Mac, and multi-threaded TCG on other hosts (explicit core topology, a 1.2 GHz TSC, and 2 GB by default. See `ACCEL` and `TSC_HZ`). `DISPLAY_MODE=rv530` makes the emulated X1600 the only GPU. `DISPLAY_MODE=std` uses `-vga std`, for installation. `DISPLAY_MODE=both` uses both. The top of the script lists all options |
| `host.sh` | Selects the accelerator for `sl-vm.sh` and `make-opencore.sh` (`kvm`, `hvf`, or `tcg`) |
| `build-ovmf.sh` | Builds OVMF (edk2) with these changes: the memory protections that the Snow Leopard `boot.efi` cannot use are off, the `Rv530GopDxe` driver is built in, and there is no TianoCore logo (`OVMF_LOGO=1` keeps the logo). On macOS, it cross-builds with `x86_64-elf-gcc` (from Homebrew on Apple silicon, from MacPorts on an Intel Mac). Output: `work/fw/OVMF_CODE.fd` and `work/fw/OVMF_VARS.fd` |
| `Rv530GopDxe/` | The firmware graphics driver for the card: a GOP with one mode over the scanout of the card. With it, the firmware, OpenCore, and `boot.efi` can draw before the ATI kexts load |
| `make-opencore.sh` | Builds the OpenCore boot volume. If you do not give it a zip, it downloads OpenCore 1.0.8 and checks it. The boot volume contains: the `ATY,Wormy` framebuffer properties for the card, iMac8,1 SMBIOS with placeholder serials and a random UUID, an ACPI patch for the RTC resource of QEMU, and `QemuTabletFix.kext`. If the guest runs under TCG, the script also adds two kernel patches and a 2 s `slto_us` (`SL_TCG`, `SL_SLTO`). The kernel patches remove the time limits from the lock waits of 10.6. The maximum `SL_SLTO` is 2147483. Above it, 10.6 panics with "clear_wait_internal: deadlock". There is no boot picker unless you set `SL_PICKER=1`. For a verbose boot, set `SL_VERBOSE=1`. Output: `work/esp/` and `work/esp.img` |
| `make-installer.py` | Makes the disk that `sl-vm.sh` attaches with `INSTALL=1`, from an install DVD image. The image can be a `.dmg` (it uses `hdiutil` on macOS or `dmg2img` on Linux) or a raw image. If the image is a bare HFS+ volume, the script puts it in a GPT disk, because XNU mounts its root only from a partition |
| `kexts/QemuTabletFix.kext` | A kext without code. It makes the HID driver of 10.6 use the full range of the usb-tablet. Thus the guest pointer follows the host pointer |
| `agent/` | `RV530Agent`, an optional helper in the guest. It does drag and drop between host and guest, text clipboard sync, and a change to the new resolution when you resize the window. `agent/install.sh` builds it with the gcc of Xcode 3.2 and installs it as a LaunchAgent. If the guest does not have Xcode, `agent/build-host.sh` builds it on a macOS host. It uses clang for 10.6, and `unixthread.py` replaces the `LC_MAIN` that the current ld writes with the `LC_UNIXTHREAD` that the dyld of 10.6 needs. Then `install.sh` installs that binary |
| `slctl.py` | Controls a running VM through QMP: screenshots, keys, typing, clicks, HMP commands, and quit |
| `sl-ssh.sh` | Opens ssh to the guest with the old algorithms that its OpenSSH 5.2 needs (`SSHPORT`, `SL_USER`, `SL_KEY`) |
| `tests/` | OpenGL and Core Image checks in the guest (`tests/README.md`) |
| `work/` | Git ignores this directory. It contains the system disk, the boot volume, the OVMF builds, and logs (`serial.log`, `qemu.log`, `vm.out`). Keep your SMC key here too, for example in `work/osk.env`. Never put it in a tracked file |

The host side of drag and drop and clipboard sync is in `../scripts/dnd/`.
`../scripts/agent-update.sh` installs the agent through ssh.

## Setup

### 1. Build QEMU, OVMF, and the boot volume

```
scripts/setup-qemu.sh                   # QEMU with the rv530, and the option ROM
sudo apt install git uuid-dev nasm acpica-tools dosfstools mtools unzip ovmf   # for the next two
snowleopard/build-ovmf.sh               # -> snowleopard/work/fw/OVMF_CODE.fd
SL_PICKER=1 snowleopard/make-opencore.sh   # with the boot picker, for installing
```

`build-ovmf.sh` clones edk2. This takes some minutes. If nasm and iasl are not
available, the script builds them.

A pre-built package from the releases of the repository already contains
QEMU, OVMF, and the option ROM (see "Pre-built packages" in the top-level
`README.md`). With a package, run only `make-opencore.sh` from the unpacked
package. It needs python3, unzip, curl, dosfstools, and mtools. On a Mac, use
dosfstools and mtools from Homebrew or MacPorts.

### 2. Install Mac OS X

First, install on the standard VGA display:

```
cd snowleopard
export OSK='...'                        # the SMC key, see above
INSTALL=1 INSTALLER=/path/to/SnowLeopard.iso DISPLAY_MODE=std QDISPLAY=gtk ./sl-vm.sh
```

If you have a `.dmg`, make the installer disk first: `./make-installer.py
/path/to/SnowLeopard.dmg`. It writes `work/installer.img`. `INSTALL=1`
attaches this file by default. On a Mac, use `QDISPLAY=cocoa` instead of
`gtk`.

The first start makes the system disk `work/snowleopard.qcow2` (40 GB). Then
do these steps:

1. In the OpenCore picker, select the installer.
2. In the installer, open Disk Utility. Erase the QEMU disk as **Mac OS
   Extended (Journaled)** with a **GUID** partition map.
3. Install Mac OS X.
4. When the installation is complete, start again without `INSTALL=1`.
5. Select the installed system and complete the setup assistant.
6. If you have the 10.6.8 Combo Update, apply it.

### 3. Change to the emulated X1600

Build the boot volume again without the picker. Then start with the rv530 as
the only GPU:

```
snowleopard/make-opencore.sh
cd snowleopard
DISPLAY_MODE=rv530 QDISPLAY=gtk SMP=4 MEM=4096 ./sl-vm.sh
```

The guest shows a black screen, then the Apple logo, then the desktop. The ATI
X1000 drivers from Apple bind to the card through the device properties that
OpenCore adds. You do not install anything in the guest. System Profiler →
Graphics/Displays shows an ATI Radeon X1600 with 256 MB.

To make sure that the display is accelerated (Quartz Extreme), run this
command in Terminal. It prints `1`:

```
python -c 'import Quartz; print Quartz.CGDisplayUsesOpenGLAcceleration(Quartz.CGMainDisplayID())'
```

`snowleopard/tests/ci_blur_check.py` compares a Core Image blur on the GPU with
the software renderer. Expect `max diff 0`.

### 4. Optional items

| Option | What it does |
| --- | --- |
| `SMP=N`, `MEM=MB` | Number of vCPUs (default 4) and memory (default 4096). More vCPUs make the GPU much faster, because the command processor runs in the vCPU that sends the work |
| `XRES`, `YRES`, `XMAX`, `YMAX` | The preferred mode of the EDID (default 1440x900) and its largest mode (default 2560x1600) |
| `REFRESH=Hz` | The refresh rate of the preferred mode (default 60, maximum 240) |
| `AUTORESIZE=1` | The guest resolution follows the QEMU window (needs the agent). View → Resize Guest with Window (GTK or Cocoa) changes this setting while the VM runs. With the Cocoa window, it also sets Zoom To Fit |
| `AUDIO=usb` | A USB audio output that the AppleUSBAudio driver of Apple controls. On a Mac, the host side is Core Audio. On Linux, it is the first of PipeWire, PulseAudio, and ALSA that QEMU can load. A release package has PulseAudio and ALSA. PulseAudio works on PipeWire desktops that have the PulseAudio client library. `AUDIO_BACKEND` selects a different backend (pipewire, pa, alsa, sdl, coreaudio, or wav with `AUDIO_WAV`) |
| `DND=1`, `CLIPBOARD=1` | Drag and drop and text clipboard sync with the agent (GTK window on Linux, Cocoa window on a Mac) |
| `SSHPORT` | The host port that forwards to ssh in the guest (default 2222, only on 127.0.0.1) |
| `EXTRA` | More QEMU arguments |

**ssh into the guest.** In the guest, turn on Remote Login in System
Preferences → Sharing. Add your public key to `~/.ssh/authorized_keys` in the
guest. By default, `sl-ssh.sh` uses `work/sl_key`.

With more than one vCPU, the on-demand sshd of launchd can fail to bind early
in the boot. A plain sshd daemon is reliable. As root in the guest, disable the
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

**The guest agent** needs Xcode 3.2 for its compiler. Xcode 3.2 is on the
install DVD, under Optional Installs. Then run `scripts/agent-update.sh` on the
host, or run `agent/install.sh` in the guest.

## The Radeon HD 5770 (experimental)

`DISPLAY_MODE=hd5770` starts the VM on the emulated Radeon HD 5770 instead of
the X1600. It uses a synthetic AtomBIOS (no VBIOS dump) and needs a second
boot volume made with `SL_FBNAME=Hoolock SL_GPUMODEL="ATI Radeon HD 5770"`
(`ESP` selects it). See
[docs/EVERGREEN.md](../docs/EVERGREEN.md).

## Mac OS X 10.7 Lion

Lion (10.7.5) installs and runs on the same card with Quartz Extreme and
Core Image. It runs as a second VM, with its own directory (`WORK`).
Apple's `InstallMacOSX.dmg` holds `InstallMacOSX.pkg`, a xar archive. That
archive holds `InstallESD.dmg`. On Linux, these commands extract it without
a mount:

```
mkdir -p work/lion && cd work/lion
dmg2img -i InstallMacOSX.dmg -o outer.img
# outer.img: Apple partition map, 2048-byte blocks, the HFS+ volume at block 18
dd if=outer.img of=outer-hfs.img bs=2048 skip=18 count=<Apple_HFS length>
guestfish --ro -a outer-hfs.img run : mount-ro /dev/sda / : download /InstallMacOSX.pkg InstallMacOSX.pkg
mkdir x && bsdtar -xf InstallMacOSX.pkg -C x InstallMacOSX.pkg/InstallESD.dmg
../../make-installer.py x/InstallMacOSX.pkg/InstallESD.dmg installer.img
cd ../..
SL_PICKER=1 SL_KCACHE=Auto ./make-opencore.sh ../OpenCore-1.0.8-DEBUG.zip work/lion/esp
```

`make-installer.py` copies the HFS+ volume from the partition map with
2048-byte blocks. Then it puts the volume in a GPT disk. With
`SL_KCACHE=Auto`, OpenCore adds its kext to the prelinked kernel of Lion,
not to an mkext.

To install Lion, do these steps:

1. Start the installer with the files of the VM, a free ssh port and a free
   VNC display (see the command below).
2. In the boot picker, select "Mac OS X Install ESD".
3. In Disk Utility, erase the empty disk as Mac OS Extended (Journaled),
   with a GUID partition map.
4. Install Lion. The installer restarts two times into the "Lion" entry.

```
. work/osk.env
WORK=$PWD/work/lion QMP=$PWD/work/lion/qmp.sock SERIAL=file:$PWD/work/lion/serial.log \
SYSDISK=$PWD/work/lion/lion.qcow2 ESP=$PWD/work/lion/esp INSTALLER=$PWD/work/lion/installer.img \
SSHPORT=2226 VNC=:4 SMP=4 MEM=4096 DISPLAY_MODE=rv530 INSTALL=1 ./sl-vm.sh
```

After the installation, build the ESP again without `SL_PICKER`. Keep
`SL_KCACHE=Auto`, and keep the `SystemUUID` with `SL_UUID`. The VM then
starts directly to the login window.

Lion needed two changes to the device. Both are about tiled memory:

- The driver fills textures through temporary `SURFACEn` tiling windows.
  The model converts the memory when the driver sets or removes a window
  (`r5xx_mc.c`).
- The driver samples NPOT tiled textures without `TXPITCH_EN`. The model
  then uses the pitch of the next power of two (`r5xx_tex.c`).

The 10.6 `tests/` binaries run on Lion without changes and give the same
results. `RV530Agent` is not installed on Lion yet.

## Mac OS X 10.5 Leopard

Leopard installs and runs on the same card with Quartz Extreme and Core
Image. We tested the 10.5.0 retail DVD (build 9A581). Leopard runs as
another VM, with its own `WORK` directory. Its kernel is 32-bit only. The
DVD image is a hybrid of ISO 9660 and an Apple partition map.
`make-installer.py` accepts it directly:

```
mkdir -p work/leopard
./make-installer.py leopard.iso work/leopard/installer.img
SL_PICKER=1 SL_KARCH=i386 ./make-opencore.sh ../OpenCore-1.0.8-DEBUG.zip work/leopard/esp
```

With `SL_KARCH=i386`, OpenCore starts the i386 kernel on the 64-bit
firmware. The option makes these changes:

- It sets the OpenCore `KernelArch` to i386.
- It disables `SetupVirtualMap`. With this quirk, the kernel reports "Bad
  EFI system table signature" and AppleEFIRuntime causes a panic.
- It enables `ClearTaskSwitchBit`.
- It removes `arch=` from the boot-args.

Do not use `SL_VERBOSE=1` with 10.5.0. Its `boot.efi` crashes in verbose
mode.

To install Leopard, start the installer with `WORK`, `SYSDISK`, `ESP`,
`INSTALLER`, `QMP`, `SERIAL`, a free `SSHPORT` and `INSTALL=1`, as for Lion.
We installed with `DISPLAY_MODE=std`. After the installation, build the ESP
again without `SL_PICKER`. Keep `SL_KARCH=i386` and `SL_UUID`.

Notes from the 10.5.0 installation:

- Setup Assistant can stop at "How Do You Connect?" with a spinning cursor
  ([#14](https://github.com/CustomDrumDestroyer/rv530-qemu/issues/14)).
  In that case, make the account from the Terminal of the install DVD. Make
  a user plist in `/var/db/dslocal/nodes/Default/users` and its shadow hash.
  Add the user to `admin` and make `/var/db/.AppleSetupDone`.
- The status of the e1000 link is inactive. The network starts only after
  `sudo ipconfig set en0 DHCP`. A LaunchDaemon can run this command at boot.
- Disable sleep with `sudo pmset -a sleep 0 displaysleep 0`. The guest
  sleeps after 10 minutes, and after that the network does not start again.
- The Xcode Tools on the DVD (`Optional Installs`) build the `tests/`
  programs.

The 10.5 driver needed two changes to the device:

- The driver reads the bus address of the VRAM from `CONFIG_APER_0_BASE`,
  not from the PCI BAR (`rv530.c`).
- For `glReadPixels` from an FBO, the driver samples the FBO texture in VRAM
  with the 8IN32 byte swap (`TXO_ENDIAN`). The device now does the texture
  swap only for textures in system memory (GART). VRAM data has no swap
  (`r5xx_3d.c`, [#13](https://github.com/CustomDrumDestroyer/rv530-qemu/issues/13)).

The `tests/` programs give the same results as on 10.6. The Core Image blur
is exact.

## Card identity

The device shows itself as the Mobility Radeon X1600 of the iMac (1002:71C5,
subsystem 106B:0080) with one DVI connector. It uses the synthetic,
clean-room AtomBIOS from `scripts/atombios/mkrom.py`. Thus you do not need a
VBIOS dump. The Apple driver gets the identity of the card from the `ATY,*`
properties that OpenCore adds.

The SMBIOS model is iMac8,1. This is the oldest model whose `boot.efi` starts
the 64-bit kernel on 10.6.0.

## Known issues

- The command processor runs in the vCPU that sends the work. A long batch,
  or a shader that compiles for the first time, keeps that vCPU out of the
  guest. If a vCPU does not answer a TLB shootdown for 250 ms, Mac OS X panics
  ("Unresponsive processor").
  - The device keeps its runs much shorter than that. It reports long runs
    (`RV530_STALL_MS`).
  - A persistent shader cache (`~/.cache/rv530`) removes most compile waits
    after the first run.
- For the same reason, short audio dropouts can occur while applications
  start.
- Sleep is disabled (S3/S4 off). Shut down from the guest.
- There are no snapshots and no migration.
