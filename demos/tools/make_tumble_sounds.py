#!/usr/bin/env python3
"""
make_tumble_sounds.py - tumble's sound effects (demos/tumble.c), made here
from nothing (8 bits, mono, 22050 Hz): what balls and boxes sound like when
they meet each other and the walls. Written to demos/tumble_sounds.c and
tumble_sounds.h, which the demo links in:

    demos/tools/make_tumble_sounds.py [--wav DIR]	(--wav: each sound as a .wav too, to listen to)

The demo plays each higher for a small body and lower for a big one, and
louder the harder they meet. A small speaker has nothing under 150 Hz or so:
what matters is at 200 Hz - 3 kHz.
"""
import os, sys, wave
import numpy as np
from scipy import signal

HERE = os.path.dirname (os.path.abspath (__file__))
OUT = os.path.join (HERE, '..')
R = 22050

def tt (seconds):
	return np.arange (int (seconds * R)) / R

def noise (n, seed):
	return np.random.default_rng (seed).uniform (-1.0, 1.0, n)

def bandpass (x, lo, hi, order=2):
	b, a = signal.butter (order, [lo / (R / 2), hi / (R / 2)], 'band')
	return signal.lfilter (b, a, x)

def resonator (x, hz, q):
	b, a = signal.iirpeak (hz / (R / 2), q)
	return signal.lfilter (b, a, x)

def sweep (t, f0, f1):
	f = f0 * (f1 / f0) ** (t / t[-1])
	return np.sin (2 * np.pi * np.cumsum (f) / R)

def unit (x):
	return x / np.abs (x).max ()

def fade (x, ms_in=1.0, ms_out=8.0):
	n_in, n_out = int (R * ms_in / 1000), int (R * ms_out / 1000)
	x = x.copy ()
	x[:n_in] *= np.linspace (0, 1, n_in)
	x[-n_out:] *= np.linspace (1, 0, n_out)
	return x

def s_clack ():
	# two balls: a marble's click, a short ring over it
	t = tt (0.09)
	hit = noise (len (t), 1) * np.exp (-t / 0.0015)
	ring = sum (g * resonator (hit, hz, 18) for hz, g in ((1750, 1.0), (2900, 0.6), (4300, 0.3)))
	return unit (ring) * np.exp (-t / 0.018) + 0.3 * unit (bandpass (hit, 1500, 6000))

def s_thock ():
	# a box on something: wood, hollow
	t = tt (0.13)
	hit = noise (len (t), 2) * np.exp (-t / 0.003)
	body = sum (g * resonator (hit, hz, 9) for hz, g in ((420, 1.0), (880, 0.7), (1650, 0.35)))
	return unit (body) * np.exp (-t / 0.035) + 0.25 * unit (bandpass (hit, 700, 3500))

def s_thud ():
	# a wall: duller, lower, the wall's own short rumble
	t = tt (0.17)
	hit = noise (len (t), 3) * np.exp (-t / 0.004)
	wall = sum (g * resonator (hit, hz, 6) for hz, g in ((230, 1.0), (470, 0.8), (760, 0.4)))
	return unit (wall) * np.exp (-t / 0.05) + 0.35 * sweep (t, 330, 180) * np.exp (-t / 0.03)

def s_pop ():
	# a new body: a drop of water
	t = tt (0.10)
	return sweep (t, 500, 1500) * np.exp (-t / 0.03) * np.minimum (t / 0.003, 1.0)

def s_boom ():
	# the kick: a burst, and what it throws
	t = tt (0.6)
	burst = unit (bandpass (noise (len (t), 4), 180, 1600)) * np.exp (-t / 0.14)
	return burst + 0.8 * sweep (t, 420, 150) * np.exp (-t / 0.10) + 0.3 * unit (bandpass (noise (len (t), 5), 1500, 5000)) * np.exp (-t / 0.03)

# name, the samples, the peak (of 1.0)
SOUNDS = [
	('CLACK', s_clack, 0.9),
	('THOCK', s_thock, 0.9),
	('THUD', s_thud, 0.9),
	('POP', s_pop, 0.7),
	('BOOM', s_boom, 0.9),
]

def main ():
	wav_dir = sys.argv[sys.argv.index ('--wav') + 1] if '--wav' in sys.argv else None
	c = ['/* tumble_sounds.c - tumble\'s sound effects: written by demos/tools/make_tumble_sounds.py */',
	     '#include "tumble_sounds.h"', '']
	table, total = [], 0
	for name, make, peak in SOUNDS:
		x = np.asarray (make (), dtype=float)
		x = fade (x - x.mean ())
		x *= peak / np.abs (x).max ()
		u8 = np.clip (np.round (x * 127.0) + 128, 0, 255).astype (np.uint8)
		total += len (u8)
		c.append ('static const uint8_t %s[%u] =	/* %.2f s at %u Hz */' % (name.lower (), len (u8), len (u8) / R, R))
		c.append ('{')
		for k in range (0, len (u8), 24):
			c.append ('\t' + ','.join ('%u' % v for v in u8[k:k + 24]) + ',')
		c += ['};', '']
		table.append ('\t[TSND_%s] = {%s, %u, %u},' % (name, name.lower (), len (u8), R))
		if wav_dir:
			os.makedirs (wav_dir, exist_ok=True)
			w = wave.open (os.path.join (wav_dir, name.lower () + '.wav'), 'wb')
			w.setnchannels (1); w.setsampwidth (1); w.setframerate (R); w.writeframes (u8.tobytes ()); w.close ()
	c += ['const tumble_sound_t tumble_sounds[TSND_COUNT] =', '{'] + table + ['};']
	open (os.path.join (OUT, 'tumble_sounds.c'), 'w').write ('\n'.join (c) + '\n')
	h = ['/* tumble_sounds.h - tumble\'s sound effects: written by demos/tools/make_tumble_sounds.py */',
	     '#ifndef TUMBLE_SOUNDS_H', '#define TUMBLE_SOUNDS_H', '', '#include <stdint.h>', '',
	     '/* (the values are the sounds\' ids on the RPi) */', 'enum', '{', '\tTSND_NONE,']
	h += ['\tTSND_%s,' % name for name, make, peak in SOUNDS]
	h += ['\tTSND_COUNT', '};', '',
	     'typedef struct', '{', '\tconst uint8_t *samples;\t\t/* 8 bits unsigned, mono */', '\tuint32_t frames;',
	     '\tuint16_t rate;', '} tumble_sound_t;', '',
	     'extern const tumble_sound_t tumble_sounds[TSND_COUNT];', '', '#endif']
	open (os.path.join (OUT, 'tumble_sounds.h'), 'w').write ('\n'.join (h) + '\n')
	print ('%d sounds, %d bytes' % (len (SOUNDS), total))

main ()
