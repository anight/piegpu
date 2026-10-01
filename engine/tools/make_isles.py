#!/usr/bin/env python3
"""make_isles.py - the demo level "isles" for the BSP engine (engine/):
islands floating in a summer sky, as a Quake .map, with the textures of
engine/tools/levelkit.py (run that first: the WAD and the palette).

  engine/tools/make_isles.py       writes engine/levels/isles.map; then compile it:
  engine/tools/build-level.sh isles (ericw-tools: qbsp, vis, light)

The start island; a platform (func_train between two path_corners) across
the void to the next; a jump pad there (trigger_push, its target the top of
the arc) up to a high island with a pond spilling over its edge; a bridge
down from it to ruins, where a portal (trigger_teleport) goes back to the
start. Stepping stones from the ruins back to the start, for a jumper. Coins
(item_coin) on the way round, one in the air over the jump; the autopilot's
waypoints (engine/levels/isles_path.h) go by all of them. Below everything
the sky: a fall is the caller's to notice (the demo: under VOID_Z).
"""
import os
from levelkit import LEVELS, box, slope, carve, write_map

SKY = (-2048, 3072, -1536, 1536)	# the sky box: x and y, z
S = (-256, -256, 256, 256, 0)		# islands: x0 y0 x1 y1, their tops
A = (704, -192, 1088, 192, 0)
B = (672, 672, 1120, 1120, 256)
C = (-192, 704, 192, 1088, 128)
TRAIN = (256, -64, 384, 64)		# the platform at its first stop; its second 320 east
PAD = (848, 80, 944, 176)
PAD_TOP = (896, 600, 352)		# the jump's arc's top
POND = (992, 960, 1120, 1056)		# on B, at its east edge
PORTAL_X = -152

def tex (top, side, bottom = None):
	return {'top': top, 'side': side, 'bottom': bottom or side}

def island (x0, y0, x1, y1, top, holes = ()):
	"""a slab of turf, rock under it in layers, each narrower"""
	w = [box (*b, tex ('grass', 'rock')) for b in carve ((x0, y0, top - 32, x1, y1, top), holes)]
	z = top - 32
	for inset, depth in ((32, 96), (96, 96), (160, 64), (208, 48)):
		if x1 - x0 <= 2 * inset + 32 or y1 - y0 <= 2 * inset + 32:
			break
		w.append (box (x0 + inset, y0 + inset, z - depth, x1 - inset, y1 - inset, z, 'rock'))
		z -= depth
	return w

def tree (x, y, z):
	return [box (x - 12, y - 12, z, x + 12, y + 12, z + 112, 'bark'),
		box (x - 64, y - 64, z + 96, x + 64, y + 64, z + 160, 'leaves'),
		box (x - 44, y - 44, z + 160, x + 44, y + 44, z + 208, 'leaves'),
		box (x - 24, y - 24, z + 208, x + 24, y + 24, z + 232, 'leaves')]

def world ():
	w = []
	w += island (*S)
	w += island (*A)
	basin = (POND[0], POND[1], B[4] - 24, POND[2], POND[3], B[4])
	w += island (*B, holes = [basin])			# (the slab's left under the pond: its floor)
	w += island (*C)
	# the jump pad on A
	w.append (box (PAD[0], PAD[1], 0, PAD[2], PAD[3], 4, tex ('light_pad', 'marble')))
	# the bridge from B down to C
	w.append (slope ('x', C[2], B[0], 864, 928, C[4], B[4], 16, 'marble', 'marble'))
	# the ruins on C: columns, some broken; the portal's arch
	for (x, y, h) in ((-120, 760, 160), (120, 760, 96), (120, 1040, 160), (-120, 1040, 64), (0, 1040, 40)):
		w.append (box (x - 20, y - 20, C[4], x + 20, y + 20, C[4] + 12, 'marble'))
		w.append (box (x - 14, y - 14, C[4] + 12, x + 14, y + 14, C[4] + h, 'marble'))
	px = PORTAL_X
	w.append (box (px - 4, 848, C[4], px + 12, 864, C[4] + 128, 'marble'))
	w.append (box (px - 4, 928, C[4], px + 12, 944, C[4] + 128, 'marble'))
	w.append (box (px - 4, 848, C[4] + 112, px + 12, 944, C[4] + 128, 'marble'))
	# stepping stones from C back to S
	for (y, top) in ((640, 96), (512, 64), (384, 32)):
		w.append (box (-32, y - 32, top - 24, 32, y + 32, top, tex ('grass', 'rock')))
		w.append (box (-20, y - 20, top - 72, 20, y + 20, top - 24, 'rock'))
	# trees
	for (x, y, z) in ((-128, -150, 0), (150, 150, 0), (770, -120, 0), (760, 1040, B[4])):
		w += tree (x, y, z)
	# the water: the pond, and its fall over B's edge into the void
	w.append (box (POND[0], POND[1], B[4] - 24, POND[2], POND[3], B[4] - 8, '*water1'))
	w.append (box (POND[2], POND[1] + 16, -700, POND[2] + 8, POND[3] - 16, B[4] - 8, '*water1'))
	# the portal
	w.append (box (px, 864, C[4], px + 8, 928, C[4] + 112, '*portal'))
	# the sky round everything
	x0, x1, z0, z1 = SKY
	w += [box (x0 - 16, x0 - 16, z0, x0, x1 + 16, z1, 'sky_day'), box (x1, x0 - 16, z0, x1 + 16, x1 + 16, z1, 'sky_day'),
	      box (x0, x0 - 16, z0, x1, x0, z1, 'sky_day'), box (x0, x1, z0, x1, x1 + 16, z1, 'sky_day'),
	      box (x0 - 16, x0 - 16, z1, x1 + 16, x1 + 16, z1 + 16, 'sky_day'),
	      box (x0 - 16, x0 - 16, z0 - 16, x1 + 16, x1 + 16, z0, 'sky_day')]
	return w

WORLDSPAWN = {'message': 'piegpu isles', '_sunlight': '260', '_sun_mangle': '60 -55 0', '_sunlight2': '150',
	      '_sunlight3': '130', '_dirt': '1', '_minlight': '32'}

# coins: by the waypoints (engine/levels/isles_path.h); one in the jump's arc
COINS = [(64, 0, 32), (480, 0, 32), (1000, -100, 32), (896, 400, 320), (1000, 800, 288), (880, 848, 288),
	 (432, 896, 224), (0, 896, 160)]

def light (x, y, z, v):
	return '{\n"classname" "light"\n"origin" "%d %d %d"\n"light" "%d"\n}\n' % (x, y, z, v)

def entities ():
	e = ['{\n"classname" "info_player_start"\n"origin" "-200 0 24"\n"angle" "0"\n}\n']
	e += ['{\n"classname" "item_coin"\n"origin" "%d %d %d"\n}\n' % c for c in COINS]
	e += [light (896, 128, 48, 160), light (PORTAL_X + 24, 896, C[4] + 64, 220)]
	# the platform: to and fro, two seconds at each end
	x0, y0, x1, y1 = TRAIN
	e.append ('{\n"classname" "func_train"\n"target" "t1"\n"speed" "160"\n'
		  + box (x0, y0, -16, x1, y1, 0, tex ('lift', 'metal_grey')) + '}\n')
	e.append ('{\n"classname" "path_corner"\n"targetname" "t1"\n"target" "t2"\n"origin" "%d %d -16"\n"wait" "2"\n}\n' % (x0, y0))
	e.append ('{\n"classname" "path_corner"\n"targetname" "t2"\n"target" "t1"\n"origin" "%d %d -16"\n"wait" "2"\n}\n' % (x0 + 320, y0))
	# the jump pad
	e.append ('{\n"classname" "trigger_push"\n"target" "arc"\n' + box (PAD[0], PAD[1], 4, PAD[2], PAD[3], 48, 'trigger') + '}\n')
	e.append ('{\n"classname" "info_notnull"\n"targetname" "arc"\n"origin" "%d %d %d"\n}\n' % PAD_TOP)
	# the portal, back to the start
	e.append ('{\n"classname" "trigger_teleport"\n"target" "home"\n'
		  + box (PORTAL_X - 8, 864, C[4], PORTAL_X + 16, 928, C[4] + 112, 'trigger') + '}\n')
	e.append ('{\n"classname" "info_teleport_destination"\n"targetname" "home"\n"origin" "-200 0 24"\n"angle" "0"\n}\n')
	return e

def main ():
	w = world ()
	write_map (os.path.join (LEVELS, 'isles.map'), 'isles: the BSP engine\'s islands in the sky', WORLDSPAWN, w, entities ())
	print ('isles.map: %d world brushes, %d coins' % (len (w), len (COINS)))

if __name__ == '__main__':
	main ()
