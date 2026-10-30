#!/usr/bin/env python3
"""
make_rockfall_sounds.py - rockfall's sounds (demos/rockfall), made here from
nothing (8 bits, mono, 22050 Hz) in the manner of the C64's SID: noise and
pulse waves through quick envelopes. A boulder landing (a thud of noise), a
diamond landing (a high ping), a diamond taken, an explosion, the exit
coming out (a crack), the last seconds' tick, a life won, the time counted
into the score, a step in the dirt; and three loops: the uncovering's
rustle, the amoeba's bubbling, the magic wall's tinkling. No theme tune (the
original's is someone's music). Written to demos/rockfall/sounds.c and
sounds.h:

    demos/tools/make_rockfall_sounds.py [--wav DIR]	(--wav: each sound as a .wav too, to listen to)
"""
import os, sys, wave
import numpy as np
from scipy import signal

HERE = os.path.dirname (os.path.abspath (__file__))
OUT = os.path.join (HERE, '..', 'rockfall')
R = 22050

def tt (seconds):
	return np.arange (int (seconds * R)) / R

def noise (n, seed):
	return np.random.default_rng (seed).uniform (-1.0, 1.0, n)

def lowpass (x, hz, order=2):
	b, a = signal.butter (order, hz / (R / 2))
	return signal.lfilter (b, a, x)

def bandpass (x, lo, hi, order=2):
	b, a = signal.butter (order, [lo / (R / 2), hi / (R / 2)], 'band')
	return signal.lfilter (b, a, x)

def pulse (f, duty=0.5):
	ph = np.cumsum (f) / R % 1.0
	return np.where (ph < duty, 1.0, -1.0)

def env (t, attack, decay):
	return np.minimum (t / attack, 1.0) * np.exp (-t / decay)

def boulder ():
	t = tt (0.18)
	return lowpass (noise (len (t), 1), 700) * env (t, 0.002, 0.05) * 3.0

def diamond_fall ():
	t = tt (0.2)
	return pulse (np.full (len (t), 2093.0), 0.3) * env (t, 0.001, 0.05)

def collect ():
	t = tt (0.22)
	f = np.where (t < 0.05, 1568.0, 2093.0)
	return pulse (f, 0.25) * env (t, 0.002, 0.08)

def explode ():
	t = tt (0.9)
	return lowpass (noise (len (t), 3), 2500) * env (t, 0.003, 0.25) * 2.0

def open_exit ():
	t = tt (0.5)
	crack = bandpass (noise (len (t), 5), 1500, 7000) * env (t, 0.001, 0.04) * 2
	sweep = pulse (np.interp (t, [0, t[-1]], [300, 1200]), 0.5) * env (t, 0.01, 0.2) * 0.4
	return crack + sweep

def tick ():
	t = tt (0.12)
	return pulse (np.full (len (t), 1760.0), 0.5) * env (t, 0.001, 0.03)

def extra ():
	out = []
	for f in (1047, 1319, 1568, 2093):
		t = tt (0.09)
		out.append (pulse (np.full (len (t), float (f)), 0.4) * env (t, 0.002, 0.06))
	return np.concatenate (out)

def bonus ():
	t = tt (0.03)
	return pulse (np.full (len (t), 1320.0), 0.5) * env (t, 0.001, 0.012)

def step ():
	t = tt (0.05)
	return bandpass (noise (len (t), 6), 300, 2500) * env (t, 0.001, 0.012) * 1.2

def loop_of (x, seconds):
	n = int (seconds * R)
	return x[:n]

def uncover ():
	t = tt (0.5)
	return bandpass (noise (len (t), 7), 2000, 8000) * (0.6 + 0.4 * np.sin (2 * np.pi * 8 * t))

def amoeba ():
	t = tt (0.48)
	f = 110 + 40 * np.sin (2 * np.pi * (25 / 12) * t * 12 / 12)
	x = pulse (f, 0.3) * (0.5 + 0.5 * np.sin (2 * np.pi * 6.25 * t) ** 2)
	return lowpass (x, 900)

def magic ():
	t = tt (0.4)
	x = np.zeros_like (t)
	for k, f in enumerate ((2637, 3136, 2349, 2794)):
		seg = (t >= k * 0.1) & (t < k * 0.1 + 0.1)
		x[seg] += np.sin (2 * np.pi * f * t[seg]) * np.exp (-(t[seg] - k * 0.1) / 0.03)
	return x

SOUNDS = [('BOULDER', boulder, 0.8), ('DIAMOND', diamond_fall, 0.5), ('COLLECT', collect, 0.6),
	  ('EXPLODE', explode, 0.9), ('OPEN', open_exit, 0.8), ('TICK', tick, 0.6), ('EXTRA', extra, 0.6),
	  ('BONUS', bonus, 0.4), ('STEP', step, 0.4), ('UNCOVER', uncover, 0.35), ('AMOEBA', amoeba, 0.35),
	  ('MAGIC', magic, 0.35)]
LOOPS = {'UNCOVER', 'AMOEBA', 'MAGIC'}

def fade (x, ms=4):
	n = min (len (x) // 2, int (ms * R / 1000))
	x = x.copy ()
	x[-n:] *= np.linspace (1, 0, n)
	return x

def main ():
	wav_dir = sys.argv[sys.argv.index ('--wav') + 1] if '--wav' in sys.argv else None
	c = ['/* sounds.c - rockfall\'s sounds: written by demos/tools/make_rockfall_sounds.py */', '#include "sounds.h"', '']
	table, total = [], 0
	for name, make, peak in SOUNDS:
		x = np.asarray (make (), dtype=float)
		x = x - x.mean ()
		if name not in LOOPS:
			x = fade (x)
		x *= peak / np.abs (x).max ()
		u8 = np.clip (np.round (x * 127.0) + 128, 0, 255).astype (np.uint8)
		total += len (u8)
		c.append ('static const uint8_t %s[%u] =	/* %.2f s at %u Hz */' % (name.lower (), len (u8), len (u8) / R, R))
		c.append ('{')
		for k in range (0, len (u8), 24):
			c.append ('\t' + ','.join ('%u' % v for v in u8[k:k + 24]) + ',')
		c += ['};', '']
		table.append ('\t[RSND_%s] = {%s, %u, %u},' % (name, name.lower (), len (u8), R))
		if wav_dir:
			os.makedirs (wav_dir, exist_ok=True)
			w = wave.open (os.path.join (wav_dir, name.lower () + '.wav'), 'wb')
			w.setnchannels (1); w.setsampwidth (1); w.setframerate (R); w.writeframes (u8.tobytes ()); w.close ()
	c += ['const rockfall_sound_t rockfall_sounds[RSND_COUNT] =', '{'] + table + ['};']
	open (os.path.join (OUT, 'sounds.c'), 'w').write ('\n'.join (c) + '\n')
	h = ['/* sounds.h - rockfall\'s sounds: written by demos/tools/make_rockfall_sounds.py */',
	     '#ifndef ROCKFALL_SOUNDS_H', '#define ROCKFALL_SOUNDS_H', '', '#include <stdint.h>', '',
	     '/* (the values are the sounds\' ids on the RPi) */', 'enum', '{', '\tRSND_NONE,']
	h += ['\tRSND_%s,' % name for name, make, peak in SOUNDS]
	h += ['\tRSND_COUNT', '};', '',
	     'typedef struct', '{', '\tconst uint8_t *samples;\t\t/* 8 bits unsigned, mono */', '\tuint32_t frames;',
	     '\tuint16_t rate;', '} rockfall_sound_t;', '',
	     'extern const rockfall_sound_t rockfall_sounds[RSND_COUNT];', '', '#endif']
	open (os.path.join (OUT, 'sounds.h'), 'w').write ('\n'.join (h) + '\n')
	print ('%d sounds, %d bytes' % (len (SOUNDS), total))

main ()
