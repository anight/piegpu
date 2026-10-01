#!/usr/bin/env python3
"""make_keep.py - the demo level "keep" for the BSP engine (engine/): a castle
at dusk, outdoors, as a Quake .map, with the textures of engine/tools/
levelkit.py (run that first: the WAD and the palette).

  engine/tools/make_keep.py        writes engine/levels/keep.map; then compile it:
  engine/tools/build-level.sh keep (ericw-tools: qbsp, vis, light)

A courtyard inside curtain walls (a walkway on top, merlons on the outer
side), a keep on an island in a moat of lava: a bridge to its portcullis
(func_door), a stepping stone on the far side, a lift inside (func_plat) to
its upper floor, and from there a bridge up to the north wall's walkway;
stairs down from the east one. Outside the walls grass, and round everything
the sky (sky brushes, the sun's light from them: _sunlight). Gems (item_gem)
on the way round; the autopilot's waypoints (engine/levels/keep_path.h) go
by all of them.
"""
import os
from levelkit import LEVELS, box, slope, carve, write_map

OUT = 640				# the grass beyond the walls
SKY_TOP = 1024
FLOOR = -160				# the ground's bottom, the moat's floor's
COURT = (0, 2048)			# the walls' outer square; 128 thick
WALL = 128
WALK = 320				# the walkway on the walls
MOAT = (576, 1472)			# the moat's outer square
ISLAND = (768, 1280)
LAVA_TOP = -40
KEEP = (832, 1216)			# the keep's outer square; walls 32 thick
KEEP_TOP = 448
UPPER = 208				# the keep's upper floor
LIFT = (1088, 1184)			# the lift, in the keep's north-east corner
BRIDGE = (976, 1072)			# the sky bridge's deck, across x

def tex (top, side, bottom = None):
	return {'top': top, 'side': side, 'bottom': bottom or side}

def boxes (bs, t):
	return [box (*b, t) for b in bs]

def ground ():
	w = []
	big = (-OUT, -OUT, FLOOR, COURT[1] + OUT, COURT[1] + OUT, 0)
	court = (COURT[0], COURT[0], FLOOR, COURT[1], COURT[1], 0)
	hole = (MOAT[0], MOAT[0], FLOOR, MOAT[1], MOAT[1], 0)
	w += boxes (carve (big, [court]), tex ('grass', 'rock'))
	w += boxes (carve (court, [hole]), tex ('cobbles', 'rock'))
	w.append (box (MOAT[0], MOAT[0], FLOOR, MOAT[1], MOAT[1], -96, 'rock'))		# the moat's floor
	w.append (box (ISLAND[0], ISLAND[0], -96, ISLAND[1], ISLAND[1], 0, tex ('flagstone', 'rock')))
	w.append (box (992, 1344, FLOOR, 1056, 1408, 0, tex ('flagstone', 'rock')))	# the stepping stone
	return w

def lava ():
	ring = (MOAT[0], MOAT[0], -96, MOAT[1], MOAT[1], LAVA_TOP)
	island = (ISLAND[0], ISLAND[0], -96, ISLAND[1], ISLAND[1], LAVA_TOP)
	stone = (992, 1344, -96, 1056, 1408, LAVA_TOP)
	return boxes (carve (ring, [island, stone]), '*lava1')

def curtain ():
	"""the walls round the courtyard, the walkway on top, a parapet and merlons outside"""
	a, b = COURT
	w = []
	walls = [(a, a, b, a + WALL), (a, b - WALL, b, b), (a, a + WALL, a + WALL, b - WALL), (b - WALL, a + WALL, b, b - WALL)]
	for (x0, y0, x1, y1) in walls:
		w.append (box (x0, y0, 0, x1, y1, WALK, tex ('flagstone', 'castle')))
	# the parapet, 24 thick, 32 high, along the outer edge; merlons 32 wide every 64 on it
	par = [(a, a, b, a + 24), (a, b - 24, b, b), (a, a + 24, a + 24, b - 24), (b - 24, a + 24, b, b - 24)]
	for (x0, y0, x1, y1) in par:
		w.append (box (x0, y0, WALK, x1, y1, WALK + 32, 'castle'))
		along_x = x1 - x0 > y1 - y0
		lo, hi = (x0, x1) if along_x else (y0, y1)
		for m in range (lo + 16, hi - 32, 64):
			if along_x:
				w.append (box (m, y0, WALK + 32, m + 32, y1, WALK + 80, 'castle'))
			else:
				w.append (box (x0, m, WALK + 32, x1, m + 32, WALK + 80, 'castle'))
	# the gate (shut) in the south wall, inside
	w.append (box (960, WALL, 0, 1088, WALL + 8, 192, 'gate'))
	# banners on the north wall, inside
	for x in (448, 1600):
		w.append (box (x - 40, b - WALL - 4, 96, x + 40, b - WALL, 288, 'banner'))
	return w

def keep ():
	a, b = KEEP
	shell = (a, a, 0, b, b, KEEP_TOP)
	inner = (a + 32, a + 32, 0, b - 32, b - 32, KEEP_TOP - 16)
	holes = [inner,
		 (992, a, 0, 1056, a + 32, 128),				# the door, south
		 (BRIDGE[0], b - 32, UPPER, BRIDGE[1], b, UPPER + 128),		# to the sky bridge, north
		 (a, 992, 256, a + 32, 1056, 320), (b - 32, 992, 256, b, 1056, 320),	# windows, west and east
		 (896, a, 64, 912, a + 32, 144), (1136, a, 64, 1152, a + 32, 144),	# arrow slits, south
		 ]
	w = boxes (carve (shell, holes), tex ('castle', 'castle', 'wood'))
	# the upper floor, a hole for the lift
	slab = (a + 32, a + 32, UPPER - 16, b - 32, b - 32, UPPER)
	w += boxes (carve (slab, [(LIFT[0], LIFT[0], UPPER - 16, LIFT[1], LIFT[1], UPPER)]), tex ('planks', 'wood', 'wood'))
	# battlements on the roof
	for (x0, y0, x1, y1) in [(a, a, b, a + 16), (a, b - 16, b, b), (a, a + 16, a + 16, b - 16), (b - 16, a + 16, b, b - 16)]:
		w.append (box (x0, y0, KEEP_TOP, x1, y1, KEEP_TOP + 24, 'castle'))
		along_x = x1 - x0 > y1 - y0
		lo, hi = (x0, x1) if along_x else (y0, y1)
		for m in range (lo + 8, hi - 24, 48):
			w.append (box (m, y0, KEEP_TOP + 24, m + 24, y1, KEEP_TOP + 56, 'castle') if along_x else
				  box (x0, m, KEEP_TOP + 24, x1, m + 24, KEEP_TOP + 56, 'castle'))
	# a banner over the door, outside; crates inside
	w.append (box (984, a - 4, 160, 1064, a, 400, 'banner'))
	w.append (box (880, 1120, 0, 944, 1184, 64, 'wood'))
	w.append (box (944, 1136, 0, 992, 1184, 48, 'wood'))
	w.append (box (888, 1128, 64, 936, 1176, 112, 'wood'))
	return w

def bridges ():
	w = []
	# south: planks over the moat to the keep's door, low rails
	w.append (box (960, MOAT[0], -16, 1088, ISLAND[0], 0, tex ('planks', 'wood')))
	w.append (box (952, MOAT[0], -16, 960, ISLAND[0], 24, 'wood'))
	w.append (box (1088, MOAT[0], -16, 1096, ISLAND[0], 24, 'wood'))
	# the sky bridge: from the keep's upper floor up to the north walkway, rails both sides
	y0, y1 = KEEP[1], COURT[1] - WALL
	w.append (slope ('y', y0, y1, BRIDGE[0], BRIDGE[1], UPPER, WALK, 16, 'planks', 'wood'))
	w.append (slope ('y', y0, y1, BRIDGE[0] - 8, BRIDGE[0], UPPER + 32, WALK + 32, 48, 'wood', 'wood'))
	w.append (slope ('y', y0, y1, BRIDGE[1], BRIDGE[1] + 8, UPPER + 32, WALK + 32, 48, 'wood', 'wood'))
	# a pillar under it in the courtyard
	under = UPPER + (1680 - y0) / (y1 - y0) * (WALK - UPPER) - 16
	w.append (box (992, 1680, 0, 1056, 1744, int (under) - 8, 'castle'))
	return w

def stairs ():
	"""down from the east walkway, southwards along the wall: a landing, 20 steps of 16, 24 deep"""
	x0, x1 = COURT[1] - WALL - 128, COURT[1] - WALL
	return [box (x0, 1720, 0, x1, 1800, WALK, tex ('flagstone', 'castle'))] + \
	       [box (x0, 1696 - 24 * i, 0, x1, 1720 - 24 * i, WALK - 16 * i, tex ('flagstone', 'castle')) for i in range (20)]

def court ():
	"""a well, braziers on the island, pillars, crates"""
	w = []
	well = (1560, 260, 0, 1640, 340, 40)
	w += boxes (carve (well, [(1576, 276, 0, 1624, 324, 40)]), tex ('castle', 'castle'))
	w.append (box (1576, 276, 0, 1624, 324, 24, '*water1'))		# the water in it
	for (x, y) in ((800, 800), (1248, 800), (800, 1248), (1248, 1248)):
		w.append (box (x - 16, y - 16, 0, x + 16, y + 16, 40, 'castle'))
		w.append (box (x - 12, y - 12, 40, x + 12, y + 12, 52, 'light_flame'))
	for (x, y) in ((384, 768), (384, 1280), (1664, 768)):
		w.append (box (x - 32, y - 32, 0, x + 32, y + 32, 16, 'flagstone'))
		w.append (box (x - 20, y - 20, 16, x + 20, y + 20, 224, 'castle'))
	for (x, y, s, h) in ((224, 1760, 64, 64), (288, 1760, 64, 48), (224, 1696, 48, 32), (232, 1768, 48, 112)):
		w.append (box (x, y, h - s if h > s else 0, x + s, y + s, h, 'wood'))
	return w

# the sun from low in the west-south-west; the sky's dome; torches and lava glow
WORLDSPAWN = {'message': 'piegpu keep', '_sunlight': '170', '_sun_mangle': '20 -28 0', '_sunlight2': '140',
	      '_dirt': '1', '_minlight': '20'}
# torches: where on a wall's face, and which way it faces
TORCHES = [(128, 512, 'x+'), (128, 1536, 'x+'), (1920, 512, 'x-'), (512, 1920, 'y-'), (1536, 1920, 'y-'),
	   (512, 128, 'y+'), (1536, 128, 'y+'), (960, 832, 'y-'), (1088, 832, 'y-')]
DIRS = {'x+': (1, 0), 'x-': (-1, 0), 'y+': (0, 1), 'y-': (0, -1)}
INSIDE = [(960, 960, 160, 260), (1100, 1000, 160, 220), (960, 1100, 400, 240), (1060, 940, 400, 220)]

def torches ():
	w = []
	for (x, y, d) in TORCHES:
		dx, dy = DIRS[d]
		cx, cy = x + dx * 6, y + dy * 6			# the bracket: from the wall out 12
		w.append (box (cx - 6, cy - 6, 136, cx + 6, cy + 6, 148, 'castle'))
		w.append (box (cx - 4, cy - 4, 148, cx + 4, cy + 4, 164, 'light_flame'))
	return w

def lights ():
	e = []
	light = lambda x, y, z, v, wait = 1.0: '{\n"classname" "light"\n"origin" "%d %d %d"\n"light" "%d"\n"wait" "%g"\n}\n' % (x, y, z, v, wait)
	for (x, y, d) in TORCHES:
		dx, dy = DIRS[d]
		e.append (light (x + dx * 32, y + dy * 32, 176, 200))
	for (x, y) in ((800, 800), (1248, 800), (800, 1248), (1248, 1248)):
		e.append (light (x, y, 80, 180))
	for (x, y, z, v) in INSIDE:
		e.append (light (x, y, z, v, 0.8))
	a, b = MOAT
	glow = set ()					# the lava's glow, round the moat
	for k in range (a + 96, b, 192):
		glow |= {(k, a + 96), (k, b - 96), (a + 96, k), (b - 96, k)}
	for (x, y) in sorted (glow):
		e.append (light (x, y, LAVA_TOP + 24, 220, 0.8))
	return e

GEMS = [(384, 384, 32), (1024, 672, 32), (960, 1000, 32), (920, 940, UPPER + 32), (1024, 1568, 296),
	(1984, 1984, WALK + 32), (1856, 1468, 192), (1760, 960, 32), (1600, 420, 32)]

def entities ():
	e = ['{\n"classname" "info_player_start"\n"origin" "1024 256 24"\n"angle" "90"\n}\n']
	e += lights ()
	e += ['{\n"classname" "item_gem"\n"origin" "%d %d %d"\n}\n' % g for g in GEMS]
	# the portcullis: up into the wall over the door
	e.append ('{\n"classname" "func_door"\n"angle" "-1"\n"speed" "120"\n"wait" "4"\n"lip" "8"\n'
		  + box (992, KEEP[0] + 8, 0, 1056, KEEP[0] + 24, 128, 'iron_bars') + '}\n')
	# the lift: up through the upper floor (func_plat: drawn at the top)
	e.append ('{\n"classname" "func_plat"\n"height" "%d"\n"speed" "150"\n' % (UPPER - 16)
		  + box (LIFT[0], LIFT[0], UPPER - 16, LIFT[1], LIFT[1], UPPER, tex ('lift', 'castle')) + '}\n')
	return e

def sky ():
	a, b = -OUT - 16, COURT[1] + OUT + 16
	lo, hi = -OUT, COURT[1] + OUT
	return [box (a, a, FLOOR, lo, b, SKY_TOP, 'sky_dusk'), box (hi, a, FLOOR, b, b, SKY_TOP, 'sky_dusk'),
		box (lo, a, FLOOR, hi, lo, SKY_TOP, 'sky_dusk'), box (lo, hi, FLOOR, hi, b, SKY_TOP, 'sky_dusk'),
		box (a, a, SKY_TOP, b, b, SKY_TOP + 16, 'sky_dusk'), box (a, a, FLOOR - 16, b, b, FLOOR, 'rock')]

def main ():
	world = ground () + lava () + curtain () + keep () + bridges () + stairs () + court () + torches () + sky ()
	write_map (os.path.join (LEVELS, 'keep.map'), 'keep: the BSP engine\'s castle', WORLDSPAWN, world, entities ())
	print ('keep.map: %d world brushes, %d gems' % (len (world), len (GEMS)))

if __name__ == '__main__':
	main ()
