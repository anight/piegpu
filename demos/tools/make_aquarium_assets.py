#!/usr/bin/env python3
"""
make_aquarium_assets.py - what demos/aquarium.c doesn't make itself, made
here from nothing and written to demos/aquarium_assets.c and
aquarium_assets.h:

- the caustics: the net of light that a rippled surface throws on what's
  under it, as a 128 x 128 texture that tiles (the bright lines are where two
  cells of scattered points meet; two sizes of cells, blurred a little). The
  demo scrolls two layers of it over each other, which is what makes it
  shimmer;
- the sounds (8 bits, mono): the air stone's bubbles and the pump's hum, loops
  that go round without a click; a pellet's plop; a knock on the glass.

    demos/tools/make_aquarium_assets.py [--out DIR]	(--out: the texture as a .png and the sounds as .wav too)
"""
import os, struct, sys, wave, zlib
import numpy as np
from scipy import ndimage, signal

HERE = os.path.dirname (os.path.abspath (__file__))
OUT = os.path.join (HERE, '..')
N = 128
R = 22050
RL = 11025			# the loops'

# ---- the caustics ---------------------------------------------------------------------

def cells (n, points, seed):
	"""how far each texel is from its two nearest points (the texture tiles: so do the points)"""
	rng = np.random.default_rng (seed)
	p = rng.uniform (0, 1, (points, 2))
	y, x = np.mgrid[0:n, 0:n] / n
	d = []
	for px, py in p:
		dx = np.abs (x - px); dx = np.minimum (dx, 1 - dx)
		dy = np.abs (y - py); dy = np.minimum (dy, 1 - dy)
		d.append (np.sqrt (dx * dx + dy * dy))
	d = np.sort (np.array (d), axis=0)
	return d[0], d[1]

def caustics ():
	out = np.zeros ((N, N))
	for points, width, gain, seed in ((14, 0.050, 1.0, 3), (40, 0.030, 0.55, 5)):
		f1, f2 = cells (N, points, seed)
		edge = np.clip (1.0 - (f2 - f1) / width, 0.0, 1.0) ** 2		# bright where two cells meet
		out += gain * edge
	out = ndimage.gaussian_filter (out, 0.8, mode='wrap')
	out = 0.10 + 0.90 * np.clip (out / np.percentile (out, 99.5), 0.0, 1.0) ** 1.3
	return np.clip (np.round (out * 255), 0, 255).astype (np.uint8)

# ---- the sounds -----------------------------------------------------------------------

def bandpass (x, lo, hi, rate, order=2):
	b, a = signal.butter (order, [lo / (rate / 2), hi / (rate / 2)], 'band')
	return signal.lfilter (b, a, x)

def resonator (x, hz, q, rate):
	b, a = signal.iirpeak (hz / (rate / 2), q)
	return signal.lfilter (b, a, x)

def periodic_noise (n, lo, hi, rate, seed):
	"""noise that goes round: random phases between lo and hi Hz"""
	rng = np.random.default_rng (seed)
	f = np.fft.rfftfreq (n, 1.0 / rate)
	spec = ((f >= lo) & (f <= hi)) * np.exp (2j * np.pi * rng.uniform (0, 1, len (f)))
	x = np.fft.irfft (spec, n)
	return x / np.abs (x).max ()

def s_bubbles ():
	# an air stone: many small bubbles, each a short rising note, over the water's hiss.
	# Three seconds that go round: a bubble near the end runs on into the start
	n = int (3.0 * RL)
	rng = np.random.default_rng (11)
	out = np.zeros (n)
	for k in range (150):
		at = rng.integers (0, n)
		dur = rng.uniform (0.018, 0.05)
		m = int (dur * RL)
		t = np.arange (m) / RL
		f0 = rng.uniform (450, 1500)
		blip = np.sin (2 * np.pi * (f0 * t + 0.5 * f0 * 9.0 * t * t)) * np.sin (np.pi * t / dur) ** 2 * rng.uniform (0.3, 1.0)
		idx = (at + np.arange (m)) % n
		out[idx] += blip
	return out / np.abs (out).max () + 0.12 * periodic_noise (n, 500, 3500, RL, 12)

def s_hum ():
	# the pump: a mains hum's upper harmonics (a small speaker has nothing lower) and its rattle
	n = RL
	t = np.arange (n) / RL
	hum = sum (g * np.sin (2 * np.pi * hz * t + ph) for hz, g, ph in ((200, 1.0, 0), (300, 0.6, 1), (400, 0.5, 2), (600, 0.25, 3)))
	return hum / np.abs (hum).max () + 0.10 * periodic_noise (n, 300, 1200, RL, 13)

def s_plop ():
	# a pellet through the surface: a drop's falling note
	t = np.arange (int (0.16 * R)) / R
	f = 900 * (380 / 900) ** (t / t[-1])
	drop = np.sin (2 * np.pi * np.cumsum (f) / R) * np.exp (-t / 0.035) * np.minimum (t / 0.002, 1.0)
	return drop + 0.25 * bandpass (np.random.default_rng (14).uniform (-1, 1, len (t)), 1200, 5000, R) * np.exp (-t / 0.012)

def s_knock ():
	# a knuckle on the glass: a short, dull, low ring
	t = np.arange (int (0.22 * R)) / R
	hit = np.random.default_rng (15).uniform (-1, 1, len (t)) * np.exp (-t / 0.002)
	glass = sum (g * resonator (hit, hz, 10, R) for hz, g in ((310, 1.0), (640, 0.6), (1180, 0.3)))
	return glass / np.abs (glass).max () * np.exp (-t / 0.05)

# name, the samples, the rate, a loop?, the peak
SOUNDS = [
	('BUBBLES', s_bubbles, RL, True, 0.85),
	('HUM', s_hum, RL, True, 0.6),
	('PLOP', s_plop, R, False, 0.9),
	('KNOCK', s_knock, R, False, 0.9),
]

def png (path, grey):
	h, w = grey.shape
	raw = b''.join (b'\x00' + grey[y].tobytes () for y in range (h))
	def chunk (kind, data):
		return struct.pack ('>I', len (data)) + kind + data + struct.pack ('>I', zlib.crc32 (kind + data))
	open (path, 'wb').write (b'\x89PNG\r\n\x1a\n' + chunk (b'IHDR', struct.pack ('>IIBBBBB', w, h, 8, 0, 0, 0, 0))
				 + chunk (b'IDAT', zlib.compress (raw)) + chunk (b'IEND', b''))

def array (c, name, data, comment):
	c.append ('static const uint8_t %s[%u] =	/* %s */' % (name, len (data), comment))
	c.append ('{')
	for k in range (0, len (data), 24):
		c.append ('\t' + ','.join ('%u' % v for v in data[k:k + 24]) + ',')
	c += ['};', '']

def main ():
	out_dir = sys.argv[sys.argv.index ('--out') + 1] if '--out' in sys.argv else None
	if out_dir:
		os.makedirs (out_dir, exist_ok=True)
	c = ['/* aquarium_assets.c - the aquarium\'s caustics and sounds: written by demos/tools/make_aquarium_assets.py */',
	     '#include "aquarium_assets.h"', '']
	tex = caustics ()
	if out_dir:
		png (os.path.join (out_dir, 'caustics.png'), np.tile (tex, (2, 2)))
	c.append ('const uint8_t aquarium_caustics[AQUARIUM_CAUSTICS * AQUARIUM_CAUSTICS] =')
	c.append ('{')
	flat = tex.flatten ()
	for k in range (0, len (flat), 32):
		c.append ('\t' + ','.join ('%u' % v for v in flat[k:k + 32]) + ',')
	c += ['};', '']
	table, total = [], 0
	for name, make, rate, loop, peak in SOUNDS:
		x = np.asarray (make (), dtype=float)
		x -= x.mean ()
		if not loop:
			k = int (rate * 0.008)
			x[-k:] *= np.linspace (1, 0, k)
		x *= peak / np.abs (x).max ()
		u8 = np.clip (np.round (x * 127.0) + 128, 0, 255).astype (np.uint8)
		total += len (u8)
		array (c, name.lower (), u8, '%.2f s at %u Hz%s' % (len (u8) / rate, rate, ', a loop' if loop else ''))
		table.append ('\t[ASND_%s] = {%s, %u, %u},' % (name, name.lower (), len (u8), rate))
		if out_dir:
			w = wave.open (os.path.join (out_dir, name.lower () + '.wav'), 'wb')
			w.setnchannels (1); w.setsampwidth (1); w.setframerate (rate); w.writeframes (u8.tobytes ()); w.close ()
	c += ['const aquarium_sound_t aquarium_sounds[ASND_COUNT] =', '{'] + table + ['};']
	open (os.path.join (OUT, 'aquarium_assets.c'), 'w').write ('\n'.join (c) + '\n')
	h = ['/* aquarium_assets.h - the aquarium\'s caustics and sounds: written by demos/tools/make_aquarium_assets.py */',
	     '#ifndef AQUARIUM_ASSETS_H', '#define AQUARIUM_ASSETS_H', '', '#include <stdint.h>', '',
	     '#define AQUARIUM_CAUSTICS\t%u\t\t/* the texture\'s side: 8 bits a texel, it tiles */' % N,
	     'extern const uint8_t aquarium_caustics[AQUARIUM_CAUSTICS * AQUARIUM_CAUSTICS];', '',
	     '/* (the values are the sounds\' ids on the RPi) */', 'enum', '{', '\tASND_NONE,']
	h += ['\tASND_%s,%s' % (name, '\t/* a loop */' if loop else '') for name, make, rate, loop, peak in SOUNDS]
	h += ['\tASND_COUNT', '};', '',
	     'typedef struct', '{', '\tconst uint8_t *samples;\t\t/* 8 bits unsigned, mono */', '\tuint32_t frames;',
	     '\tuint16_t rate;', '} aquarium_sound_t;', '',
	     'extern const aquarium_sound_t aquarium_sounds[ASND_COUNT];', '', '#endif']
	open (os.path.join (OUT, 'aquarium_assets.h'), 'w').write ('\n'.join (h) + '\n')
	print ('the caustics %u bytes, %d sounds %d bytes' % (len (flat), len (SOUNDS), total))

main ()
