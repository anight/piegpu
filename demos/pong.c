/*
 * pong.c - the Pico's side of toy-pong (toy_game.h): two computer players.
 * Each paddle moves to where it predicts the ball will cross its line
 * (bounces off the top and bottom included), aiming off by a random amount
 * each rally, at a limited speed: sometimes that is not enough, and the other
 * side scores. First to 9 wins; then the scores start again. In pixels,
 * origin bottom left (as the shader, shaders/toy_pong.frag).
 */
#include <math.h>
#include <stdlib.h>
#include "toy_game.h"

#define MARGIN		14.0f		/* as the shader */
#define PADDLE_W	4.0f
#define PADDLE_H	24.0f
#define BALL		4.0f
#define PADDLE_SPEED	170.0f		/* pixels per second */
#define SERVE_SPEED	170.0f
#define TRAIL		6

static float W, H;
static float bx, by, vx, vy;		/* the ball */
static float paddle[2], aim[2];		/* centre y, the player's error this rally */
static int score[2];
static float serve_in, since_hit = 10.0f;
static float trail[TRAIL][2];
static unsigned frames;
static GLint u_ball, u_paddles, u_score, u_flash, u_trail;

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
	serve_in = 0.8f;
	aim[0] = frand (-30.0f, 30.0f);
	aim[1] = frand (-30.0f, 30.0f);
	for (int i = 0; i < TRAIL; i++)
	{
		trail[i][0] = bx;
		trail[i][1] = by;
	}
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
	paddle[0] = paddle[1] = H / 2;
	srand (12345);
	serve (rand () & 1);
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

static void play (float dt)
{
	/* the players */
	for (int s = 0; s < 2; s++)
	{
		float line = s ? W - MARGIN - PADDLE_W - BALL : MARGIN + PADDLE_W + BALL;
		bool coming = s ? vx > 0 : vx < 0;
		float target = coming && serve_in <= 0 ? predict (line) + aim[s] : H / 2;
		float step = PADDLE_SPEED * dt;
		float d = target - paddle[s];
		paddle[s] += d > step ? step : d < -step ? -step : d;
		if (paddle[s] < PADDLE_H)
		{
			paddle[s] = PADDLE_H;
		}
		if (paddle[s] > H - PADDLE_H)
		{
			paddle[s] = H - PADDLE_H;
		}
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
	}
	if (by > H - BALL)
	{
		by = 2 * (H - BALL) - by;
		vy = -vy;
	}
	for (int s = 0; s < 2; s++)
	{
		float face = s ? W - MARGIN - PADDLE_W - BALL : MARGIN + PADDLE_W + BALL;
		bool crossing = s ? (vx > 0 && bx >= face) : (vx < 0 && bx <= face);
		float off = by - paddle[s];
		if (crossing && fabsf (off) <= PADDLE_H + BALL)
		{
			/* back out, faster, angled by where it hit the paddle */
			bx = face;
			float speed = sqrtf (vx * vx + vy * vy) * 1.06f;
			if (speed > 420.0f)
			{
				speed = 420.0f;
			}
			float a = off / (PADDLE_H + BALL) * 1.0f;	/* up to ~57 degrees */
			vx = (s ? -1.0f : 1.0f) * speed * cosf (a);
			vy = speed * sinf (a);
			since_hit = 0;
			aim[!s] = frand (-30.0f, 30.0f);		/* the other player's next try */
		}
	}
	if (bx < -BALL || bx > W + BALL)
	{
		int winner = bx < 0;
		if (++score[winner] > 9)
		{
			score[0] = score[1] = 0;
		}
		serve (!winner);
	}
}

void game_frame (float t, float dt)
{
	(void) t;
	if (dt > 0.05f)
	{
		dt = 0.05f;
	}
	play (dt);
	since_hit += dt;

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

	glUniform2f (u_ball, bx, by);
	glUniform2f (u_paddles, paddle[0], paddle[1]);
	glUniform2f (u_score, (float) score[0], (float) score[1]);
	glUniform1f (u_flash, since_hit);
	glUniform2fv (u_trail, TRAIL, &trail[0][0]);
}
