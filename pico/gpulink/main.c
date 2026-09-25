/*
 * gpulink demo - drives the Pi Zero GPU over the command protocol and uses
 * the fixed-function features of the Zero:
 *
 * - a textured, lit cube from vertex and index buffers (DRAW_ELEMENTS, u8
 *   indices, BYTE_NORM normals), a directional and an orbiting point light
 * - two lit spheres (u16 indices): smooth with COLOR_MATERIAL, and flat
 * - a floor and a grid of LINES (DRAW_INLINE) from behind the eye (clipped at
 *   the near plane) into linear fog
 * - stars as POINTS from a buffer (DRAW_ARRAYS, UBYTE_NORM colours)
 * - a translucent quad (blending, no depth writes)
 * - overlays: an alpha-tested A8 sprite, and a texture orientation and byte
 *   order test (bottom-left red, bottom-right green, top-left blue, top-right white)
 *
 * Replies: the reply sampling phase is chosen at start by PING round trips,
 * INFO is read, FRAME_DONE is requested every frame, and STATUS and a PING
 * round trip are reported every second.
 */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "pgpu.h"
#include "plasma_program.h"
#include "cube_program.h"
#include "sparks_program.h"
#include "solid_program.h"
#include "builtin_program.h"
#include "texview_program.h"

#define WIDTH	320
#define HEIGHT	240
#define PI	3.14159265f

/* object ids */
enum { BUF_CUBE = 1, BUF_CUBE_INDEX, BUF_SPHERE, BUF_SPHERE_INDEX, BUF_STARS, BUF_QUAD, BUF_SPARKS };
enum { PROG_PLASMA = 1, PROG_CUBE, PROG_SPARKS, PROG_SOLID, PROG_BUILTIN, PROG_TEXVIEW };
enum { BUF_TEST = 20, BUF_ROW };
enum { TEX_CHECKER = 1, TEX_QUADRANTS, TEX_SPRITE };

#define ALL_ARRAYS	0xFu
#define A(a)		PGPU_ATTRIB (PGPU_ATTR_ ## a)

/* ---- matrices (column-major) --------------------------------------------- */

static void mat_identity (float *m)
{
	memset (m, 0, 16 * sizeof (float));
	m[0] = m[5] = m[10] = m[15] = 1.0f;
}

static void mat_multiply (float *r, const float *a, const float *b)
{
	float t[16];
	for (int c = 0; c < 4; c++)
		for (int i = 0; i < 4; i++)
			t[c*4 + i] =   a[0*4 + i] * b[c*4 + 0] + a[1*4 + i] * b[c*4 + 1]
				     + a[2*4 + i] * b[c*4 + 2] + a[3*4 + i] * b[c*4 + 3];
	memcpy (r, t, sizeof t);
}

static void mat_perspective (float *m, float fovy, float aspect, float near, float far)
{
	float f = 1.0f / tanf (fovy / 2.0f);
	memset (m, 0, 16 * sizeof (float));
	m[0] = f / aspect;
	m[5] = f;
	m[10] = (far + near) / (near - far);
	m[11] = -1.0f;
	m[14] = 2.0f * far * near / (near - far);
}

static void mat_ortho (float *m, float l, float r, float b, float t, float n, float f)
{
	mat_identity (m);
	m[0] = 2.0f / (r - l);
	m[5] = 2.0f / (t - b);
	m[10] = -2.0f / (f - n);
	m[12] = -(r + l) / (r - l);
	m[13] = -(t + b) / (t - b);
	m[14] = -(f + n) / (f - n);
}

static void mat_translate (float *m, float x, float y, float z)
{
	mat_identity (m);
	m[12] = x; m[13] = y; m[14] = z;
}

static void mat_scale (float *m, float s)
{
	mat_identity (m);
	m[0] = m[5] = m[10] = s;
}

static void mat_rotate_x (float *m, float a)
{
	mat_identity (m);
	m[5] = cosf (a); m[6] = sinf (a); m[9] = -sinf (a); m[10] = cosf (a);
}

static void mat_rotate_y (float *m, float a)
{
	mat_identity (m);
	m[0] = cosf (a); m[2] = -sinf (a); m[8] = sinf (a); m[10] = cosf (a);
}

/* ---- helpers --------------------------------------------------------------- */

static uint32_t f2u (float f)
{
	uint32_t u;
	memcpy (&u, &f, sizeof u);
	return u;
}

static uint32_t rgba (float r, float g, float b, float a)
{
	#define C(x) ((x) < 0.0f ? 0u : (x) > 1.0f ? 255u : (uint32_t) ((x) * 255.0f + 0.5f))
	return PGPU_RGBA (C (r), C (g), C (b), C (a));
}

static uint32_t rng_state = 12345;
static float frand (void)			/* 0 .. 1 */
{
	rng_state = rng_state * 1664525u + 1013904223u;
	return (rng_state >> 8) / 16777216.0f;
}

/* ---- objects ----------------------------------------------------------------- */

struct cube_vertex { float pos[3]; int8_t n[4]; float uv[2]; };		/* 24 bytes */
struct sphere_vertex { float pos[3]; int8_t n[4]; uint8_t color[4]; };	/* 20 bytes */
struct star { float pos[3]; uint8_t color[4]; };			/* 16 bytes */

#define SPHERE_STACKS	14
#define SPHERE_SLICES	20
#define SPHERE_VERTICES	((SPHERE_STACKS + 1) * (SPHERE_SLICES + 1))
#define SPHERE_INDICES	(SPHERE_STACKS * SPHERE_SLICES * 6)
#define STARS		300

static void upload_cube (void)
{
	static const struct { int n[3], u[3], v[3]; } faces[6] =
	{
		{{ 1, 0, 0}, {0, 0,-1}, {0, 1, 0}},
		{{-1, 0, 0}, {0, 0, 1}, {0, 1, 0}},
		{{ 0, 1, 0}, {1, 0, 0}, {0, 0,-1}},
		{{ 0,-1, 0}, {1, 0, 0}, {0, 0, 1}},
		{{ 0, 0, 1}, {1, 0, 0}, {0, 1, 0}},
		{{ 0, 0,-1}, {-1, 0, 0}, {0, 1, 0}},
	};
	static const float corners[4][2] = {{-1,-1}, {1,-1}, {1,1}, {-1,1}};	/* CCW seen from outside */

	struct cube_vertex v[24];
	uint8_t index[36];
	for (int f = 0; f < 6; f++)
	{
		for (int i = 0; i < 4; i++)
		{
			struct cube_vertex *p = &v[f * 4 + i];
			for (int k = 0; k < 3; k++)
			{
				p->pos[k] = 0.5f * (faces[f].n[k] + corners[i][0] * faces[f].u[k]
						    + corners[i][1] * faces[f].v[k]);
				p->n[k] = (int8_t) (127 * faces[f].n[k]);
			}
			p->n[3] = 0;
			p->uv[0] = corners[i][0] > 0 ? 1.0f : 0.0f;
			p->uv[1] = corners[i][1] > 0 ? 1.0f : 0.0f;
		}
		static const uint8_t quad[6] = {0, 1, 2, 0, 2, 3};
		for (int i = 0; i < 6; i++)
		{
			index[f * 6 + i] = f * 4 + quad[i];
		}
	}

	pgpu_buffer_create (BUF_CUBE, sizeof v);
	pgpu_buffer_data (BUF_CUBE, 0, v, sizeof v);
	pgpu_buffer_create (BUF_CUBE_INDEX, sizeof index);
	pgpu_buffer_data (BUF_CUBE_INDEX, 0, index, sizeof index);
}

static void upload_sphere (void)
{
	static struct sphere_vertex v[SPHERE_VERTICES];
	static uint16_t index[SPHERE_INDICES];

	for (int i = 0; i <= SPHERE_STACKS; i++)
	{
		float phi = PI * i / SPHERE_STACKS;		/* 0 at the top */
		for (int j = 0; j <= SPHERE_SLICES; j++)
		{
			float theta = 2.0f * PI * j / SPHERE_SLICES;
			float n[3] = {sinf (phi) * cosf (theta), cosf (phi), -sinf (phi) * sinf (theta)};
			struct sphere_vertex *p = &v[i * (SPHERE_SLICES + 1) + j];
			for (int k = 0; k < 3; k++)
			{
				p->pos[k] = n[k];
				p->n[k] = (int8_t) lrintf (127.0f * n[k]);
			}
			p->n[3] = 0;
			/* colour bands for COLOR_MATERIAL */
			p->color[0] = (uint8_t) (127 + 127 * cosf (theta));
			p->color[1] = (uint8_t) (127 + 127 * cosf (theta + 2.1f));
			p->color[2] = (uint8_t) (127 + 127 * cosf (theta + 4.2f));
			p->color[3] = 255;
		}
	}

	uint16_t *q = index;
	for (int i = 0; i < SPHERE_STACKS; i++)
	{
		for (int j = 0; j < SPHERE_SLICES; j++)
		{
			uint16_t a = i * (SPHERE_SLICES + 1) + j, b = a + SPHERE_SLICES + 1;
			/* CCW seen from outside: a (top), b (below), b+1, a+1 */
			*q++ = a; *q++ = b; *q++ = b + 1;
			*q++ = a; *q++ = b + 1; *q++ = a + 1;
		}
	}

	pgpu_buffer_create (BUF_SPHERE, sizeof v);
	pgpu_buffer_data (BUF_SPHERE, 0, v, sizeof v);
	pgpu_buffer_create (BUF_SPHERE_INDEX, sizeof index);
	pgpu_buffer_data (BUF_SPHERE_INDEX, 0, index, sizeof index);
}

static void upload_stars (void)
{
	static struct star s[STARS];
	for (int i = 0; i < STARS; i++)
	{
		s[i].pos[0] = -28.0f + 56.0f * frand ();
		s[i].pos[1] = 1.0f + 20.0f * frand ();
		s[i].pos[2] = -36.0f;
		uint8_t b = (uint8_t) (120 + 135 * frand ());
		s[i].color[0] = b;
		s[i].color[1] = b;
		s[i].color[2] = (uint8_t) (b < 200 ? b + 55 : 255);
		s[i].color[3] = 255;
	}
	pgpu_buffer_create (BUF_STARS, sizeof s);
	pgpu_buffer_data (BUF_STARS, 0, s, sizeof s);
}

static void upload_textures (void)
{
	static uint32_t pixels[64 * 64];

	/* checkerboard with a light border (RGBA8888, R in byte 0) */
	for (int y = 0; y < 64; y++)
		for (int x = 0; x < 64; x++)
		{
			bool border = x < 3 || y < 3 || x > 60 || y > 60;
			bool check = ((x >> 3) ^ (y >> 3)) & 1;
			pixels[y * 64 + x] =   border ? PGPU_RGBA (255, 230, 120, 255)
					     : check ? PGPU_RGBA (230, 230, 230, 255)
					     : PGPU_RGBA (200, 60, 40, 255);
		}
	pgpu_texture_create (TEX_CHECKER, 64, 64, PGPU_RGBA8888);
	pgpu_texture_data (TEX_CHECKER, 0, 0, 64, 64, PGPU_RGBA8888, pixels);
	pgpu_texture_params (TEX_CHECKER, PGPU_LINEAR, PGPU_LINEAR, PGPU_REPEAT, PGPU_REPEAT);

	/* orientation and byte order: rows bottom-up, so row 0 is the bottom */
	for (int y = 0; y < 64; y++)
		for (int x = 0; x < 64; x++)
		{
			bool top = y >= 32, right = x >= 32;
			pixels[y * 64 + x] =   !top && !right ? PGPU_RGBA (255, 0, 0, 255)
					     : !top && right ? PGPU_RGBA (0, 255, 0, 255)
					     : top && !right ? PGPU_RGBA (0, 0, 255, 255)
					     : PGPU_RGBA (255, 255, 255, 255);
		}
	pgpu_texture_create (TEX_QUADRANTS, 64, 64, PGPU_RGBA8888);
	pgpu_texture_data (TEX_QUADRANTS, 0, 0, 64, 64, PGPU_RGBA8888, pixels);
	pgpu_texture_params (TEX_QUADRANTS, PGPU_NEAREST, PGPU_NEAREST, PGPU_CLAMP_TO_EDGE, PGPU_CLAMP_TO_EDGE);

	/* five-pointed star in an A8 texture, soft edge (alpha-tested) */
	uint8_t *alpha = (uint8_t *) pixels;
	for (int y = 0; y < 64; y++)
		for (int x = 0; x < 64; x++)
		{
			float dx = (x - 31.5f) / 31.5f, dy = (y - 31.5f) / 31.5f;
			float r = sqrtf (dx * dx + dy * dy);
			float a = atan2f (dy, dx);
			float edge = 0.55f + 0.4f * cosf (5.0f * (a - PI / 2));
			float v = (edge - r) * 4.0f + 0.5f;
			alpha[y * 64 + x] = (uint8_t) (v < 0 ? 0 : v > 1 ? 255 : v * 255);
		}
	pgpu_texture_create (TEX_SPRITE, 64, 64, PGPU_A8);
	pgpu_texture_data (TEX_SPRITE, 0, 0, 64, 64, PGPU_A8, alpha);
	pgpu_texture_params (TEX_SPRITE, PGPU_LINEAR, PGPU_LINEAR, PGPU_CLAMP_TO_EDGE, PGPU_CLAMP_TO_EDGE);
}

/* ---- per-frame drawing ------------------------------------------------------ */

static void put_pc (uint32_t **pp, float x, float y, float z, uint32_t color)
{
	uint32_t *p = *pp;
	p[0] = f2u (x); p[1] = f2u (y); p[2] = f2u (z); p[3] = color;
	*pp = p + 4;
}

static void put_pt (uint32_t **pp, float x, float y, float s, float t)
{
	uint32_t *p = *pp;
	p[0] = f2u (x); p[1] = f2u (y); p[2] = f2u (0.0f); p[3] = f2u (s); p[4] = f2u (t);
	*pp = p + 5;
}

static void draw_grid (float t)
{
	static uint32_t words[128 * 2 * 4];
	uint32_t *p = words;
	uint32_t c = rgba (0.2f, 0.9f, 0.4f, 1.0f);
	const float y = -1.2f;
	for (int i = -8; i <= 8; i++)			/* along z, from behind the eye (clipped) */
	{
		put_pc (&p, (float) i, y, 3.0f, c);
		put_pc (&p, (float) i, y, -34.0f, c);
	}
	float scroll = fmodf (t * 2.0f, 1.0f);
	for (int k = 0; k < 37; k++)			/* across, moving towards the viewer */
	{
		float z = 3.0f - k + scroll;
		put_pc (&p, -8.0f, y, z, c);
		put_pc (&p, 8.0f, y, z, c);
	}
	uint32_t n = (p - words) / 4;
	pgpu_draw_inline (PGPU_LINES, n, A (POSITION) | A (COLOR), words, 4);
}

static void quad_2d (float x, float y, float w, float h)
{
	uint32_t words[4 * 5], *p = words;
	put_pt (&p, x, y, 0, 0);
	put_pt (&p, x + w, y, 1, 0);
	put_pt (&p, x + w, y + h, 1, 1);
	put_pt (&p, x, y + h, 0, 1);
	pgpu_draw_inline (PGPU_TRIANGLE_FAN, 4, A (POSITION) | A (TEXCOORD), words, 5);
}

static void set_modelview (const float *view, float x, float y, float z, float s,
			   float ax, float ay)
{
	float m[16], a[16], b[16];
	mat_translate (a, x, y, z);
	mat_multiply (m, view, a);
	mat_rotate_y (a, ay);
	mat_rotate_x (b, ax);
	mat_multiply (a, a, b);
	mat_multiply (m, m, a);
	mat_scale (a, s);
	mat_multiply (m, m, a);
	pgpu_load_matrix (PGPU_MODELVIEW, m);
}

static void draw_scene (float t)
{
	const uint32_t sky = rgba (0.06f, 0.08f, 0.20f, 1.0f);
	float projection[16], view[16], ortho[16], identity[16];
	mat_perspective (projection, 1.0f, (float) WIDTH / HEIGHT, 0.5f, 40.0f);
	mat_identity (view);
	mat_identity (identity);

	/* state for the frame (re-sent every frame, so the demo recovers if the
	   Zero reboots) */
	pgpu_viewport (0, 0, WIDTH, HEIGHT, 0.0f, 1.0f);
	pgpu_load_matrix (PGPU_PROJECTION, projection);
	pgpu_load_matrix (PGPU_TEXTURE, identity);
	pgpu_disable (PGPU_CAP_ALL);
	pgpu_enable (PGPU_CAP_DEPTH_TEST | PGPU_CAP_CULL_FACE);
	pgpu_depth_func (PGPU_LESS);
	pgpu_depth_mask (true);
	pgpu_cull_face (PGPU_BACK);
	pgpu_front_face (PGPU_CCW);
	pgpu_shade_model (PGPU_SMOOTH);
	pgpu_color_mask (true, true, true, true);

	pgpu_clear (PGPU_CLEAR_COLOR | PGPU_CLEAR_DEPTH, sky, 1.0f);

	/* stars: POINTS from a buffer */
	pgpu_load_matrix (PGPU_MODELVIEW, view);
	pgpu_array (PGPU_ATTR_POSITION, BUF_STARS, 0, sizeof (struct star), 3, PGPU_FLOAT);
	pgpu_array (PGPU_ATTR_COLOR, BUF_STARS, 12, sizeof (struct star), 4, PGPU_UBYTE_NORM);
	pgpu_arrays_enable (A (POSITION) | A (COLOR));
	pgpu_draw_arrays (PGPU_POINTS, 0, STARS);

	/* ground grid in linear fog */
	pgpu_enable (PGPU_CAP_FOG);
	pgpu_fog (PGPU_FOG_LINEAR, sky, 3.0f, 26.0f, 1.0f);
	pgpu_arrays_enable (0);
	{
		/* floor from behind the eye to the far distance: near-plane clipping */
		uint32_t floor = rgba (0.04f, 0.14f, 0.16f, 1.0f);
		uint32_t words[4 * 4], *p = words;
		put_pc (&p, -8.0f, -1.21f, 4.0f, floor);
		put_pc (&p, 8.0f, -1.21f, 4.0f, floor);
		put_pc (&p, 8.0f, -1.21f, -34.0f, floor);
		put_pc (&p, -8.0f, -1.21f, -34.0f, floor);
		pgpu_draw_inline (PGPU_TRIANGLE_FAN, 4, A (POSITION) | A (COLOR), words, 4);
	}
	draw_grid (t);
	pgpu_disable (PGPU_CAP_FOG);

	/* lights: directional (eye space) and a point light orbiting the cube */
	const float att_none[3] = {1, 0, 0}, att_point[3] = {0.6f, 0.15f, 0.0f};
	const float dir[4] = {-0.4f, 0.6f, 0.7f, 0.0f};
	float orbit[4] = {1.8f * cosf (t * 1.4f), 0.6f, -4.0f + 1.8f * sinf (t * 1.4f), 1.0f};
	pgpu_light (0, dir, rgba (0, 0, 0, 1), rgba (0.8f, 0.8f, 0.8f, 1), rgba (1, 1, 1, 1), att_none);
	pgpu_light (1, orbit, rgba (0, 0, 0, 1), rgba (1.0f, 0.5f, 0.1f, 1), rgba (1, 0.6f, 0.2f, 1), att_point);
	pgpu_light_model (rgba (0.15f, 0.15f, 0.2f, 1), false);
	pgpu_enable (PGPU_CAP_LIGHTING | PGPU_CAP_LIGHT0 | PGPU_CAP_LIGHT1 | PGPU_CAP_NORMALIZE);

	/* textured lit cube: DRAW_ELEMENTS, u8 indices */
	pgpu_material (rgba (1, 1, 1, 1), rgba (1, 1, 1, 1), rgba (0.7f, 0.7f, 0.7f, 1), rgba (0, 0, 0, 1), 24.0f);
	pgpu_enable (PGPU_CAP_TEXTURE_2D);
	pgpu_texture_bind (TEX_CHECKER);
	pgpu_tex_env (PGPU_MODULATE, 0);
	pgpu_array (PGPU_ATTR_POSITION, BUF_CUBE, 0, sizeof (struct cube_vertex), 3, PGPU_FLOAT);
	pgpu_array (PGPU_ATTR_NORMAL, BUF_CUBE, 12, sizeof (struct cube_vertex), 3, PGPU_BYTE_NORM);
	pgpu_array (PGPU_ATTR_TEXCOORD, BUF_CUBE, 16, sizeof (struct cube_vertex), 2, PGPU_FLOAT);
	pgpu_arrays_enable (A (POSITION) | A (NORMAL) | A (TEXCOORD));
	set_modelview (view, 0.0f, 0.1f, -4.0f, 1.3f, t * 0.7f, t * 1.1f);
	pgpu_draw_elements (PGPU_TRIANGLES, 36, PGPU_INDEX_U8, BUF_CUBE_INDEX, 0);
	pgpu_disable (PGPU_CAP_TEXTURE_2D);

	/* spheres: smooth with COLOR_MATERIAL (left), flat (right) */
	pgpu_array (PGPU_ATTR_POSITION, BUF_SPHERE, 0, sizeof (struct sphere_vertex), 3, PGPU_FLOAT);
	pgpu_array (PGPU_ATTR_NORMAL, BUF_SPHERE, 12, sizeof (struct sphere_vertex), 3, PGPU_BYTE_NORM);
	pgpu_array (PGPU_ATTR_COLOR, BUF_SPHERE, 16, sizeof (struct sphere_vertex), 4, PGPU_UBYTE_NORM);
	pgpu_arrays_enable (A (POSITION) | A (NORMAL) | A (COLOR));

	pgpu_enable (PGPU_CAP_COLOR_MATERIAL);
	pgpu_material (rgba (1, 1, 1, 1), rgba (1, 1, 1, 1), rgba (0.8f, 0.8f, 0.8f, 1), rgba (0, 0, 0, 1), 40.0f);
	set_modelview (view, -2.1f, 0.1f, -4.6f, 0.75f, 0.3f, t * 0.8f);
	pgpu_draw_elements (PGPU_TRIANGLES, SPHERE_INDICES, PGPU_INDEX_U16, BUF_SPHERE_INDEX, 0);
	pgpu_disable (PGPU_CAP_COLOR_MATERIAL);

	pgpu_shade_model (PGPU_FLAT);
	pgpu_material (rgba (0.3f, 0.5f, 1, 1), rgba (0.3f, 0.5f, 1, 1), rgba (0.5f, 0.5f, 0.5f, 1),
		       rgba (0, 0, 0, 1), 16.0f);
	set_modelview (view, 2.1f, 0.1f, -4.6f, 0.75f, 0.3f, -t * 0.8f);
	pgpu_draw_elements (PGPU_TRIANGLES, SPHERE_INDICES, PGPU_INDEX_U16, BUF_SPHERE_INDEX, 0);
	pgpu_shade_model (PGPU_SMOOTH);
	pgpu_disable (PGPU_CAP_LIGHTING | PGPU_CAP_LIGHT0 | PGPU_CAP_LIGHT1 | PGPU_CAP_NORMALIZE);

	/* translucent quad sweeping across, in front */
	pgpu_arrays_enable (0);
	pgpu_enable (PGPU_CAP_BLEND);
	pgpu_blend_func (PGPU_SRC_ALPHA, PGPU_ONE_MINUS_SRC_ALPHA);
	pgpu_depth_mask (false);
	pgpu_disable (PGPU_CAP_CULL_FACE);
	pgpu_load_matrix (PGPU_MODELVIEW, view);
	{
		float x = 2.4f * sinf (t * 0.6f);
		uint32_t c = rgba (0.3f, 0.6f, 1.0f, 0.45f);
		uint32_t words[4 * 4], *p = words;
		put_pc (&p, x - 0.5f, -0.9f, -3.0f, c);
		put_pc (&p, x + 0.5f, -0.9f, -3.0f, c);
		put_pc (&p, x + 0.5f, 0.9f, -3.0f, c);
		put_pc (&p, x - 0.5f, 0.9f, -3.0f, c);
		pgpu_draw_inline (PGPU_TRIANGLE_FAN, 4, A (POSITION) | A (COLOR), words, 4);
	}
	pgpu_disable (PGPU_CAP_BLEND);
	pgpu_depth_mask (true);

	/* 2D overlays in window coordinates */
	mat_ortho (ortho, 0, WIDTH, 0, HEIGHT, -1, 1);
	pgpu_load_matrix (PGPU_PROJECTION, ortho);
	pgpu_load_matrix (PGPU_MODELVIEW, identity);
	pgpu_disable (PGPU_CAP_DEPTH_TEST);
	pgpu_enable (PGPU_CAP_TEXTURE_2D);

	/* alpha-tested sprite, spinning by the texture matrix */
	pgpu_texture_bind (TEX_SPRITE);
	pgpu_tex_env (PGPU_MODULATE, 0);
	pgpu_enable (PGPU_CAP_ALPHA_TEST);
	pgpu_alpha_func (PGPU_GREATER, 0.5f);
	pgpu_color (rgba (1.0f, 0.85f, 0.2f, 1));
	{
		float tm[16], a[16], b[16];
		mat_translate (a, 0.5f, 0.5f, 0);
		mat_identity (b);
		b[0] = cosf (t); b[1] = sinf (t); b[4] = -sinf (t); b[5] = cosf (t);
		mat_multiply (tm, a, b);
		mat_translate (a, -0.5f, -0.5f, 0);
		mat_multiply (tm, tm, a);
		pgpu_load_matrix (PGPU_TEXTURE, tm);
	}
	quad_2d (6, HEIGHT - 62, 56, 56);
	pgpu_load_matrix (PGPU_TEXTURE, identity);
	pgpu_disable (PGPU_CAP_ALPHA_TEST);
	pgpu_color (rgba (1, 1, 1, 1));

	/* texture orientation and byte order test */
	pgpu_texture_bind (TEX_QUADRANTS);
	pgpu_tex_env (PGPU_REPLACE, 0);
	quad_2d (WIDTH - 62, HEIGHT - 62, 56, 56);
	pgpu_disable (PGPU_CAP_TEXTURE_2D);
}

/* ---- self test ---------------------------------------------------------------
 *
 * Three frames, the last two without CLEAR, then a screenshot request
 * (DEBUG_SCREENSHOT) and a hold. Expected in the screenshot, on dark green:
 *   top row:    white quad (frame 2) | colour mask R only over it: white; over green: (255,128,0)
 *   middle:     back sides of two lit quads: two-sided (bright red) | one-sided (dark, ambient only)
 *               LINE_LOOP square (cyan) | LINE_STRIP zigzag (yellow)
 *   top right:  4x4, 2x2 (bottom green, red; top white, blue), 1x4 (red bottom, blue top)
 *   right:      2x2 RGB565, RGBA4444, RGBA5551 (quadrants), L8, LA88 (grey ramp),
 *               4x4 ETC1 (red bottom, blue top)
 *   bottom:     8x8 quadrant texture: MODULATE yellow | REPLACE | DECAL | BLEND with magenta
 * (quadrant textures: bottom-left red, bottom-right green, top-left blue, top-right white)
 */
#define TEX_SMALL	(TEX_SPRITE + 1)

static void self_test_quad (float x, float y, float w, float h)
{
	uint32_t words[4 * 5], *p = words;
	put_pt (&p, x, y, 0, 0);
	put_pt (&p, x + w, y, 1, 0);
	put_pt (&p, x + w, y + h, 1, 1);
	put_pt (&p, x, y + h, 0, 1);
	pgpu_draw_inline (PGPU_TRIANGLE_FAN, 4, A (POSITION) | A (TEXCOORD), words, 5);
}

static void self_test (void)
{
	float ortho[16], identity[16], projection[16], m[16], a[16];
	mat_ortho (ortho, 0, WIDTH, 0, HEIGHT, -1, 1);
	mat_identity (identity);

	/* 8x8 RGBA8888 texture (narrower than the 16-texel raster row padding) */
	static uint32_t small[8 * 8];
	for (int y = 0; y < 8; y++)
		for (int x = 0; x < 8; x++)
			small[y * 8 + x] =   y < 4 ? (x < 4 ? PGPU_RGBA (255, 0, 0, 255) : PGPU_RGBA (0, 255, 0, 255))
					   : (x < 4 ? PGPU_RGBA (0, 0, 255, 255) : PGPU_RGBA (255, 255, 255, 255));
	pgpu_texture_create (TEX_SMALL, 8, 8, PGPU_RGBA8888);
	pgpu_texture_data (TEX_SMALL, 0, 0, 8, 8, PGPU_RGBA8888, small);
	pgpu_texture_params (TEX_SMALL, PGPU_NEAREST, PGPU_NEAREST, PGPU_CLAMP_TO_EDGE, PGPU_CLAMP_TO_EDGE);

	/* 4x4, 2x2: bottom green, red; top white, blue. 1x4: red, red, blue, blue from the bottom */
	static const uint16_t sizes[3][2] = {{4, 4}, {2, 2}, {1, 4}};
	for (int i = 0; i < 3; i++)
	{
		int w = sizes[i][0], h = sizes[i][1];
		uint32_t tiny[16];
		for (int y = 0; y < h; y++)
			for (int x = 0; x < w; x++)
				tiny[y * w + x] =   y < h / 2 ? (x < w / 2 ? PGPU_RGBA (0, 255, 0, 255) : PGPU_RGBA (255, 0, 0, 255))
						  : (x < w / 2 ? PGPU_RGBA (255, 255, 255, 255) : PGPU_RGBA (0, 0, 255, 255));
		pgpu_texture_create (TEX_SMALL + 1 + i, w, h, PGPU_RGBA8888);
		pgpu_texture_data (TEX_SMALL + 1 + i, 0, 0, w, h, PGPU_RGBA8888, tiny);
		pgpu_texture_params (TEX_SMALL + 1 + i, PGPU_NEAREST, PGPU_NEAREST, PGPU_CLAMP_TO_EDGE, PGPU_CLAMP_TO_EDGE);
	}

	/* the other formats, 2x2: bottom-left red, bottom-right green, top-left
	   blue, top-right white; L8 and LA88: 0, 85 (bottom), 170, 255 (top) */
	static const uint16_t rgb565[4] = {0xF800, 0x07E0, 0x001F, 0xFFFF};
	static const uint16_t rgba4444[4] = {0xF00F, 0x0F0F, 0x00FF, 0xFFFF};
	static const uint16_t rgba5551[4] = {0xF801, 0x07C1, 0x003F, 0xFFFF};
	static const uint8_t l8[8] = {0, 85, 0, 0, 170, 255, 0, 0};		/* rows padded to 4 bytes */
	static const uint8_t la88[8] = {0, 255, 85, 255, 170, 255, 255, 255};
	/* ETC1, one block, individual mode, flip: rows 0-1 red, rows 2-3 blue (+2 modifier) */
	static const uint8_t etc1[8] = {0xF0, 0x00, 0x0F, 0x01, 0, 0, 0, 0};
	static const struct { uint32_t format, size; const void *data; } formats[6] =
	{
		{PGPU_RGB565, 2, rgb565}, {PGPU_RGBA4444, 2, rgba4444}, {PGPU_RGBA5551, 2, rgba5551},
		{PGPU_L8, 2, l8}, {PGPU_LA88, 2, la88}, {PGPU_ETC1, 4, etc1},
	};
	for (int i = 0; i < 6; i++)
	{
		uint32_t id = TEX_SMALL + 4 + i, s = formats[i].size;
		pgpu_texture_create (id, s, s, formats[i].format);
		pgpu_texture_data (id, 0, 0, s, s, formats[i].format, formats[i].data);
		pgpu_texture_params (id, PGPU_NEAREST, PGPU_NEAREST, PGPU_CLAMP_TO_EDGE, PGPU_CLAMP_TO_EDGE);
	}

	pgpu_viewport (0, 0, WIDTH, HEIGHT, 0.0f, 1.0f);
	pgpu_disable (PGPU_CAP_ALL);
	pgpu_arrays_enable (0);
	pgpu_color_mask (true, true, true, true);
	pgpu_load_matrix (PGPU_PROJECTION, ortho);
	pgpu_load_matrix (PGPU_MODELVIEW, identity);
	pgpu_load_matrix (PGPU_TEXTURE, identity);

	/* frame 1: clear only */
	pgpu_clear (PGPU_CLEAR_COLOR | PGPU_CLEAR_DEPTH, PGPU_RGBA (0, 128, 0, 255), 1.0f);
	pgpu_frame_end (0);

	/* frame 2: no CLEAR, a white quad */
	pgpu_color (PGPU_RGBA (255, 255, 255, 255));
	self_test_quad (20, 180, 80, 45);
	pgpu_frame_end (0);

	/* frame 3: no CLEAR */
	pgpu_color_mask (true, false, false, true);
	self_test_quad (60, 170, 100, 30);
	pgpu_color_mask (true, true, true, true);

	/* two-sided lighting: quads facing away from the viewer (culling off) */
	mat_perspective (projection, 1.0f, (float) WIDTH / HEIGHT, 0.5f, 20.0f);
	pgpu_load_matrix (PGPU_PROJECTION, projection);
	const float dir[4] = {0, 0, 1, 0}, att[3] = {1, 0, 0};
	pgpu_light (0, dir, PGPU_RGBA (0, 0, 0, 255), PGPU_RGBA (255, 255, 255, 255),
		    PGPU_RGBA (0, 0, 0, 255), att);
	pgpu_material (rgba (0.2f, 0.2f, 0.2f, 1), rgba (1, 0.1f, 0.1f, 1), rgba (0, 0, 0, 1), rgba (0, 0, 0, 1), 0);
	pgpu_enable (PGPU_CAP_LIGHTING | PGPU_CAP_LIGHT0);
	pgpu_normal (0, 0, -1);				/* front side faces -z, away from the eye */
	for (int two_side = 1; two_side >= 0; two_side--)
	{
		pgpu_light_model (rgba (0.2f, 0.2f, 0.2f, 1), two_side);
		mat_translate (m, two_side ? -2.6f : -1.2f, 0.0f, -5.0f);
		mat_scale (a, 1.2f);
		mat_multiply (m, m, a);
		pgpu_load_matrix (PGPU_MODELVIEW, m);
		/* CW as seen from the eye = CCW (front) as seen from behind */
		uint32_t words[4 * 3], *p = words;
		float quad[4][2] = {{0, 0}, {0, 1}, {1, 1}, {1, 0}};
		for (int i = 0; i < 4; i++)
		{
			p[0] = f2u (quad[i][0]); p[1] = f2u (quad[i][1]); p[2] = f2u (0.0f);
			p += 3;
		}
		pgpu_draw_inline (PGPU_TRIANGLE_FAN, 4, A (POSITION), words, 3);
	}
	pgpu_disable (PGPU_CAP_LIGHTING | PGPU_CAP_LIGHT0);
	pgpu_light_model (rgba (0.2f, 0.2f, 0.2f, 1), false);
	pgpu_load_matrix (PGPU_PROJECTION, ortho);
	pgpu_load_matrix (PGPU_MODELVIEW, identity);

	/* LINE_LOOP and LINE_STRIP */
	{
		uint32_t words[6 * 4], *p = words;
		uint32_t cyan = PGPU_RGBA (0, 255, 255, 255), yellow = PGPU_RGBA (255, 255, 0, 255);
		put_pc (&p, 190, 100, 0, cyan);
		put_pc (&p, 240, 100, 0, cyan);
		put_pc (&p, 240, 150, 0, cyan);
		put_pc (&p, 190, 150, 0, cyan);
		pgpu_draw_inline (PGPU_LINE_LOOP, 4, A (POSITION) | A (COLOR), words, 4);
		p = words;
		for (int i = 0; i < 6; i++)
		{
			put_pc (&p, 255 + i * 12, i & 1 ? 150 : 100, 0, yellow);
		}
		pgpu_draw_inline (PGPU_LINE_STRIP, 6, A (POSITION) | A (COLOR), words, 4);
	}

	/* texture environments with the 8x8 texture */
	pgpu_enable (PGPU_CAP_TEXTURE_2D);
	pgpu_texture_bind (TEX_SMALL);
	pgpu_color (PGPU_RGBA (255, 255, 0, 255));
	static const uint32_t modes[4] = {PGPU_MODULATE, PGPU_REPLACE, PGPU_DECAL, PGPU_BLEND};
	for (int i = 0; i < 4; i++)
	{
		pgpu_tex_env (modes[i], PGPU_RGBA (255, 0, 255, 255));
		self_test_quad (10 + i * 78, 10, 64, 64);
	}
	pgpu_tex_env (PGPU_REPLACE, 0);
	for (int i = 0; i < 3; i++)
	{
		pgpu_texture_bind (TEX_SMALL + 1 + i);
		self_test_quad (190 + i * 44, 180, 36, 36);
	}
	for (int i = 0; i < 6; i++)
	{
		pgpu_texture_bind (TEX_SMALL + 4 + i);
		self_test_quad (172 + i * 24, 153, 22, 22);
	}
	pgpu_disable (PGPU_CAP_TEXTURE_2D);
	pgpu_color (PGPU_RGBA (255, 255, 255, 255));
	pgpu_frame_end (0);

	pgpu_begin (PGPU_OP_DEBUG_SCREENSHOT, 0);
	pgpu_end ();
	pgpu_flush ();
	printf ("gpulink: self test frames sent, screenshot requested\n");
	sleep_ms (3000);
}

static void upload_programs (void);

/* RESET, read INFO, upload the objects (also after the Zero rebooted) */
static void setup (void)
{
	pgpu_reset ();
	pgpu_flush ();

	pgpu_reply_t r;
	if (pgpu_wait_reply (PGPU_REPLY_INFO, &r, 100) && r.length >= 7)
	{
		printf ("gpulink: INFO version %08lx, %lux%lu, max texture %lu, buffers %lu, textures %lu, "
			"lights %lu, ring %lu KB\n",
			(unsigned long) r.payload[0], (unsigned long) (r.payload[1] & 0xFFFF),
			(unsigned long) (r.payload[1] >> 16), (unsigned long) r.payload[2],
			(unsigned long) r.payload[3], (unsigned long) r.payload[4],
			(unsigned long) r.payload[5], (unsigned long) r.payload[6] / 1024);
	}
	else
	{
		printf ("gpulink: no INFO reply to RESET\n");
	}

	upload_cube ();
	upload_sphere ();
	upload_stars ();
	upload_textures ();
	upload_programs ();
	pgpu_flush ();
}

/* ---- programs (GL ES 2.0 subset) --------------------------------------------- */

#define SPARKS		240

static void upload_programs (void)
{
	pgpu_program_create (PROG_PLASMA, plasma_program, PLASMA_WORDS);
	pgpu_program_create (PROG_CUBE, cube_program, CUBE_WORDS);
	pgpu_program_create (PROG_SPARKS, sparks_program, SPARKS_WORDS);
	pgpu_program_create (PROG_SOLID, solid_program, SOLID_WORDS);
	pgpu_program_create (PROG_BUILTIN, builtin_program, BUILTIN_WORDS);
	pgpu_program_create (PROG_TEXVIEW, texview_program, TEXVIEW_WORDS);

	static const float quad[8] = {-1, -1, 1, -1, 1, 1, -1, 1};
	pgpu_buffer_create (BUF_QUAD, sizeof quad);
	pgpu_buffer_data (BUF_QUAD, 0, quad, sizeof quad);

	static struct star s[SPARKS];		/* position, colour */
	for (int i = 0; i < SPARKS; i++)
	{
		float a = 2.0f * PI * frand (), r = 0.9f + 0.8f * frand ();
		s[i].pos[0] = r * cosf (a);
		s[i].pos[1] = 3.0f * frand ();
		s[i].pos[2] = r * sinf (a);
		s[i].color[0] = 255;
		s[i].color[1] = (uint8_t) (120 + 120 * frand ());
		s[i].color[2] = (uint8_t) (40 * frand ());
		s[i].color[3] = 255;
	}
	pgpu_buffer_create (BUF_SPARKS, sizeof s);
	pgpu_buffer_data (BUF_SPARKS, 0, s, sizeof s);
}

static void draw_program_scene (float t)
{
	float projection[16], view[16], model[16], vp[16], mvp[16], a[16], b[16];
	mat_perspective (projection, 1.0f, (float) WIDTH / HEIGHT, 0.5f, 40.0f);
	mat_translate (view, 0.0f, -0.1f, -4.2f);
	mat_multiply (vp, projection, view);

	pgpu_viewport (0, 0, WIDTH, HEIGHT, 0.0f, 1.0f);
	pgpu_disable (PGPU_CAP_ALL);
	pgpu_depth_mask (true);
	pgpu_color_mask (true, true, true, true);
	pgpu_cull_face (PGPU_BACK);
	pgpu_front_face (PGPU_CCW);
	pgpu_arrays_enable (0);
	pgpu_clear (PGPU_CLEAR_COLOR | PGPU_CLEAR_DEPTH, PGPU_RGBA (0, 0, 0, 255), 1.0f);

	/* plasma background (no depth test, no depth writes) */
	pgpu_use_program (PROG_PLASMA);
	pgpu_program_uniform1f (PROG_PLASMA, plasma_u_time, t);
	pgpu_program_uniform1f (PROG_PLASMA, plasma_u_brightness, 0.45f);
	pgpu_attrib_array (PLASMA_A_POS, BUF_QUAD, 0, 0, 2, PGPU_FLOAT);
	pgpu_attribs_enable (1u << PLASMA_A_POS);
	pgpu_depth_mask (false);
	pgpu_draw_arrays (PGPU_TRIANGLE_FAN, 0, 4);
	pgpu_depth_mask (true);

	/* textured cube lit by an orbiting point light */
	pgpu_enable (PGPU_CAP_DEPTH_TEST | PGPU_CAP_CULL_FACE);
	pgpu_depth_func (PGPU_LESS);
	mat_rotate_y (a, t * 0.8f);
	mat_rotate_x (b, t * 0.5f);
	mat_multiply (model, a, b);
	mat_scale (a, 1.4f);
	mat_multiply (model, model, a);
	mat_multiply (mvp, vp, model);
	float light[3] = {2.5f * cosf (t * 1.3f), 1.2f, 2.5f * sinf (t * 1.3f)};
	float light_color[3] = {1.0f, 0.9f, 0.75f};
	float eye[3] = {0.0f, 0.1f, 4.2f};
	pgpu_use_program (PROG_CUBE);
	PGPU_UNIFORM (PROG_CUBE, cube_u_mvp, mvp);
	PGPU_UNIFORM (PROG_CUBE, cube_u_model, model);
	PGPU_UNIFORM (PROG_CUBE, cube_u_light_pos, light);
	PGPU_UNIFORM (PROG_CUBE, cube_u_light_color, light_color);
	PGPU_UNIFORM (PROG_CUBE, cube_u_eye, eye);
	pgpu_texture_bind_unit (0, TEX_CHECKER);
	pgpu_attrib_array (CUBE_A_POS, BUF_CUBE, 0, sizeof (struct cube_vertex), 3, PGPU_FLOAT);
	pgpu_attrib_array (CUBE_A_NORMAL, BUF_CUBE, 12, sizeof (struct cube_vertex), 3, PGPU_BYTE_NORM);
	pgpu_attrib_array (CUBE_A_UV, BUF_CUBE, 16, sizeof (struct cube_vertex), 2, PGPU_FLOAT);
	pgpu_attribs_enable (1u << CUBE_A_POS | 1u << CUBE_A_NORMAL | 1u << CUBE_A_UV);
	pgpu_draw_elements (PGPU_TRIANGLES, 36, PGPU_INDEX_U8, BUF_CUBE_INDEX, 0);

	/* sparks: point sprites, additive, depth tested but not written */
	pgpu_use_program (PROG_SPARKS);
	PGPU_UNIFORM (PROG_SPARKS, sparks_u_mvp, vp);
	pgpu_program_uniform1f (PROG_SPARKS, sparks_u_time, t);
	pgpu_program_uniform1f (PROG_SPARKS, sparks_u_size, 40.0f);
	pgpu_attrib_array (SPARKS_A_POS, BUF_SPARKS, 0, sizeof (struct star), 3, PGPU_FLOAT);
	pgpu_attrib_array (SPARKS_A_COLOR, BUF_SPARKS, 12, sizeof (struct star), 4, PGPU_UBYTE_NORM);
	pgpu_attribs_enable (1u << SPARKS_A_POS | 1u << SPARKS_A_COLOR);
	pgpu_enable (PGPU_CAP_BLEND);
	pgpu_blend_func (PGPU_ONE, PGPU_ONE);
	pgpu_depth_mask (false);
	pgpu_draw_arrays (PGPU_POINTS, 0, SPARKS);
	pgpu_disable (PGPU_CAP_BLEND);
	pgpu_depth_mask (true);

	/* fixed-function overlay in the same frame: the texture test quad */
	float ortho[16], identity[16];
	mat_ortho (ortho, 0, WIDTH, 0, HEIGHT, -1, 1);
	mat_identity (identity);
	pgpu_use_program (0);
	pgpu_disable (PGPU_CAP_ALL);
	pgpu_load_matrix (PGPU_PROJECTION, ortho);
	pgpu_load_matrix (PGPU_MODELVIEW, identity);
	pgpu_load_matrix (PGPU_TEXTURE, identity);
	pgpu_enable (PGPU_CAP_TEXTURE_2D);
	pgpu_texture_bind (TEX_QUADRANTS);
	pgpu_tex_env (PGPU_REPLACE, 0);
	quad_2d (WIDTH - 62, HEIGHT - 62, 56, 56);
	pgpu_disable (PGPU_CAP_TEXTURE_2D);
}

/* ---- program self test ---------------------------------------------------------
 *
 * One frame, then a screenshot request. Expected, on black:
 *   top left:     red square (drawn, then its buffer is changed: copy-on-write
 *                 keeps the square) | green triangle (the changed buffer)
 *   top right:    white square, half-covered by a blue square at alpha 0.5
 *   middle left:  yellow LINE_LOOP square outline (lines variant)
 *   middle right: row of 8 green points (colour array disabled: VERTEX_ATTRIB)
 *   bottom right: cube with the quadrant texture through sampler 0 -> unit 3
 *   One ERROR 10 (points with the solid program: no points variant).
 */
static void rect (float x, float y, float w, float h, float r, float g, float b, float a)
{
	float rc[4] = {x, y, w, h}, c[4] = {r, g, b, a};
	PGPU_UNIFORM (PROG_SOLID, solid_u_rect, rc);
	PGPU_UNIFORM (PROG_SOLID, solid_u_color, c);
}

static void program_self_test (void)
{
	static const float square[8] = {0, 0, 1, 0, 1, 1, 0, 1};
	static const float triangle[8] = {0, 0, 1, 0, 0.5f, 1, 0.5f, 1};
	pgpu_buffer_create (BUF_TEST, sizeof square);
	pgpu_buffer_data (BUF_TEST, 0, square, sizeof square);

	static float row[8][3];
	for (int i = 0; i < 8; i++)
	{
		row[i][0] = 0.1f + 0.1f * i;	/* x */
		row[i][1] = 1.5f;		/* y: mod (1.5, 3) - 1.5 = 0 */
		row[i][2] = 0.0f;
	}
	pgpu_buffer_create (BUF_ROW, sizeof row);
	pgpu_buffer_data (BUF_ROW, 0, row, sizeof row);

	pgpu_viewport (0, 0, WIDTH, HEIGHT, 0.0f, 1.0f);
	pgpu_disable (PGPU_CAP_ALL);
	pgpu_depth_mask (true);
	pgpu_color_mask (true, true, true, true);
	pgpu_clear (PGPU_CLEAR_COLOR | PGPU_CLEAR_DEPTH, PGPU_RGBA (0, 0, 0, 255), 1.0f);

	pgpu_use_program (PROG_SOLID);
	pgpu_attrib_array (SOLID_A_POS, BUF_TEST, 0, 0, 2, PGPU_FLOAT);
	pgpu_attribs_enable (1u << SOLID_A_POS);

	/* copy-on-write */
	rect (-0.95f, 0.45f, 0.4f, 0.45f, 1, 0, 0, 1);
	pgpu_draw_arrays (PGPU_TRIANGLE_FAN, 0, 4);
	pgpu_buffer_data (BUF_TEST, 0, triangle, sizeof triangle);
	rect (-0.45f, 0.45f, 0.4f, 0.45f, 0, 1, 0, 1);
	pgpu_draw_arrays (PGPU_TRIANGLE_FAN, 0, 4);
	pgpu_buffer_data (BUF_TEST, 0, square, sizeof square);

	/* alpha blending variant */
	rect (0.1f, 0.45f, 0.4f, 0.45f, 1, 1, 1, 1);
	pgpu_draw_arrays (PGPU_TRIANGLE_FAN, 0, 4);
	pgpu_enable (PGPU_CAP_BLEND);
	pgpu_blend_func (PGPU_SRC_ALPHA, PGPU_ONE_MINUS_SRC_ALPHA);
	rect (0.3f, 0.3f, 0.4f, 0.45f, 0, 0, 1, 0.5f);
	pgpu_draw_arrays (PGPU_TRIANGLE_FAN, 0, 4);
	pgpu_disable (PGPU_CAP_BLEND);

	/* lines variant; points: no variant (one ERROR 10) */
	rect (-0.9f, -0.2f, 0.4f, 0.4f, 1, 1, 0, 1);
	pgpu_draw_arrays (PGPU_LINE_LOOP, 0, 4);
	pgpu_draw_arrays (PGPU_POINTS, 0, 4);

	/* constant attribute: colour array disabled */
	float identity[16];
	mat_identity (identity);
	pgpu_use_program (PROG_SPARKS);
	PGPU_UNIFORM (PROG_SPARKS, sparks_u_mvp, identity);
	pgpu_program_uniform1f (PROG_SPARKS, sparks_u_time, 0.0f);
	pgpu_program_uniform1f (PROG_SPARKS, sparks_u_size, 6.0f);
	pgpu_attrib_array (SPARKS_A_POS, BUF_ROW, 0, 0, 3, PGPU_FLOAT);
	pgpu_attribs_enable (1u << SPARKS_A_POS);
	pgpu_vertex_attrib (SPARKS_A_COLOR, 0, 1, 0, 1);
	pgpu_enable (PGPU_CAP_BLEND);
	pgpu_blend_func (PGPU_ONE, PGPU_ONE);
	pgpu_draw_arrays (PGPU_POINTS, 0, 8);
	pgpu_disable (PGPU_CAP_BLEND);

	/* sampler 0 -> texture unit 3 */
	float model[16], a[16], mvp[16], proj[16], view[16], vp[16];
	mat_perspective (proj, 1.0f, (float) WIDTH / HEIGHT, 0.5f, 20.0f);
	mat_translate (view, 1.6f, -0.9f, -4.0f);
	mat_multiply (vp, proj, view);
	mat_rotate_y (model, 0.6f);
	mat_rotate_x (a, 0.4f);
	mat_multiply (model, model, a);
	mat_multiply (mvp, vp, model);
	float light[3] = {0, 0, 5}, white[3] = {1, 1, 1}, eye[3] = {0, 0, 5};
	pgpu_enable (PGPU_CAP_DEPTH_TEST | PGPU_CAP_CULL_FACE);
	pgpu_use_program (PROG_CUBE);
	PGPU_UNIFORM (PROG_CUBE, cube_u_mvp, mvp);
	PGPU_UNIFORM (PROG_CUBE, cube_u_model, model);
	PGPU_UNIFORM (PROG_CUBE, cube_u_light_pos, light);
	PGPU_UNIFORM (PROG_CUBE, cube_u_light_color, white);
	PGPU_UNIFORM (PROG_CUBE, cube_u_eye, eye);
	pgpu_program_sampler (PROG_CUBE, CUBE_U_TEXTURE, 3);
	pgpu_texture_bind_unit (0, TEX_CHECKER);
	pgpu_texture_bind_unit (3, TEX_QUADRANTS);
	pgpu_attrib_array (CUBE_A_POS, BUF_CUBE, 0, sizeof (struct cube_vertex), 3, PGPU_FLOAT);
	pgpu_attrib_array (CUBE_A_NORMAL, BUF_CUBE, 12, sizeof (struct cube_vertex), 3, PGPU_BYTE_NORM);
	pgpu_attrib_array (CUBE_A_UV, BUF_CUBE, 16, sizeof (struct cube_vertex), 2, PGPU_FLOAT);
	pgpu_attribs_enable (1u << CUBE_A_POS | 1u << CUBE_A_NORMAL | 1u << CUBE_A_UV);
	pgpu_draw_elements (PGPU_TRIANGLES, 36, PGPU_INDEX_U8, BUF_CUBE_INDEX, 0);
	pgpu_program_sampler (PROG_CUBE, CUBE_U_TEXTURE, 0);
	pgpu_use_program (0);

	pgpu_frame_end (0);
	pgpu_begin (PGPU_OP_DEBUG_SCREENSHOT, 0);
	pgpu_end ();
	pgpu_flush ();
	printf ("gpulink: program self test frame sent, screenshot requested\n");
	sleep_ms (3000);
}

/* ---- self test 3: built-in variables, scissor, polygon offset, line width,
 * depth across frames, client-side arrays --------------------------------------
 *
 * Two frames (identity matrices: coordinates are clip space), the second
 * without depth CLEAR, then a screenshot request. Expected:
 *   top band, left:   gl_FragCoord: red = x / 320, green = y / 240 (GL: y up)
 *   top band, middle: gl_DepthRange with depth range 0.25 .. 0.75: (0.25, 0.75, 0.5)
 *   top band, right:  one 48-pixel point, gl_PointCoord: black top left, red top
 *                     right, green bottom left (ES: origin upper left)
 *   2nd row, left:    two triangles, culling off: front facing green, back red
 *   2nd row, middle:  polygon offset: red | green (the green quad at the same
 *                     depth wins only with the offset)
 *   2nd row, right:   scissor: small blue (fixed function) and magenta (program)
 *                     rectangles of full-screen quads
 *   3rd row:          yellow 6-pixel line (fixed function), cyan 4-pixel line
 *                     loop (program)
 *   bottom left:      client arrays: orange triangle (arrays), white square
 *                     (elements)
 *   bottom middle:    red frame around a black hole: the red quad is behind
 *                     depth drawn in the previous frame
 */
static void builtin_rect (float x, float y, float w, float h, float mode)
{
	float rc[4] = {x, y, w, h};
	PGPU_UNIFORM (PROG_BUILTIN, builtin_u_rect, rc);
	pgpu_program_uniform1f (PROG_BUILTIN, builtin_u_mode, mode);
}

static void ff_quad (float x0, float y0, float x1, float y1, float z, uint32_t color)
{
	uint32_t words[4 * 4], *p = words;
	put_pc (&p, x0, y0, z, color);
	put_pc (&p, x1, y0, z, color);
	put_pc (&p, x1, y1, z, color);
	put_pc (&p, x0, y1, z, color);
	pgpu_draw_inline (PGPU_TRIANGLE_FAN, 4, A (POSITION) | A (COLOR), words, 4);
}

static void self_test_3 (void)
{
	float identity[16];
	mat_identity (identity);

	pgpu_viewport (0, 0, WIDTH, HEIGHT, 0.0f, 1.0f);
	pgpu_use_program (0);
	pgpu_disable (PGPU_CAP_ALL);
	pgpu_arrays_enable (0);
	pgpu_attribs_enable (0);
	pgpu_color_mask (true, true, true, true);
	pgpu_depth_mask (true);
	pgpu_depth_func (PGPU_LESS);
	pgpu_load_matrix (PGPU_PROJECTION, identity);
	pgpu_load_matrix (PGPU_MODELVIEW, identity);
	pgpu_load_matrix (PGPU_TEXTURE, identity);

	/* frame 1: depth only where the hole will be (colour cleared again below) */
	pgpu_clear (PGPU_CLEAR_COLOR | PGPU_CLEAR_DEPTH, PGPU_RGBA (0, 0, 0, 255), 1.0f);
	pgpu_enable (PGPU_CAP_DEPTH_TEST);
	ff_quad (-0.2f, -0.9f, 0.2f, -0.55f, -0.5f, PGPU_RGBA (0, 255, 0, 255));
	pgpu_frame_end (0);

	/* frame 2: colour clear only */
	pgpu_clear (PGPU_CLEAR_COLOR, PGPU_RGBA (0, 0, 0, 255), 1.0f);
	ff_quad (-0.35f, -0.97f, 0.35f, -0.48f, 0.5f, PGPU_RGBA (255, 0, 0, 255));	/* behind */
	pgpu_disable (PGPU_CAP_DEPTH_TEST);

	/* built-in variables */
	pgpu_use_program (PROG_BUILTIN);
	pgpu_attrib_array (BUILTIN_A_POS, BUF_TEST, 0, 0, 2, PGPU_FLOAT);	/* unit square */
	pgpu_attribs_enable (1u << BUILTIN_A_POS);
	pgpu_program_uniform1f (PROG_BUILTIN, builtin_u_point_size, 48.0f);
	builtin_rect (-0.98f, 0.6f, 0.9f, 0.38f, 0);
	pgpu_draw_arrays (PGPU_TRIANGLE_FAN, 0, 4);

	pgpu_viewport (0, 0, WIDTH, HEIGHT, 0.25f, 0.75f);
	builtin_rect (0.0f, 0.6f, 0.3f, 0.38f, 3);
	pgpu_draw_arrays (PGPU_TRIANGLE_FAN, 0, 4);
	pgpu_viewport (0, 0, WIDTH, HEIGHT, 0.0f, 1.0f);

	builtin_rect (0.65f, 0.78f, 0.0f, 0.0f, 1);		/* point at the rect origin */
	pgpu_draw_arrays (PGPU_POINTS, 0, 1);

	builtin_rect (-0.98f, 0.1f, 0.3f, 0.4f, 2);		/* CCW: front */
	pgpu_draw_arrays (PGPU_TRIANGLES, 0, 3);
	builtin_rect (-0.6f, 0.1f, 0.3f, 0.4f, 2);		/* 0, 2, 1: CW, back */
	{
		static const uint8_t cw[4] = {0, 2, 1, 0};	/* BUFFER_DATA: multiples of 4 bytes */
		pgpu_buffer_create (BUF_ROW + 1, sizeof cw);
		pgpu_buffer_data (BUF_ROW + 1, 0, cw, sizeof cw);
		pgpu_draw_elements (PGPU_TRIANGLES, 3, PGPU_INDEX_U8, BUF_ROW + 1, 0);
	}

	/* polygon offset (fixed function) */
	pgpu_use_program (0);
	pgpu_enable (PGPU_CAP_DEPTH_TEST);
	ff_quad (-0.25f, 0.1f, -0.05f, 0.5f, 0.0f, PGPU_RGBA (255, 0, 0, 255));
	ff_quad (-0.25f, 0.1f, -0.05f, 0.5f, 0.0f, PGPU_RGBA (0, 255, 0, 255));
	ff_quad (0.0f, 0.1f, 0.2f, 0.5f, 0.0f, PGPU_RGBA (255, 0, 0, 255));
	pgpu_enable (PGPU_CAP_POLYGON_OFFSET_FILL);
	pgpu_polygon_offset (0.0f, -4.0f);
	ff_quad (0.0f, 0.1f, 0.2f, 0.5f, 0.0f, PGPU_RGBA (0, 255, 0, 255));
	pgpu_disable (PGPU_CAP_POLYGON_OFFSET_FILL | PGPU_CAP_DEPTH_TEST);

	/* scissor: full-screen quads (GL window coordinates, y up) */
	pgpu_enable (PGPU_CAP_SCISSOR_TEST);
	pgpu_scissor (240, 150, 30, 30);
	ff_quad (-1, -1, 1, 1, 0, PGPU_RGBA (0, 0, 255, 255));
	pgpu_scissor (280, 150, 30, 30);
	pgpu_use_program (PROG_SOLID);
	pgpu_attrib_array (SOLID_A_POS, BUF_TEST, 0, 0, 2, PGPU_FLOAT);
	pgpu_attribs_enable (1u << SOLID_A_POS);
	rect (-1, -1, 2, 2, 1, 0, 1, 1);
	pgpu_draw_arrays (PGPU_TRIANGLE_FAN, 0, 4);
	pgpu_disable (PGPU_CAP_SCISSOR_TEST);

	/* line width */
	pgpu_line_width (4.0f);
	rect (0.3f, -0.3f, 0.3f, 0.3f, 0, 1, 1, 1);
	pgpu_draw_arrays (PGPU_LINE_LOOP, 0, 4);
	pgpu_use_program (0);
	pgpu_line_width (6.0f);
	{
		uint32_t words[2 * 4], *p = words;
		put_pc (&p, -0.9f, -0.3f, 0, PGPU_RGBA (255, 255, 0, 255));
		put_pc (&p, -0.4f, -0.05f, 0, PGPU_RGBA (255, 255, 0, 255));
		pgpu_draw_inline (PGPU_LINES, 2, A (POSITION) | A (COLOR), words, 4);
	}
	pgpu_line_width (1.0f);

	/* client-side arrays (program) */
	static const float tri[6] = {-0.95f, -0.95f, -0.6f, -0.95f, -0.775f, -0.5f};
	static const float square[8] = {-0.55f, -0.95f, -0.3f, -0.95f, -0.3f, -0.7f, -0.55f, -0.7f};
	static const uint8_t quad_indices[6] = {0, 1, 2, 0, 2, 3};
	pgpu_use_program (PROG_SOLID);
	pgpu_attribs_enable (0);
	pgpu_client_attrib_pointer (SOLID_A_POS, 2, PGPU_FLOAT, 0, tri);
	rect (0, 0, 1, 1, 1, 0.5f, 0, 1);
	pgpu_draw_arrays_client (PGPU_TRIANGLES, 0, 3);
	pgpu_client_attrib_pointer (SOLID_A_POS, 2, PGPU_FLOAT, 0, square);
	rect (0, 0, 1, 1, 1, 1, 1, 1);
	pgpu_draw_elements_client (PGPU_TRIANGLES, 6, PGPU_INDEX_U8, quad_indices);
	pgpu_client_attrib_pointer (SOLID_A_POS, 2, PGPU_FLOAT, 0, NULL);
	pgpu_use_program (0);

	pgpu_frame_end (0);
	pgpu_begin (PGPU_OP_DEBUG_SCREENSHOT, 0);
	pgpu_end ();
	pgpu_flush ();
	printf ("gpulink: self test 3 frames sent, screenshot requested\n");
	sleep_ms (3000);
}

/* ---- self test 4: blending with programs -----------------------------------
 *
 * Ten cells (2 rows of 5, 60x60 pixels from the top left): destination
 * D = (0.25, 0.5, 0.75), then source S = (0.5, 0.25, 1.0, alpha 0.5) with:
 *   0 SRC_ALPHA, ONE_MINUS_SRC_ALPHA       (0.375, 0.375, 0.875)
 *   1 SUBTRACT ONE, ONE                    (0.25, 0, 0.25)
 *   2 REVERSE_SUBTRACT ONE, ONE            (0, 0.25, 0)
 *   3 CONSTANT_COLOR (0.5, 1, 0.25), ZERO  (0.25, 0.25, 0.25)
 *   4 ONE, ONE_MINUS_CONSTANT_ALPHA (0.25) (0.6875, 0.625, 1)
 *   5 DST_COLOR, ZERO                      (0.125, 0.125, 0.75)
 *   6 no blending, colour mask R and B     (0.5, 0.5, 1)
 *   7 DST_ALPHA, ZERO (panel alpha = 1)    (0.5, 0.25, 1)
 *   8 separate: RGB ONE, ONE; alpha ZERO   (0.75, 0.75, 1)
 *   9 ONE_MINUS_DST_COLOR, ONE_MINUS_SRC_COLOR (0.5, 0.5625, 0.75)
 */
static void self_test_4 (void)
{
	pgpu_viewport (0, 0, WIDTH, HEIGHT, 0.0f, 1.0f);
	pgpu_disable (PGPU_CAP_ALL);
	pgpu_color_mask (true, true, true, true);
	pgpu_clear (PGPU_CLEAR_COLOR | PGPU_CLEAR_DEPTH, PGPU_RGBA (0, 0, 0, 255), 1.0f);
	pgpu_use_program (PROG_SOLID);
	pgpu_attrib_array (SOLID_A_POS, BUF_TEST, 0, 0, 2, PGPU_FLOAT);
	pgpu_attribs_enable (1u << SOLID_A_POS);

	for (int cell = 0; cell < 10; cell++)
	{
		/* cell in clip space: 60 pixels = 0.375 x, 0.5 y */
		float x = -1.0f + (cell % 5) * 0.375f + 0.02f, y = 1.0f - (cell / 5 + 1) * 0.5f + 0.02f;

		pgpu_disable (PGPU_CAP_BLEND);
		pgpu_color_mask (true, true, true, true);
		rect (x, y, 0.33f, 0.44f, 0.25f, 0.5f, 0.75f, 1.0f);
		pgpu_draw_arrays (PGPU_TRIANGLE_FAN, 0, 4);

		pgpu_enable (PGPU_CAP_BLEND);
		pgpu_blend_equation (PGPU_FUNC_ADD, PGPU_FUNC_ADD);
		switch (cell)
		{
		case 0: pgpu_blend_func (PGPU_SRC_ALPHA, PGPU_ONE_MINUS_SRC_ALPHA); break;
		case 1: pgpu_blend_func (PGPU_ONE, PGPU_ONE);
			pgpu_blend_equation (PGPU_FUNC_SUBTRACT, PGPU_FUNC_SUBTRACT); break;
		case 2: pgpu_blend_func (PGPU_ONE, PGPU_ONE);
			pgpu_blend_equation (PGPU_FUNC_REVERSE_SUBTRACT, PGPU_FUNC_REVERSE_SUBTRACT); break;
		case 3: pgpu_blend_color (0.5f, 1.0f, 0.25f, 1.0f);
			pgpu_blend_func (PGPU_CONSTANT_COLOR, PGPU_ZERO); break;
		case 4: pgpu_blend_color (0, 0, 0, 0.25f);
			pgpu_blend_func (PGPU_ONE, PGPU_ONE_MINUS_CONSTANT_ALPHA); break;
		case 5: pgpu_blend_func (PGPU_DST_COLOR, PGPU_ZERO); break;
		case 6: pgpu_disable (PGPU_CAP_BLEND);
			pgpu_color_mask (true, false, true, true); break;
		case 7: pgpu_blend_func (PGPU_DST_ALPHA, PGPU_ZERO); break;
		case 8: pgpu_blend_func_separate (PGPU_ONE, PGPU_ONE, PGPU_ZERO, PGPU_ZERO); break;
		case 9: pgpu_blend_func (PGPU_ONE_MINUS_DST_COLOR, PGPU_ONE_MINUS_SRC_COLOR); break;
		}
		rect (x, y, 0.33f, 0.44f, 0.5f, 0.25f, 1.0f, 0.5f);
		pgpu_draw_arrays (PGPU_TRIANGLE_FAN, 0, 4);
	}
	pgpu_disable (PGPU_CAP_BLEND);
	pgpu_color_mask (true, true, true, true);
	pgpu_blend_func (PGPU_ONE, PGPU_ZERO);
	pgpu_blend_equation (PGPU_FUNC_ADD, PGPU_FUNC_ADD);
	pgpu_use_program (0);

	pgpu_frame_end (0);
	pgpu_begin (PGPU_OP_DEBUG_SCREENSHOT, 0);
	pgpu_end ();
	pgpu_flush ();
	printf ("gpulink: self test 4 frame sent, screenshot requested\n");
	sleep_ms (3000);
}

/* ---- self test 5: stencil, attribute format conversion -------------------------
 *
 * Expected (identity matrices, clip space):
 *   top band:   green (program) inside the middle square, red (fixed function)
 *               outside it: stencil 1 was written there with the colour mask off
 *   2nd row:    two overlapping triangles, culling off: the front one writes
 *               stencil 2, the back one 3; shown as blue (2) and yellow (3)
 *               rectangles; a stencil write mask of 0 changes nothing (no white)
 *   bottom:     three cubes that must look the same: float arrays (left),
 *               short positions + float normals + ubyte_norm texcoords (middle),
 *               and the same through client-side arrays (right)
 */
static void self_test_5 (void)
{
	float identity[16];
	mat_identity (identity);

	pgpu_viewport (0, 0, WIDTH, HEIGHT, 0.0f, 1.0f);
	pgpu_use_program (0);
	pgpu_disable (PGPU_CAP_ALL);
	pgpu_arrays_enable (0);
	pgpu_attribs_enable (0);
	pgpu_color_mask (true, true, true, true);
	pgpu_depth_mask (true);
	pgpu_load_matrix (PGPU_PROJECTION, identity);
	pgpu_load_matrix (PGPU_MODELVIEW, identity);
	pgpu_load_matrix (PGPU_TEXTURE, identity);
	pgpu_clear_stencil (PGPU_CLEAR_COLOR | PGPU_CLEAR_DEPTH | PGPU_CLEAR_STENCIL,
			    PGPU_RGBA (0, 0, 0, 255), 1.0f, 0);

	/* stencil 1 in the middle square, colour mask off */
	pgpu_enable (PGPU_CAP_STENCIL_TEST);
	pgpu_stencil_func (PGPU_FRONT_AND_BACK, PGPU_ALWAYS, 1, 0xFF);
	pgpu_stencil_op (PGPU_FRONT_AND_BACK, PGPU_KEEP, PGPU_KEEP, PGPU_REPLACE_OP);
	pgpu_color_mask (false, false, false, false);
	ff_quad (-0.3f, 0.5f, 0.3f, 0.95f, 0, PGPU_RGBA (255, 255, 255, 255));
	pgpu_color_mask (true, true, true, true);

	/* green where 1 (program), red elsewhere in the band (fixed function) */
	pgpu_stencil_op (PGPU_FRONT_AND_BACK, PGPU_KEEP, PGPU_KEEP, PGPU_KEEP);
	pgpu_stencil_func (PGPU_FRONT_AND_BACK, PGPU_EQUAL, 1, 0xFF);
	pgpu_use_program (PROG_SOLID);
	pgpu_attrib_array (SOLID_A_POS, BUF_TEST, 0, 0, 2, PGPU_FLOAT);
	pgpu_attribs_enable (1u << SOLID_A_POS);
	rect (-0.95f, 0.55f, 1.9f, 0.35f, 0, 1, 0, 1);
	pgpu_draw_arrays (PGPU_TRIANGLE_FAN, 0, 4);
	pgpu_use_program (0);
	pgpu_stencil_func (PGPU_FRONT_AND_BACK, PGPU_NOTEQUAL, 1, 0xFF);
	ff_quad (-0.95f, 0.55f, 0.95f, 0.9f, 0, PGPU_RGBA (255, 0, 0, 255));

	/* two-sided: front face writes 2, back face 3 */
	pgpu_stencil_func (PGPU_FRONT_AND_BACK, PGPU_ALWAYS, 0, 0xFF);
	pgpu_stencil_func (PGPU_FRONT, PGPU_ALWAYS, 2, 0xFF);
	pgpu_stencil_func (PGPU_BACK, PGPU_ALWAYS, 3, 0xFF);
	pgpu_stencil_op (PGPU_FRONT_AND_BACK, PGPU_KEEP, PGPU_KEEP, PGPU_REPLACE_OP);
	pgpu_color_mask (false, false, false, false);
	{
		uint32_t words[6 * 4], *p = words, c = PGPU_RGBA (255, 255, 255, 255);
		put_pc (&p, -0.9f, -0.2f, 0, c);	/* CCW: front */
		put_pc (&p, -0.3f, -0.2f, 0, c);
		put_pc (&p, -0.3f, 0.4f, 0, c);
		put_pc (&p, 0.3f, -0.2f, 0, c);		/* CW: back */
		put_pc (&p, 0.3f, 0.4f, 0, c);
		put_pc (&p, 0.9f, -0.2f, 0, c);
		pgpu_draw_inline (PGPU_TRIANGLES, 6, A (POSITION) | A (COLOR), words, 4);
	}
	/* write mask 0: this REPLACE with 7 must not change anything */
	pgpu_stencil_mask (PGPU_FRONT_AND_BACK, 0);
	pgpu_stencil_func (PGPU_FRONT_AND_BACK, PGPU_ALWAYS, 7, 0xFF);
	ff_quad (-0.95f, -0.25f, 0.95f, 0.45f, 0, PGPU_RGBA (255, 255, 255, 255));
	pgpu_stencil_mask (PGPU_FRONT_AND_BACK, 0xFF);
	pgpu_color_mask (true, true, true, true);
	pgpu_stencil_op (PGPU_FRONT_AND_BACK, PGPU_KEEP, PGPU_KEEP, PGPU_KEEP);
	pgpu_stencil_func (PGPU_FRONT_AND_BACK, PGPU_EQUAL, 2, 0xFF);
	ff_quad (-0.95f, -0.25f, 0.95f, 0.45f, 0, PGPU_RGBA (0, 0, 255, 255));
	pgpu_stencil_func (PGPU_FRONT_AND_BACK, PGPU_EQUAL, 3, 0xFF);
	ff_quad (-0.95f, -0.25f, 0.95f, 0.45f, 0, PGPU_RGBA (255, 255, 0, 255));
	pgpu_stencil_func (PGPU_FRONT_AND_BACK, PGPU_EQUAL, 7, 0xFF);
	ff_quad (-0.95f, -0.25f, 0.95f, 0.45f, 0, PGPU_RGBA (255, 255, 255, 255));
	pgpu_disable (PGPU_CAP_STENCIL_TEST);

	/* the cube three times, in other array formats (converted by the Zero) */
	static int16_t pos_s[24][3];
	static float normal_f[24][3];
	static uint8_t uv_b[24][2];
	static struct cube_vertex cv[24];		/* the float cube, as uploaded */
	static bool once;
	if (!once)
	{
		/* rebuild the cube's vertices (as upload_cube ()) */
		static const struct { int n[3], u[3], v[3]; } faces[6] =
		{
			{{ 1, 0, 0}, {0, 0,-1}, {0, 1, 0}}, {{-1, 0, 0}, {0, 0, 1}, {0, 1, 0}},
			{{ 0, 1, 0}, {1, 0, 0}, {0, 0,-1}}, {{ 0,-1, 0}, {1, 0, 0}, {0, 0, 1}},
			{{ 0, 0, 1}, {1, 0, 0}, {0, 1, 0}}, {{ 0, 0,-1}, {-1, 0, 0}, {0, 1, 0}},
		};
		static const float corners[4][2] = {{-1,-1}, {1,-1}, {1,1}, {-1,1}};
		for (int f = 0; f < 6; f++)
			for (int i = 0; i < 4; i++)
			{
				int v = f * 4 + i;
				for (int k = 0; k < 3; k++)
				{
					int c = faces[f].n[k] + (int) corners[i][0] * faces[f].u[k]
						+ (int) corners[i][1] * faces[f].v[k];	/* -1 or 1 */
					pos_s[v][k] = (int16_t) c;
					cv[v].pos[k] = 0.5f * c;
					normal_f[v][k] = (float) faces[f].n[k];
				}
				uv_b[v][0] = corners[i][0] > 0 ? 255 : 0;
				uv_b[v][1] = corners[i][1] > 0 ? 255 : 0;
			}
		pgpu_buffer_create (BUF_ROW + 2, sizeof pos_s);
		pgpu_buffer_data (BUF_ROW + 2, 0, pos_s, sizeof pos_s);
		pgpu_buffer_create (BUF_ROW + 3, sizeof normal_f);
		pgpu_buffer_data (BUF_ROW + 3, 0, normal_f, sizeof normal_f);
		pgpu_buffer_create (BUF_ROW + 4, sizeof uv_b);
		pgpu_buffer_data (BUF_ROW + 4, 0, uv_b, sizeof uv_b);
		once = true;
	}

	float proj[16], view[16], vp[16], model[16], a[16], mvp[16], scaled[16];
	mat_perspective (proj, 1.0f, (float) WIDTH / HEIGHT, 0.5f, 20.0f);
	mat_rotate_y (model, 0.6f);
	mat_rotate_x (a, 0.4f);
	mat_multiply (model, model, a);
	float light[3] = {0, 0, 5}, white[3] = {1, 1, 1}, eye[3] = {0, 0, 5};
	pgpu_enable (PGPU_CAP_DEPTH_TEST | PGPU_CAP_CULL_FACE);
	pgpu_use_program (PROG_CUBE);
	PGPU_UNIFORM (PROG_CUBE, cube_u_light_pos, light);
	PGPU_UNIFORM (PROG_CUBE, cube_u_light_color, white);
	PGPU_UNIFORM (PROG_CUBE, cube_u_eye, eye);
	pgpu_texture_bind_unit (0, TEX_CHECKER);
	for (int i = 0; i < 3; i++)
	{
		mat_translate (view, -1.9f + 1.9f * i, -1.1f, -4.5f);
		mat_multiply (vp, proj, view);
		mat_multiply (mvp, vp, model);
		if (i > 0)
		{
			mat_scale (a, 0.5f);		/* short positions are +-1 */
			mat_multiply (scaled, mvp, a);
			PGPU_UNIFORM (PROG_CUBE, cube_u_mvp, scaled);
			mat_multiply (scaled, model, a);
			PGPU_UNIFORM (PROG_CUBE, cube_u_model, scaled);
		}
		else
		{
			PGPU_UNIFORM (PROG_CUBE, cube_u_mvp, mvp);
			PGPU_UNIFORM (PROG_CUBE, cube_u_model, model);
		}

		if (i == 0)
		{
			pgpu_attrib_array (CUBE_A_POS, BUF_CUBE, 0, sizeof (struct cube_vertex), 3, PGPU_FLOAT);
			pgpu_attrib_array (CUBE_A_NORMAL, BUF_CUBE, 12, sizeof (struct cube_vertex), 3, PGPU_BYTE_NORM);
			pgpu_attrib_array (CUBE_A_UV, BUF_CUBE, 16, sizeof (struct cube_vertex), 2, PGPU_FLOAT);
			pgpu_attribs_enable (1u << CUBE_A_POS | 1u << CUBE_A_NORMAL | 1u << CUBE_A_UV);
			pgpu_draw_elements (PGPU_TRIANGLES, 36, PGPU_INDEX_U8, BUF_CUBE_INDEX, 0);
		}
		else if (i == 1)
		{
			pgpu_attrib_array (CUBE_A_POS, BUF_ROW + 2, 0, 0, 3, PGPU_SHORT);
			pgpu_attrib_array (CUBE_A_NORMAL, BUF_ROW + 3, 0, 0, 3, PGPU_FLOAT);
			pgpu_attrib_array (CUBE_A_UV, BUF_ROW + 4, 0, 0, 2, PGPU_UBYTE_NORM);
			pgpu_attribs_enable (1u << CUBE_A_POS | 1u << CUBE_A_NORMAL | 1u << CUBE_A_UV);
			pgpu_draw_elements (PGPU_TRIANGLES, 36, PGPU_INDEX_U8, BUF_CUBE_INDEX, 0);
		}
		else
		{
			static uint8_t index[36];
			static const uint8_t quad[6] = {0, 1, 2, 0, 2, 3};
			for (int k = 0; k < 36; k++)
				index[k] = (k / 6) * 4 + quad[k % 6];
			pgpu_attribs_enable (0);
			pgpu_client_attrib_pointer (CUBE_A_POS, 3, PGPU_SHORT, 0, pos_s);
			pgpu_client_attrib_pointer (CUBE_A_NORMAL, 3, PGPU_FLOAT, 0, normal_f);
			pgpu_client_attrib_pointer (CUBE_A_UV, 2, PGPU_UBYTE_NORM, 0, uv_b);
			pgpu_draw_elements_client (PGPU_TRIANGLES, 36, PGPU_INDEX_U8, index);
			for (int k = 0; k < 3; k++)
				pgpu_client_attrib_pointer (k, 0, 0, 0, NULL);
		}
	}
	pgpu_use_program (0);
	pgpu_disable (PGPU_CAP_ALL);

	pgpu_frame_end (0);
	pgpu_begin (PGPU_OP_DEBUG_SCREENSHOT, 0);
	pgpu_end ();
	pgpu_flush ();
	printf ("gpulink: self test 5 frame sent, screenshot requested\n");
	sleep_ms (3000);
}

/* ---- self test 6: textures ------------------------------------------------------
 *
 * Expected (texview program; cells from the top left):
 *   row 1: a 64x64 texture with a solid colour per mip level (0 red, 1 green,
 *          2 blue, 3 yellow), NEAREST_MIPMAP_NEAREST, drawn 48, 24, 12 and 6
 *          pixels wide: red, green, blue, yellow
 *   row 2: a 1-pixel black and white checkerboard with GENERATE_MIPMAP, drawn
 *          48 and 12 pixels wide: the small one mid grey; a 100x60 texture
 *          (horizontal red ramp): clamped and linear it shows, with REPEAT it
 *          is incomplete: black
 *   row 3: cube map faces +X red, -X green, +Y blue, -Y yellow, +Z magenta,
 *          -Z cyan, sampled in those directions
 *   row 4: RGB888 quadrants (bottom red, green; top blue, white); an A8
 *          texture (alpha 128): RGB black, alpha mid grey; the fixed-function
 *          star sprite (A8, MODULATE) still yellow
 */
enum { TEX_MIPLEVELS = 40, TEX_GENERATED, TEX_NPOT, TEX_SKY, TEX_RGB, TEX_ALPHA };

static void texview (float x, float y, float w, float h, float mode)
{
	float rc[4] = {x, y, w, h};
	PGPU_UNIFORM (PROG_TEXVIEW, texview_u_rect, rc);
	pgpu_program_uniform1f (PROG_TEXVIEW, texview_u_mode, mode);
	pgpu_draw_arrays (PGPU_TRIANGLE_FAN, 0, 4);
}

static void self_test_6 (void)
{
	static uint32_t pixels[64 * 64];
	static const uint32_t level_colors[7] =
	{
		PGPU_RGBA (255, 0, 0, 255), PGPU_RGBA (0, 255, 0, 255), PGPU_RGBA (0, 0, 255, 255),
		PGPU_RGBA (255, 255, 0, 255), PGPU_RGBA (255, 0, 255, 255), PGPU_RGBA (0, 255, 255, 255),
		PGPU_RGBA (255, 255, 255, 255),
	};

	/* per-level colours */
	pgpu_texture_create (TEX_MIPLEVELS, 64, 64, PGPU_RGBA8888);
	for (int level = 0, size = 64; size >= 1; level++, size /= 2)
	{
		for (int i = 0; i < size * size; i++)
			pixels[i] = level_colors[level];
		pgpu_texture_data_level (TEX_MIPLEVELS, level, 0, 0, 0, size, size, PGPU_RGBA8888, pixels);
	}
	pgpu_texture_params (TEX_MIPLEVELS, PGPU_NEAREST_MIPMAP_NEAREST, PGPU_NEAREST,
			     PGPU_CLAMP_TO_EDGE, PGPU_CLAMP_TO_EDGE);

	/* checkerboard, mipmaps generated */
	for (int y = 0; y < 64; y++)
		for (int x = 0; x < 64; x++)
			pixels[y * 64 + x] = (x ^ y) & 1 ? PGPU_RGBA (255, 255, 255, 255) : PGPU_RGBA (0, 0, 0, 255);
	pgpu_texture_create (TEX_GENERATED, 64, 64, PGPU_RGBA8888);
	pgpu_texture_data (TEX_GENERATED, 0, 0, 64, 64, PGPU_RGBA8888, pixels);
	pgpu_generate_mipmap (TEX_GENERATED);
	pgpu_texture_params (TEX_GENERATED, PGPU_LINEAR_MIPMAP_LINEAR, PGPU_NEAREST,
			     PGPU_REPEAT, PGPU_REPEAT);

	/* non-power-of-two: 100x60, red ramp */
	static uint8_t npot[100 * 60 * 3];
	for (int y = 0; y < 60; y++)
		for (int x = 0; x < 100; x++)
		{
			uint8_t *p = &npot[(y * 100 + x) * 3];
			p[0] = (uint8_t) (x * 255 / 99);
			p[1] = 0;
			p[2] = 0;
		}
	pgpu_texture_create (TEX_NPOT, 100, 60, PGPU_RGB888);
	pgpu_texture_data (TEX_NPOT, 0, 0, 100, 60, PGPU_RGB888, npot);

	/* cube map */
	static const uint32_t face_colors[6] =
	{
		PGPU_RGBA (255, 0, 0, 255), PGPU_RGBA (0, 255, 0, 255), PGPU_RGBA (0, 0, 255, 255),
		PGPU_RGBA (255, 255, 0, 255), PGPU_RGBA (255, 0, 255, 255), PGPU_RGBA (0, 255, 255, 255),
	};
	pgpu_texture_create_cube (TEX_SKY, 16, PGPU_RGBA8888);
	for (int f = 0; f < 6; f++)
	{
		for (int i = 0; i < 16 * 16; i++)
			pixels[i] = face_colors[f];
		pgpu_texture_data_level (TEX_SKY, 0, f, 0, 0, 16, 16, PGPU_RGBA8888, pixels);
	}
	pgpu_texture_params (TEX_SKY, PGPU_NEAREST, PGPU_NEAREST, PGPU_CLAMP_TO_EDGE, PGPU_CLAMP_TO_EDGE);

	/* RGB888 2x2 quadrants, rows padded to 4 bytes */
	static const uint8_t rgb[16] = {255, 0, 0, 0, 255, 0, 0, 0,   0, 0, 255, 255, 255, 255, 0, 0};
	pgpu_texture_create (TEX_RGB, 2, 2, PGPU_RGB888);
	pgpu_texture_data (TEX_RGB, 0, 0, 2, 2, PGPU_RGB888, rgb);
	pgpu_texture_params (TEX_RGB, PGPU_NEAREST, PGPU_NEAREST, PGPU_CLAMP_TO_EDGE, PGPU_CLAMP_TO_EDGE);

	/* A8, alpha 128 */
	static uint8_t alpha[16];
	memset (alpha, 128, sizeof alpha);
	pgpu_texture_create (TEX_ALPHA, 4, 4, PGPU_A8);
	pgpu_texture_data (TEX_ALPHA, 0, 0, 4, 4, PGPU_A8, alpha);
	pgpu_texture_params (TEX_ALPHA, PGPU_NEAREST, PGPU_NEAREST, PGPU_CLAMP_TO_EDGE, PGPU_CLAMP_TO_EDGE);

	pgpu_viewport (0, 0, WIDTH, HEIGHT, 0.0f, 1.0f);
	pgpu_use_program (0);
	pgpu_disable (PGPU_CAP_ALL);
	pgpu_color_mask (true, true, true, true);
	pgpu_clear (PGPU_CLEAR_COLOR | PGPU_CLEAR_DEPTH, PGPU_RGBA (0, 0, 0, 255), 1.0f);

	pgpu_use_program (PROG_TEXVIEW);
	pgpu_attrib_array (TEXVIEW_A_POS, BUF_TEST, 0, 0, 2, PGPU_FLOAT);
	pgpu_attribs_enable (1u << TEXVIEW_A_POS);
	pgpu_program_sampler (PROG_TEXVIEW, TEXVIEW_U_TEX, 0);
	pgpu_program_sampler (PROG_TEXVIEW, TEXVIEW_U_CUBE, 1);

	/* pixel to clip space helpers: x px from the left, y px from the top */
	#define PX(x)	(-1.0f + (x) / 160.0f)
	#define PY(y)	(1.0f - (y) / 120.0f)
	#define PW(w)	((w) / 160.0f)
	#define PH(h)	((h) / 120.0f)

	/* row 1: mip levels */
	pgpu_texture_bind_unit (0, TEX_MIPLEVELS);
	static const int sizes[4] = {48, 24, 12, 6};
	for (int i = 0; i < 4; i++)
		texview (PX (8 + i * 60), PY (8 + sizes[i]), PW (sizes[i]), PH (sizes[i]), 0);

	/* row 2: generated mipmaps, NPOT clamped and repeated */
	pgpu_texture_bind_unit (0, TEX_GENERATED);
	texview (PX (8), PY (68 + 48), PW (48), PH (48), 0);
	texview (PX (68), PY (68 + 12), PW (12), PH (12), 0);
	pgpu_texture_bind_unit (0, TEX_NPOT);
	pgpu_texture_params (TEX_NPOT, PGPU_LINEAR, PGPU_LINEAR, PGPU_CLAMP_TO_EDGE, PGPU_CLAMP_TO_EDGE);
	texview (PX (128), PY (68 + 48), PW (80), PH (48), 0);
	pgpu_texture_params (TEX_NPOT, PGPU_LINEAR, PGPU_LINEAR, PGPU_REPEAT, PGPU_REPEAT);	/* per draw */
	texview (PX (224), PY (68 + 48), PW (80), PH (48), 0);

	/* row 3: cube map faces */
	pgpu_texture_bind_unit (1, TEX_SKY);
	static const float dirs[6][3] = {{1, 0.1f, 0.2f}, {-1, 0.1f, 0.2f}, {0.1f, 1, 0.2f},
					  {0.1f, -1, 0.2f}, {0.1f, 0.2f, 1}, {0.1f, 0.2f, -1}};
	for (int f = 0; f < 6; f++)
	{
		PGPU_UNIFORM (PROG_TEXVIEW, texview_u_dir, dirs[f]);
		texview (PX (8 + f * 52), PY (128 + 40), PW (40), PH (40), 3);
	}

	/* row 4: RGB888, A8 (RGB, alpha) */
	pgpu_texture_bind_unit (0, TEX_RGB);
	texview (PX (8), PY (184 + 48), PW (48), PH (48), 0);
	pgpu_texture_bind_unit (0, TEX_ALPHA);
	texview (PX (68), PY (184 + 48), PW (48), PH (48), 1);
	texview (PX (128), PY (184 + 48), PW (48), PH (48), 2);

	/* the fixed-function star sprite: A8 texture, MODULATE, yellow */
	float ortho[16], identity[16];
	mat_ortho (ortho, 0, WIDTH, 0, HEIGHT, -1, 1);
	mat_identity (identity);
	pgpu_use_program (0);
	pgpu_load_matrix (PGPU_PROJECTION, ortho);
	pgpu_load_matrix (PGPU_MODELVIEW, identity);
	pgpu_load_matrix (PGPU_TEXTURE, identity);
	pgpu_enable (PGPU_CAP_TEXTURE_2D | PGPU_CAP_ALPHA_TEST);
	pgpu_alpha_func (PGPU_GREATER, 0.5f);
	pgpu_texture_bind (TEX_SPRITE);
	pgpu_tex_env (PGPU_MODULATE, 0);
	pgpu_color (PGPU_RGBA (255, 216, 50, 255));
	quad_2d (190, 8, 48, 48);
	pgpu_color (PGPU_RGBA (255, 255, 255, 255));
	pgpu_disable (PGPU_CAP_ALL);

	pgpu_frame_end (0);
	pgpu_begin (PGPU_OP_DEBUG_SCREENSHOT, 0);
	pgpu_end ();
	pgpu_flush ();
	printf ("gpulink: self test 6 frame sent, screenshot requested\n");
	sleep_ms (3000);
}

/* ---- self test 7: render to texture, CLEAR anywhere, read-back, copy ----------
 *
 * Expected:
 *   left:       a 128x128 texture rendered through a framebuffer (with depth):
 *               blue background, the lit textured cube (program) and a yellow
 *               triangle (fixed function) at its bottom left; shown upright
 *   top right:  green square with a smaller red one inside: red drawn first,
 *               then a scissored CLEAR (green) after the draw, then red again
 *               in the middle, with depth test LESS against a depth-only CLEAR
 *               to 1.0 (a green rim, red inside)
 *   middle right: the texture's centre pixel and the panel's pixel at (300,
 *               150), read back with READ_PIXELS and drawn as squares: cube
 *               colour, and green
 *   bottom right: the top left 48x48 pixels of the panel copied into a
 *               texture (COPY_TEX_IMAGE) and drawn
 */
enum { TEX_TARGET = 50, TEX_COPY };
#define FB_TEST 1

static void self_test_7 (void)
{
	float identity[16];
	mat_identity (identity);

	pgpu_texture_create (TEX_TARGET, 128, 128, PGPU_RGBA8888);
	pgpu_texture_params (TEX_TARGET, PGPU_LINEAR, PGPU_LINEAR, PGPU_CLAMP_TO_EDGE, PGPU_CLAMP_TO_EDGE);
	pgpu_framebuffer_create (FB_TEST, TEX_TARGET, 0, PGPU_FRAMEBUFFER_DEPTH_STENCIL);

	/* the panel: black */
	pgpu_use_program (0);
	pgpu_disable (PGPU_CAP_ALL);
	pgpu_color_mask (true, true, true, true);
	pgpu_depth_mask (true);
	pgpu_viewport (0, 0, WIDTH, HEIGHT, 0.0f, 1.0f);
	pgpu_clear (PGPU_CLEAR_COLOR | PGPU_CLEAR_DEPTH, PGPU_RGBA (0, 0, 0, 255), 1.0f);

	/* render into the texture */
	pgpu_bind_framebuffer (FB_TEST);
	pgpu_viewport (0, 0, 128, 128, 0.0f, 1.0f);
	pgpu_clear (PGPU_CLEAR_COLOR | PGPU_CLEAR_DEPTH, PGPU_RGBA (40, 60, 160, 255), 1.0f);
	{
		float proj[16], view[16], vp[16], model[16], a[16], mvp[16];
		mat_perspective (proj, 1.0f, 1.0f, 0.5f, 20.0f);
		mat_translate (view, 0.0f, 0.0f, -3.0f);
		mat_multiply (vp, proj, view);
		mat_rotate_y (model, 0.6f);
		mat_rotate_x (a, 0.5f);
		mat_multiply (model, model, a);
		mat_multiply (mvp, vp, model);
		float light[3] = {0, 2, 5}, white[3] = {1, 1, 1}, eye[3] = {0, 0, 3};
		pgpu_enable (PGPU_CAP_DEPTH_TEST | PGPU_CAP_CULL_FACE);
		pgpu_use_program (PROG_CUBE);
		PGPU_UNIFORM (PROG_CUBE, cube_u_mvp, mvp);
		PGPU_UNIFORM (PROG_CUBE, cube_u_model, model);
		PGPU_UNIFORM (PROG_CUBE, cube_u_light_pos, light);
		PGPU_UNIFORM (PROG_CUBE, cube_u_light_color, white);
		PGPU_UNIFORM (PROG_CUBE, cube_u_eye, eye);
		pgpu_texture_bind_unit (0, TEX_CHECKER);
		pgpu_attrib_array (CUBE_A_POS, BUF_CUBE, 0, sizeof (struct cube_vertex), 3, PGPU_FLOAT);
		pgpu_attrib_array (CUBE_A_NORMAL, BUF_CUBE, 12, sizeof (struct cube_vertex), 3, PGPU_BYTE_NORM);
		pgpu_attrib_array (CUBE_A_UV, BUF_CUBE, 16, sizeof (struct cube_vertex), 2, PGPU_FLOAT);
		pgpu_attribs_enable (1u << CUBE_A_POS | 1u << CUBE_A_NORMAL | 1u << CUBE_A_UV);
		pgpu_draw_elements (PGPU_TRIANGLES, 36, PGPU_INDEX_U8, BUF_CUBE_INDEX, 0);
		pgpu_use_program (0);
		pgpu_disable (PGPU_CAP_ALL);
		pgpu_load_matrix (PGPU_PROJECTION, identity);
		pgpu_load_matrix (PGPU_MODELVIEW, identity);
		pgpu_load_matrix (PGPU_TEXTURE, identity);
		uint32_t words[3 * 4], *p = words, y = PGPU_RGBA (255, 230, 0, 255);
		put_pc (&p, -0.95f, -0.95f, 0, y);
		put_pc (&p, -0.45f, -0.95f, 0, y);
		put_pc (&p, -0.95f, -0.45f, 0, y);
		pgpu_draw_inline (PGPU_TRIANGLES, 3, A (POSITION) | A (COLOR), words, 4);
	}
	uint32_t centre = 0;
	bool ok_centre = pgpu_read_pixels (64, 64, 1, 1, &centre, 200);

	/* back to the panel: show the texture */
	pgpu_bind_framebuffer (0);
	pgpu_viewport (0, 0, WIDTH, HEIGHT, 0.0f, 1.0f);
	float ortho[16];
	mat_ortho (ortho, 0, WIDTH, 0, HEIGHT, -1, 1);
	pgpu_load_matrix (PGPU_PROJECTION, ortho);
	pgpu_load_matrix (PGPU_MODELVIEW, identity);
	pgpu_enable (PGPU_CAP_TEXTURE_2D);
	pgpu_texture_bind (TEX_TARGET);
	pgpu_tex_env (PGPU_REPLACE, 0);
	quad_2d (8, 56, 128, 128);
	pgpu_disable (PGPU_CAP_TEXTURE_2D);

	/* CLEAR after draws: scissored colour, then depth only */
	uint32_t words[4 * 4], *p;
	#define RECT(x0, y0, x1, y1, z, c) \
		(p = words, put_pc (&p, x0, y0, z, c), put_pc (&p, x1, y0, z, c), \
		 put_pc (&p, x1, y1, z, c), put_pc (&p, x0, y1, z, c), \
		 pgpu_draw_inline (PGPU_TRIANGLE_FAN, 4, A (POSITION) | A (COLOR), words, 4))
	RECT (240, 150, 310, 220, 0, PGPU_RGBA (255, 0, 0, 255));
	pgpu_enable (PGPU_CAP_SCISSOR_TEST);
	pgpu_scissor (240, 150, 70, 70);
	pgpu_clear (PGPU_CLEAR_COLOR, PGPU_RGBA (0, 200, 0, 255), 1.0f);
	pgpu_disable (PGPU_CAP_SCISSOR_TEST);
	pgpu_clear (PGPU_CLEAR_DEPTH, 0, 0.3f);			/* depth 0.3 everywhere ... */
	pgpu_enable (PGPU_CAP_SCISSOR_TEST);
	pgpu_scissor (255, 165, 40, 40);
	pgpu_clear (PGPU_CLEAR_DEPTH, 0, 1.0f);			/* ... 1.0 in the middle */
	pgpu_disable (PGPU_CAP_SCISSOR_TEST);
	pgpu_enable (PGPU_CAP_DEPTH_TEST);
	pgpu_depth_func (PGPU_LESS);
	RECT (240, 150, 310, 220, 0, PGPU_RGBA (255, 0, 0, 255));	/* z 0 -> 0.5: red only in the middle */
	pgpu_disable (PGPU_CAP_DEPTH_TEST);

	/* read-back: the texture's centre (read above), the panel at (300, 150) */
	uint32_t panel = 0;
	bool ok_panel = pgpu_read_pixels (300, 150, 1, 1, &panel, 200);
	printf ("gpulink: READ_PIXELS texture centre %s %08lx, panel (300, 150) %s %08lx\n",
		ok_centre ? "ok" : "TIMEOUT", (unsigned long) centre,
		ok_panel ? "ok" : "TIMEOUT", (unsigned long) panel);
	RECT (240, 90, 270, 120, 0, centre);
	RECT (280, 90, 310, 120, 0, panel);

	/* copy the panel's top left 48x48 (the texture's top left corner) */
	pgpu_texture_create (TEX_COPY, 64, 64, PGPU_RGBA8888);
	pgpu_texture_params (TEX_COPY, PGPU_NEAREST, PGPU_NEAREST, PGPU_CLAMP_TO_EDGE, PGPU_CLAMP_TO_EDGE);
	pgpu_copy_tex_image (TEX_COPY, 0, 0, 0, 0, 8, 136, 48, 48);
	pgpu_enable (PGPU_CAP_TEXTURE_2D);
	pgpu_texture_bind (TEX_COPY);
	{
		uint32_t w5[4 * 5], *q = w5;
		put_pt (&q, 250, 10, 0, 0);
		put_pt (&q, 298, 10, 0.75f, 0);
		put_pt (&q, 298, 58, 0.75f, 0.75f);
		put_pt (&q, 250, 58, 0, 0.75f);
		pgpu_draw_inline (PGPU_TRIANGLE_FAN, 4, A (POSITION) | A (TEXCOORD), w5, 5);
	}
	pgpu_disable (PGPU_CAP_TEXTURE_2D);

	pgpu_frame_end (0);
	pgpu_begin (PGPU_OP_DEBUG_SCREENSHOT, 0);
	pgpu_end ();
	pgpu_flush ();
	printf ("gpulink: self test 7 frame sent, screenshot requested\n");
	sleep_ms (3000);
}

/* ---- replies ----------------------------------------------------------------- */

static unsigned choose_reply_phase (void)
{
	unsigned ok[2] = {0, 0};
	int32_t rtt[2] = {0, 0};

	for (unsigned phase = 0; phase < 2; phase++)
	{
		pgpu_set_reply_phase (phase);
		sleep_ms (5);
		pgpu_reply_t r;
		while (pgpu_poll_reply (&r))
		{
		}
		pgpu_get_stats ();

		for (uint32_t i = 0; i < 100; i++)
		{
			int32_t us = pgpu_ping_wait (0xC0DE0000u + i, 20);
			if (us >= 0)
			{
				ok[phase]++;
				rtt[phase] = us;
			}
		}
		pgpu_stats_t s = pgpu_get_stats ();
		printf ("gpulink: reply phase %u: %u/100 pings answered (last %ld us), "
			"%lu replies, %lu CRC errors, %lu overruns\n",
			phase, ok[phase], (long) rtt[phase], (unsigned long) s.replies,
			(unsigned long) s.reply_crc_errors, (unsigned long) s.reply_overruns);
	}

	unsigned phase = ok[1] >= ok[0] ? 1 : 0;
	pgpu_set_reply_phase (phase);
	printf ("gpulink: using reply phase %u\n", phase);
	return phase;
}

int main (void)
{
	stdio_init_all ();
	pgpu_init ();

	printf ("\ngpulink: waiting for the Zero (READY)...\n");
	while (!pgpu_wait_ready (1000))
	{
		printf ("gpulink: READY is low - Zero not running yet\n");
	}
	printf ("gpulink: READY, starting\n");

	choose_reply_phase ();
	setup ();
	self_test ();
	program_self_test ();
	self_test_3 ();
	self_test_4 ();
	self_test_5 ();
	self_test_6 ();
	self_test_7 ();

	uint32_t frames = 0, timeouts = 0, frame_done = 0, last_frame_number = 0, render_us = 0;
	absolute_time_t next_report = make_timeout_time_ms (1000);
	absolute_time_t start = get_absolute_time ();

	while (true)
	{
		float t = absolute_time_diff_us (start, get_absolute_time ()) / 1e6f;

		/* 10 s fixed-function scene, 10 s program scene */
		if ((int) (t / 10.0f) & 1)
		{
			draw_program_scene (t);
		}
		else
		{
			draw_scene (t);
		}
		pgpu_frame_end (PGPU_FRAME_REPLY);
		frames++;

		/* pace on the Zero's FRAME pulse */
		if (!pgpu_wait_frame (100))
		{
			timeouts++;
		}

		/* 's' on the console: the Zero dumps the last frame to its USB log
		   (devtools/screenshot.py) */
		if (getchar_timeout_us (0) == 's')
		{
			pgpu_begin (PGPU_OP_DEBUG_SCREENSHOT, 0);
			pgpu_end ();
			pgpu_flush ();
			printf ("gpulink: screenshot requested\n");
		}

		pgpu_reply_t r;
		while (pgpu_poll_reply (&r))
		{
			if (r.opcode == PGPU_REPLY_INFO)
			{
				/* unsolicited INFO: the Zero has (re)booted, objects are gone */
				printf ("gpulink: the Zero rebooted, setting up again\n");
				while (!pgpu_wait_ready (1000))
				{
				}
				setup ();
			}
			else if (r.opcode == PGPU_REPLY_FRAME_DONE)
			{
				frame_done++;
				last_frame_number = r.payload[0];
				render_us = r.payload[1];
			}
		}

		if (time_reached (next_report))
		{
			int32_t rtt = pgpu_ping_wait (0x1234, 50);
			pgpu_status_t st;
			bool have_status = pgpu_get_status (&st, 50);
			pgpu_stats_t s = pgpu_get_stats ();

			printf ("gpulink: %lu fps sent, %lu KB/s, READY wait %lu us, frame timeouts %lu | "
				"FRAME_DONE %lu (#%lu, render %lu us), ping %ld us | "
				"replies %lu, CRC err %lu, overruns %lu, lost %lu, Zero errors %lu (last %lu/%02lx/%lu)\n",
				(unsigned long) frames, (unsigned long) (s.words * 4 / 1024),
				(unsigned long) s.ready_wait_us, (unsigned long) timeouts,
				(unsigned long) frame_done, (unsigned long) last_frame_number,
				(unsigned long) render_us, (long) rtt,
				(unsigned long) s.replies, (unsigned long) s.reply_crc_errors,
				(unsigned long) s.reply_overruns, (unsigned long) s.replies_lost,
				(unsigned long) s.zero_errors, (unsigned long) s.last_error[0],
				(unsigned long) s.last_error[1], (unsigned long) s.last_error[2]);
			if (have_status)
			{
				printf ("gpulink: STATUS frames %lu, CRC errors %lu, command errors %lu, "
					"ring free %lu KB, last frame %lu us\n",
					(unsigned long) st.frames, (unsigned long) st.crc_errors,
					(unsigned long) st.command_errors,
					(unsigned long) st.ring_free_bytes / 1024,
					(unsigned long) st.last_frame_us);
			}

			frames = 0;
			timeouts = 0;
			frame_done = 0;
			next_report = make_timeout_time_ms (1000);
		}
	}
}
