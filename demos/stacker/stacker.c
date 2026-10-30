/*
 * stacker - the falling-blocks game as Nintendo's 1989 NES version plays it
 * (game.h). The NES's 256 x 224 screen is kept on the host as colour
 * numbers; the rows that change go to the RPi, which colours them with a
 * palette (the level's two colours; the field's background white in a
 * four's flash) and shows them at the NES's 4:3.
 *
 * The title: the stick left and right picks the level to start at (0 - 9),
 * START (or A) starts. Playing: the stick moves (held: again after 16
 * frames, then every 6) and down drops; A turns clockwise, B the other way
 * (X and Y as well); START pauses; SELECT shows the numbers. The console's
 * keys: a d or the arrows, s or down to drop, x or up to turn clockwise, z
 * the other way, enter starts, p pauses, h the numbers. Left at the title
 * it plays by itself, silent.
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
#include "stacker_program.h"

#define TEX_W		256
#define TEX_H		256
#define FRAME_US	16667
#define STICK_AT	0.45f
#define KEY_HOLD_S	0.12f		/* a console key holds this long (a terminal's repeats keep it held) */
#define TITLE_FRAMES	600		/* the title this long, then the game by itself */
#define DEMO_FRAMES	(60 * 45)
#define OVER_FRAMES	240

#ifdef STACKER_RAW_TERMINAL		/* (a PC: the keys as they're pressed, not echoed) */
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

static st_t game;

/* ---- the sound ---------------------------------------------------------------------- */

enum { CH_MOVE, CH_ROTATE, CH_LOCK, CH_EVENT, CH_MUSIC };

static void play (unsigned channel, int sound, float volume, uint32_t flags)
{
	uint32_t v = (uint32_t) (volume * PGPU_SOUND_FULL);
	pgpu_sound_play (channel, sound, v, v, flags);
}

static void sounds (const st_t *g, bool paused)
{
	static bool music;
	bool want = !g->demo && !paused && (g->state == ST_FALLING || g->state == ST_CLEARING || g->state == ST_ENTRY);
	if (want != music)
	{
		music = want;
		if (want)
		{
			play (CH_MUSIC, SSND_MUSIC, 0.45f, PGPU_SOUND_LOOP);
		}
		else
		{
			pgpu_sound_stop (CH_MUSIC);
		}
	}
	if (g->demo || g->state == ST_TITLE)
	{
		return;
	}
	unsigned e = g->events;
	if (e & ST_EV_MOVE)
	{
		play (CH_MOVE, SSND_MOVE, 0.4f, 0);
	}
	if (e & ST_EV_ROTATE)
	{
		play (CH_ROTATE, SSND_ROTATE, 0.4f, 0);
	}
	if (e & ST_EV_LOCK)
	{
		play (CH_LOCK, SSND_LOCK, 0.6f, 0);
	}
	if (e & ST_EV_FOUR)
	{
		play (CH_EVENT, SSND_FOUR, 0.7f, 0);
	}
	else if (e & ST_EV_LINES)
	{
		play (CH_EVENT, SSND_LINES, 0.6f, 0);
	}
	if (e & ST_EV_LEVEL)
	{
		play (CH_EVENT, SSND_LEVEL, 0.6f, 0);
	}
	if (e & ST_EV_OVER)
	{
		play (CH_EVENT, SSND_OVER, 0.7f, 0);
	}
}

/* ---- the input ------------------------------------------------------------------------ */

typedef struct
{
	st_input_t held;
	bool start, pause, numbers, left_press, right_press;
} input_t;

static void read_input (input_t *in, bool pad_there, float dt)
{
	static unsigned was_buttons;
	static bool was_left, was_right;
	static int escape;
	static float left_t, right_t, down_t, cw_t, ccw_t;
	memset (in, 0, sizeof *in);
	pad_t pad;
	if (pad_there && pad_read (&pad))
	{
		unsigned pressed = pad.buttons & ~was_buttons;
		was_buttons = pad.buttons;
		in->held.left = pad.x <= -STICK_AT;
		in->held.right = pad.x >= STICK_AT;
		in->held.down = pad.y <= -STICK_AT && fabsf (pad.x) < STICK_AT;
		in->held.rotate_cw = pad.buttons & (PAD_A | PAD_X);
		in->held.rotate_ccw = pad.buttons & (PAD_B | PAD_Y);
		in->start = pressed & (PAD_START | PAD_A);
		in->pause = pressed & PAD_START;
		in->numbers = pressed & PAD_SELECT;
	}
	for (int c; (c = getchar_timeout_us (0)) != PICO_ERROR_TIMEOUT; )
	{
		if (escape == 1)
		{
			escape = c == '[' ? 2 : 0;
			continue;
		}
		if (escape == 2)
		{
			escape = 0;
			c = c == 'A' ? 'x' : c == 'B' ? 's' : c == 'C' ? 'd' : c == 'D' ? 'a' : 0;
		}
		switch (c)
		{
		case 27: escape = 1; break;
		case 'a': case 'A': left_t = KEY_HOLD_S * 4; break;
		case 'd': case 'D': right_t = KEY_HOLD_S * 4; break;
		case 's': case 'S': down_t = KEY_HOLD_S * 4; break;
		case 'x': case 'X': case 'w': case 'W': cw_t = KEY_HOLD_S; break;
		case 'z': case 'Z': ccw_t = KEY_HOLD_S; break;
		case '\r': case '\n': in->start = true; break;
		case 'p': case 'P': in->pause = true; break;
		case 'h': case 'H': in->numbers = true; break;
		}
	}
	left_t -= dt, right_t -= dt, down_t -= dt, cw_t -= dt, ccw_t -= dt;
	in->held.left = in->held.left || left_t > 0;
	in->held.right = in->held.right || right_t > 0;
	in->held.down = in->held.down || down_t > 0;
	in->held.rotate_cw = in->held.rotate_cw || cw_t > 0;
	in->held.rotate_ccw = in->held.rotate_ccw || ccw_t > 0;
	in->left_press = in->held.left && !was_left;
	in->right_press = in->held.right && !was_right;
	was_left = in->held.left;
	was_right = in->held.right;
}

/* ---- the game ------------------------------------------------------------------------- */

int main (void)
{
	stdio_init_all ();
	pgpu_init ();
#ifdef STACKER_RAW_TERMINAL
	raw_terminal ();
#endif
	printf ("\nstacker: waiting for the RPi (READY)...\n");
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
	glProgramBinaryOES (prog, PGL_PROGRAM_BINARY_PGPU, &stacker_info, sizeof stacker_info);
	GLint linked = 0;
	glGetProgramiv (prog, GL_LINK_STATUS, &linked);
	if (!linked)
	{
		printf ("stacker: the program didn't link\n");
	}
	GLint a_pos = glGetAttribLocation (prog, "a_pos"), a_pixel = glGetAttribLocation (prog, "a_pixel");
	GLint u_size = glGetUniformLocation (prog, "u_size");
	glUseProgram (prog);
	glUniform1i (glGetUniformLocation (prog, "u_tex"), 0);
	glUniform1i (glGetUniformLocation (prog, "u_pal"), 1);
	if (!hud_init ())
	{
		printf ("stacker: the HUD program didn't link\n");
	}
	glDisable (GL_DEPTH_TEST);
	glDisable (GL_CULL_FACE);
	for (int i = 1; i < SSND_COUNT; i++)
	{
		pgpu_sound_data (i, stacker_sounds[i].rate, PGPU_SOUND_U8, stacker_sounds[i].samples, stacker_sounds[i].frames);
	}
	GLuint tex[2];
	glGenTextures (2, tex);
	glPixelStorei (GL_UNPACK_ALIGNMENT, 1);
	for (int k = 0; k < 2; k++)
	{
		glActiveTexture (GL_TEXTURE0 + k);
		glBindTexture (GL_TEXTURE_2D, tex[k]);
		if (k == 0)
		{
			glTexImage2D (GL_TEXTURE_2D, 0, GL_LUMINANCE, TEX_W, TEX_H, 0, GL_LUMINANCE, GL_UNSIGNED_BYTE, NULL);
		}
		else
		{
			glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA, 16, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
		}
		glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	}
	glActiveTexture (GL_TEXTURE0);
	printf ("stacker: %s; keys: a d or the arrows, s drops, x or up turns, z the other way, enter starts, p pauses, h the numbers\n",
		pad_there ? "the stick moves and drops, A and B turn, START starts and pauses, SELECT the numbers" : "no stick here");

	int top = 10000, start_level = 0;		/* (the NES's table starts at 10000) */
	st_title (&game, top, start_level);
	GLint vp[4] = {0};
	perf_t perf;
	memset (&perf, 0, sizeof perf);
	bool new_perf = true, numbers = false, hud_shown = false, paused = false;
	absolute_time_t last = get_absolute_time ();
	uint64_t owed_us = 0;
	float said = 0, now = 0;
	unsigned frames = 0, rows_sent = 0;
	uint8_t palette[16][4], shown_palette[16][4];
	memset (shown_palette, 0xFF, sizeof shown_palette);
	while (true)
	{
		screen_update ("stacker", vp);
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
		bool idle_game = game.state == ST_TITLE || game.demo || game.state == ST_OVER;
		if (game.state == ST_TITLE && (in.left_press || in.right_press))
		{
			start_level = (start_level + (in.right_press ? 1 : 9)) % 10;
			game.start_level = start_level;
		}
		if (in.start && idle_game)
		{
			paused = false;
			st_new_game (&game, false, start_level, top);
		}
		else if (in.pause && !idle_game)
		{
			paused = !paused;
		}

		for (int n = 0; owed_us >= FRAME_US && n < 4; n++)
		{
			owed_us -= FRAME_US;
			if (paused)
			{
				continue;
			}
			st_input_t held = in.held;
			if (game.demo)
			{
				st_bot (&game, &held);
			}
			st_frame (&game, &held);
			sounds (&game, paused);
			if (game.state == ST_TITLE && game.frame >= TITLE_FRAMES)
			{
				st_new_game (&game, true, 0, top);
			}
			else if (game.demo && (game.state == ST_OVER || game.frame >= DEMO_FRAMES || game.spawns > 200))
			{
				st_title (&game, top, start_level);
			}
			else if (game.state == ST_OVER && game.frame >= OVER_FRAMES)
			{
				top = game.top_score > top ? game.top_score : top;
				st_title (&game, top, start_level);
			}
		}
		if (owed_us > 4 * FRAME_US)
		{
			owed_us = 0;
		}
		if (paused)
		{
			sounds (&game, true);
		}

		/* the screen's rows that changed */
		glActiveTexture (GL_TEXTURE0);
		glBindTexture (GL_TEXTURE_2D, tex[0]);
		for (int y = 0; y < ST_H; )
		{
			if (!game.dirty[y])
			{
				y++;
				continue;
			}
			int y0 = y;
			while (y < ST_H && game.dirty[y])
			{
				y++;
			}
			glTexSubImage2D (GL_TEXTURE_2D, 0, 0, y0, ST_W, y - y0, GL_LUMINANCE, GL_UNSIGNED_BYTE, &game.screen[y0][0]);
			rows_sent += (unsigned) (y - y0);
		}
		st_clean (&game);

		/* the palette: the level's colours; the field white in a four's flash; dark while paused */
		const uint32_t *lc = st_level_colors[game.level % 10];
		uint32_t colors[C_COUNT] = {0x000000, 0xFCFCFC, 0x7C7C7C, 0x3C3C3C, lc[0], lc[1], 0x000000, 0xF83800};
		if (game.state == ST_CLEARING && game.n_clear == 4 && game.frame % 4 == 0)
		{
			colors[C_FIELD] = 0xFCFCFC;
		}
		if (paused)
		{
			colors[C_LEVEL1] = colors[C_LEVEL2] = colors[C_WHITE] = 0x000000;	/* (the field hidden, as the NES's pause) */
		}
		for (int i = 0; i < 16; i++)
		{
			uint32_t c = i < C_COUNT ? colors[i] : 0;
			palette[i][0] = (uint8_t) (c >> 16), palette[i][1] = (uint8_t) (c >> 8), palette[i][2] = (uint8_t) c, palette[i][3] = 255;
		}
		if (memcmp (palette, shown_palette, sizeof palette) != 0)
		{
			memcpy (shown_palette, palette, sizeof palette);
			glActiveTexture (GL_TEXTURE1);
			glBindTexture (GL_TEXTURE_2D, tex[1]);
			glTexSubImage2D (GL_TEXTURE_2D, 0, 0, 0, 16, 1, GL_RGBA, GL_UNSIGNED_BYTE, palette);
			glActiveTexture (GL_TEXTURE0);
		}

		if (now - said >= 5.0f)
		{
			printf ("stacker: %s, level %d, %d lines, score %d; %.1f fps, %u rows sent a frame\n",
				game.state == ST_TITLE ? "title" : game.demo ? "by itself" : paused ? "paused" : "played",
				game.level, game.lines, game.score, frames / (now - said), rows_sent / (frames ? frames : 1));
			said = now;
			frames = 0;
			rows_sent = 0;
		}
		frames++;

		/* at the NES's 4:3, as large as fits */
		glViewport (vp[0], vp[1], vp[2], vp[3]);
		glClearColor (0.0f, 0.0f, 0.0f, 1.0f);
		glClear (GL_COLOR_BUFFER_BIT);
		float h = (float) vp[3], w = h * 4.0f / 3.0f;
		if (w > vp[2])
		{
			w = (float) vp[2];
			h = w * 3.0f / 4.0f;
		}
		float x0 = -w / vp[2], x1 = w / vp[2], y0 = h / vp[3], y1 = -h / vp[3];
		const float quad[6][4] =
		{
			{x0, y0, 0, 0}, {x1, y0, ST_W, 0}, {x1, y1, ST_W, ST_H},
			{x0, y0, 0, 0}, {x1, y1, ST_W, ST_H}, {x0, y1, 0, ST_H}
		};
		glUseProgram (prog);
		glUniform4f (u_size, 1.0f / TEX_W, 1.0f / TEX_H, w / ST_W, h / ST_H);
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
