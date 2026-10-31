# Unit tests for the CPU rasterizer (r5xx_raster.c).
# r5xx_tex.c / r500_us.c are replaced by stubs.c (see that file).

CC      ?= gcc
CFLAGS  ?= -std=gnu11 -O2 -g -Wall -Wextra -Wno-unused-parameter
TOP     := ../..
CPPFLAGS += -I$(TOP) -I.

SRCS := test_raster.c stubs.c $(TOP)/r5xx_raster.c $(TOP)/r5xx_core.c
OBJS := test_raster.o stubs.o r5xx_raster.o r5xx_core.o

all: test_raster

test_raster: $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) -lm

test_raster.o: test_raster.c stubs.h $(TOP)/r5xx_raster.h
	$(CC) $(CFLAGS) $(CPPFLAGS) -c -o $@ $<

stubs.o: stubs.c stubs.h $(TOP)/r5xx_tex.h $(TOP)/r500_us.h
	$(CC) $(CFLAGS) $(CPPFLAGS) -c -o $@ $<

r5xx_raster.o: $(TOP)/r5xx_raster.c $(TOP)/r5xx_raster.h $(TOP)/r5xx_tex.h $(TOP)/r500_us.h $(TOP)/r5xx_core.h
	$(CC) $(CFLAGS) $(CPPFLAGS) -c -o $@ $<

r5xx_core.o: $(TOP)/r5xx_core.c $(TOP)/r5xx_core.h
	$(CC) $(CFLAGS) $(CPPFLAGS) -c -o $@ $<

check: test_raster
	./test_raster

clean:
	rm -f $(OBJS) test_raster

.PHONY: all check clean
