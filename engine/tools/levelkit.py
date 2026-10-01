#!/usr/bin/env python3
"""levelkit.py - what the BSP engine's levels are made with (engine/tools/
make_*.py): the textures, drawn here (numpy) in RGB and quantized together to
one palette of 256 (k-means; Quake's BSP stores 8-bit textures, and the
engine converts them with that palette, engine/palette.c), the WAD2 they go
in, and brushes for Quake .map files.

  engine/tools/levelkit.py         writes engine/levels/textures.wad (all the
                                   textures, every level's) and engine/palette.c;
                                   then the levels: make_base.py, make_keep.py, make_isles.py,
                                   and engine/tools/build-level.sh NAME each

Texture names the engine treats apart (engine/render.c): light* drawn at
their own colour; sky* Quake's sky (256 x 128: the front layer on the left,
its colour 0 see-through, the back layer on the right); *NAME liquids, warped
(qbsp: *lava lava, *slime slime, other * water).
"""
import numpy as np, os, struct, sys

HERE = os.path.dirname (os.path.abspath (__file__))
LEVELS = os.path.join (HERE, '..', 'levels')
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

# ---- the castle's (make_keep.py) ----------------------------------------------------------

def castle_stone ():
	"""big dressed blocks, two courses of 32, staggered"""
	n = fbm (TEX, 20) * 0.3 + 0.7 + (rng.random ((TEX, TEX)) - 0.5) * 0.05
	img = tint (n, (0.56, 0.52, 0.46))
	for row in range (2):
		y0 = row * 32; off = 21 if row else 0
		for x0 in (0, 43):
			x0 = (x0 + off) % TEX; w = 43 if x0 == off else 21
			v = 0.85 + 0.25 * rng.random ()
			cols = [(x0 + k) % TEX for k in range (w)]
			img[y0:y0 + 32, cols] *= v
		img[y0:y0 + 2, :] = (0.25, 0.23, 0.21)		# mortar
		for x0 in (0, 43):
			img[y0:y0 + 32, [(x0 + off) % TEX, (x0 + off + 1) % TEX]] = (0.25, 0.23, 0.21)
	return img

def flagstone ():
	"""irregular paving slabs"""
	n = fbm (TEX, 21) * 0.25 + 0.75
	img = tint (n, (0.52, 0.50, 0.47))
	for (x0, y0, x1, y1) in ((0, 0, 40, 24), (40, 0, 64, 24), (0, 24, 22, 64), (22, 24, 64, 46), (22, 46, 64, 64)):
		img[y0:y1, x0:x1] *= 0.85 + 0.3 * rng.random ()
		bevel (img, x0, y0, x1, y1, 1.12, 0.55, 2)
	return img

def cobbles ():
	"""rounded stones: a jittered grid's cells, darker to their edges"""
	pts = np.array ([((i + 0.2 + 0.6 * rng.random ()) * 16, (j + 0.2 + 0.6 * rng.random ()) * 16)
			 for i in range (4) for j in range (4)])
	y, x = np.mgrid[0:TEX, 0:TEX]
	d = np.full ((TEX, TEX, 2), 1e9)
	shade = np.zeros ((TEX, TEX))
	for k, (px, py) in enumerate (pts):
		for ox in (-TEX, 0, TEX):
			for oy in (-TEX, 0, TEX):
				dd = np.hypot (x - px - ox, y - py - oy)
				closer = dd < d[..., 0]
				d[..., 1] = np.where (closer, d[..., 0], np.minimum (d[..., 1], dd))
				d[..., 0] = np.where (closer, dd, d[..., 0])
				shade = np.where (closer, 0.8 + 0.3 * ((k * 0.618) % 1.0), shade)
	edge = np.clip ((d[..., 1] - d[..., 0]) / 5.0, 0, 1)
	g = shade * (0.35 + 0.65 * edge) * (fbm (TEX, 22) * 0.2 + 0.8)
	return tint (g, (0.50, 0.47, 0.43))

def grass ():
	n = fbm (TEX, 23, 5, 2) * 0.4 + 0.6
	img = tint (n, (0.30, 0.40, 0.16))
	blades = rng.random ((TEX, TEX))
	img[blades > 0.93] *= 1.35
	img[blades < 0.05] *= 0.6
	return img

def rock ():
	n = fbm (TEX, 24, 5, 4)
	n = 0.45 + 0.75 * n ** 1.5
	return tint (n, (0.40, 0.36, 0.32))

def planks ():
	img = wood () * 0.85
	for k in range (0, TEX, 16):
		for x in (6, 56):
			img[k + 7:k + 9, x:x + 2] = (0.15, 0.15, 0.16)	# nails
	return img

def iron_bars ():
	"""a portcullis: crossed iron bars, the dark behind them"""
	img = np.full ((TEX, TEX, 3), 0.04)
	n = fbm (TEX, 25) * 0.3 + 0.7
	bar = tint (n, (0.30, 0.30, 0.32))
	for k in range (0, TEX, 16):
		img[:, k + 5:k + 11] = bar[:, k + 5:k + 11]
		img[:, k + 5] *= 1.4
		img[k + 6:k + 10, :] = bar[k + 6:k + 10, :] * 0.85
	return img

def gate ():
	"""an oak gate with iron straps"""
	img = wood () * 0.75
	for y in (10, 52):
		img[y:y + 5, :] = (0.18, 0.18, 0.19)
		for x in range (4, TEX, 12):
			img[y + 1:y + 3, x:x + 2] = (0.4, 0.4, 0.42)
	return img

def banner ():
	"""red cloth, a gold diamond"""
	n = fbm (TEX, 26) * 0.2 + 0.8
	img = tint (n, (0.55, 0.08, 0.07))
	y, x = np.mgrid[0:TEX, 0:TEX]
	d = abs (x - 31.5) + abs (y - 31.5)
	img[(d < 18) & (d > 13)] = (0.85, 0.65, 0.15)
	img[d < 8] = (0.85, 0.65, 0.15)
	img[:, :3] *= 0.6; img[:, -3:] *= 0.6
	return img

def flame ():
	"""a torch's flame, as fullbright (light*)"""
	y, x = np.mgrid[0:TEX, 0:TEX] / (TEX - 1.0)
	n = fbm (TEX, 27) * 0.4 + 0.6
	heat = np.clip ((1.0 - y) * 0.6 + n * 0.6 - abs (x - 0.5) * 0.8, 0, 1)
	return np.stack ([np.clip (0.6 + heat * 0.5, 0, 1), np.clip (0.15 + heat * 0.8, 0, 1), heat ** 3 * 0.6], -1)

def lava ():
	n = fbm (TEX, 28, 4, 2)
	hot = np.clip ((n - 0.35) * 2.2, 0, 1)
	return np.stack ([0.35 + 0.65 * hot, 0.05 + 0.55 * hot ** 1.6, 0.02 + 0.15 * hot ** 3], -1)

def water ():
	n = fbm (TEX, 29, 4, 2) * 0.5 + 0.5
	return tint (n, (0.12, 0.30, 0.34))

def sky_dusk ():
	"""Quake's 256 x 128 sky: the front layer (left) thin dark clouds, black
	   (colour 0) where it's clear; the back layer (right) a warm cloud deck"""
	h = 128
	back = np.zeros ((h, h, 3))
	n = fbm (h, 30, 5, 2)
	glow = np.clip ((n - 0.3) * 1.6, 0, 1)
	back[...] = np.array ((0.42, 0.22, 0.30)) * (1 - glow[..., None]) + np.array ((0.98, 0.62, 0.30)) * glow[..., None]
	front = np.zeros ((h, h, 3))
	m = fbm (h, 31, 5, 4)
	cloud = m > 0.56
	front[cloud] = (np.array ((0.30, 0.16, 0.22)) + 0.25 * (m[cloud, None] - 0.56))
	front[cloud] = np.maximum (front[cloud], 0.1)		# (never colour 0: see-through)
	return np.concatenate ([front, back], 1)

# ---- the islands' (make_isles.py) -----------------------------------------------------------

def bark ():
	y, x = np.mgrid[0:TEX, 0:TEX]
	ridges = np.sin ((x + fbm (TEX, 40) * 20) * 0.8) * 0.15 + 0.85
	return tint (ridges * (fbm (TEX, 41) * 0.3 + 0.7), (0.36, 0.25, 0.16))

def leaves ():
	n = fbm (TEX, 42, 5, 4)
	img = tint (0.55 + 0.6 * n, (0.20, 0.42, 0.14))
	spots = rng.random ((TEX, TEX))
	img[spots > 0.9] *= 1.4
	img[spots < 0.12] *= 0.55
	return img

def marble ():
	y, x = np.mgrid[0:TEX, 0:TEX]
	vein = np.abs (np.sin ((x + y) * 0.09 + fbm (TEX, 43, 5, 2) * 9))
	g = 0.82 + 0.16 * vein ** 0.3
	img = tint (g, (0.90, 0.88, 0.84))
	img[:2, :] *= 0.75; img[:, :2] *= 0.75
	return img

def jump_pad ():
	"""a jump pad's glow (light*: at its own colour): rings round an arrow"""
	y, x = np.mgrid[0:TEX, 0:TEX] - 31.5
	r = np.hypot (x, y)
	img = tint (np.clip (0.35 + 0.65 * (np.cos (r * 0.55) * 0.5 + 0.5) * (1 - r / 46), 0.2, 1), (0.35, 0.85, 1.0))
	arrow = ((np.abs (x) < 18 - (y + 14) * 0.9) & (y > -14) & (y < 6)) | ((np.abs (x) < 6) & (y >= 6) & (y < 22))
	img[arrow] = (1.0, 1.0, 0.95)
	return img

def portal ():
	"""a portal's swirl, warped as a liquid (*NAME)"""
	y, x = np.mgrid[0:TEX, 0:TEX] / TEX * 2 * np.pi
	n = fbm (TEX, 44, 4, 2)
	g = np.clip (0.5 + 0.35 * np.sin (x * 2 + y * 3 + n * 6) + 0.2 * (n - 0.5), 0, 1)
	return np.stack ([0.35 + 0.55 * g, 0.15 + 0.4 * g ** 2, 0.55 + 0.45 * g], -1)

def sky_day ():
	"""Quake's 256 x 128 sky: white clouds in front (black: clear), a blue back"""
	h = 128
	n = fbm (h, 45, 5, 2)
	back = np.array ((0.36, 0.58, 0.92)) * (0.85 + 0.3 * n[..., None])
	m = fbm (h, 46, 5, 4)
	front = np.zeros ((h, h, 3))
	cloud = m > 0.62
	front[cloud] = np.clip (np.array ((0.82, 0.86, 0.93)) + 0.8 * (m[cloud, None] - 0.62), 0, 1)
	return np.concatenate ([front, back], 1)

def trigger ():
	"""the triggers' brushes (never drawn)"""
	return np.full ((16, 16, 3), (0.8, 0.5, 0.2))

TEXTURES.update ({
	'bark': bark, 'leaves': leaves, 'marble': marble, 'light_pad': jump_pad, '*portal': portal,
	'sky_day': sky_day, 'trigger': trigger,
})

TEXTURES.update ({
	'castle': castle_stone, 'flagstone': flagstone, 'cobbles': cobbles, 'grass': grass, 'rock': rock,
	'planks': planks, 'iron_bars': iron_bars, 'gate': gate, 'banner': banner, 'light_flame': flame,
	'*lava1': lava, '*water1': water, 'sky_dusk': sky_dusk,
})

# ---- the palette: k-means over all the textures' texels ----------------------------------

def make_palette (images):
	"""colour 0 black (a sky's see-through), then 255 from the texels (but black)"""
	px = np.concatenate ([np.clip (i, 0, 1).reshape (-1, 3) for i in images])
	px = px[px.sum (1) > 0]
	sel = px[rng.choice (len (px), size = min (len (px), 60000), replace = False)]
	cent = sel[rng.choice (len (sel), 255, replace = False)].copy ()
	for it in range (16):
		d = ((sel[:, None, :] - cent[None, :, :]) ** 2).sum (-1)
		lab = d.argmin (1)
		for k in range (255):
			m = sel[lab == k]
			cent[k] = m.mean (0) if len (m) else sel[rng.integers (len (sel))]
	cent = cent[np.argsort (cent.sum (1))]		# dark to light
	pal = np.clip (np.round (cent * 255), 0, 255).astype (np.uint8)
	pal[pal.sum (1) == 0] = 1				# (only colour 0 black)
	return np.concatenate ([np.zeros ((1, 3), np.uint8), pal])

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
			 ' * made with their textures by engine/tools/levelkit.py (don\'t edit)\n */\n'
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

def brush (planes):
	"""a convex brush: its planes (n, d, tex), n outwards"""
	return '{\n' + '\n'.join (plane_face (n, d, t) for (n, d, t) in planes) + '\n}\n'

def slope (axis, a0, a1, c0, c1, z0, z1, thick, top, side):
	"""a sloping slab along axis ('x' or 'y'): its top rising from z0 at a0 to
	   z1 at a1, thick under it; c0 c1 across"""
	def v (a, c, z):
		return (a, c, z) if axis == 'x' else (c, a, z)
	n = np.array (v (-(z1 - z0), 0, a1 - a0), float)
	p0 = np.array (v (a0, 0, z0), float)
	q0 = np.array (v (a0, 0, z0 - thick), float)
	ax = np.array (v (1, 0, 0), float); cx = np.array (v (0, 1, 0), float)
	return brush ([(n, np.dot (n, p0), top), (-n, -np.dot (n, q0), side),
		       (-ax, -a0, side), (ax, a1, side), (-cx, -c0, side), (cx, c1, side)])

def carve (b, holes):
	"""the box b (x0 y0 z0 x1 y1 z1) less the holes: the boxes left"""
	out = [tuple (b)]
	for h in holes:
		next_ = []
		for p in out:
			if any (h[k] >= p[k + 3] or h[k + 3] <= p[k] for k in range (3)):
				next_.append (p)
				continue
			p = list (p)
			for k in range (3):			# the slabs either side of the hole, axis by axis
				if p[k] < h[k]:
					q = list (p); q[k + 3] = h[k]; next_.append (tuple (q)); p[k] = h[k]
				if p[k + 3] > h[k + 3]:
					q = list (p); q[k] = h[k + 3]; next_.append (tuple (q)); p[k + 3] = h[k + 3]
		out = next_
	return out

def write_map (path, title, worldspawn, world, entities):
	"""worldspawn: its keys (besides classname, wad); world: its brushes"""
	with open (path, 'w') as f:
		f.write ('// %s (engine/tools/make_%s.py: don\'t edit)\n' % (title, os.path.splitext (os.path.basename (path))[0]))
		f.write ('{\n"classname" "worldspawn"\n"wad" "textures.wad"\n')
		f.write (''.join ('"%s" "%s"\n' % kv for kv in worldspawn.items ()))
		f.write (''.join (world))
		f.write ('}\n')
		f.write (''.join (entities))

def main ():
	os.makedirs (LEVELS, exist_ok = True)
	images = {name: f () for name, f in TEXTURES.items ()}
	pal = make_palette (list (images.values ()))
	write_wad (os.path.join (LEVELS, 'textures.wad'), images, pal)
	write_palette_c (os.path.join (HERE, '..', 'palette.c'), pal)
	print ('textures.wad: %d textures; palette.c' % len (images))

if __name__ == '__main__':
	main ()
