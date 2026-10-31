/*
 * macOS: whether MoltenVK can be loaded at all.
 *
 * MoltenVK probes every Metal device while vkCreateInstance() runs, and
 * among other things makes an argument encoder on each.  A Metal device
 * below MTLGPUFamilyMac2 may not implement that: Apple's paravirtualized
 * GPU in an x86_64 macOS 15 VM ("Apple Paravirtual device": no Mac2
 * family, no raster order groups) abort()s in
 * -[MTLIOAccelDevice newIndirectArgumentEncoderWithLayout:], which takes
 * QEMU down before r5xx_vk_create() can fall back to the CPU path.  Every
 * GPU MoltenVK supports is in Mac2 (Intel, AMD and Apple GPUs alike), and
 * the backend needs pixel interlock, which MoltenVK builds on raster order
 * groups, so a host without such a device has nothing to lose.
 */
#import <Metal/Metal.h>
#include <stdbool.h>
#include <stdio.h>

bool r5xx_vk_metal_usable(char *err, size_t errlen);   /* r5xx_vk.h */

static bool metal_usable(NSArray<id<MTLDevice>> *devs, char *err, size_t errlen)
{
    bool rog = false;

    if (devs.count == 0) {
        snprintf(err, errlen, "no Metal devices");
        return false;
    }
    for (id<MTLDevice> d in devs) {
        if (![d supportsFamily:MTLGPUFamilyMac2]) {
            snprintf(err, errlen, "Metal device \"%s\" is not in MTLGPUFamilyMac2, "
                     "which MoltenVK needs", d.name.UTF8String);
            return false;
        }
        rog |= d.rasterOrderGroupsSupported;
    }
    if (!rog) {
        snprintf(err, errlen, "no Metal device has raster order groups "
                 "(pixel interlock)");
        return false;
    }
    return true;
}

bool r5xx_vk_metal_usable(char *err, size_t errlen)
{
    @autoreleasepool {
        NSArray<id<MTLDevice>> *devs = MTLCopyAllDevices();
        bool ok = metal_usable(devs, err, errlen);

#if !__has_feature(objc_arc)
        [devs release];
#endif
        return ok;
    }
}
