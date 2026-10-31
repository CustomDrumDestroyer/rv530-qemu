/*
 * Vulkan backend stub for builds without Vulkan / shaderc.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdio.h>
#include "r5xx_vk.h"

R5xxVk *r5xx_vk_create(void *vram, uint64_t vram_size, R5xxVkDirtyFn dirty,
                       void *opaque, char *err, size_t errlen)
{
    snprintf(err, errlen, "QEMU was built without Vulkan and shaderc");
    return NULL;
}

void r5xx_vk_destroy(R5xxVk *vk)
{
}

bool r5xx_vk_draw(R5xxVk *vk, const R5xxVkDrawReq *req, char *why, size_t len)
{
    return false;
}

uint64_t r5xx_vk_flush(R5xxVk *vk)
{
    return 0;
}

bool r5xx_vk_pending(const R5xxVk *vk)
{
    return false;
}

void r5xx_vk_stats(const R5xxVk *vk, uint64_t *draws, uint64_t *batches,
                   uint64_t *pipelines)
{
    *draws = *batches = *pipelines = 0;
}

bool r5xx_vk_set_sysram(R5xxVk *vk, void *ram, uint64_t size, char *err,
                        size_t errlen)
{
    return false;
}

uint32_t *r5xx_vk_gart_table(R5xxVk *vk, uint32_t npages)
{
    return NULL;
}

bool r5xx_vk_mirrored(const R5xxVk *vk)
{
    return false;
}

void r5xx_vk_upload(R5xxVk *vk, uint64_t off, uint64_t len)
{
}

int r5xx_vk_set_cache_dir(R5xxVk *vk, const char *dir)
{
    return 0;
}

void r5xx_vk_new_cp_run(R5xxVk *vk)
{
}

void r5xx_vk_set_wait_fn(R5xxVk *vk, R5xxVkWaitFn fn)
{
}

void r5xx_vk_upload_all_now(R5xxVk *vk)
{
}

void r5xx_vk_upload_kick(R5xxVk *vk)
{
}

uint64_t *r5xx_vk_check_scan(R5xxVk *vk)
{
    return NULL;
}

void r5xx_vk_check_missed(R5xxVk *vk, uint64_t pg)
{
}
