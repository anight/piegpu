/*
 * isles - the BSP engine (engine/) in the sky: islands floating in a summer
 * sky (engine/levels/isles.bsp, made by engine/tools/make_isles.py), a
 * platform carrying the player across the void (func_train), a jump pad
 * throwing them up to a high island (trigger_push), a pond spilling over its
 * edge, a bridge down to ruins and a portal home (trigger_teleport); eight
 * coins round the way, one in the jump's arc. All eight: the time, and
 * they're back. A fall into the void: back to the start.
 *
 * The keys (engine/keys.h): w s a d, the arrows or q e, space. Without keys
 * for PILOT_S the autopilot starts over and goes round
 * (engine/levels/isles_path.h).
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
#include "bsp.h"
#include "render.h"
#include "game.h"
#include "keys.h"
#include "levels/isles_path.h"
#include "pickups.h"

extern const uint8_t level_file[], level_file_end[];

#define FOVY		66.0f
#define PILOT_S		10.0f		/* without keys: the autopilot */
#define DONE_S		4.0f		/* "all the coins" shown */
#define FELL_S		1.5f		/* the fall's white */
#define PICK_S		0.25f		/* a coin's flash */
#define VOID_Z		-400.0f		/* under the islands: fallen */

static const float sun[3] = {-0.29f, -0.50f, 0.82f};	/* towards it: isles.map's _sun_mangle 60 -55 */

static void centered (int w, float y, const char *s, uint32_t rgba, float scale)
{
	hud_text_scaled ((float) (int) ((w - strlen (s) * HUD_CHAR_W * scale) / 2), y, s, rgba, scale);
}

int main (void)
{
	stdio_init_all ();
	pgpu_init ();
	keys_t keys;
	keys_init (&keys);
	keys.idle = PILOT_S;
	printf ("\nisles: waiting for the RPi (READY)...\n");
	while (!pgpu_wait_ready (1000))
	{
	}
	pgpu_set_reply_phase (1);
	int tries = 0;
	while (!pglInit () && ++tries < 5)		/* the first reply can be missed */
	{
	}

	static bsp_t bsp;
	if (!bsp_load (&bsp, level_file, (size_t) (level_file_end - level_file)))
	{
		return 1;
	}
	printf ("isles: the level: %d faces, %d leaves, %d models, %u bytes\n", (int) bsp.n_faces, (int) bsp.n_leaves,
		(int) bsp.n_models, (unsigned) bsp.size);
	static render_t r;
	static game_t g;
	if (!render_init (&r, &bsp) || !game_init (&g, &bsp))
	{
		printf ("isles: the level doesn't fit\n");
		return 1;
	}
	if (!hud_init ())
	{
		printf ("isles: the HUD program didn't link\n");
	}
	static pickups_t coins;
	pickups_init (&coins, PICKUPS_COIN);
	printf ("isles: %d coins; keys: w s a d, the arrows, q e, space; the autopilot after %.0f s without\n", g.n_items, PILOT_S);
	glClearColor (0.45f, 0.65f, 0.95f, 1.0f);
	glDisable (GL_CULL_FACE);
	glDisable (GL_DITHER);

	game_pilot_t pilot;
	game_pilot_init (&pilot, isles_path, ISLES_PATH);
	GLint vp[4] = {0};
	float projection[16];
	absolute_time_t last = get_absolute_time ();
	perf_t perf;
	memset (&perf, 0, sizeof perf);
	unsigned windows = 0, frame = 0;
	bool new_perf = true, was_pilot = true;
	float t = 0.0f, run = 0.0f, best = 0.0f, done = 0.0f, fell = 0.0f, pick = 0.0f, done_time = 0.0f;
	int pick_item = 0, rounds = 0, deaths = 0;
	while (true)
	{
		if (screen_update ("isles", vp))
		{
			mat4_perspective (projection, FOVY, (float) vp[2] / vp[3], 4.0f, 8192.0f);
		}
		absolute_time_t now = get_absolute_time ();
		float dt = absolute_time_diff_us (last, now) / 1e6f;
		dt = dt > 0.05f ? 0.05f : dt;
		last = now;
		t += dt;
		r.time = t;

		/* the input: the keys, or the autopilot without them (starting over) */
		game_input_t in;
		keys_input (&keys, &g, &in, dt);
		bool pilot_on = keys.idle >= PILOT_S;
		if (pilot_on && !was_pilot)
		{
			game_respawn (&g);
			game_items_reset (&g);
			game_pilot_init (&pilot, isles_path, ISLES_PATH);
			run = 0.0f;
		}
		was_pilot = pilot_on;
		if (pilot_on)
		{
			game_pilot (&pilot, &g, &in, dt);
		}
		game_update (&g, &in, dt);

		/* the coins, the fall, the round's end */
		bool hud_now = false;
		run += done > 0.0f ? 0.0f : dt;
		if (g.picked >= 0)
		{
			pick = PICK_S;
			pick_item = g.picked;
			hud_now = true;
			if (g.n_taken == g.n_items)
			{
				done = DONE_S;
				done_time = run;
				best = best == 0.0f || run < best ? run : best;
				rounds++;
				printf ("isles: all %d coins in %.1f s (best %.1f s), %s\n", g.n_items, run, best, pilot_on ? "auto" : "keys");
			}
		}
		if (g.origin[2] < VOID_Z)
		{
			game_respawn (&g);
			game_pilot_init (&pilot, isles_path, ISLES_PATH);
			fell = FELL_S;
			deaths++;
			hud_now = true;
		}
		if (done > 0.0f && (done -= dt) <= 0.0f)
		{
			game_items_reset (&g);
			run = 0.0f;
			hud_now = true;
		}
		fell = fell > dt ? fell - dt : 0.0f;
		pick = pick > dt ? pick - dt : 0.0f;

		/* the view */
		float eye[3], view[16], vpm[16];
		game_eye (&g, eye);
		const float centre[3] = {eye[0] + cosf (g.yaw) * cosf (g.pitch), eye[1] + sinf (g.yaw) * cosf (g.pitch),
					 eye[2] + sinf (g.pitch)};
		const float up[3] = {0, 0, 1};
		mat4_look_at (view, eye, centre, up);
		mat4_multiply (vpm, projection, view);

		glClear (GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
		glEnable (GL_DEPTH_TEST);
		glDisable (GL_BLEND);
		render_world (&r, eye, vpm);
		for (int k = 0; k < g.n_movers; k++)
		{
			render_model (&r, g.movers[k].model, g.movers[k].at, vpm);
		}
		pickups_draw (&coins, &g, vpm, eye, g.yaw, g.pitch, t, sun);

		if (hud_now || new_perf || frame % 6 == 0 || pick > 0.0f || fell > 0.0f || done > 0.0f)
		{
			char s[64];
			float hs = vp[3] >= 1000 ? 1.5f : vp[3] >= 600 ? 1.0f : 0.5f;
			hud_begin ();
			if (fell > 0.0f)
			{
				hud_rect (0, 0, vp[2], vp[3], HUD_RGBA (255, 255, 255, (int) (200 * fell / FELL_S)));
				centered (vp[2], vp[3] * 0.4f, "FELL!", HUD_RGBA (40, 60, 120, 255), hs * 2);
			}
			if (pick > 0.0f)
			{
				const float *c = pickups_color (&coins, pick_item);
				hud_rect (0, 0, vp[2], vp[3], HUD_RGBA ((int) (c[0] * 255), (int) (c[1] * 255), (int) (c[2] * 255),
									(int) (90 * pick / PICK_S)));
			}
			if (done > 0.0f)
			{
				snprintf (s, sizeof s, "ALL %d COINS", g.n_items);
				centered (vp[2], vp[3] * 0.33f, s, HUD_RGBA (255, 220, 90, 255), hs * 2);
				snprintf (s, sizeof s, "%d:%04.1f", (int) done_time / 60, fmodf (done_time, 60.0f));
				centered (vp[2], vp[3] * 0.33f + HUD_CHAR_H * hs * 2.4f, s, HUD_RGBA (255, 255, 255, 255), hs * 2);
			}
			snprintf (s, sizeof s, "COINS %d/%d  %d:%04.1f", g.n_taken, g.n_items, (int) run / 60, fmodf (run, 60.0f));
			hud_text_scaled (4, 4, s, HUD_RGBA (255, 220, 90, 255), hs);
			if (best > 0.0f)
				snprintf (s, sizeof s, "%s  BEST %d:%04.1f", pilot_on ? "AUTO" : "KEYS", (int) best / 60, fmodf (best, 60.0f));
			else
				snprintf (s, sizeof s, "%s", pilot_on ? "AUTO" : "KEYS");
			hud_text_scaled (4, 4 + HUD_CHAR_H * hs * 1.2f, s,
					 pilot_on ? HUD_RGBA (120, 220, 255, 255) : HUD_RGBA (255, 255, 255, 255), hs * 0.667f);
			hud_perf (vp[2] - hud_perf_width (0.5f) - 2, 2, 0.5f, &perf);
			hud_end ();
		}
		hud_draw ();
		pglSwapBuffers ();

		absolute_time_t wait_start = get_absolute_time ();
		pgpu_wait_frame (100);
		new_perf = perf_frame (absolute_time_diff_us (wait_start, get_absolute_time ()), &perf);
		frame++;
		if (new_perf && ++windows % 5 == 0)
		{
			perf_log_link ("isles", &perf);
			printf ("isles: %.1f fps, GPU %.0f%% CPU-H %.0f%%, render %.2f ms; %u faces in %u draws, %u leaves; "
				"%s at %.0f %.0f %.0f, %d/%d coins, %d rounds, %d falls; GL error 0x%x\n", perf.fps, perf.gpu * 100,
				perf.cpu_h * 100, perf.render_ms, r.stats.faces, r.stats.draws, r.stats.leaves, pilot_on ? "auto" : "keys",
				g.origin[0], g.origin[1], g.origin[2], g.n_taken, g.n_items, rounds, deaths, (unsigned) glGetError ());
		}
	}
}
