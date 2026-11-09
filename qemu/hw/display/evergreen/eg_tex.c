/*
 * Evergreen fetch units: vertex fetch (VC) and texture sampling (TC),
 * plus the buffer / texture format decoder shared with the back end.
 *
 * Resource and sampler words: Mesa evergreend.h (SQ_TEX_RESOURCE_WORD*,
 * SQ_VTX_CONSTANT_WORD*, SQ_TEX_SAMPLER_WORD*); fetch instruction words:
 * eg_sq.h.  Format names list components from the most significant bits
 * down, so channel 0 is the last one named (COLOR_1_5_5_5 is Gallium's
 * B5G5R5A1, see r600_translate_colorformat).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "eg_gfx.h"
#include <math.h>

static inline float u2f(uint32_t u)
{
    float f;
    memcpy(&f, &u, 4);
    return f;
}

static inline uint32_t f2u(float f)
{
    uint32_t u;
    memcpy(&u, &f, 4);
    return u;
}

/* ------------------------------------------------------------------ */
/* Formats                                                             */
/* ------------------------------------------------------------------ */

enum { CH_UINT, CH_FLOAT, CH_UF11, CH_UF10 };

typedef struct FmtInfo {
    uint8_t bytes;
    uint8_t nch;
    uint8_t bits[4];                    /* channel 0 = least significant */
    uint8_t kind;                       /* CH_FLOAT: all channels float */
} FmtInfo;

static const FmtInfo fmt_info[64] = {
    [0x01] = { 1, 1, { 8 } },
    [0x02] = { 1, 2, { 4, 4 } },
    [0x03] = { 1, 3, { 2, 3, 3 } },
    [0x05] = { 2, 1, { 16 } },
    [0x06] = { 2, 1, { 16 }, CH_FLOAT },
    [0x07] = { 2, 2, { 8, 8 } },
    [0x08] = { 2, 3, { 5, 6, 5 } },
    [0x09] = { 2, 3, { 5, 5, 6 } },
    [0x0a] = { 2, 4, { 5, 5, 5, 1 } },
    [0x0b] = { 2, 4, { 4, 4, 4, 4 } },
    [0x0c] = { 2, 4, { 1, 5, 5, 5 } },
    [0x0d] = { 4, 1, { 32 } },
    [0x0e] = { 4, 1, { 32 }, CH_FLOAT },
    [0x0f] = { 4, 2, { 16, 16 } },
    [0x10] = { 4, 2, { 16, 16 }, CH_FLOAT },
    [0x11] = { 4, 2, { 24, 8 } },
    [0x12] = { 4, 2, { 24, 8 } },
    [0x13] = { 4, 2, { 8, 24 } },
    [0x14] = { 4, 2, { 8, 24 } },
    [0x15] = { 4, 3, { 11, 11, 10 } },
    [0x16] = { 4, 3, { 11, 11, 10 }, CH_UF11 },
    [0x17] = { 4, 3, { 10, 11, 11 } },
    [0x18] = { 4, 3, { 10, 11, 11 }, CH_UF11 },
    [0x19] = { 4, 4, { 10, 10, 10, 2 } },
    [0x1a] = { 4, 4, { 8, 8, 8, 8 } },
    [0x1b] = { 4, 4, { 2, 10, 10, 10 } },
    [0x1c] = { 8, 2, { 32, 32 } },
    [0x1d] = { 8, 2, { 32, 32 } },
    [0x1e] = { 8, 2, { 32, 32 }, CH_FLOAT },
    [0x1f] = { 8, 4, { 16, 16, 16, 16 } },
    [0x20] = { 8, 4, { 16, 16, 16, 16 }, CH_FLOAT },
    [0x22] = { 16, 4, { 32, 32, 32, 32 } },
    [0x23] = { 16, 4, { 32, 32, 32, 32 }, CH_FLOAT },
    [0x27] = { 4, 3, { 8, 8, 8 } },     /* GB_GR, BG_RG: a 2x1 block (eg_fmt_422) */
    [0x28] = { 4, 3, { 8, 8, 8 } },
    [0x2c] = { 3, 3, { 8, 8, 8 } },
    [0x2d] = { 6, 3, { 16, 16, 16 } },
    [0x2e] = { 6, 3, { 16, 16, 16 }, CH_FLOAT },
    [0x2f] = { 12, 3, { 32, 32, 32 } },
    [0x30] = { 12, 3, { 32, 32, 32 }, CH_FLOAT },
};

#define FMT_X24_8_32_FLOAT  0x1c
#define FMT_GB_GR           0x27
#define FMT_BG_RG           0x28
#define FMT_BC1             0x31
#define FMT_BC5             0x35

/*
 * 4:2:2 formats (GL_YCBCR_422_APPLE): one 32-bit element holds two pixels
 * that share R and B.  As in Mesa, an element is a 2x1 block: the tiling
 * and the pitch count blocks.
 */
static inline bool eg_fmt_422(uint32_t fmt)
{
    return fmt == FMT_GB_GR || fmt == FMT_BG_RG;
}

/* the two pixels of a 4:2:2 element, pixel x & 1 */
static void decode_422(uint32_t fmt, const uint8_t *p, int odd, EgVec4 *out)
{
    if (fmt == FMT_GB_GR) {                 /* bytes R G0 B G1 */
        out->f[0] = p[0] / 255.0f;
        out->f[1] = p[odd ? 3 : 1] / 255.0f;
        out->f[2] = p[2] / 255.0f;
    } else {                                /* bytes G0 R G1 B */
        out->f[0] = p[1] / 255.0f;
        out->f[1] = p[odd ? 2 : 0] / 255.0f;
        out->f[2] = p[3] / 255.0f;
    }
    out->f[3] = 1.0f;
}

uint32_t eg_format_bytes(uint32_t fmt)
{
    fmt &= 0x3f;
    if (fmt >= FMT_BC1 && fmt <= FMT_BC5) {
        return (fmt == FMT_BC1 || fmt == 0x34) ? 8 : 16;    /* per 4x4 block */
    }
    return fmt_info[fmt].bytes;
}

static float half_to_float(uint32_t h)
{
    uint32_t s = (h >> 15) & 1, e = (h >> 10) & 0x1f, m = h & 0x3ff;
    float v;

    if (e == 0) {
        v = ldexpf((float)m, -24);
    } else if (e == 31) {
        v = m ? NAN : INFINITY;
    } else {
        v = ldexpf((float)(m | 0x400), (int)e - 25);
    }
    return s ? -v : v;
}

/* unsigned small floats of 10_11_11: 5 bit exponent, 6 or 5 bit mantissa */
static float ufloat_to_float(uint32_t v, int mbits)
{
    uint32_t e = v >> mbits, m = v & ((1u << mbits) - 1);

    if (e == 0) {
        return ldexpf((float)m, -14 - mbits);
    }
    if (e == 31) {
        return m ? NAN : INFINITY;
    }
    return ldexpf((float)(m | (1u << mbits)), (int)e - 15 - mbits);
}

static inline uint64_t load_le(const uint8_t *p, int n)
{
    uint64_t v = 0;
    for (int i = 0; i < n; i++) {
        v |= (uint64_t)p[i] << (8 * i);
    }
    return v;
}

/*
 * One element: num_format 0 = NORM, 1 = INT, 2 = SCALED; comp_signed per
 * channel: 0 unsigned, 1 signed, 2 unsigned biased.  INT results are raw
 * integers in out->u.
 */
static float unorm8[256];

void eg_tex_init(void)
{
    for (int c = 0; c < 256; c++) {
        unorm8[c] = c / 255.0f;
    }
}

bool eg_format_decode(uint32_t fmt, const uint8_t *p, EgVec4 *out, uint32_t num_format,
                      const uint32_t comp_signed[4], bool srf_no_zero)
{
    const FmtInfo *fi;
    int c, shift = 0;

    fmt &= 0x3f;
    /* fast path: unsigned normalized 8-bit channels (most textures) */
    if (num_format == 0 && (fmt == 0x1a || fmt == 0x07 || fmt == 0x01) &&
        !(comp_signed[0] | comp_signed[1] | comp_signed[2] | comp_signed[3])) {
        out->f[0] = unorm8[p[0]];
        out->f[1] = fmt == 0x01 ? 0.0f : unorm8[p[1]];
        out->f[2] = fmt == 0x1a ? unorm8[p[2]] : 0.0f;
        out->f[3] = fmt == 0x1a ? unorm8[p[3]] : 1.0f;
        return true;
    }
    out->f[0] = out->f[1] = out->f[2] = 0.0f;
    out->f[3] = 1.0f;
    if (num_format == 1) {
        out->u[3] = 1;
    }
    if (fmt == FMT_X24_8_32_FLOAT) {
        out->u[0] = (uint32_t)load_le(p, 4);
        out->f[1] = (float)(p[4]);
        return true;
    }
    fi = &fmt_info[fmt];
    if (!fi->bytes) {
        return false;
    }
    for (c = 0; c < fi->nch; c++) {
        int w = fi->bits[c];
        const uint8_t *q = p + shift / 8;
        uint64_t raw;

        if (w == 32) {
            raw = load_le(q, 4);
        } else if (fi->bytes > 8) {
            raw = load_le(q, 2) & ((1u << w) - 1);
        } else {
            raw = (load_le(p, fi->bytes) >> shift) & ((1ull << w) - 1);
        }
        shift += w;
        if (fi->kind == CH_FLOAT) {
            out->f[c] = w == 16 ? half_to_float((uint32_t)raw) : u2f((uint32_t)raw);
            continue;
        }
        if (fi->kind == CH_UF11) {
            out->f[c] = ufloat_to_float((uint32_t)raw, w == 10 ? 5 : 6);
            continue;
        }
        {
            uint32_t sg = comp_signed[c];
            uint64_t maxu = (w == 64) ? UINT64_MAX : ((1ull << w) - 1);
            int64_t sv = (int64_t)(raw << (64 - w)) >> (64 - w);
            switch (num_format) {
            case 1:                                 /* INT */
                out->u[c] = sg == 1 ? (uint32_t)(int32_t)sv : (uint32_t)raw;
                break;
            case 2:                                 /* SCALED */
                out->f[c] = sg == 1 ? (float)sv : (float)raw;
                break;
            default:                                /* NORM */
                if (sg == 1) {
                    double d = (double)sv / (double)((1ull << (w - 1)) - 1);
                    out->f[c] = (float)(d < -1.0 ? -1.0 : d);
                    if (!srf_no_zero && out->f[c] == -0.0f) {
                        out->f[c] = 0.0f;
                    }
                } else if (sg == 2) {
                    out->f[c] = (float)((double)raw / (double)maxu * 2.0 - 1.0);
                } else {
                    out->f[c] = (float)((double)raw / (double)maxu);
                }
                break;
            }
        }
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* Vertex fetch                                                        */
/* ------------------------------------------------------------------ */

static int stage_resource_base(const EgShader *sh)
{
    if (sh->in_fs) {
        return 992;
    }
    return eg_stage_res_base(sh->stage);
}

static void apply_dst(EgVec4 *dst, const EgVec4 *v, const uint32_t sel[4], bool int_fmt)
{
    for (int c = 0; c < 4; c++) {
        switch (sel[c]) {
        case 0: case 1: case 2: case 3:
            dst->u[c] = v->u[sel[c]];
            break;
        case 4:
            dst->u[c] = 0;
            break;
        case 5:
            dst->u[c] = int_fmt ? 1 : f2u(1.0f);
            break;
        default:
            break;                              /* 7: masked */
        }
    }
}

void eg_vtx_fetch(EgShader *sh, const uint32_t *ins, uint32_t lanes)
{
    EvergreenState *s = sh->s;
    uint32_t w0 = ins[0], w1 = ins[1], w2 = ins[2];
    uint32_t buf = (w0 >> 8) & 0xff, src = (w0 >> 16) & 0x7f, ssel = (w0 >> 24) & 3;
    uint32_t dst = w1 & 0x7f, op = w0 & 0x1f;
    uint32_t sel[4] = { (w1 >> 9) & 7, (w1 >> 12) & 7, (w1 >> 15) & 7, (w1 >> 18) & 7 };
    uint32_t fmt = (w1 >> 22) & 0x3f, numf = (w1 >> 28) & 3, comp = (w1 >> 30) & 1;
    bool srf = w1 >> 31, ucf = (w1 >> 21) & 1;
    uint32_t offset = w2 & 0xffff;
    int ridx = stage_resource_base(sh) + (int)buf;
    const uint32_t *r = s->gfx->resource[ridx & (EG_NUM_RESOURCES - 1)];
    bool valid = (r[7] >> 30) == 3;
    uint64_t base = r[0] | ((uint64_t)(r[2] & 0xff) << 32);
    uint32_t size = r[1] + 1, stride = (r[2] >> 8) & 0x7ff;
    int lane;

    if (op != 0) {
        eg_gfx_warn(s, 0xd0 + op, "evergreen: VTX op %u\n", op);
    }
    if (ucf && valid) {
        fmt = (r[2] >> 20) & 0x3f;
        numf = (r[2] >> 26) & 3;
        comp = (r[2] >> 28) & 1;
        srf = (r[2] >> 29) & 1;
        sel[0] = (r[3] >> 3) & 7;
        sel[1] = (r[3] >> 6) & 7;
        sel[2] = (r[3] >> 9) & 7;
        sel[3] = (r[3] >> 12) & 7;
    }
    if ((w2 >> 18) & 1) {
        stride = 16;                            /* CONST_BUF_NO_STRIDE */
    }
    for (lane = 0; lane < sh->nlanes; lane++) {
        uint32_t idx = sh->gpr[lane][src].u[ssel];
        uint8_t data[16] = { 0 };
        uint32_t cs[4] = { comp, comp, comp, comp };
        EgVec4 v;

        if (!((lanes >> lane) & 1)) {
            continue;
        }
        if (!valid && buf >= 128) {
            /*
             * Apple's driver reads its SET_ALU_CONST data through buffer
             * 128+ and leaves the resource for the CP microcode to fill:
             * serve the constant file of the stage.
             */
            uint32_t c = idx + offset / 16 + (sh->stage == EG_STAGE_PS ? 0 : 256);
            memcpy(&v, &s->gfx->alu_const[(c & (EG_NUM_ALU_CONSTS - 1)) * 4], 16);
            apply_dst(&sh->gpr[lane][dst], &v, sel, false);
            continue;
        }
        if (valid) {
            uint64_t off = (uint64_t)idx * stride + offset;
            uint32_t n = eg_format_bytes(fmt);
            if (n && off + n <= size) {
                eg_gpu_read(s, base + off, data, n);
            }
        }
        if (!eg_format_decode(fmt, data, &v, numf, cs, srf)) {
            eg_gfx_warn(s, 0x100 + fmt, "evergreen: vertex format %u\n", fmt);
        }
        apply_dst(&sh->gpr[lane][dst], &v, sel, numf == 1);
    }
}

/* ------------------------------------------------------------------ */
/* Textures                                                            */
/* ------------------------------------------------------------------ */

typedef struct TexDesc {
    EvergreenState *s;
    uint32_t dim, fmt, numf, comp[4], dsel[4];
    bool srf, degamma, int_fmt;
    uint32_t width, height, depth, pitch;
    uint32_t base_level, last_level, base_array, last_array;
    uint64_t base, mip;
    EgTiling tile;                          /* level 0 tiling */
    uint32_t array_mode;
    uint32_t r[8];
    /* levels, computed on first use */
    uint16_t lv_valid;
    uint64_t lv_addr[16];
    EgTiling lv_tile[16];
} TexDesc;

/* descriptors of the resources a shader context sampled recently */
#define TEX_CACHE 8
typedef struct TexCache {
    bool valid;
    TexDesc t;
} TexCache;

static void tex_desc(EvergreenState *s, const uint32_t *r, TexDesc *t)
{
    memset(t, 0, sizeof(*t));
    memcpy(t->r, r, 32);
    t->s = s;
    t->dim = r[0] & 7;
    t->pitch = (((r[0] >> 6) & 0xfff) + 1) * 8;
    t->width = ((r[0] >> 18) & 0x3fff) + 1;
    t->height = (r[1] & 0x3fff) + 1;
    t->depth = ((r[1] >> 14) & 0x1fff) + 1;
    t->array_mode = r[1] >> 28;
    t->base = (uint64_t)r[2] << 8;
    t->mip = (uint64_t)r[3] << 8;
    for (int c = 0; c < 4; c++) {
        t->comp[c] = (r[4] >> (2 * c)) & 3;
        t->dsel[c] = (r[4] >> (16 + 3 * c)) & 7;
    }
    t->numf = (r[4] >> 8) & 3;
    t->srf = (r[4] >> 10) & 1;
    t->degamma = (r[4] >> 11) & 1;
    t->base_level = r[4] >> 28;
    t->last_level = r[5] & 0xf;
    t->base_array = (r[5] >> 4) & 0x1fff;
    t->last_array = (r[5] >> 17) & 0x1fff;
    t->fmt = r[7] & 0x3f;
    t->int_fmt = t->numf == 1;
    eg_tiling_init(s, &t->tile, t->array_mode, MAX(eg_format_bytes(t->fmt), 1),
                   eg_fmt_422(t->fmt) ? DIV_ROUND_UP(t->pitch, 2) : t->pitch,
                   t->height, r[7] >> 16, r[7] >> 8, r[7] >> 10, r[7] >> 6, r[6] >> 29,
                   !((r[0] >> 5) & 1));
}

/* address of level `lv`: level 0 at BASE, the rest packed from MIP_ADDRESS */
static uint64_t tex_level_compute(const TexDesc *t, uint32_t lv, EgTiling *out)
{
    uint64_t addr = t->base;
    uint32_t l;

    *out = t->tile;
    if (!lv) {
        return addr;
    }
    addr = t->mip ? t->mip : t->base;
    for (l = 1; l <= lv; l++) {
        /*
         * Levels after the base have power-of-two sizes: the next power of
         * two of the minified size, as the driver lays out (and renders)
         * the mip chain of a non-power-of-two texture.  A 2560 x 1440
         * texture has level 1 at 2048 x 1024, not 1280 x 720.
         */
        uint32_t w = pow2ceil(MAX(t->width >> l, 1u)), h = pow2ceil(MAX(t->height >> l, 1u));
        uint32_t pw = MAX(t->pitch >> l, 1u);
        EgTiling tl = t->tile;
        bool comp = t->fmt >= FMT_BC1 && t->fmt <= FMT_BC5;

        if (comp) {
            w = DIV_ROUND_UP(w, 4);
            h = DIV_ROUND_UP(h, 4);
            pw = DIV_ROUND_UP(pw, 4);
        } else if (eg_fmt_422(t->fmt)) {
            w = DIV_ROUND_UP(w, 2);
            pw = DIV_ROUND_UP(pw, 2);
        }
        tl.pitch = ROUND_UP(MAX(pw, w), 8);
        tl.height = h;
        if (tl.array_mode == EG_ARRAY_2D_TILED_THIN1 &&
            (tl.pitch < 8u * tl.bank_width * tl.pipes * tl.macro_aspect ||
             h < 8u * tl.bank_height * tl.banks / tl.macro_aspect)) {
            tl.array_mode = EG_ARRAY_1D_TILED_THIN1;    /* small levels degrade */
        }
        eg_tiling_update(&tl);
        if (l == lv) {
            *out = tl;
            return addr;
        }
        addr += ROUND_UP(eg_tile_size(&tl) * (t->dim == 3 ? 6 : 1), 256);
    }
    return addr;
}

/* the address and tiling of level lv, cached in the descriptor */
static const EgTiling *tex_level(TexDesc *t, uint32_t lv, uint64_t *addr)
{
    lv &= 15;
    if (!((t->lv_valid >> lv) & 1)) {
        t->lv_addr[lv] = tex_level_compute(t, lv, &t->lv_tile[lv]);
        t->lv_valid |= 1u << lv;
    }
    *addr = t->lv_addr[lv];
    return &t->lv_tile[lv];
}

static float srgb_to_linear(float c)
{
    return c <= 0.04045f ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f);
}

static void decode_bc(const TexDesc *t, const uint8_t *blk, int px, int py, EgVec4 *out)
{
    int i = py * 4 + px;
    uint32_t fmt = t->fmt;

    out->f[0] = out->f[1] = out->f[2] = 0;
    out->f[3] = 1;
    if (fmt == 0x34 || fmt == 0x35) {
        /* BC4 / BC5: one or two 3-bit interpolated channels */
        for (int ch = 0; ch < (fmt == 0x35 ? 2 : 1); ch++) {
            const uint8_t *b = blk + ch * 8;
            float a0 = b[0] / 255.0f, a1 = b[1] / 255.0f, v;
            uint64_t bits = load_le(b + 2, 6);
            int code = (bits >> (3 * i)) & 7;
            if (code == 0) {
                v = a0;
            } else if (code == 1) {
                v = a1;
            } else if (b[0] > b[1]) {
                v = ((8 - code) * a0 + (code - 1) * a1) / 7;
            } else {
                v = code == 6 ? 0 : code == 7 ? 1 : ((6 - code) * a0 + (code - 1) * a1) / 5;
            }
            out->f[ch] = v;
        }
        return;
    }
    {
        const uint8_t *cb = fmt == FMT_BC1 ? blk : blk + 8;
        uint32_t c0 = cb[0] | cb[1] << 8, c1 = cb[2] | cb[3] << 8;
        uint32_t idx = (cb[4 + (i >> 2)] >> ((i & 3) * 2)) & 3;
        float r0 = ((c0 >> 11) & 31) / 31.0f, g0 = ((c0 >> 5) & 63) / 63.0f, b0 = (c0 & 31) / 31.0f;
        float r1 = ((c1 >> 11) & 31) / 31.0f, g1 = ((c1 >> 5) & 63) / 63.0f, b1 = (c1 & 31) / 31.0f;
        float k0, k1;
        bool four = c0 > c1 || fmt != FMT_BC1;

        switch (idx) {
        case 0: k0 = 1; k1 = 0; break;
        case 1: k0 = 0; k1 = 1; break;
        case 2: k0 = four ? 2 / 3.0f : 0.5f; k1 = 1 - k0; break;
        default:
            if (four) {
                k0 = 1 / 3.0f; k1 = 2 / 3.0f;
            } else {
                k0 = k1 = 0;
                out->f[3] = 0;
            }
            break;
        }
        /* channel 0 = red, as Mesa programs DST_SEL for BCn */
        out->f[0] = r0 * k0 + r1 * k1;
        out->f[1] = g0 * k0 + g1 * k1;
        out->f[2] = b0 * k0 + b1 * k1;
        if (fmt == 0x32) {
            out->f[3] = ((blk[i >> 1] >> ((i & 1) * 4)) & 15) / 15.0f;
        } else if (fmt == 0x33) {
            float a0 = blk[0] / 255.0f, a1 = blk[1] / 255.0f;
            uint64_t bits = load_le(blk + 2, 6);
            int code = (bits >> (3 * i)) & 7;
            if (code == 0) {
                out->f[3] = a0;
            } else if (code == 1) {
                out->f[3] = a1;
            } else if (blk[0] > blk[1]) {
                out->f[3] = ((8 - code) * a0 + (code - 1) * a1) / 7;
            } else {
                out->f[3] = code == 6 ? 0 : code == 7 ? 1 : ((6 - code) * a0 + (code - 1) * a1) / 5;
            }
        }
    }
}

/* one texel of a level, raw channels (before DST_SEL) */
static void texel(const TexDesc *t, uint64_t laddr, const EgTiling *tl, int x, int y,
                  int slice, EgVec4 *out)
{
    uint8_t buf[16];
    uint32_t bpe = MAX(eg_format_bytes(t->fmt), 1);
    bool comp = t->fmt >= FMT_BC1 && t->fmt <= FMT_BC5;
    int bx = comp ? x / 4 : eg_fmt_422(t->fmt) ? x / 2 : x, by = comp ? y / 4 : y;
    uint64_t off = eg_tile_offset(tl, bx, by) + (slice ? (uint64_t)slice * eg_tile_size(tl) : 0);
    const uint8_t *src = eg_vram_ptr(t->s, laddr + off, bpe);

    if (!src) {
        if (!eg_gpu_read(t->s, laddr + off, buf, bpe)) {
            memset(buf, 0, sizeof(buf));
        }
        src = buf;
    }
    if (comp) {
        decode_bc(t, src, x & 3, y & 3, out);
    } else if (eg_fmt_422(t->fmt)) {
        decode_422(t->fmt, src, x & 1, out);
    } else if (!eg_format_decode(t->fmt, src, out, t->numf, t->comp, t->srf)) {
        eg_gfx_warn(t->s, 0x140 + t->fmt, "evergreen: texture format %u\n", t->fmt);
    }
    if (t->degamma && !t->int_fmt) {
        for (int c = 0; c < 3; c++) {
            out->f[c] = srgb_to_linear(out->f[c]);
        }
    }
}

static int wrap(int i, int n, uint32_t mode, bool *border)
{
    *border = false;
    switch (mode) {
    case 0:                                         /* WRAP */
        i %= n;
        return i < 0 ? i + n : i;
    case 1: {                                       /* MIRROR */
        int p = 2 * n, m = i % p;
        if (m < 0) {
            m += p;
        }
        return m < n ? m : p - 1 - m;
    }
    case 3: case 5: case 7:                         /* MIRROR_ONCE */
        if (i < 0) {
            i = -1 - i;
        }
        if (mode == 7 && i >= n) {
            *border = true;
        }
        return MIN(i, n - 1);
    case 6: case 4:                                 /* CLAMP_(HALF_)BORDER */
        if (i < 0 || i >= n) {
            *border = true;
        }
        return MAX(0, MIN(i, n - 1));
    default:                                        /* CLAMP_LAST_TEXEL */
        return MAX(0, MIN(i, n - 1));
    }
}

static void border_color(uint32_t type, EgVec4 *v)
{
    v->f[0] = v->f[1] = v->f[2] = (type == 2) ? 1.0f : 0.0f;
    v->f[3] = (type == 0) ? 0.0f : 1.0f;
}

/* sample one level with point or bilinear filtering; u, v in texels */
static void sample_level(TexDesc *t, const uint32_t *smp, uint32_t lv, float u, float v,
                         int slice, bool linear, EgVec4 *out)
{
    uint64_t la;
    const EgTiling *tlp = tex_level(t, lv, &la);
    int w = MAX(t->width >> lv, 1u), h = MAX(t->height >> lv, 1u);
    uint32_t cx = smp[0] & 7, cy = (smp[0] >> 3) & 7, bt = (smp[0] >> 20) & 3;
    bool b;

    if (t->dim == 0 || t->dim == 4) {
        h = 1;
    }
    if (!linear) {
        int x = wrap((int)floorf(u), w, cx, &b), y = wrap((int)floorf(v), h, cy, &b);
        bool bx, by;
        wrap((int)floorf(u), w, cx, &bx);
        wrap((int)floorf(v), h, cy, &by);
        if (bx || by) {
            border_color(bt, out);
        } else {
            texel(t, la, tlp, x, y, slice, out);
        }
        return;
    }
    {
        float fu = u - 0.5f, fv = v - 0.5f;
        int x0 = (int)floorf(fu), y0 = (int)floorf(fv);
        float ax = fu - x0, ay = fv - y0;
        EgVec4 s[4];
        int k, c;
        for (k = 0; k < 4; k++) {
            bool bx, by;
            int x = wrap(x0 + (k & 1), w, cx, &bx), y = wrap(y0 + (k >> 1), h, cy, &by);
            if (bx || by) {
                border_color(bt, &s[k]);
            } else {
                texel(t, la, tlp, x, y, slice, &s[k]);
            }
        }
        if (t->int_fmt) {
            *out = s[0];
            return;
        }
        for (c = 0; c < 4; c++) {
            float top = s[0].f[c] + (s[1].f[c] - s[0].f[c]) * ax;
            float bot = s[2].f[c] + (s[3].f[c] - s[2].f[c]) * ax;
            out->f[c] = top + (bot - top) * ay;
        }
    }
}

enum {
    TEX_LD = 0x03, TEX_GET_RESINFO = 0x04, TEX_GET_NSAMPLES = 0x05, TEX_GET_LOD = 0x06,
    TEX_GET_GRAD_H = 0x07, TEX_GET_GRAD_V = 0x08, TEX_SET_OFFSETS = 0x09,
    TEX_KEEP_GRAD = 0x0a, TEX_SET_GRAD_H = 0x0b, TEX_SET_GRAD_V = 0x0c,
    TEX_SAMPLE = 0x10, TEX_SAMPLE_L = 0x11, TEX_SAMPLE_LB = 0x12, TEX_SAMPLE_LZ = 0x13,
    TEX_SAMPLE_G = 0x14, TEX_GATHER4 = 0x15, TEX_SAMPLE_G_LB = 0x16,
    TEX_SAMPLE_C = 0x18, TEX_SAMPLE_C_L = 0x19, TEX_SAMPLE_C_LB = 0x1a,
    TEX_SAMPLE_C_LZ = 0x1b, TEX_SAMPLE_C_G = 0x1c, TEX_SAMPLE_C_G_LB = 0x1e,
};

static float compare(uint32_t func, float ref, float v)
{
    switch (func) {
    case 0: return 0;
    case 1: return ref < v;
    case 2: return ref == v;
    case 3: return ref <= v;
    case 4: return ref > v;
    case 5: return ref != v;
    case 6: return ref >= v;
    default: return 1;
    }
}

void eg_tex_fetch(EgShader *sh, const uint32_t *ins, uint32_t lanes)
{
    EvergreenState *s = sh->s;
    uint32_t w0 = ins[0], w1 = ins[1], w2 = ins[2];
    uint32_t op = w0 & 0x1f, rid = (w0 >> 8) & 0xff, src = (w0 >> 16) & 0x7f;
    uint32_t dst = w1 & 0x7f, sid = (w2 >> 15) & 0x1f;
    uint32_t dsel[4] = { (w1 >> 9) & 7, (w1 >> 12) & 7, (w1 >> 15) & 7, (w1 >> 18) & 7 };
    uint32_t ssel[4] = { (w2 >> 20) & 7, (w2 >> 23) & 7, (w2 >> 26) & 7, (w2 >> 29) & 7 };
    bool norm[4] = { (w1 >> 28) & 1, (w1 >> 29) & 1, (w1 >> 30) & 1, (w1 >> 31) & 1 };
    int8_t ioff[3];
    float ibias = (float)(int8_t)(((w1 >> 21) & 0x7f) << 1) / 32.0f;
    int rbase = eg_stage_res_base(sh->stage), sbase = eg_stage_smp_base(sh->stage);
    const uint32_t *r = s->gfx->resource[(rbase + rid) & (EG_NUM_RESOURCES - 1)];
    const uint32_t *smp = s->gfx->sampler[(sbase + sid) & (EG_NUM_SAMPLERS - 1)];
    EgVec4 coord[EG_MAX_LANES], res[EG_MAX_LANES];
    TexDesc *t;
    int lane, c;

    for (c = 0; c < 3; c++) {
        int v = (w2 >> (5 * c)) & 0x1f;
        ioff[c] = (int8_t)((v & 0x10) ? v - 32 : v);
    }
    /* coordinates of every live lane (quad neighbours give derivatives) */
    for (lane = 0; lane < sh->nlanes; lane++) {
        const EgVec4 *g = &sh->gpr[lane][src];
        for (c = 0; c < 4; c++) {
            coord[lane].u[c] = ssel[c] < 4 ? g->u[ssel[c]] :
                               ssel[c] == 5 ? f2u(1.0f) : 0;
        }
    }
    switch (op) {
    case TEX_SET_GRAD_H:
    case TEX_SET_GRAD_V:
        for (lane = 0; lane < sh->nlanes; lane++) {
            (op == TEX_SET_GRAD_H ? sh->grad_h : sh->grad_v)[lane] = coord[lane];
        }
        sh->has_grad = true;
        return;
    case TEX_KEEP_GRAD:
    case TEX_SET_OFFSETS:
        return;
    case TEX_GET_GRAD_H:
    case TEX_GET_GRAD_V:
        for (lane = 0; lane < sh->nlanes; lane++) {
            int q = lane & ~3, a = q + (op == TEX_GET_GRAD_H ? ((lane & 2) | 0) : (lane & 1));
            int b = op == TEX_GET_GRAD_H ? a + 1 : a + 2;
            for (c = 0; c < 4; c++) {
                res[lane].f[c] = (sh->quad && b < sh->nlanes) ?
                                 coord[b].f[c] - coord[a].f[c] : 0.0f;
            }
        }
        goto write;
    }

    {
        /* descriptors are cached per shader context, keyed by the words */
        TexCache *tc;
        if (!sh->tex_cache) {
            sh->tex_cache = g_new0(TexCache, TEX_CACHE);
        }
        tc = &((TexCache *)sh->tex_cache)[rid % TEX_CACHE];
        if (!tc->valid || memcmp(tc->t.r, r, 32)) {
            tex_desc(s, r, &tc->t);
            tc->valid = true;
        }
        t = &tc->t;
    }
    if ((r[7] >> 30) != 2) {
        for (lane = 0; lane < sh->nlanes; lane++) {
            res[lane].f[0] = res[lane].f[1] = res[lane].f[2] = res[lane].f[3] = 0;
        }
        goto write_sel;
    }
    if (op == TEX_GET_RESINFO) {
        for (lane = 0; lane < sh->nlanes; lane++) {
            int lv = coord[lane].i[0];
            res[lane].i[0] = MAX(t->width >> lv, 1u);
            res[lane].i[1] = (t->dim == 4) ? (int)(t->last_array + 1) : (int)MAX(t->height >> lv, 1u);
            res[lane].i[2] = t->dim == 2 ? (int)MAX(t->depth >> lv, 1u) :
                             (int)(t->last_array - t->base_array + 1);
            res[lane].i[3] = t->last_level - t->base_level + 1;
        }
        goto write;
    }

    for (lane = 0; lane < sh->nlanes; lane++) {
        float u = coord[lane].f[0], v = coord[lane].f[1], lod = 0;
        int slice = 0, lv0, lv1;
        uint32_t mag = (smp[0] >> 9) & 3, minf = (smp[0] >> 11) & 3, mipf = (smp[0] >> 15) & 3;
        float minlod = (smp[1] & 0xfff) / 256.0f, maxlod = ((smp[1] >> 12) & 0xfff) / 256.0f;
        float sbias = (float)((int32_t)(smp[2] << 18) >> 18) / 256.0f;
        bool linear;

        if (!((lanes >> lane) & 1) && !sh->quad) {
            continue;
        }
        if (op == TEX_LD) {
            int x = coord[lane].i[0] + ioff[0], y = coord[lane].i[1] + ioff[1];
            int lv = (int)t->base_level + coord[lane].i[3];
            uint64_t la;
            const EgTiling *tlp = tex_level(t, MAX(lv, 0), &la);
            slice = t->dim == 4 ? coord[lane].i[1] : t->dim >= 2 ? coord[lane].i[2] : 0;
            if (t->dim == 4) {
                y = 0;
            }
            x = MAX(0, MIN(x, (int)MAX(t->width >> lv, 1u) - 1));
            y = MAX(0, MIN(y, (int)MAX(t->height >> lv, 1u) - 1));
            texel(t, la, tlp, x, y, MAX(slice, 0), &res[lane]);
            continue;
        }
        if (t->dim == 3) {
            /* cube: x, y are face coordinates in [0, 1] after CUBE + scale/bias,
             * z is the face index */
            slice = (int)coord[lane].f[2];
        } else if (t->dim == 5 || t->dim == 4) {
            slice = (int)rintf(t->dim == 4 ? v : coord[lane].f[2]);
            if (t->dim == 4) {
                v = 0.5f;
                norm[1] = true;
            }
        } else if (t->dim == 2) {
            slice = (int)floorf(coord[lane].f[2] * (norm[2] ? t->depth : 1));
        }
        slice = MAX(0, MIN(slice, (int)(t->dim == 2 ? t->depth : t->last_array + 1) - 1));
        if (norm[0]) {
            u *= t->width;
        }
        if (norm[1]) {
            v *= t->height;
        }
        u += ioff[0] * 0.5f;
        v += ioff[1] * 0.5f;

        /* level of detail */
        if (op == TEX_SAMPLE_L || op == TEX_SAMPLE_C_L) {
            lod = coord[lane].f[3];
        } else if (op == TEX_SAMPLE_LZ || op == TEX_SAMPLE_C_LZ || op == TEX_GATHER4) {
            lod = 0;
        } else {
            float dux, dvx, duy, dvy;
            if ((op == TEX_SAMPLE_G || op == TEX_SAMPLE_C_G || op == TEX_SAMPLE_G_LB ||
                 op == TEX_SAMPLE_C_G_LB) && sh->has_grad) {
                dux = sh->grad_h[lane].f[0]; dvx = sh->grad_h[lane].f[1];
                duy = sh->grad_v[lane].f[0]; dvy = sh->grad_v[lane].f[1];
            } else if (sh->quad) {
                int q = lane & ~3;
                dux = coord[q + 1].f[0] - coord[q].f[0];
                dvx = coord[q + 1].f[1] - coord[q].f[1];
                duy = coord[q + 2].f[0] - coord[q].f[0];
                dvy = coord[q + 2].f[1] - coord[q].f[1];
            } else {
                dux = dvx = duy = dvy = 0;
            }
            if (norm[0]) {
                dux *= t->width;
                duy *= t->width;
            }
            if (norm[1]) {
                dvx *= t->height;
                dvy *= t->height;
            }
            {
                float rho = MAX(dux * dux + dvx * dvx, duy * duy + dvy * dvy);
                lod = rho > 0 ? 0.5f * log2f(rho) : -100.0f;
            }
            if (op == TEX_SAMPLE_LB || op == TEX_SAMPLE_C_LB) {
                lod += coord[lane].f[3];
            }
        }
        lod += sbias + ibias;
        lod = MAX(minlod, MIN(lod, maxlod));
        linear = lod > 0 ? minf != 0 : mag != 0;
        lv0 = (int)t->base_level;
        lv1 = -1;
        if (mipf && t->last_level > t->base_level && lod > 0) {
            float l = MIN(lod, (float)(t->last_level - t->base_level));
            if (mipf == 1) {
                lv0 += (int)floorf(l + 0.5f);
            } else {
                lv0 += (int)floorf(l);
                lv1 = MIN(lv0 + 1, (int)t->last_level);
                lod = l - floorf(l);
            }
        }
        sample_level(t, smp, lv0, u / (1 << (lv0 - t->base_level)),
                     v / (1 << (lv0 - t->base_level)), slice, linear, &res[lane]);
        if (lv1 >= 0 && lv1 != lv0) {
            EgVec4 b;
            sample_level(t, smp, lv1, u / (1 << (lv1 - t->base_level)),
                         v / (1 << (lv1 - t->base_level)), slice, linear, &b);
            for (c = 0; c < 4; c++) {
                res[lane].f[c] += (b.f[c] - res[lane].f[c]) * lod;
            }
        }
        if (op >= TEX_SAMPLE_C && op <= TEX_SAMPLE_C_G_LB) {
            float ref = coord[lane].f[3];
            res[lane].f[0] = compare((smp[0] >> 22) & 7, ref, res[lane].f[0]);
        }
    }

write_sel:
    /* the resource's swizzle, then the instruction's */
    for (lane = 0; lane < sh->nlanes; lane++) {
        EgVec4 v = res[lane];
        for (c = 0; c < 4; c++) {
            uint32_t d = t->dsel[c];
            res[lane].u[c] = d < 4 ? v.u[d] : d == 4 ? 0 : (t->int_fmt ? 1 : f2u(1.0f));
        }
        if ((r[7] >> 30) != 2) {
            res[lane] = v;
        }
    }
write:
    for (lane = 0; lane < sh->nlanes; lane++) {
        if ((lanes >> lane) & 1) {
            apply_dst(&sh->gpr[lane][dst], &res[lane], dsel, false);
        }
    }
}

/* forget the cached descriptors (a new draw: resources may have changed) */
void eg_tex_cache_reset(EgShader *sh)
{
    g_free(sh->tex_cache);
    sh->tex_cache = NULL;
}

/* ------------------------------------------------------------------ */
/* Descriptors for the Vulkan backend (eg_vk.h)                        */
/* ------------------------------------------------------------------ */

static void tile_to_vk(const EgTiling *t, EvkTile *o)
{
    memset(o, 0, sizeof(*o));
    o->mode = t->array_mode;
    o->bpp = t->bpp;
    o->pitch = t->pitch;
    o->height = t->height;
    o->pipes = t->pipes;
    o->pipe_interleave = t->pipe_interleave;
    o->pi_bits = t->pi_bits;
    o->pipe_bits = t->pipe_bits;
    o->bank_bits = t->bank_bits;
    o->banks = t->banks;
    o->bank_width = t->bank_width;
    o->bank_height = t->bank_height;
    o->split = t->split;
    o->tile_split = t->tile_split;
    o->micro_bytes = t->micro_bytes;
    o->tiles_per_row = t->tiles_per_row;
    o->mt_pitch_shift = t->mt_pitch_shift;
    o->mt_height_shift = t->mt_height_shift;
    o->mt_bytes = t->mt_bytes;
    o->mt_per_row = t->mt_per_row;
    o->slice_bytes = (uint32_t)t->slice_bytes;
    o->bank_x_shift = t->bank_x_shift;
    o->bank_y_shift = t->bank_y_shift;
    o->display = t->display;
}

void eg_tiling_to_vk(const EgTiling *t, EvkTile *o)
{
    tile_to_vk(t, o);
}

/*
 * The texture resource r (8 words) as an EvkTex, every level from 0 to
 * last_level.  'locate' turns a GPU range into a VRAM or GART offset; the
 * whole texture must be in one of the two.  *lo, *len: the bytes it spans
 * (the level 0 image and the mip chain may be apart: two ranges).
 */
bool eg_tex_vk_build(EvergreenState *s, const uint32_t *r, EvkTex *o,
                     bool (*locate)(EvergreenState *s, uint64_t addr, uint64_t len,
                                    uint32_t *off, bool *gart),
                     uint64_t rd_off[2], uint64_t rd_len[2], int *nrd)
{
    TexDesc t;
    uint32_t lv, last, nslices;
    uint64_t addr, end0 = 0, mip_lo = 0, mip_end = 0;
    bool gart = false, g;
    uint32_t off;

    memset(o, 0, sizeof(*o));
    *nrd = 0;
    if ((r[7] >> 30) != 2) {
        return true;                    /* not a texture: samples as zero */
    }
    tex_desc(s, r, &t);
    nslices = t.dim == 3 ? 6 : (t.dim == 2 || t.dim >= 4) ? t.depth : 1;
    last = MIN(t.last_level, 15u);
    o->flags = EVK_TF_VALID | (t.int_fmt ? EVK_TF_INT : 0) | (t.degamma ? EVK_TF_DEGAMMA : 0) |
               (t.srf ? EVK_TF_SRF : 0) |
               (t.fmt >= FMT_BC1 && t.fmt <= FMT_BC5 ? EVK_TF_BC : 0);
    o->fmt = t.fmt;
    o->numf = t.numf;
    for (int c = 0; c < 4; c++) {
        o->comp |= t.comp[c] << (2 * c);
        o->dsel |= t.dsel[c] << (3 * c);
    }
    o->dim = t.dim;
    o->width = t.width;
    o->height = t.height;
    o->depth = t.depth;
    o->base_level = t.base_level;
    o->last_level = t.last_level;
    o->base_array = t.base_array;
    o->last_array = t.last_array;
    o->bpe = MAX(eg_format_bytes(t.fmt), 1);
    tile_to_vk(&t.tile, &o->tile);
    for (lv = 0; lv <= last; lv++) {
        const EgTiling *tl = tex_level(&t, lv, &addr);
        uint64_t size = eg_tile_size(tl) * nslices;
        o->level_pm[lv] = tl->pitch | tl->array_mode << 28;
        if (lv == 0) {
            end0 = addr + size;
        } else {
            if (!mip_lo || addr < mip_lo) {
                mip_lo = addr;
            }
            mip_end = MAX(mip_end, addr + size);
        }
    }
    /* level 0 */
    if (!locate(s, t.base, end0 - t.base, &off, &gart)) {
        return false;
    }
    o->level_addr[0] = off;
    rd_off[0] = gart ? EVK_GART_SPACE + off : off;
    rd_len[0] = end0 - t.base;
    *nrd = 1;
    if (mip_lo) {
        uint32_t moff;
        if (!locate(s, mip_lo, mip_end - mip_lo, &moff, &g) || g != gart) {
            return false;
        }
        for (lv = 1; lv <= last; lv++) {
            tex_level(&t, lv, &addr);
            o->level_addr[lv] = moff + (uint32_t)(addr - mip_lo);
        }
        rd_off[1] = g ? EVK_GART_SPACE + moff : moff;
        rd_len[1] = mip_end - mip_lo;
        *nrd = 2;
    }
    if (gart) {
        o->flags |= EVK_TF_GART;
    }
    return true;
}
