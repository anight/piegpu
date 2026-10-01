#!/usr/bin/env python3
"""
make_sounds.py - the BSP engine's sound effects (engine/sound.c), made here
from nothing: noise, sines and filters, in the manner of Quake's (8 bits,
mono; the short ones at 22050 Hz, the ambient loops at 11025). Written to
engine/sounds/sound_data.c and sound_data.h, which the demos link in:

    engine/tools/make_sounds.py [--wav DIR]	(--wav: each sound as a .wav too, to listen to)

A loop is made periodic (its noise in the frequency domain, its tones a whole
number of periods), so it goes round without a click.
"""
import os, sys, wave
import numpy as np
from scipy import signal

HERE = os.path.dirname (os.path.abspath (__file__))
OUT = os.path.join (HERE, '..', 'sounds')
R = 22050			# the short sounds'
RA = 11025			# the ambient loops'

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

def periodic_noise (n, lo, hi, rate, seed, tilt=0.0):
	"""noise that goes round: random phases between lo and hi Hz, back from the frequency domain"""
	rng = np.random.default_rng (seed)
	f = np.fft.rfftfreq (n, 1.0 / rate)
	mag = ((f >= lo) & (f <= hi)).astype (float)
	mag[1:] *= (f[1:] / max (lo, 1.0)) ** tilt
	spec = mag * np.exp (2j * np.pi * rng.uniform (0, 1, len (f)))
	x = np.fft.irfft (spec, n)
	return x / np.abs (x).max ()

def grunt (seconds, f0, f1, seed, formants=((620, 5, 1.0), (1050, 6, 0.7), (2400, 8, 0.25))):
	"""a voice's short 'hup': a falling buzz through a throat's resonances, a little breath"""
	t = tt (seconds)
	f = f0 * (f1 / f0) ** (t / t[-1])
	phase = np.cumsum (f) / R
	buzz = 2.0 * (phase % 1.0) - 1.0
	buzz = lowpass (buzz, 3500)
	out = sum (g * resonator (buzz, hz, q) for hz, q, g in formants)
	out += 0.15 * bandpass (noise (len (t), seed), 800, 3000)
	env = np.minimum (t / 0.02, 1.0) * np.exp (-t / (seconds * 0.45))
	return out * env

def step (seed, cut, gap):
	# a soft step: no tone in it (a thud with a pitch is a drum), only the
	# sole's two brushes on the ground, the heel's and then the toe's, each
	# rising in a few milliseconds
	t = tt (0.24)
	def brush (at, lo, hi, decay, seed):
		u = np.maximum (t - at, 0.0)
		return bandpass (noise (len (t), seed), lo, hi) * np.minimum (u / 0.007, 1.0) ** 2 * np.exp (-u / decay) * (t >= at)
	heel = brush (0.0, 170, cut, 0.032, seed)
	toe = brush (gap, 350, cut * 2.2, 0.026, seed + 50)
	return heel / np.abs (heel).max () + 0.55 * toe / np.abs (toe).max ()

def s_jump ():
	return grunt (0.22, 165, 115, 11)

def s_land ():
	t = tt (0.18)
	return 0.25 * sweep (t, 240, 110) * np.exp (-t / 0.05) + 1.0 * bandpass (noise (len (t), 12), 180, 1200) * np.exp (-t / 0.035)

def s_land_hard ():
	t = tt (0.38)
	thud = 0.3 * sweep (t, 220, 90) * np.exp (-t / 0.08) + 1.0 * bandpass (noise (len (t), 13), 160, 1000) * np.exp (-t / 0.05)
	ugh = np.zeros (len (t))
	g = grunt (0.26, 135, 82, 14, formants=((520, 5, 1.0), (900, 6, 0.6), (2200, 8, 0.2)))
	at = int (0.05 * R)
	ugh[at:at + len (g)] = g[:len (t) - at]
	return thud + 0.9 * ugh / np.abs (ugh).max ()

def tone (t, hz, decay):
	return (np.sin (2 * np.pi * hz * t) + 0.3 * np.sin (4 * np.pi * hz * t)) * np.exp (-t / decay)

def s_item ():
	t = tt (0.32)
	out = np.zeros (len (t))
	for at, hz in ((0.0, 987.8), (0.085, 1318.5)):
		k = int (at * R)
		out[k:] += tone (t[:len (t) - k], hz, 0.07)
	return out

def s_complete ():
	t = tt (1.0)
	out = np.zeros (len (t))
	for at, hz, decay in ((0.0, 523.3, 0.12), (0.11, 659.3, 0.12), (0.22, 784.0, 0.12), (0.33, 1046.5, 0.3),
			      (0.33, 784.0, 0.3), (0.33, 659.3, 0.3)):
		k = int (at * R)
		out[k:] += tone (t[:len (t) - k], hz, decay)
	return out

def s_plat_move ():			# a loop: machinery. An electric motor's whine, a ratchet's clatter, a chain
	n = int (0.5 * R)		# (every part a whole number of turns in the half second)
	t = np.arange (n) / R
	# the motor: 220 Hz and its harmonics, its pitch and its strength wavering 20 times a second
	waver = np.sin (2 * np.pi * 20 * t)
	phase = 2 * np.pi * 220 * t + 0.35 * waver
	whine = sum (np.sin (k * phase + k) / k ** 0.8 for k in range (1, 9)) * (1.0 + 0.25 * waver)
	# the ratchet: 24 clicks a second, strong and weak in turn, each ringing in the metal
	clatter = np.zeros (n)
	rng = np.random.default_rng (21)
	for k in range (12):
		length = int (0.016 * R)
		tk = np.arange (length) / R
		click = rng.uniform (-1, 1, length) * np.exp (-tk / 0.0012)
		ring = (np.sin (2 * np.pi * 1150 * tk) + 0.7 * np.sin (2 * np.pi * 1730 * tk + 1.0) + 0.4 * np.sin (2 * np.pi * 2610 * tk)) \
		       * np.exp (-tk / 0.004)
		idx = (int (k * n / 12) + np.arange (length)) % n
		clatter[idx] += (1.0 if k % 2 == 0 else 0.55) * (0.6 * click + ring)
	# the chain: a low pulse, 110 a second
	chain = np.sign (np.sin (2 * np.pi * 110 * t)) * (np.sin (2 * np.pi * 110 * t) ** 8)
	chain = bandpass (np.tile (chain, 3), 150, 900)[n:2 * n]
	return 0.55 * whine / np.abs (whine).max () + 0.9 * clatter / np.abs (clatter).max () + 0.5 * chain / np.abs (chain).max ()

def s_plat_stop ():			# it locks: a clank, a second one as the latch falls, the brake's hiss
	t = tt (0.42)
	def clank (decay, pitch):
		return sum (a * np.sin (2 * np.pi * hz * pitch * t) * np.exp (-t / (d * decay))
			    for hz, a, d in ((212, 0.7, 0.09), (419, 1.0, 0.08), (733, 0.7, 0.06), (1171, 0.45, 0.04), (1873, 0.25, 0.03)))
	out = clank (1.0, 1.0) + 0.8 * noise (len (t), 22) * np.exp (-t / 0.004)
	at = int (0.07 * R)
	latch = 0.6 * clank (0.6, 1.35) + 0.6 * noise (len (t), 23) * np.exp (-t / 0.003)
	out[at:] += latch[:len (t) - at]
	hiss = highpass (noise (len (t), 24), 2500) * np.minimum (t / 0.02, 1.0) * np.exp (-t / 0.12)
	out += 0.18 * np.roll (hiss, at)
	return out

def s_door_move ():			# a loop: stone on stone
	n = int (0.6 * R)
	t = np.arange (n) / R
	grit = periodic_noise (n, 250, 2400, R, 23, tilt=-0.3)
	rough = 0.55 + 0.45 * periodic_noise (n, 8, 40, R, 24)
	return grit * rough + 0.2 * np.sin (2 * np.pi * 90 * t)

def s_door_stop ():
	t = tt (0.26)
	return 0.25 * sweep (t, 230, 110) * np.exp (-t / 0.06) + 1.0 * bandpass (noise (len (t), 25), 220, 1800) * np.exp (-t / 0.03)

def s_teleport ():			# a whoosh: noise through a band that sweeps up and back, and a rising shimmer
	t = tt (0.9)
	x = noise (len (t), 26)
	out = np.zeros (len (t))
	zi = np.zeros (4)
	for k in range (0, len (t), 64):
		u = k / len (t)
		centre = 300.0 * (10.0 ** (1.0 - abs (2.0 * u - 0.9) / 1.1))
		b, a = signal.butter (2, [centre * 0.7 / (R / 2), min (centre * 1.4, R * 0.45) / (R / 2)], 'band')
		out[k:k + 64], zi = signal.lfilter (b, a, x[k:k + 64], zi=zi)
	shimmer = sweep (t, 420, 1700, 1.5) * (1.0 + 0.5 * np.sin (2 * np.pi * 31 * t))
	env = np.minimum (t / 0.05, 1.0) * np.exp (-t / 0.35)
	return (out / np.abs (out).max () + 0.5 * shimmer) * env

def s_push ():				# a jump pad: a spring's boing
	t = tt (0.45)
	wobble = 1.0 + 0.06 * np.sin (2 * np.pi * 27 * t) * np.exp (-t / 0.15)
	u = t / t[-1]
	f = 180.0 * (1100.0 / 180.0) ** (u ** 0.6) * wobble
	phase = 2 * np.pi * np.cumsum (f) / R
	return (np.sin (phase) + 0.35 * np.sin (2 * phase)) * np.exp (-t / 0.16)

def s_burn ():				# into the lava: a sizzle, crackles, a falling moan
	t = tt (0.9)
	sizzle = highpass (noise (len (t), 27), 2200) * np.minimum (t / 0.01, 1.0) * np.exp (-t / 0.35)
	rng = np.random.default_rng (28)
	crackle = np.zeros (len (t))
	for at in rng.uniform (0.0, 0.6, 28):
		k = int (at * R)
		crackle[k:k + 40] += rng.uniform (0.4, 1.0) * noise (40, int (at * 1000)) * np.exp (-np.arange (40) / 8.0)
	moan = sweep (t, 300, 85) * np.exp (-t / 0.3)
	return 0.8 * sizzle + 0.7 * crackle + 0.6 * moan

def chirps (n, rate, seed, count, f_lo, f_hi, wrap):
	"""bubbles: short rising chirps at random places (wrapped round, for a loop)"""
	rng = np.random.default_rng (seed)
	out = np.zeros (n)
	for _ in range (count):
		at = int (rng.uniform (0, n if wrap else n * 0.7))
		length = int (rate * rng.uniform (0.02, 0.05))
		t = np.arange (length) / rate
		f0 = rng.uniform (f_lo, f_hi)
		c = np.sin (2 * np.pi * (f0 * t + f0 * 8.0 * t * t)) * np.hanning (length) * rng.uniform (0.4, 1.0)
		idx = (at + np.arange (length)) % n if wrap else at + np.arange (length)
		ok = idx < n
		out[idx[ok]] += c[ok]
	return out

def s_splash ():
	t = tt (0.45)
	wash = bandpass (noise (len (t), 29), 500, 4000) * np.minimum (t / 0.005, 1.0) * np.exp (-t / 0.11)
	return wash + 0.6 * chirps (len (t), R, 30, 9, 500, 1100, False)

def s_amb_lava ():			# a loop: a deep rumble, slow bubbles
	n = int (2.0 * RA)
	return 0.7 * periodic_noise (n, 90, 600, RA, 31, tilt=-0.3) + 0.8 * chirps (n, RA, 32, 18, 180, 480, True)

def s_amb_wind ():			# a loop: wind, rising and falling
	n = int (3.0 * RA)
	t = np.arange (n) / RA
	gust = 0.6 + 0.25 * np.sin (2 * np.pi * t / 3.0) + 0.15 * np.sin (2 * np.pi * 2 * t / 3.0 + 1.3)
	whistle = 0.5 + 0.5 * np.sin (2 * np.pi * t / 1.5 + 0.4)
	return periodic_noise (n, 90, 700, RA, 33, tilt=-0.7) * gust + 0.35 * periodic_noise (n, 700, 1500, RA, 34) * gust * whistle

def s_amb_water ():			# a loop: water lapping
	n = int (2.0 * RA)
	t = np.arange (n) / RA
	lap = 0.45 + 0.55 * np.maximum (np.sin (2 * np.pi * t), 0.0) ** 2
	return periodic_noise (n, 300, 2600, RA, 35, tilt=-0.4) * lap + 0.3 * chirps (n, RA, 36, 10, 400, 900, True)

def s_amb_hum ():			# a loop: a base's machines
	n = int (1.0 * RA)
	t = np.arange (n) / RA
	hum = sum (a * np.sin (2 * np.pi * hz * t + hz) for hz, a in ((100, 0.5), (200, 1.0), (300, 0.7), (400, 0.4), (600, 0.25)))
	hum *= 1.0 + 0.15 * np.sin (2 * np.pi * 2 * t)
	return hum / np.abs (hum).max () + 0.2 * periodic_noise (n, 400, 2200, RA, 37)

# name, the samples, the rate, a loop?, the peak (of 1.0)
SOUNDS = [
	('STEP1', lambda: step (1, 700, 0.070), R, False, 0.9),
	('STEP2', lambda: step (2, 600, 0.080), R, False, 0.9),
	('STEP3', lambda: step (3, 780, 0.065), R, False, 0.9),
	('STEP4', lambda: step (4, 650, 0.075), R, False, 0.9),
	('JUMP', s_jump, R, False, 0.9),
	('LAND', s_land, R, False, 0.9),
	('LAND_HARD', s_land_hard, R, False, 0.95),
	('ITEM', s_item, R, False, 0.8),
	('COMPLETE', s_complete, R, False, 0.8),
	('PLAT_MOVE', s_plat_move, R, True, 0.8),
	('PLAT_STOP', s_plat_stop, R, False, 0.9),
	('DOOR_MOVE', s_door_move, R, True, 0.8),
	('DOOR_STOP', s_door_stop, R, False, 0.9),
	('TELEPORT', s_teleport, R, False, 0.9),
	('PUSH', s_push, R, False, 0.9),
	('BURN', s_burn, R, False, 0.9),
	('SPLASH', s_splash, R, False, 0.9),
	('AMB_LAVA', s_amb_lava, RA, True, 0.8),
	('AMB_WIND', s_amb_wind, RA, True, 0.8),
	('AMB_WATER', s_amb_water, RA, True, 0.8),
	('AMB_HUM', s_amb_hum, RA, True, 0.8),
]

def main ():
	wav_dir = sys.argv[sys.argv.index ('--wav') + 1] if '--wav' in sys.argv else None
	os.makedirs (OUT, exist_ok=True)
	c = ['/* sound_data.c - the BSP engine\'s sound effects: written by engine/tools/make_sounds.py */',
	     '#include "sound_data.h"', '']
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
		table.append ('\t[SND_%s] = {%s, %u, %u, %s},' % (name, name.lower (), len (u8), rate, 'true' if loop else 'false'))
		if wav_dir:
			os.makedirs (wav_dir, exist_ok=True)
			w = wave.open (os.path.join (wav_dir, name.lower () + '.wav'), 'wb')
			w.setnchannels (1); w.setsampwidth (1); w.setframerate (rate); w.writeframes (u8.tobytes ()); w.close ()
	c += ['const sound_data_t sound_data[SND_COUNT] =', '{'] + table + ['};']
	open (os.path.join (OUT, 'sound_data.c'), 'w').write ('\n'.join (c) + '\n')
	h = ['/* sound_data.h - the BSP engine\'s sound effects: written by engine/tools/make_sounds.py */',
	     '#ifndef ENGINE_SOUND_DATA_H', '#define ENGINE_SOUND_DATA_H', '', '#include <stdbool.h>', '#include <stdint.h>', '',
	     'enum', '{', '\tSND_NONE,']
	h += ['\tSND_%s,' % name for name, *_ in SOUNDS] + ['\tSND_COUNT', '};', '',
	     'typedef struct', '{', '\tconst uint8_t *samples;\t\t/* 8 bits unsigned, mono */', '\tuint32_t frames;',
	     '\tuint16_t rate;', '\tbool loop;\t\t\t/* made to go round */', '} sound_data_t;', '',
	     'extern const sound_data_t sound_data[SND_COUNT];', '', '#endif']
	open (os.path.join (OUT, 'sound_data.h'), 'w').write ('\n'.join (h) + '\n')
	print ('%d sounds, %d bytes' % (len (SOUNDS), total))

main ()
