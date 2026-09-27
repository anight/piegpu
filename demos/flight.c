/*
 * flight - a biplane over a cloud deck, after picojet's flight demo
 * (~/picojet, picojet-flight): the aircraft is held in the middle of the
 * frame and the world turns around it; the camera sits behind and above and
 * never rolls, the aeroplane banks into the turns. picojet has a stick; here
 * an autopilot picks the bank. The flight model is picojet's: the bank
 * follows the target through a first-order lag (ROLL_RESPONSE), the bank
 * gives the turn rate (TURN_RATE at BANK_MAX).
 *
 * The aircraft change every 10 seconds: the CC0 biplane from OpenGameArt
 * (picojet's flight demo had no memory left for it) and picojet's three jets,
 * the F-22, EF-2000 and F-117 (from the pikuma.com course; see assets/), as
 * picojet converted them. The F-22 and EF-2000 burn their afterburners
 * (shaders/plume.*: glowing cones at picojet's nozzle positions); the
 * F-117 has none, by design, and the biplane a propeller. The clouds are
 * two layers of a tileable fBm noise texture made here at start, mipmapped by
 * the GPU: a deck below (sea between the clouds, haze at the horizon) and thin
 * cirrus above, blended over a sky shader with the sun's glow.
 */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "gles/pgl.h"
#include "pgpu.h"
#include "hud.h"
#include "perf.h"
#include "mat4.h"
#include "assets/mesh_biplane.h"
#include "assets/tex_biplane.h"
#include "assets/mesh_f22.h"
#include "assets/tex_f22.h"
#include "assets/mesh_efa.h"
#include "assets/tex_efa.h"
#include "assets/mesh_f117.h"
#include "assets/tex_f117.h"
#include "sky_program.h"
#include "deck_program.h"
#include "cirrus_program.h"
#include "biplane_program.h"
#include "plume_program.h"

#define PI		3.14159265f

/* flight model (picojet's) and autopilot */
#define BANK_MAX	55.0f		/* degrees */
#define ROLL_RESPONSE	1.8f		/* 1/s */
#define TURN_RATE	0.9f		/* rad/s at BANK_MAX */
#define SPEED		20.0f		/* world units/s */

/* camera and world, in units of the aircraft (bounding sphere radius 1) */
#define FOVY		50.0f
#define CAM_BACK	3.0f
#define CAM_UP		0.7f		/* the same angle down to the aircraft as before, closer */
#define CAM_PITCH	6.0f		/* degrees down */
#define DECK_Y		-30.0f		/* far below: the deck drifts by, as seen from altitude */
#define CIRRUS_Y	40.0f
#define EXTENT		600.0f		/* half size of the cloud grids */
#define GRID		16

#define SWITCH_SECONDS	10.0f		/* the next aircraft */

/* the aircraft: meshes and 128x128 RGB565 textures as picojet stores them
   (int16 x y z u v nx ny nz, bounding sphere radius 270) */
typedef struct
{
	const char *name;
	const int16_t (*verts)[8];
	int n_verts;
	const uint16_t (*tris)[3];
	int n_tris;
	const uint16_t *texels;
	float yaw;			/* degrees: turns the mesh's nose to -z */
	float drop;			/* mesh units: lowers it to sit where the others do */
	int nozzles;			/* afterburner plumes: 0 or 2 (z mirrored) */
	float nozzle[3];		/* the mouth, mesh units */
	float nozzle_r[2];		/* its half height and half width */
	GLuint buffer, index, texture;
} aircraft_t;

static aircraft_t fleet[] =
{
	{"BIPLANE", mesh_biplane_verts, MESH_BIPLANE_VERTS, mesh_biplane_tris, MESH_BIPLANE_TRIS,
	 tex_biplane_data, -90.0f, 100, 0},		/* nose -x: checked, the wings are forward; its
							   surface is centred at y +17, the F-22's at -23:
							   lowered a little further than that */
	/* the F-22's nozzles are flat slots, read off the mesh: the aft wedge's
	   edge at x -199 between side walls at z 6 and 32, y -33 .. -18 (picojet
	   has them at z 26, round) */
	{"F-22 RAPTOR", mesh_f22_verts, MESH_F22_VERTS, mesh_f22_tris, MESH_F22_TRIS, tex_f22_data, 90.0f, 0,
	 2, {-199, -25.5f, 19}, {7.5f, 13}},
	/* the EF-2000's: hexagonal rings at x -224, y -58 .. -31, z 3 .. 27 */
	{"EF-2000 TYPHOON", mesh_efa_verts, MESH_EFA_VERTS, mesh_efa_tris, MESH_EFA_TRIS, tex_efa_data, 90.0f, 0,
	 2, {-224, -44.5f, 15}, {13, 12}},
	{"F-117 NIGHTHAWK", mesh_f117_verts, MESH_F117_VERTS, mesh_f117_tris, MESH_F117_TRIS, tex_f117_data, 90.0f, 0, 0},
};							/* the jets: nose +x (narrow end), checked */
#define FLEET	(int) (sizeof fleet / sizeof fleet[0])

/* the flame's cone: rings along it, sides around, and the cap at the nozzle */
#define PLUME_RINGS	10
#define PLUME_SIDES	12
#define PLUME_VERTICES	(PLUME_RINGS * (PLUME_SIDES + 1) + 1 + PLUME_SIDES + 1)
#define PLUME_INDICES	(((PLUME_RINGS - 1) * PLUME_SIDES * 2 + PLUME_SIDES) * 3)
#define PLUME_LENGTH	260.0f		/* mesh units, the outer flame; the core is shorter */

static const float haze[3] = {0.72f, 0.80f, 0.90f}, zenith[3] = {0.16f, 0.36f, 0.72f};

/* ---- the noise tile: fBm of value noise, wrapping at 128 texels ------------------ */

#define NOISE_SIZE	128

static float lattice (int x, int y, int octave, int period)
{
	uint32_t h = (uint32_t) (x & (period - 1)) * 374761393u + (uint32_t) (y & (period - 1)) * 668265263u
		   + (uint32_t) octave * 1274126177u;
	h = (h ^ (h >> 13)) * 1103515245u;
	return ((h ^ (h >> 16)) & 0xFFFF) / 65535.0f;
}

static float value_noise (float x, float y, int octave, int period)
{
	int ix = (int) floorf (x), iy = (int) floorf (y);
	float fx = x - ix, fy = y - iy;
	fx = fx * fx * (3 - 2 * fx);
	fy = fy * fy * (3 - 2 * fy);
	float a = lattice (ix, iy, octave, period), b = lattice (ix + 1, iy, octave, period);
	float c = lattice (ix, iy + 1, octave, period), d = lattice (ix + 1, iy + 1, octave, period);
	return (a + (b - a) * fx) + ((c + (d - c) * fx) - (a + (b - a) * fx)) * fy;
}

static void make_noise (uint8_t *texels)
{
	static float v[NOISE_SIZE * NOISE_SIZE];
	float lo = 1e9f, hi = -1e9f;
	for (int y = 0; y < NOISE_SIZE; y++)
		for (int x = 0; x < NOISE_SIZE; x++)
		{
			float sum = 0.0f, amp = 0.5f;
			for (int o = 0; o < 5; o++, amp *= 0.5f)
			{
				int period = 4 << o;			/* lattice cells per tile */
				float scale = (float) period / NOISE_SIZE;
				sum += amp * value_noise (x * scale, y * scale, o, period);
			}
			v[y * NOISE_SIZE + x] = sum;
			lo = sum < lo ? sum : lo;
			hi = sum > hi ? sum : hi;
		}
	for (int i = 0; i < NOISE_SIZE * NOISE_SIZE; i++)
	{
		texels[i] = (uint8_t) (255.0f * (v[i] - lo) / (hi - lo));
	}
}

/* ---- programs ---------------------------------------------------------------------------- */

typedef struct
{
	GLuint prog;
	GLint vp, origin, extent, eye, scale, offset, noise, haze;
	GLint grid;
} layer_t;

static void layer_init (layer_t *l, const pgpu_program_info_t *info, size_t size)
{
	l->prog = glCreateProgram ();
	glProgramBinaryOES (l->prog, PGL_PROGRAM_BINARY_PGPU, info, size);
	l->vp = glGetUniformLocation (l->prog, "u_vp");
	l->origin = glGetUniformLocation (l->prog, "u_origin");
	l->extent = glGetUniformLocation (l->prog, "u_extent");
	l->eye = glGetUniformLocation (l->prog, "u_eye");
	l->scale = glGetUniformLocation (l->prog, "u_scale");
	l->offset = glGetUniformLocation (l->prog, "u_offset");
	l->noise = glGetUniformLocation (l->prog, "u_noise");
	l->haze = glGetUniformLocation (l->prog, "u_haze");
	l->grid = glGetAttribLocation (l->prog, "a_grid");
}

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

/* the world position in tiles of 1 / scale, wrapped (kept small for the GPU) */
static float wrap (double position, float scale)
{
	double t = position * scale;
	return (float) (t - floor (t));
}

static void draw_layer (const layer_t *l, const float *vpm, const float eye[3], float y,
			float scale1, float scale2, double px, double pz, float drift, GLsizei n_index)
{
	glUseProgram (l->prog);
	glUniformMatrix4fv (l->vp, 1, GL_FALSE, vpm);
	glUniform3f (l->origin, eye[0], y, eye[2]);
	glUniform1f (l->extent, EXTENT);
	glUniform3fv (l->eye, 1, eye);
	glUniform4f (l->scale, scale1, scale2, 0.0f, 0.0f);
	glUniform4f (l->offset, wrap (px, scale1), wrap (pz, scale1),
		     wrap (px + drift, scale2), wrap (pz + 0.6 * drift, scale2));
	glUniform1i (l->noise, 0);
	arrays (l->grid, -1, -1);
	glVertexAttribPointer (l->grid, 2, GL_FLOAT, GL_FALSE, 0, (void *) 0);
	glDrawElements (GL_TRIANGLES, n_index, GL_UNSIGNED_SHORT, (void *) 0);
}

/* ---- main -------------------------------------------------------------------------------- */

int main (void)
{
	stdio_init_all ();
	pgpu_init ();
	printf ("\nflight: waiting for the Zero (READY)...\n");
	while (!pgpu_wait_ready (1000))
	{
	}
	pgpu_set_reply_phase (1);
	int tries = 0;
	while (!pglInit () && ++tries < 5)		/* the first reply can be missed */
	{
	}
	GLint vp[4];
	glGetIntegerv (GL_VIEWPORT, vp);
	float aspect = (float) vp[2] / vp[3];

	/* textures: the noise (unit 0), the aircraft's paint (unit 1), all mipmapped */
	static uint8_t noise[NOISE_SIZE * NOISE_SIZE];
	make_noise (noise);
	GLuint tex[1];
	glGenTextures (1, tex);
	glActiveTexture (GL_TEXTURE0);
	glBindTexture (GL_TEXTURE_2D, tex[0]);
	glPixelStorei (GL_UNPACK_ALIGNMENT, 1);
	glTexImage2D (GL_TEXTURE_2D, 0, GL_LUMINANCE, NOISE_SIZE, NOISE_SIZE, 0, GL_LUMINANCE, GL_UNSIGNED_BYTE, noise);
	glGenerateMipmap (GL_TEXTURE_2D);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glActiveTexture (GL_TEXTURE1);
	glPixelStorei (GL_UNPACK_ALIGNMENT, 2);
	for (int i = 0; i < FLEET; i++)
	{
		glGenTextures (1, &fleet[i].texture);
		glBindTexture (GL_TEXTURE_2D, fleet[i].texture);
		glTexImage2D (GL_TEXTURE_2D, 0, GL_RGB, 128, 128, 0, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, fleet[i].texels);
		glGenerateMipmap (GL_TEXTURE_2D);
		glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
		glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	}
	glActiveTexture (GL_TEXTURE0);

	/* programs */
	GLuint sky = glCreateProgram (), plane = glCreateProgram ();
	glProgramBinaryOES (sky, PGL_PROGRAM_BINARY_PGPU, &sky_info, sizeof sky_info);
	glProgramBinaryOES (plane, PGL_PROGRAM_BINARY_PGPU, &biplane_info, sizeof biplane_info);
	layer_t deck, cirrus;
	layer_init (&deck, &deck_info, sizeof deck_info);
	layer_init (&cirrus, &cirrus_info, sizeof cirrus_info);
	glUseProgram (deck.prog);
	glUniform3fv (deck.haze, 1, haze);

	GLint s_pos = glGetAttribLocation (sky, "a_pos");
	GLint s_horizon = glGetUniformLocation (sky, "u_horizon");
	GLint s_sun = glGetUniformLocation (sky, "u_sun");
	glUseProgram (sky);
	glUniform1f (glGetUniformLocation (sky, "u_aspect"), aspect);
	glUniform3fv (glGetUniformLocation (sky, "u_haze"), 1, haze);
	glUniform3fv (glGetUniformLocation (sky, "u_zenith"), 1, zenith);

	GLint p_vp = glGetUniformLocation (plane, "u_vp");
	GLint p_model = glGetUniformLocation (plane, "u_model");
	GLint p_pos = glGetAttribLocation (plane, "a_pos");
	GLint p_uv = glGetAttribLocation (plane, "a_uv");
	GLint p_normal = glGetAttribLocation (plane, "a_normal");
	const float sun_el = 22.0f * PI / 180, sun_az = 35.0f * PI / 180;
	const float sun[3] = {cosf (sun_el) * sinf (sun_az), sinf (sun_el), -cosf (sun_el) * cosf (sun_az)};
	glUseProgram (plane);
	glUniform3fv (glGetUniformLocation (plane, "u_sun_dir"), 1, sun);
	glUniform1i (glGetUniformLocation (plane, "u_texture"), 1);

	GLuint plume = glCreateProgram ();
	glProgramBinaryOES (plume, PGL_PROGRAM_BINARY_PGPU, &plume_info, sizeof plume_info);
	GLint f_vp = glGetUniformLocation (plume, "u_vp");
	GLint f_model = glGetUniformLocation (plume, "u_model");
	GLint f_nozzle = glGetUniformLocation (plume, "u_nozzle");
	GLint f_radius = glGetUniformLocation (plume, "u_radius");
	GLint f_length = glGetUniformLocation (plume, "u_length");
	GLint f_core = glGetUniformLocation (plume, "u_core");
	GLint f_eye = glGetUniformLocation (plume, "u_eye");
	GLint f_time = glGetUniformLocation (plume, "u_time");
	GLint f_geom = glGetAttribLocation (plume, "a_geom");

	/* buffers: the sky quad, the cloud grid, the plume strip, the aircraft as stored in flash */
	GLuint quad, grid, grid_index, cone, cone_index;
	GLuint *buffers[] = {&quad, &grid, &grid_index, &cone, &cone_index};
	for (unsigned i = 0; i < sizeof buffers / sizeof buffers[0]; i++)
	{
		glGenBuffers (1, buffers[i]);
	}
	static const float corners[8] = {-1, -1, 1, -1, -1, 1, 1, 1};
	glBindBuffer (GL_ARRAY_BUFFER, quad);
	glBufferData (GL_ARRAY_BUFFER, sizeof corners, corners, GL_STATIC_DRAW);

	/* the cone (u along, cos and sin around, 0) and the cap (0, cos, sin, 1 + radius) */
	static float cone_v[PLUME_VERTICES * 4];
	static uint8_t cone_i[PLUME_INDICES];
	int nv = 0, ni = 0;
	for (int i = 0; i < PLUME_RINGS; i++)
		for (int j = 0; j <= PLUME_SIDES; j++)
		{
			float a = 2 * PI * j / PLUME_SIDES;
			const float v4[4] = {(float) i / (PLUME_RINGS - 1), cosf (a), sinf (a), 0.0f};
			memcpy (&cone_v[4 * nv++], v4, sizeof v4);
		}
	for (int i = 0; i + 1 < PLUME_RINGS; i++)
		for (int j = 0; j < PLUME_SIDES; j++)
		{
			uint8_t a = i * (PLUME_SIDES + 1) + j, b = a + PLUME_SIDES + 1;
			const uint8_t t[6] = {a, b, a + 1, a + 1, b, b + 1};
			memcpy (&cone_i[ni], t, sizeof t);
			ni += 6;
		}
	uint8_t centre = nv;
	const float middle[4] = {0.0f, 1.0f, 0.0f, 1.0f};
	memcpy (&cone_v[4 * nv++], middle, sizeof middle);
	for (int j = 0; j <= PLUME_SIDES; j++)
	{
		float a = 2 * PI * j / PLUME_SIDES;
		const float v4[4] = {0.0f, cosf (a), sinf (a), 2.0f};
		memcpy (&cone_v[4 * nv++], v4, sizeof v4);
	}
	for (int j = 0; j < PLUME_SIDES; j++)
	{
		const uint8_t t[3] = {centre, centre + 1 + j, centre + 2 + j};
		memcpy (&cone_i[ni], t, sizeof t);
		ni += 3;
	}
	glBindBuffer (GL_ARRAY_BUFFER, cone);
	glBufferData (GL_ARRAY_BUFFER, sizeof cone_v, cone_v, GL_STATIC_DRAW);
	glBindBuffer (GL_ELEMENT_ARRAY_BUFFER, cone_index);
	glBufferData (GL_ELEMENT_ARRAY_BUFFER, sizeof cone_i, cone_i, GL_STATIC_DRAW);

	static float grid_v[(GRID + 1) * (GRID + 1) * 2];
	static uint16_t grid_i[GRID * GRID * 6];
	for (int z = 0, k = 0; z <= GRID; z++)
		for (int x = 0; x <= GRID; x++)
		{
			grid_v[k++] = 2.0f * x / GRID - 1.0f;
			grid_v[k++] = 2.0f * z / GRID - 1.0f;
		}
	for (int z = 0, k = 0; z < GRID; z++)
		for (int x = 0; x < GRID; x++)
		{
			uint16_t a = z * (GRID + 1) + x, b = a + GRID + 1;
			const uint16_t t[6] = {a, b, a + 1, a + 1, b, b + 1};
			memcpy (&grid_i[k], t, sizeof t);
			k += 6;
		}
	glBindBuffer (GL_ARRAY_BUFFER, grid);
	glBufferData (GL_ARRAY_BUFFER, sizeof grid_v, grid_v, GL_STATIC_DRAW);
	glBindBuffer (GL_ELEMENT_ARRAY_BUFFER, grid_index);
	glBufferData (GL_ELEMENT_ARRAY_BUFFER, sizeof grid_i, grid_i, GL_STATIC_DRAW);
	for (int i = 0; i < FLEET; i++)
	{
		glGenBuffers (1, &fleet[i].buffer);
		glGenBuffers (1, &fleet[i].index);
		glBindBuffer (GL_ARRAY_BUFFER, fleet[i].buffer);
		glBufferData (GL_ARRAY_BUFFER, fleet[i].n_verts * sizeof fleet[i].verts[0], fleet[i].verts, GL_STATIC_DRAW);
		glBindBuffer (GL_ELEMENT_ARRAY_BUFFER, fleet[i].index);
		glBufferData (GL_ELEMENT_ARRAY_BUFFER, fleet[i].n_tris * sizeof fleet[i].tris[0], fleet[i].tris, GL_STATIC_DRAW);
	}

	float projection[16];
	mat4_perspective (projection, FOVY, aspect, 0.5f, 1000.0f);
	glDisable (GL_DITHER);
	glDisable (GL_CULL_FACE);			/* the mesh has a few two-sided parts */
	if (!hud_init ())
	{
		printf ("flight: the HUD program didn't link\n");
	}
	printf ("flight: running, %d aircraft\n", FLEET);

	/* flight state */
	double px = 0.0, pz = 0.0;			/* world position */
	float heading = 0.0f, bank = 0.0f, target = 0.0f, next_target = 3.0f;
	uint32_t rng = 0x9E3779B9u;
	absolute_time_t start = get_absolute_time (), last = start;
	perf_t perf;
	memset (&perf, 0, sizeof perf);
	unsigned frame = 0, windows = 0;
	bool new_perf = true;

	while (true)
	{
		absolute_time_t now = get_absolute_time ();
		float t = absolute_time_diff_us (start, now) / 1e6f;
		float dt = absolute_time_diff_us (last, now) / 1e6f;
		dt = dt > 1.0f / 20 ? 1.0f / 20 : dt;
		last = now;

		/* autopilot: a new bank now and then, sometimes wings level */
		if (t >= next_target)
		{
			rng ^= rng << 13;
			rng ^= rng >> 17;
			rng ^= rng << 5;
			float r = (rng >> 8) / 16777216.0f;
			target = r < 0.25f ? 0.0f : (r < 0.625f ? -1.0f : 1.0f) * (20.0f + 30.0f * ((rng & 0xFF) / 255.0f));
			next_target = t + 4.0f + 5.0f * ((rng >> 4 & 0xFF) / 255.0f);
		}
		bank += (target - bank) * (1.0f - expf (-dt * ROLL_RESPONSE));
		heading += bank / BANK_MAX * TURN_RATE * dt;
		const float f[3] = {sinf (heading), 0.0f, -cosf (heading)};
		px += f[0] * SPEED * dt;
		pz += f[2] * SPEED * dt;

		/* camera behind and above, level but for a small pitch down */
		const float eye[3] = {-f[0] * CAM_BACK, CAM_UP, -f[2] * CAM_BACK};
		const float cp = cosf (CAM_PITCH * PI / 180), sp = sinf (CAM_PITCH * PI / 180);
		const float centre[3] = {eye[0] + f[0] * cp, eye[1] - sp, eye[2] + f[2] * cp}, up[3] = {0, 1, 0};
		float view[16], vpm[16], model[16];
		mat4_look_at (view, eye, centre, up);
		mat4_multiply (vpm, projection, view);

		/* the horizon's row and the sun's place on the screen */
		const float far[3] = {eye[0] + 1000.0f * f[0], eye[1], eye[2] + 1000.0f * f[2]};
		float horizon = (vpm[1] * far[0] + vpm[5] * far[1] + vpm[9] * far[2] + vpm[13])
			      / (vpm[3] * far[0] + vpm[7] * far[1] + vpm[11] * far[2] + vpm[15]);
		float sx = vpm[0] * sun[0] + vpm[4] * sun[1] + vpm[8] * sun[2];
		float sy = vpm[1] * sun[0] + vpm[5] * sun[1] + vpm[9] * sun[2];
		float sw = vpm[3] * sun[0] + vpm[7] * sun[1] + vpm[11] * sun[2];
		float sun_x = sw > 0.01f ? sx / sw : 10.0f, sun_y = sw > 0.01f ? sy / sw : 10.0f;

		glClear (GL_DEPTH_BUFFER_BIT);			/* the sky covers every pixel */
		glDisable (GL_DEPTH_TEST);

		/* sky */
		glUseProgram (sky);
		glUniform1f (s_horizon, horizon);
		glUniform2f (s_sun, sun_x, sun_y);
		arrays (s_pos, -1, -1);
		glBindBuffer (GL_ARRAY_BUFFER, quad);
		glVertexAttribPointer (s_pos, 2, GL_FLOAT, GL_FALSE, 0, (void *) 0);
		glDrawArrays (GL_TRIANGLE_STRIP, 0, 4);

		/* the clouds: the deck below, the cirrus above (blended); unit 0 is
		   shared with the HUD's font */
		glActiveTexture (GL_TEXTURE0);
		glBindTexture (GL_TEXTURE_2D, tex[0]);
		glBindBuffer (GL_ARRAY_BUFFER, grid);
		glBindBuffer (GL_ELEMENT_ARRAY_BUFFER, grid_index);
		draw_layer (&deck, vpm, eye, DECK_Y, 1.0f / 180, 1.0f / 50, px, pz, 1.5f * t, GRID * GRID * 6);
		glEnable (GL_BLEND);
		glBlendFunc (GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
		draw_layer (&cirrus, vpm, eye, CIRRUS_Y, 1.0f / 260, 1.0f / 90, px, pz, 4.0f * t, GRID * GRID * 6);
		glDisable (GL_BLEND);

		/* the aircraft: nose along the heading, banked, bobbing */
		const aircraft_t *a = &fleet[(int) (t / SWITCH_SECONDS) % FLEET];
		glEnable (GL_DEPTH_TEST);
		glUseProgram (plane);
		glUniformMatrix4fv (p_vp, 1, GL_FALSE, vpm);
		mat4_identity (model);
		mat4_translate (model, 0.0f, 0.08f * sinf (t * 0.8f) - a->drop / 270, 0.0f);
		mat4_rotate (model, 1, -heading * 180.0f / PI);
		mat4_rotate (model, 2, -bank);
		mat4_rotate (model, 0, 2.0f * sinf (t * 0.6f));	/* a little pitch */
		mat4_rotate (model, 1, a->yaw);
		mat4_scale (model, 1.0f / 270, 1.0f / 270, 1.0f / 270);
		glUniformMatrix4fv (p_model, 1, GL_FALSE, model);
		arrays (p_pos, p_uv, p_normal);
		glActiveTexture (GL_TEXTURE1);
		glBindTexture (GL_TEXTURE_2D, a->texture);
		glActiveTexture (GL_TEXTURE0);
		glBindBuffer (GL_ARRAY_BUFFER, a->buffer);
		glBindBuffer (GL_ELEMENT_ARRAY_BUFFER, a->index);
		glVertexAttribPointer (p_pos, 3, GL_SHORT, GL_FALSE, 16, (void *) 0);
		glVertexAttribPointer (p_uv, 2, GL_SHORT, GL_FALSE, 16, (void *) 6);
		glVertexAttribPointer (p_normal, 3, GL_SHORT, GL_FALSE, 16, (void *) 10);
		glDrawElements (GL_TRIANGLES, a->n_tris * 3, GL_UNSIGNED_SHORT, (void *) 0);

		/* afterburners: glowing, hidden by the airframe but not hiding each other */
		if (a->nozzles)
		{
			glUseProgram (plume);
			glUniformMatrix4fv (f_vp, 1, GL_FALSE, vpm);
			glUniformMatrix4fv (f_model, 1, GL_FALSE, model);
			glUniform3fv (f_eye, 1, eye);
			glUniform1f (f_time, t);
			glEnable (GL_BLEND);
			glBlendFunc (GL_ONE, GL_ONE);
			glDepthMask (GL_FALSE);
			arrays (f_geom, -1, -1);
			glBindBuffer (GL_ARRAY_BUFFER, cone);
			glBindBuffer (GL_ELEMENT_ARRAY_BUFFER, cone_index);
			glVertexAttribPointer (f_geom, 4, GL_FLOAT, GL_FALSE, 0, (void *) 0);
			for (int n = 0; n < a->nozzles; n++)
			{
				glUniform3f (f_nozzle, a->nozzle[0], a->nozzle[1], n ? -a->nozzle[2] : a->nozzle[2]);
				/* the outer flame, then the core in it */
				glUniform1f (f_length, PLUME_LENGTH);
				glUniform2f (f_radius, 1.15f * a->nozzle_r[0], 1.15f * a->nozzle_r[1]);
				glUniform1f (f_core, 0.0f);
				glDrawElements (GL_TRIANGLES, PLUME_INDICES, GL_UNSIGNED_BYTE, (void *) 0);
				glUniform1f (f_length, 0.55f * PLUME_LENGTH);
				glUniform2f (f_radius, 0.55f * a->nozzle_r[0], 0.55f * a->nozzle_r[1]);
				glUniform1f (f_core, 1.0f);
				glDrawElements (GL_TRIANGLES, PLUME_INDICES, GL_UNSIGNED_BYTE, (void *) 0);
			}
			glDepthMask (GL_TRUE);
			glDisable (GL_BLEND);
		}

		/* HUD: heading and bank at the top left (10 times a second), perf at the top right */
		if (new_perf || frame % 6 == 0)
		{
			char s[24];
			int hdg = (int) lroundf (heading * 180.0f / PI) % 360;
			hud_begin ();
			hud_rect (0, 0, 8 + 15 * HUD_CHAR_W / 2, 31, HUD_RGBA (0, 0, 0, 150));
			hud_text_scaled (4, 3, a->name, HUD_RGBA (110, 230, 255, 255), 0.5f);
			snprintf (s, sizeof s, "HDG  %03d", hdg < 0 ? hdg + 360 : hdg);
			hud_text_scaled (4, 12, s, HUD_RGBA (255, 230, 120, 255), 0.5f);
			snprintf (s, sizeof s, "BANK %2d%c", (int) lroundf (fabsf (bank)), bank > 0.5f ? 'R' : bank < -0.5f ? 'L' : ' ');
			hud_text_scaled (4, 21, s, HUD_RGBA (235, 235, 235, 255), 0.5f);
			hud_perf (vp[2] - hud_perf_width (0.5f) - 2, 2, 0.5f, &perf);
			hud_end ();
		}
		hud_draw ();
		pglSwapBuffers ();

		absolute_time_t wait_start = get_absolute_time ();
		pgpu_wait_frame (100);			/* pace on the panel */
		new_perf = perf_frame (absolute_time_diff_us (wait_start, get_absolute_time ()), &perf);
		frame++;
		if (new_perf && ++windows % 5 == 0)
		{
			GLenum e = glGetError ();
			printf ("flight: %.1f fps, load GPU %.0f%% ARM %.0f%% Pico %.0f%%, render %.2f ms, "
				"heading %.0f, bank %.0f, GL error 0x%x\n", perf.fps, perf.gpu * 100,
				perf.arm * 100, perf.pico * 100, perf.render_ms, heading * 180.0f / PI, bank,
				(unsigned) e);
		}
	}
}
