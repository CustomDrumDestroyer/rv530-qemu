/*
 * Evergreen Vulkan backend stub for builds without Vulkan / shaderc.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdio.h>
#include "eg_vk.h"

EgVk *eg_vk_create(void *vram, uint64_t vram_size, EgVkDirtyFn dirty, void *opaque,
                   char *err, size_t errlen)
{
    snprintf(err, errlen, "QEMU was built without Vulkan and shaderc");
    return NULL;
}

void eg_vk_destroy(EgVk *vk)
{
}

bool eg_vk_draw(EgVk *vk, const EgVkDrawReq *req, char *why, size_t whylen)
{
    return false;
}

bool eg_vk_has_pipeline(EgVk *vk, uint64_t prog_hash)
{
    return false;
}

bool eg_vk_set_sysram(EgVk *vk, void *ram, uint64_t size, char *err, size_t errlen)
{
    return false;
}

uint32_t *eg_vk_gart_table(EgVk *vk, uint32_t npages)
{
    return NULL;
}

bool eg_vk_mirrored(const EgVk *vk)
{
    return false;
}

int eg_vk_set_cache_dir(EgVk *vk, const char *dir)
{
    return 0;
}

void eg_vk_new_cp_run(EgVk *vk)
{
}

void eg_vk_set_wait_fn(EgVk *vk, EgVkWaitFn fn)
{
}

void eg_vk_upload(EgVk *vk, uint64_t off, uint64_t len)
{
}

void eg_vk_upload_all_now(EgVk *vk)
{
}

void eg_vk_upload_kick(EgVk *vk)
{
}

void eg_vk_submit(EgVk *vk)
{
}

uint64_t eg_vk_submitted(const EgVk *vk)
{
    return 0;
}

uint64_t eg_vk_poll(EgVk *vk)
{
    return 0;
}

uint64_t eg_vk_flush(EgVk *vk)
{
    return 0;
}

uint32_t eg_vk_batch_draws(const EgVk *vk)
{
    return 0;
}

bool eg_vk_pending(const EgVk *vk)
{
    return false;
}

uint64_t eg_vk_wait_ns(const EgVk *vk)
{
    return 0;
}

void eg_vk_stats(const EgVk *vk, uint64_t *draws, uint64_t *batches, uint64_t *pipelines,
                 uint64_t *barriers)
{
    *draws = *batches = *pipelines = *barriers = 0;
}

void eg_vk_set_notify_fn(EgVk *vk, void (*fn)(void *opaque), void *opaque)
{
}

uint64_t eg_vk_gpu_done_seq(const EgVk *vk)
{
    return 0;
}

uint64_t eg_vk_binds(const EgVk *vk)
{
    return 0;
}

uint32_t eg_vk_zpass_slot(const EgVk *vk)
{
    return 0;
}

void eg_vk_zpass_set(EgVk *vk, uint32_t slot)
{
}

uint32_t eg_vk_zpass_take(EgVk *vk, uint32_t slot)
{
    return 0;
}

void eg_vk_done_latency(const EgVk *vk, uint64_t *ns, uint64_t *n)
{
    *ns = 0;
    *n = 0;
}
