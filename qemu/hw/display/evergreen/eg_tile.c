/*
 * Evergreen surface tiling: the byte offset of pixel (x, y) in a 1D or
 * 2D tiled (THIN1) surface.
 *
 * Follows AMD's addrlib (Mesa src/amd/addrlib, EgBasedLib::
 * ComputeSurfaceAddrFromCoordMacroTiled, ComputeBankFromCoord and
 * Lib::ComputePixelIndexWithinMicroTile); the Evergreen pipe equations
 * are those of the ADDR_PIPECFG_P2/P4_16x16/P8_32x32_16x16 configs.
 * Pipe and bank swizzles are folded into the surface base address, as
 * the hardware does, so they are not parameters here.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "evergreen.h"

#define BIT_OF(v, n) (((v) >> (n)) & 1)

/* pixel order within an 8x8 micro tile */
static uint32_t eg_micro_index(uint32_t x, uint32_t y, uint32_t bpp, bool display)
{
    uint32_t x0 = BIT_OF(x, 0), x1 = BIT_OF(x, 1), x2 = BIT_OF(x, 2);
    uint32_t y0 = BIT_OF(y, 0), y1 = BIT_OF(y, 1), y2 = BIT_OF(y, 2);
    uint32_t b[6];

    if (!display) {
        b[0] = x0; b[1] = y0; b[2] = x1; b[3] = y1; b[4] = x2; b[5] = y2;
    } else {
        switch (bpp) {
        case 8:
            b[0] = x0; b[1] = x1; b[2] = x2; b[3] = y1; b[4] = y0; b[5] = y2;
            break;
        case 16:
            b[0] = x0; b[1] = x1; b[2] = x2; b[3] = y0; b[4] = y1; b[5] = y2;
            break;
        case 64:
            b[0] = x0; b[1] = y0; b[2] = x1; b[3] = x2; b[4] = y1; b[5] = y2;
            break;
        case 128:
            b[0] = y0; b[1] = x0; b[2] = x1; b[3] = x2; b[4] = y1; b[5] = y2;
            break;
        default:                                    /* 32 */
            b[0] = x0; b[1] = x1; b[2] = y0; b[3] = x2; b[4] = y1; b[5] = y2;
            break;
        }
    }
    return b[0] | b[1] << 1 | b[2] << 2 | b[3] << 3 | b[4] << 4 | b[5] << 5;
}

static inline uint32_t eg_pipe(uint32_t x, uint32_t y, uint32_t pipes)
{
    uint32_t tx = x >> 3, ty = y >> 3;
    uint32_t x3 = BIT_OF(tx, 0), x4 = BIT_OF(tx, 1), x5 = BIT_OF(tx, 2);
    uint32_t y3 = BIT_OF(ty, 0), y4 = BIT_OF(ty, 1), y5 = BIT_OF(ty, 2);

    switch (pipes) {
    case 1:
        return 0;
    case 2:
        return x3 ^ y3;
    case 4:
        return (x3 ^ y3 ^ x4) | (x4 ^ y4) << 1;
    default:
        return (x3 ^ y3 ^ x4) | (x4 ^ y4) << 1 | (x5 ^ y5) << 2;
    }
}

static inline uint32_t eg_bank(const EgTiling *t, uint32_t x, uint32_t y, uint32_t split_slice)
{
    uint32_t tx = x >> t->bank_x_shift, ty = y >> t->bank_y_shift;
    uint32_t x3 = BIT_OF(tx, 0), x4 = BIT_OF(tx, 1), x5 = BIT_OF(tx, 2), x6 = BIT_OF(tx, 3);
    uint32_t y3 = BIT_OF(ty, 0), y4 = BIT_OF(ty, 1), y5 = BIT_OF(ty, 2), y6 = BIT_OF(ty, 3);
    uint32_t bank;

    switch (t->banks) {
    case 16:
        bank = (x3 ^ y6) | (x4 ^ y5 ^ y6) << 1 | (x5 ^ y4) << 2 | (x6 ^ y3) << 3;
        break;
    case 8:
        bank = (x3 ^ y5) | (x4 ^ y4 ^ y5) << 1 | (x5 ^ y3) << 2;
        break;
    case 4:
        bank = (x3 ^ y4) | (x4 ^ y3) << 1;
        break;
    default:
        bank = x3 ^ y3;
        break;
    }
    bank ^= (t->banks / 2 + 1) * split_slice;       /* tile split rotation */
    return bank & (t->banks - 1);
}

uint64_t eg_tile_offset(const EgTiling *t, uint32_t x, uint32_t y)
{
    uint32_t elem = t->micro_elem[(y & 7) * 8 + (x & 7)];
    uint32_t split_slice = 0, pipe, bank, tile_index;
    uint64_t total;

    if (t->array_mode == EG_ARRAY_1D_TILED_THIN1) {
        return ((uint64_t)(y >> 3) * t->tiles_per_row + (x >> 3)) * t->micro_bytes + elem;
    }
    if (t->array_mode != EG_ARRAY_2D_TILED_THIN1) {
        return ((uint64_t)y * t->pitch + x) * t->bpp;                /* linear */
    }
    if (t->split) {
        split_slice = elem / t->tile_split;
        elem %= t->tile_split;
    }
    tile_index = ((y >> 3) & (t->bank_height - 1)) * t->bank_width +
                 (((x >> 3) >> t->pipe_bits) & (t->bank_width - 1));
    total = t->slice_bytes * split_slice +
            ((uint64_t)(y >> t->mt_height_shift) * t->mt_per_row + (x >> t->mt_pitch_shift)) *
            t->mt_bytes + elem + tile_index * t->micro_bytes;
    pipe = eg_pipe(x, y, t->pipes);
    bank = eg_bank(t, x, y, split_slice);

    /* pipe interleave bytes | pipe | bank | the rest */
    return (total & (t->pipe_interleave - 1)) |
           (uint64_t)pipe << t->pi_bits |
           (uint64_t)bank << (t->pi_bits + t->pipe_bits) |
           (total >> t->pi_bits) << (t->pi_bits + t->pipe_bits + t->bank_bits);
}

/* bytes the surface spans (the 2D layout pads to whole macro tiles) */
uint64_t eg_tile_size(const EgTiling *t)
{
    if (t->array_mode == EG_ARRAY_2D_TILED_THIN1) {
        return (uint64_t)ROUND_UP(t->pitch, t->mt_pitch) * ROUND_UP(t->height, t->mt_height) *
               t->bpp;
    }
    if (t->array_mode == EG_ARRAY_1D_TILED_THIN1) {
        return (uint64_t)t->pitch * ROUND_UP(t->height, 8) * t->bpp;
    }
    return (uint64_t)t->pitch * t->height * t->bpp;
}

/* the derived constants eg_tile_offset() uses; call after changing fields */
void eg_tiling_update(EgTiling *t)
{
    uint32_t micro_raw = 64 * t->bpp, i;

    for (i = 0; i < 64; i++) {
        t->micro_elem[i] = eg_micro_index(i & 7, i >> 3, t->bpp * 8, t->display) * t->bpp;
    }
    t->split = t->array_mode == EG_ARRAY_2D_TILED_THIN1 && micro_raw > t->tile_split;
    t->micro_bytes = t->split ? t->tile_split : micro_raw;
    t->tiles_per_row = MAX(t->pitch / 8, 1u);
    t->mt_pitch = 8 * t->bank_width * t->pipes * t->macro_aspect;
    t->mt_height = 8 * t->bank_height * t->banks / t->macro_aspect;
    t->mt_pitch_shift = ctz32(t->mt_pitch);
    t->mt_height_shift = ctz32(t->mt_height);
    t->mt_bytes = t->micro_bytes * (t->mt_pitch / 8) * (t->mt_height / 8) / (t->pipes * t->banks);
    t->mt_per_row = t->pitch / t->mt_pitch;
    t->slice_bytes = (uint64_t)t->mt_per_row * (t->height / t->mt_height) * t->mt_bytes;
    t->pi_bits = ctz32(t->pipe_interleave);
    t->pipe_bits = ctz32(t->pipes);
    t->bank_bits = ctz32(t->banks);
    t->bank_x_shift = 3 + ctz32(t->bank_width * t->pipes);
    t->bank_y_shift = 3 + ctz32(t->bank_height);
}

/* tiling parameters from GB_ADDR_CONFIG and a GRPH_CONTROL/CB_COLOR_ATTRIB
 * style field set (all fields already decoded by the caller) */
void eg_tiling_init(EvergreenState *s, EgTiling *t, uint32_t array_mode,
                    uint32_t bpp, uint32_t pitch, uint32_t height,
                    uint32_t banks_field, uint32_t bank_w_field,
                    uint32_t bank_h_field, uint32_t aspect_field,
                    uint32_t split_field, bool display)
{
    uint32_t gb = s->regs[EG_GB_ADDR_CONFIG >> 2];

    t->array_mode = array_mode;
    t->bpp = bpp;
    t->pitch = pitch;
    t->height = height;
    t->pipes = 1u << (gb & 7);
    t->pipe_interleave = 256u << ((gb >> 4) & 7);
    t->banks = 2u << (banks_field & 3);
    t->bank_width = 1u << (bank_w_field & 3);
    t->bank_height = 1u << (bank_h_field & 3);
    t->macro_aspect = 1u << (aspect_field & 3);
    t->tile_split = 64u << (split_field & 7);
    t->display = display;
    eg_tiling_update(t);
}
