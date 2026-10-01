#!/usr/bin/env python3
"""
make_antigrav_sounds.py - antigrav's sound effects (demos/antigrav.c), made
here from nothing: tones, noise and filters (8 bits, mono). Written to
demos/antigrav_sounds.c and antigrav_sounds.h, which the demo links in:

    demos/tools/make_antigrav_sounds.py [--wav DIR]	(--wav: each sound as a .wav too, to listen to)

The engines, the wind and the crowd are loops, made periodic (whole numbers
of periods, noise from the frequency domain) so they go round without a
click; the demo changes their pitch and volume as the race goes. A small
speaker has nothing under 150 Hz or so: what matters is at 200 Hz - 3 kHz.
"""
import os, sys, wave
import numpy as np
from scipy import signal

HERE = os.path.dirname (os.path.abspath (__file__))
OUT = os.path.join (HERE, '..')
R = 22050			# the short sounds' and the craft's own engine's
RA = 11025			# the other loops'

def tt (seconds, rate=R):
	return np.arange (int (seconds * rate)) / rate

def noise (n, seed):
	return np.random.default_rng (seed).uniform (-1.0, 1.0, n)

def lowpass (x, hz, rate=R, order=2):
	b, a = signal.butter (order, hz / (rate / 2), 'low')
	return signal.lfilter (b, a, x)

def highpass (x, hz, rate=R, order=2):
	b, a = signal.butter (order, hz / (rate / 2), 'high')
	return signal.lfilter (b, a, x)

def bandpass (x, lo, hi, rate=R, order=2):
	b, a = signal.butter (order, [lo / (rate / 2), hi / (rate / 2)], 'band')
	return signal.lfilter (b, a, x)

def resonator (x, hz, q, rate=R):
	b, a = signal.iirpeak (hz / (rate / 2), q)
	return signal.lfilter (b, a, x)

def sweep (t, f0, f1, curve=1.0):
	"""a sine whose frequency goes from f0 to f1 over t (curve > 1: most of it early)"""
	u = (t / t[-1]) ** (1.0 / curve)
	f = f0 * (f1 / f0) ** u
	return np.sin (2 * np.pi * np.cumsum (f) / (len (t) / t[-1]))

def fade (x, rate=R, ms_in=2.0, ms_out=8.0):
	n_in, n_out = int (rate * ms_in / 1000), int (rate * ms_out / 1000)
	x = x.copy ()
	x[:n_in] *= np.linspace (0, 1, n_in)
	x[-n_out:] *= np.linspace (1, 0, n_out)
	return x

def unit (x):
	return x / np.abs (x).max ()

def periodic_noise (n, lo, hi, rate, seed, tilt=0.0):
	"""noise that goes round: random phases between lo and hi Hz, back from the frequency domain"""
	rng = np.random.default_rng (seed)
	f = np.fft.rfftfreq (n, 1.0 / rate)
	mag = ((f >= lo) & (f <= hi)).astype (float)
	mag[1:] *= (f[1:] / max (lo, 1.0)) ** tilt
	spec = mag * np.exp (2j * np.pi * rng.uniform (0, 1, len (f)))
	return unit (np.fft.irfft (spec, n))

def voice (t, f0, top, centre, width, seed):
	"""a buzz that goes round in a second: f0's harmonics up to top Hz, most of them about centre Hz"""
	rng = np.random.default_rng (seed)
	out = np.zeros (len (t))
	for n in range (1, int (top / f0) + 1):
		f = n * f0
		out += (1.0 / n) * (0.35 + 2.0 * np.exp (-((f - centre) / width) ** 2)) * np.sin (2 * np.pi * f * t + rng.uniform (0, 2 * np.pi))
	return out

def s_engine ():
	# the craft followed: a turbine's drone, two of it a hertz apart (it
	# throbs), a fifth over them, the whine of the blades and their air
	t = tt (1.0)
	drone = voice (t, 110, 4200, 700, 450, 1) + 0.8 * voice (t, 111, 4200, 760, 450, 2) + 0.45 * voice (t, 165, 3600, 900, 500, 3)
	whine = np.sin (2 * np.pi * 880 * t) + 0.8 * np.sin (2 * np.pi * 884 * t) + 0.4 * np.sin (2 * np.pi * 1763 * t)
	air = periodic_noise (len (t), 500, 3800, R, 4, -0.4) * (0.75 + 0.25 * np.sin (2 * np.pi * 55 * t))
	return unit (drone) + 0.22 * unit (whine) + 0.30 * air

def s_engine_other ():
	# the others': thinner and higher, heard from outside
	t = tt (1.0, RA)
	drone = voice (t, 147, 4800, 1300, 700, 5) + 0.7 * voice (t, 149, 4800, 1500, 700, 6)
	whine = np.sin (2 * np.pi * 1176 * t) + 0.7 * np.sin (2 * np.pi * 1181 * t)
	air = periodic_noise (len (t), 900, 4500, RA, 7, -0.3)
	return unit (drone) + 0.30 * unit (whine) + 0.35 * air

def s_wind ():
	# the air past the canopy: its gusts go round in two seconds
	n = int (2.0 * RA)
	t = np.arange (n) / RA
	gust = 0.8 + 0.2 * periodic_noise (n, 0.4, 3.0, RA, 9)
	return periodic_noise (n, 280, 4200, RA, 8, -0.7) * gust

def s_crowd ():
	# the stands: many voices as one roar, swelling and sinking
	n = int (3.0 * RA)
	roar = np.zeros (n)
	for k, (lo, hi, g) in enumerate (((350, 900, 1.0), (800, 1500, 0.8), (1900, 2900, 0.25))):
		roar += g * periodic_noise (n, lo, hi, RA, 20 + k) * (0.7 + 0.3 * periodic_noise (n, 0.3, 2.4, RA, 30 + k))
	return roar

def s_boost ():
	# a speed pad: a charge that jumps up, and the air torn after it
	t = tt (0.8)
	zap = sweep (t, 260, 2100, 3.0) * np.exp (-t / 0.16) + 0.5 * sweep (t, 390, 3150, 3.0) * np.exp (-t / 0.10)
	rush = bandpass (noise (len (t), 40), 700, 5200) * np.minimum (t / 0.03, 1.0) * np.exp (-t / 0.28)
	return 0.8 * zap + 1.0 * unit (rush)

def s_whoosh ():
	# under a gantry or through a hoop: the air between, high as it comes and low as it goes
	t = tt (0.36)
	env = np.exp (-((t - 0.11) / 0.07) ** 2 * np.where (t < 0.11, 2.2, 0.6))
	x = noise (len (t), 41)
	mix = np.clip ((t - 0.06) / 0.14, 0.0, 1.0)
	return (unit (bandpass (x, 900, 3600)) * (1 - mix) + unit (bandpass (x, 350, 1500)) * mix) * env

def s_bump ():
	# two hulls touching: a short clank
	t = tt (0.22)
	hit = noise (len (t), 42) * np.exp (-t / 0.004)
	clank = sum (g * resonator (hit, hz, 14) for hz, g in ((520, 1.0), (1180, 0.8), (1730, 0.6), (2650, 0.3)))
	thud = 0.5 * sweep (t, 320, 150) * np.exp (-t / 0.035)
	return unit (clank) * np.exp (-t / 0.06) + thud

def s_scrape ():
	# the wall: metal along concrete, and its sparks
	t = tt (0.42)
	grit = lowpass (noise (len (t), 43) ** 2, 90) * 6.0
	body = unit (bandpass (noise (len (t), 44), 1100, 4800)) * (0.4 + np.clip (grit, 0, 1.5))
	ring = 0.5 * unit (resonator (noise (len (t), 45), 2350, 9))
	return (body + ring) * np.minimum (t / 0.006, 1.0) * np.exp (-t / 0.16)

def tone (t, hz, decay):
	return (np.sin (2 * np.pi * hz * t) + 0.3 * np.sin (4 * np.pi * hz * t)) * np.exp (-t / decay)

def notes (seconds, score, decay):
	t = tt (seconds)
	out = np.zeros (len (t))
	for at, hz, g in score:
		k = int (at * R)
		out[k:] += g * tone (t[:len (t) - k], hz, decay)
	return out

def s_beep ():
	# the countdown's three
	t = tt (0.22)
	return tone (t, 660, 0.25) * np.minimum (t / 0.004, 1.0) * (t < 0.17)

def s_go ():
	t = tt (0.75)
	return tone (t, 1320, 0.30) * np.minimum (t / 0.004, 1.0)

def s_lap ():
	return notes (0.5, ((0.0, 880.0, 1.0), (0.10, 1174.7, 1.0)), 0.11)

def s_finish ():
	return notes (1.5, ((0.0, 659.3, 0.8), (0.11, 784.0, 0.8), (0.22, 987.8, 0.8), (0.33, 1318.5, 1.0), (0.33, 659.3, 0.5)), 0.30)

# name, the samples, the rate, a loop?, the peak (of 1.0)
SOUNDS = [
	('ENGINE', s_engine, R, True, 0.85),
	('ENGINE_OTHER', s_engine_other, RA, True, 0.85),
	('WIND', s_wind, RA, True, 0.85),
	('CROWD', s_crowd, RA, True, 0.85),
	('BOOST', s_boost, R, False, 0.9),
	('WHOOSH', s_whoosh, R, False, 0.9),
	('BUMP', s_bump, R, False, 0.9),
	('SCRAPE', s_scrape, R, False, 0.9),
	('BEEP', s_beep, R, False, 0.8),
	('GO', s_go, R, False, 0.8),
	('LAP', s_lap, R, False, 0.8),
	('FINISH', s_finish, R, False, 0.8),
]

def main ():
	wav_dir = sys.argv[sys.argv.index ('--wav') + 1] if '--wav' in sys.argv else None
	c = ['/* antigrav_sounds.c - antigrav\'s sound effects: written by demos/tools/make_antigrav_sounds.py */',
	     '#include "antigrav_sounds.h"', '']
	table, total = [], 0
	for name, make, rate, loop, peak in SOUNDS:
		x = np.asarray (make (), dtype=float)
		x -= x.mean ()
		if not loop:
			x = fade (x, rate)
		x *= peak / np.abs (x).max ()
		u8 = np.clip (np.round (x * 127.0) + 128, 0, 255).astype (np.uint8)
		total += len (u8)
		c.append ('static const uint8_t %s[%u] =	/* %.2f s at %u Hz%s */' % (name.lower (), len (u8), len (u8) / rate, rate,
											     ', a loop' if loop else ''))
		c.append ('{')
		for k in range (0, len (u8), 24):
			c.append ('\t' + ','.join ('%u' % v for v in u8[k:k + 24]) + ',')
		c += ['};', '']
		table.append ('\t[ASND_%s] = {%s, %u, %u},' % (name, name.lower (), len (u8), rate))
		if wav_dir:
			os.makedirs (wav_dir, exist_ok=True)
			w = wave.open (os.path.join (wav_dir, name.lower () + '.wav'), 'wb')
			w.setnchannels (1); w.setsampwidth (1); w.setframerate (rate); w.writeframes (u8.tobytes ()); w.close ()
	c += ['const antigrav_sound_t antigrav_sounds[ASND_COUNT] =', '{'] + table + ['};']
	open (os.path.join (OUT, 'antigrav_sounds.c'), 'w').write ('\n'.join (c) + '\n')
	h = ['/* antigrav_sounds.h - antigrav\'s sound effects: written by demos/tools/make_antigrav_sounds.py */',
	     '#ifndef ANTIGRAV_SOUNDS_H', '#define ANTIGRAV_SOUNDS_H', '', '#include <stdint.h>', '',
	     '/* (the values are the sounds\' ids on the RPi) */', 'enum', '{', '\tASND_NONE,']
	h += ['\tASND_%s,%s' % (name, '\t/* a loop */' if loop else '') for name, make, rate, loop, peak in SOUNDS]
	h += ['\tASND_COUNT', '};', '',
	     'typedef struct', '{', '\tconst uint8_t *samples;\t\t/* 8 bits unsigned, mono */', '\tuint32_t frames;',
	     '\tuint16_t rate;', '} antigrav_sound_t;', '',
	     'extern const antigrav_sound_t antigrav_sounds[ASND_COUNT];', '', '#endif']
	open (os.path.join (OUT, 'antigrav_sounds.h'), 'w').write ('\n'.join (h) + '\n')
	print ('%d sounds, %d bytes' % (len (SOUNDS), total))

main ()
