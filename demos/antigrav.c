/*
 * antigrav - anti-gravity racing, after WipEout: six craft race three laps of
 * a figure-of-eight circuit, hovering over the road, and the camera rides
 * behind one of them (the next one each race). They race themselves: each
 * takes the inside of the curve ahead, brakes to what the curve allows,
 * overtakes on the side with room, follows when it can't, and the speed
 * pads (the chevrons) throw it forward. Races take turns at night and by
 * day.
 *
 * Everything is made here at the start, textures included (no assets): the
 * circuit is a closed Catmull-Rom spline through CONTROL points, cut into
 * rings a few metres apart, banked into its curves, crossing itself on a
 * bridge. The road, its walls and the deck under it are triangle strips along
 * the rings; pylons hold the deck up where it's high. Beside and over the
 * road, placed where the circuit leaves room: stands full of people by the
 * start, a big screen over the line, a stadium roofed with lights over a
 * straight, gantries with boards hanging over the road, hoops, banners, and
 * chevrons on the outside of the tight curves. The textures are drawn texel
 * by texel (asphalt, chevrons, walls, concrete, the crowd, the screen,
 * banners with their words, the roof's lights, the ground, the stars, each
 * craft's livery) and mipmapped by the GPU.
 *
 * Little beyond textures: light per vertex, fog, the sky (flight's by day;
 * at night one with the stars), a shadow under each craft. Where a texture's
 * alpha is 0 it glows (its own light): the road's edges, the pads, the
 * screen, the roof's lights, the craft's engines, so the night is lit.
 *
 * At 1920x1080 the GPU's fill rate is the limit (as flight's): everything
 * is drawn near to far, the sky last (at the far plane, above the horizon
 * only; the screen is cleared to the haze it meets there), so the V3D's early
 * Z leaves what's hidden unshaded; the stars are in the sky's own pass, and
 * the scenery's shader has no dither.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pico/stdlib.h"
#include "gles/pgl.h"
#include "pgpu.h"
#include "hud.h"
#include "pgpu_perf.h"
#include "screen.h"
#include "mat4.h"
#include "sky_program.h"
#include "scenery_program.h"
#include "craft_program.h"
#include "nightsky_program.h"

#define PI		3.14159265f

/* the circuit, metres */
#define HW		7.0f		/* half the road's width */
#define WALL_H		1.3f
#define WALL_LEAN	0.35f		/* the walls lean out this much at the top */
#define THICK		0.8f		/* the deck under the road */
#define RING_STEP	4.0f		/* about: the rings are spaced evenly */
#define TILE_RINGS	4		/* rings a texture tile along the road and walls */
#define MAX_RINGS	768
#define SUB		48		/* spline samples a span between control points */
#define BANK_MAX	0.30f		/* radians */
#define GROUND_Y	0.0f
#define MAX_PADS	8

/* the race */
#define CRAFTS		6
#define LAPS		3
#define A_LAT		55.0f		/* m/s^2 the craft hold in a curve (each its own, about this) */
#define ACCEL		22.0f
#define BRAKE		45.0f
#define PAD_BOOST	28.0f		/* m/s over the speed, decaying */
#define HOVER		0.75f
#define GRID_SECONDS	4.0f
#define RESULTS_SECONDS	7.0f

/* the camera */
#define FOVY		62.0f
#define CAM_BACK	10.0f
#define CAM_UP		3.2f
#define CAM_AHEAD	8.0f
#define FOG		0.0022f		/* 1/m */

/* control points: x, z, height (the start line at the first) */
static const float control[][3] =
{
	{0, 0, 2}, {200, 0, 2}, {380, 20, 4}, {470, 110, 8}, {440, 230, 12}, {320, 270, 14},
	{200, 200, 15}, {60, 0, 15}, {-60, -160, 10}, {-220, -230, 4}, {-380, -160, 16},
	{-420, 0, 7}, {-330, 110, 3}, {-200, 90, 6}, {-110, 30, 3},
};
#define CONTROL		(int) (sizeof control / sizeof control[0])

/* the sky: day, and night (the lit is dimmed to the moon's light, the
   glowing isn't; stars); a race each */
typedef struct
{
	const char *name;
	float haze[3], zenith[3], light, fog;
	bool stars;
} sky_t;

static const sky_t skies[2] =
{
	{"day", {0.80f, 0.74f, 0.68f}, {0.22f, 0.40f, 0.70f}, 1.0f, FOG, false},
	{"night", {0.09f, 0.06f, 0.15f}, {0.01f, 0.01f, 0.04f}, 0.38f, 0.0028f, true},
};
static const sky_t *sky_now = &skies[0];
static float sun[3];

/* ---- vectors --------------------------------------------------------------------- */

static void v_set (float *r, float x, float y, float z)	{ r[0] = x; r[1] = y; r[2] = z; }
static float v_dot (const float *a, const float *b)	{ return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

static void v_cross (float *r, const float *a, const float *b)
{
	float t[3] = {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
	memcpy (r, t, sizeof t);
}

static void v_norm (float *v)
{
	float l = sqrtf (v_dot (v, v));
	if (l > 0.0f)
	{
		v[0] /= l;
		v[1] /= l;
		v[2] /= l;
	}
}

/* r = a + b * s */
static void v_mad (float *r, const float *a, const float *b, float s)
{
	v_set (r, a[0] + b[0] * s, a[1] + b[1] * s, a[2] + b[2] * s);
}

static float clampf (float x, float lo, float hi)	{ return x < lo ? lo : x > hi ? hi : x; }

/* ---- the circuit ------------------------------------------------------------------ */

typedef struct
{
	float p[3];			/* the centre line */
	float t[3], r[3], u[3];		/* forward, right, up (banked) */
	float k;			/* curvature, 1/m: > 0 turning right */
} ring_t;

static ring_t rings[MAX_RINGS];
static int n_rings;
static float track_len, step;
static bool pad_tile[MAX_RINGS / TILE_RINGS];
static int n_pads;

static void catmull (const float *p0, const float *p1, const float *p2, const float *p3, float t, float *out)
{
	for (int i = 0; i < 3; i++)
	{
		out[i] = 0.5f * (2 * p1[i] + (p2[i] - p0[i]) * t + (2 * p0[i] - 5 * p1[i] + 4 * p2[i] - p3[i]) * t * t
				 + (3 * p1[i] - p0[i] - 3 * p2[i] + p3[i]) * t * t * t);
	}
}

static void build_track (void)
{
	/* the spline, sampled densely, and the length along the samples */
	static float dense[CONTROL * SUB + 1][3], along[CONTROL * SUB + 1];
	float pts[CONTROL][3];
	for (int i = 0; i < CONTROL; i++)
	{
		v_set (pts[i], control[i][0], control[i][2], control[i][1]);
	}
	int n = CONTROL * SUB;
	for (int i = 0; i < CONTROL; i++)
		for (int j = 0; j < SUB; j++)
		{
			catmull (pts[(i + CONTROL - 1) % CONTROL], pts[i], pts[(i + 1) % CONTROL],
				 pts[(i + 2) % CONTROL], (float) j / SUB, dense[i * SUB + j]);
		}
	memcpy (dense[n], dense[0], sizeof dense[0]);
	along[0] = 0.0f;
	for (int i = 1; i <= n; i++)
	{
		float d[3] = {dense[i][0] - dense[i - 1][0], dense[i][1] - dense[i - 1][1], dense[i][2] - dense[i - 1][2]};
		along[i] = along[i - 1] + sqrtf (v_dot (d, d));
	}
	track_len = along[n];

	/* rings evenly along it, a whole number of texture tiles */
	n_rings = TILE_RINGS * (int) lroundf (track_len / (RING_STEP * TILE_RINGS));
	n_rings = n_rings > MAX_RINGS ? MAX_RINGS : n_rings;
	step = track_len / n_rings;
	for (int i = 0, j = 0; i < n_rings; i++)
	{
		float s = i * step;
		while (j + 1 < n && along[j + 1] < s)
		{
			j++;
		}
		float f = (s - along[j]) / (along[j + 1] - along[j]);
		for (int c = 0; c < 3; c++)
		{
			rings[i].p[c] = dense[j][c] + (dense[j + 1][c] - dense[j][c]) * f;
		}
	}

	/* the frames: forward, the level right (in r for now), the curvature */
	const float up[3] = {0, 1, 0};
	for (int i = 0; i < n_rings; i++)
	{
		const float *a = rings[(i + n_rings - 1) % n_rings].p, *b = rings[(i + 1) % n_rings].p;
		v_set (rings[i].t, b[0] - a[0], b[1] - a[1], b[2] - a[2]);
		v_norm (rings[i].t);
		v_cross (rings[i].r, rings[i].t, up);
		v_norm (rings[i].r);
	}
	for (int i = 0; i < n_rings; i++)
	{
		const float *a = rings[(i + n_rings - 1) % n_rings].t, *b = rings[(i + 1) % n_rings].t;
		float d[3] = {(b[0] - a[0]) / (2 * step), (b[1] - a[1]) / (2 * step), (b[2] - a[2]) / (2 * step)};
		rings[i].k = v_dot (d, rings[i].r);
	}

	/* banked into the curves (the curvature smoothed over 50 m), the right
	   side down in a right-hand curve */
	int half = (int) (25.0f / step);
	for (int i = 0; i < n_rings; i++)
	{
		float k = 0.0f;
		for (int j = -half; j <= half; j++)
		{
			k += rings[(i + j + n_rings) % n_rings].k;
		}
		k /= 2 * half + 1;
		float bank = clampf (atanf (k * 900.0f / 9.81f), -BANK_MAX, BANK_MAX);
		float level[3], u0[3];
		memcpy (level, rings[i].r, sizeof level);
		v_cross (u0, level, rings[i].t);
		for (int c = 0; c < 3; c++)
		{
			rings[i].r[c] = level[c] * cosf (bank) - u0[c] * sinf (bank);
		}
		v_cross (rings[i].u, rings[i].r, rings[i].t);
	}

	/* speed pads: a tile each where the road runs straight for 60 m, 300 m apart at least */
	float last = -1e9f;
	for (int tile = 3; tile < n_rings / TILE_RINGS - 3 && n_pads < MAX_PADS; tile++)
	{
		float kmax = 0.0f;
		for (int j = tile * TILE_RINGS; j < tile * TILE_RINGS + (int) (60.0f / step); j++)
		{
			kmax = fmaxf (kmax, fabsf (rings[j % n_rings].k));
		}
		float s = tile * TILE_RINGS * step;
		if (kmax < 1.0f / 250 && s - last > 300.0f)
		{
			pad_tile[tile] = true;
			n_pads++;
			last = s;
		}
	}
}

/* the frame at s along the centre line (between the rings) */
typedef struct
{
	float p[3], t[3], r[3], u[3], k;
} frame_t;

static void track_at (float s, frame_t *f)
{
	s = fmodf (s, track_len);
	s = s < 0.0f ? s + track_len : s;
	int i = (int) (s / step) % n_rings;
	float w = s / step - (int) (s / step);
	const ring_t *a = &rings[i], *b = &rings[(i + 1) % n_rings];
	for (int c = 0; c < 3; c++)
	{
		f->p[c] = a->p[c] + (b->p[c] - a->p[c]) * w;
		f->t[c] = a->t[c] + (b->t[c] - a->t[c]) * w;
		f->r[c] = a->r[c] + (b->r[c] - a->r[c]) * w;
		f->u[c] = a->u[c] + (b->u[c] - a->u[c]) * w;
	}
	v_norm (f->t);
	v_norm (f->r);
	v_norm (f->u);
	f->k = a->k + (b->k - a->k) * w;
}

static float curvature_at (float s)
{
	int i = (int) (fmodf (s + track_len, track_len) / step) % n_rings;
	return rings[i].k;
}

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

/* the texels being made: 32 KB, the largest texture (64 x 128 RGBA) */
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
	{'A', {".###.", "#...#", "#...#", "#####", "#...#", "#...#", "#...#"}},
	{'D', {"####.", "#...#", "#...#", "#...#", "#...#", "#...#", "####."}},
	{'E', {"#####", "#....", "#....", "####.", "#....", "#....", "#####"}},
	{'G', {".###.", "#...#", "#....", "#.###", "#...#", "#...#", ".###."}},
	{'I', {".###.", "..#..", "..#..", "..#..", "..#..", "..#..", ".###."}},
	{'N', {"#...#", "##..#", "#.#.#", "#..##", "#...#", "#...#", "#...#"}},
	{'P', {"####.", "#...#", "#...#", "####.", "#....", "#....", "#...."}},
	{'R', {"####.", "#...#", "#...#", "####.", "#.#..", "#..#.", "#...#"}},
	{'T', {"#####", "..#..", "..#..", "..#..", "..#..", "..#..", "..#.."}},
	{'U', {"#...#", "#...#", "#...#", "#...#", "#...#", "#...#", ".###."}},
	{'V', {"#...#", "#...#", "#...#", "#...#", "#...#", ".#.#.", "..#.."}},
	{'3', {"####.", "....#", "....#", ".###.", "....#", "....#", "####."}},
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

/* the road: 64 texels across (u), h along (v, a tile); the edges glow */
static void asphalt (int x, int y, int h, float c[4])
{
	float n = 0.17f + 0.06f * vnoise (x / 4.0f, y * 32.0f / h / 4.0f, 16, 8, 1) + 0.035f * (rnd2 (x, y, 2) - 0.5f);
	v_set (c, n * 0.94f, n, n * 1.08f);
	c[3] = 1.0f;
	if (y % (h / 2) == 0)			/* the seams between the deck's plates */
	{
		v_set (c, c[0] * 0.55f, c[1] * 0.55f, c[2] * 0.55f);
	}
	if (x <= 1 || x >= 62)			/* the glowing edges */
	{
		v_set (c, 0.15f, 0.80f, 0.95f);
		c[3] = 0.0f;
	}
	else if (x == 4 || x == 59)		/* the edge lines */
	{
		v_set (c, 0.85f, 0.85f, 0.82f);
	}
	else if ((x == 31 || x == 32) && y % (h / 2) < h / 4)	/* the dashes down the middle, a little lit */
	{
		v_set (c, 0.80f, 0.78f, 0.60f);
		c[3] = 0.6f;
	}
}

static GLuint road_texture (void)
{
	float c[4];
	for (int y = 0; y < 128; y++)
		for (int x = 0; x < 64; x++)
		{
			asphalt (x, y, 128, c);
			put (64, x, y, c);
		}
	return rgba_texture (64, 128);
}

/* a speed pad: glowing chevrons pointing along the road (v up), on the road */
static GLuint pad_texture (void)
{
	float c[4];
	for (int y = 0; y < 64; y++)
		for (int x = 0; x < 64; x++)
		{
			asphalt (x, y, 64, c);
			if (x >= 12 && x <= 51)
			{
				int d = (int) (fabsf (x - 31.5f) * 0.9f);
				int band = ((y + d) % 16 + 16) % 16;	/* (the tip at the largest v: pointing on) */
				if (x == 12 || x == 51)
				{
					v_set (c, 0.95f, 0.80f, 0.10f);
					c[3] = 0.0f;
				}
				else if (band < 7)
				{
					v_set (c, 1.0f, 0.45f + 0.05f * band, 0.05f);
					c[3] = 0.0f;
				}
			}
			put (64, x, y, c);
		}
	return rgba_texture (64, 64);
}

/* the start and finish: a band of checks at the tile's start */
static GLuint start_texture (void)
{
	float c[4];
	for (int y = 0; y < 64; y++)
		for (int x = 0; x < 64; x++)
		{
			asphalt (x, y, 64, c);
			if (y < 16 && x > 1 && x < 62)
			{
				float w = ((x / 8 + y / 8) & 1) ? 0.92f : 0.06f;
				v_set (c, w, w, w);
			}
			put (64, x, y, c);
		}
	return rgba_texture (64, 64);
}

/* the walls: 64 along (u, a tile), 32 up (v, row 0 at the bottom); the colour band glows */
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
		}
	return rgba_texture (64, 32);
}

/* the outside of the tight curves: chevrons pointing along the road (u) */
static GLuint chevron_texture (void)
{
	for (int y = 0; y < 32; y++)
		for (int x = 0; x < 64; x++)
		{
			int band = ((int) (x + fabsf (y - 15.5f) * 0.9f) % 16 + 16) % 16;	/* (the tip at the largest u: on) */
			float c[4] = {0.06f, 0.06f, 0.07f, 1.0f};
			if (y < 2 || y >= 30)
				v_set (c, 0.85f, 0.85f, 0.85f);
			else if (band < 8)
			{
				v_set (c, 1.0f, 0.78f, 0.05f);
				c[3] = 0.4f;			/* lit a little: seen at night */
			}
			put (64, x, y, c);
		}
	return rgba_texture (64, 32);
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

/* the big screen over the start: a starburst and PIEGPU, glowing, in a frame */
static GLuint screen_texture (void)
{
	for (int y = 0; y < 64; y++)
		for (int x = 0; x < 128; x++)
		{
			float dx = (x - 63.5f) / 64, dy = (y - 31.5f) / 32, a = atan2f (dy, dx), r = sqrtf (dx * dx + dy * dy);
			bool ray = (int) floorf ((a + PI) / (2 * PI) * 18) & 1;
			float f = clampf (1.3f - 0.6f * r, 0, 1);
			float c[4] = {1.0f * f, (ray ? 0.78f : 0.30f) * f, (ray ? 0.10f : 0.05f) * f, 0.0f};
			if (x < 3 || x >= 125 || y < 3 || y >= 61)
			{
				v_set (c, 0.12f, 0.12f, 0.14f);
				c[3] = 1.0f;
			}
			put (128, x, y, c);
		}
	const float shade[4] = {0.35f, 0.05f, 0.05f, 0.0f}, white[4] = {1.0f, 1.0f, 1.0f, 0.0f};
	int s = 3, x0 = (128 - text_width ("PIEGPU", s)) / 2, y0 = (64 - 7 * s) / 2;
	text (128, "PIEGPU", x0 + 2, y0 - 2, s, shade);
	text (128, "PIEGPU", x0, y0, s, white);
	return rgba_texture (128, 64);
}

/* a banner, 128 x 32: a word on a gradient between stripes, a little lit */
static GLuint banner_texture (const char *word, const float *from, const float *to, const float *stripe)
{
	for (int y = 0; y < 32; y++)
		for (int x = 0; x < 128; x++)
		{
			float f = x / 127.0f, c[4] = {from[0] + (to[0] - from[0]) * f, from[1] + (to[1] - from[1]) * f,
						      from[2] + (to[2] - from[2]) * f, 0.45f};
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
	const float white[4] = {0.97f, 0.97f, 0.97f, 0.3f};
	text (128, word, (128 - text_width (word, s)) / 2, (32 - 7 * s) / 2, s, white);
	return rgba_texture (128, 32);
}

/* the stadium's roof: plates with glowing light panels */
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
	return rgba_texture (64, 128);
}

/* the ground: grass and dry earth, 128 x 128, made 16 rows at a time */
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
				float c[3] = {0.28f + 0.24f * d, 0.38f + 0.08f * d, 0.18f + 0.13f * d};
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
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	mipmapped ();
	return t;
}

/* ---- the craft ------------------------------------------------------------------------ */

typedef struct
{
	const char *name;
	float base[3], stripe[3], accent[3];
	uint32_t hud;			/* its colour on the HUD */
	GLuint texture;
	float vmax, grip;		/* this race's: top speed, what it holds in a curve */
	float s, x, vx, v, boost;	/* along, across, speeds (m, m/s) */
	int lap;			/* laps done: -1 on the grid, behind the line */
	int tile;			/* the road tile it's on (the pads) */
	float lap_start, best_lap, finish;
	int place;			/* at the finish, 1 ..; 0 racing */
	float roll, yaw;
} craft_t;

static craft_t craft[CRAFTS] =
{
	{.name = "VOLTA", .base = {0.95f, 0.82f, 0.10f}, .stripe = {0.10f, 0.10f, 0.12f}, .accent = {0.90f, 0.20f, 0.10f}, .hud = HUD_RGBA (255, 215, 40, 255)},
	{.name = "KESTREL", .base = {0.85f, 0.12f, 0.12f}, .stripe = {0.95f, 0.95f, 0.95f}, .accent = {0.10f, 0.10f, 0.10f}, .hud = HUD_RGBA (255, 70, 60, 255)},
	{.name = "NOVA", .base = {0.15f, 0.35f, 0.90f}, .stripe = {0.95f, 0.95f, 0.95f}, .accent = {0.95f, 0.75f, 0.10f}, .hud = HUD_RGBA (90, 150, 255, 255)},
	{.name = "ORCA", .base = {0.10f, 0.10f, 0.12f}, .stripe = {0.90f, 0.90f, 0.92f}, .accent = {0.20f, 0.80f, 0.90f}, .hud = HUD_RGBA (60, 220, 240, 255)},
	{.name = "HALO", .base = {0.92f, 0.92f, 0.95f}, .stripe = {0.10f, 0.50f, 0.95f}, .accent = {0.90f, 0.10f, 0.50f}, .hud = HUD_RGBA (240, 240, 250, 255)},
	{.name = "ZEPHYR", .base = {0.10f, 0.70f, 0.35f}, .stripe = {0.10f, 0.10f, 0.10f}, .accent = {0.95f, 0.90f, 0.10f}, .hud = HUD_RGBA (60, 220, 110, 255)},
};

/* the livery, 64 x 64 seen from above: u across (0.5 the middle), v from the
   nose (0) to the tail (1); alpha 0 glows (the engines at the tail: the
   body's and the pods' backs) */
static GLuint livery_texture (const craft_t *c)
{
	for (int y = 0; y < 64; y++)
		for (int x = 0; x < 64; x++)
		{
			float u = (x + 0.5f) / 64, v = (y + 0.5f) / 64, cu = fabsf (u - 0.5f);
			float n = 0.9f + 0.1f * vnoise (x / 4.0f, y / 4.0f, 16, 16, 11);
			float col[3] = {c->base[0] * n, c->base[1] * n, c->base[2] * n}, a = 1.0f;
			if (cu > 0.36f)
				memcpy (col, c->accent, sizeof col);
			if (cu < 0.05f && v < 0.5f)
				memcpy (col, c->stripe, sizeof col);
			if (v > 0.45f && v < 0.75f && cu < 0.12f - 0.05f * (v - 0.45f) / 0.3f)
			{
				float h = v > 0.50f && v < 0.53f ? 0.35f : 0.0f;	/* the canopy's glint */
				v_set (col, 0.05f + h, 0.10f + h, 0.18f + h);
			}
			if (v > 0.92f)
			{
				if (cu < 0.25f)
					v_set (col, 1.0f, 0.85f, 0.55f);
				else
					v_set (col, 1.0f, 0.45f, 0.15f);
				a = 0.0f;
			}
			texels.t32[y * 64 + x] = rgba (col[0], col[1], col[2], a);
		}
	GLuint t = new_texture (64, 64, GL_RGBA, GL_UNSIGNED_BYTE, texels.t32);
	mipmapped ();
	return t;
}

/* the mesh: a long wedge with an engine pod each side (x right, y up, z
   back: the nose at -z), flat faces; the livery projected from above */
typedef struct
{
	float p[3], n[3], uv[2];
} craft_vertex_t;

#define CRAFT_TRIS	53
#define NOSE_Z		-3.4f
#define TAIL_Z		2.4f
static craft_vertex_t craft_mesh[CRAFT_TRIS * 3];
static int craft_verts;

/* a flat triangle, its normal away from centre (NULL: the body's) */
static void craft_tri (const float *a, const float *b, const float *c, bool fin, const float *centre)
{
	static const float body[3] = {0, 0.2f, 0};
	const float *v[3] = {a, b, c}, *o = centre ? centre : body;
	float e1[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]}, e2[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]}, n[3];
	v_cross (n, e1, e2);
	v_norm (n);
	float mid[3] = {(a[0] + b[0] + c[0]) / 3 - o[0], (a[1] + b[1] + c[1]) / 3 - o[1], (a[2] + b[2] + c[2]) / 3 - o[2]};
	if (v_dot (n, mid) < 0.0f)		/* outwards */
	{
		v_set (n, -n[0], -n[1], -n[2]);
	}
	for (int i = 0; i < 3; i++)
	{
		craft_vertex_t *cv = &craft_mesh[craft_verts++];
		memcpy (cv->p, v[i], sizeof cv->p);
		memcpy (cv->n, n, sizeof cv->n);
		cv->uv[0] = fin ? 0.95f : v[i][0] / 3.2f + 0.5f;
		cv->uv[1] = fin ? 0.80f : (v[i][2] - NOSE_Z) / (TAIL_Z - NOSE_Z);
	}
}

/* an engine pod: a box, x0..x1, y0..y1, z0..z1 */
static void craft_pod (float x0, float x1, float y0, float y1, float z0, float z1)
{
	float c[3] = {(x0 + x1) / 2, (y0 + y1) / 2, (z0 + z1) / 2}, p[8][3];
	for (int i = 0; i < 8; i++)
	{
		v_set (p[i], i & 1 ? x1 : x0, i & 2 ? y1 : y0, i & 4 ? z1 : z0);
	}
	static const int faces[6][4] = {{0, 1, 3, 2}, {4, 5, 7, 6}, {0, 1, 5, 4}, {2, 3, 7, 6}, {0, 2, 6, 4}, {1, 3, 7, 5}};
	for (int f = 0; f < 6; f++)
	{
		craft_tri (p[faces[f][0]], p[faces[f][1]], p[faces[f][2]], false, c);
		craft_tri (p[faces[f][0]], p[faces[f][2]], p[faces[f][3]], false, c);
	}
}

static void build_craft (void)
{
	static const float N[3] = {0, 0.22f, NOSE_Z}, Nb[3] = {0, 0.06f, NOSE_Z};
	static const float FL[3] = {-0.9f, 0.30f, -0.9f}, FR[3] = {0.9f, 0.30f, -0.9f};
	static const float FLb[3] = {-0.9f, 0.02f, -0.9f}, FRb[3] = {0.9f, 0.02f, -0.9f};
	static const float WL[3] = {-1.3f, 0.22f, 1.3f}, WR[3] = {1.3f, 0.22f, 1.3f};
	static const float WLb[3] = {-1.3f, 0.04f, 1.3f}, WRb[3] = {1.3f, 0.04f, 1.3f};
	static const float BL[3] = {-0.8f, 0.36f, 2.0f}, BR[3] = {0.8f, 0.36f, 2.0f};
	static const float BLb[3] = {-0.8f, 0.0f, 2.0f}, BRb[3] = {0.8f, 0.0f, 2.0f};
	static const float C1[3] = {0, 0.58f, -0.5f}, C2[3] = {0, 0.64f, 0.95f};
	static const float F1[3] = {0, 0.62f, 0.95f}, F2[3] = {0, 0.95f, 1.9f}, F3[3] = {0, 0.36f, 2.0f};
	/* the top */
	craft_tri (N, C1, FL, false, NULL);
	craft_tri (N, FR, C1, false, NULL);
	craft_tri (FL, C1, C2, false, NULL);
	craft_tri (FR, C2, C1, false, NULL);
	craft_tri (FL, C2, WL, false, NULL);
	craft_tri (FR, WR, C2, false, NULL);
	craft_tri (WL, C2, BL, false, NULL);
	craft_tri (WR, BR, C2, false, NULL);
	craft_tri (BL, C2, BR, false, NULL);
	/* the bottom */
	craft_tri (Nb, FRb, WRb, false, NULL);
	craft_tri (Nb, WRb, BRb, false, NULL);
	craft_tri (Nb, BRb, BLb, false, NULL);
	craft_tri (Nb, BLb, WLb, false, NULL);
	craft_tri (Nb, WLb, FLb, false, NULL);
	/* the sides round the outline, the tail (an engine) among them */
	const float *top[8] = {N, FL, WL, BL, BR, WR, FR, N}, *bot[8] = {Nb, FLb, WLb, BLb, BRb, WRb, FRb, Nb};
	for (int i = 0; i < 7; i++)
	{
		craft_tri (top[i], top[i + 1], bot[i + 1], false, NULL);
		craft_tri (top[i], bot[i + 1], bot[i], false, NULL);
	}
	/* the fin, and an engine pod each side, longer than the body */
	craft_tri (F1, F2, F3, true, NULL);
	craft_pod (-1.5f, -1.0f, 0.05f, 0.52f, -0.7f, TAIL_Z);
	craft_pod (1.0f, 1.5f, 0.05f, 0.52f, -0.7f, TAIL_Z);
}

/* ---- geometry: vertices through a small buffer into GL buffers ------------------------- */

typedef struct
{
	float x, y, z, u, v, shade;
} vertex_t;

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

static void emit (const float *p, float u, float v, float shade)
{
	vertex_t *o = &chunk[chunk_n++];
	o->x = p[0];
	o->y = p[1];
	o->z = p[2];
	o->u = u;
	o->v = v;
	o->shade = shade;
	if (chunk_n == CHUNK)
	{
		flush_chunk ();
	}
}

/* the strips along the rings: the road, the walls, the deck's sides and underside */
enum { S_ROAD, S_WALL_L, S_WALL_R, S_EDGE_L, S_UNDER, S_EDGE_R, STRIPS };

static void strip_vertex (int strip, int i, int side)
{
	const ring_t *g = &rings[i % n_rings];
	float along = (float) i / TILE_RINGS;		/* tiles: whole at the loop's end */
	float a = 0, b = 0, u = 0, v = 0, n[3], p[3];
	switch (strip)
	{
	case S_ROAD:
		a = side ? HW : -HW;
		u = (float) side;
		v = along;
		memcpy (n, g->u, sizeof n);
		break;
	case S_WALL_L: case S_WALL_R:
		a = (side ? HW + WALL_LEAN : HW) * (strip == S_WALL_L ? -1 : 1);
		b = side ? WALL_H : 0.0f;
		u = along;
		v = (float) side;
		v_set (n, g->r[0], g->r[1], g->r[2]);
		if (strip == S_WALL_R)
		{
			v_set (n, -n[0], -n[1], -n[2]);	/* both face the road */
		}
		break;
	case S_EDGE_L: case S_EDGE_R:
		a = strip == S_EDGE_L ? -HW : HW;
		b = side ? -THICK : 0.0f;
		u = side * 0.25f;
		v = along;
		v_set (n, g->r[0], g->r[1], g->r[2]);
		if (strip == S_EDGE_L)
		{
			v_set (n, -n[0], -n[1], -n[2]);	/* outwards */
		}
		break;
	case S_UNDER:
		a = side ? HW : -HW;
		b = -THICK;
		u = (float) side;
		v = along;
		v_set (n, -g->u[0], -g->u[1], -g->u[2]);
		break;
	}
	v_mad (p, g->p, g->r, a);
	v_mad (p, p, g->u, b);
	emit (p, u, v, shade_of (n));
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

/* ---- the scenery by the road and over it: made in parts, a texture each ------------------ */

enum { P_CONCRETE, P_METAL, P_CROWD, P_SCREEN, P_BANNER1, P_BANNER2, P_BANNER3, P_ROOF, P_RIB, PARTS };
static int part_now;			/* the part being made: the others' quads are left out */

static void pquad (int part, const float *a, const float *b, const float *c, const float *d, float u0, float v0,
		   float u1, float v1, const float *n)
{
	if (part == part_now)
	{
		quad (a, b, c, d, u0, v0, u1, v1, n);
	}
}

/* a square pillar from y0 to y1 */
static void pillar (int part, float x, float z, float half, float y0, float y1)
{
	static const float dir[4][2] = {{0, -1}, {1, 0}, {0, 1}, {-1, 0}};	/* the faces' outward normals */
	for (int f = 0; f < 4; f++)
	{
		float nx = dir[f][0], nz = dir[f][1], tx = -nz, tz = nx;	/* along the face */
		float a[3] = {x + (nx - tx) * half, y0, z + (nz - tz) * half}, b[3] = {x + (nx + tx) * half, y0, z + (nz + tz) * half};
		float c[3] = {b[0], y1, b[2]}, d[3] = {a[0], y1, a[2]}, n[3] = {nx, 0, nz};
		pquad (part, a, b, c, d, 0, 0, 1, (y1 - y0) / 4.0f, n);
	}
}

static int ring_gap (int i, int j)
{
	int d = abs (((i % n_rings) + n_rings) % n_rings - ((j % n_rings) + n_rings) % n_rings);
	return d < n_rings - d ? d : n_rings - d;
}

/* is the ground under p clear of the road elsewhere (not near ring i)? */
static bool clear_below (const float *p, int i)
{
	for (int j = 0; j < n_rings; j++)
	{
		float dx = rings[j].p[0] - p[0], dz = rings[j].p[2] - p[2];
		if (ring_gap (i, j) > 15 && dx * dx + dz * dz < (HW + 4) * (HW + 4))
		{
			return false;
		}
	}
	return true;
}

/* rings with no other part of the circuit near: alone within 30 m across and
   18 m up or down, wide within 50 m across at any height */
static bool alone[MAX_RINGS], wide[MAX_RINGS];

static void find_neighbours (void)
{
	for (int i = 0; i < n_rings; i++)
	{
		alone[i] = wide[i] = true;
		for (int j = 0; j < n_rings; j++)
		{
			float dx = rings[j].p[0] - rings[i].p[0], dy = rings[j].p[1] - rings[i].p[1], dz = rings[j].p[2] - rings[i].p[2];
			float h2 = dx * dx + dz * dz;
			if (ring_gap (i, j) <= 15)
			{
				continue;
			}
			if (h2 < 30.0f * 30.0f && fabsf (dy) < 18.0f)
			{
				alone[i] = false;
			}
			if (h2 < 50.0f * 50.0f)
			{
				wide[i] = false;
			}
		}
	}
}

static const ring_t *ring (int i)	{ return &rings[((i % n_rings) + n_rings) % n_rings]; }

/* a point by ring i: a across (right), b up the ring's (banked) up */
static void ring_point (float *p, int i, float a, float b)
{
	v_mad (p, ring (i)->p, ring (i)->r, a);
	v_mad (p, p, ring (i)->u, b);
}

/* ... a across the level, at the height y */
static void level_point (float *p, int i, float a, float y)
{
	float r[3] = {ring (i)->r[0], 0.0f, ring (i)->r[2]};
	v_norm (r);
	v_mad (p, ring (i)->p, r, a);
	p[1] = y;
}

/* between rings i and j: the quad from profile point (a0, b0) to (a1, b1)
   (level: the b are heights), facing the road; its texture u0..u1 along
   the road, v0..v1 along the profile */
static void section (int part, int i, int j, float a0, float b0, float a1, float b1, bool level,
		     float u0, float u1, float v0, float v1)
{
	if (part != part_now)
	{
		return;
	}
	void (*at) (float *, int, float, float) = level ? level_point : ring_point;
	float p0[3], p1[3], q0[3], q1[3], e1[3], e2[3], n[3], ref[3];
	at (p0, i, a0, b0);
	at (p1, i, a1, b1);
	at (q0, j, a0, b0);
	at (q1, j, a1, b1);
	v_set (e1, q0[0] - p0[0], q0[1] - p0[1], q0[2] - p0[2]);
	v_set (e2, p1[0] - p0[0], p1[1] - p0[1], p1[2] - p0[2]);
	v_cross (n, e1, e2);
	v_norm (n);
	v_mad (ref, ring (i)->p, ring (i)->u, 6.0f);		/* over the road's middle */
	if (n[0] * (ref[0] - p0[0]) + n[1] * (ref[1] - p0[1]) + n[2] * (ref[2] - p0[2]) < 0.0f)
	{
		v_set (n, -n[0], -n[1], -n[2]);
	}
	quad (p0, q0, q1, p1, u0, v0, u1, v1, n);
}

/* a quad in ring i's plane (ring frame), moved `ahead` along the road,
   facing `facing` along it (-1 back, towards the craft coming, 1 on) */
static void plane_quad (int part, int i, float ahead, float facing, const float ab[4][2], float u0, float v0, float u1, float v1)
{
	float p[4][3], n[3];
	for (int k = 0; k < 4; k++)
	{
		ring_point (p[k], i, ab[k][0], ab[k][1]);
		v_mad (p[k], p[k], ring (i)->t, ahead);
	}
	v_set (n, facing * ring (i)->t[0], facing * ring (i)->t[1], facing * ring (i)->t[2]);
	pquad (part, p[0], p[1], p[2], p[3], u0, v0, u1, v1, n);
}

/* where the features are (place_features) */
#define TUNNEL_RINGS	32		/* the stadium: 128 m */
#define MAX_GANTRIES	3
#define HOOPS		5
#define HOOP_GAP	6		/* rings */
static int tunnel_first = -1, hoop_first = -1, gantries[MAX_GANTRIES], n_gantries;
static bool stand_ring[MAX_RINGS];
static const int banner_rings[] = {40, 130, 210, 300, 390, 470, 540};

static bool straight (int i, int len, float kmax)
{
	for (int j = i; j < i + len; j++)
	{
		if (!alone[((j % n_rings) + n_rings) % n_rings] || fabsf (ring (j)->k) > kmax)
		{
			return false;
		}
	}
	return true;
}

/* within margin rings of a feature placed already (or of the start) */
static bool near_feature (int i, int margin)
{
	if (ring_gap (i, 0) < 30 + margin)
		return true;
	if (tunnel_first >= 0 && ring_gap (i, tunnel_first + TUNNEL_RINGS / 2) < TUNNEL_RINGS / 2 + margin)
		return true;
	for (int g = 0; g < n_gantries; g++)
		if (ring_gap (i, gantries[g]) < margin)
			return true;
	if (hoop_first >= 0 && ring_gap (i, hoop_first + HOOPS * HOOP_GAP / 2) < HOOPS * HOOP_GAP / 2 + margin)
		return true;
	return false;
}

static void place_features (void)
{
	find_neighbours ();
	/* the stadium: over 128 m of straight after the start */
	for (int i = 40; i < n_rings / 3 && tunnel_first < 0; i++)
	{
		if (straight (i, TUNNEL_RINGS + 1, 1.0f / 350))
		{
			tunnel_first = i;
		}
	}
	/* the stands: by the straight at the start, where nothing else passes near */
	for (int i = -26; i <= 12; i++)
	{
		int r = (i + n_rings) % n_rings;
		stand_ring[r] = wide[r] && fabsf (rings[r].k) < 1.0f / 300;
	}
	/* the gantries: on straights, 240 m apart at least */
	for (int i = 0; i < n_rings && n_gantries < MAX_GANTRIES; i++)
	{
		if (straight (i - 6, 13, 1.0f / 300) && !near_feature (i, 20) && (n_gantries == 0 || ring_gap (i, gantries[n_gantries - 1]) > 60))
		{
			gantries[n_gantries++] = i;
		}
	}
	/* the hoops: in a row on a straight */
	for (int i = 0; i < n_rings && hoop_first < 0; i++)
	{
		if (straight (i - 4, HOOPS * HOOP_GAP + 8, 1.0f / 200) && !near_feature (i + HOOPS * HOOP_GAP / 2, 25))
		{
			hoop_first = i;
		}
	}
}

/* the stands by the start: a banner along the front, the crowd up the slope,
   a wall behind, a roof on posts */
static void stands (void)
{
	for (int i = -26; i < 12; i++)
	{
		int r = (i + n_rings) % n_rings, r1 = (i + 1 + n_rings) % n_rings;
		if (!stand_ring[r] || !stand_ring[r1])
		{
			continue;
		}
		float front = ring (i)->p[1] + 1.5f, top = front + 12.0f, along = i * step;
		for (int s = -1; s <= 1; s += 2)
		{
			float fl = s * (HW + 7), bk = s * (HW + 27), dir = -s;	/* the banner reads along the road on the left */
			section (P_BANNER1, i, i + 1, fl, GROUND_Y, fl, front, true, dir * along / 14.0f, dir * (along + step) / 14.0f, 0, 1);
			section (P_CROWD, i, i + 1, fl, front, bk, top, true, along / 8.0f, (along + step) / 8.0f, 0, 5);
			section (P_CONCRETE, i, i + 1, bk, GROUND_Y, bk, top, true, along / 4.0f, (along + step) / 4.0f, 0, (top - GROUND_Y) / 4.0f);
			section (P_METAL, i, i + 1, s * (HW + 28), top + 4.0f, s * (HW + 13), top + 3.0f, true, along / 4.0f, (along + step) / 4.0f, 0, 4);
			if (((i % 3) + 3) % 3 == 0)
			{
				float p[3];
				level_point (p, i, s * (HW + 14), 0.0f);
				pillar (P_METAL, p[0], p[2], 0.25f, front + 4.0f, top + 3.1f);
			}
			bool first = !stand_ring[(i - 1 + n_rings) % n_rings], last = !stand_ring[(i + 2 + n_rings) % n_rings];
			for (int e = 0; e < 2; e++)
			{
				if (e == 0 ? !first : !last)
				{
					continue;
				}
				int k = e == 0 ? i : i + 1;
				float p[4][3], n[3];
				level_point (p[0], k, fl, GROUND_Y);
				level_point (p[1], k, bk, GROUND_Y);
				level_point (p[2], k, bk, top);
				level_point (p[3], k, fl, front);
				v_set (n, (e ? 1 : -1) * ring (k)->t[0], 0, (e ? 1 : -1) * ring (k)->t[2]);
				pquad (P_CONCRETE, p[0], p[1], p[2], p[3], 0, 0, 5, (top - GROUND_Y) / 4.0f, n);
			}
		}
	}
}

/* the stadium over the road: the crowd up the sides, a wall, a roof of lights,
   ribs across, a portal each end with a sign */
static void stadium (void)
{
	if (tunnel_first < 0)
	{
		return;
	}
	static const float prof[4][2] = {{HW + WALL_LEAN, WALL_H}, {HW + 9, 8}, {HW + 9, 12}, {0, 14}};
	static const int part[3] = {P_CROWD, P_CONCRETE, P_ROOF};
	for (int i = tunnel_first; i < tunnel_first + TUNNEL_RINGS; i++)
	{
		float along = i * step;
		for (int s = -1; s <= 1; s += 2)
		{
			for (int k = 0; k < 3; k++)
			{
				float v1 = k == 0 ? 4.0f : k == 1 ? 1.0f : 2.0f;
				section (part[k], i, i + 1, s * prof[k][0], prof[k][1], s * prof[k + 1][0], prof[k + 1][1], false,
					 along / (k == 2 ? 16.0f : 8.0f), (along + step) / (k == 2 ? 16.0f : 8.0f), 0, v1);
			}
			if ((i - tunnel_first) % 3 != 1)
			{
				continue;
			}
			for (int k = 0; k < 3; k++)			/* a rib: the profile's edge, 0.7 m deep, 1 m wide */
			{
				float p0[3], p1[3], f0[3], f1[3], g0[3], g1[3], h0[3], h1[3], n[3], e[3], ref[3], len;
				ring_point (p0, i, s * prof[k][0], prof[k][1]);
				ring_point (p1, i, s * prof[k + 1][0], prof[k + 1][1]);
				v_set (e, p1[0] - p0[0], p1[1] - p0[1], p1[2] - p0[2]);
				len = sqrtf (v_dot (e, e));
				v_cross (n, ring (i)->t, e);
				v_norm (n);
				v_mad (ref, ring (i)->p, ring (i)->u, 6.0f);
				if (n[0] * (ref[0] - p0[0]) + n[1] * (ref[1] - p0[1]) + n[2] * (ref[2] - p0[2]) < 0.0f)
				{
					v_set (n, -n[0], -n[1], -n[2]);
				}
				v_mad (f0, p0, n, 0.7f);
				v_mad (f1, p1, n, 0.7f);
				v_mad (g0, p0, ring (i)->t, 1.0f);
				v_mad (g1, p1, ring (i)->t, 1.0f);
				v_mad (h0, f0, ring (i)->t, 1.0f);
				v_mad (h1, f1, ring (i)->t, 1.0f);
				float back[3] = {-ring (i)->t[0], -ring (i)->t[1], -ring (i)->t[2]};
				pquad (P_RIB, p0, f0, f1, p1, 0, 0, 1, len / 4, back);
				pquad (P_RIB, g0, h0, h1, g1, 0, 0, 1, len / 4, ring (i)->t);
				pquad (P_RIB, f0, h0, h1, f1, 0, 0, 1, len / 4, n);
			}
		}
	}
	/* the portals: the ends closed round the opening, down to the ground, and a sign */
	for (int e = 0; e < 2; e++)
	{
		int k = e ? tunnel_first + TUNNEL_RINGS : tunnel_first;
		float facing = e ? 1.0f : -1.0f, bg = GROUND_Y - ring (k)->p[1];
		for (int s = -1; s <= 1; s += 2)
		{
			const float side[4][2] = {{s * (HW + 9), bg}, {s * (HW + 13), bg}, {s * (HW + 13), 17}, {s * (HW + 9), 17}};
			const float under[4][2] = {{s * HW, bg}, {s * (HW + 9), bg}, {s * (HW + 9), 8}, {s * (HW + WALL_LEAN), WALL_H}};
			const float over[4][2] = {{s * (HW + 9), 12}, {0, 14}, {0, 17}, {s * (HW + 9), 17}};
			plane_quad (P_CONCRETE, k, 0.0f, facing, side, 0, 0, 1, (17 - bg) / 4);
			plane_quad (P_CONCRETE, k, 0.0f, facing, under, 0, 0, 2, 2);
			plane_quad (P_CONCRETE, k, 0.0f, facing, over, 0, 0, 2, 1);
		}
		const float sign[4][2] = {{-(HW + 2), 14.2f}, {HW + 2, 14.2f}, {HW + 2, 16.8f}, {-(HW + 2), 16.8f}};
		plane_quad (P_BANNER2, k, facing * 0.15f, facing, sign, 0, 0, 1, 1);
	}
}

/* the start gate: a big screen on two pillars, over the road just past the line */
static void gate (void)
{
	int g = 2;
	float top = ring (g)->p[1] + 16.5f, bottom = ring (g)->p[1] + 7.5f, p[3];
	for (int s = -1; s <= 1; s += 2)
	{
		level_point (p, g, s * (HW + 2.5f), 0.0f);
		pillar (P_CONCRETE, p[0], p[2], 0.8f, GROUND_Y, top + 0.5f);
	}
	for (int e = 0; e < 2; e++)
	{
		float q[4][3], n[3], sgn = e ? 1.0f : -1.0f;
		level_point (q[0], g, -(HW + 3.3f), bottom);
		level_point (q[1], g, HW + 3.3f, bottom);
		level_point (q[2], g, HW + 3.3f, top);
		level_point (q[3], g, -(HW + 3.3f), top);
		for (int k = 0; k < 4; k++)
		{
			v_mad (q[k], q[k], ring (g)->t, sgn * 0.9f);	/* in front of the pillars (0.8) */
		}
		v_set (n, sgn * ring (g)->t[0], 0, sgn * ring (g)->t[2]);
		pquad (e ? P_BANNER1 : P_SCREEN, q[0], q[1], q[2], q[3], 0, 0, e ? 2 : 1, 1, n);
	}
	float b[4][3], down[3] = {0, -1, 0};
	level_point (b[0], g, -(HW + 3.3f), bottom);
	level_point (b[1], g, HW + 3.3f, bottom);
	memcpy (b[2], b[1], sizeof b[2]);
	memcpy (b[3], b[0], sizeof b[3]);
	v_mad (b[0], b[0], ring (g)->t, -0.9f);
	v_mad (b[1], b[1], ring (g)->t, -0.9f);
	v_mad (b[2], b[2], ring (g)->t, 0.9f);
	v_mad (b[3], b[3], ring (g)->t, 0.9f);
	pquad (P_METAL, b[0], b[1], b[2], b[3], 0, 0, 4, 1, down);
}

/* a gantry: two pillars, a beam, and two boards hanging from it, tilted
   down to the sides (a roof over the road, seen from the front) */
static void gantry (int g)
{
	float p[3];
	for (int s = -1; s <= 1; s += 2)
	{
		level_point (p, g, s * (HW + 2.2f), 0.0f);
		pillar (P_CONCRETE, p[0], p[2], 0.6f, GROUND_Y, ring (g)->p[1] + 13.8f);
	}
	const float beam[4][2] = {{-(HW + 2.8f), 12.8f}, {HW + 2.8f, 12.8f}, {HW + 2.8f, 13.8f}, {-(HW + 2.8f), 13.8f}};
	plane_quad (P_METAL, g, -0.5f, -1.0f, beam, 0, 0, 4, 1);
	plane_quad (P_METAL, g, 0.5f, 1.0f, beam, 0, 0, 4, 1);
	const float right[4][2] = {{0.2f, 9.6f}, {HW + 1.2f, 4.8f}, {HW + 1.2f, 7.8f}, {0.2f, 12.6f}};
	const float left[4][2] = {{-(HW + 1.2f), 4.8f}, {-0.2f, 9.6f}, {-0.2f, 12.6f}, {-(HW + 1.2f), 7.8f}};
	plane_quad (P_BANNER2, g, -0.6f, -1.0f, right, 0, 0, 1, 1);
	plane_quad (P_BANNER3, g, -0.6f, -1.0f, left, 0, 0, 1, 1);
	plane_quad (P_METAL, g, -0.5f, 1.0f, right, 0, 0, 2, 1);
	plane_quad (P_METAL, g, -0.5f, 1.0f, left, 0, 0, 2, 1);
}

/* a hoop over the road: a half ring, 0.7 m thick, 1.2 m deep, on legs */
static void hoop (int h)
{
	const float R = HW + 2.2f, W = 0.7f, D = 1.2f;
	const ring_t *g = ring (h);
	for (int j = 0; j < 12; j++)
	{
		float a0 = PI * j / 12, a1 = PI * (j + 1) / 12;
		float i0[3], i1[3], o0[3], o1[3], i0d[3], i1d[3], o0d[3], o1d[3], n[3], back[3] = {-g->t[0], -g->t[1], -g->t[2]};
		ring_point (i0, h, R * cosf (a0), R * sinf (a0));
		ring_point (i1, h, R * cosf (a1), R * sinf (a1));
		ring_point (o0, h, (R + W) * cosf (a0), (R + W) * sinf (a0));
		ring_point (o1, h, (R + W) * cosf (a1), (R + W) * sinf (a1));
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
			ring_point (p, h, s * (R + W / 2), 0.0f);
			v_mad (p, p, g->t, D / 2);
			pillar (P_CONCRETE, p[0], p[2], 0.35f, GROUND_Y, p[1]);
		}
	}
}

/* a banner beside the road at ring i, on side (-1 left, 1 right), facing the road */
static void banner (int part, int i, int side, float y0, float y1, float length)
{
	const ring_t *g = ring (i);
	float c[3], a[3], b[3], cc[3], d[3], n[3];
	v_mad (c, g->p, g->r, side * (HW + 5.0f));
	v_mad (a, c, g->t, side * length / 2);		/* read left to right from the road */
	v_mad (b, c, g->t, -side * length / 2);
	a[1] = b[1] = y0;
	memcpy (cc, b, sizeof cc);
	memcpy (d, a, sizeof d);
	cc[1] = d[1] = y1;
	v_set (n, -side * g->r[0], 0, -side * g->r[2]);
	v_norm (n);
	pquad (part, a, b, cc, d, 0, 0, 1, 1, n);
}

/* all of it (the part part_now); the pylons and banners counted */
static int pylons, banners;

static void scenery (void)
{
	pylons = banners = 0;
	for (int i = 5; i < n_rings; i += 10)		/* pylons under the high deck */
	{
		float under = rings[i].p[1] - THICK;
		if (under > GROUND_Y + 3.0f && clear_below (rings[i].p, i))
		{
			pillar (P_CONCRETE, rings[i].p[0], rings[i].p[2], 1.0f, GROUND_Y, under);
			pylons++;
		}
	}
	for (unsigned b = 0; b < sizeof banner_rings / sizeof banner_rings[0]; b++)
	{
		int i = banner_rings[b] % n_rings;
		if (rings[i].p[1] < 8.0f && clear_below (rings[i].p, i) && !near_feature (i, 8))
		{
			banner (P_BANNER1 + b % 3, i, b % 2 ? 1 : -1, rings[i].p[1] + 2.0f, rings[i].p[1] + 5.5f, 14.0f);
			banners++;
		}
	}
	stands ();
	stadium ();
	gate ();
	for (int g = 0; g < n_gantries; g++)
	{
		gantry (gantries[g]);
	}
	for (int h = 0; hoop_first >= 0 && h < HOOPS; h++)
	{
		hoop (hoop_first + h * HOOP_GAP);
	}
}

/* ---- the race ------------------------------------------------------------------------- */

enum { GRID, RACE, RESULTS };
static int phase, finished, follow = CRAFTS - 1, passes;	/* passes: changes of the order in a race */
static float phase_start, race_start;

static float total (const craft_t *c)	{ return c->lap * track_len + c->s; }

static void race_reset (float now)
{
	static uint32_t rng = 0x2545F491u;
	for (int i = 0; i < CRAFTS; i++)
	{
		craft_t *c = &craft[i];
		rng ^= rng << 13;
		rng ^= rng >> 17;
		rng ^= rng << 5;
		c->s = track_len - 12.0f - (i / 2) * 11.0f;	/* two by two behind the line */
		c->x = i % 2 ? 3.2f : -3.2f;
		c->vx = c->v = c->boost = 0.0f;
		c->lap = -1;
		c->tile = (int) (c->s / step) / TILE_RINGS;
		c->best_lap = c->finish = 0.0f;
		c->place = 0;
		c->roll = c->yaw = 0.0f;
		c->vmax = 100.0f + 12.0f * ((rng & 0xFF) / 255.0f);		/* each race its own */
		c->grip = A_LAT * (0.92f + 0.16f * ((rng >> 8 & 0xFF) / 255.0f));
	}
	finished = passes = 0;
	follow = (follow + 1) % CRAFTS;
	static unsigned races;
	sky_now = &skies[races++ % 2 ? 0 : 1];		/* night first */
	phase = GRID;
	phase_start = now;
}

static void craft_update (craft_t *c, float dt, float t_race)
{
	float s = c->s;

	/* the speed the curves ahead allow (after the finish: a lap of honour) */
	float kmax = 0.0f;
	for (float d = 0.0f; d <= 80.0f; d += 2 * step)
	{
		kmax = fmaxf (kmax, fabsf (curvature_at (s + d)));
	}
	float lead = 0.0f;
	for (int i = 0; i < CRAFTS; i++)
	{
		lead = fmaxf (lead, total (&craft[i]));
	}
	float behind = lead - total (c);
	float vt = c->vmax * (1.0f + clampf (behind / 600.0f, 0.0f, 0.03f));	/* (a little help far behind) */
	if (kmax > 1e-4f)
	{
		vt = fminf (vt, sqrtf (c->grip / kmax));
	}
	if (c->place)
	{
		vt = fminf (vt, 55.0f);
	}
	c->v = c->v < vt ? fminf (vt, c->v + ACCEL * dt) : fmaxf (vt, c->v - BRAKE * dt);
	c->boost *= expf (-dt / 1.2f);
	float ve = c->v + c->boost;

	/* the line: the inside of the curve ahead; round the craft in the way:
	   if it would go faster than that one (its own speed, not the one it's
	   held to), out to the side with room and past; behind it only while
	   they're in line */
	float xt = clampf (curvature_at (s + 30.0f) * 700.0f, -(HW - 2.0f), HW - 2.0f);
	for (int i = 0; i < CRAFTS; i++)
	{
		const craft_t *o = &craft[i];
		float ds = total (o) - total (c), dx = o->x - c->x;
		if (o == c)
		{
			continue;
		}
		if (ds > 0.0f && ds < 25.0f && fabsf (dx) < 2.8f)
		{
			float ov = o->v + o->boost;
			if (vt + c->boost > ov + 0.5f)
			{
				xt = o->x + (o->x > 0.0f ? -3.6f : 3.6f);
			}
			if (ds < 8.0f && fabsf (dx) < 2.6f)
			{
				c->v = fminf (c->v, o->v);
				c->boost = fminf (c->boost, o->boost);
			}
		}
		if (fabsf (ds) < 4.5f && fabsf (dx) < 2.4f)	/* side by side: a nudge apart */
		{
			c->vx += (dx > 0.0f ? -12.0f : 12.0f) * dt;
		}
	}
	float ax = 7.0f * (xt - c->x) - 4.0f * c->vx;
	c->vx += ax * dt;
	c->x += c->vx * dt;
	float lim = HW - 1.5f;
	if (fabsf (c->x) > lim)				/* the wall */
	{
		c->x = copysignf (lim, c->x);
		c->vx *= -0.3f;
		c->v *= 1.0f - 0.8f * dt;
	}

	/* along: laps and pads */
	c->s += ve * dt;
	if (c->s >= track_len)
	{
		c->s -= track_len;
		c->lap++;
		if (c->lap >= 1)
		{
			float lap = t_race - c->lap_start;
			c->best_lap = c->best_lap == 0.0f || lap < c->best_lap ? lap : c->best_lap;
		}
		c->lap_start = t_race;
		if (c->lap == LAPS && !c->place)
		{
			c->place = ++finished;
			c->finish = t_race;
		}
	}
	/* no passing through another: kept behind it, beside it pushed apart */
	for (int i = 0; i < CRAFTS; i++)
	{
		craft_t *o = &craft[i];
		float ds = total (o) - total (c), dx = o->x - c->x;
		if (o == c || fabsf (dx) >= 2.6f || fabsf (ds) >= 5.2f)
		{
			continue;
		}
		if (fabsf (ds) > 3.0f)		/* nose to tail: the one behind drops back */
		{
			craft_t *back = ds > 0.0f ? c : o, *front = ds > 0.0f ? o : c;
			back->s -= 5.2f - fabsf (ds);
			if (back->s < 0.0f)
			{
				back->s += track_len;
				back->lap--;
			}
			back->v = fminf (back->v, front->v);
			back->boost = fminf (back->boost, front->boost);
		}
		else				/* side by side: apart, and bounced */
		{
			float push = (2.6f - fabsf (dx)) / 2 * (dx > 0.0f ? 1.0f : -1.0f);
			c->x -= push;
			o->x += push;
			c->vx = -fabsf (c->vx) * (dx > 0.0f ? 1.0f : -1.0f);
			o->vx = fabsf (o->vx) * (dx > 0.0f ? 1.0f : -1.0f);
		}
	}
	int tile = (int) (c->s / step) / TILE_RINGS;
	if (tile != c->tile && pad_tile[tile])
	{
		c->boost = PAD_BOOST;
	}
	c->tile = tile;

	/* how it sits: banked into the curve and its sideways drift, turned into its path */
	float roll = clampf (curvature_at (c->s) * ve * ve * 0.0045f + c->vx * 0.05f, -0.55f, 0.55f);
	c->roll += (roll - c->roll) * (1.0f - expf (-dt * 6.0f));
	c->yaw = atan2f (c->vx, fmaxf (ve, 5.0f));
}

/* the order now: by places at the finish, then by distance */
static void standings (int order[CRAFTS])
{
	for (int i = 0; i < CRAFTS; i++)
	{
		order[i] = i;
	}
	for (int i = 1; i < CRAFTS; i++)
		for (int j = i; j > 0; j--)
		{
			const craft_t *a = &craft[order[j - 1]], *b = &craft[order[j]];
			bool swap = a->place && b->place ? b->place < a->place : b->place ? true : a->place ? false : total (b) > total (a);
			if (!swap)
			{
				break;
			}
			int t = order[j];
			order[j] = order[j - 1];
			order[j - 1] = t;
		}
}

/* the craft's model matrix, and the frame it's in */
static void craft_pose (const craft_t *c, float t, float model[16], frame_t *f)
{
	track_at (c->s, f);
	float cy = cosf (c->yaw), sy = sinf (c->yaw), cr = cosf (c->roll), sr = sinf (c->roll);
	float fw[3], rt[3], r2[3], u2[3], p[3];
	for (int i = 0; i < 3; i++)
	{
		fw[i] = f->t[i] * cy + f->r[i] * sy;
		rt[i] = f->r[i] * cy - f->t[i] * sy;
		r2[i] = rt[i] * cr - f->u[i] * sr;
		u2[i] = f->u[i] * cr + rt[i] * sr;
	}
	float hover = HOVER + 0.07f * sinf (t * 2.3f + (float) (c - craft) * 1.7f);
	v_mad (p, f->p, f->r, c->x);
	v_mad (p, p, f->u, hover);
	const float m[16] = {r2[0], r2[1], r2[2], 0, u2[0], u2[1], u2[2], 0, -fw[0], -fw[1], -fw[2], 0, p[0], p[1], p[2], 1};
	memcpy (model, m, sizeof m);
}

/* the shadow's: on the road under the craft, turned with it, not rolled */
static void shadow_pose (const craft_t *c, const frame_t *f, float model[16])
{
	float cy = cosf (c->yaw), sy = sinf (c->yaw), fw[3], rt[3], p[3];
	for (int i = 0; i < 3; i++)
	{
		fw[i] = f->t[i] * cy + f->r[i] * sy;
		rt[i] = f->r[i] * cy - f->t[i] * sy;
	}
	v_mad (p, f->p, f->r, c->x);
	v_mad (p, p, f->u, 0.06f);
	const float m[16] = {rt[0], rt[1], rt[2], 0, f->u[0], f->u[1], f->u[2], 0, -fw[0], -fw[1], -fw[2], 0, p[0], p[1], p[2], 1};
	memcpy (model, m, sizeof m);
}

static void format_time (char *out, size_t size, float t)
{
	int cs = (int) (t * 100.0f + 0.5f);
	snprintf (out, size, "%d:%02d.%02d", cs / 6000, cs / 100 % 60, cs % 100);
}

/* ---- drawing -------------------------------------------------------------------------- */

/* enable exactly these vertex arrays (-1: none) */
static void arrays (GLint a, GLint b, GLint c)
{
	for (GLint i = 0; i < 4; i++)
	{
		if (i == a || i == b || i == c)
			glEnableVertexAttribArray (i);
		else
			glDisableVertexAttribArray (i);
	}
}

typedef struct
{
	GLuint prog;
	GLint vp, model, eye, fog, haze, texture, pos, uv, shade;
} scenery_t;

static void scenery_pointers (const scenery_t *sc)
{
	arrays (sc->pos, sc->uv, sc->shade);
	glVertexAttribPointer (sc->pos, 3, GL_FLOAT, GL_FALSE, sizeof (vertex_t), (void *) 0);
	glVertexAttribPointer (sc->uv, 2, GL_FLOAT, GL_FALSE, sizeof (vertex_t), (void *) 12);
	glVertexAttribPointer (sc->shade, 1, GL_FLOAT, GL_FALSE, sizeof (vertex_t), (void *) 20);
}

/* the sky's colours and light, in the programs that use them */
static struct
{
	GLuint sky, sc, cp;
	GLint sky_haze, sky_zenith, sc_haze, sc_light, sc_fog, cp_haze, cp_light, cp_fog;
} sky_u;

static void apply_sky (void)
{
	glUseProgram (sky_u.sky);
	glUniform3fv (sky_u.sky_haze, 1, sky_now->haze);
	glUniform3fv (sky_u.sky_zenith, 1, sky_now->zenith);
	glUseProgram (sky_u.sc);
	glUniform3fv (sky_u.sc_haze, 1, sky_now->haze);
	glUniform1f (sky_u.sc_light, sky_now->light);
	glUniform1f (sky_u.sc_fog, sky_now->fog);
	glUseProgram (sky_u.cp);
	glUniform3fv (sky_u.cp_haze, 1, sky_now->haze);
	glUniform1f (sky_u.cp_light, sky_now->light);
	glUniform1f (sky_u.cp_fog, sky_now->fog);
	glClearColor (sky_now->haze[0], sky_now->haze[1], sky_now->haze[2], 1.0f);
}

/* a run of road (or wall) tiles with one texture */
typedef struct
{
	int first, tiles;
	GLuint texture;
} run_t;

int main (void)
{
	stdio_init_all ();
	pgpu_init ();
	printf ("\nantigrav: waiting for the RPi (READY)...\n");
	while (!pgpu_wait_ready (1000))
	{
	}
	pgpu_set_reply_phase (1);
	int tries = 0;
	while (!pglInit () && ++tries < 5)		/* the first reply can be missed */
	{
	}
	GLint vp[4] = {0};

	const float sun_el = 32.0f * PI / 180, sun_az = -40.0f * PI / 180;
	v_set (sun, cosf (sun_el) * sinf (sun_az), sinf (sun_el), -cosf (sun_el) * cosf (sun_az));
	build_track ();
	build_craft ();
	place_features ();

	/* textures (unit 0: shared with the HUD's font, bound for each draw) */
	glActiveTexture (GL_TEXTURE0);
	GLuint t_road = road_texture (), t_pad = pad_texture (), t_start = start_texture (), t_wall = wall_texture ();
	GLuint t_chevron = chevron_texture (), t_ground = ground_texture (), t_shadow = shadow_texture (), t_stars = stars_texture ();
	static const float purple[3] = {0.08f, 0.05f, 0.28f}, violet[3] = {0.38f, 0.05f, 0.42f}, orange[3] = {1.0f, 0.50f, 0.05f};
	static const float ember[3] = {0.55f, 0.08f, 0.02f}, flame[3] = {0.95f, 0.45f, 0.05f}, white[3] = {0.95f, 0.95f, 0.95f};
	static const float navy[3] = {0.02f, 0.10f, 0.30f}, cyan[3] = {0.05f, 0.55f, 0.75f};
	GLuint part_texture[PARTS];
	part_texture[P_CONCRETE] = concrete_texture ();
	part_texture[P_METAL] = metal_texture ();
	part_texture[P_CROWD] = crowd_texture ();
	part_texture[P_SCREEN] = screen_texture ();
	part_texture[P_BANNER1] = banner_texture ("PIEGPU", purple, violet, orange);
	part_texture[P_BANNER2] = banner_texture ("ANTIGRAV", ember, flame, white);
	part_texture[P_BANNER3] = banner_texture ("V3D", navy, cyan, white);
	part_texture[P_ROOF] = roof_texture ();
	part_texture[P_RIB] = rib_texture ();
	GLuint t_metal = part_texture[P_METAL];
	for (int i = 0; i < CRAFTS; i++)
	{
		craft[i].texture = livery_texture (&craft[i]);
	}

	/* the track's strips: STRIPS of (n_rings + 1) ring pairs */
	int strip_len = (n_rings + 1) * 2;
	GLuint track, props, ground, ground_index, mesh, shadow;
	GLuint *buffers[] = {&track, &props, &ground, &ground_index, &mesh, &shadow};
	for (unsigned i = 0; i < sizeof buffers / sizeof buffers[0]; i++)
	{
		glGenBuffers (1, buffers[i]);
	}
	glBindBuffer (GL_ARRAY_BUFFER, track);
	glBufferData (GL_ARRAY_BUFFER, STRIPS * strip_len * (GLsizeiptr) sizeof (vertex_t), NULL, GL_STATIC_DRAW);
	begin_vertices ();
	for (int strip = 0; strip < STRIPS; strip++)
		for (int i = 0; i <= n_rings; i++)
		{
			strip_vertex (strip, i, 0);
			strip_vertex (strip, i, 1);
		}
	flush_chunk ();

	/* the road's runs: the start tile, the pads, the plain road between */
	static run_t runs[2 * MAX_PADS + 4];
	int n_runs = 0, tiles = n_rings / TILE_RINGS;
	for (int tile = 0; tile < tiles; tile++)
	{
		GLuint tex = tile == 0 ? t_start : pad_tile[tile] ? t_pad : t_road;
		if (n_runs && runs[n_runs - 1].texture == tex && tex == t_road)
		{
			runs[n_runs - 1].tiles++;
		}
		else
		{
			runs[n_runs++] = (run_t) {tile, 1, tex};
		}
	}

	/* the walls' runs: chevrons on the outside of the tight curves */
	static run_t wall_runs[2][MAX_RINGS / TILE_RINGS];
	int n_wall_runs[2] = {0, 0};
	for (int side = 0; side < 2; side++)
		for (int tile = 0; tile < tiles; tile++)
		{
			float k = 0.0f;
			for (int j = 0; j < TILE_RINGS; j++)
			{
				k += rings[tile * TILE_RINGS + j].k / TILE_RINGS;
			}
			bool outside = side == 0 ? k > 1.0f / 170 : k < -1.0f / 170;	/* the left wall: a right-hand curve's */
			GLuint tex = outside ? t_chevron : t_wall;
			run_t *r = n_wall_runs[side] ? &wall_runs[side][n_wall_runs[side] - 1] : NULL;
			if (r && r->texture == tex)
			{
				r->tiles++;
			}
			else
			{
				wall_runs[side][n_wall_runs[side]++] = (run_t) {tile, 1, tex};
			}
		}

	/* the scenery by the road: a part (a texture) after another */
	#define PROP_MAX	16384
	glBindBuffer (GL_ARRAY_BUFFER, props);
	glBufferData (GL_ARRAY_BUFFER, PROP_MAX * (GLsizeiptr) sizeof (vertex_t), NULL, GL_STATIC_DRAW);
	begin_vertices ();
	int part_first[PARTS], part_count[PARTS];
	for (part_now = 0; part_now < PARTS; part_now++)
	{
		part_first[part_now] = vertices_so_far ();
		scenery ();
		part_count[part_now] = vertices_so_far () - part_first[part_now];
	}
	int scenery_vertices = vertices_so_far ();
	flush_chunk ();

	/* the ground: a grid round the circuit, the texture every 48 m */
	#define GROUND_GRID	32
	#define GROUND_HALF	1000.0f
	float lo[2] = {1e9f, 1e9f}, hi[2] = {-1e9f, -1e9f};
	for (int i = 0; i < n_rings; i++)
	{
		lo[0] = fminf (lo[0], rings[i].p[0]);
		hi[0] = fmaxf (hi[0], rings[i].p[0]);
		lo[1] = fminf (lo[1], rings[i].p[2]);
		hi[1] = fmaxf (hi[1], rings[i].p[2]);
	}
	float gcx = (lo[0] + hi[0]) / 2, gcz = (lo[1] + hi[1]) / 2;
	glBindBuffer (GL_ARRAY_BUFFER, ground);
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
	glBindBuffer (GL_ELEMENT_ARRAY_BUFFER, ground_index);
	glBufferData (GL_ELEMENT_ARRAY_BUFFER, sizeof grid_i, grid_i, GL_STATIC_DRAW);

	/* the craft's mesh and the shadow's quad */
	glBindBuffer (GL_ARRAY_BUFFER, mesh);
	glBufferData (GL_ARRAY_BUFFER, craft_verts * (GLsizeiptr) sizeof (craft_vertex_t), craft_mesh, GL_STATIC_DRAW);
	static const vertex_t shadow_quad[4] =
	{
		{-1.9f, 0, -3.6f, 0, 0, 1}, {1.9f, 0, -3.6f, 1, 0, 1}, {-1.9f, 0, 2.7f, 0, 1, 1}, {1.9f, 0, 2.7f, 1, 1, 1},
	};
	glBindBuffer (GL_ARRAY_BUFFER, shadow);
	glBufferData (GL_ARRAY_BUFFER, sizeof shadow_quad, shadow_quad, GL_STATIC_DRAW);

	/* programs */
	GLuint sky = glCreateProgram ();
	glProgramBinaryOES (sky, PGL_PROGRAM_BINARY_PGPU, &sky_info, sizeof sky_info);
	GLint s_pos = glGetAttribLocation (sky, "a_pos");
	GLint s_horizon = glGetUniformLocation (sky, "u_horizon");
	GLint s_sun = glGetUniformLocation (sky, "u_sun");
	GLint s_aspect = glGetUniformLocation (sky, "u_aspect");
	sky_u.sky = sky;
	sky_u.sky_haze = glGetUniformLocation (sky, "u_haze");
	sky_u.sky_zenith = glGetUniformLocation (sky, "u_zenith");

	scenery_t sc;
	sc.prog = glCreateProgram ();
	glProgramBinaryOES (sc.prog, PGL_PROGRAM_BINARY_PGPU, &scenery_info, sizeof scenery_info);
	sc.vp = glGetUniformLocation (sc.prog, "u_vp");
	sc.model = glGetUniformLocation (sc.prog, "u_model");
	sc.eye = glGetUniformLocation (sc.prog, "u_eye");
	sc.fog = glGetUniformLocation (sc.prog, "u_fog");
	sc.haze = glGetUniformLocation (sc.prog, "u_haze");
	sc.texture = glGetUniformLocation (sc.prog, "u_texture");
	sc.pos = glGetAttribLocation (sc.prog, "a_pos");
	sc.uv = glGetAttribLocation (sc.prog, "a_uv");
	sc.shade = glGetAttribLocation (sc.prog, "a_shade");
	glUseProgram (sc.prog);
	glUniform1i (sc.texture, 0);
	sky_u.sc = sc.prog;
	sky_u.sc_haze = sc.haze;
	sky_u.sc_light = glGetUniformLocation (sc.prog, "u_light");
	sky_u.sc_fog = sc.fog;

	GLuint cp = glCreateProgram ();
	glProgramBinaryOES (cp, PGL_PROGRAM_BINARY_PGPU, &craft_info, sizeof craft_info);
	GLint c_vp = glGetUniformLocation (cp, "u_vp");
	GLint c_model = glGetUniformLocation (cp, "u_model");
	GLint c_eye = glGetUniformLocation (cp, "u_eye");
	GLint c_pos = glGetAttribLocation (cp, "a_pos");
	GLint c_normal = glGetAttribLocation (cp, "a_normal");
	GLint c_uv = glGetAttribLocation (cp, "a_uv");
	glUseProgram (cp);
	glUniform3fv (glGetUniformLocation (cp, "u_sun_dir"), 1, sun);
	glUniform1i (glGetUniformLocation (cp, "u_texture"), 0);
	sky_u.cp = cp;
	sky_u.cp_haze = glGetUniformLocation (cp, "u_haze");
	sky_u.cp_light = glGetUniformLocation (cp, "u_light");
	sky_u.cp_fog = glGetUniformLocation (cp, "u_fog");

	GLuint night = glCreateProgram ();		/* the sky at night: with the stars */
	glProgramBinaryOES (night, PGL_PROGRAM_BINARY_PGPU, &nightsky_info, sizeof nightsky_info);
	GLint n_pos = glGetAttribLocation (night, "a_pos"), n_dir = glGetAttribLocation (night, "a_dir");
	GLint n_horizon = glGetUniformLocation (night, "u_horizon");
	glUseProgram (night);
	glUniform3fv (glGetUniformLocation (night, "u_haze"), 1, skies[1].haze);
	glUniform3fv (glGetUniformLocation (night, "u_zenith"), 1, skies[1].zenith);
	glUniform1i (glGetUniformLocation (night, "u_stars"), 0);
	glBindTexture (GL_TEXTURE_2D, t_stars);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);	/* (points: not blurred away far off) */

	GLuint quad_buffer;
	glGenBuffers (1, &quad_buffer);
	glBindBuffer (GL_ARRAY_BUFFER, quad_buffer);
	glBufferData (GL_ARRAY_BUFFER, 4 * 5 * sizeof (float), NULL, GL_DYNAMIC_DRAW);	/* the sky's: each frame */

	glDisable (GL_DITHER);
	glDisable (GL_CULL_FACE);
	if (!hud_init ())
	{
		printf ("antigrav: the HUD program didn't link\n");
	}
	float kmax = 0.0f;
	for (int i = 0; i < n_rings; i++)
	{
		kmax = fmaxf (kmax, fabsf (rings[i].k));
	}
	int stand_rings = 0;
	for (int i = 0; i < n_rings; i++)
	{
		stand_rings += stand_ring[i];
	}
	printf ("antigrav: a circuit of %.0f m (%d rings, the tightest curve %.0f m round), %d speed pads, %d pylons, "
		"%d banners, stands along %.0f m, a stadium %s, %d gantries, %s; %d scenery vertices (of %d); %d craft, "
		"%d laps\n", track_len, n_rings, 1.0f / kmax, n_pads, pylons, banners, stand_rings * step,
		tunnel_first >= 0 ? "over 128 m" : "(no room)", n_gantries, hoop_first >= 0 ? "5 hoops" : "no hoops",
		scenery_vertices, PROP_MAX, CRAFTS, LAPS);

	/* the race */
	absolute_time_t start = get_absolute_time (), last = start;
	race_reset (0.0f);
	apply_sky ();
	float projection[16], cam_f[3] = {0, 0, -1}, identity[16];
	mat4_identity (identity);
	bool cam_init = false;
	perf_t perf;
	memset (&perf, 0, sizeof perf);
	unsigned frame = 0, windows = 0;
	bool new_perf = true;
	int last_phase = -1;

	while (true)
	{
		if (screen_update ("antigrav", vp))
		{
			float aspect = (float) vp[2] / vp[3];
			mat4_perspective (projection, FOVY, aspect, 0.4f, 1500.0f);
			glUseProgram (sky);
			glUniform1f (s_aspect, aspect);
		}
		absolute_time_t now = get_absolute_time ();
		float t = absolute_time_diff_us (start, now) / 1e6f;
		float dt = absolute_time_diff_us (last, now) / 1e6f;
		dt = dt > 1.0f / 20 ? 1.0f / 20 : dt;
		last = now;

		/* the race's phases: the grid's countdown, the race, the results */
		float in_phase = t - phase_start;
		if (phase == GRID && in_phase >= GRID_SECONDS)
		{
			phase = RACE;
			phase_start = race_start = t;
		}
		else if (phase == RACE && craft[follow].place && (finished == CRAFTS || t - craft[follow].finish - race_start > 10.0f))
		{
			phase = RESULTS;
			phase_start = t;
			int order[CRAFTS];
			standings (order);
			printf ("antigrav: results:");
			for (int i = 0; i < CRAFTS; i++)
			{
				const craft_t *c = &craft[order[i]];
				printf (" %d %s %.2f s (best lap %.2f),", i + 1, c->name, c->place ? c->finish : -1.0f, c->best_lap);
			}
			printf (" %d passes\n", passes);
		}
		else if (phase == RESULTS && in_phase >= RESULTS_SECONDS)
		{
			race_reset (t);
			apply_sky ();
			cam_init = false;
		}
		if (phase != GRID)
		{
			int n = (int) ceilf (dt / 0.01f);
			for (int k = 0; k < n; k++)
				for (int i = 0; i < CRAFTS; i++)
				{
					craft_update (&craft[i], dt / n, t - race_start);
				}
		}

		/* the camera: behind the craft followed, level, turning with the road ahead */
		const craft_t *me = &craft[follow];
		frame_t fr, ahead;
		track_at (me->s + 10.0f, &ahead);
		if (!cam_init)
		{
			memcpy (cam_f, ahead.t, sizeof cam_f);
			cam_init = true;
		}
		float w = 1.0f - expf (-dt * 4.0f);
		for (int i = 0; i < 3; i++)
		{
			cam_f[i] += (ahead.t[i] - cam_f[i]) * w;
		}
		v_norm (cam_f);
		track_at (me->s, &fr);
		float pos[3], eye[3], centre[3], view[16], vpm[16];
		v_mad (pos, fr.p, fr.r, me->x);
		v_mad (pos, pos, fr.u, HOVER);
		v_mad (eye, pos, cam_f, -CAM_BACK);
		eye[1] += CAM_UP;
		v_mad (centre, pos, cam_f, CAM_AHEAD);
		centre[1] += 1.0f;
		mat4_look_at (view, eye, centre, up);
		mat4_multiply (vpm, projection, view);

		/* the horizon's row and the sun's place on the screen */
		float hf[3] = {cam_f[0], 0.0f, cam_f[2]};
		v_norm (hf);
		const float far[3] = {eye[0] + 1000.0f * hf[0], eye[1], eye[2] + 1000.0f * hf[2]};
		float horizon = (vpm[1] * far[0] + vpm[5] * far[1] + vpm[9] * far[2] + vpm[13])
			      / (vpm[3] * far[0] + vpm[7] * far[1] + vpm[11] * far[2] + vpm[15]);
		float sx = vpm[0] * sun[0] + vpm[4] * sun[1] + vpm[8] * sun[2];
		float sy = vpm[1] * sun[0] + vpm[5] * sun[1] + vpm[9] * sun[2];
		float sw = vpm[3] * sun[0] + vpm[7] * sun[1] + vpm[11] * sun[2];

		/* near to far: the craft, the road, the scenery, the ground, then the
		   sky, behind all of it; the V3D's early Z (gpu/geometry.cpp) rejects
		   what's hidden before shading it. The screen is cleared to the haze,
		   which the sky meets at the horizon (a clear costs the GPU nothing) */
		glClear (GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
		glEnable (GL_DEPTH_TEST);

		/* the craft */
		float models[CRAFTS][16];
		frame_t frames[CRAFTS];
		glUseProgram (cp);
		glUniformMatrix4fv (c_vp, 1, GL_FALSE, vpm);
		glUniform3fv (c_eye, 1, eye);
		glActiveTexture (GL_TEXTURE0);
		glBindBuffer (GL_ARRAY_BUFFER, mesh);
		arrays (c_pos, c_normal, c_uv);
		glVertexAttribPointer (c_pos, 3, GL_FLOAT, GL_FALSE, sizeof (craft_vertex_t), (void *) 0);
		glVertexAttribPointer (c_normal, 3, GL_FLOAT, GL_FALSE, sizeof (craft_vertex_t), (void *) 12);
		glVertexAttribPointer (c_uv, 2, GL_FLOAT, GL_FALSE, sizeof (craft_vertex_t), (void *) 24);
		for (int i = 0; i < CRAFTS; i++)
		{
			craft_pose (&craft[i], t, models[i], &frames[i]);
			glUniformMatrix4fv (c_model, 1, GL_FALSE, models[i]);
			glBindTexture (GL_TEXTURE_2D, craft[i].texture);
			glDrawArrays (GL_TRIANGLES, 0, craft_verts);
		}

		/* the road, its walls and deck, the scenery by it, the ground */
		glUseProgram (sc.prog);
		glUniformMatrix4fv (sc.vp, 1, GL_FALSE, vpm);
		glUniformMatrix4fv (sc.model, 1, GL_FALSE, identity);
		glUniform3fv (sc.eye, 1, eye);
		glBindBuffer (GL_ARRAY_BUFFER, track);
		scenery_pointers (&sc);
		for (int r = 0; r < n_runs; r++)
		{
			glBindTexture (GL_TEXTURE_2D, runs[r].texture);
			glDrawArrays (GL_TRIANGLE_STRIP, S_ROAD * strip_len + runs[r].first * TILE_RINGS * 2,
				      (runs[r].tiles * TILE_RINGS + 1) * 2);
		}
		for (int side = 0; side < 2; side++)
			for (int r = 0; r < n_wall_runs[side]; r++)
			{
				glBindTexture (GL_TEXTURE_2D, wall_runs[side][r].texture);
				glDrawArrays (GL_TRIANGLE_STRIP, (side ? S_WALL_R : S_WALL_L) * strip_len
					      + wall_runs[side][r].first * TILE_RINGS * 2, (wall_runs[side][r].tiles * TILE_RINGS + 1) * 2);
			}
		glBindTexture (GL_TEXTURE_2D, t_metal);
		for (int strip = S_EDGE_L; strip <= S_EDGE_R; strip++)
		{
			glDrawArrays (GL_TRIANGLE_STRIP, strip * strip_len, strip_len);
		}

		glBindBuffer (GL_ARRAY_BUFFER, props);
		scenery_pointers (&sc);
		for (int part = 0; part < PARTS; part++)
		{
			if (part_count[part])
			{
				glBindTexture (GL_TEXTURE_2D, part_texture[part]);
				glDrawArrays (GL_TRIANGLES, part_first[part], part_count[part]);
			}
		}

		glBindBuffer (GL_ARRAY_BUFFER, ground);
		glBindBuffer (GL_ELEMENT_ARRAY_BUFFER, ground_index);
		scenery_pointers (&sc);
		glBindTexture (GL_TEXTURE_2D, t_ground);
		glDrawElements (GL_TRIANGLES, GROUND_GRID * GROUND_GRID * 6, GL_UNSIGNED_SHORT, (void *) 0);

		/* the sky above the horizon, at the far plane: only where nothing is */
		float bottom = clampf (horizon - 0.02f, -1.0f, 1.0f);
		if (bottom < 1.0f)
		{
			/* the corners, and (at night, for the stars) the direction seen at each */
			float corners[4][5], ty = tanf (FOVY * PI / 360), tx = ty * vp[2] / vp[3];
			for (int k = 0; k < 4; k++)
			{
				float x = k & 1 ? 1.0f : -1.0f, y = k & 2 ? 1.0f : bottom;
				corners[k][0] = x;
				corners[k][1] = y;
				for (int c = 0; c < 3; c++)	/* forward, right, up: the view's rows */
				{
					corners[k][2 + c] = -view[c * 4 + 2] + x * tx * view[c * 4] + y * ty * view[c * 4 + 1];
				}
			}
			glBindBuffer (GL_ARRAY_BUFFER, quad_buffer);
			glBufferSubData (GL_ARRAY_BUFFER, 0, sizeof corners, corners);
			if (sky_now->stars)
			{
				glUseProgram (night);
				glUniform1f (n_horizon, horizon);
				glBindTexture (GL_TEXTURE_2D, t_stars);
				arrays (n_pos, n_dir, -1);
				glVertexAttribPointer (n_pos, 2, GL_FLOAT, GL_FALSE, sizeof corners[0], (void *) 0);
				glVertexAttribPointer (n_dir, 3, GL_FLOAT, GL_FALSE, sizeof corners[0], (void *) 8);
			}
			else
			{
				glUseProgram (sky);
				glUniform1f (s_horizon, horizon);
				glUniform2f (s_sun, sw > 0.01f ? sx / sw : 10.0f, sw > 0.01f ? sy / sw : 10.0f);
				arrays (s_pos, -1, -1);
				glVertexAttribPointer (s_pos, 2, GL_FLOAT, GL_FALSE, sizeof corners[0], (void *) 0);
			}
			glDepthMask (GL_FALSE);
			glDrawArrays (GL_TRIANGLE_STRIP, 0, 4);
			glDepthMask (GL_TRUE);
		}

		/* the craft's shadows, blended onto the road */
		glUseProgram (sc.prog);
		glBindBuffer (GL_ARRAY_BUFFER, shadow);
		scenery_pointers (&sc);
		glBindTexture (GL_TEXTURE_2D, t_shadow);
		glEnable (GL_BLEND);
		glBlendFunc (GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
		glDepthMask (GL_FALSE);
		for (int i = 0; i < CRAFTS; i++)
		{
			float m[16];
			shadow_pose (&craft[i], &frames[i], m);
			glUniformMatrix4fv (sc.model, 1, GL_FALSE, m);
			glDrawArrays (GL_TRIANGLE_STRIP, 0, 4);
		}
		glDepthMask (GL_TRUE);
		glDisable (GL_BLEND);

		/* the HUD: the race on the left, the countdown or the results in the
		   middle, the numbers at the top right (10 times a second) */
		int order[CRAFTS];
		static int last_order[CRAFTS];
		standings (order);
		if (phase == RACE && t - race_start > 1.0f)	/* (the grid sorting itself out isn't passing) */
		{
			for (int i = 0; i < CRAFTS; i++)
			{
				if (order[i] != last_order[i])
				{
					passes++;
					break;
				}
			}
		}
		memcpy (last_order, order, sizeof order);
		if (new_perf || frame % 6 == 0 || phase != last_phase)
		{
			float hs = vp[3] >= 1000 ? 1.5f : vp[3] >= 600 ? 1.0f : 0.5f;	/* the font 3x, 2x, 1x */
			float lh = HUD_CHAR_H * hs * 1.15f, cw = HUD_CHAR_W * hs;
			char s[48], tm[16];
			int pos_now = 1;
			for (int i = 0; i < CRAFTS; i++)
			{
				if (order[i] == follow)
					pos_now = i + 1;
			}
			float race_t = phase == GRID ? 0.0f : (me->place ? me->finish : t - race_start);
			hud_begin ();
			hud_rect (0, 0, 4 + 13 * cw, 4 + 5 * lh, HUD_RGBA (0, 0, 0, 140));
			hud_text_scaled (4, 3, me->name, me->hud, hs);
			snprintf (s, sizeof s, "POS %d/%d", pos_now, CRAFTS);
			hud_text_scaled (4, 3 + lh, s, HUD_RGBA (255, 255, 255, 255), hs);
			snprintf (s, sizeof s, "LAP %d/%d", me->lap < 0 ? 1 : me->lap + 1 > LAPS ? LAPS : me->lap + 1, LAPS);
			hud_text_scaled (4, 3 + 2 * lh, s, HUD_RGBA (255, 255, 255, 255), hs);
			format_time (tm, sizeof tm, race_t);
			snprintf (s, sizeof s, "TIME %s", tm);
			hud_text_scaled (4, 3 + 3 * lh, s, HUD_RGBA (255, 230, 120, 255), hs);
			if (me->best_lap > 0.0f)
				format_time (tm, sizeof tm, me->best_lap);
			else
				strcpy (tm, "-:--.--");
			snprintf (s, sizeof s, "BEST %s", tm);
			hud_text_scaled (4, 3 + 4 * lh, s, HUD_RGBA (200, 200, 200, 255), hs);
			snprintf (s, sizeof s, "%3d KM/H", (int) lroundf ((me->v + me->boost) * 3.6f));
			hud_rect (0, vp[3] - 2 * HUD_CHAR_H * hs - 6, 4 + 8 * 2 * cw, 2 * HUD_CHAR_H * hs + 6, HUD_RGBA (0, 0, 0, 140));
			hud_text_scaled (4, vp[3] - 2 * HUD_CHAR_H * hs - 3, s,
					 me->boost > 3.0f ? HUD_RGBA (255, 170, 40, 255) : HUD_RGBA (255, 255, 255, 255), 2 * hs);
			const char *big = NULL;
			if (phase == GRID && in_phase > GRID_SECONDS - 3.0f)
			{
				snprintf (s, sizeof s, "%d", (int) ceilf (GRID_SECONDS - in_phase));
				big = s;
			}
			else if (phase == RACE && t - race_start < 1.0f)
			{
				big = "GO";
			}
			if (big)
			{
				float bs = 4 * hs;
				hud_text_scaled (floorf ((vp[2] - strlen (big) * HUD_CHAR_W * bs) / 2), floorf (vp[3] / 3.0f), big,
						 HUD_RGBA (255, 220, 60, 255), bs);
			}
			if (phase == RESULTS)
			{
				float x0 = floorf ((vp[2] - 20 * cw) / 2), y0 = floorf (vp[3] / 4.0f);
				hud_rect (x0 - 6, y0 - 6, 20 * cw + 12, (CRAFTS + 1.5f) * lh + 12, HUD_RGBA (0, 0, 0, 170));
				hud_text_scaled (x0, y0, "RESULTS", HUD_RGBA (255, 220, 60, 255), hs);
				for (int i = 0; i < CRAFTS; i++)
				{
					const craft_t *c = &craft[order[i]];
					if (c->place)
						format_time (tm, sizeof tm, c->finish);
					else
						strcpy (tm, "-:--.--");
					snprintf (s, sizeof s, "%d %-8s %s", i + 1, c->name, tm);
					hud_text_scaled (x0, floorf (y0 + (i + 1.5f) * lh), s, c->hud, hs);
				}
			}
			hud_perf (vp[2] - hud_perf_width (0.5f) - 2, 2, 0.5f, &perf);
			hud_end ();
			last_phase = phase;
		}
		hud_draw ();
		pglSwapBuffers ();

		absolute_time_t wait_start = get_absolute_time ();
		pgpu_wait_frame (100);			/* pace on the screen */
		new_perf = perf_frame (absolute_time_diff_us (wait_start, get_absolute_time ()), &perf);
		frame++;
		if (new_perf && ++windows % 5 == 0)
		{
			perf_log_link ("antigrav", &perf);
			GLenum e = glGetError ();
			printf ("antigrav: %.1f fps, load GPU %.0f%% CPU-G %.0f%% CPU-H %.0f%%, render %.2f ms; %s lap %d, "
				"%.0f km/h, leader %s; GL error 0x%x\n", perf.fps, perf.gpu * 100, perf.cpu_g * 100,
				perf.cpu_h * 100, perf.render_ms, me->name, me->lap < 0 ? 1 : me->lap + 1 > LAPS ? LAPS : me->lap + 1, (me->v + me->boost) * 3.6f,
				craft[order[0]].name, (unsigned) e);
		}
	}
}
