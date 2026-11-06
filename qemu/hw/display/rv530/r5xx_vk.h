/*
 * R5xx 3D engine on Vulkan: rasterisation, fragment shading, texturing and
 * the output merger run on the host GPU; the vertex stage stays on the CPU.
 *
 * Guest VRAM is imported into Vulkan as a storage buffer
 * (VK_EXT_external_memory_host), so the host GPU reads textures from and
 * writes render targets to the very bytes the guest CPU and the scanout
 * see: no copies and no coherence tracking.  Tiling, format conversion,
 * blending, depth/stencil and the R500 fragment program are emulated in
 * generated GLSL; fragment shader interlock keeps read-modify-write in
 * primitive order.  The CPU path (r5xx_raster.c) is the reference and the
 * fallback for anything this backend does not handle.
 *
 * Pure C (no QEMU headers).  Built against Vulkan + shaderc when they are
 * available, otherwise r5xx_vk_stub.c provides r5xx_vk_create() == NULL.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef R5XX_VK_H
#define R5XX_VK_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "r5xx_core.h"
#include "r5xx_tex.h"
#include "r5xx_raster.h"
#include "r500_us.h"

#define RVK_MAX_INPUTS  12

/* ---- GPU-visible structures (std430 layout, mirrored in the GLSL) ---- */

typedef struct RvkSampler {          /* 336 bytes */
    uint32_t base;                   /* VRAM byte offset of level 0 */
    uint32_t width, height, num_levels;
    uint32_t bpp;                    /* bytes per texel */
    uint32_t fmt;                    /* kind | nchan<<4 | bits[i]<<(8+5i) */
    uint32_t flags;                  /* RVK_SF_* */
    uint32_t swz;                    /* 4 x 4-bit R300_TX_FORMAT_SEL_* */
    uint32_t wrap;                   /* s | t << 4 | r << 8 (R5xxWrap) */
    uint32_t filt;                   /* min | mag << 4 | mip << 8 (R5xxFilter) */
    uint32_t signed_mask;
    uint32_t micro_mode;
    float lod_bias, min_lod, max_lod;
    uint32_t depth;                  /* 3D: slices of level 0, else 1 */
    float border[4];
    uint32_t level_offset[16];
    uint32_t level_pitch[16];
    uint32_t level_tiling[16];       /* R5xxTiling (linear if under a surface) */
    uint32_t level_layer[16];        /* 3D: bytes per slice of each level */
} RvkSampler;

#define RVK_SF_VALID     (1u << 0)
#define RVK_SF_GAMMA     (1u << 1)
#define RVK_SF_GART      (1u << 2)      /* base is a GART offset */
#define RVK_SF_ENDIAN_SHIFT 3           /* 2 bits: R5XX_ENDIAN_* */
#define RVK_SF_YUV_TO_RGB (1u << 5)     /* R300_TX_FORMAT_YUV_TO_RGB */
#define RVK_SF_YVYU      (1u << 6)      /* YUV422 byte order U Y0 V Y1 */
#define RVK_SF_3D        (1u << 7)

#define RVK_FK_UNORM 0u
#define RVK_FK_F16   1u
#define RVK_FK_F32   2u
#define RVK_FK_YUV422 3u                /* 2 texels per dword (video) */
#define RVK_FK_AYUV444 4u

typedef struct RvkSurf {             /* 48 bytes */
    uint32_t base, pitch, bpp, tiling, micro, fmt, flags, cmask, chansrc;
    uint32_t pad[3];
} RvkSurf;

#define RVK_CF_ENABLED  (1u << 0)
#define RVK_CF_BGRA     (1u << 1)
#define RVK_CF_FLOAT    (1u << 2)
#define RVK_CF_GART     (1u << 3)        /* base is a GART offset */
#define RVK_ZF_Z16      (1u << 1)
#define RVK_ZF_STENCIL  (1u << 2)

/* misc[] indices */
enum {
    RVK_M_BLEND, RVK_M_RGB_EQ, RVK_M_RGB_SRC, RVK_M_RGB_DST, RVK_M_A_EQ,
    RVK_M_A_SRC, RVK_M_A_DST, RVK_M_DISCARD, RVK_M_ROP_EN, RVK_M_ROP,
    RVK_M_ALPHA_TEST, RVK_M_ALPHA_FUNC, RVK_M_ALPHA_REF, RVK_M_Z_EN,
    RVK_M_Z_WRITE, RVK_M_Z_FUNC, RVK_M_S_EN, RVK_M_S_FUNC0, RVK_M_S_FUNC1,
    RVK_M_S_FAIL0, RVK_M_S_FAIL1, RVK_M_S_ZFAIL0, RVK_M_S_ZFAIL1,
    RVK_M_S_ZPASS0, RVK_M_S_ZPASS1, RVK_M_S_REF0, RVK_M_S_REF1,
    RVK_M_S_MASK0, RVK_M_S_MASK1, RVK_M_S_WMASK0, RVK_M_S_WMASK1,
    RVK_M_DEPTH_SCALE, RVK_M_DEPTH_OFFSET, RVK_M_MULTIWRITES,
    RVK_M_SC_X0, RVK_M_SC_Y0, RVK_M_SC_X1, RVK_M_SC_Y1, RVK_M_CLIP_RULE,
    RVK_M_CL_X0, RVK_M_CL_Y0 = RVK_M_CL_X0 + 4, RVK_M_CL_X1 = RVK_M_CL_Y0 + 4,
    RVK_M_CL_Y1 = RVK_M_CL_X1 + 4, RVK_M_CMASK_INDEP = RVK_M_CL_Y1 + 4,
    RVK_M_ZPASS_SLOT, RVK_M_DEPTH_FROM_SHADER,
    RVK_M_COUNT
};
#define RVK_MISC_WORDS 96

typedef struct RvkDraw {             /* 9088 bytes */
    float consts[256][4];
    float blend_const[4];
    RvkSampler samp[16];
    RvkSurf cb[4];
    RvkSurf zb;
    uint32_t misc[RVK_MISC_WORDS];
} RvkDraw;

typedef struct RvkVertex {           /* 416 bytes */
    float pos[4];                    /* window x, y, (unused), clip w */
    float z;                         /* window z (screen-linear) */
    uint32_t flatmask, front;
    float zoff;                      /* polygon offset, depth units */
    float smooth[RVK_MAX_INPUTS][4];
    float flat[RVK_MAX_INPUTS][4];   /* provoking vertex values */
} RvkVertex;

/* Hazard / dirty ranges of GART memory live above this offset. */
#define RVK_GART_SPACE  (1ull << 40)

/* ---- host API ---- */

typedef struct R5xxVk R5xxVk;

/* Called after a batch completes for every VRAM byte range the GPU may
 * have written (display dirty tracking). */
typedef void (*R5xxVkDirtyFn)(void *opaque, uint64_t offset, uint64_t len);

R5xxVk *r5xx_vk_create(void *vram, uint64_t vram_size,
                       R5xxVkDirtyFn dirty, void *opaque,
                       char *err, size_t errlen);
#ifdef __APPLE__
/* r5xx_vk_mac.m: false (and why in err) when loading MoltenVK would abort
 * the process or find no device the backend can use. */
bool r5xx_vk_metal_usable(char *err, size_t errlen);
#endif
void r5xx_vk_destroy(R5xxVk *vk);

/* A draw ready for the GPU.  'draw' must be fully filled; verts holds
 * nverts window-space vertices forming triangles (3 per triangle).
 * 'prog' / 'prog_hash' identify the fragment program.  dirty_* name VRAM
 * byte ranges this draw may write.  Returns false (nothing queued) when
 * the program cannot be translated; the caller then flushes and uses the
 * CPU path. */
typedef struct R5xxVkDrawReq {
    const RvkDraw *draw;
    const RvkVertex *verts;
    uint32_t nverts;
    const R500UsDecoded *prog;
    uint64_t prog_hash;
    int sc_x0, sc_y0, sc_x1, sc_y1;  /* inclusive scissor (bbox of output) */
    uint64_t dirty_off[5], dirty_len[5], dirty_sig[5];
    int ndirty;
} R5xxVkDrawReq;

bool r5xx_vk_draw(R5xxVk *vk, const R5xxVkDrawReq *req, char *why,
                  size_t whylen);
/* Guest system RAM, for GART-resident surfaces (optional). */
bool r5xx_vk_set_sysram(R5xxVk *vk, void *ram, uint64_t size, char *err,
                        size_t errlen);
/* GART translation table (GART page -> sysram page, ~0 = unmapped),
 * sized for npages; flushes pending work when it has to grow.  NULL when
 * no sysram is attached. */
uint32_t *r5xx_vk_gart_table(R5xxVk *vk, uint32_t npages);
/* Device-local VRAM mirror: true when shaders work on a GPU-side copy.
 * The caller must then report every VRAM range changed outside the GPU
 * (guest CPU, CPU-side engines) with r5xx_vk_upload() before the next
 * draw; ranges the GPU renders are copied back at r5xx_vk_flush(). */
bool r5xx_vk_mirrored(const R5xxVk *vk);
/* Persistent SPIR-V cache in 'dir' (must exist); queues every cached
 * program for pipeline creation.  Returns the number queued. */
int r5xx_vk_set_cache_dir(R5xxVk *vk, const char *dir);
/* A new CP run starts: resets the shared compile-wait budget. */
void r5xx_vk_new_cp_run(R5xxVk *vk);
/* Called with begin = true before a draw sleeps waiting for a pending
 * pipeline and with begin = false after it (same opaque as 'dirty').  The
 * device releases its big lock in between; the backend touches no device
 * state while waiting. */
typedef void (*R5xxVkWaitFn)(void *opaque, bool begin);
void r5xx_vk_set_wait_fn(R5xxVk *vk, R5xxVkWaitFn fn);
void r5xx_vk_upload(R5xxVk *vk, uint64_t off, uint64_t len);
void r5xx_vk_upload_all_now(R5xxVk *vk);
void r5xx_vk_upload_kick(R5xxVk *vk);
/* RV530_VK_CHECK_MIRROR: pages changed behind the mirror's back;
 * see r5xx_vk.c */
uint64_t *r5xx_vk_check_scan(R5xxVk *vk);
void r5xx_vk_check_missed(R5xxVk *vk, uint64_t pg);
/* Submit queued draws and wait.  Returns the number of fragments that
 * passed the depth/stencil tests (occlusion counter) since the last
 * flush. */
uint64_t r5xx_vk_flush(R5xxVk *vk);
bool r5xx_vk_pending(const R5xxVk *vk);
/* statistics */
void r5xx_vk_stats(const R5xxVk *vk, uint64_t *draws, uint64_t *batches,
                   uint64_t *pipelines);

/* ---- state conversion (r5xx_vk_state.c, pure C) ---- */

/* Fill a draw from the CPU path's decoded state.  False (with a reason)
 * when something needs the CPU path. */
typedef struct RvkMem {
    uint32_t fb_start;
    uint64_t vram_size;
    /* GART: usable when gart_prepare is set; it makes the translation
     * table cover [gart_off, gart_off + len) and fails for unmapped pages */
    uint32_t gart_start, gart_end;
    bool (*gart_prepare)(void *opaque, uint32_t gart_off, uint32_t len);
    void *opaque;
} RvkMem;

/* Fragment shader source: "#version" and the RVK_* defines (malloc'd),
 * the template with "@US_PROGRAM@", and the two with r500_us_to_glsl()'s
 * output spliced in (malloc'd). */
char *rvk_frag_prefix(bool has_f64);
const char *rvk_frag_template(void);
char *rvk_frag_source(const char *prefix, const char *us_glsl);

bool rvk_build_draw(RvkDraw *out, const R5xxRaster *r,
                    const R5xxTexSampler *samp, uint32_t tex_used,
                    const r5xx_vec4 *consts, const RvkMem *m,
                    char *why, size_t whylen);
/* One triangle as r5xx_raster_triangle() would draw it: 3 vertices
 * written, 0 when culled, -1 when it needs the CPU path. */
int rvk_triangle(RvkVertex out[3], const R5xxRaster *r, const R5xxVertex *v0,
                 const R5xxVertex *v1, const R5xxVertex *v2, bool nocull);
/* A point sprite without texture stuffing as two triangles (6 vertices). */
int rvk_point(RvkVertex out[6], const R5xxRaster *r, const R5xxVertex *v,
              float half_w, float half_h);
/* VRAM ranges of the bound color/depth buffers for rows y0..y1. */
void rvk_dirty_ranges(R5xxVkDrawReq *req, const R5xxRaster *r,
                      const RvkMem *m, int y0, int y1);

#endif /* R5XX_VK_H */
