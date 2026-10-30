#!/usr/bin/env python3
"""
make_invaders_sounds.py - the invaders' sounds (demos/invaders), made here
from nothing (8 bits, mono, 22050 Hz) after the original's: it had no sound
chip, its sounds were analogue circuits (op-amps, decaying RC networks)
switched on by bits of two output ports, one for each: the march's four
notes, the cannon's shot, an invader hit, the cannon hit, the saucer (a
warble, on while it flies) and the saucer hit, the extra cannon. These are
made the way such circuits sound - tones that drop as they decay, filtered
noise - not copied from anything. Written to demos/invaders/sounds.c and
sounds.h:

    demos/tools/make_invaders_sounds.py [--wav DIR]	(--wav: each sound as a .wav too, to listen to)

The march's notes are low (the original's thump); their harmonics carry
them on a small speaker, which has nothing under 150 Hz or so.
"""
import os, sys, wave
import numpy as np
from scipy import signal

HERE = os.path.dirname (os.path.abspath (__file__))
OUT = os.path.join (HERE, '..', 'invaders')
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

def osc (f, square=0.6):
	"""a tone of frequency f (an array: it may glide), between a sine and a square"""
	ph = 2 * np.pi * np.cumsum (f) / R
	s = np.sin (ph)
	return (1 - square) * s + square * np.tanh (4 * s)

def fleet (hz):
	t = tt (0.16)
	f = hz * (1.0 + 0.35 * np.exp (-t / 0.012))	# (a thump: it starts a little high)
	return lowpass (osc (f, 0.8) * np.exp (-t / 0.05), 1800)

def shot ():
	t = tt (0.32)
	hiss = bandpass (noise (len (t), 1), 900, 5000) * np.exp (-t / 0.09)
	tone = osc (1500 * np.exp (-t * 6.0) + 300, 0.3) * np.exp (-t / 0.06) * 0.5
	return hiss + tone

def invader_hit ():
	t = tt (0.26)
	return osc (1800 * np.exp (-t * 9.0) + 200, 0.5) * np.exp (-t / 0.07)

def player_hit ():
	t = tt (1.3)
	boom = lowpass (noise (len (t), 3), 900) * np.exp (-t / 0.35)
	crackle = bandpass (noise (len (t), 4), 1500, 6000) * np.exp (-t / 0.12) * 0.6
	return boom * 2.5 + crackle

def saucer ():
	"""one turn of the warble (it loops while the saucer flies: a whole number of cycles)"""
	n = int (R * 0.2)
	t = np.arange (n) / R
	lfo = np.sin (2 * np.pi * 5 * t)		# 5 Hz: one cycle in 0.2 s
	f = 560 + 220 * lfo
	ph = 2 * np.pi * np.cumsum (f) / R
	ph *= (round (ph[-1] / (2 * np.pi))) * 2 * np.pi / ph[-1]	# (its pitch ends where it started)
	return np.sin (ph) * 0.8 + 0.2 * np.sign (np.sin (ph))

def saucer_hit ():
	t = tt (1.0)
	f = 900 + 500 * np.sin (2 * np.pi * 9 * t)
	return osc (f, 0.4) * np.exp (-t / 0.45)

def extra ():
	t = tt (1.2)
	x = np.zeros_like (t)
	for k, a in [(1, 1.0), (2.76, 0.5), (5.4, 0.25)]:	# a bell's partials
		x += a * np.sin (2 * np.pi * 1320 * k * t)
	return x * np.exp (-t / 0.35)

SOUNDS = [('FLEET1', lambda: fleet (98), 0.95), ('FLEET2', lambda: fleet (87), 0.95),
	  ('FLEET3', lambda: fleet (78), 0.95), ('FLEET4', lambda: fleet (73), 0.95),
	  ('SHOT', shot, 0.7), ('INVADER_HIT', invader_hit, 0.8), ('PLAYER_HIT', player_hit, 1.0),
	  ('SAUCER', saucer, 0.6), ('SAUCER_HIT', saucer_hit, 0.8), ('EXTRA', extra, 0.7)]

def fade (x, ms=4):
	n = min (len (x) // 2, int (ms * R / 1000))
	x = x.copy ()
	x[-n:] *= np.linspace (1, 0, n)
	return x

def main ():
	wav_dir = sys.argv[sys.argv.index ('--wav') + 1] if '--wav' in sys.argv else None
	c = ['/* sounds.c - the invaders\' sounds: written by demos/tools/make_invaders_sounds.py */',
	     '#include "sounds.h"', '']
	table, total = [], 0
	for name, make, peak in SOUNDS:
		x = np.asarray (make (), dtype=float)
		x = x - x.mean ()
		if name != 'SAUCER':			# (a loop: no fade at its end)
			x = fade (x)
		x *= peak / np.abs (x).max ()
		u8 = np.clip (np.round (x * 127.0) + 128, 0, 255).astype (np.uint8)
		total += len (u8)
		c.append ('static const uint8_t %s[%u] =	/* %.2f s at %u Hz */' % (name.lower (), len (u8), len (u8) / R, R))
		c.append ('{')
		for k in range (0, len (u8), 24):
			c.append ('\t' + ','.join ('%u' % v for v in u8[k:k + 24]) + ',')
		c += ['};', '']
		table.append ('\t[ISND_%s] = {%s, %u, %u},' % (name, name.lower (), len (u8), R))
		if wav_dir:
			os.makedirs (wav_dir, exist_ok=True)
			w = wave.open (os.path.join (wav_dir, name.lower () + '.wav'), 'wb')
			w.setnchannels (1); w.setsampwidth (1); w.setframerate (R); w.writeframes (u8.tobytes ()); w.close ()
	c += ['const invaders_sound_t invaders_sounds[ISND_COUNT] =', '{'] + table + ['};']
	open (os.path.join (OUT, 'sounds.c'), 'w').write ('\n'.join (c) + '\n')
	h = ['/* sounds.h - the invaders\' sounds: written by demos/tools/make_invaders_sounds.py */',
	     '#ifndef INVADERS_SOUNDS_H', '#define INVADERS_SOUNDS_H', '', '#include <stdint.h>', '',
	     '/* (the values are the sounds\' ids on the RPi) */', 'enum', '{', '\tISND_NONE,']
	h += ['\tISND_%s,' % name for name, make, peak in SOUNDS]
	h += ['\tISND_COUNT', '};', '',
	     'typedef struct', '{', '\tconst uint8_t *samples;\t\t/* 8 bits unsigned, mono */', '\tuint32_t frames;',
	     '\tuint16_t rate;', '} invaders_sound_t;', '',
	     'extern const invaders_sound_t invaders_sounds[ISND_COUNT];', '', '#endif']
	open (os.path.join (OUT, 'sounds.h'), 'w').write ('\n'.join (h) + '\n')
	print ('%d sounds, %d bytes' % (len (SOUNDS), total))

main ()
