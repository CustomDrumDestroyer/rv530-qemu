/*
 * Evergreen 3D engine on Vulkan.
 *
 * Same split as the rv530 backend (r5xx_vk.h): the vertex stage, clipping
 * and primitive assembly stay on the CPU (eg_draw.c); window-space
 * triangles go to the host GPU, where a fragment shader generated from the
 * guest's pixel shader (eg_vk_ps.c) runs, followed by the DB and CB back
 * end emulated in GLSL (eg_vk_frag.glsl) inside a pixel interlock.  Guest
 * VRAM is a storage buffer (imported host memory, with a device-local
 * mirror), so textures are read from and render targets written to the
 * same bytes the guest sees, with Evergreen tiling done in the shader.
 *
 * The guest PS interpolates its inputs itself (INTERP_XY/ZW on the i/j
 * barycentrics in R0): VK_KHR_fragment_shader_barycentric hands the
 * shader each vertex's parameters and the hardware barycentrics, so the
 * translated program computes exactly what the guest asked for.
 *
 * Pure C (no QEMU headers); built when Vulkan and shaderc are available,
 * otherwise eg_vk_stub.c makes eg_vk_create() fail.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef EG_VK_H
#define EG_VK_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define EVK_MAX_PARAMS      12      /* PS inputs (NUM_INTERP) */
#define EVK_MAX_TEX         8       /* texture resources a PS may use */
#define EVK_MAX_SMP         8
#define EVK_MAX_VBUF        4       /* PS vertex-fetch buffers */
#define EVK_MAX_KC          8       /* distinct kcache windows a PS uses */
#define EVK_MAX_VS_VBUF     16      /* VS (and fetch shader) vertex buffers */

/* ---- GPU-visible structures (std430, mirrored in eg_vk_frag.glsl) ---- */

/* tiling constants of a surface, as eg_tiling_update() derives them */
typedef struct EvkTile {            /* 24 words */
    uint32_t mode, bpp, pitch, height;
    uint32_t pipes, pipe_interleave, pi_bits, pipe_bits;
    uint32_t bank_bits, banks, bank_width, bank_height;
    uint32_t split, tile_split, micro_bytes, tiles_per_row;
    uint32_t mt_pitch_shift, mt_height_shift, mt_bytes, mt_per_row;
    uint32_t slice_bytes, bank_x_shift, bank_y_shift, display;
} EvkTile;

/* a texture resource: level 0 tiling, the levels' addresses and layout */
typedef struct EvkTex {             /* 72 words */
    uint32_t flags;                 /* EVK_TF_* */
    uint32_t fmt, numf, comp;       /* DATA_FORMAT, NUM_FORMAT_ALL, 4 x 2 bits */
    uint32_t dsel;                  /* 4 x 3 bits DST_SEL */
    uint32_t dim, width, height, depth;
    uint32_t base_level, last_level, base_array, last_array;
    uint32_t bpe;                   /* bytes per element (BCn: per block) */
    uint32_t pad[2];
    EvkTile tile;                   /* level 0 */
    uint32_t level_addr[16];        /* VRAM or GART byte offset of each level */
    uint32_t level_pm[16];          /* pitch (elements) | array mode << 28 */
} EvkTex;

#define EVK_TF_VALID        (1u << 0)
#define EVK_TF_GART         (1u << 1)
#define EVK_TF_INT          (1u << 2)
#define EVK_TF_DEGAMMA      (1u << 3)
#define EVK_TF_SRF          (1u << 4)
#define EVK_TF_BC           (1u << 5)

/* a color, depth or stencil buffer */
typedef struct EvkSurf {            /* 40 words */
    uint32_t base;                  /* VRAM or GART byte offset (depth / stencil: read) */
    uint32_t flags;                 /* EVK_SF_* */
    uint32_t fmt, number, chmap;    /* chmap: 4 x 4 bits channel -> component (15: none) */
    uint32_t bpp, nch, blend, mask;
    uint32_t width, height;
    uint32_t bits;                  /* 4 x 8 bits channel widths */
    uint32_t wbase;                 /* depth / stencil: write base */
    uint32_t pad[3];
    EvkTile tile;
} EvkSurf;

#define EVK_SF_ENABLED      (1u << 0)
#define EVK_SF_GART         (1u << 1)
#define EVK_SF_FLOAT        (1u << 2)
#define EVK_SF_BLEND        (1u << 3)
#define EVK_SF_BLEND_CLAMP  (1u << 4)

/* misc[] indices */
enum {
    EVK_M_CB_MODE, EVK_M_ROP, EVK_M_COLOR_MASK,  /* bit n: PS exported color n */
    EVK_M_ALPHA_TEST, EVK_M_ALPHA_FUNC, EVK_M_ALPHA_REF,
    EVK_M_Z_FMT, EVK_M_Z_ENABLE, EVK_M_Z_WRITE, EVK_M_Z_FUNC, EVK_M_S_VALID,
    EVK_M_S_ENABLE, EVK_M_DB_CTL, EVK_M_SREF0, EVK_M_SREF1,
    EVK_M_Z_CLEAR, EVK_M_Z_CLEAR_VAL, EVK_M_S_CLEAR, EVK_M_S_CLEAR_VAL,
    EVK_M_SC_X0, EVK_M_SC_Y0, EVK_M_SC_X1, EVK_M_SC_Y1,
    EVK_M_CLIP_RULE, EVK_M_CR0,                    /* 4 rects x 4 */
    EVK_M_BARYC = EVK_M_CR0 + 16, EVK_M_IN0, EVK_M_IN1,
    EVK_M_PIX_CENTER, EVK_M_ZPASS_SLOT, EVK_M_BOOLC,
    /* the vertex shader on the GPU (EgVkDrawReq.vs_glsl) */
    EVK_M_FLATMASK,                 /* PS inputs that take vertex 2 (provoking) */
    EVK_M_GPU_VS,                   /* 1: the front face comes from the GPU */
    EVK_M_VS_BOOLC, EVK_M_VTE, EVK_M_CLIP,
    EVK_M_VPORT,                    /* 6 floats: x scale, offset, y ..., z ... */
    EVK_M_WIN_OFS = EVK_M_VPORT + 6, /* 2 x int: window offset + pixel shift */
    EVK_M_NINTERP = EVK_M_WIN_OFS + 2,
    EVK_M_ISRC,                     /* per PS input: VS param (bits 0-7, 255:
                                     * default) | default value << 8 */
    EVK_M_EARLY_Z = EVK_M_ISRC + 12, /* 1: depth test before the program */
    /* a compute dispatch: the thread count, the threads per group, the
     * group size (3), the group count (3), the first group (3) */
    EVK_M_CS,
    EVK_M_COUNT = EVK_M_CS + 11
};
#define EVK_MISC_WORDS 96

typedef struct EvkDraw {
    uint32_t kc[EVK_MAX_KC][32][4]; /* the kcache windows (EvkProgInfo.kc_*) */
    uint32_t cfile[256][4];         /* the PS constant file (SET_ALU_CONST) */
    EvkTex tex[EVK_MAX_TEX];
    uint32_t smp[EVK_MAX_SMP][4];   /* sampler words */
    uint32_t vbuf[EVK_MAX_VBUF][8]; /* PS vertex-fetch resources, raw */
    EvkSurf cb[8];
    EvkSurf zb, sb;
    float blend_const[4];
    uint32_t loopc[32];             /* PS loop constants */
    uint32_t misc[EVK_MISC_WORDS];
    /* the vertex shader on the GPU (EgVkDrawReq.vs_glsl) */
    uint32_t vs_kc[EVK_MAX_KC][32][4];
    uint32_t vs_cfile[256][4];      /* the VS constant file (SET_ALU_CONST) */
    uint32_t vs_vbuf[EVK_MAX_VS_VBUF][8];
    uint32_t vs_loopc[32];
} EvkDraw;

typedef struct EvkVertex {
    float pos[4];                   /* window x, y (sample grid), z, clip w */
    uint32_t front, pad[3];
    float param[EVK_MAX_PARAMS][4]; /* the PS inputs, by interpolator index */
} EvkVertex;

/* GART and VRAM addresses share one space: GART above this (hazards) */
#define EVK_GART_SPACE  (1ull << 40)

/* ---- the translated pixel shader (eg_vk_ps.c) ---- */

typedef struct EvkProgInfo {
    int ntex, nsmp, nvbuf, nkc;
    uint8_t tex_rid[EVK_MAX_TEX];   /* slot -> resource id (PS relative) */
    uint8_t smp_sid[EVK_MAX_SMP];
    uint16_t vbuf_id[EVK_MAX_VS_VBUF]; /* VS: absolute resource ids */
    uint8_t kc_bank[EVK_MAX_KC], kc_mode[EVK_MAX_KC], kc_addr[EVK_MAX_KC];
    uint32_t ndw;                   /* program dwords the translation depends on */
    bool uses_cfile;                /* reads the constant file (D.cfile) */
    uint32_t rat_mask;              /* compute: the RATs (CB_COLORn) it writes */
} EvkProgInfo;

/*
 * Translate an Evergreen PS (dwords read through 'fetch', NULL past the
 * end) into the GLSL function "void ps_run()".  The PS input set up
 * (SPI_BARYC_CNTL, SPI_PS_IN_CONTROL_0/1) decides which GPRs start with
 * barycentrics, the position and the face, so it is part of the program.
 * NULL with a reason when it cannot be translated; free() the result.
 */
typedef const uint32_t *(*EvkFetchFn)(void *opaque, uint32_t dw, uint32_t n);
char *evk_ps_to_glsl(EvkFetchFn fetch, void *opaque, uint32_t baryc, uint32_t in0,
                     uint32_t in1, EvkProgInfo *info, char *why, size_t whylen);
/*
 * The same for an Evergreen VS and its fetch shader (CALL_FS, read through
 * 'fetch_fs'), into "void vs_run()": R0.x is the vertex index, R0.w the
 * instance, and the exports go to EPOS[] and EPAR[].  The info lists the
 * VS kcache windows and the absolute resource ids of the vertex buffers.
 */
char *evk_vs_to_glsl(EvkFetchFn fetch, void *opaque, EvkFetchFn fetch_fs, void *fs_opaque,
                     EvkProgInfo *info, char *why, size_t whylen);
/*
 * The same for a compute shader (DISPATCH_DIRECT), into "void cs_run()".
 * R0.xyz is the thread's index in its group, and R1.xyz is the group's
 * index.  MEM_RAT goes to rat_store().  The program uses the PS tables of
 * EvkDraw (kcache windows, textures, samplers, loop constants) and
 * CB_COLORn as the RATs.
 */
char *evk_cs_to_glsl(EvkFetchFn fetch, void *opaque, EvkProgInfo *info, char *why,
                     size_t whylen);
/* bumped whenever the translator's output changes (cache key) */
uint32_t evk_ps_build_id(void);

/* ---- host API ---- */

typedef struct EgVk EgVk;

typedef void (*EgVkDirtyFn)(void *opaque, uint64_t offset, uint64_t len);
typedef void (*EgVkWaitFn)(void *opaque, bool begin);

EgVk *eg_vk_create(void *vram, uint64_t vram_size, EgVkDirtyFn dirty, void *opaque,
                   char *err, size_t errlen);
void eg_vk_destroy(EgVk *vk);

typedef struct EgVkDrawReq {
    const EvkDraw *draw;
    const EvkVertex *verts;
    uint32_t nverts;
    const char *ps_glsl;            /* the program (used when not cached yet) */
    uint64_t prog_hash;
    int sc_x0, sc_y0, sc_x1, sc_y1; /* scissor, exclusive max */
    /* memory the draw samples (read) and renders to (write) */
    uint64_t rd_off[EVK_MAX_TEX + EVK_MAX_VBUF + EVK_MAX_VS_VBUF];
    uint64_t rd_len[EVK_MAX_TEX + EVK_MAX_VBUF + EVK_MAX_VS_VBUF];
    int nrd;
    uint64_t wr_off[10], wr_len[10], wr_sig[10];
    int nwr;
    /* the part of each wr_* range the draw can change (its scissor rows):
     * the VRAM mirror copies only these back (0 length: all of wr_*) */
    uint64_t wb_off[10], wb_len[10];
    /*
     * The vertex shader on the GPU: vs_glsl ("void vs_run()", from
     * evk_vs_to_glsl), and the triangles as indices of guest vertices
     * (3 per triangle, the provoking vertex last); verts is unused then.
     */
    const char *vs_glsl;
    uint64_t vs_hash;
    const uint32_t *idx;
    uint32_t nidx, ninst;
    bool cull_front, cull_back, front_ccw;
    /* a compute dispatch (eg_vk_dispatch) */
    const char *cs_glsl;
    uint32_t cs_groups[2];
} EgVkDrawReq;

/* false (nothing queued) when the pipeline is not usable (yet) */
bool eg_vk_draw(EgVk *vk, const EgVkDrawReq *req, char *why, size_t whylen);
/*
 * A compute dispatch in the current batch: req->cs_glsl (evk_cs_to_glsl),
 * req->prog_hash, req->draw (misc[EVK_M_CS...] set), the read and write
 * ranges, and req->cs_groups (workgroups of 64 invocations in x and y).
 * False (nothing queued) when the pipeline is not usable (yet).
 */
bool eg_vk_dispatch(EgVk *vk, const EgVkDrawReq *req, char *why, size_t whylen);
bool eg_vk_has_pipeline(EgVk *vk, uint64_t prog_hash);
bool eg_vk_set_sysram(EgVk *vk, void *ram, uint64_t size, char *err, size_t errlen);
/* GART page -> sysram page (bit 31: VRAM page instead), ~0 = unmapped */
uint32_t *eg_vk_gart_table(EgVk *vk, uint32_t npages);
bool eg_vk_mirrored(const EgVk *vk);
int eg_vk_set_cache_dir(EgVk *vk, const char *dir);
void eg_vk_new_cp_run(EgVk *vk);
void eg_vk_set_wait_fn(EgVk *vk, EgVkWaitFn fn);
/* fn runs on another thread each time a submitted batch completes */
void eg_vk_set_notify_fn(EgVk *vk, void (*fn)(void *opaque), void *opaque);
/* sum and count of the times from submission to the batch's completion */
void eg_vk_done_latency(const EgVk *vk, uint64_t *ns, uint64_t *n);
uint64_t eg_vk_gpu_done_seq(const EgVk *vk);
uint64_t eg_vk_binds(const EgVk *vk);
#define EVK_ZPASS_SLOTS 1024        /* the counts buffer: 4 KiB of uint32 */
uint32_t eg_vk_zpass_slot(const EgVk *vk);
void eg_vk_zpass_set(EgVk *vk, uint32_t slot);
uint32_t eg_vk_zpass_take(EgVk *vk, uint32_t slot);
void eg_vk_upload(EgVk *vk, uint64_t off, uint64_t len);
void eg_vk_upload_all_now(EgVk *vk);
void eg_vk_upload_kick(EgVk *vk);
/* eg_vk_submit: start the recorded draws, do not wait; eg_vk_flush: run
 * everything submitted and wait (returns the ZPASS count) */
void eg_vk_submit(EgVk *vk);
/* batches complete in submission order: eg_vk_submitted() numbers the last
 * one submitted, eg_vk_poll() recycles finished ones without waiting and
 * returns the number of the last one completed */
uint64_t eg_vk_submitted(const EgVk *vk);
uint64_t eg_vk_poll(EgVk *vk);
uint64_t eg_vk_flush(EgVk *vk);
bool eg_vk_pending(const EgVk *vk);
/* draws recorded in the batch not submitted yet */
uint32_t eg_vk_batch_draws(const EgVk *vk);
uint64_t eg_vk_wait_ns(const EgVk *vk);
void eg_vk_gpu_times(const EgVk *vk, uint64_t *gpu_ns, uint64_t *back_ns, uint64_t *lat_ns,
                     uint64_t *n);
void eg_vk_stats(const EgVk *vk, uint64_t *draws, uint64_t *batches, uint64_t *pipelines,
                 uint64_t *barriers);

/* the fragment shader template (eg_vk_glsl.h) */
const char *evk_frag_template(void);

#endif
