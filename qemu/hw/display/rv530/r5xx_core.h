/*
 * R5xx (Radeon X1000 / RV530) emulation - core definitions shared by the
 * QEMU device model, the standalone replay tool and the unit tests.
 *
 * Everything declared here must be usable without QEMU headers: only the
 * C standard library.  The QEMU-specific glue lives in rv530.c, r5xx_mc.c,
 * r5xx_cp.c and r5xx_avivo.c.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef R5XX_CORE_H
#define R5XX_CORE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdarg.h>

/* ------------------------------------------------------------------ */
/* Basic math types                                                    */
/* ------------------------------------------------------------------ */

typedef struct r5xx_vec4 {
    float f[4];                     /* x, y, z, w  ==  r, g, b, a */
} r5xx_vec4;

static inline r5xx_vec4 r5xx_vec4_make(float x, float y, float z, float w)
{
    r5xx_vec4 v = { { x, y, z, w } };
    return v;
}

/* ------------------------------------------------------------------ */
/* GPU address space access                                            */
/* ------------------------------------------------------------------ */

/*
 * Every GPU-side memory access (vertex fetch, index fetch, texture reads,
 * render target reads/writes, PM4 fetch) goes through this interface.
 * Addresses are 32-bit GPU (MC) addresses; the implementation resolves
 * them to VRAM or to system memory through the PCIe GART.
 *
 * map() returns a host pointer valid for [addr, addr+len) if that range is
 * physically contiguous on the host (always true inside VRAM, true inside
 * a single GART page), otherwise NULL and the caller must fall back to
 * read()/write().  Pointers returned by map() stay valid until the next
 * GART or MC reprogramming; callers never cache them across draws.
 */
typedef struct R5xxMem {
    void *opaque;
    uint8_t *(*map)(void *opaque, uint32_t gpu_addr, uint32_t len, bool write);
    void (*read)(void *opaque, uint32_t gpu_addr, void *buf, uint32_t len);
    void (*write)(void *opaque, uint32_t gpu_addr, const void *buf, uint32_t len);
    /* diagnostic output, level: 0 = error, 1 = warning, 2 = info, 3 = debug */
    void (*log)(void *opaque, int level, const char *fmt, va_list ap)
        __attribute__((format(printf, 3, 0)));
} R5xxMem;

static inline uint32_t r5xx_mem_read32(const R5xxMem *m, uint32_t addr)
{
    uint32_t v;
    m->read(m->opaque, addr, &v, 4);
    return v;
}

static inline void r5xx_mem_write32(const R5xxMem *m, uint32_t addr, uint32_t v)
{
    m->write(m->opaque, addr, &v, 4);
}

void r5xx_logf(const R5xxMem *m, int level, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

/* A trivial R5xxMem backed by a flat host buffer (tests, replay tool). */
typedef struct R5xxFlatMem {
    R5xxMem mem;
    uint8_t *base;
    uint32_t size;
    uint32_t gpu_base;              /* GPU address of base[0] */
    int log_level;
} R5xxFlatMem;

void r5xx_flatmem_init(R5xxFlatMem *fm, uint8_t *base, uint32_t size,
                       uint32_t gpu_base);

/* ------------------------------------------------------------------ */
/* Register file                                                       */
/* ------------------------------------------------------------------ */

#define R5XX_REG_SPACE_SIZE  0x10000          /* 64 KiB MMIO BAR */
#define R5XX_NUM_REGS        (R5XX_REG_SPACE_SIZE / 4)

/* Register-name lookup (generated table, r5xx_regnames.c).  Returns a
 * static string or NULL.  Offsets are byte offsets into the MMIO BAR. */
const char *r5xx_regname(uint32_t offset);
const char *r5xx_mc_regname(uint32_t index);      /* MC indirect space */
const char *r5xx_pcie_regname(uint32_t index);    /* PCIE indirect space */
const char *r5xx_pll_regname(uint32_t index);     /* CLOCK_CNTL indirect space */
const char *r5xx_pm4_opname(uint32_t opcode);     /* type-3 opcode */

/* Helpers for bit fields */
#define R5XX_BITS(v, lo, hi)  (((v) >> (lo)) & ((1u << ((hi) - (lo) + 1)) - 1))
#define R5XX_BIT(v, b)        (((v) >> (b)) & 1u)

/* ------------------------------------------------------------------ */
/* Pixel format helpers shared by scanout, textures and render targets */
/* ------------------------------------------------------------------ */

/* Canonical host representation of a color: float RGBA 0..1 (or beyond
 * for float formats). */
void r5xx_unpack_argb8888(uint32_t pix, r5xx_vec4 *out);
uint32_t r5xx_pack_argb8888(const r5xx_vec4 *c);

float r5xx_half_to_float(uint16_t h);
uint16_t r5xx_float_to_half(float f);

static inline float r5xx_clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

#endif /* R5XX_CORE_H */
