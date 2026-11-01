/*
 * pong.c - the Pico's side of toy-pong (toy_game.h): the game for two, or for
 * one against the computer. The left paddle is the board's stick and game
 * controller's, the right the Bluetooth controller's (pad.h: players 0 and 1).
 * Nobody playing, the computer plays both sides, silently, and asks for a
 * press. A button pressed (any but SELECT) starts a game with that side played
 * by whoever pressed it; the other side's computer gives way as soon as that
 * side's button is pressed or its stick pushed, the score as it stands, and
 * takes over if a controller goes. The stick moves a paddle up and down, the
 * further the faster; a paddle moving as it hits sends the ball off that way a
 * little.
 * START pauses, SELECT shows the numbers. First to 9 wins. The console's keys:
 * w s the left paddle, i k or the arrows the right, enter or space start and
 * pause, h the numbers.
 *
 * The computer moves a paddle to where it predicts the ball will cross its
 * line (bounces off the top and bottom included), aiming off by a random
 * amount each rally, at a limited speed: sometimes that is not enough, and
 * the other side scores. In pixels, origin bottom left (as the shader,
 * shaders/toy_pong.frag). The words are a texture written here, in the old
 * games' font (arcade_font.h), when they change.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pico/stdlib.h"
#include "pgpu.h"
#include "pad.h"
#include "arcade_font.h"
#include "pong_sounds.h"
#include "toy_game.h"

#define MARGIN		14.0f		/* as the shader */
#define PADDLE_W	4.0f
#define PADDLE_H	24.0f
#define BALL		4.0f
#define PADDLE_SPEED	170.0f		/* the computer's, pixels per second */
#define PLAYER_SPEED	300.0f		/* a player's, the stick right over */
#define SPIN		0.35f		/* radians on the ball's way off, the paddle at a player's full speed */
#define SERVE_SPEED	170.0f
#define MAX_SPEED	420.0f
#define SERVE_S		1.0f
#define TRAIL		6
#define WIN_SCORE	9
#define OVER_S		6.0f		/* a game's end shown this long */
#define AGAIN_S		1.0f		/* (a press in the first of it doesn't start the next) */
#define KEY_HOLD_S	0.3f		/* a console key holds this long (a terminal sends no key-up) */
#define KEY_MOVE	0.75f		/* a console key: the stick this far over */
#define STICK_JOINS	0.5f		/* a stick this far over is someone wanting that side */
#define TEXT_UNIT	1		/* the HUD binds its font to unit 0 each frame */
#define TEXT_W		256
#define TEXT_H		16
#define LABEL_W		32

enum { CH_BALL, CH_EVENT };
enum { ATTRACT, PLAYING, PAUSED, OVER };

#ifdef PONG_RAW_TERMINAL		/* (a PC: the keys as they're pressed, not echoed) */
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

typedef struct
{
	bool there;			/* its controller */
	bool human;			/* played by someone, not the computer */
	bool by_keys;			/* (on the console's keys: no controller to lose) */
	float move;			/* -1 .. 1, up */
	bool press, pause;		/* just now: a button, START */
	bool pushed;			/* its stick over (STICK_JOINS) */
	unsigned was;			/* its buttons, the last frame */
} side_t;

static float W, H;
static float bx, by, vx, vy;		/* the ball */
static float paddle[2], aim[2];		/* centre y, the computer's error this rally */
static float paddle_v[2];		/* pixels per second */
static side_t side[2];
static int state = ATTRACT, score[2];
static float serve_in, since_hit = 10.0f, over_left;
static const char *verdict = "";	/* a game's end, in the winner's colour */
static int winner_side;
static float trail[TRAIL][2];
static unsigned frames;
static bool pad_there, numbers;
static GLint u_ball, u_paddles, u_score, u_flash, u_trail, u_chars, u_message_color;
static GLuint text_texture;

static float frand (float lo, float hi)
{
	return lo + (hi - lo) * (float) rand () / (float) RAND_MAX;
}

static void serve (int towards)
{
	bx = W / 2;
	by = H / 2;
	float a = frand (-0.5f, 0.5f);
	vx = (towards ? 1.0f : -1.0f) * SERVE_SPEED * cosf (a);
	vy = SERVE_SPEED * sinf (a);
	serve_in = SERVE_S;
	aim[0] = frand (-30.0f, 30.0f);
	aim[1] = frand (-30.0f, 30.0f);
	for (int i = 0; i < TRAIL; i++)
	{
		trail[i][0] = bx;
		trail[i][1] = by;
	}
}

/* at x across the screen, its pitch as given; the computer alone plays silently */
static void sound (unsigned channel, int id, float volume, float x, float pitch)
{
	if (state == ATTRACT)
	{
		return;
	}
	float pan = x / W - 0.5f;
	pan = pan < -0.5f ? -0.5f : pan > 0.5f ? 0.5f : pan;
	float l = volume * fminf (1.0f, 1.0f - pan), r = volume * fminf (1.0f, 1.0f + pan);
	pgpu_sound_play (channel, id, (uint32_t) (l * PGPU_SOUND_FULL), (uint32_t) (r * PGPU_SOUND_FULL), 0);
	pgpu_sound_pitch (channel, pitch);
}

/* the computer alone, the game's end gone */
static void attract (void)
{
	state = ATTRACT;
	side[0].human = side[1].human = false;
	score[0] = score[1] = 0;
	serve (rand () & 1);
}

static void new_game (void)
{
	state = PLAYING;
	score[0] = score[1] = 0;
	serve (rand () & 1);
	sound (CH_EVENT, PSND_START, 0.8f, W / 2, 1.0f);
	printf ("pong: a game, left %s, right %s\n", side[0].human ? "played" : "the computer",
		side[1].human ? "played" : "the computer");
}

static void join (int s)
{
	side[s].human = true;
	side[s].by_keys = !side[s].there;
}

static void game_over (int winner)
{
	state = OVER;
	over_left = OVER_S;
	winner_side = winner;
	verdict = side[0].human && side[1].human ? (winner ? "PLAYER 2 WINS" : "PLAYER 1 WINS")
		: side[winner].human ? "YOU WIN" : "CPU WINS";
	serve (0);				/* (the ball waits in the middle) */
	sound (CH_EVENT, PSND_WIN, 0.9f, W / 2, 1.0f);
	printf ("pong: %s, %d : %d\n", verdict, score[0], score[1]);
}

/* where the ball crosses x (bouncing between BALL and H - BALL) */
static float predict (float x)
{
	float t = (x - bx) / vx;
	if (t < 0)
	{
		return H / 2;
	}
	float lo = BALL, span = H - 2 * BALL;
	float y = fmodf (by - lo + vy * t, 2 * span);
	if (y < 0)
	{
		y += 2 * span;
	}
	return lo + (y < span ? y : 2 * span - y);
}

void game_init (GLuint prog, float width, float height)
{
	W = width;
	H = height;
	u_ball = glGetUniformLocation (prog, "uBall");
	u_paddles = glGetUniformLocation (prog, "uPaddles");
	u_score = glGetUniformLocation (prog, "uScore");
	u_flash = glGetUniformLocation (prog, "uFlash");
	u_trail = glGetUniformLocation (prog, "uTrail");
	u_chars = glGetUniformLocation (prog, "uChars");
	u_message_color = glGetUniformLocation (prog, "uMessageColor");
	paddle[0] = paddle[1] = H / 2;
	srand (12345);
	serve (rand () & 1);
#ifdef PONG_RAW_TERMINAL
	raw_terminal ();
#endif
	pad_there = pad_init ();
	for (int i = 1; i < PSND_COUNT; i++)
	{
		pgpu_sound_data (i, pong_sounds[i].rate, PGPU_SOUND_U8, pong_sounds[i].samples, pong_sounds[i].frames);
	}
	glGenTextures (1, &text_texture);
	glActiveTexture (GL_TEXTURE0 + TEXT_UNIT);
	glBindTexture (GL_TEXTURE_2D, text_texture);
	glTexImage2D (GL_TEXTURE_2D, 0, GL_LUMINANCE, TEXT_W, TEXT_H, 0, GL_LUMINANCE, GL_UNSIGNED_BYTE, NULL);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glActiveTexture (GL_TEXTURE0);
	glUniform1i (glGetUniformLocation (prog, "uText"), TEXT_UNIT);
	printf ("pong: %s; keys: w s the left paddle, i k or the arrows the right, enter starts and pauses, h the numbers\n",
		pad_there ? "the left paddle's the stick's, the right the Bluetooth controller's: a button starts, a button or the stick joins" : "no pads here");
}

void game_resize (float width, float height)
{
	float kx = width / W, ky = height / H;
	bx *= kx;
	by *= ky;
	for (int i = 0; i < TRAIL; i++)
	{
		trail[i][0] *= kx;
		trail[i][1] *= ky;
	}
	paddle[0] *= ky;
	paddle[1] *= ky;
	W = width;
	H = height;
}

/* the buttons and sticks, and the console's keys, into side[] */
static void read_input (float dt)
{
	static int escape, key_dir[2];
	static float key_left[2];
	for (int s = 0; s < 2; s++)
	{
		side_t *p = &side[s];
		pad_t pad;
		p->there = pad_there && pad_read_player (s, &pad);
		p->press = p->pause = p->pushed = false;
		p->move = 0;
		if (p->there)
		{
			unsigned pressed = pad.buttons & ~p->was;
			p->was = pad.buttons;
			p->move = pad.y;
			p->pushed = fabsf (pad.y) > STICK_JOINS;
			p->press = pressed & (PAD_A | PAD_B | PAD_X | PAD_Y | PAD_START | PAD_STICK);
			p->pause = pressed & PAD_START;
			numbers ^= (pressed & PAD_SELECT) != 0;
		}
		else
		{
			p->was = 0;
		}
	}
	for (int c; (c = getchar_timeout_us (0)) != PICO_ERROR_TIMEOUT; )
	{
		int s = 0, d = 0;
		if (escape == 1)
		{
			escape = c == '[' ? 2 : 0;
			continue;
		}
		if (escape == 2)
		{
			s = 1;
			d = c == 'A' ? 1 : c == 'B' ? -1 : 0;
			escape = 0;
		}
		else
		{
			switch (c)
			{
			case 27: escape = 1; continue;
			case 'w': case 'W': d = 1; break;
			case 's': case 'S': d = -1; break;
			case 'i': case 'I': s = 1; d = 1; break;
			case 'k': case 'K': s = 1; d = -1; break;
			case ' ': case '\r': case '\n': side[0].press = side[0].pause = true; break;
			case 'h': case 'H': numbers = !numbers; break;
			}
		}
		if (d)
		{
			key_dir[s] = d;
			key_left[s] = KEY_HOLD_S;
			side[s].press = true;		/* (a key's meant: it joins as a button does) */
		}
	}
	for (int s = 0; s < 2; s++)
	{
		key_left[s] -= dt;
		if (side[s].move == 0 && key_left[s] > 0)
		{
			side[s].move = key_dir[s] * KEY_MOVE;
		}
	}
}

static void play (float dt)
{
	/* the paddles */
	for (int s = 0; s < 2; s++)
	{
		float was = paddle[s];
		if (side[s].human)
		{
			paddle[s] += side[s].move * fabsf (side[s].move) * PLAYER_SPEED * dt;
		}
		else
		{
			float line = s ? W - MARGIN - PADDLE_W - BALL : MARGIN + PADDLE_W + BALL;
			bool coming = s ? vx > 0 : vx < 0;
			float target = coming && serve_in <= 0 ? predict (line) + aim[s] : H / 2;
			float step = PADDLE_SPEED * dt;
			float d = target - paddle[s];
			paddle[s] += d > step ? step : d < -step ? -step : d;
		}
		if (paddle[s] < PADDLE_H)
		{
			paddle[s] = PADDLE_H;
		}
		if (paddle[s] > H - PADDLE_H)
		{
			paddle[s] = H - PADDLE_H;
		}
		paddle_v[s] = dt > 0 ? (paddle[s] - was) / dt : 0;
	}

	if (serve_in > 0)
	{
		serve_in -= dt;
		return;
	}

	/* the ball */
	bx += vx * dt;
	by += vy * dt;
	if (by < BALL)
	{
		by = 2 * BALL - by;
		vy = -vy;
		sound (CH_BALL, PSND_WALL, 0.5f, bx, 1.0f);
	}
	if (by > H - BALL)
	{
		by = 2 * (H - BALL) - by;
		vy = -vy;
		sound (CH_BALL, PSND_WALL, 0.5f, bx, 1.0f);
	}
	for (int s = 0; s < 2; s++)
	{
		float face = s ? W - MARGIN - PADDLE_W - BALL : MARGIN + PADDLE_W + BALL;
		float behind = s ? W - MARGIN + PADDLE_W + BALL : MARGIN - PADDLE_W - BALL;
		bool crossing = s ? (vx > 0 && bx >= face && bx <= behind) : (vx < 0 && bx <= face && bx >= behind);
		float off = by - paddle[s];
		if (crossing && fabsf (off) <= PADDLE_H + BALL)
		{
			/* back out, faster, angled by where it hit the paddle and how it was moving */
			bx = face;
			float speed = sqrtf (vx * vx + vy * vy) * 1.06f;
			if (speed > MAX_SPEED)
			{
				speed = MAX_SPEED;
			}
			float a = off / (PADDLE_H + BALL) * 1.0f + SPIN * paddle_v[s] / PLAYER_SPEED;
			a = a < -1.0f ? -1.0f : a > 1.0f ? 1.0f : a;	/* up to ~57 degrees */
			vx = (s ? -1.0f : 1.0f) * speed * cosf (a);
			vy = speed * sinf (a);
			since_hit = 0;
			aim[!s] = frand (-30.0f, 30.0f);		/* the other player's next try */
			sound (CH_BALL, PSND_HIT, 0.7f, bx, sqrtf (speed / SERVE_SPEED));
		}
	}
	if (bx < -BALL || bx > W + BALL)
	{
		int winner = bx < 0;
		sound (CH_EVENT, PSND_POINT, 0.8f, bx, 1.0f);
		if (++score[winner] < WIN_SCORE)
		{
			serve (!winner);
		}
		else if (state == PLAYING)
		{
			game_over (winner);
		}
		else
		{
			attract ();
		}
	}
}

static void update (float dt)
{
	read_input (dt);
	for (int s = 0; s < 2; s++)
	{
		if (side[s].human && !side[s].by_keys && !side[s].there)
		{
			side[s].human = false;			/* (its controller went: the computer plays on) */
			printf ("pong: %s controller's gone\n", s ? "the right" : "the left");
		}
	}
	bool pressed = side[0].press || side[1].press;
	switch (state)
	{
	case ATTRACT:
		if (pressed)
		{
			for (int s = 0; s < 2; s++)
			{
				if (side[s].press)
				{
					join (s);
				}
			}
			new_game ();
		}
		break;
	case PLAYING:
	case PAUSED:
		for (int s = 0; s < 2; s++)
		{
			if ((side[s].press || side[s].pushed) && !side[s].human)
			{
				join (s);
				sound (CH_EVENT, PSND_JOIN, 0.8f, s ? W : 0, 1.0f);
				printf ("pong: the %s joins, %d : %d\n", s ? "right" : "left", score[0], score[1]);
			}
			else if (side[s].pause && side[s].human)
			{
				state = state == PAUSED ? PLAYING : PAUSED;
			}
		}
		break;
	case OVER:
		over_left -= dt;
		if (over_left <= 0)
		{
			attract ();
		}
		else if (pressed && over_left < OVER_S - AGAIN_S)
		{
			for (int s = 0; s < 2; s++)
			{
				if (side[s].press)
				{
					join (s);
				}
			}
			new_game ();
		}
		break;
	}
	if (state == ATTRACT || state == PLAYING)
	{
		play (dt);
	}
}

/* ---- the words --------------------------------------------------------------------- */

static void text_put (uint8_t pixels[TEXT_H][TEXT_W], int x, int y, const char *s)
{
	for (; *s; s++, x += 8)
	{
		const char *glyph = arcade_glyph (*s);
		for (int row = 0; glyph && row < 7; row++)
		{
			for (int col = 0; col < 5; col++)
			{
				if (glyph[row * 5 + col] == '#')
				{
					pixels[y + row][x + 1 + col] = 255;
				}
			}
		}
	}
}

/* the texture, when they've changed */
static void words (const char *message, const char *left, const char *right)
{
	static const char *said[3];
	static uint8_t pixels[TEXT_H][TEXT_W];
	if (message == said[0] && left == said[1] && right == said[2])
	{
		return;
	}
	said[0] = message;
	said[1] = left;
	said[2] = right;
	memset (pixels, 0, sizeof pixels);
	text_put (pixels, 0, 0, message);
	text_put (pixels, (LABEL_W - 8 * (int) strlen (left)) / 2, 8, left);
	text_put (pixels, LABEL_W + (LABEL_W - 8 * (int) strlen (right)) / 2, 8, right);
	glActiveTexture (GL_TEXTURE0 + TEXT_UNIT);
	glBindTexture (GL_TEXTURE_2D, text_texture);
	glTexSubImage2D (GL_TEXTURE_2D, 0, 0, 0, TEXT_W, TEXT_H, GL_LUMINANCE, GL_UNSIGNED_BYTE, pixels);
	glActiveTexture (GL_TEXTURE0);
}

void game_frame (float t, float dt)
{
	if (dt > 0.05f)
	{
		dt = 0.05f;
	}
	update (dt);
	since_hit += dt;
	game_hide_hud = !numbers;

	/* the trail: a place every other frame */
	if (++frames % 2 == 0)
	{
		for (int i = TRAIL - 1; i > 0; i--)
		{
			trail[i][0] = trail[i - 1][0];
			trail[i][1] = trail[i - 1][1];
		}
		trail[0][0] = bx;
		trail[0][1] = by;
	}

	/* each side's label: who's playing it; with nobody yet, whose controller's there */
	static const char *const names[2] = {"P1", "P2"};
	const char *label[2];
	for (int s = 0; s < 2; s++)
	{
		label[s] = state == ATTRACT ? (side[s].there ? names[s] : "") : side[s].human ? names[s] : "CPU";
	}
	const char *message = state == ATTRACT ? "PRESS START" : state == PAUSED ? "PAUSED" : state == OVER ? verdict : "";
	words (message, label[0], label[1]);
	bool off = state == ATTRACT && fmodf (t, 1.2f) > 0.8f;		/* (it blinks) */
	glUniform1f (u_chars, off ? 0.0f : (float) strlen (message));
	static const float colours[3][3] = {{0.2f, 0.8f, 1.0f}, {1.0f, 0.3f, 0.7f}, {1.0f, 0.9f, 0.55f}};
	glUniform3fv (u_message_color, 1, colours[state == OVER ? winner_side : 2]);

	glUniform2f (u_ball, bx, by);
	glUniform2f (u_paddles, paddle[0], paddle[1]);
	glUniform2f (u_score, (float) score[0], (float) score[1]);
	glUniform1f (u_flash, since_hit);
	glUniform2fv (u_trail, TRAIL, &trail[0][0]);
}
