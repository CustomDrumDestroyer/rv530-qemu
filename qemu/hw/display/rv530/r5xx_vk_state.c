/*
 * Conversion of the decoded CPU-path state (R5xxRaster, R5xxTexSampler,
 * fragment constants, window-space vertices) into the Vulkan backend's
 * draw parameters.  Pure C; shared by the device model and the tests.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include "r5xx_vk.h"
#include "r5xx_vk_glsl.h"

static bool why(char *w, size_t n, const char *msg)
{
    if (w && n) {
        snprintf(w, n, "%s", msg);
    }
    return false;
}

static uint32_t fbits(float f)
{
    uint32_t u;
    memcpy(&u, &f, 4);
    return u;
}

static bool in_vram(uint32_t gpu_addr, uint32_t fb_start, uint64_t vram_size,
                    uint64_t len, uint32_t *off)
{
    uint64_t o;
    if (gpu_addr < fb_start) {
        return false;
    }
    o = (uint64_t)gpu_addr - fb_start;
    if (o + len > vram_size) {
        return false;
    }
    *off = (uint32_t)o;
    return true;
}

/* Locate [gpu_addr, gpu_addr + len): VRAM (gart = false) or GART. */
static bool locate(const RvkMem *m, uint32_t gpu_addr, uint64_t len,
                   uint32_t *off, bool *gart)
{
    if (in_vram(gpu_addr, m->fb_start, m->vram_size, len, off)) {
        *gart = false;
        return true;
    }
    if (m->gart_prepare && gpu_addr >= m->gart_start &&
        (uint64_t)gpu_addr + len - 1 <= m->gart_end &&
        m->gart_prepare(m->opaque, gpu_addr - m->gart_start, (uint32_t)len)) {
        *off = gpu_addr - m->gart_start;
        *gart = true;
        return true;
    }
    return false;
}

static bool color_fmt_ok(R5xxColorFormat f)
{
    return f == R5XX_COLOR_ARGB8888 || f == R5XX_COLOR_RGB565 ||
           f == R5XX_COLOR_ARGB1555 || f == R5XX_COLOR_ARGB4444 ||
           f == R5XX_COLOR_ARGB2101010 || f == R5XX_COLOR_I8 ||
           f == R5XX_COLOR_ARGB16161616 || f == R5XX_COLOR_ARGB16161616F ||
           f == R5XX_COLOR_ARGB32323232F || f == R5XX_COLOR_UV88;
}

static uint32_t eff_tiling(const R5xxSurfaceLayout *l)
{
    return r5xx_addr_is_linear(l->gpu_addr) ? R5XX_TILE_LINEAR : (uint32_t)l->tiling;
}

static bool build_sampler(RvkSampler *o, const R5xxTexSampler *s,
                          const RvkMem *m, char *w, size_t wl)
{
    uint32_t fmt, i, last;
    uint64_t size;
    bool gart;

    memset(o, 0, sizeof(*o));
    if (!r5xx_tex_gpu_format(s, &fmt, w, wl)) {
        return false;
    }
    last = s->num_levels ? s->num_levels - 1 : 0;
    if (last > 15) {
        last = 15;
    }
    size = (uint64_t)s->level_offset[last] + (uint64_t)s->level_pitch[last] *
           ((r5xx_minify(s->height, last) + 31) & ~31u);
    if (s->is_3d) {
        size = (uint64_t)s->level_offset[last] +
               (uint64_t)s->level_layer_size[last] * r5xx_minify(s->depth, last);
    }
    if (!locate(m, s->layout.gpu_addr, size, &o->base, &gart)) {
        return why(w, wl, "texture outside VRAM and mapped GART");
    }
    o->width = s->width;
    o->height = s->height;
    o->num_levels = s->num_levels ? s->num_levels : 1;
    o->bpp = s->bpp;
    o->fmt = fmt;
    o->flags = RVK_SF_VALID | (s->gamma ? RVK_SF_GAMMA : 0) | (gart ? RVK_SF_GART : 0) |
               ((uint32_t)s->endian << RVK_SF_ENDIAN_SHIFT) |
               (s->yuv_to_rgb ? RVK_SF_YUV_TO_RGB : 0) |
               (s->hw_format == 0x15 ? RVK_SF_YVYU : 0) |  /* R300_TX_FORMAT_YVYU422 */
               (s->is_3d ? RVK_SF_3D : 0);
    o->depth = s->is_3d ? s->depth : 1;
    for (i = 0; i < 4; i++) {
        o->swz |= (uint32_t)(s->swizzle[i] & 15) << (4 * i);
        o->border[i] = s->border_color.f[i];
    }
    o->wrap = (uint32_t)s->wrap_s | ((uint32_t)s->wrap_t << 4) | ((uint32_t)s->wrap_r << 8);
    o->filt = (uint32_t)s->min_filter | ((uint32_t)s->mag_filter << 4) |
              ((uint32_t)s->mip_filter << 8);
    o->signed_mask = s->signed_mask;
    o->micro_mode = s->layout.micro_mode;
    o->lod_bias = s->lod_bias;
    o->min_lod = s->min_lod;
    o->max_lod = s->max_lod;
    for (i = 0; i < 16; i++) {
        uint32_t addr = s->layout.gpu_addr + s->level_offset[i];
        o->level_offset[i] = s->level_offset[i];
        o->level_pitch[i] = s->level_pitch[i];
        o->level_tiling[i] = r5xx_addr_is_linear(addr) ? R5XX_TILE_LINEAR
                                                       : s->level_tiling[i];
        o->level_layer[i] = s->is_3d ? s->level_layer_size[i] : 0;
    }
    return true;
}

/* bytes covered by rows [0, h) of a surface (tile rows rounded up) */
static uint64_t surf_bytes(const R5xxSurfaceLayout *l, uint32_t h)
{
    return (uint64_t)l->pitch_bytes * ((h + 31) & ~31u);
}

bool rvk_build_draw(RvkDraw *out, const R5xxRaster *r,
                    const R5xxTexSampler *samp, uint32_t tex_used,
                    const r5xx_vec4 *consts, const RvkMem *m,
                    char *w, size_t wl)
{
    bool gart;
    const struct R5xxRasterDerived *d = &r->d;
    uint32_t *mi = out->misc;
    int n, i, c;

    memset(out, 0, sizeof(*out));
    if (d->fog_enable) {
        return why(w, wl, "fog");
    }
    if (consts) {
        memcpy(out->consts, consts, sizeof(out->consts));
    }
    for (c = 0; c < 4; c++) {
        out->blend_const[c] = d->blend_const.f[c];
    }
    for (i = 0; i < 16; i++) {
        if ((tex_used >> i) & 1) {
            if (!build_sampler(&out->samp[i], &samp[i], m, w, wl)) {
                return false;
            }
        }
    }
    for (n = 0; n < R5XX_MAX_COLOR_BUFS && n < 4; n++) {
        const R5xxSurfaceLayout *l = &r->rt.cbuf[n];
        RvkSurf *sf = &out->cb[n];
        if (!r->rt.cbuf_enabled[n]) {
            continue;
        }
        if (!color_fmt_ok(r->rt.cbuf_format[n]) || l->bpp == 0 || l->bpp > 16) {
            if (w) {
                snprintf(w, wl, "color buffer %d format %d (R5xxColorFormat), %u bytes per "
                         "pixel, not supported on the GPU path", n,
                         (int)r->rt.cbuf_format[n], l->bpp);
            }
            return false;
        }
        if (!locate(m, l->gpu_addr, surf_bytes(l, (uint32_t)r->d.sc_y1 + 1),
                    &sf->base, &gart)) {
            return why(w, wl, "color buffer outside VRAM and mapped GART");
        }
        sf->pitch = l->pitch_bytes;
        sf->bpp = l->bpp;
        sf->tiling = eff_tiling(l);
        sf->micro = l->micro_mode;
        sf->fmt = (uint32_t)r->rt.cbuf_format[n];
        sf->flags = RVK_CF_ENABLED | (d->cbuf_bgra[n] ? RVK_CF_BGRA : 0) |
                    (d->cbuf_float[n] ? RVK_CF_FLOAT : 0) | (gart ? RVK_CF_GART : 0);
        sf->cmask = d->cmask[n];
        for (c = 0; c < 4; c++) {
            sf->chansrc |= (uint32_t)(d->chan_src[n][c] & 3) << (8 * c);
        }
    }
    if (r->rt.zbuf_enabled) {
        const R5xxSurfaceLayout *l = &r->rt.zbuf;
        RvkSurf *zb = &out->zb;
        if (!locate(m, l->gpu_addr, surf_bytes(l, (uint32_t)r->d.sc_y1 + 1),
                    &zb->base, &gart)) {
            return why(w, wl, "depth buffer outside VRAM and mapped GART");
        }
        zb->pitch = l->pitch_bytes;
        zb->bpp = r->rt.z_is_16 ? 2 : 4;
        zb->tiling = eff_tiling(l);
        zb->micro = l->micro_mode;
        zb->flags = RVK_CF_ENABLED | (r->rt.z_is_16 ? RVK_ZF_Z16 : 0) |
                    (r->rt.has_stencil ? RVK_ZF_STENCIL : 0) | (gart ? RVK_CF_GART : 0);
    }

    mi[RVK_M_BLEND] = d->blend_enable ? 1 : 0;
    mi[RVK_M_RGB_EQ] = d->rgb_eq;
    mi[RVK_M_RGB_SRC] = d->rgb_src;
    mi[RVK_M_RGB_DST] = d->rgb_dst;
    mi[RVK_M_A_EQ] = d->a_eq;
    mi[RVK_M_A_SRC] = d->a_src;
    mi[RVK_M_A_DST] = d->a_dst;
    mi[RVK_M_DISCARD] = d->discard_mode;
    mi[RVK_M_ROP_EN] = d->rop_enable;
    mi[RVK_M_ROP] = d->rop;
    mi[RVK_M_ALPHA_TEST] = d->alpha_test;
    mi[RVK_M_ALPHA_FUNC] = d->alpha_func;
    mi[RVK_M_ALPHA_REF] = fbits(d->alpha_ref);
    mi[RVK_M_Z_EN] = d->z_enable;
    mi[RVK_M_Z_WRITE] = d->z_write;
    mi[RVK_M_Z_FUNC] = d->z_func;
    mi[RVK_M_S_EN] = d->stencil_enable;
    for (i = 0; i < 2; i++) {
        mi[RVK_M_S_FUNC0 + i] = d->s_func[i];
        mi[RVK_M_S_FAIL0 + i] = d->s_fail[i];
        mi[RVK_M_S_ZFAIL0 + i] = d->s_zfail[i];
        mi[RVK_M_S_ZPASS0 + i] = d->s_zpass[i];
        mi[RVK_M_S_REF0 + i] = d->s_ref[i];
        mi[RVK_M_S_MASK0 + i] = d->s_mask[i];
        mi[RVK_M_S_WMASK0 + i] = d->s_wmask[i];
    }
    mi[RVK_M_DEPTH_SCALE] = fbits(d->depth_scale);
    mi[RVK_M_DEPTH_OFFSET] = fbits(d->depth_offset);
    mi[RVK_M_MULTIWRITES] = d->num_multiwrites;
    mi[RVK_M_SC_X0] = (uint32_t)d->sc_x0;
    mi[RVK_M_SC_Y0] = (uint32_t)d->sc_y0;
    mi[RVK_M_SC_X1] = (uint32_t)d->sc_x1;
    mi[RVK_M_SC_Y1] = (uint32_t)d->sc_y1;
    mi[RVK_M_CLIP_RULE] = d->clip_rule;
    for (i = 0; i < 4; i++) {
        mi[RVK_M_CL_X0 + i] = (uint32_t)d->cl_x0[i];
        mi[RVK_M_CL_Y0 + i] = (uint32_t)d->cl_y0[i];
        mi[RVK_M_CL_X1 + i] = (uint32_t)d->cl_x1[i];
        mi[RVK_M_CL_Y1 + i] = (uint32_t)d->cl_y1[i];
    }
    mi[RVK_M_CMASK_INDEP] = d->cmask_independent;
    mi[RVK_M_DEPTH_FROM_SHADER] = d->depth_from_shader;
    return true;
}

/* the CPU rasterizer snaps window positions to its subpixel grid */
static float snapf(float v, int subpix)
{
    return subpix ? (float)llrintf(v * (float)subpix) / (float)subpix : v;
}

static void put_vertex(RvkVertex *o, const R5xxVertex *v, const R5xxVertex *prov,
                       uint32_t flatmask, bool front, int ninputs, int subpix)
{
    int i;

    memset(o, 0, sizeof(*o));
    o->pos[0] = snapf(v->x, subpix);
    o->pos[1] = snapf(v->y, subpix);
    o->pos[3] = v->w > 1e-30f ? v->w : 1e-30f;
    o->z = v->z;
    o->flatmask = flatmask;
    o->front = front;
    for (i = 0; i < ninputs && i < RVK_MAX_INPUTS; i++) {
        memcpy(o->smooth[i], v->attr[i].f, 16);
        memcpy(o->flat[i], prov->attr[i].f, 16);
    }
}

int rvk_triangle(RvkVertex out[3], const R5xxRaster *r, const R5xxVertex *v0,
                 const R5xxVertex *v1, const R5xxVertex *v2, bool nocull)
{
    const struct R5xxRasterDerived *d = &r->d;
    const R5xxVertex *vs[3] = { v0, v1, v2 };
    double area;
    bool ccw, front;
    float zoff = 0;
    int prov, i;

    area = ((double)v1->x - v0->x) * ((double)v2->y - v0->y) -
           ((double)v2->x - v0->x) * ((double)v1->y - v0->y);
    if (area == 0) {
        area = 1;
    }
    ccw = area < 0;
    front = (ccw == d->front_ccw);
    if (!nocull && ((front && d->cull_front) || (!front && d->cull_back))) {
        return 0;
    }
    if (d->poly_mode_dual && (front ? d->poly_front_type : d->poly_back_type) != 2) {
        return -1;                      /* point/line fill mode */
    }
    prov = d->provoking;
    if (prov >= 3) {
        prov = 2;
    }
    if (front ? d->po_front : d->po_back) {
        zoff = (float)r5xx_raster_poly_offset(r, v0, v1, v2,
                                              front ? d->po_front_scale : d->po_back_scale,
                                              front ? d->po_front_offset : d->po_back_offset);
    }
    for (i = 0; i < 3; i++) {
        put_vertex(&out[i], vs[i], vs[prov], r->flat_inputs, front,
                   r->num_fs_inputs, d->subpix);
        out[i].zoff = zoff;
    }
    return 3;
}

int rvk_point(RvkVertex out[6], const R5xxRaster *r, const R5xxVertex *v,
              float half_w, float half_h)
{
    float xs[4] = { -half_w, half_w, half_w, -half_w };
    float ys[4] = { -half_h, -half_h, half_h, half_h };
    static const int idx[6] = { 0, 1, 2, 0, 2, 3 };
    R5xxVertex c[4];
    int i;

    if (half_w < 0.5f) half_w = 0.5f;
    if (half_h < 0.5f) half_h = 0.5f;
    xs[0] = xs[3] = -half_w; xs[1] = xs[2] = half_w;
    ys[0] = ys[1] = -half_h; ys[2] = ys[3] = half_h;
    for (i = 0; i < 4; i++) {
        c[i] = *v;
        c[i].x = v->x + xs[i];
        c[i].y = v->y + ys[i];
    }
    for (i = 0; i < 6; i++) {
        put_vertex(&out[i], &c[idx[i]], v, r->flat_inputs, true, r->num_fs_inputs,
                   r->d.subpix);
    }
    return 6;
}

static void add_range(R5xxVkDrawReq *req, const R5xxSurfaceLayout *l,
                      const RvkMem *m, int y0, int y1)
{
    uint64_t off, end, base, limit;

    if (req->ndirty >= 5 || y1 < y0) {
        return;
    }
    if (l->gpu_addr >= m->fb_start && l->gpu_addr - m->fb_start < m->vram_size) {
        base = l->gpu_addr - m->fb_start;
        limit = m->vram_size;
    } else if (m->gart_prepare && l->gpu_addr >= m->gart_start &&
               l->gpu_addr <= m->gart_end) {
        base = RVK_GART_SPACE + (l->gpu_addr - m->gart_start);
        limit = RVK_GART_SPACE + ((uint64_t)m->gart_end - m->gart_start + 1);
    } else {
        return;
    }
    y0 &= ~31;
    y1 = (y1 | 31) + 1;
    off = base + (uint64_t)y0 * l->pitch_bytes;
    end = base + (uint64_t)y1 * l->pitch_bytes;
    if (off >= limit) {
        return;
    }
    if (end > limit) {
        end = limit;
    }
    req->dirty_off[req->ndirty] = off;
    req->dirty_len[req->ndirty] = end - off;
    /* surface signature (cross-surface write hazards) */
    req->dirty_sig[req->ndirty] = ((uint64_t)l->pitch_bytes << 32) ^
        ((uint64_t)l->bpp << 24) ^ ((uint64_t)eff_tiling(l) << 16) ^
        ((uint64_t)l->micro_mode << 8) ^ base;
    req->ndirty++;
}

void rvk_dirty_ranges(R5xxVkDrawReq *req, const R5xxRaster *r,
                      const RvkMem *m, int y0, int y1)
{
    int n;

    req->ndirty = 0;
    if (y0 < 0) {
        y0 = 0;
    }
    for (n = 0; n < R5XX_MAX_COLOR_BUFS && n < 4; n++) {
        if (r->rt.cbuf_enabled[n]) {
            add_range(req, &r->rt.cbuf[n], m, y0, y1);
        }
    }
    if (r->rt.zbuf_enabled && (r->d.z_enable || r->d.stencil_enable)) {
        add_range(req, &r->rt.zbuf, m, y0, y1);
    }
}

/* ------------------------------------------------------------------ */
/* Fragment shader source (no Vulkan needed: tests/us validates it)    */
/* ------------------------------------------------------------------ */

char *rvk_frag_prefix(bool has_f64)
{
    char *b = malloc(16384);
    size_t n = 0;
    int i;

#define P(...) (n += (size_t)snprintf(b + n, 16384 - n, __VA_ARGS__))
    P("#version 460\n");
    if (has_f64) {
        P("#define RVK_HAS_F64\n");
    }
    P("#define RVK_MISC_WORDS %d\n#define RVK_MAX_INPUTS %d\n", RVK_MISC_WORDS, RVK_MAX_INPUTS);
    P("#define RVK_SF_VALID %uu\n#define RVK_SF_GAMMA %uu\n#define RVK_SF_GART %uu\n",
      RVK_SF_VALID, RVK_SF_GAMMA, RVK_SF_GART);
    P("#define RVK_SF_ENDIAN_SHIFT %u\n", RVK_SF_ENDIAN_SHIFT);
    P("#define RVK_CF_GART %uu\n", RVK_CF_GART);
    P("#define RVK_FK_UNORM %uu\n#define RVK_FK_F16 %uu\n#define RVK_FK_F32 %uu\n",
      RVK_FK_UNORM, RVK_FK_F16, RVK_FK_F32);
    P("#define RVK_FK_YUV422 %uu\n#define RVK_FK_AYUV444 %uu\n", RVK_FK_YUV422, RVK_FK_AYUV444);
    P("#define RVK_SF_YUV_TO_RGB %uu\n#define RVK_SF_YVYU %uu\n", RVK_SF_YUV_TO_RGB, RVK_SF_YVYU);
    P("#define RVK_SF_3D %uu\n", RVK_SF_3D);
    P("#define RVK_CF_ENABLED %uu\n#define RVK_CF_BGRA %uu\n#define RVK_CF_FLOAT %uu\n",
      RVK_CF_ENABLED, RVK_CF_BGRA, RVK_CF_FLOAT);
    P("#define RVK_ZF_Z16 %uu\n#define RVK_ZF_STENCIL %uu\n", RVK_ZF_Z16, RVK_ZF_STENCIL);
    {
        static const char *bl[] = {
            "ZERO", "ONE", "SRC_COLOR", "ONE_MINUS_SRC_COLOR", "DST_COLOR",
            "ONE_MINUS_DST_COLOR", "SRC_ALPHA", "ONE_MINUS_SRC_ALPHA", "DST_ALPHA",
            "ONE_MINUS_DST_ALPHA", "SRC_ALPHA_SATURATE", "CONST_COLOR",
            "ONE_MINUS_CONST_COLOR", "CONST_ALPHA", "ONE_MINUS_CONST_ALPHA",
        };
        static const char *comb[] = {
            "ADD_CLAMP", "ADD_NOCLAMP", "SUB_CLAMP", "SUB_NOCLAMP", "MIN", "MAX",
            "RSUB_CLAMP", "RSUB_NOCLAMP",
        };
        for (i = 0; i < 15; i++) {
            P("#define RVK_BL_%s %du\n", bl[i], 32 + i);   /* R300_BLEND_GL_* */
        }
        for (i = 0; i < 8; i++) {
            P("#define RVK_COMB_%s %du\n", comb[i], i);
        }
    }
    {
        static const struct { const char *n; int v; } m[] = {
#define M(x) { #x, x }
            M(RVK_M_BLEND), M(RVK_M_RGB_EQ), M(RVK_M_RGB_SRC), M(RVK_M_RGB_DST),
            M(RVK_M_A_EQ), M(RVK_M_A_SRC), M(RVK_M_A_DST), M(RVK_M_DISCARD),
            M(RVK_M_ROP_EN), M(RVK_M_ROP), M(RVK_M_ALPHA_TEST), M(RVK_M_ALPHA_FUNC),
            M(RVK_M_ALPHA_REF), M(RVK_M_Z_EN), M(RVK_M_Z_WRITE), M(RVK_M_Z_FUNC),
            M(RVK_M_S_EN), M(RVK_M_S_FUNC0), M(RVK_M_S_FAIL0), M(RVK_M_S_ZFAIL0),
            M(RVK_M_S_ZPASS0), M(RVK_M_S_REF0), M(RVK_M_S_MASK0), M(RVK_M_S_WMASK0),
            M(RVK_M_DEPTH_SCALE), M(RVK_M_DEPTH_OFFSET), M(RVK_M_MULTIWRITES),
            M(RVK_M_SC_X0), M(RVK_M_SC_Y0), M(RVK_M_SC_X1), M(RVK_M_SC_Y1),
            M(RVK_M_CLIP_RULE), M(RVK_M_CL_X0), M(RVK_M_CL_Y0), M(RVK_M_CL_X1),
            M(RVK_M_CL_Y1), M(RVK_M_CMASK_INDEP), M(RVK_M_ZPASS_SLOT),
            M(RVK_M_DEPTH_FROM_SHADER),
#undef M
        };
        for (i = 0; i < (int)(sizeof(m) / sizeof(m[0])); i++) {
            P("#define %s %du\n", m[i].n, m[i].v);
        }
    }
#undef P
    return b;
}

const char *rvk_frag_template(void)
{
    return rvk_frag_glsl;
}

char *rvk_frag_source(const char *prefix, const char *us_glsl)
{
    static const char marker[] = "@US_PROGRAM@";
    const char *tpl = rvk_frag_glsl, *at = strstr(tpl, marker);
    size_t n = strlen(prefix) + strlen(tpl) + strlen(us_glsl) + 1;
    char *src = malloc(n);

    snprintf(src, n, "%s%.*s%s%s", prefix, (int)(at - tpl), tpl, us_glsl,
             at + strlen(marker));
    return src;
}
