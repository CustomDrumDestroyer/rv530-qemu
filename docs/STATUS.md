# Status

The state of each module of the RV530 (Radeon X1600) device model.

## Summary

- Mac OS X 10.6.8 runs on the emulated X1600 with the ATI drivers from Apple.
  These drivers are not changed.
- Quartz Extreme and Core Image run on the emulated GPU. The fragment work
  runs on the host GPU through Vulkan.
- The Linux `radeon` driver is the reference path.
- Hosts:
  - x86_64 Linux with KVM (the reference host).
  - Apple silicon macOS under multi-threaded TCG.
  - Intel macOS under Hypervisor.framework.
- On macOS, the Vulkan backend runs on MoltenVK (see "macOS hosts" in the
  top-level README).

## Device model (`qemu/hw/display/rv530/`)

| File | State |
| --- | --- |
| `rv530.c` | PCI device (1002:71C5, subsystem 106B:0080), BAR0 256 MiB VRAM aperture, BAR2 64 KiB MMIO, BAR4 I/O alias, expansion ROM. Register file, MM_INDEX/DATA, and the PLL/PCIE/MC indirect spaces. Interrupts (INTx and MSI), the polling-loop detector, BQL stall reports (`RV530_STALL_MS`), and QOM properties, including the run-time `auto-resize` and `resize-to`. MMIO does not use the re-entrancy guard of QEMU and has its own guard. Thus, while a CP run has released the BQL, other vCPUs wait and do not lose accesses |
| `r5xx_mc.c` | MC address space (FB/AGP locations), RV370-style PCIe GART with a page cache, and GPU address translation for all engines |
| `r5xx_cp.c` | Ring and indirect buffers, PM4 types 0/2/3, RPTR and scratch write-back, and software interrupt. It runs synchronously in the vCPU that writes `CP_RB_WPTR` (`cp-sync`). It releases the BQL while it only waits (shader compiles, CPU raster workers, long waits for GPU batches) |
| `r5xx_avivo.c` | D1/D2 scanout (linear and tiled, 8-64 bpp), CRTC timing with vblank and scanline counters at the programmed rate, hardware cursor with hot-spot recovery, DC LUT gamma on all formats, HPD, and the DDC buses. Changes of resolution when you resize the window |
| `r5xx_edid.c` | Its own EDID 1.3 and CEA-861: CVT reduced-blanking timings up to 2560x1600, and the preferred size at `refresh` Hz. The maximum is the 330 MHz dual-link pixel clock that the Apple driver accepts (2560x1440 gets 81 Hz) |
| `r5xx_atom.c` | The mailbox behind the synthetic AtomBIOS (`scripts/atombios/mkrom.py`) |
| `r5xx_2d.c` | Legacy 2D engine: fills, blits, host data, and byte-swapped sources in system memory |
| `r5xx_3d.c` | 3D engine glue: state latching, PVS/US uploads, draw packets, vertex fetch, clipping, and MSAA resolve. It sends each draw to the Vulkan backend or to the CPU path |
| `r5xx_pvs.[ch]` | Vertex program decoder, interpreter (including the fixed-function lighting programs of Apple), disassembler, and GLSL emitter |
| `r500_us.[ch]` | Fragment program decoder, quad interpreter, disassembler, and GLSL emitter for the Vulkan backend |
| `r5xx_tex.[ch]` | Texture decoding and layout (mips, cube, 3D, tiling), all R300/R500 formats including DXT, ATI1N/2N, and Y'CbCr 4:2:2, sampling, and render-target pack/unpack |
| `r5xx_raster.[ch]` | CPU rasterizer: triangles, lines, points, depth/stencil, alpha test, blending, and logic ops. Large draws run on a pool of workers |
| `r5xx_vk.[ch]`, `r5xx_vk_state.c`, `r5xx_vk_frag.glsl` | Vulkan backend: rasterization, fragment programs, texturing (2D, rectangle, 3D, Y'CbCr), blending, and depth/stencil on the host GPU. It has a device-local VRAM mirror and a persistent shader cache. It also runs on MoltenVK: float64 is optional (without float64, depth calculation with floats is still exact). On macOS, the device checks the Metal devices before it loads MoltenVK (`r5xx_vk_mac.m`). The CPU path does the draws that the backend cannot do. `rv530_vk_fallback` gives the reason |
| `r5xx_clip.c` | Clipboard peer for the guest agent (`snowleopard/agent`) |
| `r5xx_regnames.c` | Register names for traces (made by `qemu/hw/display/rv530/scripts/gen_regnames.py`) |

## Tests

| Suite | Result |
| --- | --- |
| `make -C tests/pvs check` | 2715 checks passed |
| `make -C tests/us check` | 557 checks passed. glslangValidator validates the GLSL with and without float64. The Mesa corpus step needs `tools/mesa-rc` |
| `make -C tests/raster check` | 3932 checks passed |
| `make -C tests/tex check` | 42 checks passed: formats (including DXT1, Y'CbCr with YUV_TO_RGB, and endian swap), 3D slices, mip chains, and wrap/border/bilinear/trilinear sampling |
| `tests/vk/test_vk` (`RV530_VK_SYNC_COMPILE=1`) | Compares the GPU with the CPU path on composite, bilinear, depth/stencil, RGB565, UV88, and fp16: passed |

## Snow Leopard support (`snowleopard/`)

| Part | State |
| --- | --- |
| `Rv530GopDxe/` + `build-ovmf.sh` | A firmware GOP over the scanout of the card. OVMF without the memory protections that the `boot.efi` of 10.6 cannot use |
| `make-opencore.sh` | `ATY,Wormy` framebuffer properties, iMac8,1 SMBIOS (placeholder serials), RTC ACPI patch, and tablet kext. It boots directly to the Apple logo |
| `sl-vm.sh` | q35 launcher: the card behind a `pci-bridge`, USB input and audio, ssh forward, drag and drop, and clipboard |
| `agent/` | Drag and drop in the two directions, text clipboard, and resolution changes when you resize the window |
| `tests/` | OpenGL and Core Image checks in the guest (formats, VBOs, MSAA, depth, Y'CbCr video textures, and Core Image blur compared with software) |

## Known gaps

- The command processor runs synchronously in the vCPU that writes the ring
  pointer, because the guest drivers must see it busy.
  - A long batch keeps that vCPU out of the guest until the host GPU
    completes the batch.
  - With several vCPUs, a guest that waits too long for a TLB shootdown can
    panic (Mac OS X: approximately 250 ms).
  - `RV530_STALL_MS` reports long runs.
- The Vulkan backend emulates the R5xx framebuffer in a storage buffer with
  fragment-shader interlock. This makes overlapping draws run one after the
  other. A batch of some hundred draws takes 10-25 ms on the host GPU.
- The CPU still interprets the vertex programs and the command processing.
- The first use of a fragment program compiles a Vulkan pipeline. The device
  keeps a persistent shader cache that it prepares at start
  (`RV530_VK_CACHE_DIR`).
- There is no support for migration or snapshots (you cannot migrate the
  device).
