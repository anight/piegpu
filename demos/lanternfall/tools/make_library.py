#!/usr/bin/env python3
"""make_library.py - Lanternfall's second scene, the Drowned Library: all
indoors. An entrance hall; the reading hall, its floor under a hand of water,
the Archivist on her dais; shelves for stairs up to the gallery, whose fallen
stretch the lantern bridges; the stacks beyond, dark, with the Hollow in the
aisles and a door that wants Ody's weight on a plate; the dome, open to the
sky, with the beacon under it; and the way out to the upper canal.
"""
from lfkit import *

G = 192					# the gallery, and the floors beyond

def world ():
	w = []
	# the entrance hall, the doorway north into the reading hall
	w += shell (-192, -512, 16, 192, -32, 208, 'plaster', holes = [(-96, -32, 16, 96, 0, 176)], floor = 'stone', ceiling = 'wood')
	# the reading hall: flooded; its doorways: south (from the entrance), west at the gallery (to the stacks)
	w += shell (-512, 0, 0, 512, 1024, 400, 'plaster', holes = [(-96, -32, 16, 96, 0, 176), (-544, 896, G, -512, 1024, G + 144)],
		    floor = 'stone', ceiling = 'wood')
	w.append (box (-512, 0, 0, 512, 1024, 12, '*sea'))
	w.append (box (-96, 0, 0, 96, 64, 16, 'stone'))				# a step down from the door
	w.append (box (-480, 736, 0, -224, 992, 16, tex ('wood', 'stone')))		# the Archivist's dais
	w += [box (-470, 940, 16, -400, 980, 60, 'wood'), box (-300, 930, 16, -250, 980, 44, 'books')]
	# shelves along the walls, and the shelves that are stairs, up the east wall to the gallery
	for (x0, y0, x1, y1) in ((-512, 80, -464, 640), (-380, 0, -140, 48), (140, 0, 320, 48), (-96, 440, 96, 500), (-96, 640, 96, 700)):
		w.append (box (x0, y0, 0, x1, y1, 150, tex ('wood', 'books')))
	w += stairs ('y', 200, 584, 384, 512, 0, G, tex ('wood', 'books'))
	w.append (box (384, 584, G - 16, 512, 1024, G, 'wood'))				# the gallery: east,
	w += boxes (carve ((-512, 896, G - 16, 384, 1024, G), [(-64, 896, G - 16, 192, 1024, G)]), 'wood')	# north, but for where it fell
	for (x, y) in ((384, 700), (384, 890), (200, 896), (-72, 896), (-300, 896)):
		w.append (box (x, y, 0, x + 16, y + 16, G - 16, 'wood'))			# its posts
	# the stacks: aisles between shelves; an alcove with the plate; the gated door north to the dome
	w += shell (-1280, 384, G, -544, 1024, 400, 'plaster', holes = [(-544, 896, G, -512, 1024, G + 144), (-1000, 1024, G, -872, 1056, G + 128)],
		    floor = 'wood', ceiling = 'wood')
	for y in (560, 720):
		w += [box (-1180, y, G, -940, y + 32, G + 160, tex ('wood', 'books')), box (-840, y, G, -640, y + 32, G + 160, tex ('wood', 'books'))]
	w.append (box (-1228, 412, G, -1132, 508, G + 2, tex ('rune', 'stone')))
	# the dome: its roof open to the sky over the beacon; the gated door east to the canal
	w += shell (-1280, 1056, G, -736, 1600, 480, 'stone', holes = [(-1000, 1024, G, -872, 1056, G + 128), (-736, 1264, G, -704, 1392, G + 128),
		    (-1136, 1200, 480, -880, 1456, 512)], floor = 'stone')
	w.append (box (-1136, 1200, 496, -880, 1456, 512, 'sky_storm'))
	w.append (box (-1072, 1264, G, -944, 1392, G + 8, 'stone'))			# the beacon's step
	# the upper canal: a landing, a boat
	w += shell (-704, 1232, G, -448, 1424, G + 160, 'stone', holes = [(-736, 1264, G, -704, 1392, G + 128)], ceiling = 'wood')
	w += [box (-520, 1260, G, -456, 1396, G + 14, 'wood'), box (-500, 1320, G + 14, -488, 1332, G + 110, 'bark')]
	return w

WORLDSPAWN = dict (NIGHT, message = 'Lanternfall: the Drowned Library', _minlight = '10')

def entities ():
	e = [ent ('info_player_start', (0, -420, 40), angle = 90), ent ('lf_ody', (-110, -430, 40), angle = 90)]
	e += [light (0, -260, 150, 170), light (-360, 860, 120, 230), light (430, 800, G + 110, 120), light (-600, 960, G + 110, 150),
	      light (-576, 1328, G + 110, 160)]
	e.append (ent ('lf_archivist', (-400, 880, 40), angle = -40))
	e.append (ent ('lf_hidden', brushes = [box (-64, 896, G - 16, 192, 1024, G, 'path')], radius = 330))
	e.append (ent ('lf_shrine', (-620, 820, G)))
	e += [ent ('item_ember', (448, 820, G + 24)), ent ('item_ember', (-1230, 980, G + 24)), ent ('item_ember', (300, 100, 24))]
	e += [ent ('lf_hollow', (-800, 640, G + 24), kind = 0, wake = 330), ent ('lf_hollow', (-1100, 860, G + 24), kind = 0, wake = 330),
	      ent ('lf_hollow', (-1060, 640, G + 24), kind = 0, wake = 300)]
	e.append (ent ('lf_plate', (-1180, 460, G + 24), id = 1))
	e.append (ent ('lf_gate', brushes = [box (-1000, 1024, G, -872, 1056, G + 128, 'iron')], key = 'plate', id = 1))
	e.append (ent ('lf_beacon', (-1008, 1328, G + 8)))
	e.append (ent ('lf_gate', brushes = [box (-736, 1264, G, -704, 1392, G + 128, 'iron')], key = 'beacon'))
	e.append (ent ('lf_exit', (-600, 1328, G + 24), radius = 70))
	e += [ent ('lf_story', (-300, 780, 24), radius = 190, line = 1), ent ('lf_story', (448, 180, 24), radius = 120, line = 3),
	      ent ('lf_story', (-936, 940, G + 24), radius = 120, line = 2), ent ('lf_story', (-600, 960, G + 24), radius = 100, line = 4)]
	b = Bot ()
	b.go (0, -100, 40).go (0, 120, 24).go (-250, 700, 24).go (200, 400, 24).go (448, 170, 24).go (448, 600, G + 24)
	b.go (448, 960, G + 24).go (230, 960, G + 24, 'raise').go (-120, 960, G + 24).go (-600, 960, G + 24).go (-620, 880, G + 24)
	b.go (-890, 660, G + 24, 'fight').go (-890, 490, G + 24).go (-1080, 490, G + 24, 'ody').go (-890, 490, G + 24).go (-936, 980, G + 24)
	b.go (-936, 1150, G + 24).go (-1008, 1230, G + 32, 'raise').go (-800, 1328, G + 24).go (-600, 1328, G + 24)
	return e + b.entities ()

if __name__ == '__main__':
	write_map ('library', 'Lanternfall: the Drowned Library', WORLDSPAWN, world (), entities ())
