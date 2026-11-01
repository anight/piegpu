#!/usr/bin/env python3
"""
make_pong_sounds.py - toy-pong's sounds (demos/pong.c), made here from nothing
(8 bits, mono): square waves, as an old bat-and-ball game's. A hit, the wall,
a point, a player joining, a game starting, a game won. Written to
demos/pong_sounds.c and pong_sounds.h:

    demos/tools/make_pong_sounds.py [--wav DIR]	(--wav: each sound as a .wav too, to listen to)
"""
import os, sys, wave
import numpy as np

HERE = os.path.dirname (os.path.abspath (__file__))
OUT = os.path.join (HERE, '..')
R = 22050

def tt (seconds):
	return np.arange (int (seconds * R)) / R

def square (f):
	ph = np.cumsum (f) / R % 1.0
	return np.where (ph < 0.5, 1.0, -1.0)

def note (f, seconds, decay):
	t = tt (seconds)
	return square (np.full (len (t), float (f))) * np.minimum (t / 0.001, 1.0) * np.exp (-t / decay)

def notes (freqs, seconds, decay):
	return np.concatenate ([note (f, seconds, decay) for f in freqs])

def hit ():
	return note (480, 0.06, 0.04)

def wall ():
	return note (240, 0.04, 0.025)

def point ():
	t = tt (0.35)
	return square (np.interp (t, [0, t[-1]], [330, 110])) * np.exp (-t / 0.2)

def join ():
	return notes ((480, 720), 0.07, 0.06)

def start ():
	return notes ((360, 480, 720), 0.09, 0.08)

def win ():
	return np.concatenate ([notes ((480, 480, 480), 0.1, 0.07), note (720, 0.45, 0.2)])

SOUNDS = [('HIT', hit, 0.8), ('WALL', wall, 0.6), ('POINT', point, 0.8), ('JOIN', join, 0.7), ('START', start, 0.7),
	  ('WIN', win, 0.8)]

def fade (x, ms=4):
	n = min (len (x) // 2, int (ms * R / 1000))
	x = x.copy ()
	x[-n:] *= np.linspace (1, 0, n)
	return x

def main ():
	wav_dir = sys.argv[sys.argv.index ('--wav') + 1] if '--wav' in sys.argv else None
	c = ['/* pong_sounds.c - toy-pong\'s sounds: written by demos/tools/make_pong_sounds.py */', '#include "pong_sounds.h"', '']
	table, total = [], 0
	for name, make, peak in SOUNDS:
		x = fade (np.asarray (make (), dtype=float))
		x = x - x.mean ()
		x *= peak / np.abs (x).max ()
		u8 = np.clip (np.round (x * 127.0) + 128, 0, 255).astype (np.uint8)
		total += len (u8)
		c.append ('static const uint8_t %s[%u] =	/* %.2f s at %u Hz */' % (name.lower (), len (u8), len (u8) / R, R))
		c.append ('{')
		for k in range (0, len (u8), 24):
			c.append ('\t' + ','.join ('%u' % v for v in u8[k:k + 24]) + ',')
		c += ['};', '']
		table.append ('\t[PSND_%s] = {%s, %u, %u},' % (name, name.lower (), len (u8), R))
		if wav_dir:
			os.makedirs (wav_dir, exist_ok=True)
			w = wave.open (os.path.join (wav_dir, name.lower () + '.wav'), 'wb')
			w.setnchannels (1); w.setsampwidth (1); w.setframerate (R); w.writeframes (u8.tobytes ()); w.close ()
	c += ['const pong_sound_t pong_sounds[PSND_COUNT] =', '{'] + table + ['};']
	open (os.path.join (OUT, 'pong_sounds.c'), 'w').write ('\n'.join (c) + '\n')
	h = ['/* pong_sounds.h - toy-pong\'s sounds: written by demos/tools/make_pong_sounds.py */',
	     '#ifndef PONG_SOUNDS_H', '#define PONG_SOUNDS_H', '', '#include <stdint.h>', '',
	     '/* (the values are the sounds\' ids on the RPi) */', 'enum', '{', '\tPSND_NONE,']
	h += ['\tPSND_%s,' % name for name, make, peak in SOUNDS]
	h += ['\tPSND_COUNT', '};', '',
	     'typedef struct', '{', '\tconst uint8_t *samples;\t\t/* 8 bits unsigned, mono */', '\tuint32_t frames;',
	     '\tuint16_t rate;', '} pong_sound_t;', '',
	     'extern const pong_sound_t pong_sounds[PSND_COUNT];', '', '#endif']
	open (os.path.join (OUT, 'pong_sounds.h'), 'w').write ('\n'.join (h) + '\n')
	print ('%d sounds, %d bytes' % (len (SOUNDS), total))

main ()
