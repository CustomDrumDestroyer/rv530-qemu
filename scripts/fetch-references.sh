#!/bin/bash
# Fetch the reference sources the device model was written against:
# Linux radeon driver files, Mesa 24.0.9 (r300 driver + compiler), R520 CP firmware.
# Usage: scripts/fetch-references.sh [dir]   (default: ./reference)
set -euo pipefail
DIR=${1:-$(cd "$(dirname "$0")/.." && pwd)/reference}
mkdir -p "$DIR/linux-radeon" "$DIR/mesa"
cd "$DIR/linux-radeon"
for f in pptable.h r500_reg.h rv515d.h avivod.h r300d.h r300_reg.h r520d.h rv515.c r520.c r300.c r100.c r100d.h \
         radeon_reg.h atom.c atom.h atom-bits.h atom-names.h atom-types.h atombios.h atombios_crtc.c \
         atombios_encoders.c radeon_atombios.c radeon_bios.c radeon_ring.c radeon_fence.c radeon_gart.c \
         radeon_device.c radeon_irq_kms.c radeon_cs.c radeon.h radeon_asic.c radeon_asic.h radeon_cursor.c \
         radeon_display.c radeon_i2c.c radeon_object.c radeon_kms.c rs600.c rs600d.h r420.c radeon_mode.h \
         radeon_connectors.c radeon_encoders.c; do
    [ -f "$f" ] || curl -fsS --retry 4 -o "$f" "https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/plain/drivers/gpu/drm/radeon/$f"
done
cd "$DIR"
[ -f mesa-24.0.9.tar.xz ] || curl -fsS --retry 4 -O https://archive.mesa3d.org/mesa-24.0.9.tar.xz
tar -xJf mesa-24.0.9.tar.xz -C mesa --strip-components=1 \
    mesa-24.0.9/src/gallium/drivers/r300 mesa-24.0.9/src/gallium/include mesa-24.0.9/src/util \
    mesa-24.0.9/src/compiler mesa-24.0.9/src/mesa/program mesa-24.0.9/include \
    mesa-24.0.9/src/gallium/auxiliary
[ -f R520_cp.bin ] || curl -fsS --retry 4 -o R520_cp.bin "https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git/plain/radeon/R520_cp.bin"
echo "references in $DIR"
