/*
 * lanternfall - a third-person adventure on the BSP engine (engine/): Wren, a
 * ferry pilot with the last ember of the old light in her lantern, crosses
 * six drowned islands to the peak where the great lantern went out, with
 * Ody, a stone golem she wakes on the way. Six scenes (levels/, made by
 * tools/make_*.py), the story in lines on the screen, two endings.
 *
 * The game itself is lf.c (the lantern, Ody, the Hollow, the beacons, the
 * Tide King, the story's lines; bot.c plays it by itself). Here: the stick,
 * the camera behind Wren, the picture, the sound and the music.
 *
 *   the stick     forward and back, and turning; pushed gently: softly
 *   B             jump
 *   A             the lantern raised while it's held (it spends the ember)
 *   X             a flare (a quarter of the ember at once)
 *   Y             Ody: what the place asks (break, lift, stand, strike; come)
 *   SELECT        the camera nearer or further
 *   START         begin; a pause
 *
 * On the console: w s, a d to turn, q e sideways, space; l the lantern (up,
 * down), f a flare, y Ody; c softly (on, off), v the camera, p a pause, b the
 * game by itself (on, off), n the next scene, r the scene again, h the
 * numbers, m the antialiasing.
 *
 * The picture: the engine draws the level with this game's own shaders in
 * place of its own (shaders/world, liquid, skydome): the level's light is
 * teal where it's dim and gold where it's bright, the lantern and the nearest
 * fire light what's near them, lightning all of it, and everything far is
 * lost in fog. The people and things are flat-faced figures (figures.c), lit
 * the same way. What glows (flames, the lantern, sparks) is a few small
 * squares of light, added. 4x MSAA.
 *
 * The sound: the engine's (steps, water, wind, doors) and the game's own
 * (sounds.c, made by tools/make_sounds.py); the music is a cello's note and a
 * bell's, played here at the pitches of a few sparse phrases.
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
#include "bsp.h"
#include "render.h"
#include "game.h"
#include "keys.h"
#include "sound.h"
#include "flare_program.h"
#include "lf.h"
#include "figures.h"
#include "sounds.h"

#define PI		3.14159265f
#define FOVY		66.0f
#define KEY_HOLD_S	0.3f		/* a key on the console holds this long */
#define TITLE_IDLE_S	18.0f		/* the title left alone: the game plays itself */
#define CARD_S		4.0f		/* a scene's name shown */

extern const uint8_t lf_level_0[], lf_level_0_end[], lf_level_1[], lf_level_1_end[], lf_level_2[], lf_level_2_end[];
extern const uint8_t lf_level_3[], lf_level_3_end[], lf_level_4[], lf_level_4_end[], lf_level_5[], lf_level_5_end[];
static const struct { const uint8_t *data, *end; } levels[LF_LEVELS] =
{
	{lf_level_0, lf_level_0_end}, {lf_level_1, lf_level_1_end}, {lf_level_2, lf_level_2_end},
	{lf_level_3, lf_level_3_end}, {lf_level_4, lf_level_4_end}, {lf_level_5, lf_level_5_end},
};

/* a scene's air: the fog's colour and how far one sees; a storm over it? */
static const struct { float fog[3], far; bool storm; } airs[LF_LEVELS] =
{
	{{0.150f, 0.270f, 0.300f}, 2900.0f, false}, {{0.070f, 0.150f, 0.170f}, 2100.0f, false},
	{{0.080f, 0.180f, 0.150f}, 1900.0f, false}, {{0.110f, 0.160f, 0.220f}, 2600.0f, true},
	{{0.160f, 0.250f, 0.300f}, 3300.0f, false}, {{0.110f, 0.150f, 0.210f}, 3000.0f, true},
};

static bsp_t bsp;
static render_t r;
static lf_t s;
static lf_bot_t bot;
static figures_t fig;
static sound_t snd;
static lights_t lights;
static float t_now;			/* seconds */

static float clampf (float x, float lo, float hi)	{ return x < lo ? lo : x > hi ? hi : x; }
static void vset (float *v, float x, float y, float z)	{ v[0] = x; v[1] = y; v[2] = z; }
static void vcopy (float *v, const float *a)		{ memcpy (v, a, 3 * sizeof (float)); }

static float dist3 (const float *a, const float *b)
{
	return sqrtf ((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2]));
}

static uint32_t rng = 0x1234567;

static float rnd (void)
{
	rng = rng * 1664525u + 1013904223u;
	return (rng >> 8 & 0xFFFF) / 65535.0f;
}

/* ---- the sound, the music ------------------------------------------------------------------------ */

#define OWNER_MUSIC	40		/* the music's voices: + 0, 1, 2 */
#define OWNER_LANTERN	50
#define OWNER_ODY	51

static void sfx (int id, const float *at, int owner, float volume, float attenuation, float pitch)
{
	const lf_sound_t *d = &lf_sounds[id - LSND_FIRST];
	int ch = sound_play_other (&snd, id, (float) d->frames / d->rate / pitch, at, owner, volume, attenuation);
	if (ch >= 0 && pitch != 1.0f)
	{
		pgpu_sound_pitch (ch, pitch);
	}
}

/* A phrase: notes (when, in beats; which, in semitones from the scene's
   root; how loud). The cello plays one now and then, a long quiet between; a
   bell answers sometimes. Sparse: more silence than notes. */
typedef struct { float beat; int8_t note; uint8_t volume; } note_t;
static const note_t phrases[][6] =
{
	{{0, 0, 70}, {2, 3, 60}, {4, 7, 70}, {7, 5, 55}, {-1, 0, 0}},
	{{0, 7, 65}, {3, 5, 55}, {5, 3, 60}, {8, 0, 70}, {-1, 0, 0}},
	{{0, 0, 70}, {4, -2, 60}, {7, 0, 55}, {-1, 0, 0}},
	{{0, 3, 60}, {2, 5, 60}, {4, 7, 65}, {6, 10, 60}, {9, 7, 55}, {-1, 0, 0}},
	{{0, 12, 55}, {3, 10, 50}, {6, 7, 60}, {-1, 0, 0}},
};
#define PHRASES		(int) (sizeof phrases / sizeof phrases[0])
static const int8_t roots[LF_LEVELS] = {0, -2, 3, -4, 5, -2};	/* (from the cello's C) */

static struct
{
	int phrase, note;
	float at, next;			/* the phrase's start; when the next begins */
	float bell;
} music = {.phrase = -1, .next = 3.0f, .bell = 9.0f};

static void music_update (int level, bool playing)
{
	const float beat = 0.95f;
	if (!playing)
	{
		return;
	}
	if (music.phrase < 0 && t_now >= music.next)
	{
		music.phrase = (int) (rnd () * PHRASES) % PHRASES;
		music.note = 0;
		music.at = t_now;
	}
	if (music.phrase >= 0)
	{
		const note_t *n = &phrases[music.phrase][music.note];
		if (n->beat < 0)
		{
			music.phrase = -1;
			music.next = t_now + 5.0f + 7.0f * rnd ();
		}
		else if (t_now >= music.at + n->beat * beat)
		{
			sfx (LSND_CELLO, NULL, OWNER_MUSIC + (music.note & 1), n->volume / 255.0f, 0.0f, powf (2.0f, (n->note + roots[level]) / 12.0f));
			music.note++;
		}
	}
	if (t_now >= music.bell)			/* a bell far off, a fifth or an octave over the root */
	{
		static const int8_t over[4] = {0, 7, 12, 3};
		sfx (LSND_BELL, NULL, OWNER_MUSIC + 2, 0.16f, 0.0f, powf (2.0f, (over[(int) (rnd () * 4) & 3] + roots[level] - 7) / 12.0f));
		music.bell = t_now + 9.0f + 14.0f * rnd ();
	}
}

/* ---- the lights that are added: small squares facing the eye --------------------------------------- */

typedef struct
{
	float p[3], uv[2];
	uint8_t c[4];
} flare_vertex_t;

#define FLARES		56
static flare_vertex_t flares[FLARES * 6];
static int flare_n;
static float cam_right[3], cam_up[3], cam_eye[3];
static struct { GLuint program, buffer, texture; GLint u_vp, u_eye, u_fog, a_pos, a_uv, a_color; } fl;

/* a soft light at p, size to each side (never large on the screen: blended pixels are dear) */
static void flare (const float *p, float size, float red, float green, float blue)
{
	static const float corner[6][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, -1}, {1, 1}, {-1, 1}};
	float d = dist3 (p, cam_eye);
	if (flare_n + 6 > FLARES * 6 || d > 2600.0f)
	{
		return;
	}
	size = size < 0.07f * d ? size : 0.07f * d;
	for (int k = 0; k < 6; k++)
	{
		flare_vertex_t *v = &flares[flare_n++];
		for (int i = 0; i < 3; i++)
		{
			v->p[i] = p[i] + (cam_right[i] * corner[k][0] + cam_up[i] * corner[k][1]) * size;
		}
		v->uv[0] = 0.5f + 0.5f * corner[k][0];
		v->uv[1] = 0.5f + 0.5f * corner[k][1];
		v->c[0] = (uint8_t) (clampf (red, 0, 1) * 255);
		v->c[1] = (uint8_t) (clampf (green, 0, 1) * 255);
		v->c[2] = (uint8_t) (clampf (blue, 0, 1) * 255);
		v->c[3] = 255;
	}
}

static void flares_init (void)
{
	static uint32_t texels[32 * 32];
	for (int y = 0; y < 32; y++)
		for (int x = 0; x < 32; x++)
		{
			float dx = (x + 0.5f) / 16 - 1, dy = (y + 0.5f) / 16 - 1, a = clampf (1.0f - sqrtf (dx * dx + dy * dy), 0, 1);
			uint32_t v = (uint32_t) (clampf (a * a * (0.6f + 2.2f * a * a), 0, 1) * 255);
			texels[y * 32 + x] = v | v << 8 | v << 16 | 0xFF000000u;
		}
	glGenTextures (1, &fl.texture);
	glBindTexture (GL_TEXTURE_2D, fl.texture);
	glPixelStorei (GL_UNPACK_ALIGNMENT, 4);
	glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA, 32, 32, 0, GL_RGBA, GL_UNSIGNED_BYTE, texels);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glGenBuffers (1, &fl.buffer);
	glBindBuffer (GL_ARRAY_BUFFER, fl.buffer);
	glBufferData (GL_ARRAY_BUFFER, sizeof flares, NULL, GL_DYNAMIC_DRAW);
	fl.program = glCreateProgram ();
	glProgramBinaryOES (fl.program, PGL_PROGRAM_BINARY_PGPU, &flare_info, sizeof flare_info);
	fl.u_vp = glGetUniformLocation (fl.program, "u_vp");
	fl.u_eye = glGetUniformLocation (fl.program, "u_eye");
	fl.u_fog = glGetUniformLocation (fl.program, "u_fog");
	fl.a_pos = glGetAttribLocation (fl.program, "a_pos");
	fl.a_uv = glGetAttribLocation (fl.program, "a_uv");
	fl.a_color = glGetAttribLocation (fl.program, "a_color");
	glUseProgram (fl.program);
	glUniform1i (glGetUniformLocation (fl.program, "u_texture"), 0);
}

static void flares_draw (const float vp[16], float fog_far)
{
	if (!flare_n)
	{
		return;
	}
	glUseProgram (fl.program);
	glUniformMatrix4fv (fl.u_vp, 1, GL_FALSE, vp);
	glUniform3fv (fl.u_eye, 1, cam_eye);
	glUniform1f (fl.u_fog, 1.6f / fog_far);
	glBindBuffer (GL_ARRAY_BUFFER, fl.buffer);
	glBufferSubData (GL_ARRAY_BUFFER, 0, flare_n * (GLsizeiptr) sizeof (flare_vertex_t), flares);
	for (GLint i = 0; i < 4; i++)
	{
		if (i == fl.a_pos || i == fl.a_uv || i == fl.a_color)
			glEnableVertexAttribArray (i);
		else
			glDisableVertexAttribArray (i);
	}
	glVertexAttribPointer (fl.a_pos, 3, GL_FLOAT, GL_FALSE, sizeof (flare_vertex_t), (void *) 0);
	glVertexAttribPointer (fl.a_uv, 2, GL_FLOAT, GL_FALSE, sizeof (flare_vertex_t), (void *) 12);
	glVertexAttribPointer (fl.a_color, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof (flare_vertex_t), (void *) 20);
	glActiveTexture (GL_TEXTURE0);
	glBindTexture (GL_TEXTURE_2D, fl.texture);
	glEnable (GL_BLEND);
	glBlendFunc (GL_ONE, GL_ONE);
	glDepthMask (GL_FALSE);
	glDrawArrays (GL_TRIANGLES, 0, flare_n);
	glDepthMask (GL_TRUE);
	glDisable (GL_BLEND);
}

/* sparks: motes of light that drift and go; pieces of a broken wall, that fall */
typedef struct
{
	float p[3], v[3], age, life, size, c[3];
	bool chunk;
	float spin;
} mote_t;

#define MOTES		40
static mote_t motes[MOTES];

static void mote (const float *p, float spread, float vx, float vy, float vz, float life, float size, float red, float green,
		  float blue, bool chunk)
{
	mote_t *m = &motes[0];
	for (int i = 0; i < MOTES; i++)
	{
		if (motes[i].age >= motes[i].life)
		{
			m = &motes[i];
			break;
		}
		if (motes[i].age / motes[i].life > m->age / m->life)
		{
			m = &motes[i];
		}
	}
	vset (m->p, p[0] + (rnd () - 0.5f) * spread, p[1] + (rnd () - 0.5f) * spread, p[2] + (rnd () - 0.5f) * spread);
	vset (m->v, vx, vy, vz);
	m->age = 0.0f;
	m->life = life * (0.7f + 0.6f * rnd ());
	m->size = size;
	vset (m->c, red, green, blue);
	m->chunk = chunk;
	m->spin = rnd () * 6.0f;
}

static void burst (const float *p, int n, float spread, float speed, float up, float life, float size, float red, float green, float blue)
{
	for (int i = 0; i < n; i++)
	{
		float a = rnd () * 2 * PI;
		mote (p, spread, cosf (a) * speed * rnd (), sinf (a) * speed * rnd (), up * (0.5f + rnd ()), life, size, red, green, blue, false);
	}
}

static void motes_update (float dt)
{
	for (int i = 0; i < MOTES; i++)
	{
		mote_t *m = &motes[i];
		if (m->age >= m->life)
		{
			continue;
		}
		m->age += dt;
		if (m->chunk)
		{
			m->v[2] -= 700.0f * dt;
		}
		for (int k = 0; k < 3; k++)
		{
			m->p[k] += m->v[k] * dt;
		}
		if (!m->chunk)
		{
			float k = 1.0f - m->age / m->life;
			flare (m->p, m->size * (0.5f + 0.5f * k), m->c[0] * k, m->c[1] * k, m->c[2] * k);
		}
	}
}

/* ---- the picture's lights and the level's programs ------------------------------------------------ */

static struct { GLint lantern, gold, fire, fire_gold, fog, storm, shown; } wu[3];
static float storm, storm_next = 6.0f, thunder_at = -1.0f, dawn;

static void world_uniforms (void)
{
	for (int k = 0; k < 3; k++)
	{
		GLuint p = r.programs[k].id;
		wu[k].lantern = glGetUniformLocation (p, "u_lantern");
		wu[k].gold = glGetUniformLocation (p, "u_gold");
		wu[k].fire = glGetUniformLocation (p, "u_fire");
		wu[k].fire_gold = glGetUniformLocation (p, "u_fire_gold");
		wu[k].fog = glGetUniformLocation (p, "u_fog");
		wu[k].storm = glGetUniformLocation (p, "u_storm");
		wu[k].shown = glGetUniformLocation (p, "u_shown");
	}
	glUseProgram (r.programs[0].id);
	glUniform1f (wu[0].shown, 1.0f);
}

/* the lantern, the nearest fire that burns, the fog, the storm: into every program */
static void lights_update (const float lantern[3], float dt)
{
	float flicker = 1.0f + 0.04f * sinf (t_now * 13.0f) + 0.03f * sinf (t_now * 7.3f);
	float reach = lf_glow (&s) * flicker, strength = (0.50f + 0.80f * s.raised) * (s.ember > 0.0f ? 1.0f : 0.45f) + 1.6f * s.flash;
	reach *= 1.0f + 0.5f * s.flash;
	if (s.out > 0.0f)
	{
		strength *= clampf (s.out - 0.8f, 0.0f, 1.0f);
	}
	vcopy (lights.lantern, lantern);
	lights.lantern[3] = 1.0f / (reach * reach);
	vset (lights.gold, 1.0f * strength, 0.72f * strength, 0.36f * strength);

	/* the fires: the shrines, the beacon lit, the great lantern relit */
	float best = 1e9f, fire[3] = {0, 0, 0}, fire_reach = 1.0f, fire_strength = 0.0f;
	for (int i = 0; i < s.n_shrines; i++)
	{
		float d = dist3 (s.shrine[i], s.g.origin);
		if (d < best)
		{
			best = d;
			vset (fire, s.shrine[i][0], s.shrine[i][1], s.shrine[i][2] + 48.0f);
			fire_reach = 420.0f;
			fire_strength = 0.9f;
		}
	}
	if (s.has_beacon && (s.beacon_lit || s.kindle > 0.0f) && dist3 (s.beacon, s.g.origin) < best + 500.0f)
	{
		vset (fire, s.beacon[0], s.beacon[1], s.beacon[2] + 80.0f);
		fire_reach = 1000.0f;
		fire_strength = s.beacon_lit ? 1.25f : 0.8f * s.kindle;
	}
	if (s.ending == 1)
	{
		vset (fire, s.choice[0][0], s.choice[0][1], s.choice[0][2] + 60.0f);
		fire_reach = 2600.0f;
		fire_strength = 1.7f;
	}
	fire_strength *= 1.0f + 0.06f * sinf (t_now * 9.0f) + 0.04f * sinf (t_now * 15.7f);
	vcopy (lights.fire, fire);
	lights.fire[3] = 1.0f / (fire_reach * fire_reach);
	vset (lights.fire_gold, 1.0f * fire_strength, 0.70f * fire_strength, 0.34f * fire_strength);

	/* the storm: lightning now and then, its thunder after */
	storm *= expf (-dt * 5.0f);
	if (airs[s.level].storm && !s.ending && t_now >= storm_next)
	{
		storm = 0.5f + 0.4f * rnd ();
		storm_next = t_now + 4.0f + 9.0f * rnd ();
		thunder_at = t_now + 0.4f + 1.2f * rnd ();
	}
	if (thunder_at >= 0.0f && t_now >= thunder_at)
	{
		sfx (LSND_THUNDER, NULL, SOUND_ANY, 0.55f, 0.0f, 0.8f + 0.4f * rnd ());
		thunder_at = -1.0f;
	}
	lights.storm = storm;

	/* the fog: the scene's; after the lantern is relit, the dawn's */
	dawn = clampf (dawn + (s.ending == 1 ? dt / 6.0f : -dt), 0.0f, 1.0f);
	static const float gold_air[3] = {0.62f, 0.48f, 0.28f};
	for (int k = 0; k < 3; k++)
	{
		lights.fog[k] = airs[s.level].fog[k] + (gold_air[k] - airs[s.level].fog[k]) * dawn + 0.5f * storm * airs[s.level].fog[k];
	}
	lights.fog[3] = 1.0f / (airs[s.level].far * (1.0f + 0.6f * dawn));
	for (int k = 0; k < 3; k++)
	{
		glUseProgram (r.programs[k].id);
		glUniform4fv (wu[k].lantern, 1, lights.lantern);
		glUniform3fv (wu[k].gold, 1, lights.gold);
		glUniform4fv (wu[k].fire, 1, lights.fire);
		glUniform3fv (wu[k].fire_gold, 1, lights.fire_gold);
		glUniform4fv (wu[k].fog, 1, lights.fog);
		glUniform1f (wu[k].storm, lights.storm);
	}
	glClearColor (lights.fog[0], lights.fog[1], lights.fog[2], 1.0f);
}

/* ---- the figures -------------------------------------------------------------------------- */

static float planes[5][4];		/* the view's sides and its near plane: inside is positive */

static void planes_from (const float vp[16])
{
	static const int row[5] = {0, 0, 1, 1, 2};
	static const float sign[5] = {1, -1, 1, -1, 1};
	for (int k = 0; k < 5; k++)
	{
		float *p = planes[k], l;
		for (int c = 0; c < 4; c++)
		{
			p[c] = vp[c * 4 + 3] + sign[k] * vp[c * 4 + row[k]];
		}
		l = sqrtf (p[0] * p[0] + p[1] * p[1] + p[2] * p[2]);
		for (int c = 0; c < 4; c++)
		{
			p[c] /= l;
		}
	}
}

static bool seen (const float *c, float radius)
{
	if (dist3 (c, cam_eye) > 1.0f / lights.fog[3] + radius)
	{
		return false;				/* (in the fog) */
	}
	for (int k = 0; k < 5; k++)
	{
		if (planes[k][0] * c[0] + planes[k][1] * c[1] + planes[k][2] * c[2] + planes[k][3] < -radius)
		{
			return false;
		}
	}
	return true;
}

/* where a figure stands, turned to yaw */
static void placed (float m[16], const float *p, float yaw)
{
	mat4_identity (m);
	mat4_translate (m, p[0], p[1], p[2]);
	mat4_rotate (m, 2, yaw * 180.0f / PI);
}

/* a part of it: from a joint (in the figure), turned about the axis across by an angle */
static void jointed (float m[16], const float base[16], float x, float y, float z, float angle)
{
	memcpy (m, base, 16 * sizeof (float));
	mat4_translate (m, x, y, z);
	mat4_rotate (m, 1, angle * 180.0f / PI);
}

/* the level's light where each figure stands: looked up now and then, in turn */
#define AMBIENTS	(4 + LF_MAX_HOLLOW)
static float ambient[AMBIENTS][3];
static unsigned ambient_frame;

static const float *ambient_of (int who, const float *p)
{
	if ((ambient_frame + who * 3) % 12 == 0 || ambient[who][0] < 0.0f)
	{
		light_at (&bsp, p, 0.0f, ambient[who]);
	}
	static float out[3];
	for (int k = 0; k < 3; k++)
	{
		out[k] = ambient[who][k] + lights.storm * 0.6f;
	}
	return out;
}

static float stride;			/* how far Wren has walked (her legs) */
static float hand[3];			/* where her lantern hangs from */

static void draw_wren (bool show)
{
	float base[16], m[16], speed = sqrtf (s.g.velocity[0] * s.g.velocity[0] + s.g.velocity[1] * s.g.velocity[1]);
	float swing = s.g.on_ground ? 0.75f * clampf (speed / 300.0f, 0.0f, 1.0f) * sinf (stride * 0.075f) : 0.5f;
	float p[3] = {s.g.origin[0], s.g.origin[1], s.g.origin[2] + (s.g.on_ground ? 0.6f * sinf (t_now * 2.2f) : 0.0f)};
	const float *amb = ambient_of (0, s.g.origin);
	const float hurt[3] = {1.8f, 0.8f, 0.7f};
	const float *tint = s.hurt > 0.0f && ((int) (t_now * 14.0f) & 1) ? hurt : NULL;
	placed (base, p, s.g.yaw);
	jointed (m, base, 1.0f, -9.5f, 11.0f, -(0.3f + 2.0f * s.raised) - 0.3f * swing);	/* the arm with the lantern */
	for (int k = 0; k < 3; k++)
	{
		hand[k] = m[8 + k] * -15.0f + m[12 + k];
	}
	if (!show)
	{
		return;
	}
	figures_draw (&fig, M_WREN_ARM, m, amb, tint, NULL, 0.0f);
	figures_draw (&fig, M_WREN_BODY, base, amb, tint, NULL, 0.0f);
	figures_draw (&fig, M_WREN_HEAD, base, amb, tint, NULL, 0.0f);
	jointed (m, base, 1.0f, 9.5f, 11.0f, 0.7f * swing);
	figures_draw (&fig, M_WREN_ARM, m, amb, tint, NULL, 0.0f);
	for (int side = -1; side <= 1; side += 2)
	{
		jointed (m, base, 0.0f, side * 4.0f, -8.0f, side * swing);
		figures_draw (&fig, M_WREN_LEG, m, amb, tint, NULL, 0.0f);
	}
	placed (m, hand, s.g.yaw);
	const float dim[3] = {0.35f, 0.3f, 0.3f};
	figures_draw (&fig, M_LANTERN, m, amb, s.ember > 0.0f ? NULL : dim, NULL, 0.0f);
}

static void draw_ody (void)
{
	const lf_ody_t *o = &s.ody;
	if (o->state == LF_ODY_ABSENT || !seen (o->p, 80.0f))
	{
		return;
	}
	float base[16], m[16], p[3] = {o->p[0], o->p[1], o->p[2]};
	bool asleep = o->state == LF_ODY_ASLEEP, low = asleep || o->state == LF_ODY_KNEEL;
	float speed = sqrtf (o->v[0] * o->v[0] + o->v[1] * o->v[1]), swing = 0.5f * clampf (speed / 200.0f, 0.0f, 1.0f) * sinf (o->stride * 0.045f);
	const float *amb = ambient_of (1, o->p);
	p[2] -= low ? 12.0f : 0.0f;
	placed (base, p, o->yaw);
	if (low)
	{
		mat4_rotate (base, 1, asleep ? 14.0f : 8.0f);		/* bowed */
	}
	figures_draw (&fig, M_ODY_TORSO, base, amb, NULL, NULL, 0.0f);
	figures_draw (&fig, M_ODY_HEAD, base, amb, NULL, NULL, 0.0f);
	if (!asleep)
	{
		const float waking[3] = {o->t < 1.0f ? o->t : 1.0f, o->t < 1.0f ? o->t : 1.0f, o->t < 1.0f ? o->t : 1.0f};
		figures_draw (&fig, M_ODY_EYE, base, amb, waking, NULL, 0.0f);
	}
	for (int side = -1; side <= 1; side += 2)
	{
		jointed (m, base, 0.0f, side * 14.0f, 6.0f + (low ? 10.0f : 0.0f), side * swing + (low ? -1.0f : 0.0f));
		figures_draw (&fig, M_ODY_LEG, m, amb, NULL, NULL, 0.0f);
	}
	/* his arm: hanging; up and back, then down (a blow); held out low (a step for Wren) */
	float arm = -0.4f * swing;
	if (o->state == LF_ODY_SWING)
	{
		arm = o->swing < 0.6f ? -2.9f * o->swing / 0.6f : -2.9f + 2.1f * (o->swing - 0.6f) / 0.4f;
	}
	else if (o->state == LF_ODY_KNEEL)
	{
		arm = -1.0f - 1.3f * o->swing;
	}
	jointed (m, base, 0.0f, -37.0f, 40.0f, arm);
	figures_draw (&fig, M_ODY_ARM, m, amb, NULL, NULL, 0.0f);
	jointed (m, base, 0.0f, 35.0f, 40.0f, 0.3f * swing);
	figures_draw (&fig, M_ODY_STUMP, m, amb, NULL, NULL, 0.0f);
}

static void draw_hollow (int i)
{
	const lf_hollow_t *h = &s.hollow[i];
	if (h->state == LF_H_GONE || !seen (h->p, 90.0f))
	{
		return;
	}
	float m[16], rim[3] = {0.10f, 0.24f, 0.27f}, wobble = 1.6f;
	const float *amb = ambient_of (4 + i, h->p);
	bool weak = h->state == LF_H_STUN || h->state == LF_H_RECOVER;
	float gone = h->state == LF_H_DYING ? clampf (1.0f - h->t / 0.9f, 0.0f, 1.0f) : 1.0f;
	if (h->burn > 0.0f || weak)			/* burning, or reeling: its edges lit */
	{
		float k = weak ? 0.8f : h->burn;
		vset (rim, 0.10f + 0.9f * k, 0.24f + 0.6f * k, 0.27f + 0.25f * k);
		wobble = 3.0f;
	}
	placed (m, h->p, h->yaw);
	if (h->kind == LF_LURKER)
	{
		if (h->state == LF_H_ASLEEP)		/* a puddle on the floor */
		{
			mat4_translate (m, 0, 0, -22.0f);
			mat4_scale (m, 1.5f, 1.5f, 0.1f);
			figures_draw (&fig, M_BLOB, m, amb, NULL, rim, 0.5f);
			return;
		}
		float k = gone * (1.0f - 0.35f * h->burn), up = 1.0f + 0.12f * sinf (t_now * 5.0f + i);
		mat4_translate (m, 0, 0, -24.0f + 15.0f * k * up);
		mat4_scale (m, k * (2.0f - up), k * (2.0f - up), k * up);
		figures_draw (&fig, M_BLOB, m, amb, NULL, rim, wobble);
		figures_draw (&fig, M_EYES, m, amb, NULL, NULL, 0.0f);
	}
	else if (h->kind == LF_BRUTE)
	{
		if (h->state == LF_H_ASLEEP)
		{
			mat4_translate (m, 0, 0, -21.0f);
			mat4_scale (m, 3.2f, 3.2f, 0.2f);
			figures_draw (&fig, M_BLOB, m, amb, NULL, rim, 0.5f);
			return;
		}
		float tall = 1.0f, along = 1.0f, back = 0.0f;
		if (h->state == LF_H_WINDUP)		/* rearing */
		{
			float k = clampf (h->t / 0.5f, 0.0f, 1.0f);
			tall = 1.0f + 0.35f * k;
			back = -22.0f * k;
		}
		else if (h->state == LF_H_LUNGE)
		{
			along = 1.35f;
			tall = 0.85f;
		}
		else if (weak)
		{
			tall = 0.7f;
		}
		float k = 2.4f * gone;
		mat4_translate (m, 0, 0, -24.0f + 15.0f * k * tall);
		mat4_rotate (m, 1, back);
		mat4_scale (m, k * along, k / sqrtf (along), k * tall);
		figures_draw (&fig, M_BLOB, m, amb, NULL, rim, wobble * 0.5f);
		figures_draw (&fig, M_EYES, m, amb, NULL, NULL, 0.0f);
		figures_draw (&fig, M_HORNS, m, amb, NULL, rim, 0.0f);
	}
	else						/* a listener: swaying; bent to her when it has heard */
	{
		bool roused = h->state != LF_H_ASLEEP && h->state != LF_H_BACK;
		mat4_rotate (m, 1, roused ? (h->state == LF_H_STUN ? -25.0f : 18.0f) : 3.0f * sinf (t_now * 0.9f + i));
		const float pale[3] = {0.5f, 0.45f, 0.35f};
		figures_draw (&fig, M_LISTENER, m, amb, NULL, roused ? pale : rim, roused ? 1.5f : 0.4f);
	}
}

static void draw_figures (const float vp[16], bool wren_shown)
{
	float m[16];
	figures_begin (&fig, vp, cam_eye, &lights, t_now);
	draw_wren (wren_shown);
	draw_ody ();
	for (int i = 0; i < s.n_hollow; i++)
	{
		draw_hollow (i);
	}

	/* the fires: the shrines (lit), the beacon (its coals with its kindling), their flames */
	for (int i = 0; i <= s.n_shrines; i++)
	{
		bool beacon = i == s.n_shrines;
		const float *p = beacon ? s.beacon : s.shrine[i];
		float heat = beacon ? (s.beacon_lit ? 1.0f : s.kindle) : 1.0f, k = beacon ? 1.7f : 0.9f;
		if ((beacon && !s.has_beacon) || !seen (p, 120.0f))
		{
			continue;
		}
		const float *amb = ambient_of (2, p);
		placed (m, p, 0.0f);
		mat4_scale (m, k, k, k);
		figures_draw (&fig, M_BRAZIER, m, amb, NULL, NULL, 0.0f);
		if (heat > 0.0f)
		{
			const float glow[3] = {heat, heat, heat};
			figures_draw (&fig, M_COALS, m, amb, glow, NULL, 0.0f);
			for (int j = 0; j < 3; j++)
			{
				float f = 0.5f + 0.5f * sinf (t_now * (9.0f + 3.0f * j) + i * 2.0f + j);
				float at[3] = {p[0] + 3.0f * sinf (t_now * 5.0f + j * 2.1f), p[1] + 3.0f * cosf (t_now * 4.3f + j), p[2] + k * (46.0f + 13.0f * j + 5.0f * f)};
				flare (at, k * (17.0f - 3.5f * j) * (0.8f + 0.3f * f) * heat, 0.9f * heat, (0.5f - 0.1f * j) * heat, 0.14f * heat);
			}
			if (rnd () < 0.06f * heat)
			{
				float at[3] = {p[0], p[1], p[2] + k * 50.0f};
				mote (at, 12.0f * k, 0, 0, 40.0f, 1.6f, 4.0f, 1.0f, 0.6f, 0.2f, false);
			}
		}
	}

	/* embers lying about; the Tide King and his bolts; the Archivist; pieces of walls */
	for (int i = 0; i < s.g.n_items; i++)
	{
		const game_item_t *it = &s.g.items[i];
		if (!it->taken && seen (it->origin, 30.0f))
		{
			float at[3] = {it->origin[0], it->origin[1], it->origin[2] + 3.0f * sinf (t_now * 2.0f + i)};
			placed (m, at, t_now * 1.5f + i);
			figures_draw (&fig, M_EMBER, m, ambient[0], NULL, NULL, 0.0f);
			flare (at, 13.0f + 2.0f * sinf (t_now * 6.0f + i), 0.55f, 0.36f, 0.12f);
		}
	}
	if (s.king.here && seen (s.king.p, 120.0f))
	{
		const lf_king_t *k = &s.king;
		float rim[3] = {0.15f + k->burn, 0.35f + 0.5f * k->burn, 0.42f}, p[3] = {k->p[0], k->p[1], k->p[2] + 10.0f};
		placed (m, p, atan2f (s.g.origin[1] - k->p[1], s.g.origin[0] - k->p[0]));
		mat4_scale (m, 1.5f, 1.5f, 1.5f);
		if (k->down || k->beaten)
		{
			mat4_rotate (m, 1, 28.0f);
		}
		figures_draw (&fig, M_KING, m, ambient_of (3, k->p), NULL, rim, k->down ? 1.5f : 0.6f);
		if (!k->beaten)
		{
			figures_draw (&fig, M_KING_EYES, m, ambient[3], NULL, NULL, 0.0f);
		}
	}
	for (int i = 0; i < LF_MAX_BOLTS; i++)
	{
		if (s.bolt[i].live && seen (s.bolt[i].p, 40.0f))
		{
			const float rim[3] = {0.25f, 0.6f, 0.7f};
			placed (m, s.bolt[i].p, t_now * 6.0f);
			mat4_scale (m, 0.9f, 0.9f, 0.9f);
			figures_draw (&fig, M_BLOB, m, ambient[3], NULL, rim, 3.0f);
		}
	}
	if (s.has_archivist && seen (s.archivist, 60.0f))
	{
		placed (m, s.archivist, s.archivist_yaw);
		figures_draw (&fig, M_ARCHIVIST, m, ambient_of (3, s.archivist), NULL, NULL, 0.0f);
	}
	for (int i = 0; i < MOTES; i++)
	{
		const mote_t *c = &motes[i];
		if (c->chunk && c->age < c->life)
		{
			placed (m, c->p, c->spin + c->age * 5.0f);
			mat4_rotate (m, 1, c->age * 300.0f);
			mat4_scale (m, c->size, c->size, c->size);
			figures_draw (&fig, M_CHUNK, m, ambient[1], NULL, NULL, 0.0f);
		}
	}

	/* the lantern's own light; the peak's fires */
	float core[3] = {hand[0], hand[1], hand[2] - 7.0f}, glow = s.ember > 0.0f ? 0.5f + 0.5f * s.raised + s.flash : 0.12f;
	if (wren_shown)
	{
		flare (core, 9.0f + 9.0f * s.raised + 30.0f * s.flash, 0.75f * glow, 0.5f * glow, 0.18f * glow);
	}
	if (s.raised > 0.6f && s.ember > 0.0f && rnd () < 0.12f)
	{
		mote (core, 8.0f, (rnd () - 0.5f) * 20.0f, (rnd () - 0.5f) * 20.0f, 26.0f, 1.2f, 3.0f, 1.0f, 0.65f, 0.25f, false);
	}
	if (s.has_choice)
	{
		for (int k = 0; k < 2; k++)
		{
			float heat = s.ending == k + 1 ? 1.0f : s.choosing[k];
			if (heat > 0.0f && !(s.ending == 2 && k == 1))
			{
				float at[3] = {s.choice[k][0], s.choice[k][1], s.choice[k][2] + 30.0f};
				flare (at, 40.0f * heat * (0.9f + 0.1f * sinf (t_now * 11.0f)), heat, 0.62f * heat, 0.2f * heat);
			}
		}
	}
}

/* ---- what happened: seen and heard ------------------------------------------------------------------- */

static float shake;

static void events (void)
{
	for (int i = 0; i < s.n_events; i++)
	{
		const float *p = s.event[i].p;
		switch (s.event[i].type)
		{
		case LF_EV_REVEAL:
			sfx (LSND_REVEAL, p, SOUND_ANY, 0.8f, 0.5f, 1.0f);
			burst (p, 10, 120.0f, 30.0f, 40.0f, 1.6f, 5.0f, 1.0f, 0.75f, 0.3f);
			break;
		case LF_EV_BEACON:
			sfx (LSND_BEACON, p, SOUND_ANY, 1.0f, 0.2f, 1.0f);
			burst (p, 14, 30.0f, 60.0f, 150.0f, 2.0f, 6.0f, 1.0f, 0.6f, 0.2f);
			music.bell = t_now + 1.5f;
			break;
		case LF_EV_SHRINE:	sfx (LSND_SHRINE, p, SOUND_ANY, 0.7f, 0.5f, 1.0f); break;
		case LF_EV_EMBER:
			sound_play (&snd, SND_ITEM, NULL, SOUND_ANY, 0.6f, 0.0f);
			burst (p, 5, 6.0f, 30.0f, 50.0f, 0.8f, 4.0f, 1.0f, 0.7f, 0.3f);
			break;
		case LF_EV_FLARE:
			sfx (LSND_FLARE, NULL, OWNER_LANTERN, 0.9f, 0.0f, 1.0f);
			burst (hand, 12, 4.0f, 220.0f, 60.0f, 0.7f, 5.0f, 1.0f, 0.75f, 0.35f);
			break;
		case LF_EV_HURT:
			sfx (LSND_HURT, NULL, SOUND_PLAYER, 0.8f, 0.0f, 1.0f);
			shake = 6.0f;
			break;
		case LF_EV_OUT:		sfx (LSND_OUT, NULL, SOUND_PLAYER, 0.9f, 0.0f, 1.0f); break;
		case LF_EV_SEA:		sound_play (&snd, SND_SPLASH, NULL, SOUND_PLAYER, 0.9f, 0.0f); break;
		case LF_EV_SPENT:	sfx (LSND_RAISE, NULL, OWNER_LANTERN, 0.3f, 0.0f, 0.5f); break;
		case LF_EV_ODY_WAKE:
			sfx (LSND_SMASH, p, OWNER_ODY, 0.5f, 0.5f, 0.6f);
			burst (p, 8, 50.0f, 40.0f, 60.0f, 1.2f, 4.0f, 0.5f, 0.45f, 0.35f);
			break;
		case LF_EV_ODY_STEP:	sfx (LSND_STONE, p, OWNER_ODY, 0.5f, 1.0f, 0.9f + 0.2f * rnd ()); break;
		case LF_EV_ODY_SWING:	sfx (LSND_ROAR, p, OWNER_ODY, 0.3f, 1.0f, 1.6f); break;
		case LF_EV_ODY_SMASH:
			sfx (LSND_SMASH, p, SOUND_ANY, 1.0f, 0.4f, 1.0f);
			shake = 8.0f;
			for (int k = 0; k < 10; k++)
			{
				mote (p, 70.0f, (rnd () - 0.5f) * 300.0f, (rnd () - 0.5f) * 300.0f, 120.0f + 200.0f * rnd (), 1.2f, 1.0f + 1.6f * rnd (),
				      0, 0, 0, true);
			}
			break;
		case LF_EV_ODY_KNEEL:	sfx (LSND_STONE, p, OWNER_ODY, 0.7f, 1.0f, 0.6f); break;
		case LF_EV_ODY_THROW:	sound_play (&snd, SND_PUSH, p, SOUND_ANY, 0.6f, 1.0f); break;
		case LF_EV_ODY_COME:
			sfx (LSND_SMASH, p, OWNER_ODY, 0.4f, 0.7f, 0.7f);
			burst (p, 8, 60.0f, 50.0f, 50.0f, 1.0f, 4.0f, 0.4f, 0.4f, 0.35f);
			break;
		case LF_EV_HOLLOW_WAKE:	sfx (LSND_HOLLOW, p, SOUND_ANY, 0.8f, 0.7f, 0.9f + 0.2f * rnd ()); break;
		case LF_EV_HOLLOW_DIE:
			sfx (LSND_HOLLOW_DIE, p, SOUND_ANY, 0.8f, 0.7f, 1.0f);
			burst (p, 9, 20.0f, 50.0f, 90.0f, 1.0f, 5.0f, 0.45f, 0.75f, 0.8f);
			break;
		case LF_EV_WINDUP:	sfx (LSND_ROAR, p, SOUND_ANY, 0.9f, 0.5f, 1.0f); break;
		case LF_EV_LUNGE:	sfx (LSND_BOLT, p, SOUND_ANY, 0.8f, 0.6f, 0.7f); break;
		case LF_EV_THUD:
			sfx (LSND_THUD, p, SOUND_ANY, 1.0f, 0.5f, 1.0f);
			shake = 5.0f;
			break;
		case LF_EV_BRUTE_HURT:	sfx (LSND_HOLLOW_DIE, p, SOUND_ANY, 0.9f, 0.5f, 0.6f); break;
		case LF_EV_SHRIEK:	sfx (LSND_SHRIEK, p, SOUND_ANY, 0.9f, 0.4f, 1.0f); break;
		case LF_EV_GATE:	sound_play (&snd, SND_DOOR_STOP, p, SOUND_ANY, 1.0f, 0.4f); break;
		case LF_EV_PLATE:	sound_play (&snd, SND_PLAT_STOP, p, SOUND_ANY, 0.9f, 0.6f); break;
		case LF_EV_BOLT:	sfx (LSND_BOLT, p, SOUND_ANY, 0.8f, 0.4f, 1.0f); break;
		case LF_EV_KING_FALL:	sfx (LSND_KING, p, SOUND_ANY, 0.8f, 0.2f, 0.8f); break;
		case LF_EV_KING_HURT:
			sfx (LSND_KING, p, SOUND_ANY, 1.0f, 0.2f, 1.0f);
			burst (p, 14, 40.0f, 120.0f, 120.0f, 1.4f, 6.0f, 1.0f, 0.7f, 0.3f);
			shake = 8.0f;
			break;
		case LF_EV_KING_BEATEN:	sfx (LSND_OUT, p, SOUND_ANY, 0.8f, 0.0f, 0.6f); break;
		case LF_EV_ENDING:
			sfx (LSND_BEACON, NULL, SOUND_ANY, 1.0f, 0.0f, s.ending == 1 ? 0.8f : 0.5f);
			music.next = t_now + 3.0f;
			music.bell = t_now + 1.0f;
			break;
		case LF_EV_CAPTION:
		{
			const lf_line_t *l = lf_caption (&s);
			if (l && l->who == LF_WHO_ODY)
				sfx (LSND_ODY, s.ody.p, OWNER_ODY, 0.5f, 0.4f, 1.0f);
			else if (l && l->who == LF_WHO_KING)
				sfx (LSND_ODY, s.king.p, SOUND_ANY, 0.6f, 0.2f, 0.7f);
			break;
		}
		}
	}
}

/* ---- the stick and the keys -------------------------------------------------------------------- */

static struct
{
	bool pad, bot, paused, smooth, numbers, soft, lamp, near;
	unsigned was_buttons;
	float idle;
	float forward_until, back_until, left_until, right_until, strafe_l_until, strafe_r_until, jump_until;
	bool start, next, again;		/* pressed since */
} ui = {.smooth = true};

static void controls (lf_input_t *in, float t, float dt)
{
	bool touched = false, flare_key = false, ody_key = false;
	memset (in, 0, sizeof *in);
	static int escape;
	for (int c; (c = getchar_timeout_us (0)) != PICO_ERROR_TIMEOUT; )
	{
		bool play = true;
		if (escape == 1)
		{
			escape = c == '[' ? 2 : 0;
			continue;
		}
		if (escape == 2)
		{
			c = c == 'A' ? 'w' : c == 'B' ? 's' : c == 'C' ? 'd' : c == 'D' ? 'a' : 0;
			escape = 0;
		}
		else if (c == 27)
		{
			escape = 1;
			continue;
		}
		switch (c)
		{
		case 'w': case 'W': ui.forward_until = t + KEY_HOLD_S; break;
		case 's': case 'S': ui.back_until = t + KEY_HOLD_S; break;
		case 'a': case 'A': ui.left_until = t + KEY_HOLD_S; break;
		case 'd': case 'D': ui.right_until = t + KEY_HOLD_S; break;
		case 'q': case 'Q': ui.strafe_l_until = t + KEY_HOLD_S; break;
		case 'e': case 'E': ui.strafe_r_until = t + KEY_HOLD_S; break;
		case ' ': ui.jump_until = t + 0.15f; break;
		case 'l': case 'L': ui.lamp = !ui.lamp; break;
		case 'f': case 'F': flare_key = true; break;
		case 'y': case 'Y': ody_key = true; break;
		case 'c': case 'C': ui.soft = !ui.soft; break;
		case '\r': case '\n': ui.start = true; break;
		case 'v': case 'V': ui.near = !ui.near; play = false; break;
		case 'p': case 'P': ui.paused = !ui.paused; play = false; break;
		case 'b': case 'B': ui.bot = !ui.bot; play = false; printf ("lanternfall: %s\n", ui.bot ? "playing itself" : "yours"); break;
		case 'n': case 'N': ui.next = true; play = false; break;
		case 'r': case 'R': ui.again = true; play = false; break;
		case 'h': case 'H': ui.numbers = !ui.numbers; play = false; break;
		case 'm': case 'M':
			ui.smooth = !ui.smooth;
			printf ("lanternfall: %s\n", ui.smooth ? "antialiased" : "not antialiased");
			play = false;
			break;
		default: play = false;
		}
		touched = touched || play;
	}
	float pace = ui.soft ? 0.5f : 1.0f;
	in->move.forward = pace * ((t < ui.forward_until) - (t < ui.back_until));
	in->move.side = pace * ((t < ui.strafe_r_until) - (t < ui.strafe_l_until));
	in->move.turn = KEYS_TURN * ((t < ui.left_until) - (t < ui.right_until));
	in->move.jump = t < ui.jump_until;
	in->raise = ui.lamp;
	in->flare = flare_key;
	in->ody = ody_key;

	pad_t pad;
	if (ui.pad && pad_read (&pad))
	{
		unsigned pressed = pad.buttons & ~ui.was_buttons;
		ui.was_buttons = pad.buttons;
		touched = touched || pad.buttons || fabsf (pad.x) > 0.25f || fabsf (pad.y) > 0.25f;
		in->move.forward = clampf (in->move.forward + pad.y, -1.0f, 1.0f);
		in->move.turn -= KEYS_TURN * pad.x * (0.6f + 0.4f * fabsf (pad.x));
		in->move.jump = in->move.jump || (pad.buttons & (PAD_B | PAD_STICK));
		in->raise = in->raise || (pad.buttons & PAD_A);
		in->flare = in->flare || (pressed & PAD_X);
		in->ody = in->ody || (pressed & PAD_Y);
		if (pressed & PAD_SELECT)
		{
			ui.near = !ui.near;
		}
		ui.start = ui.start || (pressed & (PAD_START | PAD_B | PAD_A));
		if ((pressed & PAD_START) && !ui.bot)
		{
			ui.paused = !ui.paused;
		}
	}
	ui.idle = touched ? 0.0f : ui.idle + dt;
	if (touched && ui.bot)				/* the stick touched: hers again (from the start) */
	{
		ui.bot = false;
		ui.again = true;
		ui.start = false;
	}
}

/* ---- the HUD ------------------------------------------------------------------------------------ */

enum { MODE_TITLE, MODE_PLAY, MODE_LEAVE, MODE_END };
static int mode = MODE_TITLE;
static float mode_s, card_t = 1e9f, fade;		/* seconds in the mode; since the scene's card; the dark over the picture */

static void centred (float y, const char *text, uint32_t colour, float scale, int width)
{
	hud_text_scaled (floorf ((width - strlen (text) * HUD_CHAR_W * scale) / 2), floorf (y), text, colour, scale);
}

/* a line of the story: who says it, and it in rows that fit the screen */
static void caption (const lf_line_t *l, const GLint vp[4], float hs)
{
	static const char *const who[] = {"", "WREN", "ODY", "THE ARCHIVIST", "THE TIDE KING"};
	static const uint32_t colour[] =
	{
		HUD_RGBA (215, 205, 180, 255), HUD_RGBA (235, 150, 80, 255), HUD_RGBA (170, 205, 200, 255),
		HUD_RGBA (225, 215, 190, 255), HUD_RGBA (110, 200, 220, 255)
	};
	int room = (int) ((vp[2] - 16) / (HUD_CHAR_W * hs)), rows = 0;
	char row[4][96];
	const char *p = l->text;
	room = room > 95 ? 95 : room;
	while (*p && rows < 4)
	{
		int n = (int) strlen (p);
		if (n > room)
		{
			for (n = room; n > 0 && p[n] != ' '; n--)
			{
			}
			n = n ? n : room;
		}
		memcpy (row[rows], p, (size_t) n);
		row[rows++][n] = '\0';
		p += n;
		p += *p == ' ';
	}
	float lh = HUD_CHAR_H * hs * 1.25f, y = vp[3] - 6 * hs - rows * lh;	/* (at the bottom, under her feet) */
	if (l->who)
	{
		centred (y - lh, who[l->who], colour[l->who], hs, vp[2]);
	}
	for (int k = 0; k < rows; k++)
	{
		centred (y + k * lh, row[k], l->who ? HUD_RGBA (235, 228, 210, 255) : colour[0], hs, vp[2]);
	}
}

static void hud_build (const GLint vp[4], const perf_t *perf)
{
	static const char *const numerals[LF_LEVELS] = {"I", "II", "III", "IV", "V", "VI"};
	static const char *const asks[] = {"", "Y: ODY, COME", "Y: ODY, BREAK IT", "Y: ODY, LIFT ME", "Y: ODY, STAND THERE", "Y: ODY, STRIKE"};
	float hs = vp[3] >= 1000 ? 1.5f : vp[3] >= 600 ? 1.0f : 0.5f, lh = HUD_CHAR_H * hs * 1.25f;
	char text[64];
	hud_begin ();
	if (fade > 0.01f)
	{
		hud_rect (0, 0, vp[2], vp[3], HUD_RGBA (0, 0, 0, (int) (clampf (fade, 0.0f, 1.0f) * 255)));
	}
	if (mode == MODE_TITLE)
	{
		centred (vp[3] * 0.24f, "LANTERNFALL", HUD_RGBA (255, 200, 110, 255), 4 * hs, vp[2]);
		if ((int) (t_now * 1.5f) & 1)
		{
			centred (vp[3] * 0.24f + 5.5f * lh, ui.pad ? "PRESS B" : "PRESS ENTER", HUD_RGBA (225, 215, 190, 255), hs, vp[2]);
		}
	}
	else if (mode == MODE_END && !s.n_captions)
	{
		centred (vp[3] * 0.30f, "LANTERNFALL", HUD_RGBA (255, 200, 110, 255), 4 * hs, vp[2]);
		centred (vp[3] * 0.30f + 5.5f * lh, s.ending == 1 ? "THE LANTERN RELIT" : "THE DARK KEPT", HUD_RGBA (225, 215, 190, 255), hs, vp[2]);
	}
	else
	{
		/* the ember: a bar, gold; pale when it's spent */
		float w = 110 * hs, x = 6 * hs, y = 8 * hs + HUD_CHAR_H * hs;	/* (at the top left: the story's lines have the bottom) */
		hud_text_scaled (x, y - HUD_CHAR_H * hs - 2 * hs, "EMBER", s.ember > 0.0f ? HUD_RGBA (255, 200, 110, 255) : HUD_RGBA (150, 150, 150, 255), hs);
		hud_rect (x, y, w, 6 * hs, HUD_RGBA (40, 50, 55, 255));
		hud_rect (x, y, w * s.ember, 6 * hs, s.raised > 0.5f && ((int) (t_now * 6.0f) & 1) ? HUD_RGBA (255, 240, 190, 255) : HUD_RGBA (255, 190, 90, 255));
		if (card_t < CARD_S)
		{
			snprintf (text, sizeof text, "%s", numerals[s.level]);
			centred (vp[3] * 0.24f, text, HUD_RGBA (255, 200, 110, 255), 2 * hs, vp[2]);
			centred (vp[3] * 0.24f + 2.6f * lh, lf_level_name (s.level), HUD_RGBA (225, 215, 190, 255), 2 * hs, vp[2]);
		}
		const lf_line_t *l = lf_caption (&s);
		if (l)
		{
			caption (l, vp, hs);
		}
		else if (mode == MODE_PLAY && !ui.bot)		/* what a button would do here */
		{
			float lantern[3];
			lf_lantern (&s, lantern);
			const char *ask = asks[s.ody_can];
			if (s.has_beacon && !s.beacon_lit && dist3 (lantern, s.beacon) < 200.0f)
				ask = "HOLD A: KINDLE THE BEACON";
			else if (s.ody.state == LF_ODY_ASLEEP && dist3 (lantern, s.ody.p) < 240.0f)
				ask = "HOLD A: THE LANTERN";
			centred (vp[3] - 6 * hs - lh, ask, HUD_RGBA (200, 195, 175, 255), hs, vp[2]);
		}
		if (ui.bot && ((int) (t_now * 1.5f) & 1))
		{
			centred (4 * hs, "PLAYING ITSELF", HUD_RGBA (200, 195, 175, 255), hs, vp[2]);
		}
		if (ui.paused)
		{
			centred (vp[3] * 0.4f, "PAUSED", HUD_RGBA (255, 255, 255, 255), 3 * hs, vp[2]);
		}
	}
	if (ui.numbers)
	{
		hud_perf (vp[2] - hud_perf_width (hud_perf_scale (vp[3])) - 2, 2, hud_perf_scale (vp[3]), perf);
	}
	hud_end ();
}

/* ---- a scene begun ----------------------------------------------------------------------------- */

static struct { float yaw, back, eye[3]; bool set; } cam;

static bool begin_level (int level, float ember)
{
	if (r.bsp)
	{
		render_free (&r);
	}
	if (!bsp_load (&bsp, levels[level].data, (size_t) (levels[level].end - levels[level].data)) || !render_init (&r, &bsp)
	    || !lf_load (&s, &bsp, level, ember))
	{
		printf ("lanternfall: scene %d doesn't fit\n", level + 1);
		return false;
	}
	static bool sounds_sent;
	if (!sounds_sent)				/* the engine's sounds and the game's own to the RPi, once */
	{
		sound_init (&snd, &bsp, SND_AMB_WIND);
		for (int i = LSND_FIRST; i < LSND_COUNT; i++)
		{
			const lf_sound_t *d = &lf_sounds[i - LSND_FIRST];
			pgpu_sound_data (i, d->rate, PGPU_SOUND_U8, d->samples, d->frames);
		}
		sounds_sent = true;
	}
	else
	{
		sound_level (&snd, &bsp, SND_AMB_WIND);
	}
	lf_bot_init (&bot, &s);
	world_uniforms ();
	for (int i = 0; i < MOTES; i++)
	{
		motes[i].age = motes[i].life = 0.0f;
	}
	for (int i = 0; i < AMBIENTS; i++)
	{
		ambient[i][0] = -1.0f;
	}
	cam.set = false;
	card_t = 0.0f;
	storm_next = t_now + 5.0f;
	dawn = 0.0f;
	music.phrase = -1;
	music.next = t_now + 4.0f;
	printf ("lanternfall: %s: %d faces, %d models, %u bytes; %d of the Hollow, %d ways and walls, %d shrines, %d steps of its own way\n",
		lf_level_name (level), (int) bsp.n_faces, (int) bsp.n_models, (unsigned) bsp.size, s.n_hollow, s.n_brushes, s.n_shrines,
		s.n_steps);
	return true;
}

/* ---- pictures of it, on a PC ----------------------------------------------------------------- */

#if LF_SHOT
/* LF_SHOT=WIDTHxHEIGHT:PREFIX:SECONDS[,SECONDS...] (a PC): the game played by
   itself in steps of 1/60 s from scene LF_LEVEL (0 .. 5), drawn off screen;
   the picture at each of those moments is written to PREFIX-SECONDS.ppm, and
   after the last it ends */
static struct
{
	bool on;
	int w, h, n, next;
	char prefix[200];
	float at[40];
} shot;

static void shot_init (void)
{
	const char *what = getenv ("LF_SHOT");
	int used = 0;
	if (!what)
	{
		return;
	}
	if (sscanf (what, "%dx%d:%199[^:]:%n", &shot.w, &shot.h, shot.prefix, &used) < 3 || !used)
	{
		printf ("lanternfall: LF_SHOT=WIDTHxHEIGHT:PREFIX:SECONDS[,SECONDS...]\n");
		exit (2);
	}
	for (const char *p = what + used; *p && shot.n < 40; p += strcspn (p, ","), p += *p == ',')
	{
		shot.at[shot.n++] = (float) atof (p);
	}
	shot.on = true;
}

static void shot_write (float t)
{
	static uint8_t *pixels;
	char name[240];
	pixels = pixels ? pixels : malloc ((size_t) shot.w * shot.h * 4);
	glPixelStorei (GL_PACK_ALIGNMENT, 1);
	for (int i = 0; i < 20; i++)
	{
		glFinish ();
	}
	for (int y = 0; y < shot.h; y += 16)
	{
		glReadPixels (0, y, shot.w, shot.h - y < 16 ? shot.h - y : 16, GL_RGBA, GL_UNSIGNED_BYTE, &pixels[(size_t) y * shot.w * 4]);
	}
	snprintf (name, sizeof name, "%s-%g.ppm", shot.prefix, t);
	FILE *out = fopen (name, "wb");
	if (out)
	{
		fprintf (out, "P6\n%d %d\n255\n", shot.w, shot.h);
		for (int y = shot.h - 1; y >= 0; y--)
			for (int x = 0; x < shot.w; x++)
			{
				fwrite (&pixels[((size_t) y * shot.w + x) * 4], 1, 3, out);
			}
		fclose (out);
	}
	printf ("lanternfall: %s (scene %d, Wren at %.0f %.0f %.0f, ember %.2f, GL error 0x%x)\n", name, s.level + 1, s.g.origin[0],
		s.g.origin[1], s.g.origin[2], s.ember, (unsigned) glGetError ());
}
#endif

/* ---- all of it ------------------------------------------------------------------------------- */

int main (void)
{
	stdio_init_all ();
	pgpu_init ();
	keys_t keys;
	keys_init (&keys);				/* (the stick found; a PC's terminal taking keys as they come) */
	ui.pad = keys.pad;
	printf ("\nlanternfall: waiting for the RPi (READY)...\n");
	while (!pgpu_wait_ready (1000))
	{
	}
	pgpu_set_reply_phase (1);
	int tries = 0, first = 0;
#if LF_SHOT
	shot_init ();
	first = getenv ("LF_LEVEL") ? atoi (getenv ("LF_LEVEL")) % LF_LEVELS : 0;
	if (shot.on && !pglInitSurface (shot.w, shot.h))
	{
		printf ("lanternfall: no surface of %dx%d\n", shot.w, shot.h);
		return 1;
	}
	while (!shot.on && !pglInit () && ++tries < 5)
	{
	}
#else
	while (!pglInit () && ++tries < 5)		/* the first reply can be missed */
	{
	}
#endif
	if (!hud_init ())
	{
		printf ("lanternfall: the HUD program didn't link\n");
	}
	glDisable (GL_CULL_FACE);
	glDisable (GL_DITHER);
	figures_init (&fig);
	flares_init ();
	if (!begin_level (first, 0.6f))
	{
		return 1;
	}
	printf ("lanternfall: the stick: walk and turn (gently: softly); B jump, A the lantern, X a flare, Y Ody, SELECT the camera, "
		"START a pause. Keys: w s a d q e, space, l f y c v p b n r h m\n");
#if LF_SHOT
	if (shot.on)
	{
		mode = MODE_PLAY;
		ui.bot = true;
	}
#endif

	GLint vp[4] = {0};
	float projection[16];
	absolute_time_t last = get_absolute_time ();
	perf_t perf;
	memset (&perf, 0, sizeof perf);
	unsigned frame = 0, windows = 0;
	bool new_perf = true;
	const bool pulses = strcmp (pgpu_link_name (), "USB") != 0;	/* (one frame in flight, as zerog) */
	uint32_t sent = 0, shown_base = pgpu_frame_count ();
	float ember_next = 0.6f, raise_heard = 0.0f;
	const lf_line_t *shown_line = NULL;
	while (true)
	{
		pglSamples (ui.smooth ? 4 : 1);
		if (screen_update ("lanternfall", vp))
		{
			mat4_perspective (projection, FOVY, (float) vp[2] / vp[3], 4.0f, 8192.0f);
		}
		absolute_time_t now = get_absolute_time ();
		float dt = absolute_time_diff_us (last, now) / 1e6f;
		dt = dt > 0.05f ? 0.05f : dt;
		last = now;
#if LF_SHOT
		dt = shot.on ? 1.0f / 60 : dt;
#endif
		lf_input_t in;
		controls (&in, t_now, dt);
		if (!ui.paused)
		{
			t_now += dt;
		}
		float step = ui.paused ? 0.0f : dt;
		mode_s += step;
		card_t += step;
		r.time = t_now;

		/* the title; the scene played (by the stick, or by itself); the scene left; the end */
		bool load = false;
		int level = s.level;
		switch (mode)
		{
		case MODE_TITLE:
			memset (&in, 0, sizeof in);
			lf_update (&s, &in, step);
			s.n_captions = 0;
			card_t = 1e9f;
			if (ui.start || ui.idle > TITLE_IDLE_S)
			{
				ui.bot = !ui.start;
				mode = MODE_PLAY;
				load = true;
				level = 0;
				ember_next = 0.6f;
			}
			break;
		case MODE_PLAY:
			if (ui.bot)
			{
				lf_bot (&bot, &s, &in, step);
			}
			if (step > 0.0f)
			{
				lf_update (&s, &in, step);
				sound_game (&snd, &s.g, step);
				events ();
			}
			if (s.done || ui.next)
			{
				mode = MODE_LEAVE;
				mode_s = 0.0f;
			}
			else if (s.ending)
			{
				mode = MODE_END;
				mode_s = 0.0f;
			}
			else if (ui.again)
			{
				load = true;
				level = ui.bot ? level : 0;
				mode = ui.bot ? MODE_PLAY : MODE_TITLE;
				ember_next = 0.6f;
			}
			break;
		case MODE_LEAVE:				/* the dark, then the next scene */
			memset (&in, 0, sizeof in);
			lf_update (&s, &in, step);
			if (mode_s > 0.9f)
			{
				ember_next = s.ember > 0.5f ? s.ember : 0.5f;
				level = (s.level + 1) % LF_LEVELS;
				load = true;
				mode = MODE_PLAY;
			}
			break;
		case MODE_END:					/* its last lines, its name; then the title again */
			memset (&in, 0, sizeof in);
			lf_update (&s, &in, step);
			events ();
			if (s.n_captions)
			{
				mode_s = 0.0f;
			}
			else if (mode_s > 12.0f || (ui.start && mode_s > 2.0f))
			{
				load = true;
				level = 0;
				mode = MODE_TITLE;
				ui.bot = false;
				ui.idle = 0.0f;
				ember_next = 0.6f;
			}
			break;
		}
		ui.start = ui.next = ui.again = false;
		music_update (s.level, mode != MODE_TITLE && step > 0.0f);

		/* the lantern heard: raised, and humming while it's held */
		if (in.raise && s.raised > 0.3f && t_now - raise_heard > 1.2f && s.ember > 0.0f)
		{
			sfx (LSND_RAISE, NULL, OWNER_LANTERN, 0.35f, 0.0f, 1.0f);
			raise_heard = t_now;
		}
		if (lf_caption (&s) != shown_line)
		{
			shown_line = lf_caption (&s);
			new_perf = true;
		}
		fade = mode == MODE_LEAVE ? mode_s / 0.8f : s.out > 0.0f ? 1.0f - (s.out - 0.3f) / 1.5f : s.sea > 0.0f ? 0.7f
		     : mode == MODE_PLAY && card_t < 1.0f ? 1.0f - card_t : 0.0f;
		stride += sqrtf (s.g.velocity[0] * s.g.velocity[0] + s.g.velocity[1] * s.g.velocity[1]) * step;

#if LF_SHOT
		if (shot.on && t_now < shot.at[shot.next] - 0.05f && !load)	/* (drawn only for the pictures) */
		{
			motes_update (0.0f);
			flare_n = 0;
			continue;
		}
#endif

		/* the camera: behind her and over her shoulder, turning after her; not through walls. The title: round her */
		float head[3] = {s.g.origin[0], s.g.origin[1], s.g.view_z + 12.0f}, want[3], eye[3], centre[3], view[16], vpm[16];
		float back = ui.near ? 70.0f : 118.0f, up = ui.near ? 22.0f : 44.0f;
		if (!cam.set)
		{
			cam.yaw = s.g.yaw;
			cam.set = true;
		}
		float turn = (mode == MODE_TITLE || mode == MODE_END ? cam.yaw + 0.25f * dt : s.g.yaw) - cam.yaw;
		turn -= 2 * PI * floorf ((turn + PI) / (2 * PI));
		cam.yaw += mode == MODE_TITLE || mode == MODE_END ? turn : turn * (1.0f - expf (-dt * 6.0f));
		if (mode == MODE_END)
		{
			back = 260.0f;
			up = 110.0f;
		}
		vset (want, head[0] - cosf (cam.yaw) * back, head[1] - sinf (cam.yaw) * back, head[2] + up);
		collide_trace_t ct = collide_trace (&s.g.collide, 0, head, want, -1);
		float room = ct.fraction * sqrtf (back * back + up * up) - 8.0f;
		room = room < 4.0f ? 4.0f : room;
		for (int k = 0; k < 3; k++)
		{
			eye[k] = head[k] + (want[k] - head[k]) * room / sqrtf (back * back + up * up) + shake * (rnd () - 0.5f);
		}
		shake *= expf (-dt * 8.0f);
		vset (centre, head[0] + cosf (cam.yaw) * 60.0f, head[1] + sinf (cam.yaw) * 60.0f, head[2] + 6.0f);
		const float z_up[3] = {0, 0, 1};
		mat4_look_at (view, eye, centre, z_up);
		mat4_multiply (vpm, projection, view);
		planes_from (vpm);
		vcopy (cam_eye, eye);
		vset (cam_right, view[0], view[4], view[8]);
		vset (cam_up, view[1], view[5], view[9]);
		flare_n = 0;
		ambient_frame++;

		/* the picture: the level, what moves in it, the figures, the lights */
		float lantern[3] = {hand[0], hand[1], hand[2] - 7.0f};
		if (frame == 0 || load)
		{
			lf_lantern (&s, lantern);
		}
		lights_update (lantern, dt);
		glClear (GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
		glEnable (GL_DEPTH_TEST);
		glDisable (GL_BLEND);
		render_world (&r, eye, vpm);
		for (int k = 0; k < s.g.n_movers; k++)
		{
			render_model (&r, s.g.movers[k].model, s.g.movers[k].at, vpm);
		}
		for (int i = 0; i < s.n_brushes; i++)
		{
			const lf_brush_t *b = &s.brush[i];
			float off[3] = {0, 0, b->kind == LF_GATE ? -b->size[2] * b->open : 0.0f};
			if ((b->kind == LF_HIDDEN && b->open <= 0.0f) || (b->kind != LF_HIDDEN && b->done) || !seen (b->centre, b->radius))
			{
				continue;
			}
			if (b->kind == LF_HIDDEN && !b->done)		/* coming out of the dark */
			{
				glUseProgram (r.programs[0].id);
				glUniform1f (wu[0].shown, b->open * b->open);
			}
			render_model (&r, b->model, off, vpm);
			if (b->kind == LF_HIDDEN && !b->done)
			{
				glUniform1f (wu[0].shown, 1.0f);
			}
		}
		draw_figures (vpm, room > 40.0f || mode == MODE_END);
		motes_update (step);
		flares_draw (vpm, 1.0f / lights.fog[3]);

		if (new_perf || frame % 6 == 0 || load)
		{
			hud_build (vp, &perf);
		}
		hud_draw ();
#if LF_SHOT
		if (shot.on)
		{
			if (t_now >= shot.at[shot.next])
			{
				shot_write (shot.at[shot.next]);
				if (++shot.next == shot.n)
				{
					return 0;
				}
			}
			pglSwapBuffers ();
			frame++;
			if (load)
			{
				begin_level (level, ember_next);
			}
			continue;
		}
#endif
		pglSwapBuffers ();
		sent++;
		if (load)					/* the next scene: sent to the RPi while this picture stands */
		{
			if (!begin_level (level, ember_next))
			{
				return 1;
			}
			mode_s = 0.0f;
			last = get_absolute_time ();
			shown_base = pgpu_frame_count () + 1 - sent;
		}

		absolute_time_t wait_start = get_absolute_time ();
		if (!pulses)
		{
			pgpu_wait_frame (100);
		}
		else
		{
			while (pgpu_frame_count () - shown_base + 1 < sent)
			{
				if (!pgpu_wait_frame (100))
				{
					shown_base = pgpu_frame_count () + 1 - sent;	/* (a pulse was missed) */
					break;
				}
			}
		}
		new_perf = perf_frame (absolute_time_diff_us (wait_start, get_absolute_time ()), &perf);
		frame++;
		if (new_perf && ++windows % 5 == 0)
		{
			perf_log_link ("lanternfall", &perf);
			printf ("lanternfall: %.1f fps, GPU %.0f%% CPU-H %.0f%%, render %.2f ms; %u faces in %u draws, %d lights; scene %d %s, "
				"Wren at %.0f %.0f %.0f, ember %.2f, out %d times; GL error 0x%x\n", perf.fps, perf.gpu * 100, perf.cpu_h * 100,
				perf.render_ms, r.stats.faces, r.stats.draws, flare_n / 6, s.level + 1, ui.bot ? "(by itself)" : "", s.g.origin[0],
				s.g.origin[1], s.g.origin[2], s.ember, s.deaths, (unsigned) glGetError ());
		}
	}
}
