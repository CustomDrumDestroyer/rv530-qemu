# rc_compile — Mesa r300/r500 shader compiler as a command-line tool

`rc_compile` wraps Mesa's r300 Gallium shader compiler (the `rc_*` /
`r3xx_compile_*` code under `src/gallium/drivers/r300/compiler/`) so that
test shaders can be compiled to R300/R400/R500 machine code without a GPU,
and so that Mesa's own disassembly can serve as ground truth for the
rv530 emulator's fragment/vertex program decoders.

Nothing from Mesa is copied into this tree: the Makefile compiles the
compiler, its unit-test parser (`tests/rc_test_helpers.c`), and five small
`src/util` files in place from `reference/mesa` (see scripts/fetch-references.sh) (override with
`make MESA=...`).  Only the files in this directory are ours:

    Makefile              build + `make check`
    rc_compile.c          the driver (parsing glue, compiler setup, printers)
    stubs/mesa_stubs.c    3 util symbols the compiler needs (see below)
    stubs/util/u_bitcast.h  replacement for a header absent from this checkout
    samples/*.rc          sample programs used by `make check`

## Build

    make                      # -> ./rc_compile   (gcc 13, -std=gnu11)
    make check                # compiles every samples/*.rc and prints the output
    make clean

Exact compile line used for Mesa's sources (see the Makefile):

    gcc -std=gnu11 -O2 -g -Wall -Wno-unused-parameter -Istubs \
        -I$MESA/src -I$MESA/include -I$MESA/src/gallium/include \
        -I$MESA/src/gallium/auxiliary -I$MESA/src/gallium/drivers/r300 \
        -I$MESA/src/gallium/drivers/r300/compiler \
        -I$MESA/src/gallium/drivers/r300/compiler/tests \
        -DHAVE_ENDIAN_H -DHAVE_PTHREAD -DTEST_PATH='"..."' -c file.c

The NIR front end (`nir_to_rc.c`, `r300_nir.c`, `r500_nir_lower_fcsel.c`)
is not built; the compiler proper has no NIR dependency.  From `src/util`
only `register_allocate.c`, `ralloc.c`, `blob.c`, `u_dynarray.c`, and
`u_printf.c` are compiled.  `stubs/mesa_stubs.c` provides
`_util_debug_message` (prints the compiler's shader stats line),
`debug_get_num_option` (reads the env var, e.g., `RADEON_TEX_GROUP`), and
`mesa_log`, which would otherwise pull in `u_debug.c` / `log.c` and the OS
layer.

## Usage

    rc_compile [--fs-r500 | --fs-r300 | --fs-r400 | --vs-r300 | --vs-r500]
               [--dump-hex] [--glsl-like] [--uniforms] [--debug] [--no-opt]
               [--remove-unused-constants] [--dense-inputs] [--depth-output N]
               [--keep-stderr] file.rc

Default target is `--fs-r500`.  Exit status: 0 ok, 1 compiler error
(`=== ERROR ===` section carries `ErrorMsg`), 2 usage / parse error.

* `--dump-hex`   adds `=== HEX ===`: bare dwords, one per line, in the
  order Mesa uploads them (R500 FS: 6 per instruction then 4 per constant;
  R300 FS: rgb_inst[], rgb_addr[], alpha_inst[], alpha_addr[], tex[], then
  float24 constants; VS: 4 per instruction then 4 per constant).
* `--glsl-like` adds `=== GLSL-LIKE ===`, a pseudo-GLSL rendering of the
  *input* program for humans.  It is informational only (not valid GLSL,
  not what Mesa's GLSL front end would have produced).
* `--uniforms`  marks the `const[N]` definitions as `RC_CONSTANT_EXTERNAL`
  (what a GLSL/TGSI uniform becomes) instead of `RC_CONSTANT_IMMEDIATE`.
  Without it the R500 FS compiler inlines small immediates (`inline
  literals` pass) and folds the rest, and the unused immediates are
  dropped from the constant list, exactly like Mesa does for shader
  immediates.  With it the generated code has to read the constant file,
  which is what you want when testing constant-register decoding.  The
  values are still printed and emitted in `=== HEX ===`.
* `--debug`     sets `RC_DBG_LOG`: the compiler dumps the program after
  every pass (into the `=== COMPILE ===` section).
* `--no-opt`    `disable_optimizations = 1`.
* `--remove-unused-constants` sets `remove_unused_constants` as r300_fs.c
  does for R3xx (and for >200 constants); externals get renumbered and the
  remap table is printed.
* `--dense-inputs` FS only: allocate hardware input registers densely in
  input-index order (r300_fs.c style).  Default: `input[i]` is hardware
  input register `i`.
* `--depth-output N` FS only: `output[N]` is the depth output (default 4).
  `output[0..3]` are color targets 0..3.
* `--keep-stderr` do not `dup2` stderr onto stdout.  Mesa's dump functions
  print to stderr; by default stderr is merged into stdout so the sections
  appear in order in a single stream.

### Compiler setup

The compiler structs are filled the way `r300_fs.c
r300_translate_fragment_shader()` and `r300_vs.c
r300_translate_vertex_shader()` do:

| target     | is_r500 | is_r400 | max_temp_regs | max_constants | max_alu_insts | max_tex_insts |
|------------|---------|---------|---------------|---------------|---------------|---------------|
| --fs-r300  | 0       | 0       | 32            | 32            | 64            | 32            |
| --fs-r400  | 0       | 1       | 64            | 32            | 512           | 512           |
| --fs-r500  | 1       | 0       | 128           | 256           | 512           | 512           |
| --vs-r300  | 0       | 0       | 32            | 256           | 256           | -             |
| --vs-r500  | 1       | 0       | 32            | 256           | 1024          | -             |

FS: `has_half_swizzles = has_presub = has_omod = 1`; VS: all 0.  The FS
`AllocateHwInputs` callback maps inputs as described above; the VS
`SetHwInputOutput` callback maps `input[i]` to hardware input `i` and
`output[i]` to hardware output `i`; `RequiredOutputs` is the set of outputs
the program writes, so no artificial outputs are added.  `state` (shadow
compare, wrap-mode emulation, alpha_to_one) is all zero.  Compilation goes
through `r3xx_compile_fragment_program()` / `r3xx_compile_vertex_program()`,
i.e., the real pass lists.

## Input format

The text format of Mesa's compiler unit tests
(`src/gallium/drivers/r300/compiler/tests/*.test`), which is what
`rc_print_program()` prints: one instruction per line, `#` comments,
blank lines ignored, an optional leading `N:` ignored.

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

Register files: `temp`, `input`, `const`, `output`, `none`, `addr`
(destination of ARL/ARR, VS only).  Swizzle characters: `x y z w 0 1 H _`,
with a `-` before a component for per-component negation, a leading `-`
for full negation, and `|...|` for absolute value.  `_SAT` after the opcode
sets saturate.  Scalar opcodes (RCP, RSQ, EX2, LG2, SIN, COS, ...) read the
source component that sits in the destination's write-mask slot, so write
`RSQ temp[0].y, input[1].xxxx;` (or `._x__`), not `.x`.

The parser is `rc_test_helpers.c` (`parse_rc_normal_instruction`,
`add_constant`, ...).  `rc_compile.c` only adds what that parser lacks:
the texture target/unit operand, `_SAT`, `addr[0]` destinations and
`+ addr[0]` relative addressing, and it validates the opcode name (the
helper silently maps unknown opcodes to NOP).  It also pre-grows the
constant list because the helper's `add_constant()` dereferences a NULL
list for `const[0]` on an empty program.

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

* R500 FS: `inst N: inst0 inst1 inst2 inst3 inst4 inst5` — the six
  `R500_US_*` words in upload order (CMN_INST, RGB_ADDR/TEX_ADDR,
  ALPHA_ADDR/TEX_DXDY/FC_INST, RGB_INST/TEX_INST/FC_ADDR, ALPHA_INST,
  RGBA_INST), plus `int_const N:` loop counters.  REGS gives
  `R500_US_CONFIG`, `US_PIXSIZE` (= max_temp_idx), `US_FC_CTRL`,
  `US_FC_INT_CONST_n`, `US_CODE_RANGE`, `US_CODE_OFFSET`, `US_CODE_ADDR`,
  the `GA_US_VECTOR_INDEX` values used for the code and constant uploads,
  `FG_DEPTH_SRC`, `US_W_FMT`, and `writes_depth`.
* R300/R400 FS: `alu N: rgb_inst rgb_addr alpha_inst alpha_addr
  r400_ext_addr` and `tex N: word`.  REGS gives `US_CONFIG`, `US_PIXSIZE`,
  `US_CODE_OFFSET`, `US_CODE_ADDR_0..3`, (`R400_US_CODE_EXT/BANK`), the
  `PFS_PARAM_n_{X,Y,Z,W}` float24 constants, `FG_DEPTH_SRC`, `US_W_FMT`.
* VS: `inst N: op src0 src1 src2` (4 dwords per PVS instruction), `fc N:`
  flow-control entries, `inputs_read` / `outputs_written`.  REGS gives
  `VAP_PVS_CODE_CNTL_0` (FIRST/XYZW_VALID/LAST_INST), `CODE_CNTL_1`
  (last_input_read), `VECTOR_INDX_REG` values for code and constants,
  `PVS_CONST_CNTL`, `PVS_FLOW_CNTL_OPC`, all 16 `FLOW_CNTL_ADDRS(_LW/_UW)`
  and `FLOW_CNTL_LOOP_INDEX` entries, and the derived `VAP_CNTL`
  slot/controller counts.

`=== CONSTANTS ===` prints each constant as `const N: x y z w | hex32 ... |
type`, plus the float24 words for R300 FS.

## Samples (`make check`)

| file                        | target  | exercises                                          |
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

Each sample names its target in a `# target: ...` line (and optional
`# flags: ...`), which `make check` reads.

## Limitations

* The input is the compiler's own IR, not GLSL/TGSI: there is no front
  end, so things the NIR/TGSI path does before the compiler (declaring
  semantics, WPOS/FACE transforms, `rc_copy_output`, shadow-sampler
  state, texture swizzles) are not applied.  Inputs and outputs are plain
  indices.
* `rc_test_helpers.c` cannot express output modifiers (`* 2`, `/ 4`),
  presubtract, inline float literals, or `aluresult` sources in the input;
  those appear only in the compiler's output.
* `--fs-r400` uses the R300 emitter with `is_r400` capabilities
  (r390_mode / 512 instructions) and is less exercised than the others.
* Immediates that the R500 FS compiler inlines are dropped from the
  constant list (Mesa behavior); use `--uniforms` to force constant
  file reads.  Likewise, `RC_CONSTANT_IMMEDIATE` constants that are only
  reached through relative addressing (`const[N + addr[0]]`) are dropped
  by the `dead constants` pass (Mesa only ever addresses uniforms, i.e.
  externals, relatively), so programs using the address register should
  be compiled with `--uniforms` (samples/vs_addr.rc does).
* Mesa's `add_constant()` growth path is buggy (NULL deref for `const[0]`
  on an empty list; over-copy on growth); rc_compile sidesteps it by
  pre-growing the list, so constant indices up to 255 work.
* The tool is single-threaded, prints to stdout, and merges stderr; use
  `--keep-stderr` if you want to separate Mesa's dumps.
