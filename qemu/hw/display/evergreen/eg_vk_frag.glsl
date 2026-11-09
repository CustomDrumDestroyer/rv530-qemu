/*
 * Evergreen pixel pipeline on the host GPU (eg_vk.h).  The C side
 * prepends "#version" and the EVK_* defines and puts the translated pixel
 * shader (eg_vk_ps.c: "void ps_run()") in place of the program marker.
 *
 * Semantics follow the CPU reference path: eg_tile.c (tiling), eg_tex.c
 * (formats, sampling, vertex fetch) and eg_draw.c (shade_quad, db_test,
 * cb_write).  Edit this file, then run scripts/gen_vk_glsl.py.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
/*
 * With EVK_VS defined, the same source is the vertex shader of a draw
 * whose VS runs on the GPU: the translated VS (eg_vk_ps.c, "vs_run()")
 * takes the place of the program, and the fragment-only parts drop out.
 * With EVK_CS defined, it is a compute shader: the translated compute
 * program (eg_vk_ps.c, "cs_run()") runs once for each guest thread, and
 * writes its results through MEM_RAT (rat_store()).
 */
#extension GL_EXT_control_flow_attributes : require
#ifdef EVK_CS
layout(local_size_x = 64) in;
#define EVK_WR coherent
#elif !defined(EVK_VS)
#extension GL_ARB_fragment_shader_interlock : require
#extension GL_KHR_shader_subgroup_basic : require
#extension GL_KHR_shader_subgroup_quad : require
#extension GL_EXT_fragment_shader_barycentric : require

layout(pixel_interlock_ordered) in;
#define EVK_WR coherent
#else
#define EVK_WR readonly
#endif

struct Tile {
    uint mode, bpp, pitch, height;
    uint pipes, pipe_interleave, pi_bits, pipe_bits;
    uint bank_bits, banks, bank_width, bank_height;
    uint split, tile_split, micro_bytes, tiles_per_row;
    uint mt_pitch_shift, mt_height_shift, mt_bytes, mt_per_row;
    uint slice_bytes, bank_x_shift, bank_y_shift, display;
};

struct Tex {
    uint flags, fmt, numf, comp, dsel, dim, width, height, depth;
    uint base_level, last_level, base_array, last_array, bpe, pad0, pad1;
    Tile tile;
    uint level_addr[16];
    uint level_pm[16];
};

struct Surf {
    uint base, flags, fmt, number, chmap, bpp, nch, blend, mask, width, height, bits;
    uint wbase, pad0, pad1, pad2;
    Tile tile;
};

struct Draw {
    uvec4 kc[EVK_MAX_KC * 32];
    uvec4 cfile[256];
    Tex tex[EVK_MAX_TEX];
    uvec4 smp[EVK_MAX_SMP];
    uvec4 vbuf[EVK_MAX_VBUF * 2];
    Surf cb[8];
    Surf zb, sb;
    vec4 blend_const;
    uvec4 loopc[8];
    uint misc[EVK_MISC_WORDS];
    uvec4 vs_kc[EVK_MAX_KC * 32];
    uvec4 vs_cfile[256];
    uvec4 vs_vbuf[EVK_MAX_VS_VBUF * 2];
    uvec4 vs_loopc[8];
};

layout(std430, set = 0, binding = 0) EVK_WR buffer VramBuf { uint vram[]; };
layout(std430, set = 0, binding = 1) readonly buffer DrawBuf { Draw draws[]; };
#if !defined(EVK_VS) && !defined(EVK_CS)
layout(std430, set = 0, binding = 3) buffer CountBuf { uint zpass_count[]; };
#endif
layout(std430, set = 0, binding = 4) EVK_WR buffer SysBuf { uint sysram[]; };
layout(std430, set = 0, binding = 5) readonly buffer GartBuf { uint gart[]; };
/* read-only, cacheable aliases of the same memory for fetches (barriers
 * between draws make them safe) */
layout(std430, set = 0, binding = 6) restrict readonly buffer VramTex { uint vram_tex[]; };
layout(std430, set = 0, binding = 7) restrict readonly buffer SysTex { uint sysram_tex[]; };
layout(push_constant) uniform PC { uint draw; } pc;
#if !defined(EVK_VS) && defined(EVK_MARK_DIRTY)
/* one bit per 4 KiB page of the VRAM copy that a fragment wrote: the
 * copy-back at the end of the batch copies only these pages */
layout(std430, set = 0, binding = 8) buffer DirtyBuf { uint vram_dirty[]; };
#endif

#ifdef EVK_CS
/* the thread: its index in the group and its group (R0.xyz, R1.xyz) */
uvec3 cs_l, cs_g;
#elif !defined(EVK_VS)
/*
 * The PS inputs of the triangle's three vertices, in submission order.
 * One array per input: with a single "v_p[][EVK_MAX_PARAMS]" some drivers
 * (NVIDIA) hand back interpolated values, not the vertex's, for n > 0.
 * EVK_MAX_PARAMS is 12.
 */
layout(location = 0) pervertexEXT in vec4 v_p0[];
layout(location = 1) pervertexEXT in vec4 v_p1[];
layout(location = 2) pervertexEXT in vec4 v_p2[];
layout(location = 3) pervertexEXT in vec4 v_p3[];
layout(location = 4) pervertexEXT in vec4 v_p4[];
layout(location = 5) pervertexEXT in vec4 v_p5[];
layout(location = 6) pervertexEXT in vec4 v_p6[];
layout(location = 7) pervertexEXT in vec4 v_p7[];
layout(location = 8) pervertexEXT in vec4 v_p8[];
layout(location = 9) pervertexEXT in vec4 v_p9[];
layout(location = 10) pervertexEXT in vec4 v_p10[];
layout(location = 11) pervertexEXT in vec4 v_p11[];
layout(location = EVK_MAX_PARAMS) flat in uint v_front;
layout(location = EVK_MAX_PARAMS + 1) noperspective in float v_z;
#else
/* to the fragment shader: v_p<n>, v_front, v_z */
layout(location = 0) out vec4 o_p[EVK_MAX_PARAMS];
layout(location = EVK_MAX_PARAMS) flat out uint o_front;
layout(location = EVK_MAX_PARAMS + 1) noperspective out float o_z;
uvec4 EPOS[4];          /* VS exports POS 60-63 */
uvec4 EPAR[32];         /* PARAM 0-31 */
#endif

#define D draws[pc.draw]

/*
 * Per-draw state that the code branches on.  By default it comes from D.
 * A specialized pipeline (eg_vk.c, spec_defs) defines these first, with
 * the values of its draw as constants for the slots it uses, so that the
 * compiler removes the branches that the draw does not take.
 */
#ifndef TEX_FMT
#define TEX_FMT(s) D.tex[s].fmt
#endif
#ifndef TEX_NUMF
#define TEX_NUMF(s) D.tex[s].numf
#endif
#ifndef TEX_COMP
#define TEX_COMP(s) D.tex[s].comp
#endif
#ifndef TEX_DSEL
#define TEX_DSEL(s) D.tex[s].dsel
#endif
#ifndef TEX_DIM
#define TEX_DIM(s) D.tex[s].dim
#endif
#ifndef TEX_FLAGS
#define TEX_FLAGS(s) D.tex[s].flags
#endif
#ifndef TEX_TMODE
#define TEX_TMODE(s) D.tex[s].tile.mode
#endif
#ifndef CB_FMT
#define CB_FMT(n) D.cb[n].fmt
#endif
#ifndef CB_BPP
#define CB_BPP(n) D.cb[n].bpp
#endif
#ifndef CB_FLAGS
#define CB_FLAGS(n) D.cb[n].flags
#endif
#ifndef CB_NUMBER
#define CB_NUMBER(n) D.cb[n].number
#endif
#ifndef CB_CHMAP
#define CB_CHMAP(n) D.cb[n].chmap
#endif
#ifndef CB_BLEND
#define CB_BLEND(n) D.cb[n].blend
#endif
#ifndef CB_BITS
#define CB_BITS(n) D.cb[n].bits
#endif
#ifndef CB_NCH
#define CB_NCH(n) D.cb[n].nch
#endif
#ifndef CB_MASK
#define CB_MASK(n) D.cb[n].mask
#endif
#ifndef CB_TMODE
#define CB_TMODE(n) D.cb[n].tile.mode
#endif
#ifndef SMP_X
#define SMP_X(i) D.smp[i].x
#endif
#ifndef SMP_Y
#define SMP_Y(i) D.smp[i].y
#endif
#ifndef SMP_Z
#define SMP_Z(i) D.smp[i].z
#endif

Tile cb_tile(uint n)
{
    Tile t = D.cb[n].tile;
    t.mode = CB_TMODE(n);
    return t;
}
#define MISC(i) D.misc[i]
#define MISCF(i) uintBitsToFloat(D.misc[i])
#define F(x) uintBitsToFloat(x)
#define U(x) floatBitsToUint(x)

/* outputs of ps_run() */
uvec4 E[8];             /* PIXEL exports 0-7 */
uint emask;
uint edepth;            /* PIXEL 61 (MRTZ) .x */
bool edepth_written;
bool alive;             /* not killed */

/* ------------------------------------------------------------------ */
/* GPU memory                                                          */
/* ------------------------------------------------------------------ */

/*
 * Addresses are VRAM byte offsets, or GART byte offsets when 'g' is set:
 * the table maps each GART page to a guest RAM page, or (bit 31) to a
 * VRAM page.
 */
uint gentry(uint a) { return gart[a >> 12]; }
uint gword(uint e, uint a) { return ((e & 0x7fffffffu) << 10) | ((a & 4095u) >> 2); }
uint rd32(bool g, uint a)
{
    if (!g) return vram[a >> 2];
    uint e = gentry(a);
    return (e & 0x80000000u) != 0u ? vram[gword(e, a)] : sysram[gword(e, a)];
}
uint rd32_tex(bool g, uint a)
{
    if (!g) return vram_tex[a >> 2];
    uint e = gentry(a);
    return (e & 0x80000000u) != 0u ? vram_tex[gword(e, a)] : sysram_tex[gword(e, a)];
}
#ifndef EVK_VS
/* vram word index w changes */
void mark(uint w)
{
#ifdef EVK_MARK_DIRTY
    uint i = w >> 15, b = 1u << ((w >> 10) & 31u);
    if ((vram_dirty[i] & b) == 0u) atomicOr(vram_dirty[i], b);
#endif
}
void wr32(bool g, uint a, uint v)
{
    if (!g) { vram[a >> 2] = v; mark(a >> 2); return; }
    uint e = gentry(a);
    if ((e & 0x80000000u) != 0u) { vram[gword(e, a)] = v; mark(gword(e, a)); }
    else sysram[gword(e, a)] = v;
}
void wr32_masked(bool g, uint a, uint m, uint v)
{
    if (m == 0xffffffffu) { wr32(g, a, v); return; }
    if (!g) {
        atomicAnd(vram[a >> 2], ~m);
        atomicOr(vram[a >> 2], v & m);
        mark(a >> 2);
        return;
    }
    uint e = gentry(a), i = gword(e, a);
    if ((e & 0x80000000u) != 0u) {
        atomicAnd(vram[i], ~m);
        atomicOr(vram[i], v & m);
        mark(i);
    } else {
        atomicAnd(sysram[i], ~m);
        atomicOr(sysram[i], v & m);
    }
}
/* 1, 2 or 4 naturally aligned bytes */
uint rdn(bool g, uint a, uint bytes)
{
    uint w = rd32(g, a);
    if (bytes >= 4u) return w;
    uint sh = (a & 3u) * 8u;
    return (w >> sh) & (bytes == 2u ? 0xffffu : 0xffu);
}
void wrn(bool g, uint a, uint bytes, uint v)
{
    if (bytes >= 4u) { wr32(g, a, v); return; }
    uint sh = (a & 3u) * 8u;
    uint m = (bytes == 2u ? 0xffffu : 0xffu) << sh;
    wr32_masked(g, a & ~3u, m, v << sh);
}
#endif
/* n (<= 16) bytes at any alignment, little endian, through the fetch alias */
uvec4 rd_elem(bool g, uint a, uint n)
{
    uint a0 = a & ~3u, sh = (a & 3u) * 8u, nw = (n + 3u) >> 2;
    uvec4 o = uvec4(0u);
    if (sh == 0u) {
        [[dont_unroll]] for (uint i = 0u; i < nw; i++) o[i] = rd32_tex(g, a0 + 4u * i);
        return o;
    }
    uint prev = rd32_tex(g, a0);
    [[dont_unroll]] for (uint i = 0u; i < nw; i++) {
        uint next = (4u * i + 4u < (a & 3u) + n) ? rd32_tex(g, a0 + 4u * i + 4u) : 0u;
        o[i] = (prev >> sh) | (next << (32u - sh));
        prev = next;
    }
    return o;
}

/* bits [sh, sh + w) of a 128-bit little-endian value, w <= 32 */
uint bits128(uvec4 v, uint sh, uint w)
{
    uint i = sh >> 5, s = sh & 31u;
    uint r = v[i] >> s;
    if (s != 0u && i < 3u) r |= v[i + 1u] << (32u - s);
    return w >= 32u ? r : (r & ((1u << w) - 1u));
}
uvec4 setbits128(uvec4 v, uint sh, uint w, uint x)
{
    uint i = sh >> 5, s = sh & 31u;
    uint m = w >= 32u ? 0xffffffffu : ((1u << w) - 1u);
    x &= m;
    v[i] = (v[i] & ~(m << s)) | (x << s);
    if (s != 0u && s + w > 32u && i < 3u) {
        uint m2 = m >> (32u - s);
        v[i + 1u] = (v[i + 1u] & ~m2) | (x >> (32u - s));
    }
    return v;
}

/* ------------------------------------------------------------------ */
/* Tiling (eg_tile.c)                                                  */
/* ------------------------------------------------------------------ */

#define BIT(v, n) (((v) >> (n)) & 1u)

uint micro_index(uint x, uint y, uint bits, bool display)
{
    uint x0 = BIT(x, 0), x1 = BIT(x, 1), x2 = BIT(x, 2);
    uint y0 = BIT(y, 0), y1 = BIT(y, 1), y2 = BIT(y, 2);
    if (!display) return x0 | y0 << 1 | x1 << 2 | y1 << 3 | x2 << 4 | y2 << 5;
    if (bits == 8u) return x0 | x1 << 1 | x2 << 2 | y1 << 3 | y0 << 4 | y2 << 5;
    if (bits == 16u) return x0 | x1 << 1 | x2 << 2 | y0 << 3 | y1 << 4 | y2 << 5;
    if (bits == 64u) return x0 | y0 << 1 | x1 << 2 | x2 << 3 | y1 << 4 | y2 << 5;
    if (bits == 128u) return y0 | x0 << 1 | x1 << 2 | x2 << 3 | y1 << 4 | y2 << 5;
    return x0 | x1 << 1 | y0 << 2 | x2 << 3 | y1 << 4 | y2 << 5;
}

uint pipe_of(uint x, uint y, uint pipes)
{
    uint tx = x >> 3, ty = y >> 3;
    uint x3 = BIT(tx, 0), x4 = BIT(tx, 1), x5 = BIT(tx, 2);
    uint y3 = BIT(ty, 0), y4 = BIT(ty, 1), y5 = BIT(ty, 2);
    if (pipes == 1u) return 0u;
    if (pipes == 2u) return x3 ^ y3;
    if (pipes == 4u) return (x3 ^ y3 ^ x4) | (x4 ^ y4) << 1;
    return (x3 ^ y3 ^ x4) | (x4 ^ y4) << 1 | (x5 ^ y5) << 2;
}

uint bank_of(Tile t, uint x, uint y, uint ss)
{
    uint tx = x >> t.bank_x_shift, ty = y >> t.bank_y_shift;
    uint x3 = BIT(tx, 0), x4 = BIT(tx, 1), x5 = BIT(tx, 2), x6 = BIT(tx, 3);
    uint y3 = BIT(ty, 0), y4 = BIT(ty, 1), y5 = BIT(ty, 2), y6 = BIT(ty, 3);
    uint b;
    if (t.banks == 16u) b = (x3 ^ y6) | (x4 ^ y5 ^ y6) << 1 | (x5 ^ y4) << 2 | (x6 ^ y3) << 3;
    else if (t.banks == 8u) b = (x3 ^ y5) | (x4 ^ y4 ^ y5) << 1 | (x5 ^ y3) << 2;
    else if (t.banks == 4u) b = (x3 ^ y4) | (x4 ^ y3) << 1;
    else b = x3 ^ y3;
    b ^= (t.banks / 2u + 1u) * ss;
    return b & (t.banks - 1u);
}

uint tile_offset(Tile t, uint x, uint y)
{
    uint elem = micro_index(x & 7u, y & 7u, t.bpp * 8u, t.display != 0u) * t.bpp;
    if (t.mode == 2u) {
        return ((y >> 3) * t.tiles_per_row + (x >> 3)) * t.micro_bytes + elem;
    }
    if (t.mode != 4u) return (y * t.pitch + x) * t.bpp;
    uint ss = 0u;
    if (t.split != 0u) {
        ss = elem / t.tile_split;
        elem %= t.tile_split;
    }
    uint tile_index = ((y >> 3) & (t.bank_height - 1u)) * t.bank_width +
                      (((x >> 3) >> t.pipe_bits) & (t.bank_width - 1u));
    uint total = t.slice_bytes * ss +
                 ((y >> t.mt_height_shift) * t.mt_per_row + (x >> t.mt_pitch_shift)) * t.mt_bytes +
                 elem + tile_index * t.micro_bytes;
    uint p = pipe_of(x, y, t.pipes), b = bank_of(t, x, y, ss);
    return (total & (t.pipe_interleave - 1u)) | p << t.pi_bits |
           b << (t.pi_bits + t.pipe_bits) |
           (total >> t.pi_bits) << (t.pi_bits + t.pipe_bits + t.bank_bits);
}

uint round_up(uint v, uint a) { return (v + a - 1u) / a * a; }

uint tile_size(Tile t)
{
    if (t.mode == 4u) {
        return round_up(t.pitch, 1u << t.mt_pitch_shift) *
               round_up(t.height, 1u << t.mt_height_shift) * t.bpp;
    }
    if (t.mode == 2u) return t.pitch * round_up(t.height, 8u) * t.bpp;
    return t.pitch * t.height * t.bpp;
}

/* ------------------------------------------------------------------ */
/* Formats (eg_format_decode)                                          */
/* ------------------------------------------------------------------ */

/* bytes | channels << 8 | kind << 12 (0 integer, 1 float, 2 small float) */
const uint FMT_INFO[64] = uint[](
    0x000u, 0x101u, 0x201u, 0x301u, 0x000u, 0x102u, 0x1102u, 0x202u,
    0x302u, 0x302u, 0x402u, 0x402u, 0x402u, 0x104u, 0x1104u, 0x204u,
    0x1204u, 0x204u, 0x204u, 0x204u, 0x204u, 0x304u, 0x2304u, 0x304u,
    0x2304u, 0x404u, 0x404u, 0x404u, 0x208u, 0x208u, 0x1208u, 0x408u,
    0x1408u, 0x000u, 0x410u, 0x1410u, 0x000u, 0x000u, 0x000u, 0x304u,
    0x304u, 0x000u, 0x000u, 0x000u, 0x303u, 0x306u, 0x1306u, 0x30cu,
    0x130cu, 0x000u, 0x000u, 0x000u, 0x000u, 0x000u, 0x000u, 0x000u,
    0x000u, 0x000u, 0x000u, 0x000u, 0x000u, 0x000u, 0x000u, 0x000u);
/* channel widths, channel 0 in the low byte */
const uint FMT_BITS[64] = uint[](
    0u, 0x08u, 0x0404u, 0x030302u, 0u, 0x10u, 0x10u, 0x0808u,
    0x050605u, 0x060505u, 0x01050505u, 0x04040404u, 0x05050501u, 0x20u, 0x20u, 0x1010u,
    0x1010u, 0x0818u, 0x0818u, 0x1808u, 0x1808u, 0x0a0b0bu, 0x0a0b0bu, 0x0b0b0au,
    0x0b0b0au, 0x020a0a0au, 0x08080808u, 0x0a0a0a02u, 0x2020u, 0x2020u, 0x2020u, 0x10101010u,
    0x10101010u, 0u, 0x20202020u, 0x20202020u, 0u, 0u, 0u, 0u,
    0u, 0u, 0u, 0u, 0x080808u, 0x101010u, 0x101010u, 0x202020u,
    0x202020u, 0u, 0u, 0u, 0u, 0u, 0u, 0u,
    0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u);

bool fmt_is_bc(uint f) { return f >= 0x31u && f <= 0x35u; }
/* GB_GR, BG_RG: two pixels per 32-bit element, a 2x1 block (eg_tex.c) */
bool fmt_is_422(uint f) { return f == 0x27u || f == 0x28u; }
uint fmt_bytes(uint f)
{
    if (fmt_is_bc(f)) return (f == 0x31u || f == 0x34u) ? 8u : 16u;
    return FMT_INFO[f & 63u] & 255u;
}

float half_to_float(uint h)
{
    uint s = (h >> 15) & 1u, e = (h >> 10) & 31u, m = h & 0x3ffu;
    float v;
    if (e == 0u) v = ldexp(float(m), -24);
    else if (e == 31u) v = m != 0u ? uintBitsToFloat(0x7fc00000u) : uintBitsToFloat(0x7f800000u);
    else v = ldexp(float(m | 0x400u), int(e) - 25);
    return s != 0u ? -v : v;
}
uint float_to_half(float f)
{
    uint u = U(f), s = (u >> 16) & 0x8000u;
    int e = int((u >> 23) & 255u) - 127 + 15;
    uint m = u & 0x7fffffu;
    if (isnan(f)) return 0x7e00u;
    if (e >= 31) return s | 0x7c00u;
    if (e <= 0) {
        if (e < -10) return s;
        m = (m | 0x800000u) >> uint(1 - e);
        return s | ((m + 0x1000u) >> 13);
    }
    return s | (uint(e) << 10) | ((m + 0x1000u) >> 13);
}
float ufloat_to_float(uint v, uint mbits)
{
    uint e = v >> mbits, m = v & ((1u << mbits) - 1u);
    if (e == 0u) return ldexp(float(m), -14 - int(mbits));
    if (e == 31u) return m != 0u ? uintBitsToFloat(0x7fc00000u) : uintBitsToFloat(0x7f800000u);
    return ldexp(float(m | (1u << mbits)), int(e) - 15 - int(mbits));
}
float srgb_to_linear(float c)
{
    return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4);
}
float linear_to_srgb(float c)
{
    c = clamp(c, 0.0, 1.0);
    return c <= 0.0031308 ? c * 12.92 : 1.055 * pow(c, 1.0 / 2.4) - 0.055;
}

/* one element; numf 0 NORM, 1 INT, 2 SCALED; comp: 4 x 2 bits signedness.
 * Returns channel bits (floats, or integers for INT). */
uvec4 format_decode(uint fmt, uvec4 p, uint numf, uint comp)
{
    fmt &= 63u;
    uvec4 o = uvec4(0u, 0u, 0u, U(1.0));
    if (numf == 1u) o.w = 1u;
    if (fmt == 0x1cu) {                     /* X24_8_32_FLOAT */
        o.x = p.x;
        o.y = U(float(p.y & 255u));
        return o;
    }
    uint info = FMT_INFO[fmt], bytes = info & 255u, nch = (info >> 8) & 15u, kind = info >> 12;
    uint shift = 0u;
    [[dont_unroll]] for (uint c = 0u; c < nch; c++) {
        uint w = (FMT_BITS[fmt] >> (8u * c)) & 255u;
        uint raw = bits128(p, shift, w);
        shift += w;
        if (kind == 1u) {
            o[c] = w == 16u ? U(half_to_float(raw)) : raw;
            continue;
        }
        if (kind == 2u) {
            o[c] = U(ufloat_to_float(raw, w == 10u ? 5u : 6u));
            continue;
        }
        uint sg = (comp >> (2u * c)) & 3u;
        int sv = w >= 32u ? int(raw) : (int(raw << (32u - w)) >> (32u - w));
        if (numf == 1u) {
            o[c] = sg == 1u ? uint(sv) : raw;
        } else if (numf == 2u) {
            o[c] = U(sg == 1u ? float(sv) : float(raw));
        } else if (sg == 1u) {
            double d = double(sv) / double((1u << (w - 1u)) - 1u);
            o[c] = U(float(max(d, -1.0lf)));
        } else {
            double maxu = w >= 32u ? 4294967295.0lf : double((1u << w) - 1u);
            double d = double(raw) / maxu;
            o[c] = U(float(sg == 2u ? d * 2.0lf - 1.0lf : d));
        }
    }
    return o;
}

uint byte_of(uvec4 w, uint i) { return (w[i >> 2] >> ((i & 3u) * 8u)) & 255u; }

/* BC4 / BC5 / BC3 alpha style 3-bit block at byte b of the block */
float bc_alpha(uvec4 blk, uint b, uint i)
{
    float a0 = float(byte_of(blk, b)) / 255.0, a1 = float(byte_of(blk, b + 1u)) / 255.0;
    uint code = bits128(blk, (b + 2u) * 8u + 3u * i, 3u);
    if (code == 0u) return a0;
    if (code == 1u) return a1;
    if (byte_of(blk, b) > byte_of(blk, b + 1u)) {
        return (float(8u - code) * a0 + float(code - 1u) * a1) / 7.0;
    }
    if (code == 6u) return 0.0;
    if (code == 7u) return 1.0;
    return (float(6u - code) * a0 + float(code - 1u) * a1) / 5.0;
}

vec4 decode_bc(uint fmt, uvec4 blk, uint px, uint py)
{
    uint i = py * 4u + px;
    vec4 o = vec4(0.0, 0.0, 0.0, 1.0);
    if (fmt == 0x34u || fmt == 0x35u) {
        o.x = bc_alpha(blk, 0u, i);
        if (fmt == 0x35u) o.y = bc_alpha(blk, 8u, i);
        return o;
    }
    uint cb = fmt == 0x31u ? 0u : 8u;
    uint c0 = byte_of(blk, cb) | byte_of(blk, cb + 1u) << 8;
    uint c1 = byte_of(blk, cb + 2u) | byte_of(blk, cb + 3u) << 8;
    uint idx = (byte_of(blk, cb + 4u + (i >> 2)) >> ((i & 3u) * 2u)) & 3u;
    vec3 k0c = vec3(float((c0 >> 11) & 31u) / 31.0, float((c0 >> 5) & 63u) / 63.0, float(c0 & 31u) / 31.0);
    vec3 k1c = vec3(float((c1 >> 11) & 31u) / 31.0, float((c1 >> 5) & 63u) / 63.0, float(c1 & 31u) / 31.0);
    bool four = c0 > c1 || fmt != 0x31u;
    float k0, k1;
    if (idx == 0u) { k0 = 1.0; k1 = 0.0; }
    else if (idx == 1u) { k0 = 0.0; k1 = 1.0; }
    else if (idx == 2u) { k0 = four ? 2.0 / 3.0 : 0.5; k1 = 1.0 - k0; }
    else if (four) { k0 = 1.0 / 3.0; k1 = 2.0 / 3.0; }
    else { k0 = 0.0; k1 = 0.0; o.w = 0.0; }
    o.xyz = k0c * k0 + k1c * k1;
    if (fmt == 0x32u) {
        o.w = float((byte_of(blk, i >> 1) >> ((i & 1u) * 4u)) & 15u) / 15.0;
    } else if (fmt == 0x33u) {
        o.w = bc_alpha(blk, 0u, i);
    }
    return o;
}

/* ------------------------------------------------------------------ */
/* Textures (eg_tex.c)                                                 */
/* ------------------------------------------------------------------ */

/* the tiling of level lv (level 0: as set up; the rest re-derived) */
Tile tex_tile(uint s, uint lv)
{
    Tile t = D.tex[s].tile;
    t.mode = TEX_TMODE(s);
    if (lv == 0u) return t;
    uint pm = D.tex[s].level_pm[lv];
    uint h = max(D.tex[s].height >> lv, 1u);
    if (fmt_is_bc(TEX_FMT(s))) h = (h + 3u) / 4u;
    t.pitch = pm & 0x0fffffffu;
    t.mode = pm >> 28;
    t.height = h;
    t.split = (t.mode == 4u && 64u * t.bpp > t.tile_split) ? 1u : 0u;
    t.micro_bytes = t.split != 0u ? t.tile_split : 64u * t.bpp;
    t.tiles_per_row = max(t.pitch / 8u, 1u);
    uint mtp = 1u << t.mt_pitch_shift, mth = 1u << t.mt_height_shift;
    t.mt_bytes = t.micro_bytes * (mtp / 8u) * (mth / 8u) / (t.pipes * t.banks);
    t.mt_per_row = t.pitch / mtp;
    t.slice_bytes = t.mt_per_row * (t.height / mth) * t.mt_bytes;
    return t;
}

bool tex_int(uint s) { return TEX_NUMF(s) == 1u; }

/* one texel of a level: raw channels (before DST_SEL) */
uvec4 texel(uint s, uint lv, Tile tl, int x, int y, int slice)
{
    uint fmt = TEX_FMT(s);
    bool bc = fmt_is_bc(fmt);
    uint bpe = max(fmt_bytes(fmt), 1u);
    bool p422 = fmt_is_422(fmt);
    uint bx = uint(bc ? x / 4 : p422 ? x / 2 : x), by = uint(bc ? y / 4 : y);
    uint off = tile_offset(tl, bx, by) + (slice != 0 ? uint(slice) * tile_size(tl) : 0u);
    bool g = (TEX_FLAGS(s) & EVK_TF_GART) != 0u;
    uvec4 raw = rd_elem(g, D.tex[s].level_addr[lv & 15u] + off, bpe);
    uvec4 o;
    if (bc) o = U(decode_bc(fmt, raw, uint(x & 3), uint(y & 3)));
    else if (p422) {
        /* GB_GR: bytes R G0 B G1; BG_RG: bytes G0 R G1 B */
        uint e = raw.x, odd = uint(x & 1);
        bool gb = fmt == 0x27u;
        o = U(vec4(float((e >> (gb ? 0u : 8u)) & 255u),
                   float((e >> (gb ? (odd != 0u ? 24u : 8u) : (odd != 0u ? 16u : 0u))) & 255u),
                   float((e >> (gb ? 16u : 24u)) & 255u), 255.0) / 255.0);
    } else o = format_decode(fmt, raw, TEX_NUMF(s), TEX_COMP(s));
    if ((TEX_FLAGS(s) & EVK_TF_DEGAMMA) != 0u && !tex_int(s)) {
        for (int c = 0; c < 3; c++) o[c] = U(srgb_to_linear(F(o[c])));
    }
    return o;
}

/* returns the wrapped coordinate; border set when outside a border mode */
int wrap(int i, int n, uint mode, out bool border)
{
    border = false;
    if (mode == 0u) {
        i %= n;
        return i < 0 ? i + n : i;
    }
    if (mode == 1u) {
        int p = 2 * n, m = i % p;
        if (m < 0) m += p;
        return m < n ? m : p - 1 - m;
    }
    if (mode == 3u || mode == 5u || mode == 7u) {
        if (i < 0) i = -1 - i;
        if (mode == 7u && i >= n) border = true;
        return min(i, n - 1);
    }
    if (mode == 6u || mode == 4u) {
        if (i < 0 || i >= n) border = true;
        return clamp(i, 0, n - 1);
    }
    return clamp(i, 0, n - 1);
}

uvec4 border_color(uint type)
{
    float c = type == 2u ? 1.0 : 0.0;
    return U(vec4(c, c, c, type == 0u ? 0.0 : 1.0));
}

uvec4 sample_level(uint s, uint ss, uint lv, float u, float v, int slice, bool linear)
{
    Tile tl = tex_tile(s, lv);
    int w = int(max(D.tex[s].width >> lv, 1u)), h = int(max(D.tex[s].height >> lv, 1u));
    uint s0 = SMP_X(ss), cx = s0 & 7u, cy = (s0 >> 3) & 7u, bt = (s0 >> 20) & 3u;
    uint dim = TEX_DIM(s);
    bool bx, by;
    if (dim == 0u || dim == 4u) h = 1;
    if (!linear) {
        int x = wrap(int(floor(u)), w, cx, bx), y = wrap(int(floor(v)), h, cy, by);
        if (bx || by) return border_color(bt);
        return texel(s, lv, tl, x, y, slice);
    }
    float fu = u - 0.5, fv = v - 0.5;
    int x0 = int(floor(fu)), y0 = int(floor(fv));
    float ax = fu - float(x0), ay = fv - float(y0);
    uvec4 t[4];
    [[dont_unroll]] for (int k = 0; k < 4; k++) {
        int x = wrap(x0 + (k & 1), w, cx, bx), y = wrap(y0 + (k >> 1), h, cy, by);
        t[k] = (bx || by) ? border_color(bt) : texel(s, lv, tl, x, y, slice);
    }
    if (tex_int(s)) return t[0];
    vec4 top = F(t[0]) + (F(t[1]) - F(t[0])) * ax;
    vec4 bot = F(t[2]) + (F(t[3]) - F(t[2])) * ax;
    return U(top + (bot - top) * ay);
}

#if !defined(EVK_VS) && !defined(EVK_CS)
/* quad neighbours: fragment (x, y) parity picks the swap that reaches them */
void quad_corners(vec2 c, out vec2 tl, out vec2 tr, out vec2 bl)
{
    vec2 h = subgroupQuadSwapHorizontal(c), v = subgroupQuadSwapVertical(c);
    vec2 d = subgroupQuadSwapDiagonal(c);
    bool lx = (int(gl_FragCoord.x) & 1) != 0, ly = (int(gl_FragCoord.y) & 1) != 0;
    tl = !lx ? (!ly ? c : v) : (!ly ? h : d);
    tr = lx ? (!ly ? c : v) : (!ly ? h : d);
    bl = !lx ? (ly ? c : v) : (ly ? h : d);
}
/* GET_GRADIENTS_H / _V: the difference along the lane's row / column */
vec4 quad_grad(vec4 c, bool horiz)
{
    vec4 o = horiz ? subgroupQuadSwapHorizontal(c) : subgroupQuadSwapVertical(c);
    bool second = horiz ? (int(gl_FragCoord.x) & 1) != 0 : (int(gl_FragCoord.y) & 1) != 0;
    return second ? c - o : o - c;
}
bool quad_any(bool b)
{
    uint v = b ? 1u : 0u;
    v |= subgroupQuadSwapHorizontal(v);
    v |= subgroupQuadSwapVertical(v);
    return v != 0u;
}
#else
/* a vertex or a compute thread has no quad neighbours: no derivatives */
void quad_corners(vec2 c, out vec2 tl, out vec2 tr, out vec2 bl)
{
    tl = c; tr = c; bl = c;
}
vec4 quad_grad(vec4 c, bool horiz) { return vec4(0.0); }
bool quad_any(bool b) { return b; }
#endif

float tex_compare(uint func, float ref, float v)
{
    switch (func) {
    case 0u: return 0.0;
    case 1u: return ref < v ? 1.0 : 0.0;
    case 2u: return ref == v ? 1.0 : 0.0;
    case 3u: return ref <= v ? 1.0 : 0.0;
    case 4u: return ref > v ? 1.0 : 0.0;
    case 5u: return ref != v ? 1.0 : 0.0;
    case 6u: return ref >= v ? 1.0 : 0.0;
    }
    return 1.0;
}

uvec4 res_dsel(uint s, uvec4 v)
{
    uvec4 o;
    uint ds = TEX_DSEL(s);
    for (int c = 0; c < 4; c++) {
        uint d = (ds >> (3 * c)) & 7u;
        o[c] = d < 4u ? v[d] : d == 4u ? 0u : (tex_int(s) ? 1u : U(1.0));
    }
    return o;
}

/* the gradients of SET_GRADIENTS_H/V */
vec4 grad_h, grad_v;
bool has_grad;

/* TEX instruction: op, coordinates (after SRC_SEL), offsets, bias, the
 * COORD_TYPE (normalized) bits; returns the result before the
 * instruction's DST_SEL.  Called by every lane of a quad together. */
uvec4 tex_fetch(uint s, uint ss, uint op, uvec4 cb, ivec3 ioff, float ibias, uint norm)
{
    vec4 c = F(cb);
    ivec4 ci = ivec4(cb);
    uint dim = TEX_DIM(s), width = D.tex[s].width, height = D.tex[s].height;
    uint base = D.tex[s].base_level, last = D.tex[s].last_level;
    if ((TEX_FLAGS(s) & EVK_TF_VALID) == 0u) return uvec4(0u);
    if (op == 0x04u) {                                  /* GET_RESINFO */
        int lv = clamp(ci.x, 0, 15);
        uvec4 o;
        o.x = max(width >> lv, 1u);
        o.y = dim == 4u ? D.tex[s].last_array + 1u : max(height >> lv, 1u);
        o.z = dim == 2u ? max(D.tex[s].depth >> lv, 1u) :
              D.tex[s].last_array - D.tex[s].base_array + 1u;
        o.w = last - base + 1u;
        return o;
    }
    if (op == 0x03u) {                                  /* LD */
        int x = ci.x + ioff.x, y = ci.y + ioff.y;
        int lv = clamp(int(base) + ci.w, 0, 15);
        Tile tl = tex_tile(s, uint(lv));
        int slice = dim == 4u ? ci.y : dim >= 2u ? ci.z : 0;
        if (dim == 4u) y = 0;
        x = clamp(x, 0, int(max(width >> lv, 1u)) - 1);
        y = clamp(y, 0, int(max(height >> lv, 1u)) - 1);
        return res_dsel(s, texel(s, uint(lv), tl, x, y, max(slice, 0)));
    }
    float u = c.x, v = c.y, lod = 0.0;
    int slice = 0;
    uint s0 = SMP_X(ss), s1 = SMP_Y(ss), s2 = SMP_Z(ss);
    uint mag = (s0 >> 9) & 3u, minf = (s0 >> 11) & 3u, mipf = (s0 >> 15) & 3u;
    float minlod = float(s1 & 0xfffu) / 256.0, maxlod = float((s1 >> 12) & 0xfffu) / 256.0;
    float sbias = float(int(s2 << 18) >> 18) / 256.0;
    bool nx = (norm & 1u) != 0u, ny = (norm & 2u) != 0u, nz = (norm & 4u) != 0u;

    if (dim == 3u) {
        slice = int(c.z);
    } else if (dim == 5u || dim == 4u) {
        slice = int(roundEven(dim == 4u ? v : c.z));
        if (dim == 4u) { v = 0.5; ny = true; }
    } else if (dim == 2u) {
        slice = int(floor(c.z * (nz ? float(D.tex[s].depth) : 1.0)));
    }
    slice = clamp(slice, 0, int(dim == 2u ? D.tex[s].depth : D.tex[s].last_array + 1u) - 1);
    if (nx) u *= float(width);
    if (ny) v *= float(height);
    u += float(ioff.x) * 0.5;
    v += float(ioff.y) * 0.5;

    if (op == 0x11u || op == 0x19u) {                   /* SAMPLE_L */
        lod = c.w;
    } else if (op == 0x13u || op == 0x1bu || op == 0x15u) {     /* _LZ, GATHER4 */
        lod = 0.0;
    } else {
        float dux, dvx, duy, dvy;
        if ((op == 0x14u || op == 0x1cu || op == 0x16u || op == 0x1eu) && has_grad) {
            dux = grad_h.x; dvx = grad_h.y; duy = grad_v.x; dvy = grad_v.y;
        } else {
            vec2 tl, tr, bl;
            quad_corners(c.xy, tl, tr, bl);
            dux = tr.x - tl.x; dvx = tr.y - tl.y;
            duy = bl.x - tl.x; dvy = bl.y - tl.y;
        }
        if (nx) { dux *= float(width); duy *= float(width); }
        if (ny) { dvx *= float(height); dvy *= float(height); }
        float rho = max(dux * dux + dvx * dvx, duy * duy + dvy * dvy);
        lod = rho > 0.0 ? 0.5 * log2(rho) : -100.0;
        if (op == 0x12u || op == 0x1au) lod += c.w;     /* _LB */
    }
    lod += sbias + ibias;
    lod = max(minlod, min(lod, maxlod));
    bool linear = lod > 0.0 ? minf != 0u : mag != 0u;
    int lv0 = int(base), lv1 = -1;
    if (mipf != 0u && last > base && lod > 0.0) {
        float l = min(lod, float(last - base));
        if (mipf == 1u) {
            lv0 += int(floor(l + 0.5));
        } else {
            lv0 += int(floor(l));
            lv1 = min(lv0 + 1, int(last));
            lod = l - floor(l);
        }
    }
    lv0 = clamp(lv0, 0, 15);
    float sc0 = exp2(-float(lv0 - int(base)));
    uvec4 r = sample_level(s, ss, uint(lv0), u * sc0, v * sc0, slice, linear);
    if (lv1 >= 0 && lv1 != lv0) {
        float sc1 = exp2(-float(lv1 - int(base)));
        vec4 b = F(sample_level(s, ss, uint(lv1), u * sc1, v * sc1, slice, linear));
        r = U(F(r) + (b - F(r)) * lod);
    }
    if (op >= 0x18u && op <= 0x1eu) {
        r.x = U(tex_compare((s0 >> 22) & 7u, c.w, F(r.x)));
    }
    return res_dsel(s, r);
}

/* VFETCH from a PS or VS buffer (eg_vtx_fetch); 'hi': buffer id >= 128 */
#ifdef EVK_VS
#define VBUF(i) D.vs_vbuf[i]
#define VCFILE(i) D.vs_cfile[i]
#else
#define VBUF(i) D.vbuf[i]
#define VCFILE(i) D.cfile[i]
#endif
uvec4 vtx_fetch(uint slot, bool hi, uint idx, uint w1, uint w2, uvec4 dst)
{
    uvec4 r0 = VBUF(slot * 2u), r1 = VBUF(slot * 2u + 1u);
    bool valid = (r1.w >> 30) == 3u;
    uint fmt = (w1 >> 22) & 63u, numf = (w1 >> 28) & 3u, comp = (w1 >> 30) & 1u;
    uint sel = (w1 >> 9) & 0xfffu;
    bool ucf = ((w1 >> 21) & 1u) != 0u;
    uint offset = w2 & 0xffffu, size = r0.y + 1u, stride = (r0.z >> 8) & 0x7ffu;
    uvec4 v;
    if (ucf && valid) {
        fmt = (r0.z >> 20) & 63u;
        numf = (r0.z >> 26) & 3u;
        comp = (r0.z >> 28) & 1u;
        sel = (r0.w >> 3) & 0xfffu;
    }
    if (((w2 >> 18) & 1u) != 0u) stride = 16u;
    if (!valid && hi) {
        v = VCFILE((idx + offset / 16u) & 255u);
        numf = 0u;
    } else {
        uint n = fmt_bytes(fmt);
        uvec4 data = uvec4(0u);
        uint off = idx * stride + offset;
        if (valid && n != 0u && off + n <= size) {
            data = rd_elem(r1.z != 0u, r0.x + off, n);
        }
        v = format_decode(fmt, data, numf, comp * 0x55u);
    }
    for (int c = 0; c < 4; c++) {
        uint s = (sel >> (3 * c)) & 7u;
        if (s < 4u) dst[c] = v[s];
        else if (s == 4u) dst[c] = 0u;
        else if (s == 5u) dst[c] = numf == 1u ? 1u : U(1.0);
    }
    return dst;
}

/* ------------------------------------------------------------------ */
/* ALU helpers for the translated program                              */
/* ------------------------------------------------------------------ */

float mulz(float a, float b) { return (a == 0.0 || b == 0.0) ? 0.0 : a * b; }
float fmax_(float a, float b) { return isnan(a) ? b : isnan(b) ? a : max(a, b); }
float fmin_(float a, float b) { return isnan(a) ? b : isnan(b) ? a : min(a, b); }
uint f2i_sat(float v)
{
    if (isnan(v)) return 0u;
    if (v >= 2147483647.0) return 0x7fffffffu;
    if (v <= -2147483648.0) return 0x80000000u;
    return uint(int(v));
}
uint f2u_sat(float v)
{
    if (isnan(v) || v <= 0.0) return 0u;
    if (v >= 4294967295.0) return 0xffffffffu;
    return uint(v);
}
float clamp01(float f) { return f > 1.0 ? 1.0 : (f >= 0.0 ? f : 0.0); }
float recip_clamped(float a, bool ff)
{
    float f = 1.0 / a;
    if (isinf(f)) f = ff ? 0.0 * sign(f) : (f < 0.0 ? -3.40282347e38 : 3.40282347e38);
    return f;
}
float rsq_clamped(float a, bool ff)
{
    float f = inversesqrt(a);
    if (isinf(f)) f = ff ? 0.0 : 3.40282347e38;
    return f;
}
uint bfe_u(uint a, uint off, uint w)
{
    off &= 31u; w &= 31u;
    return w != 0u ? (a >> off) & ((1u << w) - 1u) : 0u;
}
uint bfe_i(uint a, uint off, uint w)
{
    off &= 31u; w &= 31u;
    return w != 0u ? uint(int(a << (32u - off - w)) >> (32u - w)) : 0u;
}
uint ffbh(uint v) { return v != 0u ? 31u - uint(findMSB(v)) : 0xffffffffu; }
uint recip_u(uint a) { return a != 0u ? uint(4294967296.0lf / double(a)) : 0u; }
uint recip_i(uint a)
{
    return a != 0u ? uint(int(4294967296.0lf / double(int(a)))) : 0u;
}
uint mulhi_i(uint a, uint b) { int hi, lo; imulExtended(int(a), int(b), hi, lo); return uint(hi); }
uint mulhi_u(uint a, uint b) { uint hi, lo; umulExtended(a, b, hi, lo); return hi; }
uint mulhi_u24(uint a, uint b) { uint hi, lo; umulExtended(a & 0xffffffu, b & 0xffffffu, hi, lo); return hi; }

/* the CUBE result of slot k from the group's vector (x, y, z) */
float cube_res(float vx, float vy, float vz, int k)
{
    float ma, sc, tc, id;
    if (abs(vx) >= abs(vy) && abs(vx) >= abs(vz)) {
        ma = vx; id = vx >= 0.0 ? 0.0 : 1.0; sc = vx >= 0.0 ? -vz : vz; tc = -vy;
    } else if (abs(vy) >= abs(vz)) {
        ma = vy; id = vy >= 0.0 ? 2.0 : 3.0; sc = vx; tc = vy >= 0.0 ? vz : -vz;
    } else {
        ma = vz; id = vz >= 0.0 ? 4.0 : 5.0; sc = vz >= 0.0 ? vx : -vx; tc = -vy;
    }
    return k == 0 ? tc : k == 1 ? sc : k == 2 ? 2.0 * abs(ma) : id;
}

#if !defined(EVK_VS) && !defined(EVK_CS)
/*
 * PS input n (a literal) of vertex k.  A flat input takes vertex 2: the
 * draw puts the provoking vertex last (the CPU path copies it to all
 * three).
 */
#define vparam(k, n) ((((MISC(EVK_M_FLATMASK) >> (n)) & 1u) != 0u) ? v_p##n[2] : v_p##n[k])

/* the triangle faces the viewer (FRONT_FACE_ENA, two-sided stencil) */
bool is_front()
{
    return MISC(EVK_M_GPU_VS) != 0u ? gl_FrontFacing : v_front != 0u;
}
#endif

#ifdef EVK_CS
void rat_store(uint n, uint inst, uvec4 a, uvec4 v, uint mask, uint es);
#endif

/* ps_run(): the translated pixel shader */
@PS_PROGRAM@

#ifndef EVK_VS
/* ------------------------------------------------------------------ */
/* Back end (eg_draw.c shade_quad, db_test, cb_write)                  */
/* ------------------------------------------------------------------ */

bool cmp_func(uint f, float a, float b)
{
    switch (f & 7u) {
    case 0u: return false;
    case 1u: return a < b;
    case 2u: return a == b;
    case 3u: return a <= b;
    case 4u: return a > b;
    case 5u: return a != b;
    case 6u: return a >= b;
    }
    return true;
}

uint stencil_op(uint op, uint v, uint ref)
{
    switch (op) {
    case 1u: return 0u;
    case 2u: return ref & 255u;
    case 3u: return v == 255u ? 255u : v + 1u;
    case 4u: return v == 0u ? 0u : v - 1u;
    case 5u: return ~v & 255u;
    case 6u: return (v + 1u) & 255u;
    case 7u: return (v - 1u) & 255u;
    }
    return v;
}

bool db_test(int x, int y, float z, bool front)
{
    bool z_valid = MISC(EVK_M_Z_FMT) != 0u, s_valid = MISC(EVK_M_S_VALID) != 0u;
    uint zfmt = MISC(EVK_M_Z_FMT);
    if (!z_valid && !s_valid) return true;
    if (x < 0 || y < 0 || uint(x) >= D.zb.tile.pitch || uint(y) >= D.zb.tile.height) return true;
    z = clamp(z, 0.0, 1.0);
    bool zg = (D.zb.flags & EVK_SF_GART) != 0u, sg = (D.sb.flags & EVK_SF_GART) != 0u;
    uint zoff = 0u, soff = 0u, zraw = 0u, st = 0u;
    float zd = 0.0;
    if (z_valid) {
        zoff = tile_offset(D.zb.tile, uint(x), uint(y));
        zraw = rdn(zg, D.zb.base + zoff, zfmt == 1u ? 2u : 4u);
        zd = zfmt == 1u ? float(zraw & 0xffffu) / 65535.0 :
             zfmt == 2u ? float(zraw & 0xffffffu) / 16777215.0 : F(zraw);
    }
    if (s_valid) {
        soff = tile_offset(D.sb.tile, uint(x), uint(y));
        st = rdn(sg, D.sb.base + soff, 1u);
    }
    bool zclear = MISC(EVK_M_Z_CLEAR) != 0u, sclear = MISC(EVK_M_S_CLEAR) != 0u;
    if (zclear && z_valid) zd = MISCF(EVK_M_Z_CLEAR_VAL);
    if (sclear && s_valid) st = MISC(EVK_M_S_CLEAR_VAL) & 255u;
    bool zen = MISC(EVK_M_Z_ENABLE) != 0u, zwr = MISC(EVK_M_Z_WRITE) != 0u;
    uint ctl = MISC(EVK_M_DB_CTL), zfunc = MISC(EVK_M_Z_FUNC);
    bool pass = true;
    if (MISC(EVK_M_S_ENABLE) != 0u) {
        uint side = (!front && ((ctl >> 7) & 1u) != 0u) ? 1u : 0u;
        uint rm = MISC(EVK_M_SREF0 + side);
        uint ref = rm & 255u, msk = (rm >> 8) & 255u, wmsk = (rm >> 16) & 255u;
        uint func = (ctl >> (side != 0u ? 20 : 8)) & 7u;
        uint ofail = (ctl >> (side != 0u ? 23 : 11)) & 7u;
        uint ozpass = (ctl >> (side != 0u ? 26 : 14)) & 7u;
        uint ozfail = (ctl >> (side != 0u ? 29 : 17)) & 7u;
        bool sp = cmp_func(func, float(ref & msk), float(st & msk));
        bool zp = !zen || cmp_func(zfunc, z, zd);
        uint nst = stencil_op(!sp ? ofail : zp ? ozpass : ozfail, st, ref);
        st = (st & ~wmsk) | (nst & wmsk);
        pass = sp && zp;
    } else if (zen) {
        pass = cmp_func(zfunc, z, zd);
    }
    if (pass && zwr) zd = z;
    if (z_valid && (zwr || zclear)) {
        uint v = zfmt == 1u ? uint(roundEven(zd * 65535.0)) :
                 zfmt == 2u ? ((zraw & 0xff000000u) | uint(roundEven(zd * 16777215.0))) : U(zd);
        wrn(zg, D.zb.wbase + zoff, zfmt == 1u ? 2u : 4u, v);
    }
    if (s_valid && (MISC(EVK_M_S_ENABLE) != 0u || sclear)) {
        wrn(sg, D.sb.wbase + soff, 1u, st);
    }
    return pass;
}

/*
 * Early depth rejection (EVK_M_EARLY_Z, set by eg_vk.c): the depth test is
 * LESS or LEQUAL, stencil is off, there is no fast clear, and since the
 * last barrier every depth write to the surface made it smaller.  A value
 * read before the interlock is then never smaller than the one db_test
 * reads, so a fail here is a fail there.
 */
bool db_early_fail(int x, int y, float z)
{
    uint zfmt = MISC(EVK_M_Z_FMT);
    if (x < 0 || y < 0 || uint(x) >= D.zb.tile.pitch || uint(y) >= D.zb.tile.height) return false;
    z = clamp(z, 0.0, 1.0);
    bool zg = (D.zb.flags & EVK_SF_GART) != 0u;
    uint zraw = rdn(zg, D.zb.base + tile_offset(D.zb.tile, uint(x), uint(y)), zfmt == 1u ? 2u : 4u);
    float zd = zfmt == 1u ? float(zraw & 0xffffu) / 65535.0 :
               zfmt == 2u ? float(zraw & 0xffffffu) / 16777215.0 : F(zraw);
    return !cmp_func(MISC(EVK_M_Z_FUNC), z, zd);
}

bool cb_is_float(uint f)
{
    return f == 0x06u || f == 0x0eu || f == 0x10u || f == 0x16u || f == 0x18u ||
           f == 0x1eu || f == 0x20u || f == 0x23u || f == 0x30u;
}

uvec4 cb_read(uint n, uint addr)
{
    bool g = (CB_FLAGS(n) & EVK_SF_GART) != 0u;
    uint bpp = CB_BPP(n);
    if (bpp <= 4u) return uvec4(rdn(g, addr, bpp), 0u, 0u, 0u);
    uvec4 o = uvec4(0u);
    [[dont_unroll]] for (uint i = 0u; i < bpp / 4u; i++) o[i] = rd32(g, addr + 4u * i);
    return o;
}
void cb_store(uint n, uint addr, uvec4 v, uvec4 old)
{
    bool g = (CB_FLAGS(n) & EVK_SF_GART) != 0u;
    uint bpp = CB_BPP(n);
    if (bpp <= 4u) {
        if (v.x != old.x) wrn(g, addr, bpp, v.x);
        return;
    }
    [[dont_unroll]] for (uint i = 0u; i < bpp / 4u; i++) {
        if (v[i] != old[i]) wr32(g, addr + 4u * i, v[i]);
    }
}

/* pixel memory <-> 4 components (float or integer bits) */
uvec4 cb_unpack(uint n, uvec4 raw)
{
    uint fmt = CB_FMT(n), number = CB_NUMBER(n), nch = CB_NCH(n);
    bool wide = CB_BPP(n) > 8u, isf = cb_is_float(fmt);
    uvec4 o = uvec4(0u, 0u, 0u, U(1.0));
    uint shift = 0u;
    [[dont_unroll]] for (uint k = 0u; k < nch; k++) {
        uint w = (CB_BITS(n) >> (8u * k)) & 255u, comp = (CB_CHMAP(n) >> (4u * k)) & 15u;
        uint v = wide ? raw[k] : bits128(raw, shift, w);
        shift += w;
        if (comp > 3u) continue;
        float f;
        if (isf) {
            f = w == 16u ? half_to_float(v) : w == 32u ? F(v) : 0.0;
        } else if (number == 1u) {
            int sv = int(v << (32u - w)) >> (32u - w);
            f = max(-1.0, float(sv) / float((1u << (w - 1u)) - 1u));
        } else if (number == 4u || number == 2u) {
            o[comp] = v;
            continue;
        } else if (number == 5u || number == 3u) {
            o[comp] = uint(int(v << (32u - w)) >> (32u - w));
            continue;
        } else {
            float m = w >= 32u ? 4294967296.0 : float((1u << w) - 1u);
            f = float(v) / m;
            if (number == 6u && comp < 3u) f = srgb_to_linear(f);
        }
        o[comp] = U(f);
    }
    return o;
}

uvec4 cb_pack(uint n, uvec4 inb, uvec4 raw, uint mask)
{
    uint fmt = CB_FMT(n), number = CB_NUMBER(n), nch = CB_NCH(n);
    bool wide = CB_BPP(n) > 8u, isf = cb_is_float(fmt);
    uint shift = 0u;
    [[dont_unroll]] for (uint k = 0u; k < nch; k++) {
        uint w = (CB_BITS(n) >> (8u * k)) & 255u, comp = (CB_CHMAP(n) >> (4u * k)) & 15u;
        uint m = w >= 32u ? 0xffffffffu : ((1u << w) - 1u);
        if (comp > 3u || ((mask >> comp) & 1u) == 0u) {
            shift += w;
            continue;
        }
        float f = F(inb[comp]);
        uint v;
        if (isf) {
            v = w == 16u ? float_to_half(f) : inb[comp];
        } else if (number == 1u) {
            float cl = clamp(f, -1.0, 1.0);
            v = uint(int(roundEven(cl * float((1u << (w - 1u)) - 1u)))) & m;
        } else if (number == 4u || number == 2u) {
            v = min(inb[comp], m);
        } else if (number == 5u || number == 3u) {
            int lo = -(1 << (w - 1u)), hi = (1 << (w - 1u)) - 1;
            v = uint(clamp(int(inb[comp]), lo, hi)) & m;
        } else {
            if (number == 6u && comp < 3u) f = linear_to_srgb(f);
            float cl = isnan(f) ? 0.0 : clamp(f, 0.0, 1.0);
            v = uint(roundEven(double(cl) * double(m)));
        }
        if (wide) raw[k] = v; else raw = setbits128(raw, shift, w, v);
        shift += w;
    }
    return raw;
}

float blend_factor(uint f, vec4 s, vec4 d, vec4 k, int c)
{
    switch (f) {
    case 0u: return 0.0;
    case 1u: return 1.0;
    case 2u: return s[c];
    case 3u: return 1.0 - s[c];
    case 4u: return s.w;
    case 5u: return 1.0 - s.w;
    case 6u: return d.w;
    case 7u: return 1.0 - d.w;
    case 8u: return d[c];
    case 9u: return 1.0 - d[c];
    case 10u: return c == 3 ? 1.0 : min(s.w, 1.0 - d.w);
    case 13u: return k[c];
    case 14u: return 1.0 - k[c];
    case 19u: return k.w;
    case 20u: return 1.0 - k.w;
    }
    return s[c];
}
float blend_comb(uint fn, float s, float d)
{
    if (fn == 1u) return s - d;
    if (fn == 2u) return min(s, d);
    if (fn == 3u) return max(s, d);
    if (fn == 4u) return d - s;
    return s + d;
}

void cb_write(uint n, int x, int y, uvec4 col)
{
    if (x < 0 || y < 0 || uint(x) >= D.cb[n].width || uint(y) >= D.cb[n].height) return;
    uint addr = D.cb[n].base + tile_offset(cb_tile(n), uint(x), uint(y));
    uint mask = CB_MASK(n), rop = MISC(EVK_M_ROP), bl = CB_BLEND(n);
    bool blend = (CB_FLAGS(n) & EVK_SF_BLEND) != 0u;
    uvec4 px = cb_read(n, addr), out_px = px;
    uvec4 src = col;
    if (blend) {
        vec4 s = F(src), d = F(cb_unpack(n, px)), k = D.blend_const;
        uint csrc = bl & 31u, cfn = (bl >> 5) & 7u, cdst = (bl >> 8) & 31u;
        uint asrc = csrc, afn = cfn, adst = cdst;
        if (((bl >> 29) & 1u) != 0u) {
            asrc = (bl >> 16) & 31u; afn = (bl >> 21) & 7u; adst = (bl >> 24) & 31u;
        }
        if ((CB_FLAGS(n) & EVK_SF_BLEND_CLAMP) != 0u && !cb_is_float(CB_FMT(n))) {
            s = clamp(s, 0.0, 1.0);
        }
        vec4 r;
        for (int c = 0; c < 4; c++) {
            uint fs = c < 3 ? csrc : asrc, fd = c < 3 ? cdst : adst, fn = c < 3 ? cfn : afn;
            float sf = blend_factor(fs, s, d, k, c), df = blend_factor(fd, s, d, k, c);
            if (fn == 2u || fn == 3u) { sf = 1.0; df = 1.0; }
            r[c] = blend_comb(fn, s[c] * sf, d[c] * df);
        }
        out_px = cb_pack(n, U(r), px, mask);
    } else if (rop != 0xccu) {
        uvec4 sp = cb_pack(n, src, px, 0xfu);
        uint b7 = ((rop >> 7) & 1u) != 0u ? ~0u : 0u, b4 = ((rop >> 4) & 1u) != 0u ? ~0u : 0u;
        uint b3 = ((rop >> 3) & 1u) != 0u ? ~0u : 0u, b0 = (rop & 1u) != 0u ? ~0u : 0u;
        uvec4 r = (sp & px & b7) | (sp & ~px & b4) | (~sp & px & b3) | (~sp & ~px & b0);
        if (CB_BPP(n) < 4u) r.x &= (1u << (8u * CB_BPP(n))) - 1u;
        out_px = mask == 15u ? r : cb_pack(n, cb_unpack(n, r), px, mask);
    } else {
        out_px = cb_pack(n, src, px, mask);
    }
    cb_store(n, addr, out_px, px);
}

#ifdef EVK_CS
/*
 * MEM_RAT of a compute shader (eg_compute.c eg_compute_rat): RAT n is
 * CB_COLORn.  STORE_TYPED writes pixel (a.x, a.y) without blend or logic
 * op.  STORE_RAW writes ELEM_SIZE + 1 dwords of element a.x to a linear
 * buffer.
 */
void rat_store(uint n, uint inst, uvec4 a, uvec4 v, uint mask, uint es)
{
    bool g = (CB_FLAGS(n) & EVK_SF_GART) != 0u;
    if (inst == 1u) {
        if (a.x >= D.cb[n].width || a.y >= D.cb[n].height) return;
        uint addr = D.cb[n].base + tile_offset(cb_tile(n), a.x, a.y);
        uvec4 px = cb_read(n, addr);
        cb_store(n, addr, cb_pack(n, v, px, mask), px);
    } else if (inst == 2u) {
        [[dont_unroll]] for (uint k = 0u; k <= es; k++) {
            if (((mask >> k) & 1u) != 0u) wr32(g, D.cb[n].base + (a.x * (es + 1u) + k) * 4u, v[k]);
        }
    }
}

/*
 * One guest thread for each invocation, numbered as eg_compute.c does:
 * thread t is thread t % TPG of group t / TPG.
 */
void main()
{
    uint t = gl_GlobalInvocationID.x + gl_GlobalInvocationID.y * gl_NumWorkGroups.x * 64u;
    if (t >= MISC(EVK_M_CS)) return;
    uint tpg = MISC(EVK_M_CS + 1u), g = t / tpg, l = t % tpg;
    uvec3 gs = uvec3(MISC(EVK_M_CS + 2u), MISC(EVK_M_CS + 3u), MISC(EVK_M_CS + 4u));
    uvec3 gn = uvec3(MISC(EVK_M_CS + 5u), MISC(EVK_M_CS + 6u), MISC(EVK_M_CS + 7u));
    uvec3 g0 = uvec3(MISC(EVK_M_CS + 8u), MISC(EVK_M_CS + 9u), MISC(EVK_M_CS + 10u));
    cs_l = uvec3(l % gs.x, (l / gs.x) % gs.y, l / (gs.x * gs.y));
    cs_g = g0 + uvec3(g % gn.x, (g / gn.x) % gn.y, g / (gn.x * gn.y));
    alive = true;
    has_grad = false;
    grad_h = vec4(0.0);
    grad_v = vec4(0.0);
    cs_run();
}
#else
bool cliprect_pass(int x, int y)
{
    uint rule = MISC(EVK_M_CLIP_RULE);
    if (rule == 0xffffu) return true;
    uint idx = 0u;
    for (uint i = 0u; i < 4u; i++) {
        int x0 = int(MISC(EVK_M_CR0 + 4u * i)), y0 = int(MISC(EVK_M_CR0 + 4u * i + 1u));
        int x1 = int(MISC(EVK_M_CR0 + 4u * i + 2u)), y1 = int(MISC(EVK_M_CR0 + 4u * i + 3u));
        if (x >= x0 && x < x1 && y >= y0 && y < y1) idx |= 1u << i;
    }
    return ((rule >> idx) & 1u) != 0u;
}

void main()
{
    for (int i = 0; i < 8; i++) E[i] = uvec4(0u);
    emask = 0u;
    edepth = 0u;
    edepth_written = false;
    alive = true;
    has_grad = false;
    grad_h = vec4(0.0);
    grad_v = vec4(0.0);
    /* skip the program when the whole quad fails the depth test (the
     * other lanes of a quad need this lane's values for derivatives) */
    bool skip = false;
#ifndef EVK_PS_WRITES_Z
    if (MISC(EVK_M_EARLY_Z) != 0u) {
        bool rej = gl_HelperInvocation ||
                   db_early_fail(int(gl_FragCoord.x), int(gl_FragCoord.y), v_z);
        skip = !quad_any(!rej);
    }
#endif
    if (!skip) {
        ps_run();
    } else {
        alive = false;
    }

    int px = int(gl_FragCoord.x), py = int(gl_FragCoord.y);
    bool live = !gl_HelperInvocation && alive && cliprect_pass(px, py);
    float z = edepth_written ? F(edepth) : v_z;
    bool front = is_front();

    beginInvocationInterlockARB();
    if (live) {
        bool ok = true;
        if (MISC(EVK_M_ALPHA_TEST) != 0u &&
            !cmp_func(MISC(EVK_M_ALPHA_FUNC), F(E[0].w), MISCF(EVK_M_ALPHA_REF))) {
            ok = false;
        }
        if (ok && !db_test(px, py, z, front)) ok = false;
        if (ok) {
            atomicAdd(zpass_count[MISC(EVK_M_ZPASS_SLOT)], 1u);
            if (MISC(EVK_M_CB_MODE) == 3u) {
                /* resolve (single sample): CB0 -> CB1 */
                if ((CB_FLAGS(0u) & EVK_SF_ENABLED) != 0u && (D.cb[1].flags & EVK_SF_ENABLED) != 0u &&
                    uint(px) < D.cb[0].width && uint(py) < D.cb[0].height) {
                    uint a = D.cb[0].base + tile_offset(cb_tile(0u), uint(px), uint(py));
                    cb_write(1u, px, py, cb_unpack(0u, cb_read(0u, a)));
                }
            } else {
                [[dont_unroll]] for (uint n = 0u; n < 8u; n++) {
                    if ((CB_FLAGS(n) & EVK_SF_ENABLED) != 0u && ((emask >> n) & 1u) != 0u) {
                        cb_write(n, px, py, E[n]);
                    }
                }
            }
        }
    }
    endInvocationInterlockARB();
}
#endif
#else
/* ------------------------------------------------------------------ */
/* Vertex shader: eg_draw.c to_window() and clip_tri() on the GPU      */
/* ------------------------------------------------------------------ */

void main()
{
    for (int i = 0; i < 4; i++) EPOS[i] = uvec4(0u, 0u, 0u, 0x3f800000u);
    for (int i = 0; i < 32; i++) EPAR[i] = uvec4(0u);
    alive = true;
    vs_run();

    vec4 c = F(EPOS[0]);
    uint vte = MISC(EVK_M_VTE), cl = MISC(EVK_M_CLIP);
    float w = c.w, rw = w != 0.0 ? 1.0 / w : 0.0;
    vec3 p = c.xyz;
    if (((vte >> 8) & 1u) == 0u) p.xy *= rw;
    if (((vte >> 9) & 1u) == 0u) p.z *= rw;
    if ((vte & 1u) != 0u) p.x *= MISCF(EVK_M_VPORT);
    if ((vte & 2u) != 0u) p.x += MISCF(EVK_M_VPORT + 1u);
    if ((vte & 4u) != 0u) p.y *= MISCF(EVK_M_VPORT + 2u);
    if ((vte & 8u) != 0u) p.y += MISCF(EVK_M_VPORT + 3u);
    if ((vte & 16u) != 0u) p.z *= MISCF(EVK_M_VPORT + 4u);
    if ((vte & 32u) != 0u) p.z += MISCF(EVK_M_VPORT + 5u);
    /* the window offset, and the guest's pixel center onto the host's */
    p.x += float(int(MISC(EVK_M_WIN_OFS))) + 0.5 - MISCF(EVK_M_PIX_CENTER);
    p.y += float(int(MISC(EVK_M_WIN_OFS + 1u))) + 0.5 - MISCF(EVK_M_PIX_CENTER);

    /*
     * Window x, y back to the clip space of the host viewport (8192 x 8192
     * at the origin), times w: linear in the guest's clip coordinates, so
     * the host clips and interpolates as the guest does.  z: the guest's
     * near and far planes (clip_tri) as the host's 0 <= z <= w.
     */
    float hw = w;
    float hz;
    if (((cl >> 16) & 1u) != 0u || ((cl >> 26) & 1u) != 0u) {
        hz = 0.5 * hw;                  /* clipping off (near and far alike) */
    } else if (((cl >> 19) & 1u) != 0u) {
        hz = c.z;                       /* DX: 0 <= z <= w */
    } else {
        hz = 0.5 * (c.z + c.w);         /* GL: -w <= z <= w */
    }
    gl_Position = vec4((p.x * (2.0 / float(EVK_VP)) - 1.0) * hw,
                       (p.y * (2.0 / float(EVK_VP)) - 1.0) * hw, hz, hw);
    o_z = p.z;
    o_front = 0u;

    uint ni = MISC(EVK_M_NINTERP);
    for (uint n = 0u; n < uint(EVK_MAX_PARAMS); n++) {
        uint e = n < ni ? MISC(EVK_M_ISRC + n) : 0xffu;
        uint src = e & 0xffu, dv = (e >> 8) & 3u;
        if (src < 32u) {
            o_p[n] = F(EPAR[src]);
        } else {
            float a = (dv & 2u) != 0u ? 1.0 : 0.0;
            o_p[n] = vec4(a, a, a, (dv == 1u || dv == 3u) ? 1.0 : 0.0);
        }
    }
}
#endif
