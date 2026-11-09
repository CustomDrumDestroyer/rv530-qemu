/*
 * Evergreen command processor (graphics ring, PM4) and async DMA engine.
 *
 * Both rings are processed synchronously when the driver moves the write
 * pointer, as on the rv530.  The PFP/ME/RLC microcode the driver uploads
 * is accepted and ignored: packets are decoded here at a high level.
 * Register writes, fences and interrupts, memory writes, indirect buffers
 * and copies are handled here; state and draw packets go to eg_draw.c.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "evergreen.h"
#include "trace.h"
#include "eg_gfx.h"

/* PM4 type-3 opcodes (evergreend.h) */
#define PKT3_NOP                0x10
#define PKT3_CLEAR_STATE        0x12
#define PKT3_DISPATCH_DIRECT    0x15
#define PKT3_DRAW_INDEX_2       0x27
#define PKT3_CONTEXT_CONTROL    0x28
#define PKT3_DRAW_INDEX         0x2b
#define PKT3_DRAW_INDEX_AUTO    0x2d
#define PKT3_INDIRECT_BUFFER    0x32
#define PKT3_INDIRECT_BUFFER_PFP 0x3f            /* undocumented; same layout */
#define PKT3_MEM_SEMAPHORE      0x39
#define PKT3_WAIT_REG_MEM       0x3c
#define PKT3_MEM_WRITE          0x3d
#define PKT3_CP_DMA             0x41
#define PKT3_SURFACE_SYNC       0x43
#define PKT3_ME_INITIALIZE      0x44
#define PKT3_EVENT_WRITE        0x46
#define PKT3_EVENT_WRITE_EOP    0x47
#define PKT3_EVENT_WRITE_EOS    0x48
#define PKT3_SET_CONFIG_REG     0x68
#define PKT3_SET_CONTEXT_REG    0x69

#define EG_CP_RB_INT_SRC        176             /* PACKET0 write of CP_INT_STATUS */
#define EG_RB_INT_STAT          (1u << 30)

/* ------------------------------------------------------------------ */
/* Graphics ring                                                       */
/* ------------------------------------------------------------------ */

static void eg_scratch_write(EvergreenState *s, uint32_t reg, uint32_t val)
{
    uint32_t n = (reg - EG_SCRATCH_REG0) >> 2;

    if (n < 8 && (s->regs[EG_SCRATCH_UMSK >> 2] >> n) & 1) {
        uint64_t a = ((uint64_t)s->regs[EG_SCRATCH_ADDR >> 2] << 8) + 4 * n;
        uint32_t v = cpu_to_le32(val);
        eg_gfx_fence(s, a, &v, 4, -1);  /* Apple's fence: after the rendering */
    }
}

/* a register write from the command stream */
static void eg_cp_reg_write(EvergreenState *s, uint32_t reg, uint32_t val)
{
    eg_reg_write(s, reg, val);
    if (reg >= EG_SCRATCH_REG0 && reg < EG_SCRATCH_REG0 + 32) {
        eg_scratch_write(s, reg, val);
    } else if (reg == EG_CP_INT_STATUS && (val & EG_RB_INT_STAT)) {
        eg_gfx_fence(s, 0, NULL, 0, EG_CP_RB_INT_SRC);
    }
}

static void eg_cp_run(EvergreenState *s, const uint32_t *pkt, uint32_t ndw);

static void eg_cp_ib(EvergreenState *s, uint64_t addr, uint32_t ndw)
{
    uint32_t *buf;

    if (s->ib_depth >= 2 || ndw > (1u << 20)) {
        qemu_log_mask(LOG_GUEST_ERROR, "evergreen: bad IB (depth %d, %u dwords)\n",
                      s->ib_depth, ndw);
        return;
    }
    buf = g_new(uint32_t, ndw ? ndw : 1);
    eg_gpu_read(s, addr, buf, ndw * 4);
    s->ib_depth++;
    eg_cp_run(s, buf, ndw);
    s->ib_depth--;
    g_free(buf);
}

/* one type-3 packet: d[0] is the first body dword, n body dwords */
static void eg_cp_pkt3(EvergreenState *s, uint32_t op, const uint32_t *d, uint32_t n)
{
    uint32_t i;

    trace_eg_cp_pkt3(op, n);
    switch (op) {
    case PKT3_SET_CONFIG_REG:
        for (i = 1; i < n; i++) {
            eg_cp_reg_write(s, 0x8000 + ((d[0] + i - 1) << 2), d[i]);
        }
        if (n > 1) {
            eg_gfx_shadow_write(s, 0, d[0], d + 1, n - 1);
        }
        break;
    case PKT3_SET_CONTEXT_REG:
        for (i = 1; i < n; i++) {
            eg_cp_reg_write(s, 0x28000 + ((d[0] + i - 1) << 2), d[i]);
        }
        if (n > 1) {
            eg_gfx_shadow_write(s, 1, d[0], d + 1, n - 1);
        }
        break;
    case PKT3_INDIRECT_BUFFER_PFP:      /* Apple's state preambles */
    case PKT3_INDIRECT_BUFFER:
        if (n >= 3) {
            eg_cp_ib(s, ((uint64_t)(d[1] & 0xff) << 32) | (d[0] & ~3u), d[2] & 0xfffff);
        }
        break;
    case PKT3_MEM_WRITE:                /* fences: after the rendering before them */
        if (n >= 4) {
            uint64_t a = ((uint64_t)(d[1] & 0xff) << 32) | (d[0] & ~3u);
            uint32_t v[2] = { cpu_to_le32(d[2]), cpu_to_le32(d[3]) };
            eg_gfx_fence(s, a, v, (d[1] & (1u << 18)) ? 4 : 8, -1);
        }
        break;
    case PKT3_EVENT_WRITE_EOP:
        if (n >= 5) {
            uint64_t a = ((uint64_t)(d[2] & 0xff) << 32) | (d[1] & ~3u);
            uint32_t data_sel = d[2] >> 29, int_sel = (d[2] >> 24) & 3;
            uint32_t v[2] = { cpu_to_le32(d[3]), cpu_to_le32(d[4]) };
            uint64_t ts = cpu_to_le64(qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
            int irq = int_sel && (s->regs[EG_CP_INT_CNTL >> 2] & EG_TIME_STAMP_INT_ENABLE) ?
                      EG_SRC_CP_EOP : -1;
            int len = data_sel == 1 ? 4 : data_sel == 2 || data_sel == 3 ? 8 : 0;
            eg_gfx_fence(s, a, data_sel == 3 ? (void *)&ts : (void *)v, len, irq);
        }
        break;
    case PKT3_CP_DMA:
        eg_gfx_flush(s);
        if (n >= 5) {
            uint64_t src = ((uint64_t)(d[1] & 0xff) << 32) | d[0];
            uint64_t dst = ((uint64_t)(d[3] & 0xff) << 32) | d[2];
            uint32_t len = d[4] & 0x1fffff;
            uint8_t *tmp = g_malloc(len ? len : 1);
            eg_gpu_read(s, src, tmp, len);
            eg_gpu_write(s, dst, tmp, len);
            g_free(tmp);
        }
        break;
    case PKT3_DISPATCH_DIRECT:
        eg_compute_dispatch(s, d, n);
        break;
    case PKT3_WAIT_REG_MEM:
    case PKT3_MEM_SEMAPHORE:
        /*
         * Nothing to wait for: the CP runs each packet to its end, and
         * every later reader of GPU results (CPU draws, CP_DMA, COPY_DW,
         * MEM_WRITE, EOP fences, the DMA engine, scanout) flushes first.
         */
        break;
    case PKT3_NOP:
    case PKT3_CLEAR_STATE:
    case PKT3_ME_INITIALIZE:
    case PKT3_SURFACE_SYNC:
    case PKT3_EVENT_WRITE:
    case PKT3_EVENT_WRITE_EOS:
        break;
    default:
        if (eg_gfx_packet(s, op, d, n)) {
            break;
        }
        if (s->debug & 2) {
            qemu_log("evergreen: PM4 type-3 opcode 0x%02x (%u dwords) ignored\n", op, n);
        }
        break;
    }
}

static void eg_cp_run(EvergreenState *s, const uint32_t *pkt, uint32_t ndw)
{
    uint32_t i = 0;

    while (i < ndw) {
        uint32_t h = le32_to_cpu(pkt[i]);
        uint32_t type = h >> 30, n = ((h >> 16) & 0x3fff) + 1;
        uint32_t body[16384], k;

        eg_gfx_poll_done(s);

        if (type == 2) {
            i++;
            continue;
        }
        if (i + 1 + n > ndw) {
            break;
        }
        for (k = 0; k < n && k < 16384; k++) {
            body[k] = le32_to_cpu(pkt[i + 1 + k]);
        }
        if (s->debug & 16) {
            g_autoptr(GString) l = g_string_new(NULL);
            g_string_append_printf(l, "evergreen: pm4 %u/%d %s%02x:", type, s->ib_depth,
                                   type == 0 ? "reg " : "", type == 0 ? (h & 0xffff) : (h >> 8) & 0xff);
            if (type == 0) {
                g_string_append_printf(l, " @%05x", (h & 0xffff) << 2);
            }
            for (k = 0; k < n && k < 12; k++) {
                g_string_append_printf(l, " %08x", body[k]);
            }
            qemu_log("%s\n", l->str);
        }
        if (type == 0) {
            uint32_t reg = (h & 0xffff) << 2;
            for (k = 0; k < n; k++) {
                eg_cp_reg_write(s, reg + 4 * k, body[k]);
            }
        } else if (type == 3) {
            eg_cp_pkt3(s, (h >> 8) & 0xff, body, MIN(n, 16384));
        }
        i += 1 + n;
    }
}

/* the ring has packets the CP has not run yet */
bool eg_cp_pending(EvergreenState *s)
{
    uint32_t size = 2u << (s->regs[EG_CP_RB_CNTL >> 2] & 0x3f);
    return s->regs[EG_CP_RB_BASE >> 2] &&
           s->cp_rptr != (qatomic_read(&s->regs[EG_CP_RB_WPTR >> 2]) & (size - 1));
}

void eg_cp_kick(EvergreenState *s)
{
    uint32_t cntl = s->regs[EG_CP_RB_CNTL >> 2];
    uint64_t base = (uint64_t)s->regs[EG_CP_RB_BASE >> 2] << 8;
    uint32_t size = 2u << (cntl & 0x3f);            /* dwords */
    uint32_t wptr = qatomic_read(&s->regs[EG_CP_RB_WPTR >> 2]) & (size - 1);
    uint32_t n, *buf, i;

    if (!base || s->cp_rptr == wptr) {
        return;
    }
    /* the packets between rptr and wptr, unwrapped */
    n = (wptr - s->cp_rptr) & (size - 1);
    buf = g_new(uint32_t, n);
    for (i = 0; i < n; ) {
        uint32_t at = (s->cp_rptr + i) & (size - 1);
        uint32_t run = MIN(n - i, size - at);
        eg_gpu_read(s, base + at * 4, buf + i, run * 4);
        i += run;
    }
    trace_eg_cp_kick(s->cp_rptr, wptr);
    eg_gfx_new_cp_run(s);
    eg_cp_run(s, buf, n);
    g_free(buf);
    eg_gfx_kick_done(s);
    s->cp_rptr = wptr;
    if (!(cntl & EG_RB_NO_UPDATE)) {
        uint64_t a = ((uint64_t)(s->regs[EG_CP_RB_RPTR_ADDR_HI >> 2] & 0xff) << 32) |
                     (s->regs[EG_CP_RB_RPTR_ADDR >> 2] & ~3u);
        uint32_t v = cpu_to_le32(s->cp_rptr);
        if (a) {
            eg_gpu_write(s, a, &v, 4);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Async DMA engine                                                    */
/* ------------------------------------------------------------------ */

#define DMA_WRITE           0x2
#define DMA_COPY            0x3
#define DMA_INDIRECT_BUFFER 0x4
#define DMA_SEMAPHORE       0x5
#define DMA_FENCE           0x6
#define DMA_TRAP            0x7
#define DMA_SRBM_WRITE      0x9
#define DMA_CONSTANT_FILL   0xd
#define DMA_NOP             0xf

static void eg_dma_run(EvergreenState *s, const uint32_t *p, uint32_t ndw);

/* returns the packet length in dwords (0: stop) */
static uint32_t eg_dma_packet(EvergreenState *s, const uint32_t *p, uint32_t avail)
{
    uint32_t h = le32_to_cpu(p[0]);
    uint32_t cmd = h >> 28, count = h & 0xfffff, sub = (h >> 20) & 0xff;
#define D(i) le32_to_cpu(p[i])

    trace_eg_dma_packet(cmd, sub, count);
    switch (cmd) {
    case DMA_WRITE: {
        /* r600-style headers (r600_dma.c) keep the count in bits 15:0 */
        uint32_t n = count & 0xffff, k;
        uint64_t a;
        if (avail < 3 + n) {
            return 0;
        }
        a = ((uint64_t)(D(2) & 0xff) << 32) | (D(1) & ~3u);
        for (k = 0; k < n; k++) {
            uint32_t v = p[3 + k];
            eg_gpu_write(s, a + 4 * k, &v, 4);
        }
        return 3 + n;
    }
    case DMA_COPY: {
        /* linear copy (sub-command 0): dst lo, src lo, dst hi, src hi */
        uint64_t dst, src;
        uint32_t len = count * 4;
        uint8_t *tmp;
        if (avail < 5) {
            return 0;
        }
        if (sub != 0) {
            qemu_log_mask(LOG_UNIMP, "evergreen: DMA copy sub-command 0x%x\n", sub);
            return 5;
        }
        dst = ((uint64_t)(D(3) & 0xff) << 32) | (D(1) & ~3u);
        src = ((uint64_t)(D(4) & 0xff) << 32) | (D(2) & ~3u);
        tmp = g_malloc(len ? len : 1);
        eg_gpu_read(s, src, tmp, len);
        eg_gpu_write(s, dst, tmp, len);
        g_free(tmp);
        return 5;
    }
    case DMA_INDIRECT_BUFFER: {
        uint64_t a;
        uint32_t n, *buf;
        if (avail < 3) {
            return 0;
        }
        a = ((uint64_t)(D(2) & 0xff) << 32) | (D(1) & ~0x1fu);
        n = (D(2) >> 12) & 0xfffff;
        if (s->ib_depth < 1) {
            buf = g_new(uint32_t, n ? n : 1);
            eg_gpu_read(s, a, buf, n * 4);
            s->ib_depth++;
            eg_dma_run(s, buf, n);
            s->ib_depth--;
            g_free(buf);
        }
        return 3;
    }
    case DMA_FENCE: {
        uint32_t v;
        if (avail < 4) {
            return 0;
        }
        v = p[3];
        eg_gpu_write(s, ((uint64_t)(D(2) & 0xff) << 32) | (D(1) & ~3u), &v, 4);
        return 4;
    }
    case DMA_TRAP:
        if (s->regs[EG_DMA_CNTL >> 2] & EG_DMA_TRAP_ENABLE) {
            eg_ih_push(s, EG_SRC_DMA_TRAP, 0);
        }
        return 1;
    case DMA_SRBM_WRITE:
        if (avail < 3) {
            return 0;
        }
        eg_reg_write(s, (D(1) & 0xffff) << 2, D(2));
        return 3;
    case DMA_SEMAPHORE:
        return 3;
    case DMA_CONSTANT_FILL: {
        /* dst lo, value, dst hi (bits 23:16); count in dwords */
        uint64_t a;
        uint32_t k;
        if (avail < 4) {
            return 0;
        }
        a = ((uint64_t)((D(3) >> 16) & 0xff) << 32) | (D(1) & ~3u);
        for (k = 0; k < count; k++) {
            uint32_t v = p[2];
            eg_gpu_write(s, a + 4 * k, &v, 4);
        }
        return 4;
    }
    case DMA_NOP:
        return 1;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "evergreen: DMA packet 0x%08x\n", h);
        return 1;
    }
#undef D
}

static void eg_dma_run(EvergreenState *s, const uint32_t *p, uint32_t ndw)
{
    uint32_t i = 0, n;

    while (i < ndw && (n = eg_dma_packet(s, p + i, ndw - i))) {
        i += n;
    }
}

void eg_dma_kick(EvergreenState *s)
{
    /* the DMA ring may wait on work of the graphics ring (semaphores) */
    if (eg_cp_pending(s)) {
        eg_cp_kick(s);
    }
    uint32_t cntl = s->regs[EG_DMA_RB_CNTL >> 2];
    uint64_t base = (uint64_t)s->regs[EG_DMA_RB_BASE >> 2] << 8;
    uint32_t size = 4u << ((cntl >> 1) & 0x1f);         /* bytes */
    uint32_t wptr = s->regs[EG_DMA_RB_WPTR >> 2] & (size - 1) & ~3u;
    uint32_t n, i, *buf;

    if (!(cntl & EG_DMA_RB_ENABLE) || s->dma_rptr == wptr) {
        return;
    }
    eg_gfx_flush(s);                    /* DMA copies read and write VRAM */
    n = ((wptr - s->dma_rptr) & (size - 1)) / 4;
    buf = g_new(uint32_t, n);
    for (i = 0; i < n; ) {
        uint32_t at = (s->dma_rptr + 4 * i) & (size - 1);
        uint32_t run = MIN(n - i, (size - at) / 4);
        eg_gpu_read(s, base + at, buf + i, run * 4);
        i += run;
    }
    eg_dma_run(s, buf, n);
    g_free(buf);
    s->dma_rptr = wptr;
    if (cntl & EG_DMA_RPTR_WRITEBACK_ENABLE) {
        uint64_t a = ((uint64_t)(s->regs[EG_DMA_RB_RPTR_ADDR_HI >> 2] & 0xff) << 32) |
                     (s->regs[EG_DMA_RB_RPTR_ADDR_LO >> 2] & ~3u);
        uint32_t v = cpu_to_le32(s->dma_rptr);
        eg_gpu_write(s, a, &v, 4);
    }
}
