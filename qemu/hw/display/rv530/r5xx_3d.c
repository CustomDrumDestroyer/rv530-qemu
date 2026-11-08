/*
 * RV530 3D engine glue (CPU reference path).
 *
 * Latches the 3D register state, the PVS (vertex program) and US (fragment
 * program) uploads, and executes the PM4 draw packets:
 *
 *   vertex fetch (PSC: VAP_PROG_STREAM_CNTL[_EXT], 3D_LOAD_VBPNTR arrays or
 *   immediate data) -> PVS program (r5xx_pvs.c) or TCL bypass ->
 *   output slots (VAP_OUTPUT_VTX_FMT_0/1) -> clipping (w, near plane, user
 *   clip planes) -> viewport transform (VAP_VTE_CNTL, SE_VPORT_*) ->
 *   RS routing (RS_IP_n / RS_INST_n) -> primitive assembly ->
 *   r5xx_raster.c (fragment program r500_us.c, textures r5xx_tex.c,
 *   per-pixel state, render targets).
 *
 * Register semantics follow Mesa src/gallium/drivers/r300 (r300_reg.h,
 * r300_emit.c, r300_render.c, r300_state_derived.c, r300_vs.c) and the
 * Linux kernel CS checker (r300.c).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/rcu.h"
#include "qemu/thread.h"
#include <math.h>
#include "rv530.h"
#include "r5xx_pvs.h"
#include "r500_us.h"
#include "r5xx_tex.h"
#include "r5xx_raster.h"
#include "r5xx_vk.h"
#include "qemu/error-report.h"
#include "qemu/timer.h"
#include "hw/core/boards.h"
#include "trace.h"

static void rv530_vk_dirty_cb(void *opaque, uint64_t off, uint64_t len)
{
    RV530State *s = opaque;

    if (r5xx_vk_mirrored(s->vk)) {
        rv530_avivo_mark_dirty(s, off, len);
    } else {
        memory_region_set_dirty(&s->vram, off, len);
    }
}

typedef struct R5xxGfx R5xxGfx;

/* RvkMem.gart_prepare: make the GPU's GART table cover a range */
static bool rv530_vk_gart_prepare(void *opaque, uint32_t off, uint32_t len);

/* ---- registers (Mesa r300_reg.h) ---- */
#define SE_VPORT_XSCALE           0x1d98
#define SE_VPORT_XOFFSET          0x1d9c
#define SE_VPORT_YSCALE           0x1da0
#define SE_VPORT_YOFFSET          0x1da4
#define SE_VPORT_ZSCALE           0x1da8
#define SE_VPORT_ZOFFSET          0x1dac
#define VAP_CNTL                  0x2080
#define VAP_VF_CNTL               0x2084
#define VAP_ALT_NUM_VERTICES      0x2088
#define VAP_INDEX_OFFSET          0x208c
#define VAP_OUTPUT_VTX_FMT_0      0x2090
#define VAP_OUTPUT_VTX_FMT_1      0x2094
#define VAP_VTE_CNTL              0x20b0
#define VAP_VTX_SIZE              0x20b4
#define VAP_VF_MAX_VTX_INDX       0x2134
#define VAP_VF_MIN_VTX_INDX       0x2138
#define VAP_CNTL_STATUS           0x2140
#define VAP_PROG_STREAM_CNTL_0    0x2150
#define VAP_PROG_STREAM_CNTL_EXT_0 0x21e0
#define VAP_PVS_VECTOR_INDX       0x2200
#define VAP_PVS_VECTOR_DATA       0x2204
#define VAP_PVS_UPLOAD_DATA       0x2208
#define VAP_CLIP_CNTL             0x221c
#define VAP_PVS_FLOW_CNTL_ADDRS_0 0x2230
#define VAP_PVS_FLOW_CNTL_LOOP_INDEX_0 0x2290
#define VAP_PVS_STATE_FLUSH       0x2284
#define VAP_PVS_CODE_CNTL_0       0x22d0
#define VAP_PVS_CONST_CNTL        0x22d4
#define VAP_PVS_CODE_CNTL_1       0x22d8
#define VAP_PVS_FLOW_CNTL_OPC     0x22dc
#define VAP_PVS_FLOW_CNTL_ADDRS_LW_0 0x2500
#define VAP_PVS_FLOW_CNTL_ADDRS_UW_0 0x2504
#define GB_TILE_CONFIG            0x4018
#define GB_SELECT                 0x401c
#define GB_AA_CONFIG              0x4020
#define R500_RS_IP_0              0x4074
#define TX_ENABLE                 0x4104
#define GA_POINT_SIZE             0x421c
#define GA_POINT_S0               0x4200
#define GA_POINT_T0               0x4204
#define GA_POINT_S1               0x4208
#define GA_POINT_T1               0x420c
#define GB_ENABLE                 0x4008
#define GB_POINT_STUFF_ENABLE     (1u << 0)
#define GA_POINT_MINMAX           0x4230
#define GA_LINE_CNTL              0x4234
#define GA_US_VECTOR_INDEX        0x4250
#define GA_US_VECTOR_DATA         0x4254
#define GA_COLOR_CONTROL_PS3      0x4258
#define GA_COLOR_CONTROL          0x4278
#define GA_POLY_MODE              0x4288
#define GA_ROUND_MODE             0x428c
#define GA_OFFSET                 0x4290
#define GA_FOG_SCALE              0x4294
#define GA_FOG_OFFSET             0x4298
#define SU_POLY_OFFSET_FRONT_SCALE 0x42a4
#define SU_POLY_OFFSET_FRONT_OFFSET 0x42a8
#define SU_POLY_OFFSET_BACK_SCALE 0x42ac
#define SU_POLY_OFFSET_BACK_OFFSET 0x42b0
#define SU_POLY_OFFSET_ENABLE     0x42b4
#define SU_CULL_MODE              0x42b8
#define SU_DEPTH_SCALE            0x42c0
#define SU_DEPTH_OFFSET           0x42c4
#define RS_COUNT                  0x4300
#define RS_INST_COUNT             0x4304
#define R500_RS_INST_0            0x4320
#define SC_EDGERULE               0x43a8
#define SC_CLIP_0_A               0x43b0
#define SC_CLIP_0_B               0x43b4
#define SC_CLIP_RULE              0x43d0
#define SC_SCISSORS_TL            0x43e0
#define SC_SCISSORS_BR            0x43e4
#define TX_FILTER0_0              0x4400
#define TX_FILTER1_0              0x4440
#define TX_FORMAT0_0              0x4480
#define TX_FORMAT1_0              0x44c0
#define TX_FORMAT2_0              0x4500
#define TX_OFFSET_0               0x4540
#define TX_CHROMA_KEY_0           0x4580
#define TX_BORDER_COLOR_0         0x45c0
#define US_CONFIG                 0x4600
#define US_PIXSIZE                0x4604
#define US_FC_BOOL_CONST          0x4620
#define US_FC_CTRL                0x4624
#define US_CODE_ADDR              0x4630
#define US_CODE_RANGE             0x4634
#define US_CODE_OFFSET            0x4638
#define US_OUT_FMT_0              0x46a4
#define US_W_FMT                  0x46b4
#define FG_FOG_BLEND              0x4bc0
#define FG_FOG_FACTOR             0x4bc4
#define FG_FOG_COLOR_R            0x4bc8
#define FG_FOG_COLOR_G            0x4bcc
#define FG_FOG_COLOR_B            0x4bd0
#define FG_ALPHA_FUNC             0x4bd4
#define FG_DEPTH_SRC              0x4bd8
#define FG_ALPHA_VALUE            0x4be0
#define US_FC_INT_CONST_0         0x4c00
#define RB3D_CCTL                 0x4e00
#define RB3D_BLENDCNTL            0x4e04
#define RB3D_ABLENDCNTL           0x4e08
#define RB3D_COLOR_CHANNEL_MASK   0x4e0c
#define RB3D_BLEND_COLOR          0x4e10
#define RB3D_ROPCNTL              0x4e18
#define RB3D_CLRCMP_FLIPE         0x4e1c
#define RB3D_CLRCMP_CLR           0x4e20
#define RB3D_CLRCMP_MSK           0x4e24
#define RB3D_COLOROFFSET0         0x4e28
#define RB3D_COLORPITCH0          0x4e38
#define RB3D_DSTCACHE_CTLSTAT     0x4e4c
#define RB3D_DITHER_CTL           0x4e50
#define RB3D_CONSTANT_COLOR_AR    0x4ef8
#define RB3D_CONSTANT_COLOR_GB    0x4efc
#define ZB_CNTL                   0x4f00
#define ZB_ZSTENCILCNTL           0x4f04
#define ZB_STENCILREFMASK         0x4f08
#define ZB_FORMAT                 0x4f10
#define ZB_ZTOP                   0x4f14
#define ZB_ZCACHE_CTLSTAT         0x4f18
#define ZB_DEPTHOFFSET            0x4f20
#define ZB_DEPTHPITCH             0x4f24
#define ZB_DEPTHCLEARVALUE        0x4f28
#define ZB_ZPASS_DATA             0x4f58
#define ZB_ZPASS_ADDR             0x4f5c
#define ZB_STENCILREFMASK_BF      0x4fd4
#define US_FC_INST_0              0x9800
#define US_FC_ADDR_0              0xa000

/* VAP_VF_CNTL */
#define VF_PRIM(v)                ((v) & 0xf)
#define VF_WALK(v)                (((v) >> 4) & 3)
#define VF_INDEX_32               (1u << 11)
#define VF_USE_ALT_NUM            (1u << 14)
#define VF_NUM(v)                 ((v) >> 16)
#define WALK_STATE                0
#define WALK_INDICES              1
#define WALK_LIST                 2
#define WALK_EMBEDDED             3

/* VAP_VTE_CNTL */
#define VTE_X_SCALE_ENA           (1u << 0)
#define VTE_X_OFFSET_ENA          (1u << 1)
#define VTE_Y_SCALE_ENA           (1u << 2)
#define VTE_Y_OFFSET_ENA          (1u << 3)
#define VTE_Z_SCALE_ENA           (1u << 4)
#define VTE_Z_OFFSET_ENA          (1u << 5)
#define VTE_XY_FMT                (1u << 8)   /* x,y already divided by w */
#define VTE_Z_FMT                 (1u << 9)   /* z already divided by w */
#define VTE_W0_FMT                (1u << 10)  /* w holds 1/w */

#define VAP_TCL_BYPASS            (1u << 8)
#define VAP_DX_CLIP_SPACE         (1u << 22)
#define CLIP_DISABLE              (1u << 16)

/* PM4 type 3 opcodes */
#define PM4_3D_DRAW_VBUF          0x28
#define PM4_3D_DRAW_IMMD          0x29
#define PM4_3D_DRAW_INDX          0x2a
#define PM4_3D_LOAD_VBPNTR        0x2f
#define PM4_3D_CLEAR_ZMASK        0x32
#define PM4_INDX_BUFFER           0x33
#define PM4_3D_DRAW_VBUF_2        0x34
#define PM4_3D_DRAW_IMMD_2        0x35
#define PM4_3D_DRAW_INDX_2        0x36
#define PM4_3D_CLEAR_HIZ          0x37
#define PM4_3D_CLEAR_CMASK        0x38
#define PM4_3D_DRAW_128           0x39

#define PVS_MEM_VECS              2048        /* code 0..1023, const 1024.., ucp 1536.. */
#define PVS_CONST_START           1024
#define PVS_UCP_START             1536
#define NUM_SLOTS                 14          /* pos, psize, col0-3, tex0-7 */
#define SLOT_POS                  0
#define SLOT_PSIZE                1
#define SLOT_COL0                 2
#define SLOT_TEX0                 6
#define MAX_VBUF                  16
#define VAP_VTX_NUM_ARRAYS        0x20c0
#define VAP_VTX_AOS_LAST          0x2120
#define VCACHE_SIZE               256
#define MAX_CLIP_VERTS            24

#define REG(s, off) ((s)->regs[(off) >> 2])

typedef struct GfxVert {
    r5xx_vec4 slot[NUM_SLOTS];        /* clip-space output slots */
} GfxVert;

typedef struct R5xxGfx {
    /* PVS memory (code, constants, user clip planes) */
    union {
        uint32_t u[PVS_MEM_VECS * 4];
        float f[PVS_MEM_VECS * 4];
    } pvs;
    uint32_t pvs_index;               /* dword index of the next PVS write */
    bool pvs_dirty;
    R5xxPvsProgram pvs_prog;
    R5xxPvsDecoded *pvs_dec;

    /* US memory */
    uint32_t us_code[R500_US_MAX_INSTR * 6];
    union {
        uint32_t u[R500_US_MAX_CONST * 4];
        float f[R500_US_MAX_CONST * 4];
    } us_const;
    uint32_t us_index;                /* dword index of the next US write */
    bool us_is_const;
    bool us_dirty;
    R500UsProgram us_prog;
    R500UsDecoded *us_dec;

    /* vertex arrays from 3D_LOAD_VBPNTR */
    struct {
        uint32_t addr, stride, size;  /* bytes */
    } vbuf[MAX_VBUF];
    int num_vbuf;

    /* 3D_DRAW_INDX_2 waiting for its INDX_BUFFER */
    bool indx_pending;
    uint32_t indx_vf_cntl;

    /* per-draw state */
    R5xxTexSampler samplers[R5XX_NUM_TEX_UNITS];
    R5xxRaster raster;
    uint32_t vcache_tag[VCACHE_SIZE];
    GfxVert vcache[VCACHE_SIZE];
    uint64_t zpass_base;              /* occlusion counter at ZB_ZPASS_DATA write */
    uint32_t zpass_data;

    /* statistics */
    uint64_t draws, prims, verts;
    uint32_t warned;
    uint32_t dbg_tex;
    uint64_t us_hash;

    /* Vulkan backend: window-space primitives collected by gfx_draw */
    bool vk_collect, vk_unsupported;
    RvkVertex *vk_verts;
    uint32_t vk_nverts, vk_cap;
    float vk_y0, vk_y1;
    uint64_t vk_fallbacks, vk_verify_draws, vk_verify_bad, vk_batches;
    /* vk-verify budget: reference-render time in this CP run, the
     * run it belongs to, and the measured CPU cost of each program */
    int64_t verify_ns;
    uint64_t verify_run, vk_verify_skipped;
    GHashTable *verify_cost;            /* us_hash -> ns per 1024 pixels */
    /* GART translation for the GPU path: entries valid for gart_gen */
    uint32_t *gart_gen_of;
    uint32_t gart_gen_pages, gart_gen;
    /* RV530_VK_STATS accounting (ns) */
    int64_t st_cpu_ns, st_flush_ns, st_last;
    uint64_t st_cpu_draws, st_gpu_draws, st_flushes, st_compiling;
} R5xxGfx;

static void gfx_warn(RV530State *s, const char *fmt, ...) G_GNUC_PRINTF(2, 3);
static void gfx_warn(RV530State *s, const char *fmt, ...)
{
    va_list ap;
    char buf[256];

    if (s->gfx->warned >= 200) {
        return;
    }
    s->gfx->warned++;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    qemu_log_mask(LOG_UNIMP, "rv530-3d: %s\n", buf);
}

/* ------------------------------------------------------------------ */
/* Init / reset                                                        */
/* ------------------------------------------------------------------ */

static void cpu_pool_init(void);

void rv530_gfx_init(RV530State *s)
{
    s->gfx = g_new0(R5xxGfx, 1);
    cpu_pool_init();
}

void rv530_gfx_reset(RV530State *s)
{
    R5xxGfx *g = s->gfx;

    if (g->pvs_dec) {
        r5xx_pvs_free(g->pvs_dec);
    }
    if (g->us_dec) {
        r500_us_free(g->us_dec);
    }
    memset(g, 0, sizeof(*g));
    g->pvs_dirty = g->us_dirty = true;
}

void rv530_gfx_free(RV530State *s)
{
    if (s->gfx) {
        if (s->gfx->pvs_dec) {
            r5xx_pvs_free(s->gfx->pvs_dec);
        }
        if (s->gfx->us_dec) {
            r500_us_free(s->gfx->us_dec);
        }
        g_free(s->gfx);
        s->gfx = NULL;
    }
}

/*
 * Parallel CPU rasterization.  A CPU-path draw (no Vulkan, a program still
 * compiling, unsupported state) runs inside the vCPU that kicked the CP; a
 * full-screen Core Image pass took ~500 ms on one thread, enough for an SMP
 * guest to panic (TLB shootdown timeout, 250 ms).  Large draws first collect
 * their window-space primitives, then every worker replays all of them in
 * order into its own copy of the rasterizer clipped to one band of rows, so
 * each pixel still sees the primitives in submission order.
 */
enum { CPU_TRI, CPU_POINT, CPU_POINT_SIZED, CPU_LINE };
typedef struct CpuPrim {
    uint8_t kind;
    /*
     * Rasterizer state the setup changes around single primitives:
     * gfx_emit_stuffed_point() draws a point sprite as two triangles with
     * flat shading and culling off, then restores them, before the pool
     * replays anything.
     */
    bool cull_front, cull_back;
    uint32_t flat_inputs;
    float size;
    R5xxVertex v[3];
} CpuPrim;

#define CPU_MAX_THREADS 16
static struct {
    int nthreads;
    bool collect, quit;
    CpuPrim *prims;
    uint32_t n, cap;
    QemuThread th[CPU_MAX_THREADS];
    QemuSemaphore go[CPU_MAX_THREADS], done;
    R5xxRaster r[CPU_MAX_THREADS];
} cpool;

static void cpu_replay(R5xxRaster *r)
{
    uint32_t i;

    for (i = 0; i < cpool.n; i++) {
        const CpuPrim *p = &cpool.prims[i];
        r->flat_inputs = p->flat_inputs;
        r->d.cull_front = p->cull_front;
        r->d.cull_back = p->cull_back;
        switch (p->kind) {
        case CPU_TRI:
            r5xx_raster_triangle(r, &p->v[0], &p->v[1], &p->v[2]);
            break;
        case CPU_POINT:
            r5xx_raster_point(r, &p->v[0]);
            break;
        case CPU_POINT_SIZED:
            r5xx_raster_point_sized(r, &p->v[0], p->size);
            break;
        case CPU_LINE:
            r5xx_raster_line(r, &p->v[0], &p->v[1]);
            break;
        }
    }
}

static void *cpu_worker(void *arg)
{
    int i = (int)(intptr_t)arg;

    rcu_register_thread();
    for (;;) {
        qemu_sem_wait(&cpool.go[i]);
        if (cpool.quit) {
            break;
        }
        cpu_replay(&cpool.r[i]);
        qemu_sem_post(&cpool.done);
    }
    rcu_unregister_thread();
    return NULL;
}

static void cpu_pool_init(void)
{
    const char *e = getenv("RV530_CPU_THREADS");
    long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
    int n = e ? atoi(e) : (int)MIN(8, ncpu / 2), i;

    if (cpool.nthreads || n < 2) {
        return;
    }
    n = MIN(n, CPU_MAX_THREADS);
    qemu_sem_init(&cpool.done, 0);
    for (i = 0; i < n; i++) {
        qemu_sem_init(&cpool.go[i], 0);
        qemu_thread_create(&cpool.th[i], "rv530-raster", cpu_worker,
                           (void *)(intptr_t)i, QEMU_THREAD_DETACHED);
    }
    cpool.nthreads = n;
}

static CpuPrim *cpu_add(const R5xxRaster *r, uint8_t kind)
{
    CpuPrim *p;

    if (cpool.n == cpool.cap) {
        cpool.cap = cpool.cap ? cpool.cap * 2 : 1024;
        cpool.prims = g_renew(CpuPrim, cpool.prims, cpool.cap);
    }
    p = &cpool.prims[cpool.n++];
    p->kind = kind;
    p->flat_inputs = r->flat_inputs;
    p->cull_front = r->d.cull_front;
    p->cull_back = r->d.cull_back;
    return p;
}

static float rv_f32(uint32_t v)
{
    float f;
    memcpy(&f, &v, 4);
    return f;
}

static bool rvk_stats_on;
static bool rvk_flush_each;      /* RV530_VK_FLUSH_EACH: debug */

static void gfx_stats_tick(RV530State *s)
{
    R5xxGfx *g = s->gfx;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);

    if (!g->st_last) {
        g->st_last = now;
        return;
    }
    if (now - g->st_last < 2000000000LL) {
        return;
    }
    fprintf(stderr, "rv530-vk stats/2s: gpu draws %" PRIu64 ", cpu draws %" PRIu64
            " (%.0f ms), compiling %" PRIu64 ", flushes %" PRIu64 " (%.0f ms waiting)\n",
            g->st_gpu_draws, g->st_cpu_draws, g->st_cpu_ns / 1e6, g->st_compiling,
            g->st_flushes, g->st_flush_ns / 1e6);
    g->st_cpu_ns = g->st_flush_ns = 0;
    g->st_cpu_draws = g->st_gpu_draws = g->st_flushes = g->st_compiling = 0;
    g->st_last = now;
}

static bool rv530_vk_gart_prepare(void *opaque, uint32_t off, uint32_t len)
{
    RV530State *s = opaque;
    R5xxGfx *g = s->gfx;
    uint32_t npages = ((s->gart_end - s->gart_start) >> 12) + 1;
    uint32_t p, last = (off + len - 1) >> 12;
    uint32_t *tab;

    if (!s->vk_sysram || !len) {
        return false;
    }
    tab = r5xx_vk_gart_table(s->vk, npages);
    if (!tab || last >= npages) {
        return false;
    }
    if (g->gart_gen_pages < npages) {
        g->gart_gen_of = g_renew(uint32_t, g->gart_gen_of, npages);
        memset(g->gart_gen_of + g->gart_gen_pages, 0,
               (npages - g->gart_gen_pages) * sizeof(uint32_t));
        g->gart_gen_pages = npages;
    }
    for (p = off >> 12; p <= last; p++) {
        uint64_t ram_off;
        if (g->gart_gen_of[p] == g->gart_gen) {
            continue;
        }
        if (!rv530_gart_ram_page(s, s->gart_start + (p << 12), s->vk_sysram,
                                 &ram_off) || (ram_off >> 12) > 0xffffffffull) {
            return false;
        }
        tab[p] = (uint32_t)(ram_off >> 12);
        g->gart_gen_of[p] = g->gart_gen;
    }
    return true;
}

void rv530_gfx_gart_changed(RV530State *s)
{
    rv530_gfx_flush(s);
    s->gfx->gart_gen++;
}

void rv530_gfx_flush(RV530State *s)
{
    R5xxGfx *g = s->gfx;

    if (s->vk && r5xx_vk_pending(s->vk)) {
        g->gart_gen++;          /* the guest may edit PTEs between batches */
        int64_t t0 = rvk_stats_on ? qemu_clock_get_ns(QEMU_CLOCK_REALTIME) : 0;
        int64_t f0 = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
        g->raster.zpass_count += r5xx_vk_flush(s->vk);
        s->prof_ns[RV_PROF_FLUSH] += qemu_clock_get_ns(QEMU_CLOCK_REALTIME) - f0;
        s->prof_n[RV_PROF_FLUSH]++;
        if (rvk_stats_on) {
            g->st_flush_ns += qemu_clock_get_ns(QEMU_CLOCK_REALTIME) - t0;
            g->st_flushes++;
            gfx_stats_tick(s);
        }
        if ((++g->vk_batches & 1023) == 0) {
            uint64_t draws, batches, pipes;
            r5xx_vk_stats(s->vk, &draws, &batches, &pipes);
            trace_rv530_vk_stats(draws, batches, pipes, g->vk_fallbacks,
                                 g->vk_verify_draws, g->vk_verify_bad,
                                 g->vk_verify_skipped);
        }
    }
}

void rv530_gfx_vk_init(RV530State *s)
{
    char err[512];

    rvk_stats_on = getenv("RV530_VK_STATS") != NULL;
    rvk_flush_each = getenv("RV530_VK_FLUSH_EACH") != NULL;
    if (!s->vulkan) {
        return;
    }
    s->vk = r5xx_vk_create(s->vram_ptr, s->vram_size, rv530_vk_dirty_cb, s,
                           err, sizeof(err));
    if (!s->vk) {
        warn_report("rv530: Vulkan backend unavailable, using the CPU path: %s",
                    err);
        return;
    }
    r5xx_vk_set_wait_fn(s->vk, rv530_vk_wait_cb);
    s->gfx->gart_gen = 1;
    {
        /* persistent SPIR-V cache: RV530_VK_CACHE_DIR, "" disables */
        const char *env = getenv("RV530_VK_CACHE_DIR");
        g_autofree char *dir = env ? g_strdup(env)
                                   : g_build_filename(g_get_user_cache_dir(), "rv530", NULL);
        if (*dir && g_mkdir_with_parents(dir, 0700) == 0) {
            int n = r5xx_vk_set_cache_dir(s->vk, dir);
            if (n) {
                info_report("rv530: prewarming %d cached fragment programs", n);
            }
        }
    }
    if (r5xx_vk_mirrored(s->vk)) {
        /*
         * Fill the mirror now, before the guest runs.  QEMU starts with
         * every RAM page dirty, so leaving it to the first batch copied
         * all 256 MB inside a vCPU's MMIO exit (170 ms; an SMP guest panics
         * at 250 ms).  The current dirty bits are dropped: they only say
         * "new memory", which the copy just covered.
         */
        g_free(rv530_vram_snapshot_dirty(s));
        r5xx_vk_upload_all_now(s->vk);
    }
    {
        MachineState *ms = MACHINE(qdev_get_machine());
        if (getenv("RV530_VK_NO_SYSRAM")) {
            /* debug: GART textures and render targets on the CPU path */
            warn_report("rv530: RV530_VK_NO_SYSRAM: GART surfaces stay on the CPU path");
        } else if (ms->ram && memory_region_is_ram(ms->ram) &&
            r5xx_vk_set_sysram(s->vk, memory_region_get_ram_ptr(ms->ram),
                               memory_region_size(ms->ram), err, sizeof(err))) {
            s->vk_sysram = ms->ram;
        } else {
            warn_report("rv530: GART surfaces stay on the CPU path: %s", err);
        }
    }
}

void rv530_gfx_vk_fini(RV530State *s)
{
    if (s->vk) {
        r5xx_vk_destroy(s->vk);
        s->vk = NULL;
    }
}

/* ------------------------------------------------------------------ */
/* Registers                                                           */
/* ------------------------------------------------------------------ */

static bool gfx_reg_range(uint32_t off)
{
    return (off >= 0x1d98 && off <= 0x1dac) ||
           (off >= 0x2000 && off < 0x5000) ||
           (off >= US_FC_INST_0 && off < US_FC_INST_0 + 0x40) ||
           (off >= US_FC_ADDR_0 && off < US_FC_ADDR_0 + 0x40);
}

bool rv530_gfx_reg_read(RV530State *s, uint32_t off, uint32_t *val)
{
    switch (off) {
    case RB3D_DSTCACHE_CTLSTAT:
    case ZB_ZCACHE_CTLSTAT:
        *val = 0;                       /* flushes complete immediately */
        return true;
    case VAP_CNTL_STATUS:
        *val = REG(s, off) & 0xffu;     /* never busy */
        return true;
    case ZB_ZPASS_DATA:
        rv530_gfx_flush(s);
        *val = s->gfx->zpass_data +
               (uint32_t)(s->gfx->raster.zpass_count - s->gfx->zpass_base);
        return true;
    default:
        return false;
    }
}

bool rv530_gfx_reg_write(RV530State *s, uint32_t off, uint32_t val)
{
    R5xxGfx *g = s->gfx;

    if (!gfx_reg_range(off)) {
        return false;
    }
    REG(s, off) = val;

    switch (off) {
    case VAP_PVS_VECTOR_INDX:
        g->pvs_index = (val & 0x7ff) * 4;
        break;
    case VAP_PVS_VECTOR_DATA:
    case VAP_PVS_UPLOAD_DATA:
        if (g->pvs_index < PVS_MEM_VECS * 4) {
            if (g->pvs_index < PVS_CONST_START * 4) {
                g->pvs_dirty = true;     /* code changed */
            }
            g->pvs.u[g->pvs_index] = val;
        }
        g->pvs_index++;
        break;
    case VAP_PVS_CODE_CNTL_0:
    case VAP_PVS_CODE_CNTL_1:
    case VAP_PVS_CONST_CNTL:
    case VAP_PVS_FLOW_CNTL_OPC:
    case VAP_CNTL:
        g->pvs_dirty = true;
        break;
    case GA_US_VECTOR_INDEX:
        g->us_is_const = (val >> 16) & 1;
        g->us_index = (val & 0x1ff) * (g->us_is_const ? 4 : 6);
        break;
    case GA_US_VECTOR_DATA:
        if (g->us_is_const) {
            if (g->us_index < R500_US_MAX_CONST * 4) {
                g->us_const.u[g->us_index] = val;
            }
        } else if (g->us_index < R500_US_MAX_INSTR * 6) {
            g->us_code[g->us_index] = val;
            g->us_dirty = true;
        }
        g->us_index++;
        break;
    case ZB_ZPASS_DATA:
        rv530_gfx_flush(s);
        g->zpass_data = val;
        g->zpass_base = g->raster.zpass_count;
        break;
    case ZB_ZPASS_ADDR: {
        /* the query result is written to memory when the address is set */
        uint32_t cnt;
        rv530_gfx_flush(s);
        cnt = g->zpass_data +
                       (uint32_t)(g->raster.zpass_count - g->zpass_base);
        rv530_gpu_write32(s, val & ~3u, cnt);
        break;
    }
    default:
        if ((off >= VAP_PVS_FLOW_CNTL_ADDRS_0 && off < 0x22c0) ||
            (off >= VAP_PVS_FLOW_CNTL_ADDRS_LW_0 && off < 0x2580)) {
            g->pvs_dirty = true;
        }
        if ((off >= US_CONFIG && off < 0x4700) || off >= US_FC_INST_0 ||
            (off >= US_FC_INT_CONST_0 && off < US_FC_INT_CONST_0 + 0x80)) {
            g->us_dirty = true;
        }
        break;
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* Programs                                                            */
/* ------------------------------------------------------------------ */

static bool gfx_update_pvs(RV530State *s)
{
    R5xxGfx *g = s->gfx;
    R5xxPvsProgram *p = &g->pvs_prog;
    char err[256];
    int i;

    if (!g->pvs_dirty && g->pvs_dec) {
        return true;
    }
    memcpy(p->code, g->pvs.u, sizeof(p->code));
    p->code_cntl0 = REG(s, VAP_PVS_CODE_CNTL_0);
    p->code_cntl1 = REG(s, VAP_PVS_CODE_CNTL_1);
    p->const_cntl = REG(s, VAP_PVS_CONST_CNTL);
    p->vap_cntl = REG(s, VAP_CNTL);
    p->fc_opc = REG(s, VAP_PVS_FLOW_CNTL_OPC);
    for (i = 0; i < R5XX_PVS_MAX_FC; i++) {
        p->fc_addrs[i] = REG(s, VAP_PVS_FLOW_CNTL_ADDRS_0 + 4 * i);
        p->fc_loop_index[i] = REG(s, VAP_PVS_FLOW_CNTL_LOOP_INDEX_0 + 4 * i);
        p->fc_addrs_lw[i] = REG(s, VAP_PVS_FLOW_CNTL_ADDRS_LW_0 + 8 * i);
        p->fc_addrs_uw[i] = REG(s, VAP_PVS_FLOW_CNTL_ADDRS_UW_0 + 8 * i);
    }
    p->is_r500 = true;
    if (g->pvs_dec) {
        r5xx_pvs_free(g->pvs_dec);
    }
    g->pvs_dec = r5xx_pvs_decode(p, err, sizeof(err));
    if (!g->pvs_dec) {
        gfx_warn(s, "vertex program decode failed: %s", err);
        return false;
    }
    g->pvs_dirty = false;
    if (trace_event_get_state_backends(TRACE_RV530_DBG)) {
        g_autofree char *buf = g_malloc(65536);
        GString *h = g_string_new(NULL);
        r5xx_pvs_disasm(p, buf, 65536);
        g_string_printf(h, "PVS cntl0 0x%08x cntl1 0x%08x vap_cntl 0x%08x "
                        "fmt0 0x%08x fmt1 0x%08x psc",
                        p->code_cntl0, p->code_cntl1, p->vap_cntl,
                        REG(s, VAP_OUTPUT_VTX_FMT_0), REG(s, VAP_OUTPUT_VTX_FMT_1));
        for (i = 0; i < 8; i++) {
            g_string_append_printf(h, " %08x/%08x",
                                   REG(s, VAP_PROG_STREAM_CNTL_0 + 4 * i),
                                   REG(s, VAP_PROG_STREAM_CNTL_EXT_0 + 4 * i));
        }
        trace_rv530_dbg(h->str);
        trace_rv530_dbg(buf);
        g_string_free(h, true);
    }
    return true;
}

static bool gfx_update_us(RV530State *s)
{
    R5xxGfx *g = s->gfx;
    R500UsProgram *p = &g->us_prog;
    char err[256];
    int i;

    if (!g->us_dirty && g->us_dec) {
        return true;
    }
    memcpy(p->code, g->us_code, sizeof(p->code));
    p->config = REG(s, US_CONFIG);
    p->pixsize = REG(s, US_PIXSIZE);
    p->code_addr = REG(s, US_CODE_ADDR);
    p->code_range = REG(s, US_CODE_RANGE);
    p->code_offset = REG(s, US_CODE_OFFSET);
    p->fc_ctrl = REG(s, US_FC_CTRL);
    for (i = 0; i < R500_US_MAX_FC; i++) {
        p->fc_inst[i] = REG(s, US_FC_INST_0 + 4 * i);
        p->fc_addr[i] = REG(s, US_FC_ADDR_0 + 4 * i);
    }
    for (i = 0; i < 4; i++) {
        p->out_fmt[i] = REG(s, US_OUT_FMT_0 + 4 * i);
    }
    p->w_fmt = REG(s, US_W_FMT);
    for (i = 0; i < 32; i++) {
        p->fc_int_const[i] = REG(s, US_FC_INT_CONST_0 + 4 * i);
    }
    p->fc_bool_const = REG(s, US_FC_BOOL_CONST);
    if (g->us_dec) {
        r500_us_free(g->us_dec);
    }
    g->us_dec = r500_us_decode(p, err, sizeof(err));
    if (!g->us_dec) {
        gfx_warn(s, "fragment program decode failed: %s", err);
        return false;
    }
    g->us_dirty = false;
    g->us_hash = r500_us_hash(g->us_dec);
    if (trace_event_get_state_backends(TRACE_RV530_DBG)) {
        g_autofree char *buf = g_malloc(65536);
        char *line, *save = NULL;
        r500_us_disasm(p, buf, 65536);
        trace_rv530_dbg("US program:");
        {
            GString *h = g_string_new("US raw");
            uint32_t a0 = (p->code_addr & 0x1ff) + (p->code_offset & 0x1ff);
            uint32_t a1 = ((p->code_addr >> 16) & 0x1ff) + (p->code_offset & 0x1ff);
            uint32_t k;
            g_string_append_printf(h, " config 0x%x addr 0x%x range 0x%x offset 0x%x:",
                                   p->config, p->code_addr, p->code_range, p->code_offset);
            for (k = a0 * 6; k < (a1 + 1) * 6 && k < R500_US_MAX_INSTR * 6; k++) {
                g_string_append_printf(h, " %08x", p->code[k]);
            }
            trace_rv530_dbg(h->str);
            g_string_free(h, true);
        }
        for (line = strtok_r(buf, "\n", &save); line;
             line = strtok_r(NULL, "\n", &save)) {
            trace_rv530_dbg(line);
        }
    }
    return true;
}

static void gfx_tex_cb(void *opaque, const R5xxTexRequest *req,
                       r5xx_vec4 out[4])
{
    RV530State *s = opaque;
    R5xxGfx *g = s->gfx;

    if (req->unit < 0 || req->unit >= R5XX_NUM_TEX_UNITS ||
        !(REG(s, TX_ENABLE) & (1u << req->unit))) {
        int i;
        for (i = 0; i < 4; i++) {
            out[i] = r5xx_vec4_make(0, 0, 0, 1);
        }
        return;
    }
    r5xx_tex_sample_quad(&g->samplers[req->unit], &s->mem, req, out);
    if (g->dbg_tex < 16 && trace_event_get_state_backends(TRACE_RV530_DBG)) {
        g_autofree char *m = g_strdup_printf(
            "TEX unit %d op %d unscaled %d c0 [%g %g %g %g] c3 [%g %g %g %g] "
            "-> [%g %g %g %g]", req->unit, req->op, req->unscaled,
            req->coord[0].f[0], req->coord[0].f[1], req->coord[0].f[2],
            req->coord[0].f[3], req->coord[3].f[0], req->coord[3].f[1],
            req->coord[3].f[2], req->coord[3].f[3],
            out[0].f[0], out[0].f[1], out[0].f[2], out[0].f[3]);
        g->dbg_tex++;
        trace_rv530_dbg(m);
    }
}

static void gfx_update_textures(RV530State *s)
{
    R5xxGfx *g = s->gfx;
    uint32_t used = g->us_dec ? r500_us_tex_units_used(g->us_dec) : 0;
    int n;

    used &= REG(s, TX_ENABLE);
    for (n = 0; n < R5XX_NUM_TEX_UNITS; n++) {
        R5xxTexRegs tr;
        char err[128];

        if (!(used & (1u << n))) {
            continue;
        }
        tr.filter0 = REG(s, TX_FILTER0_0 + 4 * n);
        tr.filter1 = REG(s, TX_FILTER1_0 + 4 * n);
        tr.format0 = REG(s, TX_FORMAT0_0 + 4 * n);
        tr.format1 = REG(s, TX_FORMAT1_0 + 4 * n);
        tr.format2 = REG(s, TX_FORMAT2_0 + 4 * n);
        tr.offset = REG(s, TX_OFFSET_0 + 4 * n);
        tr.chroma_key = REG(s, TX_CHROMA_KEY_0 + 4 * n);
        tr.border_color = REG(s, TX_BORDER_COLOR_0 + 4 * n);
        if (!r5xx_tex_decode(&tr, true, &g->samplers[n], err, sizeof(err))) {
            gfx_warn(s, "texture unit %d: %s", n, err);
        }
        /*
         * TXO_ENDIAN swaps only texels that come from system memory (GART):
         * client-storage textures in big-endian packed types.  In VRAM the
         * data is always native.  The Mac OS X 10.5 driver samples FBO
         * textures in VRAM with ENDIAN 8IN32 for glReadPixels, and draws
         * the same textures with no swap.
         */
        if (g->samplers[n].layout.gpu_addr >= s->fb_start &&
            g->samplers[n].layout.gpu_addr <= s->fb_end) {
            g->samplers[n].endian = 0;
        }
        if (trace_event_get_state_backends(TRACE_RV530_DBG)) {
            const R5xxTexSampler *t = &g->samplers[n];
            g_autofree char *m = g_strdup_printf(
                "tex%d off 0x%08x f0 0x%08x f1 0x%08x f2 0x%08x filt0 0x%08x "
                "filt1 0x%08x -> %ux%ux%u bpp %u hwfmt 0x%x float %d valid %d "
                "wrap %d/%d filt %d/%d swz %d%d%d%d", n, tr.offset, tr.format0,
                tr.format1, tr.format2, tr.filter0, tr.filter1, t->width,
                t->height, t->depth, t->bpp, t->hw_format, t->is_float,
                t->valid, t->wrap_s, t->wrap_t, t->min_filter, t->mag_filter,
                t->swizzle[0], t->swizzle[1], t->swizzle[2], t->swizzle[3]);
            trace_rv530_dbg(m);
        }
    }
}

static void gfx_raster_regs(RV530State *s, R5xxRasterRegs *r)
{
    int i;

    memset(r, 0, sizeof(*r));
    r->rb3d_cctl = REG(s, RB3D_CCTL);
    r->rb3d_blendcntl = REG(s, RB3D_BLENDCNTL);
    r->rb3d_ablendcntl = REG(s, RB3D_ABLENDCNTL);
    r->rb3d_color_channel_mask = REG(s, RB3D_COLOR_CHANNEL_MASK);
    r->rb3d_blend_color = REG(s, RB3D_BLEND_COLOR);
    r->rb3d_ropcntl = REG(s, RB3D_ROPCNTL);
    r->rb3d_clrcmp_flipe = REG(s, RB3D_CLRCMP_FLIPE);
    r->rb3d_clrcmp_clr = REG(s, RB3D_CLRCMP_CLR);
    r->rb3d_clrcmp_msk = REG(s, RB3D_CLRCMP_MSK);
    for (i = 0; i < 4; i++) {
        r->rb3d_coloroffset[i] = REG(s, RB3D_COLOROFFSET0 + 4 * i);
        r->rb3d_colorpitch[i] = REG(s, RB3D_COLORPITCH0 + 4 * i);
        r->us_out_fmt[i] = REG(s, US_OUT_FMT_0 + 4 * i);
        r->sc_clip_a[i] = REG(s, SC_CLIP_0_A + 8 * i);
        r->sc_clip_b[i] = REG(s, SC_CLIP_0_B + 8 * i);
    }
    r->rb3d_dither_ctl = REG(s, RB3D_DITHER_CTL);
    r->rb3d_constant_color_ar = REG(s, RB3D_CONSTANT_COLOR_AR);
    r->rb3d_constant_color_gb = REG(s, RB3D_CONSTANT_COLOR_GB);
    r->zb_cntl = REG(s, ZB_CNTL);
    r->zb_zstencilcntl = REG(s, ZB_ZSTENCILCNTL);
    r->zb_stencilrefmask = REG(s, ZB_STENCILREFMASK);
    r->zb_format = REG(s, ZB_FORMAT);
    r->zb_ztop = REG(s, ZB_ZTOP);
    r->zb_depthoffset = REG(s, ZB_DEPTHOFFSET);
    r->zb_depthpitch = REG(s, ZB_DEPTHPITCH);
    r->zb_depthclearvalue = REG(s, ZB_DEPTHCLEARVALUE);
    r->zb_stencilrefmask_bf = REG(s, ZB_STENCILREFMASK_BF);
    r->fg_fog_blend = REG(s, FG_FOG_BLEND);
    r->fg_fog_factor = REG(s, FG_FOG_FACTOR);
    r->fg_fog_color_r = REG(s, FG_FOG_COLOR_R);
    r->fg_fog_color_g = REG(s, FG_FOG_COLOR_G);
    r->fg_fog_color_b = REG(s, FG_FOG_COLOR_B);
    r->fg_alpha_func = REG(s, FG_ALPHA_FUNC);
    r->fg_depth_src = REG(s, FG_DEPTH_SRC);
    r->fg_alpha_value = REG(s, FG_ALPHA_VALUE);
    r->sc_scissors_tl = REG(s, SC_SCISSORS_TL);
    r->sc_scissors_br = REG(s, SC_SCISSORS_BR);
    r->sc_clip_rule = REG(s, SC_CLIP_RULE);
    r->sc_edgerule = REG(s, SC_EDGERULE);
    r->su_cull_mode = REG(s, SU_CULL_MODE);
    r->su_poly_offset_front_scale = REG(s, SU_POLY_OFFSET_FRONT_SCALE);
    r->su_poly_offset_front_offset = REG(s, SU_POLY_OFFSET_FRONT_OFFSET);
    r->su_poly_offset_back_scale = REG(s, SU_POLY_OFFSET_BACK_SCALE);
    r->su_poly_offset_back_offset = REG(s, SU_POLY_OFFSET_BACK_OFFSET);
    r->su_poly_offset_enable = REG(s, SU_POLY_OFFSET_ENABLE);
    r->su_depth_scale = REG(s, SU_DEPTH_SCALE);
    r->su_depth_offset = REG(s, SU_DEPTH_OFFSET);
    r->ga_poly_mode = REG(s, GA_POLY_MODE);
    r->ga_color_control = REG(s, GA_COLOR_CONTROL);
    r->ga_point_size = REG(s, GA_POINT_SIZE);
    r->ga_point_minmax = REG(s, GA_POINT_MINMAX);
    r->ga_line_cntl = REG(s, GA_LINE_CNTL);
    r->ga_round_mode = REG(s, GA_ROUND_MODE);
    r->ga_offset = REG(s, GA_OFFSET);
    r->ga_fog_scale = REG(s, GA_FOG_SCALE);
    r->ga_fog_offset = REG(s, GA_FOG_OFFSET);
    r->gb_tile_config = REG(s, GB_TILE_CONFIG);
    r->gb_aa_config = REG(s, GB_AA_CONFIG);
    r->rb3d_aaresolve_offset = REG(s, 0x4e80);
    r->rb3d_aaresolve_pitch = REG(s, 0x4e84);
    r->rb3d_aaresolve_ctl = REG(s, 0x4e88);
    r->gb_select = REG(s, GB_SELECT);
    r->is_r500 = true;
}

/* ------------------------------------------------------------------ */
/* Vertex fetch                                                        */
/* ------------------------------------------------------------------ */

/* dwords occupied by one element of a PSC data type */
static uint32_t psc_type_dwords(uint32_t type)
{
    switch (type) {
    case 0: return 1;           /* FLOAT_1 */
    case 1: return 2;           /* FLOAT_2 */
    case 2: return 3;           /* FLOAT_3 */
    case 3: return 4;           /* FLOAT_4 */
    case 4: return 1;           /* BYTE (x4) */
    case 5: return 1;           /* D3DCOLOR */
    case 6: return 1;           /* SHORT_2 */
    case 7: return 2;           /* SHORT_4 */
    case 8: case 9: return 1;   /* VECTOR_3_TTT / EET (packed 10:11:11) */
    case 10: return 8;          /* FLOAT_8 */
    case 11: return 1;          /* FLT16_2 */
    case 12: return 2;          /* FLT16_4 */
    default: return 1;
    }
}

/* Decode one element: d points at its dwords; result before swizzle. */
static void psc_decode(uint32_t type, bool sign, bool norm, const uint32_t *d,
                       float out[4])
{
    union { uint32_t u; float f; } c;
    int i;

    out[0] = out[1] = out[2] = 0.0f;
    out[3] = 1.0f;
    switch (type) {
    case 0: case 1: case 2: case 3:
        for (i = 0; i <= (int)type; i++) {
            c.u = le32_to_cpu(d[i]);
            out[i] = c.f;
        }
        break;
    case 10:
        for (i = 0; i < 4; i++) {
            c.u = le32_to_cpu(d[i]);
            out[i] = c.f;
        }
        break;
    case 4: case 5: {
        uint32_t v = le32_to_cpu(d[0]);
        for (i = 0; i < 4; i++) {
            uint32_t b = (v >> (8 * i)) & 0xff;
            float f = sign ? (float)(int8_t)b : (float)b;
            if (norm || type == 5) {
                f = sign ? MAX(f / 127.0f, -1.0f) : f / 255.0f;
            }
            out[i] = f;
        }
        if (type == 5) {        /* D3DCOLOR: bytes are B, G, R, A */
            float t = out[0];
            out[0] = out[2];
            out[2] = t;
        }
        break;
    }
    case 6: case 7: {
        int n = type == 6 ? 2 : 4;
        for (i = 0; i < n; i++) {
            uint32_t w = (le32_to_cpu(d[i / 2]) >> (16 * (i & 1))) & 0xffff;
            float f = sign ? (float)(int16_t)w : (float)w;
            if (norm) {
                f = sign ? MAX(f / 32767.0f, -1.0f) : f / 65535.0f;
            }
            out[i] = f;
        }
        break;
    }
    case 11: case 12: {
        int n = type == 11 ? 2 : 4;
        for (i = 0; i < n; i++) {
            uint32_t w = (le32_to_cpu(d[i / 2]) >> (16 * (i & 1))) & 0xffff;
            out[i] = r5xx_half_to_float(w);
        }
        break;
    }
    default: {
        /* 10:10:10 style packed vectors: decode as unsigned 11/11/10 */
        uint32_t v = le32_to_cpu(d[0]);
        out[0] = (float)(v & 0x7ff);
        out[1] = (float)((v >> 11) & 0x7ff);
        out[2] = (float)((v >> 22) & 0x3ff);
        break;
    }
    }
}

typedef struct VtxSource {
    const uint32_t *immd;      /* immediate vertex data or NULL */
    uint32_t immd_dwords;
    uint32_t vtx_size;         /* dwords per immediate vertex */
} VtxSource;

/*
 * Vertex arrays: VAP_VTX_NUM_ARRAYS (0x20C0) followed by, per pair of
 * arrays, VAP_VTX_AOS_ATTRnm (count/stride of both, in dwords) and the two
 * VAP_VTX_AOS_ADDRn.  Drivers either write these registers directly
 * (Apple) or through the 3D_LOAD_VBPNTR packet (Mesa); same layout.
 */
static void gfx_load_aos(RV530State *s)
{
    R5xxGfx *g = s->gfx;
    uint32_t n = REG(s, VAP_VTX_NUM_ARRAYS) & 0x1f;
    uint32_t i;

    g->num_vbuf = MIN(n, MAX_VBUF);
    for (i = 0; i < (uint32_t)g->num_vbuf; i++) {
        uint32_t pair = VAP_VTX_NUM_ARRAYS + 4 + 12 * (i / 2);
        uint32_t attr = REG(s, pair) >> (16 * (i & 1));

        g->vbuf[i].size = (attr & 0x7f) * 4;
        g->vbuf[i].stride = ((attr >> 8) & 0xff) * 4;
        g->vbuf[i].addr = REG(s, pair + 4 + 4 * (i & 1));
    }
}

/* Fill the PVS input registers for vertex 'index'. */
static void gfx_fetch(RV530State *s, const VtxSource *src, uint32_t index,
                      r5xx_vec4 in[R5XX_PVS_MAX_INPUTS])
{
    R5xxGfx *g = s->gfx;
    uint32_t off = 0;           /* dword offset inside an immediate vertex */
    int e;

    for (e = 0; e < 16; e++) {
        uint32_t psc = (REG(s, VAP_PROG_STREAM_CNTL_0 + 4 * (e / 2)) >>
                        (16 * (e & 1))) & 0xffff;
        uint32_t ext = (REG(s, VAP_PROG_STREAM_CNTL_EXT_0 + 4 * (e / 2)) >>
                        (16 * (e & 1))) & 0xffff;
        uint32_t type = psc & 0xf, skip = (psc >> 4) & 0xf;
        uint32_t dst = (psc >> 8) & 0x1f;
        bool last = psc & (1u << 13);
        bool sign = psc & (1u << 14), norm = psc & (1u << 15);
        uint32_t nd = psc_type_dwords(type);
        uint32_t raw[8] = { 0 };
        float v[4], sel[6];
        int c;

        if (src->immd) {
            uint32_t base = index * src->vtx_size + off + skip;
            if (base + nd <= src->immd_dwords) {
                memcpy(raw, src->immd + base, nd * 4);
            }
            off += skip + nd;
        } else {
            int a = e < g->num_vbuf ? e : g->num_vbuf - 1;
            if (a >= 0) {
                uint32_t addr = g->vbuf[a].addr + index * g->vbuf[a].stride +
                                skip * 4;
                rv530_gpu_read(s, addr, raw, nd * 4);
            }
        }
        psc_decode(type, sign, norm, raw, v);
        sel[0] = v[0]; sel[1] = v[1]; sel[2] = v[2]; sel[3] = v[3];
        sel[4] = 0.0f; sel[5] = 1.0f;
        if (dst < R5XX_PVS_MAX_INPUTS) {
            for (c = 0; c < 4; c++) {
                uint32_t sw = (ext >> (3 * c)) & 7;
                if ((ext >> (12 + c)) & 1) {
                    in[dst].f[c] = sel[sw < 6 ? sw : 4];
                }
            }
        }
        if (last) {
            break;
        }
    }
}

/* Run the vertex stage for one vertex: fill the 14 output slots. */
static void gfx_vertex(RV530State *s, const VtxSource *src, uint32_t index,
                       GfxVert *out)
{
    R5xxGfx *g = s->gfx;
    r5xx_vec4 in[R5XX_PVS_MAX_INPUTS];
    r5xx_vec4 res[R5XX_PVS_MAX_OUTPUTS];
    uint32_t fmt0 = REG(s, VAP_OUTPUT_VTX_FMT_0);
    uint32_t fmt1 = REG(s, VAP_OUTPUT_VTX_FMT_1);
    bool bypass = REG(s, VAP_CNTL_STATUS) & VAP_TCL_BYPASS;
    int i, k = 0;

    for (i = 0; i < R5XX_PVS_MAX_INPUTS; i++) {
        in[i] = r5xx_vec4_make(0, 0, 0, 1);
    }
    gfx_fetch(s, src, index, in);
    for (i = 0; i < NUM_SLOTS; i++) {
        out->slot[i] = r5xx_vec4_make(0, 0, 0, 1);
    }
    if (bypass) {
        /* no TCL: input vector n feeds output slot n (stream_loc_notcl) */
        for (i = 0; i < NUM_SLOTS && i < R5XX_PVS_MAX_INPUTS; i++) {
            out->slot[i] = in[i];
        }
        return;
    }
    for (i = 0; i < R5XX_PVS_MAX_OUTPUTS; i++) {
        res[i] = r5xx_vec4_make(0, 0, 0, 1);
    }
    r5xx_pvs_run(g->pvs_dec, (const r5xx_vec4 *)&g->pvs.f[PVS_CONST_START * 4],
                 in, res);
    if (trace_event_get_state_backends(TRACE_RV530_DBG) &&
        (index == 0 || (index < 256 && getenv("RV530_DBG_ALLV")))) {
        GString *h = g_string_new(NULL);
        g_string_printf(h, "VS v%u in:", index);
        for (i = 0; i < 4; i++) {
            g_string_append_printf(h, " [%g %g %g %g]", in[i].f[0], in[i].f[1],
                                   in[i].f[2], in[i].f[3]);
        }
        g_string_append(h, " out:");
        for (i = 0; i < 4; i++) {
            g_string_append_printf(h, " [%g %g %g %g]", res[i].f[0], res[i].f[1],
                                   res[i].f[2], res[i].f[3]);
        }
        trace_rv530_dbg(h->str);
        g_string_free(h, true);
    }
    /* PVS results are packed in VAP_OUTPUT_VTX_FMT slot order (r300_vs.c
     * set_vertex_inputs_outputs) */
    out->slot[SLOT_POS] = res[k++];
    if (fmt0 & (1u << 16)) {
        out->slot[SLOT_PSIZE] = res[k++];
    }
    for (i = 0; i < 4; i++) {
        if (fmt0 & (2u << i)) {
            out->slot[SLOT_COL0 + i] = res[k++];
        }
    }
    for (i = 0; i < 8 && k < R5XX_PVS_MAX_OUTPUTS; i++) {
        if ((fmt1 >> (3 * i)) & 7) {
            out->slot[SLOT_TEX0 + i] = res[k++];
        }
    }
}

/* ------------------------------------------------------------------ */
/* Clipping, viewport, RS routing                                      */
/* ------------------------------------------------------------------ */

typedef struct ClipCtx {
    int nplanes;
    float plane[8][4];
} ClipCtx;

static void gfx_clip_setup(RV530State *s, ClipCtx *cc)
{
    R5xxGfx *g = s->gfx;
    uint32_t cntl = REG(s, VAP_CLIP_CNTL);
    bool bypass = REG(s, VAP_CNTL_STATUS) & VAP_TCL_BYPASS;
    int i;

    cc->nplanes = 0;
    if (bypass || (REG(s, VAP_VTE_CNTL) & VTE_XY_FMT)) {
        return;                 /* window coordinates: nothing to clip */
    }
    /* w > 0 is always required for the divide */
    cc->plane[cc->nplanes][0] = 0;
    cc->plane[cc->nplanes][1] = 0;
    cc->plane[cc->nplanes][2] = 0;
    cc->plane[cc->nplanes][3] = 1;
    cc->nplanes++;
    if (!(cntl & CLIP_DISABLE)) {
        /* near plane: z >= -w (GL) or z >= 0 (D3D) */
        bool dx = REG(s, VAP_CNTL) & VAP_DX_CLIP_SPACE;
        cc->plane[cc->nplanes][0] = 0;
        cc->plane[cc->nplanes][1] = 0;
        cc->plane[cc->nplanes][2] = 1;
        cc->plane[cc->nplanes][3] = dx ? 0 : 1;
        cc->nplanes++;
        for (i = 0; i < 6; i++) {
            if (cntl & (1u << i)) {
                memcpy(cc->plane[cc->nplanes],
                       &g->pvs.f[(PVS_UCP_START + i) * 4], 4 * sizeof(float));
                cc->nplanes++;
            }
        }
    }
}

static inline float plane_dist(const float p[4], const r5xx_vec4 *v)
{
    return p[0] * v->f[0] + p[1] * v->f[1] + p[2] * v->f[2] + p[3] * v->f[3];
}

static void vert_lerp(GfxVert *o, const GfxVert *a, const GfxVert *b, float t)
{
    int i, c;
    for (i = 0; i < NUM_SLOTS; i++) {
        for (c = 0; c < 4; c++) {
            o->slot[i].f[c] = a->slot[i].f[c] +
                              (b->slot[i].f[c] - a->slot[i].f[c]) * t;
        }
    }
}

/* Sutherland-Hodgman against all planes; returns the vertex count. */
static int gfx_clip_poly(const ClipCtx *cc, GfxVert *in, int n, GfxVert *tmp)
{
    int p;
    GfxVert *a = in, *b = tmp;

    for (p = 0; p < cc->nplanes && n > 0; p++) {
        const float *pl = cc->plane[p];
        float eps = (p == 0) ? 1e-6f : 0.0f;
        int i, m = 0;
        bool all_in = true;

        for (i = 0; i < n; i++) {
            if (plane_dist(pl, &a[i].slot[SLOT_POS]) < eps) {
                all_in = false;
                break;
            }
        }
        if (all_in) {
            continue;
        }
        for (i = 0; i < n; i++) {
            const GfxVert *cur = &a[i], *nxt = &a[(i + 1) % n];
            float dc = plane_dist(pl, &cur->slot[SLOT_POS]) - eps;
            float dn = plane_dist(pl, &nxt->slot[SLOT_POS]) - eps;
            if (dc >= 0 && m < MAX_CLIP_VERTS) {
                b[m++] = *cur;
            }
            if ((dc >= 0) != (dn >= 0) && m < MAX_CLIP_VERTS) {
                vert_lerp(&b[m++], cur, nxt, dc / (dc - dn));
            }
        }
        n = m;
        { GfxVert *t = a; a = b; b = t; }
    }
    if (a != in) {
        memcpy(in, a, n * sizeof(GfxVert));
    }
    return n;
}

typedef struct RsCtx {
    uint32_t ip[16], inst[16];
    int ninst;
    int tex_comp_base[8];       /* first component of TEXn in the stream */
    int tex_comp_n[8];          /* its component count */
    int ntexcomp;
    bool face_used;
    int face_addr;
} RsCtx;

/*
 * GB_ENABLE texture stuffing: for the primitive classes enabled in bits
 * 0-2 (points, lines, triangles), texture coordinate set i whose
 * TEXi_SOURCE (bits 16+2i) is ST (1) or STR (2) is generated by the GA
 * instead of coming from the VAP; it then occupies 2 or 3 components of
 * the rasterizer's texture stream.  Returns the mask of stuffed sets.
 */
static uint32_t gfx_stuffed_sets(RV530State *s, uint32_t prim)
{
    uint32_t gbe = REG(s, GB_ENABLE);
    uint32_t cls = (prim == 1) ? 1 : (prim == 2 || prim == 3 || prim == 12) ? 2 : 4;
    uint32_t mask = 0;
    int i;

    if (!(gbe & cls)) {
        return 0;
    }
    for (i = 0; i < 8; i++) {
        uint32_t src = (gbe >> (16 + 2 * i)) & 3;
        if (src == 1 || src == 2) {
            mask |= 1u << i;
        }
    }
    return mask;
}

static void gfx_rs_setup(RV530State *s, RsCtx *rs, R5xxRaster *r,
                         uint32_t prim)
{
    uint32_t fmt1 = REG(s, VAP_OUTPUT_VTX_FMT_1);
    uint32_t cc = REG(s, GA_COLOR_CONTROL);
    uint32_t ps3 = REG(s, GA_COLOR_CONTROL_PS3);
    uint32_t gbe = REG(s, GB_ENABLE);
    uint32_t stuffed = gfx_stuffed_sets(s, prim);
    int i, comp = 0, max_in = 0;

    memset(rs, 0, sizeof(*rs));
    for (i = 0; i < 8; i++) {
        int n = (fmt1 >> (3 * i)) & 7;
        if (stuffed & (1u << i)) {
            n = ((gbe >> (16 + 2 * i)) & 3) == 2 ? 3 : 2;
        }
        n = n > 4 ? 4 : n;
        rs->tex_comp_base[i] = comp;
        rs->tex_comp_n[i] = n;
        comp += n;
    }
    rs->ntexcomp = comp;
    rs->ninst = (REG(s, RS_INST_COUNT) & 0xf) + 1;
    r->flat_inputs = 0;
    for (i = 0; i < 16; i++) {
        rs->ip[i] = REG(s, R500_RS_IP_0 + 4 * i);
        rs->inst[i] = REG(s, R500_RS_INST_0 + 4 * i);
    }
    for (i = 0; i < rs->ninst; i++) {
        uint32_t inst = rs->inst[i];
        if (inst & (1u << 4)) {
            int addr = (inst >> 5) & 0x7f;
            int id = inst & 0xf;
            if (addr < R5XX_MAX_FS_INPUTS) {
                max_in = MAX(max_in, addr + 1);
                if (((ps3 >> (2 * id)) & 3) == 1) {
                    r->flat_inputs |= 1u << addr;
                }
            }
        }
        if ((inst >> 16) & 3) {
            int addr = (inst >> 18) & 0x7f;
            if (addr < R5XX_MAX_FS_INPUTS) {
                max_in = MAX(max_in, addr + 1);
                if (((inst >> 16) & 3) == 3) {
                    rs->face_used = true;
                    rs->face_addr = addr;
                    r->flat_inputs |= 1u << addr;
                } else if ((cc & 3) == 1) {
                    r->flat_inputs |= 1u << addr;
                }
            }
        }
    }
    r->num_fs_inputs = max_in;
}

/* tex component stream pointer -> value */
static float rs_tex_comp(const RsCtx *rs, const GfxVert *v, int ptr)
{
    int t;

    if (ptr == 62) {
        return 0.0f;
    }
    if (ptr == 63) {
        return 1.0f;
    }
    for (t = 0; t < 8; t++) {
        int c = ptr - rs->tex_comp_base[t];
        if (c >= 0 && c < rs->tex_comp_n[t]) {
            return v->slot[SLOT_TEX0 + t].f[c];
        }
    }
    return 0.0f;
}

static r5xx_vec4 rs_color(const GfxVert *v, uint32_t ip)
{
    int ptr = (ip >> 24) & 7;
    int fmt = (ip >> 27) & 0xf;
    r5xx_vec4 c = ptr < 4 ? v->slot[SLOT_COL0 + ptr] : r5xx_vec4_make(0, 0, 0, 1);

    switch (fmt) {
    case 0: break;                                      /* RGBA */
    case 1: c.f[3] = 0; break;                          /* RGB0 */
    case 2: c.f[3] = 1; break;                          /* RGB1 */
    case 4: c.f[0] = c.f[1] = c.f[2] = 0; break;        /* 000A */
    case 5: c = r5xx_vec4_make(0, 0, 0, 0); break;      /* 0000 */
    case 6: c = r5xx_vec4_make(0, 0, 0, 1); break;      /* 0001 */
    case 8: c.f[0] = c.f[1] = c.f[2] = 1; break;        /* 111A */
    case 9: c = r5xx_vec4_make(1, 1, 1, 0); break;      /* 1110 */
    case 10: c = r5xx_vec4_make(1, 1, 1, 1); break;     /* 1111 */
    default: break;
    }
    return c;
}

static inline float regf(RV530State *s, uint32_t off)
{
    union { uint32_t u; float f; } c = { .u = REG(s, off) };
    return c.f;
}

/* viewport scale/offset register, or the identity value when disabled */
static inline float vport(RV530State *s, uint32_t vte, uint32_t off,
                          uint32_t ena, float def)
{
    return (vte & ena) ? regf(s, off) : def;
}

/* Clip-space vertex -> window vertex with routed FS inputs. */
static void gfx_to_window(RV530State *s, const RsCtx *rs, const GfxVert *v,
                          R5xxVertex *o)
{
    uint32_t vte = REG(s, VAP_VTE_CNTL);
    float x = v->slot[SLOT_POS].f[0], y = v->slot[SLOT_POS].f[1];
    float z = v->slot[SLOT_POS].f[2], w = v->slot[SLOT_POS].f[3];
    float iw;
    int i;

    if (vte & VTE_XY_FMT) {
        /* already in window space; w0 may carry 1/w */
        iw = 1.0f;
        o->w = (vte & VTE_W0_FMT) ? (w != 0.0f ? 1.0f / w : 1.0f) : 1.0f;
    } else {
        iw = (w != 0.0f) ? 1.0f / w : 0.0f;
        o->w = w;
        x *= iw;
        y *= iw;
    }
    if (!(vte & VTE_Z_FMT)) {
        z *= iw;
    }
    x = x * vport(s, vte, SE_VPORT_XSCALE, VTE_X_SCALE_ENA, 1.0f) +
        vport(s, vte, SE_VPORT_XOFFSET, VTE_X_OFFSET_ENA, 0.0f);
    y = y * vport(s, vte, SE_VPORT_YSCALE, VTE_Y_SCALE_ENA, 1.0f) +
        vport(s, vte, SE_VPORT_YOFFSET, VTE_Y_OFFSET_ENA, 0.0f);
    z = z * vport(s, vte, SE_VPORT_ZSCALE, VTE_Z_SCALE_ENA, 1.0f) +
        vport(s, vte, SE_VPORT_ZOFFSET, VTE_Z_OFFSET_ENA, 0.0f);
    o->x = x;
    o->y = y;
    o->z = z;

    for (i = 0; i < R5XX_MAX_FS_INPUTS; i++) {
        o->attr[i] = r5xx_vec4_make(0, 0, 0, 1);
    }
    for (i = 0; i < rs->ninst; i++) {
        uint32_t inst = rs->inst[i];
        if (inst & (1u << 4)) {
            uint32_t ip = rs->ip[inst & 0xf];
            int addr = (inst >> 5) & 0x7f;
            if (addr < R5XX_MAX_FS_INPUTS) {
                o->attr[addr] = r5xx_vec4_make(
                    rs_tex_comp(rs, v, ip & 0x3f),
                    rs_tex_comp(rs, v, (ip >> 6) & 0x3f),
                    rs_tex_comp(rs, v, (ip >> 12) & 0x3f),
                    rs_tex_comp(rs, v, (ip >> 18) & 0x3f));
            }
        }
        if (((inst >> 16) & 3) == 1 || ((inst >> 16) & 3) == 2) {
            uint32_t ip = rs->ip[(inst >> 12) & 0xf];
            int addr = (inst >> 18) & 0x7f;
            if (addr < R5XX_MAX_FS_INPUTS) {
                o->attr[addr] = rs_color(v, ip);
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* Primitive assembly                                                  */
/* ------------------------------------------------------------------ */

typedef struct DrawCtx {
    RV530State *s;
    ClipCtx clip;
    RsCtx rs;
    bool psize;
} DrawCtx;

/* ---- primitive sinks: CPU rasterizer or Vulkan collection ---- */

static RvkVertex *gfx_vk_reserve(R5xxGfx *g, uint32_t n)
{
    if (g->vk_nverts + n > g->vk_cap) {
        g->vk_cap = MAX(g->vk_cap * 2, g->vk_nverts + n + 1024);
        g->vk_verts = g_renew(RvkVertex, g->vk_verts, g->vk_cap);
    }
    return g->vk_verts + g->vk_nverts;
}

static void gfx_vk_bbox(R5xxGfx *g, const R5xxVertex *v, float hh)
{
    g->vk_y0 = MIN(g->vk_y0, v->y - hh);
    g->vk_y1 = MAX(g->vk_y1, v->y + hh);
}

static void gfx_raster_tri(R5xxGfx *g, const R5xxVertex *a, const R5xxVertex *b,
                           const R5xxVertex *c)
{
    int k;

    if (cpool.collect) {
        CpuPrim *p = cpu_add(&g->raster, CPU_TRI);
        p->v[0] = *a;
        p->v[1] = *b;
        p->v[2] = *c;
        return;
    }
    if (!g->vk_collect) {
        r5xx_raster_triangle(&g->raster, a, b, c);
        return;
    }
    k = rvk_triangle(gfx_vk_reserve(g, 3), &g->raster, a, b, c, false);
    if (k < 0) {
        g->vk_unsupported = true;
        return;
    }
    if (k) {
        g->vk_nverts += k;
        gfx_vk_bbox(g, a, 0);
        gfx_vk_bbox(g, b, 0);
        gfx_vk_bbox(g, c, 0);
    }
}

static void gfx_raster_point(R5xxGfx *g, const R5xxVertex *v, bool sized,
                             float size)
{
    float hw, hh;

    if (cpool.collect) {
        CpuPrim *p = cpu_add(&g->raster, sized ? CPU_POINT_SIZED : CPU_POINT);
        p->v[0] = *v;
        p->size = size;
        return;
    }
    if (!g->vk_collect) {
        if (sized) {
            r5xx_raster_point_sized(&g->raster, v, size);
        } else {
            r5xx_raster_point(&g->raster, v);
        }
        return;
    }
    if (sized) {
        if (g->raster.regs.ga_point_minmax != 0) {
            size = r5xx_clampf(size, g->raster.d.point_min, g->raster.d.point_max);
        }
        hw = hh = (float)llrintf(size * 0.5f * g->raster.d.subpix) /
                  g->raster.d.subpix;
    } else {
        hw = g->raster.d.point_half_w;
        hh = g->raster.d.point_half_h;
    }
    g->vk_nverts += rvk_point(gfx_vk_reserve(g, 6), &g->raster, v, hw, hh);
    gfx_vk_bbox(g, v, MAX(hh, 0.5f));
}

static void gfx_raster_line(R5xxGfx *g, const R5xxVertex *a, const R5xxVertex *b)
{
    if (g->vk_collect) {
        g->vk_unsupported = true;       /* lines: CPU path */
        return;
    }
    if (cpool.collect) {
        CpuPrim *p = cpu_add(&g->raster, CPU_LINE);
        p->v[0] = *a;
        p->v[1] = *b;
        return;
    }
    r5xx_raster_line(&g->raster, a, b);
}

static void gfx_emit_tri(DrawCtx *dc, const GfxVert *a, const GfxVert *b,
                         const GfxVert *c)
{
    R5xxGfx *g = dc->s->gfx;
    GfxVert poly[MAX_CLIP_VERTS], tmp[MAX_CLIP_VERTS];
    R5xxVertex w[MAX_CLIP_VERTS];
    int n, i;

    poly[0] = *a;
    poly[1] = *b;
    poly[2] = *c;
    n = gfx_clip_poly(&dc->clip, poly, 3, tmp);
    if (n < 3) {
        return;
    }
    for (i = 0; i < n; i++) {
        gfx_to_window(dc->s, &dc->rs, &poly[i], &w[i]);
    }
    if (dc->rs.face_used) {
        float area = (w[1].x - w[0].x) * (w[2].y - w[0].y) -
                     (w[2].x - w[0].x) * (w[1].y - w[0].y);
        bool ccw = area < 0;            /* y points down */
        float f = (ccw == g->raster.d.front_ccw) ? 1.0f : 0.0f;
        for (i = 0; i < n; i++) {
            w[i].attr[dc->rs.face_addr] = r5xx_vec4_make(f, f, f, f);
        }
    }
    for (i = 1; i + 1 < n; i++) {
        gfx_raster_tri(g, &w[0], &w[i], &w[i + 1]);
        g->prims++;
    }
}

static void gfx_emit_line(DrawCtx *dc, const GfxVert *a, const GfxVert *b)
{
    R5xxGfx *g = dc->s->gfx;
    GfxVert v[2] = { *a, *b };
    R5xxVertex w[2];
    int p;

    for (p = 0; p < dc->clip.nplanes; p++) {
        float da = plane_dist(dc->clip.plane[p], &v[0].slot[SLOT_POS]);
        float db = plane_dist(dc->clip.plane[p], &v[1].slot[SLOT_POS]);
        if (da < 0 && db < 0) {
            return;
        }
        if (da < 0) {
            vert_lerp(&v[0], &v[0], &v[1], da / (da - db));
        } else if (db < 0) {
            vert_lerp(&v[1], &v[0], &v[1], da / (da - db));
        }
    }
    gfx_to_window(dc->s, &dc->rs, &v[0], &w[0]);
    gfx_to_window(dc->s, &dc->rs, &v[1], &w[1]);
    gfx_raster_line(g, &w[0], &w[1]);
    g->prims++;
}

/*
 * Point "texture stuffing": with GB_ENABLE.POINT_STUFF_ENABLE, texture
 * coordinate set i whose GB_ENABLE.TEXi_SOURCE is ST/STR is replaced by
 * coordinates interpolated across the point: S from GA_POINT_S0 (left) to
 * GA_POINT_S1 (right), T from GA_POINT_T0 (bottom) to GA_POINT_T1 (top).  Apple's
 * WindowServer blits with such sprites.  Rendered as a two-triangle quad.
 */
static bool gfx_emit_stuffed_point(DrawCtx *dc, const GfxVert *a,
                                   const R5xxVertex *c)
{
    RV530State *s = dc->s;
    R5xxGfx *g = s->gfx;
    uint32_t gbe = REG(s, GB_ENABLE);
    float s0 = regf(s, GA_POINT_S0), t0 = regf(s, GA_POINT_T0);
    float s1 = regf(s, GA_POINT_S1), t1 = regf(s, GA_POINT_T1);
    float hw, hh;
    R5xxVertex w[4];
    uint32_t flat;
    bool cf, cb;
    int k, i;

    if (!(gbe & GB_POINT_STUFF_ENABLE) || !(gbe >> 16)) {
        return false;
    }
    if (dc->psize) {
        hw = hh = a->slot[SLOT_PSIZE].f[0] * 0.5f;
    } else {
        hw = g->raster.d.point_half_w;
        hh = g->raster.d.point_half_h;
    }
    for (k = 0; k < 4; k++) {
        GfxVert v = *a;
        bool right = k == 1 || k == 2, bottom = k >= 2;
        for (i = 0; i < 8; i++) {
            uint32_t src = (gbe >> (16 + 2 * i)) & 3;
            if (src == 1 || src == 2) {
                v.slot[SLOT_TEX0 + i] = r5xx_vec4_make(right ? s1 : s0,
                                                       bottom ? t0 : t1,
                                                       0.0f, 1.0f);
            }
        }
        gfx_to_window(s, &dc->rs, &v, &w[k]);
        w[k].x = c->x + (right ? hw : -hw);
        w[k].y = c->y + (bottom ? hh : -hh);
    }
    /* stuffed coordinates interpolate across the sprite even when the
     * inputs are flat shaded; everything else is equal at all corners */
    flat = g->raster.flat_inputs;
    g->raster.flat_inputs = 0;
    cf = g->raster.d.cull_front;
    cb = g->raster.d.cull_back;
    g->raster.d.cull_front = g->raster.d.cull_back = false;
    gfx_raster_tri(g, &w[0], &w[1], &w[2]);
    gfx_raster_tri(g, &w[0], &w[2], &w[3]);
    g->raster.d.cull_front = cf;
    g->raster.d.cull_back = cb;
    g->raster.flat_inputs = flat;
    return true;
}

static void gfx_emit_point(DrawCtx *dc, const GfxVert *a)
{
    R5xxGfx *g = dc->s->gfx;
    R5xxVertex w;
    int p;

    for (p = 0; p < dc->clip.nplanes; p++) {
        if (plane_dist(dc->clip.plane[p], &a->slot[SLOT_POS]) < 0) {
            return;
        }
    }
    gfx_to_window(dc->s, &dc->rs, a, &w);
    if (gfx_emit_stuffed_point(dc, a, &w)) {
        g->prims++;
        return;
    }
    if (dc->psize) {
        gfx_raster_point(g, &w, true, a->slot[SLOT_PSIZE].f[0]);
    } else {
        gfx_raster_point(g, &w, false, 0.0f);
    }
    g->prims++;
}

/* fetch + transform with a small direct-mapped cache for indexed draws */
static const GfxVert *gfx_get_vert(RV530State *s, const VtxSource *src,
                                   uint32_t index)
{
    R5xxGfx *g = s->gfx;
    uint32_t slot = index % VCACHE_SIZE;

    if (g->vcache_tag[slot] != index + 1) {
        gfx_vertex(s, src, index, &g->vcache[slot]);
        g->vcache_tag[slot] = index + 1;
        g->verts++;
    }
    return &g->vcache[slot];
}

/*
 * Draw 'n' vertices; vertex i is element idx[i] (or i when idx is NULL)
 * of the source.
 */
static void gfx_run_prims(RV530State *s, DrawCtx *dcp, uint32_t prim,
                          const VtxSource *src, const uint32_t *idx, uint32_t n)
{
    DrawCtx dc = *dcp;
    GfxVert v[4];
    uint32_t i;

    memset(s->gfx->vcache_tag, 0, sizeof(s->gfx->vcache_tag));
#define V(k) gfx_get_vert(s, src, idx ? idx[k] : (k))
    switch (prim) {
    case 1:                                             /* points */
        for (i = 0; i < n; i++) {
            gfx_emit_point(&dc, V(i));
        }
        break;
    case 2:                                             /* lines */
        for (i = 0; i + 1 < n; i += 2) {
            v[0] = *V(i);
            gfx_emit_line(&dc, &v[0], V(i + 1));
        }
        break;
    case 3:                                             /* line strip */
    case 12:                                            /* line loop */
        for (i = 0; i + 1 < n; i++) {
            v[0] = *V(i);
            gfx_emit_line(&dc, &v[0], V(i + 1));
        }
        if (prim == 12 && n > 2) {
            v[0] = *V(n - 1);
            gfx_emit_line(&dc, &v[0], V(0));
        }
        break;
    case 4:                                             /* triangles */
        for (i = 0; i + 2 < n; i += 3) {
            v[0] = *V(i); v[1] = *V(i + 1);
            gfx_emit_tri(&dc, &v[0], &v[1], V(i + 2));
        }
        break;
    case 5:                                             /* fan */
    case 15:                                            /* polygon */
        for (i = 1; i + 1 < n; i++) {
            v[0] = *V(0); v[1] = *V(i);
            gfx_emit_tri(&dc, &v[0], &v[1], V(i + 1));
        }
        break;
    case 6:                                             /* strip */
        for (i = 0; i + 2 < n; i++) {
            if (i & 1) {
                v[0] = *V(i + 1); v[1] = *V(i);
            } else {
                v[0] = *V(i); v[1] = *V(i + 1);
            }
            gfx_emit_tri(&dc, &v[0], &v[1], V(i + 2));
        }
        break;
    case 13:                                            /* quads */
        for (i = 0; i + 3 < n; i += 4) {
            v[0] = *V(i); v[1] = *V(i + 1); v[2] = *V(i + 2); v[3] = *V(i + 3);
            gfx_emit_tri(&dc, &v[0], &v[1], &v[2]);
            gfx_emit_tri(&dc, &v[0], &v[2], &v[3]);
        }
        break;
    case 14:                                            /* quad strip */
        for (i = 0; i + 3 < n; i += 2) {
            v[0] = *V(i); v[1] = *V(i + 1); v[2] = *V(i + 3); v[3] = *V(i + 2);
            gfx_emit_tri(&dc, &v[0], &v[1], &v[2]);
            gfx_emit_tri(&dc, &v[0], &v[2], &v[3]);
        }
        break;
    default:
        gfx_warn(s, "primitive type %u not implemented", prim);
        break;
    }
#undef V
}

/* CPU-path draw, on the worker pool when it is worth it (see cpool) */
static void gfx_cpu_draw(RV530State *s, DrawCtx *dc, uint32_t prim,
                         const VtxSource *src, const uint32_t *idx, uint32_t n)
{
    R5xxGfx *g = s->gfx;
    int y0 = g->raster.d.sc_y0, y1 = g->raster.d.sc_y1, h = y1 - y0 + 1;
    int t, nt, band;
    static int timing = -1;
    int64_t t0 = 0, t1 = 0;

    if (timing < 0) {
        timing = getenv("RV530_CPU_TIMING") != NULL;
    }
    if (timing) {
        t0 = get_clock();
    }
    /* n is not a size hint: WindowServer composites whole windows as one
     * sized point sprite (n = 1), so only a short scissor stays serial */
    if (cpool.nthreads < 2 || h < 64 || n < 1 ||
        trace_event_get_state_backends(TRACE_RV530_DBG)) {
        gfx_run_prims(s, dc, prim, src, idx, n);
        if (timing && get_clock() - t0 > 50000000) {
            fprintf(stderr, "rv530-cpu: unpooled draw prim %u n %u h %d: %.1f ms\n",
                    prim, n, h, (get_clock() - t0) / 1e6);
        }
        return;
    }
    cpool.n = 0;
    cpool.collect = true;
    gfx_run_prims(s, dc, prim, src, idx, n);
    cpool.collect = false;
    if (timing) {
        t1 = get_clock();
    }
    if (!cpool.n) {
        return;
    }
    nt = MIN(cpool.nthreads, h / 32);
    band = ((h + nt - 1) / nt + 1) & ~1;    /* even: 2x2 quads stay in one band */
    for (t = 0; t < nt; t++) {
        R5xxRaster *r = &cpool.r[t];
        *r = g->raster;
        r->pixels_in = r->pixels_out = r->quads = r->zpass_count = 0;
        r->d.sc_y0 = y0 + t * band;
        r->d.sc_y1 = MIN(y0 + (t + 1) * band - 1, y1);
        if (r->d.sc_y0 > y1) {
            r->d.sc_y0 = y1 + 1;            /* empty band */
        }
        qemu_sem_post(&cpool.go[t]);
    }
    /* the workers use only their rasterizer copies, VRAM and the GART cache
     * (own lock): let other vCPUs run while they work */
    rv530_vk_wait_cb(s, true);
    for (t = 0; t < nt; t++) {
        qemu_sem_wait(&cpool.done);
    }
    rv530_vk_wait_cb(s, false);
    if (timing && get_clock() - t0 > 50000000) {
        fprintf(stderr, "rv530-cpu: pooled draw prim %u n %u prims %u h %d: setup %.1f ms, raster %.1f ms\n",
                prim, n, cpool.n, h, (t1 - t0) / 1e6, (get_clock() - t1) / 1e6);
    }
    for (t = 0; t < nt; t++) {
        g->raster.pixels_in += cpool.r[t].pixels_in;
        g->raster.pixels_out += cpool.r[t].pixels_out;
        g->raster.quads += cpool.r[t].quads;
        g->raster.zpass_count += cpool.r[t].zpass_count;
    }
}

/* Try to run this draw on the Vulkan backend.  False: use the CPU path. */
static bool gfx_timed_vk_draw(RV530State *s, const R5xxVkDrawReq *req,
                              char *why, size_t whylen)
{
    int64_t t0 = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
    bool ok = r5xx_vk_draw(s->vk, req, why, whylen);

    s->prof_ns[RV_PROF_VKDRAW] += qemu_clock_get_ns(QEMU_CLOCK_REALTIME) - t0;
    s->prof_n[RV_PROF_VKDRAW]++;
    return ok;
}

/*
 * vk-verify renders each draw a second time on the CPU path, in the vCPU's
 * MMIO exit.  A CPU-path Core Image blur takes a second per frame, and xnu
 * panics an SMP guest whose vCPU misses TLB shootdowns that long, so
 * each CP run gets a budget (RV530_VK_VERIFY_BUDGET_MS, default 150; 0 =
 * verify everything).  Draws predicted to overrun it are skipped: a
 * program's cost per pixel is learned from its verified draws, and unknown
 * programs are only verified on draws of up to 256x256 (a Core Image blur
 * on the raster pool costs about 1 us per pixel).
 */
static int64_t verify_budget_ns = -1;

static uint64_t gfx_verify_area(const R5xxVkDrawReq *req)
{
    return (uint64_t)(req->sc_x1 - req->sc_x0 + 1) * (uint64_t)(req->sc_y1 - req->sc_y0 + 1);
}

static bool gfx_verify_allowed(RV530State *s, const R5xxVkDrawReq *req)
{
    R5xxGfx *g = s->gfx;
    uint64_t area = gfx_verify_area(req);
    gpointer cost;

    if (verify_budget_ns < 0) {
        const char *e = getenv("RV530_VK_VERIFY_BUDGET_MS");
        verify_budget_ns = (e ? atoi(e) : 150) * 1000000ll;
    }
    if (!verify_budget_ns) {
        return true;
    }
    if (g->verify_run != s->cp_runs) {
        g->verify_run = s->cp_runs;
        g->verify_ns = 0;
    }
    if (!g->verify_cost) {
        g->verify_cost = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, NULL);
    }
    cost = g_hash_table_lookup(g->verify_cost, &req->prog_hash);
    if (cost ? g->verify_ns + (int64_t)(GPOINTER_TO_SIZE(cost) * (area / 1024 + 1)) >
               verify_budget_ns
             : (area > 256 * 256 || g->verify_ns >= verify_budget_ns)) {
        g->vk_verify_skipped++;
        return false;
    }
    return true;
}

static void gfx_verify_account(RV530State *s, const R5xxVkDrawReq *req, int64_t ns)
{
    R5xxGfx *g = s->gfx;
    uint64_t per;
    int64_t *key;

    if (!verify_budget_ns) {
        return;
    }
    g->verify_ns += ns;
    per = (uint64_t)ns / (gfx_verify_area(req) / 1024 + 1);
    key = g_new(int64_t, 1);
    *key = (int64_t)req->prog_hash;
    g_hash_table_replace(g->verify_cost, key, GSIZE_TO_POINTER(MAX(per, 1)));
}

static bool gfx_vk_try(RV530State *s, DrawCtx *dc, uint32_t prim,
                       uint32_t vf_cntl, const VtxSource *src,
                       const uint32_t *idx, uint32_t n)
{
    R5xxGfx *g = s->gfx;
    g_autofree RvkDraw *draw = NULL;
    R5xxVkDrawReq req;
    RvkMem mem;
    uint32_t tex_used;
    char why[256];
    g_autofree uint8_t *save = NULL, *cpu = NULL;
    uint64_t total = 0;
    int64_t t0, t1;
    bool ok;
    int k;

    if (!s->vk || !g->us_dec || r500_us_uses_flow_control(g->us_dec)) {
        goto fallback;
    }
    if (g->raster.d.aa_resolve) {
        goto fallback;              /* a copy; the CPU path does it */
    }
    tex_used = r500_us_tex_units_used(g->us_dec) & REG(s, TX_ENABLE);
    mem.fb_start = s->fb_start;
    mem.vram_size = s->vram_size;
    mem.gart_start = s->gart_start;
    mem.gart_end = s->gart_end;
    mem.gart_prepare = (s->gart_enabled && s->vk_sysram) ? rv530_vk_gart_prepare : NULL;
    mem.opaque = s;
    draw = g_new(RvkDraw, 1);
    t0 = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
    ok = rvk_build_draw(draw, &g->raster, g->samplers, tex_used,
                        (const r5xx_vec4 *)g->us_const.f, &mem, why, sizeof(why));
    t1 = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
    s->prof_ns[RV_PROF_VKBUILD] += t1 - t0;
    s->prof_n[RV_PROF_VKBUILD]++;
    if (!ok) {
        trace_rv530_vk_fallback(why);
        goto fallback;
    }
    g->vk_collect = true;
    g->vk_unsupported = false;
    g->vk_nverts = 0;
    g->vk_y0 = 1e30f;
    g->vk_y1 = -1e30f;
    gfx_run_prims(s, dc, prim, src, idx, n);
    g->vk_collect = false;
    s->prof_ns[RV_PROF_VKPRIMS] += qemu_clock_get_ns(QEMU_CLOCK_REALTIME) - t1;
    s->prof_n[RV_PROF_VKPRIMS]++;
    if (g->vk_unsupported) {
        trace_rv530_vk_fallback("primitive type (lines / polygon mode / offset)");
        goto fallback;
    }
    if (!g->vk_nverts) {
        return true;                    /* everything culled */
    }

    memset(&req, 0, sizeof(req));
    req.draw = draw;
    req.verts = g->vk_verts;
    req.nverts = g->vk_nverts;
    req.prog = g->us_dec;
    req.prog_hash = g->us_hash;
    req.sc_x0 = g->raster.d.sc_x0;
    req.sc_y0 = MAX(g->raster.d.sc_y0, (int)floorf(g->vk_y0) - 1);
    req.sc_x1 = g->raster.d.sc_x1;
    req.sc_y1 = MIN(g->raster.d.sc_y1, (int)ceilf(g->vk_y1) + 1);
    if (req.sc_y1 < req.sc_y0) {
        return true;
    }
    rvk_dirty_ranges(&req, &g->raster, &mem, req.sc_y0, req.sc_y1);

    for (k = 0; k < req.ndirty; k++) {
        if (req.dirty_off[k] >= RVK_GART_SPACE) {
            break;
        }
    }
    if (s->vk_verify && k == req.ndirty && gfx_verify_allowed(s, &req)) {
        int64_t v0 = get_clock();
        /* reference result from the CPU path on the same input */
        rv530_gfx_flush(s);
        for (k = 0; k < req.ndirty; k++) {
            total += req.dirty_len[k];
        }
        save = g_malloc(total);
        cpu = g_malloc(total);
        total = 0;
        for (k = 0; k < req.ndirty; k++) {
            memcpy(save + total, s->vram_ptr + req.dirty_off[k], req.dirty_len[k]);
            total += req.dirty_len[k];
        }
        /*
         * The reference render, on the raster pool like any CPU-path draw,
         * and without the BQL: it only touches guest memory and the
         * CPU rasterizer's state, like the raster workers.
         */
        rv530_vk_wait_cb(s, true);
        gfx_cpu_draw(s, dc, prim, src, idx, n);
        rv530_vk_wait_cb(s, false);
        gfx_verify_account(s, &req, get_clock() - v0);
        total = 0;
        for (k = 0; k < req.ndirty; k++) {
            memcpy(cpu + total, s->vram_ptr + req.dirty_off[k], req.dirty_len[k]);
            memcpy(s->vram_ptr + req.dirty_off[k], save + total, req.dirty_len[k]);
            total += req.dirty_len[k];
        }
    }
    if (!r5xx_vk_pending(s->vk) && r5xx_vk_mirrored(s->vk)) {
        /* new batch: bring the GPU's VRAM copy up to date */
        rv530_collect_dirty(s);
    }
    if (!gfx_timed_vk_draw(s, &req, why, sizeof(why))) {
        if (strcmp(why, "compiling")) {
            trace_rv530_vk_fallback(why);
        } else {
            g->st_compiling++;
        }
        if (save) {
            /* restore the CPU result rather than drawing twice */
            total = 0;
            for (k = 0; k < req.ndirty; k++) {
                memcpy(s->vram_ptr + req.dirty_off[k], cpu + total, req.dirty_len[k]);
                total += req.dirty_len[k];
            }
            return true;
        }
        goto fallback;
    }
    if (s->vk_sync && !save) {
        rv530_gfx_flush(s);
    }
    if (save) {
        uint64_t bad = 0, maxd = 0, i;
        r5xx_vk_flush(s->vk);           /* zpass: keep the CPU count */
        total = 0;
        for (k = 0; k < req.ndirty; k++) {
            const uint8_t *gp = s->vram_ptr + req.dirty_off[k];
            for (i = 0; i < req.dirty_len[k]; i++) {
                int d = abs((int)gp[i] - (int)cpu[total + i]);
                if (d > 2) {
                    if (bad < 8) {
                        trace_rv530_vk_verify_byte(req.dirty_off[k] + i, gp[i],
                                                   cpu[total + i]);
                    }
                    bad++;
                }
                maxd = MAX(maxd, (uint64_t)d);
            }
            total += req.dirty_len[k];
        }
        g->vk_verify_draws++;
        if (bad) {
            g->vk_verify_bad++;
            trace_rv530_vk_verify(g->draws, g->us_hash, (uint32_t)bad,
                                  (uint32_t)maxd, (uint32_t)total);
        }
        memory_region_set_dirty(&s->vram, 0, s->vram_size);
    }
    return true;

fallback:
    g->vk_collect = false;
    if (s->vk) {
        g->vk_fallbacks++;
    }
    return false;
}

/*
 * Debug aid (trace event rv530_dump): after each draw write the render
 * target (de-tiled) and the texture of unit 0 as PPM files into
 * $RV530_DUMP_DIR (default: current directory).
 */
static void gfx_dump_ppm(const char *path, uint32_t w, uint32_t h,
                         const uint8_t *rgb)
{
    FILE *f = fopen(path, "wb");
    if (!f) {
        return;
    }
    fprintf(f, "P6\n%u %u\n255\n", w, h);
    fwrite(rgb, 3, (size_t)w * h, f);
    fclose(f);
    trace_rv530_dump(path);
}

static void gfx_dump_draw(RV530State *s)
{
    R5xxGfx *g = s->gfx;
    const char *dir = g_getenv("RV530_DUMP_DIR") ?: ".";
    const R5xxSurfaceLayout *l = &g->raster.rt.cbuf[0];
    uint32_t w = MIN(l->width, (uint32_t)MAX(g->raster.d.sc_x1 + 1, 1));
    uint32_t h = MIN(2048u, (uint32_t)MAX(g->raster.d.sc_y1 + 1, 1));
    g_autofree uint8_t *rgb = NULL;
    g_autofree char *path = NULL;
    uint32_t x, y;

    if (l->bpp == 4 && w && w <= 4096) {
        rgb = g_malloc((size_t)w * h * 3);
        for (y = 0; y < h; y++) {
            for (x = 0; x < w; x++) {
                uint32_t v = r5xx_mem_read32(&s->mem, l->gpu_addr +
                                             r5xx_surface_pixel_offset(l, x, y));
                uint8_t *p = rgb + 3 * ((size_t)y * w + x);
                p[0] = v >> 16; p[1] = v >> 8; p[2] = v;
            }
        }
        path = g_strdup_printf("%s/d%05" PRIu64 "-cb-%08x.ppm", dir, g->draws,
                               l->gpu_addr);
        gfx_dump_ppm(path, w, h, rgb);
    }
    if (g->raster.rt.zbuf_enabled && g->raster.rt.zbuf.bpp == 4 && w && w <= 4096) {
        /* depth: R = Z high byte, G = next byte, B = stencil */
        const R5xxSurfaceLayout *z = &g->raster.rt.zbuf;
        g_free(rgb);
        g_free(path);
        rgb = g_malloc((size_t)w * h * 3);
        for (y = 0; y < h; y++) {
            for (x = 0; x < w; x++) {
                uint32_t v = r5xx_mem_read32(&s->mem, z->gpu_addr +
                                             r5xx_surface_pixel_offset(z, x, y));
                uint8_t *p = rgb + 3 * ((size_t)y * w + x);
                p[0] = v >> 24; p[1] = v >> 16; p[2] = v;
            }
        }
        path = g_strdup_printf("%s/d%05" PRIu64 "-zb-%08x.ppm", dir, g->draws,
                               z->gpu_addr);
        gfx_dump_ppm(path, w, h, rgb);
    }
    for (unsigned u = 0; u < 2; u++) {
    if (g->samplers[u].valid && (REG(s, TX_ENABLE) & (1u << u))) {
        const R5xxTexSampler *t = &g->samplers[u];
        uint32_t tw = MIN(t->width, 2048u), th = MIN(t->height, 2048u);
        g_free(rgb);
        g_free(path);
        rgb = g_malloc((size_t)tw * th * 3);
        for (y = 0; y < th; y++) {
            for (x = 0; x < tw; x++) {
                r5xx_vec4 c;
                uint8_t *p = rgb + 3 * ((size_t)y * tw + x);
                r5xx_tex_fetch_texel(t, &s->mem, 0, 0, 0, x, y, &c);
                p[0] = (uint8_t)(MIN(MAX(c.f[0], 0.0f), 1.0f) * 255.0f);
                p[1] = (uint8_t)(MIN(MAX(c.f[1], 0.0f), 1.0f) * 255.0f);
                p[2] = (uint8_t)(MIN(MAX(c.f[2], 0.0f), 1.0f) * 255.0f);
            }
        }
        path = g_strdup_printf("%s/d%05" PRIu64 "-tx%u-%08x.ppm", dir, g->draws,
                               u, t->layout.gpu_addr);
        {
            g_autofree char *m = g_strdup_printf(
                "tx0 draw %" PRIu64 " off 0x%08x f0 0x%08x f1 0x%08x f2 0x%08x "
                "filt0 0x%08x -> %ux%u bpp %u hwfmt 0x%x pitch %u tiling %d "
                "micro %u pitch_en %d", g->draws, REG(s, TX_OFFSET_0),
                REG(s, TX_FORMAT0_0), REG(s, TX_FORMAT1_0), REG(s, TX_FORMAT2_0),
                REG(s, TX_FILTER0_0), t->width, t->height, t->bpp,
                t->hw_format, t->layout.pitch_bytes, t->layout.tiling,
                t->layout.micro_mode, t->pitch_en);
            trace_rv530_dbg(m);
        }
        gfx_dump_ppm(path, tw, th, rgb);
    }
    }
}

static void gfx_draw(RV530State *s, uint32_t vf_cntl, const VtxSource *src,
                     const uint32_t *idx, uint32_t n)
{
    R5xxGfx *g = s->gfx;
    DrawCtx dc;
    char err[256];
    uint32_t prim = VF_PRIM(vf_cntl);
    bool bypass = REG(s, VAP_CNTL_STATUS) & VAP_TCL_BYPASS;
    bool tried_vk;
    uint32_t i;
    uint64_t px_in0 = g->raster.pixels_in, px_out0 = g->raster.pixels_out;

    int64_t setup0 = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);

    g->draws++;
    g->dbg_tex = 0;
    trace_rv530_draw(prim, vf_cntl, n);
    if (!n || prim == 0) {
        return;
    }
    if (!bypass && !gfx_update_pvs(s)) {
        return;
    }
    gfx_load_aos(s);
    if (!gfx_update_us(s)) {
        return;
    }
    gfx_update_textures(s);

    memset(&dc, 0, sizeof(dc));
    dc.s = s;
    dc.psize = REG(s, VAP_OUTPUT_VTX_FMT_0) & (1u << 16);
    gfx_clip_setup(s, &dc.clip);

    gfx_raster_regs(s, &g->raster.regs);
    g->raster.mem = &s->mem;
    g->raster.us.prog = g->us_dec;
    g->raster.us.consts = (const r5xx_vec4 *)g->us_const.f;
    g->raster.us.tex = gfx_tex_cb;
    g->raster.us.tex_opaque = s;
    gfx_rs_setup(s, &dc.rs, &g->raster, prim);
    if (!r5xx_raster_setup(&g->raster, err, sizeof(err))) {
        gfx_warn(s, "raster setup failed: %s", err);
        return;
    }
    memset(g->vcache_tag, 0, sizeof(g->vcache_tag));

    {
        int64_t p0 = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
        s->prof_ns[RV_PROF_SETUP] += p0 - setup0;
        s->prof_n[RV_PROF_SETUP]++;
        tried_vk = gfx_vk_try(s, &dc, prim, vf_cntl, src, idx, n);
        s->prof_ns[RV_PROF_VKPREP] += qemu_clock_get_ns(QEMU_CLOCK_REALTIME) - p0;
        s->prof_n[RV_PROF_VKPREP]++;
    }
    if (!tried_vk) {
        int64_t t0;
        rv530_gfx_flush(s);
        t0 = rvk_stats_on ? qemu_clock_get_ns(QEMU_CLOCK_REALTIME) : 0;
        {
            int64_t c0 = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
            gfx_cpu_draw(s, &dc, prim, src, idx, n);
            s->prof_ns[RV_PROF_CPU] += qemu_clock_get_ns(QEMU_CLOCK_REALTIME) - c0;
            s->prof_n[RV_PROF_CPU]++;
        }
        if (rvk_stats_on) {
            g->st_cpu_ns += qemu_clock_get_ns(QEMU_CLOCK_REALTIME) - t0;
            g->st_cpu_draws++;
            gfx_stats_tick(s);
        }
    } else {
        g->st_gpu_draws++;
        if (rvk_flush_each) {
            rv530_gfx_flush(s);
        }
    }
#define V(k) gfx_get_vert(s, src, idx ? idx[k] : (k))
    if ((REG(s, GB_AA_CONFIG) & 1) || (REG(s, 0x4e88) & 1)) {
        trace_rv530_draw_aa(REG(s, GB_AA_CONFIG), REG(s, 0x4010), REG(s, 0x4014),
                            REG(s, 0x4e88), REG(s, 0x4e80), REG(s, 0x4e84),
                            REG(s, RB3D_COLOROFFSET0), REG(s, RB3D_COLORPITCH0), n, prim);
    }
    if (trace_event_get_state_backends(TRACE_RV530_DRAW_STATE)) {
        R5xxVertex w0;
        gfx_to_window(s, &dc.rs, V(0), &w0);
        trace_rv530_draw_state(n, REG(s, RB3D_COLOROFFSET0),
                               REG(s, RB3D_COLORPITCH0), REG(s, VAP_VTE_CNTL),
                               (int)w0.x, (int)w0.y,
                               (uint32_t)(g->raster.pixels_in - px_in0),
                               (uint32_t)(g->raster.pixels_out - px_out0));
        trace_rv530_draw_tex(REG(s, TX_ENABLE), REG(s, TX_OFFSET_0),
                             REG(s, TX_FORMAT0_0), REG(s, US_CODE_RANGE),
                             (int)(w0.z * 1000), (int)(w0.w * 1000));
    }
    if (trace_event_get_state_backends(TRACE_RV530_DUMP)) {
        rv530_gfx_flush(s);
        gfx_dump_draw(s);
    }
    if (trace_event_get_state_backends(TRACE_RV530_DBG)) {
        R5xxVertex w0;
        GString *h = g_string_new(NULL);
        gfx_to_window(s, &dc.rs, V(0), &w0);
        g_string_printf(h, "blend c 0x%08x a 0x%08x color 0x%08x ar 0x%08x gb 0x%08x "
                        "out_fmt 0x%08x | ",
                        REG(s, 0x4e04), REG(s, 0x4e08), REG(s, 0x4e10),
                        REG(s, 0x4ef8), REG(s, 0x4efc), REG(s, US_OUT_FMT_0));
        g_string_append_printf(h, "psize 0x%08x minmax 0x%08x sc 0x%08x-0x%08x "
                               "vap_out0 0x%08x | ", REG(s, GA_POINT_SIZE),
                               REG(s, 0x4230), REG(s, 0x43e0), REG(s, 0x43e4),
                               REG(s, 0x2090));
        g_string_append_printf(h, "zb cntl 0x%x zs 0x%08x fmt 0x%x off 0x%08x pitch 0x%08x "
                               "cull 0x%x vte 0x%x zscale %g zoff %g clip 0x%08x | ",
                               REG(s, 0x4f00), REG(s, 0x4f04), REG(s, 0x4f10),
                               REG(s, 0x4f20), REG(s, 0x4f24), REG(s, 0x42b8),
                               REG(s, 0x20b0), rv_f32(REG(s, SE_VPORT_ZSCALE)), rv_f32(REG(s, SE_VPORT_ZSCALE + 4)),
                               REG(s, 0x221c));
        g_string_append_printf(h, "prim %u gb_enable 0x%08x pointST %g %g %g %g fmt0 0x%x fmt1 0x%x "
                        "RS count 0x%08x inst_count 0x%08x ip", prim, REG(s, GB_ENABLE),
                        regf(s, GA_POINT_S0), regf(s, GA_POINT_T0),
                        regf(s, GA_POINT_S1), regf(s, GA_POINT_T1),
                        REG(s, VAP_OUTPUT_VTX_FMT_0), REG(s, VAP_OUTPUT_VTX_FMT_1),
                        REG(s, 0x4300), REG(s, 0x4304));
        for (i = 0; i < 4; i++) {
            g_string_append_printf(h, " %08x", REG(s, 0x4074 + 4 * i));
        }
        g_string_append(h, " inst");
        for (i = 0; i < 4; i++) {
            g_string_append_printf(h, " %08x", REG(s, 0x4320 + 4 * i));
        }
        g_string_append_printf(h, " | v0 slots pos [%g %g %g %g] col0 [%g %g %g %g] tex0 [%g %g %g %g]",
            V(0)->slot[SLOT_POS].f[0], V(0)->slot[SLOT_POS].f[1], V(0)->slot[SLOT_POS].f[2], V(0)->slot[SLOT_POS].f[3],
            V(0)->slot[SLOT_COL0].f[0], V(0)->slot[SLOT_COL0].f[1], V(0)->slot[SLOT_COL0].f[2], V(0)->slot[SLOT_COL0].f[3],
            V(0)->slot[SLOT_TEX0].f[0], V(0)->slot[SLOT_TEX0].f[1], V(0)->slot[SLOT_TEX0].f[2], V(0)->slot[SLOT_TEX0].f[3]);
        g_string_append_printf(h, " | tx_en 0x%x", REG(s, TX_ENABLE));
        for (i = 0; i < 2; i++) {
            g_string_append_printf(h, " tx%u f0 %08x f1 %08x f2 %08x flt %08x %08x off %08x", i,
                                   REG(s, TX_FORMAT0_0 + 4 * i), REG(s, TX_FORMAT1_0 + 4 * i),
                                   REG(s, TX_FORMAT2_0 + 4 * i), REG(s, TX_FILTER0_0 + 4 * i),
                                   REG(s, TX_FILTER1_0 + 4 * i), REG(s, TX_OFFSET_0 + 4 * i));
        }
        if (g->samplers[0].valid) {
            /* texel bytes from the middle of unit 0's first row */
            const R5xxTexSampler *t = &g->samplers[0];
            g_string_append(h, " tex0 bytes");
            for (i = 0; i < 4; i++) {
                uint32_t w = r5xx_mem_read32(&s->mem, t->layout.gpu_addr +
                                             (t->width / 2 * t->bpp & ~3u) + 4 * i);
                g_string_append_printf(h, " %02x %02x %02x %02x", w & 0xff,
                                       (w >> 8) & 0xff, (w >> 16) & 0xff, w >> 24);
            }
        }
        g_string_append(h, " | attr");
        for (i = 0; i < 3; i++) {
            g_string_append_printf(h, " [%g %g %g %g]", w0.attr[i].f[0], w0.attr[i].f[1],
                                   w0.attr[i].f[2], w0.attr[i].f[3]);
        }
        trace_rv530_dbg(h->str);
        g_string_free(h, true);
    }
#undef V
}

/* ------------------------------------------------------------------ */
/* Packets                                                             */
/* ------------------------------------------------------------------ */

static uint32_t gfx_num_verts(RV530State *s, uint32_t vf_cntl)
{
    if (vf_cntl & VF_USE_ALT_NUM) {
        return REG(s, VAP_ALT_NUM_VERTICES) & 0xffffff;
    }
    return VF_NUM(vf_cntl);
}

/* Indexed draw with indices taken from 'words' (16-bit pairs or 32-bit). */
static void gfx_draw_indexed(RV530State *s, uint32_t vf_cntl,
                             const uint32_t *words, uint32_t nwords)
{
    uint32_t n = gfx_num_verts(s, vf_cntl);
    bool i32 = vf_cntl & VF_INDEX_32;
    int32_t bias = ((int32_t)(REG(s, VAP_INDEX_OFFSET) << 7)) >> 7;
    uint32_t vmin = REG(s, VAP_VF_MIN_VTX_INDX) & 0xffffff;
    uint32_t vmax = REG(s, VAP_VF_MAX_VTX_INDX) & 0xffffff;
    uint32_t *idx, i;
    VtxSource src = { 0 };

    if (n > nwords * (i32 ? 1 : 2)) {
        n = nwords * (i32 ? 1 : 2);
    }
    idx = g_new(uint32_t, n ? n : 1);
    for (i = 0; i < n; i++) {
        uint32_t v = i32 ? le32_to_cpu(words[i])
                         : (le32_to_cpu(words[i / 2]) >> (16 * (i & 1))) & 0xffff;
        int64_t b = (int64_t)v + bias;
        if (vmax && (b < vmin || b > vmax)) {
            b = b < vmin ? vmin : vmax;
        }
        idx[i] = (uint32_t)b;
    }
    gfx_draw(s, vf_cntl, &src, idx, n);
    g_free(idx);
}

void rv530_gfx_pm4(RV530State *s, uint32_t opcode, const uint32_t *d,
                   uint32_t count)
{
    R5xxGfx *g = s->gfx;
    VtxSource src = { 0 };
    uint32_t vf, i;

    if (trace_event_get_state_backends(TRACE_RV530_DBG) &&
        (opcode == PM4_3D_LOAD_VBPNTR || opcode == PM4_3D_DRAW_IMMD_2 ||
         opcode == PM4_3D_DRAW_VBUF_2 || opcode == PM4_3D_DRAW_INDX_2)) {
        GString *h = g_string_new(NULL);
        g_string_printf(h, "pkt 0x%02x count %u vtx_size %u:", opcode, count,
                        REG(s, VAP_VTX_SIZE));
        for (i = 0; i < count && i < 16; i++) {
            g_string_append_printf(h, " %08x", d[i]);
        }
        if (opcode == PM4_3D_DRAW_VBUF_2 || opcode == PM4_3D_DRAW_INDX_2) {
            for (i = 0; i < g->num_vbuf && i < 4; i++) {
                uint32_t w[4] = { 0 };
                rv530_gpu_read(s, g->vbuf[i].addr, w, 16);
                g_string_append_printf(h, " | vb%u addr %08x stride %u: %08x %08x %08x %08x",
                                       i, g->vbuf[i].addr, g->vbuf[i].stride,
                                       w[0], w[1], w[2], w[3]);
            }
        }
        trace_rv530_dbg(h->str);
        g_string_free(h, true);
    }
    switch (opcode) {
    case PM4_3D_LOAD_VBPNTR:
        /* the packet is a burst write of VAP_VTX_NUM_ARRAYS and the
         * VAP_VTX_AOS_ATTR/ADDR registers that follow it */
        for (i = 0; i < count && VAP_VTX_NUM_ARRAYS + 4 * i <= VAP_VTX_AOS_LAST; i++) {
            REG(s, VAP_VTX_NUM_ARRAYS + 4 * i) = d[i];
        }
        gfx_load_aos(s);
        return;
    case PM4_3D_DRAW_IMMD:
    case PM4_3D_DRAW_IMMD_2: {
        uint32_t h = opcode == PM4_3D_DRAW_IMMD ? 1 : 0;
        if (count < h + 1) {
            return;
        }
        vf = d[h];
        src.immd = d + h + 1;
        src.immd_dwords = count - h - 1;
        src.vtx_size = REG(s, VAP_VTX_SIZE) & 0x7f;
        if (!src.vtx_size) {
            gfx_warn(s, "immediate draw with VAP_VTX_SIZE 0");
            return;
        }
        gfx_draw(s, vf, &src, NULL,
                 MIN(gfx_num_verts(s, vf), src.immd_dwords / src.vtx_size));
        return;
    }
    case PM4_3D_DRAW_VBUF:
    case PM4_3D_DRAW_VBUF_2: {
        uint32_t h = opcode == PM4_3D_DRAW_VBUF ? 1 : 0;
        if (count < h + 1) {
            return;
        }
        vf = d[h];
        if (VF_WALK(vf) == WALK_INDICES) {
            /* indices follow in an INDX_BUFFER packet */
            g->indx_pending = true;
            g->indx_vf_cntl = vf;
            return;
        }
        /* VAP_INDEX_OFFSET also biases the generated indices of a vertex
         * list: the ATI driver draws consecutive pieces of one vertex
         * buffer (display lists) by changing only the offset */
        if (REG(s, VAP_INDEX_OFFSET) & 0x1ffffff) {
            int32_t bias = ((int32_t)(REG(s, VAP_INDEX_OFFSET) << 7)) >> 7;
            uint32_t n = gfx_num_verts(s, vf);
            g_autofree uint32_t *idx = g_new(uint32_t, n ? n : 1);
            for (i = 0; i < n; i++) {
                idx[i] = (uint32_t)((int64_t)i + bias);
            }
            gfx_draw(s, vf, &src, idx, n);
            return;
        }
        gfx_draw(s, vf, &src, NULL, gfx_num_verts(s, vf));
        return;
    }
    case PM4_3D_DRAW_INDX:
    case PM4_3D_DRAW_INDX_2: {
        uint32_t h = opcode == PM4_3D_DRAW_INDX ? 1 : 0;
        if (count < h + 1) {
            return;
        }
        vf = d[h];
        if (count == h + 1) {
            /* indices come from the following INDX_BUFFER packet */
            g->indx_pending = true;
            g->indx_vf_cntl = vf;
            return;
        }
        gfx_draw_indexed(s, vf, d + h + 1, count - h - 1);
        return;
    }
    case PM4_INDX_BUFFER: {
        uint32_t addr, ndw;
        uint32_t *buf;
        if (count < 3 || !g->indx_pending) {
            gfx_warn(s, "INDX_BUFFER without a pending indexed draw");
            return;
        }
        g->indx_pending = false;
        addr = d[1] & ~3u;
        ndw = MIN(d[2], 1u << 22);
        buf = g_new(uint32_t, ndw ? ndw : 1);
        rv530_gpu_read(s, addr, buf, ndw * 4);
        gfx_draw_indexed(s, g->indx_vf_cntl, buf, ndw);
        g_free(buf);
        return;
    }
    case PM4_3D_CLEAR_ZMASK: {
        /* clearing the ZMASK is a depth/stencil fast clear (see
         * r5xx_raster_zmask_clear); Apple's driver clears depth this way */
        R5xxRaster *r = g_new0(R5xxRaster, 1);
        rv530_gfx_flush(s);
        gfx_raster_regs(s, &r->regs);
        r->mem = &s->mem;
        r5xx_raster_zmask_clear(r, count >= 2 ? d[1] : 0, REG(s, ZB_DEPTHCLEARVALUE));
        g_free(r);
        return;
    }
    case PM4_3D_CLEAR_HIZ:
    case PM4_3D_CLEAR_CMASK:
        /* HiZ only accelerates rejection, and color is stored
         * uncompressed: nothing to emulate */
        return;
    case PM4_3D_DRAW_128:
        gfx_warn(s, "3D_DRAW_128 not implemented");
        return;
    default:
        gfx_warn(s, "PM4 type-3 opcode 0x%02x (%s) count %u d0=0x%08x",
                 opcode, r5xx_pm4_opname(opcode) ?: "?", count,
                 count ? d[0] : 0);
        return;
    }
}
