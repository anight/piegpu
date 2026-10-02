/*
 * aquarium - a tank of tropical fish, seen through its front glass.
 *
 * Everything is made here at the start but the caustics and the sounds
 * (tools/make_aquarium_assets.py): the fish's mesh (a body of nine oval
 * rings, a forked tail, the fins) and five species' skins painted texel by
 * texel (a clownfish, a blue tang, a yellow tang, a neon tetra, an
 * angelfish); the sand, the rocks, the plants' blades, the glass behind. On
 * the bottom, of the fish's kind of vertex and drawn by its program: a
 * starfish that creeps about a rock's front, and two shrimps that walk the
 * sand and dart off backwards when a fish comes too near (or at a knock).
 *
 * What makes it water: a fish swims by a wave that runs down its body to its
 * tail (the vertex shader's), faster as it speeds up; the plants sway; the
 * caustics (the net of light a rippled surface throws) move over the sand,
 * the rocks and the fishes' backs; the light comes down in shafts; everything
 * fades into the water's colour with the distance, darker towards the floor;
 * bubbles rise from two air stones; the surface glitters from below.
 *
 * The fish: each keeps to its kind's depth and roams (a point to swim to,
 * another every few seconds), away from the glass, the rocks and each other;
 * the tetras keep together as a school. A pellet of food brings the ones near
 * it; a knock on the glass sends them off.
 *
 * The stick (engine/pad.h) looks around and comes closer; the controller's A
 * feeds, B, X or the stick pressed knocks, Y hides the numbers. A finger on
 * the panel knocks where it touches. The console's keys: a d w s look, f
 * feeds, k knocks, h hides the numbers. Left alone, the view drifts and the
 * fish are fed now and then.
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
#include "pad.h"
#include "aqua_program.h"
#include "fish_program.h"
#include "bubbles_program.h"
#include "shafts_program.h"
#include "texview_program.h"
#include "aquarium_assets.h"

#define PI		3.14159265f

/* the tank, inside: x across, y up from the sand, z towards the front glass */
#define TANK_X		8.0f		/* half its width */
#define TANK_Y		6.0f		/* the water's depth */
#define TANK_Z		4.0f		/* half its depth: where the fish are */
#define FRONT_Z		19.0f		/* the sand, the surface and the side glass go on to here, past the eye:
					   the picture is all water */

#define FOVY		40.0f
#define EYE_FAR		15.5f		/* the eye from the tank's middle; the stick comes closer */
#define EYE_NEAR	10.5f
#define FOG_DENSITY	0.05f
#define IDLE_S		20.0f
#define KEY_HOLD_S	0.3f

static const float deep[3] = {0.02f, 0.13f, 0.21f}, shallow[3] = {0.08f, 0.38f, 0.48f};

static float clampf (float x, float lo, float hi)	{ return x < lo ? lo : x > hi ? hi : x; }

static uint32_t rng = 0x51F15EEDu;
static float frand (void)		/* 0 .. 1 */
{
	rng ^= rng << 13;
	rng ^= rng >> 17;
	rng ^= rng << 5;
	return (rng >> 8) / 16777216.0f;
}

/* ---- textures ----------------------------------------------------------------------- */

static uint16_t texels[64 * 64];	/* RGB565 */

static void put (int w, int x, int y, float r, float g, float b)
{
	r = clampf (r, 0.0f, 1.0f);
	g = clampf (g, 0.0f, 1.0f);
	b = clampf (b, 0.0f, 1.0f);
	texels[y * w + x] = (uint16_t) ((unsigned) (r * 31.0f + 0.5f) << 11 | (unsigned) (g * 63.0f + 0.5f) << 5
					| (unsigned) (b * 31.0f + 0.5f));
}

static GLuint texture (int w, int h, bool repeat)
{
	GLuint t;
	glGenTextures (1, &t);
	glBindTexture (GL_TEXTURE_2D, t);
	glPixelStorei (GL_UNPACK_ALIGNMENT, 2);
	glTexImage2D (GL_TEXTURE_2D, 0, GL_RGB, w, h, 0, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, texels);
	glGenerateMipmap (GL_TEXTURE_2D);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, repeat ? GL_REPEAT : GL_CLAMP_TO_EDGE);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, repeat ? GL_REPEAT : GL_CLAMP_TO_EDGE);
	return t;
}

/* a number for a place, 0 .. 1; and smooth noise of them that goes round every `wrap` */
static float hash (int x, int y)
{
	uint32_t h = (uint32_t) x * 374761393u + (uint32_t) y * 668265263u;
	h = (h ^ (h >> 13)) * 1274126177u;
	return ((h ^ (h >> 16)) & 0xFFFF) / 65535.0f;
}

static float noise (float x, float y, int wrap)
{
	int xi = (int) floorf (x), yi = (int) floorf (y);
	float fx = x - xi, fy = y - yi;
	fx = fx * fx * (3 - 2 * fx);
	fy = fy * fy * (3 - 2 * fy);
	int x0 = ((xi % wrap) + wrap) % wrap, y0 = ((yi % wrap) + wrap) % wrap, x1 = (x0 + 1) % wrap, y1 = (y0 + 1) % wrap;
	float a = hash (x0, y0) + (hash (x1, y0) - hash (x0, y0)) * fx, b = hash (x0, y1) + (hash (x1, y1) - hash (x0, y1)) * fx;
	return a + (b - a) * fy;
}

static GLuint sand_texture (void)
{
	for (int y = 0; y < 64; y++)
		for (int x = 0; x < 64; x++)
		{
			float grain = hash (x, y), dune = noise (x / 8.0f, y / 8.0f, 8), ripple = sinf ((y + 5.0f * dune) * PI / 8.0f);
			float l = 0.78f + 0.16f * (grain - 0.5f) + 0.10f * (dune - 0.5f) + 0.05f * ripple;
			put (64, x, y, 0.93f * l, 0.84f * l, 0.62f * l);
		}
	return texture (64, 64, true);
}

static GLuint rock_texture (void)
{
	for (int y = 0; y < 64; y++)
		for (int x = 0; x < 64; x++)
		{
			float big = noise (x / 16.0f, y / 16.0f, 4), fine = noise (x / 4.0f, y / 4.0f, 16), crack = noise (x / 8.0f + 9, y / 8.0f + 3, 8);
			float l = 0.36f + 0.30f * big + 0.18f * fine - (fabsf (crack - 0.5f) < 0.04f ? 0.16f : 0.0f);
			float moss = big > 0.62f ? (big - 0.62f) * 1.6f : 0.0f;		/* (green where it's lightest: algae) */
			put (64, x, y, l * (0.95f - moss), l * (0.88f + 0.3f * moss), l * (0.78f - 0.6f * moss));
		}
	return texture (64, 64, true);
}

/* the glass behind and at the sides: the dark of the room through the water */
static GLuint glass_texture (void)
{
	for (int y = 0; y < 32; y++)
		for (int x = 0; x < 32; x++)
		{
			float l = 0.20f + 0.25f * y / 31.0f + 0.05f * noise (x / 8.0f, y / 8.0f, 4);
			put (32, x, y, 0.25f * l, 0.75f * l, 0.85f * l);
		}
	return texture (32, 32, true);
}

/* the plants: two kinds side by side (a green one, a red one), dark at the root, a rib down the middle */
static GLuint plant_texture (void)
{
	for (int y = 0; y < 32; y++)
		for (int x = 0; x < 16; x++)
		{
			float across = fabsf ((x % 8) - 3.5f) / 3.5f, l = 0.45f + 0.55f * y / 31.0f - (across < 0.15f ? 0.12f : 0.0f);
			if (x < 8)
				put (16, x, y, 0.18f * l, 0.72f * l, 0.20f * l);
			else
				put (16, x, y, 0.70f * l, 0.28f * l, 0.20f * l);
		}
	return texture (16, 32, false);
}

static GLuint surface_texture (void)
{
	for (int i = 0; i < 64; i++)
		put (8, i % 8, i / 8, 0.55f, 0.85f, 0.90f);
	return texture (8, 8, true);
}

/* ---- the fish: the kinds, their skins ------------------------------------------------- */

enum { CLOWN, BLUE_TANG, YELLOW_TANG, TETRA, ANGEL, SPECIES, STAR = SPECIES, SHRIMP, SKINS };	/* (the last two: skins only) */

static const struct
{
	const char *name;
	int count;
	float length, tall, wide;	/* of the mesh's proportions */
	float speed;			/* cruising, a second */
	float low, high;		/* the depth it keeps to */
	bool school;
} species[SPECIES] =
{
	[CLOWN] = {"clownfish", 3, 1.40f, 1.00f, 1.0f, 0.9f, 0.6f, 2.4f, false},
	[BLUE_TANG] = {"blue tang", 2, 1.90f, 1.15f, 0.8f, 1.3f, 1.6f, 4.6f, false},
	[YELLOW_TANG] = {"yellow tang", 2, 1.50f, 1.45f, 0.7f, 1.1f, 1.4f, 4.4f, false},
	[TETRA] = {"neon tetra", 12, 0.75f, 0.72f, 0.9f, 1.4f, 2.2f, 4.8f, true},
	[ANGEL] = {"angelfish", 2, 1.55f, 1.90f, 0.6f, 0.7f, 2.4f, 5.0f, false},
};
#define MAX_FISH	24

/* a skin's colour: u from the nose (0) to the tail's root (0.76), then the fins
   and the tail (to 1); v from the back (0) to the belly (1). The starfish's: u
   from its middle (0) to an arm's tip, v across the arm */
static void skin (int kind, float u, float v, float c[3])
{
	bool fin = u > 0.78f;
	float shade = 0.74f + 0.34f * v;				/* a darker back, a lighter belly */
	float r, g, b;
	switch (kind)
	{
	case STAR:
	{
		float off = fabsf (v - 0.5f), along = u * 8.0f, cell = along - floorf (along) - 0.5f;
		float row = off < 0.10f ? off : fabsf (off - 0.24f);			/* knobs: down the ridge, and a row a side */
		bool knob = fabsf (off < 0.10f ? cell : (cell < 0 ? cell + 0.5f : cell - 0.5f)) < 0.17f && row < 0.055f && u > 0.05f;
		float edge = clampf ((off - 0.30f) * 4.0f, 0.0f, 1.0f);			/* paler to its edges and tips */
		c[0] = knob ? 1.0f : 0.90f + 0.10f * edge;
		c[1] = knob ? 0.86f : 0.27f + 0.30f * edge + 0.12f * u;
		c[2] = knob ? 0.62f : 0.08f + 0.16f * edge;
		return;
	}
	case SHRIMP:
		shade = 1.0f;
		if (!fin)
		{
			r = 0.98f; g = 0.70f + 0.16f * v; b = 0.26f + 0.30f * v;	/* amber, paler below */
			if (v < 0.13f)
				r = g = b = 0.97f;					/* white down its back */
			else if (v < 0.36f)
				r = 0.86f, g = 0.07f, b = 0.07f;			/* red beside it */
			float part = (u - 0.40f) / 0.075f;
			if (u > 0.40f && part - floorf (part) < 0.16f)
				shade = 0.72f;						/* where its tail's rings meet */
		}
		else if (u < 0.86f)
			r = 0.97f, g = 0.86f, b = 0.72f;				/* its legs */
		else if (u < 0.93f)
			r = 0.86f, g = 0.07f, b = 0.07f;				/* its tail's fan: red, tipped white */
		else
			r = g = b = 0.97f;						/* its feelers */
		if (fin && u < 0.93f && u > 0.905f)
			r = g = b = 0.97f;
		break;
	case CLOWN:
	{
		static const float bands[3][2] = {{0.13f, 0.21f}, {0.36f, 0.47f}, {0.64f, 0.73f}};
		r = 1.0f; g = 0.42f; b = 0.05f;
		for (int i = 0; i < 3 && !fin; i++)
		{
			if (u > bands[i][0] - 0.015f && u < bands[i][1] + 0.015f)
			{
				r = g = b = u > bands[i][0] && u < bands[i][1] ? 0.97f : 0.04f;	/* white, edged black */
			}
		}
		if (fin && (u > 0.95f || v < 0.08f || v > 0.92f))
			r = g = b = 0.05f;
		break;
	}
	case BLUE_TANG:
		r = 0.06f; g = 0.28f; b = 0.92f;
		if (!fin && u > 0.17f && fabsf (v - 0.30f - 0.10f * sinf ((u - 0.17f) * 5.0f)) < 0.11f && !(u > 0.34f && u < 0.52f && v > 0.30f))
			r = g = b = 0.03f;						/* the black along its back */
		if (fin)
		{
			r = 1.0f; g = 0.85f; b = 0.10f;					/* a yellow tail, edged black */
			if (v < 0.14f || v > 0.86f)
				r = g = b = 0.04f;
		}
		break;
	case YELLOW_TANG:
		r = 1.0f; g = 0.86f; b = 0.10f;
		if (fin)
			g = 0.92f, b = 0.30f;
		break;
	case TETRA:
		r = 0.50f + 0.35f * v; g = 0.55f + 0.30f * v; b = 0.45f + 0.35f * v;	/* olive above, silver below */
		if (!fin && u > 0.05f && u < 0.64f && v > 0.34f && v < 0.48f)
			r = 0.10f, g = 0.95f, b = 1.0f, shade = 1.15f;			/* the neon stripe */
		else if (!fin && u > 0.40f && v >= 0.48f && v < 0.80f)
			r = 0.95f, g = 0.10f, b = 0.10f;					/* red behind, below it */
		else if (fin)
			r = g = b = 0.80f;
		break;
	default:		/* ANGEL */
	{
		static const float bands[4][2] = {{0.07f, 0.11f}, {0.26f, 0.34f}, {0.49f, 0.58f}, {0.70f, 0.76f}};
		r = 0.88f; g = 0.87f; b = 0.80f;
		for (int i = 0; i < 4; i++)
		{
			if (u > bands[i][0] && u < bands[i][1])
				r = g = b = 0.07f;
		}
		if (fin)
			r = g = b = v < 0.2f || v > 0.8f ? 0.10f : 0.75f;
		break;
	}
	}
	/* the eye: a black pupil, a pale ring */
	float ex = (u - 0.085f) / 0.030f, ey = (v - 0.36f) / 0.085f, e = ex * ex + ey * ey;
	if (e < 1.0f)
	{
		r = g = b = e < 0.45f ? 0.02f : 0.85f;
		shade = 1.0f;
	}
	c[0] = r * shade;
	c[1] = g * shade;
	c[2] = b * shade;
}

static GLuint skin_texture (int kind)
{
	float c[3];
	for (int y = 0; y < 32; y++)
		for (int x = 0; x < 64; x++)
		{
			skin (kind, (x + 0.5f) / 64.0f, (y + 0.5f) / 32.0f, c);
			put (64, x, y, c[0], c[1], c[2]);
		}
	return texture (64, 32, false);
}

/* ---- vertices: built a few at a time, into the buffer bound -------------------------- */

#define CHUNK_FLOATS	(96 * 9)
static float chunk[CHUNK_FLOATS];
static int chunk_n, emitted, stride;

static void flush (void)
{
	if (chunk_n)
	{
		glBufferSubData (GL_ARRAY_BUFFER, emitted * stride * sizeof (float), chunk_n * stride * sizeof (float), chunk);
		emitted += chunk_n;
		chunk_n = 0;
	}
}

static void begin_vertices (GLuint buffer, int floats, int capacity)
{
	glBindBuffer (GL_ARRAY_BUFFER, buffer);
	glBufferData (GL_ARRAY_BUFFER, capacity * floats * sizeof (float), NULL, GL_STATIC_DRAW);
	stride = floats;
	chunk_n = emitted = 0;
}

static void emit (const float *v)
{
	if ((chunk_n + 1) * stride > CHUNK_FLOATS)
	{
		flush ();
	}
	memcpy (&chunk[chunk_n++ * stride], v, stride * sizeof (float));
}

static int vertices_so_far (void)	{ return emitted + chunk_n; }

/* the scene's vertex: where, its normal, its texture's place, how much it sways */
static void scene_vertex (float x, float y, float z, float nx, float ny, float nz, float u, float v, float sway)
{
	const float f[9] = {x, y, z, nx, ny, nz, u, v, sway};
	emit (f);
}

/* a flat sheet of nu x nv squares from a corner along two edges (two triangles a square) */
static void sheet (const float o[3], const float eu[3], const float ev[3], const float n[3], int nu, int nv, float uscale, float vscale)
{
	static const int corner[6][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 0}, {1, 1}, {0, 1}};
	for (int j = 0; j < nv; j++)
		for (int i = 0; i < nu; i++)
			for (int k = 0; k < 6; k++)
			{
				float a = (float) (i + corner[k][0]) / nu, b = (float) (j + corner[k][1]) / nv;
				scene_vertex (o[0] + eu[0] * a + ev[0] * b, o[1] + eu[1] * a + ev[1] * b, o[2] + eu[2] * a + ev[2] * b,
					      n[0], n[1], n[2], a * uscale, b * vscale, 0.0f);
			}
}

/* the rocks: where they lie and how big (the fish keep off them too) */
static const float rocks[][4] =
{
	{-5.2f, 0.5f, -1.8f, 1.5f}, {-3.6f, 0.3f, -2.6f, 0.9f}, {4.6f, 0.6f, -1.2f, 1.7f}, {5.9f, 0.3f, 0.6f, 0.9f}, {0.8f, 0.3f, -2.9f, 1.1f},
};
#define ROCKS		(int) (sizeof rocks / sizeof rocks[0])

/* a rock's point: a ball, dented by noise, flattened */
static void rock_point (int r, float a, float b, float p[3], float n[3])
{
	float dx = cosf (b) * cosf (a), dy = sinf (b), dz = cosf (b) * sinf (a);
	float bump = 0.72f + 0.45f * noise (dx * 2.0f + r * 7.0f + 8.0f, dz * 2.0f + dy * 2.0f + 8.0f, 64);
	p[0] = rocks[r][0] + rocks[r][3] * dx * bump;
	p[1] = rocks[r][1] + rocks[r][3] * dy * bump * 0.75f;
	p[2] = rocks[r][2] + rocks[r][3] * dz * bump;
	n[0] = dx;
	n[1] = dy;
	n[2] = dz;
}

static void rock (int r)
{
	enum { AROUND = 10, UP = 6 };
	static const int corner[6][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 0}, {1, 1}, {0, 1}};
	for (int j = 0; j < UP; j++)
		for (int i = 0; i < AROUND; i++)
			for (int k = 0; k < 6; k++)
			{
				float a = 2 * PI * (i + corner[k][0]) / AROUND, b = -0.45f + (PI / 2 + 0.45f) * (j + corner[k][1]) / UP;
				float p[3], n[3];
				rock_point (r, a, b, p, n);
				scene_vertex (p[0], p[1], p[2], n[0], n[1], n[2], a * 0.7f, b * 1.3f, 0.0f);
			}
}

/* a plant's blade: a tapering strip from the sand up, leaning a little; its top takes the current */
static void blade (float x, float z, float height, float width, float lean_x, float lean_z, int kind)
{
	enum { PARTS = 7 };
	for (int i = 0; i < PARTS; i++)
	{
		float t0 = (float) i / PARTS, t1 = (float) (i + 1) / PARTS;
		float w0 = width * (1.0f - 0.85f * t0), w1 = width * (1.0f - 0.85f * t1);
		float x0 = x + lean_x * t0 * t0, x1 = x + lean_x * t1 * t1, z0 = z + lean_z * t0 * t0, z1 = z + lean_z * t1 * t1;
		float u0 = kind ? 0.5f : 0.0f, u1 = u0 + 0.5f;
		const float q[6][5] =
		{
			{x0 - w0, height * t0, z0, u0, t0}, {x0 + w0, height * t0, z0, u1, t0}, {x1 + w1, height * t1, z1, u1, t1},
			{x0 - w0, height * t0, z0, u0, t0}, {x1 + w1, height * t1, z1, u1, t1}, {x1 - w1, height * t1, z1, u0, t1},
		};
		for (int k = 0; k < 6; k++)
		{
			scene_vertex (q[k][0], q[k][1], q[k][2], 0.0f, 0.55f, 0.83f, q[k][3], q[k][4], q[k][4] * q[k][4] * (0.5f + 0.2f * height));
		}
	}
}

enum { PART_SAND, PART_GLASS, PART_ROCKS, PART_PLANTS, PART_SURFACE, PARTS };
static struct { int first, count; } part[PARTS];
#define SCENE_VERTICES	8000

static void build_scene (GLuint buffer)
{
	begin_vertices (buffer, 9, SCENE_VERTICES);
	const float up[3] = {0, 1, 0}, down[3] = {0, -1, 0}, front[3] = {0, 0, 1}, right[3] = {1, 0, 0}, left[3] = {-1, 0, 0};

	part[PART_SAND].first = vertices_so_far ();
	sheet ((const float[3]) {-TANK_X, 0, FRONT_Z}, (const float[3]) {2 * TANK_X, 0, 0}, (const float[3]) {0, 0, -(FRONT_Z + TANK_Z)}, up, 16, 16, 8.0f, 11.5f);
	part[PART_SAND].count = vertices_so_far () - part[PART_SAND].first;

	part[PART_GLASS].first = vertices_so_far ();
	sheet ((const float[3]) {-TANK_X, 0, -TANK_Z}, (const float[3]) {2 * TANK_X, 0, 0}, (const float[3]) {0, TANK_Y, 0}, front, 8, 4, 2.0f, 1.0f);
	sheet ((const float[3]) {-TANK_X, 0, FRONT_Z}, (const float[3]) {0, 0, -(FRONT_Z + TANK_Z)}, (const float[3]) {0, TANK_Y, 0}, right, 12, 4, 3.0f, 1.0f);
	sheet ((const float[3]) {TANK_X, 0, -TANK_Z}, (const float[3]) {0, 0, FRONT_Z + TANK_Z}, (const float[3]) {0, TANK_Y, 0}, left, 12, 4, 3.0f, 1.0f);
	part[PART_GLASS].count = vertices_so_far () - part[PART_GLASS].first;

	part[PART_ROCKS].first = vertices_so_far ();
	for (int r = 0; r < ROCKS; r++)
	{
		rock (r);
	}
	part[PART_ROCKS].count = vertices_so_far () - part[PART_ROCKS].first;

	/* the plants: tufts of blades about the rocks and along the back */
	part[PART_PLANTS].first = vertices_so_far ();
	static const float tufts[][4] =		/* x, z, how tall, which kind */
	{
		{-6.6f, -2.9f, 3.6f, 0}, {-4.4f, -0.4f, 2.2f, 1}, {-2.2f, -3.2f, 4.2f, 0}, {2.6f, -3.3f, 3.8f, 0}, {3.4f, 0.4f, 1.8f, 1},
		{6.6f, -2.6f, 3.2f, 0}, {7.0f, 1.6f, 2.4f, 0}, {-7.0f, 1.2f, 2.6f, 1}, {-0.6f, -1.6f, 1.6f, 0},
	};
	for (unsigned t = 0; t < sizeof tufts / sizeof tufts[0]; t++)
	{
		for (int i = 0; i < 6; i++)
		{
			float a = 2 * PI * (i + frand ()) / 6, r = 0.15f + 0.35f * frand (), h = tufts[t][2] * (0.65f + 0.35f * frand ());
			blade (tufts[t][0] + r * cosf (a), tufts[t][1] + r * sinf (a), h, 0.10f + 0.05f * frand (), 0.5f * cosf (a) * frand (),
			       0.5f * sinf (a) * frand (), (int) tufts[t][3]);
		}
	}
	part[PART_PLANTS].count = vertices_so_far () - part[PART_PLANTS].first;

	part[PART_SURFACE].first = vertices_so_far ();
	sheet ((const float[3]) {-TANK_X, TANK_Y, -TANK_Z}, (const float[3]) {2 * TANK_X, 0, 0}, (const float[3]) {0, 0, FRONT_Z + TANK_Z}, down, 8, 12, 1.0f, 1.0f);
	part[PART_SURFACE].count = vertices_so_far () - part[PART_SURFACE].first;
	flush ();
	printf ("aquarium: the tank: %d vertices (of %d)\n", vertices_so_far (), SCENE_VERTICES);
}

/* the fish's mesh, along x from its nose (0.5) to its tail's tip (-0.5): a
   body of oval rings, a forked tail, a fin on its back, one below, one a side */
static int build_fish (GLuint buffer)
{
	enum { RINGS = 9, AROUND = 8 };
	static const float at[RINGS] = {0.0f, 0.06f, 0.16f, 0.30f, 0.46f, 0.62f, 0.78f, 0.90f, 1.0f};	/* along the body */
	static const float tall[RINGS] = {0.012f, 0.085f, 0.140f, 0.172f, 0.170f, 0.145f, 0.100f, 0.055f, 0.028f};
	static const float wide[RINGS] = {0.010f, 0.050f, 0.072f, 0.082f, 0.076f, 0.060f, 0.038f, 0.018f, 0.008f};
	static const int corner[6][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 0}, {1, 1}, {0, 1}};
	begin_vertices (buffer, 8, 480);
	for (int r = 0; r + 1 < RINGS; r++)
		for (int s = 0; s < AROUND; s++)
			for (int k = 0; k < 6; k++)
			{
				int ring = r + corner[k][0];
				float a = 2 * PI * (s + corner[k][1]) / AROUND, ca = cosf (a), sa = sinf (a);
				float nl = sqrtf (wide[ring] * wide[ring] * ca * ca + tall[ring] * tall[ring] * sa * sa) + 1e-6f;
				const float v[8] = {0.5f - 0.74f * at[ring], tall[ring] * ca, wide[ring] * sa,
						    0.0f, wide[ring] * ca / nl, tall[ring] * sa / nl, 0.76f * at[ring], 0.5f - 0.5f * ca};
				emit (v);
			}
	/* flat parts: triangles in the fish's plane (z = 0) or out of it, their colours from the skin's fin part */
	static const float flat[][3][5] =		/* x, y, z, u, v */
	{
		/* the tail: two lobes from the root to their tips, a notch between */
		{{-0.24f, 0.028f, 0, 0.80f, 0.42f}, {-0.50f, 0.17f, 0, 1.0f, 0.02f}, {-0.40f, 0.0f, 0, 0.94f, 0.5f}},
		{{-0.24f, 0.028f, 0, 0.80f, 0.42f}, {-0.40f, 0.0f, 0, 0.94f, 0.5f}, {-0.24f, -0.028f, 0, 0.80f, 0.58f}},
		{{-0.24f, -0.028f, 0, 0.80f, 0.58f}, {-0.40f, 0.0f, 0, 0.94f, 0.5f}, {-0.50f, -0.17f, 0, 1.0f, 0.98f}},
		/* on its back */
		{{0.28f, 0.165f, 0, 0.84f, 0.5f}, {0.17f, 0.172f, 0, 0.86f, 0.5f}, {0.12f, 0.27f, 0, 0.90f, 0.1f}},
		{{0.17f, 0.172f, 0, 0.86f, 0.5f}, {0.04f, 0.150f, 0, 0.88f, 0.5f}, {0.12f, 0.27f, 0, 0.90f, 0.1f}},
		{{0.04f, 0.150f, 0, 0.88f, 0.5f}, {-0.02f, 0.23f, 0, 0.92f, 0.1f}, {0.12f, 0.27f, 0, 0.90f, 0.1f}},
		{{0.04f, 0.150f, 0, 0.88f, 0.5f}, {-0.10f, 0.110f, 0, 0.90f, 0.5f}, {-0.02f, 0.23f, 0, 0.92f, 0.1f}},
		/* below, behind the belly */
		{{0.02f, -0.150f, 0, 0.86f, 0.5f}, {-0.10f, -0.105f, 0, 0.90f, 0.5f}, {-0.08f, -0.24f, 0, 0.92f, 0.9f}},
		{{0.16f, -0.168f, 0, 0.84f, 0.5f}, {0.02f, -0.150f, 0, 0.86f, 0.5f}, {-0.08f, -0.24f, 0, 0.92f, 0.9f}},
		/* at its sides, behind the gills */
		{{0.27f, -0.03f, 0.078f, 0.84f, 0.5f}, {0.20f, -0.06f, 0.080f, 0.86f, 0.5f}, {0.13f, -0.10f, 0.20f, 0.92f, 0.5f}},
		{{0.27f, -0.03f, -0.078f, 0.84f, 0.5f}, {0.13f, -0.10f, -0.20f, 0.92f, 0.5f}, {0.20f, -0.06f, -0.080f, 0.86f, 0.5f}},
	};
	for (unsigned t = 0; t < sizeof flat / sizeof flat[0]; t++)
		for (int k = 0; k < 3; k++)
		{
			bool side = flat[t][0][2] != 0.0f;
			const float v[8] = {flat[t][k][0], flat[t][k][1], flat[t][k][2], 0.0f, side ? 0.9f : 0.3f,
					    side ? (flat[t][0][2] > 0 ? 0.44f : -0.44f) : 0.95f, flat[t][k][3], flat[t][k][4]};
			emit (v);
		}
	flush ();
	return vertices_so_far ();
}

/* ---- the fish, alive ---------------------------------------------------------------- */

typedef struct
{
	int kind;
	float p[3], v[3], to[3];	/* where, how fast, where it's going */
	float f[3];			/* the way it points (its speed's, eased) */
	float next;			/* another place to go to in so long */
	float phase;			/* of its tail */
	float fright;			/* still startled for so long */
	float size;			/* of its kind's: each a little its own */
} fish_t;

static fish_t fish[MAX_FISH];
static int n_fish;
static float school_to[3];		/* where the school is going */

#define PELLETS		12
static struct { float p[3]; float age; bool there; } pellet[PELLETS];

static void roam (fish_t *f)
{
	f->to[0] = (TANK_X - 1.4f) * (2 * frand () - 1);
	f->to[1] = species[f->kind].low + (species[f->kind].high - species[f->kind].low) * frand ();
	f->to[2] = (TANK_Z - 1.2f) * (2 * frand () - 1);
	f->next = 3.0f + 5.0f * frand ();
}

static void make_fish (void)
{
	n_fish = 0;
	for (int k = 0; k < SPECIES; k++)
		for (int i = 0; i < species[k].count && n_fish < MAX_FISH; i++)
		{
			fish_t *f = &fish[n_fish++];
			memset (f, 0, sizeof *f);
			f->kind = k;
			roam (f);
			memcpy (f->p, f->to, sizeof f->p);
			if (species[k].school)				/* (the school starts together) */
			{
				f->p[0] = -3.0f + 1.5f * frand ();
				f->p[1] = 3.5f + frand ();
				f->p[2] = frand ();
			}
			roam (f);
			f->v[0] = f->f[0] = frand () < 0.5f ? 1.0f : -1.0f;
			f->phase = 6.0f * frand ();
			f->size = 0.85f + 0.3f * frand ();
		}
	school_to[0] = 2.0f;
	school_to[1] = 3.5f;
	school_to[2] = 0.0f;
}

static void fish_step (fish_t *f, float dt)
{
	const float length = species[f->kind].length * f->size;
	float cruise = species[f->kind].speed * (f->fright > 0 ? 3.2f : 1.0f);

	/* where to: its own place, the school's, or a pellet near enough */
	f->next -= dt;
	if (f->next < 0)
	{
		roam (f);
	}
	float to[3] = {f->to[0], f->to[1], f->to[2]};
	if (species[f->kind].school)
	{
		to[0] = school_to[0] + 0.25f * (f->to[0] - school_to[0]) / TANK_X;
		to[1] = school_to[1];
		to[2] = school_to[2];
	}
	float nearest = 5.5f * 5.5f;
	int food = -1;
	for (int i = 0; i < PELLETS && f->fright <= 0; i++)
	{
		float dx = pellet[i].p[0] - f->p[0], dy = pellet[i].p[1] - f->p[1], dz = pellet[i].p[2] - f->p[2], d2 = dx * dx + dy * dy + dz * dz;
		if (pellet[i].there && d2 < nearest)
		{
			nearest = d2;
			food = i;
		}
	}
	if (food >= 0)
	{
		memcpy (to, pellet[food].p, sizeof to);
		cruise *= 1.8f;
		if (nearest < (0.2f + 0.3f * length) * (0.2f + 0.3f * length))	/* eaten */
		{
			pellet[food].there = false;
		}
	}

	float want[3] = {to[0] - f->p[0], to[1] - f->p[1], to[2] - f->p[2]};
	float d = sqrtf (want[0] * want[0] + want[1] * want[1] + want[2] * want[2]) + 1e-4f;
	if (d < 0.6f && food < 0)
	{
		f->next = 0.0f;
	}
	for (int i = 0; i < 3; i++)
	{
		want[i] *= cruise / d;
	}
	if (f->fright > 0)					/* off, the way it's already going */
	{
		float s = sqrtf (f->v[0] * f->v[0] + f->v[1] * f->v[1] + f->v[2] * f->v[2]) + 1e-4f;
		for (int i = 0; i < 3; i++)
		{
			want[i] = f->v[i] / s * cruise;
		}
		f->fright -= dt;
	}

	/* off the others (the school keeps closer, and with the others' way) */
	for (int i = 0; i < n_fish; i++)
	{
		const fish_t *o = &fish[i];
		float dx = f->p[0] - o->p[0], dy = f->p[1] - o->p[1], dz = f->p[2] - o->p[2], d2 = dx * dx + dy * dy + dz * dz;
		float room = 0.45f * (length + species[o->kind].length * o->size);
		if (o == f || d2 > 4.0f)
		{
			continue;
		}
		if (d2 < room * room)
		{
			float push = (room - sqrtf (d2)) * 4.0f / (sqrtf (d2) + 1e-3f);
			want[0] += dx * push;
			want[1] += dy * push;
			want[2] += dz * push;
		}
		else if (species[f->kind].school && o->kind == f->kind)
		{
			for (int k = 0; k < 3; k++)
			{
				want[k] += 0.22f * o->v[k];
			}
		}
	}
	/* off the rocks, the glass, the sand and the surface */
	for (int r = 0; r < ROCKS; r++)
	{
		float dx = f->p[0] - rocks[r][0], dy = f->p[1] - rocks[r][1], dz = f->p[2] - rocks[r][2], d2 = dx * dx + dy * dy + dz * dz;
		float room = rocks[r][3] + 0.5f * length + 0.3f;
		if (d2 < room * room)
		{
			float push = (room - sqrtf (d2)) * 5.0f / (sqrtf (d2) + 1e-3f);
			want[0] += dx * push;
			want[1] += dy * push;
			want[2] += dz * push;
		}
	}
	const float lim[3] = {TANK_X - 0.9f, 0, TANK_Z - 0.8f};
	for (int i = 0; i < 3; i += 2)
	{
		if (fabsf (f->p[i]) > lim[i])
		{
			want[i] -= (fabsf (f->p[i]) - lim[i]) * 6.0f * (f->p[i] > 0 ? 1.0f : -1.0f);
		}
	}
	want[1] += f->p[1] < 0.6f ? (0.6f - f->p[1]) * 6.0f : f->p[1] > TANK_Y - 0.6f ? (TANK_Y - 0.6f - f->p[1]) * 6.0f : 0.0f;

	/* its speed comes round to that (a fish doesn't turn on the spot); not steeply up or down */
	float turn = (f->fright > 0 ? 5.0f : 1.6f) * dt;
	for (int i = 0; i < 3; i++)
	{
		f->v[i] += (want[i] - f->v[i]) * (turn > 1.0f ? 1.0f : turn);
	}
	float flat = sqrtf (f->v[0] * f->v[0] + f->v[2] * f->v[2]);
	f->v[1] = clampf (f->v[1], -0.45f * flat, 0.45f * flat);
	float speed = sqrtf (flat * flat + f->v[1] * f->v[1]);
	for (int i = 0; i < 3; i++)
	{
		f->p[i] += f->v[i] * dt;
		f->f[i] += (f->v[i] / (speed + 1e-4f) - f->f[i]) * clampf (dt * 5.0f, 0.0f, 1.0f);
	}
	f->p[0] = clampf (f->p[0], -TANK_X + 0.3f, TANK_X - 0.3f);
	f->p[1] = clampf (f->p[1], 0.25f, TANK_Y - 0.25f);
	f->p[2] = clampf (f->p[2], -TANK_Z + 0.3f, TANK_Z - 0.3f);
	f->phase += dt * (3.0f + 7.0f * speed / length);	/* the tail beats with its speed */
}

/* a fish's place and attitude: its nose along the way it points, its back up */
static void fish_model (const fish_t *f, float m[16])
{
	float l = sqrtf (f->f[0] * f->f[0] + f->f[1] * f->f[1] + f->f[2] * f->f[2]) + 1e-5f;
	float x[3] = {f->f[0] / l, f->f[1] / l, f->f[2] / l};
	float zl = sqrtf (x[0] * x[0] + x[2] * x[2]) + 1e-5f;
	float z[3] = {-x[2] / zl, 0.0f, x[0] / zl};				/* its side: level */
	float y[3] = {z[1] * x[2] - z[2] * x[1], z[2] * x[0] - z[0] * x[2], z[0] * x[1] - z[1] * x[0]};
	float len = species[f->kind].length * f->size;
	float sy = len * species[f->kind].tall, sz = len * species[f->kind].wide;
	const float out[16] =
	{
		x[0] * len, x[1] * len, x[2] * len, 0, y[0] * sy, y[1] * sy, y[2] * sy, 0, z[0] * sz, z[1] * sz, z[2] * sz, 0,
		f->p[0], f->p[1], f->p[2], 1,
	};
	memcpy (m, out, sizeof out);
}

/* ---- on the bottom: a starfish on a rock, shrimps on the sand ---------------------- */

/* their meshes, in the fish's kind of vertex (so the fish's program draws them) */
static int star_first, star_count, shrimp_first, shrimp_count;
#define BOTTOM_VERTICES	440

/* a triangle of points (x, y, z, u, v); its normal the face's, on the side of `out` */
static void face (const float a[5], const float b[5], const float c[5], const float out[3])
{
	float e[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]}, g[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
	float n[3] = {e[1] * g[2] - e[2] * g[1], e[2] * g[0] - e[0] * g[2], e[0] * g[1] - e[1] * g[0]};
	float l = sqrtf (n[0] * n[0] + n[1] * n[1] + n[2] * n[2]) + 1e-9f;
	l = n[0] * out[0] + n[1] * out[1] + n[2] * out[2] < 0 ? -l : l;
	const float *const p[3] = {a, b, c};
	for (int k = 0; k < 3; k++)
	{
		const float v[8] = {p[k][0], p[k][1], p[k][2], n[0] / l, n[1] / l, n[2] / l, p[k][3], p[k][4]};
		emit (v);
	}
}

/* the starfish, lying in x and z, its back up: five arms with a ridge down each,
   drooping to their tips (it lies on a round rock) */
static void build_star (void)
{
	const float up[3] = {0, 1, 0};
	const float c[5] = {0, 0.11f, 0, 0.0f, 0.5f};
	star_first = vertices_so_far ();
	for (int k = 0; k < 5; k++)
	{
		float a = 2 * PI * k / 5, ca = cosf (a), sa = sinf (a);
		const float m[5] = {0.26f * ca, 0.085f, 0.26f * sa, 0.5f, 0.5f}, t[5] = {0.47f * ca, -0.035f, 0.47f * sa, 0.95f, 0.5f};
		const float tl[5] = {0.5f * ca - 0.04f * sa, -0.07f, 0.5f * sa + 0.04f * ca, 1.0f, 0.2f};
		const float tr[5] = {0.5f * ca + 0.04f * sa, -0.07f, 0.5f * sa - 0.04f * ca, 1.0f, 0.8f};
		const float nl[5] = {0.17f * cosf (a + PI / 5), 0.0f, 0.17f * sinf (a + PI / 5), 0.35f, 0.0f};
		const float nr[5] = {0.17f * cosf (a - PI / 5), 0.0f, 0.17f * sinf (a - PI / 5), 0.35f, 1.0f};
		face (c, nl, m, up);
		face (m, nl, tl, up);
		face (m, tl, t, up);
		face (c, m, nr, up);
		face (m, nr, tr, up);
		face (m, t, tr, up);
		face (t, tl, tr, up);
	}
	star_count = vertices_so_far () - star_first;
}

/* the shrimp, along x as the fish (its beak at 0.5, its tail's fan at -0.5), its
   feet at y = -SHRIMP_FEET: a body of rings that bends down to the tail, a fan,
   five legs a side, the little ones under its tail, claws, and feelers longer than itself */
#define SHRIMP_FEET	0.135f
static void build_shrimp (void)
{
	enum { RINGS = 8, AROUND = 6 };
	static const float ring[RINGS][4] =		/* x, y of its middle, half its height, half its width */
	{
		{0.50f, 0.100f, 0.005f, 0.005f}, {0.40f, 0.070f, 0.050f, 0.042f}, {0.28f, 0.050f, 0.095f, 0.075f}, {0.12f, 0.050f, 0.105f, 0.082f},
		{-0.02f, 0.055f, 0.092f, 0.072f}, {-0.15f, 0.040f, 0.075f, 0.060f}, {-0.26f, 0.005f, 0.057f, 0.046f}, {-0.34f, -0.035f, 0.035f, 0.032f},
	};
	static const int corner[6][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 0}, {1, 1}, {0, 1}};
	shrimp_first = vertices_so_far ();
	for (int r = 0; r + 1 < RINGS; r++)
		for (int s = 0; s < AROUND; s++)
			for (int k = 0; k < 6; k++)
			{
				const float *g = ring[r + corner[k][0]];
				float a = 2 * PI * (s + corner[k][1]) / AROUND, ca = cosf (a), sa = sinf (a);
				float nl = sqrtf (g[3] * g[3] * ca * ca + g[2] * g[2] * sa * sa) + 1e-6f;
				const float v[8] = {g[0], g[1] + g[2] * ca, g[3] * sa, 0.0f, g[3] * ca / nl, g[2] * sa / nl,
						    0.76f * (0.5f - g[0]) / 0.84f, 0.5f - 0.5f * ca};
				emit (v);
			}
	const float up[3] = {0, 1, 0};
	/* the fan */
	const float root_l[5] = {-0.34f, -0.035f, 0.030f, 0.87f, 0.5f}, root_r[5] = {-0.34f, -0.035f, -0.030f, 0.87f, 0.5f};
	const float mid[5] = {-0.48f, -0.080f, 0.0f, 0.92f, 0.5f};
	const float tip_l[5] = {-0.50f, -0.085f, 0.11f, 0.92f, 0.5f}, tip_r[5] = {-0.50f, -0.085f, -0.11f, 0.92f, 0.5f};
	face (root_l, tip_l, mid, up);
	face (root_l, mid, root_r, up);
	face (root_r, mid, tip_r, up);
	for (int side = -1; side <= 1; side += 2)
	{
		const float out[3] = {0, 0.3f, (float) side};
		for (int i = 0; i < 5; i++)			/* the legs it walks on */
		{
			float x = 0.27f - 0.075f * i;
			const float hip_a[5] = {x + 0.016f, -0.030f, 0.050f * side, 0.82f, 0.5f}, hip_b[5] = {x - 0.016f, -0.030f, 0.050f * side, 0.82f, 0.5f};
			const float foot[5] = {x + 0.04f - 0.025f * i, -SHRIMP_FEET, 0.16f * side, 0.82f, 0.5f};
			face (hip_a, hip_b, foot, out);
		}
		/* a claw, held out; a long feeler and a short one */
		const float claw_a[5] = {0.34f, -0.010f, 0.045f * side, 0.82f, 0.5f}, claw_b[5] = {0.32f, -0.035f, 0.045f * side, 0.82f, 0.5f};
		const float claw[5] = {0.56f, -0.070f, 0.11f * side, 0.82f, 0.5f};
		face (claw_a, claw_b, claw, out);
		const float long_a[5] = {0.44f, 0.090f, 0.020f * side, 0.97f, 0.5f}, long_b[5] = {0.44f, 0.060f, 0.020f * side, 0.97f, 0.5f};
		const float long_tip[5] = {1.08f, 0.28f, 0.30f * side, 0.97f, 0.5f};
		face (long_a, long_b, long_tip, out);
		const float short_a[5] = {0.47f, 0.100f, 0.012f * side, 0.97f, 0.5f}, short_b[5] = {0.47f, 0.080f, 0.012f * side, 0.97f, 0.5f};
		const float short_tip[5] = {0.74f, 0.25f, 0.085f * side, 0.97f, 0.5f};
		face (short_a, short_b, short_tip, out);
	}
	for (int i = 0; i < 4; i++)				/* the little legs under its tail */
	{
		float x = -0.05f - 0.07f * i, y = 0.055f - 0.092f + 0.012f * i - (i == 3 ? 0.030f : 0.0f);
		const float a[5] = {x + 0.016f, y, 0, 0.82f, 0.5f}, b[5] = {x - 0.016f, y, 0, 0.82f, 0.5f}, tip[5] = {x - 0.025f, y - 0.055f, 0, 0.82f, 0.5f};
		face (a, b, tip, (const float[3]) {0, 0, 1});
	}
	shrimp_count = vertices_so_far () - shrimp_first;
}

static void build_bottom (GLuint buffer)
{
	begin_vertices (buffer, 8, BOTTOM_VERTICES);
	build_star ();
	build_shrimp ();
	flush ();
}

/* the starfish creeps about the front of a rock: on the rock's mesh (its flat
   faces, not the round thing they are made after), its back the face's normal,
   which it comes round to slowly when it creeps over an edge */
#define STAR_ROCK	2
#define STAR_SIZE	1.15f
static float star_up[3] = {0, 0.5f, 0.85f};

static void star_model (float now, float m[16])
{
	enum { AROUND = 10, UP = 6 };				/* (as rock ()) */
	float a = 1.15f + 0.42f * sinf (now * 0.031f), b = 0.52f + 0.20f * sinf (now * 0.047f + 1.0f);
	float fa = a / (2 * PI) * AROUND, fb = (b + 0.45f) / (PI / 2 + 0.45f) * UP;
	int i = (int) floorf (fa), j = (int) floorf (fb);
	fa -= i;
	fb -= j;
	float p[2][2][3], unused[3];
	for (int u = 0; u < 2; u++)
		for (int v = 0; v < 2; v++)
		{
			rock_point (STAR_ROCK, 2 * PI * (i + u) / AROUND, -0.45f + (PI / 2 + 0.45f) * (j + v) / UP, p[u][v], unused);
		}
	/* the square's two triangles: (0,0) (1,0) (1,1) and (0,0) (1,1) (0,1) */
	float at[3], e[3], g[3];
	for (int k = 0; k < 3; k++)
	{
		e[k] = fb <= fa ? p[1][0][k] - p[0][0][k] : p[1][1][k] - p[0][1][k];
		g[k] = fb <= fa ? p[1][1][k] - p[1][0][k] : p[0][1][k] - p[0][0][k];
		at[k] = p[0][0][k] + fa * e[k] + fb * g[k];
	}
	float n[3] = {e[1] * g[2] - e[2] * g[1], e[2] * g[0] - e[0] * g[2], e[0] * g[1] - e[1] * g[0]};
	float l = sqrtf (n[0] * n[0] + n[1] * n[1] + n[2] * n[2]) + 1e-9f;
	if (n[0] * (at[0] - rocks[STAR_ROCK][0]) + n[1] * (at[1] - rocks[STAR_ROCK][1]) + n[2] * (at[2] - rocks[STAR_ROCK][2]) < 0)
	{
		l = -l;
	}
	for (int k = 0; k < 3; k++)
	{
		star_up[k] += (n[k] / l - star_up[k]) * 0.02f;
	}
	l = sqrtf (star_up[0] * star_up[0] + star_up[1] * star_up[1] + star_up[2] * star_up[2]) + 1e-9f;
	const float y[3] = {star_up[0] / l, star_up[1] / l, star_up[2] / l};
	/* it turns as it goes: an arm's way, in the face's plane */
	float spin = now * 0.04f, t[3] = {1.0f - y[0] * y[0], -y[0] * y[1], -y[0] * y[2]};		/* (x, less what of it is along y) */
	l = sqrtf (t[0] * t[0] + t[1] * t[1] + t[2] * t[2]) + 1e-9f;
	for (int k = 0; k < 3; k++)
	{
		t[k] /= l;
	}
	const float s[3] = {t[1] * y[2] - t[2] * y[1], t[2] * y[0] - t[0] * y[2], t[0] * y[1] - t[1] * y[0]};
	float x[3], z[3];
	for (int k = 0; k < 3; k++)
	{
		x[k] = t[k] * cosf (spin) + s[k] * sinf (spin);
	}
	z[0] = x[1] * y[2] - x[2] * y[1];
	z[1] = x[2] * y[0] - x[0] * y[2];
	z[2] = x[0] * y[1] - x[1] * y[0];
	const float out[16] =
	{
		x[0] * STAR_SIZE, x[1] * STAR_SIZE, x[2] * STAR_SIZE, 0, y[0] * STAR_SIZE, y[1] * STAR_SIZE, y[2] * STAR_SIZE, 0,
		z[0] * STAR_SIZE, z[1] * STAR_SIZE, z[2] * STAR_SIZE, 0,
		at[0] + 0.05f * y[0], at[1] + 0.05f * y[1], at[2] + 0.05f * y[2], 1,
	};
	memcpy (m, out, sizeof out);
}

/* a shrimp walks the sand from place to place and stands about; a fish too
   near, or a knock, and it's off backwards with a flick of its tail, and sinks
   back down */
enum { SHRIMP_STANDS, SHRIMP_WALKS, SHRIMP_DARTS };
typedef struct
{
	int does;
	float p[3], v[3];
	float heading, to[2];		/* the way it faces; where it walks to */
	float left;			/* stands so long yet */
	float phase, size;
} shrimp_t;
#define SHRIMPS		2
static shrimp_t shrimp[SHRIMPS];

static float shrimp_floor (const shrimp_t *s)	{ return SHRIMP_FEET * s->size; }

/* off the rocks: pushed out of them, on the sand */
static bool off_rocks (float p[3], float room)
{
	bool pushed = false;
	for (int r = 0; r < ROCKS; r++)
	{
		float dx = p[0] - rocks[r][0], dz = p[2] - rocks[r][2], d = sqrtf (dx * dx + dz * dz) + 1e-4f, keep = rocks[r][3] * 1.15f + room;
		if (d < keep)
		{
			p[0] += dx / d * (keep - d);
			p[2] += dz / d * (keep - d);
			pushed = true;
		}
	}
	return pushed;
}

static void shrimp_dart (shrimp_t *s)
{
	float side = 1.2f * (frand () - 0.5f);
	s->does = SHRIMP_DARTS;
	s->v[0] = -2.6f * cosf (s->heading) - side * sinf (s->heading);
	s->v[1] = 1.3f + 0.6f * frand ();
	s->v[2] = -2.6f * sinf (s->heading) + side * cosf (s->heading);
}

static void make_shrimps (void)
{
	static const float start[SHRIMPS][2] = {{-1.6f, 1.2f}, {2.2f, 2.2f}};
	for (int i = 0; i < SHRIMPS; i++)
	{
		shrimp_t *s = &shrimp[i];
		memset (s, 0, sizeof *s);
		s->size = 1.25f + 0.25f * i;
		s->p[0] = start[i][0];
		s->p[1] = shrimp_floor (s);
		s->p[2] = start[i][1];
		s->heading = 2 * PI * frand ();
		s->left = 1.0f + 2.0f * i;
	}
}

static void shrimp_step (shrimp_t *s, float dt)
{
	const float floor_y = shrimp_floor (s);
	if (s->does != SHRIMP_DARTS)
	{
		for (int i = 0; i < n_fish; i++)
		{
			float dx = fish[i].p[0] - s->p[0], dy = fish[i].p[1] - s->p[1], dz = fish[i].p[2] - s->p[2];
			if (dx * dx + dy * dy + dz * dz < 0.8f * 0.8f)
			{
				shrimp_dart (s);
				break;
			}
		}
	}
	switch (s->does)
	{
	case SHRIMP_STANDS:
		s->phase += dt * 2.0f;
		if ((s->left -= dt) < 0)
		{
			float to[3];
			do						/* somewhere on the sand in front, not under a rock */
			{
				to[0] = (TANK_X - 1.5f) * (2 * frand () - 1);
				to[2] = -2.4f + 5.4f * frand ();
			}
			while (off_rocks (to, 0.4f));
			s->to[0] = to[0];
			s->to[1] = to[2];
			s->does = SHRIMP_WALKS;
		}
		break;

	case SHRIMP_WALKS:
	{
		float dx = s->to[0] - s->p[0], dz = s->to[1] - s->p[2], turn = atan2f (dz, dx) - s->heading;
		turn -= 2 * PI * floorf (turn / (2 * PI) + 0.5f);
		s->heading += clampf (turn, -1.6f * dt, 1.6f * dt);
		if (fabsf (turn) < 0.6f)
		{
			float step = 0.35f * s->size * dt;
			s->p[0] += cosf (s->heading) * step;
			s->p[2] += sinf (s->heading) * step;
		}
		s->phase += dt * 7.0f;
		if (dx * dx + dz * dz < 0.15f * 0.15f)
		{
			s->does = SHRIMP_STANDS;
			s->left = 2.0f + 5.0f * frand ();
		}
		break;
	}

	default:		/* SHRIMP_DARTS: thrown back, the water slows it, it sinks */
		for (int k = 0; k < 3; k++)
		{
			s->p[k] += s->v[k] * dt;
		}
		s->v[0] -= s->v[0] * clampf (2.5f * dt, 0.0f, 1.0f);
		s->v[2] -= s->v[2] * clampf (2.5f * dt, 0.0f, 1.0f);
		s->v[1] = fmaxf (s->v[1] - 2.6f * dt, -0.55f);
		s->phase += dt * 24.0f;
		if (s->p[1] <= floor_y && s->v[1] < 0)
		{
			s->does = SHRIMP_STANDS;
			s->left = 1.0f + 2.0f * frand ();
		}
		break;
	}
	off_rocks (s->p, 0.25f);
	s->p[0] = clampf (s->p[0], -TANK_X + 0.7f, TANK_X - 0.7f);
	s->p[1] = clampf (s->p[1], floor_y, TANK_Y - 1.0f);
	s->p[2] = clampf (s->p[2], -TANK_Z + 0.9f, TANK_Z - 0.6f);
}

/* a shrimp's place and attitude: level on its feet, its beak up when it darts */
static void shrimp_model (const shrimp_t *s, float m[16])
{
	float pitch = s->does == SHRIMP_DARTS ? 0.35f : 0.04f * sinf (s->phase * 0.5f), ch = cosf (s->heading), sh = sinf (s->heading);
	const float x[3] = {ch * cosf (pitch), sinf (pitch), sh * cosf (pitch)}, z[3] = {-sh, 0.0f, ch};
	const float y[3] = {z[1] * x[2] - z[2] * x[1], z[2] * x[0] - z[0] * x[2], z[0] * x[1] - z[1] * x[0]};
	float len = s->size, bob = s->does == SHRIMP_WALKS ? 0.008f * len * sinf (s->phase * 2.0f) : 0.0f;
	const float out[16] =
	{
		x[0] * len, x[1] * len, x[2] * len, 0, y[0] * len, y[1] * len, y[2] * len, 0, z[0] * len, z[1] * len, z[2] * len, 0,
		s->p[0], s->p[1] + bob, s->p[2], 1,
	};
	memcpy (m, out, sizeof out);
}

/* ---- the sound ---------------------------------------------------------------------- */

enum { CH_BUBBLES, CH_HUM, CH_PLOP, CH_KNOCK };

static void play (unsigned channel, int sound, float volume, float pan)
{
	float l = clampf (volume * fminf (1.0f, 1.0f - pan), 0.0f, 1.0f), r = clampf (volume * fminf (1.0f, 1.0f + pan), 0.0f, 1.0f);
	pgpu_sound_play (channel, sound, (uint32_t) (l * PGPU_SOUND_FULL), (uint32_t) (r * PGPU_SOUND_FULL), 0);
}

/* ---- what's done to them ------------------------------------------------------------ */

static void feed (void)
{
	float x = (TANK_X - 2.5f) * (2 * frand () - 1), z = (TANK_Z - 1.5f) * (2 * frand () - 1);
	int dropped = 0;
	for (int i = 0; i < PELLETS && dropped < 5; i++)
	{
		if (!pellet[i].there)
		{
			pellet[i].there = true;
			pellet[i].age = 0.0f;
			pellet[i].p[0] = x + 0.9f * (frand () - 0.5f);
			pellet[i].p[1] = TANK_Y - 0.05f - 0.3f * frand ();
			pellet[i].p[2] = z + 0.9f * (frand () - 0.5f);
			dropped++;
		}
	}
	play (CH_PLOP, ASND_PLOP, 0.7f, clampf (x / TANK_X, -0.8f, 0.8f));
}

/* a knock on the front glass at (x, y): the fish near it are off */
static void knock (float x, float y)
{
	for (int i = 0; i < n_fish; i++)
	{
		fish_t *f = &fish[i];
		float dx = f->p[0] - x, dy = f->p[1] - y, dz = f->p[2] - TANK_Z, d = sqrtf (dx * dx + dy * dy + dz * dz) + 0.2f;
		if (d < 9.0f)
		{
			float push = 5.0f / d;
			f->v[0] += dx * push;
			f->v[1] += dy * push * 0.4f;
			f->v[2] += dz * push - 1.5f;
			f->fright = 1.2f + 0.8f * frand ();
		}
	}
	for (int i = 0; i < SHRIMPS; i++)
	{
		if (shrimp[i].does != SHRIMP_DARTS && fabsf (shrimp[i].p[0] - x) < 7.0f)
		{
			shrimp_dart (&shrimp[i]);
		}
	}
	play (CH_KNOCK, ASND_KNOCK, 0.85f, clampf (x / TANK_X, -0.8f, 0.8f));
}

/* antialiasing, each kind for AA_S seconds in turn ("m": the next one, and it stays):
   none; the scene drawn twice as wide and high into a texture, which is drawn
   over the screen through a linear filter (four texels a pixel); the RPi's
   four samples a pixel (pglSamples) */
enum { AA_NONE, AA_SUPER, AA_HARD, AA_MODES };
#define AA_S		10.0f
#define AA_SIZE_MAX	2048		/* a texture's side at most */
static const char *const aa_name[AA_MODES] = {"NO AA", "AA 2X2 SUPERSAMPLED", "AA 4X HARDWARE"};

static GLuint super_fb, super_texture, super_depth;
static int super_w, super_h;

/* the texture the supersampled scene is drawn into, for a screen of w x h; false: there's none */
static bool super_target (int w, int h)
{
	if (2 * w > AA_SIZE_MAX || 2 * h > AA_SIZE_MAX)
	{
		return false;
	}
	if (super_w == 2 * w && super_h == 2 * h)
	{
		return true;
	}
	if (!super_fb)
	{
		glGenFramebuffers (1, &super_fb);
		glGenTextures (1, &super_texture);
		glGenRenderbuffers (1, &super_depth);
	}
	super_w = super_h = 0;
	glBindTexture (GL_TEXTURE_2D, super_texture);
	glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA, 2 * w, 2 * h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glBindRenderbuffer (GL_RENDERBUFFER, super_depth);
	glRenderbufferStorage (GL_RENDERBUFFER, GL_DEPTH_COMPONENT16, 2 * w, 2 * h);
	glBindFramebuffer (GL_FRAMEBUFFER, super_fb);
	glFramebufferTexture2D (GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, super_texture, 0);
	glFramebufferRenderbuffer (GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, super_depth);
	GLenum status = glCheckFramebufferStatus (GL_FRAMEBUFFER);
	glBindFramebuffer (GL_FRAMEBUFFER, 0);
	if (status != GL_FRAMEBUFFER_COMPLETE || glGetError () != GL_NO_ERROR)
	{
		printf ("aquarium: no %dx%d texture to draw into (0x%x)\n", 2 * w, 2 * h, (unsigned) status);
		return false;
	}
	super_w = 2 * w;
	super_h = 2 * h;
	return true;
}

int main (void)
{
	stdio_init_all ();
	pgpu_init ();
	printf ("\naquarium: waiting for the RPi (READY)...\n");
	while (!pgpu_wait_ready (1000))
	{
	}
	pgpu_set_reply_phase (1);
	int tries = 0;
	while (!pglInit () && ++tries < 5)		/* the first reply can be missed */
	{
	}
	bool pad_there = pad_init ();
	if (!hud_init ())
	{
		printf ("aquarium: the HUD program didn't link\n");
	}

	/* the programs */
	GLuint aqua = glCreateProgram (), fishp = glCreateProgram (), bub = glCreateProgram (), shafts = glCreateProgram ();
	GLuint down = glCreateProgram ();
	glProgramBinaryOES (down, PGL_PROGRAM_BINARY_PGPU, &texview_info, sizeof texview_info);
	GLint d_pos = glGetAttribLocation (down, "a_pos"), d_rect = glGetUniformLocation (down, "u_rect");
	glUseProgram (down);
	glUniform1i (glGetUniformLocation (down, "u_tex"), 0);
	glUniform1i (glGetUniformLocation (down, "u_cube"), 2);		/* (not a 2D one's unit: GL ES won't draw) */
	glUniform1f (glGetUniformLocation (down, "u_mode"), 1.0f);
	glUniform4f (d_rect, -1.0f, -1.0f, 2.0f, 2.0f);
	glProgramBinaryOES (aqua, PGL_PROGRAM_BINARY_PGPU, &aqua_info, sizeof aqua_info);
	glProgramBinaryOES (fishp, PGL_PROGRAM_BINARY_PGPU, &fish_info, sizeof fish_info);
	glProgramBinaryOES (bub, PGL_PROGRAM_BINARY_PGPU, &bubbles_info, sizeof bubbles_info);
	glProgramBinaryOES (shafts, PGL_PROGRAM_BINARY_PGPU, &shafts_info, sizeof shafts_info);
	const GLuint progs[4] = {aqua, fishp, bub, shafts};
	for (int i = 0; i < 4; i++)
	{
		GLint linked = 0;
		glGetProgramiv (progs[i], GL_LINK_STATUS, &linked);
		if (!linked)
		{
			printf ("aquarium: program %d didn't link\n", i);
		}
	}
	struct { GLint pos, normal, uv, sway, vp, eye, time, water, deep, shallow, model, swim; } a, f;
	a.pos = glGetAttribLocation (aqua, "a_pos"); a.normal = glGetAttribLocation (aqua, "a_normal");
	a.uv = glGetAttribLocation (aqua, "a_uv"); a.sway = glGetAttribLocation (aqua, "a_sway");
	a.vp = glGetUniformLocation (aqua, "u_vp"); a.eye = glGetUniformLocation (aqua, "u_eye");
	a.time = glGetUniformLocation (aqua, "u_time"); a.water = glGetUniformLocation (aqua, "u_water");
	a.deep = glGetUniformLocation (aqua, "u_deep"); a.shallow = glGetUniformLocation (aqua, "u_shallow");
	f.pos = glGetAttribLocation (fishp, "a_pos"); f.normal = glGetAttribLocation (fishp, "a_normal");
	f.uv = glGetAttribLocation (fishp, "a_uv");
	f.vp = glGetUniformLocation (fishp, "u_vp"); f.eye = glGetUniformLocation (fishp, "u_eye");
	f.time = glGetUniformLocation (fishp, "u_time"); f.water = glGetUniformLocation (fishp, "u_water");
	f.deep = glGetUniformLocation (fishp, "u_deep"); f.shallow = glGetUniformLocation (fishp, "u_shallow");
	f.model = glGetUniformLocation (fishp, "u_model"); f.swim = glGetUniformLocation (fishp, "u_swim");
	GLint b_seed = glGetAttribLocation (bub, "a_seed"), b_vp = glGetUniformLocation (bub, "u_vp");
	GLint b_motion = glGetUniformLocation (bub, "u_motion"), b_color = glGetUniformLocation (bub, "u_color");
	GLint s_pos = glGetAttribLocation (shafts, "a_pos"), s_uv = glGetAttribLocation (shafts, "a_uv");
	GLint s_vp = glGetUniformLocation (shafts, "u_vp"), s_time = glGetUniformLocation (shafts, "u_time");
	GLint s_color = glGetUniformLocation (shafts, "u_color");
	glUseProgram (aqua);
	glUniform1i (glGetUniformLocation (aqua, "u_tex"), 0);
	glUniform1i (glGetUniformLocation (aqua, "u_caustics"), 1);
	glUseProgram (fishp);
	glUniform1i (glGetUniformLocation (fishp, "u_tex"), 0);
	glUniform1i (glGetUniformLocation (fishp, "u_caustics"), 1);
	glUseProgram (shafts);
	glUniform1i (glGetUniformLocation (shafts, "u_caustics"), 1);

	/* the textures: the caustics on unit 1, for good */
	GLuint t_sand = sand_texture (), t_rock = rock_texture (), t_glass = glass_texture (), t_plant = plant_texture ();
	GLuint t_surface = surface_texture (), t_skin[SKINS], t_caustics;
	for (int k = 0; k < SKINS; k++)
	{
		t_skin[k] = skin_texture (k);
	}
	glGenTextures (1, &t_caustics);
	glActiveTexture (GL_TEXTURE1);
	glBindTexture (GL_TEXTURE_2D, t_caustics);
	glPixelStorei (GL_UNPACK_ALIGNMENT, 1);
	glTexImage2D (GL_TEXTURE_2D, 0, GL_LUMINANCE, AQUARIUM_CAUSTICS, AQUARIUM_CAUSTICS, 0, GL_LUMINANCE, GL_UNSIGNED_BYTE,
		      aquarium_caustics);
	glGenerateMipmap (GL_TEXTURE_2D);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glActiveTexture (GL_TEXTURE0);

	/* the vertices */
	GLuint buffers[7];
	glGenBuffers (7, buffers);
	static const float square[12] = {0, 0, 1, 0, 1, 1, 0, 0, 1, 1, 0, 1};
	glBindBuffer (GL_ARRAY_BUFFER, buffers[5]);
	glBufferData (GL_ARRAY_BUFFER, sizeof square, square, GL_STATIC_DRAW);
	build_scene (buffers[0]);
	int fish_vertices = build_fish (buffers[1]);
	build_bottom (buffers[6]);
	enum { STONE_BUBBLES = 36, BUBBLES = 2 * STONE_BUBBLES, SHAFTS = 5, SPECKS = 96 };
	static const float stones[2][2] = {{-1.6f, 1.2f}, {5.2f, -2.6f}};
	begin_vertices (buffers[2], 4, BUBBLES);
	for (int i = 0; i < BUBBLES; i++)
	{
		const float v[4] = {stones[i / STONE_BUBBLES][0] + 0.25f * (frand () - 0.5f), stones[i / STONE_BUBBLES][1] + 0.25f * (frand () - 0.5f),
				    frand (), frand ()};
		emit (v);
	}
	flush ();
	begin_vertices (buffers[4], 4, SPECKS);			/* what drifts in any water: specks, rising slowly */
	for (int i = 0; i < SPECKS; i++)
	{
		const float v[4] = {TANK_X * (2 * frand () - 1), -TANK_Z + (TANK_Z + 9.0f) * frand (), frand (), frand ()};
		emit (v);
	}
	flush ();
	begin_vertices (buffers[3], 5, SHAFTS * 6);
	for (int i = 0; i < SHAFTS; i++)			/* sheets from the surface down, leaning with the sun */
	{
		static const int corner[6][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 0}, {1, 1}, {0, 1}};
		float x = -6.0f + 3.0f * i + frand (), z = -3.0f + 1.5f * i, w = 4.5f + 2.0f * frand ();
		for (int k = 0; k < 6; k++)
		{
			float u = corner[k][0], v = corner[k][1];
			const float q[5] = {x + w * u - 1.8f * v, TANK_Y * (1.0f - 0.92f * v), z, u, v};
			emit (q);
		}
	}
	flush ();

	for (int i = 1; i < ASND_COUNT; i++)
	{
		pgpu_sound_data (i, aquarium_sounds[i].rate, PGPU_SOUND_U8, aquarium_sounds[i].samples, aquarium_sounds[i].frames);
	}
	pgpu_sound_play (CH_BUBBLES, ASND_BUBBLES, 110, 110, PGPU_SOUND_LOOP);
	pgpu_sound_play (CH_HUM, ASND_HUM, 30, 30, PGPU_SOUND_LOOP);
	make_fish ();
	make_shrimps ();
	printf ("aquarium: %d fish of %d kinds, %d triangles each, a starfish (%d), %d shrimps (%d each); the stick looks around%s; keys: a d w s look, f feeds, k knocks, h the numbers, m the next antialiasing\n",
		n_fish, SPECIES, fish_vertices / 3, star_count / 3, SHRIMPS, shrimp_count / 3, pad_there ? " (A feeds, B knocks, Y the numbers)" : " (no stick here)");

	GLint vp[4] = {0};
	float projection[16], view[16], vpm[16], aspect = 4.0f / 3.0f;
	perf_t perf;
	memset (&perf, 0, sizeof perf);
	bool new_perf = true, numbers = true;
	absolute_time_t last = get_absolute_time ();
	float now = 0, idle = IDLE_S, look = 0, closer = 0, look_to = 0, closer_to = 0, key_x = 0, key_y = 0, key_x_left = 0, key_y_left = 0;
	float auto_feed = 12.0f, school_next = 0, said = 0;
	unsigned was_buttons = 0, frame = 0;
	uint64_t life_us = 0;
	int aa = AA_NONE, aa_shown = -1;
	bool aa_stays = false;
	float aa_left = AA_S;
	while (true)
	{
		if (screen_update ("aquarium", vp))
		{
			aspect = (float) vp[2] / (vp[3] ? vp[3] : 1);
			mat4_perspective (projection, FOVY, aspect, 1.0f, 60.0f);
		}
		absolute_time_t t0 = get_absolute_time ();
		float dt = absolute_time_diff_us (last, t0) / 1e6f, real_dt = dt;
		dt = dt > 1.0f / 20 ? 1.0f / 20 : dt;
		last = t0;
		now += dt;

		/* what's asked */
		float stick_x = 0, stick_y = 0;
		bool want_feed = false, want_knock = false, any = false;
		pad_t pad;
		if (pad_there && pad_read (&pad))
		{
			unsigned pressed = pad.buttons & ~was_buttons;
			was_buttons = pad.buttons;
			stick_x = pad.x;
			stick_y = pad.y;
			want_feed = pressed & PAD_A;
			want_knock = pressed & (PAD_B | PAD_X | PAD_STICK);
			numbers ^= (pressed & (PAD_Y | PAD_START)) != 0;
			any = pad.x != 0.0f || pad.y != 0.0f || pressed;
		}
		for (int c; (c = getchar_timeout_us (0)) != PICO_ERROR_TIMEOUT; any = true)
		{
			switch (c)
			{
			case 'a': case 'A': key_x = -1; key_x_left = KEY_HOLD_S; break;
			case 'd': case 'D': key_x = 1; key_x_left = KEY_HOLD_S; break;
			case 'w': case 'W': key_y = 1; key_y_left = KEY_HOLD_S; break;
			case 's': case 'S': key_y = -1; key_y_left = KEY_HOLD_S; break;
			case 'f': case 'F': want_feed = true; break;
			case 'k': case 'K': case ' ': want_knock = true; break;
			case 'h': case 'H': numbers = !numbers; break;
			case 'm': case 'M': aa = (aa + 1) % AA_MODES; aa_stays = true; break;
			}
		}
		aa_left -= real_dt;
		if (aa_left <= 0 && !aa_stays)
		{
			aa = (aa + 1) % AA_MODES;
			aa_left = AA_S;
		}
		key_x_left -= dt;
		key_y_left -= dt;
		stick_x = clampf (stick_x + (key_x_left > 0 ? key_x : 0), -1.0f, 1.0f);
		stick_y = clampf (stick_y + (key_y_left > 0 ? key_y : 0), -1.0f, 1.0f);
		any = any || key_x_left > 0 || key_y_left > 0;

		/* the eye: in front of the glass, round to a side with the stick, nearer with it forward */
		idle = any ? 0.0f : idle + dt;
		bool by_itself = idle >= IDLE_S;
		look_to = by_itself ? 0.22f * sinf (now * 0.11f) : stick_x != 0 ? 0.40f * stick_x : look_to;
		closer_to = by_itself ? 0.5f + 0.5f * sinf (now * 0.07f) : stick_y != 0 ? clampf (closer_to + stick_y * dt * 0.8f, 0.0f, 1.0f) : closer_to;
		look += (look_to - look) * clampf (dt * 2.0f, 0.0f, 1.0f);
		closer += (closer_to - closer) * clampf (dt * 2.0f, 0.0f, 1.0f);
		float distance = EYE_FAR + (EYE_NEAR - EYE_FAR) * closer;
		const float eye[3] = {distance * sinf (look), 3.1f + 0.3f * sinf (now * 0.13f), distance * cosf (look)};
		const float centre[3] = {0.0f, 2.8f, 0.0f}, up[3] = {0, 1, 0};
		mat4_look_at (view, eye, centre, up);
		mat4_multiply (vpm, projection, view);

		/* a finger: a knock where it touches the glass (the panel's 320 x 240, y down) */
		pgpu_touch_event_t finger;
		while (pgpu_poll_touch (&finger))
		{
			if (finger.type != PGPU_TOUCH_EVENT_DOWN)
			{
				continue;
			}
			float fw[3] = {centre[0] - eye[0], centre[1] - eye[1], centre[2] - eye[2]};
			float l = sqrtf (fw[0] * fw[0] + fw[1] * fw[1] + fw[2] * fw[2]);
			float rt[3] = {-fw[2] / l, 0.0f, fw[0] / l}, rl = sqrtf (rt[0] * rt[0] + rt[2] * rt[2]);
			float ty = tanf (FOVY * PI / 360.0f) * (1.0f - finger.y / 120.0f), tx = tanf (FOVY * PI / 360.0f) * aspect * (finger.x / 160.0f - 1.0f);
			float dir[3] = {fw[0] / l + rt[0] / rl * tx, fw[1] / l + ty, fw[2] / l + rt[2] / rl * tx};
			float k = (TANK_Z - eye[2]) / dir[2];
			knock (clampf (eye[0] + dir[0] * k, -TANK_X, TANK_X), clampf (eye[1] + dir[1] * k, 0.0f, TANK_Y));
			idle = 0.0f;
		}
		if (by_itself && (auto_feed -= dt) < 0)
		{
			auto_feed = 35.0f + 20.0f * frand ();
			want_feed = true;
		}
		if (want_feed)
		{
			feed ();
		}
		if (want_knock)
		{
			knock (eye[0] * 0.3f, 3.0f);
		}

		/* their life */
		uint64_t before = time_us_64 ();
		if ((school_next -= dt) < 0)
		{
			school_next = 4.0f + 4.0f * frand ();
			school_to[0] = (TANK_X - 2.0f) * (2 * frand () - 1);
			school_to[1] = 2.4f + 2.2f * frand ();
			school_to[2] = (TANK_Z - 1.5f) * (2 * frand () - 1);
		}
		for (int i = 0; i < n_fish; i++)
		{
			fish_step (&fish[i], dt);
		}
		for (int i = 0; i < SHRIMPS; i++)
		{
			shrimp_step (&shrimp[i], dt);
		}
		float grains[PELLETS][4];
		int n_grains = 0;
		for (int i = 0; i < PELLETS; i++)
		{
			if (!pellet[i].there)
			{
				continue;
			}
			pellet[i].age += dt;
			if (pellet[i].p[1] > 0.06f)			/* sinking, swaying; then on the sand, for a while */
			{
				pellet[i].p[1] -= 0.55f * dt;
				pellet[i].p[0] += 0.12f * sinf (pellet[i].age * 2.0f + i) * dt;
			}
			pellet[i].there = pellet[i].age < 40.0f;
			memcpy (grains[n_grains], pellet[i].p, 3 * sizeof (float));
			grains[n_grains++][3] = 1.0f;
		}
		life_us += time_us_64 () - before;
		frame++;
		if (now - said >= 10.0f)
		{
			printf ("aquarium: %.1f frames a second, %d fish, %d pellets, their life %u us a frame, %s\n", perf.fps, n_fish, n_grains,
				(unsigned) (life_us / frame), by_itself ? "by itself" : "watched");
			said = now;
			life_us = 0;
			frame = 0;
		}

		/* the picture: on the screen, or twice its size in a texture */
		bool super = aa == AA_SUPER && super_target (vp[2], vp[3]);
		int high = super ? super_h : vp[3];
		if (aa != aa_shown)
		{
			printf ("aquarium: %s%s\n", aa_name[aa], aa == AA_SUPER && !super ? " (not here: the screen is too large)" : "");
		}
		pglSamples (aa == AA_HARD ? 4 : 1);
		if (super)
		{
			glBindFramebuffer (GL_FRAMEBUFFER, super_fb);
			glViewport (0, 0, super_w, super_h);
		}
		else
		{
			glViewport (vp[0], vp[1], vp[2], vp[3]);
		}
		glClearColor (deep[0], deep[1], deep[2], 1.0f);
		glClear (GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
		glEnable (GL_DEPTH_TEST);
		glDepthMask (GL_TRUE);
		glDisable (GL_BLEND);
		const float scroll_x = now * 0.021f, scroll_z = now * 0.013f, fog_from = distance - TANK_Z;

		/* what stands and sways */
		glUseProgram (aqua);
		glUniformMatrix4fv (a.vp, 1, GL_FALSE, vpm);
		glUniform3f (a.eye, eye[0], eye[1], eye[2]);
		glUniform4f (a.time, now, scroll_x, scroll_z, 0.0f);
		glUniform4f (a.water, fog_from, FOG_DENSITY, TANK_Y, 0.0f);
		glUniform3f (a.deep, deep[0], deep[1], deep[2]);
		glUniform3f (a.shallow, shallow[0], shallow[1], shallow[2]);
		glBindBuffer (GL_ARRAY_BUFFER, buffers[0]);
		glEnableVertexAttribArray (a.pos);
		glEnableVertexAttribArray (a.normal);
		glEnableVertexAttribArray (a.uv);
		glEnableVertexAttribArray (a.sway);
		glVertexAttribPointer (a.pos, 3, GL_FLOAT, GL_FALSE, 36, (void *) 0);
		glVertexAttribPointer (a.normal, 3, GL_FLOAT, GL_FALSE, 36, (void *) 12);
		glVertexAttribPointer (a.uv, 2, GL_FLOAT, GL_FALSE, 36, (void *) 24);
		glVertexAttribPointer (a.sway, 1, GL_FLOAT, GL_FALSE, 36, (void *) 32);
		const GLuint part_texture[PARTS] = {t_sand, t_glass, t_rock, t_plant, t_surface};
		for (int i = 0; i < PARTS; i++)
		{
			if (i == PART_SURFACE)				/* (from below it glitters: all caustics) */
			{
				glUniform4f (a.time, now, scroll_x * 2.0f, scroll_z * 2.0f, 0.75f);
			}
			glBindTexture (GL_TEXTURE_2D, part_texture[i]);
			glDrawArrays (GL_TRIANGLES, part[i].first, part[i].count);
		}
		glDisableVertexAttribArray (a.sway);

		/* the fish */
		glUseProgram (fishp);
		glUniformMatrix4fv (f.vp, 1, GL_FALSE, vpm);
		glUniform3f (f.eye, eye[0], eye[1], eye[2]);
		glUniform4f (f.time, now, scroll_x, scroll_z, 0.0f);
		glUniform4f (f.water, fog_from, FOG_DENSITY, TANK_Y, 0.0f);
		glUniform3f (f.deep, deep[0], deep[1], deep[2]);
		glUniform3f (f.shallow, shallow[0], shallow[1], shallow[2]);
		glBindBuffer (GL_ARRAY_BUFFER, buffers[1]);
		glEnableVertexAttribArray (f.pos);
		glEnableVertexAttribArray (f.normal);
		glEnableVertexAttribArray (f.uv);
		glVertexAttribPointer (f.pos, 3, GL_FLOAT, GL_FALSE, 32, (void *) 0);
		glVertexAttribPointer (f.normal, 3, GL_FLOAT, GL_FALSE, 32, (void *) 12);
		glVertexAttribPointer (f.uv, 2, GL_FLOAT, GL_FALSE, 32, (void *) 24);
		int skin_bound = -1;
		for (int i = 0; i < n_fish; i++)
		{
			float m[16];
			fish_model (&fish[i], m);
			if (fish[i].kind != skin_bound)
			{
				glBindTexture (GL_TEXTURE_2D, t_skin[skin_bound = fish[i].kind]);
			}
			float speed = sqrtf (fish[i].v[0] * fish[i].v[0] + fish[i].v[1] * fish[i].v[1] + fish[i].v[2] * fish[i].v[2]);
			glUniformMatrix4fv (f.model, 1, GL_FALSE, m);
			glUniform4f (f.swim, fish[i].phase, 0.07f + 0.05f * clampf (speed / species[fish[i].kind].speed, 0.0f, 2.0f), 0.0f, 0.0f);
			glDrawArrays (GL_TRIANGLES, 0, fish_vertices);
		}
		/* and those on the bottom, of the same kind of vertex */
		glBindBuffer (GL_ARRAY_BUFFER, buffers[6]);
		glVertexAttribPointer (f.pos, 3, GL_FLOAT, GL_FALSE, 32, (void *) 0);
		glVertexAttribPointer (f.normal, 3, GL_FLOAT, GL_FALSE, 32, (void *) 12);
		glVertexAttribPointer (f.uv, 2, GL_FLOAT, GL_FALSE, 32, (void *) 24);
		float m[16];
		star_model (now, m);
		glBindTexture (GL_TEXTURE_2D, t_skin[STAR]);
		glUniformMatrix4fv (f.model, 1, GL_FALSE, m);
		glUniform4f (f.swim, now * 0.6f, 0.025f, 0.0f, 0.0f);			/* (its arms feel about, a little) */
		glDrawArrays (GL_TRIANGLES, star_first, star_count);
		glBindTexture (GL_TEXTURE_2D, t_skin[SHRIMP]);
		for (int i = 0; i < SHRIMPS; i++)
		{
			shrimp_model (&shrimp[i], m);
			glUniformMatrix4fv (f.model, 1, GL_FALSE, m);
			glUniform4f (f.swim, shrimp[i].phase, shrimp[i].does == SHRIMP_DARTS ? 0.10f : 0.012f, 0.07f, 0.0f);
			glDrawArrays (GL_TRIANGLES, shrimp_first, shrimp_count);
		}
		glDisableVertexAttribArray (f.normal);

		/* over them: the pellets; the light's shafts (added); the bubbles */
		float pixels = high / (2.0f * tanf (FOVY * PI / 360.0f));		/* of a unit at distance 1 */
		glUseProgram (bub);
		glUniformMatrix4fv (b_vp, 1, GL_FALSE, vpm);
		glEnableVertexAttribArray (b_seed);
		if (n_grains)
		{
			glBindBuffer (GL_ARRAY_BUFFER, 0);
			glVertexAttribPointer (b_seed, 4, GL_FLOAT, GL_FALSE, 16, grains);
			glUniform4f (b_motion, now, 0.0f, TANK_Y, 0.13f * pixels);
			glUniform4f (b_color, 0.55f, 0.33f, 0.14f, 1.0f);
			glDrawArrays (GL_POINTS, 0, n_grains);
		}
		glDepthMask (GL_FALSE);
		glEnable (GL_BLEND);
		glBlendFunc (GL_ONE, GL_ONE);
		glUseProgram (shafts);
		glUniformMatrix4fv (s_vp, 1, GL_FALSE, vpm);
		glUniform4f (s_time, now, 0.0f, 0.0f, 0.0f);
		glUniform4f (s_color, 0.50f, 0.80f, 0.85f, 1.6f);
		glBindBuffer (GL_ARRAY_BUFFER, buffers[3]);
		glEnableVertexAttribArray (s_pos);
		glEnableVertexAttribArray (s_uv);
		glVertexAttribPointer (s_pos, 3, GL_FLOAT, GL_FALSE, 20, (void *) 0);
		glVertexAttribPointer (s_uv, 2, GL_FLOAT, GL_FALSE, 20, (void *) 12);
		glDrawArrays (GL_TRIANGLES, 0, SHAFTS * 6);
		glBlendFunc (GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
		glUseProgram (bub);
		glBindBuffer (GL_ARRAY_BUFFER, buffers[2]);
		glEnableVertexAttribArray (b_seed);
		glVertexAttribPointer (b_seed, 4, GL_FLOAT, GL_FALSE, 16, (void *) 0);
		glUniform4f (b_motion, now, 0.16f, TANK_Y, 0.24f * pixels);
		glUniform4f (b_color, 0.85f, 0.97f, 1.0f, 0.0f);
		glDrawArrays (GL_POINTS, 0, BUBBLES);
		glBindBuffer (GL_ARRAY_BUFFER, buffers[4]);
		glVertexAttribPointer (b_seed, 4, GL_FLOAT, GL_FALSE, 16, (void *) 0);
		glUniform4f (b_motion, now, 0.012f, TANK_Y, 0.07f * pixels);
		glUniform4f (b_color, 0.80f, 0.90f, 0.85f, 0.0f);
		glDrawArrays (GL_POINTS, 0, SPECKS);
		glDepthMask (GL_TRUE);
		glDisable (GL_BLEND);

		if (super)					/* the texture over the screen: four texels a pixel */
		{
			glBindFramebuffer (GL_FRAMEBUFFER, 0);
			glViewport (vp[0], vp[1], vp[2], vp[3]);
			glDisable (GL_DEPTH_TEST);
			glUseProgram (down);
			glBindTexture (GL_TEXTURE_2D, super_texture);
			glBindBuffer (GL_ARRAY_BUFFER, buffers[5]);
			glEnableVertexAttribArray (d_pos);
			glVertexAttribPointer (d_pos, 2, GL_FLOAT, GL_FALSE, 0, (void *) 0);
			glDrawArrays (GL_TRIANGLES, 0, 6);
		}

		if (new_perf || aa != aa_shown)
		{
			float hs = vp[3] >= 480 ? (float) (vp[3] / 240) : 1.0f;
			hud_begin ();
			if (numbers)
			{
				hud_perf (vp[2] - hud_perf_width (0.5f) - 2, 2, 0.5f, &perf);
			}
			/* which antialiasing: on a dark strip (the sand under it is as bright as the letters) */
			float high_t = (HUD_CHAR_H + 4) * hs, wide_t = (strlen (aa_name[aa]) * HUD_CHAR_W + 8) * hs;
			hud_rect (0, vp[3] - high_t, wide_t, high_t, HUD_RGBA (0, 0, 0, 190));
			hud_text_scaled (4 * hs, vp[3] - high_t + 2 * hs, aa_name[aa], HUD_RGBA (255, 235, 90, 255), hs);
			hud_end ();
			aa_shown = aa;
		}
		hud_draw ();
		pglSwapBuffers ();

		absolute_time_t wait_start = get_absolute_time ();
		pgpu_wait_frame (100);			/* pace on the screen */
		new_perf = perf_frame (absolute_time_diff_us (wait_start, get_absolute_time ()), &perf);
	}
}
