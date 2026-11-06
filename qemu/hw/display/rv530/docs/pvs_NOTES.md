# r5xx_pvs.c — R300/R500 vertex program (PVS) module notes

Files: `r5xx_pvs.h` (interface, not changed), `r5xx_pvs.c` (implementation),
`tests/pvs/` (tests), and this file.

## What the module does

**Decoder** (`r5xx_pvs_decode`): decodes the instruction range that
`VAP_PVS_CODE_CNTL_0` selects (FIRST_INST .. LAST_INST, 10-bit fields) from
the raw `code[]` store. Each instruction has 4 dwords: op/dst, src0, src1, and
src2, as the Mesa `r3xx_vertprog.c` writes them. The decoder decodes all fields
of the Mesa encoding:

* Op dword: opcode (6 bits), MATH_INST (bit 6), MACRO_INST (bit 7), dst
  register class (bits 8-11), dst offset (bits 13-19), write mask (bits
  20-23), VE_SAT (bit 24) / ME_SAT (bit 25), PRED_ENABLE (26), PRED_SENSE
  (27), dst ADDR_SEL (29-30), and dst ADDR_MODE_0 (31).
* Src dwords: register class (bits 0-1), ABS (bit 3), ADDR_MODE_0 =
  relative addressing (bit 4), offset (bits 5-12), four 3-bit swizzle
  selects (X/Y/Z/W/FORCE_0/FORCE_1), four negate bits (25-28), and ADDR_SEL
  (29-30). Swizzle selects 6 and 7 are "unused" in Mesa. They read as 0.0.
* Flow control: `VAP_PVS_FLOW_CNTL_OPC` (2 bits for each slot: NOP, JUMP,
  LOOP, or JSR), and `FLOW_CNTL_LOOP_INDEX` (init/step). For the addresses,
  R500 uses the `FLOW_CNTL_ADDRS_LW/UW` pairs (ACT_ADRS, LOOP_CNT_JMP_INST /
  LAST_INST, RTN_INST — 16-bit fields). R300 uses the packed
  `FLOW_CNTL_ADDRS` (8-bit fields). `R5xxPvsProgram.is_r500` selects the
  layout.
* `VAP_PVS_CONST_CNTL`: CONST_BASE_OFFSET (bits 0-7) and MAX_CONST_ADDR
  (bits 16-23).

The decoder fails (it returns NULL and a message) for these conditions:

* LAST < FIRST.
* LAST is after the end of the code store (256 lines on R300, 1024 on R500).
* Reserved opcodes (VE/ME > 28).
* Destination classes > 5.

**Interpreter** (`r5xx_pvs_run`): runs the decoded range for one vertex.

* Vector engine: all 29 opcodes (NO_OP, DOT_PRODUCT, MULTIPLY, ADD,
  MULTIPLY_ADD, DISTANCE_VECTOR, FRACTION, MAXIMUM, MINIMUM,
  SET_GREATER_THAN_EQUAL, SET_LESS_THAN, MULTIPLYX2_ADD, MULTIPLY_CLAMP,
  FLT2FIX_DX, FLT2FIX_DX_RND, the R500 PRED_SET_*_PUSH, COND_WRITE_*,
  COND_MUX_*, SET_GREATER_THAN, SET_EQUAL, SET_NOT_EQUAL), and the two macro
  ops (2CLK_MADD, 2CLK_M2X_ADD).
* Math engine: all 29 opcodes (EXP/LOG DX partials, EXP_BASEE, LIGHT_COEFF
  (LIT), POWER_FUNC and its clamp variants, RECIP/RECIP_SQRT DX/FF/IEEE,
  ME_MULTIPLY, EXP/LOG_BASE2_FULL, SIN, COS, LOG_BASE2_IEEE, and the R500
  PRED_SET_* family).
* Source modifiers: swizzle, including constant selects. ABS, which the
  interpreter applies before the negate of each lane. Negate for each lane.
  Destination modifiers: the write mask, and VE/ME saturate.
* Register classes: temporaries, alternate temporaries, inputs (also as a
  destination), constants, outputs, outputs with X replicated, and the address
  register. Any op can write the address register. The FLT2FIX ops are the
  usual way.
* Relative addressing for sources and destinations, through the
  `a0.{x,y,z,w}` lane that ADDR_SEL selects.
* Predication (PRED_ENABLE/PRED_SENSE) and the R500 predicate stack ops.
* Flow control: LOOP, JUMP, and JSR slots (see the semantics below).
* Outputs that the program does not write keep the values that the caller put
  in `out[]`.

**Disassembler** (`r5xx_pvs_disasm`): gives exactly the same bytes as the Mesa
`r300_vertex_program_dump()`, including the `Final vertex program code:`
header and the `Flow Control Ops:` section. Thus you can compare the listings
with diff. It has snprintf semantics: a NULL buffer is permitted, and it
returns the full length.

**GLSL emitter** (`r5xx_pvs_to_glsl`): `#version 450` Vulkan GLSL. For the
names, see "Differences from the header".

## Semantics that Mesa does not define (assumptions and approximations)

The Mesa encoder tells us only how the instructions are *encoded*. It does not
tell us what the hardware calculates. These are our choices:

* **ME input lane**: math-engine ops take their scalar from the swizzled
  **X lane** of src0. Mesa copies the scalar swizzle to all lanes, so you
  cannot see this choice in the Mesa output. Exceptions:
  * LIT reads N·L from src0.x, N·H from src1.x, and the power from src2.w.
    These are the lanes that the operand layout of the Mesa `ei_lit()`
    supplies.
  * POW reads the exponent from src2.x (Mesa puts it in src2).
  * ME_MULTIPLY multiplies src0.x by src1.x.
* **Predicate stack ops** follow the R600 ISA description of the same ops.
  R600 got them from R5xx. They also agree with the use in
  `radeon_vert_fc.c`. The predicate register holds a counter. 0 means
  active.
  * PRED_SET_cc: `dst = (src cc 0) ? 0 : 1, pred = (src cc 0)`.
  * PRED_SET_cc_PUSH (VE, for each lane, the predicate bit comes from the W
    lane): `dst = (src0 == 0 && src1 cc 0) ? 0 : src0 + 1`.
  * PRED_SET_POP: `dst = max(src - 1, 0), pred = dst == 0`.
  * PRED_SET_INV: 1 -> 0 (pred), 0 -> 1, other values do not change (no
    pred).
  * PRED_SET_CLR: `dst = FLT_MAX, pred = false`.
  * PRED_SET_RESTORE: `dst = src, pred = src == 0`.
  * When the predicate bit != PRED_SENSE, the interpreter skips predicated
    instructions fully. This includes the predicate ops.
* **COND_WRITE_cc**: for each lane, write src1 where `src0 cc 0`. Do not
  change the other lanes. **COND_MUX_cc**: `(src0 cc 0) ? src1 : src2`. The
  CMP lowering of Mesa uses this, with its operands in the opposite sequence.
* **VE_MULTIPLY_CLAMP**: `clamp(src0 * src1, 0, 1)`. This is a guess that we
  did not verify. R300 has no saturate modifier, so a multiply-with-clamp op
  is possible. Mesa never emits it.
* **FLT2FIX_DX** = floor, **FLT2FIX_DX_RND** = floor(x + 0.5). Writes to the
  address register convert with floor and clamp to ±2^20.
* **EXP (ME_EXP_BASE2_DX)** = (2^floor(x), x - floor(x), 2^x, 1).
  **LOG (ME_LOG_BASE2_DX)** = (floor(log2|x|), |x|/2^floor, log2|x|, 1), and
  (-inf, 1, -inf, 1) for 0. These are the ARB_vertex_program definitions.
* **LIT**: the ARB definition. The power is clamped to [-128, 128].
  z = pow(max(N·H,0), p), with C `powf` semantics for 0^0. Mesa first clamps
  N·H to 1e-19 to prevent the hardware behavior at N·H == 0. Thus you cannot
  see this case.
* **POWER_FUNC_FF** = pow(|a|, b), based on log2/exp2 as in the hardware.
  * _CLAMP_B clamps the base to >= 0.
  * _CLAMP_B1 clamps the base to [0,1].
  * _CLAMP_01 clamps the result.
  * RECIP_SQRT_DX/FF use |x| (DX RSQ). The IEEE variants do not.
  * LOG_BASE2_FULL_DX uses |x| and gives -inf for 0.
  * RECIP of 0 gives +inf.
* **SIN/COS** calculate `sinf/cosf` of the lane. On R500, Mesa first reduces
  the input to [-pi, pi] (`r300_transform_vs_trig_input`). The real hardware
  is accurate only in that range. We do not emulate its behavior outside that
  range.
* **Dual math op** (DST bits 27-28 without predication): the third source word
  carries a scalar ME op into an alternate temporary. See the note in
  `r5xx_pvs.c`. We made it from the lighting programs of Apple. It fixes
  Chess.
* **Relative constant reads** outside `[0, MAX_CONST_ADDR]` (before the
  interpreter adds the base) read as 0 (the DirectX rule). Only the
  1024-entry store limits absolute reads. The interpreter adds
  CONST_BASE_OFFSET to each constant index.
  * Note: Mesa sets MAX_CONST_ADDR = (number of constants - 1). Programs that
    `tools/mesa-rc/rc_compile` compiles do not declare external constants.
    Thus the test sets the register itself for the relative-addressing entry
    of the corpus.
* **Flow control**: the ACT_ADRS of a slot is the instruction after which the
  op occurs.
  * LOOP: at ACT, the loop counter gets LOOP_CNT and the loop index gets
    INIT. After LAST_INST, the counter decreases by one. If the counter is
    still > 0, execution continues at RTN_INST (index += STEP). This agrees
    with the Mesa emission (ACT = the NOP that Mesa emits for BGNLOOP,
    RTN = ACT + 1, LAST = the instruction before the ENDLOOP NOP). The body
    runs LOOP_CNT times.
  * JUMP: at ACT, continue at JMP_INST.
  * JSR: at ACT, record ACT+1 and continue at JMP_INST. After LAST_INST,
    return to the recorded address (RTN_INST if there is no recorded
    address).
  * JUMP and JSR are an educated guess. Mesa never emits them.
  * The interpreter keeps the loop index register for each slot, but
    instructions cannot read it. We did not find an encoding for it.
  * A step guard (2^20 instructions) stops programs that do not stop.
* The position-only pass fields (XYZW_VALID_INST in CODE_CNTL_0,
  LAST_VTX_SRC_INST in CODE_CNTL_1) and VAP_CNTL are scheduling hints for the
  hardware. The interpreter ignores them and runs the full FIRST..LAST range.
* Inputs after slot 15 read as 0. Outputs after slot 31 are discarded.

## Differences from the header

* The header suggests the names `vin[i]` / `vout[i]` for the GLSL interface.
  But a GLSL input array cannot have a `location` for each element. Also, a
  declaration of all 32 outputs is more than the minimum output budget of
  Vulkan. Thus the emitter declares these items:
  * `layout(location = i) in vec4 vin<i>;` for each input slot that the
    program uses.
  * `layout(location = i) out vec4 vout<i>;` for each output that the program
    writes. If the program writes an output through relative addressing, the
    emitter declares all 32 outputs.
  * `gl_Position = vout0`, as the header specifies.
  * Constants are
    `layout(std140, set = 0, binding = 0) uniform PvsConsts { vec4 c[1024]; }`,
    exactly as in the header.
* Flow control in GLSL: LOOP slots that are correctly nested become `for`
  loops. All other configurations (JUMP/JSR, overlapping loops) use a
  program-counter `switch` machine in a bounded loop.

## Known gaps

* We did not validate the semantics above that are guesses against hardware.
* There is no float24/denormal model. The interpreter uses host IEEE floats.
* `r5xx_pvs_run` allocates its execution context with calloc for each call
  (≈ 8 KB). Callers that run millions of vertices possibly need a variant with
  a pool.

## Tests (`make -C tests/pvs check`)

`pvs_test` runs these tests:

1. **Corpus round trip**:
   * `tests/pvs/corpus/*.rc` are programs in the text format of the Mesa
     compiler tests.
   * `corpus/*.out` is the output of
     `tools/mesa-rc/rc_compile --vs-r500|--vs-r300 --dump-hex`: machine code,
     the registers that r300_emit.c would set, the Mesa constants, and the
     Mesa `r300_vertex_program_dump()` listing.
   * The test makes the `R5xxPvsProgram` again from the code and registers.
     `r5xx_pvs_disasm` must give exactly the same bytes as the Mesa listing.
   * `make regen` makes the `.out` files again. It needs the tool, and the
     tool needs the Mesa sources.
2. **Corpus execution**: for each corpus program, an independent C reference
   calculates the expected outputs for 8 pseudo-random sets of inputs and
   constants. We wrote the reference from the *source* `.rc`, not from the
   machine code. The interpreter must agree within 1e-5 relative. Outputs that
   the program does not write must keep their sentinel. The programs are:
   * MVP and vector ops (R500 and R300 builds).
   * Math engine (LIT/EXP/LOG/RCP/RSQ/EX2/LG2/SIN/COS/POW).
   * Relative constant addressing (ARL).
   * R500 loop with a predicated break, and R300 loop.
   * Nested loops with breaks at both levels.
   * IF/ELSE/nested IF with CMP/SEQ/SNE/SGT/SLE/saturate.
   * The 2-clock MAD macro.
3. **Hand-encoded programs**, for the items that Mesa never emits. An
   assembler that copies the Mesa `PVS_OP_DST_OPERAND`/`PVS_SRC_OPERAND`
   macros makes them. They test:
   * SET_GT/EQ/NE, MULTIPLYX2_ADD, MULTIPLY_CLAMP, COND_WRITE, COND_MUX, the
     M2X_ADD macro, and VE/ME saturate.
   * Abs with negate for each lane, constant-select and unused swizzles,
     FLT2FIX_RND, alternate temporaries, output replicate-X, and an input-class
     destination.
   * Relative destinations and sources through each a0 lane, the constant
     base offset, and relative reads out of range.
   * The remaining ME ops, and each predicate op with both predicate senses.
   * LOOP with an explicit count, JUMP, JSR, and FIRST_INST != 0.
   * The R300 FC register layout, decoder error paths, NULL arguments, and a
     program that does not stop.
4. **GLSL**: the test emits each program above with `r5xx_pvs_to_glsl`. Then
   it validates the result with
   `glslangValidator -V --target-env vulkan1.3 -S vert`. The shaders and logs
   stay in `tests/pvs/build/`. If validation fails, the test fails.
