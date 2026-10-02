/*
 * zerog - anti-gravity racing to fly yourself, after WipEout: eight craft,
 * three laps of a circuit with a jump and a fork, weapons picked up on the
 * way. The first craft is the stick's and the game controller's:
 *
 *   the stick     left and right: the nose; forward: the engine; back: both air brakes
 *   B             the engine
 *   Y, A          the left and the right air brake: one turns the craft
 *                 harder that way, both slow it
 *   X, the stick pressed   fire what a weapon pad gave
 *   SELECT        the view: behind, close behind, from the nose
 *   START         a pause; after a race, the next one
 *
 * Until something is touched the craft flies itself (as the seven others
 * always do), and again when nothing has been touched for a while. On the
 * console: a d the nose, w the engine (on, off), s the brakes, q e an air
 * brake, the space bar fires, v the view, p a pause, o the craft to itself,
 * r the race again, m the antialiasing off and on.
 *
 * The race is race.c's (the flight model, the craft that fly themselves, the
 * weapons) on track.c's circuit; what is drawn is made by art.c. Here: the
 * controls, the camera, the drawing, the sound and the HUD.
 *
 * Drawn as antigrav is: the craft, the road in the pieces that are seen, the
 * scenery, the ground, the sky behind all of it; then what's blended: the
 * shadows, and the lights (the engines, the shots, the explosions, a shield:
 * squares facing the eye, added). Antialiased by the RPi (4x MSAA).
 *
 * Heard from the player's craft, as antigrav: its engine and the wind, the
 * others' engines from where they are, the crowd by the stands, the pads, the
 * walls, the air under what's over the road; and the weapons (zerog's own
 * sounds, made by tools/make_zerog_sounds.py).
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
#include "sky_program.h"
#include "scenery_program.h"
#include "craft_program.h"
#include "nightsky_program.h"
#include "flare_program.h"
#include "vec.h"
#include "track.h"
#include "race.h"
#include "art.h"
#include "sounds.h"

#define FOVY		64.0f		/* at rest: wider with the speed */
#define IDLE_SECONDS	40.0f		/* nothing touched this long: the craft flies itself */
#define KEY_HOLD_S	0.22f		/* a key on the console holds this long (the terminal repeats it) */

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
	{"day", {0.78f, 0.74f, 0.70f}, {0.20f, 0.38f, 0.70f}, 1.0f, 0.0022f, false},
	{"night", {0.09f, 0.06f, 0.15f}, {0.01f, 0.01f, 0.04f}, 0.38f, 0.0028f, true},
};
static const sky_t *sky_now = &skies[0];
static float sun[3];

/* ---- the programs ------------------------------------------------------------------- */

static struct
{
	GLuint sky, night, sc, cp, fl;
	GLint s_pos, s_horizon, s_sun, s_aspect, s_haze, s_zenith;
	GLint n_pos, n_dir, n_horizon;
	GLint sc_vp, sc_model, sc_eye, sc_fog, sc_haze, sc_light, sc_pos, sc_uv, sc_shade;
	GLint c_vp, c_model, c_eye, c_fog, c_haze, c_light, c_pos, c_normal, c_uv;
	GLint f_vp, f_eye, f_fog, f_pos, f_uv, f_color;
} gl;

static void programs_init (void)
{
	gl.sky = glCreateProgram ();
	glProgramBinaryOES (gl.sky, PGL_PROGRAM_BINARY_PGPU, &sky_info, sizeof sky_info);
	gl.s_pos = glGetAttribLocation (gl.sky, "a_pos");
	gl.s_horizon = glGetUniformLocation (gl.sky, "u_horizon");
	gl.s_sun = glGetUniformLocation (gl.sky, "u_sun");
	gl.s_aspect = glGetUniformLocation (gl.sky, "u_aspect");
	gl.s_haze = glGetUniformLocation (gl.sky, "u_haze");
	gl.s_zenith = glGetUniformLocation (gl.sky, "u_zenith");

	gl.night = glCreateProgram ();			/* the sky at night: with the stars */
	glProgramBinaryOES (gl.night, PGL_PROGRAM_BINARY_PGPU, &nightsky_info, sizeof nightsky_info);
	gl.n_pos = glGetAttribLocation (gl.night, "a_pos");
	gl.n_dir = glGetAttribLocation (gl.night, "a_dir");
	gl.n_horizon = glGetUniformLocation (gl.night, "u_horizon");
	glUseProgram (gl.night);
	glUniform3fv (glGetUniformLocation (gl.night, "u_haze"), 1, skies[1].haze);
	glUniform3fv (glGetUniformLocation (gl.night, "u_zenith"), 1, skies[1].zenith);
	glUniform1i (glGetUniformLocation (gl.night, "u_stars"), 0);

	gl.sc = glCreateProgram ();
	glProgramBinaryOES (gl.sc, PGL_PROGRAM_BINARY_PGPU, &scenery_info, sizeof scenery_info);
	gl.sc_vp = glGetUniformLocation (gl.sc, "u_vp");
	gl.sc_model = glGetUniformLocation (gl.sc, "u_model");
	gl.sc_eye = glGetUniformLocation (gl.sc, "u_eye");
	gl.sc_fog = glGetUniformLocation (gl.sc, "u_fog");
	gl.sc_haze = glGetUniformLocation (gl.sc, "u_haze");
	gl.sc_light = glGetUniformLocation (gl.sc, "u_light");
	gl.sc_pos = glGetAttribLocation (gl.sc, "a_pos");
	gl.sc_uv = glGetAttribLocation (gl.sc, "a_uv");
	gl.sc_shade = glGetAttribLocation (gl.sc, "a_shade");
	glUseProgram (gl.sc);
	glUniform1i (glGetUniformLocation (gl.sc, "u_texture"), 0);

	gl.cp = glCreateProgram ();
	glProgramBinaryOES (gl.cp, PGL_PROGRAM_BINARY_PGPU, &craft_info, sizeof craft_info);
	gl.c_vp = glGetUniformLocation (gl.cp, "u_vp");
	gl.c_model = glGetUniformLocation (gl.cp, "u_model");
	gl.c_eye = glGetUniformLocation (gl.cp, "u_eye");
	gl.c_fog = glGetUniformLocation (gl.cp, "u_fog");
	gl.c_haze = glGetUniformLocation (gl.cp, "u_haze");
	gl.c_light = glGetUniformLocation (gl.cp, "u_light");
	gl.c_pos = glGetAttribLocation (gl.cp, "a_pos");
	gl.c_normal = glGetAttribLocation (gl.cp, "a_normal");
	gl.c_uv = glGetAttribLocation (gl.cp, "a_uv");
	glUseProgram (gl.cp);
	glUniform3fv (glGetUniformLocation (gl.cp, "u_sun_dir"), 1, sun);
	glUniform1i (glGetUniformLocation (gl.cp, "u_texture"), 0);

	gl.fl = glCreateProgram ();
	glProgramBinaryOES (gl.fl, PGL_PROGRAM_BINARY_PGPU, &flare_info, sizeof flare_info);
	gl.f_vp = glGetUniformLocation (gl.fl, "u_vp");
	gl.f_eye = glGetUniformLocation (gl.fl, "u_eye");
	gl.f_fog = glGetUniformLocation (gl.fl, "u_fog");
	gl.f_pos = glGetAttribLocation (gl.fl, "a_pos");
	gl.f_uv = glGetAttribLocation (gl.fl, "a_uv");
	gl.f_color = glGetAttribLocation (gl.fl, "a_color");
	glUseProgram (gl.fl);
	glUniform1i (glGetUniformLocation (gl.fl, "u_texture"), 0);
}

/* the sky's colours and light, in the programs that use them */
static void apply_sky (void)
{
	glUseProgram (gl.sky);
	glUniform3fv (gl.s_haze, 1, sky_now->haze);
	glUniform3fv (gl.s_zenith, 1, sky_now->zenith);
	glUseProgram (gl.sc);
	glUniform3fv (gl.sc_haze, 1, sky_now->haze);
	glUniform1f (gl.sc_light, sky_now->light);
	glUniform1f (gl.sc_fog, sky_now->fog);
	glUseProgram (gl.cp);
	glUniform3fv (gl.c_haze, 1, sky_now->haze);
	glUniform1f (gl.c_light, sky_now->light);
	glUniform1f (gl.c_fog, sky_now->fog);
	glUseProgram (gl.fl);
	glUniform1f (gl.f_fog, sky_now->fog);
	glClearColor (sky_now->haze[0], sky_now->haze[1], sky_now->haze[2], 1.0f);
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

static void scenery_pointers (void)
{
	arrays (gl.sc_pos, gl.sc_uv, gl.sc_shade);
	glVertexAttribPointer (gl.sc_pos, 3, GL_FLOAT, GL_FALSE, sizeof (vertex_t), (void *) 0);
	glVertexAttribPointer (gl.sc_uv, 2, GL_FLOAT, GL_FALSE, sizeof (vertex_t), (void *) 12);
	glVertexAttribPointer (gl.sc_shade, 1, GL_FLOAT, GL_FALSE, sizeof (vertex_t), (void *) 20);
}

/* ---- the sound ---------------------------------------------------------------------- */

#define OTHERS		4		/* the others' engines heard: the nearest */
enum
{
	CH_ENGINE, CH_WIND, CH_CROWD,
	CH_OTHER,
	CH_BOOST = CH_OTHER + OTHERS, CH_OTHER_FX, CH_WHOOSH,	/* (two, in turn: the hoops come faster than one ends) */
	CH_BUMP = CH_WHOOSH + 2, CH_SCRAPE, CH_SIGNAL, CH_WEAPON, CH_BLAST
};

static struct { int left, right; float pitch; } ch_now[PGPU_SOUND_CHANNELS];

/* a loop's volumes (0 .. 1) and pitch: sent when they change */
static void ch_set (int ch, float left, float right, float pitch)
{
	int l = (int) (clampf (left, 0.0f, 1.0f) * PGPU_SOUND_FULL + 0.5f);
	int r = (int) (clampf (right, 0.0f, 1.0f) * PGPU_SOUND_FULL + 0.5f);
	if (l != ch_now[ch].left || r != ch_now[ch].right)
	{
		pgpu_sound_volume (ch, l, r);
		ch_now[ch].left = l;
		ch_now[ch].right = r;
	}
	if (fabsf (pitch - ch_now[ch].pitch) > 0.003f)
	{
		pgpu_sound_pitch (ch, pitch);
		ch_now[ch].pitch = pitch;
	}
}

/* a loop's volume to a side: pan -1 (left) .. 1 (right) */
static void ch_set_pan (int ch, float vol, float pan, float pitch)
{
	ch_set (ch, vol * fminf (1.0f, 1.0f - pan), vol * fminf (1.0f, 1.0f + pan), pitch);
}

/* a sound once */
static void ch_shot (int ch, int sound, float vol, float pan, float pitch)
{
	float l = clampf (vol * fminf (1.0f, 1.0f - pan), 0.0f, 1.0f), r = clampf (vol * fminf (1.0f, 1.0f + pan), 0.0f, 1.0f);
	pgpu_sound_play (ch, sound, (uint32_t) (l * PGPU_SOUND_FULL), (uint32_t) (r * PGPU_SOUND_FULL), 0);
	if (pitch != 1.0f)
	{
		pgpu_sound_pitch (ch, pitch);
	}
}

/* the sounds to the RPi, the loops started (silent yet) */
static void sound_init (void)
{
	for (int i = 1; i < ASND_COUNT; i++)
	{
		pgpu_sound_data (i, antigrav_sounds[i].rate, PGPU_SOUND_U8, antigrav_sounds[i].samples, antigrav_sounds[i].frames);
	}
	for (int i = ZSND_FIRST; i < ZSND_COUNT; i++)
	{
		const antigrav_sound_t *s = &zerog_sounds[i - ZSND_FIRST];
		pgpu_sound_data (i, s->rate, PGPU_SOUND_U8, s->samples, s->frames);
	}
	for (int ch = 0; ch < CH_BOOST; ch++)
	{
		pgpu_sound_play (ch, ch == CH_ENGINE ? ASND_ENGINE : ch == CH_WIND ? ASND_WIND : ch == CH_CROWD ? ASND_CROWD : ASND_ENGINE_OTHER,
				 0, 0, PGPU_SOUND_LOOP);
		ch_now[ch].left = ch_now[ch].right = 0;
		ch_now[ch].pitch = 1.0f;
	}
}

/* a frame's: what the player's craft hears now (ev: what happened to it) */
static void sound_update (float t, float in_phase, unsigned ev, bool paused)
{
	static float last_wall = -1.0f, last_bump = -1.0f, cheer_at = -100.0f;
	static int last_count, last_phase = -1, last_ring = -1, lap_heard, whoosh, mines;
	const route_t *m = &route[ROUTE_MAIN];
	craft_t *me = &race.craft[race.player];
	float ve = craft_speed (me), rt[3];
	float quiet = paused ? 0.0f : race.phase == RESULTS ? 0.5f : 1.0f;	/* (the results are read in some peace) */
	int at = (int) (track_progress (&me->at) / m->step) % m->n;
	bool inside = me->at.route == ROUTE_MAIN && (m->rings[at].flags & RING_TUNNEL);
	v_cross (rt, me->fwd, me->f.u);

	/* its engine (its note the speed, and the throttle; revved as the countdown
	   ends; louder in the tunnel) and the wind */
	float rev = race.phase == GRID ? 0.25f * clampf ((in_phase - (GRID_SECONDS - 3.0f)) / 3.0f, 0.0f, 1.0f) : 0.08f * me->throttle;
	float engine = quiet * (inside ? 1.3f : 1.0f) * (0.26f + 0.08f * me->throttle + 0.14f * clampf (ve / 100.0f, 0.0f, 1.0f));
	ch_set (CH_ENGINE, engine, engine, 0.55f + ve / 105.0f + rev);
	float air = clampf (ve / 125.0f, 0.0f, 1.0f);
	ch_set (CH_WIND, quiet * 0.40f * air * air, quiet * 0.40f * air * air, 0.75f + 0.5f * air);

	/* the others, the nearest of them: from where they are; the note higher
	   while the gap closes, lower while it opens */
	int near[OTHERS];
	float near_d[OTHERS];
	for (int k = 0; k < OTHERS; k++)
	{
		near[k] = -1;
		near_d[k] = 1e9f;
	}
	for (int i = 0; i < CRAFTS; i++)
	{
		craft_t *o = &race.craft[i];
		float d[3], d2;
		v_sub (d, o->p, me->p);
		d2 = v_dot (d, d);
		if (o != me)
		{
			if ((o->events & EV_BOOST) && d2 < 70.0f * 70.0f)
			{
				ch_shot (CH_OTHER_FX, ASND_BOOST, 0.5f / (1.0f + d2 / 400.0f), v_dot (d, rt) / sqrtf (d2 + 4.0f), 1.15f);
			}
			if ((o->events & EV_FIRE) && d2 < 90.0f * 90.0f)
			{
				ch_shot (CH_OTHER_FX, ZSND_FIRE, 0.6f / (1.0f + d2 / 900.0f), v_dot (d, rt) / sqrtf (d2 + 4.0f), 1.1f);
			}
			o->events = 0;
			if (o->lost > 0.0f)
			{
				continue;
			}
			for (int k = 0, j = i; k < OTHERS; k++)	/* into the nearest, in order */
			{
				if (d2 < near_d[k])
				{
					float td = near_d[k];
					int tj = near[k];
					near_d[k] = d2;
					near[k] = j;
					d2 = td;
					j = tj;
				}
			}
		}
	}
	for (int k = 0; k < OTHERS; k++)
	{
		if (near[k] < 0 || near_d[k] > 120.0f * 120.0f)
		{
			ch_set (CH_OTHER + k, 0.0f, 0.0f, 1.0f);
			continue;
		}
		const craft_t *o = &race.craft[near[k]];
		float d[3], rel[3], ov = craft_speed (o), dist;
		v_sub (d, o->p, me->p);
		v_sub (rel, o->v, me->v);
		dist = sqrtf (near_d[k] + 4.0f);
		float vol = quiet * (0.25f + 0.30f * clampf (ov / 60.0f, 0.0f, 1.0f)) / (1.0f + near_d[k] / 150.0f);
		float closing = -v_dot (rel, d) / dist;
		ch_set_pan (CH_OTHER + k, vol, v_dot (d, rt) / dist, (0.62f + ov / 115.0f) * (1.0f + clampf (closing / 80.0f, -0.25f, 0.25f)));
	}

	/* the crowd: by the stands (the start's straight), and all of it for the finish */
	int from_line = at > m->n / 2 ? at - m->n : at;
	float off = from_line < -30 ? -30 - from_line : from_line > 14 ? from_line - 14 : 0;
	float cheer = clampf (1.0f - (t - cheer_at) / 6.0f, 0.0f, 1.0f);
	float crowd = quiet * fmaxf (0.28f * clampf (1.0f - off / 50.0f, 0.0f, 1.0f), 0.45f * cheer);
	ch_set (CH_CROWD, crowd, crowd, 1.0f + 0.12f * cheer);

	/* what happened to it */
	if (ev & (EV_BOOST | EV_TURBO))
	{
		ch_shot (CH_BOOST, ASND_BOOST, 0.6f, 0.0f, ev & EV_TURBO ? 1.3f : 1.0f);
	}
	if ((ev & EV_WALL) && t - last_wall > 0.35f)
	{
		ch_shot (CH_SCRAPE, ASND_SCRAPE, 0.6f, me->x > 0.0f ? 0.7f : -0.7f, 1.0f);
		last_wall = t;
	}
	if ((ev & (EV_BUMP | EV_LAND)) && t - last_bump > 0.3f)
	{
		ch_shot (CH_BUMP, ASND_BUMP, 0.6f, 0.0f, ev & EV_BUMP ? 1.0f : 0.6f);
		last_bump = t;
	}
	if (race.phase == GRID)
	{
		lap_heard = 0;
	}
	if ((ev & EV_LAP) && me->lap > lap_heard)			/* the last lap's a third higher */
	{
		ch_shot (CH_SIGNAL, ASND_LAP, 0.6f, 0.0f, me->lap == LAPS - 1 ? 1.26f : 1.0f);
		lap_heard = me->lap;
	}
	if (ev & EV_FINISH)
	{
		ch_shot (CH_SIGNAL, ASND_FINISH, 0.6f, 0.0f, 1.0f);
		cheer_at = t;
	}
	if (ev & EV_PICKUP)
	{
		ch_shot (CH_WEAPON, ZSND_PICKUP, 0.55f, 0.0f, 1.0f);
	}
	if (ev & EV_FIRE)
	{
		ch_shot (CH_WEAPON, ZSND_FIRE, 0.7f, 0.0f, 1.0f);
	}
	if (ev & EV_SHIELD)
	{
		ch_shot (CH_WEAPON, ZSND_SHIELD, 0.6f, 0.0f, 1.0f);
	}
	if (ev & EV_FALL)
	{
		ch_shot (CH_SIGNAL, ZSND_FALL, 0.6f, 0.0f, 1.0f);
	}
	if (me->mines < mines)						/* one more dropped */
	{
		ch_shot (CH_WEAPON, ZSND_MINE, 0.5f, 0.0f, 1.0f);
	}
	mines = me->mines;

	/* a missile after it: told, four times a second */
	static float warned = -1.0f;
	if (race.phase == RACE && race_hunted (race.player) && t - warned > 0.25f)
	{
		ch_shot (CH_OTHER_FX, ASND_BEEP, 0.45f, 0.0f, 1.6f);
		warned = t;
	}

	/* the explosions: from where they are */
	for (int i = 0; i < BLASTS; i++)
	{
		blast_t *b = &race.blast[i];
		if (b->fresh)
		{
			float d[3], d2;
			v_sub (d, b->p, me->p);
			d2 = v_dot (d, d);
			b->fresh = false;
			if (d2 < 250.0f * 250.0f)
			{
				ch_shot (CH_BLAST, ZSND_BLAST, quiet * 0.9f / (1.0f + d2 / 2500.0f), v_dot (d, rt) / sqrtf (d2 + 9.0f), 1.0f);
			}
		}
	}

	/* the air under what's over the road (every ring since the last frame's) */
	if (race.phase != GRID && last_ring >= 0 && me->at.route == ROUTE_MAIN && !me->lost)
	{
		for (int r = last_ring, n = 0; r != at && n < 8; n++)
		{
			r = (r + 1) % m->n;
			if (art.over[r])
			{
				ch_shot (CH_WHOOSH + whoosh, ASND_WHOOSH, 0.2f + 0.45f * air, 0.0f, 0.85f + 0.35f * air);
				whoosh ^= 1;
			}
		}
	}
	last_ring = at;

	/* the countdown */
	int count = race.phase == GRID && in_phase > GRID_SECONDS - 3.0f ? (int) ceilf (GRID_SECONDS - in_phase) : 0;
	if (count && count != last_count)
	{
		ch_shot (CH_SIGNAL, ASND_BEEP, 0.6f, 0.0f, 1.0f);
	}
	if (race.phase == RACE && last_phase == GRID)
	{
		ch_shot (CH_SIGNAL, ASND_GO, 0.6f, 0.0f, 1.0f);
	}
	last_count = count;
	last_phase = race.phase;
}

/* ---- the craft, and what they fire, in the world ------------------------------------------- */

/* a craft's model matrix: its nose (and the spin a hit sends it into), rolled
   into its turn, the nose up as it climbs; bobbing a little */
static void craft_model (const craft_t *c, float t, float m[16])
{
	const float *u = c->f.u;
	float rt[3], fw[3], r2[3], u2[3], f3[3], u3[3], p[3];
	float ct = cosf (c->twirl), st = sinf (c->twirl), cr = cosf (c->roll), sr = sinf (c->roll), cp = cosf (c->pitch), sp = sinf (c->pitch);
	v_cross (rt, c->fwd, u);
	for (int i = 0; i < 3; i++)
	{
		fw[i] = c->fwd[i] * ct + rt[i] * st;
	}
	v_cross (rt, fw, u);
	for (int i = 0; i < 3; i++)
	{
		r2[i] = rt[i] * cr - u[i] * sr;
		u2[i] = u[i] * cr + rt[i] * sr;
		f3[i] = fw[i] * cp + u2[i] * sp;
		u3[i] = u2[i] * cp - fw[i] * sp;
	}
	v_mad (p, c->p, u, c->lost > 0.0f ? 0.0f : 0.07f * sinf (t * 2.3f + (float) (c - race.craft) * 1.7f));
	const float out[16] = {r2[0], r2[1], r2[2], 0, u3[0], u3[1], u3[2], 0, -f3[0], -f3[1], -f3[2], 0, p[0], p[1], p[2], 1};
	memcpy (m, out, sizeof out);
}

/* a point of a model (its own x, y, z) in the world */
static void model_point (float *p, const float m[16], float x, float y, float z)
{
	for (int i = 0; i < 3; i++)
	{
		p[i] = m[i] * x + m[4 + i] * y + m[8 + i] * z + m[12 + i];
	}
}

/* a rocket's: along its flight, turning about it; a mine's: on its point, spinning */
static void shot_model (const shot_t *s, float m[16])
{
	static const float up[3] = {0, 1, 0};
	if (s->type == SHOT_MINE)
	{
		float a = s->age * 2.0f, k = 1.3f;
		const float out[16] = {k * cosf (a), 0, k * sinf (a), 0, -k * sinf (a), 0, k * cosf (a), 0, 0, k, 0, 0,
				       s->p[0], s->p[1] + 0.25f, s->p[2], 1};
		memcpy (m, out, sizeof out);
		return;
	}
	float fw[3], rt[3], u[3], a = s->age * 9.0f, k = 1.8f;
	v_copy (fw, s->v);
	v_norm (fw);
	v_cross (rt, fw, up);
	v_norm (rt);
	v_cross (u, rt, fw);
	float r2[3], u2[3];
	for (int i = 0; i < 3; i++)
	{
		r2[i] = k * (rt[i] * cosf (a) - u[i] * sinf (a));
		u2[i] = k * (u[i] * cosf (a) + rt[i] * sinf (a));
	}
	const float out[16] = {r2[0], r2[1], r2[2], 0, u2[0], u2[1], u2[2], 0, -k * fw[0], -k * fw[1], -k * fw[2], 0,
			       s->p[0], s->p[1], s->p[2], 1};
	memcpy (m, out, sizeof out);
}

/* ---- the lights: squares facing the eye, added ----------------------------------------------- */

typedef struct
{
	float p[3], uv[2];
	uint8_t c[4];
} flare_vertex_t;

#define FLARES		144
static flare_vertex_t flares[FLARES * 6];
static int flare_n;
static float cam_right[3], cam_up[3], cam_at[3];	/* the eye's right and up, and where it is */

static void flare_vertex (const float *p, float right, float up, float u, float v, float r, float g, float b)
{
	flare_vertex_t *o = &flares[flare_n++];
	for (int i = 0; i < 3; i++)
	{
		o->p[i] = p[i] + cam_right[i] * right + cam_up[i] * up;
	}
	o->uv[0] = u;
	o->uv[1] = v;
	o->c[0] = (uint8_t) (clampf (r, 0, 1) * 255);
	o->c[1] = (uint8_t) (clampf (g, 0, 1) * 255);
	o->c[2] = (uint8_t) (clampf (b, 0, 1) * 255);
	o->c[3] = 255;
}

/* A light's pixels are blended, four samples each: many times what the
   others cost the GPU (a screen of them: 20 ms at 1024x600). So the lights
   are kept small on the screen, however near: no larger than so much of
   their distance. */
static float seen_from (const float *p)
{
	float d[3];
	v_sub (d, p, cam_at);
	return v_len (d);
}

/* a soft light at p, `size` to each side */
static void flare (const float *p, float size, float r, float g, float b)
{
	static const float corner[6][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, -1}, {1, 1}, {-1, 1}};
	if (flare_n + 6 > FLARES * 6)
	{
		return;
	}
	size = fminf (size, 0.09f * seen_from (p));
	for (int k = 0; k < 6; k++)
	{
		flare_vertex (p, corner[k][0] * size, corner[k][1] * size, 0.5f + 0.5f * corner[k][0], 0.5f + 0.5f * corner[k][1], r, g, b);
	}
}

/* a ring of light round p, facing the eye: a band `width` wide */
static void flare_ring (const float *p, float radius, float width, float r, float g, float b)
{
	enum { SIDES = 20 };
	float dist = seen_from (p);
	if (flare_n + SIDES * 6 > FLARES * 6)
	{
		return;
	}
	radius = fminf (radius, 0.30f * dist);
	width = fminf (width, 0.03f * dist);
	for (int k = 0; k < SIDES; k++)
	{
		float a0 = 2 * PI * k / SIDES, a1 = 2 * PI * (k + 1) / SIDES, in = radius - width / 2, out = radius + width / 2;
		float c0 = cosf (a0), s0 = sinf (a0), c1 = cosf (a1), s1 = sinf (a1);
		flare_vertex (p, c0 * in, s0 * in, 0.0f, 0.5f, r, g, b);	/* (across the band: the light's middle row) */
		flare_vertex (p, c0 * out, s0 * out, 1.0f, 0.5f, r, g, b);
		flare_vertex (p, c1 * out, s1 * out, 1.0f, 0.5f, r, g, b);
		flare_vertex (p, c0 * in, s0 * in, 0.0f, 0.5f, r, g, b);
		flare_vertex (p, c1 * out, s1 * out, 1.0f, 0.5f, r, g, b);
		flare_vertex (p, c1 * in, s1 * in, 0.0f, 0.5f, r, g, b);
	}
}

/* an explosion's fire: one of three spikes through its middle, turning */
static void fire_model (float m[16], const float *p, float size, int which, float a)
{
	const float axis[3][3] = {{cosf (a), 0, sinf (a)}, {0, 1, 0}, {-sinf (a), 0, cosf (a)}};
	for (int c = 0; c < 3; c++)
	{
		const float *e = axis[(c + which) % 3];
		float k = c == 2 ? size : 0.8f * size;			/* (long through, narrower across) */
		m[c * 4] = e[0] * k;
		m[c * 4 + 1] = e[1] * k;
		m[c * 4 + 2] = e[2] * k;
		m[c * 4 + 3] = 0.0f;
	}
	m[12] = p[0];
	m[13] = p[1];
	m[14] = p[2];
	m[15] = 1.0f;
}

/* ---- the camera ------------------------------------------------------------------------- */

enum { VIEW_CHASE, VIEW_CLOSE, VIEW_NOSE, VIEWS };

static struct
{
	bool set;
	float f[3], eye[3], fov, shake;
} cam;

static uint32_t shake_rng = 12345;

static float shake_rnd (void)
{
	shake_rng = shake_rng * 1664525u + 1013904223u;
	return (shake_rng >> 8 & 0xFFFF) / 32768.0f - 1.0f;
}

/* where the eye is and what it looks at: behind the player's craft, level,
   along where it points and where it goes (so it's seen sliding); left where
   it was while the craft falls */
static void camera (const craft_t *c, float dt, int view, float eye[3], float centre[3])
{
	float speed = craft_speed (c), dir[3], pos[3];
	v_copy (dir, c->fwd);
	if (speed > 12.0f && view != VIEW_NOSE)
	{
		v_set (dir, 0.55f * c->fwd[0] + 0.45f * c->v[0] / speed, 0.55f * c->fwd[1] + 0.45f * c->v[1] / speed,
		       0.55f * c->fwd[2] + 0.45f * c->v[2] / speed);
	}
	dir[1] = clampf (dir[1], -0.35f, 0.35f);
	v_norm (dir);
	if (!cam.set)
	{
		v_copy (cam.f, dir);
		cam.fov = FOVY;
		cam.set = true;
	}
	if (c->lost > 0.0f)
	{
		v_copy (eye, cam.eye);
		v_copy (centre, c->p);
		return;
	}
	float w = 1.0f - expf (-dt * (view == VIEW_NOSE ? 12.0f : 5.0f));
	v_mix (cam.f, cam.f, dir, w);
	v_norm (cam.f);
	v_copy (pos, c->p);
	if (view == VIEW_NOSE)
	{
		v_mad (eye, pos, c->f.u, 0.62f);
		v_mad (eye, eye, cam.f, 0.4f);
		v_mad (centre, eye, cam.f, 10.0f);
	}
	else
	{
		float back = view == VIEW_CHASE ? 9.5f : 6.4f, up = view == VIEW_CHASE ? 3.1f : 1.9f;
		v_mad (eye, pos, cam.f, -back);
		eye[1] += up;
		v_mad (centre, pos, cam.f, 9.0f);
		centre[1] += 1.0f;
	}
	cam.shake *= expf (-dt * 6.0f);
	for (int i = 0; i < 3; i++)
	{
		eye[i] += cam.shake * shake_rnd ();
	}
	v_copy (cam.eye, eye);

	/* wider the faster it goes, and with a pad's kick */
	float fov = FOVY + 13.0f * smooth (55.0f, 115.0f, speed) + (c->boost > 0.0f ? 5.0f : 0.0f);
	cam.fov += (fov - cam.fov) * (1.0f - expf (-dt * 3.0f));
}

/* ---- the controls ----------------------------------------------------------------------- */

static struct
{
	bool human;			/* the stick's craft (else it flies itself) */
	float idle;			/* seconds since anything was touched */
	int view;
	bool paused, smooth, again, next;
	unsigned was_buttons;
	float steer_until, brake_until, left_until, right_until, fire_until, key_steer;
	bool key_thrust;
} ui = {.smooth = true};

/* the stick, the controller and the console's keys, into the player's craft */
static void controls (bool pad_there, float t, float dt)
{
	craft_t *me = &race.craft[race.player];
	input_t in;
	bool touched = false;
	memset (&in, 0, sizeof in);

	for (int c; (c = getchar_timeout_us (0)) != PICO_ERROR_TIMEOUT; )
	{
		bool flown = true;
		switch (c)
		{
		case 'a': case 'A': ui.key_steer = -1.0f; ui.steer_until = t + KEY_HOLD_S; break;
		case 'd': case 'D': ui.key_steer = 1.0f; ui.steer_until = t + KEY_HOLD_S; break;
		case 'w': case 'W': ui.key_thrust = !ui.key_thrust; break;
		case 's': case 'S': ui.brake_until = t + KEY_HOLD_S; break;
		case 'q': case 'Q': ui.left_until = t + KEY_HOLD_S; break;
		case 'e': case 'E': ui.right_until = t + KEY_HOLD_S; break;
		case ' ': ui.fire_until = t + 0.1f; break;
		case 'v': case 'V': ui.view = (ui.view + 1) % VIEWS; flown = false; break;
		case 'p': case 'P': ui.paused = !ui.paused; flown = false; break;
		case 'o': case 'O': ui.human = false; ui.key_thrust = false; flown = false; break;
		case 'r': case 'R': ui.again = true; flown = false; break;
		case 'm': case 'M':
			ui.smooth = !ui.smooth;
			printf ("zerog: %s\n", ui.smooth ? "antialiased" : "not antialiased");
			flown = false;
			break;
		default: flown = false;
		}
		touched = touched || flown;
	}

	pad_t pad = {0};
	if (pad_there && pad_read (&pad))
	{
		unsigned pressed = pad.buttons & ~ui.was_buttons;
		ui.was_buttons = pad.buttons;
		touched = touched || pad.buttons || fabsf (pad.x) > 0.3f || fabsf (pad.y) > 0.3f;
		if (pressed & PAD_SELECT)
		{
			ui.view = (ui.view + 1) % VIEWS;
		}
		if (pressed & PAD_START)
		{
			if (race.phase == RESULTS)
				ui.next = true;
			else
				ui.paused = !ui.paused;
		}
		in.steer = pad.x * (0.55f + 0.45f * fabsf (pad.x));		/* (finer about the middle) */
		in.thrust = pad.buttons & PAD_B ? 1.0f : clampf (pad.y * 1.25f, 0.0f, 1.0f);
		float back = clampf (-pad.y * 1.25f - 0.15f, 0.0f, 1.0f);
		in.brake_l = pad.buttons & PAD_Y ? 1.0f : back;
		in.brake_r = pad.buttons & PAD_A ? 1.0f : back;
		in.fire = pad.buttons & (PAD_X | PAD_STICK);
	}
	if (t < ui.steer_until)
	{
		in.steer = ui.key_steer;
	}
	in.thrust = ui.key_thrust ? 1.0f : in.thrust;
	if (t < ui.brake_until)
	{
		in.brake_l = in.brake_r = 1.0f;
		in.thrust = 0.0f;
	}
	in.brake_l = t < ui.left_until ? 1.0f : in.brake_l;
	in.brake_r = t < ui.right_until ? 1.0f : in.brake_r;
	in.fire = in.fire || t < ui.fire_until;

	/* touched: the craft is the stick's; left alone long enough: its own again */
	ui.idle = touched || ui.key_thrust ? 0.0f : ui.idle + dt;
	if (touched && !ui.human)
	{
		ui.human = true;
		printf ("zerog: the craft is yours\n");
	}
	else if (ui.human && ui.idle > IDLE_SECONDS)
	{
		ui.human = false;
		printf ("zerog: the craft flies itself\n");
	}
	me->human = ui.human;
	me->in = in;
}

/* ---- the HUD ---------------------------------------------------------------------------- */

static void format_time (char *out, size_t size, float t)
{
	int cs = (int) (t * 100.0f + 0.5f);
	snprintf (out, size, "%d:%02d.%02d", cs / 6000, cs / 100 % 60, cs % 100);
}

static void centred (float y, const char *s, uint32_t colour, float scale, int width)
{
	hud_text_scaled (floorf ((width - strlen (s) * HUD_CHAR_W * scale) / 2), floorf (y), s, colour, scale);
}

/* the circuit from above, for the HUD's map: points along both routes, in a square (0 .. 1) */
#define MAP_MAIN	64
#define MAP_ALT		12
static float map_point[MAP_MAIN + MAP_ALT][2], map_lo[2], map_scale;

static void map_place (float out[2], const float *p)
{
	out[0] = (p[0] - map_lo[0]) * map_scale;
	out[1] = (p[2] - map_lo[1]) * map_scale;
}

static void map_init (void)
{
	const route_t *m = &route[ROUTE_MAIN], *a = &route[ROUTE_ALT];
	float hi[2] = {-1e9f, -1e9f};
	map_lo[0] = map_lo[1] = 1e9f;
	for (int i = 0; i < m->n; i++)
	{
		map_lo[0] = fminf (map_lo[0], m->rings[i].p[0]);
		map_lo[1] = fminf (map_lo[1], m->rings[i].p[2]);
		hi[0] = fmaxf (hi[0], m->rings[i].p[0]);
		hi[1] = fmaxf (hi[1], m->rings[i].p[2]);
	}
	map_scale = 1.0f / fmaxf (hi[0] - map_lo[0], hi[1] - map_lo[1]);
	map_lo[1] -= (1.0f / map_scale - (hi[1] - map_lo[1])) / 2;		/* (in the square's middle) */
	map_lo[0] -= (1.0f / map_scale - (hi[0] - map_lo[0])) / 2;
	for (int k = 0; k < MAP_MAIN; k++)
	{
		map_place (map_point[k], m->rings[k * m->n / MAP_MAIN].p);
	}
	for (int k = 0; k < MAP_ALT; k++)
	{
		map_place (map_point[MAP_MAIN + k], a->rings[(k + 1) * a->n / (MAP_ALT + 1)].p);
	}
}

/* the race on the left, the speed and the weapon at the bottom, what's to be
   told in the middle, the numbers at the top right, the map under them */
static void hud_build (const GLint vp[4], float t, float in_phase, const perf_t *perf)
{
	const craft_t *me = &race.craft[race.player];
	float hs = vp[3] >= 1000 ? 1.5f : vp[3] >= 600 ? 1.0f : 0.5f;	/* the font 3x, 2x, 1x */
	float lh = HUD_CHAR_H * hs * 1.15f, cw = HUD_CHAR_W * hs;
	char s[48], tm[16];
	int order[CRAFTS], pos_now = 1;
	race_standings (order);
	for (int i = 0; i < CRAFTS; i++)
	{
		if (order[i] == race.player)
			pos_now = i + 1;
	}
	float race_t = race.phase == GRID ? 0.0f : (me->place ? me->finish : t - race.start);
	float speed = craft_speed (me);
	hud_begin ();
	hud_rect (0, 0, 4 + 13 * cw, 4 + 5 * lh, HUD_RGBA (0, 0, 0, 140));
	hud_text_scaled (4, 3, team[race.player].name, team[race.player].hud, hs);
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

	/* the speed, and a bar of it (the pads' kick over the engine's own) */
	float by = vp[3] - 2 * HUD_CHAR_H * hs - 6;
	snprintf (s, sizeof s, "%3d KM/H", (int) lroundf (speed * 3.6f));
	hud_rect (0, by - 5 * hs, 4 + 8 * 2 * cw, 2 * HUD_CHAR_H * hs + 6 + 5 * hs, HUD_RGBA (0, 0, 0, 140));
	hud_text_scaled (4, by + 3, s, me->boost > 0.0f ? HUD_RGBA (255, 170, 40, 255) : HUD_RGBA (255, 255, 255, 255), 2 * hs);
	hud_rect (4, by - 3 * hs, clampf (speed / 120.0f, 0.0f, 1.0f) * (8 * 2 * cw - 4), 3 * hs,
		  speed > 84.0f ? HUD_RGBA (255, 170, 40, 255) : HUD_RGBA (60, 220, 240, 255));

	/* the weapon held (to fire); else what's at work: the mines going out, the shield */
	const char *weapon = me->weapon ? weapon_name (me->weapon) : me->mines ? "MINES" : me->shield > 0.0f ? "SHIELD" : "";
	if (*weapon)
	{
		float w = 9 * cw + 8, x = vp[2] - w, y = vp[3] - HUD_CHAR_H * hs * 1.5f - 6;
		uint32_t colour = me->weapon ? HUD_RGBA (255, 110, 240, 255) : me->mines ? HUD_RGBA (255, 90, 70, 255)
				: HUD_RGBA (60, 220, 240, 255);
		hud_rect (x, y, w, HUD_CHAR_H * hs * 1.5f + 6, HUD_RGBA (0, 0, 0, 150));
		hud_rect (x, y, w, 2 * hs, colour);
		hud_text_scaled (x + 4, y + 3 + 2 * hs, weapon, colour, 1.5f * hs);
	}

	/* the map: the circuit's dots, the craft on it (the player's the larger);
	   nothing behind it (blended pixels are dear: lights, above) */
	float ms = floorf (vp[3] * 0.25f), mx = vp[2] - ms - 3 * hs - 2, my = floorf (vp[3] * 0.40f), dot = 2 * hs;
	my -= ms * 0.14f;						/* (the circuit is wider than it's tall) */
	for (int k = 0; k < MAP_MAIN + MAP_ALT; k++)
	{
		hud_rect (floorf (mx + map_point[k][0] * ms), floorf (my + map_point[k][1] * ms), dot, dot,
			  k < MAP_MAIN ? HUD_RGBA (150, 160, 175, 255) : HUD_RGBA (90, 150, 110, 255));
	}
	for (int i = CRAFTS - 1; i >= 0; i--)
	{
		float at[2], size = i == race.player ? 3 * dot : 2 * dot;
		map_place (at, race.craft[i].p);
		hud_rect (floorf (mx + at[0] * ms - size / 2 + dot / 2), floorf (my + at[1] * ms - size / 2 + dot / 2), size, size,
			  i == race.player ? HUD_RGBA (255, 255, 255, 255) : team[i].hud);
	}

	/* the middle */
	float mid = floorf (vp[3] / 3.0f), bs = 4 * hs;
	if (ui.paused)
	{
		centred (mid, "PAUSED", HUD_RGBA (255, 255, 255, 255), bs, vp[2]);
	}
	else if (race.phase == GRID && in_phase > GRID_SECONDS - 3.0f)
	{
		snprintf (s, sizeof s, "%d", (int) ceilf (GRID_SECONDS - in_phase));
		centred (mid, s, HUD_RGBA (255, 220, 60, 255), bs, vp[2]);
	}
	else if (race.phase == RACE && t - race.start < 1.0f)
	{
		centred (mid, "GO", HUD_RGBA (255, 220, 60, 255), bs, vp[2]);
	}
	else if (race.phase == RACE && me->lost > 0.0f)
	{
		centred (mid, "RESCUE", HUD_RGBA (255, 90, 70, 255), 2 * hs, vp[2]);
	}
	else if (race.phase == RACE && me->wrong_way && ui.human)
	{
		centred (mid, "WRONG WAY", HUD_RGBA (255, 90, 70, 255), 2 * hs, vp[2]);
	}
	else if (race.phase == RACE && me->lap == LAPS - 1 && t - race.start - me->lap_start < 2.0f)
	{
		centred (mid, "FINAL LAP", HUD_RGBA (255, 220, 60, 255), 2 * hs, vp[2]);
	}
	if (race.phase == RACE && race_hunted (race.player) && ((int) (t * 6.0f) & 1))
	{
		centred (mid + 2.5f * lh, "MISSILE", HUD_RGBA (255, 90, 70, 255), 2 * hs, vp[2]);
	}
	if (!ui.human && race.phase != RESULTS && ((int) (t * 1.5f) & 1))
	{
		centred (vp[3] - 4.6f * lh, "DEMO - TOUCH THE STICK", HUD_RGBA (255, 255, 255, 255), hs, vp[2]);
	}
	if (race.phase == RESULTS)
	{
		float x0 = floorf ((vp[2] - 20 * cw) / 2), y0 = floorf (vp[3] / 5.0f);
		hud_rect (x0 - 6, y0 - 6, 20 * cw + 12, (CRAFTS + 1.5f) * lh + 12, HUD_RGBA (0, 0, 0, 170));
		hud_text_scaled (x0, y0, "RESULTS", HUD_RGBA (255, 220, 60, 255), hs);
		for (int i = 0; i < CRAFTS; i++)
		{
			const craft_t *c = &race.craft[order[i]];
			if (c->place)
				format_time (tm, sizeof tm, c->finish);
			else
				strcpy (tm, "-:--.--");
			snprintf (s, sizeof s, "%d %-8s %s", i + 1, team[order[i]].name, tm);
			hud_text_scaled (x0, floorf (y0 + (i + 1.5f) * lh), s, team[order[i]].hud, hs);
		}
	}
	hud_perf (vp[2] - hud_perf_width (hud_perf_scale (vp[3])) - 2, 2, hud_perf_scale (vp[3]), perf);
	hud_end ();
}

/* ---- the road's pieces that are seen ------------------------------------------------------- */

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
	for (int k = 0; k < 5; k++)
	{
		if (planes[k][0] * c[0] + planes[k][1] * c[1] + planes[k][2] * c[2] + planes[k][3] < -radius)
		{
			return false;
		}
	}
	return true;
}

/* the pieces seen, a kind after another: pieces in a row are one draw */
static int draw_track (void)
{
	bool show[MAX_CHUNKS];
	int draws = 0;
	for (int c = 0; c < art.chunks; c++)
	{
		show[c] = seen (art.chunk[c].c, art.chunk[c].radius);
	}
	glBindBuffer (GL_ARRAY_BUFFER, art.track);
	scenery_pointers ();
	for (int kind = 0; kind < KINDS; kind++)
	{
		/* the road and the deck: their faces only (the walls are seen from both sides) */
		(kind == K_WALL ? glDisable : glEnable) (GL_CULL_FACE);
		glBindTexture (GL_TEXTURE_2D, art.t_kind[kind]);
		for (int c = 0; c < art.chunks; c++)
		{
			if (!show[c] || art.chunk[c].count[kind] <= 2)
			{
				continue;
			}
			int last = c;
			while (last + 1 < art.chunks && show[last + 1])
			{
				last++;
			}
			int first = art.chunk[c].first[kind] + 2;	/* (past what joins it to the piece before) */
			glDrawArrays (GL_TRIANGLE_STRIP, first, art.chunk[last].first[kind] + art.chunk[last].count[kind] - first);
			draws++;
			c = last;
		}
	}
	glDisable (GL_CULL_FACE);
	return draws;
}

/* ---- pictures of it, on a PC ----------------------------------------------------------------- */

#if ZEROG_SHOT
/* ZEROG_SHOT=WIDTHxHEIGHT:PREFIX:SECONDS[,SECONDS...] (a PC): the race flown by
   the craft themselves in steps of 1/60 s, drawn off screen; the picture at
   each of those moments is written to PREFIX-SECONDS.ppm, and after the last
   it ends. For looking at what it draws without the screen */
static struct
{
	bool on, blast;
	int w, h, n, next, blasts;
	char prefix[200];
	float at[32];
} shot;

static void shot_init (void)
{
	const char *what = getenv ("ZEROG_SHOT");
	int used = 0;
	if (!what)
	{
		return;
	}
	if (sscanf (what, "%dx%d:%199[^:]:%n", &shot.w, &shot.h, shot.prefix, &used) < 3 || !used)
	{
		printf ("zerog: ZEROG_SHOT=WIDTHxHEIGHT:PREFIX:SECONDS[,SECONDS...]\n");
		exit (2);
	}
	for (const char *p = what + used; *p && shot.n < 32; p += strcspn (p, ","), p += *p == ',')
	{
		shot.at[shot.n++] = (float) atof (p);
	}
	shot.on = true;
}

/* the frame just drawn, into a file */
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
	printf ("zerog: %s (GL error 0x%x)\n", name, (unsigned) glGetError ());
}
#endif

/* ---- all of it ------------------------------------------------------------------------- */

int main (void)
{
	stdio_init_all ();
	pgpu_init ();
	printf ("\nzerog: waiting for the RPi (READY)...\n");
	while (!pgpu_wait_ready (1000))
	{
	}
	pgpu_set_reply_phase (1);
	int tries = 0;
#if ZEROG_SHOT
	shot_init ();
	if (shot.on && !pglInitSurface (shot.w, shot.h))
	{
		printf ("zerog: no surface of %dx%d\n", shot.w, shot.h);
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
	GLint vp[4] = {0};

	const float sun_el = 32.0f * PI / 180, sun_az = -40.0f * PI / 180;
	v_set (sun, cosf (sun_el) * sinf (sun_az), sinf (sun_el), -cosf (sun_el) * cosf (sun_az));
	track_build ();
	art_build (sun);
	programs_init ();

	/* what changes every frame: the shadows' quads, the lights', the sky's */
	GLuint shadow_buffer, flare_buffer, quad_buffer;
	static vertex_t shadows[CRAFTS * 6];
	glGenBuffers (1, &shadow_buffer);
	glBindBuffer (GL_ARRAY_BUFFER, shadow_buffer);
	glBufferData (GL_ARRAY_BUFFER, sizeof shadows, NULL, GL_DYNAMIC_DRAW);
	glGenBuffers (1, &flare_buffer);
	glBindBuffer (GL_ARRAY_BUFFER, flare_buffer);
	glBufferData (GL_ARRAY_BUFFER, sizeof flares, NULL, GL_DYNAMIC_DRAW);
	glGenBuffers (1, &quad_buffer);
	glBindBuffer (GL_ARRAY_BUFFER, quad_buffer);
	glBufferData (GL_ARRAY_BUFFER, 4 * 5 * sizeof (float), NULL, GL_DYNAMIC_DRAW);

	glDisable (GL_DITHER);
	glDisable (GL_CULL_FACE);
	if (!hud_init ())
	{
		printf ("zerog: the HUD program didn't link\n");
	}
	bool pad_there = pad_init ();
	map_init ();
	int pads[3] = {0, 0, 0};
	for (int r = 0; r < ROUTES; r++)
		for (int tile = 0; tile < route[r].n / TILE_RINGS; tile++)
		{
			pads[route[r].pads[tile][0]]++;
			pads[route[r].pads[tile][1]]++;
		}
	printf ("zerog: a circuit of %.0f m (%d rings), a jump over %.0f m, a fork of %.0f m for %.0f m of it, %d speed pads, "
		"%d weapon pads, a tunnel of %.0f m; %d pieces of road, %d scenery vertices (%d pylons, %d towers, %d hoops); "
		"%d craft, %d laps\n", route[ROUTE_MAIN].len, route[ROUTE_MAIN].n, track_features.gap_rings * route[ROUTE_MAIN].step,
		route[ROUTE_ALT].len, route[ROUTE_ALT].to - route[ROUTE_ALT].from, pads[PAD_BOOST], pads[PAD_WEAPON],
		track_features.tunnel_rings * route[ROUTE_MAIN].step, art.chunks, art.prop_vertices, art.pylons, art.towers,
		art.hoops, CRAFTS, LAPS);
	printf ("zerog: the stick: the nose, forward the engine, back the brakes; B the engine, Y A the air brakes, X fire, "
		"SELECT the view, START a pause. Keys: a d w s q e, space, v p o r m\n");

	/* the race */
	absolute_time_t last = get_absolute_time ();
	float t = 0.0f;					/* the race's clock: stopped by a pause */
#if ZEROG_SHOT
	if (shot.on && getenv ("ZEROG_DAY"))		/* (the pictures: by day) */
	{
		sky_now = &skies[0];
	}
	if (!shot.on)
#endif
	sound_init ();
	race_reset (t, ui.human);
	apply_sky ();
	float identity[16];
	mat4_identity (identity);
	perf_t perf;
	memset (&perf, 0, sizeof perf);
	unsigned frame = 0, windows = 0;
	bool new_perf = true;
	int last_phase = -1, track_draws = 0;
	const float up[3] = {0, 1, 0};

	/* One frame in flight: the next frame is made (the race flown, its commands
	   sent) while the RPi renders this one: after a frame is sent, the one
	   before it is waited for. The RPi's FRAME pulses count the frames shown;
	   over USB there are none: the RPi is asked */
	const bool pulses = strcmp (pgpu_link_name (), "USB") != 0;
	uint32_t sent = 0, shown_base = pgpu_frame_count ();

	printf ("zerog: antialiased (4 samples a pixel); m: without, and with again\n");
	while (true)
	{
		pglSamples (ui.smooth ? 4 : 1);
		bool resized = screen_update ("zerog", vp);
		float aspect = (float) vp[2] / vp[3];
		if (resized)
		{
			glUseProgram (gl.sky);
			glUniform1f (gl.s_aspect, aspect);
		}
		absolute_time_t now = get_absolute_time ();
		float dt = absolute_time_diff_us (last, now) / 1e6f;
		dt = dt > 1.0f / 20 ? 1.0f / 20 : dt;
		last = now;
#if ZEROG_SHOT
		dt = shot.on ? 1.0f / 60 : dt;
#endif

		/* the controls, the race */
		controls (pad_there, t, dt);
		craft_t *me = &race.craft[race.player];
		if (!ui.paused)
		{
			t += dt;
			if (race_step (dt, t) || ui.again || (ui.next && race.phase == RESULTS))
			{
				race_reset (t, ui.human);
				sky_now = &skies[(race.races + 1) % 2];	/* day first */
				apply_sky ();
				cam.set = false;
			}
		}
		ui.again = ui.next = false;
		unsigned ev = me->events;
		me->events = 0;
		if (ev & (EV_HIT | EV_WALL | EV_BUMP))
		{
			cam.shake = fmaxf (cam.shake, ev & EV_HIT ? 0.5f : 0.12f);
		}
#if ZEROG_SHOT
		if (shot.on)				/* (unheard, and drawn only for the pictures) */
		{
			float eye[3], centre[3];
			for (int i = 0; i < CRAFTS; i++)
			{
				race.craft[i].events = 0;
			}
			shot.blast = false;			/* (ZEROG_BLAST: and of each explosion near, ahead) */
			for (int i = 0; i < BLASTS && getenv ("ZEROG_BLAST"); i++)
			{
				float d[3];
				v_sub (d, race.blast[i].p, me->p);
				shot.blast = shot.blast || (race.blast[i].age > 0.08f && race.blast[i].age < 0.40f && v_len (d) < 70.0f
							   && v_dot (d, me->fwd) > 8.0f);
			}
			if (t < shot.at[shot.next] - 0.05f && !shot.blast)
			{
				camera (me, dt, ui.view, eye, centre);
				continue;
			}
		}
		else
#endif
		sound_update (t, t - race.phase_start, ev, ui.paused);

		/* the camera */
		float eye[3], centre[3], projection[16], view[16], vpm[16];
		camera (me, ui.paused ? 0.0f : dt, ui.view, eye, centre);
		mat4_perspective (projection, cam.fov, aspect, 0.4f, 1600.0f);
		mat4_look_at (view, eye, centre, up);
		mat4_multiply (vpm, projection, view);
		planes_from (vpm);
		v_set (cam_right, view[0], view[4], view[8]);
		v_set (cam_up, view[1], view[5], view[9]);
		v_copy (cam_at, eye);

		/* the horizon's row and the sun's place on the screen */
		float hf[3] = {-view[2], 0.0f, -view[10]};
		v_norm (hf);
		const float far[3] = {eye[0] + 1000.0f * hf[0], eye[1], eye[2] + 1000.0f * hf[2]};
		float horizon = (vpm[1] * far[0] + vpm[5] * far[1] + vpm[9] * far[2] + vpm[13])
			      / (vpm[3] * far[0] + vpm[7] * far[1] + vpm[11] * far[2] + vpm[15]);
		float sx = vpm[0] * sun[0] + vpm[4] * sun[1] + vpm[8] * sun[2];
		float sy = vpm[1] * sun[0] + vpm[5] * sun[1] + vpm[9] * sun[2];
		float sw = vpm[3] * sun[0] + vpm[7] * sun[1] + vpm[11] * sun[2];

		/* near to far: the craft and what they fire, the road, the scenery,
		   the ground, then the sky, behind all of it */
		glClear (GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
		glEnable (GL_DEPTH_TEST);

		float models[CRAFTS][16];
		int shadow_n = 0;
		flare_n = 0;
		glUseProgram (gl.cp);
		glUniformMatrix4fv (gl.c_vp, 1, GL_FALSE, vpm);
		glUniform3fv (gl.c_eye, 1, eye);
		glActiveTexture (GL_TEXTURE0);
		glBindBuffer (GL_ARRAY_BUFFER, art.meshes);
		arrays (gl.c_pos, gl.c_normal, gl.c_uv);
		glVertexAttribPointer (gl.c_pos, 3, GL_FLOAT, GL_FALSE, sizeof (craft_vertex_t), (void *) 0);
		glVertexAttribPointer (gl.c_normal, 3, GL_FLOAT, GL_FALSE, sizeof (craft_vertex_t), (void *) 12);
		glVertexAttribPointer (gl.c_uv, 2, GL_FLOAT, GL_FALSE, sizeof (craft_vertex_t), (void *) 24);
		for (int i = 0; i < CRAFTS; i++)
		{
			const craft_t *c = &race.craft[i];
			float d[3], dist;
			craft_model (c, t, models[i]);
			v_sub (d, c->p, eye);
			dist = v_len (d);
			if (!(i == race.player && ui.view == VIEW_NOSE && !c->lost) && seen (c->p, 4.0f))
			{
				glUniformMatrix4fv (gl.c_model, 1, GL_FALSE, models[i]);
				glBindTexture (GL_TEXTURE_2D, art.t_livery[i]);
				glDrawArrays (GL_TRIANGLES, art.craft_first, art.craft_count);

				/* its engines' light (more with the throttle, blue with a pad's kick), its shield */
				if (dist < 160.0f)
				{
					float glow = 0.35f + 0.65f * c->throttle, size = 0.36f + 0.22f * c->throttle + (c->boost > 0.0f ? 0.25f : 0.0f);
					for (int s = -1; s <= 1; s += 2)
					{
						float p[3];
						model_point (p, models[i], s * 1.3f, 0.25f, 2.7f);
						if (c->boost > 0.0f)
							flare (p, size, 0.45f, 0.75f, 1.0f);
						else
							flare (p, size, glow, 0.62f * glow, 0.28f * glow);
					}
				}
			}
			if (c->shield > 0.0f)
			{
				float k = (0.75f + 0.25f * sinf (t * 14.0f)) * fminf (c->shield, 1.0f);
				flare_ring (c->p, 2.9f, 0.5f, 0.3f * k, 0.9f * k, 1.0f * k);
			}

			/* its shadow on the road under it (where there is road) */
			if (!c->lost && track_road (&c->at, c->x) && c->h > -0.5f && c->h < 9.0f)
			{
				float rt[3], on[3], k = 1.0f + 0.12f * c->h;
				static const float corner[6][2] = {{-1, 1}, {1, 1}, {-1, 0}, {1, 1}, {1, 0}, {-1, 0}};
				v_cross (rt, c->fwd, c->f.u);
				v_mad (on, c->p, c->f.u, 0.06f - c->h);
				for (int v = 0; v < 6; v++)
				{
					vertex_t *o = &shadows[shadow_n++];
					float along = corner[v][1] ? 3.6f : -2.7f;
					o->x = on[0] + k * (rt[0] * corner[v][0] * 1.9f + c->fwd[0] * along);
					o->y = on[1] + k * (rt[1] * corner[v][0] * 1.9f + c->fwd[1] * along);
					o->z = on[2] + k * (rt[2] * corner[v][0] * 1.9f + c->fwd[2] * along);
					o->u = 0.5f + 0.5f * corner[v][0];
					o->v = corner[v][1];
					o->shade = 1.0f;
				}
			}
		}

		/* what's been fired: the rockets, the mines, each with its light */
		glBindTexture (GL_TEXTURE_2D, art.t_ordnance);
		for (int i = 0; i < SHOTS; i++)
		{
			const shot_t *s = &race.shot[i];
			float m[16];
			if (!s->live || !seen (s->p, 3.0f))
			{
				continue;
			}
			shot_model (s, m);
			glUniformMatrix4fv (gl.c_model, 1, GL_FALSE, m);
			if (s->type == SHOT_MINE)
			{
				float blink = 0.5f + 0.5f * sinf (s->age * 9.0f + i);
				glDrawArrays (GL_TRIANGLES, art.mine_first, art.mine_count);
				flare (s->p, 0.5f, 0.9f * blink, 0.12f * blink, 0.08f * blink);
			}
			else
			{
				float p[3];
				glDrawArrays (GL_TRIANGLES, art.shot_first, art.shot_count);
				model_point (p, m, 0.0f, 0.0f, 0.8f);
				if (s->type == SHOT_MISSILE)
					flare (p, 0.75f, 1.0f, 0.35f, 0.9f);
				else
					flare (p, 0.65f, 1.0f, 0.7f, 0.3f);
			}
		}
		/* the explosions: fire, of solid spikes that swell and go (first a
		   flash), a light in it, and a ring of light going out */
		for (int i = 0; i < BLASTS; i++)
		{
			const blast_t *b = &race.blast[i];
			float k = b->age / 0.6f, m[16];
			if (k >= 1.0f || !seen (b->p, 8.0f))
			{
				continue;
			}
			float size = 9.0f * (1.0f - (1.0f - k) * (1.0f - k) * (1.0f - k)) * (1.0f - k * k), fade = (1.0f - k) * (1.0f - k);
			for (int j = 0; j < 3; j++)
			{
				fire_model (m, b->p, size, j, 5.0f * b->age + i);
				glUniformMatrix4fv (gl.c_model, 1, GL_FALSE, m);
				glDrawArrays (GL_TRIANGLES, k < 0.2f ? art.flash_first : art.fire_first, art.mine_count);
			}
			flare (b->p, 1.5f + 6.0f * k, fade, 0.6f * fade, 0.25f * fade);
			flare_ring (b->p, 2.0f + 16.0f * k, 0.9f, 0.9f * fade, 0.7f * fade, 0.5f * fade);
		}

		/* the road, its walls and deck, the scenery by it, the ground */
		glUseProgram (gl.sc);
		glUniformMatrix4fv (gl.sc_vp, 1, GL_FALSE, vpm);
		glUniformMatrix4fv (gl.sc_model, 1, GL_FALSE, identity);
		glUniform3fv (gl.sc_eye, 1, eye);
		track_draws = draw_track ();

		glBindBuffer (GL_ARRAY_BUFFER, art.props);
		scenery_pointers ();
		for (int part = 0; part < PARTS; part++)
		{
			if (art.part_count[part])
			{
				glBindTexture (GL_TEXTURE_2D, art.t_part[part]);
				glDrawArrays (GL_TRIANGLES, art.part_first[part], art.part_count[part]);
			}
		}

		glBindBuffer (GL_ARRAY_BUFFER, art.ground);
		glBindBuffer (GL_ELEMENT_ARRAY_BUFFER, art.ground_index);
		scenery_pointers ();
		glBindTexture (GL_TEXTURE_2D, art.t_ground);
		glDrawElements (GL_TRIANGLES, art.ground_indices, GL_UNSIGNED_SHORT, (void *) 0);

		/* the sky above the horizon, at the far plane: only where nothing is */
		float bottom = clampf (horizon - 0.02f, -1.0f, 1.0f);
		if (bottom < 1.0f)
		{
			/* the corners, and (at night, for the stars) the direction seen at each */
			float corners[4][5], ty = tanf (cam.fov * PI / 360), tx = ty * aspect;
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
				glUseProgram (gl.night);
				glUniform1f (gl.n_horizon, horizon);
				glBindTexture (GL_TEXTURE_2D, art.t_stars);
				arrays (gl.n_pos, gl.n_dir, -1);
				glVertexAttribPointer (gl.n_pos, 2, GL_FLOAT, GL_FALSE, sizeof corners[0], (void *) 0);
				glVertexAttribPointer (gl.n_dir, 3, GL_FLOAT, GL_FALSE, sizeof corners[0], (void *) 8);
			}
			else
			{
				glUseProgram (gl.sky);
				glUniform1f (gl.s_horizon, horizon);
				glUniform2f (gl.s_sun, sw > 0.01f ? sx / sw : 10.0f, sw > 0.01f ? sy / sw : 10.0f);
				arrays (gl.s_pos, -1, -1);
				glVertexAttribPointer (gl.s_pos, 2, GL_FLOAT, GL_FALSE, sizeof corners[0], (void *) 0);
			}
			glDepthMask (GL_FALSE);
			glDrawArrays (GL_TRIANGLE_STRIP, 0, 4);
			glDepthMask (GL_TRUE);
		}

		/* the shadows, blended onto the road; the lights, added */
		glEnable (GL_BLEND);
		glDepthMask (GL_FALSE);
		if (shadow_n)
		{
			glUseProgram (gl.sc);
			glBindBuffer (GL_ARRAY_BUFFER, shadow_buffer);
			glBufferSubData (GL_ARRAY_BUFFER, 0, shadow_n * (GLsizeiptr) sizeof (vertex_t), shadows);
			scenery_pointers ();
			glBindTexture (GL_TEXTURE_2D, art.t_shadow);
			glBlendFunc (GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
			glDrawArrays (GL_TRIANGLES, 0, shadow_n);
		}
		if (flare_n)
		{
			glUseProgram (gl.fl);
			glUniformMatrix4fv (gl.f_vp, 1, GL_FALSE, vpm);
			glUniform3fv (gl.f_eye, 1, eye);
			glBindBuffer (GL_ARRAY_BUFFER, flare_buffer);
			glBufferSubData (GL_ARRAY_BUFFER, 0, flare_n * (GLsizeiptr) sizeof (flare_vertex_t), flares);
			arrays (gl.f_pos, gl.f_uv, gl.f_color);
			glVertexAttribPointer (gl.f_pos, 3, GL_FLOAT, GL_FALSE, sizeof (flare_vertex_t), (void *) 0);
			glVertexAttribPointer (gl.f_uv, 2, GL_FLOAT, GL_FALSE, sizeof (flare_vertex_t), (void *) 12);
			glVertexAttribPointer (gl.f_color, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof (flare_vertex_t), (void *) 20);
			glBindTexture (GL_TEXTURE_2D, art.t_flare);
			glBlendFunc (GL_ONE, GL_ONE);
			glDrawArrays (GL_TRIANGLES, 0, flare_n);
		}
		glDepthMask (GL_TRUE);
		glDisable (GL_BLEND);

		/* the HUD (10 times a second) */
		if (new_perf || frame % 6 == 0 || race.phase != last_phase)
		{
			hud_build (vp, t, t - race.phase_start, &perf);
			last_phase = race.phase;
		}
		hud_draw ();
#if ZEROG_SHOT
		if (shot.on)
		{
			if (shot.blast && shot.blasts < 12)
			{
				shot.blasts++;
				shot_write (floorf (t * 100.0f) / 100.0f);
			}
			if (t >= shot.at[shot.next])
			{
				shot_write (shot.at[shot.next]);
				if (++shot.next == shot.n)
				{
					return 0;
				}
			}
			pglSwapBuffers ();
			frame++;
			continue;
		}
#endif
		pglSwapBuffers ();

		sent++;
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
			int order[CRAFTS];
			race_standings (order);
			perf_log_link ("zerog", &perf);
			GLenum e = glGetError ();
			printf ("zerog: %.1f fps, load GPU %.0f%% CPU-G %.0f%% CPU-H %.0f%%, render %.2f ms; %d draws of road, %d lights; "
				"%s (%s) lap %d, %.0f km/h, leader %s; GL error 0x%x\n", perf.fps, perf.gpu * 100, perf.cpu_g * 100,
				perf.cpu_h * 100, perf.render_ms, track_draws, flare_n / 6, team[race.player].name,
				ui.human ? "the stick's" : "flying itself", me->lap < 0 ? 1 : me->lap + 1 > LAPS ? LAPS : me->lap + 1,
				craft_speed (me) * 3.6f, team[order[0]].name, (unsigned) e);
		}
	}
}
