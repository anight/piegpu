#!/usr/bin/env python3
"""lfkit.py - what Lanternfall's levels are made with (demos/lanternfall/tools/
make_*.py), on the BSP engine's kit (engine/tools/levelkit.py: its noise, its
palette's k-means, its WAD and its brushes): the game's own textures, painted
here in few colours (deep teal, rust, bone white; gold is the light's, not
the paint's), their palette, and the entities the game knows.

  demos/lanternfall/tools/lfkit.py     writes levels/lanternfall.wad and palette.c;
                                       then build.sh makes and compiles the levels

The look: every texture is a few flat tones laid in strokes (noise stretched
along a direction, then cut into steps), so walls read as painted, not as
photographs of stone.
"""
import numpy as np, os, sys

HERE = os.path.dirname (os.path.abspath (__file__))
sys.path.insert (0, os.path.join (HERE, '..', '..', '..', 'engine', 'tools'))
import levelkit as lk
from levelkit import box, slope, carve, brush, ramp_x, plane_face, noise, fbm, tint

LEVELS = os.path.join (HERE, '..', 'levels')
TEX = 64

TEAL = np.array ((0.10, 0.30, 0.33))
DEEP = np.array ((0.04, 0.13, 0.16))
RUST = np.array ((0.55, 0.25, 0.12))
BONE = np.array ((0.86, 0.82, 0.72))
SLATE = np.array ((0.20, 0.30, 0.34))
MOSS = np.array ((0.18, 0.32, 0.22))
GOLD = np.array ((1.00, 0.78, 0.36))

# ---- paint ---------------------------------------------------------------------------

def strokes (seed, along = 'x', stretch = 4, tones = 5, size = TEX):
	"""brush strokes, 0 .. 1 in `tones` steps: noise stretched along a direction"""
	a = fbm (size * stretch, seed, 4, 4)[::stretch, :size] if along == 'x' else fbm (size * stretch, seed, 4, 4)[:size, ::stretch]
	a = (a - a.min ()) / (a.max () - a.min ())
	return np.floor (a * tones * 0.999) / (tones - 1)

def paint (seed, dark, light, along = 'x', tones = 5, stretch = 4, size = TEX):
	s = strokes (seed, along, stretch, tones, size)[..., None]
	return np.array (dark)[None, None, :] * (1 - s) + np.array (light)[None, None, :] * s

def lines (img, ys = (), xs = (), k = 0.6, w = 1):
	for y in ys:
		img[y:y + w, :] *= k
	for x in xs:
		img[:, x:x + w] *= k
	return img

def slate ():
	"""roof slates: rows, each slate a tone of its own"""
	img = paint (101, SLATE * 0.55, SLATE * 1.25, 'x', 4)
	r = np.random.default_rng (1)
	for row in range (4):
		y0 = row * 16; off = 8 if row % 2 else 0
		for col in range (4):
			cols = [(col * 16 + off + k) % TEX for k in range (16)]
			img[y0:y0 + 16, cols] *= 0.8 + 0.4 * r.random ()
			img[y0:y0 + 16, cols[0]] *= 0.55
		img[y0:y0 + 2, :] *= 0.5
	return img

def roof_rust ():
	img = paint (102, RUST * 0.45, RUST * 1.2, 'y', 5)
	lines (img, xs = range (0, TEX, 8), k = 0.7)
	stain = strokes (103, 'y', 6, 3)[..., None]
	return img * (1 - 0.35 * stain) + TEAL * 0.35 * stain

def plaster ():
	img = paint (104, BONE * 0.62, BONE, 'y', 4, 3)
	stain = strokes (105, 'y', 8, 4)[..., None]
	return img * (1 - 0.3 * stain) + SLATE * 0.3 * stain

def stone ():
	"""big blocks, teal-grey"""
	img = paint (106, SLATE * 0.6, SLATE * 1.35, 'x', 4, 3)
	r = np.random.default_rng (2)
	for row in range (2):
		y0 = row * 32; off = 20 if row else 0
		for x0, w in ((0, 40), (40, 24)):
			cols = [(x0 + off + k) % TEX for k in range (w)]
			img[y0:y0 + 32, cols] *= 0.85 + 0.3 * r.random ()
			img[y0:y0 + 32, cols[0]:cols[0] + 2] *= 0.5
		img[y0:y0 + 2, :] *= 0.5
	return img

def wood ():
	img = paint (107, RUST * 0.30, RUST * 0.80, 'x', 5, 8)
	return lines (img, ys = range (0, TEX, 16), k = 0.5, w = 2)

def moss ():
	return paint (108, MOSS * 0.5, MOSS * 1.4, 'x', 5, 2)

def books ():
	"""a shelf: two rows of spines"""
	img = np.zeros ((TEX, TEX, 3))
	r = np.random.default_rng (3)
	cols = (RUST, RUST * 0.6, TEAL * 1.4, DEEP * 2.2, BONE * 0.8, SLATE)
	for row in range (2):
		y0 = row * 32; x = 0
		while x < TEX:
			w = int (r.integers (3, 8)); h = int (r.integers (20, 27))
			c = cols[int (r.integers (len (cols)))] * (0.8 + 0.4 * r.random ())
			img[y0 + 28 - h:y0 + 28, x:x + w] = c
			img[y0 + 28 - h:y0 + 28, x] *= 0.6
			if r.random () < 0.4:
				img[y0 + 30 - h:y0 + 32 - h, x + 1:x + w] = BONE * 0.9
			x += w
		img[y0 + 28:y0 + 32, :] = RUST * 0.35
	return img

def bark ():
	img = paint (109, DEEP * 1.2, RUST * 0.45 + TEAL * 0.3, 'y', 5, 8)
	return img

def leaves ():
	return paint (110, DEEP * 1.1, MOSS * 1.1, 'x', 4, 2)

def glass ():
	"""a greenhouse's panes: teal glass in rusted frames"""
	img = paint (111, TEAL * 0.45, TEAL * 1.0 + BONE * 0.08, 'y', 4, 3)
	for k in (0, 32):
		img[k:k + 3, :] = RUST * 0.5; img[:, k:k + 3] = RUST * 0.5
	return img

def iron ():
	img = paint (112, RUST * 0.25 + DEEP, RUST * 0.75, 'y', 5, 5)
	return img

def statue ():
	"""pale carved stone: folds running down"""
	img = paint (113, BONE * 0.45, BONE * 0.95, 'y', 5, 8)
	return lines (img, ys = (0, 32), k = 0.7, w = 2)

def brass ():
	return paint (114, RUST * 0.5, RUST * 0.6 + BONE * 0.45, 'y', 5, 4)

def cracked ():
	"""a wall to be broken: plaster, cracked from the middle"""
	img = plaster () * 0.92
	y, x = np.mgrid[0:TEX, 0:TEX]
	for a, b, c in ((1.0, 0.35, 0), (-0.6, 1.0, 20), (0.25, -1.0, 44), (1.0, -0.9, 5)):
		d = np.abs (a * (x - 32) + b * (y - 32) + c * 0.0) / np.hypot (a, b)
		along = np.hypot (x - 32, y - 32)
		img[(d < 1.2) & (along < 30)] = DEEP
	return img

def rune ():
	"""a plate for Ody: stone with a ring and a hand"""
	img = stone () * 0.9
	y, x = np.mgrid[0:TEX, 0:TEX] - 31.5
	r = np.hypot (x, y)
	img[(r > 22) & (r < 26)] = RUST * 0.9
	img[(np.abs (x) < 9) & (y > -6) & (y < 12)] = RUST * 0.9
	for k in (-8, -3, 2, 7):
		img[(np.abs (x - k) < 1.6) & (y > -16) & (y <= -6)] = RUST * 0.9
	return img

def path ():
	"""a hidden way, shown by the lantern: pale stone, gold-edged"""
	img = paint (115, BONE * 0.5, BONE * 0.8, 'x', 4, 3)
	img[:3, :] = GOLD * 0.7; img[-3:, :] = GOLD * 0.7; img[:, :3] = GOLD * 0.7; img[:, -3:] = GOLD * 0.7
	return img

def lamp ():
	"""a lit window, a lamp (light*)"""
	y, x = np.mgrid[0:TEX, 0:TEX]
	d = np.minimum (np.minimum (x, TEX - 1 - x), np.minimum (y, TEX - 1 - y))
	img = tint (np.clip (0.6 + d / 24.0, 0, 1), GOLD)
	img[d < 3] = RUST * 0.35
	img[:, 30:34] = RUST * 0.35
	return img

def cloth ():
	img = paint (116, RUST * 0.5, RUST * 1.15, 'y', 4, 6)
	return lines (img, ys = (0, 61), k = 0.6, w = 3)

def sea_paint ():
	return paint (117, DEEP * 0.8, TEAL * 0.9, 'x', 5, 4)

def sky ():
	"""Quake's sky, small (128 x 64: it's all soft): thin dark clouds in front
	   (colour 0: clear), a deep overcast behind"""
	h = 64
	n = fbm (h, 118, 4, 2)
	back = DEEP[None, None, :] * (0.7 + 1.6 * n[..., None]) + TEAL[None, None, :] * 0.25 * n[..., None]
	m = fbm (h, 119, 4, 4)
	front = np.zeros ((h, h, 3))
	cloud = m > 0.58
	front[cloud] = np.maximum (DEEP * 0.9 + 0.5 * (m[cloud, None] - 0.58), 0.02)
	return np.concatenate ([front, back], 1)

def trigger ():
	return np.full ((16, 16, 3), (0.8, 0.5, 0.2))

TEXTURES = {
	'slate': slate, 'roof_rust': roof_rust, 'plaster': plaster, 'stone': stone, 'wood': wood, 'moss': moss,
	'books': books, 'bark': bark, 'leaves': leaves, 'glass': glass, 'iron': iron, 'statue': statue, 'brass': brass,
	'cracked': cracked, 'rune': rune, 'path': path, 'light_lamp': lamp, 'cloth': cloth, '*sea': sea_paint,
	'sky_storm': sky, 'trigger': trigger,
}

def write_palette_c (path, pal):
	with open (path, 'w') as f:
		f.write ('/*\n * palette.c - the palette Lanternfall\'s levels\' textures are in (256 RGB),\n'
			 ' * made with them by demos/lanternfall/tools/lfkit.py (don\'t edit)\n */\n'
			 '#include "bsp.h"\n\nconst uint8_t bsp_palette[256][3] =\n{\n')
		for i in range (0, 256, 4):
			f.write ('\t' + ' '.join ('{%3d, %3d, %3d},' % tuple (pal[j]) for j in range (i, i + 4)) + '\n')
		f.write ('};\n')

# ---- the map: brushes and the game's entities ------------------------------------------------

def tex (top, side, bottom = None):
	return {'top': top, 'side': side, 'bottom': bottom or side}

def boxes (bs, t):
	return [box (*b, t) for b in bs]

def ent (classname, origin = None, brushes = None, **keys):
	"""an entity: a point (origin) or brushes; its other keys as given (a
	   trailing _ dropped: class_, from_)"""
	s = '{\n"classname" "%s"\n' % classname
	if origin is not None:
		s += '"origin" "%d %d %d"\n' % tuple (origin)
	for k, v in keys.items ():
		s += '"%s" "%s"\n' % (k.rstrip ('_'), v)
	return s + ''.join (brushes or []) + '}\n'

def light (x, y, z, v, **keys):
	return ent ('light', (x, y, z), light = v, **keys)

def skybox (x0, y0, z0, x1, y1, z1, t = 'sky_storm', w = 16):
	"""the sky round everything (and under it: the sea's bed is inside)"""
	return [box (x0 - w, y0 - w, z0, x0, y1 + w, z1, t), box (x1, y0 - w, z0, x1 + w, y1 + w, z1, t),
		box (x0, y0 - w, z0, x1, y0, z1, t), box (x0, y1, z0, x1, y1 + w, z1, t),
		box (x0 - w, y0 - w, z1, x1 + w, y1 + w, z1 + w, t)]

SEA = 0				# the sea's top, in every level
BED = -40				# its bed: deep enough to be over Wren's knees (the game: the sea takes her)

def sea (x0, y0, x1, y1):
	"""the sea over the whole level, and its bed (never seen under the water: not drawn, not lit)"""
	return [box (x0, y0, BED - 32, x1, y1, BED, 'skip'), box (x0, y0, BED, x1, y1, SEA, '*sea')]

def shell (x0, y0, z0, x1, y1, z1, t, wall = 32, holes = (), floor = None, ceiling = None):
	"""a room: the space x0 .. z1, walls round it (wall thick) but for the holes (boxes cut out
	   of them: doorways, a skylight); t: the walls' texture; floor, ceiling: theirs"""
	slabs = [((x0 - wall, y0 - wall, z0 - wall, x1 + wall, y1 + wall, z0), floor or t),
		 ((x0 - wall, y0 - wall, z1, x1 + wall, y1 + wall, z1 + wall), ceiling or t),
		 ((x0 - wall, y0 - wall, z0, x0, y1 + wall, z1), t), ((x1, y0 - wall, z0, x1 + wall, y1 + wall, z1), t),
		 ((x0, y0 - wall, z0, x1, y0, z1), t), ((x0, y1, z0, x1, y1 + wall, z1), t)]
	out = []
	for b, tx in slabs:
		out += boxes (carve (b, holes), tx)
	return out

def stairs (axis, a0, a1, c0, c1, z0, z1, t, solid = True):
	"""steps of 16 from z0 at a0 up to z1 at a1 along axis ('x' or 'y'; a1 may be less than a0),
	   c0 .. c1 across; solid down to z0, or slabs"""
	n = int (round ((z1 - z0) / 16))
	out = []
	for k in range (n):
		p, q = a0 + (a1 - a0) * k / n, a0 + (a1 - a0) * (k + 1) / n
		lo, hi = min (p, q), max (p, q)
		bottom = z0 if solid else z0 + 16 * k
		out.append (box (lo, c0, bottom, hi, c1, z0 + 16 * (k + 1), t) if axis == 'x' else box (c0, lo, bottom, c1, hi, z0 + 16 * (k + 1), t))
	return out

NIGHT = {'_sunlight': '70', '_sun_mangle': '40 -50 0', '_sunlight2': '30', '_dirt': '1', '_minlight': '14'}

class Bot:
	"""the way the game plays itself through a level (lf_bot entities, in
	   order): at (x, y, z) Wren's origin (24 over the floor), then what to do there"""
	def __init__ (self):
		self.steps = []
	def go (self, x, y, z, do = ''):
		self.steps.append ((x, y, z, do))
		return self
	def entities (self):
		return [ent ('lf_bot', (x, y, z), n = i, do = do) for i, (x, y, z, do) in enumerate (self.steps)]

def write_map (name, title, worldspawn, world, entities):
	path = os.path.join (LEVELS, name + '.map')
	with open (path, 'w') as f:
		f.write ('// %s (demos/lanternfall/tools/make_%s.py: don\'t edit)\n' % (title, name))
		f.write ('{\n"classname" "worldspawn"\n"wad" "lanternfall.wad"\n')
		f.write (''.join ('"%s" "%s"\n' % kv for kv in worldspawn.items ()))
		f.write (''.join (world))
		f.write ('}\n')
		f.write (''.join (entities))
	print ('%s.map: %d world brushes, %d entities' % (name, len (world), len (entities)))

def main ():
	os.makedirs (LEVELS, exist_ok = True)
	images = {name: f () for name, f in TEXTURES.items ()}
	pal = lk.make_palette (list (images.values ()))
	lk.write_wad (os.path.join (LEVELS, 'lanternfall.wad'), images, pal)
	write_palette_c (os.path.join (HERE, '..', 'palette.c'), pal)
	print ('lanternfall.wad: %d textures; palette.c' % len (images))
	if '--sheet' in sys.argv:			# the textures side by side, to look at
		from PIL import Image
		names = [n for n in images if images[n].shape[0] == TEX]
		sheet = np.zeros ((TEX * 2, TEX * ((len (names) + 1) // 2), 3))
		for i, n in enumerate (names):
			sheet[(i % 2) * TEX:(i % 2 + 1) * TEX, (i // 2) * TEX:(i // 2 + 1) * TEX] = np.clip (images[n], 0, 1)
		Image.fromarray ((sheet * 255).astype (np.uint8)).resize ((sheet.shape[1] * 3, sheet.shape[0] * 3), Image.NEAREST).save (
			sys.argv[sys.argv.index ('--sheet') + 1])

if __name__ == '__main__':
	main ()
