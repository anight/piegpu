#!/usr/bin/env python3
"""make_base.py - the demo level "base" for the BSP engine (engine/), as a
Quake .map, its textures (a WAD2) and the palette they're in, all made here.

  engine/tools/make_base.py        writes engine/levels/base.map, textures.wad,
                                   engine/palette.c; then compile it:
  engine/tools/build-level.sh base (ericw-tools: qbsp, vis, light)

The level is rooms (empty boxes) on a 64-unit grid: every cell that isn't in
a room is solid, and the solid cells merge into box brushes, so the level is
sealed whatever the rooms are. Details (stairs, a ramp, pillars, railings,
light fixtures) are brushes of their own; a door (func_door) and a lift
(func_plat) are brush entities. Quake units: a player is 32 wide, 56 tall,
climbs steps of 18.

The textures are drawn here (numpy), in RGB, then quantized together to a
palette of 256 made from them (k-means): Quake's BSP stores 8-bit textures,
and the engine converts them with the same palette (engine/palette.c).
"""
import numpy as np, os, struct, sys

HERE = os.path.dirname (os.path.abspath (__file__))
LEVELS = os.path.join (HERE, '..', 'levels')
CELL = 64
TEX = 64					# texture size

rng = np.random.default_rng (12345)

# ---- noise and drawing -------------------------------------------------------------

def noise (size, cells, seed):
	"""tileable value noise, size x size, `cells` lattice cells across"""
	r = np.random.default_rng (seed).random ((cells, cells))
	c = np.arange (size) * cells / size
	i = np.floor (c).astype (int); f = c - i; f = f * f * (3 - 2 * f)
	i1 = (i + 1) % cells
	a = r[np.ix_ (i, i)]; b = r[np.ix_ (i, i1)]; cc = r[np.ix_ (i1, i)]; d = r[np.ix_ (i1, i1)]
	fx = f[None, :]; fy = f[:, None]
	return (a * (1 - fx) + b * fx) * (1 - fy) + (cc * (1 - fx) + d * fx) * fy

def fbm (size, seed, octaves = 4, base = 4):
	t = np.zeros ((size, size)); amp = 1.0; tot = 0
	for o in range (octaves):
		t += amp * noise (size, base << o, seed + o); tot += amp; amp *= 0.5
	return t / tot

def tint (gray, color):
	return gray[..., None] * np.array (color)[None, None, :]

def bevel (img, x0, y0, x1, y1, light = 1.25, dark = 0.7, w = 1):
	"""a raised panel's edges: lit top and left, dark bottom and right"""
	img[y0:y0 + w, x0:x1] *= light; img[y0:y1, x0:x0 + w] *= light
	img[y1 - w:y1, x0:x1] *= dark; img[y0:y1, x1 - w:x1] *= dark

# ---- the textures ----------------------------------------------------------------------

def concrete ():
	n = fbm (TEX, 1) * 0.35 + 0.65 + (rng.random ((TEX, TEX)) - 0.5) * 0.06
	img = tint (n, (0.55, 0.54, 0.52))
	for y in (0, 32):
		img[y, :] *= 0.75				# form lines
	return img

def floor_tiles ():
	n = fbm (TEX, 2) * 0.2 + 0.8
	img = tint (n, (0.50, 0.48, 0.44))
	for ty in range (2):
		for tx in range (2):
			v = 0.9 + 0.2 * rng.random ()
			img[ty * 32:(ty + 1) * 32, tx * 32:(tx + 1) * 32] *= v
			bevel (img, tx * 32, ty * 32, tx * 32 + 32, ty * 32 + 32, 1.15, 0.6)
	return img

def floor_plate ():
	"""diamond plate: raised diagonal studs"""
	n = fbm (TEX, 3) * 0.15 + 0.85
	img = tint (n, (0.42, 0.44, 0.46))
	y, x = np.mgrid[0:TEX, 0:TEX]
	for k in (0, 1):
		u = ((x + (y // 8 % 2) * 8 + k) % 16) - 8; v = (y % 8) - 4
		stud = (abs (u - v) < 2) & (abs (u + v) < 5)
		img[stud] *= 1.35 if k == 0 else 0.9
	return img

def metal_panels (color):
	n = fbm (TEX, 4) * 0.2 + 0.8
	img = tint (n, color)
	for y0, y1 in ((0, 24), (24, 64)):
		for x0, x1 in ((0, 32), (32, 64)):
			bevel (img, x0, y0, x1, y1, 1.3, 0.55, 2)
			for rx, ry in ((x0 + 4, y0 + 4), (x1 - 5, y0 + 4), (x0 + 4, y1 - 5), (x1 - 5, y1 - 5)):
				img[ry:ry + 2, rx:rx + 2] *= 1.5	# rivets
	return img

def bricks ():
	n = fbm (TEX, 5) * 0.25 + 0.75
	img = tint (n, (0.58, 0.30, 0.20))
	for row in range (8):
		y0 = row * 8; off = 8 if row % 2 else 0
		for col in range (5):
			x0 = (col * 16 + off) % TEX
			v = 0.8 + 0.35 * rng.random ()
			for xx in range (16):
				img[y0:y0 + 8, (x0 + xx) % TEX] *= v
		img[y0, :] = (0.30, 0.28, 0.25)		# mortar
		for col in range (4):
			img[y0:y0 + 8, (col * 16 + off) % TEX] = (0.30, 0.28, 0.25)
	return img

def stone ():
	n = fbm (TEX, 6, 5, 2) * 0.5 + 0.5
	img = tint (n, (0.46, 0.45, 0.40))
	return img

def ceiling_panels ():
	n = fbm (TEX, 7) * 0.1 + 0.9
	img = tint (n, (0.62, 0.62, 0.60))
	for k in (0, 32):
		img[k:k + 2, :] *= 0.5; img[:, k:k + 2] *= 0.5
	y, x = np.mgrid[0:TEX, 0:TEX]
	img[((x % 4) == 2) & ((y % 4) == 2)] *= 0.8	# perforations
	return img

def ceiling_grid ():
	img = tint (fbm (TEX, 8) * 0.1 + 0.9, (0.30, 0.32, 0.36))
	for k in range (0, TEX, 16):
		img[k:k + 3, :] = (0.55, 0.56, 0.58); img[:, k:k + 3] = (0.55, 0.56, 0.58)
	return img

def wood ():
	y, x = np.mgrid[0:TEX, 0:TEX]
	grain = np.sin ((x + fbm (TEX, 9) * 24) * 0.9) * 0.1 + 0.9
	img = tint (grain * (fbm (TEX, 10) * 0.2 + 0.8), (0.50, 0.34, 0.20))
	for k in range (0, TEX, 16):
		img[k, :] *= 0.5				# planks
	return img

def hazard ():
	y, x = np.mgrid[0:TEX, 0:TEX]
	stripe = ((x + y) // 8) % 2 == 0
	img = np.where (stripe[..., None], np.array ((0.95, 0.75, 0.08)), np.array ((0.08, 0.08, 0.09)))
	img = img * (fbm (TEX, 11) * 0.2 + 0.8)[..., None]
	img[:4, :] = (0.5, 0.5, 0.52); img[-4:, :] = (0.5, 0.5, 0.52)
	return img

def door ():
	img = metal_panels ((0.45, 0.47, 0.50))
	img[:, 31:33] = (0.1, 0.1, 0.1)			# the seam
	img[28:36, :] = np.where ((((np.arange (TEX) // 4) % 2) == 0)[:, None], (0.95, 0.75, 0.08), (0.1, 0.1, 0.1))
	return img

def lift ():
	img = floor_plate () * 1.1
	img[:3, :] = (0.95, 0.75, 0.08); img[-3:, :] = (0.95, 0.75, 0.08)
	img[:, :3] = (0.95, 0.75, 0.08); img[:, -3:] = (0.95, 0.75, 0.08)
	return img

def light_panel (color):
	y, x = np.mgrid[0:TEX, 0:TEX]
	d = np.minimum (np.minimum (x, TEX - 1 - x), np.minimum (y, TEX - 1 - y))
	g = np.clip (0.55 + d / 10.0, 0, 1.0)
	img = tint (g, color)
	img[d < 3] = (0.35, 0.35, 0.37)
	return img

# name: (drawing, the engine's flag: "light" names are shown at full brightness)
TEXTURES = {
	'concrete': concrete, 'floor_tiles': floor_tiles, 'floor_plate': floor_plate,
	'metal_grey': lambda: metal_panels ((0.45, 0.46, 0.47)), 'metal_blue': lambda: metal_panels ((0.22, 0.32, 0.50)),
	'bricks': bricks, 'stone': stone, 'ceil_panels': ceiling_panels, 'ceil_grid': ceiling_grid, 'wood': wood,
	'hazard': hazard, 'door': door, 'lift': lift,
	'light_white': lambda: light_panel ((1.0, 0.97, 0.90)), 'light_blue': lambda: light_panel ((0.7, 0.85, 1.0)),
}

# ---- the palette: k-means over all the textures' texels ----------------------------------

def make_palette (images):
	px = np.concatenate ([np.clip (i, 0, 1).reshape (-1, 3) for i in images])
	sel = px[rng.choice (len (px), size = min (len (px), 60000), replace = False)]
	cent = sel[rng.choice (len (sel), 256, replace = False)].copy ()
	for it in range (16):
		d = ((sel[:, None, :] - cent[None, :, :]) ** 2).sum (-1)
		lab = d.argmin (1)
		for k in range (256):
			m = sel[lab == k]
			cent[k] = m.mean (0) if len (m) else sel[rng.integers (len (sel))]
	cent = cent[np.argsort (cent.sum (1))]		# dark to light
	return np.clip (np.round (cent * 255), 0, 255).astype (np.uint8)

def quantize (img, pal):
	p = pal.astype (float) / 255
	flat = np.clip (img, 0, 1).reshape (-1, 3)
	idx = np.empty (len (flat), np.uint8)
	for s in range (0, len (flat), 4096):
		d = ((flat[s:s + 4096, None, :] - p[None, :, :]) ** 2).sum (-1)
		idx[s:s + 4096] = d.argmin (1)
	return idx.reshape (img.shape[:2])

def mips (img):
	"""the 4 mip levels (box filtered, then quantized)"""
	out = [img]
	for m in range (3):
		a = out[-1]
		out.append ((a[0::2, 0::2] + a[1::2, 0::2] + a[0::2, 1::2] + a[1::2, 1::2]) / 4)
	return out

def write_wad (path, textures, pal):
	lumps = []
	for name, img in textures.items ():
		levels = [quantize (m, pal) for m in mips (img)]
		h, w = img.shape[:2]
		head = struct.pack ('<16sII', name.encode ()[:15], w, h)
		ofs = 16 + 8 + 16; offsets = []
		for lv in levels:
			offsets.append (ofs); ofs += lv.size
		data = head + struct.pack ('<4I', *offsets) + b''.join (lv.tobytes () for lv in levels)
		lumps.append ((name, data))
	body = b''; infos = []; pos = 12
	for name, data in lumps:
		infos.append (struct.pack ('<iiibbh16s', pos, len (data), len (data), 0x44, 0, 0, name.encode ()[:15]))
		body += data; pos += len (data)
	with open (path, 'wb') as f:
		f.write (b'WAD2' + struct.pack ('<ii', len (lumps), pos) + body + b''.join (infos))

def write_palette_c (path, pal):
	with open (path, 'w') as f:
		f.write ('/*\n * palette.c - the palette the BSP levels\' textures are in (256 RGB),\n'
			 ' * made with their textures by engine/tools/make_base.py (don\'t edit)\n */\n'
			 '#include "bsp.h"\n\nconst uint8_t bsp_palette[256][3] =\n{\n')
		for i in range (0, 256, 4):
			f.write ('\t' + ' '.join ('{%3d, %3d, %3d},' % tuple (pal[j]) for j in range (i, i + 4)) + '\n')
		f.write ('};\n')

# ---- brushes -------------------------------------------------------------------------------

def plane_face (n, d, tex):
	"""a brush face on the plane n.p = d (n outwards): three points ordered as
	   the .map format has them, (p2 - p0) x (p1 - p0) along n"""
	n = np.array (n, float)
	u = np.cross (n, (0, 0, 1)) if abs (n[2]) < 0.9 else np.cross (n, (1, 0, 0))
	u /= np.linalg.norm (u); v = np.cross (n / np.linalg.norm (n), u)
	p0 = n / np.dot (n, n) * d
	p1 = p0 + v * 128; p2 = p0 + u * 128		# (u x v = n)
	fmt = lambda p: '( %s )' % ' '.join (('%.4f' % c).rstrip ('0').rstrip ('.') for c in p)
	return '%s %s %s %s 0 0 0 1 1' % (fmt (p0), fmt (p1), fmt (p2), tex)

def box (x0, y0, z0, x1, y1, z1, tex):
	"""an axis box; tex: one name, or a dict by side ('top', 'bottom', 'side')"""
	t = tex if isinstance (tex, dict) else {'top': tex, 'bottom': tex, 'side': tex}
	faces = [plane_face ((-1, 0, 0), -x0, t['side']), plane_face ((1, 0, 0), x1, t['side']),
		 plane_face ((0, -1, 0), -y0, t['side']), plane_face ((0, 1, 0), y1, t['side']),
		 plane_face ((0, 0, -1), -z0, t['bottom']), plane_face ((0, 0, 1), z1, t['top'])]
	return '{\n' + '\n'.join (faces) + '\n}\n'

def ramp_x (x0, x1, y0, y1, z0, z1, top, side):
	"""a wedge: its top rising from z0 at x0 to z1 at x1, solid down to z0"""
	n = np.array ((-(z1 - z0), 0, x1 - x0), float)
	faces = [plane_face ((1, 0, 0), x1, side), plane_face ((0, -1, 0), -y0, side),
		 plane_face ((0, 1, 0), y1, side), plane_face ((0, 0, -1), -z0, side),
		 plane_face (n, np.dot (n, (x0, 0, z0)), top)]
	return '{\n' + '\n'.join (faces) + '\n}\n'

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
	os.makedirs (LEVELS, exist_ok = True)
	images = {name: f () for name, f in TEXTURES.items ()}
	pal = make_palette (list (images.values ()))
	write_wad (os.path.join (LEVELS, 'textures.wad'), images, pal)
	write_palette_c (os.path.join (HERE, '..', 'palette.c'), pal)
	g = cells ()
	world = solid_brushes (g) + details ()
	with open (os.path.join (LEVELS, 'base.map'), 'w') as f:
		f.write ('// base: the BSP engine\'s demo level (engine/tools/make_base.py: don\'t edit)\n')
		f.write ('{\n"classname" "worldspawn"\n"wad" "textures.wad"\n"message" "piegpu base"\n"_minlight" "20"\n')
		f.write (''.join (world))
		f.write ('}\n')
		f.write (''.join (entities ()))
	print ('base.map: %d world brushes, %d lights; textures.wad: %d textures' % (len (world), len (LIGHTS), len (images)))

if __name__ == '__main__':
	main ()
