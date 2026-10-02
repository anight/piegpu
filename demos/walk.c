/*
 * walk - the BSP engine (engine/): a Quake-format level (BSP29, compiled with
 * ericw-tools; engine/levels/base.bsp by default, PGPU_LEVEL) linked in, drawn
 * by the RPi (textures, lightmaps, the visible set from the PVS) and walked
 * through: Quake's movement against the level's clipping hulls, doors that
 * open, a lift (engine/game.c), with their sounds and the player's
 * (engine/sound.c: steps, the lift's motor, the doors, a base's hum).
 *
 * The keys (engine/keys.h): w s a d, the arrows or q e, space. Without keys
 * for PILOT_S the autopilot walks the level (engine/levels/base_path.h).
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
#include "levels/base_path.h"

extern const uint8_t level_file[], level_file_end[];

#define FOVY		62.0f
#define PILOT_S		10.0f		/* without keys: the autopilot */

int main (void)
{
	stdio_init_all ();
	pgpu_init ();
	keys_t keys;
	keys_init (&keys);
	keys.idle = PILOT_S;
	printf ("\nwalk: waiting for the RPi (READY)...\n");
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
	printf ("walk: the level: %d faces, %d leaves (%d visible), %d models, %u bytes\n", (int) bsp.n_faces, (int) bsp.n_leaves,
		(int) bsp.models[0].visleafs, (int) bsp.n_models, (unsigned) bsp.size);
	static render_t r;
	static game_t g;
	if (!render_init (&r, &bsp) || !game_init (&g, &bsp))
	{
		printf ("walk: the level doesn't fit\n");
		return 1;
	}
	if (!hud_init ())
	{
		printf ("walk: the HUD program didn't link\n");
	}
	static sound_t snd;
	sound_init (&snd, &bsp, SND_AMB_HUM);
	printf ("walk: keys: w s a d, the arrows, q e, space; the autopilot after %.0f s without\n", PILOT_S);
	glClearColor (0.0f, 0.0f, 0.0f, 1.0f);
	glDisable (GL_CULL_FACE);
	glDisable (GL_DITHER);

	game_pilot_t pilot;
	game_pilot_init (&pilot, base_path, BASE_PATH);
	GLint vp[4] = {0};
	float projection[16];
	absolute_time_t last = get_absolute_time ();
	perf_t perf;
	memset (&perf, 0, sizeof perf);
	unsigned windows = 0, frame = 0;
	bool new_perf = true;
	while (true)
	{
		if (screen_update ("walk", vp))
		{
			mat4_perspective (projection, FOVY, (float) vp[2] / vp[3], 4.0f, 4096.0f);
		}
		absolute_time_t now = get_absolute_time ();
		float dt = absolute_time_diff_us (last, now) / 1e6f;
		dt = dt > 0.05f ? 0.05f : dt;
		last = now;
		r.time += dt;

		/* the input: the keys, or the autopilot without them */
		game_input_t in;
		keys_input (&keys, &g, &in, dt);
		bool pilot_on = keys.idle >= PILOT_S;
		if (pilot_on)
		{
			game_pilot (&pilot, &g, &in, dt);
		}
		game_update (&g, &in, dt);
		sound_game (&snd, &g, dt);

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
		render_world (&r, eye, vpm);
		for (int k = 0; k < g.n_movers; k++)
		{
			render_model (&r, g.movers[k].model, g.movers[k].at, vpm);
		}

		if (new_perf || frame % 6 == 0)
		{
			char s[64];
			float hs = vp[3] >= 1000 ? 1.5f : vp[3] >= 600 ? 1.0f : 0.5f;
			hud_begin ();
			snprintf (s, sizeof s, "%s  %.0f %.0f %.0f", pilot_on ? "AUTO" : "KEYS", g.origin[0], g.origin[1], g.origin[2]);
			hud_text_scaled (4, 4, s, pilot_on ? HUD_RGBA (120, 220, 255, 255) : HUD_RGBA (255, 220, 80, 255), hs);
			snprintf (s, sizeof s, "FACES %u  DRAWS %u  LEAVES %u", r.stats.faces, r.stats.draws, r.stats.leaves);
			hud_text_scaled (4, 4 + HUD_CHAR_H * hs * 1.2f, s, HUD_RGBA (220, 220, 220, 255), hs * 0.667f);
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
			perf_log_link ("walk", &perf);
			printf ("walk: %.1f fps, GPU %.0f%% CPU-H %.0f%%, render %.2f ms; %u faces in %u draws, %u leaves; "
				"%s at %.0f %.0f %.0f, %u waypoints; GL error 0x%x\n", perf.fps, perf.gpu * 100, perf.cpu_h * 100,
				perf.render_ms, r.stats.faces, r.stats.draws, r.stats.leaves, pilot_on ? "auto" : "keys", g.origin[0],
				g.origin[1], g.origin[2], pilot.reached, (unsigned) glGetError ());
		}
	}
}
