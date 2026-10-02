/*
 * keep - the BSP engine (engine/) outdoors: a castle at dusk
 * (engine/levels/keep.bsp, made by engine/tools/make_keep.py), its sky's
 * clouds drifting and its moat of lava swaying (engine/render.c), and nine
 * gems to find round it: across the bridge, through the portcullis, up the
 * lift, over the sky bridge, along the walls. All nine: the time, and they're
 * back. The lava: back to the start. With sounds (engine/sound.c): the wind,
 * the lava bubbling where it's near, steps, the lift and the portcullis, a
 * gem's chime.
 *
 * The keys (engine/keys.h): w s a d, the arrows or q e, space. Without keys
 * for PILOT_S the autopilot starts over and goes round
 * (engine/levels/keep_path.h).
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
#include "sound.h"
#include "levels/keep_path.h"
#include "pickups.h"

extern const uint8_t level_file[], level_file_end[];

#define FOVY		66.0f
#define PILOT_S		10.0f		/* without keys: the autopilot */
#define DONE_S		4.0f		/* "all the gems" shown */
#define LAVA_S		1.5f		/* the lava's red */
#define PICK_S		0.3f		/* a gem's flash */

static const float sun[3] = {-0.83f, -0.30f, 0.47f};	/* towards it: keep.map's _sun_mangle 20 -28 */

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
	printf ("\nkeep: waiting for the RPi (READY)...\n");
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
	printf ("keep: the level: %d faces, %d leaves, %d models, %u bytes\n", (int) bsp.n_faces, (int) bsp.n_leaves,
		(int) bsp.n_models, (unsigned) bsp.size);
	static render_t r;
	static game_t g;
	if (!render_init (&r, &bsp) || !game_init (&g, &bsp))
	{
		printf ("keep: the level doesn't fit\n");
		return 1;
	}
	if (!hud_init ())
	{
		printf ("keep: the HUD program didn't link\n");
	}
	static pickups_t gems;
	pickups_init (&gems, PICKUPS_GEM);
	static sound_t snd;
	sound_init (&snd, &bsp, SND_AMB_WIND);
	printf ("keep: %d gems; keys: w s a d, the arrows, q e, space; the autopilot after %.0f s without\n", g.n_items, PILOT_S);
	glClearColor (0.0f, 0.0f, 0.0f, 1.0f);
	glDisable (GL_CULL_FACE);
	glDisable (GL_DITHER);

	game_pilot_t pilot;
	game_pilot_init (&pilot, keep_path, KEEP_PATH);
	GLint vp[4] = {0};
	float projection[16];
	absolute_time_t last = get_absolute_time ();
	perf_t perf;
	memset (&perf, 0, sizeof perf);
	unsigned windows = 0, frame = 0;
	bool new_perf = true, was_pilot = true;
	float t = 0.0f, run = 0.0f, best = 0.0f, done = 0.0f, lava = 0.0f, pick = 0.0f, done_time = 0.0f;
	int pick_item = 0, rounds = 0, deaths = 0;
	while (true)
	{
		if (screen_update ("keep", vp))
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
			game_pilot_init (&pilot, keep_path, KEEP_PATH);
			run = 0.0f;
		}
		was_pilot = pilot_on;
		if (pilot_on)
		{
			game_pilot (&pilot, &g, &in, dt);
		}
		game_update (&g, &in, dt);
		sound_game (&snd, &g, dt);

		/* the gems, the lava, the round's end */
		bool hud_now = false;
		run += done > 0.0f ? 0.0f : dt;
		if (g.picked >= 0)
		{
			pick = PICK_S;
			pick_item = g.picked;
			hud_now = true;
			sound_play (&snd, g.n_taken == g.n_items ? SND_COMPLETE : SND_ITEM, NULL, SOUND_ANY, 0.8f, 0.0f);
			if (g.n_taken == g.n_items)
			{
				done = DONE_S;
				done_time = run;
				best = best == 0.0f || run < best ? run : best;
				rounds++;
				printf ("keep: all %d gems in %.1f s (best %.1f s), %s\n", g.n_items, run, best, pilot_on ? "auto" : "keys");
			}
		}
		if (g.contents == BSP_CONTENTS_LAVA || g.contents == BSP_CONTENTS_SLIME)
		{
			sound_play (&snd, SND_BURN, NULL, SOUND_PLAYER, 1.0f, 0.0f);
			game_respawn (&g);
			game_pilot_init (&pilot, keep_path, KEEP_PATH);
			lava = LAVA_S;
			deaths++;
			hud_now = true;
		}
		if (done > 0.0f && (done -= dt) <= 0.0f)
		{
			game_items_reset (&g);
			run = 0.0f;
			hud_now = true;
		}
		lava = lava > dt ? lava - dt : 0.0f;
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
		pickups_draw (&gems, &g, vpm, eye, g.yaw, g.pitch, t, sun);

		if (hud_now || new_perf || frame % 6 == 0 || pick > 0.0f || lava > 0.0f || done > 0.0f)
		{
			char s[64];
			float hs = vp[3] >= 1000 ? 1.5f : vp[3] >= 600 ? 1.0f : 0.5f;
			hud_begin ();
			if (lava > 0.0f)
			{
				hud_rect (0, 0, vp[2], vp[3], HUD_RGBA (255, 40, 0, (int) (160 * lava / LAVA_S)));
				centered (vp[2], vp[3] * 0.4f, "THE LAVA!", HUD_RGBA (255, 230, 200, 255), hs * 2);
			}
			if (pick > 0.0f)
			{
				const float *c = pickups_color (&gems, pick_item);
				hud_rect (0, 0, vp[2], vp[3], HUD_RGBA ((int) (c[0] * 255), (int) (c[1] * 255), (int) (c[2] * 255),
									(int) (90 * pick / PICK_S)));
			}
			if (done > 0.0f)
			{
				snprintf (s, sizeof s, "ALL %d GEMS", g.n_items);
				centered (vp[2], vp[3] * 0.33f, s, HUD_RGBA (255, 220, 90, 255), hs * 2);
				snprintf (s, sizeof s, "%d:%04.1f", (int) done_time / 60, fmodf (done_time, 60.0f));
				centered (vp[2], vp[3] * 0.33f + HUD_CHAR_H * hs * 2.4f, s, HUD_RGBA (255, 255, 255, 255), hs * 2);
			}
			snprintf (s, sizeof s, "GEMS %d/%d  %d:%04.1f", g.n_taken, g.n_items, (int) run / 60, fmodf (run, 60.0f));
			hud_text_scaled (4, 4, s, HUD_RGBA (255, 220, 90, 255), hs);
			if (best > 0.0f)
				snprintf (s, sizeof s, "%s  BEST %d:%04.1f", pilot_on ? "AUTO" : "KEYS", (int) best / 60, fmodf (best, 60.0f));
			else
				snprintf (s, sizeof s, "%s", pilot_on ? "AUTO" : "KEYS");
			hud_text_scaled (4, 4 + HUD_CHAR_H * hs * 1.2f, s,
					 pilot_on ? HUD_RGBA (120, 220, 255, 255) : HUD_RGBA (255, 255, 255, 255), hs * 0.667f);
			hud_perf (vp[2] - hud_perf_width (hud_perf_scale (vp[3])) - 2, 2, hud_perf_scale (vp[3]), &perf);
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
			perf_log_link ("keep", &perf);
			printf ("keep: %.1f fps, GPU %.0f%% CPU-H %.0f%%, render %.2f ms; %u faces in %u draws, %u leaves; "
				"%s at %.0f %.0f %.0f, %d/%d gems, %d rounds, %d in the lava; GL error 0x%x\n", perf.fps, perf.gpu * 100,
				perf.cpu_h * 100, perf.render_ms, r.stats.faces, r.stats.draws, r.stats.leaves, pilot_on ? "auto" : "keys",
				g.origin[0], g.origin[1], g.origin[2], g.n_taken, g.n_items, rounds, deaths, (unsigned) glGetError ());
		}
	}
}
