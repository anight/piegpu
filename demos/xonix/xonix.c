/*
 * xonix - Xonix (game.h): claim the sea for the land, a trail at a time,
 * while the balls bounce about. The field is a texture on the RPi, a texel a
 * cell (only the rows that change are sent); a shader draws the sea, the
 * land's tiles, the trail and the flash of what was just claimed. The player
 * and the balls are drawn over it, each on its way from one cell to the next.
 *
 * The stick or the game controller's (engine/pad.h) steers: on land the
 * player goes while it's held, at sea it goes on and turns, never straight
 * back. START pauses, SELECT shows the numbers, A (or B, X, Y) starts a game
 * after one's over. The console's keys: the arrows or w a s d, p pauses, h
 * the numbers, r a new game. Left alone for 20 s it plays by itself
 * (bot.c); a key or the stick then starts a game to play.
 *
 * The sounds are the RPi's to mix (sounds.c, made by
 * demos/tools/make_xonix_sounds.py): the trail ticking out, a chime for
 * each capture (lower for a big one), the balls' bounces, a life lost, the
 * level's end, the game's.
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
#include "xonix_field_program.h"
#include "xonix_sprite_program.h"

#define TEX_W		128		/* the field's texture (a texel a cell) */
#define TEX_H		64
#define IDLE_S		20.0f		/* without input: it plays by itself */
#define KEY_HOLD_S	0.3f		/* a console key holds this long (a terminal sends no key-up) */
#define STICK_AT	0.45f		/* the stick this far over: that way */
#define NEW_GAME_S	4.0f		/* playing by itself: the next game after its end */

#ifdef XONIX_RAW_TERMINAL		/* (a PC: the keys as they're pressed, not echoed) */
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

static xn_t game;
static xn_bot_t bot;

/* ---- the sound ---------------------------------------------------------------------- */

enum { CH_STEP, CH_CAPTURE, CH_EVENT, CH_BOUNCE };
#define BOUNCE_CHANNELS	3
#define BOUNCE_PAUSE	0.09f		/* the bounces heard: this far apart at least */

static float clampf (float x, float lo, float hi)	{ return x < lo ? lo : x > hi ? hi : x; }

static void play (unsigned channel, int sound, float volume, float pan, float pitch)
{
	float l = clampf (volume * fminf (1.0f, 1.0f - pan), 0.0f, 1.0f), r = clampf (volume * fminf (1.0f, 1.0f + pan), 0.0f, 1.0f);
	pgpu_sound_play (channel, sound, (uint32_t) (l * PGPU_SOUND_FULL), (uint32_t) (r * PGPU_SOUND_FULL), 0);
	pgpu_sound_pitch (channel, pitch);
}

static float pan_of (int x)		{ return clampf ((x + 0.5f) / XN_COLS * 1.6f - 0.8f, -0.8f, 0.8f); }

static void sounds (const xn_t *g, float now)
{
	static float bounced;
	static unsigned bounce_channel;
	unsigned e = g->events;
	if (e & XN_EV_OVER)
	{
		play (CH_EVENT, XSND_OVER, 0.8f, 0.0f, 1.0f);
	}
	else if (e & XN_EV_DIE)
	{
		pgpu_sound_stop (CH_STEP);
		play (CH_EVENT, XSND_DIE, 0.9f, pan_of (g->died_x), 1.0f);
	}
	else if (e & XN_EV_CLEARED)
	{
		play (CH_EVENT, XSND_CLEARED, 0.8f, 0.0f, 1.0f);
	}
	else if (e & XN_EV_START)
	{
		play (CH_EVENT, XSND_START, 0.6f, 0.0f, 1.0f);
	}
	if (e & XN_EV_CAPTURE)
	{
		float size = clampf (logf ((float) g->captured + 1.0f) / logf (2000.0f), 0.0f, 1.0f);
		play (CH_CAPTURE, XSND_CAPTURE, 0.45f + 0.45f * size, pan_of (g->x), 1.35f - 0.55f * size);
	}
	else if (e & XN_EV_OUT)
	{
		play (CH_STEP, XSND_OUT, 0.35f, pan_of (g->x), 1.0f);
	}
	else if (e & XN_EV_STEP)
	{
		play (CH_STEP, XSND_STEP, 0.22f, pan_of (g->x), 1.0f + 0.004f * (g->trail % 60));
	}
	if ((e & XN_EV_BOUNCE) && now - bounced >= BOUNCE_PAUSE)
	{
		bounced = now;
		play (CH_BOUNCE + bounce_channel++ % BOUNCE_CHANNELS, XSND_BOUNCE, 0.16f, pan_of (g->bounce_x),
		      0.9f + 0.2f * ((g->bounce_x * 7 + g->level) % 5) / 4.0f);
	}
}

/* ---- the picture -------------------------------------------------------------------- */

typedef struct { float x, y, cx, cy; } field_vertex_t;
typedef struct { float x, y, lx, ly; uint8_t rgba[4]; } sprite_vertex_t;

#define MAX_SPRITES	(XN_MAX_SEA + XN_MAX_LAND + 2)
static sprite_vertex_t sprites[MAX_SPRITES * 6];
static int n_sprites;

/* where the field is on the screen: its top left (pixels from the top left), a cell's size */
static float field_x, field_y, cell;
static GLint vp[4];

static void layout (int band)
{
	float w = (float) vp[2], h = (float) (vp[3] - band);
	cell = fminf (w / XN_COLS, h / XN_ROWS);
	if (cell >= 1.0f)
	{
		cell = floorf (cell);			/* (whole pixels: every cell the same) */
	}
	field_x = floorf ((w - cell * XN_COLS) / 2);
	field_y = band + floorf ((h - cell * XN_ROWS) / 2);
}

static float clip_x (float px)		{ return px / vp[2] * 2.0f - 1.0f; }
static float clip_y (float py)		{ return 1.0f - py / vp[3] * 2.0f; }

/* a square over cell (x, y) (fractions: on the way), size cells across; mode in the alpha:
   255 a ball, 128 a ring, 0 a square */
static void sprite (float x, float y, float size, uint8_t r, uint8_t g, uint8_t b, uint8_t mode)
{
	if (n_sprites == MAX_SPRITES)
	{
		return;
	}
	static const float corner[6][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, -1}, {1, 1}, {-1, 1}};
	float mx = field_x + (x + 0.5f) * cell, my = field_y + (y + 0.5f) * cell, half = size * cell / 2;
	sprite_vertex_t *v = &sprites[n_sprites++ * 6];
	for (int k = 0; k < 6; k++, v++)
	{
		v->x = clip_x (mx + corner[k][0] * half);
		v->y = clip_y (my - corner[k][1] * half);
		v->lx = corner[k][0];
		v->ly = corner[k][1];
		v->rgba[0] = r, v->rgba[1] = g, v->rgba[2] = b, v->rgba[3] = mode;
	}
}

static float along (float t_left, float speed)	/* of the way from the cell before to this one */
{
	return clampf (1.0f - t_left * speed, 0.0f, 1.0f);
}

static void build_sprites (const xn_t *g, float now)
{
	n_sprites = 0;
	bool moving = g->state == XN_PLAYING;
	float fs = moving ? along (g->t_sea, xn_sea_speed (g)) : 1.0f, fl = moving ? along (g->t_land, xn_land_speed (g)) : 1.0f;
	for (int i = 0; i < g->n_land; i++)
	{
		const xn_ball_t *b = &g->land[i];
		sprite (b->px + (b->x - b->px) * fl, b->py + (b->y - b->py) * fl, 1.25f, 40, 22, 30, 255);
	}
	for (int i = 0; i < g->n_sea; i++)
	{
		const xn_ball_t *b = &g->sea[i];
		sprite (b->px + (b->x - b->px) * fs, b->py + (b->y - b->py) * fs, 1.25f, 255, 244, 214, 255);
	}
	if (g->state == XN_DYING)
	{
		float t = g->state_t / 1.2f;
		sprite (g->died_x, g->died_y, 2.0f + 14.0f * t, 255, (uint8_t) (200 * (1 - t)), 40, 128);
		return;
	}
	if (g->state == XN_OVER || (g->safe > 0 && fmodf (now, 0.2f) < 0.1f))
	{
		return;					/* (just back: it blinks) */
	}
	float fp = moving && g->dir != XN_NONE ? along (g->t_player, xn_player_speed (g)) : 1.0f;
	float x = g->px + (g->x - g->px) * fp, y = g->py + (g->y - g->py) * fp;
	if (g->at_sea)
	{
		sprite (x, y, 1.35f, 255, 120, 200, 0);
	}
	else
	{
		sprite (x, y, 1.35f, 255, 255, 255, 0);
	}
}

/* ---- the HUD ------------------------------------------------------------------------- */

static void hud_build (const xn_t *g, bool by_itself, bool paused, bool numbers, float scale, const perf_t *perf)
{
	char s[48];
	const float cw = HUD_CHAR_W * scale, band = 16 * scale;
	hud_begin ();
	hud_rect (0, 0, vp[2], band, HUD_RGBA (6, 10, 26, 255));
	snprintf (s, sizeof s, "LV%d", g->level);
	hud_text_scaled (3 * scale, 0, s, HUD_RGBA (120, 230, 240, 255), scale);
	float x = (3 + 4 * 12) * scale;
	hud_rect (x, 4 * scale, 8 * scale, 8 * scale, HUD_RGBA (255, 255, 255, 255));	/* (the lives: the player's square) */
	snprintf (s, sizeof s, "%d", g->lives);
	hud_text_scaled (x + 10 * scale, 0, s, HUD_RGBA (255, 255, 255, 255), scale);
	/* how much is land: a bar, to its goal and on */
	float bx = x + 10 * scale + 2 * cw, bw = vp[2] - bx - 7 * cw - 6 * scale, filled = xn_filled (g) / 100.0f;
	if (bw > 40 * scale)
	{
		hud_rect (bx, 3 * scale, bw, 10 * scale, HUD_RGBA (20, 34, 60, 255));
		hud_rect (bx, 3 * scale, bw * filled, 10 * scale, filled >= XN_GOAL / 100.0f ? HUD_RGBA (250, 220, 90, 255) : HUD_RGBA (13, 140, 158, 255));
		hud_rect (bx + bw * XN_GOAL / 100.0f, 1 * scale, 2 * scale, 14 * scale, HUD_RGBA (250, 220, 90, 255));
		snprintf (s, sizeof s, "%d%%", xn_filled (g));
		hud_text_scaled (bx + bw / 2 - strlen (s) * cw / 4, 4 * scale, s, HUD_RGBA (255, 255, 255, 255), scale / 2);
	}
	snprintf (s, sizeof s, "%06d", g->score % 1000000);
	hud_text_scaled (vp[2] - 6 * cw - 3 * scale, 0, s, HUD_RGBA (255, 230, 120, 255), scale);

	/* what's to be told, in the middle */
	const char *say = NULL;
	char level[24];
	if (paused)
	{
		say = "PAUSED";
	}
	else if (g->state == XN_OVER)
	{
		say = "GAME OVER";
	}
	else if (g->state == XN_CLEARED)
	{
		say = "CLEARED!";
	}
	else if (g->state == XN_PLAYING && g->state_t < 1.5f && !g->trail && g->sea_left == g->inside)
	{
		snprintf (level, sizeof level, "LEVEL %d", g->level);
		say = level;
	}
	if (say)
	{
		float big = 2 * scale, w = strlen (say) * HUD_CHAR_W * big;
		float y = field_y + cell * XN_ROWS / 2 - 8 * big;
		hud_rect (vp[2] / 2 - w / 2 - 8 * scale, y - 4 * scale, w + 16 * scale, 16 * big + 8 * scale, HUD_RGBA (0, 0, 0, 150));
		hud_text_scaled (vp[2] / 2 - w / 2, y, say, HUD_RGBA (255, 240, 200, 255), big);
	}
	if (by_itself)
	{
		const char *demo = "DEMO - MOVE TO PLAY";
		float w = strlen (demo) * HUD_CHAR_W * scale / 2;
		hud_text_scaled (vp[2] / 2 - w / 2, field_y + cell * XN_ROWS - 12 * scale, demo, HUD_RGBA (255, 255, 255, 220), scale / 2);
	}
	if (numbers)
	{
		hud_perf (vp[2] - hud_perf_width (hud_perf_scale (vp[3])) - 2, band + 2, hud_perf_scale (vp[3]), perf);
	}
	hud_end ();
}

/* ---- the input ------------------------------------------------------------------------ */

typedef struct
{
	int dir;				/* held: XN_* (XN_NONE) */
	bool any;				/* anything at all */
	bool pause, numbers, start, restart;	/* (as they're pressed): start: after a game's end */
} input_t;

static void read_input (input_t *in, bool pad_there, float dt)
{
	static unsigned was_buttons;
	static int key_dir = XN_NONE, escape;
	static float key_left;
	memset (in, 0, sizeof *in);
	in->dir = XN_NONE;
	pad_t pad;
	if (pad_there && pad_read (&pad))
	{
		unsigned pressed = pad.buttons & ~was_buttons;
		was_buttons = pad.buttons;
		if (fmaxf (fabsf (pad.x), fabsf (pad.y)) >= STICK_AT)
		{
			in->dir = fabsf (pad.x) > fabsf (pad.y) ? (pad.x > 0 ? XN_RIGHT : XN_LEFT) : (pad.y > 0 ? XN_UP : XN_DOWN);
		}
		in->pause = pressed & PAD_START;
		in->numbers = pressed & PAD_SELECT;
		in->start = pressed & (PAD_A | PAD_B | PAD_X | PAD_Y | PAD_STICK);
		in->any = in->dir != XN_NONE || pressed;
	}
	for (int c; (c = getchar_timeout_us (0)) != PICO_ERROR_TIMEOUT; in->any = true)
	{
		int d = XN_NONE;
		if (escape == 1)
		{
			escape = c == '[' ? 2 : 0;
			continue;
		}
		if (escape == 2)
		{
			d = c == 'A' ? XN_UP : c == 'B' ? XN_DOWN : c == 'C' ? XN_RIGHT : c == 'D' ? XN_LEFT : XN_NONE;
			escape = 0;
		}
		else
		{
			switch (c)
			{
			case 27: escape = 1; continue;
			case 'w': case 'W': d = XN_UP; break;
			case 's': case 'S': d = XN_DOWN; break;
			case 'a': case 'A': d = XN_LEFT; break;
			case 'd': case 'D': d = XN_RIGHT; break;
			case 'p': case 'P': in->pause = true; break;
			case 'h': case 'H': in->numbers = true; break;
			case 'r': case 'R': in->restart = true; break;
			case ' ': case '\r': case '\n': in->start = true; break;
			}
		}
		if (d != XN_NONE)
		{
			key_dir = d;
			key_left = KEY_HOLD_S;
		}
	}
	key_left -= dt;
	if (in->dir == XN_NONE && key_left > 0)
	{
		in->dir = key_dir;
	}
}

/* ---- the game ------------------------------------------------------------------------- */

int main (void)
{
	stdio_init_all ();
	pgpu_init ();
#ifdef XONIX_RAW_TERMINAL
	raw_terminal ();
#endif
	printf ("\nxonix: waiting for the RPi (READY)...\n");
	while (!pgpu_wait_ready (1000))
	{
	}
	pgpu_set_reply_phase (1);
	int tries = 0;
	while (!pglInit () && ++tries < 5)		/* the first reply can be missed */
	{
	}
	bool pad_there = pad_init ();

	GLuint field_prog = glCreateProgram (), sprite_prog = glCreateProgram ();
	glProgramBinaryOES (field_prog, PGL_PROGRAM_BINARY_PGPU, &xonix_field_info, sizeof xonix_field_info);
	glProgramBinaryOES (sprite_prog, PGL_PROGRAM_BINARY_PGPU, &xonix_sprite_info, sizeof xonix_sprite_info);
	GLint linked[2] = {0, 0};
	glGetProgramiv (field_prog, GL_LINK_STATUS, &linked[0]);
	glGetProgramiv (sprite_prog, GL_LINK_STATUS, &linked[1]);
	if (!linked[0] || !linked[1])
	{
		printf ("xonix: a program didn't link\n");
	}
	GLint f_pos = glGetAttribLocation (field_prog, "a_pos"), f_cell = glGetAttribLocation (field_prog, "a_cell");
	GLint u_size = glGetUniformLocation (field_prog, "u_size"), u_fresh = glGetUniformLocation (field_prog, "u_fresh");
	GLint s_pos = glGetAttribLocation (sprite_prog, "a_pos"), s_local = glGetAttribLocation (sprite_prog, "a_local");
	GLint s_color = glGetAttribLocation (sprite_prog, "a_color");
	glUseProgram (field_prog);
	glUniform1i (glGetUniformLocation (field_prog, "u_tex"), 0);
	if (!hud_init ())
	{
		printf ("xonix: the HUD program didn't link\n");
	}
	glDisable (GL_DEPTH_TEST);
	glDisable (GL_CULL_FACE);
	for (int i = 1; i < XSND_COUNT; i++)
	{
		pgpu_sound_data (i, xonix_sounds[i].rate, PGPU_SOUND_U8, xonix_sounds[i].samples, xonix_sounds[i].frames);
	}

	GLuint tex;
	glGenTextures (1, &tex);
	glBindTexture (GL_TEXTURE_2D, tex);
	glPixelStorei (GL_UNPACK_ALIGNMENT, 1);
	glTexImage2D (GL_TEXTURE_2D, 0, GL_LUMINANCE, TEX_W, TEX_H, 0, GL_LUMINANCE, GL_UNSIGNED_BYTE, NULL);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	printf ("xonix: %s; keys: the arrows or w a s d, p pauses, h the numbers, r a new game\n",
		pad_there ? "the stick steers (START pauses, SELECT the numbers, A a new game)" : "no stick here");

	uint32_t seed = time_us_32 () | 1;
	xn_new_game (&game, seed);
	xn_bot_init (&bot, seed * 2654435761u);
	perf_t perf;
	memset (&perf, 0, sizeof perf);
	bool new_perf = true, by_itself = true, paused = false, numbers = false;
	absolute_time_t last = get_absolute_time ();
	float now = 0, idle = IDLE_S, said = 0, over_t = 0;
	char shown[96] = "";
	int frames = 0;
	while (true)
	{
		if (screen_update ("xonix", vp))
		{
			shown[0] = '\0';		/* (the HUD again, the field placed again) */
		}
		const float scale = vp[3] >= 480 ? 2.0f : 1.0f;
		layout ((int) (16 * scale));
		absolute_time_t t0 = get_absolute_time ();
		float dt = absolute_time_diff_us (last, t0) / 1e6f;
		dt = dt > 0.1f ? 0.1f : dt;
		last = t0;
		now += dt;

		input_t in;
		read_input (&in, pad_there, dt);
		idle = in.any ? 0.0f : idle + dt;
		if (in.numbers)
		{
			numbers = !numbers;
		}
		if (by_itself && in.any)		/* someone's there: a game of their own */
		{
			by_itself = false;
			paused = false;
			xn_new_game (&game, time_us_32 () | 1);
		}
		else if (!by_itself && idle >= IDLE_S && !paused)
		{
			by_itself = true;		/* (no one: it plays on by itself) */
			xn_bot_init (&bot, time_us_32 () | 1);
		}
		else if (!by_itself)
		{
			if (in.pause && game.state != XN_OVER)
			{
				paused = !paused;
				play (CH_EVENT, XSND_START, 0.4f, 0.0f, paused ? 0.7f : 1.0f);
			}
			if ((in.start && game.state == XN_OVER) || in.restart)
			{
				paused = false;
				xn_new_game (&game, time_us_32 () | 1);
			}
		}

		if (!paused)
		{
			int want = by_itself ? xn_bot (&bot, &game, dt) : in.dir;
			xn_update (&game, want, dt);
			sounds (&game, now);
		}
		if (game.state == XN_OVER)
		{
			over_t += dt;
			if (by_itself && over_t >= NEW_GAME_S)
			{
				xn_new_game (&game, time_us_32 () | 1);
				xn_bot_init (&bot, time_us_32 () | 1);
			}
		}
		else
		{
			over_t = 0;
		}
		if (now - said >= 5.0f)
		{
			printf ("xonix: level %d, %d%% land, %d lives, score %d, %s; %.1f fps\n", game.level, xn_filled (&game),
				game.lives, game.score, by_itself ? "by itself" : paused ? "paused" : "played", frames / (now - said));
			said = now;
			frames = 0;
		}
		frames++;

		/* the field's rows that changed */
		glBindTexture (GL_TEXTURE_2D, tex);
		if (game.dirty1 >= game.dirty0)
		{
			glTexSubImage2D (GL_TEXTURE_2D, 0, 0, game.dirty0, XN_COLS, game.dirty1 - game.dirty0 + 1, GL_LUMINANCE,
					 GL_UNSIGNED_BYTE, &game.cell[game.dirty0][0]);
			xn_clean (&game);
		}

		glViewport (vp[0], vp[1], vp[2], vp[3]);
		glClearColor (0.0f, 0.0f, 0.0f, 1.0f);
		glClear (GL_COLOR_BUFFER_BIT);

		/* the field: one quad, the cells from the texture */
		float x0 = clip_x (field_x), x1 = clip_x (field_x + cell * XN_COLS);
		float y0 = clip_y (field_y), y1 = clip_y (field_y + cell * XN_ROWS);
		const field_vertex_t quad[6] =
		{
			{x0, y0, 0, 0}, {x1, y0, XN_COLS, 0}, {x1, y1, XN_COLS, XN_ROWS},
			{x0, y0, 0, 0}, {x1, y1, XN_COLS, XN_ROWS}, {x0, y1, 0, XN_ROWS}
		};
		glUseProgram (field_prog);
		float flash = game.fresh_t < 0.7f ? 1.0f - game.fresh_t / 0.7f : 0.0f;
		glUniform4f (u_size, 1.0f / TEX_W, 1.0f / TEX_H, fmodf (now, 1000.0f), flash * flash);
		glUniform1f (u_fresh, game.fresh / 255.0f);
		glBindBuffer (GL_ARRAY_BUFFER, 0);
		glEnableVertexAttribArray (f_pos);
		glEnableVertexAttribArray (f_cell);
		glVertexAttribPointer (f_pos, 2, GL_FLOAT, GL_FALSE, sizeof (field_vertex_t), &quad[0].x);
		glVertexAttribPointer (f_cell, 2, GL_FLOAT, GL_FALSE, sizeof (field_vertex_t), &quad[0].cx);
		glDrawArrays (GL_TRIANGLES, 0, 6);
		glDisableVertexAttribArray (f_cell);

		/* the balls and the player */
		build_sprites (&game, now);
		if (n_sprites)
		{
			glUseProgram (sprite_prog);
			glEnableVertexAttribArray (s_pos);
			glEnableVertexAttribArray (s_local);
			glEnableVertexAttribArray (s_color);
			glVertexAttribPointer (s_pos, 2, GL_FLOAT, GL_FALSE, sizeof (sprite_vertex_t), &sprites[0].x);
			glVertexAttribPointer (s_local, 2, GL_FLOAT, GL_FALSE, sizeof (sprite_vertex_t), &sprites[0].lx);
			glVertexAttribPointer (s_color, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof (sprite_vertex_t), sprites[0].rgba);
			glDrawArrays (GL_TRIANGLES, 0, n_sprites * 6);
			glDisableVertexAttribArray (s_local);
			glDisableVertexAttribArray (s_color);
		}

		/* the HUD: built again when what it shows has changed */
		char now_shown[96];
		snprintf (now_shown, sizeof now_shown, "%d %d %d %d %d %d %d %d %d %d", game.level, game.lives, game.score,
			  xn_filled (&game), game.state, paused, by_itself, numbers,
			  game.state == XN_PLAYING && game.state_t < 1.5f, vp[2]);
		if (strcmp (now_shown, shown) != 0 || (numbers && new_perf))
		{
			strcpy (shown, now_shown);
			hud_build (&game, by_itself, paused, numbers, scale, &perf);
		}
		hud_draw ();
		pglSwapBuffers ();

		absolute_time_t wait_start = get_absolute_time ();
		pgpu_wait_frame (100);			/* pace on the screen */
		new_perf = perf_frame (absolute_time_diff_us (wait_start, get_absolute_time ()), &perf);
	}
}
