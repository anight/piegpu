#!/bin/bash
#
# Configure Circle (not in git) for the pico-gpu apps on one board, then
# rebuild its libraries:
#
#   devtools/configure-circle.sh [zero|zero2]
#
#   zero    Pi Zero / Zero W: ./circle, RASPPI=1, 32 bit (kernel.img)
#   zero2   Pi Zero 2 W: ./circle-zero2, RASPPI=3, 64 bit (kernel8.img). The
#           tree is made from ./circle (the same commit) and patches/ if it
#           isn't there.
#
# Circle builds in its source tree, so each board has its own. Toolchains:
# Arm GNU 15.2 (arm-none-eabi for zero, aarch64-none-elf for zero2) in
# ~/toolchains; Circle takes the architecture from the prefix.
#
# Heap buckets up to 64 MB: Circle's heap puts a freed block back on the free
# list of its size class (bucket) only if there is one - a block bigger than
# the largest bucket (512 KB by default) is lost for good when freed (circle/
# include/circle/sysconfig.h, HEAP_BLOCK_BUCKET_SIZES). The gpu app allocates
# textures (up to ~22 MB with mipmaps at 2048x2048), buffers and depth/stencil
# storage from the heap, and GL programs create and delete them all the time:
# with the default buckets the dEQP texture tests ran the Zero out of memory
# within minutes. PGPU_HEAP_BUCKETS tells the gpu app it was built this way
# (gpu/textures.cpp).
#
# MEM_PERSISTENT_SIZE (patches/circle-persistent-memory.patch): Circle leaves
# the top 64 KB of the ARM memory to the app, for the log that survives a
# restart (devtools/runlog.h).
#
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$HERE/..
TOOLCHAINS=$HOME/toolchains
PATCHES="circle-cdc-endpoint-gadget circle-cdc-rx-overrun circle-cdc-short-packets
	 circle-ep0-vendor-in circle-fatfs-mkfs circle-cdc-throughput
	 circle-persistent-memory"	# (in this order)
PATCHES64="circle-vcos-aarch64"		# and for a 64-bit tree

case "${1:-zero}" in
zero)
	TREE=$ROOT/circle
	RASPPI=1
	PREFIX=$TOOLCHAINS/arm-gnu-toolchain-15.2.rel1-x86_64-arm-none-eabi/bin/arm-none-eabi-
	;;
zero2)
	TREE=$ROOT/circle-zero2
	RASPPI=3
	PREFIX=$TOOLCHAINS/arm-gnu-toolchain-15.2.rel1-x86_64-aarch64-none-elf/bin/aarch64-none-elf-
	if [ ! -d "$TREE" ]; then
		git clone -q "$ROOT/circle" "$TREE"
		git -C "$TREE" checkout -q "$(git -C "$ROOT/circle" rev-parse HEAD)"
		for p in $PATCHES $PATCHES64; do
			git -C "$TREE" apply "$ROOT/patches/$p.patch"
		done
	fi
	;;
*)
	echo "usage: $0 [zero|zero2]" >&2
	exit 1
	;;
esac

cd "$TREE"
./configure -r "$RASPPI" -f -p "$PREFIX" \
	-d HEAP_BLOCK_BUCKET_SIZES=0x40,0x400,0x1000,0x4000,0x10000,0x40000,0x80000,0x100000,0x200000,0x400000,0x800000,0x1000000,0x2000000,0x4000000 \
	-d PGPU_HEAP_BUCKETS -d MEM_PERSISTENT_SIZE=0x10000
./makeall clean >/dev/null
./makeall
# the add-ons the gpu app links (gpu/Makefile: LIBS)
for addon in addon/SDCard addon/fatfs addon/linux addon/vc4/interface/vcos addon/vc4/vchiq; do
	make -C "$addon" clean >/dev/null
	make -C "$addon"
done
