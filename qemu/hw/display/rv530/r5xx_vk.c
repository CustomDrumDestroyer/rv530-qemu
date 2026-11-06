/*
 * R5xx 3D engine on Vulkan, host side.  See r5xx_vk.h for the design.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <inttypes.h>
#include <time.h>
#include <pthread.h>
#include <dirent.h>
#include <vulkan/vulkan.h>
#include <shaderc/shaderc.h>
#include "r5xx_vk.h"

#define RVK_VP              8192        /* viewport / render area size */
#define RVK_DRAW_SLOTS      4096        /* RvkDraw entries per batch */
#define RVK_VERT_SLOTS      (1u << 17)  /* RvkVertex entries per batch */
#define RVK_MAX_DIRTY       8192
#define RVK_PAGE_SHIFT     12
/* batches upload all stale pages up front only when they are few */
#define RVK_EAGER_UPLOAD   (16ull << 20)
/* pages a batch may render to and merge back: 32 MB, two 2560x1600
 * ARGB8888 surfaces */
#define RVK_WB_PAGES       8192
/* staging for the uploads of one batch (see stage_copies) */
#define RVK_STAGE_BYTES    (64ull << 20)
#define RVK_PAGE_MASK      ((1ull << RVK_PAGE_SHIFT) - 1)
#define MIN_U64(a, b)      ((a) < (b) ? (a) : (b))

#define RVK_PIPE_BUCKETS    1024

_Static_assert(sizeof(RvkSampler) == 336, "RvkSampler layout");
_Static_assert(sizeof(RvkSurf) == 48, "RvkSurf layout");
_Static_assert(sizeof(RvkDraw) == 10112, "RvkDraw layout");
_Static_assert(sizeof(RvkVertex) == 416, "RvkVertex layout");
_Static_assert(RVK_M_COUNT <= RVK_MISC_WORDS, "misc words");

enum { RVK_PIPE_PENDING, RVK_PIPE_READY, RVK_PIPE_FAILED };

typedef struct RvkPipe {
    uint64_t hash;
    VkPipeline pipe;
    int state;                  /* RVK_PIPE_*, written by the compiler thread */
    char *src;                  /* GLSL waiting for compilation */
    uint32_t *spv;              /* or SPIR-V from the disk cache */
    size_t spv_bytes;
    struct RvkPipe *next;       /* hash chain */
    struct RvkPipe *qnext;      /* compile queue */
} RvkPipe;

#define RVK_COMPILERS 6

typedef struct RvkBuf {
    VkBuffer buf;
    VkDeviceMemory mem;
    void *map;
    VkDeviceSize size;
} RvkBuf;

struct R5xxVk {
    VkInstance inst;
    VkPhysicalDevice phys;
    VkDevice dev;
    VkQueue queue;
    uint32_t qfam;
    VkCommandPool pool;
    VkCommandBuffer cmd;
    VkFence fence;
    /* mirror uploads submitted between batches (r5xx_vk_upload_kick) */
    VkCommandBuffer upcmd;
    VkFence upfence;
    bool up_inflight, up_barrier;
    VkDescriptorSetLayout dsl;
    VkDescriptorPool dpool;
    VkDescriptorSet dset;
    VkPipelineLayout layout;
    VkShaderModule vert;
    VkPipelineCache pcache;

    RvkBuf vram;                /* imported guest VRAM */
    RvkBuf draws, verts, counts;
    RvkBuf sysram;              /* imported guest RAM (GART targets), optional */
    /* device-local mirror of VRAM: shaders use it; uploads of CPU-dirty
     * pages at batch start, downloads of rendered ranges at flush */
    RvkBuf vram_dev;
    bool mirror;
    /* VRAM pages changed outside the GPU since they were last copied to
     * the mirror (4 KiB granularity) */
    uint64_t *stale;
    uint64_t stale_pages;
    /* pages a draw of the current batch renders to: within the render
     * ranges the mirror is newer than host VRAM until the batch is read
     * back, so mid-batch uploads there skip those bytes */
    uint64_t *batch_wr;
    /*
     * Uploads to the mirror copy from 'stage', a snapshot of host VRAM taken
     * when the copy is recorded.  A copy straight from host VRAM reads it
     * when the GPU runs the batch: if the guest changed a texture between
     * two draws of one batch, the earlier draw sampled the new texels too.
     */
    RvkBuf stage;
    uint64_t stage_used;
    /*
     * Merged write-back.  Copying whole render bands back to host
     * VRAM reverted what other vCPUs wrote there while the batch ran (the
     * band holds every byte of its rows, and the BQL is released during
     * long GPU waits).  Instead each VRAM page a batch renders to is copied
     * from the mirror into wb_before just before its first draw and into
     * wb_after at flush; only the bytes that differ are written back.
     */
    bool wb_merge;              /* false: RV530_VK_WB_COPY, the old copy */
    RvkBuf wb_before, wb_after; /* slot i at i << RVK_PAGE_SHIFT */
    uint32_t *wb_slot;          /* page -> slot + 1 in this batch, 0: none */
    uint32_t wb_page[RVK_WB_PAGES];
    uint32_t wb_n;
    uint64_t wb_kept, wb_kept_batches;
    /* RV530_VK_CHECK_MIRROR: host VRAM as last copied to the mirror
     * or read back from it; a page that differs while it is not stale
     * changed without being reported */
    uint8_t *shadow;
    uint8_t *vram_host;
    uint64_t *diff;
    int check_mode;             /* 1 report, 2 report and re-upload */
    int64_t check_next_ns, check_ms;
    uint64_t check_scans, check_missed, check_diff;
    uint64_t check_reverted, check_revert_batches;
    RvkBuf gart;                /* GART page -> sysram page */
    uint32_t gart_pages;
    VkDeviceSize sysram_range;
    uint64_t vram_size;

    shaderc_compiler_t sc;
    shaderc_compile_options_t sco;
    char *frag_prefix;          /* #version + defines */
    bool has_f64;               /* shaderFloat64 (not on Apple GPUs) */
    bool portability;           /* VK_KHR_portability_subset (MoltenVK) */
    /* persistent SPIR-V cache (r5xx_vk_set_cache_dir): <dir>/<tpl>-<prog>.spv
     * plus the order in which the guest first used each program */
    char *cache_dir;
    uint64_t tpl_hash;
    FILE *order;
    int64_t wait_ns;            /* compile waiting in the current batch */
    int new_pipes;              /* created since the pipeline cache was saved */
    int64_t pcache_saved;       /* CLOCK_MONOTONIC s of the last save */

    RvkPipe *pipes[RVK_PIPE_BUCKETS];
    VkPipeline bound;

    /* asynchronous pipeline compilation */
    pthread_t compilers[RVK_COMPILERS];
    int ncompilers;
    pthread_mutex_t qlock;
    pthread_cond_t qcond;
    RvkPipe *qhead, *qtail;
    bool quit, sync_compile;
    bool barrier_each;          /* RV530_VK_BARRIER_EACH: debug */

    /* current batch */
    bool recording, rendering;
    uint32_t ndraws, nverts;
    uint64_t dirty_off[RVK_MAX_DIRTY], dirty_len[RVK_MAX_DIRTY];
    int ndirty;
    /* hazards since the last barrier: ranges written (with the surface
     * signature that wrote them) and ranges sampled */
    int nw, nr;
    uint64_t w_off[RVK_MAX_DIRTY], w_len[RVK_MAX_DIRTY], w_sig[RVK_MAX_DIRTY];
    uint64_t r_off[RVK_MAX_DIRTY], r_len[RVK_MAX_DIRTY];

    R5xxVkDirtyFn dirty_fn;
    R5xxVkWaitFn wait_fn;
    void *opaque;

    uint64_t stat_draws, stat_batches, stat_pipes, stat_barriers, stat_upload_bytes;
    /* RV530_VK_TRACE: per-batch log */
    bool trace;
    uint64_t b_area;
    uint32_t b_gart, b_barriers;
    uint64_t b_up_bytes;
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

static int find_mem_type(R5xxVk *vk, uint32_t bits, VkMemoryPropertyFlags want)
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

static bool host_buf(R5xxVk *vk, RvkBuf *b, VkDeviceSize size,
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

/* A host-visible buffer the CPU reads back: cached memory when there is
 * some (uncached reads crawl on discrete GPUs) */
static bool readback_buf(R5xxVk *vk, RvkBuf *b, VkDeviceSize size)
{
    VkBufferCreateInfo bi = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = size,
        .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    VkMemoryRequirements mr;
    VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    int mt;

    b->size = size;
    if (vkCreateBuffer(vk->dev, &bi, NULL, &b->buf) != VK_SUCCESS) {
        return false;
    }
    vkGetBufferMemoryRequirements(vk->dev, b->buf, &mr);
    mt = find_mem_type(vk, mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                       VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
                       VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
    if (mt < 0) {
        mt = find_mem_type(vk, mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                           VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    }
    ai.allocationSize = mr.size;
    ai.memoryTypeIndex = (uint32_t)mt;
    return mt >= 0 &&
           vkAllocateMemory(vk->dev, &ai, NULL, &b->mem) == VK_SUCCESS &&
           vkBindBufferMemory(vk->dev, b->buf, b->mem, 0) == VK_SUCCESS &&
           vkMapMemory(vk->dev, b->mem, 0, VK_WHOLE_SIZE, 0, &b->map) == VK_SUCCESS;
}

static bool import_host(R5xxVk *vk, RvkBuf *out, void *ptr, uint64_t size,
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

static VkShaderModule spv_module(R5xxVk *vk, const uint32_t *words, size_t bytes,
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
static uint32_t *glsl_to_spv(R5xxVk *vk, const char *src, shaderc_shader_kind kind,
                             size_t *bytes, char *err, size_t errlen)
{
    shaderc_compilation_result_t r =
        shaderc_compile_into_spv(vk->sc, src, strlen(src), kind, "rv530",
                                 "main", vk->sco);
    uint32_t *words = NULL;

    if (shaderc_result_get_compilation_status(r) != shaderc_compilation_status_success) {
        set_err(err, errlen, "%s", shaderc_result_get_error_message(r));
        if (getenv("RV530_VK_DUMP_FAILED")) {
            FILE *f = fopen(getenv("RV530_VK_DUMP_FAILED"), "w");
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

static VkShaderModule compile(R5xxVk *vk, const char *src, shaderc_shader_kind kind,
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

static const char *vert_src =
    "#version 460\n"
    "struct Vtx { vec4 pos; float z; uint flatmask, front; float zoff; vec4 s[12]; vec4 f[12]; };\n"
    "layout(std430, set = 0, binding = 2) readonly buffer VtxBuf { Vtx verts[]; };\n"
    "layout(push_constant) uniform PC { uint draw; } pc;\n"
    "layout(location = 0) out vec4 v_s[12];\n"
    "layout(location = 12) flat out vec4 v_f[12];\n"
    "layout(location = 24) flat out uvec3 v_misc;\n"
    "layout(location = 25) noperspective out float v_z;\n"
    "void main() {\n"
    "  Vtx v = verts[gl_VertexIndex];\n"
    "  float w = max(v.pos.w, 1e-20);\n"
    "  gl_Position = vec4((v.pos.x * (2.0 / 8192.0) - 1.0) * w,\n"
    "                     (v.pos.y * (2.0 / 8192.0) - 1.0) * w, 0.5 * w, w);\n"
    "  for (int i = 0; i < 12; i++) { v_s[i] = v.s[i]; v_f[i] = v.f[i]; }\n"
    "  v_misc = uvec3(v.flatmask, v.front, floatBitsToUint(v.zoff));\n"
    "  v_z = v.z;\n"
    "}\n";

static char *frag_source(R5xxVk *vk, const R500UsDecoded *prog)
{
    char *us = r500_us_to_glsl(prog, NULL);
    char *src;

    if (!us) {
        return NULL;
    }
    src = rvk_frag_source(vk->frag_prefix, us);
    free(us);
    return src;
}

static char *cache_path(R5xxVk *vk, uint64_t hash)
{
    size_t n = strlen(vk->cache_dir) + 48;
    char *path = malloc(n);

    snprintf(path, n, "%s/%016llx-%016llx.spv", vk->cache_dir,
             (unsigned long long)vk->tpl_hash, (unsigned long long)hash);
    return path;
}

/* write via a temporary file so a reader never sees a partial file */
static void cache_store(R5xxVk *vk, uint64_t hash, const uint32_t *words, size_t bytes)
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

static uint32_t *cache_load(R5xxVk *vk, uint64_t hash, size_t *bytes)
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
static VkPipeline make_pipeline(R5xxVk *vk, RvkPipe *p, char *err, size_t errlen)
{
    VkShaderModule frag = VK_NULL_HANDLE;
    VkPipeline pipe = VK_NULL_HANDLE;
    uint32_t *words = p->spv;
    size_t bytes = p->spv_bytes;

    p->spv = NULL;
    if (!words && p->src) {
        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        words = glsl_to_spv(vk, p->src, shaderc_fragment_shader, &bytes, err, errlen);
        clock_gettime(CLOCK_MONOTONIC, &t1);
        if (getenv("RV530_VK_TIMING")) {
            fprintf(stderr, "rv530-vk:   glsl->spirv %.1f ms (%zu bytes of GLSL)\n",
                    (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6,
                    strlen(p->src));
        }
        if (getenv("RV530_VK_SAVE_GLSL")) {
            FILE *f = fopen(getenv("RV530_VK_SAVE_GLSL"), "w");
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
        return VK_NULL_HANDLE;
    }

    {
        VkPipelineShaderStageCreateInfo st[2] = {
            { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
              .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vk->vert, .pName = "main" },
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
        VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
        VkPipelineDynamicStateCreateInfo ds = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
            .dynamicStateCount = 2, .pDynamicStates = dyn,
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
        }
    }
    vkDestroyShaderModule(vk->dev, frag, NULL);
    __atomic_add_fetch(&vk->stat_pipes, 1, __ATOMIC_RELAXED);
    return pipe;
}

static void pcache_save(R5xxVk *vk);

static void compile_entry(R5xxVk *vk, RvkPipe *p)
{
    char err[1024];
    struct timespec t0, t1;
    VkPipeline pipe;

    clock_gettime(CLOCK_MONOTONIC, &t0);
    pipe = make_pipeline(vk, p, err, sizeof(err));
    clock_gettime(CLOCK_MONOTONIC, &t1);
    if (getenv("RV530_VK_TIMING")) {
        fprintf(stderr, "rv530-vk: pipeline %016llx %.1f ms%s%s\n",
                (unsigned long long)p->hash,
                (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6,
                pipe ? "" : " failed: ", pipe ? "" : err);
    }
    p->pipe = pipe;
    __atomic_store_n(&p->state, pipe ? RVK_PIPE_READY : RVK_PIPE_FAILED,
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
    R5xxVk *vk = opaque;

    for (;;) {
        RvkPipe *p;
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
 * RV530_VK_COMPILE_BUDGET_MS (400); one draw waits at most
 * RV530_VK_COMPILE_WAIT_MS (250).
 */
static VkPipeline wait_pipeline(R5xxVk *vk, RvkPipe *p, char *err, size_t errlen)
{
    struct timespec t0, now, nap = { 0, 200 * 1000 };
    int64_t limit, waited = 0;
    int state;

    if (compile_wait_ms < 0) {
        const char *e = getenv("RV530_VK_COMPILE_WAIT_MS");
        const char *b = getenv("RV530_VK_COMPILE_BUDGET_MS");
        compile_wait_ms = e ? atoi(e) : 250;
        compile_budget_ms = b ? atoi(b) : 400;
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
        if (state != RVK_PIPE_PENDING) {
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
    if (state == RVK_PIPE_PENDING) {
        set_err(err, errlen, "compiling");
        return VK_NULL_HANDLE;
    }
    if (state == RVK_PIPE_READY) {
        return p->pipe;
    }
    set_err(err, errlen, "fragment program failed to compile");
    return VK_NULL_HANDLE;
}

/* Look up (or start compiling) the pipeline for a fragment program.
 * VK_NULL_HANDLE with a reason when it is not usable (yet). */
static VkPipeline get_pipeline(R5xxVk *vk, const R500UsDecoded *prog,
                               uint64_t hash, char *err, size_t errlen)
{
    RvkPipe **head = &vk->pipes[hash % RVK_PIPE_BUCKETS], *p;

    for (p = *head; p; p = p->next) {
        if (p->hash == hash) {
            switch (__atomic_load_n(&p->state, __ATOMIC_ACQUIRE)) {
            case RVK_PIPE_READY:
                return p->pipe;
            case RVK_PIPE_PENDING:
                return wait_pipeline(vk, p, err, errlen);
            default:
                set_err(err, errlen, "fragment program failed to compile");
                return VK_NULL_HANDLE;
            }
        }
    }
    p = calloc(1, sizeof(*p));
    p->hash = hash;
    p->state = RVK_PIPE_PENDING;
    p->src = frag_source(vk, prog);
    if (vk->order && p->src) {
        fprintf(vk->order, "%016llx\n", (unsigned long long)hash);
        fflush(vk->order);
    }
    p->next = *head;
    *head = p;
    if (!p->src) {
        p->state = RVK_PIPE_FAILED;
        set_err(err, errlen, "fragment program not translatable (flow control)");
        return VK_NULL_HANDLE;
    }
    if (vk->sync_compile || !vk->ncompilers) {
        compile_entry(vk, p);
        if (p->state == RVK_PIPE_READY) {
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

static bool pick_device(R5xxVk *vk, char *err, size_t errlen)
{
    uint32_t n = 0, i, best = UINT32_MAX;
    VkPhysicalDevice *pd;
    const char *want = getenv("RV530_VK_DEVICE");
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
        VkPhysicalDeviceFragmentShaderInterlockFeaturesEXT il = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADER_INTERLOCK_FEATURES_EXT,
        };
        VkPhysicalDeviceFeatures2 f2 = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &il,
        };
        int score;

        vkGetPhysicalDeviceProperties(pd[i], &pr);
        vkGetPhysicalDeviceFeatures2(pd[i], &f2);
        if (pr.apiVersion < VK_API_VERSION_1_3 ||
            !has_ext(pd[i], VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME) ||
            !has_ext(pd[i], VK_EXT_FRAGMENT_SHADER_INTERLOCK_EXTENSION_NAME) ||
            !il.fragmentShaderPixelInterlock ||
            !f2.features.fragmentStoresAndAtomics) {
            continue;
        }
        score = pr.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 3 :
                pr.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 2 : 1;
        /* float64 only makes depth_to_fixed simpler; prefer, not require */
        score = score * 2 + (f2.features.shaderFloat64 ? 1 : 0);
        if (want && (uint32_t)atoi(want) == i) {
            score = 100;
        }
        if (score > best_score) {
            best_score = score;
            best = i;
        }
    }
    if (best == UINT32_MAX) {
        free(pd);
        set_err(err, errlen, "no Vulkan 1.3 device with external_memory_host, "
                "pixel interlock and fragment stores");
        return false;
    }
    vk->phys = pd[best];
    free(pd);
    {
        VkPhysicalDeviceFeatures f;
        vkGetPhysicalDeviceFeatures(vk->phys, &f);
        vk->has_f64 = f.shaderFloat64;
    }
    vk->portability = has_ext(vk->phys, "VK_KHR_portability_subset");
    return true;
}

R5xxVk *r5xx_vk_create(void *vram, uint64_t vram_size, R5xxVkDirtyFn dirty,
                       void *opaque, char *err, size_t errlen)
{
    R5xxVk *vk = calloc(1, sizeof(*vk));
    VkApplicationInfo app = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "qemu-rv530", .apiVersion = VK_API_VERSION_1_3,
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
    if (!r5xx_vk_metal_usable(err, errlen)) {
        free(vk);
        return NULL;
    }
    /*
     * MoltenVK compiles shaders with Metal fast math by default, which may
     * reassociate the exact float arithmetic of depth_to_fixed and drops
     * NaN/Inf semantics the R500 program translation relies on.
     */
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
            "VK_KHR_portability_subset",    /* must be enabled where offered */
        };
        VkPhysicalDeviceFragmentShaderInterlockFeaturesEXT il = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADER_INTERLOCK_FEATURES_EXT,
            .fragmentShaderPixelInterlock = VK_TRUE,
        };
        VkPhysicalDeviceVulkan13Features f13 = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
            .pNext = &il, .dynamicRendering = VK_TRUE,
        };
        VkPhysicalDeviceFeatures2 f2 = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &f13,
            .features = { .fragmentStoresAndAtomics = VK_TRUE, .shaderFloat64 = vk->has_f64 },
        };
        VkDeviceQueueCreateInfo qi = {
            .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
            .queueFamilyIndex = vk->qfam, .queueCount = 1, .pQueuePriorities = &prio,
        };
        VkDeviceCreateInfo di = {
            .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &f2,
            .queueCreateInfoCount = 1, .pQueueCreateInfos = &qi,
            .enabledExtensionCount = vk->portability ? 3 : 2,
            .ppEnabledExtensionNames = exts,
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
    vk->vram_host = vram;
    if (!host_buf(vk, &vk->draws, (VkDeviceSize)RVK_DRAW_SLOTS * sizeof(RvkDraw),
                  VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, err, errlen) ||
        !host_buf(vk, &vk->verts, (VkDeviceSize)RVK_VERT_SLOTS * sizeof(RvkVertex),
                  VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, err, errlen) ||
        !host_buf(vk, &vk->counts, 4096, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                  err, errlen) ||
        !host_buf(vk, &vk->gart, 4096, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                  err, errlen)) {
        goto fail;
    }
    memset(vk->counts.map, 0, 4096);
    if (!getenv("RV530_VK_NO_MIRROR")) {
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
                vk->stale = calloc(((vram_size >> RVK_PAGE_SHIFT) + 63) / 64, 8);
                vk->batch_wr = calloc(((vram_size >> RVK_PAGE_SHIFT) + 63) / 64, 8);
                if (!host_buf(vk, &vk->stage, RVK_STAGE_BYTES, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                              NULL, 0)) {
                    vk->stage.map = NULL;   /* copy from host VRAM, as before */
                }
                if (!getenv("RV530_VK_WB_COPY") &&
                    readback_buf(vk, &vk->wb_before, (VkDeviceSize)RVK_WB_PAGES << RVK_PAGE_SHIFT) &&
                    readback_buf(vk, &vk->wb_after, (VkDeviceSize)RVK_WB_PAGES << RVK_PAGE_SHIFT)) {
                    vk->wb_slot = calloc(vram_size >> RVK_PAGE_SHIFT, sizeof(uint32_t));
                    vk->wb_merge = vk->wb_slot != NULL;
                }
                if (getenv("RV530_VK_CHECK_MIRROR")) {
                    const char *ms = getenv("RV530_VK_CHECK_MS");
                    vk->check_mode = atoi(getenv("RV530_VK_CHECK_MIRROR")) == 2 ? 2 : 1;
                    vk->check_ms = ms ? atoi(ms) : 250;
                    vk->shadow = malloc(vram_size);
                    vk->diff = calloc(((vram_size >> RVK_PAGE_SHIFT) + 63) / 64, 8);
                }
            }
        }
    }

    {
        VkDescriptorSetLayoutBinding b[8];
        VkDescriptorSetLayoutCreateInfo li = {
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
            .bindingCount = 8, .pBindings = b,
        };
        VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 8 };
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
        VkDescriptorBufferInfo bi[8] = {
            { shader_vram, 0, VK_WHOLE_SIZE }, { vk->draws.buf, 0, VK_WHOLE_SIZE },
            { vk->verts.buf, 0, VK_WHOLE_SIZE }, { vk->counts.buf, 0, VK_WHOLE_SIZE },
            { vk->counts.buf, 0, VK_WHOLE_SIZE }, { vk->gart.buf, 0, VK_WHOLE_SIZE },
            { shader_vram, 0, VK_WHOLE_SIZE }, { vk->counts.buf, 0, VK_WHOLE_SIZE },
        };
        VkWriteDescriptorSet w[8];
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

        for (i = 0; i < 8; i++) {
            b[i] = (VkDescriptorSetLayoutBinding) {
                .binding = i, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                .descriptorCount = 1,
                .stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
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
        ai.descriptorPool = vk->dpool;
        if (vkAllocateDescriptorSets(vk->dev, &ai, &vk->dset) != VK_SUCCESS) {
            set_err(err, errlen, "descriptor set allocation failed");
            goto fail;
        }
        for (i = 0; i < 8; i++) {
            w[i].dstSet = vk->dset;
        }
        vkUpdateDescriptorSets(vk->dev, 8, w, 0, NULL);
        if (vkCreateCommandPool(vk->dev, &cpi, NULL, &vk->pool) != VK_SUCCESS) {
            set_err(err, errlen, "command pool creation failed");
            goto fail;
        }
        cai.commandPool = vk->pool;
        if (vkAllocateCommandBuffers(vk->dev, &cai, &vk->cmd) != VK_SUCCESS ||
            vkAllocateCommandBuffers(vk->dev, &cai, &vk->upcmd) != VK_SUCCESS ||
            vkCreateFence(vk->dev, &fi, NULL, &vk->fence) != VK_SUCCESS ||
            vkCreateFence(vk->dev, &fi, NULL, &vk->upfence) != VK_SUCCESS ||
            vkCreatePipelineCache(vk->dev, &pci, NULL, &vk->pcache) != VK_SUCCESS) {
            set_err(err, errlen, "command buffer / fence creation failed");
            goto fail;
        }
    }

    vk->sc = shaderc_compiler_initialize();
    vk->sco = shaderc_compile_options_initialize();
    shaderc_compile_options_set_target_env(vk->sco, shaderc_target_env_vulkan,
                                           shaderc_env_version_vulkan_1_3);
    shaderc_compile_options_set_optimization_level(vk->sco,
                                                   shaderc_optimization_level_zero);
    vk->frag_prefix = rvk_frag_prefix(vk->has_f64);
    vk->vert = compile(vk, vert_src, shaderc_vertex_shader, err, errlen);
    if (!vk->vert) {
        goto fail;
    }
    pthread_mutex_init(&vk->qlock, NULL);
    pthread_cond_init(&vk->qcond, NULL);
    vk->sync_compile = getenv("RV530_VK_SYNC_COMPILE") != NULL;
    vk->barrier_each = getenv("RV530_VK_BARRIER_EACH") != NULL;
    vk->trace = getenv("RV530_VK_TRACE") != NULL;
    for (i = 0; i < RVK_COMPILERS && !vk->sync_compile; i++) {
        if (pthread_create(&vk->compilers[i], NULL, compiler_thread, vk) == 0) {
            vk->ncompilers++;
        }
    }
    {
        VkPhysicalDeviceProperties pr;
        vkGetPhysicalDeviceProperties(vk->phys, &pr);
        fprintf(stderr, "rv530: Vulkan backend on %s%s\n", pr.deviceName,
                vk->has_f64 ? "" : " (no float64: emulated depth math)");
    }
    /*
     * The driver's first pipeline creation takes ~300 ms of one-time setup,
     * during which it also holds back queue submissions from other threads.
     * Pay it here, before the guest runs, instead of inside a CP run.
     */
    {
        static const char warm_src[] =
            "#version 450\nlayout(location = 0) out vec4 o;\nvoid main() { o = vec4(0); }\n";
        RvkPipe warm = { .src = strdup(warm_src) };
        char werr[256];
        VkPipeline wp = make_pipeline(vk, &warm, werr, sizeof(werr));
        if (wp) {
            vkDestroyPipeline(vk->dev, wp, NULL);
        }
    }
    return vk;

fail:
    r5xx_vk_destroy(vk);
    return NULL;
}

void r5xx_vk_destroy(R5xxVk *vk)
{
    int i;

    if (!vk) {
        return;
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
        for (i = 0; i < RVK_PIPE_BUCKETS; i++) {
            RvkPipe *p = vk->pipes[i];
            while (p) {
                RvkPipe *n = p->next;
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
        if (vk->fence) vkDestroyFence(vk->dev, vk->fence, NULL);
        if (vk->upfence) vkDestroyFence(vk->dev, vk->upfence, NULL);
        if (vk->pool) vkDestroyCommandPool(vk->dev, vk->pool, NULL);
        if (vk->dpool) vkDestroyDescriptorPool(vk->dev, vk->dpool, NULL);
        if (vk->layout) vkDestroyPipelineLayout(vk->dev, vk->layout, NULL);
        if (vk->dsl) vkDestroyDescriptorSetLayout(vk->dev, vk->dsl, NULL);
        {
            RvkBuf *bufs[10] = { &vk->vram, &vk->draws, &vk->verts, &vk->counts,
                                 &vk->sysram, &vk->gart, &vk->vram_dev,
                                 &vk->wb_before, &vk->wb_after, &vk->stage };
            for (i = 0; i < 10; i++) {
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
    free(vk->wb_slot);
    free(vk->shadow);
    free(vk->diff);
    free(vk->batch_wr);
    if (vk->inst) vkDestroyInstance(vk->inst, NULL);
    free(vk);
}

/* ------------------------------------------------------------------ */
/* Batches                                                             */
/* ------------------------------------------------------------------ */

static void begin_rendering(R5xxVk *vk)
{
    VkRenderingInfo ri = {
        .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
        .renderArea = { { 0, 0 }, { RVK_VP, RVK_VP } },
        .layerCount = 1,
    };
    VkViewport vp = { 0, 0, RVK_VP, RVK_VP, 0.0f, 1.0f };

    vkCmdBeginRendering(vk->cmd, &ri);
    vkCmdSetViewport(vk->cmd, 0, 1, &vp);
    vk->rendering = true;
    vk->bound = VK_NULL_HANDLE;
}

static void storage_barrier(R5xxVk *vk, VkPipelineStageFlags dst_stage,
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


typedef struct RvkCopies {
    VkBufferCopy *rg;
    uint32_t n, cap;
    uint64_t bytes;
} RvkCopies;

static void copies_add(RvkCopies *c, uint64_t off, uint64_t len)
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
static void copies_add_unrendered(R5xxVk *vk, uint64_t a, uint64_t b, RvkCopies *c)
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
static void stale_take_ex(R5xxVk *vk, uint64_t off, uint64_t len, RvkCopies *c,
                          const uint64_t *rendered)
{
    uint64_t pg, end;

    if (!vk->stale_pages || off >= vk->vram_size || !len) {
        return;
    }
    if (off + len > vk->vram_size) {
        len = vk->vram_size - off;
    }
    end = (off + len - 1) >> RVK_PAGE_SHIFT;
    for (pg = off >> RVK_PAGE_SHIFT; pg <= end; pg++) {
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
            copies_add_unrendered(vk, pg << RVK_PAGE_SHIFT,
                                  (pg + 1) << RVK_PAGE_SHIFT, c);
            /*
             * The guest may have written inside the render ranges too, and
             * those bytes were not copied.  Leave the page stale, so the
             * next batch uploads all of it, after the write-back has put
             * the GPU's bytes in host VRAM.
             */
            vk->stale[pg >> 6] |= bit;
            vk->stale_pages++;
        } else {
            copies_add(c, pg << RVK_PAGE_SHIFT, 1ull << RVK_PAGE_SHIFT);
        }
    }
}

static void stale_take(R5xxVk *vk, uint64_t off, uint64_t len, RvkCopies *c)
{
    stale_take_ex(vk, off, len, c, NULL);
}

static void batch_wr_mark(R5xxVk *vk, uint64_t off, uint64_t len)
{
    uint64_t pg, end;

    if (off >= vk->vram_size || !len) {
        return;
    }
    if (off + len > vk->vram_size) {
        len = vk->vram_size - off;
    }
    end = (off + len - 1) >> RVK_PAGE_SHIFT;
    for (pg = off >> RVK_PAGE_SHIFT; pg <= end; pg++) {
        vk->batch_wr[pg >> 6] |= 1ull << (pg & 63);
    }
}

static void upload_barrier(R5xxVk *vk)
{
    VkMemoryBarrier mb = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
    };
    vkCmdPipelineBarrier(vk->cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
}

/* RV530_VK_CHECK_MIRROR: the mirror gets these host VRAM bytes */
static void shadow_copy(R5xxVk *vk, const VkBufferCopy *rg, uint32_t n)
{
    uint32_t i;

    if (!vk->shadow) {
        return;
    }
    for (i = 0; i < n; i++) {
        memcpy(vk->shadow + rg[i].dstOffset, vk->vram_host + rg[i].dstOffset, rg[i].size);
    }
}

/*
 * Snapshot the host VRAM of the upload regions in 'c' into the staging
 * buffer now: their srcOffset becomes the staging offset.  Regions that do
 * not fit move to 'live' and are copied from host VRAM, as before.
 */
static void copies_push(RvkCopies *c, uint64_t src, uint64_t dst, uint64_t len);

static void stage_copies(R5xxVk *vk, RvkCopies *c, RvkCopies *live)
{
    uint32_t i, n = 0;

    for (i = 0; i < c->n; i++) {
        VkBufferCopy r = c->rg[i];
        if (vk->stage.map && vk->stage_used + r.size <= vk->stage.size) {
            memcpy((uint8_t *)vk->stage.map + vk->stage_used, vk->vram_host + r.dstOffset,
                   r.size);
            r.srcOffset = vk->stage_used;
            vk->stage_used = (vk->stage_used + r.size + 15) & ~15ull;
            c->rg[n++] = r;
        } else {
            copies_push(live, r.dstOffset, r.dstOffset, r.size);
            c->bytes -= r.size;
        }
    }
    c->n = n;
}

/* Record staged uploads 'st' and live uploads 'live' to the mirror */
static void emit_copies(R5xxVk *vk, RvkCopies *st, RvkCopies *live)
{
    shadow_copy(vk, st->rg, st->n);
    shadow_copy(vk, live->rg, live->n);
    if (st->n) {
        vkCmdCopyBuffer(vk->cmd, vk->stage.buf, vk->vram_dev.buf, st->n, st->rg);
    }
    if (live->n) {
        vkCmdCopyBuffer(vk->cmd, vk->vram.buf, vk->vram_dev.buf, live->n, live->rg);
    }
    vk->stat_upload_bytes += st->bytes + live->bytes;
    vk->b_up_bytes += st->bytes + live->bytes;
    vk->b_up_n += st->n + live->n;
    free(st->rg);
    free(live->rg);
    st->rg = live->rg = NULL;
    st->n = live->n = 0;
}

static void record_copies(R5xxVk *vk, RvkCopies *c)
{
    RvkCopies live = { 0 };

    stage_copies(vk, c, &live);
    emit_copies(vk, c, &live);
}

/*
 * Batch start.  A few stale pages are copied right away; a flood (the guest
 * clearing VRAM, KVM reporting a remapped BAR as all dirty) is left to the
 * draws that touch it (upload_for_draw) and to r5xx_vk_upload_kick, so a
 * vCPU never waits for hundreds of MB to cross PCIe.
 */
static void record_uploads(R5xxVk *vk)
{
    RvkCopies c = { 0 };

    static int64_t eager = -1;

    if (eager < 0) {
        const char *e = getenv("RV530_VK_EAGER_UPLOAD_MB");
        eager = e ? (int64_t)atoi(e) << 20 : (int64_t)RVK_EAGER_UPLOAD;
    }
    if (vk->stale_pages && (int64_t)(vk->stale_pages << RVK_PAGE_SHIFT) <= eager) {
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

static void sampler_range(const RvkSampler *sp, uint64_t *lo, uint64_t *len);

/*
 * Copy the stale pages a draw samples or renders to, before the draw.
 * Pages go stale mid-batch too (the guest CPU fills a texture between two
 * command buffers of one batch); bytes rendered earlier in the batch keep
 * the GPU's result, as host VRAM does not have it yet.
 */
/* A copy region src -> dst; merged with the last one when both continue it */
static void copies_push(RvkCopies *c, uint64_t src, uint64_t dst, uint64_t len)
{
    VkBufferCopy *l = c->n ? &c->rg[c->n - 1] : NULL;

    if (l && l->srcOffset + l->size == src && l->dstOffset + l->size == dst) {
        l->size += len;
    } else {
        if (c->n == c->cap) {
            c->cap = c->cap ? c->cap * 2 : 64;
            c->rg = realloc(c->rg, c->cap * sizeof(*c->rg));
        }
        c->rg[c->n].srcOffset = src;
        c->rg[c->n].dstOffset = dst;
        c->rg[c->n].size = len;
        c->n++;
    }
    c->bytes += len;
}

/* VRAM pages of a draw's render ranges not yet captured this batch */
static uint32_t wb_new_pages(R5xxVk *vk, const R5xxVkDrawReq *req)
{
    uint32_t n = 0;
    int i;

    for (i = 0; i < req->ndirty; i++) {
        uint64_t off = req->dirty_off[i], len = req->dirty_len[i], pg, end;
        if (off >= vk->vram_size || !len) {
            continue;
        }
        end = (MIN_U64(off + len, vk->vram_size) - 1) >> RVK_PAGE_SHIFT;
        for (pg = off >> RVK_PAGE_SHIFT; pg <= end; pg++) {
            n += !vk->wb_slot[pg];
        }
    }
    return n;
}

/*
 * Merged write-back: give each page this draw renders to for the
 * first time in the batch a slot, and copy its mirror content (as uploaded,
 * before any rendering) to wb_before.  Uploads in 'up' into pages that
 * already have a slot go to wb_before as well, so the merge does not take
 * the uploaded bytes for rendered ones.
 */
static void wb_upload_before(R5xxVk *vk, const RvkCopies *up, RvkCopies *upb)
{
    uint32_t i;

    for (i = 0; i < up->n; i++) {
        uint64_t a = up->rg[i].dstOffset, b = a + up->rg[i].size;
        uint64_t src = up->rg[i].srcOffset - a;     /* staging or VRAM offset - VRAM offset */
        while (a < b) {
            uint64_t pg = a >> RVK_PAGE_SHIFT;
            uint64_t e = MIN_U64(b, (pg + 1) << RVK_PAGE_SHIFT);
            if (vk->wb_slot[pg]) {
                copies_push(upb, src + a, ((uint64_t)(vk->wb_slot[pg] - 1) << RVK_PAGE_SHIFT) +
                                          (a & RVK_PAGE_MASK), e - a);
            }
            a = e;
        }
    }
}

/* 'st' are staged uploads, 'live' uploads from host VRAM (stage_copies) */
static void wb_capture(R5xxVk *vk, const R5xxVkDrawReq *req, const RvkCopies *st,
                       const RvkCopies *live, RvkCopies *cap, RvkCopies *upb_st,
                       RvkCopies *upb_live)
{
    int k;

    wb_upload_before(vk, st, upb_st);
    wb_upload_before(vk, live, upb_live);
    for (k = 0; k < req->ndirty; k++) {
        uint64_t off = req->dirty_off[k], len = req->dirty_len[k], pg, end;
        if (off >= vk->vram_size || !len) {
            continue;
        }
        end = (MIN_U64(off + len, vk->vram_size) - 1) >> RVK_PAGE_SHIFT;
        for (pg = off >> RVK_PAGE_SHIFT; pg <= end; pg++) {
            if (vk->wb_slot[pg] || vk->wb_n >= RVK_WB_PAGES) {
                continue;               /* full: flush falls back to a copy */
            }
            vk->wb_page[vk->wb_n] = (uint32_t)pg;
            copies_push(cap, pg << RVK_PAGE_SHIFT, (uint64_t)vk->wb_n << RVK_PAGE_SHIFT,
                        1ull << RVK_PAGE_SHIFT);
            vk->wb_slot[pg] = ++vk->wb_n;
        }
    }
}

/*
 * Copy the stale pages a draw samples or renders to, before the draw.
 * Pages go stale mid-batch too (the guest CPU fills a texture between two
 * command buffers of one batch); bytes rendered earlier in the batch keep
 * the GPU's result, as host VRAM does not have it yet.
 */
static void upload_for_draw(R5xxVk *vk, const R5xxVkDrawReq *req)
{
    const RvkDraw *d = req->draw;
    RvkCopies c = { 0 }, live = { 0 }, cap = { 0 }, upb = { 0 }, upb_live = { 0 };
    int i;

    if (!vk->mirror) {
        return;                         /* RV530_VK_NO_MIRROR: no stale or batch_wr maps */
    }
    if (vk->stale_pages) {
        for (i = 0; i < 16; i++) {
            uint64_t lo, len;
            if (!(d->samp[i].flags & RVK_SF_VALID) || (d->samp[i].flags & RVK_SF_GART)) {
                continue;
            }
            sampler_range(&d->samp[i], &lo, &len);
            stale_take_ex(vk, lo, len, &c, vk->batch_wr);
        }
        for (i = 0; i < req->ndirty; i++) {
            if (req->dirty_off[i] < RVK_GART_SPACE) {
                stale_take_ex(vk, req->dirty_off[i], req->dirty_len[i], &c, vk->batch_wr);
            }
        }
    }
    stage_copies(vk, &c, &live);
    if (vk->wb_merge) {
        wb_capture(vk, req, &c, &live, &cap, &upb, &upb_live);
    }
    if (c.n || live.n || cap.n) {
        if (vk->rendering) {
            vkCmdEndRendering(vk->cmd);
            vk->rendering = false;
        }
        if (vk->ndraws) {
            /*
             * Earlier draws of this batch read and write VRAM through the
             * mirror.  upload_barrier() orders these copies before the later
             * draws only: without this barrier a copy can overwrite a page
             * while an earlier draw still samples it.
             */
            VkMemoryBarrier mb = {
                .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                .srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
            };
            vkCmdPipelineBarrier(vk->cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                 VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
        }
        if (upb.n) {
            vkCmdCopyBuffer(vk->cmd, vk->stage.buf, vk->wb_before.buf, upb.n, upb.rg);
        }
        if (upb_live.n) {
            vkCmdCopyBuffer(vk->cmd, vk->vram.buf, vk->wb_before.buf, upb_live.n, upb_live.rg);
        }
        if (c.n || live.n) {
            emit_copies(vk, &c, &live);
            if (cap.n) {
                /* capture what was just uploaded */
                VkMemoryBarrier mb = {
                    .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                    .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                    .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
                };
                vkCmdPipelineBarrier(vk->cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                     VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
            }
        }
        if (cap.n) {
            vkCmdCopyBuffer(vk->cmd, vk->vram_dev.buf, vk->wb_before.buf, cap.n, cap.rg);
        }
        upload_barrier(vk);
        begin_rendering(vk);
    }
    free(c.rg);
    free(live.rg);
    free(cap.rg);
    free(upb.rg);
    free(upb_live.rg);
    for (i = 0; i < req->ndirty; i++) {
        if (req->dirty_off[i] < RVK_GART_SPACE) {
            batch_wr_mark(vk, req->dirty_off[i], req->dirty_len[i]);
        }
    }
}

void r5xx_vk_upload(R5xxVk *vk, uint64_t off, uint64_t len)
{
    uint64_t pg, end;

    if (!vk || !vk->mirror || !len || off >= vk->vram_size) {
        return;
    }
    if (off + len > vk->vram_size) {
        len = vk->vram_size - off;
    }
    end = (off + len - 1) >> RVK_PAGE_SHIFT;
    for (pg = off >> RVK_PAGE_SHIFT; pg <= end; pg++) {
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
void r5xx_vk_upload_kick(R5xxVk *vk)
{
    VkCommandBufferBeginInfo bi = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    VkSubmitInfo si = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
    };
    RvkCopies c = { 0 };

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
    shadow_copy(vk, c.rg, c.n);
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
void r5xx_vk_upload_all_now(R5xxVk *vk)
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
    memset(vk->stale, 0, ((vk->vram_size >> RVK_PAGE_SHIFT) + 63) / 64 * 8);
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
    shadow_copy(vk, &rg, 1);
}

bool r5xx_vk_mirrored(const R5xxVk *vk)
{
    return vk && vk->mirror;
}

static void begin_batch(R5xxVk *vk)
{
    VkCommandBufferBeginInfo bi = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    vkBeginCommandBuffer(vk->cmd, &bi);
    if (vk->mirror) {
        memset(vk->batch_wr, 0, ((vk->vram_size >> RVK_PAGE_SHIFT) + 63) / 64 * 8);
        vk->stage_used = 0;
        record_uploads(vk);
    }
    vkCmdBindDescriptorSets(vk->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, vk->layout,
                            0, 1, &vk->dset, 0, NULL);
    begin_rendering(vk);
    vk->recording = true;
    vk->ndraws = vk->nverts = 0;
    vk->ndirty = 0;
    vk->nw = vk->nr = 0;
}

static bool ranges_overlap(uint64_t a, uint64_t al, uint64_t b, uint64_t bl)
{
    return a < b + bl && b < a + al;
}

static void sampler_range(const RvkSampler *sp, uint64_t *lo, uint64_t *len)
{
    uint32_t last = sp->num_levels ? sp->num_levels - 1 : 0;
    if (last > 15) {
        last = 15;
    }
    *lo = sp->base + ((sp->flags & RVK_SF_GART) ? RVK_GART_SPACE : 0);
    *len = (uint64_t)sp->level_offset[last] +
           (uint64_t)sp->level_pitch[last] * (((sp->height >> last) + 32) & ~31u) + 2048;
}

/*
 * Memory hazards between draws of one batch.  Same-pixel read-modify-write
 * of render targets is ordered by the fragment interlock; everything else
 * needs a barrier: sampling memory written earlier (RAW), writing memory
 * sampled earlier (WAR), and writing memory written earlier through a
 * differently shaped binding (WAW across surfaces).
 */
static bool needs_barrier(R5xxVk *vk, const R5xxVkDrawReq *req)
{
    const RvkDraw *d = req->draw;
    int s, i, k;

    for (s = 0; s < 16; s++) {
        uint64_t lo, len;
        if (!(d->samp[s].flags & RVK_SF_VALID)) {
            continue;
        }
        sampler_range(&d->samp[s], &lo, &len);
        for (i = 0; i < vk->nw; i++) {
            if (ranges_overlap(lo, len, vk->w_off[i], vk->w_len[i])) {
                return true;
            }
        }
    }
    for (k = 0; k < req->ndirty; k++) {
        uint64_t sig = req->dirty_sig[k];
        for (i = 0; i < vk->nr; i++) {
            if (ranges_overlap(req->dirty_off[k], req->dirty_len[k],
                               vk->r_off[i], vk->r_len[i])) {
                return true;
            }
        }
        for (i = 0; i < vk->nw; i++) {
            if (vk->w_sig[i] != sig &&
                ranges_overlap(req->dirty_off[k], req->dirty_len[k],
                               vk->w_off[i], vk->w_len[i])) {
                return true;
            }
        }
    }
    return false;
}

static void note_hazards(R5xxVk *vk, const R5xxVkDrawReq *req)
{
    const RvkDraw *d = req->draw;
    int s, k;

    for (s = 0; s < 16 && vk->nr < RVK_MAX_DIRTY; s++) {
        if (d->samp[s].flags & RVK_SF_VALID) {
            sampler_range(&d->samp[s], &vk->r_off[vk->nr], &vk->r_len[vk->nr]);
            vk->nr++;
        }
    }
    for (k = 0; k < req->ndirty && vk->nw < RVK_MAX_DIRTY; k++) {
        vk->w_off[vk->nw] = req->dirty_off[k];
        vk->w_len[vk->nw] = req->dirty_len[k];
        vk->w_sig[vk->nw] = req->dirty_sig[k];
        vk->nw++;
    }
}

bool r5xx_vk_draw(R5xxVk *vk, const R5xxVkDrawReq *req, char *why, size_t whylen)
{
    VkPipeline pipe;
    VkRect2D sc;
    uint32_t idx;
    int i;

    if (!req->nverts) {
        return true;
    }
    if (req->nverts > RVK_VERT_SLOTS) {
        set_err(why, whylen, "too many vertices");
        return false;
    }
    pipe = get_pipeline(vk, req->prog, req->prog_hash, why, whylen);
    if (!pipe) {
        return false;
    }
    if (vk->recording && (vk->ndraws >= RVK_DRAW_SLOTS ||
                          vk->nverts + req->nverts > RVK_VERT_SLOTS ||
                          vk->ndirty + req->ndirty > RVK_MAX_DIRTY ||
                          (vk->wb_merge && vk->wb_n + wb_new_pages(vk, req) > RVK_WB_PAGES))) {
        r5xx_vk_flush(vk);
    }
    if (!vk->recording) {
        begin_batch(vk);
    }
    upload_for_draw(vk, req);
    if (vk->nw >= RVK_MAX_DIRTY - 8 || vk->nr >= RVK_MAX_DIRTY - 16 ||
        vk->barrier_each || needs_barrier(vk, req)) {
        vkCmdEndRendering(vk->cmd);
        storage_barrier(vk, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                        VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
        begin_rendering(vk);
        vk->nw = vk->nr = 0;
        vk->stat_barriers++;
    }
    note_hazards(vk, req);

    idx = vk->ndraws++;
    memcpy((RvkDraw *)vk->draws.map + idx, req->draw, sizeof(RvkDraw));
    ((RvkDraw *)vk->draws.map)[idx].misc[RVK_M_ZPASS_SLOT] = 0;
    memcpy((RvkVertex *)vk->verts.map + vk->nverts, req->verts,
           (size_t)req->nverts * sizeof(RvkVertex));

    if (vk->bound != pipe) {
        vkCmdBindPipeline(vk->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
        vk->bound = pipe;
    }
    {
        uint32_t pc[4] = { idx, 0, 0, 0 };
        vkCmdPushConstants(vk->cmd, vk->layout,
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, 16, pc);
    }
    sc.offset.x = req->sc_x0 < 0 ? 0 : (req->sc_x0 > RVK_VP ? RVK_VP : req->sc_x0);
    sc.offset.y = req->sc_y0 < 0 ? 0 : (req->sc_y0 > RVK_VP ? RVK_VP : req->sc_y0);
    {
        int x1 = req->sc_x1 + 1 > RVK_VP ? RVK_VP : req->sc_x1 + 1;
        int y1 = req->sc_y1 + 1 > RVK_VP ? RVK_VP : req->sc_y1 + 1;
        sc.extent.width = x1 > sc.offset.x ? (uint32_t)(x1 - sc.offset.x) : 0;
        sc.extent.height = y1 > sc.offset.y ? (uint32_t)(y1 - sc.offset.y) : 0;
    }
    vkCmdSetScissor(vk->cmd, 0, 1, &sc);
    vkCmdDraw(vk->cmd, req->nverts, 1, vk->nverts, 0);
    vk->nverts += req->nverts;
    for (i = 0; i < req->ndirty && vk->ndirty < RVK_MAX_DIRTY; i++) {
        vk->dirty_off[vk->ndirty] = req->dirty_off[i];
        vk->dirty_len[vk->ndirty] = req->dirty_len[i];
        vk->ndirty++;
    }
    vk->stat_draws++;
    if (vk->trace) {
        size_t l = strlen(vk->b_progs);
        vk->b_area += (uint64_t)sc.extent.width * sc.extent.height;
        for (i = 0; i < 16; i++) {
            if (req->draw->samp[i].flags & RVK_SF_GART) vk->b_gart |= 1;
        }
        if (req->draw->cb[0].flags & RVK_CF_GART) vk->b_gart |= 2;
        if (l < sizeof(vk->b_progs) - 24) {
            snprintf(vk->b_progs + l, sizeof(vk->b_progs) - l, " %04x:%ux%u",
                     (unsigned)(req->prog_hash & 0xffff), sc.extent.width, sc.extent.height);
        }
    }
    return true;
}

bool r5xx_vk_pending(const R5xxVk *vk)
{
    return vk && vk->recording;
}

/*
 * The driver's compiled pipelines (VkPipelineCache) next to the SPIR-V:
 * creating a pipeline from it takes ms instead of ~300 ms until the next
 * driver update, which the cache header detects (the data is then ignored).
 */
static void pcache_save(R5xxVk *vk)
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

static void pcache_load(R5xxVk *vk)
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

void r5xx_vk_new_cp_run(R5xxVk *vk)
{
    if (vk) {
        vk->wait_ns = 0;
    }
}

void r5xx_vk_set_wait_fn(R5xxVk *vk, R5xxVkWaitFn fn)
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
int r5xx_vk_set_cache_dir(R5xxVk *vk, const char *dir)
{
    char path[4096];
    FILE *f;
    int n = 0;

    if (!vk || !dir || !*dir || vk->cache_dir) {
        return 0;
    }
    vk->cache_dir = strdup(dir);
    pcache_load(vk);
    vk->tpl_hash = fnv_str(fnv_str(fnv_str(1469598103934665603ull, vk->frag_prefix),
                                   rvk_frag_template()), r500_us_glsl_build_id());
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
            RvkPipe **head = &vk->pipes[hash % RVK_PIPE_BUCKETS], *p;
            size_t bytes;
            uint32_t *words;

            for (p = *head; p && p->hash != hash; p = p->next) {
            }
            if (p || !(words = cache_load(vk, hash, &bytes))) {
                continue;
            }
            p = calloc(1, sizeof(*p));
            p->hash = hash;
            p->state = RVK_PIPE_PENDING;
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

/*
 * Merged write-back, after the fence: a byte of a captured page that
 * the batch changed (wb_after != wb_before) goes to host VRAM; the others
 * keep whatever the guest wrote there meanwhile, which the dirty log has
 * and the next batch uploads.  Clears the slots; wb_n is reset by the
 * caller once the shadow (RV530_VK_CHECK_MIRROR) has been updated.
 */
static void wb_merge_pages(R5xxVk *vk)
{
    static int report = -1;
    uint64_t kept = 0;
    uint32_t k;

    if (report < 0) {
        report = getenv("RV530_VK_WB_STATS") != NULL;
    }
    for (k = 0; k < vk->wb_n; k++) {
        uint64_t pg = vk->wb_page[k];
        const uint64_t *b = (const uint64_t *)((uint8_t *)vk->wb_before.map +
                                               ((uint64_t)k << RVK_PAGE_SHIFT));
        const uint64_t *a = (const uint64_t *)((uint8_t *)vk->wb_after.map +
                                               ((uint64_t)k << RVK_PAGE_SHIFT));
        uint64_t *h = (uint64_t *)(vk->vram_host + (pg << RVK_PAGE_SHIFT));
        unsigned w;

        if (!report && !memcmp(a, b, 1u << RVK_PAGE_SHIFT)) {
            w = (1u << RVK_PAGE_SHIFT) / 8;     /* the batch did not change it */
        } else {
            w = 0;
        }
        for (; w < (1u << RVK_PAGE_SHIFT) / 8; w++) {
            uint64_t aw = a[w], bw = b[w];
            if (aw != bw) {
                uint64_t hw = h[w], m = 0;
                unsigned j;
                for (j = 0; j < 8; j++) {
                    if (((aw ^ bw) >> (j * 8)) & 0xff) {
                        m |= 0xffull << (j * 8);
                    }
                }
                if ((hw & ~m) != (bw & ~m)) {
                    kept += 8;          /* coarse: words with kept bytes */
                }
                h[w] = (hw & ~m) | (aw & m);
            } else if (report && h[w] != bw) {
                kept += 8;
            }
        }
        if (vk->shadow) {
            memcpy(vk->shadow + (pg << RVK_PAGE_SHIFT), a, 1u << RVK_PAGE_SHIFT);
        }
        vk->wb_slot[pg] = 0;
    }
    if (kept) {
        vk->wb_kept += kept;
        vk->wb_kept_batches++;
        if (report && (vk->wb_kept_batches <= 50 || !(vk->wb_kept_batches & 255))) {
            fprintf(stderr, "rv530-vk: write-back kept ~%" PRIu64 " bytes the guest wrote during "
                    "the batch (%" PRIu64 " batches so far)\n", kept, vk->wb_kept_batches);
        }
    }
}

/*
 * RV530_VK_CHECK_MIRROR: the write-back copies whole row bands of
 * the mirror to host VRAM, not just the pixels the batch drew.  A byte the
 * GPU did not draw (mirror still equals what it was given, the shadow)
 * while host VRAM had a newer value before the write-back (saved) has
 * just been reverted.  Mode 2 puts the newer value back.
 */
static void check_writeback(R5xxVk *vk, uint8_t **saved)
{
    uint64_t n = 0, first = 0;
    int i;

    for (i = 0; i < vk->ndirty; i++) {
        uint64_t off = vk->dirty_off[i], len, j;
        uint8_t *host, *base;

        if (!saved[i]) {
            continue;
        }
        len = vk->dirty_len[i] < vk->vram_size - off ? vk->dirty_len[i] : vk->vram_size - off;
        host = vk->vram_host + off;
        base = vk->shadow + off;
        for (j = 0; j < len; j++) {
            if (saved[i][j] != base[j] && host[j] == base[j]) {
                if (!n) {
                    first = off + j;
                }
                n++;
                if (vk->check_mode == 2) {
                    host[j] = saved[i][j];
                }
            }
        }
        if (vk->check_mode == 2 && n) {
            r5xx_vk_upload(vk, off, len);
        }
        free(saved[i]);
    }
    free(saved);
    if (n) {
        vk->check_reverted += n;
        vk->check_revert_batches++;
        if (vk->check_revert_batches <= 200 || !(vk->check_revert_batches & 255)) {
            fprintf(stderr, "rv530-vk check: write-back reverted %" PRIu64 " bytes the CPU wrote "
                    "(first at VRAM 0x%" PRIx64 ")%s; %" PRIu64 " bytes in %" PRIu64 " batches\n",
                    n, first, vk->check_mode == 2 ? ", restored" : "",
                    vk->check_reverted, vk->check_revert_batches);
        }
    }
}

uint64_t r5xx_vk_flush(R5xxVk *vk)
{
    VkSubmitInfo si = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
    };
    uint64_t zpass;
    uint8_t **saved = NULL;
    int i;

    if (!vk || !vk->recording) {
        return 0;
    }
    if (vk->rendering) {
        vkCmdEndRendering(vk->cmd);
        vk->rendering = false;
    }
    if (vk->mirror && (vk->ndirty || vk->wb_n)) {
        VkBufferCopy *rg = calloc(vk->ndirty + 1, sizeof(*rg));
        VkMemoryBarrier mb = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
        };
        uint32_t n = 0;
        storage_barrier(vk, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
        if (vk->wb_merge) {
            /* captured pages to wb_after, merged after the fence; pages
             * without a slot (none free) are copied as before */
            RvkCopies a = { 0 }, direct = { 0 };
            uint32_t k;
            for (k = 0; k < vk->wb_n; k++) {
                copies_push(&a, (uint64_t)vk->wb_page[k] << RVK_PAGE_SHIFT,
                            (uint64_t)k << RVK_PAGE_SHIFT, 1ull << RVK_PAGE_SHIFT);
            }
            for (i = 0; i < vk->ndirty; i++) {
                uint64_t o = vk->dirty_off[i], e;
                if (o >= vk->vram_size) {
                    continue;
                }
                e = MIN_U64(o + vk->dirty_len[i], vk->vram_size);
                while (o < e) {
                    uint64_t pe = MIN_U64(e, ((o >> RVK_PAGE_SHIFT) + 1) << RVK_PAGE_SHIFT);
                    if (!vk->wb_slot[o >> RVK_PAGE_SHIFT]) {
                        copies_push(&direct, o, o, pe - o);
                    }
                    o = pe;
                }
            }
            if (a.n) {
                vkCmdCopyBuffer(vk->cmd, vk->vram_dev.buf, vk->wb_after.buf, a.n, a.rg);
            }
            if (direct.n) {
                vkCmdCopyBuffer(vk->cmd, vk->vram_dev.buf, vk->vram.buf, direct.n, direct.rg);
            }
            free(a.rg);
            free(direct.rg);
        } else {
            for (i = 0; i < vk->ndirty; i++) {
                if (vk->dirty_off[i] < RVK_GART_SPACE) {
                    rg[n].srcOffset = rg[n].dstOffset = vk->dirty_off[i];
                    rg[n].size = vk->dirty_len[i];
                    n++;
                }
            }
            if (n) {
                vkCmdCopyBuffer(vk->cmd, vk->vram_dev.buf, vk->vram.buf, n, rg);
            }
        }
        vkCmdPipelineBarrier(vk->cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
        free(rg);
    }
    storage_barrier(vk, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_READ_BIT);
    vkEndCommandBuffer(vk->cmd);
    /* RV530_VK_CHECK_MIRROR: host VRAM of the write-back ranges as it is
     * now, to find bytes the write-back reverts (see check_writeback) */
    if (vk->shadow && vk->mirror && vk->ndirty && !vk->wb_merge) {
        saved = calloc(vk->ndirty, sizeof(*saved));
        for (i = 0; i < vk->ndirty; i++) {
            if (vk->dirty_off[i] < vk->vram_size) {
                uint64_t len = vk->dirty_len[i] < vk->vram_size - vk->dirty_off[i] ?
                               vk->dirty_len[i] : vk->vram_size - vk->dirty_off[i];
                saved[i] = malloc(len);
                memcpy(saved[i], vk->vram_host + vk->dirty_off[i], len);
            }
        }
    }
    si.pCommandBuffers = &vk->cmd;
    {
        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        vkQueueSubmit(vk->queue, 1, &si, vk->fence);
        /* most batches are done within 2 ms; a longer GPU wait (a large
         * batch, the driver finishing a pipeline) releases the device's
         * big lock like a compile wait does, instead of holding a vCPU's
         * MMIO exit and the BQL for 100 ms */
        if (vkWaitForFences(vk->dev, 1, &vk->fence, VK_TRUE,
                            2000000) == VK_TIMEOUT) {
            if (vk->wait_fn) {
                vk->wait_fn(vk->opaque, true);
            }
            vkWaitForFences(vk->dev, 1, &vk->fence, VK_TRUE, UINT64_MAX);
            if (vk->wait_fn) {
                vk->wait_fn(vk->opaque, false);
            }
        }
        clock_gettime(CLOCK_MONOTONIC, &t1);
        if (vk->trace) {
            fprintf(stderr, "rv530-vk batch: %u draws, %.1f Mpx, gart %u, upload %u ranges %.1f MB, %.2f ms:%s\n",
                    vk->ndraws, vk->b_area / 1e6, vk->b_gart, vk->b_up_n, vk->b_up_bytes / 1e6,
                    (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6,
                    vk->b_progs);
            vk->b_up_n = 0;
            vk->b_up_bytes = 0;
            vk->b_area = 0;
            vk->b_gart = 0;
            vk->b_progs[0] = 0;
        }
    }
    vkResetFences(vk->dev, 1, &vk->fence);
    vkResetCommandBuffer(vk->cmd, 0);
    vk->recording = false;
    vk->stat_batches++;

    if (saved) {
        check_writeback(vk, saved);
    }
    if (vk->shadow && vk->mirror) {
        /*
         * Ranges copied straight to host VRAM match it now.  Captured pages
         * don't: wb_merge_pages() gives the shadow what the mirror really
         * holds, so a byte the mirror missed still shows up in the check.
         */
        for (i = 0; i < vk->ndirty; i++) {
            if (vk->dirty_off[i] < RVK_GART_SPACE && vk->dirty_off[i] < vk->vram_size) {
                uint64_t o = vk->dirty_off[i];
                uint64_t e = o + (vk->dirty_len[i] < vk->vram_size - o ?
                                  vk->dirty_len[i] : vk->vram_size - o);
                while (o < e) {
                    uint64_t pe = MIN_U64(e, ((o >> RVK_PAGE_SHIFT) + 1) << RVK_PAGE_SHIFT);
                    if (!vk->wb_merge || !vk->wb_slot[o >> RVK_PAGE_SHIFT]) {
                        memcpy(vk->shadow + o, vk->vram_host + o, pe - o);
                    }
                    o = pe;
                }
            }
        }
    }
    if (vk->wb_n) {
        wb_merge_pages(vk);
    }
    vk->wb_n = 0;
    zpass = ((volatile uint32_t *)vk->counts.map)[0];
    ((volatile uint32_t *)vk->counts.map)[0] = 0;
    if (vk->dirty_fn) {
        for (i = 0; i < vk->ndirty; i++) {
            if (vk->dirty_off[i] < RVK_GART_SPACE) {
                vk->dirty_fn(vk->opaque, vk->dirty_off[i], vk->dirty_len[i]);
            }
        }
    }
    vk->ndirty = 0;
    return zpass;
}

static void update_desc(R5xxVk *vk, uint32_t binding, VkBuffer buf, VkDeviceSize range)
{
    VkDescriptorBufferInfo bi = { buf, 0, range };
    VkWriteDescriptorSet w = {
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = vk->dset,
        .dstBinding = binding, .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &bi,
    };
    vkUpdateDescriptorSets(vk->dev, 1, &w, 0, NULL);
}

bool r5xx_vk_set_sysram(R5xxVk *vk, void *ram, uint64_t size, char *err,
                        size_t errlen)
{
    VkPhysicalDeviceProperties pr;

    r5xx_vk_flush(vk);
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

uint32_t *r5xx_vk_gart_table(R5xxVk *vk, uint32_t npages)
{
    if (!vk || !vk->sysram.buf) {
        return NULL;
    }
    if (npages > vk->gart_pages) {
        RvkBuf nb = { 0 };
        char err[128];
        r5xx_vk_flush(vk);
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

void r5xx_vk_stats(const R5xxVk *vk, uint64_t *draws, uint64_t *batches,
                   uint64_t *pipelines)
{
    *draws = vk ? vk->stat_draws : 0;
    *batches = vk ? vk->stat_batches : 0;
    *pipelines = vk ? vk->stat_pipes : 0;
}

/*
 * RV530_VK_CHECK_MIRROR=1|2: find VRAM pages the guest changed
 * without the change reaching the mirror.  Called before the dirty-log
 * snapshot: r5xx_vk_check_scan() marks the pages that differ from what the
 * mirror was given and are not stale; the caller passes each of them that
 * the snapshot does not show dirty to r5xx_vk_check_missed(), i.e. a write
 * the dirty log lost.  Every RV530_VK_CHECK_MS (250) ms.
 */
uint64_t *r5xx_vk_check_scan(R5xxVk *vk)
{
    struct timespec ts;
    int64_t now;
    uint64_t pg, npages, n = 0;

    if (!vk || !vk->shadow || vk->recording) {
        return NULL;
    }
    if (vk->up_inflight && vkGetFenceStatus(vk->dev, vk->upfence) != VK_SUCCESS) {
        return NULL;                    /* a kicked upload is still copying */
    }
    clock_gettime(CLOCK_MONOTONIC, &ts);
    now = ts.tv_sec * 1000000000LL + ts.tv_nsec;
    if (now < vk->check_next_ns) {
        return NULL;
    }
    vk->check_next_ns = now + vk->check_ms * 1000000LL;
    npages = vk->vram_size >> RVK_PAGE_SHIFT;
    memset(vk->diff, 0, (npages + 63) / 64 * 8);
    for (pg = 0; pg < npages; pg++) {
        uint64_t bit = 1ull << (pg & 63);
        if (vk->stale[pg >> 6] & bit) {
            continue;
        }
        if (memcmp(vk->shadow + (pg << RVK_PAGE_SHIFT), vk->vram_host + (pg << RVK_PAGE_SHIFT),
                   1u << RVK_PAGE_SHIFT)) {
            vk->diff[pg >> 6] |= bit;
            n++;
        }
    }
    vk->check_scans++;
    vk->check_diff = n;
    if (!(vk->check_scans % 40)) {
        fprintf(stderr, "rv530-vk check: %" PRIu64 " scans, %" PRIu64 " pages changed and "
                "not stale in the last, %" PRIu64 " missed writes so far\n",
                vk->check_scans, n, vk->check_missed);
    }
    return n ? vk->diff : NULL;
}

void r5xx_vk_check_missed(R5xxVk *vk, uint64_t pg)
{
    uint64_t off = pg << RVK_PAGE_SHIFT;
    const uint8_t *a = vk->shadow + off, *b = vk->vram_host + off;
    unsigned i, first = 0, nbytes = 0;

    for (i = 0; i < (1u << RVK_PAGE_SHIFT); i++) {
        if (a[i] != b[i]) {
            if (!nbytes) {
                first = i;
            }
            nbytes++;
        }
    }
    vk->check_missed++;
    if (vk->check_missed <= 200 || !(vk->check_missed & 1023)) {
        fprintf(stderr, "rv530-vk check: missed write: page 0x%" PRIx64 " (%u bytes differ, "
                "first at +0x%x), %" PRIu64 " missed in %" PRIu64 " scans\n",
                off, nbytes, first, vk->check_missed, vk->check_scans);
    }
    if (vk->check_mode == 2) {
        r5xx_vk_upload(vk, off, 1u << RVK_PAGE_SHIFT);
    } else {
        memcpy(vk->shadow + off, vk->vram_host + off, 1u << RVK_PAGE_SHIFT);
    }
}
