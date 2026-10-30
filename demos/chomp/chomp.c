/*
 * chomp - the 1980 maze game, after Namco's (game.h: its rules from "The
 * Pac-Man Dossier"; the pictures and sounds made here). The maze and the
 * text are a screen of colour numbers, 224 x 288, kept on the host: the rows
 * that change go to the RPi, which colours them with a 16-colour palette
 * (the walls' flash at a level's end, the energizers' blink: the palette
 * changed, not the screen). The player, the ghosts, the fruit and the
 * scores are sprites from an atlas drawn by code at the start. Upright at
 * the monitor's 3:4, as large as the screen allows.
 *
 * 60 frames a second whatever the screen does. The attract mode: its
 * characters written out, then a silent game played by itself till it loses
 * a life. START (or A) starts a game; the stick steers (held: the turn is
 * taken where there's a way, early or late, as the arcade's); START pauses;
 * SELECT shows the numbers. The console's keys: the arrows or w a s d (a key
 * holds a moment), s or enter starts, p pauses, h the numbers.
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
#include "art.h"
#include "sounds.h"
#include "chomp_maze_program.h"
#include "chomp_sprite_program.h"

#define TEX_W		256		/* the screen's texture */
#define TEX_H		512
#define FRAME_US	16667		/* 60 a second */
#define STICK_AT	0.4f
#define KEY_HOLD_S	0.45f
#define ATTRACT_FRAMES	480		/* the characters shown this long, then the game by itself */
#define OVER_FRAMES	240

#ifdef CHOMP_RAW_TERMINAL		/* (a PC: the keys as they're pressed, not echoed) */
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

static ch_t game;

/* ---- the sound ---------------------------------------------------------------------- */

enum { CH_EAT, CH_LOOP, CH_EVENT, CH_EXTRA };

static void play (unsigned channel, int sound, float volume, uint32_t flags)
{
	uint32_t v = (uint32_t) (volume * PGPU_SOUND_FULL);
	pgpu_sound_play (channel, sound, v, v, flags);
}

static void sounds (const ch_t *g)
{
	static int loop = CSND_NONE;
	static bool waka;
	int want_loop = CSND_NONE;
	if (!g->demo && g->state == CH_PLAYING)
	{
		bool eyes = false;
		for (int i = 0; i < 4; i++)
		{
			eyes = eyes || g->ghost[i].state == G_EYES || g->ghost[i].state == G_ENTERING;
		}
		want_loop = eyes ? CSND_EYES : g->fright_t ? CSND_FRIGHT : CSND_SIREN1 + (g->dots_eaten * 5 / (CH_DOTS + 1));
	}
	if (want_loop != loop)
	{
		loop = want_loop;
		if (loop == CSND_NONE)
		{
			pgpu_sound_stop (CH_LOOP);
		}
		else
		{
			play (CH_LOOP, loop, 0.35f, PGPU_SOUND_LOOP);
		}
	}
	if (g->demo || g->state == CH_ATTRACT)		/* (the attract mode is silent) */
	{
		return;
	}
	unsigned e = g->events;
	if (e & (CH_EV_DOT | CH_EV_ENERGIZER))
	{
		play (CH_EAT, waka ? CSND_WAKA2 : CSND_WAKA1, 0.5f, 0);
		waka = !waka;
	}
	if (e & CH_EV_GHOST)
	{
		play (CH_EVENT, CSND_GHOST, 0.7f, 0);
	}
	if (e & CH_EV_FRUIT)
	{
		play (CH_EVENT, CSND_FRUIT, 0.7f, 0);
	}
	if (e & CH_EV_EXTRA)
	{
		play (CH_EXTRA, CSND_EXTRA, 0.7f, 0);
	}
	if (g->state == CH_DYING && g->frame == 60)	/* (as the death's animation starts) */
	{
		play (CH_EVENT, CSND_DEATH, 0.8f, 0);
	}
	if ((e & CH_EV_READY) && g->level == 1 && g->lives == 3 && g->score == 0)
	{
		play (CH_EVENT, CSND_START, 0.7f, 0);
	}
}

/* ---- the picture -------------------------------------------------------------------- */

typedef struct { float x, y, u, v; uint8_t rgba[4]; } sprite_vertex_t;

#define MAX_SPRITES	40
static sprite_vertex_t sprites[MAX_SPRITES * 6];
static int n_sprites;
static float view_x0, view_y0, view_sx, view_sy;	/* game pixels to clip space */

/* a sprite of `cells` cells wide, its middle at the game's (x, y) */
static void sprite (float x, float y, int cell, int cells, uint32_t rgb)
{
	if (n_sprites == MAX_SPRITES)
	{
		return;
	}
	float w = 16.0f * cells, u0 = (cell % 16) * 16.0f, v0 = (cell / 16) * 16.0f;
	static const float corner[6][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 0}, {1, 1}, {0, 1}};
	sprite_vertex_t *v = &sprites[n_sprites++ * 6];
	for (int k = 0; k < 6; k++, v++)
	{
		float gx = x - w / 2 + corner[k][0] * w, gy = y - 8.0f + corner[k][1] * 16.0f;
		v->x = view_x0 + gx * view_sx;
		v->y = view_y0 - gy * view_sy;
		v->u = u0 + corner[k][0] * w;
		v->v = v0 + corner[k][1] * 16.0f;
		v->rgba[0] = (uint8_t) (rgb >> 16), v->rgba[1] = (uint8_t) (rgb >> 8), v->rgba[2] = (uint8_t) rgb, v->rgba[3] = 255;
	}
}

static void ghost_sprites (const ch_t *g, int i, bool show_body)
{
	const ch_actor_t *gh = &g->ghost[i];
	int legs = (game.frame / 8) & 1;
	if (gh->state == G_EYES || gh->state == G_ENTERING)
	{
		sprite (gh->x, gh->y, A_EYES (gh->dir), 1, 0xFFFFFF);
		return;
	}
	if (gh->fright && show_body)
	{
		int flash_frames = ch_fright_flashes (g->level) * 24;
		bool white = g->fright_t < flash_frames && (g->fright_t / 12) % 2 == 0;
		sprite (gh->x, gh->y, white ? A_FLASH (legs) : A_FRIGHT (legs), 1, 0xFFFFFF);
		return;
	}
	sprite (gh->x, gh->y, A_GHOST (legs), 1, ch_ghost_color[i]);
	sprite (gh->x, gh->y, A_EYES (gh->dir < 0 ? CH_LEFT : gh->dir), 1, 0xFFFFFF);
}

static void build_sprites (const ch_t *g)
{
	n_sprites = 0;
	if (g->state == CH_ATTRACT)
	{
		static const int rows[4] = {8, 11, 14, 17};
		for (int i = 0; i < 4; i++)
		{
			if (g->frame >= 40 + 40 * i)
			{
				sprite (6 * 8, rows[i] * 8 + 4, A_GHOST ((g->frame / 8) & 1), 1, ch_ghost_color[i]);
				sprite (6 * 8, rows[i] * 8 + 4, A_EYES (CH_RIGHT), 1, 0xFFFFFF);
			}
		}
		return;
	}
	const int fruit_y = 20 * 8 + 4;
	if (g->fruit_t && g->state != CH_DYING)
	{
		sprite (112, fruit_y, A_FRUIT (ch_fruit_kind (g->level)), 1, 0xFFFFFF);
	}
	if (g->fruit_score_t)
	{
		sprite (112, fruit_y, A_FRUIT_SCORE (ch_fruit_kind (g->level)), 2, 0xFFFFFF);
	}
	bool ghosts = g->state == CH_READY || g->state == CH_PLAYING || g->state == CH_EATEN
		      || (g->state == CH_DYING && g->frame < 60) || (g->state == CH_LEVEL_DONE && g->frame < 60);
	for (int i = 0; ghosts && i < 4; i++)
	{
		if (g->state == CH_EATEN && i == g->eaten_ghost)
		{
			continue;
		}
		ghost_sprites (g, i, true);
	}
	if (g->state == CH_EATEN)
	{
		int n = g->popup_value == 200 ? 0 : g->popup_value == 400 ? 1 : g->popup_value == 800 ? 2 : 3;
		sprite (g->popup_x, g->popup_y, A_GHOST_SCORE (n), 2, 0xFFFFFF);
	}
	else if (g->state == CH_DYING)
	{
		int k = (g->frame - 60) / 8;
		if (k < 0)
		{
			sprite (g->pac.x, g->pac.y, A_PAC (g->pac.dir, 1), 1, 0xFFFFFF);
		}
		else if (k <= 10)
		{
			sprite (g->pac.x, g->pac.y, A_DEATH (k), 1, 0xFFFFFF);
		}
	}
	else if (g->state != CH_GAME_OVER)
	{
		static const int cycle[4] = {2, 1, 0, 1};	/* wide, half, shut, half */
		int f = g->state == CH_PLAYING ? cycle[(g->mouth / 2) % 4] : g->state == CH_READY ? 0 : 1;
		sprite (g->pac.x, g->pac.y, A_PAC (g->pac.dir, f), 1, 0xFFFFFF);
	}
	/* the lives left, the fruit of the levels lately */
	for (int i = 0; i < g->lives - 1 && i < 5; i++)
	{
		sprite (24 + 16 * i, 34 * 8 + 8, A_PAC (CH_LEFT, 1), 1, 0xFFFFFF);
	}
	for (int l = g->level, k = 0; l >= 1 && k < 7; l--, k++)
	{
		sprite (200 - 16 * k, 34 * 8 + 8, A_FRUIT (ch_fruit_kind (l)), 1, 0xFFFFFF);
	}
}

/* ---- the input ------------------------------------------------------------------------ */

typedef struct
{
	int dir;				/* CH_* or CH_NONE */
	bool start, pause, numbers;		/* (as they're pressed) */
} input_t;

static void read_input (input_t *in, bool pad_there, float dt)
{
	static unsigned was_buttons;
	static int key_dir = CH_NONE, escape;
	static float key_left;
	memset (in, 0, sizeof *in);
	in->dir = CH_NONE;
	pad_t pad;
	if (pad_there && pad_read (&pad))
	{
		unsigned pressed = pad.buttons & ~was_buttons;
		was_buttons = pad.buttons;
		if (fmaxf (fabsf (pad.x), fabsf (pad.y)) >= STICK_AT)
		{
			in->dir = fabsf (pad.x) > fabsf (pad.y) ? (pad.x > 0 ? CH_RIGHT : CH_LEFT) : (pad.y > 0 ? CH_UP : CH_DOWN);
		}
		in->start = pressed & (PAD_START | PAD_A);
		in->pause = pressed & PAD_START;
		in->numbers = pressed & PAD_SELECT;
	}
	for (int c; (c = getchar_timeout_us (0)) != PICO_ERROR_TIMEOUT; )
	{
		int d = CH_NONE;
		if (escape == 1)
		{
			escape = c == '[' ? 2 : 0;
			continue;
		}
		if (escape == 2)
		{
			d = c == 'A' ? CH_UP : c == 'B' ? CH_DOWN : c == 'C' ? CH_RIGHT : c == 'D' ? CH_LEFT : CH_NONE;
			escape = 0;
		}
		else
		{
			switch (c)
			{
			case 27: escape = 1; continue;
			case 'w': case 'W': d = CH_UP; break;
			case 's': case 'S': d = CH_DOWN; break;
			case 'a': case 'A': d = CH_LEFT; break;
			case 'd': case 'D': d = CH_RIGHT; break;
			case '\r': case '\n': case ' ': in->start = true; break;
			case 'p': case 'P': in->pause = true; break;
			case 'h': case 'H': in->numbers = true; break;
			}
		}
		if (d != CH_NONE)
		{
			key_dir = d;
			key_left = KEY_HOLD_S;
		}
	}
	key_left -= dt;
	if (in->dir == CH_NONE && key_left > 0)
	{
		in->dir = key_dir;
	}
}

/* ---- the game ------------------------------------------------------------------------- */

static GLuint load_program (const pgpu_program_info_t *info, size_t size, const char *name)
{
	GLuint p = glCreateProgram ();
	glProgramBinaryOES (p, PGL_PROGRAM_BINARY_PGPU, info, (GLsizei) size);
	GLint linked = 0;
	glGetProgramiv (p, GL_LINK_STATUS, &linked);
	if (!linked)
	{
		printf ("chomp: the %s program didn't link\n", name);
	}
	return p;
}

int main (void)
{
	stdio_init_all ();
	pgpu_init ();
#ifdef CHOMP_RAW_TERMINAL
	raw_terminal ();
#endif
	printf ("\nchomp: waiting for the RPi (READY)...\n");
	while (!pgpu_wait_ready (1000))
	{
	}
	pgpu_set_reply_phase (1);
	int tries = 0;
	while (!pglInit () && ++tries < 5)		/* the first reply can be missed */
	{
	}
	bool pad_there = pad_init ();

	GLuint maze_prog = load_program (&chomp_maze_info, sizeof chomp_maze_info, "maze");
	GLuint sprite_prog = load_program (&chomp_sprite_info, sizeof chomp_sprite_info, "sprite");
	GLint m_pos = glGetAttribLocation (maze_prog, "a_pos"), m_pixel = glGetAttribLocation (maze_prog, "a_pixel");
	GLint m_size = glGetUniformLocation (maze_prog, "u_size");
	GLint s_pos = glGetAttribLocation (sprite_prog, "a_pos"), s_uv = glGetAttribLocation (sprite_prog, "a_uv");
	GLint s_color = glGetAttribLocation (sprite_prog, "a_color"), s_size = glGetUniformLocation (sprite_prog, "u_size");
	glUseProgram (maze_prog);
	glUniform1i (glGetUniformLocation (maze_prog, "u_tex"), 0);
	glUniform1i (glGetUniformLocation (maze_prog, "u_pal"), 1);
	glUseProgram (sprite_prog);
	glUniform1i (glGetUniformLocation (sprite_prog, "u_atlas"), 2);
	if (!hud_init ())
	{
		printf ("chomp: the HUD program didn't link\n");
	}
	glDisable (GL_DEPTH_TEST);
	glDisable (GL_CULL_FACE);
	for (int i = 1; i < CSND_COUNT; i++)
	{
		pgpu_sound_data (i, chomp_sounds[i].rate, PGPU_SOUND_U8, chomp_sounds[i].samples, chomp_sounds[i].frames);
	}

	/* the textures: the screen (unit 0), the palette (1), the atlas (2) */
	GLuint tex[3];
	glGenTextures (3, tex);
	glPixelStorei (GL_UNPACK_ALIGNMENT, 1);
	for (int k = 0; k < 3; k++)
	{
		glActiveTexture (GL_TEXTURE0 + k);
		glBindTexture (GL_TEXTURE_2D, tex[k]);
		GLint filter = k == 2 ? GL_LINEAR : GL_NEAREST;
		if (k == 0)
		{
			glTexImage2D (GL_TEXTURE_2D, 0, GL_LUMINANCE, TEX_W, TEX_H, 0, GL_LUMINANCE, GL_UNSIGNED_BYTE, NULL);
		}
		else if (k == 1)
		{
			glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA, 16, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
		}
		else
		{
			uint8_t *atlas = malloc (ATLAS_W * ATLAS_H * 4);
			ch_draw_atlas (atlas);
			glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA, ATLAS_W, ATLAS_H, 0, GL_RGBA, GL_UNSIGNED_BYTE, atlas);
			free (atlas);
		}
		glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
		glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
		glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	}
	glActiveTexture (GL_TEXTURE0);
	printf ("chomp: %s; keys: the arrows or w a s d, enter starts, p pauses, h the numbers\n",
		pad_there ? "the stick steers, START (or A) starts, START pauses, SELECT the numbers" : "no stick here");

	int hi_score = 0;
	ch_attract (&game, hi_score);
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
		screen_update ("chomp", vp);
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
		bool idle_game = game.state == CH_ATTRACT || game.demo || game.state == CH_GAME_OVER;
		if (in.start && idle_game)
		{
			paused = false;
			ch_new_game (&game, false, hi_score);
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
			ch_frame (&game, game.demo ? ch_bot (&game) : in.dir);
			sounds (&game);
			if (game.state == CH_ATTRACT && game.frame >= ATTRACT_FRAMES)
			{
				ch_new_game (&game, true, hi_score);
			}
			else if ((game.demo && (game.events & CH_EV_DIE)) || (game.state == CH_GAME_OVER && game.frame >= OVER_FRAMES))
			{
				hi_score = game.hi_score > hi_score ? game.hi_score : hi_score;
				ch_attract (&game, hi_score);
			}
		}
		if (owed_us > 4 * FRAME_US)
		{
			owed_us = 0;
		}
		if (paused)
		{
			pgpu_sound_stop (CH_LOOP);
		}

		/* the screen's rows that changed */
		glActiveTexture (GL_TEXTURE0);
		glBindTexture (GL_TEXTURE_2D, tex[0]);
		for (int y = 0; y < CH_H; )
		{
			if (!game.dirty[y])
			{
				y++;
				continue;
			}
			int y0 = y;
			while (y < CH_H && game.dirty[y])
			{
				y++;
			}
			glTexSubImage2D (GL_TEXTURE_2D, 0, 0, y0, CH_W, y - y0, GL_LUMINANCE, GL_UNSIGNED_BYTE, &game.screen[y0][0]);
			rows_sent += (unsigned) (y - y0);
		}
		ch_clean (&game);

		/* the palette: the walls white in a level's end's flashes, the energizers blinking */
		for (int i = 0; i < 16; i++)
		{
			uint32_t c = i < P_COUNT ? ch_palette[i] : 0;
			if (i == P_WALL && game.state == CH_LEVEL_DONE && game.frame >= 60 && ((game.frame - 60) / 12) % 2 == 0)
			{
				c = 0xDEDEDE;
			}
			if (i == P_ENERGIZER && game.state == CH_PLAYING && (game.frame / 10) % 2)
			{
				c = 0;
			}
			palette[i][0] = (uint8_t) (c >> 16), palette[i][1] = (uint8_t) (c >> 8), palette[i][2] = (uint8_t) c, palette[i][3] = 255;
		}
		if (memcmp (palette, shown_palette, sizeof palette) != 0)
		{
			memcpy (shown_palette, palette, sizeof palette);
			glActiveTexture (GL_TEXTURE1);
			glBindTexture (GL_TEXTURE_2D, tex[1]);
			glTexSubImage2D (GL_TEXTURE_2D, 0, 0, 0, 16, 1, GL_RGBA, GL_UNSIGNED_BYTE, palette);
		}

		if (now - said >= 5.0f)
		{
			printf ("chomp: %s, level %d, score %d, %d lives; %.1f fps, %u rows sent a frame\n",
				game.state == CH_ATTRACT ? "attract" : game.demo ? "by itself" : paused ? "paused" : "played",
				game.level, game.score, game.lives, frames / (now - said), rows_sent / (frames ? frames : 1));
			said = now;
			frames = 0;
			rows_sent = 0;
		}
		frames++;

		/* upright at 3:4, as large as fits */
		glViewport (vp[0], vp[1], vp[2], vp[3]);
		glClearColor (0.0f, 0.0f, 0.0f, 1.0f);
		glClear (GL_COLOR_BUFFER_BIT);
		float h = (float) vp[3], w = h * 0.75f;
		if (w > vp[2])
		{
			w = (float) vp[2];
			h = w / 0.75f;
		}
		view_x0 = -w / vp[2];
		view_y0 = h / vp[3];
		view_sx = 2.0f * w / vp[2] / CH_W;
		view_sy = 2.0f * h / vp[3] / CH_H;
		float x1 = -view_x0, y1 = -view_y0;
		const float quad[6][4] =
		{
			{view_x0, view_y0, 0, 0}, {x1, view_y0, CH_W, 0}, {x1, y1, CH_W, CH_H},
			{view_x0, view_y0, 0, 0}, {x1, y1, CH_W, CH_H}, {view_x0, y1, 0, CH_H}
		};
		glUseProgram (maze_prog);
		glUniform4f (m_size, 1.0f / TEX_W, 1.0f / TEX_H, w / CH_W, h / CH_H);
		glBindBuffer (GL_ARRAY_BUFFER, 0);
		glEnableVertexAttribArray (m_pos);
		glEnableVertexAttribArray (m_pixel);
		glVertexAttribPointer (m_pos, 2, GL_FLOAT, GL_FALSE, sizeof quad[0], &quad[0][0]);
		glVertexAttribPointer (m_pixel, 2, GL_FLOAT, GL_FALSE, sizeof quad[0], &quad[0][2]);
		glDrawArrays (GL_TRIANGLES, 0, 6);
		glDisableVertexAttribArray (m_pixel);

		build_sprites (&game);
		if (n_sprites)
		{
			glUseProgram (sprite_prog);
			glUniform4f (s_size, 1.0f / ATLAS_W, 1.0f / ATLAS_H, w / CH_W, h / CH_H);
			glEnable (GL_BLEND);
			glBlendFunc (GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
			glEnableVertexAttribArray (s_pos);
			glEnableVertexAttribArray (s_uv);
			glEnableVertexAttribArray (s_color);
			glVertexAttribPointer (s_pos, 2, GL_FLOAT, GL_FALSE, sizeof (sprite_vertex_t), &sprites[0].x);
			glVertexAttribPointer (s_uv, 2, GL_FLOAT, GL_FALSE, sizeof (sprite_vertex_t), &sprites[0].u);
			glVertexAttribPointer (s_color, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof (sprite_vertex_t), sprites[0].rgba);
			glDrawArrays (GL_TRIANGLES, 0, n_sprites * 6);
			glDisableVertexAttribArray (s_uv);
			glDisableVertexAttribArray (s_color);
			glDisable (GL_BLEND);
		}

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
