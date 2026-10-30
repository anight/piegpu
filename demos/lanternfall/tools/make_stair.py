#!/usr/bin/env python3
"""make_stair.py - Lanternfall's fifth scene, the Stair of Kings: a climb
round a tower of rock out of the sea, a flight to each side, past colossal
statues. South: whole. East: a stretch fallen away, which the lantern
bridges. North: two of the Hollow on the steps. At the north-west landing a
wall Ody breaks; the west flight ends under a ledge he lifts Wren to; on the
top the beacon, more of the Hollow, and the gate to the bridge to the peak.
"""
from lfkit import *

X0, Y0, X1, Y1, TOP = -700, -700, 2300, 2300, 1700
B = 32					# the base terrace
C0, C1 = 512, 1024			# the tower's core
O0, O1 = 320, 1216			# the flights' outer edge
SE, NE, NW, WEST, TOPZ = 288, 544, 800, 960, 1056	# the landings' heights; the west flight's end; the top

def colossus (x, y, face):
	"""a king of stone, 620 tall, his back to the tower (face: the way he looks: 'x' or 'y', and its sign)"""
	ax, sg = face
	def b (a0, a1, c0, c1, z0, z1, t = 'statue'):
		return box (x + a0, y + c0, z0, x + a1, y + c1, z1, t) if ax == 'x' else box (x + c0, y + a0, z0, x + c1, y + a1, z1, t)
	return [b (-60, 60, -100, -20, BED, 250), b (-60, 60, 20, 100, BED, 250), b (-70, 70, -110, 110, 250, 500),
		b (-50, 50, -150, -110, 300, 480), b (-50, 50, 110, 150, 300, 480), b (-45, 45, -50, 50, 500, 590),
		b (-55, 55, -60, 60, 590, 620, 'brass')]

def world ():
	w = []
	w += sea (X0, Y0, X1, Y1)
	w += skybox (X0, Y0, BED - 32, X1, Y1, TOP)
	w.append (box (192, 192, BED, 1344, 1344, B, tex ('stone', 'stone')))			# the base
	w.append (box (C0, C0, B, C1, C1, TOPZ, tex ('stone', 'stone')))			# the tower
	w += [box (192, 120, BED, 290, 192, B - 8, 'wood')]					# a landing stage
	# south flight, east along the tower's foot; its landing
	w += stairs ('x', C0, C1, O0, C0, B, SE, tex ('stone', 'stone'))
	w.append (box (C1, O0, B, O1, C0, SE, 'stone'))
	# east flight, north: whole but for a stretch (the lantern's)
	w.append (box (C1, C0, B, O1, C1, B + 8, 'stone'))
	for k in range (16):
		y = C0 + 32 * k
		if not 5 <= k < 11:
			w.append (box (C1, y, SE - 16, O1, y + 32, SE + 16 * (k + 1), 'stone'))
	w.append (box (C1, C1, NE - 16, O1, O1, NE, 'stone'))					# north-east landing
	w += [box (O1 - 24, O1 - 24, B, O1, O1, NE - 16, 'stone'), box (C1, C1, B, C1 + 24, C1 + 24, NE - 16, 'stone')]
	# north flight, west; north-west landing
	for k in range (16):
		x = C1 - 32 * (k + 1)
		w.append (box (x, C1, NE - 16 + 16 * k, x + 32, O1, NE + 16 * (k + 1), 'stone'))
	w.append (box (O0, C1, NW - 16, C0, O1, NW, 'stone'))
	w += [box (O0, O1 - 24, B, O0 + 24, O1, NW - 16, 'stone')]
	# west flight, south: ten steps, then a level stretch under the top's ledge
	for k in range (10):
		y = C1 - 24 - 32 * (k + 1)
		w.append (box (O0, y, NW - 16 + 16 * k, C0, y + 32, NW + 16 * (k + 1), 'stone'))
	w.append (box (O0, C0, WEST - 16, C0, C1 - 24 - 320, WEST, 'stone'))
	w.append (box (O0 + 40, C0 + 20, WEST, O0 + 152, C0 + 132, WEST + 2, tex ('rune', 'stone')))
	# the top: the beacon's step, broken columns; the bridge east
	w.append (box (704, 704, TOPZ, 832, 832, TOPZ + 8, 'stone'))
	for (x, y, h) in ((540, 540, 150), (980, 560, 90), (560, 980, 60), (960, 960, 190)):
		w.append (box (x - 20, y - 20, TOPZ, x + 20, y + 20, TOPZ + h, 'statue'))
	w.append (box (C1, 704, TOPZ - 24, 1500, 832, TOPZ, 'stone'))
	w += [box (1380, 704, B, 1500, 832, TOPZ - 24, 'stone')]
	# the kings, in the sea round the tower
	w += colossus (768, 20, ('y', -1)) + colossus (1520, 768, ('x', 1)) + colossus (768, 1520, ('y', 1)) + colossus (20, 768, ('x', -1))
	return w

WORLDSPAWN = dict (NIGHT, message = 'Lanternfall: the Stair of Kings', _sunlight = '85')

def entities ():
	e = [ent ('info_player_start', (300, 300, B + 24), angle = 0), ent ('lf_ody', (300, 400, B + 24), angle = 0)]
	e += [light (1120, 1120, NE + 70, 190)]
	e.append (ent ('lf_hidden', brushes = [box (C1, C0 + 32 * k, SE - 16, O1, C0 + 32 * k + 32, SE + 16 * (k + 1), 'path') for k in range (5, 11)],
		       radius = 330))
	e.append (ent ('lf_shrine', (1150, 1150, NE)))
	e += [ent ('lf_hollow', (900, 1120, NE + 90), kind = 0, wake = 300), ent ('lf_hollow', (650, 1120, NE + 215), kind = 0, wake = 300)]
	e.append (ent ('lf_break', brushes = [box (O0, C1 - 24, NW, C0, C1, NW + 144, 'cracked')]))
	e.append (ent ('lf_lift', (O0 + 96, C0 + 76, WEST + 24), target = 'up'))
	e.append (ent ('info_notnull', (C0 + 110, C0 + 90, TOPZ + 150), targetname = 'up'))
	e += [ent ('lf_hollow', (900, 900, TOPZ + 24), kind = 0, wake = 330), ent ('lf_hollow', (900, 620, TOPZ + 24), kind = 0, wake = 330)]
	e += [ent ('item_ember', (1120, 416, SE + 24)), ent ('item_ember', (416, 1120, NW + 24)), ent ('item_ember', (620, 900, TOPZ + 24))]
	e.append (ent ('lf_beacon', (768, 768, TOPZ + 8)))
	e.append (ent ('lf_gate', brushes = [box (C1, 704, TOPZ, C1 + 24, 832, TOPZ + 120, 'iron')], key = 'beacon'))
	e.append (ent ('lf_exit', (1420, 768, TOPZ + 24), radius = 70))
	e += [ent ('lf_story', (1120, 416, SE + 24), radius = 110, line = 1), ent ('lf_story', (416, 1120, NW + 24), radius = 110, line = 2)]
	b = Bot ()
	b.go (460, 416, B + 24).go (1000, 416, SE + 8).go (1120, 416, SE + 24).go (1120, 590, SE + 56, 'raise').go (1120, 1010, NE + 8)
	b.go (1120, 1120, NE + 24, 'fight').go (1000, 1120, NE + 40).go (530, 1120, NW + 8).go (416, 1120, NW + 24).go (416, 1080, NW + 24, 'ody')
	b.go (416, 960, NW + 40).go (416, 680, WEST + 24).go (470, 600, WEST + 24, 'ody').go (700, 700, TOPZ + 24, 'fight')
	b.go (768, 700, TOPZ + 32, 'raise').go (980, 768, TOPZ + 24).go (1420, 768, TOPZ + 24)
	return e + b.entities ()

if __name__ == '__main__':
	write_map ('stair', 'Lanternfall: the Stair of Kings', WORLDSPAWN, world (), entities ())
