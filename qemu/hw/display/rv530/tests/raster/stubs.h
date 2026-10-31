/* Test-only knobs for the stand-in fragment shader (see stubs.c). */
#ifndef RASTER_TEST_STUBS_H
#define RASTER_TEST_STUBS_H

enum {
    STUB_US_PASSTHROUGH = 0,
    STUB_US_KILL,
    STUB_US_DEPTH,
    STUB_US_MRT,
};

extern int stub_us_mode;
extern int stub_us_calls;

#endif
