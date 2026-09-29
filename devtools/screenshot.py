#!/usr/bin/env python3
"""Extract a screenshot dumped by the gpu app (PGPU_OP_DEBUG_SCREENSHOT) from
an RPi log file and write it as PNG.

usage: devtools/screenshot.py [log] [out.png] [--scale N] [--index N]
  --index: which screenshot in the log (0 = first, default: the last)
"""
import base64, struct, sys, zlib

args = [a for a in sys.argv[1:] if not a.startswith('--')]
scale, index = 1, -1
if '--scale' in sys.argv:
    scale = int(sys.argv[sys.argv.index('--scale') + 1])
if '--index' in sys.argv:
    index = int(sys.argv[sys.argv.index('--index') + 1])
opts = [sys.argv[i + 1] for i, a in enumerate(sys.argv) if a in ('--scale', '--index')]
args = [a for a in sys.argv[1:] if not a.startswith('--') and a not in opts]
log = args[0] if args else 'devtools/logs/last.log'
out = args[1] if len(args) > 1 else 'devtools/logs/screenshot.png'

lines = open(log, errors='replace').read().splitlines()
start = [i for i, l in enumerate(lines) if l.startswith('#SCREENSHOT')][index]
_, w, h, fmt = lines[start].split()
w, h = int(w), int(h)
end = next(i for i in range(start, len(lines)) if lines[i].startswith('#END'))
data = base64.b64decode(''.join(lines[start + 1:end]))
assert len(data) == w * h * 2, (len(data), w * h * 2)

rows = []
for y in range(h):
    row = bytearray()
    for x in range(w):
        v = data[2 * (y * w + x)] | data[2 * (y * w + x) + 1] << 8
        r, g, b = (v >> 11) & 31, (v >> 5) & 63, v & 31
        px = bytes(((r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)))
        row += px * scale
    for _ in range(scale):
        rows.append(b'\0' + bytes(row))

def chunk(t, d):
    return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d))

png = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w * scale, h * scale, 8, 2, 0, 0, 0))
png += chunk(b'IDAT', zlib.compress(b''.join(rows), 9)) + chunk(b'IEND', b'')
open(out, 'wb').write(png)
print(f'{out}: {w}x{h} (x{scale})')
