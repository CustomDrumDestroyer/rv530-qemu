Pre-built packages of QEMU 11.1.3 with the emulated Radeon X1600 (rv530) and
the experimental Radeon HD 5770. Each package also contains OVMF (with the
firmware graphics driver of the X1600), the synthetic option ROMs, and the
scripts and documentation of this repository.

| Package | Host |
| --- | --- |
| `rv530-qemu-*-linux-x86_64.tar.gz` | x86_64 Linux with glibc 2.35 or later (Ubuntu 22.04, Debian 12, Fedora, Arch, and similar). Needs GTK 3 and a Vulkan driver. Desktop distributions have them |
| `rv530-qemu-*-macos-arm64.tar.gz` | Apple silicon Macs (the guest runs under TCG) |
| `rv530-qemu-*-macos-x86_64.tar.gz` | Intel Macs (the guest runs under Hypervisor.framework) |
| `rv530-qemu-*-windows-x86_64.zip` | x86_64 Windows 10 and 11, experimental (the guest runs under TCG). The scripts need MSYS2 |

**Changes in this release:**

- **Windows hosts (experimental).** QEMU with the rv530 and the HD 5770 builds
  in MSYS2 (UCRT64), and a release package for Windows is new. `sl-vm.sh`,
  `make-opencore.sh`, and `slctl.py` run in the MSYS2 shell. QMP uses TCP on
  Windows. The README section "Windows hosts" tells how to start.
- **`make-opencore.sh`** no longer needs `unzip`.

We tested Windows in a Windows 10 virtual machine without a GPU, with
Mac OS X 10.6.8 under TCG inside it. The CPU path gives correct results there.
We did not test the Vulkan backend on a real GPU under Windows.

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
