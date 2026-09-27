/*
 * gears - three meshing gears, in the spirit of the classic glxgears (Brian
 * Paul) and its GL ES 2.0 port es2gears; written from scratch for pgl. Plain
 * GL ES 2.0: one precompiled program (shaders/gears.*, lit per pixel), a
 * vertex buffer per gear, three draws per frame.
 *
 * A HUD (hud.c) shows the frame rate, the load of the V3D, of the Zero's ARM
 * and of the Pico, and the V3D time and panel wait per frame; the Zero
 * measures its part (STATUS, docs/protocol.md 9). The console (UART) prints
 * the same every 5 seconds.
 */
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "gles/pgl.h"
#include "pgpu.h"
#include "hud.h"
#include "pgpu_perf.h"
#include "screen.h"
#include "gears_program.h"

#define PI	3.14159265f

/* ---- matrices (column-major, as GL) ---------------------------------------- */

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

static void mat_translate (float *m, float x, float y, float z)
{
	float t[16];
	mat_identity (t);
	t[12] = x;
	t[13] = y;
	t[14] = z;
	mat_multiply (m, m, t);
}

/* rotate about axis 0 (x), 1 (y) or 2 (z) */
static void mat_rotate (float *m, int axis, float degrees)
{
	float r[16], s = sinf (degrees * PI / 180.0f), c = cosf (degrees * PI / 180.0f);
	int i = (axis + 1) % 3, j = (axis + 2) % 3;
	mat_identity (r);
	r[i*4 + i] = c;
	r[i*4 + j] = s;
	r[j*4 + i] = -s;
	r[j*4 + j] = c;
	mat_multiply (m, m, r);
}

static void mat_frustum (float *m, float l, float r, float b, float t, float n, float f)
{
	memset (m, 0, 16 * sizeof (float));
	m[0] = 2*n / (r - l);
	m[5] = 2*n / (t - b);
	m[8] = (r + l) / (r - l);
	m[9] = (t + b) / (t - b);
	m[10] = -(f + n) / (f - n);
	m[11] = -1.0f;
	m[14] = -2*f*n / (f - n);
}

/* ---- gear geometry ------------------------------------------------------------ */

typedef struct { float x, y, z, nx, ny, nz; } vertex_t;

#define MAX_TEETH	20
#define VERTICES_PER_TOOTH	66		/* 11 quads */

static vertex_t vertices[MAX_TEETH * VERTICES_PER_TOOTH];
static unsigned n_vertices;

static void vertex (const float p[2], float z, float nx, float ny, float nz)
{
	vertices[n_vertices++] = (vertex_t) {p[0], p[1], z, nx, ny, nz};
}

/* a flat face at z: points counter-clockwise seen from +z; the back face
   (normal -z) takes them the other way round */
static void face (const float p[4][2], float z, float nz)
{
	static const int front[6] = {0, 1, 2, 0, 2, 3}, back[6] = {0, 2, 1, 0, 3, 2};
	const int *order = nz > 0 ? front : back;
	for (int i = 0; i < 6; i++)
	{
		vertex (p[order[i]], z, 0.0f, 0.0f, nz);
	}
}

/* the side from P to Q between the faces (z = +-w): outwards is to the right
   of P -> Q seen from +z; normals given per end */
static void side (const float p[2], const float q[2], const float np[2], const float nq[2], float w)
{
	vertex (p, w, np[0], np[1], 0.0f);
	vertex (p, -w, np[0], np[1], 0.0f);
	vertex (q, -w, nq[0], nq[1], 0.0f);
	vertex (p, w, np[0], np[1], 0.0f);
	vertex (q, -w, nq[0], nq[1], 0.0f);
	vertex (q, w, nq[0], nq[1], 0.0f);
}

/* a flat side: the normal is perpendicular to P -> Q */
static void flat_side (const float p[2], const float q[2], float w)
{
	float dx = q[0] - p[0], dy = q[1] - p[1], l = sqrtf (dx*dx + dy*dy);
	float n[2] = {dy / l, -dx / l};
	side (p, q, n, n, w);
}

/* a gear around the z axis: hub hole r_inner, teeth from r_outer - depth/2
   to r_outer + depth/2, width along z */
static void gear (float r_inner, float r_outer, float width, int teeth, float depth)
{
	float r0 = r_inner, r1 = r_outer - depth / 2, r2 = r_outer + depth / 2;
	float da = 2 * PI / teeth / 4, w = width / 2;

	n_vertices = 0;
	for (int i = 0; i < teeth; i++)
	{
		/* a tooth: rises over a0..a1, top a1..a2, falls a2..a3, gap a3..a4 */
		float a[5], c[5], s[5];
		for (int k = 0; k < 5; k++)
		{
			a[k] = i * 2 * PI / teeth + k * da;
			c[k] = cosf (a[k]);
			s[k] = sinf (a[k]);
		}
#define P(r, k)	{(r) * c[k], (r) * s[k]}
		const float in0[2] = P (r0, 0), in3[2] = P (r0, 3), in4[2] = P (r0, 4);
		const float base0[2] = P (r1, 0), base3[2] = P (r1, 3), base4[2] = P (r1, 4);
		const float top1[2] = P (r2, 1), top2[2] = P (r2, 2);
#undef P
		const float ring1[4][2] = {{in0[0], in0[1]}, {base0[0], base0[1]}, {base3[0], base3[1]}, {in3[0], in3[1]}};
		const float ring2[4][2] = {{in3[0], in3[1]}, {base3[0], base3[1]}, {base4[0], base4[1]}, {in4[0], in4[1]}};
		const float tooth[4][2] = {{base0[0], base0[1]}, {top1[0], top1[1]}, {top2[0], top2[1]}, {base3[0], base3[1]}};

		for (int f = 0; f < 2; f++)			/* front, back */
		{
			float z = f ? -w : w, nz = f ? -1.0f : 1.0f;
			face (ring1, z, nz);
			face (ring2, z, nz);
			face (tooth, z, nz);
		}

		/* outside: rounded top and gap, flat flanks */
		const float n1[2] = {c[1], s[1]}, n2[2] = {c[2], s[2]};
		const float n3[2] = {c[3], s[3]}, n4[2] = {c[4], s[4]};
		flat_side (base0, top1, w);
		side (top1, top2, n1, n2, w);
		flat_side (top2, base3, w);
		side (base3, base4, n3, n4, w);

		/* the hub hole, facing the axis (Q -> P keeps the outside right) */
		const float m0[2] = {-c[0], -s[0]}, m4[2] = {-c[4], -s[4]};
		side (in4, in0, m4, m0, w);
	}
}

/* ---- the scene -------------------------------------------------------------------- */

typedef struct
{
	GLuint buffer;
	GLsizei count;
	float x, y;		/* position */
	float speed, phase;	/* rotation: speed * angle + phase (degrees) */
	float color[3];
} gear_t;

static gear_t gears[3] =
{
	{0, 0, -3.0f, -2.0f,  1.0f,   0.0f, {0.85f, 0.12f, 0.08f}},
	{0, 0,  3.1f, -2.0f, -2.0f,  -9.0f, {0.10f, 0.80f, 0.25f}},
	{0, 0, -3.1f,  4.2f, -2.0f, -25.0f, {0.20f, 0.35f, 1.00f}},
};

int main (void)
{
	stdio_init_all ();
	pgpu_init ();
	printf ("\ngears: waiting for the Zero (READY)...\n");
	while (!pgpu_wait_ready (1000))
	{
	}
	pgpu_set_reply_phase (1);
	int tries = 0;
	while (!pglInit () && ++tries < 5)		/* the first reply can be missed */
	{
	}
	if (tries == 5)
	{
		printf ("gears: no answer from the Zero\n");
	}

	GLuint prog = glCreateProgram ();
	glProgramBinaryOES (prog, PGL_PROGRAM_BINARY_PGPU, &gears_info, sizeof gears_info);
	GLint linked = 0;
	glGetProgramiv (prog, GL_LINK_STATUS, &linked);
	if (!linked)
	{
		printf ("gears: the program didn't link\n");
	}
	glUseProgram (prog);
	GLint u_modelview = glGetUniformLocation (prog, "u_modelview");
	GLint u_projection = glGetUniformLocation (prog, "u_projection");
	GLint u_color = glGetUniformLocation (prog, "u_color");
	GLint u_light_dir = glGetUniformLocation (prog, "u_light_dir");
	GLint a_pos = glGetAttribLocation (prog, "a_pos");
	GLint a_normal = glGetAttribLocation (prog, "a_normal");

	/* the classic three: big red, small green, thin blue */
	static const struct { float r_inner, r_outer, width; int teeth; } shape[3] =
	{
		{1.0f, 4.0f, 1.0f, 20}, {0.5f, 2.0f, 2.0f, 10}, {1.3f, 2.0f, 0.5f, 10},
	};
	for (int g = 0; g < 3; g++)
	{
		gear (shape[g].r_inner, shape[g].r_outer, shape[g].width, shape[g].teeth, 0.7f);
		glGenBuffers (1, &gears[g].buffer);
		glBindBuffer (GL_ARRAY_BUFFER, gears[g].buffer);
		glBufferData (GL_ARRAY_BUFFER, n_vertices * sizeof (vertex_t), vertices, GL_STATIC_DRAW);
		gears[g].count = n_vertices;
	}
	GLint vp[4] = {0};
	const float light[3] = {0.408f, 0.408f, 0.816f};	/* (5, 5, 10) normalized */
	glUniform3fv (u_light_dir, 1, light);

	glClearColor (0.04f, 0.04f, 0.08f, 1.0f);
	glDisable (GL_DITHER);
	if (!hud_init ())
	{
		printf ("gears: the HUD program didn't link\n");
	}

	printf ("gears: %u + %u + %u vertices, running\n", (unsigned) gears[0].count,
		(unsigned) gears[1].count, (unsigned) gears[2].count);
	absolute_time_t start = get_absolute_time ();
	unsigned windows = 0;
	perf_t m;
	memset (&m, 0, sizeof m);
	while (true)
	{
		if (screen_update ("gears", vp))
		{
			float projection[16];
			float aspect = (float) vp[2] / vp[3];	/* the screen is wide: keep the height */
			mat_frustum (projection, -aspect, aspect, -1.0f, 1.0f, 5.0f, 60.0f);
			glUseProgram (prog);
			glUniformMatrix4fv (u_projection, 1, GL_FALSE, projection);
		}
		float t = absolute_time_diff_us (start, get_absolute_time ()) / 1e6f;
		float angle = 70.0f * t;			/* degrees, as glxgears */

		/* the camera sways gently around the classic view */
		float view[16];
		mat_identity (view);
		mat_translate (view, 0.0f, 0.0f, -40.0f);
		mat_rotate (view, 0, 20.0f + 10.0f * sinf (t * 0.31f));
		mat_rotate (view, 1, 30.0f + 25.0f * sinf (t * 0.19f));

		glClear (GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
		glUseProgram (prog);
		glEnable (GL_DEPTH_TEST);
		glEnable (GL_CULL_FACE);
		glEnableVertexAttribArray (a_pos);
		glEnableVertexAttribArray (a_normal);
		for (int g = 0; g < 3; g++)
		{
			float modelview[16];
			memcpy (modelview, view, sizeof view);
			mat_translate (modelview, gears[g].x, gears[g].y, 0.0f);
			mat_rotate (modelview, 2, gears[g].speed * angle + gears[g].phase);
			glUniformMatrix4fv (u_modelview, 1, GL_FALSE, modelview);
			glUniform3fv (u_color, 1, gears[g].color);
			glBindBuffer (GL_ARRAY_BUFFER, gears[g].buffer);
			glVertexAttribPointer (a_pos, 3, GL_FLOAT, GL_FALSE, sizeof (vertex_t), (void *) 0);
			glVertexAttribPointer (a_normal, 3, GL_FLOAT, GL_FALSE, sizeof (vertex_t),
					       (void *) offsetof (vertex_t, nx));
			glDrawArrays (GL_TRIANGLES, 0, gears[g].count);
		}
		hud_draw ();
		pglSwapBuffers ();

		absolute_time_t wait_start = get_absolute_time ();
		pgpu_wait_frame (100);			/* pace on the panel (swap interval 1) */
		if (perf_frame (absolute_time_diff_us (wait_start, get_absolute_time ()), &m))
		{
			hud_begin ();
			hud_perf (vp[2] - hud_perf_width (0.5f) - 2, 2, 0.5f, &m);	/* half size, top right */
			hud_end ();
			if (++windows % 5 == 0)
			{
				GLenum e = glGetError ();
				printf ("gears: %.1f fps, load GPU %.0f%% CPU-G %.0f%% CPU-H %.0f%%, "
					"render %.2f ms, panel wait %.2f ms, GL error 0x%x\n",
					m.fps, m.gpu * 100, m.cpu_g * 100, m.cpu_h * 100,
					m.render_ms, m.panel_ms, (unsigned) e);
			}
		}
	}
}
