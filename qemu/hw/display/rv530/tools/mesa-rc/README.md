# rc_compile — Mesa r300/r500 shader compiler as a command-line tool

`rc_compile` puts the Mesa r300 Gallium shader compiler in a command-line
tool. This is the `rc_*` / `r3xx_compile_*` code under
`src/gallium/drivers/r300/compiler/`. The tool has two purposes:

* It compiles test shaders to R300/R400/R500 machine code without a GPU.
* The disassembly from Mesa is the reference for the fragment and vertex
  program decoders of the rv530 emulator.

This tree does not contain a copy of any Mesa file. The Makefile compiles
these files where they are in `reference/mesa` (see
scripts/fetch-references.sh): the compiler, its unit-test parser
(`tests/rc_test_helpers.c`), and five small `src/util` files. To use a
different Mesa tree, use `make MESA=...`. Only the files in this directory are
ours:

    Makefile              build + `make check`
    rc_compile.c          the driver (parsing glue, compiler setup, printers)
    stubs/mesa_stubs.c    3 util symbols the compiler needs (see below)
    stubs/util/u_bitcast.h  replacement for a header absent from this checkout
    samples/*.rc          sample programs used by `make check`

## Build

    make                      # -> ./rc_compile   (gcc 13, -std=gnu11)
    make check                # compiles every samples/*.rc and prints the output
    make clean

This is the exact compile line for the Mesa sources (see the Makefile):

    gcc -std=gnu11 -O2 -g -Wall -Wno-unused-parameter -Istubs \
        -I$MESA/src -I$MESA/include -I$MESA/src/gallium/include \
        -I$MESA/src/gallium/auxiliary -I$MESA/src/gallium/drivers/r300 \
        -I$MESA/src/gallium/drivers/r300/compiler \
        -I$MESA/src/gallium/drivers/r300/compiler/tests \
        -DHAVE_ENDIAN_H -DHAVE_PTHREAD -DTEST_PATH='"..."' -c file.c

The build does not include the NIR front end (`nir_to_rc.c`, `r300_nir.c`,
`r500_nir_lower_fcsel.c`). The compiler itself does not need NIR. From
`src/util`, the build compiles only `register_allocate.c`, `ralloc.c`,
`blob.c`, `u_dynarray.c`, and `u_printf.c`.

`stubs/mesa_stubs.c` supplies three functions. Without them, the build would
need `u_debug.c` / `log.c` and the OS layer:

* `_util_debug_message`: prints the shader statistics line of the compiler.
* `debug_get_num_option`: reads the environment variable, for example
  `RADEON_TEX_GROUP`.
* `mesa_log`.

## Usage

    rc_compile [--fs-r500 | --fs-r300 | --fs-r400 | --vs-r300 | --vs-r500]
               [--dump-hex] [--glsl-like] [--uniforms] [--debug] [--no-opt]
               [--remove-unused-constants] [--dense-inputs] [--depth-output N]
               [--keep-stderr] file.rc

The default target is `--fs-r500`. Exit status:

* 0: OK.
* 1: compiler error. The `=== ERROR ===` section contains `ErrorMsg`.
* 2: usage error or parse error.

Options:

* `--dump-hex` adds `=== HEX ===`: bare dwords, one on each line, in the
  sequence that Mesa uploads them.
  * R500 FS: 6 for each instruction, then 4 for each constant.
  * R300 FS: rgb_inst[], rgb_addr[], alpha_inst[], alpha_addr[], tex[], then
    float24 constants.
  * VS: 4 for each instruction, then 4 for each constant.
* `--glsl-like` adds `=== GLSL-LIKE ===`. This is a pseudo-GLSL view of the
  *input* program for persons to read. It is only for information. It is not
  valid GLSL, and it is not what the GLSL front end of Mesa would make.
* `--uniforms` marks the `const[N]` definitions as `RC_CONSTANT_EXTERNAL`
  (what a GLSL/TGSI uniform becomes) and not as `RC_CONSTANT_IMMEDIATE`.
  * Without this option, the R500 FS compiler puts small immediates directly
    in the instructions (the `inline literals` pass) and folds the other
    immediates. It removes the unused immediates from the constant list. Mesa
    does the same for shader immediates.
  * With this option, the generated code must read the constant file. Use it
    to test the decoding of constant registers.
  * The tool still prints the values and emits them in `=== HEX ===`.
* `--debug` sets `RC_DBG_LOG`. The compiler dumps the program after each pass
  (into the `=== COMPILE ===` section).
* `--no-opt` sets `disable_optimizations = 1`.
* `--remove-unused-constants` sets `remove_unused_constants`, as r300_fs.c
  does for R3xx (and for more than 200 constants). The external constants get
  new numbers, and the tool prints the remap table.
* `--dense-inputs` (FS only): allocates the hardware input registers densely,
  in the sequence of the input indices (as r300_fs.c does). By default,
  `input[i]` is hardware input register `i`.
* `--depth-output N` (FS only): `output[N]` is the depth output (default 4).
  `output[0..3]` are the color targets 0..3.
* `--keep-stderr` does not `dup2` stderr onto stdout. The dump functions of
  Mesa print to stderr. By default, the tool merges stderr into stdout, so the
  sections occur in sequence in one stream.

### Compiler setup

The tool fills the compiler structs in the same way as `r300_fs.c
r300_translate_fragment_shader()` and `r300_vs.c
r300_translate_vertex_shader()`:

| target     | is_r500 | is_r400 | max_temp_regs | max_constants | max_alu_insts | max_tex_insts |
|------------|---------|---------|---------------|---------------|---------------|---------------|
| --fs-r300  | 0       | 0       | 32            | 32            | 64            | 32            |
| --fs-r400  | 0       | 1       | 64            | 32            | 512           | 512           |
| --fs-r500  | 1       | 0       | 128           | 256           | 512           | 512           |
| --vs-r300  | 0       | 0       | 32            | 256           | 256           | -             |
| --vs-r500  | 1       | 0       | 32            | 256           | 1024          | -             |

* FS: `has_half_swizzles = has_presub = has_omod = 1`. VS: all 0.
* The FS `AllocateHwInputs` callback maps the inputs as the options above
  describe.
* The VS `SetHwInputOutput` callback maps `input[i]` to hardware input `i` and
  `output[i]` to hardware output `i`.
* `RequiredOutputs` is the set of outputs that the program writes. Thus the
  tool adds no artificial outputs.
* All of `state` (shadow compare, wrap-mode emulation, alpha_to_one) is zero.
* Compilation goes through `r3xx_compile_fragment_program()` /
  `r3xx_compile_vertex_program()`, thus through the real pass lists.

## Input format

The input uses the text format of the Mesa compiler unit tests
(`src/gallium/drivers/r300/compiler/tests/*.test`). This is the format that
`rc_print_program()` prints:

* One instruction on each line.
* `#` starts a comment.
* The tool ignores blank lines and an optional `N:` at the start of a line.

Example:

    # comment
    const[0] {1.0, 0.5, 0.25, 8.0}        # also accepts  const[0] = { 1 0.5 0.25 8 }
    MUL temp[0], input[0], const[0].xxxx;
    MAD_SAT temp[1].xyz, temp[0].xyz_, -const[0].yyy_, |input[1]|;
    DP3 temp[2].w, temp[1].xyz_, temp[1].xyz_;
    TEX temp[3], input[1].xy__, 2D[0];     # target: 1D 2D 3D CUBE RECT 1D_ARRAY 2D_ARRAY, +SHADOW
    KIL temp[3];
    IF temp[2].x;  ...  ELSE;  ...  ENDIF;
    BGNLOOP;  ...  BRK;  ...  ENDLOOP;
    ARL addr[0].x, input[1].x___;          # vertex programs
    DP4 output[0].x, input[0], const[1 + addr[0]];
    MOV output[0], temp[3];

* Register files: `temp`, `input`, `const`, `output`, `none`, and `addr`
  (the destination of ARL/ARR, VS only).
* Swizzle characters: `x y z w 0 1 H _`.
  * A `-` before a component negates that component.
  * A `-` at the start negates all components.
  * `|...|` gives the absolute value.
* `_SAT` after the opcode sets saturate.
* Scalar opcodes (RCP, RSQ, EX2, LG2, SIN, COS, ...) read the source component
  that is in the write-mask slot of the destination. Thus write
  `RSQ temp[0].y, input[1].xxxx;` (or `._x__`), not `.x`.

The parser is `rc_test_helpers.c` (`parse_rc_normal_instruction`,
`add_constant`, ...). `rc_compile.c` adds only the items that this parser
does not have:

* The texture target/unit operand.
* `_SAT`.
* `addr[0]` destinations and `+ addr[0]` relative addressing.
* A check of the opcode name. The helper changes unknown opcodes to NOP and
  gives no message.

It also makes the constant list larger before it starts. The reason: on an
empty program, the `add_constant()` of the helper dereferences a NULL list
for `const[0]`.

## Output sections

    # rc_compile: file, target
    === INPUT ===          rc_print_program() + rc_constants_print() of the parsed program
    === GLSL-LIKE ===      (--glsl-like)
    === COMPILE ===        everything the compiler prints (stats line, --debug dumps)
    === CODE ===           machine code, parseable (below)
    === CONSTANTS ===      constant list after compilation (+ remap table)
    === REGS ===           register values as r300_emit.c / r300_fs.c program them
    === MESA DUMP ===      r500FragmentProgramDump / r300FragmentProgramDump / r300_vertex_program_dump
    === HEX ===            (--dump-hex)
    === FINAL PROGRAM ===  rc_print_program() of the final pair/scheduled IR
    === END ===

`=== CODE ===` lines:

* R500 FS: `inst N: inst0 inst1 inst2 inst3 inst4 inst5`. These are the six
  `R500_US_*` words in upload sequence (CMN_INST, RGB_ADDR/TEX_ADDR,
  ALPHA_ADDR/TEX_DXDY/FC_INST, RGB_INST/TEX_INST/FC_ADDR, ALPHA_INST,
  RGBA_INST). Also `int_const N:` loop counters. REGS gives these values:
  `R500_US_CONFIG`, `US_PIXSIZE` (= max_temp_idx), `US_FC_CTRL`,
  `US_FC_INT_CONST_n`, `US_CODE_RANGE`, `US_CODE_OFFSET`, `US_CODE_ADDR`, the
  `GA_US_VECTOR_INDEX` values for the code and constant uploads,
  `FG_DEPTH_SRC`, `US_W_FMT`, and `writes_depth`.
* R300/R400 FS: `alu N: rgb_inst rgb_addr alpha_inst alpha_addr
  r400_ext_addr` and `tex N: word`. REGS gives `US_CONFIG`, `US_PIXSIZE`,
  `US_CODE_OFFSET`, `US_CODE_ADDR_0..3`, (`R400_US_CODE_EXT/BANK`), the
  `PFS_PARAM_n_{X,Y,Z,W}` float24 constants, `FG_DEPTH_SRC`, and `US_W_FMT`.
* VS: `inst N: op src0 src1 src2` (4 dwords for each PVS instruction), `fc N:`
  flow-control entries, and `inputs_read` / `outputs_written`. REGS gives
  `VAP_PVS_CODE_CNTL_0` (FIRST/XYZW_VALID/LAST_INST), `CODE_CNTL_1`
  (last_input_read), the `VECTOR_INDX_REG` values for code and constants,
  `PVS_CONST_CNTL`, `PVS_FLOW_CNTL_OPC`, all 16 `FLOW_CNTL_ADDRS(_LW/_UW)`
  and `FLOW_CNTL_LOOP_INDEX` entries, and the derived `VAP_CNTL`
  slot/controller counts.

`=== CONSTANTS ===` prints each constant as `const N: x y z w | hex32 ... |
type`. For R300 FS, it also prints the float24 words.

## Samples (`make check`)

| file                        | target  | tests                                              |
|-----------------------------|---------|----------------------------------------------------|
| samples/fs_arith.rc         | fs-r500 | MUL/MAD/ADD_SAT, inline literals                   |
| samples/fs_swizzle.rc       | fs-r500 | swizzles, negate, abs, 0/1/H components, RCP, RSQ  |
| samples/fs_tex.rc           | fs-r500 | TEX 2D, TXP, TEX CUBE, dependent ALU                |
| samples/fs_texkill.rc       | fs-r500 | KIL (texkill)                                      |
| samples/fs_branch_loop.rc   | fs-r500 | BGNLOOP/BRK/ENDLOOP with counter, IF/ELSE/ENDIF    |
| samples/fs_depth.rc         | fs-r500 | color + depth output (`output[4]`)                |
| samples/fs_r300_arith.rc    | fs-r300 | R300 ALU + TEX, float24 constants                  |
| samples/fs_r300_texkill.rc  | fs-r300 | R300 texkill                                       |
| samples/vs_basic.rc         | vs-r300 | 4x DP4 transform + pass-through                    |
| samples/vs_addr.rc          | vs-r300 | ARL + relative constant addressing (`--uniforms`)  |
| samples/vs_r500_math.rc     | vs-r500 | scalar math ops, MAX/MIN/SGE, MAD                  |

Each sample gives its target in a `# target: ...` line (and optionally
`# flags: ...`). `make check` reads these lines.

## Limitations

* The input is the IR of the compiler, not GLSL/TGSI. There is no front end.
  Thus the tool does not apply the steps that the NIR/TGSI path does before
  the compiler: semantic declarations, WPOS/FACE transforms,
  `rc_copy_output`, shadow-sampler state, and texture swizzles. Inputs and
  outputs are plain indices.
* `rc_test_helpers.c` cannot express these items in the input: output
  modifiers (`* 2`, `/ 4`), presubtract, inline float literals, and
  `aluresult` sources. They occur only in the output of the compiler.
* `--fs-r400` uses the R300 emitter with the `is_r400` capabilities
  (r390_mode / 512 instructions). It has fewer tests than the other targets.
* The R500 FS compiler removes the immediates that it puts directly in the
  instructions from the constant list (Mesa behavior). Use `--uniforms` to
  make the code read the constant file.
  * In the same way, the `dead constants` pass removes `RC_CONSTANT_IMMEDIATE`
    constants that the program reads only through relative addressing
    (`const[N + addr[0]]`). Mesa uses relative addressing only for uniforms,
    which are external constants. Thus compile programs that use the address
    register with `--uniforms` (samples/vs_addr.rc does this).
* The growth path of the Mesa `add_constant()` has bugs: a NULL dereference
  for `const[0]` on an empty list, and a copy of too much data when the list
  grows. rc_compile prevents these bugs: it makes the list larger before it
  starts. Thus constant indices up to 255 work.
* The tool uses one thread, prints to stdout, and merges stderr. To keep the
  dumps from Mesa separate, use `--keep-stderr`.
