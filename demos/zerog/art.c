/*
 * art.c - what zerog draws (art.h), all of it made here: no assets.
 *
 * The textures are drawn texel by texel, as antigrav's: where a texture's
 * alpha is 0 it glows (the scenery's shader: its own light, not the sun's),
 * so the road's edges, the pads, the tunnel's lights, the towers' windows and
 * the engines light the night.
 *
 * The road is two halves, a tile of texture at a time: the tiles are in one
 * texture, one over another (plain, a speed pad, a weapon pad, the start's
 * checks), and each half tile takes the one it is. The walls' two (plain,
 * chevrons on the outside of the tight curves) likewise. All of it is
 * triangle strips, joined by empty triangles into one draw a piece of road
 * and kind. The road's and the deck's face one way (the road up, the
 * underside down, the sides out), so their backs can be culled: under the
 * road the deck is not drawn at all, seen from above.
 */
#include "art.h"
#include <stdlib.h>
#include "vec.h"
#include "hud.h"

#define WALL_LEAN	0.35f		/* the walls lean out this much at the top */
#define THICK		0.8f		/* the deck under the road */
#define ROAD_CELLS	4		/* the road texture's tiles */
enum { CELL_PLAIN, CELL_BOOST, CELL_WEAPON, CELL_START };

art_t art;

const team_t team[CRAFTS] =
{
	{"VOLTA", {0.95f, 0.80f, 0.08f}, {0.08f, 0.08f, 0.10f}, {0.90f, 0.20f, 0.10f}, HUD_RGBA (255, 215, 40, 255)},
	{"KESTREL", {0.85f, 0.10f, 0.10f}, {0.95f, 0.95f, 0.95f}, {0.10f, 0.10f, 0.10f}, HUD_RGBA (255, 70, 60, 255)},
	{"NOVA", {0.12f, 0.32f, 0.90f}, {0.95f, 0.95f, 0.95f}, {0.95f, 0.75f, 0.10f}, HUD_RGBA (90, 150, 255, 255)},
	{"ORCA", {0.09f, 0.09f, 0.11f}, {0.90f, 0.90f, 0.92f}, {0.20f, 0.80f, 0.90f}, HUD_RGBA (60, 220, 240, 255)},
	{"HALO", {0.92f, 0.92f, 0.95f}, {0.10f, 0.50f, 0.95f}, {0.90f, 0.10f, 0.50f}, HUD_RGBA (240, 240, 250, 255)},
	{"ZEPHYR", {0.10f, 0.68f, 0.32f}, {0.10f, 0.10f, 0.10f}, {0.95f, 0.90f, 0.10f}, HUD_RGBA (60, 220, 110, 255)},
	{"EMBER", {0.95f, 0.42f, 0.06f}, {0.12f, 0.08f, 0.06f}, {0.98f, 0.92f, 0.80f}, HUD_RGBA (255, 140, 40, 255)},
	{"QUARTZ", {0.62f, 0.22f, 0.80f}, {0.95f, 0.95f, 0.98f}, {0.15f, 0.90f, 0.75f}, HUD_RGBA (200, 110, 255, 255)},
};

static float sun[3];

/* ---- textures: drawn texel by texel -------------------------------------------------- */

static uint32_t hash2 (int x, int y, int seed)
{
	uint32_t h = (uint32_t) x * 374761393u + (uint32_t) y * 668265263u + (uint32_t) seed * 1274126177u;
	h = (h ^ (h >> 13)) * 1103515245u;
	return h ^ (h >> 16);
}

static float rnd2 (int x, int y, int seed)	{ return (hash2 (x, y, seed) & 0xFFFF) / 65535.0f; }

/* value noise wrapping at px x py lattice cells */
static float vnoise (float x, float y, int px, int py, int seed)
{
	int ix = (int) floorf (x), iy = (int) floorf (y);
	float fx = x - ix, fy = y - iy;
	fx = fx * fx * (3 - 2 * fx);
	fy = fy * fy * (3 - 2 * fy);
	int x0 = ((ix % px) + px) % px, x1 = (x0 + 1) % px, y0 = ((iy % py) + py) % py, y1 = (y0 + 1) % py;
	float a = rnd2 (x0, y0, seed), b = rnd2 (x1, y0, seed), c = rnd2 (x0, y1, seed), d = rnd2 (x1, y1, seed);
	return (a + (b - a) * fx) + ((c + (d - c) * fx) - (a + (b - a) * fx)) * fy;
}

static uint16_t rgb565 (float r, float g, float b)
{
	int R = (int) (clampf (r, 0, 1) * 31 + 0.5f), G = (int) (clampf (g, 0, 1) * 63 + 0.5f), B = (int) (clampf (b, 0, 1) * 31 + 0.5f);
	return (uint16_t) (R << 11 | G << 5 | B);
}

static uint32_t rgba (float r, float g, float b, float a)
{
	return   (uint32_t) (clampf (r, 0, 1) * 255 + 0.5f) | (uint32_t) (clampf (g, 0, 1) * 255 + 0.5f) << 8
	       | (uint32_t) (clampf (b, 0, 1) * 255 + 0.5f) << 16 | (uint32_t) (clampf (a, 0, 1) * 255 + 0.5f) << 24;
}

/* the texels being made: 32 KB, the largest piece (64 x 128 RGBA) */
static union { uint16_t t16[128 * 128]; uint32_t t32[64 * 128]; } texels;

static GLuint new_texture (int w, int h, GLenum format, GLenum type, const void *data)
{
	GLuint t;
	glGenTextures (1, &t);
	glBindTexture (GL_TEXTURE_2D, t);
	glPixelStorei (GL_UNPACK_ALIGNMENT, type == GL_UNSIGNED_BYTE ? 4 : 2);
	glTexImage2D (GL_TEXTURE_2D, 0, format, w, h, 0, format, type, data);
	return t;
}

static void mipmapped (void)
{
	glGenerateMipmap (GL_TEXTURE_2D);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
}

/* the texture just made: between its mipmaps the nearer one, not both
   mixed. The GPU reads half as much for it (on the starting grid, where the
   crowd fills the picture: 1.3 ms of 14 a frame); the step from one to the
   next is seen on the road's lines, not on what's as rough as the crowd or
   the ground */
static void coarse (void)
{
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_NEAREST);
}

static void clamped (void)
{
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}

/* texels.t32 (RGBA: alpha 1 lit, 0 glowing) or texels.t16 (RGB565, lit) as a texture */
static GLuint rgba_texture (int w, int h)
{
	GLuint t = new_texture (w, h, GL_RGBA, GL_UNSIGNED_BYTE, texels.t32);
	mipmapped ();
	return t;
}

static GLuint rgb_texture (int w, int h)
{
	GLuint t = new_texture (w, h, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, texels.t16);
	mipmapped ();
	return t;
}

static void put (int w, int x, int y, const float c[4])
{
	texels.t32[y * w + x] = rgba (c[0], c[1], c[2], c[3]);
}

/* the signs' letters: 5 x 7, the top row first */
static const struct { char c; const char *rows[7]; } font[] =
{
	{'D', {"####.", "#...#", "#...#", "#...#", "#...#", "#...#", "####."}},
	{'E', {"#####", "#....", "#....", "####.", "#....", "#....", "#####"}},
	{'G', {".###.", "#...#", "#....", "#.###", "#...#", "#...#", ".###."}},
	{'I', {".###.", "..#..", "..#..", "..#..", "..#..", "..#..", ".###."}},
	{'J', {"..###", "...#.", "...#.", "...#.", "...#.", "#..#.", ".##.."}},
	{'M', {"#...#", "##.##", "#.#.#", "#.#.#", "#...#", "#...#", "#...#"}},
	{'O', {".###.", "#...#", "#...#", "#...#", "#...#", "#...#", ".###."}},
	{'P', {"####.", "#...#", "#...#", "####.", "#....", "#....", "#...."}},
	{'R', {"####.", "#...#", "#...#", "####.", "#.#..", "#..#.", "#...#"}},
	{'U', {"#...#", "#...#", "#...#", "#...#", "#...#", "#...#", ".###."}},
	{'V', {"#...#", "#...#", "#...#", "#...#", "#...#", ".#.#.", "..#.."}},
	{'Z', {"#####", "....#", "...#.", "..#..", ".#...", "#....", "#####"}},
	{'3', {"####.", "....#", "....#", ".###.", "....#", "....#", "####."}},
	{'>', {"#....", ".#...", "..#..", "...#.", "..#..", ".#...", "#...."}},
};

/* text into texels.t32 (w wide, row 0 at the bottom), its bottom left at x0,
   y0, each font pixel scale x scale */
static void text (int w, const char *s, int x0, int y0, int scale, const float c[4])
{
	for (; *s; s++, x0 += 6 * scale)
		for (unsigned g = 0; g < sizeof font / sizeof font[0]; g++)
		{
			if (font[g].c != *s)
			{
				continue;
			}
			for (int gy = 0; gy < 7; gy++)
				for (int gx = 0; gx < 5; gx++)
					if (font[g].rows[gy][gx] == '#')
						for (int k = 0; k < scale * scale; k++)
						{
							put (w, x0 + gx * scale + k % scale, y0 + (6 - gy) * scale + k / scale, c);
						}
		}
}

static int text_width (const char *s, int scale)	{ return (int) strlen (s) * 6 * scale - scale; }

/* half the road, a tile: 64 texels from the middle (x 0) to the edge, 128
   along (y: on, the way the craft go). The edge glows; the pads' marks do */
static void road_texel (int cell, int x, int y, float c[4])
{
	float n = 0.16f + 0.06f * vnoise (x / 4.0f, y / 8.0f, 16, 16, 1) + 0.035f * (rnd2 (x, y, 2) - 0.5f);
	v_set (c, n * 0.92f, n, n * 1.10f);
	c[3] = 1.0f;
	if (y % 64 == 0 || x == 32)			/* the seams between the deck's plates */
	{
		v_set (c, c[0] * 0.55f, c[1] * 0.55f, c[2] * 0.55f);
	}
	if (x >= 61)					/* the glowing edge */
	{
		v_set (c, 0.15f, 0.80f, 0.95f);
		c[3] = 0.0f;
	}
	else if (x == 55 || x == 56)			/* the edge line */
	{
		v_set (c, 0.85f, 0.85f, 0.82f);
	}
	else if (x < 2 && y % 64 < 32)			/* the dashes down the middle, a little lit */
	{
		v_set (c, 0.80f, 0.78f, 0.60f);
		c[3] = 0.6f;
	}
	if (cell == CELL_BOOST && x >= 6 && x <= 50)	/* chevrons pointing on */
	{
		int band = (((y + (int) (fabsf (x - 28.0f) * 1.3f)) % 32) + 32) % 32;
		if (x == 6 || x == 50)
		{
			v_set (c, 0.95f, 0.80f, 0.10f);
			c[3] = 0.0f;
		}
		else if (band < 14)
		{
			v_set (c, 1.0f, 0.42f + 0.03f * band, 0.05f);
			c[3] = 0.0f;
		}
	}
	else if (cell == CELL_WEAPON && x >= 6 && x <= 50)	/* a lattice of light, in a frame */
	{
		int a = ((x + y) % 16 + 16) % 16, b = ((x - y) % 16 + 16) % 16;
		if (x == 6 || x == 50 || y < 3 || y >= 125)
		{
			v_set (c, 0.95f, 0.25f, 0.85f);
			c[3] = 0.0f;
		}
		else if (a < 3 || b < 3)
		{
			float f = (float) y / 127;
			v_set (c, 0.15f + 0.75f * f, 0.85f - 0.55f * f, 0.95f);
			c[3] = 0.0f;
		}
	}
	else if (cell == CELL_START && y < 32 && x < 60)	/* checks */
	{
		float w = ((x / 8 + y / 8) & 1) ? 0.92f : 0.06f;
		v_set (c, w, w, w);
	}
}

static GLuint road_texture (void)
{
	GLuint t = new_texture (64, 128 * ROAD_CELLS, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	float c[4];
	for (int cell = 0; cell < ROAD_CELLS; cell++)
	{
		for (int y = 0; y < 128; y++)
			for (int x = 0; x < 64; x++)
			{
				road_texel (cell, x, y, c);
				put (64, x, y, c);
			}
		glTexSubImage2D (GL_TEXTURE_2D, 0, 0, cell * 128, 64, 128, GL_RGBA, GL_UNSIGNED_BYTE, texels.t32);
	}
	mipmapped ();
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	return t;
}

/* the walls: 64 along (u, a tile), two of 32 up (v): plain (the colour band
   glows), and chevrons pointing along the road */
static GLuint wall_texture (void)
{
	for (int y = 0; y < 32; y++)
		for (int x = 0; x < 64; x++)
		{
			float n = 0.9f + 0.2f * vnoise (x / 4.0f, y / 4.0f, 16, 8, 3);
			float c[4] = {0.16f * n, 0.20f * n, 0.30f * n, 1.0f};
			if (y < 4)				/* hazard stripes at the foot */
			{
				if (((x + y) / 4) & 1)
					v_set (c, 0.95f, 0.72f, 0.08f);
				else
					v_set (c, 0.07f, 0.07f, 0.08f);
			}
			else if (y >= 14 && y < 19)		/* the colour band */
			{
				if (x < 32)
					v_set (c, 0.10f, 0.70f, 0.90f);
				else
					v_set (c, 0.85f, 0.15f, 0.60f);
				c[3] = 0.0f;
			}
			else if (y >= 28)			/* the rail on top */
			{
				float w = y == 28 ? 0.45f : 0.85f;
				v_set (c, w, w * 1.02f, w * 1.06f);
			}
			if (x % 32 == 0 && y >= 4 && y < 28)
			{
				v_set (c, c[0] * 0.5f, c[1] * 0.5f, c[2] * 0.5f);
			}
			put (64, x, y, c);

			int band = ((int) (x + fabsf (y - 15.5f) * 0.9f) % 16 + 16) % 16;	/* (the tip at the largest u: on) */
			float d[4] = {0.06f, 0.06f, 0.07f, 1.0f};
			if (y < 2 || y >= 30)
				v_set (d, 0.85f, 0.85f, 0.85f);
			else if (band < 8)
			{
				v_set (d, 1.0f, 0.78f, 0.05f);
				d[3] = 0.4f;			/* lit a little: seen at night */
			}
			put (64, x, y + 32, d);
		}
	return rgba_texture (64, 64);
}

/* the deck's sides and underside, beams and roofs: riveted plates */
static GLuint metal_texture (void)
{
	for (int y = 0; y < 32; y++)
		for (int x = 0; x < 32; x++)
		{
			float w = 0.24f + 0.05f * vnoise (x / 4.0f, y / 4.0f, 8, 8, 4);
			if (x % 16 == 0 || y % 16 == 0)
				w = 0.14f;
			else if ((x % 16 == 3 || x % 16 == 12) && (y % 16 == 3 || y % 16 == 12))
				w = 0.42f;
			texels.t16[y * 32 + x] = rgb565 (w, w * 1.02f, w * 1.08f);
		}
	return rgb_texture (32, 32);
}

/* the pylons, pillars and stands: concrete, streaked */
static GLuint concrete_texture (void)
{
	for (int y = 0; y < 32; y++)
		for (int x = 0; x < 32; x++)
		{
			float w = 0.56f + 0.10f * vnoise (x / 4.0f, y / 4.0f, 8, 8, 5) - 0.12f * vnoise (x / 2.0f, y / 16.0f, 16, 2, 6)
				+ 0.04f * (rnd2 (x, y, 7) - 0.5f);
			texels.t16[y * 32 + x] = rgb565 (w, w * 0.98f, w * 0.94f);
		}
	return rgb_texture (32, 32);
}

/* the crowd in the stands: rows of people (8 texels a row: a step, then
   them), 3 texels a person, some seats empty */
static GLuint crowd_texture (void)
{
	static const float shirts[8][3] =
	{
		{0.85f, 0.15f, 0.12f}, {0.15f, 0.35f, 0.85f}, {0.95f, 0.85f, 0.20f}, {0.92f, 0.92f, 0.92f},
		{0.15f, 0.65f, 0.30f}, {0.90f, 0.45f, 0.10f}, {0.20f, 0.20f, 0.22f}, {0.60f, 0.20f, 0.70f},
	};
	for (int y = 0; y < 64; y++)
		for (int x = 0; x < 64; x++)
		{
			int row = y % 8, who = (int) hash2 (x / 3, y / 8, 12);
			float c[3];
			if (row < 2)				/* the step */
				v_set (c, 0.42f, 0.42f, 0.44f);
			else if ((who & 7) == 0)		/* an empty seat */
				v_set (c, 0.12f, 0.18f, 0.40f);
			else if (row < 6)
				memcpy (c, shirts[(who >> 3) & 7], sizeof c);
			else if (who & 0x100)			/* heads */
				v_set (c, 0.85f, 0.64f, 0.50f);
			else
				v_set (c, 0.45f, 0.30f, 0.20f);
			if (x % 3 == 2)
			{
				v_set (c, c[0] * 0.5f, c[1] * 0.5f, c[2] * 0.5f);
			}
			texels.t16[y * 64 + x] = rgb565 (c[0], c[1], c[2]);
		}
	return rgb_texture (64, 64);
}

/* the big screen over the start: rings of light and ZEROG, glowing, in a frame */
static GLuint screen_texture (void)
{
	for (int y = 0; y < 64; y++)
		for (int x = 0; x < 128; x++)
		{
			float dx = (x - 63.5f) / 64, dy = (y - 31.5f) / 32, r = sqrtf (dx * dx + dy * dy);
			bool ring = (int) floorf (r * 9.0f) & 1;
			float f = clampf (1.25f - 0.7f * r, 0, 1);
			float c[4] = {(ring ? 0.10f : 0.45f) * f, (ring ? 0.75f : 0.10f) * f, (ring ? 0.95f : 0.60f) * f, 0.0f};
			if (x < 3 || x >= 125 || y < 3 || y >= 61)
			{
				v_set (c, 0.12f, 0.12f, 0.14f);
				c[3] = 1.0f;
			}
			put (128, x, y, c);
		}
	const float shade[4] = {0.02f, 0.05f, 0.25f, 0.0f}, white[4] = {1.0f, 1.0f, 1.0f, 0.0f};
	int s = 3, x0 = (128 - text_width ("ZEROG", s)) / 2, y0 = (64 - 7 * s) / 2;
	text (128, "ZEROG", x0 + 2, y0 - 2, s, shade);
	text (128, "ZEROG", x0, y0, s, white);
	return rgba_texture (128, 64);
}

/* a banner, 128 x 32: a word on a gradient between stripes, a little lit */
static GLuint banner_texture (const char *word, const float *from, const float *to, const float *stripe, float lit)
{
	for (int y = 0; y < 32; y++)
		for (int x = 0; x < 128; x++)
		{
			float f = x / 127.0f, c[4] = {from[0] + (to[0] - from[0]) * f, from[1] + (to[1] - from[1]) * f,
						      from[2] + (to[2] - from[2]) * f, lit};
			if (y < 3 || y >= 29)
			{
				memcpy (c, stripe, 3 * sizeof (float));
			}
			put (128, x, y, c);
		}
	int s = 3;
	while (s > 1 && text_width (word, s) > 118)
	{
		s--;
	}
	const float white[4] = {0.97f, 0.97f, 0.97f, 0.3f * lit / 0.45f};
	text (128, word, (128 - text_width (word, s)) / 2, (32 - 7 * s) / 2, s, white);
	return rgba_texture (128, 32);
}

/* the tunnel's lining: plates with glowing light panels */
static GLuint roof_texture (void)
{
	for (int y = 0; y < 64; y++)
		for (int x = 0; x < 64; x++)
		{
			float w = 0.20f + 0.04f * vnoise (x / 4.0f, y / 4.0f, 16, 16, 13);
			float c[4] = {w, w * 1.03f, w * 1.1f, 1.0f};
			if (x % 32 == 0 || y % 32 == 0)
				v_set (c, 0.10f, 0.10f, 0.12f);
			if (x >= 6 && x < 58 && y >= 26 && y < 38)
			{
				v_set (c, 0.85f, 0.92f, 1.0f);
				c[3] = 0.0f;
			}
			put (64, x, y, c);
		}
	return rgba_texture (64, 64);
}

/* the ribs and hoops: orange metal with a glowing blue strip */
static GLuint rib_texture (void)
{
	for (int y = 0; y < 32; y++)
		for (int x = 0; x < 32; x++)
		{
			float n = 0.9f + 0.15f * vnoise (x / 4.0f, y / 4.0f, 8, 8, 14);
			float c[4] = {0.90f * n, 0.45f * n, 0.10f * n, 1.0f};
			if (x < 2 || x >= 30)
				v_set (c, 0.25f, 0.13f, 0.05f);
			else if (x >= 14 && x < 18)
			{
				v_set (c, 0.20f, 0.80f, 1.0f);
				c[3] = 0.0f;
			}
			put (32, x, y, c);
		}
	return rgba_texture (32, 32);
}

/* hazard stripes, a little lit: the jump's faces, the fork's nose */
static GLuint hazard_texture (void)
{
	for (int y = 0; y < 32; y++)
		for (int x = 0; x < 32; x++)
		{
			float c[4] = {0.06f, 0.06f, 0.07f, 1.0f};
			if (((x + y) / 8) & 1)
			{
				v_set (c, 0.98f, 0.76f, 0.06f);
				c[3] = 0.35f;
			}
			put (32, x, y, c);
		}
	return rgba_texture (32, 32);
}

/* the towers: rows of windows, some lit */
static GLuint tower_texture (void)
{
	for (int y = 0; y < 64; y++)
		for (int x = 0; x < 64; x++)
		{
			int wx = x % 8, wy = y % 8, who = (int) hash2 (x / 8, y / 8, 21);
			float n = 0.16f + 0.05f * vnoise (x / 8.0f, y / 8.0f, 8, 8, 22);
			float c[4] = {n, n * 1.04f, n * 1.14f, 1.0f};
			if (wx >= 2 && wx < 7 && wy >= 2 && wy < 6)
			{
				if ((who & 7) < 3)		/* lit */
				{
					float warm = (who >> 3 & 15) / 15.0f;
					v_set (c, 0.95f, 0.80f + 0.12f * warm, 0.45f + 0.45f * warm);
					c[3] = 0.0f;
				}
				else
				{
					v_set (c, 0.05f, 0.08f, 0.13f);
				}
			}
			put (64, x, y, c);
		}
	return rgba_texture (64, 64);
}

/* the night's stars: points of light on black (the night sky's shader adds them) */
static GLuint stars_texture (void)
{
	for (int y = 0; y < 128; y++)
		for (int x = 0; x < 64; x++)
		{
			float r = rnd2 (x, y, 15), b = 0.4f + 0.6f * rnd2 (x, y, 16);
			float c[4] = {0, 0, 0, 0};
			if (r > 0.9975f)			/* (1 texel in 400) */
				v_set (c, b, b, b * 1.1f);
			put (64, x, y, c);
		}
	GLuint t = rgba_texture (64, 128);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);	/* (points: not blurred away far off) */
	return t;
}

/* the ground: dark earth and scrub, 128 x 128, made 16 rows at a time */
static GLuint ground_texture (void)
{
	GLuint t = new_texture (128, 128, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, NULL);
	for (int y0 = 0; y0 < 128; y0 += 16)
	{
		for (int y = y0; y < y0 + 16; y++)
			for (int x = 0; x < 128; x++)
			{
				float n = 0.6f * vnoise (x / 16.0f, y / 16.0f, 8, 8, 8) + 0.3f * vnoise (x / 8.0f, y / 8.0f, 16, 16, 9)
					+ 0.1f * rnd2 (x, y, 10);
				float d = clampf ((n - 0.35f) * 3.0f, 0, 1);
				float c[3] = {0.24f + 0.22f * d, 0.30f + 0.10f * d, 0.22f + 0.10f * d};
				if (x % 64 == 0 || y % 64 == 0)	/* a grid of paths */
				{
					v_set (c, 0.40f, 0.40f, 0.42f);
				}
				texels.t16[(y - y0) * 128 + x] = rgb565 (c[0], c[1], c[2]);
			}
		glTexSubImage2D (GL_TEXTURE_2D, 0, 0, y0, 128, 16, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, texels.t16);
	}
	mipmapped ();
	return t;
}

/* a craft's shadow: black, soft-edged (alpha) */
static GLuint shadow_texture (void)
{
	for (int y = 0; y < 32; y++)
		for (int x = 0; x < 32; x++)
		{
			float dx = (x + 0.5f) / 16 - 1, dy = (y + 0.5f) / 16 - 1, a = clampf (1.0f - (dx * dx + dy * dy), 0, 1);
			texels.t32[y * 32 + x] = rgba (0, 0, 0, 0.65f * a * sqrtf (a));
		}
	GLuint t = new_texture (32, 32, GL_RGBA, GL_UNSIGNED_BYTE, texels.t32);
	clamped ();
	mipmapped ();
	return t;
}

/* the lights' texture, 32 x 32: a soft light, brightest in the middle */
static GLuint flare_texture (void)
{
	for (int y = 0; y < 32; y++)
		for (int x = 0; x < 32; x++)
		{
			float dx = (x + 0.5f) / 16 - 1, dy = (y + 0.5f) / 16 - 1, r = sqrtf (dx * dx + dy * dy);
			float a = clampf (1.0f - r, 0, 1), blob = a * a * (0.6f + 2.2f * a * a);
			texels.t32[y * 32 + x] = rgba (blob, blob, blob, 1);
		}
	GLuint t = new_texture (32, 32, GL_RGBA, GL_UNSIGNED_BYTE, texels.t32);
	clamped ();
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	return t;
}

/* what's fired, 16 x 64, seen from above (v from the nose), four of 16 rows:
   a rocket (white, a red band, its tail glowing), a mine (dark, glowing
   bands), an explosion's fire, and its first flash (both all light) */
static GLuint ordnance_texture (void)
{
	for (int y = 0; y < 16; y++)
		for (int x = 0; x < 16; x++)
		{
			float c[4] = {0.90f, 0.90f, 0.92f, 1.0f}, d[4] = {0.10f, 0.10f, 0.12f, 1.0f}, f = y / 15.0f;
			float fire[4] = {1.0f, 0.72f - 0.50f * f, 0.14f - 0.10f * f, 0.0f}, flash[4] = {1.0f, 1.0f - 0.2f * f, 0.85f - 0.5f * f, 0.0f};
			if (y >= 4 && y < 7)
				v_set (c, 0.85f, 0.10f, 0.08f);
			if (y >= 13)
			{
				v_set (c, 1.0f, 0.75f, 0.30f);
				c[3] = 0.0f;
			}
			if (y % 8 >= 3 && y % 8 < 5)
			{
				v_set (d, 1.0f, 0.15f, 0.10f);
				d[3] = 0.0f;
			}
			put (16, x, y, c);
			put (16, x, y + 16, d);
			put (16, x, y + 32, fire);
			put (16, x, y + 48, flash);
		}
	return rgba_texture (16, 64);
}

/* a craft's livery, 64 x 64 seen from above: u across (0.5 the middle), v
   from the nose (0) to the tail (1); alpha 0 glows (the engines, at the
   pontoons' backs) */
static GLuint livery_texture (const team_t *c)
{
	for (int y = 0; y < 64; y++)
		for (int x = 0; x < 64; x++)
		{
			float u = (x + 0.5f) / 64, v = (y + 0.5f) / 64, cu = fabsf (u - 0.5f);
			float n = 0.9f + 0.1f * vnoise (x / 4.0f, y / 4.0f, 16, 16, 11);
			float col[3] = {c->base[0] * n, c->base[1] * n, c->base[2] * n}, a = 1.0f;
			if (cu > 0.27f)					/* the pontoons */
			{
				memcpy (col, c->accent, sizeof col);
				if (v > 0.45f && v < 0.62f)
					memcpy (col, c->stripe, sizeof col);
			}
			else if (cu > 0.17f)				/* the wing between */
			{
				v_set (col, col[0] * 0.55f, col[1] * 0.55f, col[2] * 0.55f);
			}
			if (cu < 0.045f && v < 0.42f)
				memcpy (col, c->stripe, sizeof col);
			if (v > 0.43f && v < 0.72f && cu < 0.10f - 0.04f * (v - 0.43f) / 0.29f)
			{
				float h = v > 0.47f && v < 0.50f ? 0.35f : 0.0f;	/* the canopy's glint */
				v_set (col, 0.05f + h, 0.10f + h, 0.18f + h);
			}
			if (v > 0.87f && cu < 0.29f)			/* the rear wing */
				memcpy (col, v > 0.93f ? c->accent : c->stripe, sizeof col);
			if (v > 0.965f && cu > 0.30f)
			{
				v_set (col, 1.0f, 0.70f, 0.35f);
				a = 0.0f;
			}
			texels.t32[y * 64 + x] = rgba (col[0], col[1], col[2], a);
		}
	GLuint t = new_texture (64, 64, GL_RGBA, GL_UNSIGNED_BYTE, texels.t32);
	clamped ();
	mipmapped ();
	return t;
}

/* ---- the meshes: flat-faced hulls, their texture projected from above ----------------------- */

#define MESH_MAX	600
static craft_vertex_t mesh[MESH_MAX];
static int mesh_n;
static float mesh_nose, mesh_tail, mesh_span, mesh_v0, mesh_v1;

/* a flat triangle, its normal away from centre */
static void tri (const float *a, const float *b, const float *c, const float *centre)
{
	const float *v[3] = {a, b, c};
	float e1[3], e2[3], n[3], mid[3];
	v_sub (e1, b, a);
	v_sub (e2, c, a);
	v_cross (n, e1, e2);
	v_norm (n);
	v_set (mid, (a[0] + b[0] + c[0]) / 3 - centre[0], (a[1] + b[1] + c[1]) / 3 - centre[1], (a[2] + b[2] + c[2]) / 3 - centre[2]);
	if (v_dot (n, mid) < 0.0f)
	{
		v_set (n, -n[0], -n[1], -n[2]);
	}
	for (int i = 0; i < 3 && mesh_n < MESH_MAX; i++)
	{
		craft_vertex_t *cv = &mesh[mesh_n++];
		v_copy (cv->p, v[i]);
		v_copy (cv->n, n);
		cv->uv[0] = v[i][0] / mesh_span + 0.5f;
		cv->uv[1] = mesh_v0 + (mesh_v1 - mesh_v0) * clampf ((v[i][2] - mesh_nose) / (mesh_tail - mesh_nose), 0.02f, 0.98f);
	}
}

/* a hull about x = cx: from z0 (half as wide as w0, from y0f up to y1f) to z1 (w1, y0b .. y1b) */
static void hull (float cx, float w0, float w1, float y0f, float y1f, float y0b, float y1b, float z0, float z1)
{
	float p[8][3], c[3] = {cx, (y0f + y1f + y0b + y1b) / 4, (z0 + z1) / 2};
	for (int i = 0; i < 8; i++)
	{
		bool back = i & 4;
		v_set (p[i], cx + (i & 1 ? 1 : -1) * (back ? w1 : w0), i & 2 ? (back ? y1b : y1f) : (back ? y0b : y0f), back ? z1 : z0);
	}
	static const int faces[6][4] = {{0, 1, 3, 2}, {4, 5, 7, 6}, {0, 1, 5, 4}, {2, 3, 7, 6}, {0, 2, 6, 4}, {1, 3, 7, 5}};
	for (int f = 0; f < 6; f++)
	{
		tri (p[faces[f][0]], p[faces[f][1]], p[faces[f][2]], c);
		tri (p[faces[f][0]], p[faces[f][2]], p[faces[f][3]], c);
	}
}

static void build_meshes (void)
{
	/* the craft: a long body, a pontoon each side on a wing, a tail wing on fins
	   (x right, y up, z back: the nose at -z) */
	mesh_nose = -3.4f;
	mesh_tail = 2.5f;
	mesh_span = 3.4f;
	mesh_v0 = 0.0f;
	mesh_v1 = 1.0f;
	art.craft_first = mesh_n;
	hull (0.0f, 0.10f, 0.55f, 0.16f, 0.30f, 0.10f, 0.66f, -3.4f, 1.7f);
	hull (0.0f, 0.20f, 0.33f, 0.44f, 0.52f, 0.58f, 0.90f, -0.9f, 0.9f);
	for (int s = -1; s <= 1; s += 2)
	{
		hull (s * 1.3f, 0.08f, 0.30f, 0.05f, 0.16f, 0.04f, 0.44f, -2.3f, 2.5f);
		hull (s * 0.95f, 0.04f, 0.04f, 0.30f, 0.36f, 0.30f, 1.00f, 0.9f, 2.2f);
	}
	hull (0.0f, 1.3f, 1.3f, 0.20f, 0.30f, 0.20f, 0.32f, -0.2f, 1.3f);
	hull (0.0f, 1.0f, 1.0f, 0.95f, 1.02f, 0.98f, 1.06f, 1.7f, 2.35f);
	art.craft_count = mesh_n - art.craft_first;

	/* a rocket: a dart; a mine: two pyramids, base to base; an explosion's
	   fire: the same, all light (and again, for its first flash) */
	mesh_nose = -0.9f;
	mesh_tail = 0.7f;
	mesh_span = 0.6f;
	mesh_v1 = 0.25f;
	art.shot_first = mesh_n;
	hull (0.0f, 0.02f, 0.14f, -0.02f, 0.02f, -0.14f, 0.14f, -0.9f, 0.7f);
	hull (0.0f, 0.30f, 0.30f, -0.01f, 0.01f, -0.01f, 0.01f, 0.1f, 0.7f);
	art.shot_count = mesh_n - art.shot_first;
	mesh_nose = -0.5f;
	mesh_tail = 0.5f;
	mesh_span = 1.0f;
	int *first[3] = {&art.mine_first, &art.fire_first, &art.flash_first};
	for (int k = 0; k < 3; k++)
	{
		mesh_v0 = 0.25f * (k + 1);
		mesh_v1 = mesh_v0 + 0.25f;
		*first[k] = mesh_n;
		hull (0.0f, 0.02f, 0.38f, -0.02f, 0.02f, -0.38f, 0.38f, -0.5f, 0.0f);
		hull (0.0f, 0.38f, 0.02f, -0.38f, 0.38f, -0.02f, 0.02f, 0.0f, 0.5f);
	}
	art.mine_count = art.fire_first - art.mine_first;

	glGenBuffers (1, &art.meshes);
	glBindBuffer (GL_ARRAY_BUFFER, art.meshes);
	glBufferData (GL_ARRAY_BUFFER, mesh_n * (GLsizeiptr) sizeof (craft_vertex_t), mesh, GL_STATIC_DRAW);
}

/* ---- geometry: vertices through a small buffer into GL buffers ------------------------- */

#define CHUNK		128
static vertex_t chunk[CHUNK];
static int chunk_n, emitted;		/* in the chunk; written to the bound buffer */

static void flush_chunk (void)
{
	if (chunk_n)
	{
		glBufferSubData (GL_ARRAY_BUFFER, emitted * (GLintptr) sizeof (vertex_t), chunk_n * (GLsizeiptr) sizeof (vertex_t), chunk);
		emitted += chunk_n;
		chunk_n = 0;
	}
}

/* into GL_ARRAY_BUFFER's buffer, from vertex `emitted` on */
static void begin_vertices (void)
{
	chunk_n = emitted = 0;
}

static int vertices_so_far (void)	{ return emitted + chunk_n; }

static float shade_of (const float *n)
{
	return 0.50f + 0.62f * fmaxf (v_dot (n, sun), 0.0f) + 0.10f * fmaxf (n[1], 0.0f);
}

static void emit_vertex (const vertex_t *v)
{
	chunk[chunk_n++] = *v;
	if (chunk_n == CHUNK)
	{
		flush_chunk ();
	}
}

static void emit (const float *p, float u, float v, float shade)
{
	emit_vertex (&(vertex_t) {p[0], p[1], p[2], u, v, shade});
}

/* strips, one after another in a draw: joined by empty triangles (the last
   vertex and the next again) */
static bool strip_new;
static vertex_t strip_last;

static void strip_break (void)	{ strip_new = true; }

static void strip (const float *p, float u, float v, const float *n)
{
	vertex_t x = {p[0], p[1], p[2], u, v, shade_of (n)};
	if (strip_new)
	{
		emit_vertex (&strip_last);
		emit_vertex (&x);
		strip_new = false;
	}
	emit_vertex (&x);
	strip_last = x;
}

/* a quad a b c d (round its edge), its texture u0..u1 along a-b, v0..v1 along a-d */
static void quad (const float *a, const float *b, const float *c, const float *d, float u0, float v0, float u1, float v1,
		  const float *n)
{
	float s = shade_of (n);
	emit (a, u0, v0, s);
	emit (b, u1, v0, s);
	emit (c, u1, v1, s);
	emit (a, u0, v0, s);
	emit (c, u1, v1, s);
	emit (d, u0, v1, s);
}

/* ---- the circuit's rings ---------------------------------------------------------------- */

static const ring_t *ring (int r, int i)
{
	const route_t *m = &route[r];
	return &m->rings[m->loop ? ((i % m->n) + m->n) % m->n : i < 0 ? 0 : i > m->n ? m->n : i];
}

/* a point by a ring: a across (right), b up the ring's (banked) up */
static void ring_point (float *p, int r, int i, float a, float b)
{
	v_mad (p, ring (r, i)->p, ring (r, i)->r, a);
	v_mad (p, p, ring (r, i)->u, b);
}

/* ... a across the level, at the height y */
static void level_point (float *p, int r, int i, float a, float y)
{
	float lr[3] = {ring (r, i)->r[0], 0.0f, ring (r, i)->r[2]};
	v_norm (lr);
	v_mad (p, ring (r, i)->p, lr, a);
	p[1] = y;
}

/* the fork's ring where its road and the main one touch: its inner edge is
   the main road's (a little under it, and in from its edge: no crack
   between them). Where that is across the fork's ring, and the point b up */
static bool fork_touch (int r, int i, float *x, float *p, float b)
{
	const route_t *a = &route[ROUTE_ALT];
	if (r != ROUTE_ALT || ring (r, i)->sep >= OPEN_SEP)
	{
		return false;
	}
	frame_t f;
	track_frame (ROUTE_MAIN, a->main_s[i < 0 ? 0 : i > a->n ? a->n : i], &f);
	*x = a->side * (HW - ring (r, i)->sep);
	v_mad (p, f.p, f.r, a->side * (HW - 0.15f));
	v_mad (p, p, f.u, b - 0.02f);
	return true;
}

/* the road's edge at a ring, on a side (0 left, 1 right): where across, and the point b up */
static float road_edge (float *p, int r, int i, int side, float b)
{
	float x;
	if ((side ? 1 : -1) != route[ROUTE_ALT].side && fork_touch (r, i, &x, p, b))
	{
		return x;
	}
	ring_point (p, r, i, side ? HW : -HW, b);
	return side ? HW : -HW;
}

/* its middle (the fork's is at the main road's edge while that covers it) */
static float road_middle (float *p, int r, int i, float b)
{
	float x;
	if (fork_touch (r, i, &x, p, b) && x * route[ROUTE_ALT].side > 0.0f)
	{
		return x;
	}
	ring_point (p, r, i, 0.0f, b);
	return 0.0f;
}

/* ---- the road, the walls, the deck: strips, a piece of road at a time ----------------------- */

static bool wall_there (int r, int i, int side)
{
	return !(ring (r, i)->flags & (RING_GAP | (side ? RING_OPEN_R : RING_OPEN_L)));
}

static void wall_pair (int r, int i, int side, int cell)
{
	float foot[3], top[3], n[3], s = side ? 1.0f : -1.0f, u = (float) i / TILE_RINGS;
	ring_point (foot, r, i, s * HW, 0.0f);
	ring_point (top, r, i, s * (HW + WALL_LEAN), WALL_H);
	v_set (n, -s * ring (r, i)->r[0], -s * ring (r, i)->r[1], -s * ring (r, i)->r[2]);	/* both face the road */
	strip (foot, u, (cell + 0.5f / 32) / 2, n);
	strip (top, u, (cell + 31.5f / 32) / 2, n);
}

static void chunk_strips (int kind, int r, int tile0, int tiles)
{
	const route_t *m = &route[r];
	float p[3], q[3], n[3];
	for (int tile = tile0; tile < tile0 + tiles; tile++)
	{
		int i0 = tile * TILE_RINGS;
		switch (kind)
		{
		case K_ROAD:
			if (m->rings[i0].flags & RING_GAP)
			{
				break;
			}
			for (int half = 0; half < 2; half++)
			{
				int pad = m->pads[tile][half];
				int cell = r == ROUTE_MAIN && tile == 0 ? CELL_START : pad == PAD_BOOST ? CELL_BOOST
					 : pad == PAD_WEAPON ? CELL_WEAPON : CELL_PLAIN;
				strip_break ();
				for (int j = 0; j <= TILE_RINGS; j++)
				{
					float v = (cell + (0.5f + j * 127.0f / TILE_RINGS) / 128.0f) / ROAD_CELLS;
					float xm = road_middle (p, r, i0 + j, 0.0f), xe = road_edge (q, r, i0 + j, half, 0.0f);
					if (half)			/* (left to right: the road's face is up) */
					{
						strip (p, fabsf (xm) / HW, v, ring (r, i0 + j)->u);
					}
					strip (q, fminf (fabsf (xe) / HW, 1.0f), v, ring (r, i0 + j)->u);
					if (!half)
					{
						strip (p, fabsf (xm) / HW, v, ring (r, i0 + j)->u);
					}
				}
			}
			break;

		case K_WALL:
			for (int side = 0; side < 2; side++)
			{
				float k = 0.0f;
				for (int j = 0; j < TILE_RINGS; j++)
				{
					k += ring (r, i0 + j)->k / TILE_RINGS;
				}
				int cell = side == 0 ? k > 1.0f / 170 : k < -1.0f / 170;	/* the left wall: a right-hand curve's */
				bool on = false;
				for (int j = 0; j < TILE_RINGS; j++)
				{
					if (!wall_there (r, i0 + j, side))
					{
						on = false;
						continue;
					}
					if (!on)
					{
						strip_break ();
						wall_pair (r, i0 + j, side, cell);
						on = true;
					}
					wall_pair (r, i0 + j + 1, side, cell);
				}
			}
			break;

		case K_METAL:
			for (int part = 0; part < 3; part++)		/* the left side, the underside, the right side */
			{
				bool on = false;
				for (int j = 0; j <= TILE_RINGS; j++)
				{
					int i = i0 + j, side = part == 2;
					bool there = j < TILE_RINGS ? !(ring (r, i)->flags & RING_GAP) && (part == 1 || wall_there (r, i, side)) : on;
					if (j == TILE_RINGS && !on)
					{
						break;
					}
					if (!on && there)
					{
						strip_break ();
					}
					if (there || on)
					{
						float along = (float) i / TILE_RINGS;
						if (part == 1)
						{
							v_set (n, -ring (r, i)->u[0], -ring (r, i)->u[1], -ring (r, i)->u[2]);
							road_edge (p, r, i, 1, -THICK);		/* (right to left: it faces down) */
							road_edge (q, r, i, 0, -THICK);
							strip (p, 1.0f, along, n);
							strip (q, 0.0f, along, n);
						}
						else
						{
							float s = side ? 1.0f : -1.0f;
							v_set (n, s * ring (r, i)->r[0], s * ring (r, i)->r[1], s * ring (r, i)->r[2]);
							ring_point (p, r, i, s * HW, 0.0f);
							ring_point (q, r, i, s * HW, -THICK);
							if (side)			/* (each faces out) */
							{
								strip (p, 0.0f, along, n);
							}
							strip (q, 0.25f, along, n);
							if (!side)
							{
								strip (p, 0.0f, along, n);
							}
						}
					}
					on = there;
				}
			}
			break;
		}
	}
}

static void build_track (void)
{
	/* the pieces: CHUNK_TILES tiles of a route each, and a sphere round each */
	int tiles_all = 0;
	art.chunks = 0;
	static struct { uint8_t route; uint16_t tile0, tiles; } piece[MAX_CHUNKS];
	for (int r = 0; r < ROUTES; r++)
	{
		const route_t *m = &route[r];
		int tiles = m->n / TILE_RINGS;
		tiles_all += tiles;
		for (int tile0 = 0; tile0 < tiles && art.chunks < MAX_CHUNKS; tile0 += CHUNK_TILES)
		{
			chunk_t *c = &art.chunk[art.chunks];
			int n = tiles - tile0 < CHUNK_TILES ? tiles - tile0 : CHUNK_TILES, rings = n * TILE_RINGS + 1;
			piece[art.chunks].route = (uint8_t) r;
			piece[art.chunks].tile0 = (uint16_t) tile0;
			piece[art.chunks++].tiles = (uint16_t) n;
			v_set (c->c, 0, 0, 0);
			for (int j = 0; j < rings; j++)
			{
				v_mad (c->c, c->c, ring (r, tile0 * TILE_RINGS + j)->p, 1.0f / rings);
			}
			c->radius = 0.0f;
			for (int j = 0; j < rings; j++)
			{
				float d[3];
				v_sub (d, ring (r, tile0 * TILE_RINGS + j)->p, c->c);
				c->radius = fmaxf (c->radius, v_len (d));
			}
			c->radius += HW + 4.0f;
		}
	}

	/* their strips: a kind after another, a piece after another */
	glGenBuffers (1, &art.track);
	glBindBuffer (GL_ARRAY_BUFFER, art.track);
	glBufferData (GL_ARRAY_BUFFER, tiles_all * 92 * (GLsizeiptr) sizeof (vertex_t), NULL, GL_STATIC_DRAW);
	begin_vertices ();
	memset (&strip_last, 0, sizeof strip_last);
	for (int kind = 0; kind < KINDS; kind++)
		for (int c = 0; c < art.chunks; c++)
		{
			art.chunk[c].first[kind] = vertices_so_far ();
			strip_break ();
			chunk_strips (kind, piece[c].route, piece[c].tile0, piece[c].tiles);
			art.chunk[c].count[kind] = vertices_so_far () - art.chunk[c].first[kind];
		}
	flush_chunk ();
}

/* ---- the scenery by the road and over it: made in parts, a texture each ------------------ */

static int part_now;			/* the part being made: the others' quads are left out */

static void pquad (int part, const float *a, const float *b, const float *c, const float *d, float u0, float v0,
		   float u1, float v1, const float *n)
{
	if (part == part_now)
	{
		quad (a, b, c, d, u0, v0, u1, v1, n);
	}
}

/* a square pillar from y0 to y1 (the texture every `tile` metres up) */
static void pillar (int part, float x, float z, float half, float y0, float y1, float tile)
{
	static const float dir[4][2] = {{0, -1}, {1, 0}, {0, 1}, {-1, 0}};	/* the faces' outward normals */
	for (int f = 0; f < 4; f++)
	{
		float nx = dir[f][0], nz = dir[f][1], tx = -nz, tz = nx;	/* along the face */
		float a[3] = {x + (nx - tx) * half, y0, z + (nz - tz) * half}, b[3] = {x + (nx + tx) * half, y0, z + (nz + tz) * half};
		float c[3] = {b[0], y1, b[2]}, d[3] = {a[0], y1, a[2]}, n[3] = {nx, 0, nz};
		pquad (part, a, b, c, d, 0, 0, fmaxf (1.0f, roundf (2 * half / tile)), (y1 - y0) / tile, n);
	}
}

/* how far apart two rings of a loop are */
static int ring_gap (int i, int j)
{
	int n = route[ROUTE_MAIN].n, d = abs (((i % n) + n) % n - ((j % n) + n) % n);
	return d < n - d ? d : n - d;
}

/* how near the road (either route's) passes a point on the ground, but for
   the rings about ring i of route r (-1: none left out) */
static float road_near (const float *p, int r, int i)
{
	float best = 1e9f;
	for (int q = 0; q < ROUTES; q++)
		for (int j = 0; j <= route[q].n - (route[q].loop ? 1 : 0); j++)
		{
			float dx = route[q].rings[j].p[0] - p[0], dz = route[q].rings[j].p[2] - p[2];
			if (q == r && (q == ROUTE_MAIN ? ring_gap (i, j) : abs (i - j)) <= 15)
			{
				continue;
			}
			best = fminf (best, dx * dx + dz * dz);
		}
	return sqrtf (best);
}

/* between rings i and j: the quad from profile point (a0, b0) to (a1, b1)
   (level: the b are heights), facing the road; its texture u0..u1 along
   the road, v0..v1 along the profile */
static void section (int part, int r, int i, int j, float a0, float b0, float a1, float b1, bool level,
		     float u0, float u1, float v0, float v1)
{
	if (part != part_now)
	{
		return;
	}
	void (*at) (float *, int, int, float, float) = level ? level_point : ring_point;
	float p0[3], p1[3], q0[3], q1[3], e1[3], e2[3], n[3], ref[3], d[3];
	at (p0, r, i, a0, b0);
	at (p1, r, i, a1, b1);
	at (q0, r, j, a0, b0);
	at (q1, r, j, a1, b1);
	v_sub (e1, q0, p0);
	v_sub (e2, p1, p0);
	v_cross (n, e1, e2);
	v_norm (n);
	v_mad (ref, ring (r, i)->p, ring (r, i)->u, 4.0f);		/* over the road's middle */
	v_sub (d, ref, p0);
	if (v_dot (n, d) < 0.0f)
	{
		v_set (n, -n[0], -n[1], -n[2]);
	}
	quad (p0, q0, q1, p1, u0, v0, u1, v1, n);
}

/* a quad in ring i's plane (ring frame), moved `ahead` along the road,
   facing `facing` along it (-1 back, towards the craft coming, 1 on) */
static void plane_quad (int part, int r, int i, float ahead, float facing, const float ab[4][2], float u0, float v0, float u1, float v1)
{
	float p[4][3], n[3];
	for (int k = 0; k < 4; k++)
	{
		ring_point (p[k], r, i, ab[k][0], ab[k][1]);
		v_mad (p[k], p[k], ring (r, i)->t, ahead);
	}
	v_set (n, facing * ring (r, i)->t[0], facing * ring (r, i)->t[1], facing * ring (r, i)->t[2]);
	pquad (part, p[0], p[1], p[2], p[3], u0, v0, u1, v1, n);
}

/* the start gate: a big screen on two pillars, over the road just past the line */
static void over (int i)
{
	art.over[((i % route[ROUTE_MAIN].n) + route[ROUTE_MAIN].n) % route[ROUTE_MAIN].n] = 1;
}

static void gate (void)
{
	int g = 2;
	over (g);
	float top = ring (0, g)->p[1] + 16.5f, bottom = ring (0, g)->p[1] + 7.5f, p[3];
	for (int s = -1; s <= 1; s += 2)
	{
		level_point (p, 0, g, s * (HW + 2.5f), 0.0f);
		pillar (P_CONCRETE, p[0], p[2], 0.8f, GROUND_Y, top + 0.5f, 4.0f);
	}
	for (int e = 0; e < 2; e++)
	{
		float q[4][3], n[3], sgn = e ? 1.0f : -1.0f, left = sgn * (HW + 3.3f);	/* (the reader's left) */
		level_point (q[0], 0, g, left, bottom);
		level_point (q[1], 0, g, -left, bottom);
		level_point (q[2], 0, g, -left, top);
		level_point (q[3], 0, g, left, top);
		for (int k = 0; k < 4; k++)
		{
			v_mad (q[k], q[k], ring (0, g)->t, sgn * 0.9f);	/* in front of the pillars (0.8) */
		}
		v_set (n, sgn * ring (0, g)->t[0], 0, sgn * ring (0, g)->t[2]);
		pquad (e ? P_BANNER2 : P_SCREEN, q[0], q[1], q[2], q[3], 0, 0, e ? 2 : 1, 1, n);
	}
	float b[4][3], down[3] = {0, -1, 0};
	level_point (b[0], 0, g, -(HW + 3.3f), bottom);
	level_point (b[1], 0, g, HW + 3.3f, bottom);
	v_copy (b[2], b[1]);
	v_copy (b[3], b[0]);
	v_mad (b[0], b[0], ring (0, g)->t, -0.9f);
	v_mad (b[1], b[1], ring (0, g)->t, -0.9f);
	v_mad (b[2], b[2], ring (0, g)->t, 0.9f);
	v_mad (b[3], b[3], ring (0, g)->t, 0.9f);
	pquad (P_METAL, b[0], b[1], b[2], b[3], 0, 0, 4, 1, down);
}

/* the stands by the start: a banner along the front, the crowd up the slope,
   a roof on posts (no wall behind: from the road it would be hidden, and a
   hidden pixel costs the GPU half a seen one) */
static void stands (void)
{
	const route_t *m = &route[ROUTE_MAIN];
	for (int i = -30; i < 14; i++)
	{
		if (fabsf (ring (0, i)->k) > 1.0f / 300 || fabsf (ring (0, i + 1)->k) > 1.0f / 300)
		{
			continue;
		}
		float front = ring (0, i)->p[1] + 1.5f, top = front + 12.0f, along = i * m->step;
		for (int s = -1; s <= 1; s += 2)
		{
			float fl = s * (HW + 7), bk = s * (HW + 27), dir = -s;	/* the banner reads along the road on the left */
			section (P_BANNER1, 0, i, i + 1, fl, GROUND_Y, fl, front, true, dir * along / 14.0f, dir * (along + m->step) / 14.0f, 0, 1);
			section (P_CROWD, 0, i, i + 1, fl, front, bk, top, true, along / 8.0f, (along + m->step) / 8.0f, 0, 5);
			section (P_METAL, 0, i, i + 1, s * (HW + 28), top + 4.0f, s * (HW + 13), top + 3.0f, true, along / 4.0f, (along + m->step) / 4.0f, 0, 4);
			if (((i % 3) + 3) % 3 == 0)
			{
				float p[3];
				level_point (p, 0, i, s * (HW + 14), 0.0f);
				pillar (P_METAL, p[0], p[2], 0.25f, front + 4.0f, top + 3.1f, 4.0f);
			}
		}
	}
}

/* the tunnel: a lining of lights from the walls up and over, ribs across, a
   portal each end with a sign */
static void tunnel (void)
{
	static const float prof[4][2] = {{HW + WALL_LEAN, WALL_H}, {HW + 1.2f, 5.2f}, {HW - 2.8f, 8.4f}, {0, 9.4f}};
	int first = track_features.tunnel_first, rings = track_features.tunnel_rings;
	float step = route[ROUTE_MAIN].step;
	over (first);
	over (first + rings);
	for (int i = first; i < first + rings; i++)
	{
		float along = i * step;
		for (int s = -1; s <= 1; s += 2)
		{
			for (int k = 0; k < 3; k++)
			{
				section (k == 1 ? P_ROOF : P_METAL, 0, i, i + 1, s * prof[k][0], prof[k][1], s * prof[k + 1][0], prof[k + 1][1], false,
					 along / 16.0f, (along + step) / 16.0f, 0, 1);
			}
			if ((i - first) % 4 != 2)
			{
				continue;
			}
			for (int k = 0; k < 3; k++)			/* a rib: the profile's edge, 0.5 m deep, 0.8 m wide */
			{
				float p0[3], p1[3], f0[3], f1[3], g0[3], g1[3], h0[3], h1[3], n[3], e[3], ref[3], d[3], len;
				ring_point (p0, 0, i, s * prof[k][0], prof[k][1]);
				ring_point (p1, 0, i, s * prof[k + 1][0], prof[k + 1][1]);
				v_sub (e, p1, p0);
				len = v_len (e);
				v_cross (n, ring (0, i)->t, e);
				v_norm (n);
				v_mad (ref, ring (0, i)->p, ring (0, i)->u, 4.0f);
				v_sub (d, ref, p0);
				if (v_dot (n, d) < 0.0f)
				{
					v_set (n, -n[0], -n[1], -n[2]);
				}
				v_mad (f0, p0, n, 0.5f);
				v_mad (f1, p1, n, 0.5f);
				v_mad (g0, p0, ring (0, i)->t, 0.8f);
				v_mad (g1, p1, ring (0, i)->t, 0.8f);
				v_mad (h0, f0, ring (0, i)->t, 0.8f);
				v_mad (h1, f1, ring (0, i)->t, 0.8f);
				float back[3] = {-ring (0, i)->t[0], -ring (0, i)->t[1], -ring (0, i)->t[2]};
				pquad (P_RIB, p0, f0, f1, p1, 0, 0, 1, len / 4, back);
				pquad (P_RIB, g0, h0, h1, g1, 0, 0, 1, len / 4, ring (0, i)->t);
				pquad (P_RIB, f0, h0, h1, f1, 0, 0, 1, len / 4, n);
			}
		}
	}
	/* the portals: the ends closed round the opening, down to the ground, and a sign */
	for (int e = 0; e < 2; e++)
	{
		int k = e ? first + rings : first;
		float facing = e ? 1.0f : -1.0f, bg = GROUND_Y - ring (0, k)->p[1];
		for (int s = -1; s <= 1; s += 2)
		{
			const float side[4][2] = {{s * (HW + 1.2f), bg}, {s * (HW + 5.0f), bg}, {s * (HW + 5.0f), 13}, {s * (HW + 1.2f), 13}};
			const float under[4][2] = {{s * HW, bg}, {s * (HW + 1.2f), bg}, {s * (HW + 1.2f), 5.2f}, {s * (HW + WALL_LEAN), WALL_H}};
			const float over[4][2] = {{s * (HW + 1.2f), 5.2f}, {s * (HW - 2.8f), 8.4f}, {s * (HW - 2.8f), 13}, {s * (HW + 1.2f), 13}};
			const float crown[4][2] = {{s * (HW - 2.8f), 8.4f}, {0, 9.4f}, {0, 13}, {s * (HW - 2.8f), 13}};
			plane_quad (P_CONCRETE, 0, k, 0.0f, facing, side, 0, 0, 1, (13 - bg) / 4);
			plane_quad (P_CONCRETE, 0, k, 0.0f, facing, under, 0, 0, 1, 2);
			plane_quad (P_CONCRETE, 0, k, 0.0f, facing, over, 0, 0, 1, 2);
			plane_quad (P_CONCRETE, 0, k, 0.0f, facing, crown, 0, 0, 1, 1);
		}
		float left = facing * (HW - 1);				/* (the reader's left) */
		const float sign[4][2] = {{left, 10.0f}, {-left, 10.0f}, {-left, 12.6f}, {left, 12.6f}};
		plane_quad (e ? P_BANNER2 : P_BANNER3, 0, k, facing * 0.15f, facing, sign, 0, 0, 1, 1);
	}
}

/* a hoop over the road: a half ring, 0.7 m thick, 1.2 m deep, on legs */
static void hoop (int h)
{
	const float R = HW + 2.2f, W = 0.7f, D = 1.2f;
	const ring_t *g = ring (0, h);
	over (h);
	for (int j = 0; j < 12; j++)
	{
		float a0 = PI * j / 12, a1 = PI * (j + 1) / 12;
		float i0[3], i1[3], o0[3], o1[3], i0d[3], i1d[3], o0d[3], o1d[3], n[3], back[3] = {-g->t[0], -g->t[1], -g->t[2]};
		ring_point (i0, 0, h, R * cosf (a0), R * sinf (a0));
		ring_point (i1, 0, h, R * cosf (a1), R * sinf (a1));
		ring_point (o0, 0, h, (R + W) * cosf (a0), (R + W) * sinf (a0));
		ring_point (o1, 0, h, (R + W) * cosf (a1), (R + W) * sinf (a1));
		v_mad (i0d, i0, g->t, D);
		v_mad (i1d, i1, g->t, D);
		v_mad (o0d, o0, g->t, D);
		v_mad (o1d, o1, g->t, D);
		float am = (a0 + a1) / 2, arc = R * PI / 12 / 4;
		for (int c = 0; c < 3; c++)
		{
			n[c] = -(g->r[c] * cosf (am) + g->u[c] * sinf (am));		/* inwards */
		}
		pquad (P_RIB, i0, i0d, i1d, i1, 0, 0, 1, arc, n);
		v_set (n, -n[0], -n[1], -n[2]);
		pquad (P_RIB, o0, o0d, o1d, o1, 0, 0, 1, arc, n);
		pquad (P_RIB, i0, o0, o1, i1, 0, 0, 1, arc, back);
		pquad (P_RIB, i0d, o0d, o1d, i1d, 0, 0, 1, arc, g->t);
	}
	if (g->p[1] > GROUND_Y + 1.0f)
	{
		for (int s = -1; s <= 1; s += 2)
		{
			float p[3];
			ring_point (p, 0, h, s * (R + W / 2), 0.0f);
			v_mad (p, p, g->t, D / 2);
			pillar (P_CONCRETE, p[0], p[2], 0.35f, GROUND_Y, p[1], 4.0f);
		}
	}
}

/* a board over the road on two posts, read by the craft coming */
static void board (int part, int i, float y0, float y1)
{
	float p[3];
	over (i);
	for (int s = -1; s <= 1; s += 2)
	{
		level_point (p, 0, i, s * (HW + 1.6f), 0.0f);
		pillar (P_METAL, p[0], p[2], 0.3f, GROUND_Y, ring (0, i)->p[1] + y1 + 0.3f, 4.0f);
	}
	const float face[4][2] = {{-(HW + 1.4f), y0}, {HW + 1.4f, y0}, {HW + 1.4f, y1}, {-(HW + 1.4f), y1}};
	plane_quad (part, 0, i, -0.35f, -1.0f, face, 0, 0, 1, 1);
	plane_quad (P_METAL, 0, i, 0.35f, 1.0f, face, 0, 0, 4, 1);
}

/* a banner beside the road at ring i, on side (-1 left, 1 right), facing the road */
static void banner (int part, int i, int side, float y0, float y1, float length)
{
	const ring_t *g = ring (0, i);
	float c[3], a[3], b[3], cc[3], d[3], n[3];
	v_mad (c, g->p, g->r, side * (HW + 5.0f));
	v_mad (a, c, g->t, side * length / 2);		/* read left to right from the road */
	v_mad (b, c, g->t, -side * length / 2);
	a[1] = b[1] = y0;
	v_copy (cc, b);
	v_copy (d, a);
	cc[1] = d[1] = y1;
	v_set (n, -side * g->r[0], 0, -side * g->r[2]);
	v_norm (n);
	pquad (part, a, b, cc, d, 0, 0, 1, 1, n);
}

/* the jump: the deck's ends faced with hazard stripes (the landing's is what
   a craft too slow flies at) */
static void jump_faces (void)
{
	int lip = track_features.gap_first, land = lip + track_features.gap_rings;
	const float face[4][2] = {{-HW, -3.5f}, {HW, -3.5f}, {HW, -0.02f}, {-HW, -0.02f}};
	plane_quad (P_HAZARD, 0, lip, 0.0f, 1.0f, face, 0, 0, 8, 1.75f);
	plane_quad (P_HAZARD, 0, land, 0.0f, -1.0f, face, 0, 0, 8, 1.75f);
}

/* the fork's noses: where the two roads part, and where they meet again */
static void fork_noses (void)
{
	const route_t *a = &route[ROUTE_ALT];
	for (int i = 1; i < a->n; i++)
	{
		bool open = a->rings[i].sep < OPEN_SEP, before = a->rings[i - 1].sep < OPEN_SEP;
		if (open != before)
		{
			float p[3];
			ring_point (p, ROUTE_ALT, open ? i - 1 : i, -a->side * (HW + 0.6f), 0.0f);
			pillar (P_HAZARD, p[0], p[2], 0.75f, p[1] - 1.0f, p[1] + 2.4f, 2.0f);
		}
	}
}

/* towers round the circuit, on a grid, where no road passes */
static void towers (void)
{
	const route_t *m = &route[ROUTE_MAIN];
	float lo[2] = {1e9f, 1e9f}, hi[2] = {-1e9f, -1e9f};
	for (int i = 0; i < m->n; i++)
	{
		lo[0] = fminf (lo[0], m->rings[i].p[0]);
		hi[0] = fmaxf (hi[0], m->rings[i].p[0]);
		lo[1] = fminf (lo[1], m->rings[i].p[2]);
		hi[1] = fmaxf (hi[1], m->rings[i].p[2]);
	}
	art.towers = part_now == P_TOWER ? 0 : art.towers;
	for (int gz = 0; gz < 12; gz++)
		for (int gx = 0; gx < 14; gx++)
		{
			uint32_t h = hash2 (gx, gz, 31);
			float p[3] = {lo[0] - 150.0f + gx * (hi[0] - lo[0] + 300.0f) / 13 + ((h & 255) / 255.0f - 0.5f) * 40.0f, 0,
				      lo[1] - 150.0f + gz * (hi[1] - lo[1] + 300.0f) / 11 + ((h >> 8 & 255) / 255.0f - 0.5f) * 40.0f};
			float half = 11.0f + (h >> 16 & 7) * 2.0f, tall = 28.0f + (h >> 19 & 31) * 3.6f;
			if ((h >> 24 & 3) == 0 || road_near (p, -1, 0) < 60.0f + half)
			{
				continue;
			}
			pillar (P_TOWER, p[0], p[2], half, GROUND_Y, tall, 16.0f);
			float a[3] = {p[0] - half, tall, p[2] - half}, b[3] = {p[0] + half, tall, p[2] - half};
			float c[3] = {p[0] + half, tall, p[2] + half}, d[3] = {p[0] - half, tall, p[2] + half}, up[3] = {0, 1, 0};
			pquad (P_METAL, a, b, c, d, 0, 0, 2, 2, up);
			art.towers += part_now == P_TOWER;
		}
}

/* all of it (the part part_now) */
static void scenery (void)
{
	const route_t *m = &route[ROUTE_MAIN];
	art.pylons = art.hoops = 0;
	for (int r = 0; r < ROUTES; r++)			/* pylons under the high deck */
		for (int i = 5; i < route[r].n; i += 10)
		{
			const ring_t *g = &route[r].rings[i];
			float under = g->p[1] - THICK;
			if (under > GROUND_Y + 3.0f && !(g->flags & RING_GAP) && road_near (g->p, r, i) > HW + 4.0f)
			{
				pillar (P_CONCRETE, g->p[0], g->p[2], 1.0f, GROUND_Y, under, 4.0f);
				art.pylons++;
			}
		}
	gate ();
	stands ();
	tunnel ();
	jump_faces ();
	fork_noses ();

	/* over the road: JUMP before the ramp; hoops after the landing and along
	   the first straight; the fork told by a board and by arrows beside it */
	int ramp = track_features.ramp_first, land = track_features.gap_first + track_features.gap_rings;
	int fork = (int) (route[ROUTE_ALT].from / m->step);
	board (P_BANNER4, ramp - 14, 6.5f, 9.5f);
	for (int k = 0; k < 4; k++)
	{
		hoop (land + 26 + k * 7);
		hoop (40 + k * 7);
		art.hoops += 2;
	}
	board (P_BANNER5, fork - 18, 6.5f, 9.0f);
	banner (P_BANNER5, fork - 4, route[ROUTE_ALT].side, ring (0, fork - 4)->p[1] + 1.8f, ring (0, fork - 4)->p[1] + 4.6f, 14.0f);

	/* banners beside the road, where it's clear */
	static const int banner_rings[] = {70, 120, 250, 300, 420, 470, 560, 610};
	for (unsigned b = 0; b < sizeof banner_rings / sizeof banner_rings[0]; b++)
	{
		int i = banner_rings[b] % m->n, side = b % 2 ? 1 : -1;
		float c[3];
		v_mad (c, ring (0, i)->p, ring (0, i)->r, side * (HW + 5.0f));
		if (!(ring (0, i)->flags & (RING_TUNNEL | RING_GAP)) && road_near (c, 0, i) > HW + 6.0f)
		{
			banner (P_BANNER1 + b % 3, i, side, ring (0, i)->p[1] + 2.0f, ring (0, i)->p[1] + 5.5f, 14.0f);
		}
	}
	towers ();
}

#define PROP_MAX	20000

static void build_props (void)
{
	glGenBuffers (1, &art.props);
	glBindBuffer (GL_ARRAY_BUFFER, art.props);
	glBufferData (GL_ARRAY_BUFFER, PROP_MAX * (GLsizeiptr) sizeof (vertex_t), NULL, GL_STATIC_DRAW);
	begin_vertices ();
	for (part_now = 0; part_now < PARTS; part_now++)
	{
		art.part_first[part_now] = vertices_so_far ();
		scenery ();
		art.part_count[part_now] = vertices_so_far () - art.part_first[part_now];
	}
	art.prop_vertices = vertices_so_far ();
	flush_chunk ();
}

/* the ground: a grid round the circuit, the texture every 48 m */
#define GROUND_GRID	32
#define GROUND_HALF	1100.0f

static void build_ground (void)
{
	const route_t *m = &route[ROUTE_MAIN];
	float lo[2] = {1e9f, 1e9f}, hi[2] = {-1e9f, -1e9f};
	for (int i = 0; i < m->n; i++)
	{
		lo[0] = fminf (lo[0], m->rings[i].p[0]);
		hi[0] = fmaxf (hi[0], m->rings[i].p[0]);
		lo[1] = fminf (lo[1], m->rings[i].p[2]);
		hi[1] = fmaxf (hi[1], m->rings[i].p[2]);
	}
	float gcx = (lo[0] + hi[0]) / 2, gcz = (lo[1] + hi[1]) / 2;
	glGenBuffers (1, &art.ground);
	glBindBuffer (GL_ARRAY_BUFFER, art.ground);
	glBufferData (GL_ARRAY_BUFFER, (GROUND_GRID + 1) * (GROUND_GRID + 1) * (GLsizeiptr) sizeof (vertex_t), NULL, GL_STATIC_DRAW);
	begin_vertices ();
	const float up[3] = {0, 1, 0};
	for (int z = 0; z <= GROUND_GRID; z++)
		for (int x = 0; x <= GROUND_GRID; x++)
		{
			float p[3] = {gcx + (2.0f * x / GROUND_GRID - 1) * GROUND_HALF, GROUND_Y, gcz + (2.0f * z / GROUND_GRID - 1) * GROUND_HALF};
			emit (p, p[0] / 48.0f, p[2] / 48.0f, shade_of (up));
		}
	flush_chunk ();
	static uint16_t grid_i[GROUND_GRID * GROUND_GRID * 6];
	for (int z = 0, k = 0; z < GROUND_GRID; z++)
		for (int x = 0; x < GROUND_GRID; x++)
		{
			uint16_t a = z * (GROUND_GRID + 1) + x, b = a + GROUND_GRID + 1;
			const uint16_t t[6] = {a, b, a + 1, a + 1, b, b + 1};
			memcpy (&grid_i[k], t, sizeof t);
			k += 6;
		}
	glGenBuffers (1, &art.ground_index);
	glBindBuffer (GL_ELEMENT_ARRAY_BUFFER, art.ground_index);
	glBufferData (GL_ELEMENT_ARRAY_BUFFER, sizeof grid_i, grid_i, GL_STATIC_DRAW);
	art.ground_indices = GROUND_GRID * GROUND_GRID * 6;
}

void art_build (const float sun_dir[3])
{
	static const float purple[3] = {0.08f, 0.05f, 0.28f}, violet[3] = {0.38f, 0.05f, 0.42f}, orange[3] = {1.0f, 0.50f, 0.05f};
	static const float ember[3] = {0.55f, 0.08f, 0.02f}, flame[3] = {0.95f, 0.45f, 0.05f}, white[3] = {0.95f, 0.95f, 0.95f};
	static const float navy[3] = {0.02f, 0.10f, 0.30f}, cyan[3] = {0.05f, 0.55f, 0.75f};
	static const float red[3] = {0.70f, 0.05f, 0.03f}, yellow[3] = {0.98f, 0.80f, 0.08f};
	static const float pine[3] = {0.02f, 0.25f, 0.10f}, green[3] = {0.10f, 0.75f, 0.30f};
	v_copy (sun, sun_dir);

	glActiveTexture (GL_TEXTURE0);
	art.t_kind[K_ROAD] = road_texture ();
	art.t_kind[K_WALL] = wall_texture ();
	art.t_kind[K_METAL] = art.t_part[P_METAL] = metal_texture ();
	coarse ();
	art.t_part[P_CONCRETE] = concrete_texture ();
	coarse ();
	art.t_part[P_ROOF] = roof_texture ();
	coarse ();
	art.t_part[P_RIB] = rib_texture ();
	art.t_part[P_CROWD] = crowd_texture ();
	coarse ();
	art.t_part[P_SCREEN] = screen_texture ();
	art.t_part[P_HAZARD] = hazard_texture ();
	coarse ();
	art.t_part[P_TOWER] = tower_texture ();
	coarse ();
	art.t_part[P_BANNER1] = banner_texture ("ZEROG", purple, violet, orange, 0.45f);
	art.t_part[P_BANNER2] = banner_texture ("PIEGPU", ember, flame, white, 0.45f);
	art.t_part[P_BANNER3] = banner_texture ("V3D", navy, cyan, white, 0.45f);
	art.t_part[P_BANNER4] = banner_texture ("JUMP", red, red, yellow, 0.2f);
	art.t_part[P_BANNER5] = banner_texture (">>>", pine, green, white, 0.2f);
	art.t_ground = ground_texture ();
	coarse ();
	art.t_shadow = shadow_texture ();
	art.t_stars = stars_texture ();
	art.t_flare = flare_texture ();
	art.t_ordnance = ordnance_texture ();
	for (int i = 0; i < CRAFTS; i++)
	{
		art.t_livery[i] = livery_texture (&team[i]);
	}

	build_meshes ();
	build_track ();
	build_props ();
	build_ground ();
}
