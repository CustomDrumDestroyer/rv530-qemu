/*
 * R5xx emulation core helpers (QEMU-independent).
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "r5xx_core.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

void r5xx_logf(const R5xxMem *m, int level, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    if (m && m->log) {
        m->log(m->opaque, level, fmt, ap);
    } else {
        vfprintf(stderr, fmt, ap);
        fputc('\n', stderr);
    }
    va_end(ap);
}

/* ---- flat memory ---- */

static uint8_t *flat_map(void *opaque, uint32_t addr, uint32_t len, bool write)
{
    R5xxFlatMem *fm = opaque;
    uint64_t off = (uint64_t)addr - fm->gpu_base;
    if (addr < fm->gpu_base || off + len > fm->size) {
        return NULL;
    }
    return fm->base + off;
}

static void flat_read(void *opaque, uint32_t addr, void *buf, uint32_t len)
{
    R5xxFlatMem *fm = opaque;
    uint8_t *p = flat_map(opaque, addr, len, false);
    if (p) {
        memcpy(buf, p, len);
    } else {
        if (fm->log_level >= 1) {
            fprintf(stderr, "r5xx: flatmem read fault addr=0x%08x len=%u\n",
                    addr, len);
        }
        memset(buf, 0, len);
    }
}

static void flat_write(void *opaque, uint32_t addr, const void *buf, uint32_t len)
{
    R5xxFlatMem *fm = opaque;
    uint8_t *p = flat_map(opaque, addr, len, true);
    if (p) {
        memcpy(p, buf, len);
    } else if (fm->log_level >= 1) {
        fprintf(stderr, "r5xx: flatmem write fault addr=0x%08x len=%u\n",
                addr, len);
    }
}

static void __attribute__((format(printf, 3, 0))) flat_log(void *opaque, int level, const char *fmt, va_list ap)
{
    R5xxFlatMem *fm = opaque;
    if (level <= fm->log_level) {
        vfprintf(stderr, fmt, ap);
        fputc('\n', stderr);
    }
}

void r5xx_flatmem_init(R5xxFlatMem *fm, uint8_t *base, uint32_t size,
                       uint32_t gpu_base)
{
    memset(fm, 0, sizeof(*fm));
    fm->base = base;
    fm->size = size;
    fm->gpu_base = gpu_base;
    fm->log_level = 1;
    fm->mem.opaque = fm;
    fm->mem.map = flat_map;
    fm->mem.read = flat_read;
    fm->mem.write = flat_write;
    fm->mem.log = flat_log;
}

/* ---- formats ---- */

void r5xx_unpack_argb8888(uint32_t pix, r5xx_vec4 *out)
{
    out->f[0] = ((pix >> 16) & 0xff) / 255.0f;
    out->f[1] = ((pix >> 8) & 0xff) / 255.0f;
    out->f[2] = (pix & 0xff) / 255.0f;
    out->f[3] = ((pix >> 24) & 0xff) / 255.0f;
}

static inline uint32_t to_u8(float v)
{
    v = r5xx_clampf(v, 0.0f, 1.0f);
    return (uint32_t)(v * 255.0f + 0.5f);
}

uint32_t r5xx_pack_argb8888(const r5xx_vec4 *c)
{
    return (to_u8(c->f[3]) << 24) | (to_u8(c->f[0]) << 16) |
           (to_u8(c->f[1]) << 8) | to_u8(c->f[2]);
}

float r5xx_half_to_float(uint16_t h)
{
    uint32_t sign = (h >> 15) & 1, exp = (h >> 10) & 0x1f, mant = h & 0x3ff;
    uint32_t bits;
    float f;
    if (exp == 0) {
        if (mant == 0) {
            bits = sign << 31;
        } else {
            /* subnormal */
            int e = -1;
            do {
                e++;
                mant <<= 1;
            } while (!(mant & 0x400));
            mant &= 0x3ff;
            bits = (sign << 31) | ((127 - 15 - e) << 23) | (mant << 13);
        }
    } else if (exp == 31) {
        bits = (sign << 31) | 0x7f800000 | (mant << 13);
    } else {
        bits = (sign << 31) | ((exp - 15 + 127) << 23) | (mant << 13);
    }
    memcpy(&f, &bits, 4);
    return f;
}

uint16_t r5xx_float_to_half(float f)
{
    uint32_t bits;
    memcpy(&bits, &f, 4);
    uint32_t sign = (bits >> 16) & 0x8000;
    int32_t exp = (int32_t)((bits >> 23) & 0xff) - 127 + 15;
    uint32_t mant = bits & 0x7fffff;
    if (((bits >> 23) & 0xff) == 0xff) {
        return sign | 0x7c00 | (mant ? 0x200 : 0);
    }
    if (exp >= 31) {
        return sign | 0x7c00;
    }
    if (exp <= 0) {
        if (exp < -10) {
            return sign;
        }
        mant |= 0x800000;
        uint32_t shift = 14 - exp;
        uint32_t hm = mant >> shift;
        uint32_t rem = mant & ((1u << shift) - 1);
        if (rem > (1u << (shift - 1)) ||
            (rem == (1u << (shift - 1)) && (hm & 1))) {
            hm++;
        }
        return sign | hm;
    }
    uint32_t hm = mant >> 13;
    uint32_t rem = mant & 0x1fff;
    uint16_t h = sign | (exp << 10) | hm;
    if (rem > 0x1000 || (rem == 0x1000 && (hm & 1))) {
        h++;
    }
    return h;
}
