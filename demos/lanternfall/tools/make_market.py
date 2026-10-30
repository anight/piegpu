#!/usr/bin/env python3
"""make_market.py - Lanternfall's fourth scene, the Glass Market: greenhouses
in a storm. A glass passage with the Hollow among its planters; a shrine;
then the market hall, its roof fallen in, where two brutes wait: the first
big fight. When the last of them is gone the north gate opens on the court
of the beacon, and the pier beyond.
"""
from lfkit import *

X0, Y0, X1, Y1, TOP = -512, -512, 1792, 2816, 900
G = 32

def glass_house (x0, y0, x1, y1, z1, holes, roof = True):
	"""walls of glass on iron posts round x0 .. y1 (16 thick), but for the holes; a roof of glass"""
	w = []
	for b in ((x0, y0, G, x1, y0 + 16, z1), (x0, y1 - 16, G, x1, y1, z1), (x0, y0 + 16, G, x0 + 16, y1 - 16, z1), (x1 - 16, y0 + 16, G, x1, y1 - 16, z1)):
		w += boxes (carve (b, holes), 'glass')
	for (x, y) in ((x0, y0), (x1 - 24, y0), (x0, y1 - 24), (x1 - 24, y1 - 24)):
		w.append (box (x, y, G, x + 24, y + 24, z1 + 16, 'iron'))
	if roof:
		w.append (box (x0, y0, z1, x1, y1, z1 + 16, 'glass'))
	return w

def world ():
	w = []
	w += sea (X0, Y0, X1, Y1)
	w += skybox (X0, Y0, BED - 32, X1, Y1, TOP)
	w.append (box (0, -64, BED, 1280, 2304, G, tex ('stone', 'stone')))			# the island
	w += [box (576, -256, BED, 704, -64, G - 8, 'wood')]					# the jetty
	# the glass passage: planters down its sides
	w += glass_house (512, 128, 768, 640, 232, [(576, 128, G, 704, 144, 176), (576, 624, G, 704, 640, 176)])
	for y in (176, 336, 496):
		w += [box (528, y, G, 584, y + 112, G + 34, tex ('moss', 'wood')), box (696, y, G, 752, y + 112, G + 34, tex ('moss', 'wood'))]
	# the market hall: its roof gone; planters and stalls to go round
	w += glass_house (128, 768, 1152, 1792, 330, [(576, 768, G, 704, 784, 176), (576, 1776, G, 704, 1792, 176)], roof = False)
	for (x, y) in ((300, 1000), (820, 1000), (300, 1480), (820, 1480)):
		w.append (box (x, y, G, x + 160, y + 96, G + 38, tex ('moss', 'wood')))
	w += [box (180, 1240, G, 276, 1300, G + 70, 'wood'), box (1010, 1220, G, 1100, 1290, G + 70, 'wood'),
	      box (200, 1680, G, 330, 1740, G + 90, 'wood'), box (1000, 840, G, 1090, 900, G + 60, 'wood')]
	w += [box (150, 1300, 280, 1130, 1316, 296, 'iron'), box (632, 790, 280, 648, 1770, 296, 'iron')]	# what's left of the roof
	# the court of the beacon, walled; the gate east to the pier
	w += boxes (carve ((352, 1792, G, 928, 2240, G + 150), [(384, 1792, G, 896, 2208, G + 150), (576, 1792, G, 704, 1824, G + 144),
		    (896, 1936, G, 928, 2064, G + 120)]), 'plaster')
	w.append (box (576, 1936, G, 704, 2064, G + 8, 'stone'))
	w.append (box (928, 1936, BED, 1500, 2064, G, 'wood'))
	w += [box (1380, 2080, -8, 1560, 2170, G - 12, 'wood'), box (1460, 2110, G - 12, 1476, 2126, G + 120, 'bark')]
	# glass houses far off, in the sea
	w += [box (-300, 600, BED, -120, 1000, 150, 'glass'), box (1400, 300, BED, 1640, 560, 200, 'glass'), box (-350, 1700, BED, -150, 1900, 110, 'glass')]
	return w

WORLDSPAWN = dict (NIGHT, message = 'Lanternfall: the Glass Market', _sunlight = '60')

def entities ():
	e = [ent ('info_player_start', (640, 20, G + 24), angle = 90), ent ('lf_ody', (740, 0, G + 24), angle = 90)]
	e += [light (640, 700, G + 70, 190), light (640, 400, 200, 110)]
	e.append (ent ('lf_shrine', (560, 700, G)))
	e += [ent ('lf_hollow', (640, 480, G + 24), kind = 0, wake = 300), ent ('lf_hollow', (640, 600, G + 24), kind = 0, wake = 260)]
	e += [ent ('lf_hollow', (640, 1500, G + 24), kind = 1, wake = 560, group = 1), ent ('lf_hollow', (330, 1640, G + 24), kind = 1, wake = 330, group = 1),
	      ent ('lf_hollow', (1000, 1100, G + 24), kind = 0, wake = 320, group = 1), ent ('lf_hollow', (980, 1660, G + 24), kind = 0, wake = 320, group = 1)]
	e += [ent ('item_ember', (220, 860, G + 24)), ent ('item_ember', (1080, 1700, G + 24)), ent ('item_ember', (640, 1240, G + 24)),
	      ent ('item_ember', (200, 1560, G + 24))]
	e.append (ent ('lf_gate', brushes = [box (576, 1776, G, 704, 1824, G + 144, 'iron')], key = 'kill', id = 1))
	e.append (ent ('lf_beacon', (640, 2000, G + 8)))
	e.append (ent ('lf_gate', brushes = [box (896, 1936, G, 928, 2064, G + 120, 'iron')], key = 'beacon'))
	e.append (ent ('lf_exit', (1420, 2000, G + 24), radius = 70))
	b = Bot ()
	g = G + 24
	b.go (640, 100, g).go (640, 330, g, 'fight').go (640, 690, g).go (590, 700, g).go (640, 900, g).go (640, 1150, g, 'fight')
	b.go (500, 1400, g, 'fight').go (640, 1700, g).go (640, 1880, g).go (640, 1930, g, 'raise').go (860, 2000, g).go (1420, 2000, g)
	return e + b.entities ()

if __name__ == '__main__':
	write_map ('market', 'Lanternfall: the Glass Market', WORLDSPAWN, world (), entities ())
