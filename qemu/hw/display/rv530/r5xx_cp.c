/*
 * RV530 command processor: ring buffer, indirect buffers, PM4 packets,
 * read-pointer write-back, scratch registers and the software interrupt.
 *
 * The CP is executed on the main loop (bottom half) or synchronously on
 * the vCPU thread when the guest polls a status register, which keeps
 * the model deterministic.  Both paths hold the BQL.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/main-loop.h"
#include "rv530.h"
#include "r5xx_vk.h"
#include "trace.h"

#define CP_PACKET_TYPE(h)     (((h) >> 30) & 3)
#define CP_PACKET0_REG(h)     (((h) & 0x1fff) << 2)
#define CP_PACKET0_COUNT(h)   ((((h) >> 16) & 0x3fff) + 1)
#define CP_PACKET0_ONE_REG    (1u << 15)
#define CP_PACKET3_OPCODE(h)  (((h) >> 8) & 0xff)
#define CP_PACKET3_COUNT(h)   ((((h) >> 16) & 0x3fff) + 1)

#define PM4_NOP               0x10
#define PM4_WAIT_FOR_IDLE     0x26
#define PM4_LOAD_MICROCODE    0x24

#define MAX_IB_DWORDS         (1u << 20)

static void rv530_cp_bh(void *opaque)
{
    RV530State *s = opaque;
    RV530Stall st;

    rv530_stall_start(s, &st);
    rv530_cp_run(s);
    rv530_stall_end(s, &st, "CP bottom half", 0);
}

void rv530_cp_init(RV530State *s)
{
    s->cp_bh = qemu_bh_new_guarded(rv530_cp_bh, s,
                                   &DEVICE(s)->mem_reentrancy_guard);
}

void rv530_cp_reset(RV530State *s)
{
    s->cp_busy = false;
    s->cp_rptr = s->cp_wptr = 0;
    s->cp_ring_mask = 0;
    s->cp_ring_base = 0;
    s->cp_rptr_addr = 0;
    s->cp_ib_depth = 0;
    s->regs[RV_CP_RB_CNTL >> 2] = 0;
    s->regs[RV_CP_CSQ_CNTL >> 2] = 0;
    s->regs[RV_CP_CSQ_MODE >> 2] = 0;
    s->regs[RV_SCRATCH_UMSK >> 2] = 0;
}

static void rv530_cp_writeback_rptr(RV530State *s, uint32_t rptr)
{
    uint32_t cntl = s->regs[RV_CP_RB_CNTL >> 2];

    /*
     * With several vCPUs the guest may act on the new RPTR at once, read
     * from memory or from the register (CP_RB_RPTR reads are served while
     * a CP run waits unlocked): finish the GPU work it covers before
     * publishing it.  The flush itself may wait unlocked, so cp_rptr only
     * moves afterwards.
     */
    rv530_gfx_flush(s);
    s->cp_rptr = rptr;
    s->regs[RV_CP_RB_RPTR >> 2] = s->cp_rptr;
    if (s->cp_rptr_addr && !(cntl & RV_RB_NO_UPDATE)) {
        rv530_gpu_write32(s, s->cp_rptr_addr, s->cp_rptr);
    }
}

/* Execute one PM4 stream from GPU memory.  Returns the number of dwords
 * consumed. */
static uint32_t rv530_cp_execute(RV530State *s, uint32_t base,
                                 uint32_t ndw, uint32_t mask, uint32_t pos)
{
    uint32_t start = pos;

    while (pos - start < ndw) {
        uint32_t hdr = rv530_gpu_read32(s, base + ((pos & mask) << 2));
        uint32_t type = CP_PACKET_TYPE(hdr);
        uint32_t i;

        pos++;
        s->cp_packets++;
        switch (type) {
        case 0: {
            uint32_t reg = CP_PACKET0_REG(hdr);
            uint32_t count = CP_PACKET0_COUNT(hdr);
            bool one_reg = hdr & CP_PACKET0_ONE_REG;
            trace_rv530_pm4_type0(reg, r5xx_regname(reg) ?: "?", count, one_reg);
            for (i = 0; i < count; i++) {
                uint32_t v = rv530_gpu_read32(s, base + ((pos & mask) << 2));
                pos++;
                rv530_reg_write(s, one_reg ? reg : reg + i * 4, v);
            }
            break;
        }
        case 2:
            /* padding */
            break;
        case 3: {
            uint32_t op = CP_PACKET3_OPCODE(hdr);
            uint32_t count = CP_PACKET3_COUNT(hdr);
            uint32_t *data = s->pm4_buf;
            trace_rv530_pm4_type3(op, r5xx_pm4_opname(op) ?: "?", count);
            /* payload may wrap around the end of the ring */
            for (i = 0; i < count; i++) {
                data[i] = rv530_gpu_read32(s, base + (((pos + i) & mask) << 2));
            }
            switch (op) {
            case PM4_NOP:
            case PM4_WAIT_FOR_IDLE:
                break;
            case PM4_LOAD_MICROCODE:
                break;
            default:
                /* 2D packets, then 3D and everything else */
                if (!rv530_2d_pm4(s, op, data, count)) {
                    rv530_gfx_pm4(s, op, data, count);
                }
                break;
            }
            pos += count;
            break;
        }
        default:
            qemu_log_mask(LOG_GUEST_ERROR,
                          "rv530: CP: invalid packet type 1 header 0x%08x at %u\n",
                          hdr, pos - 1);
            break;
        }
    }
    return pos - start;
}

void rv530_cp_execute_buffer(RV530State *s, uint32_t addr, uint32_t ndw)
{
    if (s->cp_ib_depth >= 2) {
        qemu_log_mask(LOG_GUEST_ERROR, "rv530: CP: nested IB ignored\n");
        return;
    }
    if (ndw > MAX_IB_DWORDS) {
        qemu_log_mask(LOG_GUEST_ERROR, "rv530: CP: IB too large (%u dw)\n", ndw);
        return;
    }
    trace_rv530_cp_ib(addr, ndw);
    if (!s->cp_ib_depth && r5xx_vk_mirrored(s->vk) && r5xx_vk_pending(s->vk)) {
        /* an IB is one guest submission and a GPU batch spans several:
         * what the guest CPU wrote before this one (a texture it just
         * drew, say) must reach the GPU's copy of VRAM too */
        rv530_collect_dirty(s);
    }
    s->cp_ib_depth++;
    rv530_cp_execute(s, addr, ndw, 0xffffffff, 0);
    s->cp_ib_depth--;
}

void rv530_cp_run(RV530State *s)
{
    /* also not while another thread's CP work waits with the BQL released
     * (the bottom half on the main loop): that thread finishes the ring */
    if (s->cp_running || s->cp_unlocked) {
        return;
    }
    r5xx_vk_new_cp_run(s->vk);
    s->cp_runs++;
    s->cp_running = true;
    while (s->cp_rptr != s->cp_wptr && (s->regs[RV_CP_CSQ_CNTL >> 2] != 0)) {
        uint32_t avail = (s->cp_wptr - s->cp_rptr) & s->cp_ring_mask;
        uint32_t used;
        if (!avail) {
            break;
        }
        used = rv530_cp_execute(s, s->cp_ring_base, avail, s->cp_ring_mask,
                                s->cp_rptr);
        rv530_cp_writeback_rptr(s, (s->cp_rptr + used) & s->cp_ring_mask);
    }
    if (s->cp_rptr == s->cp_wptr || !s->regs[RV_CP_CSQ_CNTL >> 2]) {
        s->cp_busy = false;
    }
    s->cp_running = false;
    rv530_gfx_flush(s);
}

static void rv530_cp_kick(RV530State *s)
{
    if (s->cp_rptr == s->cp_wptr) {
        return;
    }
    s->cp_busy = true;
    if (s->cp_sync) {
        rv530_cp_run(s);
    } else {
        qemu_bh_schedule(s->cp_bh);
    }
}

bool rv530_cp_reg_read(RV530State *s, uint32_t off, uint32_t *val)
{
    switch (off) {
    case RV_CP_RB_RPTR:
        if (s->cp_busy) {
            rv530_cp_run(s);
        }
        *val = s->cp_rptr;
        return true;
    case RV_CP_RB_WPTR:
        *val = s->cp_wptr;
        return true;
    case RV_CP_ME_RAM_DATAH:
    case RV_CP_ME_RAM_DATAL:
        *val = 0;
        return true;
    default:
        return false;
    }
}

bool rv530_cp_reg_write(RV530State *s, uint32_t off, uint32_t val)
{
    uint32_t idx = off >> 2;

    switch (off) {
    case RV_CP_RB_CNTL: {
        uint32_t bufsz = val & 0x3f;
        s->regs[idx] = val;
        s->cp_ring_mask = (2u << bufsz) - 1;        /* dwords */
        if (val & RV_RB_RPTR_WR_ENA) {
            s->cp_rptr = s->regs[RV_CP_RB_RPTR_WR >> 2] & s->cp_ring_mask;
        }
        trace_rv530_cp_ring_cntl(val, s->cp_ring_mask + 1);
        return true;
    }
    case RV_CP_RB_BASE:
        s->regs[idx] = val;
        s->cp_ring_base = val & ~0xffu;             /* 256 byte aligned */
        return true;
    case RV_CP_RB_RPTR_ADDR:
        s->regs[idx] = val;
        s->cp_rptr_addr = val & ~3u;                /* byte address, bits 1:0 are control */
        return true;
    case RV_CP_RB_RPTR_WR:
        s->regs[idx] = val;
        if (s->regs[RV_CP_RB_CNTL >> 2] & RV_RB_RPTR_WR_ENA) {
            s->cp_rptr = val & s->cp_ring_mask;
        }
        return true;
    case RV_CP_RB_WPTR:
        s->cp_wptr = val & s->cp_ring_mask;
        trace_rv530_cp_wptr(s->cp_wptr, s->cp_rptr);
        rv530_cp_kick(s);
        return true;
    case RV_CP_RB_RPTR:
        return true;
    case RV_CP_IB_BASE:
        s->regs[idx] = val;
        return true;
    case RV_CP_IB_BUFSZ:
        s->regs[idx] = val;
        rv530_cp_execute_buffer(s, s->regs[RV_CP_IB_BASE >> 2], val);
        return true;
    case RV_CP_CSQ_CNTL:
        s->regs[idx] = val;
        if (val) {
            rv530_cp_kick(s);
        }
        return true;
    case RV_CP_ME_RAM_ADDR:
        s->regs[idx] = val;
        if (val == 0) {
            s->cp_me_hash = 0;
            s->cp_me_words = 0;
        }
        return true;
    case RV_CP_ME_RAM_DATAH:
    case RV_CP_ME_RAM_DATAL:
        /* accept the microcode upload and remember a hash of it */
        s->cp_me_hash = s->cp_me_hash * 31 + val;
        s->cp_me_words++;
        if (s->cp_me_words == 512) {
            trace_rv530_cp_microcode(s->cp_me_words, s->cp_me_hash);
        }
        return true;
    default:
        return false;
    }
}
