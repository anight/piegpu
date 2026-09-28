#!/bin/bash
#
# Puts the files the installer page serves into web/installer/firmware/: the
# Raspberry Pi firmware (Circle's boot/: bootcode.bin, start.elf, fixup.dat)
# and the gpu app (gpu/kernel.img, built first), with a manifest the page
# reads. config.txt and cmdline.txt come from the page's settings.
#
#   web/installer/make-firmware.sh
#
set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
OUT=$HERE/firmware

make -C "$ROOT/gpu" -j4 >/dev/null
mkdir -p "$OUT"
cp "$ROOT"/circle/boot/{bootcode.bin,start.elf,fixup.dat} "$OUT/"
cp "$ROOT/gpu/kernel.img" "$OUT/"

python3 - "$OUT" "$(git -C "$ROOT" describe --always --dirty)" <<'EOF'
import json, os, sys, time, zlib
out, version = sys.argv[1], sys.argv[2]
files = []
for name in ['bootcode.bin', 'start.elf', 'fixup.dat', 'kernel.img']:
	data = open(os.path.join(out, name), 'rb').read()
	files.append({'name': name, 'size': len(data), 'crc32': '%08x' % (zlib.crc32(data) & 0xffffffff)})
json.dump({'version': version, 'built': time.strftime('%Y-%m-%d %H:%M'), 'files': files},
	  open(os.path.join(out, 'manifest.json'), 'w'), indent=1)
print('firmware/: pico-gpu %s, %d files' % (version, len(files)))
EOF
