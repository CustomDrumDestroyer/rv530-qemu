Pre-built packages of QEMU 11.1.3 with the emulated Radeon X1600 (rv530),
with OVMF (with the card's firmware graphics driver), the synthetic option
ROM, and this repository's scripts and documentation.

| Package | Host |
| --- | --- |
| `rv530-qemu-*-linux-x86_64.tar.gz` | x86_64 Linux with glibc 2.35 or later (Ubuntu 22.04, Debian 12, Fedora, Arch, and similar); needs GTK 3 and a Vulkan driver, which desktop distributions have |
| `rv530-qemu-*-macos-arm64.tar.gz` | Apple silicon Macs (the guest runs under TCG) |
| `rv530-qemu-*-macos-x86_64.tar.gz` | Intel Macs (the guest runs under Hypervisor.framework) |

Unpack one and follow `snowleopard/README.md` from step 2: `sl-vm.sh` uses
the package's emulator, firmware, and option ROM, so there is nothing to
build.  The macOS packages bundle MoltenVK.

**Sound on Linux:** `AUDIO=usb` plays through PulseAudio, which works on
PipeWire desktops with the PulseAudio client library installed, or ALSA.

**macOS:** the packages are not notarized, so Gatekeeper blocks them at
first.  After unpacking, run `xattr -dr com.apple.quarantine
rv530-qemu-*-macos-*` once.

**How these were tested:** every package is built and smoke-tested on GitHub's
runners (it starts and initializes the emulated card), and the Linux package
on five distributions in clean containers.  The Linux package was also booted
into Snow Leopard by hand.  The macOS packages have not been booted into Snow
Leopard, and the Vulkan backend has not been tested on an Intel Mac's own GPU.

`THIRD-PARTY-NOTICES.md` in each package lists the bundled libraries and
their licenses.  The QEMU source the packages are built from
(`qemu-11.1.3.tar.xz`) and this repository at the tag are attached below.
This project is provided as is, without warranty (see the README's
Disclaimer).
