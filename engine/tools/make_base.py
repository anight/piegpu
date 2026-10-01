#!/usr/bin/env python3
"""make_base.py - the demo level "base" for the BSP engine (engine/), a Quake
.map, with the textures of engine/tools/levelkit.py (run that first: the WAD
and the palette).

  engine/tools/make_base.py        writes engine/levels/base.map; then compile it:
  engine/tools/build-level.sh base (ericw-tools: qbsp, vis, light)

The level is rooms (empty boxes) on a 64-unit grid: every cell that isn't in
a room is solid, and the solid cells merge into box brushes, so the level is
sealed whatever the rooms are. Details (stairs, a ramp, pillars, railings,
light fixtures) are brushes of their own; a door (func_door) and a lift
(func_plat) are brush entities. Quake units: a player is 32 wide, 56 tall,
climbs steps of 18.
"""
import numpy as np, os
from levelkit import LEVELS, box, ramp_x, write_map

CELL = 64

# ---- the level ---------------------------------------------------------------------------------

THEMES = {
	'base':  {'side': 'concrete',   'top': 'floor_tiles', 'bottom': 'ceil_panels'},
	'tech':  {'side': 'metal_grey', 'top': 'floor_plate', 'bottom': 'ceil_panels'},
	'blue':  {'side': 'metal_blue', 'top': 'floor_plate', 'bottom': 'ceil_grid'},
	'brick': {'side': 'bricks',     'top': 'stone',       'bottom': 'wood'},
}

# rooms, in cells: x0, x1, y0, y1, z0, z1, theme
ROOMS = [
	(0, 10, 0, 8, 0, 4, 'base'),		# A: the start, pillars
	(10, 20, 3, 5, 0, 2, 'tech'),		# B: a corridor with a door, to C
	(20, 30, 0, 12, 0, 8, 'blue'),		# C: the atrium: a mezzanine, stairs, a lift
	(12, 20, 9, 11, 4, 6, 'tech'),		# F: from the mezzanine, high up, to G
	(0, 12, 9, 16, 0, 8, 'brick'),		# G: a hall, a ramp down to its floor
	(2, 4, 8, 9, 0, 2, 'base'),		# a passage from G back to A
]
MEZZANINE = (20, 24, 0, 12, 3, 4)		# solid cells: C's west part, floor at z 256

def cells ():
	"""the grid: -1 solid, else the room's index; with a border of solid"""
	x1 = max (r[1] for r in ROOMS) + 1; y1 = max (r[3] for r in ROOMS) + 1; z1 = max (r[5] for r in ROOMS) + 1
	g = -np.ones ((x1 + 1, y1 + 1, z1 + 1), int)	# indexed [x + 1, y + 1, z + 1]
	for k, (a, b, c, d, e, f, th) in enumerate (ROOMS):
		g[a + 1:b + 1, c + 1:d + 1, e + 1:f + 1] = k
	a, b, c, d, e, f = MEZZANINE
	g[a + 1:b + 1, c + 1:d + 1, e + 1:f + 1] = -1
	return g

def solid_brushes (g):
	"""the solid cells as few boxes, each textured by the room it faces"""
	X, Y, Z = g.shape
	theme = -np.ones (g.shape, int)			# a solid cell's theme: a room next to it
	for dx, dy, dz in ((1, 0, 0), (-1, 0, 0), (0, 1, 0), (0, -1, 0), (0, 0, 1), (0, 0, -1)):
		nb = np.roll (g, (-dx, -dy, -dz), (0, 1, 2))
		pick = (g == -1) & (nb >= 0) & (theme == -1)
		theme[pick] = nb[pick]
	used = g >= 0
	out = []
	for x in range (X):
		for y in range (Y):
			for z in range (Z):
				if used[x, y, z]:
					continue
				t = theme[x, y, z]
				ok = lambda sx, sy, sz: (not used[sx, sy, sz].any ()) and \
					np.isin (theme[sx, sy, sz], (-1, t)).all ()
				x1, y1, z1 = x + 1, y + 1, z + 1
				while x1 < X and ok (slice (x1, x1 + 1), slice (y, y1), slice (z, z1)): x1 += 1
				while y1 < Y and ok (slice (x, x1), slice (y1, y1 + 1), slice (z, z1)): y1 += 1
				while z1 < Z and ok (slice (x, x1), slice (y, y1), slice (z1, z1 + 1)): z1 += 1
				used[x:x1, y:y1, z:z1] = True
				th = THEMES[ROOMS[t][6]] if t >= 0 else THEMES['base']
				out.append (box ((x - 1) * CELL, (y - 1) * CELL, (z - 1) * CELL,
						 (x1 - 1) * CELL, (y1 - 1) * CELL, (z1 - 1) * CELL, th))
	return out

def details ():
	d = []
	# A: four pillars, hazard at their feet
	for cx in (176, 464):
		for cy in (160, 352):
			d.append (box (cx - 24, cy - 24, 0, cx + 24, cy + 24, 256, {'top': 'concrete', 'bottom': 'concrete', 'side': 'concrete'}))
			d.append (box (cx - 28, cy - 28, 0, cx + 28, cy + 28, 12, 'hazard'))
	# C: stairs up the north side to the mezzanine (16 steps of 16, 21 deep), a
	# landing at their foot, between them and the east wall, to step onto them from
	for i in range (16):
		x1 = 1872 - i * 21
		d.append (box (x1 - 21, 640, 0, x1, 768, (i + 1) * 16, {'top': 'floor_plate', 'bottom': 'floor_plate', 'side': 'hazard'}))
	# C: a railing along the mezzanine's edge, between the lift and the stairs
	d.append (box (1528, 128, 256, 1536, 640, 296, {'top': 'metal_grey', 'bottom': 'metal_grey', 'side': 'hazard'}))
	# C: the mezzanine's edge trimmed
	d.append (box (1528, 0, 192, 1536, 768, 256, 'hazard'))
	# G: a ramp from its floor up to F's
	d.append (ramp_x (256, 768, 576, 704, 0, 256, 'stone', 'bricks'))
	# light fixtures
	for (x, y, z, tex) in FIXTURES:
		d.append (box (x - 24, y - 24, z - 6, x + 24, y + 24, z, tex))
	return d

LIGHT_SCALE = 0.7
LIGHT_WAIT = 0.6			# their falloff: 1 Quake's, less reaches further

# lights: x, y, z (under the ceiling), brightness, and a fixture on the ceiling above
LIGHTS = [
	(176, 256, 250, 450, True), (464, 256, 250, 450, True), (320, 64, 250, 350, True), (320, 448, 250, 350, True),
	(800, 256, 122, 300, True), (1120, 256, 122, 300, True),
	(1408, 384, 186, 350, True), (1408, 96, 186, 250, True), (1856, 128, 506, 650, True), (1856, 640, 506, 650, True),
	(1664, 384, 506, 650, True), (1760, 384, 300, 350, False),
	(960, 640, 378, 300, True), (1216, 640, 378, 300, True),
	(256, 800, 506, 600, True), (640, 800, 506, 600, True), (448, 900, 300, 350, False), (160, 960, 96, 250, False),
]
FIXTURES = [(x, y, z + 6, 'light_blue' if 1280 <= x < 1920 else 'light_white') for (x, y, z, v, f) in LIGHTS if f]

def entities ():
	e = []
	e.append ('{\n"classname" "info_player_start"\n"origin" "96 256 24"\n"angle" "0"\n}\n')
	for (x, y, z, v, f) in LIGHTS:
		e.append ('{\n"classname" "light"\n"origin" "%d %d %d"\n"light" "%d"\n"wait" "%g"\n}\n' % (x, y, z - 16, v * LIGHT_SCALE, LIGHT_WAIT))
	# B: a door across the corridor, sliding up
	e.append ('{\n"classname" "func_door"\n"angle" "-1"\n"speed" "140"\n"wait" "3"\n"lip" "8"\n'
		  + box (952, 192, 0, 968, 320, 128, 'door') + '}\n')
	# C: a lift from the floor up to the mezzanine (func_plat: drawn at the top)
	e.append ('{\n"classname" "func_plat"\n"height" "240"\n"speed" "150"\n'
		  + box (1536, 0, 240, 1664, 128, 256, {'top': 'lift', 'bottom': 'metal_grey', 'side': 'hazard'}) + '}\n')
	return e

def main ():
	g = cells ()
	world = solid_brushes (g) + details ()
	write_map (os.path.join (LEVELS, 'base.map'), 'base: the BSP engine\'s demo level',
		   {'message': 'piegpu base', '_minlight': '20'}, world, entities ())
	print ('base.map: %d world brushes, %d lights' % (len (world), len (LIGHTS)))

if __name__ == '__main__':
	main ()
