# Tests for the R500 fragment program module (r500_us.c).
#
#   make check        build and run everything
#
# The corpus test needs the Mesa compiler driver tools/mesa-rc/rc_compile
# (built with `make -C ../../tools/mesa-rc`); when it is missing the
# corpus is compiled with the local fallback in mesa-rc-local/ if that
# exists, otherwise the corpus cases are skipped with a notice.

CC      ?= gcc
CFLAGS  ?= -std=gnu11 -O2 -g -Wall -Wextra -Wno-unused-parameter
TOP     := ../..
SRCS    := test_us.c $(TOP)/r500_us.c $(TOP)/r5xx_core.c $(TOP)/r5xx_vk_state.c $(TOP)/r5xx_tex.c
RC      := $(TOP)/tools/mesa-rc/rc_compile
RC_LOCAL:= mesa-rc-local/rc_compile
GLSLANG ?= glslangValidator

all: test_us

test_us: $(SRCS) $(TOP)/r500_us.h $(TOP)/r5xx_core.h $(TOP)/r5xx_tex.h $(TOP)/r5xx_vk.h $(TOP)/r5xx_vk_glsl.h
	$(CC) $(CFLAGS) -I$(TOP) -o $@ $(SRCS) -lm

check: test_us
	@if [ -x $(RC) ]; then RCC=$(RC); elif [ -x $(RC_LOCAL) ]; then RCC=$(RC_LOCAL); else RCC=; fi; \
	./test_us --rc "$$RCC" --corpus corpus --glslang $(GLSLANG)

clean:
	rm -f test_us *.o

.PHONY: all check clean
