# r5xx_pvs.c — R300/R500 vertex program (PVS) module notes

Files: `r5xx_pvs.h` (interface, unchanged), `r5xx_pvs.c` (implementation),
`tests/pvs/` (tests), this file.

## What is implemented

**Decoder** (`r5xx_pvs_decode`): decodes the instruction range selected by
`VAP_PVS_CODE_CNTL_0` (FIRST_INST .. LAST_INST, 10-bit fields) from the
raw `code[]` store (4 dwords per instruction: op/dst, src0, src1, src2 as
Mesa's `r3xx_vertprog.c` writes them).  Every field of the Mesa encoding is
decoded:

* op dword: opcode (6 bits), MATH_INST (bit 6), MACRO_INST (bit 7), dst
  register class (bits 8-11), dst offset (bits 13-19), write mask (bits
  20-23), VE_SAT (bit 24) / ME_SAT (bit 25), PRED_ENABLE (26), PRED_SENSE
  (27), dst ADDR_SEL (29-30), dst ADDR_MODE_0 (31).
* src dwords: register class (bits 0-1), ABS (bit 3), ADDR_MODE_0 =
  relative addressing (bit 4), offset (bits 5-12), four 3-bit swizzle
  selects (X/Y/Z/W/FORCE_0/FORCE_1; 6 and 7 are Mesa's "unused" and read
  as 0.0), four negate bits (25-28), ADDR_SEL (29-30).
* flow control: `VAP_PVS_FLOW_CNTL_OPC` (2 bits per slot: NOP/JUMP/LOOP/
  JSR), the R500 `FLOW_CNTL_ADDRS_LW/UW` pairs (ACT_ADRS, LOOP_CNT_JMP_INST
  / LAST_INST, RTN_INST — 16-bit fields) or the R300 packed
  `FLOW_CNTL_ADDRS` (8-bit fields), and `FLOW_CNTL_LOOP_INDEX` (init/step).
  The layout is chosen by `R5xxPvsProgram.is_r500`.
* `VAP_PVS_CONST_CNTL`: CONST_BASE_OFFSET (bits 0-7) and MAX_CONST_ADDR
  (bits 16-23).

Decode fails (NULL + message) for LAST < FIRST, LAST beyond the code store
(256 lines on R300, 1024 on R500), reserved opcodes (VE/ME > 28), and
destination classes > 5.

**Interpreter** (`r5xx_pvs_run`): executes the decoded range for one vertex.

* Vector engine: all 29 opcodes (NO_OP, DOT_PRODUCT, MULTIPLY, ADD,
  MULTIPLY_ADD, DISTANCE_VECTOR, FRACTION, MAXIMUM, MINIMUM,
  SET_GREATER_THAN_EQUAL, SET_LESS_THAN, MULTIPLYX2_ADD, MULTIPLY_CLAMP,
  FLT2FIX_DX, FLT2FIX_DX_RND, the R500 PRED_SET_*_PUSH, COND_WRITE_*,
  COND_MUX_*, SET_GREATER_THAN, SET_EQUAL, SET_NOT_EQUAL) and the two macro
  ops (2CLK_MADD, 2CLK_M2X_ADD).
* Math engine: all 29 opcodes (EXP/LOG DX partials, EXP_BASEE, LIGHT_COEFF
  (LIT), POWER_FUNC and its clamp variants, RECIP/RECIP_SQRT DX/FF/IEEE,
  ME_MULTIPLY, EXP/LOG_BASE2_FULL, SIN, COS, LOG_BASE2_IEEE, and the R500
  PRED_SET_* family).
* Source modifiers: swizzle incl. constant selects, ABS (applied before the
  per-lane negate), per-lane negate; destination write mask; VE/ME saturate.
* Register classes: temporaries, alternate temporaries, inputs (also as a
  destination), constants, outputs, outputs with X replicated, address
  register (written through any op; FLT2FIX ops are the normal way).
* Relative addressing for sources and destinations through `a0.{x,y,z,w}`
  selected by ADDR_SEL.
* Predication (PRED_ENABLE/PRED_SENSE) and the R500 predicate stack ops.
* Flow control: LOOP, JUMP, and JSR slots (see semantics below).
* Outputs that are not written keep whatever the caller put in `out[]`.

**Disassembler** (`r5xx_pvs_disasm`): byte-identical to Mesa's
`r300_vertex_program_dump()` (including the `Final vertex program code:`
header and the `Flow Control Ops:` section) so listings can be diffed.
snprintf semantics (NULL buffer allowed, returns the full length).

**GLSL emitter** (`r5xx_pvs_to_glsl`): `#version 450` Vulkan GLSL; see
"Deviations from the header" for the naming.

## Semantics that are not pinned down by Mesa (assumptions / approximations)

Mesa's encoder only tells us how instructions are *encoded*, not what the
hardware computes.  The choices are:

* **ME input lane**: math-engine ops take their scalar from the swizzled
  **X lane** of src0 (Mesa replicates the scalar swizzle to all lanes so
  this is unobservable for Mesa output).  Exceptions: LIT reads N·L from
  src0.x, N·H from src1.x, and the power from src2.w — the lanes Mesa's
  `ei_lit()` operand layout provides; POW reads the exponent from src2.x
  (Mesa puts it in src2); ME_MULTIPLY multiplies src0.x by src1.x.
* **Predicate stack ops** follow the R600 ISA description of the same
  ops (R600 inherited them from R5xx) and match the way
  `radeon_vert_fc.c` uses them: the predicate register holds a counter,
  0 = active.  PRED_SET_cc: `dst = (src cc 0) ? 0 : 1, pred = (src cc 0)`;
  PRED_SET_cc_PUSH (VE, per lane, the predicate bit comes from the W lane):
  `dst = (src0 == 0 && src1 cc 0) ? 0 : src0 + 1`; PRED_SET_POP:
  `dst = max(src - 1, 0), pred = dst == 0`; PRED_SET_INV: 1 -> 0 (pred),
  0 -> 1, else unchanged (no pred); PRED_SET_CLR: `dst = FLT_MAX, pred =
  false`; PRED_SET_RESTORE: `dst = src, pred = src == 0`.  Predicated
  instructions (including predicate ops themselves) are skipped entirely
  when the predicate bit != PRED_SENSE.
* **COND_WRITE_cc**: per lane, write src1 where `src0 cc 0`, leave the
  other lanes untouched.  **COND_MUX_cc**: `(src0 cc 0) ? src1 : src2`
  (this is what Mesa's CMP lowering relies on, with its operands swapped).
* **VE_MULTIPLY_CLAMP**: `clamp(src0 * src1, 0, 1)`.  Unverified guess
  (R300 has no saturate modifier, so a multiply-with-clamp op is plausible);
  Mesa never emits it.
* **FLT2FIX_DX** = floor, **FLT2FIX_DX_RND** = floor(x + 0.5).  Writes to
  the address register convert with floor and clamp to ±2^20.
* **EXP (ME_EXP_BASE2_DX)** = (2^floor(x), x - floor(x), 2^x, 1);
  **LOG (ME_LOG_BASE2_DX)** = (floor(log2|x|), |x|/2^floor, log2|x|, 1), and
  (-inf, 1, -inf, 1) for 0 — the ARB_vertex_program definitions.
* **LIT**: ARB definition; power clamped to [-128, 128]; z = pow(max(N·H,0),
  p) with C `powf` semantics for 0^0 (Mesa works around the hardware's
  behavior at N·H == 0 by clamping to 1e-19 first, so it is unobservable).
* **POWER_FUNC_FF** = pow(|a|, b) (log2/exp2 based like the hardware);
  _CLAMP_B clamps the base to >= 0, _CLAMP_B1 to [0,1], _CLAMP_01 clamps the
  result.  RECIP_SQRT_DX/FF use |x| (DX RSQ), the IEEE variants do not.
  LOG_BASE2_FULL_DX uses |x| and gives -inf for 0.  RECIP of 0 gives +inf.
* **SIN/COS** compute `sinf/cosf` of the lane.  Mesa pre-reduces the input
  to [-pi, pi] on R500 (`r300_transform_vs_trig_input`); the real hardware
  is only accurate in that range and we do not emulate its behavior
  outside it.
* **Dual math op** (DST bits 27-28 without predication): the third source word carries a scalar ME op into an alt temp; see the note in `r5xx_pvs.c` (implemented from Apple's lighting programs; fixes Chess).
* **Relative constant reads** outside `[0, MAX_CONST_ADDR]` (before adding
  the base) read as 0 (DirectX rule); absolute reads are only bounded by the
  1024-entry store.  CONST_BASE_OFFSET is added to every constant index.
  Note: Mesa programs MAX_CONST_ADDR = (number of constants - 1); programs
  compiled by `tools/mesa-rc/rc_compile` do not declare external constants,
  so the test overrides the register for the relative-addressing corpus
  entry.
* **Flow control**: a slot's ACT_ADRS is the instruction after whose
  execution the op fires.  LOOP: at ACT the loop counter is loaded with
  LOOP_CNT and the loop index with INIT; after executing LAST_INST the
  counter is decremented and, if still > 0, execution continues at RTN_INST
  (index += STEP).  This matches Mesa's emission (ACT = the NOP emitted for
  BGNLOOP, RTN = ACT + 1, LAST = instruction before the ENDLOOP NOP) and
  runs the body LOOP_CNT times.  JUMP: at ACT continue at JMP_INST.  JSR:
  at ACT remember ACT+1 and continue at JMP_INST; after LAST_INST return to
  the remembered address (RTN_INST if none).  JUMP/JSR are an educated
  guess — Mesa never emits them.  The loop index register is kept per slot
  but cannot be read by instructions (no encoding for it was identified).
  A step guard (2^20 instructions) stops runaway programs.
* The position-only pass fields (XYZW_VALID_INST in CODE_CNTL_0,
  LAST_VTX_SRC_INST in CODE_CNTL_1) and VAP_CNTL are scheduling hints for
  the hardware and are ignored; the whole FIRST..LAST range is executed.
* Inputs beyond slot 15 and outputs beyond 31 read as 0 / are dropped.

## Deviations from the header

* The header suggests `vin[i]` / `vout[i]` naming for the GLSL interface.
  A GLSL input array cannot have per-element `location`s and declaring all
  32 outputs would exceed the Vulkan minimum output budget, so the emitter
  declares `layout(location = i) in vec4 vin<i>;` for each used input slot
  and `layout(location = i) out vec4 vout<i>;` for each written output
  (all 32 if an output is written through relative addressing).
  `gl_Position = vout0` as specified.  Constants are
  `layout(std140, set = 0, binding = 0) uniform PvsConsts { vec4 c[1024]; }`
  exactly as in the header.
* Flow control in GLSL: properly nested LOOP slots become `for` loops; any
  other configuration (JUMP/JSR, overlapping loops) falls back to a
  program-counter `switch` machine inside a bounded loop.

## Known gaps

* Semantics listed above as guesses are not validated against hardware.
* No float24/denormal modeling: the interpreter uses host IEEE floats.
* `r5xx_pvs_run` allocates its execution context with calloc per call
  (≈ 8 KB); callers running millions of vertices may want a pooled variant.

## Tests (`make -C tests/pvs check`)

`pvs_test` runs:

1. **Corpus round-trip**: `tests/pvs/corpus/*.rc` are programs in Mesa's
   compiler-test text format; `corpus/*.out` is the output of
   `tools/mesa-rc/rc_compile --vs-r500|--vs-r300 --dump-hex` (machine code,
   the registers r300_emit.c would program, Mesa's constants, and Mesa's
   `r300_vertex_program_dump()` listing).  The test rebuilds the
   `R5xxPvsProgram` from the code/registers and requires `r5xx_pvs_disasm`
   to reproduce Mesa's listing byte for byte.  `make regen` recreates the
   `.out` files (needs the tool, which needs the Mesa sources).
2. **Corpus execution**: for every corpus program an independent C reference
   written from the *source* `.rc` (not the machine code) computes the
   expected outputs for 8 pseudo-random input/constant sets; the
   interpreter must match within 1e-5 relative, and unwritten outputs must
   keep their sentinel.  Programs: MVP + vector ops (R500 and R300 builds),
   math engine (LIT/EXP/LOG/RCP/RSQ/EX2/LG2/SIN/COS/POW), relative constant
   addressing (ARL), R500 loop with predicated break, R300 loop, nested
   loops with breaks at both levels, IF/ELSE/nested IF + CMP/SEQ/SNE/SGT/
   SLE/saturate, 2-clock MAD macro.
3. **Hand-encoded programs** (an assembler mirroring Mesa's
   `PVS_OP_DST_OPERAND`/`PVS_SRC_OPERAND` macros) for what Mesa never emits:
   SET_GT/EQ/NE, MULTIPLYX2_ADD, MULTIPLY_CLAMP, COND_WRITE, COND_MUX,
   M2X_ADD macro, VE/ME saturate, abs + per-lane negate, constant-select and
   unused swizzles, FLT2FIX_RND, alternate temporaries, output replicate-X,
   input-class destination, relative destinations/sources through every
   a0 lane, constant base offset and out-of-range relative reads, the
   remaining ME ops, every predicate op with both predicate senses, LOOP
   with an explicit count + JUMP + JSR + FIRST_INST != 0, the R300 FC
   register layout, decoder error paths, NULL arguments, a runaway program.
4. **GLSL**: every program above is emitted with `r5xx_pvs_to_glsl` and
   validated with `glslangValidator -V --target-env vulkan1.3 -S vert`
   (shaders and logs are left in `tests/pvs/build/`).  A validation failure
   fails the test.
