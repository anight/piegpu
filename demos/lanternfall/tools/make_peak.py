#!/usr/bin/env python3
"""make_peak.py - Lanternfall's last scene, the Lantern Peak: the summit, in
the storm. A bridge to it; a shrine; the court where the Tide King waits over
three anchors of black water, which Ody breaks: each time the King falls,
and the lantern burns a third of him. Then the choice: up the steps to the
great lantern, or down to the tide pool at the edge.
"""
from lfkit import *

X0, Y0, X1, Y1, TOP = -800, -900, 2336, 2336, 1100
G = 64
ANCHORS = [(384, 600), (1152, 600), (768, 1150)]

def world ():
	w = []
	w += sea (X0, Y0, X1, Y1)
	w += skybox (X0, Y0, BED - 32, X1, Y1, TOP)
	w.append (box (0, 0, BED, 1536, 1536, G, tex ('stone', 'stone')))			# the summit
	w.append (box (704, -420, G - 24, 832, 0, G, 'stone'))					# the bridge to it
	w += [box (704, -420, BED, 832, -300, G - 24, 'stone')]
	# the great lantern: steps up to its dais; four posts and a crown of brass; its bowl
	w += stairs ('y', 1216, 1408, 576, 960, G, 160, 'stone')
	w.append (box (576, 1408, G, 960, 1536, 160, 'stone'))
	for (x, y) in ((640, 1440), (880, 1440), (640, 1510), (880, 1510)):
		w.append (box (x, y, 160, x + 16, y + 16, 330, 'iron'))
	w += [box (624, 1424, 330, 912, 1536, 350, 'brass'), box (700, 1450, 350, 836, 1520, 400, 'brass'),
	      box (736, 1456, 160, 800, 1520, 190, 'iron')]
	# the tide pool at the south-east edge: a basin, a hand of water
	basin = (1280, 96, G - 20, 1472, 288, G)
	w = [b for b in w if b is not None]
	w.append (box (1280, 96, G - 20, 1472, 288, G - 8, '*sea'))
	w += [box (1264, 80, G, 1280, 304, G + 12, 'stone'), box (1472, 80, G, 1488, 304, G + 12, 'stone'),
	      box (1280, 80, G, 1472, 96, G + 12, 'stone')]
	# broken columns round the court; rocks
	for (x, y, h) in ((160, 160, 200), (160, 1376, 120), (1376, 1376, 260), (1376, 560, 90), (160, 760, 150), (480, 1300, 70), (1056, 1300, 110)):
		w.append (box (x - 24, y - 24, G, x + 24, y + 24, G + h, 'statue'))
	return w

WORLDSPAWN = dict (NIGHT, message = 'Lanternfall: the Lantern Peak', _sunlight = '75')

def entities ():
	e = [ent ('info_player_start', (768, -240, G + 24), angle = 90), ent ('lf_ody', (768, -370, G + 24), angle = 90)]
	e += [light (700, 130, G + 70, 190)]
	e.append (ent ('lf_shrine', (690, 120, G)))
	for (x, y) in ANCHORS:
		e.append (ent ('lf_break', brushes = [box (x - 44, y - 44, G, x + 44, y + 44, G + 230, 'iron'), box (x - 60, y - 60, G, x + 60, y + 60, G + 24, 'iron')],
			       key = 'anchor'))
	e.append (ent ('lf_king', (768, 1000, G + 300)))
	e += [ent ('lf_hollow', (220, 300, G + 24), kind = 0, wake = 1), ent ('lf_hollow', (1320, 1240, G + 24), kind = 0, wake = 1),
	      ent ('lf_hollow', (240, 1240, G + 24), kind = 0, wake = 1)]
	e += [ent ('item_ember', (300, 900, G + 24)), ent ('item_ember', (1240, 900, G + 24)), ent ('item_ember', (768, 420, G + 24))]
	e += [ent ('lf_choice', (768, 1440, 184), which = 0), ent ('lf_choice', (1376, 192, G + 24), which = 1)]
	b = Bot ()
	g = G + 24
	b.go (768, -60, g).go (740, 150, g).go (490, 520, g, 'ody').go (768, 700, g, 'raise').go (1050, 520, g, 'ody').go (768, 700, g, 'raise')
	b.go (768, 1030, g, 'ody').go (768, 700, g, 'raise').go (768, 1180, g).go (768, 1380, 184, 'raise').go (1340, 330, g).go (1376, 260, g, 'raise')
	return e + b.entities ()

if __name__ == '__main__':
	write_map ('peak', 'Lanternfall: the Lantern Peak', WORLDSPAWN, world (), entities ())
