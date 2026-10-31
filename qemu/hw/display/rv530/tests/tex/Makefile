# Standalone tests for the R300/R500 texture module (r5xx_tex.c).
CC      ?= gcc
CFLAGS  ?= -std=gnu11 -O2 -g -Wall -Wextra -Wno-unused-parameter
TOP     := ../..
CPPFLAGS += -I$(TOP)
LDLIBS  += -lm

SRCS = test_tex.c $(TOP)/r5xx_tex.c $(TOP)/r5xx_core.c
HDRS = $(TOP)/r5xx_tex.h $(TOP)/r5xx_core.h

all: test_tex

test_tex: $(SRCS) $(HDRS)
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ $(SRCS) $(LDLIBS)

check: test_tex
	./test_tex

clean:
	rm -f test_tex

.PHONY: all check clean
