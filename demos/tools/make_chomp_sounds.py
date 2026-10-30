#!/usr/bin/env python3
"""
make_chomp_sounds.py - chomp's sounds (demos/chomp), made here from nothing
(8 bits, mono, 22050 Hz) in the manner of the early-80s wave-table chips:
short sweeps of a buzzy wave. The eating sound in two halves (a dot plays
the next), the siren in five loops, each higher (the maze emptying), the
frightened ghosts' loop and the eyes' going home, a life lost, a ghost
eaten, a fruit eaten, a life won, and a jingle of its own for a game's
start (not the arcade's tune). Written to demos/chomp/sounds.c and sounds.h:

    demos/tools/make_chomp_sounds.py [--wav DIR]	(--wav: each sound as a .wav too, to listen to)

The loops are whole cycles, to repeat without a click.
"""
import os, sys, wave
import numpy as np

HERE = os.path.dirname (os.path.abspath (__file__))
OUT = os.path.join (HERE, '..', 'chomp')
R = 22050

def wave_of (ph):
	"""a wave-table-ish tone: a sine with a little of its 2nd and 3rd harmonics, 4-bit steps"""
	x = np.sin (ph) + 0.35 * np.sin (2 * ph) + 0.2 * np.sin (3 * ph)
	return np.round (x * 7) / 7

def sweep (f, seconds=None):
	"""a tone whose pitch follows f (an array of Hz, one a sample)"""
	return wave_of (2 * np.pi * np.cumsum (f) / R)

def tt (seconds):
	return np.arange (int (seconds * R)) / R

def loop_cycle (f_of_t, period, cycles=1):
	"""a whole number of periods of a pitch curve, its phase ending where it began"""
	n = int (round (period * cycles * R))
	t = np.arange (n) / R
	f = f_of_t (t)
	ph = 2 * np.pi * np.cumsum (f) / R
	ph *= round (ph[-1] / (2 * np.pi)) * 2 * np.pi / ph[-1]
	return wave_of (ph)

def waka (up):
	t = tt (0.085)
	f = np.interp (t, [0, t[-1]], [250, 520] if up else [520, 250])
	return sweep (f) * np.minimum (1, (t[-1] - t) / 0.01)

def siren (level):
	lo, hi = 380 + 70 * level, 560 + 110 * level
	period = 0.42 - 0.04 * level
	return loop_cycle (lambda t: lo + (hi - lo) * (0.5 - 0.5 * np.cos (2 * np.pi * t / period)), period, 2)

def fright ():
	period = 0.14
	return loop_cycle (lambda t: 180 + 260 * ((t % period) / period), period, 4)

def eyes ():
	period = 0.11
	return loop_cycle (lambda t: 1500 - 900 * ((t % period) / period), period, 4)

def death ():
	parts = []
	for k in range(9):			# down-turning warbles, lower each time
		t = tt (0.12)
		f = np.interp (t, [0, 0.06, 0.12], [900 - 70 * k, 1050 - 70 * k, 500 - 40 * k])
		parts.append (sweep (f) * 0.9)
	for k in range(2):			# two last pops
		t = tt (0.09)
		parts.append (sweep (np.interp (t, [0, t[-1]], [180, 900])) * np.exp (-t / 0.05))
		parts.append (np.zeros (int (0.05 * R)))
	return np.concatenate (parts)

def ghost ():
	t = tt (0.5)
	return sweep (np.interp (t, [0, t[-1]], [200, 1800])) * np.minimum (1, (t[-1] - t) / 0.05)

def fruit ():
	t = tt (0.33)
	f = np.where ((t * 12).astype (int) % 2 == 0, 880, 1320)
	return sweep (f) * np.exp (-t / 0.25)

def extra ():
	parts = []
	for k in range(3):
		t = tt (0.12)
		parts.append (sweep (np.full (len (t), 1568.0)) * np.exp (-t / 0.08))
		parts.append (np.zeros (int (0.05 * R)))
	return np.concatenate (parts)

def start ():
	# a jingle of its own: up a broken chord, a bounce, a held note (bass under it)
	notes = [(523, 0.14), (659, 0.14), (784, 0.14), (1047, 0.28), (880, 0.14), (988, 0.14), (1047, 0.14),
		 (784, 0.14), (659, 0.14), (784, 0.14), (880, 0.28), (1047, 0.56)]
	out = []
	for f, s in notes:
		t = tt (s)
		out.append (sweep (np.full (len (t), float (f))) * (0.8 * np.minimum (1, (t[-1] - t) / 0.02)))
	melody = np.concatenate (out)
	t = np.arange (len (melody)) / R
	bass = np.where (t < 1.0, 131, np.where (t < 1.84, 175, 131)).astype (float)
	return melody + 0.4 * sweep (bass)

SOUNDS = [('WAKA1', lambda: waka (False), 0.7), ('WAKA2', lambda: waka (True), 0.7)]
SOUNDS += [('SIREN%d' % (k + 1), (lambda k=k: siren (k)), 0.45) for k in range (5)]
SOUNDS += [('FRIGHT', fright, 0.5), ('EYES', eyes, 0.45), ('DEATH', death, 0.8), ('GHOST', ghost, 0.75),
	   ('FRUIT', fruit, 0.75), ('EXTRA', extra, 0.7), ('START', start, 0.7)]
LOOPS = {'SIREN1', 'SIREN2', 'SIREN3', 'SIREN4', 'SIREN5', 'FRIGHT', 'EYES'}

def fade (x, ms=4):
	n = min (len (x) // 2, int (ms * R / 1000))
	x = x.copy ()
	x[-n:] *= np.linspace (1, 0, n)
	return x

def main ():
	wav_dir = sys.argv[sys.argv.index ('--wav') + 1] if '--wav' in sys.argv else None
	c = ['/* sounds.c - chomp\'s sounds: written by demos/tools/make_chomp_sounds.py */', '#include "sounds.h"', '']
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
		table.append ('\t[CSND_%s] = {%s, %u, %u},' % (name, name.lower (), len (u8), R))
		if wav_dir:
			os.makedirs (wav_dir, exist_ok=True)
			w = wave.open (os.path.join (wav_dir, name.lower () + '.wav'), 'wb')
			w.setnchannels (1); w.setsampwidth (1); w.setframerate (R); w.writeframes (u8.tobytes ()); w.close ()
	c += ['const chomp_sound_t chomp_sounds[CSND_COUNT] =', '{'] + table + ['};']
	open (os.path.join (OUT, 'sounds.c'), 'w').write ('\n'.join (c) + '\n')
	h = ['/* sounds.h - chomp\'s sounds: written by demos/tools/make_chomp_sounds.py */',
	     '#ifndef CHOMP_SOUNDS_H', '#define CHOMP_SOUNDS_H', '', '#include <stdint.h>', '',
	     '/* (the values are the sounds\' ids on the RPi) */', 'enum', '{', '\tCSND_NONE,']
	h += ['\tCSND_%s,' % name for name, make, peak in SOUNDS]
	h += ['\tCSND_COUNT', '};', '',
	     'typedef struct', '{', '\tconst uint8_t *samples;\t\t/* 8 bits unsigned, mono */', '\tuint32_t frames;',
	     '\tuint16_t rate;', '} chomp_sound_t;', '',
	     'extern const chomp_sound_t chomp_sounds[CSND_COUNT];', '', '#endif']
	open (os.path.join (OUT, 'sounds.h'), 'w').write ('\n'.join (h) + '\n')
	print ('%d sounds, %d bytes' % (len (SOUNDS), total))

main ()
