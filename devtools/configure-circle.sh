#!/bin/bash
#
# Configure Circle (not in git) for the piegpu apps on one board, then
# rebuild its libraries:
#
#   devtools/configure-circle.sh [zero|zero2]
#
#   zero    Pi Zero / Zero W: ./circle, RASPPI=1, 32 bit (kernel.img)
#   zero2   Pi Zero 2 W: ./circle-zero2, RASPPI=3, 64 bit (kernel8.img)
#
# A tree that isn't there is cloned from piegpu's fork of Circle: the branch
# "piegpu" of github.com/anight/circle, Circle Step51.1 with piegpu's changes
# as commits (the CDC and EP0 gadget, FatFs' f_mkfs, MEM_PERSISTENT_SIZE,
# vcos in 64 bit). CIRCLE_REPO and CIRCLE_BRANCH pick another. A tree that
# is there is used as it is.
#
# These two are the boards piegpu supports for now; more RPi boards are to
# come.
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
# with the default buckets the dEQP texture tests ran the RPi out of memory
# within minutes. PGPU_HEAP_BUCKETS tells the gpu app it was built this way
# (gpu/textures.cpp).
#
# MEM_PERSISTENT_SIZE (the fork's "Memory: MEM_PERSISTENT_SIZE ..." commit):
# Circle leaves the top 64 KB of the ARM memory to the app, for the log that
# survives a restart (devtools/runlog.h).
#
# ARM_ALLOW_MULTI_CORE (the Zero 2 W, four cores): the gpu app decodes the
# audio stream's AAC on core 1 (gpu/audio); the Zero has one core.
#
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$HERE/..
TOOLCHAINS=$HOME/toolchains
JOBS=$(( $(nproc) > 1 ? $(nproc) - 1 : 1 ))	# a core left free
CIRCLE_REPO=${CIRCLE_REPO:-https://github.com/anight/circle.git}
CIRCLE_BRANCH=${CIRCLE_BRANCH:-piegpu}

case "${1:-zero}" in
zero)
	TREE=$ROOT/circle
	RASPPI=1
	MULTICORE=
	PREFIX=$TOOLCHAINS/arm-gnu-toolchain-15.2.rel1-x86_64-arm-none-eabi/bin/arm-none-eabi-
	;;
zero2)
	TREE=$ROOT/circle-zero2
	RASPPI=3
	MULTICORE="-d ARM_ALLOW_MULTI_CORE"
	PREFIX=$TOOLCHAINS/arm-gnu-toolchain-15.2.rel1-x86_64-aarch64-none-elf/bin/aarch64-none-elf-
	;;
*)
	echo "usage: $0 [zero|zero2]" >&2
	exit 1
	;;
esac

if [ ! -d "$TREE" ]; then
	git clone -q --branch "$CIRCLE_BRANCH" "$CIRCLE_REPO" "$TREE"
fi

cd "$TREE"
./configure -r "$RASPPI" -f -p "$PREFIX" \
	-d HEAP_BLOCK_BUCKET_SIZES=0x40,0x400,0x1000,0x4000,0x10000,0x40000,0x80000,0x100000,0x200000,0x400000,0x800000,0x1000000,0x2000000,0x4000000 \
	-d PGPU_HEAP_BUCKETS -d MEM_PERSISTENT_SIZE=0x10000 $MULTICORE
./makeall clean >/dev/null
MAKE="make -j$JOBS" ./makeall
# the add-ons the gpu app links (gpu/Makefile: LIBS)
for addon in addon/SDCard addon/fatfs addon/linux addon/vc4/interface/vcos addon/vc4/vchiq addon/vc4/sound; do
	make -C "$addon" clean >/dev/null
	make -C "$addon" -j"$JOBS"
done
