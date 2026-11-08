Pre-built packages of QEMU 11.1.3 with the emulated Radeon X1600 (rv530) and
the experimental Radeon HD 5770. Each package also contains OVMF (with the
firmware graphics driver of the X1600), the synthetic option ROMs, and the
scripts and documentation of this repository.

| Package | Host |
| --- | --- |
| `rv530-qemu-*-linux-x86_64.tar.gz` | x86_64 Linux with glibc 2.35 or later (Ubuntu 22.04, Debian 12, Fedora, Arch, and similar). Needs GTK 3 and a Vulkan driver. Desktop distributions have them |
| `rv530-qemu-*-macos-arm64.tar.gz` | Apple silicon Macs (the guest runs under TCG) |
| `rv530-qemu-*-macos-x86_64.tar.gz` | Intel Macs (the guest runs under Hypervisor.framework) |

**Changes in this release:**

- **Mac OS X 10.7 Lion and 10.5 Leopard.** Both run on the X1600 with Quartz
  Extreme and Core Image. `snowleopard/README.md` tells how to install them.
  The README has screenshots of each guest.
- **Experimental Radeon HD 5770 (`DISPLAY_MODE=hd5770`).** A second device
  emulates the Evergreen GPU of the Mac Pro HD 5770. Mac OS X 10.6 and 10.7
  run on it with Quartz Extreme and Core Image. The vertex and pixel shaders
  run on the host GPU through Vulkan. Nexuiz `bench1` runs at approximately
  27 frames per second at 1280 x 720 with 4 vCPUs.
- **No VBIOS dump for the HD 5770.** The HD 5770 uses a synthetic, clean-room
  AtomBIOS (`scripts/atombios/mkrom-evergreen.py`), as the X1600 does.
- **`glReadPixels` from an FBO on 10.5 (#13).** Green and blue were in the
  wrong order. The device now does the texture byte swap only for textures in
  system memory.

We tested these changes on an x86_64 Linux host (KVM, NVIDIA GPU). We did not
test the HD 5770 on macOS hosts. The known problems of the HD 5770 are in
`docs/EVERGREEN.md` (#15, #16).

Unpack a package and follow `snowleopard/README.md` from step 2. `sl-vm.sh`
uses the emulator, firmware, and option ROM of the package. Thus you do not
need to build anything. The macOS packages contain MoltenVK.

**Sound on Linux:** `AUDIO=usb` plays through PulseAudio or ALSA. PulseAudio
works on PipeWire desktops that have the PulseAudio client library.

**macOS:** the packages are not notarized. Thus Gatekeeper blocks them at
first. After you unpack a package, run `xattr -dr com.apple.quarantine
rv530-qemu-*-macos-*` one time.

**Tests of these packages:** GitHub runners build each package and do a smoke
test on it. The smoke test starts the emulator and initializes the emulated
card. The Linux package also gets this test on five distributions in clean
containers. Nobody booted these packages into Snow Leopard. Nobody tested the
Vulkan backend on the GPU of an Intel Mac.

`THIRD-PARTY-NOTICES.md` in each package lists the bundled libraries and their
licenses. The release also contains the QEMU source of the packages
(`qemu-11.1.3.tar.xz`) and this repository at the tag. We provide this project
"as is," without warranty. See the Disclaimer in the README.
