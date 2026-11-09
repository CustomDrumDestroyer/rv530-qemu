# Tests for the R300/R500 vertex program module (r5xx_pvs.c).
#
#   make check     build and run pvs_test (corpus round-trip + execution,
#                  hand-encoded programs, GLSL validation with glslangValidator)
#   make regen     recompile corpus/*.rc with tools/mesa-rc/rc_compile
#                  (needs Mesa sources; the .out files are checked in so
#                  `make check` does not depend on the tool)

CC      ?= gcc
CFLAGS  ?= -std=gnu11 -O2 -g -Wall -Wextra -Wno-unused-parameter
LDLIBS   = -lm
TOP      = ../..
# the bare name when it is on PATH: on Windows (MSYS2) the test runs it
# through cmd.exe, which cannot use an MSYS2 path
GLSLANG ?= $(if $(shell command -v glslangValidator),glslangValidator,/usr/bin/glslangValidator)
RC       = $(TOP)/tools/mesa-rc/rc_compile

SRCS = pvs_test.c $(TOP)/r5xx_pvs.c $(TOP)/r5xx_core.c
RCS  = $(sort $(wildcard corpus/*.rc))

all: pvs_test

pvs_test: $(SRCS) $(TOP)/r5xx_pvs.h $(TOP)/r5xx_core.h
	$(CC) $(CFLAGS) -I$(TOP) -o $@ $(SRCS) $(LDLIBS)

check: pvs_test
	mkdir -p build
	./pvs_test -c corpus -o build -g $(GLSLANG)

regen: $(RC)
	@for f in $(RCS); do \
		t=$$(sed -n 's/^# *target: *\([a-z0-9-]*\).*/\1/p' $$f | head -1); \
		echo "$(RC) --$$t --dump-hex $$f > $${f%.rc}.out"; \
		$(RC) --$$t --dump-hex $$f > $${f%.rc}.out || exit 1; \
	done

$(RC):
	$(MAKE) -C $(TOP)/tools/mesa-rc

clean:
	rm -rf pvs_test build

.PHONY: all check regen clean
