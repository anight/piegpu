#!/bin/bash
#
# Puts the files the installer page serves into web/installer/firmware/, a
# set per board (the ones piegpu supports for now: the Zero / Zero W and the
# Zero 2 W): the Raspberry Pi firmware (Circle's boot/: bootcode.bin,
# start.elf, fixup.dat) and the gpu app built for it (devtools/build-gpu.sh:
# firmware/zero/kernel.img; firmware/zero2/kernel8.img if Circle is
# configured for the Zero 2 W), with a manifest the page reads. config.txt and
# cmdline.txt come from the page's settings. And the demos the page runs (Test
# OpenGL, Test video: hosts/web, into web/installer/demos/), if Emscripten is
# there (EMSDK, default ~/emsdk), with the test video (web/installer/videos/).
#
#   web/installer/make-firmware.sh
#
set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
OUT=$HERE/firmware
BOOT=$ROOT/circle/boot
JOBS=$(( $(nproc) > 1 ? $(nproc) - 1 : 1 ))	# a core left free

rm -rf "$OUT"
BOARDS=zero
"$ROOT/devtools/build-gpu.sh" zero >/dev/null
mkdir -p "$OUT/zero"
cp "$BOOT/bootcode.bin" "$BOOT/start.elf" "$BOOT/fixup.dat" "$ROOT/gpu/kernel.img" "$OUT/zero/"
if [ -f "$ROOT/circle-zero2/lib/libcircle.a" ]; then
	BOARDS="zero zero2"
	"$ROOT/devtools/build-gpu.sh" zero2 >/dev/null
	mkdir -p "$OUT/zero2"
	cp "$BOOT/bootcode.bin" "$BOOT/start.elf" "$BOOT/fixup.dat" "$ROOT/build/zero2/gpu/kernel8.img" "$OUT/zero2/"
fi

# shellcheck disable=SC2086
python3 - "$OUT" "$(git -C "$ROOT" describe --always --dirty)" $BOARDS <<'PY'
import json, os, sys, time, zlib
out, version, boards = sys.argv[1], sys.argv[2], sys.argv[3:]
kernels = {'zero': 'kernel.img', 'zero2': 'kernel8.img'}
manifest = {'version': version, 'built': time.strftime('%Y-%m-%d %H:%M'), 'boards': {}}
for board in boards:
	files = []
	for name in ['bootcode.bin', 'start.elf', 'fixup.dat', kernels[board]]:
		data = open(os.path.join(out, board, name), 'rb').read()
		files.append({'name': name, 'size': len(data), 'crc32': '%08x' % (zlib.crc32(data) & 0xffffffff)})
	manifest['boards'][board] = {'kernel': kernels[board], 'files': files}
json.dump(manifest, open(os.path.join(out, 'manifest.json'), 'w'), indent=1)
print('firmware/: piegpu %s for %s' % (version, ', '.join(boards)))
PY

# the test video (the Test video button): the Sintel trailer, 854x480 H.264,
# Blender Foundation, CC BY 3.0 (its server doesn't let a page fetch it)
VIDEO=$HERE/videos/sintel_trailer-480p.mp4
if [ ! -f "$VIDEO" ]; then
	mkdir -p "$HERE/videos"
	curl -sSfL -o "$VIDEO.part" https://download.blender.org/durian/trailer/sintel_trailer-480p.mp4 \
		&& mv "$VIDEO.part" "$VIDEO" && echo "videos/: the Sintel trailer" \
		|| echo "videos/: none (the download failed): the page can't test video"
fi

# the page's demos (WebAssembly)
EMSDK_ENV=${EMSDK:-$HOME/emsdk}/emsdk_env.sh
if [ -f "$EMSDK_ENV" ]; then
	# shellcheck disable=SC1090
	source "$EMSDK_ENV" >/dev/null 2>&1
	emcmake cmake -S "$ROOT/hosts/web" -B "$ROOT/hosts/web/build" -DCMAKE_BUILD_TYPE=Release >/dev/null
	make -C "$ROOT/hosts/web/build" -j"$JOBS" >/dev/null
	echo "demos/: gears, video (WebAssembly)"
else
	echo "demos/: none (no Emscripten at $EMSDK_ENV): the page can't run its demos"
fi
