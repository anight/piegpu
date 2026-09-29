#!/bin/bash
#
# Build the gpu app for a board (Circle configured by devtools/configure-
# circle.sh first):
#
#   devtools/build-gpu.sh [zero|zero2|all]
#
#   zero    Pi Zero / Zero W, 32 bit: gpu/kernel.img (in place, as make -C gpu)
#   zero2   Pi Zero 2 W, 64 bit: build/zero2/gpu/kernel8.img
#
# (The boards pigpu supports for now; more RPi boards are to come.)
#
# Circle's builds put the objects next to the sources, so the Zero 2 W's is
# made in a copy of them (build/zero2: gpu, drivers, devtools, protocol,
# synced before each build; its objects stay for the next one).
#
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/.." && pwd)
JOBS=$(nproc)

build_zero ()
{
	make -C "$ROOT/gpu" -j"$JOBS"
	echo "zero: $ROOT/gpu/kernel.img"
}

build_zero2 ()
{
	local out=$ROOT/build/zero2
	if [ ! -f "$ROOT/circle-zero2/lib/libcircle.a" ]; then
		echo "zero2: no Circle for it: devtools/configure-circle.sh zero2" >&2
		exit 1
	fi
	mkdir -p "$out"
	for dir in gpu drivers devtools protocol; do
		rsync -a --delete --exclude '*.o' --exclude '*.d' --exclude '*.a' --exclude '*.elf' \
			--exclude '*.img' --exclude '*.lst' --exclude '*.map' --exclude logs/ \
			--exclude usbboot/ "$ROOT/$dir/" "$out/$dir/"
	done
	make -C "$out/gpu" -j"$JOBS" CIRCLEHOME="$ROOT/circle-zero2"
	echo "zero2: $out/gpu/kernel8.img"
}

case "${1:-zero}" in
zero)	build_zero ;;
zero2)	build_zero2 ;;
all)	build_zero; build_zero2 ;;
*)	echo "usage: $0 [zero|zero2|all]" >&2; exit 1 ;;
esac
