#!/usr/bin/env python3
"""A Jet scene on the GPU against the original, at the same moment.

The original: JetExamples' own capture tool (its tools/screenshots, built
natively: Jet's software renderer with the scene's ESP32 settings); the
GPU's: the PC host's jet-NAME_host with JET_SHOT (demos/jet/kit.cpp), drawn
by the RPi and read back. Both are 480x320. Written side by side with what
differs (four times stronger) to OUT/NAME-SECONDS.png, and how much they
differ is printed: the mean difference of a channel (of 255) and the share
of pixels more than 32 off in some channel. The original renders half as
wide and doubles its pixels, and its colours are RGB565: a few percent off
along edges is that.

usage: compare.py JETEXAMPLES_BUILD HOST_BUILD OUT NAME[@SECONDS[,SECONDS...]] ...
  JETEXAMPLES_BUILD  where JetExamples' tools/screenshots was built
  HOST_BUILD         hosts/pc/build
  NAME               a scene (template-cube ...); without @: its moment in
                     JetExamples' docs/screenshots/manifest.json
"""
import json, os, subprocess, sys
import numpy as np
from PIL import Image, ImageDraw

if len(sys.argv) < 5:
    sys.exit(__doc__)
ref_build, host_build, out = sys.argv[1:4]
os.makedirs(out, exist_ok=True)
manifest = os.path.join(ref_build, '..', 'docs', 'screenshots', 'manifest.json')
moments = {c['example'][6:]: c['seconds'] for c in json.load(open(manifest))['captures']} if os.path.exists(manifest) else {}
for item in sys.argv[4:]:
    name, _, times = item.partition('@')
    for seconds in ([float(t) for t in times.split(',')] if times else [moments[name]]):
        base = os.path.join(out, '%s-%g' % (name, seconds))
        r = subprocess.run([os.path.join(ref_build, 'esp32-' + name), str(seconds), base + '-ref.ppm'], capture_output=True, text=True)
        if r.returncode:
            print('%s: the original failed: %s' % (name, (r.stdout + r.stderr).strip()[-200:]))
            continue
        g = subprocess.run([os.path.join(host_build, 'jet-%s_host' % name)], capture_output=True, text=True, timeout=600,
                           env=dict(os.environ, JET_SHOT='%g:%s-gpu.ppm' % (seconds, base)))
        if g.returncode:
            print('%s: the GPU failed: %s' % (name, (g.stdout + g.stderr).strip()[-300:]))
            continue
        a = np.asarray(Image.open(base + '-ref.ppm').convert('RGB')).astype(int)
        b = np.asarray(Image.open(base + '-gpu.ppm').convert('RGB')).astype(int)
        d = np.abs(a - b)
        sheet = Image.new('RGB', (3 * 484 - 4, 320 + 16), (0, 0, 0))
        for i, (title, picture) in enumerate((('original', a), ('GPU', b), ('difference x4', np.minimum(d * 4, 255)))):
            sheet.paste(Image.fromarray(picture.astype('uint8')), (i * 484, 16))
            ImageDraw.Draw(sheet).text((i * 484 + 4, 2), '%s %s at %g s' % (name, title, seconds), fill=(255, 255, 0))
        sheet.save(base + '.png')
        os.remove(base + '-ref.ppm')
        os.remove(base + '-gpu.ppm')
        print('%-18s %6g s: mean difference %5.2f, %5.2f%% of the pixels far off   %s' % (
              name, seconds, d.mean(), 100.0 * (d.max(axis=2) > 32).mean(), g.stdout.strip().splitlines()[-1].split(': ', 2)[-1]))
