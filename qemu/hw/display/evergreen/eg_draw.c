/*
 * Evergreen 3D: state packets, the VGT (indices, primitive assembly),
 * vertex processing, clipping and the viewport, the scan converter, pixel
 * shader dispatch over 2x2 quads and the DB / CB back end.
 *
 * Register layouts: Mesa src/gallium/drivers/r600/evergreend.h.  The CP's
 * state shadowing (CONTEXT_CONTROL and the LOAD_* packets 0x60-0x67) is
 * modelled after the R6xx PM4 description: LOAD_<class> latches the
 * shadow base of the class and loads registers from it; while shadowing
 * of a class is enabled, SET_<class> writes are copied to the shadow.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/thread.h"
#include "qemu/rcu.h"
#include "qemu/error-report.h"
#include "hw/core/boards.h"
#include "eg_gfx.h"
#include "trace.h"
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

void eg_gfx_warn(EvergreenState *s, int id, const char *fmt, ...)
{
    va_list ap;

    if (s->gfx->warned[id & 0xff]) {
        return;
    }
    s->gfx->warned[id & 0xff] = true;
    va_start(ap, fmt);
    {
        g_autofree char *m = g_strdup_vprintf(fmt, ap);
        qemu_log_mask(LOG_UNIMP, "%s", m);
    }
    va_end(ap);
}

void eg_gfx_init(EvergreenState *s)
{
    s->gfx = g_new0(EgGfx, 1);
    eg_tex_init();
}

/* the texture descriptor cache of vk_tex_build() */
#define VK_TEX_CACHE 512

typedef struct VkTexEntry {
    uint32_t key[10];
    bool valid;
    int nloc, nrd;
    uint64_t loc_addr[2], loc_len[2];
    uint32_t loc_off[2];
    bool loc_gart[2];
    uint64_t ro[2], rl[2];
    EvkTex tex;
} VkTexEntry;

void eg_gfx_reset(EvergreenState *s)
{
    EgGfx *g = s->gfx;
    /* the host side lives on across resets */
    EgVk *vk = g->vk;
    MemoryRegion *sysram = g->vk_sysram;
    GHashTable *ps_cache = g->ps_cache;
    QEMUTimer *idle = g->vk_idle_flush, *poll = g->vk_poll;
    QEMUBH *done_bh = g->vk_done_bh;
    GArray *fence_ops = g->fence_ops;
    uint32_t *gen_of = g->gart_gen_of, gen_pages = g->gart_gen_pages, gen = g->gart_gen;
    uint64_t *pte_of = g->gart_pte_of;
    void *vk_tex = g->vk_tex, *pgm_fast = g->pgm_fast;

    eg_gfx_flush(s);
    memset(g, 0, sizeof(*g));
    g->num_instances = 1;
    g->vk = vk;
    g->vk_sysram = sysram;
    g->ps_cache = ps_cache;
    g->vk_idle_flush = idle;
    g->vk_poll = poll;
    g->vk_done_bh = done_bh;
    g->fence_ops = fence_ops;
    g->gart_gen_of = gen_of;
    g->gart_pte_of = pte_of;
    g->vk_tex = vk_tex;         /* emptied below: VRAM moves on a reset */
    g->pgm_fast = pgm_fast;     /* points into ps_cache, which stays */
    g->gart_gen_pages = gen_pages;
    g->gart_gen = gen + 1;
    if (vk_tex) {
        memset(vk_tex, 0, sizeof(VkTexEntry) * VK_TEX_CACHE);
    }
}

/* ------------------------------------------------------------------ */
/* State packets                                                       */
/* ------------------------------------------------------------------ */

enum {
    SH_CONFIG, SH_CONTEXT, SH_ALU, SH_BOOL, SH_LOOP, SH_RESOURCE, SH_SAMPLER, SH_CTL
};

static uint32_t *shadow_slot(EvergreenState *s, int cls, uint32_t off)
{
    EgGfx *g = s->gfx;

    switch (cls) {
    case SH_ALU:
        return off < EG_NUM_ALU_CONSTS * 4 ? &g->alu_const[off] : NULL;
    case SH_BOOL:
        return off < EG_NUM_BOOL_CONSTS ? &g->bool_const[off] : NULL;
    case SH_LOOP:
        return off < EG_NUM_LOOP_CONSTS ? &g->loop_const[off] : NULL;
    case SH_RESOURCE:
        return off < EG_NUM_RESOURCES * 8 ? &g->resource[off / 8][off % 8] : NULL;
    case SH_SAMPLER:
        return off < EG_NUM_SAMPLERS * 3 ? &g->sampler[off / 3][off % 3] : NULL;
    case SH_CTL:
        return off < 64 ? &g->ctl_const[off] : NULL;
    }
    return NULL;
}

/* register-class base offsets (dwords) of the SET_* packets */
static const uint32_t class_base[8] = {
    0x8000 / 4, 0x28000 / 4, 0, 0, 0, 0, 0, 0,
};

static void class_write(EvergreenState *s, int cls, uint32_t off, uint32_t val)
{
    if (cls == SH_CONFIG || cls == SH_CONTEXT) {
        eg_reg_write(s, (class_base[cls] + off) * 4, val);
    } else {
        uint32_t *p = shadow_slot(s, cls, off);
        if (p) {
            *p = val;
        }
    }
}

/* SET_* side effect: copy to the class's shadow while shadowing is on */
void eg_gfx_shadow_write(EvergreenState *s, int cls, uint32_t off, const uint32_t *v,
                         uint32_t n)
{
    EgGfx *g = s->gfx;

    if (!g->shadow[cls] || !((g->shadow_ena >> cls) & 1) || !(g->shadow_ena >> 31)) {
        return;
    }
    {
        g_autofree uint32_t *le = g_new(uint32_t, n);
        for (uint32_t i = 0; i < n; i++) {
            le[i] = cpu_to_le32(v[i]);
        }
        eg_gpu_write(s, g->shadow[cls] + off * 4ull, le, n * 4);
    }
}

static void set_class(EvergreenState *s, int cls, const uint32_t *d, uint32_t n)
{
    uint32_t i;

    if (n < 1) {
        return;
    }
    for (i = 1; i < n; i++) {
        class_write(s, cls, d[0] + i - 1, d[i]);
    }
    eg_gfx_shadow_write(s, cls, d[0], d + 1, n - 1);
}

/* LOAD_<class>: base (2 dwords), then (offset, count) pairs */
static void load_class(EvergreenState *s, int cls, const uint32_t *d, uint32_t n)
{
    uint64_t base;
    uint32_t i, k;

    if (n < 2) {
        return;
    }
    base = ((uint64_t)(d[1] & 0xff) << 32) | (d[0] & ~3u);
    s->gfx->shadow[cls] = base;
    for (i = 2; i + 1 < n; i += 2) {
        uint32_t off = d[i] & 0xffff, cnt = d[i + 1] & 0x3fff;
        g_autofree uint32_t *buf = g_new(uint32_t, cnt ? cnt : 1);
        eg_gpu_read(s, base + off * 4ull, buf, cnt * 4);
        for (k = 0; k < cnt; k++) {
            class_write(s, cls, off + k, le32_to_cpu(buf[k]));
        }
    }
}

/* ------------------------------------------------------------------ */
/* Vertices                                                            */
/* ------------------------------------------------------------------ */

typedef struct EgVert {
    float clip[4];                      /* VS POS0 */
    float win[4];                       /* x, y, z window, w = 1/w_clip */
    float psize;
    EgVec4 param[EG_NUM_PARAMS];
} EgVert;

typedef struct DrawCtx {
    EvergreenState *s;
    EgProgram vs, fs, ps;
    bool have_fs;
    uint32_t prim;
    uint32_t instance;
    /* vertex cache: index -> vertex */
    GHashTable *cache;
    EgVert *verts;
    uint32_t nverts, cap;
    /* vertices to shade: index and slot (vs_flush) */
    uint32_t *pend_idx, *pend_slot;
    uint32_t npend, pend_cap;
    /* derived raster state */
    int sc_x0, sc_y0, sc_x1, sc_y1;     /* scissor, exclusive max */
    uint32_t cliprect_rule;
    int cr[4][4];
    float pix_center;
    bool cull_front, cull_back, front_ccw, provoking_last;
    /* PS input setup */
    int ninterp;
    int interp_src[32];                 /* VS param, or -1 for default */
    uint32_t interp_cntl[32];
    /* back end */
    uint64_t pixels;
    EgShader *vsh;
    GPtrArray *jobs;                    /* TriJob, waiting for flush_jobs() */
    uint64_t job_area;
    /* Vulkan: window-space triangles collected instead of rasterized */
    bool vkc;
    GArray *vk_verts;                   /* EvkVertex, 3 per triangle */
} DrawCtx;

/* run the VS on pending vertices [a, b), 16 at a time, with shader sh and
 * programs vs, fs (each thread has its own: they cache decoded groups) */
static void vs_shade(DrawCtx *dc, EgShader *sh, EgProgram *vs, EgProgram *fs,
                     uint32_t a, uint32_t b)
{
    uint32_t k;
    int lane, n;

    for (k = a; k < b; k += EG_MAX_LANES) {
        n = MIN(EG_MAX_LANES, (int)(b - k));
        sh->killed = sh->pred = sh->param_mask = 0;
        sh->loop_index = 0;
        memset(sh->ar, 0, sizeof(sh->ar));
        memset(sh->pos, 0, sizeof(sh->pos));
        sh->s = dc->s;
        sh->prog = vs;
        sh->fetch = dc->have_fs ? fs : NULL;
        sh->stage = EG_STAGE_VS;
        sh->nlanes = n;
        sh->live = (1u << n) - 1;
        for (lane = 0; lane < n; lane++) {
            sh->gpr[lane][0].u[0] = dc->pend_idx[k + lane];
            sh->gpr[lane][0].u[3] = dc->instance;
            sh->pos[lane][0].f[3] = 1.0f;
        }
        eg_shader_run(sh);
        for (lane = 0; lane < n; lane++) {
            EgVert *v = &dc->verts[dc->pend_slot[k + lane]];
            memcpy(v->clip, sh->pos[lane][0].f, 16);
            v->psize = sh->pos[lane][1].f[0];
            memcpy(v->param, sh->param[lane], sizeof(v->param));
        }
    }
}

static bool vs_parallel(DrawCtx *dc);

/* shade the pending vertices: on the raster workers when there are many */
static void vs_flush(DrawCtx *dc)
{
    if (!dc->npend) {
        return;
    }
    if (!vs_parallel(dc)) {
        vs_shade(dc, dc->vsh, &dc->vs, &dc->fs, 0, dc->npend);
    }
    dc->npend = 0;
}

/* vertex slot of an index; a new one waits in pend_* for vs_flush() */
static uint32_t vtx_get(DrawCtx *dc, uint32_t index)
{
    gpointer val;
    uint32_t slot;

    if (g_hash_table_lookup_extended(dc->cache, GUINT_TO_POINTER(index + 1), NULL, &val)) {
        return GPOINTER_TO_UINT(val);
    }
    if (dc->nverts == dc->cap) {
        dc->cap = dc->cap ? dc->cap * 2 : 64;
        dc->verts = g_renew(EgVert, dc->verts, dc->cap);
    }
    slot = dc->nverts++;
    g_hash_table_insert(dc->cache, GUINT_TO_POINTER(index + 1), GUINT_TO_POINTER(slot));
    if (dc->npend == dc->pend_cap) {
        dc->pend_cap = dc->pend_cap ? dc->pend_cap * 2 : 256;
        dc->pend_idx = g_renew(uint32_t, dc->pend_idx, dc->pend_cap);
        dc->pend_slot = g_renew(uint32_t, dc->pend_slot, dc->pend_cap);
    }
    dc->pend_idx[dc->npend] = index;
    dc->pend_slot[dc->npend++] = slot;
    return slot;
}

/* window coordinates of a clip-space vertex */
static void to_window(DrawCtx *dc, EgVert *v)
{
    EvergreenState *s = dc->s;
    uint32_t vte = CTX(s, 0x28818);
    float x = v->clip[0], y = v->clip[1], z = v->clip[2], w = v->clip[3];
    float rw = w != 0 ? 1.0f / w : 0.0f;       /* VTX_W0_FMT: 1/W to the rasterizer */

    if (!((vte >> 8) & 1)) {
        x *= rw;
        y *= rw;
    }
    if (!((vte >> 9) & 1)) {
        z *= rw;
    }
    if (vte & 1) {
        x *= u2f(CTX(s, 0x2843c));
    }
    if (vte & 2) {
        x += u2f(CTX(s, 0x28440));
    }
    if (vte & 4) {
        y *= u2f(CTX(s, 0x28444));
    }
    if (vte & 8) {
        y += u2f(CTX(s, 0x28448));
    }
    if (vte & 16) {
        z *= u2f(CTX(s, 0x2844c));
    }
    if (vte & 32) {
        z += u2f(CTX(s, 0x28450));
    }
    if ((CTX(s, 0x28814) >> 16) & 1) {          /* VTX_WINDOW_OFFSET_ENABLE */
        uint32_t wo = CTX(s, 0x28200);
        x += (int16_t)(wo & 0xffff);
        y += (int16_t)(wo >> 16);
    }
    v->win[0] = x;
    v->win[1] = y;
    v->win[2] = z;
    v->win[3] = rw;
}

/* ------------------------------------------------------------------ */
/* Back end: color buffers                                             */
/* ------------------------------------------------------------------ */

typedef struct CBuf {
    bool enabled;
    uint64_t base;
    uint32_t fmt, number, swap, endian, mask;
    bool blend_bypass, blend_clamp;
    uint32_t blend;
    EgTiling tile;
    uint32_t bpp;
    int ch_comp[4];                     /* memory channel -> component */
    int nch;
    uint32_t width, height;
    uint8_t *host;                      /* the surface, when it is all in VRAM */
} CBuf;

typedef struct FbState {
    CBuf cb[8];
    uint32_t rop, cb_mode;
    bool color_disable;
    float blend_const[4];
    /* depth / stencil */
    bool z_enable, z_write, s_enable, z_valid, s_valid;
    uint32_t zfunc, zfmt, db_ctl, srefmask[2];
    uint64_t z_read, z_write_base, s_read, s_write;
    EgTiling ztile, stile;
    bool depth_clear, stencil_clear;
    float depth_clear_val;
    uint32_t stencil_clear_val;
    /* alpha test */
    bool alpha_test;
    uint32_t alpha_func;
    float alpha_ref;
} FbState;

static const uint8_t cb_bits[64][4] = {
    [0x01] = { 8 }, [0x02] = { 4, 4 }, [0x03] = { 2, 3, 3 }, [0x05] = { 16 }, [0x06] = { 16 },
    [0x07] = { 8, 8 }, [0x08] = { 5, 6, 5 }, [0x09] = { 5, 5, 6 }, [0x0a] = { 5, 5, 5, 1 },
    [0x0b] = { 4, 4, 4, 4 }, [0x0c] = { 1, 5, 5, 5 }, [0x0d] = { 32 }, [0x0e] = { 32 },
    [0x0f] = { 16, 16 }, [0x10] = { 16, 16 }, [0x11] = { 24, 8 }, [0x13] = { 8, 24 },
    [0x15] = { 11, 11, 10 }, [0x16] = { 11, 11, 10 }, [0x17] = { 10, 11, 11 },
    [0x18] = { 10, 11, 11 }, [0x19] = { 10, 10, 10, 2 }, [0x1a] = { 8, 8, 8, 8 },
    [0x1b] = { 2, 10, 10, 10 }, [0x1d] = { 32, 32 }, [0x1e] = { 32, 32 },
    [0x1f] = { 16, 16, 16, 16 }, [0x20] = { 16, 16, 16, 16 }, [0x22] = { 32, 32, 32, 32 },
    [0x23] = { 32, 32, 32, 32 }, [0x30] = { 32, 32, 32 },
};

static bool cb_is_float(uint32_t fmt)
{
    switch (fmt) {
    case 0x06: case 0x0e: case 0x10: case 0x16: case 0x18: case 0x1e: case 0x20:
    case 0x23: case 0x30:
        return true;
    }
    return false;
}

static void cb_setup(EvergreenState *s, CBuf *c, int i, bool unmasked)
{
    uint32_t r = 0x28c60 + 0x3c * i;
    uint32_t info = CTX(s, r + 0x10), attr = CTX(s, r + 0x14);
    uint32_t pitch = ((CTX(s, r + 4) & 0x7ff) + 1) * 8;
    uint32_t slice = ((CTX(s, r + 8) & 0x3fffff) + 1) * 64;
    uint32_t mode, bits = 0, k;

    memset(c, 0, sizeof(*c));
    c->fmt = (info >> 2) & 0x3f;
    c->mask = unmasked ? 0xf : (CTX(s, 0x28238) >> (4 * i)) & (CTX(s, 0x2823c) >> (4 * i)) & 0xf;
    if (!c->fmt || !cb_bits[c->fmt][0] || !c->mask) {
        return;
    }
    c->enabled = true;
    c->base = ((uint64_t)CTX(s, r) << 8) +
              (uint64_t)(CTX(s, r + 0xc) & 0x7ff) * 0;      /* slice view: not yet */
    c->number = (info >> 12) & 7;
    c->swap = (info >> 15) & 3;
    c->endian = info & 3;
    c->blend_clamp = (info >> 19) & 1;
    c->blend_bypass = (info >> 20) & 1;
    c->blend = CTX(s, 0x28780 + 4 * i);
    for (k = 0; k < 4 && cb_bits[c->fmt][k]; k++) {
        bits += cb_bits[c->fmt][k];
    }
    c->nch = k;
    c->bpp = bits / 8;
    mode = (info >> 8) & 0xf;
    c->width = pitch;
    c->height = slice / pitch;
    eg_tiling_init(s, &c->tile, mode, c->bpp, pitch, c->height, attr >> 10, attr >> 13,
                   attr >> 16, attr >> 19, attr >> 5, !((attr >> 4) & 1));
    c->host = eg_vram_ptr(s, c->base, eg_tile_size(&c->tile));
    /* channel order (r600_translate_colorswap) */
    for (k = 0; k < 4; k++) {
        c->ch_comp[k] = -1;
    }
    switch (c->nch) {
    case 1:
        c->ch_comp[0] = c->swap == 0 ? 0 : c->swap == 1 ? 1 : c->swap == 2 ? 2 : 3;
        break;
    case 2:
        if (c->swap == 0) {
            c->ch_comp[0] = 0; c->ch_comp[1] = 1;
        } else if (c->swap == 2) {
            c->ch_comp[0] = 1; c->ch_comp[1] = 0;
        } else if (c->swap == 1) {
            c->ch_comp[0] = 0; c->ch_comp[1] = 3;
        } else {
            c->ch_comp[0] = 3; c->ch_comp[1] = 0;
        }
        break;
    case 3:
        if (c->swap == 2) {
            c->ch_comp[0] = 2; c->ch_comp[1] = 1; c->ch_comp[2] = 0;
        } else {
            c->ch_comp[0] = 0; c->ch_comp[1] = 1; c->ch_comp[2] = 2;
        }
        break;
    default:
        switch (c->swap) {
        case 0: c->ch_comp[0] = 0; c->ch_comp[1] = 1; c->ch_comp[2] = 2; c->ch_comp[3] = 3; break;
        case 1: c->ch_comp[0] = 2; c->ch_comp[1] = 1; c->ch_comp[2] = 0; c->ch_comp[3] = 3; break;
        case 2: c->ch_comp[0] = 3; c->ch_comp[1] = 2; c->ch_comp[2] = 1; c->ch_comp[3] = 0; break;
        default: c->ch_comp[0] = 3; c->ch_comp[1] = 0; c->ch_comp[2] = 1; c->ch_comp[3] = 2; break;
        }
        break;
    }
}

static float linear_to_srgb(float c)
{
    c = c < 0 ? 0 : c > 1 ? 1 : c;
    return c <= 0.0031308f ? c * 12.92f : 1.055f * powf(c, 1.0f / 2.4f) - 0.055f;
}

static float srgb_to_linear(float c)
{
    return c <= 0.04045f ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f);
}

static uint16_t f_to_half(float f)
{
    uint32_t u = f2u(f), s = (u >> 16) & 0x8000;
    int e = (int)((u >> 23) & 0xff) - 127 + 15;
    uint32_t m = u & 0x7fffff;

    if (isnan(f)) {
        return 0x7e00;
    }
    if (e >= 31) {
        return s | 0x7c00;
    }
    if (e <= 0) {
        if (e < -10) {
            return s;
        }
        m = (m | 0x800000) >> (1 - e);
        return s | ((m + 0x1000) >> 13);
    }
    return s | (e << 10) | ((m + 0x1000) >> 13);
}

static float h_to_f(uint32_t h)
{
    uint32_t s = (h >> 15) & 1, e = (h >> 10) & 0x1f, m = h & 0x3ff;
    float v = e == 0 ? ldexpf((float)m, -24) : e == 31 ? (m ? NAN : INFINITY) :
              ldexpf((float)(m | 0x400), (int)e - 25);
    return s ? -v : v;
}

/* pixel memory <-> 4 float (or int) components */
static void cb_unpack(const CBuf *c, const uint8_t *p, EgVec4 *out)
{
    uint64_t raw = 0;
    int k, shift = 0;

    out->f[0] = out->f[1] = out->f[2] = 0;
    out->f[3] = 1;
    if (c->bpp <= 8) {
        for (k = 0; k < (int)c->bpp; k++) {
            raw |= (uint64_t)p[k] << (8 * k);
        }
    }
    for (k = 0; k < c->nch; k++) {
        int w = cb_bits[c->fmt][k], comp = c->ch_comp[k];
        uint32_t v;
        float f;
        if (c->bpp > 8) {
            memcpy(&v, p + 4 * k, 4);
            v = le32_to_cpu(v);
        } else {
            v = (uint32_t)((raw >> shift) & ((1ull << w) - 1));
        }
        shift += w;
        if (comp < 0) {
            continue;
        }
        if (cb_is_float(c->fmt)) {
            f = w == 16 ? h_to_f(v) : w == 32 ? u2f(v) : 0.0f;
        } else {
            switch (c->number) {
            case 1: {                                   /* SNORM */
                int32_t sv = (int32_t)(v << (32 - w)) >> (32 - w);
                f = MAX(-1.0f, (float)sv / (float)((1u << (w - 1)) - 1));
                break;
            }
            case 4: case 2:
                out->u[comp] = v;
                continue;
            case 5: case 3:
                out->i[comp] = (int32_t)(v << (32 - w)) >> (32 - w);
                continue;
            case 6:
                f = (float)v / (float)((1ull << w) - 1);
                if (comp < 3) {
                    f = srgb_to_linear(f);
                }
                break;
            default:
                f = (float)v / (float)((1ull << w) - 1);
                break;
            }
        }
        out->f[comp] = f;
    }
}

static void cb_pack(const CBuf *c, const EgVec4 *in, uint8_t *p, uint32_t mask)
{
    uint64_t raw = 0;
    int k, shift = 0;

    if (c->bpp <= 8) {
        for (k = 0; k < (int)c->bpp; k++) {
            raw |= (uint64_t)p[k] << (8 * k);
        }
    }
    for (k = 0; k < c->nch; k++) {
        int w = cb_bits[c->fmt][k], comp = c->ch_comp[k];
        uint64_t m = (w == 64 ? ~0ull : (1ull << w) - 1);
        uint32_t v;
        float f;
        if (comp < 0 || !((mask >> comp) & 1)) {
            shift += w;
            continue;
        }
        f = in->f[comp];
        if (cb_is_float(c->fmt)) {
            v = w == 16 ? f_to_half(f) : f2u(f);
        } else {
            switch (c->number) {
            case 1: {
                float cl = f < -1 ? -1 : f > 1 ? 1 : f;
                v = (uint32_t)(int32_t)lrintf(cl * (float)((1u << (w - 1)) - 1)) & (uint32_t)m;
                break;
            }
            case 4: case 2:
                v = in->u[comp] > m ? (uint32_t)m : in->u[comp];
                break;
            case 5: case 3: {
                int32_t lo = -(1 << (w - 1)), hi = (1 << (w - 1)) - 1;
                int32_t iv = in->i[comp];
                v = (uint32_t)MAX(lo, MIN(hi, iv)) & (uint32_t)m;
                break;
            }
            case 6:
                if (comp < 3) {
                    f = linear_to_srgb(f);
                }
                /* fall through */
            default: {
                float cl = isnan(f) ? 0 : f < 0 ? 0 : f > 1 ? 1 : f;
                v = (uint32_t)lrint((double)cl * (double)m);
                break;
            }
            }
        }
        if (c->bpp > 8) {
            uint32_t le = cpu_to_le32(v);
            memcpy(p + 4 * k, &le, 4);
        } else {
            raw = (raw & ~(m << shift)) | ((uint64_t)v << shift);
        }
        shift += w;
    }
    if (c->bpp <= 8) {
        for (k = 0; k < (int)c->bpp; k++) {
            p[k] = raw >> (8 * k);
        }
    }
}

static float blend_factor(uint32_t f, const EgVec4 *src, const EgVec4 *dst, const float *k,
                          int comp)
{
    switch (f) {
    case 0: return 0;
    case 1: return 1;
    case 2: return src->f[comp];
    case 3: return 1 - src->f[comp];
    case 4: return src->f[3];
    case 5: return 1 - src->f[3];
    case 6: return dst->f[3];
    case 7: return 1 - dst->f[3];
    case 8: return dst->f[comp];
    case 9: return 1 - dst->f[comp];
    case 10: return comp == 3 ? 1 : MIN(src->f[3], 1 - dst->f[3]);
    case 13: return k[comp];
    case 14: return 1 - k[comp];
    case 19: return k[3];
    case 20: return 1 - k[3];
    default: return src->f[comp];
    }
}

static float blend_comb(uint32_t fn, float s, float d)
{
    switch (fn) {
    case 1: return s - d;
    case 2: return MIN(s, d);
    case 3: return MAX(s, d);
    case 4: return d - s;
    default: return s + d;
    }
}

static void cb_write(DrawCtx *dc, FbState *fb, int x, int y, const EgVec4 *col, int i)
{
    CBuf *c = &fb->cb[i];
    uint8_t px[16];
    uint64_t addr;
    EgVec4 src = *col, dst;
    uint32_t bl = c->blend;
    bool blend = (bl >> 30) & 1 && !c->blend_bypass && !cb_is_float(c->fmt) ? true :
                 ((bl >> 30) & 1) && !c->blend_bypass;

    if (x < 0 || y < 0 || (uint32_t)x >= c->width || (uint32_t)y >= c->height) {
        return;
    }
    addr = eg_tile_offset(&c->tile, x, y);
    if (c->number == 4 || c->number == 5 || c->number == 2 || c->number == 3) {
        blend = false;
    }
    if (c->host) {
        /* fast path: write in place */
        uint8_t *hp = c->host + addr;
        if (!blend && fb->rop == 0xcc && c->mask == 0xf) {
            cb_pack(c, &src, hp, 0xf);
            return;
        }
        memcpy(px, hp, c->bpp);
    } else {
        eg_gpu_read(dc->s, c->base + addr, px, c->bpp);
    }
    addr += c->base;
    if (blend || fb->rop != 0xcc) {
        cb_unpack(c, px, &dst);
    }
    if (blend) {
        int k;
        uint32_t csrc = bl & 0x1f, cfn = (bl >> 5) & 7, cdst = (bl >> 8) & 0x1f;
        uint32_t asrc = csrc, afn = cfn, adst = cdst;
        if ((bl >> 29) & 1) {
            asrc = (bl >> 16) & 0x1f;
            afn = (bl >> 21) & 7;
            adst = (bl >> 24) & 0x1f;
        }
        if (c->blend_clamp && !cb_is_float(c->fmt)) {
            for (k = 0; k < 4; k++) {
                src.f[k] = MAX(0.0f, MIN(1.0f, src.f[k]));
            }
        }
        for (k = 0; k < 4; k++) {
            uint32_t fs = k < 3 ? csrc : asrc, fd = k < 3 ? cdst : adst, fn = k < 3 ? cfn : afn;
            float sf = blend_factor(fs, &src, &dst, fb->blend_const, k);
            float df = blend_factor(fd, &src, &dst, fb->blend_const, k);
            if (fn == 2 || fn == 3) {
                sf = df = 1;
            }
            src.f[k] = blend_comb(fn, src.f[k] * sf, dst.f[k] * df);
        }
    } else if (fb->rop != 0xcc) {
        /* logic ops on the packed pixel */
        uint8_t sp[16];
        int b;
        memcpy(sp, px, sizeof(sp));
        cb_pack(c, &src, sp, 0xf);
        for (b = 0; b < (int)c->bpp; b++) {
            uint8_t sv = sp[b], dv = px[b], r = 0;
            for (int bit = 0; bit < 8; bit++) {
                int idx = (((sv >> bit) & 1) << 1) | ((dv >> bit) & 1);
                /* ROP3 with pattern = source: bits indexed by (S, D) */
                int code = ((fb->rop >> (idx == 3 ? 7 : idx == 2 ? 4 : idx == 1 ? 3 : 0)) & 1);
                r |= code << bit;
            }
            sp[b] = r;
        }
        /* honour the channel mask */
        if (c->mask == 0xf) {
            memcpy(px, sp, c->bpp);
        } else {
            EgVec4 v;
            cb_unpack(c, sp, &v);
            cb_pack(c, &v, px, c->mask);
        }
    }
    if (blend || fb->rop == 0xcc) {
        cb_pack(c, &src, px, c->mask);      /* the blended or plain color */
    }
    if (c->host) {
        memcpy(c->host + (addr - c->base), px, c->bpp);
    } else {
        eg_gpu_write(dc->s, addr, px, c->bpp);
    }
}

/* ------------------------------------------------------------------ */
/* Back end: depth / stencil                                           */
/* ------------------------------------------------------------------ */

static void fb_setup(DrawCtx *dc, FbState *fb)
{
    EvergreenState *s = dc->s;
    uint32_t zinfo = CTX(s, 0x28040), sinfo = CTX(s, 0x28044), dctl = CTX(s, 0x28800);
    uint32_t size = CTX(s, 0x28058);
    uint32_t pitch = ((size & 0x7ff) + 1) * 8, height = (((size >> 11) & 0x7ff) + 1) * 8;
    uint32_t rctl = CTX(s, 0x28000);
    uint32_t mode = CTX(s, 0x28808);
    int i;

    memset(fb, 0, sizeof(*fb));
    fb->rop = (mode >> 16) & 0xff;
    fb->cb_mode = (mode >> 4) & 7;
    /*
     * Only CB_NORMAL writes the shader's colors.  ELIMINATE_FAST_CLEAR and
     * the DECOMPRESS modes fix up compressed surfaces, which are never
     * compressed here (a zero-color pass Apple runs after every GL frame);
     * RESOLVE copies CB0 to CB1 (cb_resolve).
     */
    fb->color_disable = fb->cb_mode != 1 || ((rctl >> 12) & 1);
    if (fb->cb_mode == 3) {
        /* the resolve copies whole pixels, whatever the shader masks say */
        cb_setup(s, &fb->cb[0], 0, true);
        cb_setup(s, &fb->cb[1], 1, true);
        fb->cb[1].blend = 0;
    }
    for (i = 0; i < 4; i++) {
        fb->blend_const[i] = u2f(CTX(s, 0x28414 + 4 * i));
    }
    for (i = 0; i < 8 && !fb->color_disable; i++) {
        cb_setup(s, &fb->cb[i], i, false);
    }
    fb->zfmt = zinfo & 3;
    fb->z_valid = fb->zfmt != 0;
    fb->s_valid = sinfo & 1;
    fb->z_enable = fb->z_valid && ((dctl >> 1) & 1);
    fb->z_write = fb->z_enable && ((dctl >> 2) & 1);
    fb->s_enable = fb->s_valid && (dctl & 1);
    fb->zfunc = (dctl >> 4) & 7;
    fb->db_ctl = dctl;
    fb->srefmask[0] = CTX(s, 0x28430);
    fb->srefmask[1] = CTX(s, 0x28434);
    fb->z_read = (uint64_t)CTX(s, 0x28048) << 8;
    fb->z_write_base = (uint64_t)CTX(s, 0x28050) << 8;
    fb->s_read = (uint64_t)CTX(s, 0x2804c) << 8;
    fb->s_write = (uint64_t)CTX(s, 0x28054) << 8;
    eg_tiling_init(s, &fb->ztile, (zinfo >> 4) & 0xf, fb->zfmt == 1 ? 2 : 4, pitch, height,
                   zinfo >> 12, zinfo >> 16, zinfo >> 20, zinfo >> 24, zinfo >> 8, false);
    eg_tiling_init(s, &fb->stile, (zinfo >> 4) & 0xf, 1, pitch, height,
                   zinfo >> 12, zinfo >> 16, zinfo >> 20, zinfo >> 24, sinfo >> 8, false);
    fb->depth_clear = rctl & 1;
    fb->stencil_clear = (rctl >> 1) & 1;
    fb->depth_clear_val = u2f(CTX(s, 0x2802c));
    fb->stencil_clear_val = CTX(s, 0x28028) & 0xff;
    {
        uint32_t at = CTX(s, 0x28410);
        fb->alpha_test = (at >> 3) & 1 && !((at >> 8) & 1);
        fb->alpha_func = at & 7;
        fb->alpha_ref = u2f(CTX(s, 0x28438));
    }
}

static bool cmp_func(uint32_t f, float a, float b)
{
    switch (f) {
    case 0: return false;
    case 1: return a < b;
    case 2: return a == b;
    case 3: return a <= b;
    case 4: return a > b;
    case 5: return a != b;
    case 6: return a >= b;
    default: return true;
    }
}

static uint8_t stencil_op(uint32_t op, uint8_t v, uint8_t ref)
{
    switch (op) {
    case 1: return 0;
    case 2: return ref;
    case 3: return v == 255 ? 255 : v + 1;
    case 4: return v == 0 ? 0 : v - 1;
    case 5: return ~v;
    case 6: return v + 1;
    case 7: return v - 1;
    default: return v;
    }
}

/* depth and stencil test of one sample; returns whether it passes */
static bool db_test(DrawCtx *dc, FbState *fb, int x, int y, float z, bool front)
{
    EvergreenState *s = dc->s;
    bool pass = true;
    uint64_t zoff = 0, soff = 0;
    float zd = 0;
    uint8_t st = 0;
    uint32_t zraw = 0;

    if (!fb->z_valid && !fb->s_valid) {
        return true;
    }
    if (x < 0 || y < 0 || (uint32_t)x >= fb->ztile.pitch || (uint32_t)y >= fb->ztile.height) {
        return true;
    }
    z = z < 0 ? 0 : z > 1 ? 1 : z;
    if (fb->z_valid) {
        const uint8_t *zp;
        zoff = eg_tile_offset(&fb->ztile, x, y);
        zp = eg_vram_ptr(s, fb->z_read + zoff, 4);
        if (zp) {
            zraw = fb->zfmt == 1 ? lduw_le_p(zp) : ldl_le_p(zp);
        } else {
            eg_gpu_read(s, fb->z_read + zoff, &zraw, fb->zfmt == 1 ? 2 : 4);
            zraw = le32_to_cpu(zraw);
        }
        zd = fb->zfmt == 1 ? (zraw & 0xffff) / 65535.0f :
             fb->zfmt == 2 ? (zraw & 0xffffff) / 16777215.0f : u2f(zraw);
    }
    if (fb->s_valid) {
        soff = eg_tile_offset(&fb->stile, x, y);
        eg_gpu_read(s, fb->s_read + soff, &st, 1);
    }
    if (fb->depth_clear && fb->z_valid) {
        zd = fb->depth_clear_val;
    }
    if (fb->stencil_clear && fb->s_valid) {
        st = fb->stencil_clear_val;
    }
    if (fb->s_enable) {
        int side = (!front && ((fb->db_ctl >> 7) & 1)) ? 1 : 0;
        uint32_t rm = fb->srefmask[side];
        uint8_t ref = rm & 0xff, msk = (rm >> 8) & 0xff, wmsk = (rm >> 16) & 0xff;
        uint32_t func = (fb->db_ctl >> (side ? 20 : 8)) & 7;
        uint32_t ofail = (fb->db_ctl >> (side ? 23 : 11)) & 7;
        uint32_t ozpass = (fb->db_ctl >> (side ? 26 : 14)) & 7;
        uint32_t ozfail = (fb->db_ctl >> (side ? 29 : 17)) & 7;
        bool sp = cmp_func(func, (float)(ref & msk), (float)(st & msk));
        uint8_t nst;
        bool zp = !fb->z_enable || cmp_func(fb->zfunc, z, zd);
        nst = stencil_op(!sp ? ofail : zp ? ozpass : ozfail, st, ref);
        st = (st & ~wmsk) | (nst & wmsk);
        pass = sp && zp;
    } else if (fb->z_enable) {
        pass = cmp_func(fb->zfunc, z, zd);
    }
    if (pass && fb->z_write) {
        zd = z;
    }
    if (fb->z_valid && (fb->z_write || fb->depth_clear)) {
        uint32_t v = fb->zfmt == 1 ? (uint32_t)lrintf(zd * 65535.0f) :
                     fb->zfmt == 2 ? ((zraw & 0xff000000) | (uint32_t)lrintf(zd * 16777215.0f)) :
                     f2u(zd);
        {
            uint8_t *zp = eg_vram_ptr(s, fb->z_write_base + zoff, 4);
            if (zp && fb->zfmt == 1) {
                stw_le_p(zp, v);
            } else if (zp) {
                stl_le_p(zp, v);
            } else {
                v = cpu_to_le32(v);
                eg_gpu_write(s, fb->z_write_base + zoff, &v, fb->zfmt == 1 ? 2 : 4);
            }
        }
    }
    if (fb->s_valid && (fb->s_enable || fb->stencil_clear)) {
        eg_gpu_write(s, fb->s_write + soff, &st, 1);
    }
    return pass;
}

/* ------------------------------------------------------------------ */
/* Rasterization                                                       */
/* ------------------------------------------------------------------ */

static void raster_setup(DrawCtx *dc)
{
    EvergreenState *s = dc->s;
    uint32_t tl, br, wo = CTX(s, 0x28200);
    int ox = (int16_t)(wo & 0xffff), oy = (int16_t)(wo >> 16);
    uint32_t mode = CTX(s, 0x28814);
    int i;

    /* screen scissor */
    tl = CTX(s, 0x28030);
    br = CTX(s, 0x28034);
    dc->sc_x0 = tl & 0xffff;
    dc->sc_y0 = tl >> 16;
    dc->sc_x1 = br & 0xffff;
    dc->sc_y1 = br >> 16;
#define ISECT(tlr, brr, off)                                                   \
    do {                                                                       \
        uint32_t a = CTX(s, tlr), b = CTX(s, brr);                             \
        int dx = (off && !(a >> 31)) ? ox : 0, dy = (off && !(a >> 31)) ? oy : 0; \
        dc->sc_x0 = MAX(dc->sc_x0, (int)(a & 0x7fff) + dx);                    \
        dc->sc_y0 = MAX(dc->sc_y0, (int)((a >> 16) & 0x7fff) + dy);            \
        dc->sc_x1 = MIN(dc->sc_x1, (int)(b & 0x7fff) + dx);                    \
        dc->sc_y1 = MIN(dc->sc_y1, (int)((b >> 16) & 0x7fff) + dy);            \
    } while (0)
    ISECT(0x28204, 0x28208, 1);                 /* window scissor */
    ISECT(0x28240, 0x28244, 1);                 /* generic scissor */
    if ((CTX(s, 0x28a48) >> 1) & 1) {
        ISECT(0x28250, 0x28254, 1);             /* viewport 0 scissor */
    }
#undef ISECT
    dc->cliprect_rule = CTX(s, 0x2820c) & 0xffff;
    for (i = 0; i < 4; i++) {
        uint32_t a = CTX(s, 0x28210 + 8 * i), b = CTX(s, 0x28214 + 8 * i);
        dc->cr[i][0] = (a & 0x7fff) + ox;
        dc->cr[i][1] = ((a >> 16) & 0x7fff) + oy;
        dc->cr[i][2] = (b & 0x7fff) + ox;
        dc->cr[i][3] = ((b >> 16) & 0x7fff) + oy;
    }
    dc->pix_center = (CTX(s, 0x28c08) & 1) ? 0.5f : 0.0f;
    dc->cull_front = mode & 1;
    dc->cull_back = (mode >> 1) & 1;
    dc->front_ccw = !((mode >> 2) & 1);
    dc->provoking_last = (mode >> 19) & 1;
}

static bool cliprect_pass(DrawCtx *dc, int x, int y)
{
    int i, idx = 0;

    if (dc->cliprect_rule == 0xffff) {
        return true;
    }
    for (i = 0; i < 4; i++) {
        if (x >= dc->cr[i][0] && x < dc->cr[i][2] && y >= dc->cr[i][1] && y < dc->cr[i][3]) {
            idx |= 1 << i;
        }
    }
    return (dc->cliprect_rule >> idx) & 1;
}

/* PS input interpolation data for one triangle */
static void setup_interp(DrawCtx *dc, const EgVert *v0, const EgVert *v1, const EgVert *v2,
                         const EgVert *prov, EgInterp *ip)
{
    int n;

    for (n = 0; n < dc->ninterp; n++) {
        int src = dc->interp_src[n];
        uint32_t cntl = dc->interp_cntl[n];
        if (src < 0) {
            uint32_t dv = (cntl >> 8) & 3;
            float a = (dv & 2) ? 1.0f : 0.0f;
            ip[n].p0.f[0] = ip[n].p0.f[1] = ip[n].p0.f[2] = a;
            ip[n].p0.f[3] = (dv == 1 || dv == 3) ? 1.0f : 0.0f;
            memset(&ip[n].p10, 0, 16);
            memset(&ip[n].p20, 0, 16);
            continue;
        }
        if ((cntl >> 10) & 1) {                 /* FLAT_SHADE */
            ip[n].p0 = prov->param[src];
            memset(&ip[n].p10, 0, 16);
            memset(&ip[n].p20, 0, 16);
            continue;
        }
        for (int c = 0; c < 4; c++) {
            ip[n].p0.f[c] = v0->param[src].f[c];
            ip[n].p10.f[c] = v1->param[src].f[c] - v0->param[src].f[c];
            ip[n].p20.f[c] = v2->param[src].f[c] - v0->param[src].f[c];
        }
    }
}

typedef struct TriSetup {
    float x[3], y[3], z[3], rw[3];
    float area;
    bool front;
} TriSetup;

/* a set-up triangle waiting for the scan converter */
typedef struct TriJob {
    TriSetup t;
    int x0, y0, x1, y1;                 /* bounding box within the scissor */
    int64_t fx[3], fy[3], ex[3], ey[3], bias[3], area;
    EgInterp ip[];                      /* dc->ninterp entries */
} TriJob;

/* per-thread scan converter state */
typedef struct RasterCtx {
    DrawCtx *dc;
    FbState *fb;
    EgShader *sh;
    EgProgram ps;                       /* a private copy (decode cache) */
    EgProgram vs, fs;                   /* the same, for vs_parallel() */
    uint64_t pixels;
} RasterCtx;

/* run the PS for one quad and write the covered pixels */
static void shade_quad(RasterCtx *rc, const TriSetup *t, const EgInterp *ip,
                       int qx, int qy, uint32_t cover, const float bary[4][3])
{
    DrawCtx *dc = rc->dc;
    FbState *fb = rc->fb;
    EvergreenState *s = dc->s;
    EgShader *sh = rc->sh;
    uint32_t in0 = CTX(s, 0x286cc), in1 = CTX(s, 0x286d0), baryc = CTX(s, 0x286e0);
    int lane, reg, ch;

    /* GPRs keep the previous quad's values, as on hardware; reset the rest */
    sh->killed = 0;
    sh->pred = 0;
    sh->color_mask = 0;
    sh->param_mask = 0;
    sh->depth_written = false;
    sh->has_grad = false;
    sh->loop_index = 0;
    memset(sh->ar, 0, sizeof(sh->ar));
    sh->s = s;
    sh->prog = &rc->ps;
    sh->stage = EG_STAGE_PS;
    sh->nlanes = 4;
    sh->live = 0xf;
    sh->quad = true;
    sh->interp = ip;
    sh->ninterp = dc->ninterp;

    for (lane = 0; lane < 4; lane++) {
        const float *l = bary[lane];
        float pw = l[0] * t->rw[0] + l[1] * t->rw[1] + l[2] * t->rw[2];
        float pi = pw != 0 ? l[1] * t->rw[1] / pw : l[1];
        float pj = pw != 0 ? l[2] * t->rw[2] / pw : l[2];
        float px = qx + (lane & 1) + dc->pix_center, py = qy + (lane >> 1) + dc->pix_center;
        float z = l[0] * t->z[0] + l[1] * t->z[1] + l[2] * t->z[2];
        int k;

        /* barycentric pairs in Mesa's order: persp sample/center/centroid,
         * linear sample/center/centroid */
        reg = 0;
        ch = 0;
        for (k = 0; k < 6; k++) {
            static const int shift[6] = { 8, 0, 4, 24, 16, 20 };
            if (!((baryc >> shift[k]) & 3)) {
                continue;
            }
            sh->gpr[lane][reg].f[ch] = k < 3 ? pi : l[1];
            sh->gpr[lane][reg].f[ch + 1] = k < 3 ? pj : l[2];
            ch += 2;
            if (ch == 4) {
                ch = 0;
                reg++;
            }
        }
        if ((in0 >> 8) & 1) {                   /* POSITION_ENA */
            EgVec4 *p = &sh->gpr[lane][(in0 >> 10) & 0x1f];
            p->f[0] = px;
            p->f[1] = py;
            p->f[2] = z;
            p->f[3] = pw;
        }
        if ((in1 >> 8) & 1) {                   /* FRONT_FACE_ENA */
            EgVec4 *p = &sh->gpr[lane][(in1 >> 12) & 0x1f];
            uint32_t c = (in1 >> 9) & 3;
            if ((in1 >> 11) & 1) {
                p->u[c] = t->front ? ~0u : 0;
            } else {
                p->f[c] = t->front ? 1.0f : -1.0f;
            }
        }
        if ((in1 >> 24) & 1) {                  /* FIXED_PT_POSITION_ENA */
            EgVec4 *p = &sh->gpr[lane][(in1 >> 25) & 0x1f];
            p->u[0] = (uint32_t)(qx + (lane & 1)) << 4;
            p->u[1] = (uint32_t)(qy + (lane >> 1)) << 4;
            p->u[2] = 0;
            p->u[3] = 0;
        }
        sh->depth[lane].f[0] = z;
    }

    eg_shader_run(sh);

    for (lane = 0; lane < 4; lane++) {
        int x = qx + (lane & 1), y = qy + (lane >> 1), i;
        const float *l = bary[lane];
        float z = l[0] * t->z[0] + l[1] * t->z[1] + l[2] * t->z[2];
        if (!((cover >> lane) & 1) || ((sh->killed >> lane) & 1)) {
            continue;
        }
        if (sh->depth_written) {
            z = sh->depth[lane].f[0];
        }
        if (fb->alpha_test &&
            !cmp_func(fb->alpha_func, sh->color[lane][0].f[3], fb->alpha_ref)) {
            continue;
        }
        if (!db_test(dc, fb, x, y, z, t->front)) {
            continue;
        }
        rc->pixels++;
        if (fb->cb_mode == 3) {
            /* MSAA resolve (single-sample here): CB0 -> CB1.  Apple's GL
             * swap (generateSwapFlush) resolves the back buffer into the
             * window surface this way. */
            uint8_t px[16];
            EgVec4 v;
            if (fb->cb[0].enabled && fb->cb[1].enabled &&
                (uint32_t)x < fb->cb[0].width && (uint32_t)y < fb->cb[0].height) {
                eg_gpu_read(dc->s, fb->cb[0].base + eg_tile_offset(&fb->cb[0].tile, x, y), px,
                            fb->cb[0].bpp);
                cb_unpack(&fb->cb[0], px, &v);
                cb_write(dc, fb, x, y, &v, 1);
            }
            continue;
        }
        for (i = 0; i < 8; i++) {
            if (fb->cb[i].enabled && ((sh->color_mask >> i) & 1)) {
                cb_write(dc, fb, x, y, &sh->color[lane][i], i);
            }
        }
    }
}

/* scan convert a set-up triangle within the region [cx0, cx1) x [cy0, cy1) */
static void raster_job(RasterCtx *rc, const TriJob *j, int cx0, int cy0, int cx1, int cy1)
{
    DrawCtx *dc = rc->dc;
    int x0 = MAX(j->x0, cx0), y0 = MAX(j->y0, cy0);
    int x1 = MIN(j->x1, cx1), y1 = MIN(j->y1, cy1);
    int64_t pc = (int64_t)(dc->pix_center * 256);
    int qx, qy, k;

    for (qy = y0 & ~1; qy < y1; qy += 2) {
        for (qx = x0 & ~1; qx < x1; qx += 2) {
            float bary[4][3];
            uint32_t cover = 0;
            int lane;
            for (lane = 0; lane < 4; lane++) {
                int px = qx + (lane & 1), py = qy + (lane >> 1);
                int64_t sx = (int64_t)px * 256 + pc, sy = (int64_t)py * 256 + pc;
                bool in = true;
                for (k = 0; k < 3; k++) {
                    int a = (k + 1) % 3;
                    int64_t w = j->ex[k] * (sy - j->fy[a]) - j->ey[k] * (sx - j->fx[a]);
                    in = in && w + j->bias[k] >= 0;
                    bary[lane][k] = (float)((double)w / (double)j->area);
                }
                if (in && px >= x0 && px < x1 && py >= y0 && py < y1 &&
                    cliprect_pass(dc, px, py)) {
                    cover |= 1u << lane;
                }
            }
            if (cover) {
                shade_quad(rc, &j->t, j->ip, qx, qy, cover, bary);
            }
        }
    }
}

/* set up one triangle in window coordinates and queue it */
static void vk_add_tri(DrawCtx *dc, const EgVert *const v[3], const EgVert *prov, bool front);

static void raster_tri(DrawCtx *dc, FbState *fb, const EgVert *v0, const EgVert *v1,
                       const EgVert *v2, const EgVert *prov)
{
    TriSetup t;
    TriJob *j;
    const EgVert *v[3] = { v0, v1, v2 };
    float minx, maxx, miny, maxy;
    int x0, x1, y0, y1, k;
    int64_t area2;
    bool ccw;

    for (k = 0; k < 3; k++) {
        t.x[k] = v[k]->win[0];
        t.y[k] = v[k]->win[1];
        t.z[k] = v[k]->win[2];
        t.rw[k] = v[k]->win[3];
    }
    t.area = (t.x[1] - t.x[0]) * (t.y[2] - t.y[0]) - (t.x[2] - t.x[0]) * (t.y[1] - t.y[0]);
    if (t.area == 0 || isnan(t.area)) {
        return;
    }
    /* y points down: a positive area is clockwise on screen */
    ccw = t.area < 0;
    t.front = ccw == dc->front_ccw;
    if ((t.front && dc->cull_front) || (!t.front && dc->cull_back)) {
        return;
    }
    minx = MIN(t.x[0], MIN(t.x[1], t.x[2]));
    maxx = MAX(t.x[0], MAX(t.x[1], t.x[2]));
    miny = MIN(t.y[0], MIN(t.y[1], t.y[2]));
    maxy = MAX(t.y[0], MAX(t.y[1], t.y[2]));
    x0 = MAX(dc->sc_x0, (int)floorf(minx));
    y0 = MAX(dc->sc_y0, (int)floorf(miny));
    x1 = MIN(dc->sc_x1, (int)ceilf(maxx) + 1);
    y1 = MIN(dc->sc_y1, (int)ceilf(maxy) + 1);
    if (x0 >= x1 || y0 >= y1) {
        return;
    }
    if (dc->vkc) {
        vk_add_tri(dc, v, prov, t.front);
        return;
    }
    j = g_malloc(sizeof(TriJob) + sizeof(EgInterp) * dc->ninterp);
    j->t = t;
    j->x0 = x0;
    j->y0 = y0;
    j->x1 = x1;
    j->y1 = y1;
    setup_interp(dc, v0, v1, v2, prov, j->ip);
    dc->s->gfx->prims++;
    {
        /* EG_PROBE=x,y: log the triangles that cover a pixel */
        static int px = -2, py;
        const char *e;
        if (px == -2) {
            px = -1;
            e = getenv("EG_PROBE");
            if (e && sscanf(e, "%d,%d", &px, &py) != 2) {
                px = -1;
            }
        }
        if (px >= 0 && px >= x0 && px < x1 && py >= y0 && py < y1) {
            qemu_log("evergreen: probe draw #%" PRIu64 " tri (%g,%g) (%g,%g) (%g,%g) "
                     "cb0 %08x ps %" PRIx64 " p0 %g %g %g %g\n",
                     dc->s->gfx->draws, t.x[0], t.y[0], t.x[1], t.y[1], t.x[2], t.y[2],
                     CTX(dc->s, 0x28c60), dc->ps.addr, v0->param[0].f[0], v0->param[0].f[1],
                     v0->param[0].f[2], v0->param[0].f[3]);
        }
    }

    /*
     * Edge functions in fixed point (8 fractional bits), so that pixels on
     * an edge shared by two triangles belong to exactly one of them (the
     * top-left rule needs exact zeros).
     */
    for (k = 0; k < 3; k++) {
        j->fx[k] = llrintf(t.x[k] * 256.0f);
        j->fy[k] = llrintf(t.y[k] * 256.0f);
    }
    area2 = (j->fx[1] - j->fx[0]) * (j->fy[2] - j->fy[0]) -
            (j->fx[2] - j->fx[0]) * (j->fy[1] - j->fy[0]);
    if (!area2) {
        g_free(j);
        return;
    }
    for (k = 0; k < 3; k++) {
        int a = (k + 1) % 3, b = (k + 2) % 3;
        int sign = area2 > 0 ? 1 : -1;
        /* edge opposite vertex k, oriented so inside is positive */
        j->ex[k] = (j->fx[b] - j->fx[a]) * sign;
        j->ey[k] = (j->fy[b] - j->fy[a]) * sign;
        /* top or left edge: owned, others need a strictly positive w */
        j->bias[k] = ((j->ey[k] == 0 && j->ex[k] > 0) || j->ey[k] < 0) ? 0 : -1;
    }
    j->area = area2 > 0 ? area2 : -area2;
    g_ptr_array_add(dc->jobs, j);
    dc->job_area += (uint64_t)(x1 - x0) * (y1 - y0);
    (void)fb;
}

/* ------------------------------------------------------------------ */
/* Raster workers                                                      */
/* ------------------------------------------------------------------ */

#define RASTER_TILE     32
#define RASTER_MAX      16

static struct {
    int n;
    QemuThread th[RASTER_MAX];
    QemuSemaphore go[RASTER_MAX], done;
    RasterCtx rc[RASTER_MAX];
    GPtrArray *jobs;
    DrawCtx *vs_dc;                     /* set: the workers run vertex shaders */
    uint32_t vs_chunk;
} rpool;

static void raster_tiles(RasterCtx *rc, GPtrArray *jobs, int me, int n)
{
    guint i;

    for (i = 0; i < jobs->len; i++) {
        const TriJob *j = g_ptr_array_index(jobs, i);
        int tx, ty;
        for (ty = j->y0 / RASTER_TILE; ty <= (j->y1 - 1) / RASTER_TILE; ty++) {
            for (tx = j->x0 / RASTER_TILE; tx <= (j->x1 - 1) / RASTER_TILE; tx++) {
                if ((tx + ty * 5) % n == me) {
                    raster_job(rc, j, tx * RASTER_TILE, ty * RASTER_TILE,
                               (tx + 1) * RASTER_TILE, (ty + 1) * RASTER_TILE);
                }
            }
        }
    }
}

static void *raster_worker(void *arg)
{
    int me = (int)(intptr_t)arg;

    rcu_register_thread();
    for (;;) {
        qemu_sem_wait(&rpool.go[me]);
        if (rpool.vs_dc) {
            DrawCtx *dc = rpool.vs_dc;
            uint32_t a = MIN(dc->npend, me * rpool.vs_chunk);
            RasterCtx *rc = &rpool.rc[me];
            vs_shade(dc, rc->sh, &rc->vs, &rc->fs, a, MIN(dc->npend, a + rpool.vs_chunk));
        } else {
            raster_tiles(&rpool.rc[me], rpool.jobs, me, rpool.n);
        }
        qemu_sem_post(&rpool.done);
    }
    return NULL;
}

static void raster_pool_init(void)
{
    const char *e = getenv("EG_RASTER_THREADS");
    long ncpu = g_get_num_processors();
    int i;

    if (rpool.n) {
        return;
    }
    rpool.n = MAX(1, MIN(RASTER_MAX, e ? atoi(e) : (int)(ncpu / 2)));
    qemu_sem_init(&rpool.done, 0);
    for (i = 0; i < rpool.n; i++) {
        rpool.rc[i].sh = g_new0(EgShader, 1);
        qemu_sem_init(&rpool.go[i], 0);
        if (rpool.n > 1) {
            qemu_thread_create(&rpool.th[i], "eg-raster", raster_worker,
                               (void *)(intptr_t)i, QEMU_THREAD_DETACHED);
        }
    }
}

/*
 * Large draws shade their vertices on the raster workers, each with its
 * own shader, in contiguous runs of vertices (as the RV530 rasterizes on a
 * pool).  Vertex fetch only reads guest memory.
 */
#define VS_PARALLEL_MIN 128

static bool vs_parallel(DrawCtx *dc)
{
    int i, n;

    if (dc->npend < VS_PARALLEL_MIN) {
        return false;
    }
    raster_pool_init();
    if (rpool.n < 2) {
        return false;
    }
    n = MIN(rpool.n, (int)(dc->npend / (VS_PARALLEL_MIN / 2)));
    rpool.vs_chunk = ROUND_UP(DIV_ROUND_UP(dc->npend, n), EG_MAX_LANES);
    rpool.vs_dc = dc;
    for (i = 0; i < n; i++) {
        RasterCtx *rc = &rpool.rc[i];
        rc->vs = dc->vs;
        rc->vs.code = g_memdup2(dc->vs.code, dc->vs.ndw * 4);
        rc->vs.groups = NULL;
        rc->fs = dc->fs;
        rc->fs.code = dc->have_fs ? g_memdup2(dc->fs.code, dc->fs.ndw * 4) : NULL;
        rc->fs.groups = NULL;
        memset(rpool.rc[i].sh->kc_key, 0, sizeof(rpool.rc[i].sh->kc_key));
        eg_tex_cache_reset(rpool.rc[i].sh);
        qemu_sem_post(&rpool.go[i]);
    }
    for (i = 0; i < n; i++) {
        qemu_sem_wait(&rpool.done);
    }
    rpool.vs_dc = NULL;
    for (i = 0; i < n; i++) {
        eg_tex_cache_reset(rpool.rc[i].sh);
        eg_program_free(&rpool.rc[i].vs);
        if (dc->have_fs) {
            eg_program_free(&rpool.rc[i].fs);
        }
    }
    return true;
}

static void raster_ctx_begin(RasterCtx *rc, DrawCtx *dc, FbState *fb)
{
    rc->dc = dc;
    rc->fb = fb;
    rc->pixels = 0;
    memset(rc->sh->kc_key, 0, sizeof(rc->sh->kc_key));
    eg_tex_cache_reset(rc->sh);
    rc->ps = dc->ps;
    rc->ps.code = g_memdup2(dc->ps.code, dc->ps.ndw * 4);
    rc->ps.groups = NULL;
}

static void raster_ctx_end(RasterCtx *rc)
{
    rc->dc->pixels += rc->pixels;
    eg_program_free(&rc->ps);
}

/* scan convert the queued triangles: in parallel by screen tiles when there
 * is enough work (each pixel stays with one thread, in submission order) */
static void flush_jobs(DrawCtx *dc, FbState *fb)
{
    int i, n;

    if (!dc->jobs->len) {
        return;
    }
    raster_pool_init();
    n = dc->job_area >= 32 * 1024 ? rpool.n : 1;
    for (i = 0; i < n; i++) {
        raster_ctx_begin(&rpool.rc[i], dc, fb);
    }
    if (n == 1) {
        guint k;
        for (k = 0; k < dc->jobs->len; k++) {
            raster_job(&rpool.rc[0], g_ptr_array_index(dc->jobs, k), INT_MIN, INT_MIN,
                       INT_MAX, INT_MAX);
        }
    } else {
        rpool.jobs = dc->jobs;
        for (i = 0; i < n; i++) {
            qemu_sem_post(&rpool.go[i]);
        }
        for (i = 0; i < n; i++) {
            qemu_sem_wait(&rpool.done);
        }
    }
    for (i = 0; i < n; i++) {
        raster_ctx_end(&rpool.rc[i]);
    }
    g_ptr_array_set_size(dc->jobs, 0);
    dc->job_area = 0;
}

/* clip a triangle against w > eps and the z planes, then rasterize */
static void clip_tri(DrawCtx *dc, FbState *fb, const EgVert *a, const EgVert *b,
                     const EgVert *c, const EgVert *prov)
{
    EvergreenState *s = dc->s;
    uint32_t cl = CTX(s, 0x28810);
    bool dx_z = (cl >> 19) & 1;
    EgVert buf[2][12], tmp;
    int n = 3, plane, i, cur = 0;
    float planes[3][4] = {
        { 0, 0, 0, 1 },                                 /* w > eps */
        { 0, 0, 1, dx_z ? 0.0f : 1.0f },                /* near */
        { 0, 0, -1, 1 },                                /* far */
    };
    bool need = false;
    const EgVert *in[3] = { a, b, c };

    for (i = 0; i < 3; i++) {
        const float *p = in[i]->clip;
        if (p[3] <= 1e-6f || (!((cl >> 26) & 1) && (dx_z ? p[2] < 0 : p[2] < -p[3])) ||
            (!((cl >> 27) & 1) && p[2] > p[3])) {
            need = true;
        }
    }
    if (!need || ((cl >> 16) & 1)) {
        EgVert va = *a, vb = *b, vc = *c;
        if ((a->clip[3] <= 0 || b->clip[3] <= 0 || c->clip[3] <= 0) &&
            !((CTX(s, 0x28818) >> 8) & 1)) {
            return;                         /* clipping disabled, behind the eye */
        }
        to_window(dc, &va);
        to_window(dc, &vb);
        to_window(dc, &vc);
        raster_tri(dc, fb, &va, &vb, &vc, prov);
        return;
    }
    buf[0][0] = *a;
    buf[0][1] = *b;
    buf[0][2] = *c;
    for (plane = 0; plane < 3; plane++) {
        EgVert *src = buf[cur], *dst = buf[cur ^ 1];
        int m = 0;
        if ((plane == 1 && ((cl >> 26) & 1)) || (plane == 2 && ((cl >> 27) & 1))) {
            continue;
        }
        for (i = 0; i < n; i++) {
            EgVert *p = &src[i], *q = &src[(i + 1) % n];
            const float *pl = planes[plane];
            float dp = pl[0] * p->clip[0] + pl[1] * p->clip[1] + pl[2] * p->clip[2] +
                       pl[3] * p->clip[3] - (plane == 0 ? 1e-6f : 0);
            float dq = pl[0] * q->clip[0] + pl[1] * q->clip[1] + pl[2] * q->clip[2] +
                       pl[3] * q->clip[3] - (plane == 0 ? 1e-6f : 0);
            if (dp >= 0 && m < 12) {
                dst[m++] = *p;
            }
            if ((dp >= 0) != (dq >= 0) && m < 12) {
                float tt = dp / (dp - dq);
                int k, cc;
                for (k = 0; k < 4; k++) {
                    tmp.clip[k] = p->clip[k] + (q->clip[k] - p->clip[k]) * tt;
                }
                tmp.psize = p->psize;
                for (k = 0; k < EG_NUM_PARAMS; k++) {
                    for (cc = 0; cc < 4; cc++) {
                        tmp.param[k].f[cc] = p->param[k].f[cc] +
                                             (q->param[k].f[cc] - p->param[k].f[cc]) * tt;
                    }
                }
                dst[m++] = tmp;
            }
        }
        n = m;
        cur ^= 1;
        if (n < 3) {
            return;
        }
    }
    for (i = 0; i < n; i++) {
        to_window(dc, &buf[cur][i]);
    }
    for (i = 1; i + 1 < n; i++) {
        raster_tri(dc, fb, &buf[cur][0], &buf[cur][i], &buf[cur][i + 1], prov);
    }
}

/* a screen-aligned quad around a point or along a line */
static void raster_quad_win(DrawCtx *dc, FbState *fb, EgVert q[4], const EgVert *prov)
{
    raster_tri(dc, fb, &q[0], &q[1], &q[2], prov);
    raster_tri(dc, fb, &q[1], &q[3], &q[2], prov);
}

static void draw_point(DrawCtx *dc, FbState *fb, const EgVert *p)
{
    EgVert v = *p, q[4];
    uint32_t ps = CTX(dc->s, 0x28a00);
    float hw = (ps >> 16) / 16.0f, hh = (ps & 0xffff) / 16.0f;
    int k;

    if (v.clip[3] <= 0) {
        return;
    }
    if ((CTX(dc->s, 0x2881c) >> 16) & 1) {
        hw = hh = v.psize / 2;
    }
    to_window(dc, &v);
    for (k = 0; k < 4; k++) {
        q[k] = v;
        q[k].win[0] += (k & 1) ? hw : -hw;
        q[k].win[1] += (k & 2) ? hh : -hh;
    }
    raster_quad_win(dc, fb, q, p);
}

static void draw_line(DrawCtx *dc, FbState *fb, const EgVert *a, const EgVert *b)
{
    EgVert va = *a, vb = *b, q[4];
    float w = (CTX(dc->s, 0x28a08) & 0xffff) / 16.0f, dx, dy, len, nx, ny;

    if (va.clip[3] <= 0 || vb.clip[3] <= 0) {
        return;
    }
    if (w <= 0) {
        w = 0.5f;
    }
    to_window(dc, &va);
    to_window(dc, &vb);
    dx = vb.win[0] - va.win[0];
    dy = vb.win[1] - va.win[1];
    len = sqrtf(dx * dx + dy * dy);
    if (len == 0) {
        return;
    }
    nx = -dy / len * w;
    ny = dx / len * w;
    q[0] = va; q[1] = vb; q[2] = va; q[3] = vb;
    q[0].win[0] += nx; q[0].win[1] += ny;
    q[1].win[0] += nx; q[1].win[1] += ny;
    q[2].win[0] -= nx; q[2].win[1] -= ny;
    q[3].win[0] -= nx; q[3].win[1] -= ny;
    raster_quad_win(dc, fb, q, dc->provoking_last ? b : a);
}

/* RECTLIST: three corners, the fourth is completed opposite the right angle */
static void draw_rect(DrawCtx *dc, FbState *fb, const EgVert *a, const EgVert *b,
                      const EgVert *c)
{
    const EgVert *v[3] = { a, b, c };
    EgVert w[3], d;
    int k, corner = 0, i, cc;

    for (k = 0; k < 3; k++) {
        w[k] = *v[k];
        to_window(dc, &w[k]);
    }
    {
        /* the right angle: the corner whose two edges are perpendicular */
        float best = INFINITY;
        for (k = 0; k < 3; k++) {
            const EgVert *p = &w[k], *q = &w[(k + 1) % 3], *r = &w[(k + 2) % 3];
            float dot = fabsf((q->win[0] - p->win[0]) * (r->win[0] - p->win[0]) +
                              (q->win[1] - p->win[1]) * (r->win[1] - p->win[1]));
            if (dot < best) {
                best = dot;
                corner = k;
            }
        }
    }
    {
        const EgVert *p = &w[corner], *q = &w[(corner + 1) % 3], *r = &w[(corner + 2) % 3];
        d = *p;
        for (i = 0; i < 4; i++) {
            d.win[i] = q->win[i] + r->win[i] - p->win[i];
            d.clip[i] = q->clip[i] + r->clip[i] - p->clip[i];
        }
        for (i = 0; i < EG_NUM_PARAMS; i++) {
            for (cc = 0; cc < 4; cc++) {
                d.param[i].f[cc] = q->param[i].f[cc] + r->param[i].f[cc] - p->param[i].f[cc];
            }
        }
        raster_tri(dc, fb, p, q, r, a);
        raster_tri(dc, fb, q, &d, r, a);
    }
}

/* ------------------------------------------------------------------ */
/* Vulkan backend glue (eg_vk.h)                                       */
/* ------------------------------------------------------------------ */

/*
 * The pixel stage of a draw runs on the host GPU when the PS translates
 * and every surface it touches is in VRAM or the GART: the vertex stage,
 * clipping and triangle set-up stay above (raster_tri collects the
 * window-space triangles), the PS and the DB/CB back end run in one
 * fragment shader (eg_vk_frag.glsl).  The GPU reads and writes guest VRAM
 * in place (imported host memory), so queued work is finished
 * (eg_gfx_flush) before the CPU touches GPU memory: CPU-path draws, CP and
 * DMA copies, fences, memory writes, query results, HDP apertures and the
 * display.
 */

typedef struct EgPsEntry {
    char *glsl;                         /* NULL: does not translate */
    EvkProgInfo info;
    char why[96];
    uint32_t used[2];                   /* dwords of the programs it was made from */
    bool warned;
} EgPsEntry;

/*
 * Program hash (the translation cache key), 8 bytes at a time: each draw
 * hashes up to three 4 KiB program chunks, and a byte-wise FNV of them
 * cost more than the rest of the draw's preparation.
 */
static uint64_t fnv64(uint64_t h, const void *p, size_t n)
{
    const uint8_t *b = p;

    for (; n >= 8; n -= 8, b += 8) {
        h = (h ^ ldq_le_p(b)) * 0x9e3779b97f4a7c15ull;
        h ^= h >> 29;
    }
    while (n--) {
        h = (h ^ *b++) * 0x100000001b3ull;
    }
    return h;
}

static void ps_entry_free(gpointer p)
{
    EgPsEntry *e = p;
    free(e->glsl);
    g_free(e);
}

static const uint32_t *ps_fetch(void *opaque, uint32_t dw, uint32_t n)
{
    EgProgram *p = opaque;
    if (dw + n > p->ndw) {
        return NULL;
    }
    p->used = MAX(p->used, dw + n);
    return p->code + dw;
}

/*
 * Per program address: how many dwords its translation read, and their
 * hash.  When the guest's dwords there still hash the same, the draw uses
 * that translation again without translating (pgm_cache_add) or hashing
 * the rest of the loaded chunk.
 */
#define PGM_FAST 256

typedef struct PgmFast {
    uint64_t a[2];
    uint32_t tag, len[2];
    uint64_t hl, h;
    EgPsEntry *e;
} PgmFast;

static uint64_t pgm_fast_hash(const uint32_t *key, size_t nkey, const EgProgram *p0,
                              const EgProgram *p1, const uint32_t len[2])
{
    uint64_t h = fnv64(0x84222325cbf29ce4ull, key, nkey * 4);
    h = fnv64(h, p0->code, (size_t)len[0] * 4);
    if (p1) {
        h = fnv64(h, p1->code, (size_t)len[1] * 4);
    }
    return h;
}

static PgmFast *pgm_fast_slot(EgGfx *g, uint32_t tag, uint64_t a0, uint64_t a1)
{
    uint64_t k = (a0 * 0x9e3779b97f4a7c15ull) ^ (a1 * 0xc2b2ae3d27d4eb4full) ^ tag;

    if (!g->pgm_fast) {
        g->pgm_fast = g_new0(PgmFast, PGM_FAST);
    }
    return &((PgmFast *)g->pgm_fast)[(k ^ (k >> 29)) % PGM_FAST];
}

static EgPsEntry *pgm_fast_find(EgGfx *g, uint32_t tag, const uint32_t *key, size_t nkey,
                                const EgProgram *p0, const EgProgram *p1, uint64_t *h)
{
    PgmFast *f = pgm_fast_slot(g, tag, p0->addr, p1 ? p1->addr : 0);

    if (f->e && f->tag == tag && f->a[0] == p0->addr && f->a[1] == (p1 ? p1->addr : 0) &&
        f->len[0] <= p0->ndw && (!p1 || f->len[1] <= p1->ndw) &&
        pgm_fast_hash(key, nkey, p0, p1, f->len) == f->hl) {
        *h = f->h;
        return f->e;
    }
    return NULL;
}

static void pgm_fast_note(EgGfx *g, uint32_t tag, const uint32_t *key, size_t nkey,
                          const EgProgram *p0, const EgProgram *p1, EgPsEntry *e, uint64_t h)
{
    PgmFast *f = pgm_fast_slot(g, tag, p0->addr, p1 ? p1->addr : 0);

    if (!e->used[0]) {
        return;                         /* not translated here: lengths unknown */
    }
    f->tag = tag;
    f->a[0] = p0->addr;
    f->a[1] = p1 ? p1->addr : 0;
    f->len[0] = e->used[0];
    f->len[1] = p1 ? e->used[1] : 0;
    f->hl = pgm_fast_hash(key, nkey, p0, p1, f->len);
    f->h = h;
    f->e = e;
}

/*
 * The translation cache key: the dwords that the translation read, not the
 * whole loaded chunk.  The guest keeps other data after its programs, and a
 * hash of that made a new translation and new pipelines for each change.
 * A program that is in the cache already keeps the entry (and pipelines) it
 * has; the new translation 'e' is then dropped.
 */
static EgPsEntry *pgm_cache_add(EgGfx *g, const uint32_t *key, size_t nkey,
                                const EgProgram *p0, const EgProgram *p1, EgPsEntry *e,
                                uint64_t *hash)
{
    uint64_t h = fnv64(0xcbf29ce484222325ull, key, nkey * 4), *k;
    EgPsEntry *old;

    h = fnv64(h, p0->code, (size_t)MAX(e->used[0], 1) * 4);
    if (p1) {
        h = fnv64(h ^ 0x4653, p1->code, (size_t)e->used[1] * 4);
    }
    *hash = h;
    old = g_hash_table_lookup(g->ps_cache, &h);
    if (old) {
        ps_entry_free(e);
        return old;
    }
    k = g_new(uint64_t, 1);
    *k = h;
    g_hash_table_insert(g->ps_cache, k, e);
    return e;
}

/* VRAM byte offset, or GART byte offset (*gart), of [addr, addr + len) */
static bool vk_locate(EvergreenState *s, uint64_t addr, uint64_t len, uint32_t *off,
                      bool *gart)
{
    EgGfx *g = s->gfx;
    uint64_t start, end, p, last, nptes = 0;
    const uint64_t *ptes;
    uint32_t npages, *tab;

    len = MAX(len, 1);
    if (eg_vram_ptr(s, addr, len)) {
        uint64_t fbs = (uint64_t)(s->regs[EG_MC_VM_FB_LOCATION >> 2] & 0xffff) << 24;
        if (addr - fbs + len > 0xffffffffull) {
            return false;
        }
        *off = (uint32_t)(addr - fbs);
        *gart = false;
        return true;
    }
    if (!g->vk_sysram || !eg_gart_range(s, &start, &end) || addr < start ||
        addr + len - 1 > end || addr - start + len > 0xffffffffull) {
        return false;
    }
    npages = (uint32_t)MIN((end - start + 1) >> 12, 1u << 20);
    if (((addr + len - 1 - start) >> 12) >= npages ||
        !(tab = eg_vk_gart_table(g->vk, npages)) ||
        !(ptes = eg_gart_ptes(s, &nptes)) || ((addr + len - 1 - start) >> 12) >= nptes) {
        return false;
    }
    if (g->gart_gen_pages < npages) {
        g->gart_gen_of = g_renew(uint32_t, g->gart_gen_of, npages);
        g->gart_pte_of = g_renew(uint64_t, g->gart_pte_of, npages);
        memset(g->gart_gen_of + g->gart_gen_pages, 0,
               (npages - g->gart_gen_pages) * sizeof(uint32_t));
        g->gart_gen_pages = npages;
    }
    if (g->gart_key != (start ^ ((uint64_t)s->regs[EG_MC_VM_FB_LOCATION >> 2] << 32))) {
        g->gart_key = start ^ ((uint64_t)s->regs[EG_MC_VM_FB_LOCATION >> 2] << 32);
        g->gart_gen++;          /* VRAM pages moved, or the aperture did */
    }
    last = (addr + len - 1 - start) >> 12;
    /*
     * An entry stays valid while its PTE keeps its value: the guest edits
     * PTEs in VRAM, which the CPU sees at once.  Resets bump gart_gen.
     */
    for (p = (addr - start) >> 12; p <= last; p++) {
        uint64_t pte = ptes[p];
        if (g->gart_gen_of[p] == g->gart_gen && g->gart_pte_of[p] == pte) {
            continue;
        }
        if (!eg_gart_vk_entry(s, start + (p << 12), g->vk_sysram, &tab[p])) {
            return false;
        }
        g->gart_gen_of[p] = g->gart_gen;
        g->gart_pte_of[p] = pte;
    }
    *off = (uint32_t)(addr - start);
    *gart = true;
    return true;
}

/* the translated PS of a draw, cached by its code and input set-up */
static EgPsEntry *vk_ps(DrawCtx *dc, uint64_t *hash)
{
    EvergreenState *s = dc->s;
    EgGfx *g = s->gfx;
    uint32_t key[4] = { CTX(s, 0x286e0), CTX(s, 0x286cc), CTX(s, 0x286d0), evk_ps_build_id() };
    uint64_t h;
    EgPsEntry *e = pgm_fast_find(g, 0x5053, key, 4, &dc->ps, NULL, &h);

    if (e) {
        *hash = h;
        return e;
    }
    e = g_new0(EgPsEntry, 1);
    dc->ps.used = 0;
    e->glsl = evk_ps_to_glsl(ps_fetch, &dc->ps, key[0], key[1], key[2], &e->info,
                             e->why, sizeof(e->why));
    e->used[0] = dc->ps.used;
    e = pgm_cache_add(g, key, 4, &dc->ps, NULL, e, &h);
    if (!e->glsl && !e->warned) {
        e->warned = true;
        eg_gfx_warn(s, 0xf0, "evergreen: a pixel shader stays on the CPU path: %s\n", e->why);
        trace_eg_vk_fallback(e->why);
    }
    pgm_fast_note(g, 0x5053, key, 4, &dc->ps, NULL, e, h);
    *hash = h;
    return e;
}

static bool vk_surf(EvergreenState *s, EvkSurf *o, uint64_t base, const EgTiling *t)
{
    uint32_t off;
    bool gart;

    if (!vk_locate(s, base, eg_tile_size(t), &off, &gart)) {
        return false;
    }
    o->base = off;
    o->flags |= EVK_SF_ENABLED | (gart ? EVK_SF_GART : 0);
    eg_tiling_to_vk(t, &o->tile);
    return true;
}

static void vk_range(uint64_t *off, uint64_t *len, int *n, int max, const EvkSurf *o,
                     uint32_t base, const EgTiling *t)
{
    if (*n < max) {
        off[*n] = ((o->flags & EVK_SF_GART) ? EVK_GART_SPACE : 0) + base;
        len[*n] = eg_tile_size(t);
        (*n)++;
    }
}

/*
 * The bytes of rows [y0, y1) of a surface: one span in every layout (a 2D
 * macro tile row is contiguous before the pipe and bank bits go in above
 * the pipe interleave).  Tile-split surfaces: all of it.
 */
static void surf_rows(const EgTiling *t, int y0, int y1, uint64_t *lo, uint64_t *hi)
{
    uint64_t full = eg_tile_size(t);

    *lo = 0;
    *hi = full;
    y0 = MAX(y0, 0);
    y1 = MIN(y1, (int)t->height);
    if (y0 >= y1) {
        *hi = 0;
        return;
    }
    if (t->array_mode == EG_ARRAY_1D_TILED_THIN1) {
        *lo = (uint64_t)(y0 >> 3) * t->tiles_per_row * t->micro_bytes;
        *hi = (uint64_t)((y1 + 7) >> 3) * t->tiles_per_row * t->micro_bytes;
    } else if (t->array_mode != EG_ARRAY_2D_TILED_THIN1) {
        *lo = (uint64_t)y0 * t->pitch * t->bpp;
        *hi = (uint64_t)y1 * t->pitch * t->bpp;
    } else if (!t->split) {
        uint64_t row = (uint64_t)t->mt_per_row * t->mt_bytes;
        uint64_t t0 = (uint64_t)(y0 >> t->mt_height_shift) * row;
        uint64_t t1 = (uint64_t)(((y1 - 1) >> t->mt_height_shift) + 1) * row;
        int sh = t->pi_bits + t->pipe_bits + t->bank_bits;
        *lo = (t0 >> t->pi_bits) << sh;
        *hi = (((t1 - 1) >> t->pi_bits) + 1) << sh;
    }
    *hi = MIN(*hi, full);
    *lo = MIN(*lo, *hi);
}

/* vk_range, plus the rows the scissor lets the draw change (EgVkDrawReq.wb_*) */
static void vk_range_rows(DrawCtx *dc, EgVkDrawReq *r, const EvkSurf *o, uint32_t base,
                          const EgTiling *t)
{
    int n = r->nwr;
    uint64_t lo, hi;

    vk_range(r->wr_off, r->wr_len, &r->nwr, 10, o, base, t);
    if (r->nwr > n) {
        surf_rows(t, dc->sc_y0, dc->sc_y1, &lo, &hi);
        r->wb_off[n] = r->wr_off[n] + lo;
        r->wb_len[n] = hi > lo ? hi - lo : 0;
        if (!r->wb_len[n]) {
            r->wb_off[n] = r->wr_off[n];        /* nothing: keep it a valid range */
            r->wb_len[n] = 1;
        }
    }
}

typedef struct VkPrep {
    EvkDraw draw;
    EgVkDrawReq req;
    EgPsEntry *ps;
    uint64_t wr_gpu[10];                /* GPU addresses of req.wr_* (verify) */
    EgTiling wr_tile[10];
} VkPrep;

/*
 * Texture descriptors by resource words: building one walks the tiling of
 * every mip level.  A hit locates its ranges again (GART PTEs can change).
 */

static VkTexEntry *vk_tex_rec;

static bool vk_locate_rec(EvergreenState *s, uint64_t addr, uint64_t len, uint32_t *off,
                          bool *gart)
{
    VkTexEntry *e = vk_tex_rec;

    if (!vk_locate(s, addr, len, off, gart)) {
        return false;
    }
    if (e && e->nloc < 2) {
        e->loc_addr[e->nloc] = addr;
        e->loc_len[e->nloc] = len;
        e->loc_off[e->nloc] = *off;
        e->loc_gart[e->nloc] = *gart;
        e->nloc++;
    }
    return true;
}

static bool vk_tex_build(EvergreenState *s, const uint32_t *res, EvkTex *o, uint64_t ro[2],
                         uint64_t rl[2], int *n)
{
    EgGfx *g = s->gfx;
    uint32_t key[10];
    uint64_t h;
    VkTexEntry *e;
    int k;

    memcpy(key, res, 32);
    key[8] = s->regs[EG_GB_ADDR_CONFIG >> 2];
    key[9] = s->regs[EG_MC_VM_FB_LOCATION >> 2];
    h = fnv64(0xcbf29ce484222325ull, key, sizeof(key));
    if (!g->vk_tex) {
        g->vk_tex = g_new0(VkTexEntry, VK_TEX_CACHE);
    }
    e = &((VkTexEntry *)g->vk_tex)[(h ^ (h >> 32)) % VK_TEX_CACHE];
    if (e->valid && !memcmp(e->key, key, sizeof(key))) {
        for (k = 0; k < e->nloc; k++) {
            uint32_t off;
            bool gart;
            if (!vk_locate(s, e->loc_addr[k], e->loc_len[k], &off, &gart) ||
                off != e->loc_off[k] || gart != e->loc_gart[k]) {
                break;
            }
        }
        if (k == e->nloc) {
            *o = e->tex;
            memcpy(ro, e->ro, sizeof(e->ro));
            memcpy(rl, e->rl, sizeof(e->rl));
            *n = e->nrd;
            return true;
        }
    }
    e->valid = false;
    e->nloc = 0;
    vk_tex_rec = e;
    if (!eg_tex_vk_build(s, res, o, vk_locate_rec, ro, rl, n)) {
        vk_tex_rec = NULL;
        return false;
    }
    vk_tex_rec = NULL;
    memcpy(e->key, key, sizeof(key));
    e->tex = *o;
    memcpy(e->ro, ro, sizeof(e->ro));
    memcpy(e->rl, rl, sizeof(e->rl));
    e->nrd = *n;
    e->valid = true;
    return true;
}

/* everything the GPU needs besides the vertices; false: use the CPU path */
/* PS inputs with FLAT_SHADE: they take the provoking vertex */
static uint32_t vk_flatmask(DrawCtx *dc)
{
    uint32_t m = 0;
    int n;

    for (n = 0; n < dc->ninterp && n < EVK_MAX_PARAMS; n++) {
        if ((dc->interp_cntl[n] >> 10) & 1) {
            m |= 1u << n;
        }
    }
    return m;
}

static bool vk_prepare(DrawCtx *dc, FbState *fb, VkPrep *vp, char *why, size_t whylen)
{
    EvergreenState *s = dc->s;
    EgGfx *g = s->gfx;
    EvkDraw *d = &vp->draw;
    EgVkDrawReq *r = &vp->req;
    uint64_t hash;
    int i, k;

    memset(vp, 0, sizeof(*vp));
    if (dc->ninterp > EVK_MAX_PARAMS) {
        snprintf(why, whylen, "%d PS inputs", dc->ninterp);
        return false;
    }
    vp->ps = vk_ps(dc, &hash);
    if (!vp->ps->glsl) {
        snprintf(why, whylen, "PS: %s", vp->ps->why);
        return false;
    }
    r->draw = d;
    r->ps_glsl = vp->ps->glsl;
    r->prog_hash = hash;
    r->sc_x0 = dc->sc_x0;
    r->sc_y0 = dc->sc_y0;
    r->sc_x1 = dc->sc_x1;
    r->sc_y1 = dc->sc_y1;

    /* constants */
    for (i = 0; i < vp->ps->info.nkc; i++) {
        uint32_t bank = vp->ps->info.kc_bank[i], mode = vp->ps->info.kc_mode[i];
        uint64_t base = ((uint64_t)CTX(s, 0x28940 + 4 * (bank & 15)) << 8) +
                        (uint64_t)vp->ps->info.kc_addr[i] * 16 * 16;
        if (mode && !CTX(s, 0x28940 + 4 * (bank & 15))) {
            eg_kcache_cfile(s, true, vp->ps->info.kc_addr[i], mode == 2 ? 32 : 16, d->kc[i]);
        } else if (mode) {
            eg_gpu_read(s, base, d->kc[i], (mode == 2 ? 32 : 16) * 16);
        }
    }
    if (vp->ps->info.uses_cfile) {
        memcpy(d->cfile, g->alu_const, sizeof(d->cfile));
    }
    memcpy(d->loopc, g->loop_const, sizeof(d->loopc));
    memcpy(d->blend_const, fb->blend_const, sizeof(d->blend_const));

    /* textures, samplers, vertex buffers */
    for (i = 0; i < vp->ps->info.ntex; i++) {
        const uint32_t *res = g->resource[vp->ps->info.tex_rid[i] & (EG_NUM_RESOURCES - 1)];
        uint64_t ro[2], rl[2];
        int n;
        if (!vk_tex_build(s, res, &d->tex[i], ro, rl, &n)) {
            snprintf(why, whylen, "texture %u not in VRAM or the GART", vp->ps->info.tex_rid[i]);
            return false;
        }
        if (n && r->nrd < EVK_MAX_TEX + EVK_MAX_VBUF) {
            /* one range per texture: the hazard check over-approximates */
            uint64_t lo = ro[0], hi = ro[0] + rl[0];
            for (k = 1; k < n; k++) {
                lo = MIN(lo, ro[k]);
                hi = MAX(hi, ro[k] + rl[k]);
            }
            r->rd_off[r->nrd] = lo;
            r->rd_len[r->nrd] = hi - lo;
            r->nrd++;
        }
    }
    for (i = 0; i < vp->ps->info.nsmp; i++) {
        memcpy(d->smp[i], g->sampler[vp->ps->info.smp_sid[i] & (EG_NUM_SAMPLERS - 1)], 12);
    }
    for (i = 0; i < vp->ps->info.nvbuf; i++) {
        const uint32_t *res = g->resource[vp->ps->info.vbuf_id[i] & (EG_NUM_RESOURCES - 1)];
        uint64_t base = res[0] | ((uint64_t)(res[2] & 0xff) << 32);
        uint32_t off;
        bool gart = false;
        memcpy(d->vbuf[i], res, 32);
        if ((res[7] >> 30) == 3) {
            if (!vk_locate(s, base, (uint64_t)res[1] + 1, &off, &gart)) {
                snprintf(why, whylen, "PS vertex buffer not in VRAM or the GART");
                return false;
            }
            d->vbuf[i][0] = off;
            if (r->nrd < EVK_MAX_TEX + EVK_MAX_VBUF) {
                r->rd_off[r->nrd] = (gart ? EVK_GART_SPACE : 0) + off;
                r->rd_len[r->nrd] = (uint64_t)res[1] + 1;
                r->nrd++;
            }
        }
        d->vbuf[i][6] = gart;
    }

    /* color buffers */
    for (i = 0; i < 8; i++) {
        const CBuf *c = &fb->cb[i];
        EvkSurf *o = &d->cb[i];
        bool blend = ((c->blend >> 30) & 1) && !c->blend_bypass &&
                     !(c->number == 2 || c->number == 3 || c->number == 4 || c->number == 5);
        if (!c->enabled) {
            continue;
        }
        if (!vk_surf(s, o, c->base, &c->tile)) {
            snprintf(why, whylen, "color buffer %d not in VRAM or the GART", i);
            return false;
        }
        o->flags |= (cb_is_float(c->fmt) ? EVK_SF_FLOAT : 0) | (blend ? EVK_SF_BLEND : 0) |
                    (c->blend_clamp ? EVK_SF_BLEND_CLAMP : 0);
        o->fmt = c->fmt;
        o->number = c->number;
        for (k = 0; k < 4; k++) {
            o->chmap |= (uint32_t)(c->ch_comp[k] < 0 ? 15 : c->ch_comp[k]) << (4 * k);
            o->bits |= (uint32_t)cb_bits[c->fmt][k] << (8 * k);
        }
        o->bpp = c->bpp;
        o->nch = c->nch;
        o->blend = c->blend;
        o->mask = c->mask;
        o->width = c->width;
        o->height = c->height;
        if (fb->cb_mode == 3 && i == 0) {
            /* resolve source: read only */
            if (r->nrd < EVK_MAX_TEX + EVK_MAX_VBUF) {
                r->rd_off[r->nrd] = ((o->flags & EVK_SF_GART) ? EVK_GART_SPACE : 0) + o->base;
                r->rd_len[r->nrd] = eg_tile_size(&c->tile);
                r->nrd++;
            }
            continue;
        }
        vk_range_rows(dc, r, o, o->base, &c->tile);
        r->wr_sig[r->nwr - 1] = c->base ^ ((uint64_t)c->fmt << 40);
        vp->wr_gpu[r->nwr - 1] = c->base;
        vp->wr_tile[r->nwr - 1] = c->tile;
    }

    /* depth and stencil */
    if (fb->z_valid) {
        EvkSurf zw = { 0 };
        if (!vk_surf(s, &d->zb, fb->z_read, &fb->ztile) ||
            !vk_surf(s, &zw, fb->z_write_base, &fb->ztile) ||
            (zw.flags & EVK_SF_GART) != (d->zb.flags & EVK_SF_GART)) {
            snprintf(why, whylen, "depth buffer not in VRAM or the GART");
            return false;
        }
        d->zb.wbase = zw.base;
        vk_range_rows(dc, r, &zw, zw.base, &fb->ztile);
        r->wr_sig[r->nwr - 1] = fb->z_write_base ^ (1ull << 41);
        vp->wr_gpu[r->nwr - 1] = fb->z_write_base;
    }
    if (fb->s_valid) {
        EvkSurf sw = { 0 };
        if (!vk_surf(s, &d->sb, fb->s_read, &fb->stile) ||
            !vk_surf(s, &sw, fb->s_write, &fb->stile) ||
            (sw.flags & EVK_SF_GART) != (d->sb.flags & EVK_SF_GART)) {
            snprintf(why, whylen, "stencil buffer not in VRAM or the GART");
            return false;
        }
        d->sb.wbase = sw.base;
        vk_range_rows(dc, r, &sw, sw.base, &fb->stile);
        r->wr_sig[r->nwr - 1] = fb->s_write ^ (1ull << 42);
        vp->wr_gpu[r->nwr - 1] = fb->s_write;
    }

    /* the rest of the back end */
    d->misc[EVK_M_CB_MODE] = fb->cb_mode;
    d->misc[EVK_M_ROP] = fb->rop;
    d->misc[EVK_M_ALPHA_TEST] = fb->alpha_test;
    d->misc[EVK_M_ALPHA_FUNC] = fb->alpha_func;
    d->misc[EVK_M_ALPHA_REF] = f2u(fb->alpha_ref);
    d->misc[EVK_M_Z_FMT] = fb->z_valid ? fb->zfmt : 0;
    d->misc[EVK_M_Z_ENABLE] = fb->z_enable;
    d->misc[EVK_M_Z_WRITE] = fb->z_write;
    d->misc[EVK_M_Z_FUNC] = fb->zfunc;
    d->misc[EVK_M_S_VALID] = fb->s_valid;
    d->misc[EVK_M_S_ENABLE] = fb->s_enable;
    d->misc[EVK_M_DB_CTL] = fb->db_ctl;
    d->misc[EVK_M_SREF0] = fb->srefmask[0];
    d->misc[EVK_M_SREF1] = fb->srefmask[1];
    d->misc[EVK_M_Z_CLEAR] = fb->depth_clear;
    d->misc[EVK_M_Z_CLEAR_VAL] = f2u(fb->depth_clear_val);
    d->misc[EVK_M_S_CLEAR] = fb->stencil_clear;
    d->misc[EVK_M_S_CLEAR_VAL] = fb->stencil_clear_val;
    d->misc[EVK_M_SC_X0] = dc->sc_x0;
    d->misc[EVK_M_SC_Y0] = dc->sc_y0;
    d->misc[EVK_M_SC_X1] = dc->sc_x1;
    d->misc[EVK_M_SC_Y1] = dc->sc_y1;
    d->misc[EVK_M_CLIP_RULE] = dc->cliprect_rule;
    for (i = 0; i < 4; i++) {
        for (k = 0; k < 4; k++) {
            d->misc[EVK_M_CR0 + 4 * i + k] = (uint32_t)dc->cr[i][k];
        }
    }
    d->misc[EVK_M_BARYC] = CTX(s, 0x286e0);
    d->misc[EVK_M_IN0] = CTX(s, 0x286cc);
    d->misc[EVK_M_IN1] = CTX(s, 0x286d0);
    d->misc[EVK_M_PIX_CENTER] = f2u(dc->pix_center);
    d->misc[EVK_M_BOOLC] = g->bool_const[0];
    d->misc[EVK_M_FLATMASK] = vk_flatmask(dc);
    return true;
}

/* one window-space triangle, with the PS inputs as the CPU path sets them up */
static void vk_add_tri(DrawCtx *dc, const EgVert *const v[3], const EgVert *prov, bool front)
{
    /* the host samples at pixel centers; the guest at pix_center */
    float shift = 0.5f - dc->pix_center;
    int k, n;

    for (k = 0; k < 3; k++) {
        EvkVertex o;
        memset(&o, 0, sizeof(o));
        o.pos[0] = v[k]->win[0] + shift;
        o.pos[1] = v[k]->win[1] + shift;
        o.pos[2] = v[k]->win[2];
        o.pos[3] = v[k]->win[3] != 0 ? 1.0f / v[k]->win[3] : 1.0f;
        o.front = front;
        for (n = 0; n < dc->ninterp; n++) {
            int src = dc->interp_src[n];
            uint32_t cntl = dc->interp_cntl[n];
            if (src < 0) {
                uint32_t dv = (cntl >> 8) & 3;
                float a = (dv & 2) ? 1.0f : 0.0f;
                o.param[n][0] = o.param[n][1] = o.param[n][2] = a;
                o.param[n][3] = (dv == 1 || dv == 3) ? 1.0f : 0.0f;
            } else if ((cntl >> 10) & 1) {      /* FLAT_SHADE */
                memcpy(o.param[n], prov->param[src].f, 16);
            } else {
                memcpy(o.param[n], v[k]->param[src].f, 16);
            }
        }
        g_array_append_val(dc->vk_verts, o);
    }
    dc->s->gfx->prims++;
}

static bool vk_submit(DrawCtx *dc, VkPrep *vp, char *why, size_t whylen)
{
    EgGfx *g = dc->s->gfx;

    if (!dc->vk_verts->len) {
        return true;
    }
    vp->req.verts = (const EvkVertex *)dc->vk_verts->data;
    vp->req.nverts = dc->vk_verts->len;
    if (!eg_vk_draw(g->vk, &vp->req, why, whylen)) {
        return false;
    }
    if ((++g->vk_draws & 1023) == 0 && getenv("EG_VK_STATS")) {
        uint64_t d, b, p, br;
        eg_vk_stats(g->vk, &d, &b, &p, &br);
        qemu_log("evergreen: vk %" PRIu64 " draws, %" PRIu64 " fallbacks, %" PRIu64
                 " batches, %" PRIu64 " barriers, %" PRIu64 " pipelines\n", g->vk_draws,
                 g->vk_fallbacks, b, br, p);
    }
    return true;
}

/*
 * EG_PROFILE: time of the phases of a draw: 0 program load and state,
 * 1 GPU state (vk_prepare), 2 vertex shader, 3 assembly and setup,
 * 4 submit.  prof_mark(k) closes phase k, prof_mark(-1) starts phase 0.
 */
static int64_t prof_phase_ns[5], prof_last;
static bool prof_on_cached;

/*
 * EG_PROFILE=1: every 2 s, log the time spent preparing draws on the CPU
 * and waiting for GPU batches, and which callers flushed.
 */
static struct {
    int on;
    int64_t t0, draw_ns, flush_ns;
    uint64_t draws, flushes, kicks, cpu_draws, barriers, last_barriers, last_vk, last_wait;
    uint64_t gpu_vs;
    GHashTable *by_caller;
} prof = { .on = -1 };

static EvergreenState *prof_dev;
static uint64_t prof_fences, prof_fences_now, prof_fence_irqs;
static int64_t prof_fence_ns, prof_fence_max;

static void prof_report(void)
{
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
    GHashTableIter it;
    gpointer k, v;
    g_autoptr(GString) l = g_string_new(NULL);

    if (now - prof.t0 < 2000000000) {
        return;
    }
    g_hash_table_iter_init(&it, prof.by_caller);
    while (g_hash_table_iter_next(&it, &k, &v)) {
        g_string_append_printf(l, " %s=%u", strrchr((char *)k, '/') ?
                               strrchr((char *)k, '/') + 1 : (char *)k, GPOINTER_TO_UINT(v));
    }
    uint64_t wait = 0;
    if (prof_dev && prof_dev->gfx->vk) {
        uint64_t w = eg_vk_wait_ns(prof_dev->gfx->vk);
        wait = w - prof.last_wait;
        prof.last_wait = w;
    }
    qemu_log("evergreen: profile %.1fs: %" PRIu64 " draws (%" PRIu64 " GPU VS) %.0f ms (%"
             PRIu64 " on the CPU; state %.0f, vk %.0f, vs %.0f, setup %.0f, submit %.0f ms), %" PRIu64 " kicks, %"
             PRIu64 " GPU waits %.0f ms, frame waits %.0f ms, %" PRIu64 " barriers;%s\n",
             (now - prof.t0) / 1e9, prof.draws, prof.gpu_vs, prof.draw_ns / 1e6, prof.cpu_draws,
             prof_phase_ns[0] / 1e6, prof_phase_ns[1] / 1e6, prof_phase_ns[2] / 1e6,
             prof_phase_ns[3] / 1e6, prof_phase_ns[4] / 1e6, prof.kicks,
             prof.flushes, prof.flush_ns / 1e6, wait / 1e6, prof.barriers, l->str);
    if (prof_dev) {
        qemu_log("evergreen: profile mmio: %" PRIu64 " reads, %" PRIu64 " busy GRBM_STATUS, %"
                 PRIu64 " scratch flushes\n", prof_dev->stat_mmio_reads,
                 prof_dev->stat_busy_reads, prof_dev->stat_scratch_flushes);
        prof_dev->stat_mmio_reads = prof_dev->stat_busy_reads = 0;
        prof_dev->stat_scratch_flushes = 0;
        qemu_log("evergreen: profile fences: %" PRIu64 " queued (%" PRIu64 " with irq), %"
                 PRIu64 " at once; queue to write avg %.0f us max %.0f us\n", prof_fences,
                 prof_fence_irqs, prof_fences_now,
                 prof_fences ? prof_fence_ns / 1e3 / prof_fences : 0, prof_fence_max / 1e3);
        if (prof_dev->gfx->vk) {
            static uint64_t lg, lb, ll, ln;
            uint64_t gn, bn, ln2, n;
            eg_vk_gpu_times(prof_dev->gfx->vk, &gn, &bn, &ln2, &n);
            if (n) {
                qemu_log("evergreen: profile gpu: %" PRIu64 " batches, GPU %.0f ms (avg %.0f us, "
                         "copy-back %.0f ms), submit to recycle avg %.0f us\n", n - ln,
                         (gn - lg) / 1e6, n > ln ? (gn - lg) / 1e3 / (n - ln) : 0,
                         (bn - lb) / 1e6, n > ln ? (ln2 - ll) / 1e3 / (n - ln) : 0);
            }
            lg = gn; lb = bn; ll = ln2; ln = n;
        }
        {
            static uint64_t ldn, ldc;
            uint64_t dn, dc;
            eg_vk_done_latency(prof_dev->gfx->vk, &dn, &dc);
            static uint64_t lbind;
            uint64_t bind = eg_vk_binds(prof_dev->gfx->vk);
            if (dc > ldc) {
                qemu_log("evergreen: profile done: submit to GPU done avg %.0f us (%" PRIu64
                         " batches), %" PRIu64 " pipeline changes\n",
                         (dn - ldn) / 1e3 / (dc - ldc), dc - ldc, bind - lbind);
            }
            ldn = dn; ldc = dc; lbind = bind;
        }
        prof_fences = prof_fence_irqs = prof_fences_now = 0;
        prof_fence_ns = prof_fence_max = 0;
    }
    memset(prof_phase_ns, 0, sizeof(prof_phase_ns));
    g_hash_table_remove_all(prof.by_caller);
    prof.cpu_draws = prof.barriers = prof.gpu_vs = 0;
    prof.t0 = now;
    prof.draw_ns = prof.flush_ns = 0;
    prof.draws = prof.flushes = prof.kicks = 0;
}

static bool prof_enabled(void)
{
    if (prof.on < 0) {
        prof.on = getenv("EG_PROFILE") != NULL;
        prof_on_cached = prof.on;
        prof.by_caller = g_hash_table_new(g_str_hash, g_str_equal);
        prof.t0 = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
    }
    return prof.on;
}

static void fence_run(EvergreenState *s, uint64_t done);

void eg_gfx_flush_at(EvergreenState *s, const char *at)
{
    EgGfx *g = s->gfx;

    if (g && g->vk && eg_vk_pending(g->vk)) {
        int64_t t0 = prof_enabled() ? qemu_clock_get_ns(QEMU_CLOCK_REALTIME) : 0;
        g->pixels += eg_vk_flush(g->vk);
        eg_vk_poll(g->vk);
        if (prof.on) {
            gpointer n = g_hash_table_lookup(prof.by_caller, at);
            g_hash_table_insert(prof.by_caller, (gpointer)at,
                                GUINT_TO_POINTER(GPOINTER_TO_UINT(n) + 1));
            prof.flush_ns += qemu_clock_get_ns(QEMU_CLOCK_REALTIME) - t0;
            prof.flushes++;
        }
    }
    if (g && g->fence_ops && g->fence_ops->len) {
        fence_run(s, UINT64_MAX);
    }
}

/*
 * The end of a ring kick starts the queued draws on the GPU but does not
 * wait for them: RPTR only frees ring space, and the driver learns of
 * finished work from fences (eg_gfx_fence: written when the batch before
 * them completes) and register reads (which flush).  When the ring goes
 * idle, a timer starts the draws still recorded.
 */
/* a ring kick submits the batch once it has this many draws (fewer: the
 * per-batch cost on the GPU is larger than the overlap gained) */
#define EG_VK_SUBMIT_DRAWS 256

/*
 * Fences (EOP, MEM_WRITE, scratch write-back, the ring interrupt) do not
 * wait for the GPU.  Each one waits in fence_ops, tagged with the last
 * batch submitted before it, until that batch completes: the guest sees
 * the fence only after the rendering, and the vCPU goes on meanwhile.
 * The ops run in order: a fence never overtakes an earlier one.
 */
typedef struct EgFenceOp {
    uint64_t seq, addr;
    uint32_t data[2];
    int len, irq;
    int zslot;                          /* >= 0: a ZPASS_DONE result (below) */
    uint64_t zcpu;                      /* its CPU-path pixels */
    int64_t queued;                     /* EG_PROFILE */
} EgFenceOp;

static void fence_queue(EvergreenState *s, EgFenceOp *op);


#define EG_POLL_NS 200000               /* fence and dirty-range polling */

static void fence_apply(EvergreenState *s, const EgFenceOp *op)
{
    if (op->zslot >= 0) {
        /* ZPASS_DONE: the pixel count so far (begin/end pairs) */
        EgGfx *g = s->gfx;
        uint64_t v;
        g->gpu_pixels += eg_vk_zpass_take(g->vk, op->zslot);
        g->zslot_busy[op->zslot] = 0;
        v = cpu_to_le64((op->zcpu + g->gpu_pixels) | (1ull << 63));
        eg_gpu_write(s, op->addr, &v, 8);
        return;
    }
    if (op->len) {
        eg_gpu_write(s, op->addr, op->data, op->len);
    }
    if (op->irq >= 0) {
        eg_ih_push(s, op->irq, 0);
    }
}

/* run the ops whose batch has completed (all of them: done = UINT64_MAX) */
static void fence_run(EvergreenState *s, uint64_t done)
{
    GArray *q = s->gfx->fence_ops;
    guint n = 0;

    while (n < q->len && g_array_index(q, EgFenceOp, n).seq <= done) {
        EgFenceOp *op = &g_array_index(q, EgFenceOp, n);
        if (op->queued) {
            int64_t d = qemu_clock_get_ns(QEMU_CLOCK_REALTIME) - op->queued;
            prof_fence_ns += d;
            prof_fence_max = MAX(prof_fence_max, d);
        }
        fence_apply(s, op);
        n++;
    }
    if (n) {
        g_array_remove_range(q, 0, n);
    }
}

void eg_gfx_poll(EvergreenState *s)
{
    EgGfx *g = s->gfx;

    if (!g || !g->vk) {
        return;
    }
    if (s->cp_unlocked) {               /* the CP is inside the backend */
        timer_mod(g->vk_poll, qemu_clock_get_ns(QEMU_CLOCK_REALTIME) + EG_POLL_NS);
        return;
    }
    fence_run(s, eg_vk_poll(g->vk));
    if (g->fence_ops->len || eg_vk_pending(g->vk)) {
        timer_mod(g->vk_poll, qemu_clock_get_ns(QEMU_CLOCK_REALTIME) + EG_POLL_NS);
    }
}

void eg_gfx_fence(EvergreenState *s, uint64_t addr, const void *data, int len, int irq)
{
    EgFenceOp op = { .addr = addr, .len = len, .irq = irq, .zslot = -1 };

    assert(len >= 0 && len <= 8);
    memcpy(op.data, data, len);
    fence_queue(s, &op);
}

/*
 * ZPASS_DONE without waiting for the GPU: the draws before it counted
 * their pixels in the current slot; the op writes the total when their
 * batch has completed, in order with the other fences.
 */
void eg_gfx_zpass(EvergreenState *s, uint64_t addr)
{
    EgGfx *g = s->gfx;
    uint32_t slot = eg_vk_zpass_slot(g->vk), next = (slot + 1) % EVK_ZPASS_SLOTS;
    EgFenceOp op = { .addr = addr, .irq = -1, .zslot = (int)slot, .zcpu = g->pixels };

    if (g->zslot_busy[next]) {
        eg_gfx_flush(s);                /* 1024 queries in flight: drain them */
    }
    g->zslot_busy[slot] = 1;
    eg_vk_zpass_set(g->vk, next);
    fence_queue(s, &op);
}

static void fence_queue(EvergreenState *s, EgFenceOp *opp)
{
    EgGfx *g = s->gfx;
    EgFenceOp op = *opp;

    if (g && g->vk && (eg_vk_pending(g->vk) || g->fence_ops->len)) {
        eg_vk_submit(g->vk);            /* the fence follows everything queued */
        op.seq = eg_vk_submitted(g->vk);
        if (prof.on > 0) {
            op.queued = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
            prof_fences++;
            prof_fence_irqs += op.irq != 0;
        }
        g_array_append_val(g->fence_ops, op);
        eg_gfx_poll(s);
        return;
    }
    prof_fences_now++;
    fence_apply(s, &op);
}

/*
 * Between packets of a long CP run: write the fences whose batch the
 * completion thread saw complete, without waiting for the main loop.
 */
void eg_gfx_poll_done(EvergreenState *s)
{
    EgGfx *g = s->gfx;

    if (g && g->vk && g->fence_ops->len && !s->cp_unlocked &&
        eg_vk_gpu_done_seq(g->vk) >= g_array_index(g->fence_ops, EgFenceOp, 0).seq) {
        eg_gfx_poll(s);
    }
}

bool eg_gfx_busy(EvergreenState *s)
{
    EgGfx *g = s->gfx;

    if (!g || !g->vk) {
        return false;
    }
    eg_vk_submit(g->vk);
    eg_gfx_poll(s);
    return eg_vk_pending(g->vk) || g->fence_ops->len;
}

static void eg_gfx_poll_cb(void *opaque)
{
    eg_gfx_poll(opaque);
}

/* on the backend's completion thread: run the fences in the main loop now */
static void eg_gfx_done_notify(void *opaque)
{
    EvergreenState *s = opaque;

    qemu_bh_schedule(s->gfx->vk_done_bh);
}

/* the ring went idle with draws still recorded: start them */
static void eg_gfx_idle_flush(void *opaque)
{
    EvergreenState *s = opaque;

    if (s->cp_unlocked) {
        timer_mod(s->gfx->vk_idle_flush, qemu_clock_get_ms(QEMU_CLOCK_REALTIME) + 1);
        return;
    }
    if (s->gfx && s->gfx->vk) {
        eg_vk_submit(s->gfx->vk);
        eg_gfx_poll(s);
    }
}

bool eg_gfx_pending(EvergreenState *s)
{
    return s->gfx && s->gfx->vk && eg_vk_pending(s->gfx->vk);
}

void eg_gfx_kick_done(EvergreenState *s)
{
    EgGfx *g = s->gfx;

    static int kick_flush = -1;

    if (kick_flush < 0) {
        kick_flush = getenv("EG_KICK_FLUSH") != NULL;     /* debug: wait at each kick */
    }
    if (kick_flush) {
        eg_gfx_flush(s);
        return;
    }
    if (g && g->vk && eg_vk_pending(g->vk)) {
        static int submit_draws = -1;
        if (submit_draws < 0) {
            const char *e = getenv("EG_VK_SUBMIT_DRAWS");
            submit_draws = e ? atoi(e) : EG_VK_SUBMIT_DRAWS;
        }
        if (eg_vk_batch_draws(g->vk) >= (uint32_t)submit_draws) {
            eg_vk_submit(g->vk);        /* the GPU runs it while the CPU goes on */
        }
        timer_mod(g->vk_idle_flush, qemu_clock_get_ms(QEMU_CLOCK_REALTIME) + 2);
    }
}

void eg_gfx_new_cp_run(EvergreenState *s)
{
    eg_gfx_mirror_collect(s);
    if (prof_enabled()) {
        prof_dev = s;
        prof.kicks++;
        prof_report();
    }
    if (s->gfx && s->gfx->vk) {
        eg_vk_new_cp_run(s->gfx->vk);
    }
}

static void eg_vk_dirty_cb(void *opaque, uint64_t off, uint64_t len)
{
    EvergreenState *s = opaque;

    /*
     * With the mirror, the dirty log means "the guest CPU wrote it" (to
     * upload); the display refreshes in full anyway.  Without it, the GPU
     * wrote host VRAM: log it.
     */
    if (off < s->vram_size && !eg_vk_mirrored(s->gfx->vk)) {
        memory_region_set_dirty(&s->vram, off, MIN(len, s->vram_size - off));
    }
}

/*
 * The VRAM mirror (device-local, eg_vk.c) must see every CPU write to
 * VRAM: the guest's through BAR0 and the device's own (CPU draws, DMA,
 * HDP, MEM_WRITE ...).  The VGA dirty log has them all; the display does
 * not use it.  Queue the dirty pages for upload before the GPU uses them.
 */
#define EG_MIRROR_PAGE_BITS 12

void eg_gfx_mirror_collect(EvergreenState *s)
{
    EgGfx *g = s->gfx;
    DirtyBitmapSnapshot *snap;
    uint64_t pg, chunk, npages = s->vram_size >> EG_MIRROR_PAGE_BITS, run = 0, run_len = 0;

    if (!g || !g->vk || !eg_vk_mirrored(g->vk)) {
        return;
    }
    snap = memory_region_snapshot_and_clear_dirty(&s->vram, 0, s->vram_size, DIRTY_MEMORY_VGA);
    for (chunk = 0; chunk < npages; chunk += 64) {
        uint64_t end = MIN(chunk + 64, npages);
        if (!memory_region_snapshot_get_dirty(&s->vram, snap, chunk << EG_MIRROR_PAGE_BITS,
                                              (end - chunk) << EG_MIRROR_PAGE_BITS)) {
            continue;
        }
        for (pg = chunk; pg < end; pg++) {
            if (!memory_region_snapshot_get_dirty(&s->vram, snap, pg << EG_MIRROR_PAGE_BITS,
                                                  (1ull << EG_MIRROR_PAGE_BITS))) {
                continue;
            }
            if (run_len && run + run_len == pg) {
                run_len++;
                continue;
            }
            if (run_len) {
                eg_vk_upload(g->vk, run << EG_MIRROR_PAGE_BITS, run_len << EG_MIRROR_PAGE_BITS);
            }
            run = pg;
            run_len = 1;
        }
    }
    if (run_len) {
        eg_vk_upload(g->vk, run << EG_MIRROR_PAGE_BITS, run_len << EG_MIRROR_PAGE_BITS);
    }
    g_free(snap);
}

void eg_gfx_vk_init(EvergreenState *s)
{
    EgGfx *g = s->gfx;
    MachineState *ms = MACHINE(qdev_get_machine());
    char err[512];

    g->ps_cache = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, ps_entry_free);
    if (!s->vulkan) {
        return;
    }
    g->vk = eg_vk_create(s->vram_ptr, s->vram_size, eg_vk_dirty_cb, s, err, sizeof(err));
    if (!g->vk) {
        warn_report("radeon-hd5770: Vulkan backend unavailable, 3D stays on the CPU: %s", err);
        return;
    }
    g->gart_gen = 1;
    g->vk_idle_flush = timer_new_ms(QEMU_CLOCK_REALTIME, eg_gfx_idle_flush, s);
    eg_vk_set_wait_fn(g->vk, eg_cp_wait_cb);
    g->vk_poll = timer_new_ns(QEMU_CLOCK_REALTIME, eg_gfx_poll_cb, s);
    g->vk_done_bh = qemu_bh_new(eg_gfx_poll_cb, s);
    eg_vk_set_notify_fn(g->vk, eg_gfx_done_notify, s);
    g->fence_ops = g_array_new(false, false, sizeof(EgFenceOp));
    {
        /* persistent SPIR-V cache: EG_VK_CACHE_DIR, "" disables */
        const char *env = getenv("EG_VK_CACHE_DIR");
        g_autofree char *dir = env ? g_strdup(env)
                                   : g_build_filename(g_get_user_cache_dir(), "rv530",
                                                      "evergreen", NULL);
        if (*dir && g_mkdir_with_parents(dir, 0700) == 0) {
            int n = eg_vk_set_cache_dir(g->vk, dir);
            if (n) {
                info_report("radeon-hd5770: prewarming %d cached pixel shaders", n);
            }
        }
    }
    if (eg_vk_mirrored(g->vk)) {
        eg_vk_upload_all_now(g->vk);
    }
    if (ms->ram && memory_region_is_ram(ms->ram) &&
        eg_vk_set_sysram(g->vk, memory_region_get_ram_ptr(ms->ram),
                         memory_region_size(ms->ram), err, sizeof(err))) {
        g->vk_sysram = ms->ram;
    } else {
        warn_report("radeon-hd5770: GART surfaces stay on the CPU path: %s", err);
    }
}

void eg_gfx_vk_fini(EvergreenState *s)
{
    EgGfx *g = s->gfx;

    if (g && g->vk_idle_flush) {
        timer_free(g->vk_idle_flush);
        g->vk_idle_flush = NULL;
    }
    if (g && g->vk_poll) {
        timer_free(g->vk_poll);
        g->vk_poll = NULL;
    }
    if (g && g->fence_ops) {
        g_array_free(g->fence_ops, true);
        g->fence_ops = NULL;
    }
    if (g && g->vk) {
        eg_vk_destroy(g->vk);
        g->vk = NULL;
    }
    if (g && g->ps_cache) {
        g_hash_table_destroy(g->ps_cache);
        g->ps_cache = NULL;
        g_free(g->pgm_fast);
        g->pgm_fast = NULL;
    }
    if (g) {
        g_free(g->gart_gen_of);
        g_free(g->gart_pte_of);
        g_free(g->vk_tex);
        g->vk_tex = NULL;
        g->gart_gen_of = NULL;
        g->gart_pte_of = NULL;
        g->gart_gen_pages = 0;
    }
}

/* ------------------------------------------------------------------ */
/* Draws                                                               */
/* ------------------------------------------------------------------ */

enum {
    PT_POINTLIST = 1, PT_LINELIST = 2, PT_LINESTRIP = 3, PT_TRILIST = 4, PT_TRIFAN = 5,
    PT_TRISTRIP = 6, PT_RECTLIST = 0x11, PT_LINELOOP = 0x12, PT_QUADLIST = 0x13,
    PT_QUADSTRIP = 0x14, PT_POLYGON = 0x15,
};

typedef struct IndexSrc {
    uint64_t addr;                      /* 0: auto index */
    const uint32_t *immd;               /* DRAW_INDEX_IMMD */
    bool is32;
    uint32_t count;
} IndexSrc;

static uint32_t get_index(DrawCtx *dc, const IndexSrc *src, uint32_t i)
{
    EvergreenState *s = dc->s;
    uint32_t v;

    if (src->immd) {
        v = src->is32 ? src->immd[i] : (src->immd[i / 2] >> (16 * (i & 1))) & 0xffff;
    } else if (src->addr) {
        uint8_t b[4] = { 0 };
        eg_gpu_read(s, src->addr + (uint64_t)i * (src->is32 ? 4 : 2), b, src->is32 ? 4 : 2);
        v = src->is32 ? ldl_le_p(b) : lduw_le_p(b);
    } else {
        return i + CTX(s, 0x28408);             /* VGT_INDX_OFFSET */
    }
    return v + CTX(s, 0x28408);
}

static bool is_restart(DrawCtx *dc, const IndexSrc *src, uint32_t raw)
{
    EvergreenState *s = dc->s;

    if (!src->addr && !src->immd) {
        return false;
    }
    if (!(CTX(s, 0x28a94) & 1)) {
        return false;
    }
    return raw - CTX(s, 0x28408) == CTX(s, 0x2840c);
}

static void ps_input_setup(DrawCtx *dc)
{
    EvergreenState *s = dc->s;
    uint8_t vs_sem[EG_NUM_PARAMS];
    int n, p, nvs = ((CTX(s, 0x286c4) >> 1) & 0x1f) + 1;

    for (p = 0; p < EG_NUM_PARAMS; p++) {
        vs_sem[p] = (CTX(s, 0x2861c + 4 * (p / 4)) >> (8 * (p % 4))) & 0xff;
    }
    dc->ninterp = MIN(CTX(s, 0x286cc) & 0x3f, 32);
    for (n = 0; n < dc->ninterp; n++) {
        uint32_t c = CTX(s, 0x28644 + 4 * n);
        dc->interp_cntl[n] = c;
        dc->interp_src[n] = -1;
        for (p = 0; p < MIN(nvs, EG_NUM_PARAMS); p++) {
            if (vs_sem[p] == (c & 0xff)) {
                dc->interp_src[n] = p;
                break;
            }
        }
    }
}

/*
 * EG_VK_VERIFY=1: every GPU draw is also run on the CPU path from the same
 * VRAM contents, and the first bytes that differ are logged (debugging
 * the backend; slow).  The CPU result is kept.
 */
typedef struct VkVerify {
    VkPrep *vp;                         /* for the mismatch dump */
    uint32_t *ps_code, ps_ndw;
    int n;
    uint64_t off[10], len[10];
    uint8_t *before[10], *gpu[10];
    uint64_t hash;
    GByteArray *verts;                  /* the submitted EvkVertex list */
} VkVerify;

static int vk_verify_on = -1;

static void prof_mark(int k)
{
    int64_t now;

    if (!prof_on_cached) {
        return;
    }
    now = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
    if (k >= 0) {
        prof_phase_ns[k] += now - prof_last;
    }
    prof_last = now;
}

static void vk_verify_free(VkVerify *vv)
{
    for (int k = 0; k < vv->n; k++) {
        g_free(vv->before[k]);
    }
    g_free(vv->vp);
    g_free(vv->ps_code);
    if (vv->verts) {
        g_byte_array_free(vv->verts, true);
    }
    memset(vv, 0, sizeof(*vv));
}

/* ------------------------------------------------------------------ */
/* The vertex shader on the GPU                                        */
/* ------------------------------------------------------------------ */

/*
 * Triangle draws can run their vertex shader on the host GPU too: the VS
 * and its fetch shader translate to GLSL (evk_vs_to_glsl), the CPU only
 * turns the indices into triangles (provoking vertex last), and the host
 * does the window transform, clipping and culling as clip_tri() and
 * raster_tri() do.  Rect lists, lines and points stay on the CPU.
 * EG_NO_GPU_VS=1 turns it off.
 */
static EgPsEntry *vk_vs(DrawCtx *dc, uint64_t *hash)
{
    EgGfx *g = dc->s->gfx;
    uint32_t key[3] = { 0x56534753, evk_ps_build_id(), dc->have_fs };
    const EgProgram *fs = dc->have_fs ? &dc->fs : NULL;
    uint64_t h;
    EgPsEntry *e = pgm_fast_find(g, 0x5653, key, 3, &dc->vs, fs, &h);

    if (e) {
        *hash = h;
        return e;
    }
    e = g_new0(EgPsEntry, 1);
    dc->vs.used = dc->fs.used = 0;
    e->glsl = evk_vs_to_glsl(ps_fetch, &dc->vs, dc->have_fs ? ps_fetch : NULL, &dc->fs,
                             &e->info, e->why, sizeof(e->why));
    e->used[0] = dc->vs.used;
    e->used[1] = dc->fs.used;
    e = pgm_cache_add(g, key, 3, &dc->vs, fs, e, &h);
    if (!e->glsl) {
        trace_eg_vk_fallback(e->why);
    }
    pgm_fast_note(g, 0x5653, key, 3, &dc->vs, fs, e, h);
    *hash = h;
    return e;
}

static bool vk_gpu_vs_ok(DrawCtx *dc)
{
    EvergreenState *s = dc->s;
    uint32_t cl = CTX(s, 0x28810), mode = CTX(s, 0x28814);
    static int off = -1;

    if (off < 0) {
        off = getenv("EG_NO_GPU_VS") != NULL;
    }
    if (off) {
        return false;
    }
    switch (dc->prim) {
    case PT_TRILIST: case PT_TRISTRIP: case PT_TRIFAN: case PT_POLYGON:
    case PT_QUADSTRIP:
        break;
    case PT_QUADLIST:
        /* the provoking vertex of a quad is in both triangles only first */
        if (vk_flatmask(dc) && dc->provoking_last) {
            return false;
        }
        break;
    default:
        return false;
    }
    if (((cl >> 26) & 1) != ((cl >> 27) & 1)) {
        return false;                   /* only one of near and far clips */
    }
    if ((mode >> 3) & 3) {
        return false;                   /* polygon mode (lines, points) */
    }
    if (CTX(s, 0x28a40) & 7) {
        return false;                   /* VGT_GS_MODE: GS or stream out */
    }
    return true;
}

/* the VS state of the draw: constants, vertex buffers, the window transform */
static bool vk_prepare_vs(DrawCtx *dc, VkPrep *vp, const EgPsEntry *ve, char *why,
                          size_t whylen)
{
    EvergreenState *s = dc->s;
    EgGfx *g = s->gfx;
    EvkDraw *d = &vp->draw;
    EgVkDrawReq *r = &vp->req;
    uint32_t mode = CTX(s, 0x28814);
    int i;

    for (i = 0; i < ve->info.nkc; i++) {
        uint32_t bank = ve->info.kc_bank[i], km = ve->info.kc_mode[i];
        uint32_t breg = CTX(s, 0x28980 + 4 * (bank & 15));
        if (!km) {
            continue;
        }
        if (!breg) {
            eg_kcache_cfile(s, false, ve->info.kc_addr[i], km == 2 ? 32 : 16, d->vs_kc[i]);
        } else {
            eg_gpu_read(s, ((uint64_t)breg << 8) + (uint64_t)ve->info.kc_addr[i] * 16 * 16,
                        d->vs_kc[i], (km == 2 ? 32 : 16) * 16);
        }
    }
    if (ve->info.uses_cfile) {
        memcpy(d->vs_cfile, &g->alu_const[256 * 4], sizeof(d->vs_cfile));
    }
    memcpy(d->vs_loopc, &g->loop_const[32], sizeof(d->vs_loopc));
    for (i = 0; i < ve->info.nvbuf; i++) {
        const uint32_t *res = g->resource[ve->info.vbuf_id[i] & (EG_NUM_RESOURCES - 1)];
        uint64_t base = res[0] | ((uint64_t)(res[2] & 0xff) << 32);
        uint32_t off;
        bool gart = false;
        memcpy(d->vs_vbuf[i], res, 32);
        if ((res[7] >> 30) == 3) {
            if (!vk_locate(s, base, (uint64_t)res[1] + 1, &off, &gart)) {
                snprintf(why, whylen, "VS vertex buffer not in VRAM or the GART");
                return false;
            }
            d->vs_vbuf[i][0] = off;
            if (r->nrd < EVK_MAX_TEX + EVK_MAX_VBUF + EVK_MAX_VS_VBUF) {
                r->rd_off[r->nrd] = (gart ? EVK_GART_SPACE : 0) + off;
                r->rd_len[r->nrd] = (uint64_t)res[1] + 1;
                r->nrd++;
            }
        }
        d->vs_vbuf[i][6] = gart;
    }
    d->misc[EVK_M_GPU_VS] = 1;
    d->misc[EVK_M_VS_BOOLC] = g->bool_const[1];
    d->misc[EVK_M_VTE] = CTX(s, 0x28818);
    d->misc[EVK_M_CLIP] = CTX(s, 0x28810);
    for (i = 0; i < 6; i++) {
        d->misc[EVK_M_VPORT + i] = CTX(s, 0x2843c + 4 * i);
    }
    if ((mode >> 16) & 1) {                     /* VTX_WINDOW_OFFSET_ENABLE */
        uint32_t wo = CTX(s, 0x28200);
        d->misc[EVK_M_WIN_OFS] = (uint32_t)(int32_t)(int16_t)(wo & 0xffff);
        d->misc[EVK_M_WIN_OFS + 1] = (uint32_t)(int32_t)(int16_t)(wo >> 16);
    }
    d->misc[EVK_M_NINTERP] = MIN(dc->ninterp, EVK_MAX_PARAMS);
    for (i = 0; i < dc->ninterp && i < EVK_MAX_PARAMS; i++) {
        int src = dc->interp_src[i];
        d->misc[EVK_M_ISRC + i] = (src < 0 || src > 31 ? 0xffu : (uint32_t)src) |
                                  ((dc->interp_cntl[i] >> 8) & 3) << 8;
    }
    r->vs_glsl = ve->glsl;
    r->ninst = MAX(g->num_instances, 1u);
    r->cull_front = dc->cull_front;
    r->cull_back = dc->cull_back;
    r->front_ccw = dc->front_ccw;
    return true;
}

/* append triangle a, b, c (in that winding) with provoking vertex p last */
static void tri_out(GArray *o, uint32_t a, uint32_t b, uint32_t c, uint32_t p)
{
    uint32_t t[3];

    if (p == a && p != c) {
        t[0] = b; t[1] = c; t[2] = a;
    } else if (p == b && p != c) {
        t[0] = c; t[1] = a; t[2] = b;
    } else {
        t[0] = a; t[1] = b; t[2] = c;
    }
    g_array_append_vals(o, t, 3);
}

/* the draw's triangles as vertex indices, as the CPU path assembles them */
static GArray *vk_vs_triangles(DrawCtx *dc, const IndexSrc *src)
{
    EvergreenState *s = dc->s;
    uint32_t n = src->count, i, ofs = CTX(s, 0x28408);
    bool prim_restart = (src->addr || src->immd) && (CTX(s, 0x28a94) & 1);
    uint32_t reset = CTX(s, 0x2840c);
    uint32_t *v = g_new(uint32_t, n);
    bool *rs = g_new0(bool, n);
    GArray *o = g_array_sized_new(false, false, 4, n * 2);
    bool last = dc->provoking_last;

    if (src->immd) {
        for (i = 0; i < n; i++) {
            v[i] = src->is32 ? src->immd[i] : (src->immd[i / 2] >> (16 * (i & 1))) & 0xffff;
        }
    } else if (src->addr) {
        uint32_t sz = src->is32 ? 4 : 2;
        g_autofree uint8_t *b = g_malloc((size_t)n * sz);
        eg_gpu_read(s, src->addr, b, (size_t)n * sz);
        for (i = 0; i < n; i++) {
            v[i] = src->is32 ? ldl_le_p(b + 4 * i) : lduw_le_p(b + 2 * i);
        }
    } else {
        for (i = 0; i < n; i++) {
            v[i] = i;
        }
    }
    for (i = 0; i < n; i++) {
        rs[i] = prim_restart && v[i] == reset;
        v[i] += ofs;
    }
#define T(a, b, c, p) tri_out(o, v[a], v[b], v[c], v[p])
    switch (dc->prim) {
    case PT_TRILIST:
        for (i = 0; i + 2 < n; i += 3) {
            T(i, i + 1, i + 2, last ? i + 2 : i);
        }
        break;
    case PT_TRISTRIP: {
        uint32_t start = 0;
        for (i = 0; i + 2 < n; i++) {
            if (rs[i] || rs[i + 1] || rs[i + 2]) {
                start = i + 1;
                continue;
            }
            if ((i - start) & 1) {
                T(i + 1, i, i + 2, last ? i + 2 : i);
            } else {
                T(i, i + 1, i + 2, last ? i + 2 : i);
            }
        }
        break;
    }
    case PT_TRIFAN:
    case PT_POLYGON:
        for (i = 1; i + 1 < n; i++) {
            T(0, i, i + 1, last ? i + 1 : 0);
        }
        break;
    case PT_QUADLIST:
        for (i = 0; i + 3 < n; i += 4) {
            T(i, i + 1, i + 2, i);              /* provoking first (vk_gpu_vs_ok) */
            T(i, i + 2, i + 3, i);
        }
        break;
    case PT_QUADSTRIP:
        for (i = 0; i + 3 < n; i += 2) {
            T(i, i + 1, i + 3, i + 3);
            T(i, i + 3, i + 2, i + 3);
        }
        break;
    default:
        break;
    }
#undef T
    g_free(v);
    g_free(rs);
    return o;
}

/* the CPU rasterizer wrote these surfaces through host pointers: log them
 * (the GPU's VRAM mirror uploads what the dirty log reports) */
static void cpu_draw_mark_dirty(DrawCtx *dc, FbState *fb)
{
    EvergreenState *s = dc->s;
    uint8_t *p;
    int i;

#define MARK(addr, tl)                                                        \
    do {                                                                      \
        uint64_t len_ = eg_tile_size(tl);                                     \
        if ((addr) && (p = eg_vram_ptr(s, (addr), len_))) {                   \
            memory_region_set_dirty(&s->vram, p - s->vram_ptr, len_);         \
        }                                                                     \
    } while (0)
    for (i = 0; i < 8; i++) {
        if (fb->cb[i].enabled) {
            MARK(fb->cb[i].base, &fb->cb[i].tile);
        }
    }
    if (fb->z_valid && fb->z_write) {
        MARK(fb->z_write_base, &fb->ztile);
    }
    if (fb->s_valid) {
        MARK(fb->s_write, &fb->stile);
    }
#undef MARK
}

/* false: the GPU path refused the draw and nothing was drawn */
static bool do_draw_one(EvergreenState *s, const IndexSrc *src, bool try_vk, VkVerify *vv)
{
    EgGfx *g = s->gfx;
    DrawCtx *dc = g_new0(DrawCtx, 1);
    FbState *fb = g_new0(FbState, 1);
    VkPrep *vp = NULL;
    bool ok = true, gpu_vs = false;
    char why[160];
    uint64_t vs = (uint64_t)CTX(s, 0x2885c) << 8, ps = (uint64_t)CTX(s, 0x28840) << 8;
    uint64_t fsa = (uint64_t)CTX(s, 0x288a4) << 8;
    uint32_t i, n = src->count, inst;
    int64_t t0 = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);

    dc->s = s;
    dc->prim = s->regs[0x8958 >> 2] & 0x3f;     /* VGT_PRIMITIVE_TYPE */
    g->draws++;
    prof_mark(-1);
    if (!n || !vs || !ps) {
        goto out;
    }
    eg_program_load(s, &dc->vs, vs, EG_STAGE_VS);
    eg_program_load(s, &dc->ps, ps, EG_STAGE_PS);
    dc->have_fs = fsa != 0;
    if (dc->have_fs) {
        eg_program_load(s, &dc->fs, fsa, EG_STAGE_FS);
    }
    raster_setup(dc);
    fb_setup(dc, fb);
    ps_input_setup(dc);
    prof_mark(0);
    if (try_vk) {
        vp = g_new(VkPrep, 1);
        if (vk_prepare(dc, fb, vp, why, sizeof(why))) {
            dc->vkc = true;
            if (vk_gpu_vs_ok(dc)) {
                uint64_t vh;
                EgPsEntry *ve = vk_vs(dc, &vh);
                if (ve->glsl && vk_prepare_vs(dc, vp, ve, why, sizeof(why))) {
                    gpu_vs = true;
                    vp->req.vs_hash = vh;
                } else {
                    vp->req.vs_glsl = NULL;
                    vp->draw.misc[EVK_M_GPU_VS] = 0;
                }
            }
            if (!gpu_vs) {
                dc->vk_verts = g_array_new(false, false, sizeof(EvkVertex));
            }
            if (vv) {
                int k;
                eg_gfx_flush(s);
                vv->n = 0;
                vv->vp = g_memdup2(vp, sizeof(*vp));
                vv->ps_code = g_memdup2(dc->ps.code, dc->ps.ndw * 4);
                vv->ps_ndw = dc->ps.ndw;
                vv->hash = vp->req.prog_hash;
                for (k = 0; k < vp->req.nwr && vv->n < 10; k++) {
                    uint64_t l = vp->req.wr_len[k];
                    vv->off[vv->n] = vp->wr_gpu[k];
                    vv->len[vv->n] = l;
                    vv->before[vv->n] = g_malloc(l);
                    eg_gpu_read(s, vp->wr_gpu[k], vv->before[vv->n], l);
                    vv->n++;
                }
            }
        } else {
            trace_eg_vk_fallback(why);
            ok = false;
        }
    } else {
        eg_gfx_flush(s);                /* the CPU path reads and writes VRAM */
    }
    prof_mark(1);
    dc->cache = g_hash_table_new(NULL, NULL);
    dc->vsh = g_new0(EgShader, 1);
    dc->jobs = g_ptr_array_new_with_free_func(g_free);
    trace_eg_draw(dc->prim, n, vs, ps, CTX(s, 0x28c60), CTX(s, 0x28c70));

    if (ok && gpu_vs) {
        GArray *tris = vk_vs_triangles(dc, src);
        prof_mark(2);
        prof.gpu_vs++;
        vp->req.idx = (const uint32_t *)tris->data;
        vp->req.nidx = tris->len;
        if (!eg_vk_draw(g->vk, &vp->req, why, sizeof(why))) {
            if (!strcmp(why, "vs compiling")) {
                /* the CPU VS and the GPU PS draw it until the pipeline is ready */
                gpu_vs = false;
                prof.gpu_vs--;
                vp->req.vs_glsl = NULL;
                vp->req.idx = NULL;
                vp->req.nidx = 0;
                vp->draw.misc[EVK_M_GPU_VS] = 0;
                dc->vk_verts = g_array_new(false, false, sizeof(EvkVertex));
            } else {
                if (strcmp(why, "compiling")) {
                    trace_eg_vk_fallback(why);
                }
                ok = false;
            }
        }
        g_array_free(tris, true);
        prof_mark(4);
    }
    for (inst = 0; ok && !gpu_vs && inst < MAX(g->num_instances, 1u); inst++) {
        uint32_t *slots = g_new(uint32_t, n);
        bool *restart = g_new0(bool, n);

        dc->instance = inst;
        g_hash_table_remove_all(dc->cache);
        dc->nverts = 0;
        /* shade all vertices first, then assemble */
        for (i = 0; i < n; i++) {
            uint32_t idx = get_index(dc, src, i);
            if (is_restart(dc, src, idx)) {
                restart[i] = true;
                continue;
            }
            slots[i] = vtx_get(dc, idx);
        }
        vs_flush(dc);
        prof_mark(2);
        if ((s->debug & 8) && inst == 0) {
            qemu_log("evergreen:  scissor %d,%d-%d,%d vte %08x clip %08x rule %04x ninterp %d\n",
                     dc->sc_x0, dc->sc_y0, dc->sc_x1, dc->sc_y1, CTX(s, 0x28818),
                     CTX(s, 0x28810), dc->cliprect_rule, dc->ninterp);
            qemu_log("evergreen:  db ctl %08x zinfo %08x sinfo %08x rctl %08x ovr %08x "
                     "clear %g/%u zr %08x zw %08x size %08x sx %08x cbctl %08x tmask %08x\n",
                     CTX(s, 0x28800), CTX(s, 0x28040), CTX(s, 0x28044), CTX(s, 0x28000),
                     CTX(s, 0x2800c), u2f(CTX(s, 0x2802c)), CTX(s, 0x28028),
                     CTX(s, 0x28048), CTX(s, 0x28050), CTX(s, 0x28058), CTX(s, 0x28410),
                     CTX(s, 0x28808), CTX(s, 0x28238));
            {
                /* PS constant buffer 0: its address and first constant */
                uint64_t kb = (uint64_t)CTX(s, 0x28940) << 8;
                uint32_t k0[4] = { 0 };
                eg_gpu_read(s, kb, k0, sizeof(k0));
                qemu_log("evergreen:  ps cb0 @%" PRIx64 " size %08x: %08x %08x %08x %08x\n",
                         kb, CTX(s, 0x28140), k0[0], k0[1], k0[2], k0[3]);
            }
            for (i = 0; i < MIN(n, 4u) && i < dc->nverts; i++) {
                EgVert w = dc->verts[i];
                to_window(dc, &w);
                qemu_log("evergreen:  v%u clip %g %g %g %g win %g %g %g p0 %g %g %g %g\n", i,
                         w.clip[0], w.clip[1], w.clip[2], w.clip[3], w.win[0], w.win[1],
                         w.win[2], w.param[0].f[0], w.param[0].f[1], w.param[0].f[2],
                         w.param[0].f[3]);
            }
        }

#define V(k) (&dc->verts[slots[k]])
        switch (dc->prim) {
        case PT_POINTLIST:
            for (i = 0; i < n; i++) {
                if (!restart[i]) {
                    draw_point(dc, fb, V(i));
                }
            }
            break;
        case PT_LINELIST:
            for (i = 0; i + 1 < n; i += 2) {
                draw_line(dc, fb, V(i), V(i + 1));
            }
            break;
        case PT_LINESTRIP:
        case PT_LINELOOP:
            for (i = 0; i + 1 < n; i++) {
                if (!restart[i] && !restart[i + 1]) {
                    draw_line(dc, fb, V(i), V(i + 1));
                }
            }
            if (dc->prim == PT_LINELOOP && n > 2) {
                draw_line(dc, fb, V(n - 1), V(0));
            }
            break;
        case PT_TRILIST:
            for (i = 0; i + 2 < n; i += 3) {
                clip_tri(dc, fb, V(i), V(i + 1), V(i + 2), dc->provoking_last ? V(i + 2) : V(i));
            }
            break;
        case PT_TRISTRIP: {
            uint32_t start = 0;
            for (i = 0; i + 2 < n; i++) {
                if (restart[i] || restart[i + 1] || restart[i + 2]) {
                    start = i + 1;
                    continue;
                }
                if ((i - start) & 1) {
                    clip_tri(dc, fb, V(i + 1), V(i), V(i + 2),
                             dc->provoking_last ? V(i + 2) : V(i));
                } else {
                    clip_tri(dc, fb, V(i), V(i + 1), V(i + 2),
                             dc->provoking_last ? V(i + 2) : V(i));
                }
            }
            break;
        }
        case PT_TRIFAN:
        case PT_POLYGON:
            for (i = 1; i + 1 < n; i++) {
                clip_tri(dc, fb, V(0), V(i), V(i + 1), dc->provoking_last ? V(i + 1) : V(0));
            }
            break;
        case PT_QUADLIST:
            for (i = 0; i + 3 < n; i += 4) {
                clip_tri(dc, fb, V(i), V(i + 1), V(i + 2), V(dc->provoking_last ? i + 3 : i));
                clip_tri(dc, fb, V(i), V(i + 2), V(i + 3), V(dc->provoking_last ? i + 3 : i));
            }
            break;
        case PT_QUADSTRIP:
            for (i = 0; i + 3 < n; i += 2) {
                clip_tri(dc, fb, V(i), V(i + 1), V(i + 3), V(i + 3));
                clip_tri(dc, fb, V(i), V(i + 3), V(i + 2), V(i + 3));
            }
            break;
        case PT_RECTLIST:
            for (i = 0; i + 2 < n; i += 3) {
                draw_rect(dc, fb, V(i), V(i + 1), V(i + 2));
            }
            break;
        default:
            eg_gfx_warn(s, 0x30 + dc->prim, "evergreen: primitive type 0x%x\n", dc->prim);
            break;
        }
#undef V
        flush_jobs(dc, fb);
        if (!dc->vkc) {
            cpu_draw_mark_dirty(dc, fb);
        }
        g_free(slots);
        g_free(restart);
        prof_mark(3);
    }
    if (ok && dc->vkc && !gpu_vs && !vk_submit(dc, vp, why, sizeof(why))) {
        if (strcmp(why, "compiling")) {
            trace_eg_vk_fallback(why);
        }
        ok = false;
    }
    prof_mark(4);
    if (dc->vk_verts && vv && ok) {
        vv->verts = g_byte_array_new();
        g_byte_array_append(vv->verts, (const guint8 *)dc->vk_verts->data,
                            dc->vk_verts->len * sizeof(EvkVertex));
    }
    if (dc->vk_verts) {
        g_array_free(dc->vk_verts, true);
    }
    g_free(vp);
    g->pixels += dc->pixels;
    g_hash_table_destroy(dc->cache);
    eg_tex_cache_reset(dc->vsh);
    g_free(dc->vsh);
    g_ptr_array_free(dc->jobs, true);
    g_free(dc->verts);
    g_free(dc->pend_idx);
    g_free(dc->pend_slot);
    eg_program_free(&dc->vs);
    eg_program_free(&dc->ps);
    if (dc->have_fs) {
        eg_program_free(&dc->fs);
    }
    if (s->debug & 4) {
        const uint32_t *r = g->resource[0];
        qemu_log("evergreen: draw #%" PRIu64 " prim %u n %u vs %" PRIx64 " ps %" PRIx64
                 " cb0 %08x/%08x/%08x px %" PRIu64 " %" PRId64 " us t0 %08x %ux%u p%u f%u m%u"
                 " pitch %u slice %u\n",
                 g->draws, dc->prim, n, vs, ps, CTX(s, 0x28c60), CTX(s, 0x28c70),
                 CTX(s, 0x28c74), dc->pixels,
                 (qemu_clock_get_ns(QEMU_CLOCK_REALTIME) - t0) / 1000,
                 r[2], ((r[0] >> 18) & 0x3fff) + 1, (r[1] & 0x3fff) + 1,
                 (((r[0] >> 6) & 0xfff) + 1) * 8, r[7] & 0x3f, r[1] >> 28,
                 ((CTX(s, 0x28c64) & 0x7ff) + 1) * 8, ((CTX(s, 0x28c68) & 0x3fffff) + 1) * 64);
        for (i = 1; i < 18; i++) {
            r = g->resource[i];
            if ((r[7] >> 30) == 2) {
                qemu_log("evergreen:  t%u %08x %ux%u f%u m%u\n", i, r[2],
                         ((r[0] >> 18) & 0x3fff) + 1, (r[1] & 0x3fff) + 1, r[7] & 0x3f,
                         r[1] >> 28);
            }
        }
    }
out:
    g_free(fb);
    g_free(dc);
    return ok;
}

/*
 * <base>.replay: what scripts/eg_vk_replay needs to run the draw again
 * outside QEMU: "EVKR", the EvkDraw, the request, the vertices and the
 * memory the draw reads and writes (as it was before the draw).
 */
static void vk_replay_dump(EvergreenState *s, const char *base, const VkVerify *vv)
{
    g_autofree char *fn = g_strconcat(base, ".replay", NULL);
    g_autoptr(GByteArray) b = g_byte_array_new();
    const EgVkDrawReq *r = &vv->vp->req;
    uint32_t w[8] = { 0x524b5645, sizeof(EvkDraw), sizeof(EvkVertex),
                      vv->verts ? vv->verts->len / sizeof(EvkVertex) : 0,
                      r->nrd + vv->n, 0, 0, 0 };
    int32_t sc[4] = { r->sc_x0, r->sc_y0, r->sc_x1, r->sc_y1 };
    int k;

    g_byte_array_append(b, (guint8 *)w, sizeof(w));
    g_byte_array_append(b, (guint8 *)&vv->vp->draw, sizeof(EvkDraw));
    g_byte_array_append(b, (guint8 *)sc, sizeof(sc));
    g_byte_array_append(b, (guint8 *)&r->prog_hash, 8);
    if (vv->verts) {
        g_byte_array_append(b, vv->verts->data, vv->verts->len);
    }
    for (k = 0; k < r->nrd + vv->n; k++) {
        bool wr = k >= r->nrd;
        /* offsets in the backend's space: VRAM, or EVK_GART_SPACE + GART */
        uint64_t off = wr ? r->wr_off[k - r->nrd] : r->rd_off[k];
        uint64_t len = wr ? vv->len[k - r->nrd] : r->rd_len[k];
        uint64_t hdr[3] = { off, len, wr };
        g_autofree uint8_t *d = g_malloc0(len);
        if (wr) {
            memcpy(d, vv->before[k - r->nrd], len);
        } else if (off + len <= s->vram_size) {
            memcpy(d, s->vram_ptr + off, len);
        }
        g_byte_array_append(b, (guint8 *)hdr, sizeof(hdr));
        g_byte_array_append(b, d, len);
    }
    g_file_set_contents(fn, (char *)b->data, b->len, NULL);
}

static void vk_verify(EvergreenState *s, const IndexSrc *src, VkVerify *vv)
{
    EgGfx *g = s->gfx;
    int k;

    eg_gfx_flush(s);
    for (k = 0; k < vv->n; k++) {
        vv->gpu[k] = g_malloc(vv->len[k]);
        eg_gpu_read(s, vv->off[k], vv->gpu[k], vv->len[k]);
        eg_gpu_write(s, vv->off[k], vv->before[k], vv->len[k]);
    }
    g->draws--;
    do_draw_one(s, src, false, NULL);
    for (k = 0; k < vv->n; k++) {
        g_autofree uint8_t *c = g_malloc(vv->len[k]);
        const uint8_t *q = vv->gpu[k];
        uint64_t i, bad = 0, first = 0;
        eg_gpu_read(s, vv->off[k], c, vv->len[k]);
        for (i = 0; i < vv->len[k]; i++) {
            if (c[i] != q[i]) {
                if (!bad++) {
                    first = i;
                }
            }
        }
        if (bad && getenv("EG_VK_DUMP_DIR") && vv->vp) {
            /* once per program: microcode, GLSL and the draw's state */
            g_autofree char *base = g_strdup_printf("%s/ps-%016" PRIx64, getenv("EG_VK_DUMP_DIR"),
                                                    vv->hash);
            g_autofree char *bin = g_strconcat(base, ".bin", NULL);
            if (!g_file_test(bin, G_FILE_TEST_EXISTS)) {
                g_autofree char *gl = g_strconcat(base, ".glsl", NULL);
                g_autofree char *st = g_strconcat(base, ".txt", NULL);
                g_autoptr(GString) t = g_string_new(NULL);
                const EvkDraw *d = &vv->vp->draw;
                g_file_set_contents(bin, (char *)vv->ps_code, vv->ps_ndw * 4, NULL);
                g_file_set_contents(gl, vv->vp->ps->glsl, -1, NULL);
                for (int j = 0; j < vv->vp->ps->info.ntex; j++) {
                    const EvkTex *x = &d->tex[j];
                    g_string_append_printf(t, "tex%d rid %u flags %x fmt %u numf %u comp %x dsel %03x"
                                           " dim %u %ux%ux%u lv %u-%u mode %u pitch %u addr %08x\n",
                                           j, vv->vp->ps->info.tex_rid[j], x->flags, x->fmt, x->numf,
                                           x->comp, x->dsel, x->dim, x->width, x->height, x->depth,
                                           x->base_level, x->last_level, x->tile.mode, x->tile.pitch,
                                           x->level_addr[0]);
                }
                for (int j = 0; j < vv->vp->ps->info.nsmp; j++) {
                    g_string_append_printf(t, "smp%d %08x %08x %08x\n", j, d->smp[j][0],
                                           d->smp[j][1], d->smp[j][2]);
                }
                for (int j = 0; j < 8; j++) {
                    const EvkSurf *cs = &d->cb[j];
                    if (cs->flags & EVK_SF_ENABLED) {
                        g_string_append_printf(t, "cb%d flags %x fmt %u number %u chmap %x bpp %u "
                                               "blend %08x mask %x %ux%u mode %u\n", j, cs->flags,
                                               cs->fmt, cs->number, cs->chmap, cs->bpp, cs->blend,
                                               cs->mask, cs->width, cs->height, cs->tile.mode);
                    }
                }
                g_string_append_printf(t, "rop %02x cbmode %u baryc %08x in0 %08x in1 %08x "
                                       "pixcenter %08x\n", d->misc[EVK_M_ROP], d->misc[EVK_M_CB_MODE],
                                       d->misc[EVK_M_BARYC], d->misc[EVK_M_IN0], d->misc[EVK_M_IN1],
                                       d->misc[EVK_M_PIX_CENTER]);
                g_file_set_contents(st, t->str, -1, NULL);
                vk_replay_dump(s, base, vv);
                {
                    /* the surface as the CPU and the GPU left it (raw, tiled) */
                    g_autofree char *cf = g_strdup_printf("%s.s%d.cpu", base, k);
                    g_autofree char *gf = g_strdup_printf("%s.s%d.gpu", base, k);
                    g_file_set_contents(cf, (char *)c, vv->len[k], NULL);
                    g_file_set_contents(gf, (char *)q, vv->len[k], NULL);
                }
                if (vv->vp->wr_tile[k].bpp == 4) {
                    /* detiled, CPU left and GPU right, as a PPM */
                    const EgTiling *tl = &vv->vp->wr_tile[k];
                    uint32_t w = tl->pitch, h = tl->height, x, y;
                    g_autofree char *pf = g_strdup_printf("%s.s%d.ppm", base, k);
                    g_autofree uint8_t *img = g_malloc0((size_t)w * 2 * h * 3);
                    for (y = 0; y < h; y++) {
                        for (x = 0; x < w; x++) {
                            uint64_t o = eg_tile_offset(tl, x, y);
                            if (o + 4 > vv->len[k]) {
                                continue;
                            }
                            for (int side = 0; side < 2; side++) {
                                const uint8_t *px = (side ? q : c) + o;
                                uint8_t *dp = img + ((size_t)y * w * 2 + x + side * w) * 3;
                                dp[0] = px[2];
                                dp[1] = px[1];
                                dp[2] = px[0];
                            }
                        }
                    }
                    {
                        g_autofree char *hdr = g_strdup_printf("P6\n%u %u\n255\n", w * 2, h);
                        g_autoptr(GString) gs = g_string_new(hdr);
                        g_string_append_len(gs, (char *)img, (gssize)w * 2 * h * 3);
                        g_file_set_contents(pf, gs->str, gs->len, NULL);
                    }
                }
            }
        }
        if (bad) {
            qemu_log("evergreen: vk-verify draw #%" PRIu64 " ps %016" PRIx64 " surface %d @%08"
                     PRIx64 ": %" PRIu64 " of %" PRIu64 " bytes differ, first +%" PRIu64
                     " cpu %02x%02x%02x%02x gpu %02x%02x%02x%02x\n", g->draws, vv->hash, k,
                     vv->off[k], bad, vv->len[k], first, c[first], c[first + 1], c[first + 2],
                     c[first + 3], q[first], q[first + 1], q[first + 2], q[first + 3]);
        }
        g_free(vv->gpu[k]);
        vv->gpu[k] = NULL;
    }
    vk_verify_free(vv);
}

static void do_draw(EvergreenState *s, const IndexSrc *src)
{
    EgGfx *g = s->gfx;
    VkVerify vv = { 0 };
    static int flush_each = -1;
    int64_t t0 = prof_enabled() ? qemu_clock_get_ns(QEMU_CLOCK_REALTIME) : 0;

    if (flush_each < 0) {
        flush_each = getenv("EG_VK_FLUSH_DRAW") != NULL;
    }
    if (flush_each) {
        eg_gfx_flush(s);                /* debug: no CPU read sees a pending GPU write */
    }
    if (vk_verify_on < 0) {
        vk_verify_on = getenv("EG_VK_VERIFY") != NULL;
    }
    if (!do_draw_one(s, src, g->vk != NULL, vk_verify_on ? &vv : NULL)) {
        vk_verify_free(&vv);
        g->vk_fallbacks++;
        g->draws--;                     /* counted again */
        do_draw_one(s, src, false, NULL);
    } else if (vv.n) {
        vk_verify(s, src, &vv);
    } else {
        vk_verify_free(&vv);
    }
    if (prof.on) {
        uint64_t d, b, p, br;
        eg_vk_stats(g->vk, &d, &b, &p, &br);
        prof.draws++;
        prof.cpu_draws += d == prof.last_vk;
        prof.barriers += br - prof.last_barriers;
        prof.last_vk = d;
        prof.last_barriers = br;
        prof.draw_ns += qemu_clock_get_ns(QEMU_CLOCK_REALTIME) - t0;
    }
}

/* ------------------------------------------------------------------ */
/* Packets                                                             */
/* ------------------------------------------------------------------ */

#define PKT3_INDEX_BUFFER_SIZE  0x13
#define PKT3_INDEX_BASE         0x26
#define PKT3_DRAW_INDEX_2       0x27
#define PKT3_CONTEXT_CONTROL    0x28
#define PKT3_INDEX_TYPE         0x2a
#define PKT3_DRAW_INDEX         0x2b
#define PKT3_DRAW_INDEX_AUTO    0x2d
#define PKT3_DRAW_INDEX_IMMD    0x2e
#define PKT3_NUM_INSTANCES      0x2f
#define PKT3_DRAW_INDEX_OFFSET_2 0x35
#define PKT3_COPY_DW            0x3b
#define PKT3_COND_WRITE         0x45
#define PKT3_EVENT_WRITE        0x46
#define PKT3_SET_ALU_CONST      0x6a
#define PKT3_SET_BOOL_CONST     0x6b
#define PKT3_SET_LOOP_CONST     0x6c
#define PKT3_SET_RESOURCE       0x6d
#define PKT3_SET_SAMPLER        0x6e
#define PKT3_SET_CTL_CONST      0x6f

bool eg_gfx_packet(EvergreenState *s, uint32_t op, const uint32_t *d, uint32_t n)
{
    EgGfx *g = s->gfx;
    IndexSrc src = { 0 };

    switch (op) {
    case 0x60: case 0x61: case 0x62: case 0x63:
    case 0x64: case 0x65: case 0x66: case 0x67:
        if (n >= 2) {
            load_class(s, op - 0x60, d, n);
        }
        return true;
    case PKT3_CONTEXT_CONTROL:
        if (n >= 2) {
            g->load_ena = d[0];
            g->shadow_ena = d[1];
        }
        return true;
    case PKT3_SET_ALU_CONST:
        set_class(s, SH_ALU, d, n);
        return true;
    case PKT3_SET_BOOL_CONST:
        set_class(s, SH_BOOL, d, n);
        return true;
    case PKT3_SET_LOOP_CONST:
        set_class(s, SH_LOOP, d, n);
        return true;
    case PKT3_SET_RESOURCE:
        set_class(s, SH_RESOURCE, d, n);
        return true;
    case PKT3_SET_SAMPLER:
        set_class(s, SH_SAMPLER, d, n);
        return true;
    case PKT3_SET_CTL_CONST:
        set_class(s, SH_CTL, d, n);
        return true;
    case PKT3_INDEX_TYPE:
        g->index_type = n ? d[0] & 3 : 0;
        return true;
    case PKT3_NUM_INSTANCES:
        g->num_instances = n ? MAX(d[0], 1u) : 1;
        return true;
    case PKT3_INDEX_BASE:
        if (n >= 2) {
            g->index_base = ((uint64_t)(d[1] & 0xff) << 32) | (d[0] & ~1u);
        }
        return true;
    case PKT3_INDEX_BUFFER_SIZE:
        g->index_size = n ? d[0] : 0;
        return true;
    case PKT3_DRAW_INDEX_AUTO:
        if (n >= 1) {
            src.count = d[0];
            do_draw(s, &src);
        }
        return true;
    case PKT3_DRAW_INDEX_2:
        if (n >= 4) {
            src.addr = ((uint64_t)(d[2] & 0xff) << 32) | d[1];
            src.count = d[3];
            src.is32 = g->index_type & 1;
            do_draw(s, &src);
        }
        return true;
    case PKT3_DRAW_INDEX:
        if (n >= 3) {
            src.addr = ((uint64_t)(d[1] & 0xff) << 32) | d[0];
            src.count = d[2];
            src.is32 = g->index_type & 1;
            do_draw(s, &src);
        }
        return true;
    case PKT3_DRAW_INDEX_OFFSET_2:
        if (n >= 3) {
            src.addr = g->index_base + (uint64_t)d[1] * ((g->index_type & 1) ? 4 : 2);
            src.count = d[2];
            src.is32 = g->index_type & 1;
            do_draw(s, &src);
        }
        return true;
    case PKT3_DRAW_INDEX_IMMD:
        if (n >= 2) {
            src.count = d[0];
            src.immd = d + 2;
            src.is32 = g->index_type & 1;
            if (src.is32 ? n - 2 < src.count : (n - 2) * 2 < src.count) {
                src.count = src.is32 ? n - 2 : (n - 2) * 2;
            }
            do_draw(s, &src);
        }
        return true;
    case PKT3_COPY_DW:
        eg_gfx_flush(s);
        if (n >= 5) {
            uint32_t v = 0;
            if (d[0] & 1) {
                v = eg_reg_read(s, (d[1] & 0xffff) << 2);
            } else {
                uint64_t a = ((uint64_t)(d[2] & 0xff) << 32) | (d[1] & ~3u);
                eg_gpu_read(s, a, &v, 4);
                v = le32_to_cpu(v);
            }
            if (d[0] & 2) {
                eg_reg_write(s, (d[3] & 0xffff) << 2, v);
            } else {
                uint64_t a = ((uint64_t)(d[4] & 0xff) << 32) | (d[3] & ~3u);
                v = cpu_to_le32(v);
                eg_gpu_write(s, a, &v, 4);
            }
        }
        return true;
    case PKT3_EVENT_WRITE:
        if (n >= 3 && (d[0] & 0x3f) == 0x15) {
            uint64_t a = ((uint64_t)(d[2] & 0xff) << 32) | (d[1] & ~7u);
            if (g->vk) {
                eg_gfx_zpass(s, a);     /* written once the GPU's draws complete */
            } else {
                /* ZPASS_DONE: the pixel count so far (begin/end pairs) */
                uint64_t v = cpu_to_le64(g->pixels | (1ull << 63));
                eg_gpu_write(s, a, &v, 8);
            }
        }
        return true;
    }
    return false;
}
