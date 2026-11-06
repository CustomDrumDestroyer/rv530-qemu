# Contributing to rv530-qemu

We welcome contributions: bug reports, test results, code, and documentation.
You do not need to ask first. Fork the repository and open a pull request. For
a large change, open an issue first. This helps to prevent duplicate work.

## Where we especially want help

### 1. Better support for macOS and Apple silicon hosts

Macs work, but Linux is still the most stable host. Apple silicon is the least
stable host, because TCG emulates the CPU of the guest. These tasks are
useful:

- Fix the open macOS and TCG bugs:
  [#4](https://github.com/CustomDrumDestroyer/rv530-qemu/issues/4) (stale window
  content with several vCPUs),
  [#5](https://github.com/CustomDrumDestroyer/rv530-qemu/issues/5) (a 1-pixel
  line that stays on the screen),
  [#6](https://github.com/CustomDrumDestroyer/rv530-qemu/issues/6) (VRAM writes
  that the dirty log does not see),
  [#7](https://github.com/CustomDrumDestroyer/rv530-qemu/issues/7) (MoltenVK
  rendering differences), and
  [#8](https://github.com/CustomDrumDestroyer/rv530-qemu/issues/8) (a Cocoa crash
  at exit).
- Make the guest faster and more stable under TCG.
- Test on different Mac models and macOS versions. Intel Macs especially need
  more tests. Reports that something works are also useful.

### 2. Support for Windows hosts

Windows is not supported now. As far as we know, nobody tried it. Thus this is
a new port, and we do not know how far Snow Leopard can get. A port must change
at least these areas:

- **Acceleration:** `snowleopard/host.sh` selects only KVM,
  Hypervisor.framework, or TCG. The Windows accelerator of QEMU (WHPX) is the
  obvious candidate. TCG is the fallback.
- **Build:** `scripts/setup-qemu.sh`, `build-ovmf.sh`, and `make-opencore.sh`
  are bash scripts. They have branches only for Linux and macOS.
- **Launcher:** `snowleopard/sl-vm.sh` is also bash. It uses Unix sockets for
  QMP and the clipboard.
- **Guest agent channels:** `scripts/dnd/` uses `AF_UNIX` sockets. Drag and drop
  and clipboard sync also need a Windows UI on the host side. The patches in
  `qemu/patches/` change only the GTK and Cocoa windows of QEMU.
- **Device:** the Vulkan backend (`r5xx_vk.c`) needs Vulkan 1.3 with
  `VK_EXT_external_memory_host` and fragment shader interlock. We built it only
  for Linux and macOS.

We welcome partial progress. A build that compiles, or a first boot under TCG,
is sufficient for a pull request. If you start this work, please open an issue.
Then other people can find the work and help.

## Reporting bugs

Use the [bug report template](https://github.com/CustomDrumDestroyer/rv530-qemu/issues/new?template=bug_report.md).
The host, the accelerator, the number of vCPUs, and the guest version are the
most important data, because many bugs depend on them.

Before you send logs or screenshots, remove all private data. Examples are your
Apple SMC key, serial numbers, and the paths of your home folder.

## Pull requests

1. Fork the repository and make a branch from `main`.
2. Put only one change in each pull request. Use the same style as the code
   around your change. In the documentation, use American spelling.
3. Run the unit tests (below). If the change has an effect on what the guest
   sees, also test in a guest.
4. In the pull request, tell us what you tested and on which host. If you
   tested on only one host, say so. A change to shared code can behave
   differently on other hosts.
5. If the behavior changes, update the documentation.

The repository does not run CI on pull requests. Thus your test results are
the only data that reviewers have.

```
make -C qemu/hw/display/rv530/tests/pvs check
make -C qemu/hw/display/rv530/tests/us check
make -C qemu/hw/display/rv530/tests/raster check
make -C qemu/hw/display/rv530/tests/tex check
make -C qemu/hw/display/rv530/tests/vk && RV530_VK_SYNC_COMPILE=1 qemu/hw/display/rv530/tests/vk/test_vk
```

Some test steps need the reference sources from `scripts/fetch-references.sh`.
On macOS, build the tests with `CC=clang`.

To debug a rendering problem, use the device properties, environment
variables, and traces in the README. One of them is `vk-verify`, which
compares the GPU result with the CPU path.

## What we cannot accept

You must never add these items to the repository. This includes tests,
patches, and attachments to issues:

- VBIOS images of real cards, or CP microcode
- Operating system install media or disk images
- The Apple SMC key
- Apple software of any type

For this reason, the option ROM in this repository is synthetic and clean-room.

## License

When you contribute, you agree that your contribution uses the GNU General
Public License, version 2 or (at your option) any later version. The project
and QEMU use the same license (see `COPYING`). If you use code from other
projects, keep its attribution and make sure that its license is compatible.
