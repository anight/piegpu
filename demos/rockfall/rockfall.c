/*
 * rockfall - the 1984 cave game, after First Star's (game.h: its engine as
 * documented; the caves, the pictures and the sounds made here). The cave
 * is a map of tile numbers on the RPi (64 x 32: the cave's 40 x 22, the
 * status line in the last row), sent whole each frame (880 bytes), drawn
 * through an atlas of the tiles in colour numbers and the cave's palette:
 * each cave its own three colours, as the C64's. A view of 20 x 12 cells
 * scrolls after the miner, at a whole scale of the screen, the status line
 * over it.
 *
 * The stick moves the miner (horizontal first); A (or B, X, Y) held, it
 * grabs instead, from where it stands. START starts; SELECT the numbers. The
 * console's keys: the arrows or w a s d, space (held a moment) to grab,
 * enter starts, h the numbers. Left at the title it plays by itself, silent,
 * till its miner's lost.
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
#include "rockfall_program.h"

#define MAP_W		64
#define MAP_H		32
#define STATUS_ROW	31
#define VIEW_W		20		/* cells (the C64's) */
#define VIEW_H		12
#define FRAME_US	16667
#define STICK_AT	0.4f
#define KEY_HOLD_S	0.3f
#define TITLE_FRAMES	240		/* the title, then the game by itself */
#define DEMO_FRAMES	(60 * 60)
#define OVER_FRAMES	240
#define SCROLL_PX	2		/* a frame */

#ifdef ROCKFALL_RAW_TERMINAL		/* (a PC: the keys as they're pressed, not echoed) */
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

static rf_t game;
static uint8_t map[MAP_H][MAP_W];

/* ---- the sound ---------------------------------------------------------------------- */

enum { CH_STEP, CH_BOULDER, CH_DIAMOND, CH_COLLECT, CH_BANG, CH_TICK, CH_COVER, CH_AMOEBA, CH_MAGIC };

static void play (unsigned channel, int sound, float volume, uint32_t flags)
{
	uint32_t v = (uint32_t) (volume * PGPU_SOUND_FULL);
	pgpu_sound_play (channel, sound, v, v, flags);
}

static void loop (unsigned channel, int sound, bool on, bool *playing)
{
	if (on != *playing)
	{
		*playing = on;
		if (on)
		{
			play (channel, sound, 0.35f, PGPU_SOUND_LOOP);
		}
		else
		{
			pgpu_sound_stop (channel);
		}
	}
}

static void sounds (const rf_t *g)
{
	static bool covering, bubbling, tinkling;
	bool heard = !g->demo && g->state != RF_ATTRACT;
	loop (CH_COVER, RSND_UNCOVER, heard && g->state == RF_UNCOVER, &covering);
	loop (CH_AMOEBA, RSND_AMOEBA, heard && g->state == RF_PLAYING && g->last_amoeba > 0, &bubbling);
	loop (CH_MAGIC, RSND_MAGIC, heard && g->state == RF_PLAYING && g->magic_on, &tinkling);
	if (!heard)
	{
		return;
	}
	unsigned e = g->events;
	if (e & RF_EV_MOVE)
	{
		play (CH_STEP, RSND_STEP, 0.3f, 0);
	}
	if (e & (RF_EV_BOULDER | RF_EV_PUSH))
	{
		play (CH_BOULDER, RSND_BOULDER, e & RF_EV_BOULDER ? 0.7f : 0.4f, 0);
	}
	if (e & RF_EV_DIAMOND_FALL)
	{
		play (CH_DIAMOND, RSND_DIAMOND, 0.5f, 0);
	}
	if (e & RF_EV_COLLECT)
	{
		play (CH_COLLECT, RSND_COLLECT, 0.6f, 0);
	}
	if (e & RF_EV_EXPLODE)
	{
		play (CH_BANG, RSND_EXPLODE, 0.9f, 0);
	}
	if (e & RF_EV_OPEN)
	{
		play (CH_BANG, RSND_OPEN, 0.8f, 0);
	}
	if (e & RF_EV_TICK)
	{
		play (CH_TICK, RSND_TICK, 0.6f, 0);
		pgpu_sound_pitch (CH_TICK, 1.0f + 0.06f * (10 - g->time));	/* (higher each second) */
	}
	if (e & RF_EV_BONUS)
	{
		play (CH_TICK, RSND_BONUS, 0.4f, 0);
	}
	if (e & RF_EV_EXTRA)
	{
		play (CH_COLLECT, RSND_EXTRA, 0.6f, 0);
	}
}

/* ---- the picture -------------------------------------------------------------------- */

static int glyph (char c, bool yellow)
{
	const char *p = strchr (rf_glyph_chars, c);
	return T_GLYPH (p ? (int) (p - rf_glyph_chars) : (int) strlen (rf_glyph_chars) - 1, yellow ? 1 : 0);
}

static void status_text (int col, const char *s, bool yellow)
{
	for (; *s && col < VIEW_W; s++, col++)
	{
		map[STATUS_ROW][col] = (uint8_t) glyph (*s, yellow);
	}
}

static void status_number (int col, int n, int digits, bool yellow)
{
	char s[8];
	for (int i = digits - 1; i >= 0; i--, n /= 10)
	{
		s[i] = (char) ('0' + n % 10);
	}
	s[digits] = '\0';
	status_text (col, s, yellow);
}

static void build_status (const rf_t *g)
{
	for (int c = 0; c < VIEW_W; c++)
	{
		map[STATUS_ROW][c] = (uint8_t) glyph (' ', false);
	}
	if (g->state == RF_ATTRACT)
	{
		status_text (1, (g->frame / 30) % 2 ? "ROCKFALL - START" : "ROCKFALL", true);
		return;
	}
	if (g->state == RF_GAME_OVER)
	{
		status_text (5, "GAME OVER", true);
		return;
	}
	if (g->state == RF_UNCOVER)
	{
		char s[24];
		snprintf (s, sizeof s, "%s %d", rf_caves[g->cave].name, g->difficulty);
		status_text ((VIEW_W - (int) strlen (s)) / 2, s, false);
		return;
	}
	if (g->state == RF_DEAD && g->time == 0)
	{
		status_text (4, "OUT OF TIME", true);
		return;
	}
	/* the needed and what each is worth, the taken, the time, the score */
	status_number (0, g->needed, 2, true);
	map[STATUS_ROW][2] = T_DIAMOND_ICON;
	status_number (3, g->diamonds >= g->needed ? g->value_extra : g->value, 2, false);
	status_number (6, g->diamonds, 2, true);
	status_number (9, g->time, 3, false);
	status_number (13, g->score % 1000000, 6, false);
}

static int miner_tile (const rf_t *g)
{
	static int seq, blinking, tapping;
	if (g->anim % 16 == 0)				/* (each 16 frames: a blink 1 in 4, the tapping toggled 1 in 16) */
	{
		blinking = rand () % 4 == 0;
		if (rand () % 16 == 0)
		{
			tapping = !tapping;
		}
		seq = g->anim;
	}
	if (g->moving)
	{
		return (g->facing_right ? T_RIGHT0 : T_LEFT0) + (g->anim / 4) % 4;
	}
	if (blinking && g->anim - seq < 6)
	{
		return T_BLINK;
	}
	if (tapping)
	{
		return (g->anim / 4) % 2 ? T_TAP1 : T_TAP0;
	}
	return T_MINER;
}

static void build_map (const rf_t *g)
{
	int a = g->anim;
	for (int y = 0; y < RF_H; y++)
	{
		for (int x = 0; x < RF_W; x++)
		{
			uint16_t c = g->cell[y][x];
			int t;
			switch (rf_object (c))
			{
			case O_DIRT: t = T_DIRT; break;
			case O_BRICK: t = T_BRICK; break;
			case O_MAGIC: t = g->magic_on ? T_MAGIC0 + (a / 4) % 4 : T_BRICK; break;
			case O_STEEL: case O_PREOUT: t = T_STEEL; break;
			case O_BOULDER: t = T_BOULDER; break;
			case O_DIAMOND: t = T_DIAMOND0 + (a / 3) % 8; break;
			case O_FIREFLY: t = T_FIREFLY0 + (a / 4) % 4; break;
			case O_BUTTERFLY: t = T_BUTTERFLY0 + (a / 4) % 4; break;
			case O_AMOEBA: t = T_AMOEBA0 + (a / 6) % 4; break;
			case O_EXPLODE: case O_EXPLODE_DIAMOND: t = T_EXPLODE0 + (rf_dir (c) > 4 ? 4 : rf_dir (c)); break;
			case O_MINER: t = miner_tile (g); break;
			case O_INBOX: case O_OUTBOX: t = T_OUTBOX0 + (a / 8) % 2; break;
			default: t = T_SPACE; break;
			}
			map[y][x] = (uint8_t) (g->covered[y][x] ? T_COVER : t);
		}
	}
	build_status (g);
}

/* ---- the input ------------------------------------------------------------------------ */

typedef struct
{
	int dx, dy;
	bool fire;
	bool start, numbers;
} input_t;

static void read_input (input_t *in, bool pad_there, float dt)
{
	static unsigned was_buttons;
	static int key_dx, key_dy, escape;
	static float key_left, fire_left;
	memset (in, 0, sizeof *in);
	pad_t pad;
	if (pad_there && pad_read (&pad))
	{
		unsigned pressed = pad.buttons & ~was_buttons;
		was_buttons = pad.buttons;
		in->dx = pad.x >= STICK_AT ? 1 : pad.x <= -STICK_AT ? -1 : 0;
		in->dy = pad.y >= STICK_AT ? -1 : pad.y <= -STICK_AT ? 1 : 0;	/* (forward: up the cave) */
		in->fire = pad.buttons & (PAD_A | PAD_B | PAD_X | PAD_Y | PAD_STICK);
		in->start = pressed & (PAD_START | PAD_A);
		in->numbers = pressed & PAD_SELECT;
	}
	for (int c; (c = getchar_timeout_us (0)) != PICO_ERROR_TIMEOUT; )
	{
		int dx = 0, dy = 0;
		if (escape == 1)
		{
			escape = c == '[' ? 2 : 0;
			continue;
		}
		if (escape == 2)
		{
			dx = c == 'C' ? 1 : c == 'D' ? -1 : 0;
			dy = c == 'A' ? -1 : c == 'B' ? 1 : 0;
			escape = 0;
		}
		else
		{
			switch (c)
			{
			case 27: escape = 1; continue;
			case 'w': case 'W': dy = -1; break;
			case 's': case 'S': dy = 1; break;
			case 'a': case 'A': dx = -1; break;
			case 'd': case 'D': dx = 1; break;
			case ' ': fire_left = KEY_HOLD_S; break;
			case '\r': case '\n': in->start = true; break;
			case 'h': case 'H': in->numbers = true; break;
			}
		}
		if (dx || dy)
		{
			key_dx = dx;
			key_dy = dy;
			key_left = KEY_HOLD_S;
		}
	}
	key_left -= dt;
	fire_left -= dt;
	if (!in->dx && !in->dy && key_left > 0)
	{
		in->dx = key_dx;
		in->dy = key_dy;
	}
	in->fire = in->fire || fire_left > 0;
}

/* ---- the game ------------------------------------------------------------------------- */

int main (void)
{
	stdio_init_all ();
	pgpu_init ();
#ifdef ROCKFALL_RAW_TERMINAL
	raw_terminal ();
#endif
	printf ("\nrockfall: waiting for the RPi (READY)...\n");
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
	glProgramBinaryOES (prog, PGL_PROGRAM_BINARY_PGPU, &rockfall_info, sizeof rockfall_info);
	GLint linked = 0;
	glGetProgramiv (prog, GL_LINK_STATUS, &linked);
	if (!linked)
	{
		printf ("rockfall: the program didn't link\n");
	}
	GLint a_pos = glGetAttribLocation (prog, "a_pos"), a_pixel = glGetAttribLocation (prog, "a_pixel");
	glUseProgram (prog);
	glUniform1i (glGetUniformLocation (prog, "u_map"), 0);
	glUniform1i (glGetUniformLocation (prog, "u_atlas"), 1);
	glUniform1i (glGetUniformLocation (prog, "u_pal"), 2);
	if (!hud_init ())
	{
		printf ("rockfall: the HUD program didn't link\n");
	}
	glDisable (GL_DEPTH_TEST);
	glDisable (GL_CULL_FACE);
	for (int i = 1; i < RSND_COUNT; i++)
	{
		pgpu_sound_data (i, rockfall_sounds[i].rate, PGPU_SOUND_U8, rockfall_sounds[i].samples, rockfall_sounds[i].frames);
	}

	/* the textures: the map (unit 0), the atlas (1), the palette (2) */
	GLuint tex[3];
	glGenTextures (3, tex);
	glPixelStorei (GL_UNPACK_ALIGNMENT, 1);
	uint8_t *atlas = malloc (ATLAS_SIZE * ATLAS_SIZE);
	rf_draw_atlas (atlas);
	for (int k = 0; k < 3; k++)
	{
		glActiveTexture (GL_TEXTURE0 + k);
		glBindTexture (GL_TEXTURE_2D, tex[k]);
		if (k == 0)
		{
			glTexImage2D (GL_TEXTURE_2D, 0, GL_LUMINANCE, MAP_W, MAP_H, 0, GL_LUMINANCE, GL_UNSIGNED_BYTE, NULL);
		}
		else if (k == 1)
		{
			glTexImage2D (GL_TEXTURE_2D, 0, GL_LUMINANCE, ATLAS_SIZE, ATLAS_SIZE, 0, GL_LUMINANCE, GL_UNSIGNED_BYTE, atlas);
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
	free (atlas);
	glActiveTexture (GL_TEXTURE0);
	printf ("rockfall: %s; keys: the arrows or w a s d, space grabs, enter starts, h the numbers\n",
		pad_there ? "the stick moves, A (held) grabs, START starts, SELECT the numbers" : "no stick here");

	int hi_score = 0;
	rf_attract (&game, hi_score);
	GLint vp[4] = {0};
	perf_t perf;
	memset (&perf, 0, sizeof perf);
	bool new_perf = true, numbers = false, hud_shown = false;
	absolute_time_t last = get_absolute_time ();
	uint64_t owed_us = 0;
	float said = 0, now = 0;
	unsigned frames = 0;
	int view_x = 0, view_y = 0, shown_cave = -1;
	uint8_t palette[16][4], shown_palette[16][4];
	memset (shown_palette, 0xFF, sizeof shown_palette);
	srand (time_us_32 ());
	while (true)
	{
		screen_update ("rockfall", vp);
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
		if (in.start && (game.state == RF_ATTRACT || game.demo || game.state == RF_GAME_OVER))
		{
			rf_new_game (&game, false, hi_score);
		}

		for (int n = 0; owed_us >= FRAME_US && n < 4; n++)
		{
			owed_us -= FRAME_US;
			int dx = in.dx, dy = in.dy;
			bool fire = in.fire;
			if (game.demo)
			{
				rf_bot (&game, &dx, &dy, &fire);
			}
			rf_frame (&game, dx, dy, fire);
			sounds (&game);
			if (game.state == RF_ATTRACT && game.frame >= TITLE_FRAMES)
			{
				rf_new_game (&game, true, hi_score);
			}
			else if (game.demo && (game.state == RF_DEAD || game.frame >= DEMO_FRAMES || game.cave != 0))
			{
				rf_attract (&game, hi_score);
			}
			else if (game.state == RF_GAME_OVER && game.frame >= OVER_FRAMES)
			{
				hi_score = game.hi_score > hi_score ? game.hi_score : hi_score;
				rf_attract (&game, hi_score);
			}
		}
		if (owed_us > 4 * FRAME_US)
		{
			owed_us = 0;
		}

		/* the view after the miner (an intermission's all in view) */
		const rf_cave_t *cave = &rf_caves[game.cave];
		int max_x = cave->intermission ? 0 : (RF_W - VIEW_W) * 16, max_y = cave->intermission ? 0 : (RF_H - VIEW_H) * 16;
		int want_x = view_x, want_y = view_y, mx = game.mx * 16, my = game.my * 16;
		if (mx < view_x + 5 * 16 || mx > view_x + (VIEW_W - 6) * 16)
		{
			want_x = mx - (VIEW_W / 2) * 16;
		}
		if (my < view_y + 3 * 16 || my > view_y + (VIEW_H - 4) * 16)
		{
			want_y = my - (VIEW_H / 2) * 16;
		}
		want_x = want_x < 0 ? 0 : want_x > max_x ? max_x : want_x;
		want_y = want_y < 0 ? 0 : want_y > max_y ? max_y : want_y;
		if (game.cave != shown_cave || game.state == RF_UNCOVER || game.state == RF_ATTRACT)
		{
			view_x = want_x;		/* (a new cave: straight there) */
			view_y = want_y;
			shown_cave = game.cave;
		}
		else
		{
			view_x += want_x > view_x ? (want_x - view_x < SCROLL_PX ? want_x - view_x : SCROLL_PX) : want_x < view_x ? -(view_x - want_x < SCROLL_PX ? view_x - want_x : SCROLL_PX) : 0;
			view_y += want_y > view_y ? (want_y - view_y < SCROLL_PX ? want_y - view_y : SCROLL_PX) : want_y < view_y ? -(view_y - want_y < SCROLL_PX ? view_y - want_y : SCROLL_PX) : 0;
		}

		build_map (&game);
		glActiveTexture (GL_TEXTURE0);
		glBindTexture (GL_TEXTURE_2D, tex[0]);
		glTexSubImage2D (GL_TEXTURE_2D, 0, 0, 0, MAP_W, MAP_H, GL_LUMINANCE, GL_UNSIGNED_BYTE, map);

		/* the palette: the cave's colours; the screen white a moment when the exit's out */
		for (int i = 0; i < 16; i++)
		{
			uint32_t c = i >= 1 && i <= 3 ? cave->colors[i - 1] : i < K_COUNT ? rf_fixed_colors[i] : 0;
			if (i == K_BLACK && game.flash)
			{
				c = 0xFFFFFF;
			}
			palette[i][0] = (uint8_t) (c >> 16), palette[i][1] = (uint8_t) (c >> 8), palette[i][2] = (uint8_t) c, palette[i][3] = 255;
		}
		if (memcmp (palette, shown_palette, sizeof palette) != 0)
		{
			memcpy (shown_palette, palette, sizeof palette);
			glActiveTexture (GL_TEXTURE2);
			glBindTexture (GL_TEXTURE_2D, tex[2]);
			glTexSubImage2D (GL_TEXTURE_2D, 0, 0, 0, 16, 1, GL_RGBA, GL_UNSIGNED_BYTE, palette);
		}
		glActiveTexture (GL_TEXTURE0);

		if (now - said >= 5.0f)
		{
			printf ("rockfall: %s, cave %d (%s) level %d, %d of %d diamonds, %d s, score %d, %d lives; %.1f fps\n",
				game.state == RF_ATTRACT ? "title" : game.demo ? "by itself" : "played", game.cave + 1, cave->name,
				game.difficulty, game.diamonds, game.needed, game.time, game.score, game.lives, frames / (now - said));
			said = now;
			frames = 0;
		}
		frames++;

		/* the status line and the view, at a whole scale, in the middle */
		glViewport (vp[0], vp[1], vp[2], vp[3]);
		glClearColor (0.0f, 0.0f, 0.0f, 1.0f);
		glClear (GL_COLOR_BUFFER_BIT);
		int scale = (int) fminf (vp[2] / (VIEW_W * 16.0f), vp[3] / ((VIEW_H + 1) * 16.0f));
		scale = scale < 1 ? 1 : scale;
		float w = VIEW_W * 16.0f * scale, h = (VIEW_H + 1) * 16.0f * scale;
		float left = floorf ((vp[2] - w) / 2), top = floorf ((vp[3] - h) / 2), row = 16.0f * scale;
		float x0 = left / vp[2] * 2 - 1, x1 = (left + w) / vp[2] * 2 - 1;
		float ys0 = 1 - top / vp[3] * 2, ys1 = 1 - (top + row) / vp[3] * 2, yv1 = 1 - (top + h) / vp[3] * 2;
		float vx0 = (float) view_x, vy0 = (float) view_y, vx1 = vx0 + VIEW_W * 16, vy1 = vy0 + VIEW_H * 16;
		const float quads[12][4] =
		{
			{x0, ys0, 0, STATUS_ROW * 16}, {x1, ys0, VIEW_W * 16, STATUS_ROW * 16}, {x1, ys1, VIEW_W * 16, MAP_H * 16},
			{x0, ys0, 0, STATUS_ROW * 16}, {x1, ys1, VIEW_W * 16, MAP_H * 16}, {x0, ys1, 0, MAP_H * 16},
			{x0, ys1, vx0, vy0}, {x1, ys1, vx1, vy0}, {x1, yv1, vx1, vy1},
			{x0, ys1, vx0, vy0}, {x1, yv1, vx1, vy1}, {x0, yv1, vx0, vy1}
		};
		glUseProgram (prog);
		glBindBuffer (GL_ARRAY_BUFFER, 0);
		glEnableVertexAttribArray (a_pos);
		glEnableVertexAttribArray (a_pixel);
		glVertexAttribPointer (a_pos, 2, GL_FLOAT, GL_FALSE, sizeof quads[0], &quads[0][0]);
		glVertexAttribPointer (a_pixel, 2, GL_FLOAT, GL_FALSE, sizeof quads[0], &quads[0][2]);
		glDrawArrays (GL_TRIANGLES, 0, 12);
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
