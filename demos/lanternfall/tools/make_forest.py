#!/usr/bin/env python3
"""make_forest.py - Lanternfall's third scene, the Bell Forest: an island of
giant trees, their crowns a roof with holes the moon comes through, silent
bells hung in them. Listeners stand by the way: past them softly, the
lantern low. A ravine of sea cuts the island; the lantern shows a root
across it. Beyond: two of the Hollow, then the clearing and its beacon, and
the gate to the north shore. Ody waits at the landing till the beacon burns.
"""
import random
from lfkit import *

X0, Y0, X1, Y1, TOP = -448, -448, 1984, 2752, 900
GROUND = 32
CROWN = 592				# the crowns' underside
RAVINE = (1000, 1160)			# across the island, y
WAY = [(768, 60), (760, 300), (760, 520), (900, 800), (1104, 950), (1104, 1200), (860, 1500), (620, 1700), (800, 1880),
       (1200, 2080), (900, 1990), (900, 2180), (900, 2290)]
LISTENERS = [(420, 520), (1250, 1480), (300, 1750)]
BEACON = (900, 2050)

def near_way (x, y, d):
	for (ax, ay), (bx, by) in zip (WAY, WAY[1:]):
		t = max (0, min (1, ((x - ax) * (bx - ax) + (y - ay) * (by - ay)) / float ((bx - ax) ** 2 + (by - ay) ** 2)))
		if (x - ax - t * (bx - ax)) ** 2 + (y - ay - t * (by - ay)) ** 2 < d * d:
			return True
	return any ((x - px) ** 2 + (y - py) ** 2 < (d + 60) ** 2 for (px, py) in LISTENERS + [BEACON])

def trees ():
	r = random.Random (5)
	out = []
	for gy in range (60, 2240, 290):
		for gx in range (90, 1500, 270):
			x, y = gx + r.randint (-70, 70), gy + r.randint (-70, 70)
			if near_way (x, y, 130) or RAVINE[0] - 80 < y < RAVINE[1] + 80:
				continue
			out.append ((x, y, r.choice ((40, 48, 56)), r.random () < 0.35))
	return out

def world ():
	w = []
	w += sea (X0, Y0, X1, Y1)
	w += skybox (X0, Y0, BED - 32, X1, Y1, TOP)
	# the island, cut by the ravine; a jetty south, a pier north beyond the root wall
	w.append (box (0, 0, BED, 1536, RAVINE[0], GROUND, tex ('moss', 'stone')))
	w.append (box (0, RAVINE[1], BED, 1536, 2240, GROUND, tex ('moss', 'stone')))
	w += [box (720, -200, BED, 816, 0, GROUND - 8, 'wood'), box (852, 2240, BED, 948, 2400, GROUND, 'wood')]
	w += boxes (carve ((0, 2176, GROUND, 1536, 2240, GROUND + 160), [(836, 2176, GROUND, 964, 2240, GROUND + 120)]), 'bark')
	# the trees: a trunk each, roots at its foot; a bell in some
	for (x, y, h, bell) in trees ():
		w.append (box (x - h, y - h, GROUND, x + h, y + h, CROWN, 'bark'))
		w.append (box (x - h - 20, y - h - 20, GROUND, x + h + 20, y + h + 20, GROUND + 14, 'bark'))
		if bell:
			w += [box (x + h, y - 6, 300, x + h + 90, y + 6, 312, 'bark'), box (x + h + 50, y - 22, 236, x + h + 94, y + 22, 300, 'brass')]
	# their crowns: a roof over the island in long strips, the moon between them (their tops never seen)
	for y in range (-64, 2304, 480):
		w.append (box (-64, y, CROWN, 1600, y + 400, CROWN + 40, tex ('skip', 'leaves', 'leaves')))
	# rocks in the clearing, by the ravine
	w += [box (1240, 900, GROUND, 1330, 990, GROUND + 70, 'stone'), box (940, 1180, GROUND, 1010, 1250, GROUND + 50, 'stone'),
	      box (BEACON[0] - 60, BEACON[1] - 60, GROUND, BEACON[0] + 60, BEACON[1] + 60, GROUND + 8, 'stone')]
	return w

WORLDSPAWN = dict (NIGHT, message = 'Lanternfall: the Bell Forest', _sunlight = '110', _minlight = '9')

def entities ():
	e = [ent ('info_player_start', (768, 60, GROUND + 24), angle = 90), ent ('lf_ody', (880, 70, GROUND + 24), angle = 90, stay = 1)]
	e += [light (700, 150, GROUND + 60, 170), light (1104, 1290, GROUND + 60, 170)]
	e += [ent ('lf_shrine', (700, 150, GROUND)), ent ('lf_shrine', (1190, 1290, GROUND))]
	e.append (ent ('lf_hidden', brushes = [box (1040, RAVINE[0], GROUND - 16, 1168, RAVINE[1], GROUND, 'path')], radius = 300))
	e += [ent ('lf_hollow', (x, y, GROUND + 24), kind = 2) for (x, y) in LISTENERS]
	e += [ent ('lf_hollow', (1380, 2000, GROUND + 24), kind = 0, wake = 380), ent ('lf_hollow', (1280, 2170, GROUND + 24), kind = 0, wake = 380)]
	e += [ent ('item_ember', (1000, 700, GROUND + 24)), ent ('item_ember', (700, 1420, GROUND + 24)), ent ('item_ember', (1200, 2050, GROUND + 24))]
	e.append (ent ('lf_beacon', (BEACON[0], BEACON[1], GROUND + 8)))
	e.append (ent ('lf_gate', brushes = [box (836, 2176, GROUND, 964, 2240, GROUND + 120, 'iron')], key = 'beacon'))
	e.append (ent ('lf_exit', (900, 2330, GROUND + 24), radius = 70))
	e += [ent ('lf_story', (764, 230, GROUND + 24), radius = 110, line = 1), ent ('lf_story', (900, 2190, GROUND + 24), radius = 110, line = 2)]
	b = Bot ()
	g = GROUND + 24
	b.go (760, 300, g).go (760, 520, g, 'sneak').go (900, 800, g, 'sneak').go (1104, 950, g, 'raise').go (1104, 1200, g, 'sneak')
	b.go (860, 1500, g, 'sneak').go (620, 1700, g, 'sneak').go (800, 1880, g, 'sneak').go (1200, 2080, g, 'fight')
	b.go (900, 1980, g, 'raise').go (900, 2180, g).go (900, 2330, g)
	return e + b.entities ()

if __name__ == '__main__':
	write_map ('forest', 'Lanternfall: the Bell Forest', WORLDSPAWN, world (), entities ())
