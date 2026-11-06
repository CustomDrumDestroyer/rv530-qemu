Pre-built packages of QEMU 11.1.3 with the emulated Radeon X1600 (rv530).
Each package also contains OVMF (with the firmware graphics driver of the
card), the synthetic option ROM, and the scripts and documentation of this
repository.

| Package | Host |
| --- | --- |
| `rv530-qemu-*-linux-x86_64.tar.gz` | x86_64 Linux with glibc 2.35 or later (Ubuntu 22.04, Debian 12, Fedora, Arch, and similar). Needs GTK 3 and a Vulkan driver. Desktop distributions have them |
| `rv530-qemu-*-macos-arm64.tar.gz` | Apple silicon Macs (the guest runs under TCG) |
| `rv530-qemu-*-macos-x86_64.tar.gz` | Intel Macs (the guest runs under Hypervisor.framework) |

**Changes in this release:**

- **Stale or mixed content in 3D and window drawing (#10, #11, #4).** An
  upload to the GPU copy of VRAM read guest memory when the GPU ran the batch,
  not when the guest sent the draw. If the guest changed a texture between two
  draws of one batch, the earlier draw also used the new texels. Uploads now
  copy from a snapshot that is taken when the draw is recorded. In our tests on
  an Apple silicon Mac, Neverball, OpenArena, and Exposé showed no streaks,
  strips, or tearing after this change.
- **Fewer stalls under TCG (#12).** Polygon offset now draws on the GPU, not on
  the CPU. The vertex shader interpreter and depth clears are faster. In
  Neverball, one draw batch held the QEMU main lock for a maximum of 233 ms
  before. Now the maximum is approximately 190 ms. Snow Leopard panics at
  250 ms, so #12 stays open.

We tested these changes with a source build on one Apple silicon Mac. If you
see stale content, streaks, or long stalls, please report them.

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
