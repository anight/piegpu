#!/usr/bin/env python3
"""
make_stacker_sounds.py - stacker's sounds (demos/stacker), made here from
nothing (8 bits, mono) in the manner of the NES's chip: pulse waves and
a triangle. A step across, a turn, a lock, lines cleared, four cleared, a
level up, the game's end; and the music: Korobeiniki, the Russian folk song
(1861, the public domain's) the NES's first tune was, arranged here - the
melody on a pulse wave, a bass on a triangle - looped while playing.
Written to demos/stacker/sounds.c and sounds.h:

    demos/tools/make_stacker_sounds.py [--wav DIR]	(--wav: each sound as a .wav too, to listen to)
"""
import os, sys, wave
import numpy as np

HERE = os.path.dirname (os.path.abspath (__file__))
OUT = os.path.join (HERE, '..', 'stacker')
R = 22050

def tt (seconds, rate=R):
	return np.arange (int (seconds * rate)) / rate

def pulse (f, duty, rate=R):
	ph = np.cumsum (f) / rate % 1.0
	return np.where (ph < duty, 1.0, -1.0)

def triangle (f, rate=R):
	ph = np.cumsum (f) / rate % 1.0
	return 4 * np.abs (ph - 0.5) - 1

def env (t, attack, decay):
	return np.minimum (t / attack, 1.0) * np.exp (-t / decay)

def move ():
	t = tt (0.04)
	return pulse (np.full (len (t), 1200.0), 0.25) * env (t, 0.001, 0.012)

def rotate ():
	t = tt (0.06)
	return pulse (np.interp (t, [0, t[-1]], [700, 1400]), 0.5) * env (t, 0.001, 0.02)

def lock ():
	t = tt (0.12)
	return triangle (np.interp (t, [0, t[-1]], [220, 90])) * env (t, 0.001, 0.05)

def lines ():
	t = tt (0.35)
	return pulse (np.interp (t, [0, t[-1]], [400, 1600]), 0.5) * env (t, 0.002, 0.15)

def four ():
	out = []
	for f in (523, 659, 784, 1047, 1319, 1568):
		t = tt (0.07)
		out.append (pulse (np.full (len (t), float (f)), 0.25) * env (t, 0.002, 0.08))
	t = tt (0.5)
	out.append (pulse (np.full (len (t), 2093.0), 0.25) * env (t, 0.002, 0.2))
	return np.concatenate (out)

def level ():
	out = []
	for f in (784, 988, 1175, 1568):
		t = tt (0.1)
		out.append (pulse (np.full (len (t), float (f)), 0.5) * env (t, 0.002, 0.08))
	return np.concatenate (out)

def over ():
	out = []
	for f in (523, 494, 466, 440, 415, 392):
		t = tt (0.18)
		out.append (pulse (np.full (len (t), float (f)), 0.5) * env (t, 0.005, 0.15))
	return np.concatenate (out)

NOTE = {'A2': 110.0, 'B2': 123.47, 'C3': 130.81, 'D3': 146.83, 'E3': 164.81, 'F3': 174.61, 'G3': 196.0,
	'G#3': 207.65, 'A3': 220.0, 'E2': 82.41, 'D2': 73.42, 'C2': 65.41, 'G2': 98.0, 'G#2': 103.83,
	'A4': 440.0, 'B4': 493.88, 'C5': 523.25, 'D5': 587.33, 'E5': 659.26, 'F5': 698.46, 'G5': 783.99,
	'A5': 880.0, 'R': 0.0}

def music ():
	"""Korobeiniki: 8 bars of 4/4 at 150 a minute, an eighth the unit"""
	rate = 11025
	melody = [('E5', 2), ('B4', 1), ('C5', 1), ('D5', 2), ('C5', 1), ('B4', 1),
		  ('A4', 2), ('A4', 1), ('C5', 1), ('E5', 2), ('D5', 1), ('C5', 1),
		  ('B4', 3), ('C5', 1), ('D5', 2), ('E5', 2),
		  ('C5', 2), ('A4', 2), ('A4', 4),
		  ('D5', 3), ('F5', 1), ('A5', 2), ('G5', 1), ('F5', 1),
		  ('E5', 3), ('C5', 1), ('E5', 2), ('D5', 1), ('C5', 1),
		  ('B4', 2), ('B4', 1), ('C5', 1), ('D5', 2), ('E5', 2),
		  ('C5', 2), ('A4', 2), ('A4', 2), ('R', 2)]
	bars = [('E2', 'B2'), ('A2', 'E3'), ('G#2', 'E3'), ('A2', 'E3'), ('D2', 'A2'), ('C2', 'G2'), ('E2', 'B2'), ('A2', 'E3')]
	eighth = 60.0 / 150 / 2
	lead = []
	for name, n in melody:
		t = tt (eighth * n, rate)
		f = NOTE[name]
		tone = pulse (np.full (len (t), f), 0.25, rate) if f else np.zeros (len (t))
		lead.append (tone * np.minimum (1, (t[-1] - t) / 0.02) * (0.85 - 0.25 * np.minimum (t / (eighth * 4), 1)))
	lead = np.concatenate (lead)
	bass = []
	for root, fifth in bars:
		for k in range (8):
			t = tt (eighth, rate)
			f = NOTE[root if k % 2 == 0 else fifth]
			bass.append (triangle (np.full (len (t), f), rate) * np.minimum (1, (t[-1] - t) / 0.01))
	bass = np.concatenate (bass)
	n = min (len (lead), len (bass))
	return lead[:n] * 0.6 + bass[:n] * 0.5, rate

SOUNDS = [('MOVE', move, 0.5), ('ROTATE', rotate, 0.5), ('LOCK', lock, 0.7), ('LINES', lines, 0.7),
	  ('FOUR', four, 0.7), ('LEVEL', level, 0.6), ('OVER', over, 0.7), ('MUSIC', music, 0.6)]
LOOPS = {'MUSIC'}

def fade (x, ms=4, rate=R):
	n = min (len (x) // 2, int (ms * rate / 1000))
	x = x.copy ()
	x[-n:] *= np.linspace (1, 0, n)
	return x

def main ():
	wav_dir = sys.argv[sys.argv.index ('--wav') + 1] if '--wav' in sys.argv else None
	c = ['/* sounds.c - stacker\'s sounds: written by demos/tools/make_stacker_sounds.py */', '#include "sounds.h"', '']
	table, total = [], 0
	for name, make, peak in SOUNDS:
		made = make ()
		x, rate = made if isinstance (made, tuple) else (made, R)
		x = np.asarray (x, dtype=float)
		x = x - x.mean ()
		if name not in LOOPS:
			x = fade (x, rate=rate)
		x *= peak / np.abs (x).max ()
		u8 = np.clip (np.round (x * 127.0) + 128, 0, 255).astype (np.uint8)
		total += len (u8)
		c.append ('static const uint8_t %s[%u] =	/* %.2f s at %u Hz */' % (name.lower (), len (u8), len (u8) / rate, rate))
		c.append ('{')
		for k in range (0, len (u8), 24):
			c.append ('\t' + ','.join ('%u' % v for v in u8[k:k + 24]) + ',')
		c += ['};', '']
		table.append ('\t[SSND_%s] = {%s, %u, %u},' % (name, name.lower (), len (u8), rate))
		if wav_dir:
			os.makedirs (wav_dir, exist_ok=True)
			w = wave.open (os.path.join (wav_dir, name.lower () + '.wav'), 'wb')
			w.setnchannels (1); w.setsampwidth (1); w.setframerate (rate); w.writeframes (u8.tobytes ()); w.close ()
	c += ['const stacker_sound_t stacker_sounds[SSND_COUNT] =', '{'] + table + ['};']
	open (os.path.join (OUT, 'sounds.c'), 'w').write ('\n'.join (c) + '\n')
	h = ['/* sounds.h - stacker\'s sounds: written by demos/tools/make_stacker_sounds.py */',
	     '#ifndef STACKER_SOUNDS_H', '#define STACKER_SOUNDS_H', '', '#include <stdint.h>', '',
	     '/* (the values are the sounds\' ids on the RPi) */', 'enum', '{', '\tSSND_NONE,']
	h += ['\tSSND_%s,' % name for name, make, peak in SOUNDS]
	h += ['\tSSND_COUNT', '};', '',
	     'typedef struct', '{', '\tconst uint8_t *samples;\t\t/* 8 bits unsigned, mono */', '\tuint32_t frames;',
	     '\tuint16_t rate;', '} stacker_sound_t;', '',
	     'extern const stacker_sound_t stacker_sounds[SSND_COUNT];', '', '#endif']
	open (os.path.join (OUT, 'sounds.h'), 'w').write ('\n'.join (h) + '\n')
	print ('%d sounds, %d bytes' % (len (SOUNDS), total))

main ()
