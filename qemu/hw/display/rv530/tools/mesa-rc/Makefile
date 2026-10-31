# rc_compile: standalone driver for Mesa's r300/r500 shader compiler.
#
# Mesa sources are compiled in place from $(MESA) (nothing is copied into
# this tree).  Only rc_compile.c, stubs/, samples/ and this Makefile are ours.
#
#   make            build ./rc_compile
#   make check      compile every samples/*.rc and show the output
#   make clean
#
# Override the Mesa location with:  make MESA=/path/to/mesa

MESA      ?= $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../../../../../../reference/mesa)
RC_DIR     = $(MESA)/src/gallium/drivers/r300/compiler
UTIL_DIR   = $(MESA)/src/util

CC        ?= gcc
BUILD      = build

# Compiler core (meson.build files_r300, minus the NIR front end:
# nir_to_rc.c, r300_nir.c, r500_nir_lower_fcsel.c are not needed to drive
# r3xx_compile_{fragment,vertex}_program from an rc_program).
RC_SRCS = \
	memory_pool.c \
	r300_fragprog.c \
	r300_fragprog_emit.c \
	r300_fragprog_swizzle.c \
	r3xx_fragprog.c \
	r3xx_vertprog.c \
	r3xx_vertprog_dump.c \
	r500_fragprog.c \
	r500_fragprog_emit.c \
	radeon_code.c \
	radeon_compiler.c \
	radeon_compiler_util.c \
	radeon_dataflow.c \
	radeon_dataflow_deadcode.c \
	radeon_dataflow_swizzles.c \
	radeon_inline_literals.c \
	radeon_list.c \
	radeon_opcodes.c \
	radeon_optimize.c \
	radeon_pair_dead_sources.c \
	radeon_pair_regalloc.c \
	radeon_pair_schedule.c \
	radeon_pair_translate.c \
	radeon_program.c \
	radeon_program_alu.c \
	radeon_program_pair.c \
	radeon_program_print.c \
	radeon_program_tex.c \
	radeon_regalloc.c \
	radeon_remove_constants.c \
	radeon_rename_regs.c \
	radeon_variable.c \
	radeon_vert_fc.c

# The unit-test parser we reuse (add_instruction / add_constant / ...).
TEST_SRCS = rc_test_helpers.c

# Mesa util pieces the compiler needs (graph-coloring register allocator,
# ralloc, blob, dynarray, printf helper).  Everything else is stubbed in
# stubs/mesa_stubs.c (util_debug_message, debug_get_num_option, mesa_log).
UTIL_SRCS = register_allocate.c ralloc.c blob.c u_dynarray.c u_printf.c

OWN_SRCS  = rc_compile.c stubs/mesa_stubs.c

VPATH = $(RC_DIR):$(RC_DIR)/tests:$(UTIL_DIR):.

OBJS = $(addprefix $(BUILD)/rc/,$(RC_SRCS:.c=.o)) \
       $(addprefix $(BUILD)/rc/,$(TEST_SRCS:.c=.o)) \
       $(addprefix $(BUILD)/util/,$(UTIL_SRCS:.c=.o)) \
       $(addprefix $(BUILD)/own/,$(notdir $(OWN_SRCS:.c=.o)))

INCLUDES = -Istubs \
	-I$(MESA)/src \
	-I$(MESA)/include \
	-I$(MESA)/src/gallium/include \
	-I$(MESA)/src/gallium/auxiliary \
	-I$(MESA)/src/gallium/drivers/r300 \
	-I$(RC_DIR) \
	-I$(RC_DIR)/tests

# HAVE_ENDIAN_H: util/u_endian.h needs to know how to detect byte order.
# HAVE_PTHREAD:  util/u_thread.h / simple_mtx.h defaults.
# TEST_PATH:     rc_test_helpers.c's load_program() (unused by us) needs it.
DEFINES  = -DHAVE_ENDIAN_H -DHAVE_PTHREAD -DTEST_PATH='"$(RC_DIR)/tests"'

COMMON_CFLAGS = -std=gnu11 -O2 -g -Wall -Wno-unused-parameter $(INCLUDES) $(DEFINES)
# Mesa's own sources are compiled with warnings relaxed; ours with -Wextra.
MESA_CFLAGS = $(COMMON_CFLAGS) -Wno-unused-variable -Wno-unused-function \
	-Wno-sign-compare -Wno-missing-field-initializers
OWN_CFLAGS  = $(COMMON_CFLAGS) -Wextra -Wno-sign-compare  # (sign-compare: Mesa headers)
LDFLAGS   ?=
LDLIBS     = -lm

all: rc_compile

rc_compile: $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $(OBJS) $(LDLIBS)

$(BUILD)/rc/%.o: %.c | $(BUILD)/rc
	$(CC) $(MESA_CFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/util/%.o: %.c | $(BUILD)/util
	$(CC) $(MESA_CFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/own/%.o: %.c | $(BUILD)/own
	$(CC) $(OWN_CFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/own/mesa_stubs.o: stubs/mesa_stubs.c | $(BUILD)/own
	$(CC) $(OWN_CFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/rc $(BUILD)/util $(BUILD)/own:
	mkdir -p $@

# ---------------------------------------------------------------------------
# Samples.  Each samples/NAME.rc carries its target in a "# target: ..." line
# (one of fs-r500, fs-r300, fs-r400, vs-r300, vs-r500) and optional extra
# flags in a "# flags: ..." line.
# ---------------------------------------------------------------------------
SAMPLES = $(sort $(wildcard samples/*.rc))

check: rc_compile
	@status=0; \
	for s in $(SAMPLES); do \
		target=$$(sed -n 's/^# *target: *\([a-z0-9-]*\).*/\1/p' $$s | head -1); \
		flags=$$(sed -n 's/^# *flags: *//p' $$s | head -1); \
		[ -n "$$target" ] || target=fs-r500; \
		echo "###############################################################"; \
		echo "### ./rc_compile --$$target $$flags $$s"; \
		echo "###############################################################"; \
		if ./rc_compile --$$target $$flags $$s; then \
			echo "### PASS $$s"; \
		else \
			echo "### FAIL $$s"; status=1; \
		fi; \
		echo; \
	done; \
	echo "check: $$(echo $(SAMPLES) | wc -w) samples"; \
	exit $$status

clean:
	rm -rf $(BUILD) rc_compile

-include $(OBJS:.o=.d)

.PHONY: all check clean
