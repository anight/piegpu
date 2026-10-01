/*
 * pickups.c - see pickups.h
 */
#include "pickups.h"
#include <math.h>
#include <string.h>
#include "mat4.h"
#include "gem_program.h"
#include "halo_program.h"

#define GEM_H		14.0f		/* a gem: its half height, its radius */
#define GEM_R		9.0f
#define COIN_R		12.0f		/* a coin: its radius, half its thickness */
#define COIN_T		2.5f
#define COIN_SIDES	16
#define MAX_VERTICES	(COIN_SIDES * 12)

static const float gem_colors[][3] =
{
	{1.0f, 0.18f, 0.22f}, {0.2f, 0.9f, 0.35f}, {0.25f, 0.5f, 1.0f}, {1.0f, 0.8f, 0.15f}, {0.8f, 0.3f, 1.0f},
	{0.2f, 0.95f, 0.95f}, {1.0f, 0.5f, 0.15f}, {1.0f, 0.45f, 0.8f}, {0.85f, 0.95f, 1.0f},
};
static const float gold[3] = {1.0f, 0.74f, 0.18f};

const float *pickups_color (const pickups_t *s, int item)
{
	return s->shape == PICKUPS_COIN ? gold : gem_colors[item % (int) (sizeof gem_colors / sizeof gem_colors[0])];
}

static float mesh[MAX_VERTICES][6];
static int n;

/* a triangle, counter-clockwise from outside; its normal the face's */
static void triangle (const float a[3], const float b[3], const float c[3])
{
	float u[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]}, v[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
	float nn[3] = {u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0]};
	float l = sqrtf (nn[0] * nn[0] + nn[1] * nn[1] + nn[2] * nn[2]);
	const float *p[3] = {a, b, c};
	for (int k = 0; k < 3; k++, n++)
	{
		memcpy (mesh[n], p[k], 3 * sizeof (float));
		for (int i = 0; i < 3; i++)
			mesh[n][3 + i] = nn[i] / l;
	}
}

/* a gem: an octahedron, stretched up and down */
static void gem (void)
{
	const float top[3] = {0, 0, GEM_H}, bottom[3] = {0, 0, -GEM_H};
	for (int k = 0; k < 4; k++)
	{
		float a0 = k * 1.5707963f, a1 = (k + 1) * 1.5707963f;
		float p0[3] = {cosf (a0) * GEM_R, sinf (a0) * GEM_R, 0}, p1[3] = {cosf (a1) * GEM_R, sinf (a1) * GEM_R, 0};
		triangle (top, p0, p1);
		triangle (bottom, p1, p0);
	}
}

/* a coin: a disc standing up (its faces along y), round z it spins */
static void coin (void)
{
	const float front[3] = {0, -COIN_T, 0}, back[3] = {0, COIN_T, 0};
	for (int k = 0; k < COIN_SIDES; k++)
	{
		float a0 = k * 6.2831853f / COIN_SIDES, a1 = (k + 1) * 6.2831853f / COIN_SIDES;
		float x0 = cosf (a0) * COIN_R, z0 = sinf (a0) * COIN_R, x1 = cosf (a1) * COIN_R, z1 = sinf (a1) * COIN_R;
		float f0[3] = {x0, -COIN_T, z0}, f1[3] = {x1, -COIN_T, z1}, b0[3] = {x0, COIN_T, z0}, b1[3] = {x1, COIN_T, z1};
		triangle (front, f0, f1);
		triangle (back, b1, b0);
		triangle (f0, b0, b1);
		triangle (f0, b1, f1);
	}
}

void pickups_init (pickups_t *s, int shape)
{
	static const float corners[6][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, -1}, {1, 1}, {-1, 1}};
	memset (s, 0, sizeof *s);
	s->shape = shape;
	n = 0;
	if (shape == PICKUPS_COIN)
		coin ();
	else
		gem ();
	s->vertices = n;

	s->gem = glCreateProgram ();
	glProgramBinaryOES (s->gem, PGL_PROGRAM_BINARY_PGPU, &gem_info, sizeof gem_info);
	s->g_vp = glGetUniformLocation (s->gem, "u_vp");
	s->g_model = glGetUniformLocation (s->gem, "u_model");
	s->g_sun = glGetUniformLocation (s->gem, "u_sun");
	s->g_eye = glGetUniformLocation (s->gem, "u_eye");
	s->g_color = glGetUniformLocation (s->gem, "u_color");
	s->g_pos = glGetAttribLocation (s->gem, "a_pos");
	s->g_normal = glGetAttribLocation (s->gem, "a_normal");
	s->halo = glCreateProgram ();
	glProgramBinaryOES (s->halo, PGL_PROGRAM_BINARY_PGPU, &halo_info, sizeof halo_info);
	s->h_vp = glGetUniformLocation (s->halo, "u_vp");
	s->h_center = glGetUniformLocation (s->halo, "u_center");
	s->h_right = glGetUniformLocation (s->halo, "u_right");
	s->h_up = glGetUniformLocation (s->halo, "u_up");
	s->h_size = glGetUniformLocation (s->halo, "u_size");
	s->h_color = glGetUniformLocation (s->halo, "u_color");
	s->h_corner = glGetAttribLocation (s->halo, "a_corner");

	glGenBuffers (1, &s->mesh);
	glBindBuffer (GL_ARRAY_BUFFER, s->mesh);
	glBufferData (GL_ARRAY_BUFFER, n * sizeof mesh[0], mesh, GL_STATIC_DRAW);
	glGenBuffers (1, &s->quad);
	glBindBuffer (GL_ARRAY_BUFFER, s->quad);
	glBufferData (GL_ARRAY_BUFFER, sizeof corners, corners, GL_STATIC_DRAW);
}

static void attributes (GLint a, GLint b)
{
	for (GLint i = 0; i < 4; i++)
	{
		if (i == a || i == b)
			glEnableVertexAttribArray (i);
		else
			glDisableVertexAttribArray (i);
	}
}

void pickups_draw (const pickups_t *s, const game_t *g, const float vp[16], const float eye[3], float yaw, float pitch,
		   float t, const float sun[3])
{
	glUseProgram (s->gem);
	glUniformMatrix4fv (s->g_vp, 1, GL_FALSE, vp);
	glUniform3f (s->g_sun, sun[0], sun[1], sun[2]);
	glUniform3f (s->g_eye, eye[0], eye[1], eye[2]);
	glBindBuffer (GL_ARRAY_BUFFER, s->mesh);
	attributes (s->g_pos, s->g_normal);
	glVertexAttribPointer (s->g_pos, 3, GL_FLOAT, GL_FALSE, 24, (void *) 0);
	glVertexAttribPointer (s->g_normal, 3, GL_FLOAT, GL_FALSE, 24, (void *) 12);
	for (int i = 0; i < g->n_items; i++)
	{
		const game_item_t *it = &g->items[i];
		if (it->taken)
			continue;
		float m[16];
		mat4_identity (m);
		mat4_translate (m, it->origin[0], it->origin[1], it->origin[2] + 4.0f * sinf (t * 2.0f + i));
		mat4_rotate (m, 2, t * (s->shape == PICKUPS_COIN ? 180.0f : 90.0f) + i * 40.0f);
		glUniformMatrix4fv (s->g_model, 1, GL_FALSE, m);
		const float *c = pickups_color (s, i);
		glUniform3f (s->g_color, c[0], c[1], c[2]);
		glDrawArrays (GL_TRIANGLES, 0, s->vertices);
	}

	float cy = cosf (yaw), sy = sinf (yaw), cp = cosf (pitch), sp = sinf (pitch);
	float right[3] = {sy, -cy, 0}, up[3] = {-cy * sp, -sy * sp, cp};
	glUseProgram (s->halo);
	glUniformMatrix4fv (s->h_vp, 1, GL_FALSE, vp);
	glUniform3f (s->h_right, right[0], right[1], right[2]);
	glUniform3f (s->h_up, up[0], up[1], up[2]);
	glBindBuffer (GL_ARRAY_BUFFER, s->quad);
	attributes (s->h_corner, -1);
	glVertexAttribPointer (s->h_corner, 2, GL_FLOAT, GL_FALSE, 8, (void *) 0);
	glEnable (GL_BLEND);
	glBlendFunc (GL_ONE, GL_ONE);
	glDepthMask (GL_FALSE);
	for (int i = 0; i < g->n_items; i++)
	{
		const game_item_t *it = &g->items[i];
		if (it->taken)
			continue;
		float pulse = 0.45f + 0.15f * sinf (t * 3.0f + i);
		const float *c = pickups_color (s, i);
		glUniform3f (s->h_center, it->origin[0], it->origin[1], it->origin[2] + 4.0f * sinf (t * 2.0f + i));
		glUniform1f (s->h_size, s->shape == PICKUPS_COIN ? 30.0f : 34.0f);
		glUniform3f (s->h_color, c[0] * pulse, c[1] * pulse, c[2] * pulse);
		glDrawArrays (GL_TRIANGLES, 0, 6);
	}
	glDepthMask (GL_TRUE);
	glDisable (GL_BLEND);
}
