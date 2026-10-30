/*
 * figures.c - see figures.h. A mesh is prisms: a rectangle at one height, a
 * rectangle at another, the four sides between; each face flat, in a tone of
 * the figure's colour of its own (painted, not shaded smooth).
 */
#include "figures.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "lf_figure_program.h"

typedef struct
{
	float p[3], n[3];
	uint8_t c[4];
} vertex_t;

#define MAX_VERTICES	2400
static vertex_t *mesh;
static int mesh_n;
static uint32_t tone_seed = 7;

/* the paint */
static const float DEEP[3] = {0.05f, 0.13f, 0.16f}, TEAL[3] = {0.12f, 0.34f, 0.38f}, RUST[3] = {0.58f, 0.27f, 0.13f};
static const float BONE[3] = {0.86f, 0.82f, 0.72f}, STONE[3] = {0.50f, 0.56f, 0.55f}, MOSS[3] = {0.22f, 0.38f, 0.24f};
static const float IRON[3] = {0.16f, 0.14f, 0.13f}, GOLD[3] = {1.0f, 0.80f, 0.40f}, BLACK[3] = {0.035f, 0.07f, 0.085f};
static const float COLD[3] = {0.70f, 0.95f, 1.0f};

static void triangle (const float *a, const float *b, const float *c, const float *centre, const float *colour, bool glow)
{
	const float *v[3] = {a, b, c};
	float e1[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]}, e2[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
	float n[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
	float l = sqrtf (n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
	if (l < 1e-6f || mesh_n + 3 > MAX_VERTICES)
	{
		return;
	}
	float out = (a[0] + b[0] + c[0] - 3 * centre[0]) * n[0] + (a[1] + b[1] + c[1] - 3 * centre[1]) * n[1]
		  + (a[2] + b[2] + c[2] - 3 * centre[2]) * n[2];
	float s = out < 0.0f ? -1.0f / l : 1.0f / l;
	for (int i = 0; i < 3; i++)
	{
		vertex_t *o = &mesh[mesh_n++];
		memcpy (o->p, v[i], sizeof o->p);
		o->n[0] = n[0] * s;
		o->n[1] = n[1] * s;
		o->n[2] = n[2] * s;
		for (int k = 0; k < 3; k++)
		{
			float x = colour[k] * 255.0f;
			o->c[k] = (uint8_t) (x > 255.0f ? 255.0f : x);
		}
		o->c[3] = glow ? 0 : 255;
	}
}

/* a face's own tone of a colour */
static void toned (float out[3], const float *colour, bool glow)
{
	tone_seed = tone_seed * 1664525u + 1013904223u;
	float k = glow ? 1.0f : 0.88f + 0.24f * ((tone_seed >> 16 & 255) / 255.0f);
	for (int i = 0; i < 3; i++)
	{
		out[i] = colour[i] * k;
	}
}

static void quad (const float *a, const float *b, const float *c, const float *d, const float *centre, const float *colour, bool glow)
{
	float t[3];
	toned (t, colour, glow);
	triangle (a, b, c, centre, t, glow);
	triangle (a, c, d, centre, t, glow);
}

/* a prism: at z0 the rectangle x0 .. x1, y0 .. y1; at z1 the rectangle X0 .. X1, Y0 .. Y1 */
static void prism (float x0, float x1, float y0, float y1, float z0, float X0, float X1, float Y0, float Y1, float z1,
		   const float *colour, bool glow)
{
	const float p[8][3] = {{x0, y0, z0}, {x1, y0, z0}, {x1, y1, z0}, {x0, y1, z0}, {X0, Y0, z1}, {X1, Y0, z1}, {X1, Y1, z1}, {X0, Y1, z1}};
	const float c[3] = {(x0 + x1 + X0 + X1) / 4, (y0 + y1 + Y0 + Y1) / 4, (z0 + z1) / 2};
	static const int f[6][4] = {{0, 1, 2, 3}, {4, 5, 6, 7}, {0, 1, 5, 4}, {1, 2, 6, 5}, {2, 3, 7, 6}, {3, 0, 4, 7}};
	for (int k = 0; k < 6; k++)
	{
		quad (p[f[k][0]], p[f[k][1]], p[f[k][2]], p[f[k][3]], c, colour, glow);
	}
}

/* a box */
static void block (float x0, float x1, float y0, float y1, float z0, float z1, const float *colour, bool glow)
{
	prism (x0, x1, y0, y1, z0, x0, x1, y0, y1, z1, colour, glow);
}

/* a ball of 32 faces (an octahedron's, each cut in four), r across, squashed to rz up */
static void ball (float r, float rz, const float *colour, bool glow)
{
	static const float o[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
	static const int f[8][3] = {{0, 2, 4}, {2, 1, 4}, {1, 3, 4}, {3, 0, 4}, {2, 0, 5}, {1, 2, 5}, {3, 1, 5}, {0, 3, 5}};
	const float centre[3] = {0, 0, 0};
	for (int k = 0; k < 8; k++)
	{
		float v[6][3];
		for (int i = 0; i < 3; i++)
		{
			const float *a = o[f[k][i]], *b = o[f[k][(i + 1) % 3]];
			float m[3] = {a[0] + b[0], a[1] + b[1], a[2] + b[2]}, l = sqrtf (m[0] * m[0] + m[1] * m[1] + m[2] * m[2]);
			for (int c = 0; c < 3; c++)
			{
				v[i][c] = a[c] * (c == 2 ? rz : r);
				v[3 + i][c] = m[c] / l * (c == 2 ? rz : r);
			}
		}
		static const int t[4][3] = {{0, 3, 5}, {3, 1, 4}, {5, 4, 2}, {3, 4, 5}};
		for (int j = 0; j < 4; j++)
		{
			float tc[3];
			toned (tc, colour, glow);
			triangle (v[t[j][0]], v[t[j][1]], v[t[j][2]], centre, tc, glow);
		}
	}
}

static void build (figures_t *f)
{
	#define MESH(m)		f->count[last] = mesh_n - f->first[last]; last = m; f->first[m] = mesh_n
	int last = 0;
	f->first[0] = 0;

	/* Wren: a cloak, a hood with a face in it; a leg, an arm (drawn twice each); her lantern.
	   Her middle is 24 over her feet (the engine's player's) */
	MESH (M_WREN_BODY);
	prism (-7, 7, -9, 9, -12, -5, 5, -7, 7, 14, RUST, false);
	block (-5.5f, 5.5f, -7.5f, 7.5f, 10, 14, BONE, false);			/* a scarf */
	block (-8, -5, -6, 6, -4, 10, IRON, false);				/* her pack */
	MESH (M_WREN_HEAD);
	prism (-6, 5, -6, 6, 13, -5, 4, -5, 5, 30, TEAL, false);
	block (4.5f, 6.5f, -3.5f, 3.5f, 17, 25, BONE, false);
	MESH (M_WREN_LEG);							/* (from its hip, at 0 0 0, down) */
	prism (-3, 3, -3, 3, -16, -3, 3, -3, 3, 0, DEEP, false);
	block (-3, 5, -3, 3, -16, -12, IRON, false);				/* a boot */
	MESH (M_WREN_ARM);							/* (from its shoulder down) */
	prism (-2, 2, -2, 2, -13, -2.5f, 2.5f, -2.5f, 2.5f, 0, RUST, false);
	block (-2, 2, -2, 2, -15, -13, BONE, false);
	MESH (M_LANTERN);							/* (hanging from 0 0 0) */
	block (-0.5f, 0.5f, -0.5f, 0.5f, -3, 0, IRON, false);
	block (-3.5f, 3.5f, -3.5f, 3.5f, -4.5f, -3, IRON, false);
	block (-2.5f, 2.5f, -2.5f, 2.5f, -10, -4.5f, GOLD, true);
	block (-3.5f, 3.5f, -3.5f, 3.5f, -11.5f, -10, IRON, false);

	/* Ody: a body of stone, broad at the shoulders; a head with one eye; his
	   arm, and the stump of the other. 88 tall: his middle 24 over his feet */
	MESH (M_ODY_TORSO);
	prism (-15, 15, -24, 24, 2, -19, 19, -30, 30, 44, STONE, false);
	block (-12, 12, 12, 30, 44, 49, MOSS, false);
	block (-16, -12, -20, 4, 30, 44, MOSS, false);
	block (-16, 16, -22, 22, -2, 2, IRON, false);				/* a belt of iron */
	MESH (M_ODY_HEAD);
	prism (-9, 11, -10, 10, 44, -7, 9, -8, 8, 60, STONE, false);
	MESH (M_ODY_EYE);							/* his eye, and the mark on his chest: lit when he's awake */
	block (11, 12, -2, 4, 50, 55, GOLD, true);
	block (19, 20, -5, 5, 22, 32, GOLD, true);
	block (19, 20, -9, 9, 26, 28, GOLD, true);
	MESH (M_ODY_LEG);							/* (from the hip down) */
	prism (-12, 12, -9, 9, -30, -10, 10, -9, 9, 0, STONE, false);
	MESH (M_ODY_ARM);							/* (from the shoulder down: the arm, the fist) */
	prism (-8, 8, -7, 7, -30, -9, 9, -8, 8, 0, STONE, false);
	block (-12, 12, -11, 11, -46, -30, STONE, false);
	block (-13, 13, -12, 12, -34, -31, IRON, false);
	MESH (M_ODY_STUMP);
	prism (-6, 6, -5, 5, -11, -8, 8, -7, 7, 0, STONE, false);
	block (-5, 5, -4, 4, -12, -11, RUST, false);

	/* the Hollow: a ball of black water; its eyes; a brute's horns; a listener */
	MESH (M_BLOB);
	ball (18.0f, 15.0f, BLACK, false);
	MESH (M_EYES);
	block (15.5f, 17.5f, 3, 7, 0, 3, BONE, true);
	block (15.5f, 17.5f, -7, -3, 0, 3, BONE, true);
	MESH (M_HORNS);
	prism (-3, 3, 6, 12, 8, -9, -7, 13, 15, 26, BLACK, false);
	prism (-3, 3, -12, -6, 8, -9, -7, -15, -13, 26, BLACK, false);
	MESH (M_LISTENER);
	prism (-9, 9, -9, 9, -24, -4, 4, -4, 4, 14, BLACK, false);
	prism (-4, 4, -4, 4, 14, -3, 3, -3, 3, 52, BLACK, false);
	prism (-4, 4, -4, 4, 52, -18, 18, -18, 18, 58, BONE, false);		/* its dish, to hear with */
	block (-2, 2, -2, 2, 44, 48, BONE, true);

	/* the Tide King: a robe, shoulders of iron, a crowned head; his eyes */
	MESH (M_KING);
	prism (-14, 14, -16, 16, -24, -8, 8, -11, 11, 40, BLACK, false);
	block (-9, 9, -18, 18, 36, 44, IRON, false);
	prism (-6, 6, -6, 6, 44, -5, 5, -5, 5, 58, STONE, false);
	for (int k = -1; k <= 1; k++)
	{
		prism (-1.5f, 1.5f, k * 4.5f - 1.5f, k * 4.5f + 1.5f, 58, -0.2f, 0.2f, k * 4.5f - 0.2f, k * 4.5f + 0.2f, k ? 68 : 74, RUST, false);
	}
	block (6, 12, 14, 20, 4, 14, IRON, false);				/* the lantern he put out */
	MESH (M_KING_EYES);
	block (5, 6.5f, 1.5f, 4, 50, 52.5f, COLD, true);
	block (5, 6.5f, -4, -1.5f, 50, 52.5f, COLD, true);

	/* the Archivist, sitting: a robe, a blindfold, white hair */
	MESH (M_ARCHIVIST);
	prism (-11, 13, -12, 12, -24, -8, 9, -10, 10, -2, TEAL, false);
	prism (-7, 7, -9, 9, -2, -5, 5, -7, 7, 20, TEAL, false);
	block (-5, 5, -5, 5, 20, 32, BONE, false);
	block (4.5f, 6, -5.5f, 5.5f, 25, 28, RUST, false);
	prism (-7, -3, -6, 6, 12, -6, 0, -5.5f, 5.5f, 33, BONE, false);

	/* a fire's bowl on its foot (a beacon: drawn larger), its coals; an ember; a piece of a broken wall */
	MESH (M_BRAZIER);
	block (-7, 7, -7, 7, 0, 26, IRON, false);
	prism (-9, 9, -9, 9, 26, -20, 20, -20, 20, 40, IRON, false);
	MESH (M_COALS);
	block (-16, 16, -16, 16, 38, 42, GOLD, true);
	MESH (M_EMBER);
	ball (6.0f, 8.0f, GOLD, true);
	MESH (M_CHUNK);
	prism (-6, 6, -5, 5, -4, -4, 5, -3, 4, 4, BONE, false);
	f->count[last] = mesh_n - f->first[last];
	#undef MESH
}

bool figures_init (figures_t *f)
{
	memset (f, 0, sizeof *f);
	mesh = malloc (MAX_VERTICES * sizeof *mesh);
	if (!mesh)
	{
		return false;
	}
	mesh_n = 0;
	build (f);
	glGenBuffers (1, &f->buffer);
	glBindBuffer (GL_ARRAY_BUFFER, f->buffer);
	glBufferData (GL_ARRAY_BUFFER, mesh_n * (GLsizeiptr) sizeof *mesh, mesh, GL_STATIC_DRAW);
	free (mesh);
	mesh = NULL;

	f->program = glCreateProgram ();
	glProgramBinaryOES (f->program, PGL_PROGRAM_BINARY_PGPU, &lf_figure_info, sizeof lf_figure_info);
	f->u_vp = glGetUniformLocation (f->program, "u_vp");
	f->u_model = glGetUniformLocation (f->program, "u_model");
	f->u_eye = glGetUniformLocation (f->program, "u_eye");
	f->u_ambient = glGetUniformLocation (f->program, "u_ambient");
	f->u_lantern = glGetUniformLocation (f->program, "u_lantern");
	f->u_gold = glGetUniformLocation (f->program, "u_gold");
	f->u_fire = glGetUniformLocation (f->program, "u_fire");
	f->u_fire_gold = glGetUniformLocation (f->program, "u_fire_gold");
	f->u_rim = glGetUniformLocation (f->program, "u_rim");
	f->u_tint = glGetUniformLocation (f->program, "u_tint");
	f->u_fog = glGetUniformLocation (f->program, "u_fog");
	f->u_time = glGetUniformLocation (f->program, "u_time");
	f->u_wobble = glGetUniformLocation (f->program, "u_wobble");
	f->a_pos = glGetAttribLocation (f->program, "a_pos");
	f->a_normal = glGetAttribLocation (f->program, "a_normal");
	f->a_color = glGetAttribLocation (f->program, "a_color");
	return true;
}

void figures_begin (const figures_t *f, const float vp[16], const float eye[3], const lights_t *l, float time)
{
	glUseProgram (f->program);
	glUniformMatrix4fv (f->u_vp, 1, GL_FALSE, vp);
	glUniform3fv (f->u_eye, 1, eye);
	glUniform4fv (f->u_lantern, 1, l->lantern);
	glUniform3fv (f->u_gold, 1, l->gold);
	glUniform4fv (f->u_fire, 1, l->fire);
	glUniform3fv (f->u_fire_gold, 1, l->fire_gold);
	glUniform4fv (f->u_fog, 1, l->fog);
	glUniform1f (f->u_time, fmodf (time, 6.2831853f * 20));
	glBindBuffer (GL_ARRAY_BUFFER, f->buffer);
	for (GLint i = 0; i < 4; i++)
	{
		if (i == f->a_pos || i == f->a_normal || i == f->a_color)
			glEnableVertexAttribArray (i);
		else
			glDisableVertexAttribArray (i);
	}
	glVertexAttribPointer (f->a_pos, 3, GL_FLOAT, GL_FALSE, sizeof (vertex_t), (void *) 0);
	glVertexAttribPointer (f->a_normal, 3, GL_FLOAT, GL_FALSE, sizeof (vertex_t), (void *) 12);
	glVertexAttribPointer (f->a_color, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof (vertex_t), (void *) 24);
}

void figures_draw (const figures_t *f, int m, const float model[16], const float ambient[3], const float tint[3],
		   const float rim[3], float wobble)
{
	static const float none[3] = {0, 0, 0}, one[3] = {1, 1, 1};
	glUniformMatrix4fv (f->u_model, 1, GL_FALSE, model);
	glUniform3fv (f->u_ambient, 1, ambient);
	glUniform3fv (f->u_tint, 1, tint ? tint : one);
	glUniform3fv (f->u_rim, 1, rim ? rim : none);
	glUniform1f (f->u_wobble, wobble);
	glDrawArrays (GL_TRIANGLES, f->first[m], f->count[m]);
}

/* ---- the level's light at a place --------------------------------------------------------------- */

/* the lightmap's value (0 .. 255) on the first floor under p (down the tree, as Quake's light point); -1 none */
static int light_point (const bsp_t *b, int num, const float *start, const float *end)
{
	while (num >= 0)
	{
		const bsp_node_t *node = &b->nodes[num];
		const bsp_plane_t *pl = &b->planes[node->plane];
		float front = pl->normal[0] * start[0] + pl->normal[1] * start[1] + pl->normal[2] * start[2] - pl->dist;
		float back = pl->normal[0] * end[0] + pl->normal[1] * end[1] + pl->normal[2] * end[2] - pl->dist;
		int side = front < 0.0f;
		if ((back < 0.0f) == side)
		{
			num = node->children[side];
			continue;
		}
		float frac = front / (front - back), mid[3];
		for (int k = 0; k < 3; k++)
		{
			mid[k] = start[k] + (end[k] - start[k]) * frac;
		}
		int r = light_point (b, node->children[side], start, mid);
		if (r >= 0)
		{
			return r;
		}
		for (int i = 0; i < node->faces; i++)
		{
			const bsp_face_t *f = &b->faces[node->first_face + i];
			const bsp_texinfo_t *ti = &b->texinfo[f->texinfo];
			if (f->light_ofs < 0 || f->styles[0] == 255)
			{
				continue;
			}
			float mn[2] = {1e30f, 1e30f}, mx[2] = {-1e30f, -1e30f}, st[2];
			for (int k = 0; k < f->edges; k++)
			{
				const float *v = bsp_face_vertex (b, f, k);
				for (int a = 0; a < 2; a++)
				{
					float s = v[0] * ti->vecs[a][0] + v[1] * ti->vecs[a][1] + v[2] * ti->vecs[a][2] + ti->vecs[a][3];
					mn[a] = s < mn[a] ? s : mn[a];
					mx[a] = s > mx[a] ? s : mx[a];
				}
			}
			for (int a = 0; a < 2; a++)
			{
				st[a] = mid[0] * ti->vecs[a][0] + mid[1] * ti->vecs[a][1] + mid[2] * ti->vecs[a][2] + ti->vecs[a][3];
			}
			if (st[0] < mn[0] || st[0] > mx[0] || st[1] < mn[1] || st[1] > mx[1])
			{
				continue;
			}
			int bmin[2] = {(int) floorf (mn[0] / 16), (int) floorf (mn[1] / 16)};
			int w = (int) ceilf (mx[0] / 16) - bmin[0] + 1, h = (int) ceilf (mx[1] / 16) - bmin[1] + 1;
			int x = (int) (st[0] / 16) - bmin[0], y = (int) (st[1] / 16) - bmin[1];
			x = x < 0 ? 0 : x >= w ? w - 1 : x;
			y = y < 0 ? 0 : y >= h ? h - 1 : y;
			if ((size_t) f->light_ofs + (size_t) (y * w + x) < b->lighting_size)
			{
				return b->lighting[f->light_ofs + y * w + x];
			}
		}
		return light_point (b, node->children[!side], mid, end);	/* on, behind the plane */
	}
	return -1;
}

void light_at (const bsp_t *bsp, const float p[3], float storm, float rgb[3])
{
	static const float cold[3] = {0.50f, 0.86f, 0.92f}, warm[3] = {1.0f, 0.76f, 0.42f}, flash[3] = {0.55f, 0.75f, 0.85f};
	const float down[3] = {p[0], p[1], p[2] - 256.0f};
	int v = light_point (bsp, bsp->models[0].headnode[0], p, down);
	float l = v < 0 ? 0.3f : v / 128.0f, w = l * 1.2f - 0.45f;
	w = w < 0.0f ? 0.0f : w > 1.0f ? 1.0f : w;
	for (int k = 0; k < 3; k++)
	{
		rgb[k] = (cold[k] + (warm[k] - cold[k]) * w) * (0.55f + 1.25f * l) + flash[k] * storm;	/* (as shaders/world.frag) */
	}
}
