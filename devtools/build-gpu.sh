#!/bin/bash
#
# Build the gpu app for a board, with CMake on Circle's CMake build
# (third_party/circle, the submodule: piegpu's fork, github.com/anight/circle, branch piegpu):
#
#   devtools/build-gpu.sh [zero|zerow|zero2|all]
#
#   zero    Pi Zero, 32 bit: build/zero/kernel.img
#   zerow   Pi Zero W, 32 bit: build/zerow/kernel.img
#   zero2   Pi Zero 2 W, 64 bit: build/zero2/kernel8.img
#
# (The boards piegpu supports for now; more RPi boards are to come.) One
# Circle tree serves them all: each board has its build directory, configured
# here the first time, and again when its Circle options (below) aren't the
# ones it was configured with. The submodule and
# Circle's boot files (third_party/circle/boot: bootcode.bin, start.elf, fixup.dat) are
# fetched if they aren't there.
#
# Toolchains: Arm GNU 15.2 in ~/toolchains (TOOLCHAINS): arm-none-eabi for
# the Zero, aarch64-none-elf for the Zero 2 W.
#
# Circle's options (CIRCLE_DEFINES):
#
# Heap buckets up to 64 MB: Circle's heap puts a freed block back on the free
# list of its size class (bucket) only if there is one - a block bigger than
# the largest bucket (512 KB by default) is lost for good when freed (Circle's
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
# PGPU_WIRELESS (the Zero W, the Zero 2 W): the board has the wireless chip,
# and its kernel is the one with the Bluetooth code (for speakers, to come:
# for now the Bluetooth switch in Settings). The Zero has no such chip and
# its kernel is built without: that's all its build and the Zero W's differ
# by. Either kernel starts on either board (a Zero W with the Zero's has no
# Bluetooth; the build's line says which it is: fwconfig=...,wireless).
#
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/.." && pwd)
TOOLCHAINS=${TOOLCHAINS:-$HOME/toolchains}
JOBS=$(( $(nproc) > 1 ? $(nproc) - 1 : 1 ))	# a core left free
GENERATOR=$(command -v ninja >/dev/null && echo Ninja || echo "Unix Makefiles")

DEFINES="HEAP_BLOCK_BUCKET_SIZES=0x40,0x400,0x1000,0x4000,0x10000,0x40000,0x80000,0x100000,0x200000,0x400000,0x800000,0x1000000,0x2000000,0x4000000;PGPU_HEAP_BUCKETS;MEM_PERSISTENT_SIZE=0x10000"

# one build number for all the boards of this build (the version:
# VERSION's major.minor.patch and it; devtools/build-info.sh)
export PGPU_BUILD=${PGPU_BUILD:-$("$HERE/next-build.sh")}

if [ ! -f "$ROOT/third_party/circle/CMakeLists.txt" ]; then
	git -C "$ROOT" submodule update --init third_party/circle
fi
# LVGL (Circle's addon/lvgl/lvgl, a submodule of the submodule: the Settings
# app's widgets, gpu/ui), one commit of it
if [ ! -f "$ROOT/third_party/circle/addon/lvgl/lvgl/lvgl.h" ]; then
	git -C "$ROOT/third_party/circle" submodule update --init --depth 1 addon/lvgl/lvgl
fi
if [ ! -f "$ROOT/third_party/circle/boot/start.elf" ]; then
	make -C "$ROOT/third_party/circle/boot" firmware >/dev/null
fi

# build BOARD RASPPI PREFIX DEFINES IMAGE
build ()
{
	local out=$ROOT/build/$1
	if ! grep -qxF "CIRCLE_DEFINES:UNINITIALIZED=$4" "$out/CMakeCache.txt" 2>/dev/null \
	   && ! grep -qxF "CIRCLE_DEFINES:STRING=$4" "$out/CMakeCache.txt" 2>/dev/null; then
		cmake -S "$ROOT/gpu" -B "$out" -G "$GENERATOR" \
			-DCMAKE_TOOLCHAIN_FILE="$ROOT/third_party/circle/cmake/toolchain.cmake" \
			-DCIRCLE_PREFIX="$3" -DCIRCLE_RASPPI="$2" "-DCIRCLE_DEFINES=$4" >/dev/null
	fi
	cmake --build "$out" -j "$JOBS"
	echo "$1: $out/$5"
}

build_zero ()
{
	build zero 1 "$TOOLCHAINS/arm-gnu-toolchain-15.2.rel1-x86_64-arm-none-eabi/bin/arm-none-eabi-" \
		"$DEFINES" kernel.img
}

build_zerow ()
{
	build zerow 1 "$TOOLCHAINS/arm-gnu-toolchain-15.2.rel1-x86_64-arm-none-eabi/bin/arm-none-eabi-" \
		"$DEFINES;PGPU_WIRELESS" kernel.img
}

build_zero2 ()
{
	build zero2 3 "$TOOLCHAINS/arm-gnu-toolchain-15.2.rel1-x86_64-aarch64-none-elf/bin/aarch64-none-elf-" \
		"$DEFINES;ARM_ALLOW_MULTI_CORE;PGPU_WIRELESS" kernel8.img
}

case "${1:-zero}" in
zero)	build_zero ;;
zerow)	build_zerow ;;
zero2)	build_zero2 ;;
all)	build_zero; build_zerow; build_zero2 ;;
*)	echo "usage: $0 [zero|zerow|zero2|all]" >&2; exit 1 ;;
esac
