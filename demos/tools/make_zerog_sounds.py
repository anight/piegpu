#!/usr/bin/env python3
"""
make_zerog_sounds.py - zerog's own sound effects (demos/zerog): the weapons',
made as antigrav's are (tools/make_antigrav_sounds.py: its tones, noise and
filters; zerog plays antigrav's sounds too). Written to demos/zerog/sounds.c
and sounds.h, which the demo links in:

    demos/tools/make_zerog_sounds.py [--wav DIR]	(--wav: each sound as a .wav too, to listen to)
"""
import os, sys, wave
import numpy as np
import make_antigrav_sounds as ag
from make_antigrav_sounds import tt, noise, lowpass, highpass, bandpass, resonator, sweep, fade, unit, notes, R

OUT = os.path.join (ag.HERE, '..', 'zerog')

def s_fire ():
	# a rocket leaving: the charge's hiss, and its note going up and away
	t = tt (0.55)
	hiss = unit (bandpass (noise (len (t), 60), 600, 5200)) * np.minimum (t / 0.008, 1.0) * np.exp (-t / 0.16)
	note = sweep (t, 240, 1250, 2.0) * np.exp (-t / 0.20)
	return hiss + 0.6 * note

def s_blast ():
	# an explosion: the crack, the boom under it, the rubble after
	t = tt (1.0)
	crack = unit (highpass (noise (len (t), 61), 1500)) * np.exp (-t / 0.035)
	boom = unit (lowpass (noise (len (t), 62), 700, order=3)) * np.minimum (t / 0.004, 1.0) * np.exp (-t / 0.28)
	thud = sweep (t, 260, 110) * np.exp (-t / 0.12)
	rubble = unit (bandpass (noise (len (t), 63), 400, 2200)) * lowpass (noise (len (t), 64) ** 2, 40) * 5.0 * np.exp (-t / 0.35)
	return 0.7 * crack + 1.0 * boom + 0.7 * thud + 0.5 * rubble

def s_pickup ():
	# a weapon taken: three notes up
	return notes (0.42, ((0.0, 880.0, 0.8), (0.07, 1318.5, 0.9), (0.14, 1760.0, 1.0)), 0.10)

def s_mine ():
	# a mine dropped: a low blip
	t = tt (0.14)
	return (np.sin (2 * np.pi * 420 * t) + 0.4 * np.sin (2 * np.pi * 840 * t)) * np.minimum (t / 0.003, 1.0) * np.exp (-t / 0.05)

def s_shield ():
	# a shield going up: a shimmer rising, two voices a little apart
	t = tt (0.7)
	a = sweep (t, 330, 1320, 1.6)
	b = sweep (t, 336, 1345, 1.6)
	trem = 0.65 + 0.35 * np.sin (2 * np.pi * 14 * t)
	air = 0.3 * unit (bandpass (noise (len (t), 65), 2500, 6000))
	return (a + b + air) * trem * np.minimum (t / 0.03, 1.0) * np.exp (-t / 0.45)

def s_fall ():
	# off the road: a note falling away
	t = tt (0.9)
	f = 900 * (150.0 / 900) ** (t / t[-1]) * (1 + 0.03 * np.sin (2 * np.pi * 9 * t))
	x = np.sin (2 * np.pi * np.cumsum (f) / R)
	return (x + 0.3 * np.sign (x)) * np.minimum (t / 0.01, 1.0) * (1 - t / t[-1]) ** 0.5

# name, the samples, the rate, the peak (of 1.0)
SOUNDS = [
	('FIRE', s_fire, R, 0.9),
	('BLAST', s_blast, R, 0.95),
	('PICKUP', s_pickup, R, 0.8),
	('MINE', s_mine, R, 0.8),
	('SHIELD', s_shield, R, 0.8),
	('FALL', s_fall, R, 0.8),
]

def main ():
	wav_dir = sys.argv[sys.argv.index ('--wav') + 1] if '--wav' in sys.argv else None
	c = ['/* sounds.c - zerog\'s own sound effects: written by demos/tools/make_zerog_sounds.py */',
	     '#include "sounds.h"', '']
	table, total = [], 0
	for name, make, rate, peak in SOUNDS:
		x = np.asarray (make (), dtype=float)
		x -= x.mean ()
		x = fade (x, rate)
		x *= peak / np.abs (x).max ()
		u8 = np.clip (np.round (x * 127.0) + 128, 0, 255).astype (np.uint8)
		total += len (u8)
		c.append ('static const uint8_t %s[%u] =	/* %.2f s at %u Hz */' % (name.lower (), len (u8), len (u8) / rate, rate))
		c.append ('{')
		for k in range (0, len (u8), 24):
			c.append ('\t' + ','.join ('%u' % v for v in u8[k:k + 24]) + ',')
		c += ['};', '']
		table.append ('\t[ZSND_%s - ZSND_FIRST] = {%s, %u, %u},' % (name, name.lower (), len (u8), rate))
		if wav_dir:
			os.makedirs (wav_dir, exist_ok=True)
			w = wave.open (os.path.join (wav_dir, name.lower () + '.wav'), 'wb')
			w.setnchannels (1); w.setsampwidth (1); w.setframerate (rate); w.writeframes (u8.tobytes ()); w.close ()
	c += ['const antigrav_sound_t zerog_sounds[ZSND_COUNT - ZSND_FIRST] =', '{'] + table + ['};']
	open (os.path.join (OUT, 'sounds.c'), 'w').write ('\n'.join (c) + '\n')
	h = ['/* sounds.h - zerog\'s own sound effects: written by demos/tools/make_zerog_sounds.py */',
	     '#ifndef ZEROG_SOUNDS_H', '#define ZEROG_SOUNDS_H', '', '#include "antigrav_sounds.h"', '',
	     '/* (the values are the sounds\' ids on the RPi: after antigrav\'s, which zerog plays too) */', 'enum', '{',
	     '\tZSND_FIRST = ASND_COUNT,', '\tZSND_%s = ZSND_FIRST,' % SOUNDS[0][0]]
	h += ['\tZSND_%s,' % name for name, make, rate, peak in SOUNDS[1:]]
	h += ['\tZSND_COUNT', '};', '', 'extern const antigrav_sound_t zerog_sounds[ZSND_COUNT - ZSND_FIRST];', '', '#endif']
	open (os.path.join (OUT, 'sounds.h'), 'w').write ('\n'.join (h) + '\n')
	print ('%d sounds, %d bytes' % (len (SOUNDS), total))

if __name__ == '__main__':
	main ()
