/*
 * Evergreen 3D engine on Vulkan, host side.  See eg_vk.h for the design.
 *
 * Derived from the rv530 backend (rv530/r5xx_vk.c): the same device
 * setup, VRAM import and device-local mirror, GART table, batching,
 * hazard barriers and asynchronous pipeline compilation with a persistent
 * SPIR-V cache.  What differs is the vertex shader (per-vertex parameters
 * for the fragment shader's own interpolation), the fragment source (the
 * Evergreen template plus the translated PS) and the hazard ranges, which
 * the caller lists in the request.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include <inttypes.h>
#include <pthread.h>
#include <dirent.h>
#include <vulkan/vulkan.h>
#include <shaderc/shaderc.h>
#include "eg_vk.h"

#define EVK_VP              8192        /* viewport / render area size */
#define EVK_DRAW_SLOTS      1024        /* EvkDraw entries per batch */
#define EVK_FRAMES          4           /* batches the GPU may run while one records */
#define EVK_BATCH_DRAWS     64          /* draws per batch within a ring kick */
#define EVK_IDLE_DRAWS      0           /* draws that start a batch on an idle GPU (0: off) */
#define EVK_IDX_SLOTS       (1u << 20)  /* GPU VS vertex indices per batch */
#define EVK_VERT_SLOTS      (1u << 16)  /* EvkVertex entries per batch */
#define EVK_VSOUT_SLOTS     (1u << 17)  /* GPU VS corner outputs per batch (EVK_NO_PERVERTEX) */
#define EVK_MAX_DIRTY       8192
#define EVK_PAGE_SHIFT     12
/* batches upload all stale pages up front only when they are few */
#define EVK_EAGER_UPLOAD   (16ull << 20)

#define EVK_PIPE_BUCKETS    1024

_Static_assert(sizeof(EvkTile) == 96, "EvkTile layout");
_Static_assert(sizeof(EvkTex) == 288, "EvkTex layout");
_Static_assert(sizeof(EvkSurf) == 160, "EvkSurf layout");
_Static_assert(sizeof(EvkDraw) == 21712, "EvkDraw layout");
_Static_assert(sizeof(EvkVertex) == 32 + 16 * EVK_MAX_PARAMS, "EvkVertex layout");
_Static_assert(EVK_M_COUNT <= EVK_MISC_WORDS, "misc words");

enum { EVK_PIPE_PENDING, EVK_PIPE_READY, EVK_PIPE_FAILED };

typedef struct EvkPipe {
    uint64_t hash;
    VkPipeline pipe;
    int state;                  /* EVK_PIPE_*, written by the compiler thread */
    char *src;                  /* GLSL waiting for compilation */
    char *vsrc;                 /* its vertex shader (GPU VS), or NULL: vk->vert */
    bool compute;               /* src is a compute shader (eg_vk_dispatch) */
    uint32_t *spv;              /* or SPIR-V from the disk cache */
    size_t spv_bytes;
    struct EvkPipe *next;       /* hash chain */
    struct EvkPipe *qnext;      /* compile queue */
} EvkPipe;

#define EVK_COMPILERS 6

typedef struct EvkBuf {
    VkBuffer buf;
    VkDeviceMemory mem;
    void *map;
    VkDeviceSize size;
} EvkBuf;

struct EgVk {
    VkInstance inst;
    VkPhysicalDevice phys;
    VkDevice dev;
    VkQueue queue;
    uint32_t qfam;
    VkCommandPool pool;
    VkCommandBuffer cmd;        /* the recording batch: cmds[frame] */
    VkFence fence;
    VkCommandBuffer cmds[EVK_FRAMES];
    VkFence fences[EVK_FRAMES];
    /*
     * The draw states: the CPU writes 'draws' (host memory), and at submit
     * precmds[frame] copies the batch's states to draws_dev (device local)
     * ahead of the batch, so the shaders do not read them across PCIe.
     * EG_VK_DRAWS_HOST=1: the shaders read 'draws'.
     */
    VkCommandBuffer precmds[EVK_FRAMES];
    EvkBuf draws_dev;
    bool draws_local;
    bool busy[EVK_FRAMES];      /* submitted, fence not waited for yet */
    int frame;
    /* submission sequence numbers: batches complete in this order */
    uint64_t submit_seq, done_seq, frame_seq[EVK_FRAMES];
    /*
     * Completion thread: it waits for the fence of each submitted batch in
     * order and publishes gpu_done_seq, then calls notify (on its thread).
     * The main thread recycles a frame only once gpu_done_seq covers it, so
     * it never resets a fence that the thread still waits for.
     */
    pthread_t waiter;
    bool waiter_on, dquit;
    pthread_mutex_t dlock;
    pthread_cond_t dcond;           /* new submission, or a batch done */
    uint64_t pub_seq;               /* submitted, as the thread sees it */
    uint64_t gpu_done_seq;
    void (*notify)(void *opaque);
    void *notify_opaque;
    uint64_t stat_done_ns, stat_done_n;
    /* VRAM ranges each submitted batch renders to (dirty_fn on completion) */
    uint64_t *fdirty_off[EVK_FRAMES], *fdirty_len[EVK_FRAMES];
    int fndirty[EVK_FRAMES];
    /* mirror uploads submitted between batches (eg_vk_upload_kick) */
    VkCommandBuffer upcmd;
    VkFence upfence;
    bool up_inflight, up_barrier;
    VkDescriptorSetLayout dsl;
    VkDescriptorPool dpool;
    VkDescriptorSet dset;
    VkPipelineLayout layout;
    VkPipelineLayout cslayout;  /* compute dispatches: the same set, push constants */
    VkShaderModule vert;
    VkPipelineCache pcache;

    EvkBuf vram;                /* imported guest VRAM */
    EvkBuf draws, verts, counts;
    EvkBuf idxbuf;              /* GPU VS indices, EVK_IDX_SLOTS per frame */
    /*
     * EVK_NO_PERVERTEX (MoltenVK: Metal has no per-vertex fragment inputs):
     * the PS reads the three corners of its triangle from buffers.  vsout
     * holds the GPU VS outputs of each corner, EVK_VSOUT_SLOTS per frame.
     */
    bool no_pervertex;
    EvkBuf vsout;
    uint32_t nvsout;
    uint32_t nidx;
    EvkBuf sysram;              /* imported guest RAM (GART targets), optional */
    /* device-local mirror of VRAM: shaders use it; uploads of CPU-dirty
     * pages at batch start, downloads of rendered ranges at flush */
    EvkBuf vram_dev;
    bool mirror;
    /* VRAM pages changed outside the GPU since they were last copied to
     * the mirror (4 KiB granularity) */
    uint64_t *stale;
    uint64_t stale_pages;
    /* pages a draw of the current batch renders to: within the render
     * ranges the mirror is newer than host VRAM until the batch is read
     * back, so mid-batch uploads there skip those bytes */
    uint64_t *batch_wr;
    /* pages of vram_dev that fragments wrote (bit per 4 KiB): the copy-back
     * copies only these, in a compute pass (cback) */
    EvkBuf dirty_bits;
    bool bits_clear;            /* dirty_bits zeroed on the GPU */
    VkDescriptorSetLayout cdsl;
    VkDescriptorPool cdpool;
    VkDescriptorSet cdset;
    VkPipelineLayout clayout;
    VkPipeline cback;
    /* early depth test: depth surfaces (write bases) that a draw of the
     * current batch made larger since the last barrier */
    uint64_t zgrow[8];
    int nzgrow;
    bool early_z;               /* EG_VK_NO_EARLY_Z=1: off */
    EvkBuf gart;                /* GART page -> sysram page */
    uint32_t gart_pages;
    VkDeviceSize sysram_range;
    uint64_t vram_size;

    shaderc_compiler_t sc;
    shaderc_compile_options_t sco;
    char *frag_prefix;          /* #version + defines */
    bool has_f64;               /* shaderFloat64 (Apple GPUs have none) */
    bool portability;           /* VK_KHR_portability_subset (MoltenVK) */
    /* persistent SPIR-V cache (eg_vk_set_cache_dir): <dir>/<tpl>-<prog>.spv
     * plus the order in which the guest first used each program */
    char *cache_dir;
    uint64_t tpl_hash;
    FILE *order;
    int64_t wait_ns;            /* compile waiting in the current batch */
    int new_pipes;              /* created since the pipeline cache was saved */
    int64_t pcache_saved;       /* CLOCK_MONOTONIC s of the last save */

    EvkPipe *pipes[EVK_PIPE_BUCKETS];
    VkPipeline bound;

    /* asynchronous pipeline compilation */
    pthread_t compilers[EVK_COMPILERS];
    int ncompilers;
    pthread_mutex_t qlock;
    pthread_cond_t qcond;
    EvkPipe *qhead, *qtail;
    bool quit, sync_compile;
    bool barrier_each;          /* EG_VK_BARRIER_EACH: debug */
    bool specialize;            /* spec_pipeline(); EG_VK_NO_SPECIALIZE=1: off */

    /* current batch */
    bool recording, rendering;
    uint32_t ndraws, nverts;
    uint64_t dirty_off[EVK_MAX_DIRTY], dirty_len[EVK_MAX_DIRTY];
    int ndirty;
    /* hazards since the last barrier: ranges written (with the surface
     * signature that wrote them) and ranges sampled */
    int nw, nr;
    uint64_t w_off[EVK_MAX_DIRTY], w_len[EVK_MAX_DIRTY], w_sig[EVK_MAX_DIRTY];
    uint64_t r_off[EVK_MAX_DIRTY], r_len[EVK_MAX_DIRTY];

    EgVkDirtyFn dirty_fn;
    EgVkWaitFn wait_fn;
    void *opaque;

    uint64_t stat_draws, stat_batches, stat_pipes, stat_barriers, stat_upload_bytes;
    uint64_t stat_dispatches;
    uint64_t stat_binds;        /* pipeline changes between draws */
    uint32_t zslot;             /* counts[] slot of the passing pixels of new draws */
    /* EG_VK_DRAWSTAMPS: a timestamp after each draw, GPU time by program */
    VkQueryPool dsq;
    uint64_t *dprog;            /* [EVK_FRAMES][EVK_DRAW_SLOTS] program hash */
    uint32_t *darea;            /* scissor area */
    uint32_t dn[EVK_FRAMES];
    uint64_t stat_wait_ns;      /* waiting for a frame to complete */
    /* GPU time of the batches (timestamps) and submit-to-recycle latency */
    VkQueryPool tsq;
    double ts_period;
    int64_t submit_t[EVK_FRAMES];
    uint64_t stat_gpu_ns, stat_lat_ns, stat_timed, stat_gpu_back_ns;
    /* EG_VK_TRACE: per-batch log */
    bool trace;
    uint64_t b_area;
    uint32_t b_gart, b_barriers;
    uint64_t b_up_bytes, b_back_bytes;
    uint32_t b_up_n;
    char b_progs[512];
};

static void set_err(char *err, size_t len, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));
static void set_err(char *err, size_t len, const char *fmt, ...)
{
    va_list ap;
    if (!err || !len) {
        return;
    }
    va_start(ap, fmt);
    vsnprintf(err, len, fmt, ap);
    va_end(ap);
}

/* ------------------------------------------------------------------ */
/* Buffers                                                             */
/* ------------------------------------------------------------------ */

static int find_mem_type(EgVk *vk, uint32_t bits, VkMemoryPropertyFlags want)
{
    VkPhysicalDeviceMemoryProperties mp;
    uint32_t i;

    vkGetPhysicalDeviceMemoryProperties(vk->phys, &mp);
    for (i = 0; i < mp.memoryTypeCount; i++) {
        if ((bits & (1u << i)) &&
            (mp.memoryTypes[i].propertyFlags & want) == want) {
            return (int)i;
        }
    }
    return -1;
}

static bool host_buf(EgVk *vk, EvkBuf *b, VkDeviceSize size,
                     VkBufferUsageFlags usage, char *err, size_t errlen)
{
    VkBufferCreateInfo bi = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = size, .usage = usage,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    VkMemoryRequirements mr;
    VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    int mt;

    b->size = size;
    if (vkCreateBuffer(vk->dev, &bi, NULL, &b->buf) != VK_SUCCESS) {
        set_err(err, errlen, "vkCreateBuffer failed");
        return false;
    }
    vkGetBufferMemoryRequirements(vk->dev, b->buf, &mr);
    mt = find_mem_type(vk, mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                       VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (mt < 0) {
        set_err(err, errlen, "no host-visible coherent memory type");
        return false;
    }
    ai.allocationSize = mr.size;
    ai.memoryTypeIndex = (uint32_t)mt;
    if (vkAllocateMemory(vk->dev, &ai, NULL, &b->mem) != VK_SUCCESS ||
        vkBindBufferMemory(vk->dev, b->buf, b->mem, 0) != VK_SUCCESS ||
        vkMapMemory(vk->dev, b->mem, 0, VK_WHOLE_SIZE, 0, &b->map) != VK_SUCCESS) {
        set_err(err, errlen, "host buffer allocation failed");
        return false;
    }
    return true;
}

static bool import_host(EgVk *vk, EvkBuf *out, void *ptr, uint64_t size,
                        char *err, size_t errlen)
{
    PFN_vkGetMemoryHostPointerPropertiesEXT get_props =
        (PFN_vkGetMemoryHostPointerPropertiesEXT)
        vkGetDeviceProcAddr(vk->dev, "vkGetMemoryHostPointerPropertiesEXT");
    VkMemoryHostPointerPropertiesEXT hp = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT,
    };
    VkExternalMemoryBufferCreateInfo eb = {
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT,
    };
    VkBufferCreateInfo bi = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .pNext = &eb,
        .size = size, .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                 VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    VkImportMemoryHostPointerInfoEXT imp = {
        .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT,
        .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT,
        .pHostPointer = ptr,
    };
    VkMemoryAllocateInfo ai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = &imp,
        .allocationSize = size,
    };
    VkMemoryRequirements mr;
    int mt;

    if (!get_props) {
        set_err(err, errlen, "vkGetMemoryHostPointerPropertiesEXT missing");
        return false;
    }
    if (get_props(vk->dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT,
                  ptr, &hp) != VK_SUCCESS) {
        set_err(err, errlen, "host pointer not importable");
        return false;
    }
    if (vkCreateBuffer(vk->dev, &bi, NULL, &out->buf) != VK_SUCCESS) {
        set_err(err, errlen, "host buffer creation failed");
        return false;
    }
    vkGetBufferMemoryRequirements(vk->dev, out->buf, &mr);
    mt = find_mem_type(vk, hp.memoryTypeBits & mr.memoryTypeBits,
                       VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (mt < 0) {
        mt = find_mem_type(vk, hp.memoryTypeBits & mr.memoryTypeBits, 0);
    }
    if (mt < 0) {
        set_err(err, errlen, "no memory type for imported VRAM");
        return false;
    }
    ai.memoryTypeIndex = (uint32_t)mt;
    if (vkAllocateMemory(vk->dev, &ai, NULL, &out->mem) != VK_SUCCESS ||
        vkBindBufferMemory(vk->dev, out->buf, out->mem, 0) != VK_SUCCESS) {
        set_err(err, errlen, "host memory import failed");
        return false;
    }
    out->size = size;
    out->map = ptr;
    return true;
}

/* ------------------------------------------------------------------ */
/* Shaders                                                             */
/* ------------------------------------------------------------------ */

static VkShaderModule spv_module(EgVk *vk, const uint32_t *words, size_t bytes,
                                 char *err, size_t errlen)
{
    VkShaderModuleCreateInfo ci = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = bytes, .pCode = words,
    };
    VkShaderModule m = VK_NULL_HANDLE;

    if (vkCreateShaderModule(vk->dev, &ci, NULL, &m) != VK_SUCCESS) {
        set_err(err, errlen, "vkCreateShaderModule failed");
        m = VK_NULL_HANDLE;
    }
    return m;
}

/* GLSL -> SPIR-V (malloc'd), NULL with a reason on failure */
static uint32_t *glsl_to_spv(EgVk *vk, const char *src, shaderc_shader_kind kind,
                             size_t *bytes, char *err, size_t errlen)
{
    shaderc_compilation_result_t r =
        shaderc_compile_into_spv(vk->sc, src, strlen(src), kind, "evergreen",
                                 "main", vk->sco);
    uint32_t *words = NULL;

    if (shaderc_result_get_compilation_status(r) != shaderc_compilation_status_success) {
        set_err(err, errlen, "%s", shaderc_result_get_error_message(r));
        if (getenv("EG_VK_DUMP_FAILED")) {
            FILE *f = fopen(getenv("EG_VK_DUMP_FAILED"), "w");
            if (f) {
                fputs(src, f);
                fprintf(f, "\n/* %s */\n", shaderc_result_get_error_message(r));
                fclose(f);
            }
        }
    } else {
        *bytes = shaderc_result_get_length(r);
        words = malloc(*bytes);
        memcpy(words, shaderc_result_get_bytes(r), *bytes);
    }
    shaderc_result_release(r);
    return words;
}

static VkShaderModule compile(EgVk *vk, const char *src, shaderc_shader_kind kind,
                              char *err, size_t errlen)
{
    size_t bytes;
    uint32_t *words = glsl_to_spv(vk, src, kind, &bytes, err, errlen);
    VkShaderModule m = VK_NULL_HANDLE;

    if (words) {
        m = spv_module(vk, words, bytes, err, errlen);
        free(words);
    }
    return m;
}

#define EVK_STR2(x) #x
#define EVK_STR(x) EVK_STR2(x)
#define EVK_MAX_PARAMS_STR EVK_STR(EVK_MAX_PARAMS)

static const char *vert_src =
    "#version 460\n"
    "struct Vtx { vec4 pos; uvec4 misc; vec4 p[" EVK_MAX_PARAMS_STR "]; };\n"
    "layout(std430, set = 0, binding = 2) readonly buffer VtxBuf { Vtx verts[]; };\n"
    "layout(push_constant) uniform PC { uint draw; } pc;\n"
    "layout(location = 0) out vec4 v_p[" EVK_MAX_PARAMS_STR "];\n"
    "layout(location = " EVK_MAX_PARAMS_STR ") flat out uint v_front;\n"
    "layout(location = " EVK_MAX_PARAMS_STR " + 1) noperspective out float v_z;\n"
    "void main() {\n"
    "  Vtx v = verts[gl_VertexIndex];\n"
    "  float w = max(v.pos.w, 1e-20);\n"
    "  gl_Position = vec4((v.pos.x * (2.0 / 8192.0) - 1.0) * w,\n"
    "                     (v.pos.y * (2.0 / 8192.0) - 1.0) * w, 0.5 * w, w);\n"
    "  for (int i = 0; i < " EVK_MAX_PARAMS_STR "; i++) v_p[i] = v.p[i];\n"
    "  v_front = v.misc.x;\n"
    "  v_z = v.pos.z;\n"
    "}\n";

/*
 * EVK_NO_PERVERTEX: the PS reads the corners from verts[] itself.  v_base is
 * the triangle's first vertex (the draw's vertices are triangles from
 * pc.first on).
 */
static const char *vert_src_nopv =
    "#version 460\n"
    "struct Vtx { vec4 pos; uvec4 misc; vec4 p[" EVK_MAX_PARAMS_STR "]; };\n"
    "layout(std430, set = 0, binding = 2) readonly buffer VtxBuf { Vtx verts[]; };\n"
    "layout(push_constant) uniform PC { uint draw; uint first; uint outbase; uint nidx; } pc;\n"
    "layout(location = " EVK_MAX_PARAMS_STR ") flat out uint v_front;\n"
    "layout(location = " EVK_MAX_PARAMS_STR " + 1) noperspective out float v_z;\n"
    "layout(location = " EVK_MAX_PARAMS_STR " + 2) flat out uint v_base;\n"
    "void main() {\n"
    "  Vtx v = verts[gl_VertexIndex];\n"
    "  float w = max(v.pos.w, 1e-20);\n"
    "  gl_Position = vec4((v.pos.x * (2.0 / 8192.0) - 1.0) * w,\n"
    "                     (v.pos.y * (2.0 / 8192.0) - 1.0) * w, 0.5 * w, w);\n"
    "  uint c = uint(gl_VertexIndex) - pc.first;\n"
    "  v_base = uint(gl_VertexIndex) - c % 3u;\n"
    "  v_front = v.misc.x;\n"
    "  v_z = v.pos.z;\n"
    "}\n";

/* "#version", the EVK_* defines, the template with the program spliced in */
static char *frag_source(EgVk *vk, const char *ps, const char *defs)
{
    const char *tpl = evk_frag_template(), *mark = strstr(tpl, "@PS_PROGRAM@");
    size_t np = strlen(vk->frag_prefix), nd = defs ? strlen(defs) : 0, na, nps = strlen(ps), nb;
    char *src;

    if (!mark) {
        return NULL;
    }
    na = mark - tpl;
    nb = strlen(mark + 12);
    src = malloc(np + nd + na + nps + nb + 1);
    memcpy(src, vk->frag_prefix, np);
    memcpy(src + np, defs, nd);
    memcpy(src + np + nd, tpl, na);
    memcpy(src + np + nd + na, ps, nps);
    memcpy(src + np + nd + na + nps, mark + 12, nb + 1);
    return src;
}

/* the vertex shader of a GPU VS draw: the template as EVK_VS, with vs_run() */
static char *vert_source(EgVk *vk, const char *vs)
{
    static const char def[] = "#define EVK_VS 1\n";
    char *f = frag_source(vk, vs, def);
    return f;
}

static char *frag_prefix(void)
{
    /* every EVK_* constant the template and the translated programs use */
    static const struct { const char *name; uint32_t v; } defs[] = {
#define D(x) { #x, (uint32_t)(x) }
        D(EVK_MAX_PARAMS), D(EVK_MAX_TEX), D(EVK_MAX_SMP), D(EVK_MAX_VBUF), D(EVK_MAX_KC),
        D(EVK_MISC_WORDS),
        D(EVK_TF_VALID), D(EVK_TF_GART), D(EVK_TF_INT), D(EVK_TF_DEGAMMA), D(EVK_TF_SRF),
        D(EVK_TF_BC),
        D(EVK_SF_ENABLED), D(EVK_SF_GART), D(EVK_SF_FLOAT), D(EVK_SF_BLEND),
        D(EVK_SF_BLEND_CLAMP),
        D(EVK_M_CB_MODE), D(EVK_M_ROP), D(EVK_M_COLOR_MASK), D(EVK_M_ALPHA_TEST),
        D(EVK_M_ALPHA_FUNC), D(EVK_M_ALPHA_REF), D(EVK_M_Z_FMT), D(EVK_M_Z_ENABLE),
        D(EVK_M_Z_WRITE), D(EVK_M_Z_FUNC), D(EVK_M_S_VALID), D(EVK_M_S_ENABLE),
        D(EVK_M_DB_CTL), D(EVK_M_SREF0), D(EVK_M_SREF1), D(EVK_M_Z_CLEAR),
        D(EVK_M_Z_CLEAR_VAL), D(EVK_M_S_CLEAR), D(EVK_M_S_CLEAR_VAL), D(EVK_M_SC_X0),
        D(EVK_M_SC_Y0), D(EVK_M_SC_X1), D(EVK_M_SC_Y1), D(EVK_M_CLIP_RULE), D(EVK_M_CR0),
        D(EVK_M_BARYC), D(EVK_M_IN0), D(EVK_M_IN1), D(EVK_M_PIX_CENTER),
        D(EVK_M_ZPASS_SLOT), D(EVK_M_BOOLC),
        D(EVK_MAX_VS_VBUF), D(EVK_VP), D(EVK_M_FLATMASK), D(EVK_M_GPU_VS), D(EVK_M_VS_BOOLC),
        D(EVK_M_VTE), D(EVK_M_CLIP), D(EVK_M_VPORT), D(EVK_M_WIN_OFS), D(EVK_M_NINTERP),
        D(EVK_M_ISRC), D(EVK_M_EARLY_Z), D(EVK_M_CS),
#undef D
    };
    char buf[4096];
    size_t i, n = snprintf(buf, sizeof(buf), "#version 460\n");

    for (i = 0; i < sizeof(defs) / sizeof(defs[0]) && n < sizeof(buf); i++) {
        n += snprintf(buf + n, sizeof(buf) - n, "#define %s %uu\n", defs[i].name, defs[i].v);
    }
    /* EG_VK_DEFS: extra lines for experiments, ';' for a new line */
    if (getenv("EG_VK_DEFS") && n < sizeof(buf)) {
        const char *e = getenv("EG_VK_DEFS");
        for (; *e && n < sizeof(buf) - 2; e++) {
            buf[n++] = *e == ';' ? '\n' : *e;
        }
        buf[n++] = '\n';
        buf[n] = 0;
    }
    return strdup(buf);
}

static char *cache_path(EgVk *vk, uint64_t hash)
{
    size_t n = strlen(vk->cache_dir) + 48;
    char *path = malloc(n);

    snprintf(path, n, "%s/%016llx-%016llx.spv", vk->cache_dir,
             (unsigned long long)vk->tpl_hash, (unsigned long long)hash);
    return path;
}

/* write via a temporary file so a reader never sees a partial file */
static void cache_store(EgVk *vk, uint64_t hash, const uint32_t *words, size_t bytes)
{
    char *path = cache_path(vk, hash);
    size_t n = strlen(path) + 8;
    char *tmp = malloc(n);
    FILE *f;

    snprintf(tmp, n, "%s.tmp", path);
    f = fopen(tmp, "wb");
    if (f) {
        bool ok = fwrite(words, 1, bytes, f) == bytes;
        if (fclose(f) == 0 && ok) {
            rename(tmp, path);
        } else {
            remove(tmp);
        }
    }
    free(tmp);
    free(path);
}

static uint32_t *cache_load(EgVk *vk, uint64_t hash, size_t *bytes)
{
    char *path = cache_path(vk, hash);
    FILE *f = fopen(path, "rb");
    uint32_t *words = NULL;
    long len;

    free(path);
    if (!f) {
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) == 0 && (len = ftell(f)) > 20 && len % 4 == 0 &&
        fseek(f, 0, SEEK_SET) == 0) {
        words = malloc(len);
        if (fread(words, 1, len, f) != (size_t)len || words[0] != 0x07230203u) {
            free(words);                /* short read or not SPIR-V */
            words = NULL;
        } else {
            *bytes = len;
        }
    }
    fclose(f);
    return words;
}

/* SPIR-V (cached or compiled from p->src) + pipeline; consumes src/spv */
static VkPipeline make_pipeline(EgVk *vk, EvkPipe *p, char *err, size_t errlen)
{
    VkShaderModule frag = VK_NULL_HANDLE, vert = VK_NULL_HANDLE;
    VkPipeline pipe = VK_NULL_HANDLE;
    uint32_t *words = p->spv;
    size_t bytes = p->spv_bytes;

    p->spv = NULL;
    if (p->compute) {
        /* SPIR-V from the disk cache, else compiled (and stored) */
        if (!words && vk->cache_dir) {
            words = cache_load(vk, p->hash, &bytes);
        }
        if (!words && p->src) {
            words = glsl_to_spv(vk, p->src, shaderc_compute_shader, &bytes, err, errlen);
            if (words && vk->cache_dir) {
                cache_store(vk, p->hash, words, bytes);
            }
        }
        free(p->src);
        p->src = NULL;
        frag = words ? spv_module(vk, words, bytes, err, errlen) : VK_NULL_HANDLE;
        free(words);
        if (!frag) {
            return VK_NULL_HANDLE;
        }
        if (vkCreateComputePipelines(vk->dev, vk->pcache, 1, &(VkComputePipelineCreateInfo) {
                .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                .stage = { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                           .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = frag,
                           .pName = "main" },
                .layout = vk->cslayout }, NULL, &pipe) != VK_SUCCESS) {
            set_err(err, errlen, "vkCreateComputePipelines failed");
            pipe = VK_NULL_HANDLE;
        }
        vkDestroyShaderModule(vk->dev, frag, NULL);
        __atomic_add_fetch(&vk->stat_pipes, 1, __ATOMIC_RELAXED);
        return pipe;
    }
    if (!words && p->src) {
        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        words = glsl_to_spv(vk, p->src, shaderc_fragment_shader, &bytes, err, errlen);
        clock_gettime(CLOCK_MONOTONIC, &t1);
        if (getenv("EG_VK_TIMING")) {
            fprintf(stderr, "eg-vk:   glsl->spirv %.1f ms (%zu bytes of GLSL)\n",
                    (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6,
                    strlen(p->src));
        }
        if (getenv("EG_VK_SAVE_GLSL")) {
            FILE *f = fopen(getenv("EG_VK_SAVE_GLSL"), "w");
            if (f) { fputs(p->src, f); fclose(f); }
        }
        if (words && vk->cache_dir) {
            cache_store(vk, p->hash, words, bytes);
        }
    }
    free(p->src);
    p->src = NULL;
    if (words) {
        frag = spv_module(vk, words, bytes, err, errlen);
        free(words);
    }
    if (!frag) {
        free(p->vsrc);
        p->vsrc = NULL;
        return VK_NULL_HANDLE;
    }
    if (p->vsrc) {
        /* the GPU VS: SPIR-V cached under the pipeline key with a tag */
        uint64_t vkey = p->hash ^ 0x7673000000000000ull;
        size_t vb = 0;
        uint32_t *vw = vk->cache_dir ? cache_load(vk, vkey, &vb) : NULL;
        if (!vw) {
            vw = glsl_to_spv(vk, p->vsrc, shaderc_vertex_shader, &vb, err, errlen);
            if (vw && vk->cache_dir) {
                cache_store(vk, vkey, vw, vb);
            }
        }
        free(p->vsrc);
        p->vsrc = NULL;
        vert = vw ? spv_module(vk, vw, vb, err, errlen) : VK_NULL_HANDLE;
        free(vw);
        if (!vert) {
            vkDestroyShaderModule(vk->dev, frag, NULL);
            return VK_NULL_HANDLE;
        }
    }

    {
        VkPipelineShaderStageCreateInfo st[2] = {
            { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
              .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vert ? vert : vk->vert,
              .pName = "main" },
            { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
              .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = frag, .pName = "main" },
        };
        VkPipelineVertexInputStateCreateInfo vi = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        };
        VkPipelineInputAssemblyStateCreateInfo ia = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
            .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
        };
        VkPipelineViewportStateCreateInfo vp = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
            .viewportCount = 1, .scissorCount = 1,
        };
        VkPipelineRasterizationStateCreateInfo rs = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
            .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE,
            .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE, .lineWidth = 1.0f,
        };
        VkPipelineMultisampleStateCreateInfo ms = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
            .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
        };
        VkDynamicState dyn[4] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR,
                                  VK_DYNAMIC_STATE_CULL_MODE, VK_DYNAMIC_STATE_FRONT_FACE };
        VkPipelineDynamicStateCreateInfo ds = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
            .dynamicStateCount = 4, .pDynamicStates = dyn,
        };
        VkPipelineRenderingCreateInfo ri = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
        };
        VkGraphicsPipelineCreateInfo gi = {
            .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
            .pNext = &ri, .stageCount = 2, .pStages = st,
            .pVertexInputState = &vi, .pInputAssemblyState = &ia,
            .pViewportState = &vp, .pRasterizationState = &rs,
            .pMultisampleState = &ms, .pDynamicState = &ds,
            .layout = vk->layout,
        };
        if (vkCreateGraphicsPipelines(vk->dev, vk->pcache, 1, &gi, NULL, &pipe) != VK_SUCCESS) {
            set_err(err, errlen, "vkCreateGraphicsPipelines failed");
            pipe = VK_NULL_HANDLE;
            /* EG_VK_DUMP_FAILED=file: also the sources of a pipeline the driver
             * rejects (file.frag, file.vert) */
            if (getenv("EG_VK_DUMP_FAILED")) {
                char fn[1024], vn[1024];
                FILE *f;

                snprintf(fn, sizeof(fn), "%s.frag", getenv("EG_VK_DUMP_FAILED"));
                snprintf(vn, sizeof(vn), "%s.vert", getenv("EG_VK_DUMP_FAILED"));
                f = fopen(fn, "w");
                if (f) {
                    fputs(p->src, f);
                    fclose(f);
                }
                f = fopen(vn, "w");
                if (f) {
                    fputs(p->vsrc ? p->vsrc : (vk->no_pervertex ? vert_src_nopv : vert_src), f);
                    fclose(f);
                }
            }
        }
    }
    vkDestroyShaderModule(vk->dev, frag, NULL);
    if (vert) {
        vkDestroyShaderModule(vk->dev, vert, NULL);
    }
    __atomic_add_fetch(&vk->stat_pipes, 1, __ATOMIC_RELAXED);
    return pipe;
}

static void pcache_save(EgVk *vk);
static bool make_copy_back(EgVk *vk);
static void *waiter_thread(void *opaque);

static void compile_entry(EgVk *vk, EvkPipe *p)
{
    char err[1024];
    struct timespec t0, t1;
    VkPipeline pipe;

    clock_gettime(CLOCK_MONOTONIC, &t0);
    pipe = make_pipeline(vk, p, err, sizeof(err));
    clock_gettime(CLOCK_MONOTONIC, &t1);
    if (getenv("EG_VK_TIMING")) {
        fprintf(stderr, "eg-vk: pipeline %016llx %.1f ms%s%s\n",
                (unsigned long long)p->hash,
                (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6,
                pipe ? "" : " failed: ", pipe ? "" : err);
    }
    p->pipe = pipe;
    __atomic_store_n(&p->state, pipe ? EVK_PIPE_READY : EVK_PIPE_FAILED,
                     __ATOMIC_RELEASE);
    if (pipe && vk->cache_dir) {
        /*
         * Save the pipeline cache now and then, from this worker:
         * vkGetPipelineCacheData waits for pipeline creations that use the
         * cache, which once kept the CP (and a vCPU) blocked for a whole
         * 900 ms compile.  QEMU may exit without cleanup, so save as we go.
         */
        int64_t last = __atomic_load_n(&vk->pcache_saved, __ATOMIC_RELAXED);
        __atomic_add_fetch(&vk->new_pipes, 1, __ATOMIC_RELAXED);
        if (t1.tv_sec - last >= 10 &&
            __atomic_compare_exchange_n(&vk->pcache_saved, &last, (int64_t)t1.tv_sec,
                                        false, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
            __atomic_store_n(&vk->new_pipes, 0, __ATOMIC_RELAXED);
            pcache_save(vk);
        }
    }
}

static void *compiler_thread(void *opaque)
{
    EgVk *vk = opaque;

    for (;;) {
        EvkPipe *p;
        pthread_mutex_lock(&vk->qlock);
        while (!vk->quit && !vk->qhead) {
            pthread_cond_wait(&vk->qcond, &vk->qlock);
        }
        if (vk->quit) {
            pthread_mutex_unlock(&vk->qlock);
            return NULL;
        }
        p = vk->qhead;
        vk->qhead = p->qnext;
        if (!vk->qhead) {
            vk->qtail = NULL;
        }
        pthread_mutex_unlock(&vk->qlock);
        compile_entry(vk, p);
    }
}

/*
 * Wait a bounded time for a pending pipeline before falling back to the
 * CPU path.  A compile (GLSL -> SPIR-V plus pipeline creation) takes 10-100
 * ms, while the CPU fallback for the same draw took up to 700 ms for large
 * Core Image or game passes, so waiting wins by far.  The device releases
 * its big lock while we sleep (wait_fn), so other vCPUs and devices keep
 * running; only the vCPU that kicked the CP waits.
 */
static int compile_wait_ms = -1, compile_budget_ms = -1;

/*
 * Several new programs in one ring segment must not add up past xnu's lock
 * timeout (1 s with slto_us): the waits of a batch (one CP run) share
 * EG_VK_COMPILE_BUDGET_MS (400); one draw waits at most
 * EG_VK_COMPILE_WAIT_MS (250).
 */
static VkPipeline wait_pipeline(EgVk *vk, EvkPipe *p, char *err, size_t errlen)
{
    struct timespec t0, now, nap = { 0, 200 * 1000 };
    int64_t limit, waited = 0;
    int state;

    if (compile_wait_ms < 0) {
        const char *e = getenv("EG_VK_COMPILE_WAIT_MS");
        const char *b = getenv("EG_VK_COMPILE_BUDGET_MS");
        /* short: the CPU path draws while the BQL is held, and a vCPU kept
         * out of the guest for 250 ms panics an SMP Snow Leopard */
        compile_wait_ms = e ? atoi(e) : 30;
        compile_budget_ms = b ? atoi(b) : 60;
    }
    limit = (int64_t)compile_wait_ms * 1000000;
    if (limit > (int64_t)compile_budget_ms * 1000000 - vk->wait_ns) {
        limit = (int64_t)compile_budget_ms * 1000000 - vk->wait_ns;
    }
    if (limit <= 0) {
        set_err(err, errlen, "compiling");
        return VK_NULL_HANDLE;
    }
    if (vk->wait_fn) {
        vk->wait_fn(vk->opaque, true);
    }
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (;;) {
        state = __atomic_load_n(&p->state, __ATOMIC_ACQUIRE);
        if (state != EVK_PIPE_PENDING) {
            break;
        }
        clock_gettime(CLOCK_MONOTONIC, &now);
        waited = (now.tv_sec - t0.tv_sec) * 1000000000ll + (now.tv_nsec - t0.tv_nsec);
        if (waited >= limit) {
            break;
        }
        nanosleep(&nap, NULL);
    }
    if (vk->wait_fn) {
        vk->wait_fn(vk->opaque, false);
    }
    vk->wait_ns += waited;
    if (state == EVK_PIPE_PENDING) {
        set_err(err, errlen, "compiling");
        return VK_NULL_HANDLE;
    }
    if (state == EVK_PIPE_READY) {
        return p->pipe;
    }
    set_err(err, errlen, "fragment program failed to compile");
    return VK_NULL_HANDLE;
}

/* Look up (or start compiling) the pipeline for a fragment program.
 * VK_NULL_HANDLE with a reason when it is not usable (yet). */
static VkPipeline get_pipeline(EgVk *vk, const char *ps, const char *vs,
                               uint64_t hash, char *err, size_t errlen)
{
    EvkPipe **head = &vk->pipes[hash % EVK_PIPE_BUCKETS], *p;

    for (p = *head; p; p = p->next) {
        if (p->hash == hash) {
            switch (__atomic_load_n(&p->state, __ATOMIC_ACQUIRE)) {
            case EVK_PIPE_READY:
                return p->pipe;
            case EVK_PIPE_PENDING:
                return wait_pipeline(vk, p, err, errlen);
            default:
                set_err(err, errlen, "fragment program failed to compile");
                return VK_NULL_HANDLE;
            }
        }
    }
    p = calloc(1, sizeof(*p));
    p->hash = hash;
    p->state = EVK_PIPE_PENDING;
    p->src = ps ? frag_source(vk, ps, NULL) : NULL;
    p->vsrc = vs ? vert_source(vk, vs) : NULL;
    /* the prewarm list: generic pipelines with the CPU-path vertex shader only */
    if (vk->order && p->src && !p->vsrc) {
        fprintf(vk->order, "%016llx\n", (unsigned long long)hash);
        fflush(vk->order);
    }
    p->next = *head;
    *head = p;
    if (!p->src) {
        p->state = EVK_PIPE_FAILED;
        set_err(err, errlen, "fragment program not translatable");
        return VK_NULL_HANDLE;
    }
    if (vk->sync_compile || !vk->ncompilers) {
        compile_entry(vk, p);
        if (p->state == EVK_PIPE_READY) {
            return p->pipe;
        }
        set_err(err, errlen, "fragment program failed to compile");
        return VK_NULL_HANDLE;
    }
    pthread_mutex_lock(&vk->qlock);
    if (vk->qtail) {
        vk->qtail->qnext = p;
    } else {
        vk->qhead = p;
    }
    vk->qtail = p;
    pthread_cond_signal(&vk->qcond);
    pthread_mutex_unlock(&vk->qlock);
    return wait_pipeline(vk, p, err, errlen);
}

/* ------------------------------------------------------------------ */
/* Device setup                                                        */
/* ------------------------------------------------------------------ */

static bool has_ext(VkPhysicalDevice pd, const char *name)
{
    uint32_t n = 0, i;
    VkExtensionProperties *e;
    bool found = false;

    vkEnumerateDeviceExtensionProperties(pd, NULL, &n, NULL);
    e = calloc(n ? n : 1, sizeof(*e));
    vkEnumerateDeviceExtensionProperties(pd, NULL, &n, e);
    for (i = 0; i < n; i++) {
        if (!strcmp(e[i].extensionName, name)) {
            found = true;
            break;
        }
    }
    free(e);
    return found;
}

static bool pick_device(EgVk *vk, char *err, size_t errlen)
{
    uint32_t n = 0, i, best = UINT32_MAX;
    VkPhysicalDevice *pd;
    const char *want = getenv("EG_VK_DEVICE");
    int best_score = -1;

    vkEnumeratePhysicalDevices(vk->inst, &n, NULL);
    if (!n) {
        set_err(err, errlen, "no Vulkan devices");
        return false;
    }
    pd = calloc(n, sizeof(*pd));
    vkEnumeratePhysicalDevices(vk->inst, &n, pd);
    for (i = 0; i < n; i++) {
        VkPhysicalDeviceProperties pr;
        VkPhysicalDeviceFragmentShaderBarycentricFeaturesKHR bary = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADER_BARYCENTRIC_FEATURES_KHR,
        };
        VkPhysicalDeviceFragmentShaderInterlockFeaturesEXT il = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADER_INTERLOCK_FEATURES_EXT,
            .pNext = &bary,
        };
        VkPhysicalDeviceFeatures2 f2 = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &il,
        };
        VkPhysicalDeviceSubgroupProperties sg = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES,
        };
        VkPhysicalDeviceProperties2 p2 = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &sg,
        };
        int score;

        vkGetPhysicalDeviceProperties(pd[i], &pr);
        vkGetPhysicalDeviceProperties2(pd[i], &p2);
        vkGetPhysicalDeviceFeatures2(pd[i], &f2);
        if (pr.apiVersion < VK_API_VERSION_1_3 ||
            !has_ext(pd[i], VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME) ||
            !has_ext(pd[i], VK_EXT_FRAGMENT_SHADER_INTERLOCK_EXTENSION_NAME) ||
            !has_ext(pd[i], VK_KHR_FRAGMENT_SHADER_BARYCENTRIC_EXTENSION_NAME) ||
            !il.fragmentShaderPixelInterlock || !bary.fragmentShaderBarycentric ||
            !(sg.supportedStages & VK_SHADER_STAGE_FRAGMENT_BIT) ||
            !(sg.supportedOperations & VK_SUBGROUP_FEATURE_QUAD_BIT) ||
            !f2.features.fragmentStoresAndAtomics) {
            continue;
        }
        score = pr.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 3 :
                pr.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 2 : 1;
        /* float64 is optional: without it the template uses exact 32-bit
         * code (EVK_HAS_F64), so prefer a device that has it */
        score = score * 2 + (f2.features.shaderFloat64 ? 1 : 0);
        if (want && (uint32_t)atoi(want) == i) {
            score = 100;
        }
        if (score > best_score) {
            best_score = score;
            best = i;
            vk->has_f64 = f2.features.shaderFloat64;
        }
    }
    if (best == UINT32_MAX) {
        free(pd);
        set_err(err, errlen, "no Vulkan 1.3 device with external_memory_host, "
                "pixel interlock, fragment barycentrics, quad subgroup operations "
                "and fragment stores");
        return false;
    }
    vk->phys = pd[best];
    free(pd);
    vk->portability = has_ext(vk->phys, "VK_KHR_portability_subset");
    /* EG_VK_PERVERTEX=0/1 overrides the choice (to compare the two paths) */
    vk->no_pervertex = getenv("EG_VK_PERVERTEX") ? !atoi(getenv("EG_VK_PERVERTEX"))
                                                 : vk->portability;
    return true;
}

EgVk *eg_vk_create(void *vram, uint64_t vram_size, EgVkDirtyFn dirty,
                       void *opaque, char *err, size_t errlen)
{
    EgVk *vk = calloc(1, sizeof(*vk));
    VkApplicationInfo app = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "qemu-evergreen", .apiVersion = VK_API_VERSION_1_3,
    };
    /* MoltenVK (macOS) is a portability driver: the loader lists it only
     * when the instance asks for portability enumeration */
    static const char *iexts[] = { "VK_KHR_portability_enumeration" };
    VkInstanceCreateInfo ici = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app,
    };
    uint32_t nq = 0, i;
    VkQueueFamilyProperties *qp;
    float prio = 1.0f;

    vk->dirty_fn = dirty;
    vk->opaque = opaque;
    vk->vram_size = vram_size;
#ifdef __APPLE__
    /* Metal fast math may reassociate float arithmetic and drops the NaN
     * and Inf semantics that the shader translation relies on */
    setenv("MVK_CONFIG_FAST_MATH_ENABLED", "0", 0);
#endif
    {
        uint32_t ne = 0, k;
        VkExtensionProperties *ie;

        vkEnumerateInstanceExtensionProperties(NULL, &ne, NULL);
        ie = calloc(ne ? ne : 1, sizeof(*ie));
        vkEnumerateInstanceExtensionProperties(NULL, &ne, ie);
        for (k = 0; k < ne; k++) {
            if (!strcmp(ie[k].extensionName, iexts[0])) {
                ici.flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
                ici.enabledExtensionCount = 1;
                ici.ppEnabledExtensionNames = iexts;
            }
        }
        free(ie);
    }
    if (vkCreateInstance(&ici, NULL, &vk->inst) != VK_SUCCESS) {
        set_err(err, errlen, "vkCreateInstance failed");
        free(vk);
        return NULL;
    }
    if (!pick_device(vk, err, errlen)) {
        goto fail;
    }
    vkGetPhysicalDeviceQueueFamilyProperties(vk->phys, &nq, NULL);
    qp = calloc(nq, sizeof(*qp));
    vkGetPhysicalDeviceQueueFamilyProperties(vk->phys, &nq, qp);
    vk->qfam = UINT32_MAX;
    for (i = 0; i < nq; i++) {
        if (qp[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            vk->qfam = i;
            break;
        }
    }
    free(qp);
    if (vk->qfam == UINT32_MAX) {
        set_err(err, errlen, "no graphics queue");
        goto fail;
    }
    {
        static const char *exts[] = {
            VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME,
            VK_EXT_FRAGMENT_SHADER_INTERLOCK_EXTENSION_NAME,
            VK_KHR_FRAGMENT_SHADER_BARYCENTRIC_EXTENSION_NAME,
            "VK_KHR_portability_subset",    /* must be enabled where offered */
        };
        VkPhysicalDeviceFragmentShaderBarycentricFeaturesKHR bary = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADER_BARYCENTRIC_FEATURES_KHR,
            .fragmentShaderBarycentric = VK_TRUE,
        };
        VkPhysicalDeviceFragmentShaderInterlockFeaturesEXT il = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADER_INTERLOCK_FEATURES_EXT,
            .pNext = &bary, .fragmentShaderPixelInterlock = VK_TRUE,
        };
        VkPhysicalDeviceVulkan13Features f13 = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
            .pNext = &il, .dynamicRendering = VK_TRUE,
        };
        VkPhysicalDeviceFeatures2 f2 = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &f13,
            .features = { .fragmentStoresAndAtomics = VK_TRUE, .shaderFloat64 = vk->has_f64,
                          .vertexPipelineStoresAndAtomics = vk->no_pervertex },
        };
        VkDeviceQueueCreateInfo qi = {
            .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
            .queueFamilyIndex = vk->qfam, .queueCount = 1, .pQueuePriorities = &prio,
        };
        VkDeviceCreateInfo di = {
            .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &f2,
            .queueCreateInfoCount = 1, .pQueueCreateInfos = &qi,
            .enabledExtensionCount = vk->portability ? 4 : 3, .ppEnabledExtensionNames = exts,
        };
        if (vkCreateDevice(vk->phys, &di, NULL, &vk->dev) != VK_SUCCESS) {
            set_err(err, errlen, "vkCreateDevice failed");
            goto fail;
        }
    }
    vkGetDeviceQueue(vk->dev, vk->qfam, 0, &vk->queue);

    if (!import_host(vk, &vk->vram, vram, vram_size, err, errlen)) {
        goto fail;
    }
    if (!host_buf(vk, &vk->draws, (VkDeviceSize)EVK_FRAMES * EVK_DRAW_SLOTS * sizeof(EvkDraw),
                  VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                  err, errlen) ||
        !host_buf(vk, &vk->idxbuf, (VkDeviceSize)EVK_FRAMES * EVK_IDX_SLOTS * 4,
                  VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                  err, errlen) ||
        !host_buf(vk, &vk->vsout, vk->no_pervertex ?
                  (VkDeviceSize)EVK_FRAMES * EVK_VSOUT_SLOTS * 16 * EVK_MAX_PARAMS : 4096,
                  VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, err, errlen) ||
        !host_buf(vk, &vk->verts,
                  (VkDeviceSize)EVK_FRAMES * EVK_VERT_SLOTS * sizeof(EvkVertex),
                  VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, err, errlen) ||
        !host_buf(vk, &vk->counts, 4096, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                  err, errlen) ||
        !host_buf(vk, &vk->gart, 4096, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                  err, errlen)) {
        goto fail;
    }
    memset(vk->counts.map, 0, 4096);
    if (!getenv("EG_VK_DRAWS_HOST")) {
        VkBufferCreateInfo bi = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = vk->draws.size,
            .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        };
        VkMemoryRequirements mr;
        VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        int mt;
        if (vkCreateBuffer(vk->dev, &bi, NULL, &vk->draws_dev.buf) == VK_SUCCESS) {
            vkGetBufferMemoryRequirements(vk->dev, vk->draws_dev.buf, &mr);
            mt = find_mem_type(vk, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            ai.allocationSize = mr.size;
            ai.memoryTypeIndex = (uint32_t)mt;
            if (mt >= 0 &&
                vkAllocateMemory(vk->dev, &ai, NULL, &vk->draws_dev.mem) == VK_SUCCESS &&
                vkBindBufferMemory(vk->dev, vk->draws_dev.buf, vk->draws_dev.mem, 0) ==
                    VK_SUCCESS) {
                vk->draws_dev.size = vk->draws.size;
                vk->draws_local = true;
            }
        }
    }
    /* the device-local mirror needs every CPU write to VRAM reported
     * (eg_vk_upload); opt in with EG_VK_MIRROR=1 until the device does */
    if (!getenv("EG_VK_NO_MIRROR")) {
        VkBufferCreateInfo bi = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = vram_size,
            .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                     VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        };
        VkMemoryRequirements mr;
        VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        int mt;
        if (vkCreateBuffer(vk->dev, &bi, NULL, &vk->vram_dev.buf) == VK_SUCCESS) {
            vkGetBufferMemoryRequirements(vk->dev, vk->vram_dev.buf, &mr);
            mt = find_mem_type(vk, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            ai.allocationSize = mr.size;
            ai.memoryTypeIndex = (uint32_t)mt;
            if (mt >= 0 && vkAllocateMemory(vk->dev, &ai, NULL, &vk->vram_dev.mem) == VK_SUCCESS &&
                vkBindBufferMemory(vk->dev, vk->vram_dev.buf, vk->vram_dev.mem, 0) == VK_SUCCESS) {
                vk->vram_dev.size = vram_size;
                vk->mirror = true;
                vk->stale = calloc(((vram_size >> EVK_PAGE_SHIFT) + 63) / 64, 8);
                vk->batch_wr = calloc(((vram_size >> EVK_PAGE_SHIFT) + 63) / 64, 8);
            }
        }
        if (vk->mirror) {
            VkDeviceSize bsz = ((vram_size >> EVK_PAGE_SHIFT) + 31) / 32 * 4;
            bi.size = bsz;
            bi.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
            if (vkCreateBuffer(vk->dev, &bi, NULL, &vk->dirty_bits.buf) == VK_SUCCESS) {
                vkGetBufferMemoryRequirements(vk->dev, vk->dirty_bits.buf, &mr);
                mt = find_mem_type(vk, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
                ai.allocationSize = mr.size;
                ai.memoryTypeIndex = (uint32_t)mt;
                if (mt >= 0 &&
                    vkAllocateMemory(vk->dev, &ai, NULL, &vk->dirty_bits.mem) == VK_SUCCESS &&
                    vkBindBufferMemory(vk->dev, vk->dirty_bits.buf, vk->dirty_bits.mem, 0) ==
                        VK_SUCCESS) {
                    vk->dirty_bits.size = bsz;
                } else {
                    vkDestroyBuffer(vk->dev, vk->dirty_bits.buf, NULL);
                    vk->dirty_bits.buf = VK_NULL_HANDLE;
                }
            }
        }
    }

    {
        VkDescriptorSetLayoutBinding b[11];
        VkDescriptorSetLayoutCreateInfo li = {
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
            .bindingCount = 11, .pBindings = b,
        };
        VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 11 };
        VkDescriptorPoolCreateInfo pi = {
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
            .maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &ps,
        };
        VkPushConstantRange pcr = {
            VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16,
        };
        VkPipelineLayoutCreateInfo pli = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
            .setLayoutCount = 1, .pSetLayouts = &vk->dsl,
            .pushConstantRangeCount = 1, .pPushConstantRanges = &pcr,
        };
        VkDescriptorSetAllocateInfo ai = {
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
            .descriptorSetCount = 1, .pSetLayouts = &vk->dsl,
        };
        VkBuffer shader_vram = vk->mirror ? vk->vram_dev.buf : vk->vram.buf;
        VkDescriptorBufferInfo bi[11] = {
            { shader_vram, 0, VK_WHOLE_SIZE },
            { vk->draws_local ? vk->draws_dev.buf : vk->draws.buf, 0, VK_WHOLE_SIZE },
            { vk->verts.buf, 0, VK_WHOLE_SIZE }, { vk->counts.buf, 0, VK_WHOLE_SIZE },
            { vk->counts.buf, 0, VK_WHOLE_SIZE }, { vk->gart.buf, 0, VK_WHOLE_SIZE },
            { shader_vram, 0, VK_WHOLE_SIZE }, { vk->counts.buf, 0, VK_WHOLE_SIZE },
            /* without the bitmap the shaders do not mark (no EVK_MARK_DIRTY) */
            { vk->dirty_bits.buf ? vk->dirty_bits.buf : vk->counts.buf, 0, VK_WHOLE_SIZE },
            /* EVK_NO_PERVERTEX: the GPU VS outputs and indices of each corner */
            { vk->vsout.buf, 0, VK_WHOLE_SIZE }, { vk->idxbuf.buf, 0, VK_WHOLE_SIZE },
        };
        VkWriteDescriptorSet w[11];
        VkCommandPoolCreateInfo cpi = {
            .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
            .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
            .queueFamilyIndex = vk->qfam,
        };
        VkCommandBufferAllocateInfo cai = {
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1,
        };
        VkFenceCreateInfo fi = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
        VkPipelineCacheCreateInfo pci = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO,
        };

        for (i = 0; i < 11; i++) {
            b[i] = (VkDescriptorSetLayoutBinding) {
                .binding = i, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                .descriptorCount = 1,
                .stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT |
                              VK_SHADER_STAGE_COMPUTE_BIT,
            };
            w[i] = (VkWriteDescriptorSet) {
                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .dstBinding = i, .descriptorCount = 1,
                .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                .pBufferInfo = &bi[i],
            };
        }
        if (vkCreateDescriptorSetLayout(vk->dev, &li, NULL, &vk->dsl) != VK_SUCCESS ||
            vkCreateDescriptorPool(vk->dev, &pi, NULL, &vk->dpool) != VK_SUCCESS ||
            vkCreatePipelineLayout(vk->dev, &pli, NULL, &vk->layout) != VK_SUCCESS) {
            set_err(err, errlen, "descriptor/pipeline layout creation failed");
            goto fail;
        }
        {
            VkPushConstantRange cpcr = { VK_SHADER_STAGE_COMPUTE_BIT, 0, 16 };
            pli.pPushConstantRanges = &cpcr;
            if (vkCreatePipelineLayout(vk->dev, &pli, NULL, &vk->cslayout) != VK_SUCCESS) {
                set_err(err, errlen, "compute pipeline layout creation failed");
                goto fail;
            }
        }
        ai.descriptorPool = vk->dpool;
        if (vkAllocateDescriptorSets(vk->dev, &ai, &vk->dset) != VK_SUCCESS) {
            set_err(err, errlen, "descriptor set allocation failed");
            goto fail;
        }
        for (i = 0; i < 11; i++) {
            w[i].dstSet = vk->dset;
        }
        vkUpdateDescriptorSets(vk->dev, 11, w, 0, NULL);
        if (vkCreateCommandPool(vk->dev, &cpi, NULL, &vk->pool) != VK_SUCCESS) {
            set_err(err, errlen, "command pool creation failed");
            goto fail;
        }
        cai.commandPool = vk->pool;
        for (i = 0; i < EVK_FRAMES; i++) {
            if (vkAllocateCommandBuffers(vk->dev, &cai, &vk->cmds[i]) != VK_SUCCESS ||
                vkAllocateCommandBuffers(vk->dev, &cai, &vk->precmds[i]) != VK_SUCCESS ||
                vkCreateFence(vk->dev, &fi, NULL, &vk->fences[i]) != VK_SUCCESS) {
                set_err(err, errlen, "command buffer / fence creation failed");
                goto fail;
            }
        }
        if (vkAllocateCommandBuffers(vk->dev, &cai, &vk->upcmd) != VK_SUCCESS ||
            vkCreateFence(vk->dev, &fi, NULL, &vk->upfence) != VK_SUCCESS ||
            vkCreatePipelineCache(vk->dev, &pci, NULL, &vk->pcache) != VK_SUCCESS) {
            set_err(err, errlen, "command buffer / fence creation failed");
            goto fail;
        }
        vk->cmd = vk->cmds[0];
        vk->fence = vk->fences[0];
        for (i = 0; i < EVK_FRAMES; i++) {
            vk->fdirty_off[i] = calloc(EVK_MAX_DIRTY, sizeof(uint64_t));
            vk->fdirty_len[i] = calloc(EVK_MAX_DIRTY, sizeof(uint64_t));
        }
    }

    vk->sc = shaderc_compiler_initialize();
    vk->sco = shaderc_compile_options_initialize();
    shaderc_compile_options_set_target_env(vk->sco, shaderc_target_env_vulkan,
                                           shaderc_env_version_vulkan_1_3);
    shaderc_compile_options_set_optimization_level(vk->sco,
                                                   shaderc_optimization_level_zero);
    vk->frag_prefix = frag_prefix();
    if (vk->no_pervertex) {
        static const char def[] = "#define EVK_NO_PERVERTEX 1\n";
        size_t n = strlen(vk->frag_prefix);
        vk->frag_prefix = realloc(vk->frag_prefix, n + sizeof(def));
        memcpy(vk->frag_prefix + n, def, sizeof(def));
    }
    if (vk->has_f64) {
        static const char def[] = "#define EVK_HAS_F64 1\n";
        size_t n = strlen(vk->frag_prefix);
        vk->frag_prefix = realloc(vk->frag_prefix, n + sizeof(def));
        memcpy(vk->frag_prefix + n, def, sizeof(def));
    }
    if (vk->dirty_bits.buf) {
        static const char def[] = "#define EVK_MARK_DIRTY 1\n";
        size_t n = strlen(vk->frag_prefix);
        vk->frag_prefix = realloc(vk->frag_prefix, n + sizeof(def));
        memcpy(vk->frag_prefix + n, def, sizeof(def));
    }
    vk->vert = compile(vk, vk->no_pervertex ? vert_src_nopv : vert_src, shaderc_vertex_shader,
                       err, errlen);
    if (!vk->vert) {
        goto fail;
    }
    /* EG_VK_COPY_RANGES=1: copy back whole ranges (the pages are marked anyway) */
    if (vk->dirty_bits.buf && !getenv("EG_VK_COPY_RANGES") && !make_copy_back(vk)) {
        /* the shaders mark pages but nothing reads the marks: harmless */
        fprintf(stderr, "evergreen: no compute copy-back, copying whole ranges\n");
    }
    vk->early_z = getenv("EG_VK_NO_EARLY_Z") == NULL;
    pthread_mutex_init(&vk->qlock, NULL);
    pthread_cond_init(&vk->qcond, NULL);
    vk->sync_compile = getenv("EG_VK_SYNC_COMPILE") != NULL;
    vk->barrier_each = getenv("EG_VK_BARRIER_EACH") != NULL;
    vk->specialize = getenv("EG_VK_NO_SPECIALIZE") == NULL;
    vk->trace = getenv("EG_VK_TRACE") != NULL;
    pthread_mutex_init(&vk->dlock, NULL);
    pthread_cond_init(&vk->dcond, NULL);
    if (!getenv("EG_VK_NO_WAITER") &&
        pthread_create(&vk->waiter, NULL, waiter_thread, vk) == 0) {
        vk->waiter_on = true;
    }
    for (i = 0; i < EVK_COMPILERS && !vk->sync_compile; i++) {
        if (pthread_create(&vk->compilers[i], NULL, compiler_thread, vk) == 0) {
            vk->ncompilers++;
        }
    }
    {
        VkPhysicalDeviceProperties pr;
        VkQueryPoolCreateInfo qi = {
            .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
            .queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = 4 * EVK_FRAMES,
        };
        vkGetPhysicalDeviceProperties(vk->phys, &pr);
        fprintf(stderr, "evergreen: Vulkan backend on %s\n", pr.deviceName);
        vk->ts_period = pr.limits.timestampPeriod;
        if (getenv("EG_VK_DRAWSTAMPS") && pr.limits.timestampComputeAndGraphics) {
            VkQueryPoolCreateInfo di = qi;
            di.queryCount = EVK_FRAMES * (EVK_DRAW_SLOTS + 1);
            if (vkCreateQueryPool(vk->dev, &di, NULL, &vk->dsq) == VK_SUCCESS) {
                vk->dprog = calloc(EVK_FRAMES * EVK_DRAW_SLOTS, 8);
                vk->darea = calloc(EVK_FRAMES * EVK_DRAW_SLOTS, 4);
            } else {
                vk->dsq = VK_NULL_HANDLE;
            }
        }
        if (!getenv("EG_VK_TIMESTAMPS") || !pr.limits.timestampComputeAndGraphics ||
            vkCreateQueryPool(vk->dev, &qi, NULL, &vk->tsq) != VK_SUCCESS) {
            vk->tsq = VK_NULL_HANDLE;
        }
    }
    /*
     * The driver's first pipeline creation takes ~300 ms of one-time setup,
     * during which it also holds back queue submissions from other threads.
     * Pay it here, before the guest runs, instead of inside a CP run.
     */
    {
        static const char warm_src[] =
            "#version 450\nlayout(location = 0) out vec4 o;\nvoid main() { o = vec4(0); }\n";
        EvkPipe warm = { .src = strdup(warm_src) };
        char werr[256];
        VkPipeline wp = make_pipeline(vk, &warm, werr, sizeof(werr));
        if (wp) {
            vkDestroyPipeline(vk->dev, wp, NULL);
        }
    }
    return vk;

fail:
    eg_vk_destroy(vk);
    return NULL;
}

static void *waiter_thread(void *opaque)
{
    EgVk *vk = opaque;

    pthread_mutex_lock(&vk->dlock);
    for (;;) {
        uint64_t next = vk->gpu_done_seq + 1;
        VkFence fence = VK_NULL_HANDLE;
        int f, i;
        struct timespec ts;

        while (!vk->dquit && vk->pub_seq < next) {
            pthread_cond_wait(&vk->dcond, &vk->dlock);
        }
        if (vk->pub_seq < next) {
            break;                      /* quit, nothing left to wait for */
        }
        for (i = 0, f = -1; i < EVK_FRAMES; i++) {
            if (vk->frame_seq[i] == next) {
                f = i;
                fence = vk->fences[i];
            }
        }
        pthread_mutex_unlock(&vk->dlock);
        if (f >= 0) {
            vkWaitForFences(vk->dev, 1, &fence, VK_TRUE, UINT64_MAX);
        }
        clock_gettime(CLOCK_MONOTONIC, &ts);
        pthread_mutex_lock(&vk->dlock);
        if (f >= 0) {
            vk->stat_done_ns += ts.tv_sec * 1000000000ll + ts.tv_nsec - vk->submit_t[f];
            vk->stat_done_n++;
        }
        __atomic_store_n(&vk->gpu_done_seq, next, __ATOMIC_RELEASE);
        pthread_cond_broadcast(&vk->dcond);
        if (vk->notify) {
            pthread_mutex_unlock(&vk->dlock);
            vk->notify(vk->notify_opaque);
            pthread_mutex_lock(&vk->dlock);
        }
    }
    pthread_mutex_unlock(&vk->dlock);
    return NULL;
}

void eg_vk_set_notify_fn(EgVk *vk, void (*fn)(void *opaque), void *opaque)
{
    if (!vk) {
        return;
    }
    pthread_mutex_lock(&vk->dlock);
    vk->notify = fn;
    vk->notify_opaque = opaque;
    pthread_mutex_unlock(&vk->dlock);
}

/* the last batch that the completion thread saw complete (0: no thread) */
uint64_t eg_vk_gpu_done_seq(const EgVk *vk)
{
    return vk && vk->waiter_on ? __atomic_load_n(&vk->gpu_done_seq, __ATOMIC_ACQUIRE) : 0;
}

void eg_vk_done_latency(const EgVk *vk, uint64_t *ns, uint64_t *n)
{
    *ns = vk ? vk->stat_done_ns : 0;
    *n = vk ? vk->stat_done_n : 0;
}

uint64_t eg_vk_binds(const EgVk *vk)
{
    return vk ? vk->stat_binds : 0;
}

void eg_vk_destroy(EgVk *vk)
{
    int i;

    if (!vk) {
        return;
    }
    if (vk->waiter_on) {
        pthread_mutex_lock(&vk->dlock);
        vk->dquit = true;
        pthread_cond_broadcast(&vk->dcond);
        pthread_mutex_unlock(&vk->dlock);
        pthread_join(vk->waiter, NULL);
        vk->waiter_on = false;
    }
    if (vk->ncompilers) {
        pthread_mutex_lock(&vk->qlock);
        vk->quit = true;
        pthread_cond_broadcast(&vk->qcond);
        pthread_mutex_unlock(&vk->qlock);
        for (i = 0; i < vk->ncompilers; i++) {
            pthread_join(vk->compilers[i], NULL);
        }
    }
    if (vk->dev) {
        vkDeviceWaitIdle(vk->dev);
        if (vk->pcache && vk->new_pipes) {
            pcache_save(vk);
        }
        for (i = 0; i < EVK_PIPE_BUCKETS; i++) {
            EvkPipe *p = vk->pipes[i];
            while (p) {
                EvkPipe *n = p->next;
                if (p->pipe) {
                    vkDestroyPipeline(vk->dev, p->pipe, NULL);
                }
                free(p->src);
                free(p->spv);
                free(p);
                p = n;
            }
        }
        if (vk->vert) vkDestroyShaderModule(vk->dev, vk->vert, NULL);
        if (vk->pcache) vkDestroyPipelineCache(vk->dev, vk->pcache, NULL);
        for (i = 0; i < EVK_FRAMES; i++) {
            if (vk->fences[i]) vkDestroyFence(vk->dev, vk->fences[i], NULL);
            free(vk->fdirty_off[i]);
            free(vk->fdirty_len[i]);
        }
        if (vk->upfence) vkDestroyFence(vk->dev, vk->upfence, NULL);
        if (vk->tsq) vkDestroyQueryPool(vk->dev, vk->tsq, NULL);
        if (vk->pool) vkDestroyCommandPool(vk->dev, vk->pool, NULL);
        if (vk->dpool) vkDestroyDescriptorPool(vk->dev, vk->dpool, NULL);
        if (vk->cback) vkDestroyPipeline(vk->dev, vk->cback, NULL);
        if (vk->clayout) vkDestroyPipelineLayout(vk->dev, vk->clayout, NULL);
        if (vk->cdpool) vkDestroyDescriptorPool(vk->dev, vk->cdpool, NULL);
        if (vk->cdsl) vkDestroyDescriptorSetLayout(vk->dev, vk->cdsl, NULL);
        if (vk->dirty_bits.buf) vkDestroyBuffer(vk->dev, vk->dirty_bits.buf, NULL);
        if (vk->draws_dev.buf) vkDestroyBuffer(vk->dev, vk->draws_dev.buf, NULL);
        if (vk->draws_dev.mem) vkFreeMemory(vk->dev, vk->draws_dev.mem, NULL);
        if (vk->dirty_bits.mem) vkFreeMemory(vk->dev, vk->dirty_bits.mem, NULL);
        if (vk->layout) vkDestroyPipelineLayout(vk->dev, vk->layout, NULL);
        if (vk->cslayout) vkDestroyPipelineLayout(vk->dev, vk->cslayout, NULL);
        if (vk->dsl) vkDestroyDescriptorSetLayout(vk->dev, vk->dsl, NULL);
        {
            EvkBuf *bufs[9] = { &vk->vram, &vk->draws, &vk->verts, &vk->counts,
                                &vk->sysram, &vk->gart, &vk->vram_dev, &vk->idxbuf,
                                &vk->vsout };
            for (i = 0; i < 9; i++) {
                if (bufs[i]->buf) vkDestroyBuffer(vk->dev, bufs[i]->buf, NULL);
                if (bufs[i]->mem) vkFreeMemory(vk->dev, bufs[i]->mem, NULL);
            }
        }
        vkDestroyDevice(vk->dev, NULL);
    }
    if (vk->sco) shaderc_compile_options_release(vk->sco);
    if (vk->sc) shaderc_compiler_release(vk->sc);
    free(vk->frag_prefix);
    free(vk->cache_dir);
    if (vk->order) {
        fclose(vk->order);
    }
    free(vk->stale);
    free(vk->batch_wr);
    if (vk->inst) vkDestroyInstance(vk->inst, NULL);
    free(vk);
}

/* ------------------------------------------------------------------ */
/* Batches                                                             */
/* ------------------------------------------------------------------ */

/*
 * The copy-back of the VRAM copy: one workgroup per 4 KiB page of a range
 * that the batch can have changed.  It copies the page only if a fragment
 * wrote it (dirty_bits), and clears the bit.
 */
static const char cback_src[] =
    "#version 460\n"
    "layout(local_size_x = 256) in;\n"
    "layout(std430, set = 0, binding = 0) readonly buffer Src { uvec4 src[]; };\n"
    "layout(std430, set = 0, binding = 1) writeonly buffer Dst { uvec4 dst[]; };\n"
    "layout(std430, set = 0, binding = 2) buffer Bits { uint bits[]; };\n"
    "layout(push_constant) uniform PC { uint first; uint count; } pc;\n"
    "shared uint hit;\n"
    "void main() {\n"
    "  uint pg = pc.first + gl_WorkGroupID.x;\n"
    "  uint b = 1u << (pg & 31u);\n"
    "  if (gl_LocalInvocationID.x == 0u) hit = bits[pg >> 5] & b;\n"
    "  barrier();\n"
    "  if (hit == 0u) return;\n"
    "  uint i = pg * 256u + gl_LocalInvocationID.x;\n"
    "  dst[i] = src[i];\n"
    "  if (gl_LocalInvocationID.x == 0u) atomicAnd(bits[pg >> 5], ~b);\n"
    "}\n";

static bool make_copy_back(EgVk *vk)
{
    VkDescriptorSetLayoutBinding b[3];
    VkWriteDescriptorSet w[3];
    VkDescriptorBufferInfo bi[3] = {
        { vk->vram_dev.buf, 0, VK_WHOLE_SIZE }, { vk->vram.buf, 0, VK_WHOLE_SIZE },
        { vk->dirty_bits.buf, 0, VK_WHOLE_SIZE },
    };
    VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 3 };
    VkPushConstantRange pcr = { VK_SHADER_STAGE_COMPUTE_BIT, 0, 8 };
    char err[512];
    VkShaderModule m;
    int i;

    for (i = 0; i < 3; i++) {
        b[i] = (VkDescriptorSetLayoutBinding) {
            .binding = i, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
        };
        w[i] = (VkWriteDescriptorSet) {
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = i,
            .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .pBufferInfo = &bi[i],
        };
    }
    if (vkCreateDescriptorSetLayout(vk->dev, &(VkDescriptorSetLayoutCreateInfo) {
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
            .bindingCount = 3, .pBindings = b }, NULL, &vk->cdsl) != VK_SUCCESS ||
        vkCreateDescriptorPool(vk->dev, &(VkDescriptorPoolCreateInfo) {
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
            .maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &ps }, NULL, &vk->cdpool) !=
            VK_SUCCESS ||
        vkCreatePipelineLayout(vk->dev, &(VkPipelineLayoutCreateInfo) {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
            .setLayoutCount = 1, .pSetLayouts = &vk->cdsl,
            .pushConstantRangeCount = 1, .pPushConstantRanges = &pcr }, NULL, &vk->clayout) !=
            VK_SUCCESS ||
        vkAllocateDescriptorSets(vk->dev, &(VkDescriptorSetAllocateInfo) {
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
            .descriptorPool = vk->cdpool, .descriptorSetCount = 1, .pSetLayouts = &vk->cdsl },
            &vk->cdset) != VK_SUCCESS) {
        return false;
    }
    for (i = 0; i < 3; i++) {
        w[i].dstSet = vk->cdset;
    }
    vkUpdateDescriptorSets(vk->dev, 3, w, 0, NULL);
    m = compile(vk, cback_src, shaderc_compute_shader, err, sizeof(err));
    if (!m) {
        fprintf(stderr, "evergreen: copy-back shader: %s\n", err);
        return false;
    }
    if (vkCreateComputePipelines(vk->dev, VK_NULL_HANDLE, 1, &(VkComputePipelineCreateInfo) {
            .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
            .stage = { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                       .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = m, .pName = "main" },
            .layout = vk->clayout }, NULL, &vk->cback) != VK_SUCCESS) {
        vk->cback = VK_NULL_HANDLE;
    }
    vkDestroyShaderModule(vk->dev, m, NULL);
    return vk->cback != VK_NULL_HANDLE;
}

static void begin_rendering(EgVk *vk)
{
    VkRenderingInfo ri = {
        .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
        .renderArea = { { 0, 0 }, { EVK_VP, EVK_VP } },
        .layerCount = 1,
    };
    VkViewport vp = { 0, 0, EVK_VP, EVK_VP, 0.0f, 1.0f };

    vkCmdBeginRendering(vk->cmd, &ri);
    vkCmdSetViewport(vk->cmd, 0, 1, &vp);
    vk->rendering = true;
    vk->bound = VK_NULL_HANDLE;
}

static void storage_barrier(EgVk *vk, VkPipelineStageFlags dst_stage,
                            VkAccessFlags dst_access)
{
    VkMemoryBarrier mb = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
        .dstAccessMask = dst_access,
    };
    vkCmdPipelineBarrier(vk->cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, dst_stage,
                         0, 1, &mb, 0, NULL, 0, NULL);
}


typedef struct EvkCopies {
    VkBufferCopy *rg;
    uint32_t n, cap;
    uint64_t bytes;
} EvkCopies;

static void copies_add(EvkCopies *c, uint64_t off, uint64_t len)
{
    if (c->n && c->rg[c->n - 1].srcOffset + c->rg[c->n - 1].size == off) {
        c->rg[c->n - 1].size += len;
    } else {
        if (c->n == c->cap) {
            c->cap = c->cap ? c->cap * 2 : 64;
            c->rg = realloc(c->rg, c->cap * sizeof(*c->rg));
        }
        c->rg[c->n].srcOffset = c->rg[c->n].dstOffset = off;
        c->rg[c->n].size = len;
        c->n++;
    }
    c->bytes += len;
}

/*
 * A stale page some draw of this batch renders to: the guest CPU wrote
 * another surface that shares the page (the menu bar's Apple logo did).
 * Copy only the bytes outside this batch's render ranges; inside them the
 * GPU's result is the newer one.
 */
static void copies_add_unrendered(EgVk *vk, uint64_t a, uint64_t b, EvkCopies *c)
{
    while (a < b) {
        uint64_t s = b, e = b;
        int i;
        for (i = 0; i < vk->ndirty; i++) {
            uint64_t o = vk->dirty_off[i], l = vk->dirty_len[i];
            uint64_t from = o > a ? o : a;
            if (o < b && o + l > a && from < s) {
                s = from;
                e = o + l;
            }
        }
        if (s > a) {
            copies_add(c, a, s - a);
        }
        a = e;
    }
}

/* Move the stale pages of [off, off+len) into copy regions, clearing them;
 * pages in 'rendered' are copied only outside this batch's render ranges. */
static void stale_take_ex(EgVk *vk, uint64_t off, uint64_t len, EvkCopies *c,
                          const uint64_t *rendered)
{
    uint64_t pg, end;

    if (!vk->stale_pages || off >= vk->vram_size || !len) {
        return;
    }
    if (off + len > vk->vram_size) {
        len = vk->vram_size - off;
    }
    end = (off + len - 1) >> EVK_PAGE_SHIFT;
    for (pg = off >> EVK_PAGE_SHIFT; pg <= end; pg++) {
        uint64_t w = vk->stale[pg >> 6], bit = 1ull << (pg & 63);
        if (!w) {
            pg |= 63;                   /* skip a clean 64-page word */
            continue;
        }
        if (!(w & bit)) {
            continue;
        }
        vk->stale[pg >> 6] = w & ~bit;
        vk->stale_pages--;
        if (rendered && (rendered[pg >> 6] & bit)) {
            copies_add_unrendered(vk, pg << EVK_PAGE_SHIFT,
                                  (pg + 1) << EVK_PAGE_SHIFT, c);
        } else {
            copies_add(c, pg << EVK_PAGE_SHIFT, 1ull << EVK_PAGE_SHIFT);
        }
    }
}

static void stale_take(EgVk *vk, uint64_t off, uint64_t len, EvkCopies *c)
{
    stale_take_ex(vk, off, len, c, NULL);
}

static void batch_wr_mark(EgVk *vk, uint64_t off, uint64_t len)
{
    uint64_t pg, end;

    if (off >= vk->vram_size || !len) {
        return;
    }
    if (off + len > vk->vram_size) {
        len = vk->vram_size - off;
    }
    end = (off + len - 1) >> EVK_PAGE_SHIFT;
    pg = off >> EVK_PAGE_SHIFT;
    if ((pg >> 6) == (end >> 6)) {
        vk->batch_wr[pg >> 6] |= (~0ull >> (63 - (end & 63))) & (~0ull << (pg & 63));
        return;
    }
    vk->batch_wr[pg >> 6] |= ~0ull << (pg & 63);
    for (pg = (pg >> 6) + 1; pg < (end >> 6); pg++) {
        vk->batch_wr[pg] = ~0ull;
    }
    vk->batch_wr[end >> 6] |= ~0ull >> (63 - (end & 63));
}

static void upload_barrier(EgVk *vk)
{
    VkMemoryBarrier mb = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
    };
    vkCmdPipelineBarrier(vk->cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
}

static void record_copies(EgVk *vk, EvkCopies *c)
{
    if (c->n) {
        vkCmdCopyBuffer(vk->cmd, vk->vram.buf, vk->vram_dev.buf, c->n, c->rg);
        vk->stat_upload_bytes += c->bytes;
        vk->b_up_bytes += c->bytes;
        vk->b_up_n += c->n;
    }
    free(c->rg);
}

/*
 * Batch start.  A few stale pages are copied right away; a flood (the guest
 * clearing VRAM, KVM reporting a remapped BAR as all dirty) is left to the
 * draws that touch it (upload_for_draw) and to eg_vk_upload_kick, so a
 * vCPU never waits for hundreds of MB to cross PCIe.
 */
static void record_uploads(EgVk *vk)
{
    EvkCopies c = { 0 };

    static int64_t eager = -1;

    if (eager < 0) {
        const char *e = getenv("EG_VK_EAGER_UPLOAD_MB");
        eager = e ? (int64_t)atoi(e) << 20 : (int64_t)EVK_EAGER_UPLOAD;
    }
    if (vk->stale_pages && (int64_t)(vk->stale_pages << EVK_PAGE_SHIFT) <= eager) {
        stale_take(vk, 0, vk->vram_size, &c);
    }
    if (c.n || vk->up_barrier) {
        record_copies(vk, &c);
        upload_barrier(vk);         /* also orders an earlier kicked copy */
        vk->up_barrier = false;
    } else {
        free(c.rg);
    }
}

/*
 * Copy the stale pages a draw samples or renders to, before the draw.
 * Pages go stale mid-batch too (the guest CPU fills a texture between two
 * command buffers of one batch); bytes rendered earlier in the batch keep
 * the GPU's result, as host VRAM does not have it yet.
 */
static void upload_for_draw(EgVk *vk, const EgVkDrawReq *req)
{
    EvkCopies c = { 0 };
    int i;

    if (!vk->mirror) {
        return;                         /* shaders use host VRAM directly */
    }
    if (!vk->stale_pages) {
        goto mark;
    }
    for (i = 0; i < req->nrd; i++) {
        if (req->rd_off[i] < EVK_GART_SPACE) {
            stale_take_ex(vk, req->rd_off[i], req->rd_len[i], &c, vk->batch_wr);
        }
    }
    for (i = 0; i < req->nwr; i++) {
        if (req->wr_off[i] < EVK_GART_SPACE) {
            stale_take_ex(vk, req->wr_off[i], req->wr_len[i], &c, vk->batch_wr);
        }
    }
    if (!c.n) {
        free(c.rg);
        goto mark;
    }
    if (vk->rendering) {
        vkCmdEndRendering(vk->cmd);
        vk->rendering = false;
    }
    record_copies(vk, &c);
    upload_barrier(vk);
    begin_rendering(vk);
mark:
    for (i = 0; i < req->nwr; i++) {
        if (req->wr_off[i] < EVK_GART_SPACE) {
            batch_wr_mark(vk, req->wr_off[i], req->wr_len[i]);
        }
    }
}

void eg_vk_upload(EgVk *vk, uint64_t off, uint64_t len)
{
    uint64_t pg, end;

    if (!vk || !vk->mirror || !len || off >= vk->vram_size) {
        return;
    }
    if (off + len > vk->vram_size) {
        len = vk->vram_size - off;
    }
    end = (off + len - 1) >> EVK_PAGE_SHIFT;
    for (pg = off >> EVK_PAGE_SHIFT; pg <= end; pg++) {
        uint64_t bit = 1ull << (pg & 63);
        if (!(vk->stale[pg >> 6] & bit)) {
            vk->stale[pg >> 6] |= bit;
            vk->stale_pages++;
        }
    }
}
/*
 * Submit the queued mirror uploads now, between batches, without waiting.
 * The device calls this from the display refresh: by the time the guest
 * draws, the GPU has long done the copy, instead of a first batch copying
 * all 256 MB inside a vCPU's MMIO exit (130 ms; SMP guests panic at 250).
 * Same-queue order plus the barrier in the next batch keep it correct.
 */
void eg_vk_upload_kick(EgVk *vk)
{
    VkCommandBufferBeginInfo bi = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    VkSubmitInfo si = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
    };
    EvkCopies c = { 0 };

    if (!vk || !vk->mirror || vk->recording || !vk->stale_pages) {
        return;
    }
    if (vk->up_inflight) {
        if (vkGetFenceStatus(vk->dev, vk->upfence) != VK_SUCCESS) {
            return;                     /* previous copy still running */
        }
        vkResetFences(vk->dev, 1, &vk->upfence);
        vkResetCommandBuffer(vk->upcmd, 0);
        vk->up_inflight = false;
    }
    stale_take(vk, 0, vk->vram_size, &c);
    vkBeginCommandBuffer(vk->upcmd, &bi);
    vkCmdCopyBuffer(vk->upcmd, vk->vram.buf, vk->vram_dev.buf, c.n, c.rg);
    vkEndCommandBuffer(vk->upcmd);
    vk->stat_upload_bytes += c.bytes;
    free(c.rg);
    si.pCommandBuffers = &vk->upcmd;
    vkQueueSubmit(vk->queue, 1, &si, vk->upfence);
    vk->up_inflight = true;
    vk->up_barrier = true;
}

/* Copy all of VRAM to the mirror now, synchronously (device init, before
 * the guest runs); queued uploads are dropped. */
void eg_vk_upload_all_now(EgVk *vk)
{
    VkCommandBufferBeginInfo bi = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    VkSubmitInfo si = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1, .pCommandBuffers = &vk->cmd,
    };
    VkBufferCopy rg = { 0, 0, vk->vram_size };

    if (!vk || !vk->mirror || vk->recording) {
        return;
    }
    memset(vk->stale, 0, ((vk->vram_size >> EVK_PAGE_SHIFT) + 63) / 64 * 8);
    vk->stale_pages = 0;
    vkBeginCommandBuffer(vk->cmd, &bi);
    vkCmdCopyBuffer(vk->cmd, vk->vram.buf, vk->vram_dev.buf, 1, &rg);
    vkEndCommandBuffer(vk->cmd);
    vkQueueSubmit(vk->queue, 1, &si, vk->fence);
    vkWaitForFences(vk->dev, 1, &vk->fence, VK_TRUE, UINT64_MAX);
    /* leave fence and command buffer as a batch expects them: an already
     * signaled fence at the next vkQueueSubmit cost a 1 s stall */
    vkResetFences(vk->dev, 1, &vk->fence);
    vkResetCommandBuffer(vk->cmd, 0);
    vk->stat_upload_bytes += vk->vram_size;
}

bool eg_vk_mirrored(const EgVk *vk)
{
    return vk && vk->mirror;
}

static void frame_wait(EgVk *vk, int f);

static void begin_batch(EgVk *vk)
{
    VkCommandBufferBeginInfo bi = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    frame_wait(vk, vk->frame);
    vkBeginCommandBuffer(vk->cmd, &bi);
    if (vk->dsq) {
        vkCmdResetQueryPool(vk->cmd, vk->dsq, vk->frame * (EVK_DRAW_SLOTS + 1),
                            EVK_DRAW_SLOTS + 1);
        vk->dn[vk->frame] = 0;
    }
    if (vk->tsq) {
        vkCmdResetQueryPool(vk->cmd, vk->tsq, 4 * vk->frame, 4);
        /* 0 start, 2 before the copy-back, 3 end (1 unused) */
        vkCmdWriteTimestamp(vk->cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, vk->tsq, 4 * vk->frame);
    }
    /* the batch before may still run: order against its VRAM accesses */
    vkCmdPipelineBarrier(vk->cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                         VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT |
                         VK_PIPELINE_STAGE_VERTEX_SHADER_BIT, 0, 1,
                         &(VkMemoryBarrier){ .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                                             .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT |
                                                              VK_ACCESS_TRANSFER_WRITE_BIT,
                                             .dstAccessMask = VK_ACCESS_SHADER_READ_BIT |
                                                              VK_ACCESS_SHADER_WRITE_BIT |
                                                              VK_ACCESS_TRANSFER_READ_BIT |
                                                              VK_ACCESS_TRANSFER_WRITE_BIT },
                         0, NULL, 0, NULL);
    if (vk->mirror) {
        memset(vk->batch_wr, 0, ((vk->vram_size >> EVK_PAGE_SHIFT) + 63) / 64 * 8);
        record_uploads(vk);
    }
    if (vk->dirty_bits.buf && !vk->bits_clear) {
        vkCmdFillBuffer(vk->cmd, vk->dirty_bits.buf, 0, VK_WHOLE_SIZE, 0);
        vkCmdPipelineBarrier(vk->cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 1,
                             &(VkMemoryBarrier){ .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                                                 .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                                                 .dstAccessMask = VK_ACCESS_SHADER_READ_BIT |
                                                                  VK_ACCESS_SHADER_WRITE_BIT },
                             0, NULL, 0, NULL);
        vk->bits_clear = true;
    }
    vk->nzgrow = 0;                 /* the barrier above orders all of it */
    vkCmdBindDescriptorSets(vk->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, vk->layout,
                            0, 1, &vk->dset, 0, NULL);
    begin_rendering(vk);
    vk->recording = true;
    vk->ndraws = vk->nverts = 0;
    vk->nidx = 0;
    vk->nvsout = 0;
    vkCmdBindIndexBuffer(vk->cmd, vk->idxbuf.buf, 0, VK_INDEX_TYPE_UINT32);
    vk->ndirty = 0;
    vk->nw = vk->nr = 0;
}

static bool ranges_overlap(uint64_t a, uint64_t al, uint64_t b, uint64_t bl)
{
    return a < b + bl && b < a + al;
}

/*
 * Memory hazards between draws of one batch.  Same-pixel read-modify-write
 * of render targets is ordered by the fragment interlock; everything else
 * needs a barrier: sampling memory written earlier (RAW), writing memory
 * sampled earlier (WAR), and writing memory written earlier through a
 * differently shaped binding (WAW across surfaces).
 */
static bool needs_barrier(EgVk *vk, const EgVkDrawReq *req)
{
    int s, i, k;

    for (s = 0; s < req->nrd; s++) {
        for (i = 0; i < vk->nw; i++) {
            if (ranges_overlap(req->rd_off[s], req->rd_len[s], vk->w_off[i], vk->w_len[i])) {
                return true;
            }
        }
    }
    for (k = 0; k < req->nwr; k++) {
        uint64_t sig = req->wr_sig[k];
        for (i = 0; i < vk->nr; i++) {
            if (ranges_overlap(req->wr_off[k], req->wr_len[k], vk->r_off[i], vk->r_len[i])) {
                return true;
            }
        }
        for (i = 0; i < vk->nw; i++) {
            if (vk->w_sig[i] != sig &&
                ranges_overlap(req->wr_off[k], req->wr_len[k], vk->w_off[i], vk->w_len[i])) {
                return true;
            }
        }
    }
    return false;
}

/*
 * Draws of a batch mostly sample the same textures and render to the same
 * surfaces: a range that one already listed covers is not listed again, so
 * that needs_barrier() stays short.
 */
static void note_hazards(EgVk *vk, const EgVkDrawReq *req)
{
    int s, k, i;

    for (s = 0; s < req->nrd && vk->nr < EVK_MAX_DIRTY; s++) {
        uint64_t o = req->rd_off[s], l = req->rd_len[s];
        for (i = vk->nr - 1; i >= 0; i--) {
            if (vk->r_off[i] <= o && vk->r_off[i] + vk->r_len[i] >= o + l) {
                break;
            }
        }
        if (i < 0) {
            vk->r_off[vk->nr] = o;
            vk->r_len[vk->nr] = l;
            vk->nr++;
        }
    }
    for (k = 0; k < req->nwr && vk->nw < EVK_MAX_DIRTY; k++) {
        uint64_t o = req->wr_off[k], l = req->wr_len[k];
        for (i = vk->nw - 1; i >= 0; i--) {
            if (vk->w_sig[i] == req->wr_sig[k] && vk->w_off[i] <= o &&
                vk->w_off[i] + vk->w_len[i] >= o + l) {
                break;
            }
        }
        if (i < 0) {
            vk->w_off[vk->nw] = o;
            vk->w_len[vk->nw] = l;
            vk->w_sig[vk->nw] = req->wr_sig[k];
            vk->nw++;
        }
    }
}

/*
 * Specialized pipelines.  The template branches on the formats, swizzles,
 * tiling and sampler words of the textures, and on the formats, blending
 * and tiling of the color buffers.  For each program, the device also
 * compiles a pipeline with these values of the draw as constants (2-4x
 * faster on texture-heavy programs).  Until it is ready, the draw uses the
 * generic pipeline.
 */
#define SPEC_TEX_WORDS 10
#define SPEC_CB_WORDS 10
#define SPEC_WORDS (EVK_MAX_TEX * SPEC_TEX_WORDS + 8 * SPEC_CB_WORDS)

static int spec_words(const EvkDraw *d, uint32_t *w)
{
    int i, n = 0;

    for (i = 0; i < EVK_MAX_TEX; i++) {
        const EvkTex *t = &d->tex[i];
        const uint32_t *sm = d->smp[i];
        if (!(t->flags & EVK_TF_VALID)) {
            continue;
        }
        w[n++] = 0x100 + i;
        w[n++] = t->fmt; w[n++] = t->numf; w[n++] = t->comp; w[n++] = t->dsel;
        w[n++] = t->dim; w[n++] = t->flags; w[n++] = t->tile.mode;
        w[n++] = sm[0]; w[n++] = sm[1];         /* sm[2] (LOD bias) varies: leave it */
    }
    for (i = 0; i < 8; i++) {
        const EvkSurf *c = &d->cb[i];
        if (!(c->flags & EVK_SF_ENABLED)) {
            continue;
        }
        w[n++] = 0x200 + i;
        w[n++] = c->fmt; w[n++] = c->bpp; w[n++] = c->flags; w[n++] = c->number;
        w[n++] = c->chmap; w[n++] = c->blend; w[n++] = c->bits; w[n++] = c->nch;
        w[n++] = c->tile.mode;
    }
    return n;
}

/* "#define TEX_FMT(s) ((s) == 0u ? 26u : D.tex[s].fmt)" and so on */
static char *spec_defs(const uint32_t *w, int n)
{
    static const char *const tex[] = { "FMT", "NUMF", "COMP", "DSEL", "DIM", "FLAGS", "TMODE",
                                       NULL, NULL };
    static const char *const cb[] = { "FMT", "BPP", "FLAGS", "NUMBER", "CHMAP", "BLEND", "BITS",
                                      "NCH", "TMODE" };
    static const char *const texf[] = { "fmt", "numf", "comp", "dsel", "dim", "flags",
                                        "tile.mode" };
    static const char *const cbf[] = { "fmt", "bpp", "flags", "number", "chmap", "blend",
                                       "bits", "nch", "tile.mode" };
    size_t cap = 16384, len = 0;
    char *out = malloc(cap);
    int f, k;

    out[0] = 0;
    /* texture fields and the two sampler words */
    for (f = 0; f < 9; f++) {
        const char *name = f < 7 ? tex[f] : NULL;
        len += snprintf(out + len, cap - len, "#define %s%s(s) (", f < 7 ? "TEX_" : "SMP_",
                        f < 7 ? name : (f == 7 ? "X" : "Y"));
        for (k = 0; k < n; k += SPEC_TEX_WORDS) {
            if (w[k] < 0x100 || w[k] >= 0x200) {
                break;
            }
            len += snprintf(out + len, cap - len, "(s) == %uu ? %uu : ", w[k] - 0x100,
                            w[k + 1 + f]);
        }
        if (f < 7) {
            len += snprintf(out + len, cap - len, "D.tex[s].%s)\n", texf[f]);
        } else {
            len += snprintf(out + len, cap - len, "D.smp[s].%c)\n", f == 7 ? 'x' : 'y');
        }
    }
    for (f = 0; f < 9; f++) {
        len += snprintf(out + len, cap - len, "#define CB_%s(n) (", cb[f]);
        for (k = 0; k < n; ) {
            if (w[k] < 0x200) {
                k += SPEC_TEX_WORDS;
                continue;
            }
            len += snprintf(out + len, cap - len, "(n) == %uu ? %uu : ", w[k] - 0x200,
                            w[k + 1 + f]);
            k += SPEC_CB_WORDS;
        }
        len += snprintf(out + len, cap - len, "D.cb[n].%s)\n", cbf[f]);
    }
    return out;
}

/* the pipeline of 'key' if it is ready; queue it (from ps + defs) if new */
static VkPipeline pipeline_nowait(EgVk *vk, const char *ps, const char *vs, uint64_t key,
                                  const char *defs)
{
    EvkPipe **head = &vk->pipes[key % EVK_PIPE_BUCKETS], *p;

    for (p = *head; p; p = p->next) {
        if (p->hash == key) {
            return __atomic_load_n(&p->state, __ATOMIC_ACQUIRE) == EVK_PIPE_READY ?
                   p->pipe : VK_NULL_HANDLE;
        }
    }
    p = calloc(1, sizeof(*p));
    p->hash = key;
    p->state = EVK_PIPE_PENDING;
    p->src = frag_source(vk, ps, defs);
    p->vsrc = vs ? vert_source(vk, vs) : NULL;
    p->next = *head;
    *head = p;
    if (!p->src) {
        p->state = EVK_PIPE_FAILED;
        return VK_NULL_HANDLE;
    }
    if (vk->sync_compile || !vk->ncompilers) {
        compile_entry(vk, p);
        return p->state == EVK_PIPE_READY ? p->pipe : VK_NULL_HANDLE;
    }
    pthread_mutex_lock(&vk->qlock);
    if (vk->qtail) {
        vk->qtail->qnext = p;
    } else {
        vk->qhead = p;
    }
    vk->qtail = p;
    pthread_cond_signal(&vk->qcond);
    pthread_mutex_unlock(&vk->qlock);
    return VK_NULL_HANDLE;
}

/* pipelines of GPU VS draws: the PS and the VS */
static uint64_t pipe_key(const EgVkDrawReq *req)
{
    return req->vs_glsl ? req->prog_hash ^ (req->vs_hash * 0xc2b2ae3d27d4eb4full) ^ 0x9e37u
                        : req->prog_hash;
}

/* the specialized pipeline of the draw, or VK_NULL_HANDLE (not ready) */
static VkPipeline spec_pipeline(EgVk *vk, const EgVkDrawReq *req)
{
    uint32_t w[SPEC_WORDS];
    int n = spec_words(req->draw, w), i;
    uint64_t h = 0xcbf29ce484222325ull, key;
    char *defs;
    VkPipeline pipe;
    EvkPipe *p;

    for (i = 0; i < n; i++) {
        h = (h ^ w[i]) * 0x100000001b3ull;
    }
    key = pipe_key(req) ^ (h * 0x9e3779b97f4a7c15ull) ^ 0x5bd1e995u;
    for (p = vk->pipes[key % EVK_PIPE_BUCKETS]; p; p = p->next) {
        if (p->hash == key) {
            return __atomic_load_n(&p->state, __ATOMIC_ACQUIRE) == EVK_PIPE_READY ?
                   p->pipe : VK_NULL_HANDLE;
        }
    }
    defs = spec_defs(w, n);
    pipe = pipeline_nowait(vk, req->ps_glsl, req->vs_glsl, key, defs);
    free(defs);
    return pipe;
}

/* whether the draw can make stored depth larger (see db_early_fail) */
static bool z_can_grow(const uint32_t *mi)
{
    uint32_t f = mi[EVK_M_Z_FUNC] & 7;
    return mi[EVK_M_Z_FMT] &&
           ((mi[EVK_M_Z_WRITE] && (!mi[EVK_M_Z_ENABLE] || f > 3)) || mi[EVK_M_Z_CLEAR]);
}

static bool zgrow_has(const EgVk *vk, uint64_t base)
{
    int i;

    if (vk->nzgrow > 8) {
        return true;                /* too many to list */
    }
    for (i = 0; i < vk->nzgrow; i++) {
        if (vk->zgrow[i] == base) {
            return true;
        }
    }
    return false;
}

bool eg_vk_draw(EgVk *vk, const EgVkDrawReq *req, char *why, size_t whylen)
{
    VkPipeline pipe;
    VkRect2D sc;
    uint32_t idx;
    int i;
    const uint32_t *mi = req->draw->misc;
    /* the depth test can run before the program (db_early_fail) */
    bool early = vk->early_z && mi[EVK_M_Z_FMT] && mi[EVK_M_Z_ENABLE] &&
                 ((mi[EVK_M_Z_FUNC] & 7) == 1 || (mi[EVK_M_Z_FUNC] & 7) == 3) &&
                 !mi[EVK_M_S_ENABLE] && !mi[EVK_M_Z_CLEAR] &&
                 req->draw->zb.base == req->draw->zb.wbase;

    if (req->vs_glsl ? !req->nidx : !req->nverts) {
        return true;
    }
    if (req->vs_glsl ? req->nidx > EVK_IDX_SLOTS : req->nverts > EVK_VERT_SLOTS) {
        set_err(why, whylen, "too many vertices");
        return false;
    }
    if (vk->no_pervertex && req->vs_glsl &&
        (uint64_t)req->nidx * (req->ninst ? req->ninst : 1) > EVK_VSOUT_SLOTS) {
        set_err(why, whylen, "too many corners");
        return false;
    }
    pipe = vk->specialize ? spec_pipeline(vk, req) : VK_NULL_HANDLE;
    if (!pipe && req->vs_glsl) {
        /* never wait for a GPU VS pipeline: the CPU VS path draws meanwhile */
        pipe = pipeline_nowait(vk, req->ps_glsl, req->vs_glsl, pipe_key(req), NULL);
        if (!pipe) {
            set_err(why, whylen, "vs compiling");
            return false;
        }
    }
    if (!pipe) {
        pipe = get_pipeline(vk, req->ps_glsl, req->vs_glsl, pipe_key(req), why, whylen);
    }
    if (!pipe) {
        return false;
    }
    if (vk->recording && (vk->ndraws >= EVK_DRAW_SLOTS ||
                          vk->nverts + req->nverts > EVK_VERT_SLOTS ||
                          vk->nidx + req->nidx > EVK_IDX_SLOTS ||
                          (vk->no_pervertex && req->vs_glsl &&
                           vk->nvsout + (uint64_t)req->nidx * (req->ninst ? req->ninst : 1) >
                           EVK_VSOUT_SLOTS) ||
                          vk->ndirty + req->nwr > EVK_MAX_DIRTY)) {
        eg_vk_submit(vk);
    }
    if (!vk->recording) {
        begin_batch(vk);
    }
    upload_for_draw(vk, req);
    if (vk->nw >= EVK_MAX_DIRTY - 8 || vk->nr >= EVK_MAX_DIRTY - 16 ||
        vk->barrier_each || needs_barrier(vk, req) ||
        (early && zgrow_has(vk, req->draw->zb.base))) {
        vkCmdEndRendering(vk->cmd);
        storage_barrier(vk, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                        VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
        begin_rendering(vk);
        vk->nw = vk->nr = 0;
        vk->nzgrow = 0;
        vk->stat_barriers++;
    }
    if (z_can_grow(mi)) {
        if (vk->nzgrow < 8) {
            vk->zgrow[vk->nzgrow++] = req->draw->zb.wbase;
        } else {
            vk->nzgrow = 9;
        }
    }
    note_hazards(vk, req);

    idx = vk->frame * EVK_DRAW_SLOTS + vk->ndraws++;
    memcpy((EvkDraw *)vk->draws.map + idx, req->draw, sizeof(EvkDraw));
    ((EvkDraw *)vk->draws.map)[idx].misc[EVK_M_ZPASS_SLOT] = vk->zslot;
    ((EvkDraw *)vk->draws.map)[idx].misc[EVK_M_EARLY_Z] = early;
    if (req->vs_glsl) {
        memcpy((uint32_t *)vk->idxbuf.map + vk->frame * EVK_IDX_SLOTS + vk->nidx, req->idx,
               (size_t)req->nidx * 4);
    } else {
        memcpy((EvkVertex *)vk->verts.map + vk->frame * EVK_VERT_SLOTS + vk->nverts, req->verts,
               (size_t)req->nverts * sizeof(EvkVertex));
    }

    if (vk->bound != pipe) {
        vkCmdBindPipeline(vk->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
        vk->bound = pipe;
        vk->stat_binds++;
    }
    if (vk->dsq && vk->ndraws == 1) {
        vkCmdWriteTimestamp(vk->cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, vk->dsq,
                            vk->frame * (EVK_DRAW_SLOTS + 1));
    }
    {
        uint32_t pc[4] = { idx, 0, 0, 0 };
        if (vk->no_pervertex) {
            if (req->vs_glsl) {
                pc[1] = vk->frame * EVK_IDX_SLOTS + vk->nidx;
                pc[2] = vk->frame * EVK_VSOUT_SLOTS + vk->nvsout;
                pc[3] = req->nidx;
            } else {
                pc[1] = vk->frame * EVK_VERT_SLOTS + vk->nverts;
            }
        }
        vkCmdPushConstants(vk->cmd, vk->layout,
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, 16, pc);
    }
    sc.offset.x = req->sc_x0 < 0 ? 0 : (req->sc_x0 > EVK_VP ? EVK_VP : req->sc_x0);
    sc.offset.y = req->sc_y0 < 0 ? 0 : (req->sc_y0 > EVK_VP ? EVK_VP : req->sc_y0);
    {
        int x1 = req->sc_x1 > EVK_VP ? EVK_VP : req->sc_x1;    /* exclusive */
        int y1 = req->sc_y1 > EVK_VP ? EVK_VP : req->sc_y1;
        sc.extent.width = x1 > sc.offset.x ? (uint32_t)(x1 - sc.offset.x) : 0;
        sc.extent.height = y1 > sc.offset.y ? (uint32_t)(y1 - sc.offset.y) : 0;
    }
    vkCmdSetScissor(vk->cmd, 0, 1, &sc);
    if (req->vs_glsl) {
        /* the guest's culling on the host rasterizer (CPU VS: done already) */
        if (vk->portability) {
            /*
             * MoltenVK: a cull mode or front face equal to the last one set
             * did not always reach the draw (after a new render encoder, the
             * Metal state can be the default).  Then a quad of the menu bar
             * clock was culled, and the old text stayed.  Set other values
             * first, so that each set is a change.
             */
            vkCmdSetCullMode(vk->cmd, VK_CULL_MODE_FRONT_AND_BACK);
            vkCmdSetFrontFace(vk->cmd, req->front_ccw ? VK_FRONT_FACE_CLOCKWISE
                                                      : VK_FRONT_FACE_COUNTER_CLOCKWISE);
        }
        vkCmdSetCullMode(vk->cmd, (req->cull_front ? VK_CULL_MODE_FRONT_BIT : 0) |
                                  (req->cull_back ? VK_CULL_MODE_BACK_BIT : 0));
        vkCmdSetFrontFace(vk->cmd, req->front_ccw ? VK_FRONT_FACE_COUNTER_CLOCKWISE
                                                  : VK_FRONT_FACE_CLOCKWISE);
        if (vk->no_pervertex) {
            /* one invocation per corner: the VS fetches its index (EVK_VID) */
            vkCmdDraw(vk->cmd, req->nidx, req->ninst ? req->ninst : 1,
                      vk->frame * EVK_IDX_SLOTS + vk->nidx, 0);
            vk->nvsout += req->nidx * (req->ninst ? req->ninst : 1);
        } else {
            vkCmdDrawIndexed(vk->cmd, req->nidx, req->ninst ? req->ninst : 1,
                             vk->frame * EVK_IDX_SLOTS + vk->nidx, 0, 0);
        }
        vk->nidx += req->nidx;
    } else {
        if (vk->portability) {
            /* MoltenVK: make each set a change (see above) */
            vkCmdSetCullMode(vk->cmd, VK_CULL_MODE_FRONT_AND_BACK);
            vkCmdSetFrontFace(vk->cmd, VK_FRONT_FACE_CLOCKWISE);
        }
        vkCmdSetCullMode(vk->cmd, VK_CULL_MODE_NONE);
        vkCmdSetFrontFace(vk->cmd, VK_FRONT_FACE_COUNTER_CLOCKWISE);
        vkCmdDraw(vk->cmd, req->nverts, 1, vk->frame * EVK_VERT_SLOTS + vk->nverts, 0);
        vk->nverts += req->nverts;
    }
    if (vk->dsq) {
        uint32_t k = vk->frame * EVK_DRAW_SLOTS + vk->ndraws - 1;
        vkCmdWriteTimestamp(vk->cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, vk->dsq,
                            vk->frame * (EVK_DRAW_SLOTS + 1) + vk->ndraws);
        vk->dprog[k] = req->prog_hash;
        vk->darea[k] = sc.extent.width * sc.extent.height;
        vk->dn[vk->frame] = vk->ndraws;
    }
    for (i = 0; i < req->nwr && vk->ndirty < EVK_MAX_DIRTY; i++) {
        /* what the draw can change: its scissor rows (wb_*), else the surface */
        uint64_t o = req->wb_len[i] ? req->wb_off[i] : req->wr_off[i];
        uint64_t l = req->wb_len[i] ? req->wb_len[i] : req->wr_len[i];
        int k;
        /* draws mostly render to the surfaces of the draw before */
        for (k = vk->ndirty - 1; k >= 0 && k >= vk->ndirty - 8; k--) {
            if (vk->dirty_off[k] <= o && vk->dirty_off[k] + vk->dirty_len[k] >= o + l) {
                break;
            }
        }
        if (k >= 0 && k >= vk->ndirty - 8) {
            continue;
        }
        vk->dirty_off[vk->ndirty] = o;
        vk->dirty_len[vk->ndirty] = l;
        vk->ndirty++;
    }
    vk->stat_draws++;
    {
        /* a batch goes to the GPU once it has EVK_BATCH_DRAWS draws
         * (EG_VK_BATCH_DRAWS, 0: at fences and kick ends only), so the GPU
         * runs it while the CP records the next one: a fence then waits for
         * a short tail of draws, not a whole kick */
        static int batch_max = -1;
        if (batch_max < 0) {
            const char *e = getenv("EG_VK_BATCH_DRAWS");
            batch_max = e ? atoi(e) : EVK_BATCH_DRAWS;
        }
        static int idle_min = -1;
        if (idle_min < 0) {
            const char *e = getenv("EG_VK_IDLE_DRAWS");
            idle_min = e ? atoi(e) : EVK_IDLE_DRAWS;
        }
        if (batch_max > 0 && vk->ndraws >= (uint32_t)batch_max) {
            eg_vk_submit(vk);
            return true;
        }
        /* the GPU has nothing to run: start what is recorded now, so the
         * next fence waits for fewer draws (EG_VK_IDLE_DRAWS, 0: off) */
        if (idle_min > 0 && vk->waiter_on && vk->ndraws >= (uint32_t)idle_min &&
            __atomic_load_n(&vk->gpu_done_seq, __ATOMIC_ACQUIRE) == vk->submit_seq) {
            eg_vk_submit(vk);
            return true;
        }
    }
    if (vk->trace) {
        size_t l = strlen(vk->b_progs);
        vk->b_area += (uint64_t)sc.extent.width * sc.extent.height;
        for (i = 0; i < req->nrd; i++) {
            if (req->rd_off[i] >= EVK_GART_SPACE) vk->b_gart |= 1;
        }
        if (req->nwr && req->wr_off[0] >= EVK_GART_SPACE) vk->b_gart |= 2;
        if (l < sizeof(vk->b_progs) - 24) {
            snprintf(vk->b_progs + l, sizeof(vk->b_progs) - l, " %04x:%ux%u",
                     (unsigned)(req->prog_hash & 0xffff), sc.extent.width, sc.extent.height);
        }
    }
    return true;
}

/*
 * The pipeline of a compute program: the specialized one when it is ready,
 * else the generic one.  The generic one gets a bounded wait while it
 * compiles, as for draws.
 */
static VkPipeline cs_pipeline(EgVk *vk, const EgVkDrawReq *req, char *why, size_t whylen)
{
    static const char cs_def[] = "#define EVK_CS 1\n";
    uint64_t key = req->prog_hash ^ 0x63730000c5c5c5c5ull, skey;
    EvkPipe **head, *p;
    VkPipeline pipe = VK_NULL_HANDLE;

    if (vk->specialize) {
        uint32_t w[SPEC_WORDS];
        int n = spec_words(req->draw, w), i;
        uint64_t h = 0xcbf29ce484222325ull;
        for (i = 0; i < n; i++) {
            h = (h ^ w[i]) * 0x100000001b3ull;
        }
        skey = key ^ (h * 0x9e3779b97f4a7c15ull) ^ 0x5bd1e995u;
        for (p = vk->pipes[skey % EVK_PIPE_BUCKETS]; p; p = p->next) {
            if (p->hash == skey) {
                break;
            }
        }
        if (p) {
            if (__atomic_load_n(&p->state, __ATOMIC_ACQUIRE) == EVK_PIPE_READY) {
                return p->pipe;
            }
        } else {
            char *defs = spec_defs(w, n);
            size_t nd = strlen(defs);
            char *all = malloc(sizeof(cs_def) + nd);
            memcpy(all, cs_def, sizeof(cs_def) - 1);
            memcpy(all + sizeof(cs_def) - 1, defs, nd + 1);
            free(defs);
            p = calloc(1, sizeof(*p));
            p->hash = skey;
            p->compute = true;
            p->state = EVK_PIPE_PENDING;
            p->src = frag_source(vk, req->cs_glsl, all);
            free(all);
            head = &vk->pipes[skey % EVK_PIPE_BUCKETS];
            p->next = *head;
            *head = p;
            if (!p->src) {
                p->state = EVK_PIPE_FAILED;
            } else if (vk->sync_compile || !vk->ncompilers) {
                compile_entry(vk, p);
                if (p->state == EVK_PIPE_READY) {
                    return p->pipe;
                }
            } else {
                pthread_mutex_lock(&vk->qlock);
                if (vk->qtail) {
                    vk->qtail->qnext = p;
                } else {
                    vk->qhead = p;
                }
                vk->qtail = p;
                pthread_cond_signal(&vk->qcond);
                pthread_mutex_unlock(&vk->qlock);
            }
        }
    }
    head = &vk->pipes[key % EVK_PIPE_BUCKETS];
    for (p = *head; p; p = p->next) {
        if (p->hash == key) {
            switch (__atomic_load_n(&p->state, __ATOMIC_ACQUIRE)) {
            case EVK_PIPE_READY:
                return p->pipe;
            case EVK_PIPE_PENDING:
                return wait_pipeline(vk, p, why, whylen);
            default:
                set_err(why, whylen, "compute program failed to compile");
                return VK_NULL_HANDLE;
            }
        }
    }
    p = calloc(1, sizeof(*p));
    p->hash = key;
    p->compute = true;
    p->state = EVK_PIPE_PENDING;
    p->src = frag_source(vk, req->cs_glsl, cs_def);
    p->next = *head;
    *head = p;
    if (!p->src) {
        p->state = EVK_PIPE_FAILED;
        set_err(why, whylen, "compute program not translatable");
        return VK_NULL_HANDLE;
    }
    if (vk->sync_compile || !vk->ncompilers) {
        compile_entry(vk, p);
        pipe = p->state == EVK_PIPE_READY ? p->pipe : VK_NULL_HANDLE;
        if (!pipe) {
            set_err(why, whylen, "compute program failed to compile");
        }
        return pipe;
    }
    pthread_mutex_lock(&vk->qlock);
    if (vk->qtail) {
        vk->qtail->qnext = p;
    } else {
        vk->qhead = p;
    }
    vk->qtail = p;
    pthread_cond_signal(&vk->qcond);
    pthread_mutex_unlock(&vk->qlock);
    return wait_pipeline(vk, p, why, whylen);
}

bool eg_vk_dispatch(EgVk *vk, const EgVkDrawReq *req, char *why, size_t whylen)
{
    VkPipeline pipe;
    uint32_t idx;
    int i;
    VkMemoryBarrier mb = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER };

    if (!req->cs_groups[0] || !req->cs_groups[1]) {
        return true;
    }
    pipe = cs_pipeline(vk, req, why, whylen);
    if (!pipe) {
        return false;
    }
    if (vk->recording && (vk->ndraws >= EVK_DRAW_SLOTS ||
                          vk->ndirty + req->nwr > EVK_MAX_DIRTY)) {
        eg_vk_submit(vk);
    }
    if (!vk->recording) {
        begin_batch(vk);
    }
    upload_for_draw(vk, req);
    if (vk->rendering) {
        vkCmdEndRendering(vk->cmd);
        vk->rendering = false;
    }
    /* after every earlier draw, upload and dispatch of the batch */
    mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(vk->cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &mb, 0, NULL, 0, NULL);

    idx = vk->frame * EVK_DRAW_SLOTS + vk->ndraws++;
    memcpy((EvkDraw *)vk->draws.map + idx, req->draw, sizeof(EvkDraw));
    vkCmdBindPipeline(vk->cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
    vkCmdBindDescriptorSets(vk->cmd, VK_PIPELINE_BIND_POINT_COMPUTE, vk->cslayout, 0, 1,
                            &vk->dset, 0, NULL);
    {
        uint32_t pc[4] = { idx, 0, 0, 0 };
        vkCmdPushConstants(vk->cmd, vk->cslayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, 16, pc);
    }
    vkCmdDispatch(vk->cmd, req->cs_groups[0], req->cs_groups[1], 1);

    /* before every later draw, dispatch, copy and the copy-back */
    mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT |
                       VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(vk->cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 1, &mb, 0, NULL, 0, NULL);
    begin_rendering(vk);
    vk->nw = vk->nr = 0;
    vk->nzgrow = 0;
    for (i = 0; i < req->nwr && vk->ndirty < EVK_MAX_DIRTY; i++) {
        vk->dirty_off[vk->ndirty] = req->wr_off[i];
        vk->dirty_len[vk->ndirty] = req->wr_len[i];
        vk->ndirty++;
    }
    vk->stat_dispatches++;
    return true;
}

/* whether the pipeline of a program exists (ready or compiling) */
bool eg_vk_has_pipeline(EgVk *vk, uint64_t hash)
{
    EvkPipe *p;

    for (p = vk->pipes[hash % EVK_PIPE_BUCKETS]; p; p = p->next) {
        if (p->hash == hash) {
            return __atomic_load_n(&p->state, __ATOMIC_ACQUIRE) != EVK_PIPE_FAILED;
        }
    }
    return false;
}

uint32_t eg_vk_batch_draws(const EgVk *vk)
{
    return vk && vk->recording ? vk->ndraws : 0;
}

bool eg_vk_pending(const EgVk *vk)
{
    int i;

    if (!vk) {
        return false;
    }
    for (i = 0; i < EVK_FRAMES; i++) {
        if (vk->busy[i]) {
            return true;
        }
    }
    return vk->recording;
}

/*
 * The driver's compiled pipelines (VkPipelineCache) next to the SPIR-V:
 * creating a pipeline from it takes ms instead of ~300 ms until the next
 * driver update, which the cache header detects (the data is then ignored).
 */
static void pcache_save(EgVk *vk)
{
    char path[4096], tmp[4200];
    size_t len = 0;
    void *data;
    FILE *f;

    if (!vk->cache_dir ||
        vkGetPipelineCacheData(vk->dev, vk->pcache, &len, NULL) != VK_SUCCESS || !len) {
        return;
    }
    data = malloc(len);
    if (vkGetPipelineCacheData(vk->dev, vk->pcache, &len, data) == VK_SUCCESS) {
        snprintf(path, sizeof(path), "%s/pipelines.bin", vk->cache_dir);
        snprintf(tmp, sizeof(tmp), "%s.tmp", path);
        f = fopen(tmp, "wb");
        if (f) {
            bool ok = fwrite(data, 1, len, f) == len;
            if (fclose(f) == 0 && ok) {
                rename(tmp, path);
            } else {
                remove(tmp);
            }
        }
    }
    free(data);
}

static void pcache_load(EgVk *vk)
{
    char path[4096];
    FILE *f;
    long len;

    snprintf(path, sizeof(path), "%s/pipelines.bin", vk->cache_dir);
    f = fopen(path, "rb");
    if (!f) {
        return;
    }
    if (fseek(f, 0, SEEK_END) == 0 && (len = ftell(f)) > 0 && fseek(f, 0, SEEK_SET) == 0) {
        void *data = malloc(len);
        if (fread(data, 1, len, f) == (size_t)len) {
            VkPipelineCacheCreateInfo ci = {
                .sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO,
                .initialDataSize = len, .pInitialData = data,
            };
            VkPipelineCache loaded;
            if (vkCreatePipelineCache(vk->dev, &ci, NULL, &loaded) == VK_SUCCESS) {
                vkMergePipelineCaches(vk->dev, vk->pcache, 1, &loaded);
                vkDestroyPipelineCache(vk->dev, loaded, NULL);
            }
        }
        free(data);
    }
    fclose(f);
}

void eg_vk_new_cp_run(EgVk *vk)
{
    if (vk) {
        vk->wait_ns = 0;
    }
}

void eg_vk_set_wait_fn(EgVk *vk, EgVkWaitFn fn)
{
    vk->wait_fn = fn;
}

static uint64_t fnv_str(uint64_t h, const char *str)
{
    for (; *str; str++) {
        h = (h ^ (uint8_t)*str) * 1099511628211ull;
    }
    return h;
}

/*
 * Persistent SPIR-V cache and prewarm.  A host GPU driver update throws away
 * the driver's own shader cache, and the first boot afterwards then compiled
 * every guest fragment program cold while the guest waited (hundreds of ms
 * per CP run, enough to panic an SMP guest).  Programs seen before are
 * queued for pipeline creation here, in the order the guest first used
 * them, while firmware and the guest kernel still boot.
 */
int eg_vk_set_cache_dir(EgVk *vk, const char *dir)
{
    char path[4096];
    FILE *f;
    int n = 0;

    if (!vk || !dir || !*dir || vk->cache_dir) {
        return 0;
    }
    vk->cache_dir = strdup(dir);
    pcache_load(vk);
    vk->tpl_hash = fnv_str(fnv_str(1469598103934665603ull, vk->frag_prefix),
                           evk_frag_template()) ^ ((uint64_t)evk_ps_build_id() << 32);
    /* drop SPIR-V and order files of other template/emitter versions */
    {
        DIR *d = opendir(dir);
        struct dirent *e;
        char key[17];

        snprintf(key, sizeof(key), "%016llx", (unsigned long long)vk->tpl_hash);
        while (d && (e = readdir(d))) {
            const char *nm = e->d_name;
            const char *k = !strncmp(nm, "order-", 6) ? nm + 6 : nm;
            size_t len = strlen(nm);
            bool ours = !strncmp(k, key, 16);
            bool spv = len > 4 && !strcmp(nm + len - 4, ".spv");
            if (!ours && (spv || k != nm)) {
                snprintf(path, sizeof(path), "%s/%s", dir, nm);
                remove(path);
            }
        }
        if (d) {
            closedir(d);
        }
    }
    snprintf(path, sizeof(path), "%s/order-%016llx", dir,
             (unsigned long long)vk->tpl_hash);
    f = fopen(path, "r");
    if (f && vk->ncompilers && !vk->sync_compile) {
        char line[64];
        while (fgets(line, sizeof(line), f)) {
            uint64_t hash = strtoull(line, NULL, 16);
            EvkPipe **head = &vk->pipes[hash % EVK_PIPE_BUCKETS], *p;
            size_t bytes;
            uint32_t *words;

            for (p = *head; p && p->hash != hash; p = p->next) {
            }
            if (p || !(words = cache_load(vk, hash, &bytes))) {
                continue;
            }
            p = calloc(1, sizeof(*p));
            p->hash = hash;
            p->state = EVK_PIPE_PENDING;
            p->spv = words;
            p->spv_bytes = bytes;
            p->next = *head;
            *head = p;
            pthread_mutex_lock(&vk->qlock);
            if (vk->qtail) {
                vk->qtail->qnext = p;
            } else {
                vk->qhead = p;
            }
            vk->qtail = p;
            pthread_cond_signal(&vk->qcond);
            pthread_mutex_unlock(&vk->qlock);
            n++;
        }
    }
    if (f) {
        fclose(f);
    }
    vk->order = fopen(path, "a");
    return n;
}

/* frame f has completed: recycle it and report what it rendered */
static void frame_done(EgVk *vk, int f)
{
    int i;

    if (vk->tsq) {
        uint64_t t[4];
        /* 1 is never written: read 0 and 2..3 */
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        if (vkGetQueryPoolResults(vk->dev, vk->tsq, 4 * f, 1, 8, t, 8,
                                  VK_QUERY_RESULT_64_BIT) == VK_SUCCESS &&
            vkGetQueryPoolResults(vk->dev, vk->tsq, 4 * f + 2, 2, 16, t + 2, 8,
                                  VK_QUERY_RESULT_64_BIT) == VK_SUCCESS && t[3] >= t[0]) {
            vk->stat_gpu_ns += (uint64_t)((t[3] - t[0]) * vk->ts_period);
            vk->stat_gpu_back_ns += (uint64_t)((t[3] - t[2]) * vk->ts_period);
            vk->stat_lat_ns += ts.tv_sec * 1000000000ll + ts.tv_nsec - vk->submit_t[f];
            vk->stat_timed++;
        }
    }
    if (vk->dsq && vk->dn[f]) {
        static uint64_t tot_ns, tot_draws, tot_px, last;
        static struct { uint64_t h, ns, n, px; } top[256];
        uint64_t t[EVK_DRAW_SLOTS + 1];
        uint32_t n = vk->dn[f], j;
        if (vkGetQueryPoolResults(vk->dev, vk->dsq, f * (EVK_DRAW_SLOTS + 1), n + 1,
                                  sizeof(t), t, 8, VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
            for (j = 1; j <= n; j++) {
                uint64_t d = t[j] > t[j - 1] ? (uint64_t)((t[j] - t[j - 1]) * vk->ts_period) : 0;
                uint64_t h = vk->dprog[f * EVK_DRAW_SLOTS + j - 1];
                uint32_t a = vk->darea[f * EVK_DRAW_SLOTS + j - 1], b = (uint32_t)(h % 256), k;
                for (k = 0; k < 256 && top[(b + k) % 256].h && top[(b + k) % 256].h != h; k++) {
                }
                if (k < 256) {
                    top[(b + k) % 256].h = h;
                    top[(b + k) % 256].ns += d;
                    top[(b + k) % 256].n++;
                    top[(b + k) % 256].px += a;
                }
                tot_ns += d;
                tot_draws++;
                tot_px += a;
            }
        }
        if (tot_draws - last >= 20000) {
            int r, k2;
            fprintf(stderr, "eg-vk drawstamps: %" PRIu64 " draws, %.1f us/draw, %.0f px/draw\n",
                    tot_draws, tot_ns / 1e3 / tot_draws, (double)tot_px / tot_draws);
            for (r = 0; r < 12; r++) {
                int best = -1;
                for (k2 = 0; k2 < 256; k2++) {
                    if (top[k2].n && (best < 0 || top[k2].ns > top[best].ns)) {
                        best = k2;
                    }
                }
                if (best < 0) {
                    break;
                }
                fprintf(stderr, "  %016" PRIx64 " %5.1f%% %7" PRIu64 " draws %6.1f us/draw %8.0f px/draw\n",
                        top[best].h, 100.0 * top[best].ns / tot_ns, top[best].n,
                        top[best].ns / 1e3 / top[best].n, (double)top[best].px / top[best].n);
                top[best].n = 0;        /* printed: drop it */
            }
            memset(top, 0, sizeof(top));
            tot_ns = tot_draws = tot_px = 0;
            last = 0;
        }
    }
    vkResetFences(vk->dev, 1, &vk->fences[f]);
    vkResetCommandBuffer(vk->cmds[f], 0);
    vk->busy[f] = false;
    if (vk->frame_seq[f] > vk->done_seq) {
        vk->done_seq = vk->frame_seq[f];
    }
    if (vk->dirty_fn) {
        for (i = 0; i < vk->fndirty[f]; i++) {
            vk->dirty_fn(vk->opaque, vk->fdirty_off[f][i], vk->fdirty_len[f][i]);
        }
    }
    vk->fndirty[f] = 0;
}

/* the batch was submitted earlier: wait for frame f */
static void frame_wait(EgVk *vk, int f)
{
    struct timespec t0, t1;

    if (!vk->busy[f]) {
        return;
    }
    clock_gettime(CLOCK_MONOTONIC, &t0);
    if (vk->waiter_on) {
        if (__atomic_load_n(&vk->gpu_done_seq, __ATOMIC_ACQUIRE) < vk->frame_seq[f]) {
            if (vk->wait_fn) {
                vk->wait_fn(vk->opaque, true);
            }
            pthread_mutex_lock(&vk->dlock);
            while (vk->gpu_done_seq < vk->frame_seq[f]) {
                pthread_cond_wait(&vk->dcond, &vk->dlock);
            }
            pthread_mutex_unlock(&vk->dlock);
            if (vk->wait_fn) {
                vk->wait_fn(vk->opaque, false);
            }
        }
    } else if (vkGetFenceStatus(vk->dev, vk->fences[f]) != VK_SUCCESS) {
        if (vk->wait_fn) {
            vk->wait_fn(vk->opaque, true);
        }
        vkWaitForFences(vk->dev, 1, &vk->fences[f], VK_TRUE, UINT64_MAX);
        if (vk->wait_fn) {
            vk->wait_fn(vk->opaque, false);
        }
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    vk->stat_wait_ns += (t1.tv_sec - t0.tv_sec) * 1000000000ull + (t1.tv_nsec - t0.tv_nsec);
    if (vk->trace) {
        fprintf(stderr, "eg-vk frame %d waited %.2f ms\n", f,
                (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6);
    }
    frame_done(vk, f);
}

/*
 * Recycle the frames whose batches have completed, oldest first, without
 * waiting.  Returns the sequence number of the last completed batch.
 */
uint64_t eg_vk_poll(EgVk *vk)
{
    int i;

    if (!vk) {
        return 0;
    }
    for (i = 0; i < EVK_FRAMES; i++) {
        int f = (vk->frame + i) % EVK_FRAMES;   /* the oldest submission first */
        if (!vk->busy[f]) {
            continue;
        }
        if (vk->waiter_on ? __atomic_load_n(&vk->gpu_done_seq, __ATOMIC_ACQUIRE) <
                                vk->frame_seq[f]
                          : vkGetFenceStatus(vk->dev, vk->fences[f]) != VK_SUCCESS) {
            break;
        }
        frame_done(vk, f);
    }
    return vk->done_seq;
}

/* the sequence number of the last batch submitted (0: none yet) */
uint64_t eg_vk_submitted(const EgVk *vk)
{
    return vk ? vk->submit_seq : 0;
}

static int range_cmp(const void *a, const void *b)
{
    const VkBufferCopy *x = a, *y = b;
    return x->srcOffset < y->srcOffset ? -1 : x->srcOffset > y->srcOffset;
}

/*
 * The mirror: copy what the batch rendered back to host VRAM at its end,
 * so the guest and the CPU paths see it once the batch completes.  The
 * ranges are sorted and merged (one copy may not have overlapping
 * regions, and many draws render to the same surface).
 */
static void record_copy_back(EgVk *vk)
{
    VkBufferCopy *rg = calloc(vk->ndirty ? vk->ndirty : 1, sizeof(*rg));
    VkMemoryBarrier mb = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_HOST_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT |
                         VK_ACCESS_TRANSFER_WRITE_BIT,
    };
    uint32_t n = 0, m = 0, i;

    for (i = 0; i < (uint32_t)vk->ndirty; i++) {
        if (vk->dirty_off[i] < EVK_GART_SPACE && vk->dirty_off[i] < vk->vram_size) {
            rg[n].srcOffset = rg[n].dstOffset = vk->dirty_off[i];
            uint64_t room = vk->vram_size - vk->dirty_off[i];
            rg[n].size = vk->dirty_len[i] < room ? vk->dirty_len[i] : room;
            n++;
        }
    }
    if (!n) {
        free(rg);
        return;
    }
    qsort(rg, n, sizeof(*rg), range_cmp);
    for (i = 1; i < n; i++) {
        VkBufferCopy *l = &rg[m];
        if (rg[i].srcOffset <= l->srcOffset + l->size) {
            uint64_t e1 = l->srcOffset + l->size, e2 = rg[i].srcOffset + rg[i].size;
            uint64_t end = e1 > e2 ? e1 : e2;
            l->size = end - l->srcOffset;
        } else {
            rg[++m] = rg[i];
        }
    }
    n = m + 1;
    for (i = 0; i < n; i++) {
        vk->b_back_bytes += rg[i].size;
    }
    if (vk->cback) {
        /* only the pages that fragments wrote (dirty_bits) */
        storage_barrier(vk, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                        VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
        vkCmdBindPipeline(vk->cmd, VK_PIPELINE_BIND_POINT_COMPUTE, vk->cback);
        vkCmdBindDescriptorSets(vk->cmd, VK_PIPELINE_BIND_POINT_COMPUTE, vk->clayout, 0, 1,
                                &vk->cdset, 0, NULL);
        for (i = 0; i < n; i++) {
            uint64_t p0 = rg[i].srcOffset >> EVK_PAGE_SHIFT;
            uint64_t p1 = (rg[i].srcOffset + rg[i].size + (1u << EVK_PAGE_SHIFT) - 1) >>
                          EVK_PAGE_SHIFT;
            while (p0 < p1) {
                uint32_t cnt = p1 - p0 > 65535 ? 65535 : (uint32_t)(p1 - p0);
                uint32_t pc[2] = { (uint32_t)p0, cnt };
                vkCmdPushConstants(vk->cmd, vk->clayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, 8, pc);
                vkCmdDispatch(vk->cmd, cnt, 1, 1);
                p0 += cnt;
            }
        }
        /* the cleared bits too: a fragment of the next batch that read a
         * stale set bit would not mark its page again */
        mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        mb.dstAccessMask |= VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(vk->cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT |
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
        free(rg);
        return;
    }
    storage_barrier(vk, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    vkCmdCopyBuffer(vk->cmd, vk->vram_dev.buf, vk->vram.buf, n, rg);
    vkCmdPipelineBarrier(vk->cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb,
                         0, NULL, 0, NULL);
    free(rg);
}

/*
 * End the recording batch and hand it to the GPU without waiting.  The
 * next batch records into the other frame (waiting for it first if the
 * GPU still runs it).  With the VRAM mirror the copy back needs the
 * results, so that path waits (eg_vk_flush).
 */
void eg_vk_submit(EgVk *vk)
{
    VkSubmitInfo si = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
    };
    int i;

    if (!vk || !vk->recording) {
        return;
    }
    if (vk->rendering) {
        vkCmdEndRendering(vk->cmd);
        vk->rendering = false;
    }
    if (vk->tsq) {
        vkCmdWriteTimestamp(vk->cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, vk->tsq,
                            4 * vk->frame + 2);
    }
    if (vk->mirror) {
        record_copy_back(vk);
    }
    storage_barrier(vk, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_READ_BIT);
    if (vk->tsq) {
        vkCmdWriteTimestamp(vk->cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, vk->tsq,
                            4 * vk->frame + 3);
    }
    vkEndCommandBuffer(vk->cmd);
    si.pCommandBuffers = &vk->cmd;
    if (vk->draws_local && vk->ndraws) {
        /* the batch's draw states to device-local memory, ahead of it */
        VkCommandBuffer pre = vk->precmds[vk->frame];
        VkCommandBuffer both[2] = { pre, vk->cmd };
        VkCommandBufferBeginInfo bi = {
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
            .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
        };
        VkBufferCopy rg = {
            .srcOffset = (VkDeviceSize)vk->frame * EVK_DRAW_SLOTS * sizeof(EvkDraw),
            .dstOffset = (VkDeviceSize)vk->frame * EVK_DRAW_SLOTS * sizeof(EvkDraw),
            .size = (VkDeviceSize)vk->ndraws * sizeof(EvkDraw),
        };
        vkBeginCommandBuffer(pre, &bi);
        vkCmdCopyBuffer(pre, vk->draws.buf, vk->draws_dev.buf, 1, &rg);
        vkCmdPipelineBarrier(pre, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1,
                             &(VkMemoryBarrier){ .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                                                 .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                                                 .dstAccessMask = VK_ACCESS_SHADER_READ_BIT },
                             0, NULL, 0, NULL);
        vkEndCommandBuffer(pre);
        si.commandBufferCount = 2;
        si.pCommandBuffers = both;
        vkQueueSubmit(vk->queue, 1, &si, vk->fence);
    } else {
        vkQueueSubmit(vk->queue, 1, &si, vk->fence);
    }
    {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        vk->submit_t[vk->frame] = ts.tv_sec * 1000000000ll + ts.tv_nsec;
    }
    vk->busy[vk->frame] = true;
    pthread_mutex_lock(&vk->dlock);
    vk->frame_seq[vk->frame] = ++vk->submit_seq;
    vk->pub_seq = vk->submit_seq;
    pthread_cond_broadcast(&vk->dcond);
    pthread_mutex_unlock(&vk->dlock);
    vk->recording = false;
    vk->stat_batches++;
    if (vk->trace) {
        fprintf(stderr, "eg-vk batch: %u draws, %.1f Mpx, gart %u, up %u/%.2f MB, back %.2f MB:%s\n",
                vk->ndraws, vk->b_area / 1e6, vk->b_gart, vk->b_up_n, vk->b_up_bytes / 1e6,
                vk->b_back_bytes / 1e6, vk->b_progs);
        vk->b_up_n = 0;
        vk->b_up_bytes = 0;
        vk->b_back_bytes = 0;
        vk->b_area = 0;
        vk->b_gart = 0;
        vk->b_progs[0] = 0;
    }
    /* the display sees these ranges change when the batch completes */
    for (i = 0; i < vk->ndirty; i++) {
        if (vk->dirty_off[i] < EVK_GART_SPACE) {
            int f = vk->frame, n = vk->fndirty[f]++;
            vk->fdirty_off[f][n] = vk->dirty_off[i];
            vk->fdirty_len[f][n] = vk->dirty_len[i];
        }
    }
    vk->ndirty = 0;
    vk->frame = (vk->frame + 1) % EVK_FRAMES;
    vk->cmd = vk->cmds[vk->frame];
    vk->fence = vk->fences[vk->frame];
}

uint64_t eg_vk_flush(EgVk *vk)
{
    int i;

    if (!vk || !eg_vk_pending(vk)) {
        return 0;
    }
    eg_vk_submit(vk);
    for (i = 0; i < EVK_FRAMES; i++) {
        frame_wait(vk, (vk->frame + i) % EVK_FRAMES);
    }
    return 0;                       /* the pixels are in the ZPASS slots */
}

/*
 * Occlusion queries: draws add their passing pixels to counts[zslot].  A
 * ZPASS_DONE event closes the slot (the device takes it once the batch has
 * completed) and moves new draws to the next slot.
 */
uint32_t eg_vk_zpass_slot(const EgVk *vk)
{
    return vk ? vk->zslot : 0;
}

void eg_vk_zpass_set(EgVk *vk, uint32_t slot)
{
    if (vk) {
        ((volatile uint32_t *)vk->counts.map)[slot % EVK_ZPASS_SLOTS] = 0;
        vk->zslot = slot % EVK_ZPASS_SLOTS;
    }
}

uint32_t eg_vk_zpass_take(EgVk *vk, uint32_t slot)
{
    uint32_t v;

    if (!vk) {
        return 0;
    }
    v = ((volatile uint32_t *)vk->counts.map)[slot % EVK_ZPASS_SLOTS];
    ((volatile uint32_t *)vk->counts.map)[slot % EVK_ZPASS_SLOTS] = 0;
    return v;
}

static void update_desc(EgVk *vk, uint32_t binding, VkBuffer buf, VkDeviceSize range)
{
    VkDescriptorBufferInfo bi = { buf, 0, range };
    VkWriteDescriptorSet w = {
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = vk->dset,
        .dstBinding = binding, .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &bi,
    };
    vkUpdateDescriptorSets(vk->dev, 1, &w, 0, NULL);
}

bool eg_vk_set_sysram(EgVk *vk, void *ram, uint64_t size, char *err,
                        size_t errlen)
{
    VkPhysicalDeviceProperties pr;

    eg_vk_flush(vk);
    if (!import_host(vk, &vk->sysram, ram, size, err, errlen)) {
        return false;
    }
    vkGetPhysicalDeviceProperties(vk->phys, &pr);
    vk->sysram_range = size < pr.limits.maxStorageBufferRange ? size
                       : (pr.limits.maxStorageBufferRange & ~(VkDeviceSize)4095);
    update_desc(vk, 4, vk->sysram.buf, vk->sysram_range);
    update_desc(vk, 7, vk->sysram.buf, vk->sysram_range);
    return true;
}

uint32_t *eg_vk_gart_table(EgVk *vk, uint32_t npages)
{
    if (!vk || !vk->sysram.buf) {
        return NULL;
    }
    if (npages > vk->gart_pages) {
        EvkBuf nb = { 0 };
        char err[128];
        eg_vk_flush(vk);
        if (!host_buf(vk, &nb, (VkDeviceSize)npages * 4 + 4096,
                      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, err, sizeof(err))) {
            return NULL;
        }
        memset(nb.map, 0xff, (size_t)npages * 4);
        if (vk->gart.map && vk->gart_pages) {
            memcpy(nb.map, vk->gart.map, (size_t)vk->gart_pages * 4);
        }
        vkDestroyBuffer(vk->dev, vk->gart.buf, NULL);
        vkFreeMemory(vk->dev, vk->gart.mem, NULL);
        vk->gart = nb;
        vk->gart_pages = npages;
        update_desc(vk, 5, vk->gart.buf, VK_WHOLE_SIZE);
    }
    return vk->gart.map;
}

/* ns spent waiting for frames to complete (GPU backpressure) */
/*
 * EG_VK_TIMESTAMPS: GPU time of the completed batches, the part of it in
 * the copy-back, their submit-to-recycle time, and their count
 */
void eg_vk_gpu_times(const EgVk *vk, uint64_t *gpu_ns, uint64_t *back_ns, uint64_t *lat_ns,
                     uint64_t *n)
{
    *gpu_ns = vk ? vk->stat_gpu_ns : 0;
    *back_ns = vk ? vk->stat_gpu_back_ns : 0;
    *lat_ns = vk ? vk->stat_lat_ns : 0;
    *n = vk ? vk->stat_timed : 0;
}

uint64_t eg_vk_wait_ns(const EgVk *vk)
{
    return vk ? vk->stat_wait_ns : 0;
}

void eg_vk_stats(const EgVk *vk, uint64_t *draws, uint64_t *batches,
                   uint64_t *pipelines, uint64_t *barriers)
{
    *barriers = vk ? vk->stat_barriers : 0;
    *draws = vk ? vk->stat_draws : 0;
    *batches = vk ? vk->stat_batches : 0;
    *pipelines = vk ? vk->stat_pipes : 0;
}
