/*
 * R5xx fragment pipeline on the host GPU.  The C side prepends
 * "#version", the RVK_* defines and appends the translated fragment
 * program (r500_us_to_glsl) in place of the program marker below.
 *
 * Semantics follow the CPU reference path: r5xx_tex.c (addressing,
 * formats, filtering) and r5xx_raster.c (shade_pixel / write_color).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#extension GL_ARB_fragment_shader_interlock : require
#extension GL_KHR_shader_subgroup_basic : require
#extension GL_KHR_shader_subgroup_quad : require
#extension GL_EXT_control_flow_attributes : require

layout(pixel_interlock_ordered) in;

struct Sampler {
    uint base, width, height, num_levels;
    uint bpp, fmt, flags, swz;
    uint wrap, filt, signed_mask, micro_mode;
    float lod_bias, min_lod, max_lod;
    uint depth;
    vec4 border;
    uint level_offset[16];
    uint level_pitch[16];
    uint level_tiling[16];
    uint level_layer[16];
};

struct Surf {
    uint base, pitch, bpp, tiling, micro, fmt, flags, cmask, chansrc;
    uint pad0, pad1, pad2;
};

struct Draw {
    vec4 consts[256];
    vec4 blend_const;
    Sampler samp[16];
    Surf cb[4];
    Surf zb;
    uint misc[RVK_MISC_WORDS];
};

layout(std430, set = 0, binding = 0) coherent buffer VramBuf { uint vram[]; };
layout(std430, set = 0, binding = 1) readonly buffer DrawBuf { Draw draws[]; };
layout(std430, set = 0, binding = 3) buffer CountBuf { uint zpass_count[]; };
layout(std430, set = 0, binding = 4) coherent buffer SysBuf { uint sysram[]; };
layout(std430, set = 0, binding = 5) readonly buffer GartBuf { uint gart[]; };
/* read-only, cacheable aliases of the same memory for texture fetches
 * (barriers between draws make them safe) */
layout(std430, set = 0, binding = 6) restrict readonly buffer VramTex { uint vram_tex[]; };
layout(std430, set = 0, binding = 7) restrict readonly buffer SysTex { uint sysram_tex[]; };
layout(push_constant) uniform PC { uint draw; } pc;

layout(location = 0) in vec4 v_s[RVK_MAX_INPUTS];
layout(location = 12) flat in vec4 v_f[RVK_MAX_INPUTS];
layout(location = 24) flat in uvec2 v_misc;       /* flat mask, front */
layout(location = 25) noperspective in float v_z;

#define D draws[pc.draw]
#define MISC(i) D.misc[i]
#define MISCF(i) uintBitsToFloat(D.misc[i])

vec4 us_out[4];
float us_depth;
bool us_depth_written;
bool us_killed;

/* ------------------------------------------------------------------ */
/* VRAM access                                                         */
/* ------------------------------------------------------------------ */

/* Addresses are VRAM byte offsets, or GART byte offsets when 'g' is set
 * (translated through the per-page table into guest RAM). */
uint gidx(uint a) { return (gart[a >> 12] << 10) | ((a & 4095u) >> 2); }
uint rd32(bool g, uint a) { return g ? sysram[gidx(a)] : vram[a >> 2]; }
uint rd32_tex(bool g, uint a) { return g ? sysram_tex[gidx(a)] : vram_tex[a >> 2]; }
void wr32(bool g, uint a, uint v) { if (g) sysram[gidx(a)] = v; else vram[a >> 2] = v; }
uint rdn(bool g, uint a, uint bytes)
{
    uint w = rd32(g, a);
    if (bytes >= 4u) return w;
    uint sh = (a & 3u) * 8u;
    uint m = bytes == 2u ? 0xffffu : 0xffu;
    return (w >> sh) & m;
}
void wrn(bool g, uint a, uint bytes, uint v)
{
    if (bytes >= 4u) { wr32(g, a, v); return; }
    uint sh = (a & 3u) * 8u;
    uint m = (bytes == 2u ? 0xffffu : 0xffu) << sh;
    if (g) {
        uint i = gidx(a);
        atomicAnd(sysram[i], ~m);
        atomicOr(sysram[i], (v << sh) & m);
    } else {
        atomicAnd(vram[a >> 2], ~m);
        atomicOr(vram[a >> 2], (v << sh) & m);
    }
}

/* ------------------------------------------------------------------ */
/* Surface layout (r5xx_surface_pixel_offset)                          */
/* ------------------------------------------------------------------ */

const uint TT[60] = uint[](
    /* linear-macro, [l2bpp][micro][dim] */
    32u,1u, 8u,4u, 0u,0u,   16u,1u, 8u,2u, 4u,4u,   8u,1u, 4u,2u, 0u,0u,
    4u,1u, 2u,2u, 0u,0u,    2u,1u, 0u,0u, 0u,0u,
    /* macro */
    256u,8u, 64u,32u, 0u,0u,  128u,8u, 64u,16u, 32u,32u,  64u,8u, 32u,16u, 0u,0u,
    32u,8u, 16u,16u, 0u,0u,   16u,8u, 0u,0u, 0u,0u);

uint tt(uint macro, uint l2, uint micro, uint dim)
{
    return TT[((macro * 5u + l2) * 3u + micro) * 2u + dim];
}
uint l2bpp(uint bpp) { return min(uint(findMSB(max(bpp, 1u))), 4u); }

uint micro_index(uint bpp, uint tiling, uint micro_mode)
{
    if (tiling != 1u && tiling != 3u) return 0u;
    uint idx = micro_mode != 0u ? micro_mode : (bpp == 2u ? 2u : 1u);
    idx = min(idx, 2u);
    uint l2 = l2bpp(bpp);
    if (tt(0u, l2, idx, 0u) == 0u) {
        idx = 1u;
        if (tt(0u, l2, idx, 0u) == 0u) idx = 0u;
    }
    return idx;
}
uint palign(uint bpp, uint micro, bool macro, uint dim)
{
    uint v = tt(macro ? 1u : 0u, l2bpp(bpp), micro, dim);
    return v != 0u ? v : 1u;
}

uint surf_offset(uint pitch, uint bpp, uint tiling, uint micro_mode, uint x, uint y)
{
    bool macro = tiling == 2u || tiling == 3u;
    uint micro = micro_index(bpp, tiling, micro_mode);
    if (!macro && micro == 0u) return y * pitch + x * bpp;
    uint mw = 1u, mh = 1u;
    if (micro != 0u) { mw = palign(bpp, micro, false, 0u); mh = palign(bpp, micro, false, 1u); }
    if (!macro) {
        uint tx = x / mw, ty = y / mh, ix = x % mw, iy = y % mh;
        return ty * pitch * mh + tx * 32u + (iy * mw + ix) * bpp;
    }
    uint Mw = palign(bpp, micro, true, 0u), Mh = palign(bpp, micro, true, 1u);
    uint ix = x % Mw, iy = y % Mh;
    uint off = (y / Mh) * pitch * Mh + (x / Mw) * 2048u;
    if (micro != 0u) {
        uint tx = ix / mw, ty = iy / mh, px = ix % mw, py = iy % mh;
        off += (ty * (Mw / mw) + tx) * 32u + (py * mw + px) * bpp;
    } else {
        off += iy * (Mw * bpp) + ix * bpp;
    }
    return off;
}

/* ------------------------------------------------------------------ */
/* Textures                                                            */
/* ------------------------------------------------------------------ */

float srgb_to_linear(float c)
{
    return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4);
}

vec4 unpack_raw(uint si, uvec4 w)
{
    uint fmt = D.samp[si].fmt, kind = fmt & 15u, nchan = (fmt >> 4) & 15u;
    uint smask = D.samp[si].signed_mask;
    vec4 raw = vec4(0.0, 0.0, 0.0, 1.0);
    if (kind == RVK_FK_UNORM) {
        uint shift = 0u;
        for (uint i = 0u; i < nchan; i++) {
            uint nb = (fmt >> (8u + 5u * i)) & 31u;
            if (nb == 0u) break;
            uint word = shift >> 5, s = shift & 31u;
            uint v = w[word] >> s;
            if (s + nb > 32u && word < 3u) v |= w[word + 1u] << (32u - s);
            v &= nb >= 32u ? 0xffffffffu : ((1u << nb) - 1u);
            shift += nb;
            if (((smask >> i) & 1u) != 0u) {
                int sv = int(v << (32u - nb)) >> (32u - nb);
                raw[i] = max(float(sv) / float((1u << (nb - 1u)) - 1u), -1.0);
            } else {
                raw[i] = float(v) / float((nb >= 32u) ? 0xffffffffu : ((1u << nb) - 1u));
            }
        }
    } else if (kind == RVK_FK_F16) {
        for (uint i = 0u; i < nchan; i++) {
            raw[i] = unpackHalf2x16(w[i >> 1] >> ((i & 1u) * 16u)).x;
        }
    } else {
        for (uint i = 0u; i < nchan; i++) raw[i] = uintBitsToFloat(w[i]);
    }
    if ((D.samp[si].flags & RVK_SF_GAMMA) != 0u && kind == RVK_FK_UNORM) {
        for (int i = 0; i < 3; i++) if (raw[i] > 0.0) raw[i] = srgb_to_linear(raw[i]);
    }
    return raw;
}

vec4 apply_swizzle(uint si, vec4 raw)
{
    uint swz = D.samp[si].swz;
    vec4 o;
    for (int i = 0; i < 4; i++) {
        uint sel = (swz >> (4 * i)) & 15u;
        float v;
        if (sel < 4u) v = raw[sel];
        else if (sel == 4u) v = 0.0;
        else if (sel == 5u) v = 1.0;
        else { v = 2.0 * raw[sel == 6u ? 2 : 3]; if (v > 1.0) v = 0.0; }
        o[i] = v;
    }
    return o;
}

uint minify(uint v, uint l) { return max(v >> l, 1u); }

/* r5xx_endian_swap: TX_OFFSET byte order, per dword */
uint endian_swap(uint mode, uint w)
{
    if (mode == 1u) return ((w & 0x00ff00ffu) << 8) | ((w >> 8) & 0x00ff00ffu);
    if (mode == 2u) return (w << 24) | ((w & 0xff00u) << 8) | ((w >> 8) & 0xff00u) | (w >> 24);
    if (mode == 3u) return (w << 16) | (w >> 16);
    return w;
}

/* video formats (r5xx_tex.c unpack_raw): the raw channels are Z = U,
 * Y = luma, X = V, or the converted B, G, R with YUV_TO_RGB */
vec4 unpack_yuv(uint si, uint w, uint sx)
{
    float b0 = float(w & 255u) / 255.0, b1 = float((w >> 8) & 255u) / 255.0;
    float b2 = float((w >> 16) & 255u) / 255.0, b3 = float(w >> 24) / 255.0;
    float cy, cu, cv, a = 1.0;
    uint flags = D.samp[si].flags;
    if ((D.samp[si].fmt & 15u) == RVK_FK_AYUV444) {
        cu = b0; cy = b1; cv = b2; a = b3;
    } else if ((flags & RVK_SF_YVYU) != 0u) {
        cu = b0; cv = b2; cy = sx != 0u ? b3 : b1;
    } else {
        cu = b1; cv = b3; cy = sx != 0u ? b2 : b0;
    }
    if ((flags & RVK_SF_YUV_TO_RGB) != 0u) {
        float u = cu - 0.5, v = cv - 0.5;
        float r = clamp(cy + 1.402 * v, 0.0, 1.0);
        float gg = clamp(cy - 0.344136 * u - 0.714136 * v, 0.0, 1.0);
        float bb = clamp(cy + 1.772 * u, 0.0, 1.0);
        return vec4(bb, gg, r, a);
    }
    return vec4(cv, cy, cu, a);
}

vec4 fetch_texel(uint si, uint level, int z, int x, int y)
{
    uint lw = minify(D.samp[si].width, level), lh = minify(D.samp[si].height, level);
    uint ld = minify(D.samp[si].depth, level);
    x = clamp(x, 0, int(lw) - 1);
    y = clamp(y, 0, int(lh) - 1);
    z = clamp(z, 0, int(ld) - 1);
    uint bpp = D.samp[si].bpp, kind = D.samp[si].fmt & 15u;
    /* 422: the dword holding the texel pair */
    uint px = kind == RVK_FK_YUV422 ? uint(x) & ~1u : uint(x);
    uint addr = D.samp[si].base + D.samp[si].level_offset[level] +
                uint(z) * D.samp[si].level_layer[level] +
                surf_offset(D.samp[si].level_pitch[level], bpp, D.samp[si].level_tiling[level],
                            D.samp[si].micro_mode, px, uint(y));
    bool g = (D.samp[si].flags & RVK_SF_GART) != 0u;
    uint endian = (D.samp[si].flags >> RVK_SF_ENDIAN_SHIFT) & 3u;
    uvec4 w = uvec4(0u);
    if (kind == RVK_FK_YUV422 || kind == RVK_FK_AYUV444) {
        uint d = endian_swap(endian, rd32_tex(g, addr & ~3u));
        if ((addr & 3u) != 0u) {
            d = (d >> ((addr & 3u) * 8u)) |
                (endian_swap(endian, rd32_tex(g, (addr & ~3u) + 4u)) << (32u - (addr & 3u) * 8u));
        }
        return apply_swizzle(si, unpack_yuv(si, d, uint(x) & 1u));
    }
    if (bpp >= 4u) {
        for (uint i = 0u; i < bpp / 4u; i++) w[i] = endian_swap(endian, rd32_tex(g, addr + 4u * i));
    } else {
        w.x = (endian_swap(endian, rd32_tex(g, addr)) >> ((addr & 3u) * 8u)) &
              (bpp == 2u ? 0xffffu : 0xffu);
    }
    return apply_swizzle(si, unpack_raw(si, w));
}

vec4 fetch_or_border(uint si, uint level, int z, int x, int y, int lw, int lh, int ld)
{
    if (x < 0 || x >= lw || y < 0 || y >= lh || z < 0 || z >= ld) return D.samp[si].border;
    return fetch_texel(si, level, z, x, y);
}

int wrap_nearest(uint mode, float s, int size)
{
    float fs = float(size);
    int i;
    if (mode == 0u) {
        i = int(floor(s * fs)) % size;
        return i < 0 ? i + size : i;
    } else if (mode == 1u) {
        float u = fract(s);
        if ((int(floor(s)) & 1) != 0) u = 1.0 - u;
        return clamp(int(floor(u * fs)), 0, size - 1);
    } else if (mode == 2u || mode == 3u) {
        if (mode == 3u) s = abs(s);
        return clamp(int(floor(s * fs)), 0, size - 1);
    } else if (mode == 4u || mode == 5u) {
        if (mode == 5u) s = abs(s);
        s *= fs;
        if (s <= 0.0) return 0;
        if (s >= fs) return size - 1;
        return int(floor(s));
    } else {
        if (mode == 7u) s = abs(s);
        float mn = -1.0 / (2.0 * fs), mx = 1.0 - mn;
        if (s <= mn) return -1;
        if (s >= mx) return size;
        return int(floor(s * fs));
    }
}

/* returns i0, i1 in xy and the weight in uintBitsToFloat(z) */
ivec3 wrap_linear(uint mode, float s, int size)
{
    float fs = float(size), u, w;
    int i0, i1;
    if (mode == 0u) {
        u = s * fs - 0.5;
        i0 = int(floor(u));
        w = fract(u);
        i0 = i0 % size;
        if (i0 < 0) i0 += size;
        i1 = (i0 + 1) % size;
    } else if (mode == 1u) {
        u = fract(s);
        if ((int(floor(s)) & 1) != 0) u = 1.0 - u;
        u = u * fs - 0.5;
        i0 = int(floor(u)); i1 = i0 + 1; w = fract(u);
        i0 = max(i0, 0); i1 = min(i1, size - 1);
    } else if (mode == 2u || mode == 3u) {
        if (mode == 3u) s = abs(s);
        s = clamp(s, 0.0, 1.0);
        u = s * fs - 0.5;
        i0 = int(floor(u)); i1 = i0 + 1; w = fract(u);
        i0 = max(i0, 0); i1 = min(i1, size - 1);
    } else if (mode == 4u || mode == 5u) {
        if (mode == 5u) s = abs(s);
        s = clamp(s, 0.0, 1.0);
        u = s * fs - 0.5;
        i0 = int(floor(u)); i1 = i0 + 1; w = fract(u);
    } else {
        if (mode == 7u) s = abs(s);
        float mn = -1.0 / (2.0 * fs), mx = 1.0 - mn;
        s = clamp(s, mn, mx);
        u = s * fs - 0.5;
        i0 = int(floor(u)); i1 = i0 + 1; w = fract(u);
    }
    return ivec3(i0, i1, int(floatBitsToUint(w)));
}

/* one level, nearest or (bi/tri)linear; w is the 3D coordinate */
vec4 sample_level(uint si, uint level, float u, float v, float w, uint filt_sel)
{
    int lw = int(minify(D.samp[si].width, level)), lh = int(minify(D.samp[si].height, level));
    int ld = int(minify(D.samp[si].depth, level));
    uint ws = D.samp[si].wrap & 15u, wt = (D.samp[si].wrap >> 4) & 15u;
    uint wr = (D.samp[si].wrap >> 8) & 15u;
    bool is3d = (D.samp[si].flags & RVK_SF_3D) != 0u;
    ivec3 tx[8];
    float tw[8];
    int ntaps;
    if (filt_sel == 1u) {
        tx[0] = ivec3(wrap_nearest(ws, u, lw), wrap_nearest(wt, v, lh),
                      is3d ? wrap_nearest(wr, w, ld) : 0);
        tw[0] = 1.0;
        ntaps = 1;
    } else {
        ivec3 xs = wrap_linear(ws, u, lw), ys = wrap_linear(wt, v, lh);
        ivec3 zs = is3d ? wrap_linear(wr, w, ld) : ivec3(0, 0, int(floatBitsToUint(0.0)));
        float fx = uintBitsToFloat(uint(xs.z)), fy = uintBitsToFloat(uint(ys.z));
        float fz = uintBitsToFloat(uint(zs.z));
        tx[0] = ivec3(xs.x, ys.x, zs.x); tw[0] = (1.0 - fx) * (1.0 - fy);
        tx[1] = ivec3(xs.y, ys.x, zs.x); tw[1] = fx * (1.0 - fy);
        tx[2] = ivec3(xs.x, ys.y, zs.x); tw[2] = (1.0 - fx) * fy;
        tx[3] = ivec3(xs.y, ys.y, zs.x); tw[3] = fx * fy;
        ntaps = 4;
        if (is3d && ld > 1) {
            for (int k = 0; k < 4; k++) {
                tx[4 + k] = ivec3(tx[k].xy, zs.y);
                tw[4 + k] = tw[k] * fz;
                tw[k] *= 1.0 - fz;
            }
            ntaps = 8;
        }
    }
    vec4 acc = vec4(0.0);
    [[dont_unroll]] for (int i = 0; i < ntaps; i++) {
        acc += tw[i] * fetch_or_border(si, level, tx[i].z, tx[i].x, tx[i].y, lw, lh, ld);
    }
    return acc;
}

/* op: R500_TEX_INST_* (1 LD, 3 PROJ, 4 LODBIAS, 5 LOD, 6 DXDY) */
vec4 us_tex(uint unit, uint op, bool unscaled, vec4 c, vec4 ddx, vec4 ddy)
{
    uint si = unit;
    if ((D.samp[si].flags & RVK_SF_VALID) == 0u) return D.samp[si].border;
    vec3 cc = c.xyz;
    if (op == 3u && c.w != 0.0) cc /= c.w;
    float fw = float(D.samp[si].width), fh = float(D.samp[si].height);
    float fd = float(D.samp[si].depth);
    bool is3d = (D.samp[si].flags & RVK_SF_3D) != 0u;
    float u = cc.x, v = cc.y, w3 = cc.z;
    if (unscaled) { u /= fw; v /= fh; }
    float dudx, dvdx, dudy, dvdy, dwdx, dwdy;
    if (op == 6u) {
        dudx = ddx.x; dvdx = ddx.y; dudy = ddy.x; dvdy = ddy.y; dwdx = ddx.z; dwdy = ddy.z;
        if (unscaled) { dudx /= fw; dvdx /= fh; dudy /= fw; dvdy /= fh; }
    } else {
        dudx = dFdxFine(u); dvdx = dFdxFine(v); dudy = dFdyFine(u); dvdy = dFdyFine(v);
        dwdx = dFdxFine(w3); dwdy = dFdyFine(w3);
    }
    float lod;
    if (op == 5u) {
        lod = c.w;
    } else {
        dudx *= fw; dvdx *= fh; dudy *= fw; dvdy *= fh;
        if (is3d) { dwdx *= fd; dwdy *= fd; } else { dwdx = 0.0; dwdy = 0.0; }
        float lx = sqrt(dudx * dudx + dvdx * dvdx + dwdx * dwdx);
        float ly = sqrt(dudy * dudy + dvdy * dvdy + dwdy * dwdy);
        float rho = max(lx, ly);
        lod = rho > 0.0 ? log2(rho) : -1000.0;
        if (op == 4u) lod += c.w;
    }
    lod += D.samp[si].lod_bias;
    float minl = D.samp[si].min_lod, maxl = D.samp[si].max_lod;
    lod = clamp(lod, minl, maxl);
    uint fmin = D.samp[si].filt & 15u, fmag = (D.samp[si].filt >> 4) & 15u;
    uint fmip = (D.samp[si].filt >> 8) & 15u;
    uint filt_sel = lod > 0.0 ? fmin : fmag;
    if (filt_sel == 0u) filt_sel = lod > 0.0 ? 1u : 2u;
    uint lev[2];
    float lwt[2];
    int nlev = 1;
    lwt[0] = 1.0;
    if (fmip == 0u || D.samp[si].num_levels == 1u) {
        lev[0] = uint(minl);
    } else if (fmip == 1u) {
        lev[0] = uint(clamp(int(ceil(lod + 0.5)) - 1, int(minl), int(maxl)));
    } else {
        int l0 = int(floor(lod)), l1 = l0 + 1;
        float f = lod - float(l0);
        l0 = max(l0, int(minl)); l1 = min(l1, int(maxl)); l0 = min(l0, int(maxl)); l1 = max(l1, l0);
        lev[0] = uint(l0);
        if (l1 != l0 && f > 0.0) {
            lev[1] = uint(l1); lwt[0] = 1.0 - f; lwt[1] = f; nlev = 2;
        }
    }
    vec4 acc = vec4(0.0);
    [[dont_unroll]] for (int i = 0; i < nlev; i++) {
        acc += lwt[i] * sample_level(si, lev[i], u, v, w3, filt_sel);
    }
    return acc;
}

/* ------------------------------------------------------------------ */
/* Fragment program support                                            */
/* ------------------------------------------------------------------ */

vec4 us_input(int i)
{
    return ((v_misc.x >> i) & 1u) != 0u ? v_f[i] : v_s[i];
}
vec4 us_const(uint i) { return D.consts[i & 255u]; }
float us_mul(float a, float b) { return a * b; }
float us_mulz(float a, float b) { return (a == 0.0 || b == 0.0) ? 0.0 : a * b; }
float us_flush(float v) { return abs(v) < 1.17549435e-38 ? 0.0 * sign(v) : v; }
bool us_cmp(float v, uint op)
{
    if (abs(v) < 1.17549435e-38) v = 0.0;
    if (op == 0u) return v == 0.0;
    if (op == 1u) return v < 0.0;
    if (op == 2u) return v >= 0.0;
    return v != 0.0;
}
uint us_pm(uint pred, uint sel, bool inv)
{
    uint m;
    if (sel == 1u) m = pred & 15u;
    else if (sel >= 2u && sel <= 5u) m = ((pred >> (sel - 2u)) & 1u) != 0u ? 15u : 0u;
    else return 15u;
    return inv ? (m ^ 15u) : m;
}

@US_PROGRAM@

/* ------------------------------------------------------------------ */
/* Output merger (r5xx_raster.c shade_pixel / write_color)             */
/* ------------------------------------------------------------------ */

/* depth and stencil values are integers below 2^24: compare them exactly */
bool zs_compare(uint f, uint a, uint b)
{
    switch (f & 7u) {
    case 0u: return false;
    case 1u: return a < b;
    case 2u: return a <= b;
    case 3u: return a == b;
    case 4u: return a >= b;
    case 5u: return a > b;
    case 6u: return a != b;
    }
    return true;
}
bool alpha_compare(uint f, float a, float r)
{
    switch (f & 7u) {
    case 0u: return false;
    case 1u: return a < r;
    case 2u: return a == r;
    case 3u: return a <= r;
    case 4u: return a > r;
    case 5u: return a != r;
    case 6u: return a >= r;
    }
    return true;
}
uint stencil_op(uint op, uint s, uint ref)
{
    switch (op & 7u) {
    case 0u: return s;
    case 1u: return 0u;
    case 2u: return ref;
    case 3u: return s == 255u ? 255u : s + 1u;
    case 4u: return s == 0u ? 0u : s - 1u;
    case 5u: return (~s) & 255u;
    case 6u: return (s + 1u) & 255u;
    }
    return (s - 1u) & 255u;
}

/* color formats (R5xxColorFormat): 0 ARGB8888 1 RGB565 2 ARGB1555
 * 3 ARGB4444 4 ARGB2101010 5 I8, 9 UV88 (byte 0 = R slot, byte 1 = B) */
vec4 color_unpack(uint fmt, uint v)
{
    if (fmt == 0u) return vec4((v >> 16) & 255u, (v >> 8) & 255u, v & 255u, v >> 24) / 255.0;
    if (fmt == 1u) return vec4(float((v >> 11) & 31u) / 31.0, float((v >> 5) & 63u) / 63.0, float(v & 31u) / 31.0, 1.0);
    if (fmt == 2u) return vec4(float((v >> 10) & 31u) / 31.0, float((v >> 5) & 31u) / 31.0, float(v & 31u) / 31.0, float((v >> 15) & 1u));
    if (fmt == 3u) return vec4((v >> 8) & 15u, (v >> 4) & 15u, v & 15u, (v >> 12) & 15u) / 15.0;
    if (fmt == 4u) return vec4(float((v >> 20) & 1023u) / 1023.0, float((v >> 10) & 1023u) / 1023.0, float(v & 1023u) / 1023.0, float((v >> 30) & 3u) / 3.0);
    if (fmt == 9u) return vec4(float(v & 255u) / 255.0, 0.0, float((v >> 8) & 255u) / 255.0, 1.0);
    return vec4(float(v & 255u) / 255.0);
}
uint unorm(float v, float m) { return uint(clamp(v, 0.0, 1.0) * m + 0.5); }

/* wide formats (6 ARGB16161616, 7 ARGB16161616F, 8 ARGB32323232F):
 * channels are stored B, G, R, A from the lowest address */
vec4 color_unpack_wide(uint fmt, uvec4 w)
{
    if (fmt == 6u) {
        return vec4(float(w.y & 0xffffu), float(w.x >> 16), float(w.x & 0xffffu), float(w.y >> 16)) / 65535.0;
    }
    if (fmt == 7u) {
        vec2 bg = unpackHalf2x16(w.x), ra = unpackHalf2x16(w.y);
        return vec4(ra.x, bg.y, bg.x, ra.y);
    }
    return vec4(uintBitsToFloat(w.z), uintBitsToFloat(w.y), uintBitsToFloat(w.x), uintBitsToFloat(w.w));
}
uvec4 color_pack_wide(uint fmt, vec4 c)
{
    if (fmt == 6u) {
        return uvec4(unorm(c.z, 65535.0) | (unorm(c.y, 65535.0) << 16),
                     unorm(c.x, 65535.0) | (unorm(c.w, 65535.0) << 16), 0u, 0u);
    }
    if (fmt == 7u) {
        return uvec4(packHalf2x16(vec2(c.z, c.y)), packHalf2x16(vec2(c.x, c.w)), 0u, 0u);
    }
    return uvec4(floatBitsToUint(c.z), floatBitsToUint(c.y), floatBitsToUint(c.x), floatBitsToUint(c.w));
}
uint color_pack(uint fmt, vec4 c)
{
    if (fmt == 0u) return (unorm(c.w, 255.0) << 24) | (unorm(c.x, 255.0) << 16) | (unorm(c.y, 255.0) << 8) | unorm(c.z, 255.0);
    if (fmt == 1u) return (unorm(c.x, 31.0) << 11) | (unorm(c.y, 63.0) << 5) | unorm(c.z, 31.0);
    if (fmt == 2u) return (unorm(c.w, 1.0) << 15) | (unorm(c.x, 31.0) << 10) | (unorm(c.y, 31.0) << 5) | unorm(c.z, 31.0);
    if (fmt == 3u) return (unorm(c.w, 15.0) << 12) | (unorm(c.x, 15.0) << 8) | (unorm(c.y, 15.0) << 4) | unorm(c.z, 15.0);
    if (fmt == 4u) return (unorm(c.w, 3.0) << 30) | (unorm(c.x, 1023.0) << 20) | (unorm(c.y, 1023.0) << 10) | unorm(c.z, 1023.0);
    if (fmt == 9u) return unorm(c.x, 255.0) | (unorm(c.z, 255.0) << 8);
    return unorm(c.x, 255.0);
}

float blend_factor(uint code, int ch, vec4 s, vec4 d, vec4 k)
{
    switch (code) {
    case RVK_BL_ZERO: return 0.0;
    case RVK_BL_ONE: return 1.0;
    case RVK_BL_SRC_COLOR: return s[ch];
    case RVK_BL_ONE_MINUS_SRC_COLOR: return 1.0 - s[ch];
    case RVK_BL_DST_COLOR: return d[ch];
    case RVK_BL_ONE_MINUS_DST_COLOR: return 1.0 - d[ch];
    case RVK_BL_SRC_ALPHA: return s.w;
    case RVK_BL_ONE_MINUS_SRC_ALPHA: return 1.0 - s.w;
    case RVK_BL_DST_ALPHA: return d.w;
    case RVK_BL_ONE_MINUS_DST_ALPHA: return 1.0 - d.w;
    case RVK_BL_SRC_ALPHA_SATURATE: return ch < 3 ? min(s.w, 1.0 - d.w) : 1.0;
    case RVK_BL_CONST_COLOR: return k[ch];
    case RVK_BL_ONE_MINUS_CONST_COLOR: return 1.0 - k[ch];
    case RVK_BL_CONST_ALPHA: return k.w;
    case RVK_BL_ONE_MINUS_CONST_ALPHA: return 1.0 - k.w;
    }
    return 1.0;
}
float blend_channel(uint eq, uint fs, uint fd, int ch, vec4 s, vec4 d, vec4 k, bool isf)
{
    float a = s[ch], b = d[ch], v;
    if (eq == RVK_COMB_MIN) return min(a, b);
    if (eq == RVK_COMB_MAX) return max(a, b);
    a *= blend_factor(fs, ch, s, d, k);
    b *= blend_factor(fd, ch, s, d, k);
    if (eq == RVK_COMB_SUB_CLAMP || eq == RVK_COMB_SUB_NOCLAMP) v = a - b;
    else if (eq == RVK_COMB_RSUB_CLAMP || eq == RVK_COMB_RSUB_NOCLAMP) v = b - a;
    else v = a + b;
    if (isf && (eq & 1u) == 0u) v = clamp(v, 0.0, 1.0);
    return v;
}
uint rop_apply(uint rop, uint s, uint d)
{
    uint v = 0u;
    if ((rop & 1u) != 0u) v |= ~s & ~d;
    if ((rop & 2u) != 0u) v |= ~s & d;
    if ((rop & 4u) != 0u) v |= s & ~d;
    if ((rop & 8u) != 0u) v |= s & d;
    return v;
}
bool discard_pixel(uint mode, vec4 s)
{
    bool rgb0 = s.x == 0.0 && s.y == 0.0 && s.z == 0.0;
    bool rgb1 = s.x >= 1.0 && s.y >= 1.0 && s.z >= 1.0;
    bool a0 = s.w == 0.0, a1 = s.w >= 1.0;
    switch (mode) {
    case 1u: return a0;
    case 2u: return rgb0;
    case 3u: return a0 && rgb0;
    case 4u: return a1;
    case 5u: return rgb1;
    case 6u: return a1 && rgb1;
    }
    return false;
}

uint surf_addr(Surf sf, uint x, uint y)
{
    return sf.base + surf_offset(sf.pitch, sf.bpp, sf.tiling, sf.micro, x, y);
}

bool write_color(int n, uint x, uint y, vec4 shader)
{
    Surf sf = D.cb[n];
    uint hwmask = MISC(RVK_M_CMASK_INDEP) != 0u ? sf.cmask : D.cb[0].cmask;
    bool bgra = (sf.flags & RVK_CF_BGRA) != 0u;
    uint chan_mask = 0u;
    for (int c = 0; c < 4; c++) {
        if (((hwmask >> c) & 1u) != 0u) {
            uint pos = bgra ? uint[](2u, 1u, 0u, 3u)[c] : uint(c);
            chan_mask |= 1u << pos;
        }
    }
    if (chan_mask == 0u) return false;
    vec4 src;
    for (int c = 0; c < 4; c++) src[c] = shader[(sf.chansrc >> (8 * c)) & 255u];
    bool blend = (MISC(RVK_M_BLEND) & 1u) != 0u;
    if (blend && discard_pixel(MISC(RVK_M_DISCARD), src)) return false;
    bool rop = MISC(RVK_M_ROP_EN) != 0u && (sf.flags & RVK_CF_FLOAT) == 0u;
    bool need_dst = blend || rop || chan_mask != 15u;
    uint addr = surf_addr(sf, x, y);
    bool g = (sf.flags & RVK_CF_GART) != 0u;
    bool wide = sf.bpp > 4u;
    uint nw = sf.bpp / 4u;
    if (wide) rop = false;
    uint dbits = 0u;
    uvec4 dw = uvec4(0u);
    vec4 dst = vec4(0.0, 0.0, 0.0, 1.0);
    if (need_dst) {
        if (wide) {
            [[dont_unroll]] for (uint i = 0u; i < nw; i++) dw[i] = rd32(g, addr + 4u * i);
            dst = color_unpack_wide(sf.fmt, dw);
        } else {
            dbits = rdn(g, addr, sf.bpp);
            dst = color_unpack(sf.fmt, dbits);
        }
    }
    vec4 res = src;
    if (rop) {
        uint sbits = color_pack(sf.fmt, src);
        uint v = rop_apply(MISC(RVK_M_ROP), sbits, dbits);
        res = color_unpack(sf.fmt, v);
    } else if (blend) {
        bool isf = (sf.flags & RVK_CF_FLOAT) != 0u;
        vec4 k = D.blend_const;
        for (int c = 0; c < 3; c++)
            res[c] = blend_channel(MISC(RVK_M_RGB_EQ), MISC(RVK_M_RGB_SRC), MISC(RVK_M_RGB_DST), c, src, dst, k, isf);
        res.w = blend_channel(MISC(RVK_M_A_EQ), MISC(RVK_M_A_SRC), MISC(RVK_M_A_DST), 3, src, dst, k, isf);
    }
    for (int c = 0; c < 4; c++) if (((chan_mask >> c) & 1u) == 0u) res[c] = dst[c];
    if (wide) {
        uvec4 o = color_pack_wide(sf.fmt, res);
        [[dont_unroll]] for (uint i = 0u; i < nw; i++) wr32(g, addr + 4u * i, o[i]);
    } else {
        wrn(g, addr, sf.bpp, color_pack(sf.fmt, res));
    }
    return true;
}

#ifdef RVK_HAS_F64
uint depth_to_fixed(float z, bool z16)
{
    double zf = double(z) * double(MISCF(RVK_M_DEPTH_SCALE)) + double(MISCF(RVK_M_DEPTH_OFFSET));
    if (z16) zf = clamp(zf / 256.0, 0.0lf, 65535.0lf);
    else zf = clamp(zf, 0.0lf, 16777215.0lf);
    return uint(zf + 0.5lf);
}
#else
/*
 * No shaderFloat64 (Apple GPUs through MoltenVK): z * scale + offset is
 * carried as an unevaluated float sum hi + lo (an exact two-product and
 * two-sum), which holds the double result of the path above exactly, then
 * rounded the same way.  Needs IEEE float math (MoltenVK fast math off).
 */
uint depth_to_fixed(float z, bool z16)
{
    precise float sc = MISCF(RVK_M_DEPTH_SCALE), off = MISCF(RVK_M_DEPTH_OFFSET);
    precise float p = z * sc;
    precise float pe = fma(z, sc, -p);              /* z * sc == p + pe */
    precise float s = p + off;
    precise float bb = s - p;
    precise float se = (p - (s - bb)) + (off - bb); /* p + off == s + se */
    precise float hi = s, lo = se + pe;
    float lim = 16777215.0;
    if (z16) {
        hi *= 1.0 / 256.0;
        lo *= 1.0 / 256.0;
        lim = 65535.0;
    }
    if (hi + lo <= 0.0) return 0u;
    if (hi + lo >= lim) return uint(lim);
    /* round half up: n = floor(hi + lo + 0.5), fixed up from the remainder */
    precise float n = floor(hi + 0.5);
    precise float r = ((hi - n) + 0.5) + lo;        /* hi - n is exact */
    if (r < 0.0) n -= 1.0;
    else if (r >= 1.0) n += 1.0;
    return uint(n);
}
#endif

bool pixel_visible(int x, int y)
{
    if (x < int(MISC(RVK_M_SC_X0)) || x > int(MISC(RVK_M_SC_X1)) ||
        y < int(MISC(RVK_M_SC_Y0)) || y > int(MISC(RVK_M_SC_Y1))) return false;
    uint code = 0u;
    for (uint i = 0u; i < 4u; i++) {
        if (x >= int(MISC(RVK_M_CL_X0 + i)) && x <= int(MISC(RVK_M_CL_X1 + i)) &&
            y >= int(MISC(RVK_M_CL_Y0 + i)) && y <= int(MISC(RVK_M_CL_Y1 + i))) code |= 1u << i;
    }
    return ((MISC(RVK_M_CLIP_RULE) >> code) & 1u) != 0u;
}

void main()
{
    for (int i = 0; i < 4; i++) us_out[i] = vec4(0.0);
    us_depth = 0.0;
    us_depth_written = false;
    us_killed = false;
    us_run();

    int px = int(gl_FragCoord.x), py = int(gl_FragCoord.y);
    bool live = !gl_HelperInvocation && !us_killed && pixel_visible(px, py);
    float z = us_depth_written ? us_depth : v_z;
    bool front = v_misc.y != 0u;

    beginInvocationInterlockARB();
    if (live) {
        bool ok = true;
        if (MISC(RVK_M_ALPHA_TEST) != 0u &&
            !alpha_compare(MISC(RVK_M_ALPHA_FUNC), us_out[0].w, MISCF(RVK_M_ALPHA_REF))) ok = false;
        Surf zb = D.zb;
        bool zs_active = ok && (zb.flags & RVK_CF_ENABLED) != 0u &&
                         (MISC(RVK_M_Z_EN) != 0u || MISC(RVK_M_S_EN) != 0u);
        if (zs_active) {
            bool z16 = (zb.flags & RVK_ZF_Z16) != 0u;
            bool do_stencil = MISC(RVK_M_S_EN) != 0u && (zb.flags & RVK_ZF_STENCIL) != 0u;
            uint face = front ? 0u : 1u;
            uint zaddr = surf_addr(zb, uint(px), uint(py));
            bool zg = (zb.flags & RVK_CF_GART) != 0u;
            uint raw = rdn(zg, zaddr, zb.bpp);
            uint zold = z16 ? (raw & 0xffffu) : (raw >> 8), sold = z16 ? 0u : (raw & 255u);
            uint snew = sold, znew = 0u;
            uint ref = MISC(RVK_M_S_REF0 + face), smask = MISC(RVK_M_S_MASK0 + face);
            uint wmask = MISC(RVK_M_S_WMASK0 + face);
            bool zpass = true;
            bool done = false;
            if (do_stencil && !zs_compare(MISC(RVK_M_S_FUNC0 + face), ref & smask, sold & smask)) {
                snew = stencil_op(MISC(RVK_M_S_FAIL0 + face), sold, ref);
                snew = (sold & ~wmask) | (snew & wmask);
                if (snew != sold) wrn(zg, zaddr, zb.bpp, z16 ? zold : ((zold << 8) | snew));
                ok = false;
                done = true;
            }
            if (!done) {
                if (MISC(RVK_M_Z_EN) != 0u) {
                    znew = depth_to_fixed(z, z16);
                    zpass = zs_compare(MISC(RVK_M_Z_FUNC), znew, zold);
                }
                if (do_stencil) {
                    snew = stencil_op(zpass ? MISC(RVK_M_S_ZPASS0 + face) : MISC(RVK_M_S_ZFAIL0 + face), sold, ref);
                    snew = (sold & ~wmask) | (snew & wmask);
                }
                if (!zpass) {
                    if (snew != sold) wrn(zg, zaddr, zb.bpp, z16 ? zold : ((zold << 8) | snew));
                    ok = false;
                } else if (MISC(RVK_M_Z_EN) != 0u && MISC(RVK_M_Z_WRITE) != 0u) {
                    if (znew != zold || snew != sold) wrn(zg, zaddr, zb.bpp, z16 ? znew : ((znew << 8) | snew));
                } else if (snew != sold) {
                    wrn(zg, zaddr, zb.bpp, z16 ? zold : ((zold << 8) | snew));
                }
            }
        }
        if (ok) {
            atomicAdd(zpass_count[MISC(RVK_M_ZPASS_SLOT)], 1u);
            bool mw = MISC(RVK_M_MULTIWRITES) > 1u;
            [[dont_unroll]] for (int n = 0; n < 4; n++) {
                if ((D.cb[n].flags & RVK_CF_ENABLED) != 0u) {
                    write_color(n, uint(px), uint(py), us_out[mw ? 0 : n]);
                }
            }
        }
    }
    endInvocationInterlockARB();
}
