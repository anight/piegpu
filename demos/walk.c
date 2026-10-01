/*
 * walk - the BSP engine (engine/): a Quake-format level (BSP29, compiled with
 * ericw-tools; engine/levels/base.bsp by default, PGPU_LEVEL) linked in, drawn
 * by the RPi (textures, lightmaps, the visible set from the PVS) and walked
 * through: Quake's movement against the level's clipping hulls, doors that
 * open, a lift (engine/game.c).
 *
 * The keys, on the console (a PC's terminal is switched to take them at once:
 * WALK_RAW_TERMINAL): w s forward and back, a d sideways, the arrows or q e
 * to turn (up and down: walk), space to jump. A terminal sends no key-up, only
 * a key's repeats: a key holds for HOLD_S after each. Without keys for
 * PILOT_S the autopilot walks the level (engine/levels/base_path.h).
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
#include "levels/base_path.h"
#ifdef WALK_RAW_TERMINAL
#include <signal.h>
#include <termios.h>
#include <unistd.h>
#endif

extern const uint8_t level_file[], level_file_end[];

#define FOVY		62.0f
#define HOLD_S		0.3f		/* a key's hold after it's seen */
#define PILOT_S		10.0f		/* without keys: the autopilot */
#define TURN		2.2f		/* radians a second */

#ifdef WALK_RAW_TERMINAL
/* the terminal: keys as they're pressed, not echoed; as it was at the end */
static struct termios saved;
static struct sigaction previous[3];
static const int signals[3] = {SIGINT, SIGTERM, SIGHUP};

static void restore (void)		{ tcsetattr (0, TCSANOW, &saved); }

static void on_signal (int sig)
{
	restore ();
	for (int i = 0; i < 3; i++)
	{
		if (signals[i] == sig && previous[i].sa_handler != SIG_DFL && previous[i].sa_handler != SIG_IGN)
		{
			previous[i].sa_handler (sig);	/* (the transport's: the session's end) */
		}
	}
	_exit (128 + sig);
}

static void raw_terminal (void)
{
	if (!isatty (0) || tcgetattr (0, &saved) != 0)
	{
		return;
	}
	struct termios t = saved;
	t.c_lflag &= ~(ICANON | ECHO);
	t.c_cc[VMIN] = 0;
	t.c_cc[VTIME] = 0;
	tcsetattr (0, TCSANOW, &t);
	atexit (restore);
	struct sigaction sa;
	memset (&sa, 0, sizeof sa);
	sa.sa_handler = on_signal;
	for (int i = 0; i < 3; i++)
	{
		sigaction (signals[i], &sa, &previous[i]);
	}
}
#endif

/* the keys held: each for HOLD_S after it was last seen */
enum { K_FORWARD, K_BACK, K_LEFT, K_RIGHT, K_TURN_LEFT, K_TURN_RIGHT, K_JUMP, KEYS };

static bool read_keys (float held[KEYS])
{
	static int escape;			/* ESC [ x: the arrows */
	bool any = false;
	int c;
	while ((c = getchar_timeout_us (0)) != PICO_ERROR_TIMEOUT)
	{
		int k = -1;
		if (escape == 1)
		{
			escape = c == '[' ? 2 : 0;
			continue;
		}
		if (escape == 2)
		{
			k = c == 'A' ? K_FORWARD : c == 'B' ? K_BACK : c == 'C' ? K_TURN_RIGHT : c == 'D' ? K_TURN_LEFT : -1;
			escape = 0;
		}
		else if (c == 27)
		{
			escape = 1;
			continue;
		}
		else
		{
			switch (c)
			{
			case 'w': case 'W': k = K_FORWARD; break;
			case 's': case 'S': k = K_BACK; break;
			case 'a': case 'A': k = K_LEFT; break;
			case 'd': case 'D': k = K_RIGHT; break;
			case 'q': case 'Q': k = K_TURN_LEFT; break;
			case 'e': case 'E': k = K_TURN_RIGHT; break;
			case ' ': k = K_JUMP; break;
			}
		}
		if (k >= 0)
		{
			held[k] = HOLD_S;
			any = true;
		}
	}
	return any;
}

int main (void)
{
	stdio_init_all ();
	pgpu_init ();
#ifdef WALK_RAW_TERMINAL
	raw_terminal ();
#endif
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
	printf ("walk: keys: w s a d, the arrows, q e, space; the autopilot after %.0f s without\n", PILOT_S);
	glClearColor (0.0f, 0.0f, 0.0f, 1.0f);
	glDisable (GL_CULL_FACE);
	glDisable (GL_DITHER);

	game_pilot_t pilot;
	game_pilot_init (&pilot, base_path, BASE_PATH);
	float held[KEYS] = {0}, idle = PILOT_S;
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

		/* the input: the keys, or the autopilot without them */
		game_input_t in;
		idle = read_keys (held) ? 0.0f : idle + dt;
		bool pilot_on = idle >= PILOT_S;
		if (pilot_on)
		{
			game_pilot (&pilot, &g, &in, dt);
		}
		else
		{
			memset (&in, 0, sizeof in);
			in.forward = (held[K_FORWARD] > 0) - (held[K_BACK] > 0);
			in.side = (held[K_RIGHT] > 0) - (held[K_LEFT] > 0);
			in.turn = TURN * ((held[K_TURN_LEFT] > 0) - (held[K_TURN_RIGHT] > 0));
			in.look = -g.pitch * 2.0f;
			in.jump = held[K_JUMP] > 0;
		}
		for (int k = 0; k < KEYS; k++)
		{
			held[k] = held[k] > dt ? held[k] - dt : 0.0f;
		}
		game_update (&g, &in, dt);

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
			printf ("walk: %.1f fps, GPU %.0f%% CPU-H %.0f%%, render %.2f ms; %u faces in %u draws, %u leaves; "
				"%s at %.0f %.0f %.0f, %u waypoints; GL error 0x%x\n", perf.fps, perf.gpu * 100, perf.cpu_h * 100,
				perf.render_ms, r.stats.faces, r.stats.draws, r.stats.leaves, pilot_on ? "auto" : "keys", g.origin[0],
				g.origin[1], g.origin[2], pilot.reached, (unsigned) glGetError ());
		}
	}
}
