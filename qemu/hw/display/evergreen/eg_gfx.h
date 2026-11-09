/*
 * Evergreen 3D engine: state, shader core and pixel pipeline.
 *
 * A CPU reference implementation of what Apple's ATIRadeonX3000 and
 * Linux/Mesa drive: the CP's state packets, the VGT (index fetch,
 * primitive types), the SQ shader core (CF/ALU/TEX/VTX clauses of the
 * Evergreen ISA, VS + fetch shader + PS), the PA/SC rasterizer and the
 * CB/DB back end, all reading and writing guest GPU memory.
 *
 * References: Mesa src/gallium/drivers/r600 (evergreend.h, eg_sq.h,
 * r600_isa.c, evergreen_state.c, sfn/), src/amd/addrlib (tiling), the
 * AMD Evergreen ISA guide.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_DISPLAY_EG_GFX_H
#define HW_DISPLAY_EG_GFX_H

#include "evergreen.h"
#include "eg_vk.h"

#define EG_NUM_RESOURCES    1024        /* SET_RESOURCE 0x30000-0x38000 */
#define EG_NUM_SAMPLERS     128         /* SET_SAMPLER 0x3c000-0x3c600 */
#define EG_NUM_ALU_CONSTS   512         /* vec4; PS 0-255, VS 256-511 */
#define EG_NUM_LOOP_CONSTS  192
#define EG_NUM_BOOL_CONSTS  6
#define EG_NUM_GPRS         128
#define EG_NUM_PARAMS       32
#define EG_MAX_LANES        16          /* one 2x2 pixel quad (4), or 16 vertices */

/* shader stages (resource / sampler / constant bases differ) */
enum { EG_STAGE_PS, EG_STAGE_VS, EG_STAGE_FS, EG_STAGE_CS };

/* the first fetch resource and sampler of a stage (compute uses the LS slots) */
static inline int eg_stage_res_base(int stage)
{
    return stage == EG_STAGE_PS ? 0 : stage == EG_STAGE_CS ? 816 : 176;
}

static inline int eg_stage_smp_base(int stage)
{
    return stage == EG_STAGE_PS ? 0 : stage == EG_STAGE_CS ? 90 : 18;
}

typedef union EgVec4 {
    float f[4];
    uint32_t u[4];
    int32_t i[4];
} EgVec4;

/* a context register (0x28000-0x28fff) */
#define CTX(s, r) ((s)->ctx[((r) - EG_CTX_BASE) >> 2])

/* the "microcode" state that is not in the register file */
typedef struct EgGfx {
    uint32_t resource[EG_NUM_RESOURCES][8];
    uint32_t sampler[EG_NUM_SAMPLERS][3];
    uint32_t alu_const[EG_NUM_ALU_CONSTS * 4];
    uint32_t loop_const[EG_NUM_LOOP_CONSTS];
    uint32_t bool_const[EG_NUM_BOOL_CONSTS];
    uint32_t ctl_const[64];
    /* VGT state set by packets */
    uint32_t index_type;                /* INDEX_TYPE: 0 = 16 bit, 1 = 32 bit */
    uint32_t num_instances;
    uint64_t index_base;
    uint32_t index_size;
    /* state shadowing (CONTEXT_CONTROL / LOAD_*): base of each class */
    uint64_t shadow[8];
    uint32_t load_ena, shadow_ena;      /* CONTEXT_CONTROL */
    /* statistics, for the debug log */
    uint64_t draws, prims, pixels;
    bool warned[256];
    /* Vulkan backend (eg_vk.h): NULL when unavailable or off */
    EgVk *vk;
    MemoryRegion *vk_sysram;            /* guest RAM, imported for GART surfaces */
    uint32_t gart_gen, *gart_gen_of, gart_gen_pages;
    uint64_t *gart_pte_of;      /* the PTE each table entry was made from */
    uint64_t gart_key;
    void *vk_tex;               /* VkTexEntry cache (eg_draw.c) */
    void *pgm_fast;             /* PgmFast table (eg_draw.c) */
    GHashTable *ps_cache;               /* PS hash -> EgPsEntry */
    QEMUTimer *vk_idle_flush;           /* runs batches left after a ring kick */
    QEMUTimer *vk_poll;                 /* completes fences of running batches */
    QEMUBH *vk_done_bh;                 /* a batch completed (eg_vk notify) */
    GArray *fence_ops;                  /* EgFenceOp, waiting for their batch */
    uint64_t gpu_pixels;                /* ZPASS slots taken so far */
    uint8_t zslot_busy[1024];           /* a ZPASS op still waits for the slot */
    uint64_t vk_draws, vk_fallbacks;
} EgGfx;

/* one shader program, fetched from GPU memory for a draw */
typedef struct EgProgram {
    uint64_t addr;                      /* byte address */
    uint32_t *code;                     /* dwords */
    uint32_t ndw;
    int stage;
    uint32_t used;                      /* dwords the GLSL translation read */
    GHashTable *groups;                 /* dword offset -> decoded ALU group */
} EgProgram;

/* per-primitive interpolation data for one PS input */
typedef struct EgInterp {
    EgVec4 p0, p10, p20;                /* v0, v1 - v0, v2 - v0 */
} EgInterp;

typedef struct EgShader {
    EvergreenState *s;
    const EgProgram *prog;
    const EgProgram *fetch;             /* CALL_FS target (VS only) */
    int stage;
    bool in_fs;                         /* running the fetch shader */
    int nlanes;
    uint32_t live;                      /* lanes that exist */
    uint32_t active;                    /* lanes executing */
    uint32_t killed;                    /* pixel lanes killed (KILL*) */
    bool quad;                          /* lanes form a 2x2 quad (derivatives) */
    EgVec4 gpr[EG_MAX_LANES][EG_NUM_GPRS];
    int32_t ar[EG_MAX_LANES][4];        /* address register (MOVA) */
    uint32_t pred;                      /* predicate bit per lane */
    int32_t loop_index;
    /* exports */
    EgVec4 pos[EG_MAX_LANES][4];        /* POS 60..63 */
    EgVec4 param[EG_MAX_LANES][EG_NUM_PARAMS];
    uint32_t param_mask;
    EgVec4 color[EG_MAX_LANES][8];      /* PIXEL 0..7 */
    uint32_t color_mask;
    EgVec4 depth[EG_MAX_LANES];         /* PIXEL 61 (MRTZ) */
    bool depth_written;
    /* PS inputs */
    const EgInterp *interp;
    int ninterp;
    /* gradients from SET_GRADIENTS_H/V */
    EgVec4 grad_h[EG_MAX_LANES], grad_v[EG_MAX_LANES];
    bool has_grad;
    int steps;                          /* runaway guard */
    /* kcache sets KC0-KC3, loaded on demand and kept for the whole draw */
    EgVec4 kc_cache[4][32];
    uint32_t kc_key[4];
    void *tex_cache;                    /* eg_tex.c: texture descriptors */
    void *rat;                          /* eg_compute.c: RAT targets (compute only) */
} EgShader;

/* eg_shader.c */
void eg_shader_run(EgShader *sh);
bool eg_program_load(EvergreenState *s, EgProgram *p, uint64_t addr, int stage);
void eg_program_free(EgProgram *p);

/* eg_tex.c */
void eg_vtx_fetch(EgShader *sh, const uint32_t *ins, uint32_t lanes);
void eg_tex_fetch(EgShader *sh, const uint32_t *ins, uint32_t lanes);
void eg_tex_cache_reset(EgShader *sh);
void eg_tex_init(void);
/* decode one element of a buffer/texture format into 4 raw channels
 * (floats, or integers in the u[] view for INT formats) */
bool eg_format_decode(uint32_t fmt, const uint8_t *p, EgVec4 *out, uint32_t num_format,
                      const uint32_t comp_signed[4], bool srf_no_zero);
uint32_t eg_format_bytes(uint32_t fmt);

/* eg_tex.c: descriptors for the Vulkan backend */
void eg_tiling_to_vk(const EgTiling *t, EvkTile *o);
void eg_kcache_cfile(EvergreenState *s, bool ps, uint32_t addr, uint32_t n, void *dst);
bool eg_tex_vk_build(EvergreenState *s, const uint32_t *r, EvkTex *o,
                     bool (*locate)(EvergreenState *s, uint64_t addr, uint64_t len,
                                    uint32_t *off, bool *gart),
                     uint64_t rd_off[2], uint64_t rd_len[2], int *nrd);

/* eg_draw.c */
void eg_gfx_init(EvergreenState *s);
void eg_gfx_vk_init(EvergreenState *s);
void eg_gfx_vk_fini(EvergreenState *s);
/* finish the queued GPU work: before the CPU reads or writes GPU memory */
/* run the queued GPU draws and wait for them; 'at' names the caller (EG_PROFILE) */
void eg_gfx_flush_at(EvergreenState *s, const char *at);
#define EG_STR2(x) #x
#define EG_STR(x) EG_STR2(x)
#define eg_gfx_flush(s) eg_gfx_flush_at(s, __FILE__ ":" EG_STR(__LINE__))
void eg_gfx_new_cp_run(EvergreenState *s);
void eg_gfx_kick_done(EvergreenState *s);
bool eg_gfx_pending(EvergreenState *s);
/* a fence of the command stream: write 'len' bytes of 'data' at GPU address
 * 'addr' (len 0: none), then raise interrupt 'irq' (< 0: none), once the
 * rendering queued before it has completed */
void eg_gfx_fence(EvergreenState *s, uint64_t addr, const void *data, int len, int irq);
void eg_gfx_zpass(EvergreenState *s, uint64_t addr);
void eg_gfx_poll(EvergreenState *s);
void eg_gfx_poll_done(EvergreenState *s);
void eg_gfx_mirror_collect(EvergreenState *s);
/* GPU batches are recorded or running (starts recorded ones, no wait) */
bool eg_gfx_busy(EvergreenState *s);
void eg_gfx_reset(EvergreenState *s);
bool eg_gfx_packet(EvergreenState *s, uint32_t op, const uint32_t *d, uint32_t n);
/* SET_CONFIG_REG (cls 0) / SET_CONTEXT_REG (cls 1) shadowing */
void eg_gfx_shadow_write(EvergreenState *s, int cls, uint32_t off, const uint32_t *v,
                         uint32_t n);
void eg_gfx_warn(EvergreenState *s, int id, const char *fmt, ...) G_GNUC_PRINTF(3, 4);

/* eg_draw.c: CB_COLORn as a store target of compute shaders (RAT) */
void *eg_cb_open(EvergreenState *s, int i);
void eg_cb_store(EvergreenState *s, void *cb, uint32_t x, uint32_t y, const EgVec4 *v,
                 uint32_t mask);
void eg_cb_close(EvergreenState *s, void *cb);

/* eg_compute.c */
void eg_compute_dispatch(EvergreenState *s, const uint32_t *d, uint32_t n);
/* a MEM_RAT* CF instruction of a compute shader */
void eg_compute_rat(EgShader *sh, uint32_t w0, uint32_t w1, uint32_t lanes);

#endif
