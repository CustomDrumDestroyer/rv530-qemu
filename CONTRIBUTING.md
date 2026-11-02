# Contributing to rv530-qemu

Contributions are welcome, from bug reports and test results to code and
documentation. You don't need to ask first: fork the repository and open a
pull request. For a large change, opening an issue first helps avoid
duplicated work.

## Especially welcome

### 1. Better macOS and Apple silicon host support

Macs work, but Linux is still the most stable host. Apple silicon is the least
stable, because the guest's CPU is emulated under TCG. Useful work includes:

- Fixing the open macOS and TCG bugs:
  [#4](https://github.com/CustomDrumDestroyer/rv530-qemu/issues/4) (stale window
  content with several vCPUs),
  [#5](https://github.com/CustomDrumDestroyer/rv530-qemu/issues/5) (a leftover
  1-pixel line),
  [#6](https://github.com/CustomDrumDestroyer/rv530-qemu/issues/6) (VRAM writes
  missing from the dirty log),
  [#7](https://github.com/CustomDrumDestroyer/rv530-qemu/issues/7) (MoltenVK
  rendering mismatches), and
  [#8](https://github.com/CustomDrumDestroyer/rv530-qemu/issues/8) (a Cocoa crash
  at exit).
- Making the guest faster and more stable under TCG.
- Testing on Mac models and macOS versions, including Intel Macs, which are
  less tested. Reports that something works are useful too.

### 2. Windows host support

Windows isn't supported yet, and as far as we know nobody has tried it, so
this is a new port and it's unknown how far Snow Leopard gets. These are the
places we know a port would touch:

- **Acceleration:** `snowleopard/host.sh` picks only KVM, Hypervisor.framework,
  or TCG. QEMU's Windows accelerator (WHPX) is the obvious candidate, and TCG
  is the fallback.
- **Build:** `scripts/setup-qemu.sh`, `build-ovmf.sh`, and `make-opencore.sh`
  are bash scripts with Linux and macOS branches only.
- **Launcher:** `snowleopard/sl-vm.sh` is also bash, and it uses Unix sockets
  for QMP and the clipboard.
- **Guest agent channels:** `scripts/dnd/` uses `AF_UNIX` sockets, and drag and
  drop and clipboard sync need a Windows UI on the host side (the patches in
  `qemu/patches/` cover only QEMU's GTK and Cocoa windows).
- **Device:** the Vulkan backend (`r5xx_vk.c`) needs Vulkan 1.3 with
  `VK_EXT_external_memory_host` and fragment shader interlock, and it has only
  been built for Linux and macOS.

Partial progress is welcome: a build that compiles or a first boot under TCG
is worth a pull request. Please open an issue if you start on this, so others
can find and join the work.

## Reporting bugs

Use the [bug report template](https://github.com/CustomDrumDestroyer/rv530-qemu/issues/new?template=bug_report.md).
The host, the accelerator, the vCPU count, and the guest version matter most,
because many bugs depend on them.

Before you post logs or screenshots, remove anything private, such as your
Apple SMC key, serial numbers, and home folder paths.

## Pull requests

1. Fork the repository and branch from `main`.
2. Keep each pull request to one change, and match the style of the code around
   it. In the docs, use American spelling.
3. Run the unit tests (below), and test in a guest when the change affects what
   the guest sees.
4. Say in the pull request what you tested, on which host. If you only tested on
   one host, say so: a change to shared code can behave differently on others.
5. Update the docs if behavior changes.

The repository doesn't run CI on pull requests, so your test results are what
reviewers have to go on.

```
make -C qemu/hw/display/rv530/tests/pvs check
make -C qemu/hw/display/rv530/tests/us check
make -C qemu/hw/display/rv530/tests/raster check
make -C qemu/hw/display/rv530/tests/tex check
make -C qemu/hw/display/rv530/tests/vk && RV530_VK_SYNC_COMPILE=1 qemu/hw/display/rv530/tests/vk/test_vk
```

Some test steps need the reference sources from `scripts/fetch-references.sh`.
On macOS, build the tests with `CC=clang`.

To debug a rendering problem, the README documents the device properties,
environment variables, and traces, including `vk-verify`, which compares the GPU
against the CPU path.

## What can't be accepted

These must never be added to the repository, including in tests, patches, or
issue attachments:

- VBIOS images of real cards, or CP microcode
- Operating system install media or disk images
- The Apple SMC key
- Apple software of any kind

The option ROM in this repository is synthetic and clean-room for this reason.

## License

By contributing, you agree that your contribution is licensed under the GNU
General Public License, version 2 or (at your option) any later version, the
same as the project and QEMU (see `COPYING`). Keep the attribution when you use
code from other projects, and make sure its license is compatible.
