/*
 * Evergreen compute dispatch (PKT3_DISPATCH_DIRECT).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/atomic.h"
#include "qemu/rcu.h"
#include "qemu/thread.h"
#include "eg_gfx.h"

/* EG_CS_DUMP=dir: write the state of the first dispatches to dir (debug) */
static void cs_dump(EvergreenState *s, const uint32_t *d, uint32_t n)
{
    static int count;
    const char *dir = getenv("EG_CS_DUMP");
    g_autofree char *path = NULL;
    EgGfx *g = s->gfx;
    FILE *f;
    uint32_t i, k;
    uint64_t pgm;

    if (!dir || count >= 64) {
        return;
    }
    path = g_strdup_printf("%s/cs%03d.txt", dir, count++);
    f = fopen(path, "w");
    if (!f) {
        return;
    }
    fprintf(f, "dispatch:");
    for (i = 0; i < n; i++) {
        fprintf(f, " %08x", d[i]);
    }
    fprintf(f, "\nconfig 0x8900-0x8a00:\n");
    for (i = 0x8900; i < 0x8a00; i += 4) {
        if (s->regs[i >> 2]) {
            fprintf(f, "  %05x = %08x\n", i, s->regs[i >> 2]);
        }
    }
    fprintf(f, "context:\n");
    for (i = 0; i < EG_CTX_SIZE / 4; i++) {
        if (s->ctx[i]) {
            fprintf(f, "  %05x = %08x\n", EG_CTX_BASE + i * 4, s->ctx[i]);
        }
    }
    fprintf(f, "resources:\n");
    for (i = 0; i < EG_NUM_RESOURCES; i++) {
        if (g->resource[i][7] || g->resource[i][0]) {
            fprintf(f, "  %4u:", i);
            for (k = 0; k < 8; k++) {
                fprintf(f, " %08x", g->resource[i][k]);
            }
            fprintf(f, "\n");
        }
    }
    fprintf(f, "samplers:\n");
    for (i = 0; i < EG_NUM_SAMPLERS; i++) {
        if (g->sampler[i][0] || g->sampler[i][1] || g->sampler[i][2]) {
            fprintf(f, "  %3u: %08x %08x %08x\n", i, g->sampler[i][0], g->sampler[i][1],
                    g->sampler[i][2]);
        }
    }
    fprintf(f, "loop consts:\n");
    for (i = 0; i < EG_NUM_LOOP_CONSTS; i++) {
        if (g->loop_const[i]) {
            fprintf(f, "  %3u: %08x\n", i, g->loop_const[i]);
        }
    }
    pgm = (uint64_t)CTX(s, 0x288d0) << 8;
    fprintf(f, "program (SQ_PGM_START_LS) at %" PRIx64 ":\n", pgm);
    {
        g_autofree uint32_t *code = g_new0(uint32_t, 4096);
        eg_gpu_read(s, pgm, code, 4096 * 4);
        for (i = 0; i < 4096; i += 4) {
            fprintf(f, "  %04x: %08x %08x %08x %08x\n", i, le32_to_cpu(code[i]),
                    le32_to_cpu(code[i + 1]), le32_to_cpu(code[i + 2]), le32_to_cpu(code[i + 3]));
        }
    }
    fclose(f);
}

/* RAT n is CB_COLORn: opened at its first store, closed after the dispatch */
#define EG_NUM_RATS 12

typedef struct RatCtx {
    QemuMutex lock;
    void *cb[EG_NUM_RATS];
    bool tried[EG_NUM_RATS];
    uint32_t w[EG_NUM_RATS], h[EG_NUM_RATS];
} RatCtx;

enum { RAT_STORE_TYPED = 1, RAT_STORE_RAW = 2 };

static void *rat_target(EvergreenState *s, RatCtx *rc, uint32_t id)
{
    if (!qatomic_load_acquire(&rc->tried[id])) {
        qemu_mutex_lock(&rc->lock);
        if (!rc->tried[id]) {
            uint32_t r = id < 8 ? 0x28c60 + 0x3c * id : 0x28e40 + 0x1c * (id - 8);
            uint32_t dim = CTX(s, r + 0x18);
            rc->cb[id] = eg_cb_open(s, id);
            rc->w[id] = (dim & 0xffff) + 1;
            rc->h[id] = (dim >> 16) + 1;
            qatomic_store_release(&rc->tried[id], true);
        }
        qemu_mutex_unlock(&rc->lock);
    }
    return rc->cb[id];
}

void eg_compute_rat(EgShader *sh, uint32_t w0, uint32_t w1, uint32_t lanes)
{
    EvergreenState *s = sh->s;
    RatCtx *rc = sh->rat;
    uint32_t id = w0 & 15, inst = (w0 >> 4) & 0x3f, type = (w0 >> 13) & 3;
    uint32_t rw = (w0 >> 15) & 0x7f, idx = (w0 >> 23) & 0x7f, es = w0 >> 30;
    uint32_t mask = (w1 >> 12) & 0xf, burst = (w1 >> 16) & 0xf;
    void *cb;
    int lane;

    if (!rc || id >= EG_NUM_RATS) {
        return;
    }
    cb = rat_target(s, rc, id);
    if (!cb) {
        eg_gfx_warn(s, 0x1f0, "evergreen: RAT %u has no surface\n", id);
        return;
    }
    if (burst || !(type & 1)) {
        eg_gfx_warn(s, 0x1f1, "evergreen: MEM_RAT type %u burst %u\n", type, burst);
    }
    for (lane = 0; lane < sh->nlanes; lane++) {
        const EgVec4 *a = &sh->gpr[lane][idx];
        const EgVec4 *v = &sh->gpr[lane][rw];
        if (!((lanes >> lane) & 1)) {
            continue;
        }
        switch (inst) {
        case RAT_STORE_TYPED:
            if (a->u[0] < rc->w[id] && a->u[1] < rc->h[id]) {
                eg_cb_store(s, cb, a->u[0], a->u[1], v, mask);
            }
            break;
        case RAT_STORE_RAW: {
            /* a linear buffer of (ELEM_SIZE + 1) dwords per element */
            uint32_t k;
            uint64_t base = (uint64_t)CTX(s, id < 8 ? 0x28c60 + 0x3c * id :
                                           0x28e40 + 0x1c * (id - 8)) << 8;
            for (k = 0; k <= es; k++) {
                if ((mask >> k) & 1) {
                    uint32_t le = cpu_to_le32(v->u[k]);
                    eg_gpu_write(s, base + ((uint64_t)a->u[0] * (es + 1) + k) * 4, &le, 4);
                }
            }
            break;
        }
        default:
            eg_gfx_warn(s, 0x1f2, "evergreen: MEM_RAT instruction %u\n", inst);
            return;
        }
    }
}

/* one dispatch, shared by the workers */
typedef struct Dispatch {
    EvergreenState *s;
    uint64_t pgm;
    uint32_t gn[3], gs[3], g0[3];
    uint64_t tpg, total;
    uint64_t next;                      /* the first thread not taken yet */
    RatCtx rat;
} Dispatch;

#define CS_CHUNK    (16 * EG_MAX_LANES)
#define CS_WORKERS  16

/* run chunks of threads until all threads have run */
static void cs_run(Dispatch *dp, EgShader *sh, EgProgram *prog)
{
    uint64_t t, end;
    uint32_t k;

    if (!eg_program_load(dp->s, prog, dp->pgm, EG_STAGE_CS)) {
        return;
    }
    memset(sh, 0, sizeof(*sh));
    sh->s = dp->s;
    sh->prog = prog;
    sh->stage = EG_STAGE_CS;
    sh->nlanes = EG_MAX_LANES;
    sh->rat = &dp->rat;
    while ((t = qatomic_fetch_add(&dp->next, CS_CHUNK)) < dp->total) {
        end = MIN(dp->total, t + CS_CHUNK);
        for (; t < end; t += EG_MAX_LANES) {
            sh->live = 0;
            for (k = 0; k < EG_MAX_LANES && t + k < end; k++) {
                uint64_t g = (t + k) / dp->tpg, l = (t + k) % dp->tpg;
                const uint32_t *gs = dp->gs, *gn = dp->gn;
                EgVec4 *r = sh->gpr[k];
                r[0].u[0] = l % gs[0];
                r[0].u[1] = (l / gs[0]) % gs[1];
                r[0].u[2] = l / ((uint64_t)gs[0] * gs[1]);
                r[0].u[3] = 0;
                r[1].u[0] = dp->g0[0] + g % gn[0];
                r[1].u[1] = dp->g0[1] + (g / gn[0]) % gn[1];
                r[1].u[2] = dp->g0[2] + g / ((uint64_t)gn[0] * gn[1]);
                r[1].u[3] = 0;
                sh->live |= 1u << k;
            }
            sh->killed = 0;
            sh->pred = 0;
            sh->loop_index = 0;
            memset(sh->ar, 0, sizeof(sh->ar));
            eg_shader_run(sh);
        }
    }
    eg_tex_cache_reset(sh);
}

static struct {
    int n;
    QemuThread th[CS_WORKERS];
    QemuSemaphore go, done;
    Dispatch *dp;
} cpool;

static void *cs_worker(void *arg)
{
    EgShader *sh = g_new0(EgShader, 1);
    EgProgram prog;

    rcu_register_thread();
    for (;;) {
        qemu_sem_wait(&cpool.go);
        cs_run(cpool.dp, sh, &prog);
        eg_program_free(&prog);
        qemu_sem_post(&cpool.done);
    }
    return NULL;
}

/* EG_CS_THREADS: the workers that run compute threads besides the caller */
static void cs_pool_init(void)
{
    const char *e = getenv("EG_CS_THREADS");
    int i;

    if (cpool.n) {
        return;
    }
    cpool.n = MAX(1, MIN(CS_WORKERS, e ? atoi(e) + 1 : (int)g_get_num_processors() / 2));
    qemu_sem_init(&cpool.go, 0);
    qemu_sem_init(&cpool.done, 0);
    for (i = 1; i < cpool.n; i++) {
        qemu_thread_create(&cpool.th[i], "eg-compute", cs_worker, NULL, QEMU_THREAD_DETACHED);
    }
}

/*
 * DISPATCH_DIRECT: run the thread groups of the compute shader at
 * SQ_PGM_START_LS.  The shader interpreter runs EG_MAX_LANES threads at a
 * time, on the caller and on the compute workers.  Each thread starts with
 * its index in the group in R0.xyz and its group index in R1.xyz
 * (SPI_COMPUTE_INPUT_CNTL).  It reads the LS constant caches, the fetch
 * resources from 816 and the samplers from 90.  MEM_RAT writes its results
 * to CB_COLORn.
 */
void eg_compute_dispatch(EvergreenState *s, const uint32_t *d, uint32_t n)
{
    static int prof = -1;
    Dispatch dp = { .s = s };
    EgShader *sh;
    EgProgram prog;
    int64_t t0 = 0;
    int i;

    if (n < 3) {
        return;
    }
    cs_dump(s, d, n);
    for (i = 0; i < 3; i++) {
        dp.gn[i] = d[i];
        dp.gs[i] = MAX(CTX(s, 0x286ec + 4 * i), 1u);    /* SPI_COMPUTE_NUM_THREAD_X/Y/Z */
        dp.g0[i] = s->regs[(0x899c + 4 * i) >> 2];       /* VGT_COMPUTE_START_X/Y/Z */
    }
    dp.tpg = (uint64_t)dp.gs[0] * dp.gs[1] * dp.gs[2];
    dp.total = (uint64_t)dp.gn[0] * dp.gn[1] * dp.gn[2] * dp.tpg;
    if (!dp.total || dp.tpg > 1024) {
        return;
    }
    dp.pgm = (uint64_t)CTX(s, 0x288d0) << 8;
    if (prof < 0) {
        prof = getenv("EG_CS_PROFILE") != NULL;
    }
    if (prof) {
        t0 = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
    }
    qemu_mutex_init(&dp.rat.lock);
    eg_gfx_flush(s);                    /* the shader reads and writes VRAM */
    cs_pool_init();
    /*
     * A large dispatch takes longer than XNU lets a vCPU wait for another
     * one: run it without the BQL, as the CP's GPU waits do.  The workers
     * read the device state, which stays as it is until the CP has the
     * BQL again (eg_mmio_enter).
     */
    eg_cp_wait_cb(s, true);
    cpool.dp = &dp;
    for (i = 1; i < cpool.n; i++) {
        qemu_sem_post(&cpool.go);
    }
    sh = g_new(EgShader, 1);
    cs_run(&dp, sh, &prog);
    eg_program_free(&prog);
    g_free(sh);
    for (i = 1; i < cpool.n; i++) {
        qemu_sem_wait(&cpool.done);
    }
    eg_cp_wait_cb(s, false);
    for (i = 0; i < EG_NUM_RATS; i++) {
        if (dp.rat.cb[i]) {
            eg_cb_close(s, dp.rat.cb[i]);
        }
    }
    qemu_mutex_destroy(&dp.rat.lock);
    if (prof) {
        qemu_log("evergreen: compute %ux%ux%u groups of %" PRIu64 " threads at %" PRIx64
                 " (with the flush): %" PRId64 " us\n", dp.gn[0], dp.gn[1], dp.gn[2], dp.tpg, dp.pgm,
                 (qemu_clock_get_ns(QEMU_CLOCK_REALTIME) - t0) / 1000);
    }
}
