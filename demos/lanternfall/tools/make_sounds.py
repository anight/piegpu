#!/usr/bin/env python3
"""
make_sounds.py - Lanternfall's own sounds (demos/lanternfall/sounds.c, .h),
made from nothing as the others are (demos/tools/make_antigrav_sounds.py: its
tones, noise and filters; the engine's steps, doors and water it plays too):

    demos/lanternfall/tools/make_sounds.py [--wav DIR]	(--wav: each as a .wav too, to listen to)

The music is two of them: a cello's note and a bell's, which the game plays
at the pitches of its phrases (lanternfall.c).
"""
import os, sys, wave
import numpy as np
HERE = os.path.dirname (os.path.abspath (__file__))
sys.path.insert (0, os.path.join (HERE, '..', '..', 'tools'))
from make_antigrav_sounds import noise, lowpass, highpass, bandpass, resonator, fade, unit
from scipy import signal

OUT = os.path.join (HERE, '..')
R = 11025

def tt (seconds):
	return np.arange (int (seconds * R)) / R

def sweep (t, f0, f1):
	f = f0 * (f1 / f0) ** (t / t[-1])
	return 2 * np.pi * np.cumsum (f) / R

def s_cello ():
	# a bowed string at C3: a sawtooth's harmonics through the body's resonances, a vibrato coming in, the bow's noise
	t = tt (2.6); f0 = 130.81
	vib = 1 + 0.006 * np.sin (2 * np.pi * 5.2 * t) * np.clip ((t - 0.4) / 0.6, 0, 1)
	ph = 2 * np.pi * np.cumsum (f0 * vib) / R
	x = sum (np.sin (n * ph) / n for n in range (1, 28))
	x = x + 0.08 * bandpass (noise (len (t), 80), 400, 3000, R)
	body = sum (g * resonator (x, hz, q, R) for hz, q, g in ((280, 4, 1.0), (460, 5, 0.7), (900, 4, 0.5), (1700, 3, 0.25)))
	env = np.minimum (t / 0.22, 1) * np.exp (-np.clip (t - 1.2, 0, None) / 0.7)
	return (0.5 * x + body) * env

def s_bell ():
	# a bell far off: partials that don't line up, each dying at its own pace
	t = tt (2.6); f0 = 392.0
	x = sum (g * np.sin (2 * np.pi * f0 * r * t) * np.exp (-t / d)
		 for r, g, d in ((0.5, 0.5, 1.6), (1.0, 1.0, 1.1), (1.19, 0.5, 0.8), (1.56, 0.35, 0.55), (2.0, 0.5, 0.6), (2.66, 0.22, 0.3), (3.76, 0.12, 0.18)))
	return x * np.minimum (t / 0.004, 1)

def s_raise ():
	t = tt (0.5)
	return (np.sin (sweep (t, 220, 440)) + 0.5 * np.sin (sweep (t, 330, 660))) * np.sin (np.pi * t / t[-1]) ** 0.7 \
	       + 0.15 * bandpass (noise (len (t), 81), 1500, 4000, R) * np.exp (-t / 0.15)

def s_flare ():
	t = tt (0.8)
	burst = unit (bandpass (noise (len (t), 82), 500, 4500, R)) * np.exp (-t / 0.16)
	ring = np.sin (sweep (t, 880, 660)) * np.exp (-t / 0.3) + 0.5 * np.sin (sweep (t, 1320, 990)) * np.exp (-t / 0.2)
	return burst + 0.7 * ring

def s_reveal ():
	t = tt (1.1)
	x = sum (np.sin (2 * np.pi * f * t) * np.clip ((t - d) / 0.05, 0, 1) * np.exp (-np.clip (t - d, 0, None) / 0.35)
		 for f, d in ((523.3, 0.0), (659.3, 0.12), (784.0, 0.24), (1046.5, 0.36)))
	return x * (0.8 + 0.2 * np.sin (2 * np.pi * 11 * t))

def s_beacon ():
	# a fire catching: the rush of it, and a low bell under
	t = tt (2.2)
	rush = unit (bandpass (noise (len (t), 83), 200, 2600, R)) * np.minimum (t / 0.25, 1) * np.exp (-t / 0.7)
	bell = sum (g * np.sin (2 * np.pi * 196.0 * r * t) * np.exp (-t / d) for r, g, d in ((1, 1, 1.4), (2, 0.5, 0.9), (2.4, 0.3, 0.6), (3, 0.3, 0.5)))
	return rush + 0.9 * bell

def s_shrine ():
	t = tt (0.9)
	return (np.sin (2 * np.pi * 784 * t) + 0.6 * np.sin (2 * np.pi * 1174.7 * t)) * np.exp (-t / 0.3) * np.minimum (t / 0.004, 1)

def s_hurt ():
	t = tt (0.3)
	return unit (bandpass (noise (len (t), 84), 300, 1800, R)) * np.exp (-t / 0.05) + 0.6 * np.sin (sweep (t, 300, 140)) * np.exp (-t / 0.1)

def s_out ():
	# the light gone: a note sinking away
	t = tt (1.7)
	return (np.sin (sweep (t, 330, 82)) + 0.4 * np.sin (sweep (t, 495, 123))) * (1 - t / t[-1]) ** 0.6 * np.minimum (t / 0.02, 1)

def s_hollow ():
	# black water waking: a wet hiss, bubbles
	t = tt (0.8)
	hiss = unit (bandpass (noise (len (t), 85), 700, 3500, R)) * np.minimum (t / 0.1, 1) * np.exp (-t / 0.35)
	bub = np.sin (sweep (t, 180, 420)) * (np.sin (2 * np.pi * 13 * t) > 0.3) * np.exp (-t / 0.4)
	return hiss + 0.6 * bub

def s_hollow_die ():
	t = tt (0.7)
	return unit (highpass (noise (len (t), 86), 1800, R)) * np.exp (-t / 0.2) + 0.5 * np.sin (sweep (t, 500, 90)) * np.exp (-t / 0.25)

def s_roar ():
	# a brute rearing: a low growl coming up
	t = tt (1.1)
	g = np.sign (np.sin (sweep (t, 55, 110))) * 0.6 + np.sin (sweep (t, 82, 165))
	return lowpass (g * (0.6 + 0.4 * np.sin (2 * np.pi * 23 * t)), 900, R) * np.minimum (t / 0.3, 1) * np.minimum ((t[-1] - t) / 0.1, 1)

def s_thud ():
	t = tt (0.45)
	return np.sin (sweep (t, 140, 45)) * np.exp (-t / 0.12) + 0.6 * unit (lowpass (noise (len (t), 87), 500, R)) * np.exp (-t / 0.06)

def s_shriek ():
	t = tt (0.9)
	f = 1400 + 500 * np.sin (2 * np.pi * 9 * t)
	x = np.sin (2 * np.pi * np.cumsum (f) / R) + 0.5 * np.sin (2 * np.pi * np.cumsum (f * 1.5) / R)
	return x * np.minimum (t / 0.03, 1) * np.exp (-t / 0.5)

def s_stone ():
	# Ody's foot
	t = tt (0.3)
	return np.sin (sweep (t, 110, 50)) * np.exp (-t / 0.07) + 0.5 * unit (lowpass (noise (len (t), 88), 700, R)) * np.exp (-t / 0.04)

def s_smash ():
	# a wall coming down
	t = tt (1.0)
	hit = np.sin (sweep (t, 160, 40)) * np.exp (-t / 0.1)
	rubble = unit (bandpass (noise (len (t), 89), 200, 3000, R)) * lowpass (noise (len (t), 90) ** 2, 30, R) * 6 * np.exp (-t / 0.35)
	crack = unit (highpass (noise (len (t), 91), 1500, R)) * np.exp (-t / 0.03)
	return hit + rubble + 0.6 * crack

def s_ody ():
	# Ody speaking: two low notes, stone on stone
	t = tt (0.5)
	x = np.sin (2 * np.pi * 98 * t) * (t < 0.22) + np.sin (2 * np.pi * 82.4 * t) * (t >= 0.25)
	return lowpass (np.sign (x) * 0.4 + x, 600, R) * (0.7 + 0.3 * np.sin (2 * np.pi * 30 * t)) * np.minimum ((t[-1] - t) / 0.08, 1)

def s_thunder ():
	t = tt (3.0)
	x = lowpass (noise (len (t), 92), 220, R, 3) * lowpass (np.abs (noise (len (t), 93)), 6, R) * 8
	crack = unit (bandpass (noise (len (t), 94), 300, 2500, R)) * np.exp (-t / 0.12)
	return unit (x) * np.exp (-t / 1.3) + 0.5 * crack

def s_bolt ():
	t = tt (0.6)
	return unit (bandpass (noise (len (t), 95), 300, 1500, R)) * np.sin (np.pi * t / t[-1]) + 0.5 * np.sin (sweep (t, 300, 120))

def s_king ():
	# the Tide King struck: a great bell cracked
	t = tt (1.4)
	x = sum (g * np.sin (2 * np.pi * 110.0 * r * t) * np.exp (-t / d) for r, g, d in ((1, 1, 0.9), (1.47, 0.7, 0.6), (2.09, 0.5, 0.4), (2.56, 0.4, 0.3)))
	return x + 0.4 * unit (bandpass (noise (len (t), 96), 300, 2000, R)) * np.exp (-t / 0.08)

SOUNDS = [
	('CELLO', s_cello, 0.85), ('BELL', s_bell, 0.8), ('RAISE', s_raise, 0.6), ('FLARE', s_flare, 0.9), ('REVEAL', s_reveal, 0.7),
	('BEACON', s_beacon, 0.9), ('SHRINE', s_shrine, 0.6), ('HURT', s_hurt, 0.85), ('OUT', s_out, 0.8), ('HOLLOW', s_hollow, 0.8),
	('HOLLOW_DIE', s_hollow_die, 0.8), ('ROAR', s_roar, 0.9), ('THUD', s_thud, 0.9), ('SHRIEK', s_shriek, 0.8),
	('STONE', s_stone, 0.8), ('SMASH', s_smash, 0.95), ('ODY', s_ody, 0.6), ('THUNDER', s_thunder, 0.9), ('BOLT', s_bolt, 0.7),
	('KING', s_king, 0.9),
]

def main ():
	wav_dir = sys.argv[sys.argv.index ('--wav') + 1] if '--wav' in sys.argv else None
	c = ['/* sounds.c - Lanternfall\'s own sounds: written by demos/lanternfall/tools/make_sounds.py */', '#include "sounds.h"', '']
	table, total = [], 0
	for name, make, peak in SOUNDS:
		x = np.asarray (make (), dtype=float)
		x -= x.mean ()
		x = fade (x, R)
		x *= peak / np.abs (x).max ()
		u8 = np.clip (np.round (x * 127.0) + 128, 0, 255).astype (np.uint8)
		total += len (u8)
		c.append ('static const uint8_t %s[%u] =	/* %.2f s at %u Hz */' % (name.lower (), len (u8), len (u8) / R, R))
		c.append ('{')
		for k in range (0, len (u8), 24):
			c.append ('\t' + ','.join ('%u' % v for v in u8[k:k + 24]) + ',')
		c += ['};', '']
		table.append ('\t[LSND_%s - LSND_FIRST] = {%s, %u, %u},' % (name, name.lower (), len (u8), R))
		if wav_dir:
			os.makedirs (wav_dir, exist_ok=True)
			w = wave.open (os.path.join (wav_dir, name.lower () + '.wav'), 'wb')
			w.setnchannels (1); w.setsampwidth (1); w.setframerate (R); w.writeframes (u8.tobytes ()); w.close ()
	c += ['const lf_sound_t lf_sounds[LSND_COUNT - LSND_FIRST] =', '{'] + table + ['};']
	open (os.path.join (OUT, 'sounds.c'), 'w').write ('\n'.join (c) + '\n')
	h = ['/* sounds.h - Lanternfall\'s own sounds: written by demos/lanternfall/tools/make_sounds.py */',
	     '#ifndef LANTERNFALL_SOUNDS_H', '#define LANTERNFALL_SOUNDS_H', '', '#include <stdint.h>', '#include "sounds/sound_data.h"', '',
	     '/* (the values are the sounds\' ids on the RPi: after the engine\'s, which the game plays too) */', 'enum', '{',
	     '\tLSND_FIRST = SND_COUNT,', '\tLSND_%s = LSND_FIRST,' % SOUNDS[0][0]]
	h += ['\tLSND_%s,' % name for name, make, peak in SOUNDS[1:]]
	h += ['\tLSND_COUNT', '};', '', 'typedef struct', '{', '\tconst uint8_t *samples;\t\t/* 8 bits unsigned, mono */',
	      '\tuint32_t frames;', '\tuint16_t rate;', '} lf_sound_t;', '', 'extern const lf_sound_t lf_sounds[LSND_COUNT - LSND_FIRST];', '', '#endif']
	open (os.path.join (OUT, 'sounds.h'), 'w').write ('\n'.join (h) + '\n')
	print ('%d sounds, %d bytes' % (len (SOUNDS), total))

if __name__ == '__main__':
	main ()
