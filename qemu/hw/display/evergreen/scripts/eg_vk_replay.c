/*
 * Run one dumped Evergreen draw (EG_VK_VERIFY=1 EG_VK_DUMP_DIR=..., the
 * <base>.replay file) through the Vulkan backend outside QEMU.
 *
 *   eg_vk_replay ps-<hash>.replay ps.glsl|ps.bin out.bin [surface]
 *
 * ps.glsl is the translated program (<base>.glsl, or an edited copy);
 * ps.bin is the microcode (<base>.bin), translated again here.
 * EG_REPLAY_REPEAT=n draws it n times and prints the GPU time (with
 * EG_VK_MIRROR=1: on the device-local VRAM mirror).
 * out.bin receives the surface (default 0) as the GPU left it, raw and
 * tiled like <base>.s0.gpu.  Build with eg_vk_replay.sh.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <time.h>
#include <sys/mman.h>
#include "eg_vk.h"

#define VRAM_SIZE (1ull << 30)

static uint8_t *slurp(const char *fn, size_t *len)
{
    FILE *f = fopen(fn, "rb");
    uint8_t *d;
    long n;

    if (!f) {
        perror(fn);
        exit(1);
    }
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    d = malloc(n + 1);
    if (fread(d, 1, n, f) != (size_t)n) {
        perror(fn);
        exit(1);
    }
    d[n] = 0;
    fclose(f);
    *len = n;
    return d;
}

static const uint8_t *ps_code;
static size_t ps_len;

static const uint32_t *fetch(void *opaque, uint32_t dw, uint32_t n)
{
    return (dw + n) * 4 <= ps_len ? (const uint32_t *)ps_code + dw : NULL;
}

static uint64_t fnv(const char *s)
{
    uint64_t h = 0xcbf29ce484222325ull;
    while (*s) {
        h = (h ^ (uint8_t)*s++) * 0x100000001b3ull;
    }
    return h;
}

int main(int argc, char **argv)
{
    size_t len, glen;
    uint8_t *f, *p, *vram;
    char *glsl, err[512];
    const uint32_t *w;
    EgVkDrawReq req;
    EvkDraw draw;
    EgVk *vk;
    int k, nreg, surf = argc > 4 ? atoi(argv[4]) : 0, nwr = 0, tries;
    uint64_t wr_off[10], wr_len[10];
    static uint8_t sysram[1 << 20] __attribute__((aligned(4096)));

    if (argc < 4) {
        fprintf(stderr, "usage: %s draw.replay ps.glsl out.bin [surface]\n", argv[0]);
        return 1;
    }
    f = slurp(argv[1], &len);
    glsl = (char *)slurp(argv[2], &glen);
    w = (const uint32_t *)f;
    if (w[0] != 0x524b5645 || w[1] != sizeof(EvkDraw) || w[2] != sizeof(EvkVertex)) {
        fprintf(stderr, "not a replay file for this build\n");
        return 1;
    }
    vram = mmap(NULL, VRAM_SIZE, PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    memset(&req, 0, sizeof(req));
    p = f + 32;
    memcpy(&draw, p, sizeof(draw));
    p += sizeof(draw);
    memcpy(&req.sc_x0, p, 4);
    memcpy(&req.sc_y0, p + 4, 4);
    memcpy(&req.sc_x1, p + 8, 4);
    memcpy(&req.sc_y1, p + 12, 4);
    p += 16 + 8;
    req.draw = &draw;
    req.nverts = w[3];
    req.verts = (const EvkVertex *)p;
    p += (size_t)w[3] * sizeof(EvkVertex);
    nreg = w[4];
    for (k = 0; k < nreg; k++) {
        uint64_t h[3];
        memcpy(h, p, sizeof(h));
        p += sizeof(h);
        if (h[0] + h[1] <= VRAM_SIZE) {
            memcpy(vram + h[0], p, h[1]);
        } else {
            fprintf(stderr, "region %d at %llx: not in VRAM, left empty\n", k,
                    (unsigned long long)h[0]);
        }
        if (h[2]) {
            req.wr_off[req.nwr] = h[0];
            req.wr_len[req.nwr] = h[1];
            wr_off[nwr] = h[0];
            wr_len[nwr++] = h[1];
            req.nwr++;
        } else {
            req.rd_off[req.nrd] = h[0];
            req.rd_len[req.nrd++] = h[1];
        }
        p += h[1];
    }
    if (strlen(argv[2]) > 4 && !strcmp(argv[2] + strlen(argv[2]) - 4, ".bin")) {
        EvkProgInfo info;
        ps_code = (const uint8_t *)glsl;
        ps_len = glen;
        glsl = evk_ps_to_glsl(fetch, NULL, draw.misc[EVK_M_BARYC], draw.misc[EVK_M_IN0],
                              draw.misc[EVK_M_IN1], &info, err, sizeof(err));
        if (!glsl) {
            fprintf(stderr, "translation failed: %s\n", err);
            return 1;
        }
    }
    req.ps_glsl = glsl;
    req.prog_hash = fnv(glsl);

    setenv("EG_VK_SYNC_COMPILE", "1", 0);
    vk = eg_vk_create(vram, VRAM_SIZE, NULL, NULL, err, sizeof(err));
    if (!vk) {
        fprintf(stderr, "eg_vk_create: %s\n", err);
        return 1;
    }
    if (!eg_vk_set_sysram(vk, sysram, sizeof(sysram), err, sizeof(err))) {
        fprintf(stderr, "sysram: %s\n", err);
        return 1;
    }
    eg_vk_gart_table(vk, 16);
    if (eg_vk_mirrored(vk)) {
        eg_vk_upload_all_now(vk);
    }
    for (tries = 0; !eg_vk_draw(vk, &req, err, sizeof(err)); tries++) {
        if (strcmp(err, "compiling") || tries > 1000) {
            fprintf(stderr, "eg_vk_draw: %s\n", err);
            return 1;
        }
        struct timespec ts = { 0, 10000000 };
        nanosleep(&ts, NULL);
    }
    eg_vk_flush(vk);
    if (getenv("EG_REPLAY_REPEAT")) {
        int r, nrep = atoi(getenv("EG_REPLAY_REPEAT"));
        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        for (r = 0; r < nrep; r++) {
            eg_vk_draw(vk, &req, err, sizeof(err));
        }
        eg_vk_flush(vk);
        clock_gettime(CLOCK_MONOTONIC, &t1);
        fprintf(stderr, "%d draws of %u vertices: %.2f ms\n", nrep, req.nverts,
                (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6);
    }
    if (surf >= nwr) {
        fprintf(stderr, "no surface %d\n", surf);
        return 1;
    }
    {
        FILE *o = fopen(argv[3], "wb");
        fwrite(vram + wr_off[surf], 1, wr_len[surf], o);
        fclose(o);
    }
    eg_vk_destroy(vk);
    return 0;
}
