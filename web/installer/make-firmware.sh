#!/bin/bash
#
# Puts the files the installer page serves into web/installer/firmware/, a
# set per board (the ones piegpu supports for now: the Zero / Zero W and the
# Zero 2 W): the Raspberry Pi firmware (Circle's boot/: bootcode.bin,
# start.elf, fixup.dat) and the gpu app built for it (devtools/build-gpu.sh:
# firmware/zero/kernel.img, firmware/zero2/kernel8.img), with a manifest the
# page reads. config.txt and
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
BOOT=$ROOT/third_party/circle/boot
JOBS=$(( $(nproc) > 1 ? $(nproc) - 1 : 1 ))	# a core left free

rm -rf "$OUT"
# one build number for both boards' kernels: one version (gpu/Makefile)
export PGPU_BUILD=${PGPU_BUILD:-$("$ROOT/devtools/next-build.sh")}
BOARDS="zero zero2"
"$ROOT/devtools/build-gpu.sh" all >/dev/null	# (build/zero, build/zero2)
mkdir -p "$OUT/zero" "$OUT/zero2"
cp "$BOOT/bootcode.bin" "$BOOT/start.elf" "$BOOT/fixup.dat" "$ROOT/build/zero/kernel.img" "$OUT/zero/"
cp "$BOOT/bootcode.bin" "$BOOT/start.elf" "$BOOT/fixup.dat" "$ROOT/build/zero2/kernel8.img" "$OUT/zero2/"

# shellcheck disable=SC2086
python3 - "$OUT" "$(git -C "$ROOT" describe --always --dirty)" $BOARDS <<'PY'
import json, os, sys, time, zlib
out, version, boards = sys.argv[1], sys.argv[2], sys.argv[3:]
kernels = {'zero': 'kernel.img', 'zero2': 'kernel8.img'}
manifest = {'version': version, 'built': time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime()), 'boards': {}}

# a kernel's build line (gpu/build_info.h: between \x01PGPU-BUILD\x02 and
# \x03), as the RPi's installer reads it from a card (PGI INFO)
def build_info(data):
	start = data.find(b'\x01PGPU-BUILD\x02')
	if start < 0:
		return None
	start += len(b'\x01PGPU-BUILD\x02')
	kv = dict(p.split('=', 1) for p in data[start:data.index(b'\x03', start)].decode().split() if '=' in p)
	return {'version': kv.get('fw', ''), 'git': kv.get('fwgit', ''), 'built': kv.get('fwbuilt', ''),
		'config': kv.get('fwconfig', '')}

for board in boards:
	files = []
	for name in ['bootcode.bin', 'start.elf', 'fixup.dat', kernels[board]]:
		data = open(os.path.join(out, board, name), 'rb').read()
		files.append({'name': name, 'size': len(data), 'crc32': '%08x' % (zlib.crc32(data) & 0xffffffff)})
	kernel = open(os.path.join(out, board, kernels[board]), 'rb').read()
	manifest['boards'][board] = {'kernel': kernels[board], 'build': build_info(kernel), 'files': files}
# the version: the kernels' (their build lines: major.minor.patch.build), the
# commit beside it
builds = [manifest['boards'][b]['build'] for b in boards if manifest['boards'][b]['build']]
if builds:
	manifest['version'] = builds[0]['version']
	manifest['git'] = builds[0]['git']
json.dump(manifest, open(os.path.join(out, 'manifest.json'), 'w'), indent=1)
print('firmware/: piegpu %s (%s) for %s' % (manifest['version'], manifest.get('git', version), ', '.join(boards)))
PY

# the test video (the Test video button): the Big Buck Bunny trailer, 853x480
# H.264, Blender Foundation, CC BY 3.0 (its server doesn't let a page fetch it)
VIDEO=$HERE/videos/bbb_trailer-480p.mov
if [ ! -f "$VIDEO" ]; then
	mkdir -p "$HERE/videos"
	curl -sSfL -o "$VIDEO.part" https://download.blender.org/peach/trailer/trailer_480p.mov \
		&& mv "$VIDEO.part" "$VIDEO" && echo "videos/: the Big Buck Bunny trailer" \
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
