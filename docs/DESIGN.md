# R5xx GPU Emulation for Snow Leopard QE/CI

This document is the original design plan. We completed phases 0-10. Snow
Leopard now runs with Quartz Extreme and Core Image, and the fragment work runs
on Vulkan. For the current state, read `docs/STATUS.md`. To run the project,
read `snowleopard/README.md`.

## Overview

Build a QEMU device model of an ATI Radeon X1600 (RV530, R5xx family). The
device translates its 3D engine to Vulkan. The goal: the ATI X1000 driver stack
of Snow Leopard, without changes, reports Quartz Extreme and Core Image as
Supported on an x86\_64 KVM guest. The project does not use GPU passthrough.

The approach is the same as ppcosxkvm. ppcosxkvm did the same for Tiger on
PowerPC with an emulated Radeon 9700 (R300). We do not write guest kexts or GL
bundles. The Apple drivers run as Apple supplies them. All the work occurs in
the emulated hardware.

The guest communicates with the emulated card in the same way as with real
hardware. The 3D and shader translation is the largest new part.

**Success criteria**

1. The System Profiler of a 10.6.8 guest shows the emulated X1600 with Quartz
   Extreme and Core Image both Supported.
2. Dock effects, the Dashboard ripple, and the Core Image Fun House filters
   render correctly.
3. A Linux guest with the in-kernel `radeon` driver and the Mesa r300 driver
   passes an agreed subset of piglit.
4. The desktop is interactive at native resolution on the host GPU.

**Working rules**

- Make each feature work on a Linux guest first, because the Linux driver is
  open source. Start to work on Snow Leopard only when Linux is correct.
- From the first day, trace each register access and command packet.
- Correctness comes before speed. Each translator has a CPU reference path
  before you tune the Vulkan path.

## Prerequisites

You need these items:

- A Linux host with a Vulkan 1.3 driver.
- The reference material below.
- Experience with QEMU device code, Vulkan, and shader compilation.

**Skills**

- C in the QEMU style: QOM devices, MemoryRegions, bottom halves, and trace
  events.
- Vulkan: pipelines, descriptors, render passes or dynamic rendering, and
  SPIR-V (or GLSL that shaderc compiles).
- GPU concepts at register level: command rings, GART, tiling, and scanout.
- How to boot legacy OS X on QEMU with OVMF and OpenCore.

**Hardware**

- Host: a Linux computer with a modern Vulkan driver and KVM. A recent NVIDIA
  or AMD GPU is satisfactory.
- Optional but valuable: a real X1600 (a 2006 iMac or MacBook Pro, or a PCIe
  X1600 in a Linux PC). With it, you get a VBIOS dump for private tests, and
  `mmiotrace` captures as the reference.

**Reference material to get**

| Source | What it gives you |
| --- | --- |
| AMD R5xx Acceleration and R5xx display register documents (published for the X.org project, 2007–2008) | The meanings of the registers for the 3D engine and the AVIVO display |
| Linux `drivers/gpu/drm/radeon` (`rv515.c`, `r520.c`, `r300.c`, the `rv370` PCIe GART code, `atom.c`, `atombios_*.c`, `rv515d.h`, `r500_reg.h`, `avivod.h`) | Init sequences, register names, the GART entry format, and the AtomBIOS interpreter |
| Mesa `src/gallium/drivers/r300` and its `compiler/` directory (`r500_fragprog_emit.c`, `r3xx_vertprog.c`, `r300_texture_desc.c`) | The exact shader encodings, texture layout, and tiling rules |
| `xf86-video-ati` (older releases with AVIVO and EXA code) | Display setup and use of the 2D engine |
| Upstream QEMU `hw/display/ati.c` and `ati_2d.c` | A structure for an ATI PCI device, and 2D blit emulation |
| QEMU `hw/display/i2c-ddc.c` and `hw/i2c/bitbang_i2c.c` | EDID over bit-banged DDC, ready to use |
| ppcosxkvm and its QEMU fork (R300 work from the poweremu-qemu forks) | The R300 command processor, PVS, and fragment translation to Metal and Vulkan |
| LegacyOSXKVM / DarwinKVM | An OVMF and OpenCore setup for Snow Leopard on QEMU that is known to work |

**Guest media**: Snow Leopard 10.6.3 retail, the 10.6.8 combo update, and a
Linux distribution that still contains the Mesa r300 driver.

## Phase 0: Base, layout, and tracing

Start from the QEMU fork of ppcosxkvm. Then you get its R300 command processor
and Vulkan backend. Build the tracing before any device logic.

1. Clone the QEMU fork of ppcosxkvm at its pinned commit. Make sure that
   `qemu-system-ppc` builds and runs Tiger with its Vulkan backend on your
   host. Then you know that the backend works.
2. In the same tree, enable the `x86_64-softmmu` target with KVM.
3. Move the R300 parts that you can use again into a shared directory, for
   example `hw/display/radeon-common/`. These parts are the PM4 parser, the
   PVS decoder, the blend/depth state mapping, the Vulkan device, and the
   pipeline cache. Then the PPC and x86 devices both link them.
4. Make the new device directory with one file for each subsystem:
   - `rv530.c` – PCI device, register file, reset
   - `r5xx_mc.c` – memory controller, VRAM, PCIe GART
   - `r5xx_cp.c` – ring, indirect buffers, fences, interrupts
   - `r5xx_avivo.c` – display, cursor, vblank, DDC
   - `r5xx_3d.c` – 3D state cache and draw dispatch
   - `r5xx_pvs.c` and `r500_us.c` – decoders and translators for vertex and
     fragment shaders
   - `r5xx_tex.c` – formats, layout, tiling
   - `r5xx_vk.c` – Vulkan backend glue
   - `r5xx_regnames.c` – the register-name table that a script makes
   - `trace-events`
5. Make the register-name table with a script that parses `r500_reg.h`,
   `rv515d.h`, and `avivod.h` from the Linux tree. Each log line must show
   names, not offsets.
6. Add trace events for each MMIO read and write, each PM4 packet, each draw,
   and each unknown register.
7. Add a capture mode. It records the command stream and the memory ranges
   that the stream refers to in a file. Also add a replay tool without a
   display. It runs a capture through the Vulkan backend and writes PNGs. This
   becomes your regression suite.
8. Add a polling-loop detector. It logs when the device reads the same
   register more than some hundred times in sequence with no change of value.
   While you make the driver work, this shows the status bit that you do not
   emulate.

**Exit criterion**: the empty device builds into `qemu-system-x86_64`, and the
replay of an R300 capture from the PPC side still renders.

## Phase 1: PCIe device skeleton

The device must have an ID that the Apple kexts match. It must have the BAR
layout that the Linux `radeon` driver expects for R5xx. Its status registers
must show an idle GPU.

1. **Get the device ID from the kexts, not from memory.** On an installed
   10.6.8 system, list each kext whose `IOPCIMatch` contains an ATI ID in the
   0x71xx range: `grep -l '0x71' /System/Library/Extensions/*/Contents/Info.plist`.
   Record which kexts match (framebuffer, `ATIRadeonX1000.kext`). Select an
   X1600 ID that all of them contain. We think that the Mobility X1600 in
   early Intel Macs is 1002:71C5. Check this against the plists and the Linux
   `radeon_pciids`.
2. **Set an Apple subsystem vendor (106B)**. If possible, copy the subsystem
   ID from an `ioreg` dump of a real X1600 Mac.
3. **Register a PCIe device** with a PCIe capability. It goes behind a root
   port on the `q35` machine.
4. **Make the BARs** that the R5xx driver path expects:
   - BAR0: 64-bit prefetchable VRAM aperture, 256 MB, with a RAM MemoryRegion
     behind it.
   - BAR2: register MMIO, 64 KB. Linux maps the R5xx registers from BAR2.
   - BAR4: a small I/O BAR for legacy index/data access.
   - Expansion ROM BAR: the VBIOS image (Phase 4).
5. **Build the register file** as a flat 64 KB array with read and write
   hooks for each register. An unknown register keeps the written value, gives
   it back on a read, and makes one log entry.
6. **Add indirect access** through `MM_INDEX`/`MM_DATA` at offsets 0x0/0x4.
   Also add the index/data pairs for MC and PCIe (Phase 2).
7. **Report idle and size**: `RBBM_STATUS` with the GUI-active bit clear, and
   `CONFIG_MEMSIZE` equal to the VRAM size.
8. **Add legacy INTx first.** Add MSI later, only if a trace shows that a
   driver enables it.

**Exit criterion**: `lspci -vv` in a Linux guest shows the card with the
correct IDs and BARs. `radeon` probes the card and reads the memory size. It
can fail at a later step.

## Phase 2: Memory controller, VRAM, and GART

Each GPU access goes through one function. This function changes a GPU (MC)
address into host memory, for VRAM and for the PCIe GART.

1. **Model the MC address space.** The driver puts VRAM and the GART window in
   the address space of the GPU with MC registers. It gets to these registers
   through the MC indirect index/data pair. Get the exact register indices for
   RV530 from `rv515d.h` and `rv515.c`. RV515, RV530, and R520 are different
   here. Thus match one chip exactly.
2. **Report MC idle.** Drivers poll an MC status register before they set the
   locations again. Always return idle.
3. **Make the PCIe GART** that RV515/RV530 use. This is the RV370-style GART in
   the Linux `rv370_pcie_gart_*` functions. The driver gets to it through the
   PCIe index/data pair:
   - Latch the GART base, start, and end registers and the control register.
   - Page-table entries are in VRAM or in system memory. Decode them with the
     entry format in `rv370_pcie_gart_set_page`.
   - When the driver sets the TLB invalidate bit, remove all cached
     translations.
4. **Write the translator**: `r5xx_mc_rw(dev, mc_addr, buf, len, is_write)`.
   For addresses in the framebuffer window, it gives VRAM bytes. For the GART
   window, it walks the GART. For all other addresses, it logs a fault.
5. **Add a fast path** for the command processor and vertex fetch. It gives a
   host pointer for a contiguous range if the range is fully in VRAM or in one
   GART page.

**Exit criterion**: unit tests map random GART pages and read them back
through the translator. The Linux driver sets the MC locations and the GART
without a timeout.

## Phase 3: Command processor

R5xx keeps the R300 command processor and PM4 packet format. Thus most of this
phase moves the shared R300 code to the new device and threading model.

1. **Accept microcode uploads and ignore them.** Drivers load CP microcode
   through the CP microcode RAM registers. Linux uses `radeon/R520_cp.bin`.
   Apple supplies its own. Emulate the CP at a high level. Log only a hash of
   the upload.
2. **Make the ring**: ring base, size (log2 in the ring control register),
   read and write pointers, and the read-pointer write-back address. Copy
   RPTR into guest memory each time it increases.
3. **Run the CP on its own thread.** A write to the write pointer starts the
   thread. vCPU MMIO never waits for GPU work. While work is pending,
   `RBBM_STATUS` must report busy, because drivers poll it.
4. **Parse PM4 packets**:
   - Type 0: register writes into the same register file that MMIO uses,
     including the one-register-write mode.
   - Type 2: padding.
   - Type 3: draws (immediate, vertex-buffer, and indexed), loads of vertex
     array pointers, index buffers, wait-for-idle, NOP, and the Hyper-Z clear
     packets. Get the opcode values from the Mesa `r300_reg.h` and the Linux
     `r300d.h`.
5. **Run indirect buffers.** On R300-family parts, the ring writes the IB base
   and size registers. Your write hook for the size register sends the buffer
   through the same parser.
6. **Make fences and scratch registers**: scratch writes, scratch write-back
   to memory when it is enabled, and the software interrupt.
7. **Make interrupts** through `GEN_INT_CNTL`/`GEN_INT_STATUS` for the CP.
   Use the AVIVO display interrupt registers (Phase 4) for vblank.
8. **Log each type-3 opcode that the device does not handle**, with its
   payload. The Apple driver can use packets that Linux and Mesa never send.

**Exit criterion**: the Linux `radeon` driver reports that its ring test and
IB test passed.

## Phase 4: AVIVO display, VBIOS, and firmware console

R5xx replaced the legacy CRTC with the AVIVO display engine. This phase is new
code. The VBIOS is the part with the highest risk.

**Scanout**

1. Make the graphics surface registers of the D1 display controller: enable,
   primary surface address, pitch, X/Y start and end, pixel format, and the
   viewport registers. Get the names from `avivod.h` and `r500_reg.h`. D2 can
   be a stub.
2. At each display refresh, read the surface through the MC translator,
   convert the pixel format, and give it to the QEMU console. If the surface
   is linear 32-bit XRGB with a matching stride, use a zero-copy
   DisplaySurface.
3. If a trace shows that the driver enables tiling on the front buffer, add
   support for tiled scanout surfaces.
4. Make the gamma/LUT registers. At first, you can pass the pixels through
   without a change.

**Timing and interrupts**

5. Run a 60 Hz timer. It sets the vblank status bit, raises the display
   interrupt when the interrupt is enabled, and increases the CRTC position
   counter. Drivers poll both.
6. Accept writes to the CRTC timing registers, and calculate the mode from
   them.

**Cursor and outputs**

7. Connect the hardware cursor registers (surface address, size, position,
   hotspot) to the cursor-define and mouse-set calls of QEMU.
8. Select one output: DVI (TMDS). Accept writes to the encoder, transmitter,
   PLL, and DAC. Make status reads report locked, done, and connected.
9. Connect the DDC GPIO pins to the bit-banged I2C of QEMU with an `i2c-ddc`
   EDID device. Then Linux and OS X both read a real EDID.

**VBIOS (AtomBIOS)**

10. Supply a ROM through the expansion ROM BAR. There are two options:
    - For private tests: a dump from a real X1600 that you own. Do not
      redistribute it.
    - For distribution: a synthetic AtomBIOS image. It has valid headers,
      data tables for one TMDS connector, firmware information, and PLL
      limits. Its command tables only return.
11. Trace the reads of the ROM BAR and the execution of the AtomBIOS tables.
    This shows which tables each driver uses. Linux runs ASIC init only when
    the card seems unposted. Thus report the card as posted (display already
    enabled, memory size set).
12. Find out by experiment if the Apple framebuffer runs the AtomBIOS command
    tables or uses the properties that EFI adds. The answer decides how much
    of the VBIOS you must build.

**Firmware console**

13. OVMF has no GOP driver for this card. Write a small EDK2 DXE driver that
    OpenCore loads. It sets one fixed AVIVO mode and supplies a GOP with a
    linear framebuffer. Without it, `boot.efi` has no surface to draw on before
    the kexts load.

**Exit criterion**: the Linux `radeon` KMS shows a console at native resolution
from the EDID, and the hardware cursor works in X.

## Phase 5: Linux guest bring-up

You can debug the Linux guest, so it is your reference. Prove each 3D feature
here before you try it on Snow Leopard.

1. Install a distribution whose kernel contains `radeon` and whose Mesa still
   builds the r300 Gallium driver. Also install `linux-firmware` for
   `R520_cp.bin`.
2. Boot with `drm.debug=0x1e` and capture the kernel log through a serial
   console.
3. Make the items work in this sequence. Fix each item before you start the
   next item:
   1. Driver probe, MC init, and GART init.
   2. Ring test and IB test.
   3. KMS console at the EDID resolution.
   4. A display server that runs, and `glxinfo` that reports the r300 driver.
   5. `glxgears`, and then simple demos with textures and blending.
   6. Piglit: start with `ARB_vertex_program`, `ARB_fragment_program`, and
      basic texturing tests. Then add more tests.
4. At first, keep the Mesa Hyper-Z off. The r300 driver enables it only on
   request (the `RADEON_HYPERZ` environment variable).
5. Use the Mesa `RADEON_DEBUG` options to print the vertex and fragment
   programs that Mesa compiles. Compare them with the disassembly that your
   decoder makes from the same binaries.
6. Capture each scene that passes with the Phase 0 capture tool. Add it to
   the replay regression suite.

**Exit criterion**: a desktop session runs on the emulated card, and the
selected subset of piglit passes.

## Phase 6: 3D state, vertex fetch, and vertex shaders

Draw packets change the latched 3D registers into one Vulkan draw. The R500
vertex engine extends the R300 vertex engine. Thus use the shared PVS decoder
again, and add the additional R500 features.

**State**

1. Latch all the 3D register blocks (VAP, GA, SU, RS, US, TX, ZB, RB3D, FG, SC)
   into a state struct, with dirty flags for each group.
2. On a draw packet, copy the state into a draw record: primitive type, vertex
   count, index buffer, and vertex streams.
3. Calculate a hash of the state that the pipeline uses. Cache `VkPipeline`
   objects by that hash. Keep a `VkPipelineCache` on disk.

**Vertex fetch**

4. Decode the vertex stream setup (the `VAP_PROG_STREAM_CNTL` registers and
   their extended swizzle registers). Also decode the array pointers from the
   load-vertex-pointers packet.
5. Use vertex pulling. Bind the guest vertex memory as a storage buffer. In the
   generated vertex shader, fetch, convert, and swizzle the data. This method
   handles all formats and swizzles. It avoids the vertex input limits of
   Vulkan.
6. For immediate-mode vertices, copy the packet payload into a host buffer.

**Vertex shaders (PVS)**

7. Capture the PVS code and constants that the driver writes through the PVS
   vector index/data registers. Also capture the code-control registers that
   set the program range.
8. Extend the shared R300 decoder for R500: more temporaries and constants,
   flow-control instructions, and all new opcodes. The Mesa `r3xx_vertprog.c`
   and the register headers define the encodings.
9. Handle the PVS bypass mode. In this mode, the vertices are already
   transformed.
10. Apply the viewport transform in the generated shader, so that it is the
    same as the hardware. The registers are the VAP viewport scale and offset
    registers and the VTE control register. Set the Vulkan viewport to cover
    the render target.
11. Connect the PVS outputs to the fragment inputs through the interpolator
    setup of the rasterizer (the RS registers).

**Rasterization**

12. Map cull mode, polygon offset, point size, line width, scissor, and polygon
    mode to Vulkan state. If Vulkan state cannot express the clip rectangles
    and their combine rule, use fragment discards.
13. Build a CPU reference interpreter for PVS programs. Compare its outputs
    with the GPU path on captured draws.

**Exit criterion**: the piglit vertex-program tests and `glxgears` give the
same images as the CPU reference path.

## Phase 7: R500 fragment shader translator

This translator is the core of the Core Image support. It is the largest
single part of new code. The R500 fragment ISA has almost nothing in common
with the R300 ISA. Thus plan to write it new, against the Mesa encoder.

**Know the ISA**

- A program has a maximum of 512 instructions. Each instruction has six
  dwords. The driver uploads them with constants through the
  `GA_US_VECTOR_INDEX`/`GA_US_VECTOR_DATA` pair.
- Instruction types:
  - ALU: a vector RGB operation and a scalar alpha operation. Each has its own
    source selection, swizzles, modifiers, and pre-subtract.
  - Output writes.
  - Texture: sample, projected sample, LOD bias, kill, and sample with
    supplied derivatives.
  - Flow control: jumps, loops, calls, returns, and predicate conditions.
- R500 fragment arithmetic is FP32 (R300 used FP24). FP32 maps directly to
  Vulkan `float`.

**Build sequence**

1. **Disassembler first.** Build the Mesa r300 compiler as a standalone tool.
   Compile a corpus of ARB fragment programs and GLSL 1.20 shaders to R500
   binaries. Your disassembler must decode them back to the same listing that
   Mesa makes.
2. **CPU reference interpreter.** Run decoded programs on the CPU, one pixel
   at a time. It is slow, but its result is final. It helps you debug each
   later stage.
3. **Lift to an IR** of vec4 temporaries, inputs, constants, and outputs, with
   explicit predicates.
4. **Emit GLSL and compile it with shaderc** at first. Change to direct SPIR-V
   emission only if the compile time is a problem. Cache the compiled modules
   by program hash, on disk.
5. **Control flow**: translate structured loops and branches directly. For
   unstructured flow, use a program-counter loop with a `switch`. Examine Core
   Image captures early. They show how much flow control the Apple compiler
   makes.
6. **Transcendentals and special operations**: use exactly the same domains as
   the hardware. Examples: the input scaling of SIN and COS, and RCP/RSQ of
   zero. Use the Mesa lowering code as the reference for each operation.
7. **Texture instructions** map to combined image samplers that you bind for
   each draw. Make kill a `discard`. Make projected sampling explicit.
8. **Outputs**: map the color outputs to a maximum of four render targets with
   the US output format registers. Handle depth writes.
9. **Differential tests**: run each captured program through the CPU
   interpreter and through the Vulkan path, with random inputs. Compare the
   results with a small tolerance.

**Why Core Image puts load on this translator**: Core Image compiles its
kernels into GPU fragment programs while it runs. Thus expect many long and
different shaders, many texture operations, and many changes in the shader
cache.

**Exit criterion**: the piglit ARB fragment program tests and GLSL 1.20
fragment tests pass on Linux. The differential tests agree on the full capture
corpus.

## Phase 8: Textures, render targets, blending, Hyper-Z, and queries

The rule for this phase: calculate each surface layout exactly as the Mesa
r300 driver does. Keep the surfaces uncompressed on the host, so that the guest
cannot see a difference in compression.

**Textures**

1. Decode the texture unit registers (filter, format, offset with tiling bits,
   border color, enable) into a sampler and an image description.
2. Map the formats to a `VkFormat` and a component mapping. DXT1/3/5 map
   directly to BC1–3. Convert formats that Vulkan does not have in a compute
   shader.
3. Copy the rules of `r300_texture_desc.c` for mip, cube, and 3D layout,
   alignment, and non-power-of-two sizes. A layout error of one byte makes all
   later mips incorrect.
4. Make micro and macro tiling as compute-shader passes that detile and
   retile. Test them against the Mesa surface code with random sizes.

**Render targets and depth**

5. Decode the color buffer offsets, pitches, formats, and tiling (the RB3D
   color registers) for a maximum of four targets. Also decode the depth
   buffer registers (offset, pitch, format, depth/stencil control).
6. Support FP16 and FP32 color targets. Core Image needs floating-point
   surfaces.
7. Record which guest ranges are render targets. Then later texture reads of
   the same memory see the rendered results (Phase 9).

**Fixed-function state for each pixel**

8. Map the blend registers (color and alpha blend control, constant color,
   color mask) to Vulkan blend state. Map raster operations to `logicOp`.
9. Do the alpha test and fog in a generated epilogue in the fragment shader.

**Hyper-Z**

10. Use the meaning of Z-mask and HiZ clears. Do a real Vulkan depth clear to
    the clear value. Ignore the HiZ data, because it changes only performance.
11. Keep depth uncompressed on the host. If the guest reads a depth buffer as
    a texture or through the CPU, supply uncompressed values.
12. Accept Z-cache flush writes and control writes, and report them complete.

**2D engine and queries**

13. Make the solid fill, screen-to-screen copy, and host-data blits of the
    legacy 2D engine. Use the upstream QEMU `ati_2d.c` again. Start in
    software.
14. Make occlusion queries. When the driver sets the Z-pass result address,
    run a Vulkan occlusion query. Write the count into guest memory at the next
    sync point.

**Exit criterion**: the piglit texturing, FBO, blending, and occlusion-query
tests pass, including floating-point targets.

## Phase 9: VRAM coherency and performance

The guest CPU writes VRAM directly. At the same time, host Vulkan images keep
the GPU view of the same memory. Correctness and speed both depend on the
record of which copy is current.

**Resource tracking**

1. Keep an interval map from guest memory ranges (VRAM and GART) to host
   resources. Each resource has one of three states: CPU copy current, GPU
   copy current, or both.
2. Before a draw reads a range whose CPU copy is newer, upload the range
   (detile it if necessary).
3. After the GPU writes a range, mark the GPU copy current.
4. Write the GPU results back to guest memory at sync points: fences, scratch
   writes, wait-for-idle, and before scanout reads a surface.

**Detecting CPU writes**

5. Enable dirty-page logging on the VRAM region. Use it to find pages that the
   CPU changed, at low cost.
6. Start with eager write-back of each range that the GPU wrote, at each
   fence. It is simple and correct.
7. Then make it faster. Write back only the resources that the guest CPU
   reads. Think about write protection of GPU-current pages to catch reads
   when they occur.

**Zero copy where possible**

8. Allocate VRAM as host memory that Vulkan imports
   (`VK_EXT_external_memory_host`). Then linear vertex, index, and constant
   data needs no copy. Tiled surfaces still need conversion.
9. When the scanout surface is a GPU render target, show the Vulkan image
   directly through the dmabuf/GL display path of QEMU. Do not read it back.

**Throughput**

10. Put the draws into one command buffer for each sync point. Submit one time
    for each fence.
11. Keep the pipeline and shader caches between runs.
12. Profile the QEMU process with `perf`. Profile the backend with the
    in-application capture API of RenderDoc.

**Exit criterion**: the Linux desktop and the replay suite keep 60 fps at
native resolution with eager write-back disabled. The image diffs of the
replay suite show no regression.

## Phase 10: Snow Leopard bring-up

Start this phase only after Linux passes Phases 5–9. From this point, the
driver is a black box. Your traces are the only way to see into it.

**Base VM**

1. Start from a LegacyOSXKVM/DarwinKVM Snow Leopard setup that is known to
   work, on `q35` with OVMF and OpenCore. Use its default display first.
   Update to 10.6.8.
2. Make a list of the ATI X1000 stack on the installed system:
   - The framebuffer kext.
   - `ATIRadeonX1000.kext`.
   - The user-space bundles in `/System/Library/Extensions` (GL driver, GA
     plugin, VA decoder).

   Record the IDs that each one matches.
3. Run `lipo -info` on the kext binaries. If they are only 32-bit, boot the
   guest with the 32-bit kernel (`arch=i386`).
4. Select the SMBIOS of a Mac that Apple sold with an X1600 (an early Intel
   iMac or MacBook Pro). The driver behavior can possibly depend on the model.

**Attach the card**

5. Add the emulated X1600 behind a root port at a fixed slot, in place of the
   default display. Load the Phase 4 GOP driver through OpenCore.
6. Add device properties for its PCI path through OpenCore `DeviceProperties`.
   Real Macs got ATI properties from EFI (names, connector descriptions, VRAM
   size, device-type entries). If possible, make the set from an `ioreg` dump
   of a real X1600 Mac. Also use the property lists in old injector projects,
   for example Natit and ATIinject.
7. Boot verbose with `-v keepsyms=1`, and capture the serial output.

**Debug the black box**

8. Compare the MMIO and PM4 trace of the Apple driver with the Linux trace for
   the same phase. The differences show the registers and packets that you
   must add.
9. Watch the polling-loop detector. A stall usually means that one status bit
   is missing.
10. Disassemble the kexts and the GL bundle (Ghidra or Hopper). They export C++
    symbols. Thus you can read the init sequences and the register use.
11. For deeper work, attach the QEMU gdbstub to the guest kernel with the
    symbols of the 10.6.8 Kernel Debug Kit.
12. Use `ioreg -lw0` to see which drivers attached. Use OpenGL Driver Monitor
    (from the Developer Tools) to watch the driver statistics.

**Verify QE/CI**

13. `system_profiler SPDisplaysDataType` shows the X1600 with Quartz Extreme
    and Core Image Supported.
14. Check Dock magnification and the genie effect, the Dashboard ripple, the
    Core Image Fun House filters, Quartz Composer compositions, and Chess.app.
15. Capture each verified scene into the replay suite.

**Exit criterion**: 10.6.8 meets the success criteria in the Overview.

## Testing and debugging

Test at four levels, from pure functions to full guests. Then the level with
the lowest cost that can see a regression finds it.

| Level | What it covers | How |
| --- | --- | --- |
| Unit | GART translation, tiling, texture layout, format conversion | Random tests against the Mesa layout code, compiled into the test binary |
| Shader | PVS and R500 fragment decoding and translation | Disassembler round trip against the Mesa compiler. Differential tests of the CPU interpreter against Vulkan |
| Replay | Full command streams | Replay of captured streams without a display. Image diffs against approved PNGs |
| Guest | Real drivers from start to end | Piglit on Linux. Scripted Snow Leopard scenes with QMP `screendump` and perceptual diffs |

**Tools that are always on**

- QEMU trace events with register names, filtered for each subsystem.
- The polling-loop detector, and a report of unknown registers, sorted by
  frequency.
- A register coverage matrix: which registers each driver uses, and which
  registers you emulate.
- Vulkan validation layers in debug builds, and a build of QEMU with
  AddressSanitizer.
- RenderDoc captures of the backend when an image is incorrect but the command
  stream seems correct.

**Reference data when available**: `mmiotrace` captures from a real X1600
under Linux settle disagreements about the init sequence and the behavior of
status registers.

## Milestones, risks, and open questions

Expect approximately six to twelve months of part-time work before the result
is usable. Most of the schedule risk is in the fragment translator and the
Snow Leopard black-box phase.

**Milestones**

| # | Milestone | Phases | Test | Approximate effort |
| --- | --- | --- | --- | --- |
| M1 | Card enumerates and probes | 0–2 | `lspci` IDs and BARs correct. `radeon` sets up MC and GART | 2–4 weeks |
| M2 | Rings run | 3 | Linux ring and IB tests pass | 1–2 weeks |
| M3 | Linux console | 4 | KMS console from EDID, hardware cursor | 2–4 weeks |
| M4 | First 3D | 5–6 | `glxgears`, vertex-program piglit tests | 1–2 months |
| M5 | Fragment programs | 7 | Fragment-program and GLSL 1.20 piglit tests. Differential tests clean | 2–3 months |
| M6 | Full pipeline | 8 | Texturing, FBO, float-target, and query tests | 1–2 months |
| M7 | Sufficiently fast | 9 | 60 fps desktop, eager write-back off | 1 month |
| M8 | Snow Leopard desktop | 10 | Native resolution on the X1600 framebuffer | weeks to months |
| M9 | QE/CI Supported | 10 | System Profiler and the scene checks in the Overview | weeks |

**Risks**

- **Framebuffer and AtomBIOS init** can depend on behavior that has no
  documentation, or on properties from EFI that you cannot make again.
  Mitigation: get traces from real hardware and disassemble the kexts early.
- **Packets or registers that only Apple uses** possibly do not occur in a
  Linux or Mesa path. Mitigation: log unknown opcodes and disassemble the GL
  bundle.
- **Unstructured flow control** in Core Image shaders can make you use the
  slow program-counter fallback. Mitigation: measure with real captures before
  you make it faster.
- **Coherency bugs** cause corruption that occurs only sometimes and is
  difficult to make occur again. Mitigation: start with eager write-back, and
  keep it as a debug switch.
- **Licensing**: never distribute a VBIOS dump or Apple binaries. Apple's
  license for the Snow Leopard client does not permit virtualization. Its
  license for Snow Leopard Server does.

**Open questions**

- Does the 10.6.8 X1000 stack contain 64-bit kext code, or must the guest run
  the 32-bit kernel?
- Does the Apple framebuffer run AtomBIOS command tables, or does it use only
  the properties from EFI?
- Does the Apple driver use tiled scanout, Hyper-Z, or MSI?
- Which exact X1600 device ID gives the best match across all the X1000
  kexts?
- How much flow control do the fragment programs that Core Image makes use on
  this hardware?
