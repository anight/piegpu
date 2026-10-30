#!/usr/bin/env python3
"""make_docks.py - Lanternfall's first scene, the Ferry Docks: a harbour of
rooftops in the fog. Wren lands at a jetty, goes up a plank onto the roofs,
crosses a gap by a way her lantern shows, drops into a boathouse where Ody
sleeps, has him break its wall, meets the first of the Hollow in the yard
beyond, is lifted onto the harbour tower to light its beacon, and leaves by
the gate that opens to the next ferry.
"""
from lfkit import *

X0, Y0, X1, Y1, TOP = -640, -896, 2688, 1792, 832

def world ():
	w = []
	w += sea (X0, Y0, X1, Y1)
	w += skybox (X0, Y0, BED - 32, X1, Y1, TOP)

	# the jetty and the ferry at its end
	w.append (box (-64, -640, 8, 64, -128, 24, 'wood'))
	for y in range (-608, -128, 160):
		for x in (-56, 40):
			w.append (box (x, y, BED, x + 16, y + 16, 56, 'bark'))
	w += [box (-176, -640, -8, -80, -448, 20, 'wood'), box (-160, -620, 20, -96, -468, 28, 'wood'),
	      box (-136, -560, 28, -120, -544, 132, 'bark'), box (-176, -470, 20, -80, -448, 40, 'wood')]
	# the quay, a plank up to the first roof
	w.append (box (-256, -128, BED, 512, 256, 32, tex ('stone', 'stone')))
	w += [box (-200, 120, 32, -136, 184, 96, 'wood'), box (-190, 130, 96, -146, 174, 140, 'wood')]	# crates
	w.append (slope ('x', 512, 768, 0, 128, 32, 144, 12, 'wood', 'wood'))
	# the first house: its roof the road
	w.append (box (768, -128, BED, 1216, 384, 144, tex ('slate', 'plaster')))
	w += [box (816, 240, 144, 912, 336, 232, tex ('slate', 'plaster')), box (840, 264, 232, 888, 312, 280, 'stone')]
	w.append (box (814, 264, 168, 816, 312, 216, 'light_lamp'))				# a lit window
	# the boathouse: a floor, walls (a doorway north, to be broken through), a roof with a hole fallen in
	w.append (box (1536, -192, BED, 2176, 448, 16, 'stone'))
	w += boxes (carve ((1536, 416, 16, 2176, 448, 128), [(1760, 416, 16, 1888, 448, 120)]), 'plaster')
	w += [box (1536, -192, 16, 2176, -160, 128, 'plaster'), box (1536, -160, 16, 1568, 416, 128, 'plaster'),
	      box (2144, -160, 16, 2176, 416, 128, 'plaster')]
	w += boxes (carve ((1536, -192, 128, 2176, 448, 144), [(1920, 64, 128, 2080, 256, 144)]), tex ('slate', 'wood'))
	w += [box (1936, 0, 16, 2064, 56, 40, 'wood'), box (1600, -120, 16, 1700, -60, 60, 'wood')]	# rubble, a bench
	# the yard beyond, walled; the tower in its north wall; the gate west, the pier beyond it
	w.append (box (1376, 448, BED, 2464, 1312, 16, tex ('stone', 'stone')))
	w += [box (1376, 448, 16, 1536, 480, 176, 'plaster'), box (2176, 448, 16, 2464, 480, 176, 'plaster'),
	      box (2432, 480, 16, 2464, 1312, 176, 'plaster'),
	      box (1376, 1280, 16, 2112, 1312, 176, 'plaster'), box (2368, 1280, 16, 2464, 1312, 176, 'plaster')]
	w += boxes (carve ((1376, 480, 16, 1408, 1280, 176), [(1376, 800, 16, 1408, 928, 112)]), 'plaster')
	w.append (box (2112, 1280, BED, 2368, 1536, 128, tex ('stone', 'stone')))		# the tower: its top the beacon's
	w += [box (2112, 1504, 128, 2368, 1536, 176, 'stone'), box (2112, 1312, 128, 2128, 1504, 152, 'stone'),
	      box (2352, 1312, 128, 2368, 1504, 152, 'stone')]
	for (x, y) in ((1700, 900), (1764, 900), (1732, 900), (2300, 640), (1500, 1180)):		# crates
		w.append (box (x, y, 16, x + 64, y + 64, 80, 'wood'))
	w.append (box (1732, 900, 80, 1796, 964, 144, 'wood'))
	w += [box (2000, 1180, 16, 2032, 1212, 220, 'bark'), box (1900, 1190, 204, 2130, 1202, 216, 'bark')]	# a crane
	w.append (box (896, 800, BED, 1376, 928, 16, 'wood'))					# the pier
	w += [box (912, 944, -8, 1104, 1040, 20, 'wood'), box (928, 960, 20, 1088, 1024, 28, 'wood'),
	      box (1000, 984, 28, 1016, 1000, 140, 'bark')]					# the next ferry
	# what leans out of the sea, far off: a bell tower, roofs
	w += [box (-400, 900, BED, -272, 1028, 420, 'plaster'), box (-416, 884, 420, -256, 1044, 452, 'slate'),
	      box (-384, 916, 452, -288, 1012, 520, 'slate'), box (-352, 948, 520, -320, 980, 600, 'brass')]
	w += [box (300, 1300, BED, 620, 1560, 60, tex ('slate', 'plaster')), box (-300, 400, BED, -120, 620, 40, tex ('roof_rust', 'plaster')),
	      box (1000, 1300, BED, 1250, 1500, 90, tex ('slate', 'plaster'))]
	return w

WORLDSPAWN = {'message': 'Lanternfall: the Ferry Docks', '_sunlight': '70', '_sun_mangle': '40 -50 0', '_sunlight2': '30',
	      '_dirt': '1', '_minlight': '14'}

def entities ():
	e = [ent ('info_player_start', (0, -560, 48), angle = 90)]
	e += [light (-128, -520, 80, 180), light (800, 288, 200, 170), light (1700, 560, 72, 200)]
	e.append (ent ('lf_hidden', brushes = [box (1216, 64, 128, 1536, 192, 144, 'path')], radius = 320))
	e.append (ent ('lf_ody', (1700, 128, 40), angle = 0, asleep = 1))
	e.append (ent ('lf_break', brushes = [box (1760, 416, 16, 1888, 448, 120, 'cracked')]))
	e.append (ent ('lf_shrine', (1700, 560, 16)))
	e += [ent ('item_ember', (1000, 300, 168)), ent ('item_ember', (2200, 700, 40)), ent ('item_ember', (1600, 1100, 40))]
	e += [ent ('lf_hollow', (2100, 800, 40), kind = 0, wake = 520), ent ('lf_hollow', (1900, 1100, 40), kind = 0, wake = 520),
	      ent ('lf_hollow', (1560, 760, 40), kind = 0, wake = 420)]
	e.append (ent ('lf_lift', (2240, 1190, 40), target = 'lift1'))
	e.append (ent ('info_notnull', (2240, 1370, 300), targetname = 'lift1'))
	e.append (ent ('lf_beacon', (2240, 1440, 128)))
	e.append (ent ('lf_gate', brushes = [box (1376, 800, 16, 1408, 928, 112, 'iron')], key = 'beacon'))
	e.append (ent ('lf_exit', (980, 864, 40), radius = 90))
	e += [ent ('lf_story', (0, -400, 48), radius = 150, line = 1), ent ('lf_story', (1150, 128, 168), radius = 110, line = 2),
	      ent ('lf_story', (1900, 128, 40), radius = 200, line = 3), ent ('lf_story', (1824, 330, 40), radius = 110, line = 4),
	      ent ('lf_story', (2240, 1140, 40), radius = 130, line = 5), ent ('lf_story', (1300, 864, 40), radius = 110, line = 6)]
	b = Bot ()
	b.go (0, -160, 48).go (256, 64, 56).go (500, 64, 56).go (800, 64, 168).go (1170, 128, 168, 'raise')
	b.go (1560, 128, 168).go (1880, 160, 168).go (2000, 160, 40).go (1780, 128, 40, 'raise').go (1824, 360, 40, 'ody')
	b.go (1824, 560, 40).go (2000, 880, 40, 'fight').go (2150, 1140, 40, 'ody')
	b.go (2240, 1400, 152, 'raise').go (2240, 1200, 40).go (1460, 864, 40).go (980, 864, 40)
	return e + b.entities ()

if __name__ == '__main__':
	write_map ('docks', 'Lanternfall: the Ferry Docks', WORLDSPAWN, world (), entities ())
