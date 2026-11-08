#!/bin/sh
# Build qemu/hw/display/evergreen/scripts/eg_vk_replay (the Evergreen Vulkan backend outside QEMU).
# Needs the Vulkan and shaderc development files.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
EG=$HERE/..
OUT=${1:-$HERE/eg_vk_replay}
cc -O1 -g -Wall -I"$EG" -o "$OUT" "$HERE/eg_vk_replay.c" "$EG/eg_vk.c" "$EG/eg_vk_ps.c" \
    $(pkg-config --cflags --libs vulkan shaderc) -lpthread -lm
echo "$OUT"
