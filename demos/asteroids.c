/*
 * asteroids.c - the Pico's side of toy-asteroids (toy_game.h): the computer
 * flies. It turns to where the nearest rock will be when a shot gets there,
 * fires when lined up, and when a rock comes too close it turns away and
 * burns. Rocks split in two (large, medium, small: 20, 50 and 100 points);
 * a cleared field brings a new wave with one more rock. Three ships a game.
 * In pixels, origin bottom left, the field wrapping round (as the shader,
 * shaders/toy_asteroids.frag).
 */
#include <math.h>
#include <stdbool.h>
#include <stdlib.h>
#include "toy_game.h"

#define MAX_ROCKS	10
#define MAX_SHOTS	6
#define SHOT_SPEED	260.0f
#define SHOT_LIFE	0.9f
#define TURN		4.5f		/* radians per second */
#define THRUST		160.0f		/* pixels per second squared */
#define PI		3.14159265f

typedef struct { float x, y, vx, vy; int size; float seed; } rock_t;	/* size -1: none */
typedef struct { float x, y, vx, vy, life; } shot_t;

static const float radius[3] = {24.0f, 14.0f, 8.0f};
static const int points[3] = {20, 50, 100};

static float W, H;
static rock_t rocks[MAX_ROCKS];
static shot_t shots[MAX_SHOTS];
static float sx, sy, svx, svy, heading;
static bool alive, thrusting;
static float respawn, safe, cooldown, boom_x, boom_y, boom_age = 9.0f, next_wave;
static int score, ships, wave;
static GLint u_ship, u_heading, u_thrust, u_rocks, u_shots, u_boom, u_score;

static float frand (float lo, float hi)
{
	return lo + (hi - lo) * (float) rand () / (float) RAND_MAX;
}

/* the shortest way from a to b round the wrapping field (margin m either side) */
static float wrapd (float d, float span)
{
	while (d > span / 2)
	{
		d -= span;
	}
	while (d < -span / 2)
	{
		d += span;
	}
	return d;
}

static void wrap (float *x, float *y, float m)
{
	if (*x < -m) *x += W + 2 * m;
	if (*x > W + m) *x -= W + 2 * m;
	if (*y < -m) *y += H + 2 * m;
	if (*y > H + m) *y -= H + 2 * m;
}

static void add_rock (float x, float y, int size, float speed)
{
	for (int i = 0; i < MAX_ROCKS; i++)
	{
		if (rocks[i].size < 0)
		{
			float a = frand (0, 2 * PI);
			rocks[i] = (rock_t) {x, y, speed * cosf (a), speed * sinf (a), size, frand (0, 6.28f)};
			return;
		}
	}
}

static void new_wave (void)
{
	int n = 3 + wave++ % 4;
	for (int i = 0; i < n; i++)
	{
		/* at the edges, away from the ship */
		float x = (i & 1) ? frand (0, W) : (rand () & 1 ? -20.0f : W + 20.0f);
		float y = (i & 1) ? (rand () & 1 ? -20.0f : H + 20.0f) : frand (0, H);
		add_rock (x, y, 0, frand (18, 35));
	}
}

static void new_ship (void)
{
	sx = W / 2;
	sy = H / 2;
	svx = svy = 0;
	heading = PI / 2;
	alive = true;
	safe = 2.0f;
}

static void new_game (void)
{
	for (int i = 0; i < MAX_ROCKS; i++)
	{
		rocks[i].size = -1;
	}
	score = 0;
	ships = 3;
	wave = 0;
	new_wave ();
	new_ship ();
}

void game_init (GLuint prog, float width, float height)
{
	W = width;
	H = height;
	u_ship = glGetUniformLocation (prog, "uShip");
	u_heading = glGetUniformLocation (prog, "uHeading");
	u_thrust = glGetUniformLocation (prog, "uThrust");
	u_rocks = glGetUniformLocation (prog, "uRocks");
	u_shots = glGetUniformLocation (prog, "uShots");
	u_boom = glGetUniformLocation (prog, "uBoom");
	u_score = glGetUniformLocation (prog, "uScore");
	srand (2024);
	for (int i = 0; i < MAX_SHOTS; i++)
	{
		shots[i].life = 0;
	}
	new_game ();
}

void game_resize (float width, float height)
{
	float kx = width / W, ky = height / H;
	for (int i = 0; i < MAX_ROCKS; i++)
	{
		rocks[i].x *= kx;
		rocks[i].y *= ky;
	}
	for (int i = 0; i < MAX_SHOTS; i++)
	{
		shots[i].x *= kx;
		shots[i].y *= ky;
	}
	sx *= kx;
	sy *= ky;
	W = width;
	H = height;
}

static void fly (float dt)
{
	thrusting = false;
	if (!alive)
	{
		return;
	}

	/* the nearest rock, and the nearest threat (closing in) */
	int target = -1, threat = -1;
	float best = 1e9f, worst = 1e9f;
	for (int i = 0; i < MAX_ROCKS; i++)
	{
		if (rocks[i].size < 0)
		{
			continue;
		}
		float dx = wrapd (rocks[i].x - sx, W), dy = wrapd (rocks[i].y - sy, H);
		float d = sqrtf (dx * dx + dy * dy) - radius[rocks[i].size];
		if (d < best)
		{
			best = d;
			target = i;
		}
		float closing = -(dx * (rocks[i].vx - svx) + dy * (rocks[i].vy - svy));
		if (d < 38.0f && closing > 0 && d < worst)
		{
			worst = d;
			threat = i;
		}
	}

	float want = heading;
	bool fire = false;
	if (threat >= 0)
	{
		/* turn away and burn */
		float dx = wrapd (rocks[threat].x - sx, W), dy = wrapd (rocks[threat].y - sy, H);
		want = atan2f (-dy, -dx) + 0.4f;
		thrusting = fabsf (wrapd (want - heading, 2 * PI)) < 0.6f;
	}
	else if (target >= 0)
	{
		/* lead the target: where it will be when the shot arrives */
		rock_t *r = &rocks[target];
		float dx = wrapd (r->x - sx, W), dy = wrapd (r->y - sy, H);
		float t = sqrtf (dx * dx + dy * dy) / SHOT_SPEED;
		dx += (r->vx - svx) * t;
		dy += (r->vy - svy) * t;
		want = atan2f (dy, dx);
		fire = fabsf (wrapd (want - heading, 2 * PI)) < 0.1f && best < 150.0f;
		/* drift back towards the middle when idle */
		float cx = W / 2 - sx, cy = H / 2 - sy;
		if (cx * cx + cy * cy > 70.0f * 70.0f && fabsf (wrapd (atan2f (cy, cx) - heading, 2 * PI)) < 0.5f)
		{
			thrusting = true;
		}
	}
	float turn = wrapd (want - heading, 2 * PI);
	float step = TURN * dt;
	heading += turn > step ? step : turn < -step ? -step : turn;
	heading = wrapd (heading, 2 * PI);

	if (thrusting)
	{
		svx += THRUST * cosf (heading) * dt;
		svy += THRUST * sinf (heading) * dt;
	}
	float drag = 1.0f - 0.7f * dt;
	svx *= drag;
	svy *= drag;
	sx += svx * dt;
	sy += svy * dt;
	wrap (&sx, &sy, 0);

	cooldown -= dt;
	if (fire && cooldown <= 0)
	{
		for (int i = 0; i < MAX_SHOTS; i++)
		{
			if (shots[i].life <= 0)
			{
				float c = cosf (heading), s = sinf (heading);
				shots[i] = (shot_t) {sx + 10 * c, sy + 10 * s, svx + SHOT_SPEED * c, svy + SHOT_SPEED * s, SHOT_LIFE};
				cooldown = 0.22f;
				break;
			}
		}
	}
}

static void play (float dt)
{
	fly (dt);

	for (int i = 0; i < MAX_ROCKS; i++)
	{
		if (rocks[i].size >= 0)
		{
			rocks[i].x += rocks[i].vx * dt;
			rocks[i].y += rocks[i].vy * dt;
			wrap (&rocks[i].x, &rocks[i].y, radius[rocks[i].size] * 1.3f);
		}
	}
	for (int j = 0; j < MAX_SHOTS; j++)
	{
		shot_t *s = &shots[j];
		if (s->life <= 0)
		{
			continue;
		}
		s->life -= dt;
		s->x += s->vx * dt;
		s->y += s->vy * dt;
		wrap (&s->x, &s->y, 0);
		for (int i = 0; i < MAX_ROCKS; i++)
		{
			rock_t *r = &rocks[i];
			if (r->size < 0)
			{
				continue;
			}
			float dx = s->x - r->x, dy = s->y - r->y, rr = radius[r->size];
			if (dx * dx + dy * dy < rr * rr)
			{
				s->life = 0;
				score += points[r->size];
				int size = r->size;
				float x = r->x, y = r->y;
				r->size = -1;
				if (size < 2)
				{
					add_rock (x, y, size + 1, frand (30, 55) + 15 * size);
					add_rock (x, y, size + 1, frand (30, 55) + 15 * size);
				}
				break;
			}
		}
	}

	/* the ship against the rocks */
	safe -= dt;
	if (alive && safe <= 0)
	{
		for (int i = 0; i < MAX_ROCKS; i++)
		{
			if (rocks[i].size < 0)
			{
				continue;
			}
			float dx = sx - rocks[i].x, dy = sy - rocks[i].y, rr = radius[rocks[i].size] * 0.9f + 6.0f;
			if (dx * dx + dy * dy < rr * rr)
			{
				alive = false;
				boom_x = sx;
				boom_y = sy;
				boom_age = 0;
				respawn = 2.0f;
				break;
			}
		}
	}
	if (!alive)
	{
		respawn -= dt;
		if (respawn <= 0)
		{
			if (--ships <= 0)
			{
				new_game ();
			}
			else
			{
				new_ship ();
			}
		}
	}

	bool any = false;
	for (int i = 0; i < MAX_ROCKS; i++)
	{
		any |= rocks[i].size >= 0;
	}
	if (!any)
	{
		next_wave += dt;
		if (next_wave > 1.0f)
		{
			next_wave = 0;
			new_wave ();
		}
	}
}

void game_frame (float t, float dt)
{
	if (dt > 0.05f)
	{
		dt = 0.05f;
	}
	play (dt);
	boom_age += dt;

	float rock_u[MAX_ROCKS][4], shot_u[MAX_SHOTS][2];
	for (int i = 0; i < MAX_ROCKS; i++)
	{
		rock_u[i][0] = rocks[i].x;
		rock_u[i][1] = rocks[i].y;
		rock_u[i][2] = rocks[i].size >= 0 ? radius[rocks[i].size] : 0.0f;
		rock_u[i][3] = rocks[i].seed;
	}
	for (int i = 0; i < MAX_SHOTS; i++)
	{
		shot_u[i][0] = shots[i].life > 0 ? shots[i].x : -100.0f;
		shot_u[i][1] = shots[i].life > 0 ? shots[i].y : -100.0f;
	}
	/* blinking while the new ship is safe */
	bool shown = alive && (safe <= 0 || fmodf (t, 0.25f) < 0.15f);
	glUniform4f (u_ship, sx, sy, shown ? 1.0f : 0.0f, 0.0f);
	glUniform2f (u_heading, cosf (heading), sinf (heading));
	glUniform1f (u_thrust, thrusting ? 1.0f : 0.0f);
	glUniform4fv (u_rocks, MAX_ROCKS, &rock_u[0][0]);
	glUniform2fv (u_shots, MAX_SHOTS, &shot_u[0][0]);
	glUniform3f (u_boom, boom_x, boom_y, boom_age);
	glUniform1f (u_score, (float) score);
}
