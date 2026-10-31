# R5xx GPU Emulation for Snow Leopard QE/CI

This is the original design plan.  Phases 0-10 have since been carried out
(Snow Leopard runs with Quartz Extreme and Core Image, with fragment work on
Vulkan); for the current state read `docs/STATUS.md`, and for running it
`snowleopard/README.md`.

## Overview

Build a QEMU device model of an ATI Radeon X1600 (RV530, R5xx family) whose 3D engine is translated to Vulkan, so Snow Leopard's unmodified ATI X1000 driver stack reports Quartz Extreme and Core Image as Supported on an x86\_64 KVM guest, with no GPU passthrough.

The approach mirrors ppcosxkvm, which did the same for Tiger on PowerPC with an emulated Radeon 9700 (R300). No guest kexts or GL bundles are written: Apple's drivers run as shipped, and all work happens in the emulated hardware.

The guest talks to the emulated card exactly as to real hardware; the 3D and shader translation is the largest new component.

**Success criteria**

1. A 10.6.8 guest's System Profiler lists the emulated X1600 with Quartz Extreme and Core Image both Supported.
2. Dock effects, Dashboard's ripple, and Core Image Fun House filters render correctly.
3. A Linux guest using the in-kernel `radeon` driver and Mesa's r300 driver passes an agreed piglit subset.
4. The desktop is interactive at native resolution on the host GPU.

**Working rules**

- Bring every feature up on a Linux guest first, where the driver is open source; move to Snow Leopard only when Linux is correct.
- Trace every register access and command packet from day one.
- Correctness before speed: a CPU reference path exists for each translator before the Vulkan path is tuned.

## Prerequisites

You need a Linux host with a Vulkan 1.3 driver, the reference material below, and comfort with QEMU device code, Vulkan, and shader compilation.

**Skills**

- C in QEMU's style: QOM devices, MemoryRegions, bottom halves, trace events.
- Vulkan: pipelines, descriptors, render passes or dynamic rendering, SPIR-V (or GLSL compiled through shaderc).
- GPU register-level concepts: command rings, GART, tiling, scanout.
- Booting legacy OS X on QEMU with OVMF and OpenCore.

**Hardware**

- Host: any Linux machine with a modern Vulkan driver and KVM. A recent NVIDIA or AMD GPU is fine.
- Optional but valuable: a real X1600 (2006 iMac or MacBook Pro, or a PCIe X1600 in a Linux PC). It gives you a VBIOS dump for private testing and `mmiotrace` captures as ground truth.

**Reference material to gather**

| Source | What it gives you |
| --- | --- |
| AMD R5xx Acceleration and R5xx display register docs (published for the X.org project, 2007–2008) | Register meanings for the 3D engine and AVIVO display |
| Linux `drivers/gpu/drm/radeon` (`rv515.c`, `r520.c`, `r300.c`, `rv370` PCIe GART code, `atom.c`, `atombios_*.c`, `rv515d.h`, `r500_reg.h`, `avivod.h`) | Init sequences, register names, GART entry format, AtomBIOS interpreter |
| Mesa `src/gallium/drivers/r300` and its `compiler/` directory (`r500_fragprog_emit.c`, `r3xx_vertprog.c`, `r300_texture_desc.c`) | Exact shader encodings, texture layout, and tiling rules |
| `xf86-video-ati` (older releases with AVIVO and EXA code) | Display setup and 2D engine usage |
| Upstream QEMU `hw/display/ati.c` and `ati_2d.c` | Structure for an ATI PCI device and 2D blit emulation |
| QEMU `hw/display/i2c-ddc.c` and `hw/i2c/bitbang_i2c.c` | Ready-made EDID over bit-banged DDC |
| ppcosxkvm and its QEMU fork (R300 work from the poweremu-qemu forks) | R300 command processor, PVS, and fragment translation to Metal and Vulkan |
| LegacyOSXKVM / DarwinKVM | A known-good OVMF + OpenCore setup for Snow Leopard on QEMU |

**Guest media**: Snow Leopard 10.6.3 retail plus the 10.6.8 combo update, and a Linux distribution that still ships Mesa's r300 driver.

## Phase 0: Base, layout, and tracing

Start from ppcosxkvm's QEMU fork so you inherit its R300 command processor and Vulkan backend, then build tracing before any device logic.

1. Clone ppcosxkvm's QEMU fork at its pinned commit. Confirm `qemu-system-ppc` builds and runs Tiger with its Vulkan backend on your host, so you know the backend works.
2. Enable the `x86_64-softmmu` target with KVM in the same tree.
3. Move the reusable R300 pieces (PM4 parser, PVS decoder, blend/depth state mapping, Vulkan device, and pipeline cache) into a shared directory, for example `hw/display/radeon-common/`, so both the PPC and x86 devices link them.
4. Create the new device directory with one file per subsystem:
   - `rv530.c` – PCI device, register file, reset
   - `r5xx_mc.c` – memory controller, VRAM, PCIe GART
   - `r5xx_cp.c` – ring, indirect buffers, fences, interrupts
   - `r5xx_avivo.c` – display, cursor, vblank, DDC
   - `r5xx_3d.c` – 3D state cache and draw dispatch
   - `r5xx_pvs.c` and `r500_us.c` – vertex and fragment shader decoders and translators
   - `r5xx_tex.c` – formats, layout, tiling
   - `r5xx_vk.c` – Vulkan backend glue
   - `r5xx_regnames.c` – generated register-name table
   - `trace-events`
5. Generate the register-name table with a script that parses `r500_reg.h`, `rv515d.h`, and `avivod.h` from the Linux tree. Every log line should print names, not offsets.
6. Add trace events for every MMIO read and write, every PM4 packet, every draw, and every unknown register.
7. Add a capture mode that records the command stream plus the memory ranges it references to a file, and a headless replay tool that runs a capture through the Vulkan backend and writes PNGs. This becomes your regression suite.
8. Add a polling-loop detector: log when the same register is read more than a few hundred times in a row with an unchanged value. During driver bring-up this pinpoints the status bit you are not emulating.

**Exit criterion**: the empty device builds into `qemu-system-x86_64`, and replay of an existing R300 capture from the PPC side still renders.

## Phase 1: PCIe device skeleton

The device must enumerate with an ID Apple's kexts match, expose the BAR layout Linux's `radeon` driver expects for R5xx, and answer status registers as an idle GPU.

1. **Pick the device ID from the kexts, not from memory.** On an installed 10.6.8 system, list every kext whose `IOPCIMatch` contains an ATI ID in the 0x71xx range: `grep -l '0x71' /System/Library/Extensions/*/Contents/Info.plist`. Note which kexts match (framebuffer, `ATIRadeonX1000.kext`) and choose an X1600 ID present in all of them. The Mobility X1600 in early Intel Macs is believed to be 1002:71C5; verify it against the plists and Linux's `radeon_pciids`.
2. **Set an Apple subsystem vendor (106B)** and a subsystem ID copied from a real X1600 Mac's `ioreg` dump if you can find one.
3. **Register a PCIe device** with a PCIe capability, intended to sit behind a root port on the `q35` machine.
4. **Expose the BARs** the R5xx driver path expects:
   - BAR0: 64-bit prefetchable VRAM aperture, 256 MB, backed by a RAM MemoryRegion.
   - BAR2: register MMIO, 64 KB (Linux maps R5xx registers from BAR2).
   - BAR4: a small I/O BAR for legacy index/data access.
   - Expansion ROM BAR: the VBIOS image (Phase 4).
5. **Build the register file** as a flat 64 KB array with per-register read and write hooks. Unknown registers store the written value, read it back, and log once.
6. **Implement indirect access** through `MM_INDEX`/`MM_DATA` at offsets 0x0/0x4, plus the MC and PCIe indirect index/data pairs (Phase 2).
7. **Report idle and size**: `RBBM_STATUS` with the GUI-active bit clear, and `CONFIG_MEMSIZE` equal to the VRAM size.
8. **Implement legacy INTx first.** Add MSI later only if a trace shows a driver enabling it.

**Exit criterion**: a Linux guest's `lspci -vv` shows the card with the right IDs and BARs, and `radeon` probes it and reads the memory size before failing further along.

## Phase 2: Memory controller, VRAM, and GART

Every GPU access goes through one function that turns a GPU (MC) address into host memory, covering VRAM and the PCIe GART.

1. **Model the MC address space.** The driver places VRAM and the GART window in the GPU's own address space through MC registers reached via the MC indirect index/data pair. Take the exact register indices for RV530 from `rv515d.h` and `rv515.c`; RV515, RV530, and R520 differ here, so match one chip exactly.
2. **Report MC idle.** Drivers poll an MC status register before reprogramming locations; return idle unconditionally.
3. **Implement the PCIe GART** used by RV515/RV530 (the RV370-style GART in Linux's `rv370_pcie_gart_*` functions), reached through the PCIe index/data pair:
   - Latch the GART base, start, and end registers and the control register.
   - Page-table entries live in VRAM or system memory; decode them with the entry format in `rv370_pcie_gart_set_page`.
   - Honor the TLB invalidate bit by dropping any cached translations.
4. **Write the translator**: `r5xx_mc_rw(dev, mc_addr, buf, len, is_write)` returns VRAM bytes for addresses in the framebuffer window, walks the GART for the GART window, and logs anything else as a fault.
5. **Add a fast path** that returns a host pointer for a contiguous range when it lies entirely in VRAM or within one GART page, for the command processor and vertex fetch.

**Exit criterion**: unit tests map random GART pages and read them back through the translator; the Linux driver programs MC locations and the GART without timing out.

## Phase 3: Command processor

R5xx keeps the R300 command processor and PM4 packet format, so most of this phase is porting the shared R300 code onto the new device and threading model.

1. **Accept microcode uploads and ignore them.** Drivers load CP microcode (Linux uses `radeon/R520_cp.bin`; Apple ships its own) through the CP microcode RAM registers. Emulate the CP at a high level and only log a hash of what was uploaded.
2. **Implement the ring**: ring base, size (log2 in the ring control register), read and write pointers, and the read-pointer write-back address. Mirror RPTR into guest memory whenever it advances.
3. **Run the CP on its own thread.** A write to the write pointer wakes the thread; vCPU MMIO never blocks on GPU work. While work is pending, `RBBM_STATUS` must report busy, because drivers poll it.
4. **Parse PM4 packets**:
   - Type 0: register writes into the same register file MMIO uses, including the one-register-write mode.
   - Type 2: padding.
   - Type 3: draws (immediate, vertex-buffer, and indexed), vertex array pointer loads, index buffers, wait-for-idle, NOP, and the Hyper-Z clear packets. Take opcode values from Mesa's `r300_reg.h` and Linux's `r300d.h`.
5. **Execute indirect buffers.** On R300-family parts the ring writes the IB base and size registers; your write hook for the size register runs the referenced buffer through the same parser.
6. **Implement fences and scratch registers**: scratch writes, scratch write-back to memory when enabled, and the software interrupt.
7. **Implement interrupts** through `GEN_INT_CNTL`/`GEN_INT_STATUS` for the CP, and the AVIVO display interrupt registers (Phase 4) for vblank.
8. **Log every unhandled type-3 opcode** with its payload. Apple's driver may use packets that Linux and Mesa never emit.

**Exit criterion**: the Linux `radeon` driver reports its ring test and IB test as succeeded.

## Phase 4: AVIVO display, VBIOS, and firmware console

R5xx replaced the legacy CRTC with the AVIVO display engine; this phase is new code, and the VBIOS is its highest-risk part.

**Scanout**

1. Implement the D1 display controller's graphics surface registers: enable, primary surface address, pitch, X/Y start and end, pixel format, and the viewport registers. Take names from `avivod.h` and `r500_reg.h`; D2 can be stubbed.
2. On each display refresh, read the surface through the MC translator, convert the pixel format, and hand it to QEMU's console. Use a zero-copy DisplaySurface when the surface is linear 32-bit XRGB with a matching stride.
3. Handle tiled scanout surfaces if a trace shows the driver enabling tiling on the front buffer.
4. Implement the gamma/LUT registers, or pass pixels through unchanged at first.

**Timing and interrupts**

5. Run a 60 Hz timer that sets the vblank status bit, raises the display interrupt when enabled, and advances the CRTC position counter. Drivers poll both.
6. Accept writes to the CRTC timing registers and derive the mode from them.

**Cursor and outputs**

7. Map the hardware cursor registers (surface address, size, position, hotspot) to QEMU's cursor-define and mouse-set calls.
8. Pick one output, DVI (TMDS). Accept encoder, transmitter, PLL, and DAC writes, and make status reads report locked, done, and connected.
9. Wire the DDC GPIO pins to QEMU's bit-banged I2C with an `i2c-ddc` EDID device, so both Linux and OS X read a real EDID.

**VBIOS (AtomBIOS)**

10. Expose a ROM through the expansion ROM BAR. Two options:
    - Private testing: a dump from a real X1600 you own. Do not redistribute it.
    - Shippable: a synthetic AtomBIOS image with valid headers, data tables describing one TMDS connector, firmware info, and PLL limits, and command tables that only return.
11. Trace ROM BAR reads and AtomBIOS table execution to see which tables each driver actually uses. Linux runs ASIC init only when the card looks unposted, so report it as posted (display already enabled, memory size set).
12. Find out empirically whether Apple's framebuffer executes AtomBIOS command tables or relies on properties injected by EFI. That answer decides how much of the VBIOS you must build.

**Firmware console**

13. OVMF has no GOP driver for this card. Write a small EDK2 DXE driver, loaded by OpenCore, that programs one fixed AVIVO mode and publishes a linear-framebuffer GOP. Without it, `boot.efi` has nothing to draw on before the kexts load.

**Exit criterion**: Linux `radeon` KMS lights a native-resolution console from EDID, with a working hardware cursor in X.

## Phase 5: Linux guest bring-up

The Linux guest is your debuggable reference: every 3D feature is proven here before Snow Leopard touches it.

1. Install a distribution whose kernel includes `radeon` and whose Mesa still builds the r300 Gallium driver, plus `linux-firmware` for `R520_cp.bin`.
2. Boot with `drm.debug=0x1e` and capture the kernel log over a serial console.
3. Bring up in this order, fixing each before moving on:
   1. Driver probe, MC, and GART init.
   2. Ring test and IB test.
   3. KMS console at the EDID resolution.
   4. A display server running; `glxinfo` reporting the r300 driver.
   5. `glxgears`, then simple textured and blended demos.
   6. Piglit: start with `ARB_vertex_program`, `ARB_fragment_program`, and basic texturing tests, then widen.
4. Keep Mesa's Hyper-Z off at first; the r300 driver enables it only on request (the `RADEON_HYPERZ` environment variable).
5. Use Mesa's `RADEON_DEBUG` options to print the vertex and fragment programs it compiles, and diff them against your decoder's disassembly of the same binaries.
6. Capture every passing scene with the Phase 0 capture tool and add it to the replay regression suite.

**Exit criterion**: a desktop session runs on the emulated card and the chosen piglit subset passes.

## Phase 6: 3D state, vertex fetch, and vertex shaders

Draw packets turn the latched 3D registers into one Vulkan draw; the R500 vertex engine extends R300's, so reuse the shared PVS decoder and add R500's extras.

**State**

1. Latch all 3D register blocks (VAP, GA, SU, RS, US, TX, ZB, RB3D, FG, SC) into a state struct with dirty flags per group.
2. On a draw packet, snapshot the state into a draw record: primitive type, vertex count, index buffer, and vertex streams.
3. Hash the pipeline-relevant state and cache `VkPipeline` objects by that hash; persist a `VkPipelineCache` to disk.

**Vertex fetch**

4. Decode the vertex stream setup (the `VAP_PROG_STREAM_CNTL` registers and their extended swizzle registers) and the array pointers from the load-vertex-pointers packet.
5. Use vertex pulling: bind guest vertex memory as a storage buffer and fetch, convert, and swizzle in the generated vertex shader. This handles every format and swizzle without fighting Vulkan's vertex input limits.
6. Support immediate-mode vertices by copying the packet payload into a host buffer.

**Vertex shaders (PVS)**

7. Capture PVS code and constants written through the PVS vector index/data registers, and the code-control registers that set the program range.
8. Extend the shared R300 decoder for R500: more temporaries and constants, flow-control instructions, and any new opcodes. Mesa's `r3xx_vertprog.c` and register headers define the encodings.
9. Handle PVS bypass mode, where vertices arrive already transformed.
10. Apply the viewport transform (the VAP viewport scale and offset registers and the VTE control register) inside the generated shader so it matches hardware exactly; set the Vulkan viewport to cover the render target.
11. Map PVS outputs to fragment inputs through the rasterizer's interpolator setup (the RS registers).

**Rasterization**

12. Map cull mode, polygon offset, point size, line width, scissor, and polygon mode to Vulkan state. Implement clip rectangles and their combine rule as fragment discards if Vulkan state cannot express them.
13. Build a CPU reference interpreter for PVS programs and compare its outputs with the GPU path on captured draws.

**Exit criterion**: piglit vertex-program tests and `glxgears` render identically to the CPU reference path.

## Phase 7: R500 fragment shader translator

This is the core of Core Image support and the largest single piece of new code: the R500 fragment ISA shares almost nothing with R300's, so plan to write it from scratch against Mesa's encoder.

**Know the ISA**

- Programs hold up to 512 instructions of six dwords each, uploaded with constants through the `GA_US_VECTOR_INDEX`/`GA_US_VECTOR_DATA` pair.
- Instruction types: ALU (a vector RGB operation paired with a scalar alpha operation, each with its own source selection, swizzles, modifiers, and pre-subtract), output writes, texture (sample, projected sample, LOD bias, kill, derivative-supplied sample), and flow control (jumps, loops, calls, and returns, predicate conditions).
- R500 fragment arithmetic is FP32 (R300's was FP24), which maps cleanly onto Vulkan `float`.

**Build order**

1. **Disassembler first.** Build Mesa's r300 compiler as a standalone tool, compile a corpus of ARB fragment programs and GLSL 1.20 shaders to R500 binaries, and require your disassembler to decode them back to Mesa's own listing.
2. **CPU reference interpreter.** Execute decoded programs on the CPU, one pixel at a time. It is slow but definitive, and it debugs every later stage.
3. **Lift to an IR** of vec4 temporaries, inputs, constants, and outputs, with explicit predicates.
4. **Emit GLSL and compile with shaderc** at first; switch to direct SPIR-V emission only if compile time hurts. Cache compiled modules by program hash, on disk.
5. **Control flow**: translate structured loops and branches directly. Fall back to a program-counter loop with a `switch` for anything unstructured. Check Core Image captures early to see how much flow control Apple's compiler actually emits.
6. **Transcendentals and special ops**: match hardware domains exactly (for example, the input scaling of SIN and COS, and RCP/RSQ of zero). Use Mesa's lowering code as the reference for each.
7. **Texture instructions** map to combined image samplers bound per draw; implement kill as `discard` and projected sampling explicitly.
8. **Outputs**: map color outputs to up to four render targets using the US output format registers, and handle depth writes.
9. **Differential testing**: run every captured program through both the CPU interpreter and the Vulkan path on random inputs and compare within a tight tolerance.

**Why Core Image stresses this**: Core Image compiles its kernels at runtime into GPU fragment programs, so expect many long, varied shaders with heavy texture use and constant shader-cache churn.

**Exit criterion**: piglit ARB fragment program and GLSL 1.20 fragment tests pass on Linux, and differential tests agree on the whole capture corpus.

## Phase 8: Textures, render targets, blending, Hyper-Z, and queries

The rule for this phase: compute every surface layout exactly as Mesa's r300 driver does, and keep compression invisible by storing surfaces uncompressed on the host.

**Textures**

1. Decode the texture unit registers (filter, format, offset with tiling bits, border color, enable) into a sampler and image description.
2. Map formats to `VkFormat` plus a component mapping. DXT1/3/5 map straight to BC1–3; convert anything Vulkan lacks in a compute shader.
3. Replicate `r300_texture_desc.c` for mip, cube, and 3D layout, alignment, and non-power-of-two rules. A one-byte layout error corrupts every later mip.
4. Implement micro and macro tiling as compute-shader detile and retile passes. Unit-test them against Mesa's surface code with random sizes.

**Render targets and depth**

5. Decode color buffer offsets, pitches, formats, and tiling (the RB3D color registers) for up to four targets, plus the depth buffer registers (offset, pitch, format, depth/stencil control).
6. Support FP16 and FP32 color targets; Core Image depends on floating-point surfaces.
7. Track which guest ranges are render targets so later texture reads of the same memory see rendered results (Phase 9).

**Fixed-function per-pixel state**

8. Map the blend registers (color and alpha blend control, constant color, color mask) to Vulkan blend state, and raster operations to `logicOp`.
9. Implement alpha test and fog as a generated epilogue in the fragment shader.

**Hyper-Z**

10. Treat Z-mask and HiZ clears semantically: perform a real Vulkan depth clear to the clear value and ignore the HiZ data, which only affects performance.
11. Store depth uncompressed on the host. If the guest ever reads a depth buffer as a texture or via the CPU, supply uncompressed values.
12. Accept Z-cache flush and control writes and report them complete.

**2D engine and queries**

13. Implement the legacy 2D engine's solid fill, screen-to-screen copy, and host-data blits, reusing upstream QEMU's `ati_2d.c`. Start in software.
14. Implement occlusion queries: when the driver sets the Z-pass result address, run a Vulkan occlusion query and write the count into guest memory at the next sync point.

**Exit criterion**: piglit texturing, FBO, blending, and occlusion-query tests pass, including floating-point targets.

## Phase 9: VRAM coherency and performance

The guest CPU writes VRAM directly while host Vulkan images hold the GPU's view of the same memory; tracking which copy is current decides both correctness and speed.

**Resource tracking**

1. Keep an interval map from guest memory ranges (VRAM and GART) to host resources, each in one of three states: CPU copy current, GPU copy current, or both.
2. Before a draw reads a range whose CPU copy is newer, upload it (detiling as needed).
3. After the GPU writes a range, mark the GPU copy current.
4. Write GPU results back to guest memory at sync points: fences, scratch writes, wait-for-idle, and before scanout reads a surface.

**Detecting CPU writes**

5. Enable dirty-page logging on the VRAM region and use it to find CPU-modified pages cheaply.
6. Start with eager write-back of every GPU-written range at each fence. It is simple and correct.
7. Then optimize: write back only resources the guest has been seen reading by CPU, and consider write-protecting GPU-current pages to catch reads on demand.

**Zero-copy where possible**

8. Allocate VRAM as host memory that Vulkan imports (`VK_EXT_external_memory_host`), so linear vertex, index, and constant data needs no copy. Tiled surfaces still go through conversion.
9. When the scanout surface is a GPU render target, present the Vulkan image directly through QEMU's dmabuf/GL display path instead of reading it back.

**Throughput**

10. Batch draws into one command buffer per sync point; submit once per fence.
11. Persist pipeline and shader caches across runs.
12. Profile with `perf` on the QEMU process and RenderDoc's in-application capture API on the backend.

**Exit criterion**: the Linux desktop and replay suite hold 60 fps at native resolution with eager write-back disabled, and no regression in the replay image diffs.

## Phase 10: Snow Leopard bring-up

Only start here once Linux passes Phases 5–9; from now on the driver is a black box, and your traces are the only window into it.

**Base VM**

1. Start from a known-good LegacyOSXKVM/DarwinKVM Snow Leopard setup on `q35` with OVMF and OpenCore, using its default display first. Update to 10.6.8.
2. Inventory the ATI X1000 stack on the installed system: the framebuffer kext, `ATIRadeonX1000.kext`, and the user-space bundles in `/System/Library/Extensions` (GL driver, GA plugin, VA decoder). Record which IDs each matches.
3. Run `lipo -info` on the kext binaries. If they are 32-bit only, boot the guest with the 32-bit kernel (`arch=i386`).
4. Choose SMBIOS for a Mac that shipped with an X1600 (an early Intel iMac or MacBook Pro) in case driver behavior keys off the model.

**Attach the card**

5. Add the emulated X1600 behind a root port at a fixed slot, replacing the default display, and load the Phase 4 GOP driver through OpenCore.
6. Inject device properties for its PCI path through OpenCore `DeviceProperties`. Real Macs received ATI properties from EFI (names, connector descriptions, VRAM size, device-type entries). Build the set from a real X1600 Mac's `ioreg` dump if you can find one, and from the property lists in historical injector projects such as Natit and ATIinject.
7. Boot verbose with `-v keepsyms=1` and capture serial output.

**Debug the black box**

8. Compare the Apple driver's MMIO and PM4 trace with Linux's for the same phase. Differences point to registers and packets to implement.
9. Watch the polling-loop detector: a stall usually means one missing status bit.
10. Disassemble the kexts and GL bundle (Ghidra or Hopper); they export C++ symbols, which makes init sequences and register use readable.
11. For deeper work, attach QEMU's gdbstub to the guest kernel with the 10.6.8 Kernel Debug Kit symbols.
12. Use `ioreg -lw0` to confirm which drivers attached, and OpenGL Driver Monitor (from the Developer Tools) to watch driver statistics.

**Verify QE/CI**

13. `system_profiler SPDisplaysDataType` shows the X1600 with Quartz Extreme and Core Image Supported.
14. Check Dock magnification and the genie effect, Dashboard's ripple, Core Image Fun House filters, Quartz Composer compositions, and Chess.app.
15. Capture each verified scene into the replay suite.

**Exit criterion**: the success criteria in the Overview are met on 10.6.8.

## Testing and debugging

Test at four levels, from pure functions up to whole guests, so a regression is caught at the cheapest level that can see it.

| Level | What it covers | How |
| --- | --- | --- |
| Unit | GART translation, tiling, texture layout, format conversion | Randomized tests against Mesa's layout code compiled into the test binary |
| Shader | PVS and R500 fragment decoding and translation | Disassembler round-trip against Mesa's compiler; CPU interpreter vs Vulkan differential tests |
| Replay | Whole command streams | Captured streams replayed headless; image diffs against approved PNGs |
| Guest | Real drivers end to end | Piglit on Linux; scripted Snow Leopard scenes with QMP `screendump` and perceptual diffs |

**Always-on tools**

- QEMU trace events with register names, filtered per subsystem.
- The polling-loop detector and an unknown-register report sorted by frequency.
- A register coverage matrix: which registers each driver touches, and which you implement.
- Vulkan validation layers in debug builds, and an AddressSanitizer build of QEMU.
- RenderDoc captures of the backend when an image is wrong but the command stream looks right.

**Ground truth when available**: `mmiotrace` captures from a real X1600 under Linux settle disputes about init order and status-register behavior.

## Milestones, risks, and open questions

Expect roughly six to twelve months of part-time work to a usable result; the fragment translator and the Snow Leopard black-box phase carry most of the schedule risk.

**Milestones**

| # | Milestone | Phases | Test | Rough effort |
| --- | --- | --- | --- | --- |
| M1 | Card enumerates and probes | 0–2 | `lspci` IDs and BARs correct; `radeon` sets up MC and GART | 2–4 weeks |
| M2 | Rings run | 3 | Linux ring and IB tests pass | 1–2 weeks |
| M3 | Linux console | 4 | KMS console from EDID, hardware cursor | 2–4 weeks |
| M4 | First 3D | 5–6 | `glxgears`, vertex-program piglit tests | 1–2 months |
| M5 | Fragment programs | 7 | Fragment-program and GLSL 1.20 piglit tests; differential tests clean | 2–3 months |
| M6 | Full pipeline | 8 | Texturing, FBO, float-target, and query tests | 1–2 months |
| M7 | Fast enough | 9 | 60 fps desktop, eager write-back off | 1 month |
| M8 | Snow Leopard desktop | 10 | Native resolution on the X1600 framebuffer | weeks to months |
| M9 | QE/CI Supported | 10 | System Profiler and the Overview's scene checks | weeks |

**Risks**

- **Framebuffer and AtomBIOS init** may depend on undocumented behavior or EFI-injected properties you cannot reconstruct. Mitigation: real-hardware traces and kext disassembly early, not late.
- **Apple-only packets or registers** may not appear in any Linux or Mesa path. Mitigation: unknown-opcode logging and disassembly of the GL bundle.
- **Unstructured flow control** in Core Image shaders could force the slow program-counter fallback. Mitigation: measure against real captures before optimizing.
- **Coherency bugs** cause intermittent corruption that is hard to reproduce. Mitigation: start with eager write-back and keep it as a debug switch.
- **Licensing**: never distribute a VBIOS dump or Apple binaries. Snow Leopard client's license does not permit virtualization; Snow Leopard Server's does.

**Open questions**

- Does the 10.6.8 X1000 stack include 64-bit kext code, or must the guest run the 32-bit kernel?
- Does Apple's framebuffer execute AtomBIOS command tables, or rely only on EFI-injected properties?
- Does Apple's driver use tiled scanout, Hyper-Z, or MSI?
- Which exact X1600 device ID gives the cleanest match across all the X1000 kexts?
- How much flow control do Core Image's generated fragment programs use on this hardware?
