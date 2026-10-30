#!/usr/bin/env python3
"""
make_xonix_sounds.py - Xonix's sound effects (demos/xonix), made here from
nothing (8 bits, mono, 22050 Hz), in the manner of the old games' beeps,
softened: the trail ticking out over the sea, a blip as it leaves the land, a
chime when the sea is claimed (the demo plays it lower for a big claim), the
sea balls' bounces, a life lost, the level's end, the game's, a level's start.
Written to demos/xonix/sounds.c and sounds.h, which the demo links in:

    demos/tools/make_xonix_sounds.py [--wav DIR]	(--wav: each sound as a .wav too, to listen to)

A small speaker has nothing under 150 Hz or so: what matters is at
200 Hz - 3 kHz.
"""
import os, sys, wave
import numpy as np

HERE = os.path.dirname (os.path.abspath (__file__))
OUT = os.path.join (HERE, '..', 'xonix')
R = 22050

def tt (seconds):
	return np.arange (int (seconds * R)) / R

def env (t, attack, decay):
	"""a quick rise, then down exponentially (decay: seconds to 1/e)"""
	return np.minimum (t / attack, 1.0) * np.exp (-t / decay)

def tone (f, t, harmonics=(1.0, 0.0, 0.25)):
	"""a soft square-ish tone: odd harmonics, few"""
	x = np.zeros_like (t)
	for k, a in enumerate (harmonics):
		x += a * np.sin (2 * np.pi * f * (2 * k + 1) * t)
	return x

def note (f, seconds, decay, attack=0.004, harmonics=(1.0, 0.0, 0.25)):
	t = tt (seconds)
	return tone (f, t, harmonics) * env (t, attack, decay)

def seq (notes, gap):
	"""notes (frequency, seconds, decay) one after another, gap seconds apart, overlapping"""
	n = int (gap * R)
	total = n * (len (notes) - 1) + max (int (s * R) for f, s, d in notes)
	x = np.zeros (total)
	for i, (f, s, d) in enumerate (notes):
		y = note (f, s, d)
		x[i * n:i * n + len (y)] += y
	return x

def step ():
	t = tt (0.035)
	return np.sign (np.sin (2 * np.pi * 1450 * t)) * env (t, 0.001, 0.008) * 0.6

def out ():
	t = tt (0.09)
	f = 520 + 900 * t / t[-1]
	return np.sin (2 * np.pi * np.cumsum (f) / R) * env (t, 0.003, 0.035)

def capture ():
	return seq ([(784, 0.5, 0.18), (988, 0.5, 0.18), (1175, 0.6, 0.22), (1568, 0.8, 0.30)], 0.055)

def bounce ():
	t = tt (0.05)
	return np.sin (2 * np.pi * 660 * t) * env (t, 0.002, 0.012)

def die ():
	t = tt (0.9)
	f = 900 * np.exp (-t * 3.2) + 90
	zap = np.sign (np.sin (2 * np.pi * np.cumsum (f) / R)) * env (t, 0.002, 0.30)
	noise = np.random.default_rng (7).uniform (-1, 1, len (t)) * env (t, 0.001, 0.12)
	return 0.7 * zap + 0.5 * noise

def cleared ():
	return seq ([(523, 0.3, 0.12), (659, 0.3, 0.12), (784, 0.3, 0.12), (1047, 0.35, 0.14),
		     (784, 0.25, 0.10), (1047, 0.9, 0.35)], 0.11)

def over ():
	return seq ([(587, 0.5, 0.2), (554, 0.5, 0.2), (523, 0.5, 0.2), (440, 1.4, 0.5)], 0.32)

def start ():
	return seq ([(659, 0.2, 0.06), (988, 0.35, 0.12)], 0.09)

SOUNDS = [('STEP', step, 0.6), ('OUT', out, 0.8), ('CAPTURE', capture, 0.9), ('BOUNCE', bounce, 0.7),
	  ('DIE', die, 1.0), ('CLEARED', cleared, 0.9), ('OVER', over, 0.9), ('START', start, 0.8)]

def fade (x, ms=4):
	n = min (len (x) // 2, int (ms * R / 1000))
	x = x.copy ()
	x[-n:] *= np.linspace (1, 0, n)
	return x

def main ():
	wav_dir = sys.argv[sys.argv.index ('--wav') + 1] if '--wav' in sys.argv else None
	c = ['/* sounds.c - Xonix\'s sound effects: written by demos/tools/make_xonix_sounds.py */',
	     '#include "sounds.h"', '']
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
		table.append ('\t[XSND_%s] = {%s, %u, %u},' % (name, name.lower (), len (u8), R))
		if wav_dir:
			os.makedirs (wav_dir, exist_ok=True)
			w = wave.open (os.path.join (wav_dir, name.lower () + '.wav'), 'wb')
			w.setnchannels (1); w.setsampwidth (1); w.setframerate (R); w.writeframes (u8.tobytes ()); w.close ()
	c += ['const xonix_sound_t xonix_sounds[XSND_COUNT] =', '{'] + table + ['};']
	open (os.path.join (OUT, 'sounds.c'), 'w').write ('\n'.join (c) + '\n')
	h = ['/* sounds.h - Xonix\'s sound effects: written by demos/tools/make_xonix_sounds.py */',
	     '#ifndef XONIX_SOUNDS_H', '#define XONIX_SOUNDS_H', '', '#include <stdint.h>', '',
	     '/* (the values are the sounds\' ids on the RPi) */', 'enum', '{', '\tXSND_NONE,']
	h += ['\tXSND_%s,' % name for name, make, peak in SOUNDS]
	h += ['\tXSND_COUNT', '};', '',
	     'typedef struct', '{', '\tconst uint8_t *samples;\t\t/* 8 bits unsigned, mono */', '\tuint32_t frames;',
	     '\tuint16_t rate;', '} xonix_sound_t;', '',
	     'extern const xonix_sound_t xonix_sounds[XSND_COUNT];', '', '#endif']
	open (os.path.join (OUT, 'sounds.h'), 'w').write ('\n'.join (h) + '\n')
	print ('%d sounds, %d bytes' % (len (SOUNDS), total))

main ()
