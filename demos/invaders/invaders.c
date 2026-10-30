/*
 * invaders - the 1978 arcade game, after Taito's (game.h: its rules, timings
 * and tables from its disassembly; the pictures and sounds made here). The
 * game draws into its one-bit screen, 224 x 256, on the Pico; the rows that
 * change go to the RPi as a texture's, and a shader shows it through the
 * cabinet's coloured overlay, upright at the monitor's 3:4, as large as the
 * screen allows.
 *
 * The game runs 60 frames a second whatever the screen does (frames are
 * made up if one is late). The attract mode as the original's: the score
 * table written out, then a game played by itself, silent, till its cannon
 * is hit. START (or A) starts a game; the stick moves the cannon, A (or B,
 * X, Y, the stick pressed) fires - let go to fire again, as the original.
 * SELECT shows the numbers. The console's keys: a d or the arrows move,
 * space fires, s or enter starts, h the numbers.
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
#include "pad.h"
#include "game.h"
#include "sounds.h"
#include "invaders_program.h"

#define TEX_W		256		/* the screen's texture */
#define TEX_H		256
#define FRAME_US	16667		/* the game's frame: 60 a second */
#define STICK_AT	0.4f
#define KEY_HOLD_S	0.3f		/* a console key holds this long (a terminal sends no key-up) */
#define ATTRACT_FRAMES	480		/* the score table's shown this long, then the game by itself */
#define OVER_FRAMES	240		/* a game's end shown this long */

#ifdef INVADERS_RAW_TERMINAL		/* (a PC: the keys as they're pressed, not echoed) */
#include <signal.h>
#include <termios.h>
#include <unistd.h>

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

static iv_t game;

/* ---- the sound ---------------------------------------------------------------------- */

enum { CH_FLEET, CH_SHOT, CH_HIT, CH_PLAYER, CH_SAUCER, CH_EXTRA };

static float clampf (float x, float lo, float hi)	{ return x < lo ? lo : x > hi ? hi : x; }

static void play (unsigned channel, int sound, float volume, int x, uint32_t flags)
{
	float pan = clampf ((x - IV_W / 2) / (float) IV_W, -0.5f, 0.5f);
	float l = clampf (volume * fminf (1.0f, 1.0f - pan), 0.0f, 1.0f), r = clampf (volume * fminf (1.0f, 1.0f + pan), 0.0f, 1.0f);
	pgpu_sound_play (channel, sound, (uint32_t) (l * PGPU_SOUND_FULL), (uint32_t) (r * PGPU_SOUND_FULL), flags);
}

static void sounds (const iv_t *g)
{
	unsigned e = g->events;
	if (g->demo || g->state == IV_ATTRACT)		/* (the attract mode is silent: the original's) */
	{
		if (e & (IV_EV_SAUCER_OFF | IV_EV_GAME_OVER))
		{
			pgpu_sound_stop (CH_SAUCER);
		}
		return;
	}
	if (e & IV_EV_FLEET)
	{
		play (CH_FLEET, ISND_FLEET1 + g->fleet_note, 0.9f, IV_W / 2, 0);
	}
	if (e & IV_EV_SHOT)
	{
		play (CH_SHOT, ISND_SHOT, 0.5f, g->event_x, 0);
	}
	if (e & IV_EV_INVADER_HIT)
	{
		play (CH_HIT, ISND_INVADER_HIT, 0.7f, g->event_x, 0);
	}
	if (e & IV_EV_PLAYER_HIT)
	{
		pgpu_sound_stop (CH_SAUCER);
		play (CH_PLAYER, ISND_PLAYER_HIT, 1.0f, g->event_x, 0);
	}
	if (e & IV_EV_SAUCER_ON)
	{
		play (CH_SAUCER, ISND_SAUCER, 0.45f, IV_W / 2, PGPU_SOUND_LOOP);
	}
	if (e & IV_EV_SAUCER_OFF)
	{
		pgpu_sound_stop (CH_SAUCER);
	}
	if (e & IV_EV_SAUCER_HIT)
	{
		play (CH_HIT, ISND_SAUCER_HIT, 0.8f, g->event_x, 0);
	}
	if (e & IV_EV_EXTRA)
	{
		play (CH_EXTRA, ISND_EXTRA, 0.7f, IV_W / 2, 0);
	}
	if (e & IV_EV_GAME_OVER)
	{
		pgpu_sound_stop (CH_SAUCER);
	}
}

/* ---- the input ------------------------------------------------------------------------ */

typedef struct
{
	int dir;				/* -1, 0, 1 */
	bool fire;				/* held */
	bool start, numbers;			/* (as they're pressed) */
} input_t;

static void read_input (input_t *in, bool pad_there, float dt)
{
	static unsigned was_buttons;
	static int key_dir, escape, fire_frames;
	static float key_left;
	memset (in, 0, sizeof *in);
	pad_t pad;
	if (pad_there && pad_read (&pad))
	{
		unsigned pressed = pad.buttons & ~was_buttons;
		was_buttons = pad.buttons;
		in->dir = pad.x >= STICK_AT ? 1 : pad.x <= -STICK_AT ? -1 : 0;
		in->fire = pad.buttons & (PAD_A | PAD_B | PAD_X | PAD_Y | PAD_STICK);
		in->start = pressed & (PAD_START | PAD_A);
		in->numbers = pressed & PAD_SELECT;
	}
	for (int c; (c = getchar_timeout_us (0)) != PICO_ERROR_TIMEOUT; )
	{
		int d = 0;
		if (escape == 1)
		{
			escape = c == '[' ? 2 : 0;
			continue;
		}
		if (escape == 2)
		{
			d = c == 'C' ? 1 : c == 'D' ? -1 : 0;
			escape = 0;
		}
		else
		{
			switch (c)
			{
			case 27: escape = 1; continue;
			case 'a': case 'A': d = -1; break;
			case 'd': case 'D': d = 1; break;
			case ' ': fire_frames = 3; break;		/* (pressed, held a moment, let go) */
			case 's': case 'S': case '\r': case '\n': in->start = true; break;
			case 'h': case 'H': in->numbers = true; break;
			}
		}
		if (d)
		{
			key_dir = d;
			key_left = KEY_HOLD_S;
		}
	}
	key_left -= dt;
	if (!in->dir && key_left > 0)
	{
		in->dir = key_dir;
	}
	if (fire_frames > 0)
	{
		fire_frames--;
		in->fire = in->fire || fire_frames > 0;
	}
}

/* ---- the game ------------------------------------------------------------------------- */

int main (void)
{
	stdio_init_all ();
	pgpu_init ();
#ifdef INVADERS_RAW_TERMINAL
	raw_terminal ();
#endif
	printf ("\ninvaders: waiting for the RPi (READY)...\n");
	while (!pgpu_wait_ready (1000))
	{
	}
	pgpu_set_reply_phase (1);
	int tries = 0;
	while (!pglInit () && ++tries < 5)		/* the first reply can be missed */
	{
	}
	bool pad_there = pad_init ();

	GLuint prog = glCreateProgram ();
	glProgramBinaryOES (prog, PGL_PROGRAM_BINARY_PGPU, &invaders_info, sizeof invaders_info);
	GLint linked = 0;
	glGetProgramiv (prog, GL_LINK_STATUS, &linked);
	if (!linked)
	{
		printf ("invaders: the program didn't link\n");
	}
	GLint a_pos = glGetAttribLocation (prog, "a_pos"), a_pixel = glGetAttribLocation (prog, "a_pixel");
	GLint u_size = glGetUniformLocation (prog, "u_size");
	glUseProgram (prog);
	glUniform1i (glGetUniformLocation (prog, "u_tex"), 0);
	if (!hud_init ())
	{
		printf ("invaders: the HUD program didn't link\n");
	}
	glDisable (GL_DEPTH_TEST);
	glDisable (GL_CULL_FACE);
	for (int i = 1; i < ISND_COUNT; i++)
	{
		pgpu_sound_data (i, invaders_sounds[i].rate, PGPU_SOUND_U8, invaders_sounds[i].samples, invaders_sounds[i].frames);
	}
	GLuint tex;
	glGenTextures (1, &tex);
	glBindTexture (GL_TEXTURE_2D, tex);
	glPixelStorei (GL_UNPACK_ALIGNMENT, 1);
	glTexImage2D (GL_TEXTURE_2D, 0, GL_LUMINANCE, TEX_W, TEX_H, 0, GL_LUMINANCE, GL_UNSIGNED_BYTE, NULL);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	printf ("invaders: %s; keys: a d or the arrows move, space fires, s or enter starts, h the numbers\n",
		pad_there ? "the stick moves, A fires, START starts, SELECT the numbers" : "no stick here");

	int hi_score = 0;
	iv_attract (&game, hi_score);
	GLint vp[4] = {0};
	perf_t perf;
	memset (&perf, 0, sizeof perf);
	bool new_perf = true, numbers = false, hud_shown = false;
	absolute_time_t last = get_absolute_time ();
	uint64_t owed_us = 0;
	float said = 0, now = 0;
	unsigned frames = 0, uploads = 0, upload_rows = 0;
	while (true)
	{
		screen_update ("invaders", vp);
		absolute_time_t t0 = get_absolute_time ();
		int64_t us = absolute_time_diff_us (last, t0);
		last = t0;
		owed_us += (uint64_t) (us > 100000 ? 100000 : us);
		float dt = us / 1e6f;
		now += dt;

		input_t in;
		read_input (&in, pad_there, dt);
		if (in.numbers)
		{
			numbers = !numbers;
		}
		if (in.start && (game.state == IV_ATTRACT || game.demo || game.state == IV_GAME_OVER))
		{
			pgpu_sound_stop (CH_SAUCER);
			iv_new_game (&game, false, hi_score);
		}

		/* the game's frames due (60 a second) */
		for (int n = 0; owed_us >= FRAME_US && n < 4; n++)
		{
			owed_us -= FRAME_US;
			int dir = in.dir;
			bool fire = in.fire;
			if (game.demo)
			{
				iv_bot (&game, &dir, &fire);
			}
			iv_frame (&game, dir, fire);
			sounds (&game);
			if (game.state == IV_ATTRACT && game.frame >= ATTRACT_FRAMES)
			{
				iv_new_game (&game, true, hi_score);
			}
			else if (game.state == IV_GAME_OVER && game.frame >= (game.demo ? 60 : OVER_FRAMES))
			{
				hi_score = game.hi_score;
				iv_attract (&game, hi_score);
			}
		}
		if (owed_us > 4 * FRAME_US)
		{
			owed_us = 0;			/* (too far behind: given up) */
		}

		/* the rows that changed, in runs */
		glBindTexture (GL_TEXTURE_2D, tex);
		for (int y = 0; y < IV_H; )
		{
			if (!game.dirty[y])
			{
				y++;
				continue;
			}
			int y0 = y;
			while (y < IV_H && game.dirty[y])
			{
				y++;
			}
			glTexSubImage2D (GL_TEXTURE_2D, 0, 0, y0, IV_W, y - y0, GL_LUMINANCE, GL_UNSIGNED_BYTE, &game.screen[y0][0]);
			uploads++;
			upload_rows += (unsigned) (y - y0);
		}
		iv_clean (&game);
		if (now - said >= 5.0f)
		{
			printf ("invaders: %s, round %d, score %d, %d cannons; %.1f fps, %u rows sent a frame in %u runs\n",
				game.state == IV_ATTRACT ? "attract" : game.demo ? "by itself" : "played", game.round + 1, game.score,
				game.lives, frames / (now - said), upload_rows / (frames ? frames : 1), uploads / (frames ? frames : 1));
			said = now;
			frames = 0;
			uploads = 0;
			upload_rows = 0;
		}
		frames++;

		/* the screen upright at 3:4, as large as fits */
		glViewport (vp[0], vp[1], vp[2], vp[3]);
		glClearColor (0.0f, 0.0f, 0.0f, 1.0f);
		glClear (GL_COLOR_BUFFER_BIT);
		float h = (float) vp[3], w = h * 0.75f;
		if (w > vp[2])
		{
			w = (float) vp[2];
			h = w / 0.75f;
		}
		float x0 = -w / vp[2], x1 = w / vp[2], y0 = h / vp[3], y1 = -h / vp[3];
		const float quad[6][4] =
		{
			{x0, y0, 0, 0}, {x1, y0, IV_W, 0}, {x1, y1, IV_W, IV_H},
			{x0, y0, 0, 0}, {x1, y1, IV_W, IV_H}, {x0, y1, 0, IV_H}
		};
		glUseProgram (prog);
		glUniform4f (u_size, 1.0f / TEX_W, 1.0f / TEX_H, w / IV_W, h / IV_H);
		glBindBuffer (GL_ARRAY_BUFFER, 0);
		glEnableVertexAttribArray (a_pos);
		glEnableVertexAttribArray (a_pixel);
		glVertexAttribPointer (a_pos, 2, GL_FLOAT, GL_FALSE, sizeof quad[0], &quad[0][0]);
		glVertexAttribPointer (a_pixel, 2, GL_FLOAT, GL_FALSE, sizeof quad[0], &quad[0][2]);
		glDrawArrays (GL_TRIANGLES, 0, 6);
		glDisableVertexAttribArray (a_pixel);

		if (numbers && new_perf)
		{
			hud_begin ();
			hud_perf (vp[2] - hud_perf_width (hud_perf_scale (vp[3])) - 2, 2, hud_perf_scale (vp[3]), &perf);
			hud_end ();
			hud_shown = true;
		}
		else if (!numbers && hud_shown)
		{
			hud_begin ();
			hud_end ();
			hud_shown = false;
		}
		hud_draw ();
		pglSwapBuffers ();

		absolute_time_t wait_start = get_absolute_time ();
		pgpu_wait_frame (100);			/* pace on the screen */
		new_perf = perf_frame (absolute_time_diff_us (wait_start, get_absolute_time ()), &perf);
	}
}
