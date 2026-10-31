/*
 * rc_compile - standalone driver for Mesa's r300/r500 shader compiler.
 *
 * Reads a program in the text format used by Mesa's compiler unit tests
 * (src/gallium/drivers/r300/compiler/tests, the .test files), compiles it with the
 * real r3xx_compile_{fragment,vertex}_program() pipeline, and prints the
 * resulting machine code in a parseable form together with Mesa's own
 * disassembly (r500FragmentProgramDump / r300FragmentProgramDump /
 * r300_vertex_program_dump) and rc_print_program() output.
 *
 * Everything in this file is glue; the parser (add_instruction /
 * add_constant / parse_rc_normal_instruction ...) and the compiler come
 * unmodified from the Mesa tree and are compiled in place by the Makefile.
 *
 * Part of the rv530 QEMU device model tooling.  Not a Mesa file.
 */


#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "radeon_compiler.h"
#include "radeon_compiler_util.h"
#include "radeon_code.h"
#include "radeon_opcodes.h"
#include "radeon_program.h"
#include "radeon_program_constants.h"
#include "radeon_regalloc.h"
#include "radeon_swizzle.h"
#include "r300_fragprog.h"
#include "r500_fragprog.h"
#include "r300_fragprog_swizzle.h"

#include "rc_test_helpers.h"

/* ------------------------------------------------------------------------
 * Register definitions.
 *
 * Copied from Mesa 24.0.9 src/gallium/drivers/r300/r300_reg.h (MIT license,
 * Copyright 2005 Nicolai Haehnle et.al.) so that this tool does not
 * include driver headers beyond the compiler directory.  The values are
 * those r300_emit.c / r300_fs.c program into the command stream.
 * ------------------------------------------------------------------------ */

/* Vertex program (PVS) */
#define R300_VAP_CNTL                        0x2080
#define R300_VAP_PVS_VECTOR_INDX_REG         0x2200
#   define R300_PVS_CODE_START               0
#   define R300_PVS_CONST_START              512
#   define R500_PVS_CONST_START              1024
#define R300_VAP_PVS_UPLOAD_DATA             0x2208
#define R300_VAP_PVS_FLOW_CNTL_ADDRS_0       0x2230
#define R300_VAP_PVS_FLOW_CNTL_LOOP_INDEX_0  0x2290
#define R300_VAP_PVS_CODE_CNTL_0             0x22D0
#   define R300_PVS_FIRST_INST(x)            ((x) << 0)
#   define R300_PVS_XYZW_VALID_INST(x)       ((x) << 10)
#   define R300_PVS_LAST_INST(x)             ((x) << 20)
#define R300_VAP_PVS_CONST_CNTL              0x22D4
#   define R300_PVS_CONST_BASE_OFFSET(x)     (x)
#   define R300_PVS_MAX_CONST_ADDR(x)        ((x) << 16)
#define R300_VAP_PVS_CODE_CNTL_1             0x22D8
#define R300_VAP_PVS_FLOW_CNTL_OPC           0x22DC
#define R500_VAP_PVS_FLOW_CNTL_ADDRS_LW_0    0x2500
#define R500_VAP_PVS_FLOW_CNTL_ADDRS_UW_0    0x2504

/* R300/R400 fragment program (US) */
#define R300_US_CONFIG                       0x4600
#define R300_US_PIXSIZE                      0x4604
#define R300_US_CODE_OFFSET                  0x4608
#define R300_US_CODE_ADDR_0                  0x4610
#define R300_US_TEX_INST_0                   0x4620
#define R400_US_CODE_BANK                    0x46b8
#   define R400_BANK_SHIFT                   0
#   define R400_R390_MODE_ENABLE             (1 << 4)
#define R400_US_CODE_EXT                     0x46bc
#define R300_US_ALU_RGB_ADDR_0               0x46C0
#define R300_US_ALU_ALPHA_ADDR_0             0x47C0
#define R300_US_ALU_RGB_INST_0               0x48C0
#define R300_US_ALU_ALPHA_INST_0             0x49C0
#define R400_US_ALU_EXT_ADDR_0               0x4ac0
#define R300_PFS_PARAM_0_X                   0x4C00

/* Shared */
#define R300_FG_DEPTH_SRC                    0x4bd8
#   define R300_FG_DEPTH_SRC_SCAN            (0 << 0)
#   define R300_FG_DEPTH_SRC_SHADER          (1 << 0)
#define R300_US_W_FMT                        0x46b4
#   define R300_W_FMT_W0                     (0 << 0)
#   define R300_W_FMT_W24                    (1 << 0)
#   define R300_W_SRC_US                     (0 << 2)

/* R500 fragment program (US) */
#define R500_GA_US_VECTOR_INDEX              0x4250
#   define R500_GA_US_VECTOR_INDEX_MASK      0x000000ff
#   define R500_GA_US_VECTOR_INDEX_TYPE_INSTR (0 << 16)
#   define R500_GA_US_VECTOR_INDEX_TYPE_CONST (1 << 16)
#define R500_GA_US_VECTOR_DATA               0x4254
#define R500_US_CONFIG                       0x4600
#   define R500_ZERO_TIMES_ANYTHING_EQUALS_ZERO (1 << 1)
#define R500_US_PIXSIZE                      0x4604
#define R500_US_FC_CTRL                      0x4624
#   define R500_FC_TEST_EN                   (1 << 30)
#   define R500_FC_FULL_FC_EN                (1U << 31)
#define R500_US_CODE_ADDR                    0x4630
#   define R500_US_CODE_START_ADDR(x)        ((x) << 0)
#   define R500_US_CODE_END_ADDR(x)          ((x) << 16)
#define R500_US_CODE_RANGE                   0x4634
#   define R500_US_CODE_RANGE_ADDR(x)        ((x) << 0)
#   define R500_US_CODE_RANGE_SIZE(x)        ((x) << 16)
#define R500_US_CODE_OFFSET                  0x4638
#define R500_US_FC_INT_CONST_0               0x4c00

/* ------------------------------------------------------------------------
 * Options
 * ------------------------------------------------------------------------ */

enum target {
    TARGET_FS_R500,
    TARGET_FS_R300,
    TARGET_FS_R400,
    TARGET_VS_R300,
    TARGET_VS_R500,
};

struct options {
    enum target target;
    bool dump_hex;
    bool glsl_like;
    bool debug;             /* RC_DBG_LOG: dump after every pass */
    bool no_opt;            /* disable_optimizations */
    bool remove_unused_constants;
    bool uniforms;          /* treat const[] as externals (uniforms) */
    bool keep_stderr;       /* do not merge stderr into stdout */
    bool dense_inputs;      /* FS: pack used inputs into hw regs 0..n-1 */
    int  depth_output;      /* FS: output[] index treated as depth */
    const char *file;
};

static void usage(FILE *f)
{
    fprintf(f,
"usage: rc_compile [options] file.rc\n"
"\n"
"Target (default --fs-r500):\n"
"  --fs-r500        R5xx fragment program (R500 US)\n"
"  --fs-r300        R3xx fragment program (R300 US)\n"
"  --fs-r400        R4xx fragment program (R300 US, r400 extensions)\n"
"  --vs-r300        R3xx vertex program (PVS)\n"
"  --vs-r500        R5xx vertex program (PVS, r500 flow control)\n"
"Output:\n"
"  --dump-hex       add a '=== HEX ===' section: bare dwords in upload order\n"
"  --glsl-like      add a '=== GLSL-LIKE ===' pseudo-source rendering of the\n"
"                   input program (for humans; not compilable GLSL)\n"
"  --debug          let the compiler dump the program after every pass\n"
"  --keep-stderr    do not redirect stderr into stdout (Mesa's dumps go to\n"
"                   stderr; by default they are merged so sections stay in\n"
"                   order)\n"
"Compiler setup:\n"
"  --no-opt         disable_optimizations=1 (like RADEON_DEBUG=noopt)\n"
"  --remove-unused-constants\n"
"                   remove_unused_constants=1 (r300_fs.c sets this for R3xx\n"
"                   and for >200 constants; constants are then renumbered,\n"
"                   see the remap table in === CONSTANTS ===)\n"
"  --uniforms       treat const[N] definitions as RC_CONSTANT_EXTERNAL (what a\n"
"                   GLSL/TGSI uniform becomes) instead of RC_CONSTANT_IMMEDIATE,\n"
"                   so the compiler must read them from the constant file\n"
"                   instead of inlining/folding them\n"
"  --dense-inputs   FS: allocate hw input registers densely in input-index\n"
"                   order like r300_fs.c does (default: input[i] -> hw reg i)\n"
"  --depth-output N FS: output[N] is the depth output (default 4; output[0..3]\n"
"                   are color targets 0..3)\n"
"  -h, --help       this text\n"
"\n"
"Input format: the Mesa compiler unit-test format, one instruction per line\n"
"as printed by rc_print_program(), e.g.\n"
"  const[0] {1.0, 2.0, 3.0, 4.0}\n"
"  MUL temp[0], input[0], const[0].xxxx;\n"
"  TEX temp[1], input[1].xy__, 2D[0];\n"
"  MOV output[0], temp[1];\n"
"Lines starting with '#' are comments.  See README.md.\n");
}

/* ------------------------------------------------------------------------
 * Input parsing (on top of rc_test_helpers.c)
 * ------------------------------------------------------------------------ */

static int g_line_no;

static void parse_error(const char *fmt, ...)
{
    va_list ap;
    fflush(stdout);
    fprintf(stderr, "rc_compile: line %d: ", g_line_no);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(2);
}

static void rstrip(char *s)
{
    size_t n = strlen(s);
    while (n > 0 && isspace((unsigned char)s[n - 1]))
        s[--n] = '\0';
}

static const char *lskip(const char *s)
{
    while (*s && isspace((unsigned char)*s))
        s++;
    return s;
}

static int parse_texture_target(const char *tok, unsigned *target, unsigned *unit,
                                unsigned *shadow)
{
    static const struct { const char *name; unsigned target; } tab[] = {
        { "2D_ARRAY", RC_TEXTURE_2D_ARRAY },
        { "1D_ARRAY", RC_TEXTURE_1D_ARRAY },
        { "CUBE",     RC_TEXTURE_CUBE },
        { "RECT",     RC_TEXTURE_RECT },
        { "3D",       RC_TEXTURE_3D },
        { "2D",       RC_TEXTURE_2D },
        { "1D",       RC_TEXTURE_1D },
    };
    unsigned i;
    tok = lskip(tok);
    for (i = 0; i < sizeof(tab) / sizeof(tab[0]); i++) {
        size_t l = strlen(tab[i].name);
        if (strncmp(tok, tab[i].name, l) == 0) {
            const char *p = tok + l;
            *target = tab[i].target;
            *shadow = 0;
            if (strncmp(p, "SHADOW", 6) == 0) {
                *shadow = 1;
                p += 6;
            }
            if (*p != '[')
                return 0;
            *unit = (unsigned)strtoul(p + 1, NULL, 10);
            return 1;
        }
    }
    return 0;
}

/*
 * Parse one instruction line.  Handles the few things rc_test_helpers.c's
 * parser does not: texture target/unit ("2D[0]"), _SAT, relative
 * addressing ("const[3 + addr[0]]") and the address register as
 * destination ("addr[0]").
 */
static void add_instruction_line(struct radeon_compiler *c, const char *line)
{
    char buf[512];
    char opname[32];
    const char *p;
    const char *opstart;
    size_t oplen;
    bool sat = false;
    bool dst_is_addr = false;
    unsigned reladdr_mask = 0;
    struct rc_instruction *inst;
    const struct rc_opcode_info *info;
    char *s;
    int ncommas_before;

    if (strlen(line) >= sizeof(buf))
        parse_error("line too long");
    strcpy(buf, line);

    /* Strip an optional leading "N:" instruction number. */
    s = buf;
    while (isdigit((unsigned char)*s)) s++;
    if (*s == ':') {
        memmove(buf, s + 1, strlen(s + 1) + 1);
    }
    s = (char *)lskip(buf);
    memmove(buf, s, strlen(s) + 1);

    /* Opcode token: upper case letters, digits, '_' */
    opstart = buf;
    p = buf;
    while (isupper((unsigned char)*p) || isdigit((unsigned char)*p) || *p == '_')
        p++;
    oplen = (size_t)(p - opstart);
    if (oplen == 0 || oplen >= sizeof(opname))
        parse_error("cannot find opcode in '%s'", line);
    memcpy(opname, opstart, oplen);
    opname[oplen] = '\0';
    if (oplen > 4 && strcmp(opname + oplen - 4, "_SAT") == 0) {
        sat = true;
        opname[oplen - 4] = '\0';
    }

    /* Relative addressing: "file[N + addr[0]]" -> "file[N]", remember the
     * operand position (comma count before the occurrence; operand 0 is the
     * destination when the opcode has one). */
    for (;;) {
        char *q = strstr(buf, "addr[0]]");
        char *r;
        if (!q)
            break;
        r = q;
        while (r > buf && r[-1] == ' ')
            r--;
        if (r == buf || r[-1] != '+')
            parse_error("unsupported use of addr[0] in '%s'", line);
        r--;                                    /* the '+' */
        while (r > buf && r[-1] == ' ')
            r--;
        ncommas_before = 0;
        for (s = buf; s < q; s++)
            if (*s == ',')
                ncommas_before++;
        reladdr_mask |= 1u << ncommas_before;
        memmove(r, q + 7, strlen(q + 7) + 1);  /* keep the ']' closing file[N] */
    }

    /* Destination address register (ARL): "addr[0]" -> "temp[0]" */
    if (strncmp(lskip(buf + oplen), "addr[", 5) == 0) {
        char *a = strstr(buf, "addr[");
        memcpy(a, "temp[", 5);
        dst_is_addr = true;
    }

    /* Hand to Mesa's parser. */
    inst = rc_insert_new_instruction(c, c->Program.Instructions.Prev);
    if (!parse_rc_normal_instruction(inst, buf))
        parse_error("Mesa's parser rejected '%s'", line);

    info = rc_get_opcode_info(inst->U.I.Opcode);
    if (strcmp(info->Name, opname) != 0)
        parse_error("unknown opcode '%s' (Mesa resolved it to %s)", opname, info->Name);

    if (sat)
        inst->U.I.SaturateMode = RC_SATURATE_ZERO_ONE;

    if (dst_is_addr) {
        if (!info->HasDstReg)
            parse_error("addr[] destination on an opcode without destination");
        inst->U.I.DstReg.File = RC_FILE_ADDRESS;
    }

    if (reladdr_mask) {
        unsigned i;
        unsigned first_src = info->HasDstReg ? 1 : 0;
        if (reladdr_mask & 1u && info->HasDstReg)
            parse_error("relative addressing on destination is not supported");
        for (i = 0; i < info->NumSrcRegs; i++) {
            if (reladdr_mask & (1u << (first_src + i)))
                inst->U.I.SrcReg[i].RelAddr = 1;
        }
    }

    if (info->HasTexture) {
        /* The texture operand is the last comma separated token. */
        const char *last = strrchr(buf, ',');
        unsigned target, unit, shadow;
        if (!last || !parse_texture_target(last + 1, &target, &unit, &shadow))
            parse_error("texture instruction needs a target operand like "
                        "'2D[0]', 'CUBE[1]', '2DSHADOW[2]': '%s'", line);
        inst->U.I.TexSrcTarget = target;
        inst->U.I.TexSrcUnit = unit;
        inst->U.I.TexShadow = shadow;
        if (strstr(last, "SEM_WAIT"))
            inst->U.I.TexSemWait = 1;
        if (strstr(last, "SEM_ACQUIRE"))
            inst->U.I.TexSemAcquire = 1;
        if (shadow)
            c->Program.ShadowSamplers |= 1u << unit;
    }
}

/* rc_test_helpers.c add_constant() only grows the constant list when
 * _Reserved < index, so with an empty list and index 0 it dereferences a
 * NULL Constants pointer (and its growth path copies too much).  Grow the
 * list here first so that path is never taken. */
static void ensure_constant_room(struct radeon_compiler *c, unsigned idx)
{
    struct rc_constant_list *cl = &c->Program.Constants;
    if (idx < cl->_Reserved && cl->Constants)
        return;
    unsigned new_reserved = idx + 128;
    struct rc_constant *nl = calloc(new_reserved, sizeof(*nl));
    if (!nl)
        parse_error("out of memory");
    if (cl->Constants) {
        memcpy(nl, cl->Constants, sizeof(*nl) * cl->Count);
        free(cl->Constants);
    }
    cl->Constants = nl;
    cl->_Reserved = new_reserved;
}

/* Accept both "const[0] {1, 2, 3, 4}" (unit tests) and
 * "const[0] = {1 2 3 4}" (rc_constants_print output). */
static void add_constant_line(struct radeon_compiler *c, const char *line)
{
    char buf[256];
    unsigned idx;
    float v[4];
    int n;

    if (sscanf(line, "const[%u]", &idx) != 1)
        parse_error("cannot parse constant '%s'", line);
    ensure_constant_room(c, idx);
    if (add_constant(c, line))
        return;

    n = sscanf(line, "const[%u] = { %f %f %f %f }", &idx, &v[0], &v[1], &v[2], &v[3]);
    if (n != 5)
        n = sscanf(line, "const[%u] = {%f, %f, %f, %f}", &idx, &v[0], &v[1], &v[2], &v[3]);
    if (n != 5)
        parse_error("cannot parse constant '%s'", line);
    snprintf(buf, sizeof(buf), "const[%u] {%.9g, %.9g, %.9g, %.9g}", idx, v[0], v[1], v[2], v[3]);
    if (!add_constant(c, buf))
        parse_error("cannot parse constant '%s'", line);
}

/* Values of constants converted to externals by --uniforms, for printing. */
static float g_uniform_values[256][4];
static bool  g_uniform_valid[256];

static void make_constants_external(struct radeon_compiler *c)
{
    struct rc_constant_list *cl = &c->Program.Constants;
    unsigned i;
    for (i = 0; i < cl->Count && i < 256; i++) {
        struct rc_constant *k = &cl->Constants[i];
        if (k->Type != RC_CONSTANT_IMMEDIATE)
            continue;
        memcpy(g_uniform_values[i], k->u.Immediate, sizeof(float) * 4);
        g_uniform_valid[i] = true;
        k->Type = RC_CONSTANT_EXTERNAL;
        k->u.External = i;
    }
}

static void load_file(struct radeon_compiler *c, const char *path)
{
    FILE *f = fopen(path, "r");
    char line[1024];

    if (!f) {
        fprintf(stderr, "rc_compile: %s: %s\n", path, strerror(errno));
        exit(2);
    }
    g_line_no = 0;
    while (fgets(line, sizeof(line), f)) {
        const char *p;
        char *hash;
        g_line_no++;
        rstrip(line);
        /* strip trailing comment */
        hash = strchr(line, '#');
        if (hash) {
            *hash = '\0';
            rstrip(line);
        }
        p = lskip(line);
        if (*p == '\0' || *p == '=')
            continue;
        if (strncmp(p, "const[", 6) == 0) {
            add_constant_line(c, p);
        } else {
            add_instruction_line(c, p);
        }
    }
    fclose(f);
}

/* ------------------------------------------------------------------------
 * GLSL-like pretty printer (input program only)
 * ------------------------------------------------------------------------ */

static const char *file_name(unsigned file)
{
    switch (file) {
    case RC_FILE_TEMPORARY: return "temp";
    case RC_FILE_INPUT:     return "input";
    case RC_FILE_OUTPUT:    return "output";
    case RC_FILE_ADDRESS:   return "addr";
    case RC_FILE_CONSTANT:  return "const";
    case RC_FILE_NONE:      return "none";
    default:                return "reg";
    }
}

static void glsl_src(char *out, size_t n, const struct rc_src_register *src)
{
    char swz[16];
    char reg[64];
    unsigned i, len = 0;
    bool all_neg = (src->Negate & RC_MASK_XYZW) == RC_MASK_XYZW;
    bool is_xyzw = src->Swizzle == RC_SWIZZLE_XYZW;

    for (i = 0; i < 4; i++) {
        unsigned s = GET_SWZ(src->Swizzle, i);
        if (!all_neg && (src->Negate & (1u << i)))
            swz[len++] = '-';
        switch (s) {
        case RC_SWIZZLE_X: swz[len++] = 'x'; break;
        case RC_SWIZZLE_Y: swz[len++] = 'y'; break;
        case RC_SWIZZLE_Z: swz[len++] = 'z'; break;
        case RC_SWIZZLE_W: swz[len++] = 'w'; break;
        case RC_SWIZZLE_ZERO: swz[len++] = '0'; break;
        case RC_SWIZZLE_ONE: swz[len++] = '1'; break;
        case RC_SWIZZLE_HALF: swz[len++] = 'H'; break;
        default: swz[len++] = '_'; break;
        }
    }
    swz[len] = '\0';

    if (src->File == RC_FILE_NONE)
        snprintf(reg, sizeof(reg), "vec4(%c,%c,%c,%c)",
                 swz[0] == '_' ? '0' : swz[0], swz[1] == '_' ? '0' : swz[1],
                 swz[2] == '_' ? '0' : swz[2], swz[3] == '_' ? '0' : swz[3]);
    else if (src->RelAddr)
        snprintf(reg, sizeof(reg), "%s[addr0 + %d]%s%s", file_name(src->File),
                 src->Index, is_xyzw ? "" : ".", is_xyzw ? "" : swz);
    else
        snprintf(reg, sizeof(reg), "%s%d%s%s", file_name(src->File), src->Index,
                 is_xyzw ? "" : ".", is_xyzw ? "" : swz);

    snprintf(out, n, "%s%s%s%s", all_neg ? "-" : "", src->Abs ? "abs(" : "", reg,
             src->Abs ? ")" : "");
}

static void glsl_dst(char *out, size_t n, const struct rc_dst_register *dst)
{
    char mask[6];
    unsigned len = 0;
    if (dst->WriteMask & RC_MASK_X) mask[len++] = 'x';
    if (dst->WriteMask & RC_MASK_Y) mask[len++] = 'y';
    if (dst->WriteMask & RC_MASK_Z) mask[len++] = 'z';
    if (dst->WriteMask & RC_MASK_W) mask[len++] = 'w';
    mask[len] = '\0';
    if (dst->WriteMask == RC_MASK_XYZW)
        snprintf(out, n, "%s%d", file_name(dst->File), dst->Index);
    else
        snprintf(out, n, "%s%d.%s", file_name(dst->File), dst->Index, mask);
}

static void print_glsl_like(const struct rc_program *prog)
{
    const struct rc_instruction *inst;
    int indent = 1;

    printf("// pseudo-GLSL rendering of the input program (informational)\n");
    printf("void main() {\n");
    for (inst = prog->Instructions.Next; inst != &prog->Instructions; inst = inst->Next) {
        const struct rc_opcode_info *info;
        char d[64], a[96], b[96], cc[96], expr[400];
        const char *sat_open = "", *sat_close = "";
        unsigned i;

        if (inst->Type != RC_INSTRUCTION_NORMAL)
            continue;
        info = rc_get_opcode_info(inst->U.I.Opcode);
        a[0] = b[0] = cc[0] = '\0';
        if (info->NumSrcRegs > 0) glsl_src(a, sizeof(a), &inst->U.I.SrcReg[0]);
        if (info->NumSrcRegs > 1) glsl_src(b, sizeof(b), &inst->U.I.SrcReg[1]);
        if (info->NumSrcRegs > 2) glsl_src(cc, sizeof(cc), &inst->U.I.SrcReg[2]);
        glsl_dst(d, sizeof(d), &inst->U.I.DstReg);
        if (inst->U.I.SaturateMode == RC_SATURATE_ZERO_ONE) {
            sat_open = "clamp(";
            sat_close = ", 0.0, 1.0)";
        }

        switch (inst->U.I.Opcode) {
        case RC_OPCODE_ELSE:
            printf("%*s} else {\n", (indent - 1) * 4, "");
            continue;
        case RC_OPCODE_ENDIF:
        case RC_OPCODE_ENDLOOP:
            indent--;
            printf("%*s}\n", indent * 4, "");
            continue;
        case RC_OPCODE_IF:
            printf("%*sif (%s.x != 0.0) {\n", indent * 4, "", a);
            indent++;
            continue;
        case RC_OPCODE_BGNLOOP:
            printf("%*sfor (;;) {\n", indent * 4, "");
            indent++;
            continue;
        case RC_OPCODE_BRK:
            printf("%*sbreak;\n", indent * 4, "");
            continue;
        case RC_OPCODE_CONT:
            printf("%*scontinue;\n", indent * 4, "");
            continue;
        case RC_OPCODE_KIL:
            printf("%*sif (any(lessThan(%s, vec4(0.0)))) discard;\n", indent * 4, "", a);
            continue;
        case RC_OPCODE_KILP:
            printf("%*sdiscard;\n", indent * 4, "");
            continue;
        case RC_OPCODE_ADD: snprintf(expr, sizeof(expr), "%s + %s", a, b); break;
        case RC_OPCODE_SUB: snprintf(expr, sizeof(expr), "%s - %s", a, b); break;
        case RC_OPCODE_MUL: snprintf(expr, sizeof(expr), "%s * %s", a, b); break;
        case RC_OPCODE_MAD: snprintf(expr, sizeof(expr), "%s * %s + %s", a, b, cc); break;
        case RC_OPCODE_MOV: snprintf(expr, sizeof(expr), "%s", a); break;
        case RC_OPCODE_ARL: snprintf(expr, sizeof(expr), "int(floor(%s))", a); break;
        case RC_OPCODE_ARR: snprintf(expr, sizeof(expr), "int(round(%s))", a); break;
        case RC_OPCODE_DP2: snprintf(expr, sizeof(expr), "vec4(dot((%s).xy, (%s).xy))", a, b); break;
        case RC_OPCODE_DP3: snprintf(expr, sizeof(expr), "vec4(dot((%s).xyz, (%s).xyz))", a, b); break;
        case RC_OPCODE_DP4: snprintf(expr, sizeof(expr), "vec4(dot(%s, %s))", a, b); break;
        case RC_OPCODE_MIN: snprintf(expr, sizeof(expr), "min(%s, %s)", a, b); break;
        case RC_OPCODE_MAX: snprintf(expr, sizeof(expr), "max(%s, %s)", a, b); break;
        case RC_OPCODE_RCP: snprintf(expr, sizeof(expr), "vec4(1.0 / (%s).x)", a); break;
        case RC_OPCODE_RSQ: snprintf(expr, sizeof(expr), "vec4(inversesqrt(abs((%s).x)))", a); break;
        case RC_OPCODE_EX2: snprintf(expr, sizeof(expr), "vec4(exp2((%s).x))", a); break;
        case RC_OPCODE_LG2: snprintf(expr, sizeof(expr), "vec4(log2((%s).x))", a); break;
        case RC_OPCODE_SIN: snprintf(expr, sizeof(expr), "vec4(sin((%s).x))", a); break;
        case RC_OPCODE_COS: snprintf(expr, sizeof(expr), "vec4(cos((%s).x))", a); break;
        case RC_OPCODE_FRC: snprintf(expr, sizeof(expr), "fract(%s)", a); break;
        case RC_OPCODE_ROUND: snprintf(expr, sizeof(expr), "round(%s)", a); break;
        case RC_OPCODE_POW: snprintf(expr, sizeof(expr), "vec4(pow((%s).x, (%s).x))", a, b); break;
        case RC_OPCODE_CMP: snprintf(expr, sizeof(expr), "mix(%s, %s, lessThan(%s, vec4(0.0)))", cc, b, a); break;
        case RC_OPCODE_CND: snprintf(expr, sizeof(expr), "mix(%s, %s, greaterThan(%s, vec4(0.5)))", cc, b, a); break;
        case RC_OPCODE_SEQ: snprintf(expr, sizeof(expr), "vec4(equal(%s, %s))", a, b); break;
        case RC_OPCODE_SNE: snprintf(expr, sizeof(expr), "vec4(notEqual(%s, %s))", a, b); break;
        case RC_OPCODE_SGE: snprintf(expr, sizeof(expr), "vec4(greaterThanEqual(%s, %s))", a, b); break;
        case RC_OPCODE_SGT: snprintf(expr, sizeof(expr), "vec4(greaterThan(%s, %s))", a, b); break;
        case RC_OPCODE_SLE: snprintf(expr, sizeof(expr), "vec4(lessThanEqual(%s, %s))", a, b); break;
        case RC_OPCODE_SLT: snprintf(expr, sizeof(expr), "vec4(lessThan(%s, %s))", a, b); break;
        case RC_OPCODE_TEX:
        case RC_OPCODE_TXB:
        case RC_OPCODE_TXL:
        case RC_OPCODE_TXP:
        case RC_OPCODE_TXD:
            snprintf(expr, sizeof(expr), "%s(sampler%u, %s%s%s)",
                     inst->U.I.Opcode == RC_OPCODE_TXP ? "textureProj" :
                     inst->U.I.Opcode == RC_OPCODE_TXL ? "textureLod" :
                     inst->U.I.Opcode == RC_OPCODE_TXB ? "textureBias" :
                     inst->U.I.Opcode == RC_OPCODE_TXD ? "textureGrad" : "texture",
                     inst->U.I.TexSrcUnit, a,
                     info->NumSrcRegs > 1 ? ", " : "", info->NumSrcRegs > 1 ? b : "");
            break;
        default:
            snprintf(expr, sizeof(expr), "%s(", info->Name);
            for (i = 0; i < info->NumSrcRegs; i++) {
                strncat(expr, i == 0 ? a : i == 1 ? b : cc, sizeof(expr) - strlen(expr) - 1);
                if (i + 1 < info->NumSrcRegs)
                    strncat(expr, ", ", sizeof(expr) - strlen(expr) - 1);
            }
            strncat(expr, ")", sizeof(expr) - strlen(expr) - 1);
            break;
        }
        if (info->HasDstReg)
            printf("%*s%s = %s%s%s;\n", indent * 4, "", d, sat_open, expr, sat_close);
        else
            printf("%*s%s;\n", indent * 4, "", expr);
    }
    printf("}\n");
}

/* ------------------------------------------------------------------------
 * Compiler callbacks (what r300_fs.c / r300_vs.c would provide)
 * ------------------------------------------------------------------------ */

static const struct options *g_opts;

static void allocate_hardware_inputs(struct r300_fragment_program_compiler *c,
                                     void (*allocate)(void *data, unsigned input, unsigned hwreg),
                                     void *mydata)
{
    unsigned i, reg = 0;
    for (i = 0; i < 32; i++) {
        if (c->Base.Program.InputsRead & (1u << i)) {
            allocate(mydata, i, g_opts->dense_inputs ? reg++ : i);
        }
    }
}

static void set_vertex_inputs_outputs(struct r300_vertex_program_compiler *c)
{
    int i;
    for (i = 0; i < VSF_MAX_INPUTS; i++)
        c->code->inputs[i] = i;
    for (i = 0; i < VSF_MAX_OUTPUTS; i++)
        c->code->outputs[i] = i;
}

/* ------------------------------------------------------------------------
 * Output
 * ------------------------------------------------------------------------ */

static uint32_t f2u(float f)
{
    uint32_t u;
    memcpy(&u, &f, sizeof(u));
    return u;
}

/* Copied from Mesa r300_emit.c pack_float24() (MIT, Copyright 2008 Corbin
 * Simpson): R300 US constants are 24-bit floats with a 7-bit exponent. */
static uint32_t pack_float24(float f)
{
    union { float fl; uint32_t u; } u;
    float mantissa;
    int exponent;
    uint32_t float24 = 0;

    if (f == 0.0f)
        return 0;
    u.fl = f;
    mantissa = frexpf(f, &exponent);
    if (mantissa < 0) {
        float24 |= (1 << 23);
        mantissa = mantissa * -1.0f;
    }
    exponent += 62;
    float24 |= (exponent << 16);
    float24 |= (u.u & 0x7FFFFF) >> 7;
    return float24;
}

/* Value of a constant as the driver would upload it: immediates carry
 * their value; externals converted by --uniforms have it in the side table. */
static bool const_value(const struct rc_constant *k, float v[4])
{
    if (k->Type == RC_CONSTANT_IMMEDIATE) {
        memcpy(v, k->u.Immediate, sizeof(float) * 4);
        return true;
    }
    if (k->Type == RC_CONSTANT_EXTERNAL && k->u.External < 256 &&
        g_uniform_valid[k->u.External]) {
        memcpy(v, g_uniform_values[k->u.External], sizeof(float) * 4);
        return true;
    }
    return false;
}

static void section(const char *name)
{
    fflush(stdout);
    printf("=== %s ===\n", name);
    fflush(stdout);
}

static const char *const_type_name(unsigned t)
{
    switch (t) {
    case RC_CONSTANT_EXTERNAL:  return "EXTERNAL";
    case RC_CONSTANT_IMMEDIATE: return "IMMEDIATE";
    case RC_CONSTANT_STATE:     return "STATE";
    default:                    return "?";
    }
}

static const char *state_name(unsigned s)
{
    switch (s) {
    case RC_STATE_SHADOW_AMBIENT:          return "SHADOW_AMBIENT";
    case RC_STATE_R300_WINDOW_DIMENSION:   return "R300_WINDOW_DIMENSION";
    case RC_STATE_R300_TEXRECT_FACTOR:     return "R300_TEXRECT_FACTOR";
    case RC_STATE_R300_TEXSCALE_FACTOR:    return "R300_TEXSCALE_FACTOR";
    case RC_STATE_R300_VIEWPORT_SCALE:     return "R300_VIEWPORT_SCALE";
    case RC_STATE_R300_VIEWPORT_OFFSET:    return "R300_VIEWPORT_OFFSET";
    default:                               return "?";
    }
}

static void print_constants(const struct rc_constant_list *cl, const unsigned *remap,
                            unsigned remap_count, bool r300_float24)
{
    unsigned i;
    printf("# count %u\n", cl->Count);
    printf("# const N: x y z w | hex32 x y z w%s | type\n",
           r300_float24 ? " | float24 x y z w (as written to R300_PFS_PARAM)" : "");
    for (i = 0; i < cl->Count; i++) {
        const struct rc_constant *k = &cl->Constants[i];
        if (k->Type == RC_CONSTANT_IMMEDIATE) {
            const float *v = k->u.Immediate;
            printf("const %u: %.9g %.9g %.9g %.9g | %08x %08x %08x %08x",
                   i, v[0], v[1], v[2], v[3], f2u(v[0]), f2u(v[1]), f2u(v[2]), f2u(v[3]));
            if (r300_float24)
                printf(" | %06x %06x %06x %06x", pack_float24(v[0]), pack_float24(v[1]),
                       pack_float24(v[2]), pack_float24(v[3]));
            printf(" | IMMEDIATE size %u\n", k->Size);
        } else if (k->Type == RC_CONSTANT_STATE) {
            printf("const %u: STATE %s(%u) size %u\n", i, state_name(k->u.State[0]),
                   k->u.State[1], k->Size);
        } else if (k->Type == RC_CONSTANT_EXTERNAL && k->u.External < 256 &&
                   g_uniform_valid[k->u.External]) {
            const float *v = g_uniform_values[k->u.External];
            printf("const %u: %.9g %.9g %.9g %.9g | %08x %08x %08x %08x",
                   i, v[0], v[1], v[2], v[3], f2u(v[0]), f2u(v[1]), f2u(v[2]), f2u(v[3]));
            if (r300_float24)
                printf(" | %06x %06x %06x %06x", pack_float24(v[0]), pack_float24(v[1]),
                       pack_float24(v[2]), pack_float24(v[3]));
            printf(" | EXTERNAL %u (uniform; value from the .rc file, uploaded by "
                   "r300_emit_*_constants)\n", k->u.External);
        } else {
            printf("const %u: %s %u size %u\n", i, const_type_name(k->Type),
                   k->u.External, k->Size);
        }
    }
    if (remap) {
        printf("# remap table (hw index -> original const index), %u entries:", remap_count);
        for (i = 0; i < remap_count; i++)
            printf(" %u:%u", i, remap[i]);
        printf("\n");
    }
}

static void reg(const char *name, uint32_t addr, uint32_t val)
{
    printf("%-34s 0x%04x = 0x%08x\n", name, addr, val);
}

static void output_fs_r500(struct r300_fragment_program_compiler *c,
                           const struct options *o)
{
    struct rX00_fragment_program_code *gc = c->code;
    struct r500_fragment_program_code *code = &gc->code.r500;
    unsigned n = code->inst_end + 1;
    unsigned i;

    section("CODE");
    printf("# target r500-fs, %u instructions (inst_end %d), max_temp_idx %d\n",
           n, code->inst_end, code->max_temp_idx);
    printf("# inst N: inst0(CMN_INST) inst1(RGB_ADDR/TEX_ADDR) inst2(ALPHA_ADDR/TEX_DXDY/FC_INST) "
           "inst3(RGB_INST/TEX_INST/FC_ADDR) inst4(ALPHA_INST) inst5(RGBA_INST)\n");
    for (i = 0; i < n; i++) {
        printf("inst %u: %08x %08x %08x %08x %08x %08x\n", i,
               code->inst[i].inst0, code->inst[i].inst1, code->inst[i].inst2,
               code->inst[i].inst3, code->inst[i].inst4, code->inst[i].inst5);
    }

    section("CONSTANTS");
    print_constants(&gc->constants, gc->constants_remap_table,
                    gc->constants_remap_table ? gc->constants.Count : 0, false);
    printf("# int constants (loop counters, R500_US_FC_INT_CONST_n): %u\n",
           code->int_constant_count);
    for (i = 0; i < code->int_constant_count; i++)
        printf("int_const %u: %08x\n", i, code->int_constants[i]);

    section("REGS");
    printf("# as programmed by r300_fs.c r300_emit_fs_code_to_buffer() for R5xx\n");
    reg("R500_US_CONFIG", R500_US_CONFIG, R500_ZERO_TIMES_ANYTHING_EQUALS_ZERO);
    reg("R500_US_PIXSIZE", R500_US_PIXSIZE, (uint32_t)code->max_temp_idx);
    reg("R500_US_FC_CTRL", R500_US_FC_CTRL, code->us_fc_ctrl);
    for (i = 0; i < code->int_constant_count; i++) {
        char name[48];
        snprintf(name, sizeof(name), "R500_US_FC_INT_CONST_%u", i);
        reg(name, R500_US_FC_INT_CONST_0 + i * 4, code->int_constants[i]);
    }
    reg("R500_US_CODE_RANGE", R500_US_CODE_RANGE,
        R500_US_CODE_RANGE_ADDR(0) | R500_US_CODE_RANGE_SIZE(code->inst_end));
    reg("R500_US_CODE_OFFSET", R500_US_CODE_OFFSET, 0);
    reg("R500_US_CODE_ADDR", R500_US_CODE_ADDR,
        R500_US_CODE_START_ADDR(0) | R500_US_CODE_END_ADDR(code->inst_end));
    reg("R500_GA_US_VECTOR_INDEX (code)", R500_GA_US_VECTOR_INDEX,
        R500_GA_US_VECTOR_INDEX_TYPE_INSTR);
    printf("#   followed by %u dwords written to R500_GA_US_VECTOR_DATA (0x%04x)\n",
           n * 6, R500_GA_US_VECTOR_DATA);
    for (i = 0; i < gc->constants.Count; i++) {
        float v[4];
        if (const_value(&gc->constants.Constants[i], v)) {
            char name[48];
            snprintf(name, sizeof(name), "R500_GA_US_VECTOR_INDEX (const %u)", i);
            reg(name, R500_GA_US_VECTOR_INDEX,
                R500_GA_US_VECTOR_INDEX_TYPE_CONST | (i & R500_GA_US_VECTOR_INDEX_MASK));
        }
    }
    printf("#   each followed by 4 float dwords to R500_GA_US_VECTOR_DATA\n");
    reg("R300_FG_DEPTH_SRC", R300_FG_DEPTH_SRC,
        gc->writes_depth ? R300_FG_DEPTH_SRC_SHADER : R300_FG_DEPTH_SRC_SCAN);
    reg("R300_US_W_FMT", R300_US_W_FMT,
        gc->writes_depth ? (R300_W_FMT_W24 | R300_W_SRC_US) : (R300_W_FMT_W0 | R300_W_SRC_US));
    printf("writes_depth %u\n", gc->writes_depth);

    section("MESA DUMP");
    fflush(stdout);
    r500FragmentProgramDump(&c->Base, NULL);
    fflush(stderr);

    if (o->dump_hex) {
        section("HEX");
        printf("# code: %u dwords, 6 per instruction, upload order\n", n * 6);
        for (i = 0; i < n; i++) {
            printf("%08x\n%08x\n%08x\n%08x\n%08x\n%08x\n",
                   code->inst[i].inst0, code->inst[i].inst1, code->inst[i].inst2,
                   code->inst[i].inst3, code->inst[i].inst4, code->inst[i].inst5);
        }
        for (i = 0; i < gc->constants.Count; i++) {
            float v[4];
            if (const_value(&gc->constants.Constants[i], v)) {
                printf("# const %u\n%08x\n%08x\n%08x\n%08x\n", i,
                       f2u(v[0]), f2u(v[1]), f2u(v[2]), f2u(v[3]));
            }
        }
    }
}

static void output_fs_r300(struct r300_fragment_program_compiler *c,
                           const struct options *o)
{
    struct rX00_fragment_program_code *gc = c->code;
    struct r300_fragment_program_code *code = &gc->code.r300;
    unsigned i;

    section("CODE");
    printf("# target %s-fs, %u alu instructions, %u tex instructions, r390_mode %u\n",
           c->Base.is_r400 ? "r400" : "r300", code->alu.length, code->tex.length,
           code->r390_mode);
    printf("# alu N: rgb_inst rgb_addr alpha_inst alpha_addr r400_ext_addr\n");
    for (i = 0; i < code->alu.length; i++) {
        printf("alu %u: %08x %08x %08x %08x %08x\n", i,
               code->alu.inst[i].rgb_inst, code->alu.inst[i].rgb_addr,
               code->alu.inst[i].alpha_inst, code->alu.inst[i].alpha_addr,
               code->alu.inst[i].r400_ext_addr);
    }
    printf("# tex N: tex_inst\n");
    for (i = 0; i < code->tex.length; i++)
        printf("tex %u: %08x\n", i, code->tex.inst[i]);

    section("CONSTANTS");
    print_constants(&gc->constants, gc->constants_remap_table,
                    gc->constants_remap_table ? gc->constants.Count : 0, true);

    section("REGS");
    printf("# as programmed by r300_fs.c r300_emit_fs_code_to_buffer() for R3xx/R4xx\n");
    reg("R300_US_CONFIG", R300_US_CONFIG, code->config);
    reg("R300_US_PIXSIZE", R300_US_PIXSIZE, code->pixsize);
    reg("R300_US_CODE_OFFSET", R300_US_CODE_OFFSET, code->code_offset);
    if (code->r390_mode)
        reg("R400_US_CODE_EXT", R400_US_CODE_EXT, code->r400_code_offset_ext);
    else if (c->Base.is_r400)
        reg("R400_US_CODE_EXT", R400_US_CODE_EXT, 0);
    for (i = 0; i < 4; i++) {
        char name[32];
        snprintf(name, sizeof(name), "R300_US_CODE_ADDR_%u", i);
        reg(name, R300_US_CODE_ADDR_0 + i * 4, code->code_addr[i]);
    }
    if (c->Base.is_r400)
        reg("R400_US_CODE_BANK", R400_US_CODE_BANK,
            code->r390_mode ? R400_R390_MODE_ENABLE : 0);
    printf("# ALU words go to R300_US_ALU_RGB_INST_0 (0x%04x), R300_US_ALU_RGB_ADDR_0 (0x%04x),\n"
           "#   R300_US_ALU_ALPHA_INST_0 (0x%04x), R300_US_ALU_ALPHA_ADDR_0 (0x%04x)%s;\n"
           "#   TEX words to R300_US_TEX_INST_0 (0x%04x); 64 ALU / 32 TEX per bank\n",
           R300_US_ALU_RGB_INST_0, R300_US_ALU_RGB_ADDR_0, R300_US_ALU_ALPHA_INST_0,
           R300_US_ALU_ALPHA_ADDR_0,
           code->r390_mode ? ", R400_US_ALU_EXT_ADDR_0 (0x4ac0)" : "", R300_US_TEX_INST_0);
    for (i = 0; i < gc->constants.Count; i++) {
        float v[4];
        if (const_value(&gc->constants.Constants[i], v)) {
            char name[32];
            unsigned j;
            for (j = 0; j < 4; j++) {
                snprintf(name, sizeof(name), "R300_PFS_PARAM_%u_%c", i, "XYZW"[j]);
                reg(name, R300_PFS_PARAM_0_X + i * 16 + j * 4, pack_float24(v[j]));
            }
        }
    }
    reg("R300_FG_DEPTH_SRC", R300_FG_DEPTH_SRC,
        gc->writes_depth ? R300_FG_DEPTH_SRC_SHADER : R300_FG_DEPTH_SRC_SCAN);
    reg("R300_US_W_FMT", R300_US_W_FMT,
        gc->writes_depth ? (R300_W_FMT_W24 | R300_W_SRC_US) : (R300_W_FMT_W0 | R300_W_SRC_US));
    printf("writes_depth %u\n", gc->writes_depth);

    section("MESA DUMP");
    fflush(stdout);
    r300FragmentProgramDump(&c->Base, NULL);
    fflush(stderr);

    if (o->dump_hex) {
        section("HEX");
        printf("# rgb_inst[%u]\n", code->alu.length);
        for (i = 0; i < code->alu.length; i++) printf("%08x\n", code->alu.inst[i].rgb_inst);
        printf("# rgb_addr[%u]\n", code->alu.length);
        for (i = 0; i < code->alu.length; i++) printf("%08x\n", code->alu.inst[i].rgb_addr);
        printf("# alpha_inst[%u]\n", code->alu.length);
        for (i = 0; i < code->alu.length; i++) printf("%08x\n", code->alu.inst[i].alpha_inst);
        printf("# alpha_addr[%u]\n", code->alu.length);
        for (i = 0; i < code->alu.length; i++) printf("%08x\n", code->alu.inst[i].alpha_addr);
        if (code->r390_mode) {
            printf("# r400_ext_addr[%u]\n", code->alu.length);
            for (i = 0; i < code->alu.length; i++) printf("%08x\n", code->alu.inst[i].r400_ext_addr);
        }
        printf("# tex[%u]\n", code->tex.length);
        for (i = 0; i < code->tex.length; i++) printf("%08x\n", code->tex.inst[i]);
        for (i = 0; i < gc->constants.Count; i++) {
            float v[4];
            if (const_value(&gc->constants.Constants[i], v)) {
                printf("# const %u (float24)\n%08x\n%08x\n%08x\n%08x\n", i,
                       pack_float24(v[0]), pack_float24(v[1]),
                       pack_float24(v[2]), pack_float24(v[3]));
            }
        }
    }
}

static unsigned bitcount32(uint32_t v)
{
    unsigned n = 0;
    while (v) { n += v & 1; v >>= 1; }
    return n;
}

static void output_vs(struct r300_vertex_program_compiler *c, const struct options *o)
{
    struct r300_vertex_program_code *code = c->code;
    unsigned n = code->length / 4;
    unsigned i;
    bool r500 = c->Base.is_r500;
    unsigned max_const = code->constants.Count ? code->constants.Count - 1 : 0;

    section("CODE");
    printf("# target %s-vs, %u instructions (%d dwords), num_temporaries %d\n",
           r500 ? "r500" : "r300", n, code->length, code->num_temporaries);
    printf("# inst N: op src0 src1 src2\n");
    for (i = 0; i < n; i++) {
        printf("inst %u: %08x %08x %08x %08x\n", i, code->body.d[i * 4],
               code->body.d[i * 4 + 1], code->body.d[i * 4 + 2], code->body.d[i * 4 + 3]);
    }
    printf("# flow control: num_fc_ops %u fc_ops %08x\n", code->num_fc_ops, code->fc_ops);
    for (i = 0; i < code->num_fc_ops; i++) {
        if (r500)
            printf("fc %u: lw %08x uw %08x loop_index %08x\n", i,
                   code->fc_op_addrs.r500[i].lw, code->fc_op_addrs.r500[i].uw,
                   (uint32_t)code->fc_loop_index[i]);
        else
            printf("fc %u: addr %08x loop_index %08x\n", i, code->fc_op_addrs.r300[i],
                   (uint32_t)code->fc_loop_index[i]);
    }
    printf("# inputs_read %08x outputs_written %08x\n", code->InputsRead, code->OutputsWritten);
    printf("# input[i] -> hw input i, output[i] -> hw output i (set_vertex_inputs_outputs)\n");

    section("CONSTANTS");
    print_constants(&code->constants, code->constants_remap_table,
                    code->constants_remap_table ? code->constants.Count : 0, false);

    section("REGS");
    printf("# as programmed by r300_emit.c r300_emit_vs_state() / r300_emit_vs_constants()\n");
    reg("R300_VAP_PVS_CODE_CNTL_0", R300_VAP_PVS_CODE_CNTL_0,
        R300_PVS_FIRST_INST(0) | R300_PVS_XYZW_VALID_INST(code->last_pos_write) |
        R300_PVS_LAST_INST(n ? n - 1 : 0));
    reg("R300_VAP_PVS_CODE_CNTL_1", R300_VAP_PVS_CODE_CNTL_1, code->last_input_read);
    printf("#   last_pos_write %u last_input_read %u pos_end %d\n", code->last_pos_write,
           code->last_input_read, code->pos_end);
    reg("R300_VAP_PVS_VECTOR_INDX_REG (code)", R300_VAP_PVS_VECTOR_INDX_REG, R300_PVS_CODE_START);
    printf("#   followed by %d dwords written to R300_VAP_PVS_UPLOAD_DATA (0x%04x)\n",
           code->length, R300_VAP_PVS_UPLOAD_DATA);
    reg("R300_VAP_PVS_CONST_CNTL", R300_VAP_PVS_CONST_CNTL,
        R300_PVS_CONST_BASE_OFFSET(0) | R300_PVS_MAX_CONST_ADDR(max_const));
    reg("R300_VAP_PVS_VECTOR_INDX_REG (consts)", R300_VAP_PVS_VECTOR_INDX_REG,
        r500 ? R500_PVS_CONST_START : R300_PVS_CONST_START);
    printf("#   followed by 4 float dwords per constant to R300_VAP_PVS_UPLOAD_DATA\n");
    reg("R300_VAP_PVS_FLOW_CNTL_OPC", R300_VAP_PVS_FLOW_CNTL_OPC, code->fc_ops);
    for (i = 0; i < R300_VS_MAX_FC_OPS; i++) {
        char name[48];
        if (r500) {
            snprintf(name, sizeof(name), "R500_VAP_PVS_FLOW_CNTL_ADDRS_LW_%u", i);
            reg(name, R500_VAP_PVS_FLOW_CNTL_ADDRS_LW_0 + i * 8, code->fc_op_addrs.r500[i].lw);
            snprintf(name, sizeof(name), "R500_VAP_PVS_FLOW_CNTL_ADDRS_UW_%u", i);
            reg(name, R500_VAP_PVS_FLOW_CNTL_ADDRS_UW_0 + i * 8, code->fc_op_addrs.r500[i].uw);
        } else {
            snprintf(name, sizeof(name), "R300_VAP_PVS_FLOW_CNTL_ADDRS_%u", i);
            reg(name, R300_VAP_PVS_FLOW_CNTL_ADDRS_0 + i * 4, code->fc_op_addrs.r300[i]);
        }
    }
    for (i = 0; i < R300_VS_MAX_FC_OPS; i++) {
        char name[48];
        snprintf(name, sizeof(name), "R300_VAP_PVS_FLOW_CNTL_LOOP_INDEX_%u", i);
        reg(name, R300_VAP_PVS_FLOW_CNTL_LOOP_INDEX_0 + i * 4, (uint32_t)code->fc_loop_index[i]);
    }
    {
        unsigned vtx_mem_size = r500 ? 128 : 72;
        unsigned input_count = bitcount32(code->InputsRead);
        unsigned output_count = bitcount32(code->OutputsWritten);
        unsigned temp_count = code->num_temporaries;
        unsigned slots, cntlrs;
        if (input_count < 1) input_count = 1;
        if (output_count < 1) output_count = 1;
        if (temp_count < 1) temp_count = 1;
        slots = vtx_mem_size / input_count;
        if (vtx_mem_size / output_count < slots) slots = vtx_mem_size / output_count;
        if (slots > 10) slots = 10;
        cntlrs = vtx_mem_size / temp_count;
        if (cntlrs > 5) cntlrs = 5;
        printf("# R300_VAP_CNTL (0x%04x): PVS_NUM_SLOTS %u PVS_NUM_CNTLRS %u "
               "(NUM_FPUS / VF_MAX_VTX_NUM / clip bits depend on the chip and state)\n",
               R300_VAP_CNTL, slots, cntlrs);
    }

    section("MESA DUMP");
    fflush(stdout);
    r300_vertex_program_dump(&c->Base, NULL);
    fflush(stderr);

    if (o->dump_hex) {
        section("HEX");
        printf("# code: %d dwords, 4 per instruction, upload order\n", code->length);
        for (i = 0; i < (unsigned)code->length; i++)
            printf("%08x\n", code->body.d[i]);
        for (i = 0; i < code->constants.Count; i++) {
            float v[4];
            if (const_value(&code->constants.Constants[i], v)) {
                printf("# const %u\n%08x\n%08x\n%08x\n%08x\n", i,
                       f2u(v[0]), f2u(v[1]), f2u(v[2]), f2u(v[3]));
            }
        }
    }
}

/* ------------------------------------------------------------------------
 * main
 * ------------------------------------------------------------------------ */

static void setup_common(struct radeon_compiler *c, const struct options *o,
                         struct rc_regalloc_state *rs, enum rc_program_type type,
                         bool is_r500, bool is_r400)
{
    rc_init_regalloc_state(rs, type);
    rc_init(c, rs);
    c->type = type;
    c->is_r500 = is_r500;
    c->is_r400 = is_r400;
    c->disable_optimizations = o->no_opt;
    c->debug = NULL;
    if (o->debug)
        c->Debug |= RC_DBG_LOG;
}

int main(int argc, char **argv)
{
    struct options o;
    struct rc_regalloc_state rs;
    struct rX00_fragment_program_code fs_code;
    struct r300_vertex_program_code vs_code;
    struct r300_fragment_program_compiler fs;
    struct r300_vertex_program_compiler vs;
    struct radeon_compiler *base;
    bool is_fs;
    int i;

    memset(&o, 0, sizeof(o));
    o.target = TARGET_FS_R500;
    o.depth_output = 4;

    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--fs-r500")) o.target = TARGET_FS_R500;
        else if (!strcmp(a, "--fs-r300")) o.target = TARGET_FS_R300;
        else if (!strcmp(a, "--fs-r400")) o.target = TARGET_FS_R400;
        else if (!strcmp(a, "--vs-r300")) o.target = TARGET_VS_R300;
        else if (!strcmp(a, "--vs-r500")) o.target = TARGET_VS_R500;
        else if (!strcmp(a, "--dump-hex")) o.dump_hex = true;
        else if (!strcmp(a, "--glsl-like")) o.glsl_like = true;
        else if (!strcmp(a, "--debug")) o.debug = true;
        else if (!strcmp(a, "--no-opt")) o.no_opt = true;
        else if (!strcmp(a, "--remove-unused-constants")) o.remove_unused_constants = true;
        else if (!strcmp(a, "--keep-stderr")) o.keep_stderr = true;
        else if (!strcmp(a, "--dense-inputs")) o.dense_inputs = true;
        else if (!strcmp(a, "--uniforms")) o.uniforms = true;
        else if (!strcmp(a, "--depth-output")) {
            if (i + 1 >= argc) { usage(stderr); return 2; }
            o.depth_output = atoi(argv[++i]);
        }
        else if (!strcmp(a, "-h") || !strcmp(a, "--help")) { usage(stdout); return 0; }
        else if (a[0] == '-' && a[1] != '\0') {
            fprintf(stderr, "rc_compile: unknown option %s\n", a);
            usage(stderr);
            return 2;
        }
        else if (!o.file) o.file = a;
        else { usage(stderr); return 2; }
    }
    if (!o.file) {
        usage(stderr);
        return 2;
    }
    g_opts = &o;

    /* Mesa's dumps print to stderr; merge so everything is in order. */
    if (!o.keep_stderr) {
        fflush(stdout);
        fflush(stderr);
        dup2(STDOUT_FILENO, STDERR_FILENO);
        setvbuf(stdout, NULL, _IOLBF, 0);
    }

    is_fs = o.target == TARGET_FS_R500 || o.target == TARGET_FS_R300 ||
            o.target == TARGET_FS_R400;

    memset(&fs, 0, sizeof(fs));
    memset(&vs, 0, sizeof(vs));
    memset(&fs_code, 0, sizeof(fs_code));
    memset(&vs_code, 0, sizeof(vs_code));

    if (is_fs) {
        /* Mirrors r300_fs.c r300_translate_fragment_shader(). */
        bool r500 = o.target == TARGET_FS_R500;
        bool r400 = o.target == TARGET_FS_R400;
        setup_common(&fs.Base, &o, &rs, RC_FRAGMENT_PROGRAM, r500, r400);
        fs.code = &fs_code;
        fs.Base.has_half_swizzles = true;
        fs.Base.has_presub = true;
        fs.Base.has_omod = true;
        fs.Base.max_temp_regs = r500 ? 128 : (r400 ? 64 : 32);
        fs.Base.max_constants = r500 ? 256 : 32;
        fs.Base.max_alu_insts = (r500 || r400) ? 512 : 64;
        fs.Base.max_tex_insts = (r500 || r400) ? 512 : 32;
        fs.AllocateHwInputs = &allocate_hardware_inputs;
        fs.UserData = NULL;
        for (i = 0; i < 4; i++)
            fs.OutputColor[i] = i;
        fs.OutputDepth = o.depth_output;
        base = &fs.Base;
    } else {
        /* Mirrors r300_vs.c r300_translate_vertex_shader(). */
        bool r500 = o.target == TARGET_VS_R500;
        setup_common(&vs.Base, &o, &rs, RC_VERTEX_PROGRAM, r500, false);
        vs.code = &vs_code;
        vs.UserData = NULL;
        vs.Base.has_half_swizzles = false;
        vs.Base.has_presub = false;
        vs.Base.has_omod = false;
        vs.Base.max_temp_regs = 32;
        vs.Base.max_constants = 256;
        vs.Base.max_alu_insts = r500 ? 1024 : 256;
        vs.SetHwInputOutput = &set_vertex_inputs_outputs;
        base = &vs.Base;
    }

    load_file(base, o.file);
    if (o.uniforms)
        make_constants_external(base);
    rc_calculate_inputs_outputs(base);
    base->remove_unused_constants = o.remove_unused_constants;
    if (!is_fs) {
        /* r300_vs.c requires every declared output; here every output the
         * program writes is required, so no artificial outputs are added. */
        vs.RequiredOutputs = base->Program.OutputsWritten;
    }

    printf("# rc_compile: %s, target %s\n", o.file,
           o.target == TARGET_FS_R500 ? "r500-fs" : o.target == TARGET_FS_R300 ? "r300-fs" :
           o.target == TARGET_FS_R400 ? "r400-fs" : o.target == TARGET_VS_R300 ? "r300-vs" :
           "r500-vs");
    section("INPUT");
    printf("# inputs_read %08x outputs_written %08x shadow_samplers %08x\n",
           base->Program.InputsRead, base->Program.OutputsWritten, base->Program.ShadowSamplers);
    fflush(stdout);
    rc_print_program(&base->Program);
    fflush(stderr);
    rc_constants_print(&base->Program.Constants);
    fflush(stderr);

    if (o.glsl_like) {
        section("GLSL-LIKE");
        print_glsl_like(&base->Program);
    }

    section("COMPILE");
    fflush(stdout);
    if (is_fs)
        r3xx_compile_fragment_program(&fs);
    else
        r3xx_compile_vertex_program(&vs);
    fflush(stderr);

    if (base->Error) {
        fflush(stdout);
        printf("=== ERROR ===\n%s\n", base->ErrorMsg ? base->ErrorMsg : "(no message)");
        fflush(stdout);
        fprintf(stderr, "rc_compile: compilation failed: %s\n",
                base->ErrorMsg ? base->ErrorMsg : "(no message)");
        return 1;
    }

    if (o.target == TARGET_FS_R500)
        output_fs_r500(&fs, &o);
    else if (is_fs)
        output_fs_r300(&fs, &o);
    else
        output_vs(&vs, &o);

    section("FINAL PROGRAM");
    printf("# rc_print_program() of the compiler's final (paired/scheduled) IR\n");
    fflush(stdout);
    rc_print_program(&base->Program);
    fflush(stderr);

    section("END");

    if (is_fs) {
        free(fs_code.constants.Constants);
        free(fs_code.constants_remap_table);
    } else {
        free(vs_code.constants.Constants);
        free(vs_code.constants_remap_table);
    }
    rc_destroy(base);
    rc_destroy_regalloc_state(&rs);
    return 0;
}
