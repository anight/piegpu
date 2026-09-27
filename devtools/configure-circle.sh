#!/bin/bash
#
# Configure Circle (./circle, not in git) for the pico-gpu apps, then rebuild
# its libraries:
#
#   devtools/configure-circle.sh
#
# Pi Zero (RASPPI=1, 32 bit), the Arm GNU 15.2 toolchain, and heap buckets up
# to 64 MB: Circle's heap puts a freed block back on the free list of its size
# class (bucket) only if there is one - a block bigger than the largest bucket
# (512 KB by default) is lost for good when freed (circle/include/circle/
# sysconfig.h, HEAP_BLOCK_BUCKET_SIZES). The gpu app allocates textures (up to
# ~22 MB with mipmaps at 2048x2048), buffers and depth/stencil storage from the
# heap, and GL programs create and delete them all the time: with the default
# buckets the dEQP texture tests ran the Zero out of memory within minutes.
# PGPU_HEAP_BUCKETS tells the gpu app it was built this way (gpu/textures.cpp).
#
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
cd "$HERE/../circle"
./configure -r 1 -f \
	-p "$HOME/toolchains/arm-gnu-toolchain-15.2.rel1-x86_64-arm-none-eabi/bin/arm-none-eabi-" \
	-d HEAP_BLOCK_BUCKET_SIZES=0x40,0x400,0x1000,0x4000,0x10000,0x40000,0x80000,0x100000,0x200000,0x400000,0x800000,0x1000000,0x2000000,0x4000000 \
	-d PGPU_HEAP_BUCKETS
./makeall clean >/dev/null
./makeall
