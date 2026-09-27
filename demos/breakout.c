/*
 * breakout - a self-playing 3D Breakout, after Atari's 1976 arcade game;
 * written for pgl (plain GL ES 2.0). The Pico has no pins left for buttons,
 * so the paddle plays itself: it predicts where the ball comes down (with the
 * wall bounces) and angles the ball at a brick still standing; now and then
 * it is sloppy, and at higher levels too slow.
 *
 * Drawing: lit blocks (shaders/blocks.*): walls, all bricks in one vertex
 * buffer (a broken brick's vertices are zeroed: its triangles vanish), the
 * paddle, the ball (indexed, u8); additive glowing points (shaders/glow.*):
 * stars, sparks of broken bricks, the ball's trail; the HUD (hud.c).
 */
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "gles/pgl.h"
#include "pgpu.h"
#include "hud.h"
#include "pgpu_perf.h"
#include "screen.h"
#include "mat4.h"
#include "blocks_program.h"
#include "glow_program.h"

#define PI		3.14159265f

/* the field, world units in the z = 0 plane: the walls' inner sides are at
   x = +-FIELD_X and y = FIELD_TOP, the bottom is open */
#define FIELD_X		10.0f
#define FIELD_TOP	7.6f
#define BOTTOM		-9.0f
#define COLS		10
#define ROWS		6
#define BRICK_HW	0.92f		/* half sizes */
#define BRICK_HH	0.33f
#define BRICK_HD	0.5f
#define BRICK_TOP_Y	6.4f		/* centre of the top row */
#define BRICK_PITCH_X	2.0f
#define BRICK_PITCH_Y	0.8f
#define PADDLE_Y	-6.8f
#define PADDLE_HW	1.6f
#define PADDLE_HH	0.25f
#define BALL_R		0.3f
#define MAX_BOUNCE	60.0f		/* degrees from vertical at the paddle's ends */

/* ---- geometry ------------------------------------------------------------------ */

typedef struct { float x, y, z, nx, ny, nz; uint8_t rgba[4]; } vertex_t;
typedef struct { float x, y, z; uint8_t rgba[4]; } point_t;

#define BOX_VERTICES	36

/* a box centred at c with half sizes h: 6 faces, counter-clockwise from outside */
static void box (vertex_t *v, const float c[3], const float h[3], const uint8_t rgba[4])
{
	static const float corner[4][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
	static const int front[6] = {0, 1, 2, 0, 2, 3}, back[6] = {0, 2, 1, 0, 3, 2};
	for (int f = 0; f < 6; f++)
	{
		int a = f / 2, b = (a + 1) % 3, d = (a + 2) % 3;	/* (b, d, a) right-handed */
		float s = f & 1 ? -1.0f : 1.0f;
		const int *order = s > 0 ? front : back;
		for (int i = 0; i < 6; i++)
		{
			float p[3], n[3] = {0, 0, 0};
			p[a] = c[a] + s * h[a];
			p[b] = c[b] + corner[order[i]][0] * h[b];
			p[d] = c[d] + corner[order[i]][1] * h[d];
			n[a] = s;
			*v++ = (vertex_t) {p[0], p[1], p[2], n[0], n[1], n[2],
					   {rgba[0], rgba[1], rgba[2], rgba[3]}};
		}
	}
}

/* a unit sphere, indexed: (STACKS + 1) * (SLICES + 1) vertices */
#define STACKS		6
#define SLICES		10
#define SPHERE_VERTICES	((STACKS + 1) * (SLICES + 1))
#define SPHERE_INDICES	(STACKS * SLICES * 6)

static void sphere (vertex_t *v, uint8_t *index, const uint8_t rgba[4])
{
	for (int i = 0; i <= STACKS; i++)
	{
		float phi = PI * i / STACKS;			/* from the +z pole */
		for (int j = 0; j <= SLICES; j++)
		{
			float th = 2 * PI * j / SLICES;
			float x = sinf (phi) * cosf (th), y = sinf (phi) * sinf (th), z = cosf (phi);
			*v++ = (vertex_t) {x, y, z, x, y, z, {rgba[0], rgba[1], rgba[2], rgba[3]}};
		}
	}
	for (int i = 0; i < STACKS; i++)
	{
		for (int j = 0; j < SLICES; j++)
		{
			uint8_t a = i * (SLICES + 1) + j, b = a + SLICES + 1;
			const uint8_t t[6] = {a, b, a + 1, a + 1, b, b + 1};	/* phi x theta = outwards */
			memcpy (index, t, 6);
			index += 6;
		}
	}
}

/* ---- random numbers (xorshift) -------------------------------------------------- */

static uint32_t rng = 0x2545F491u;

static float frand (void)				/* [0, 1) */
{
	rng ^= rng << 13;
	rng ^= rng >> 17;
	rng ^= rng << 5;
	return (rng >> 8) / 16777216.0f;
}

static float srand1 (void)				/* [-1, 1) */
{
	return 2.0f * frand () - 1.0f;
}

/* ---- particles --------------------------------------------------------------------- */

#define MAX_PARTICLES	256

typedef struct
{
	float x, y, z, vx, vy, vz;
	float life, max_life, gravity;
	uint8_t rgb[3];
} particle_t;

static particle_t particles[MAX_PARTICLES];
static unsigned n_particles;

static void emit (float x, float y, float z, float vx, float vy, float vz, float life,
		  float gravity, const uint8_t rgb[3])
{
	if (n_particles == MAX_PARTICLES)
	{
		return;
	}
	particles[n_particles++] = (particle_t) {x, y, z, vx, vy, vz, life, life, gravity,
						 {rgb[0], rgb[1], rgb[2]}};
}

static void burst (float x, float y, unsigned n, float speed, const uint8_t rgb[3])
{
	for (unsigned i = 0; i < n; i++)
	{
		emit (x + 0.5f * srand1 (), y + 0.2f * srand1 (), 0.3f * srand1 (),
		      speed * srand1 (), speed * (srand1 () + 0.4f), speed * 1.2f * frand (),
		      0.6f + 0.6f * frand (), 12.0f, rgb);
	}
}

static void particles_update (float dt)
{
	float drag = 1.0f - 1.2f * dt;
	for (unsigned i = 0; i < n_particles; )
	{
		particle_t *p = &particles[i];
		p->life -= dt;
		if (p->life <= 0.0f)
		{
			*p = particles[--n_particles];
			continue;
		}
		p->vy -= p->gravity * dt;
		p->vx *= drag;
		p->vy *= drag;
		p->vz *= drag;
		p->x += p->vx * dt;
		p->y += p->vy * dt;
		p->z += p->vz * dt;
		i++;
	}
}

/* ---- the game ------------------------------------------------------------------------ */

enum { SERVE, PLAY, LOST, CLEARED, OVER };

static const uint8_t row_colors[ROWS][4] =
{
	{255, 60, 90, 255}, {255, 140, 40, 255}, {250, 220, 50, 255},
	{70, 230, 90, 255}, {40, 190, 255, 255}, {170, 90, 255, 255},
};

static struct
{
	int state;
	float timer;
	float bx, by, vx, vy, speed;
	float px;				/* paddle centre */
	int aim_row, aim_col;			/* the brick the paddle aims at */
	bool aim_valid;
	float aim_error;
	bool alive[ROWS][COLS];
	int left, score, lives, level;
	float shake;
} G;

static GLuint brick_buffer;

static float brick_x (int c)	{ return (c - (COLS - 1) / 2.0f) * BRICK_PITCH_X; }
static float brick_y (int r)	{ return BRICK_TOP_Y - r * BRICK_PITCH_Y; }

static void new_level (void)
{
	static vertex_t v[BOX_VERTICES];
	for (int r = 0; r < ROWS; r++)
	{
		for (int c = 0; c < COLS; c++)
		{
			const float centre[3] = {brick_x (c), brick_y (r), 0.0f};
			const float half[3] = {BRICK_HW, BRICK_HH, BRICK_HD};
			box (v, centre, half, row_colors[r]);
			glBindBuffer (GL_ARRAY_BUFFER, brick_buffer);
			glBufferSubData (GL_ARRAY_BUFFER, (r * COLS + c) * sizeof v, sizeof v, v);
			G.alive[r][c] = true;
		}
	}
	G.left = ROWS * COLS;
	G.state = SERVE;
	G.timer = 1.5f;
	G.aim_valid = false;
}

static void new_game (void)
{
	G.score = 0;
	G.lives = 3;
	G.level = 1;
	G.px = 0.0f;
	new_level ();
}

static void break_brick (int r, int c)
{
	static const vertex_t zero[BOX_VERTICES];	/* all at the origin: no area */
	G.alive[r][c] = false;
	glBindBuffer (GL_ARRAY_BUFFER, brick_buffer);
	glBufferSubData (GL_ARRAY_BUFFER, (r * COLS + c) * sizeof zero, sizeof zero, zero);
	G.left--;
	G.score += (ROWS - r) * 10 * G.level;
	G.shake += 0.12f;
	burst (brick_x (c), brick_y (r), 14, 5.0f, row_colors[r]);
	if (r == G.aim_row && c == G.aim_col)
	{
		G.aim_valid = false;
	}
	if (!G.left)
	{
		G.state = CLEARED;
		G.timer = 2.5f;
		for (int i = 0; i < 6; i++)
		{
			burst (srand1 () * 8.0f, 2.0f + 4.0f * frand (), 16, 7.0f, row_colors[i]);
		}
	}
}

/* x at the paddle's height, the side walls folded in (the ball bounces) */
static float fold (float x)
{
	float l = FIELD_X - BALL_R, u = fmodf (x + l, 4 * l);
	if (u < 0)
	{
		u += 4 * l;
	}
	return (u > 2 * l ? 4 * l - u : u) - l;
}

static void paddle_ai (float dt, float t)
{
	float hit_y = PADDLE_Y + PADDLE_HH + BALL_R, target = 2.5f * sinf (t * 0.9f);
	if (G.state == PLAY && G.vy < 0.0f)
	{
		float x = fold (G.bx + G.vx * (G.by - hit_y) / -G.vy);
		if (!G.aim_valid)
		{
			/* aim at a random standing brick; now and then badly */
			int n = (int) (frand () * G.left);
			for (int r = 0; r < ROWS; r++)
				for (int c = 0; c < COLS; c++)
					if (G.alive[r][c] && n-- == 0)
					{
						G.aim_row = r;
						G.aim_col = c;
					}
			G.aim_error = frand () < 0.07f ? 1.4f * srand1 () : 0.1f * srand1 ();
			G.aim_valid = true;
		}
		float angle = atan2f (brick_x (G.aim_col) - x, brick_y (G.aim_row) - hit_y) * 180.0f / PI;
		angle = angle > 55.0f ? 55.0f : angle < -55.0f ? -55.0f : angle;
		target = x - angle / MAX_BOUNCE * (PADDLE_HW + BALL_R) + G.aim_error;
	}
	else if (G.state == PLAY)
	{
		/* going up: under where it comes down, if no brick is in the way */
		float up = (FIELD_TOP - BALL_R - G.by) / G.vy, down = (FIELD_TOP - BALL_R - hit_y) / G.vy;
		target = fold (G.bx + G.vx * (up + down));
	}

	float step = (14.0f + 1.5f * G.level) * dt, d = target - G.px;
	G.px += d > step ? step : d < -step ? -step : d;
	float limit = FIELD_X - PADDLE_HW;
	G.px = G.px > limit ? limit : G.px < -limit ? -limit : G.px;
}

static void launch (void)
{
	float a = srand1 () * 30.0f * PI / 180.0f;
	G.speed = 10.0f + 1.5f * (G.level - 1);
	G.speed = G.speed > 18.0f ? 18.0f : G.speed;
	G.vx = G.speed * sinf (a);
	G.vy = G.speed * cosf (a);
	G.state = PLAY;
}

static void ball_physics (float dt)
{
	static const uint8_t cyan[3] = {110, 230, 255}, red[3] = {255, 70, 60};
	int steps = (int) (G.speed * dt / 0.1f) + 1;
	float h = dt / steps;
	for (int s = 0; s < steps; s++)
	{
		G.bx += G.vx * h;
		G.by += G.vy * h;
		if (G.bx < -FIELD_X + BALL_R)
		{
			G.bx = -FIELD_X + BALL_R;
			G.vx = fabsf (G.vx);
		}
		if (G.bx > FIELD_X - BALL_R)
		{
			G.bx = FIELD_X - BALL_R;
			G.vx = -fabsf (G.vx);
		}
		if (G.by > FIELD_TOP - BALL_R)
		{
			G.by = FIELD_TOP - BALL_R;
			G.vy = -fabsf (G.vy);
		}

		/* bricks: the ball as a box; bounce off the side it came through */
		if (G.by > brick_y (ROWS - 1) - BRICK_HH - BALL_R)
		{
			for (int r = 0; r < ROWS; r++)
				for (int c = 0; c < COLS; c++)
				{
					float dx = G.bx - brick_x (c), dy = G.by - brick_y (r);
					float ox = BRICK_HW + BALL_R - fabsf (dx), oy = BRICK_HH + BALL_R - fabsf (dy);
					if (!G.alive[r][c] || ox <= 0.0f || oy <= 0.0f)
					{
						continue;
					}
					if (ox < oy)
						G.vx = copysignf (fabsf (G.vx), dx);
					else
						G.vy = copysignf (fabsf (G.vy), dy);
					break_brick (r, c);
					if (G.state != PLAY)
					{
						return;
					}
					goto next_step;
				}
		}

		/* the paddle: the angle depends on where it hits */
		if (   G.vy < 0.0f && G.by - BALL_R <= PADDLE_Y + PADDLE_HH
		    && G.by > PADDLE_Y - PADDLE_HH && fabsf (G.bx - G.px) <= PADDLE_HW + BALL_R)
		{
			float rel = (G.bx - G.px) / (PADDLE_HW + BALL_R);
			float a = rel * MAX_BOUNCE * PI / 180.0f;
			G.speed = G.speed * 1.01f < 20.0f ? G.speed * 1.01f : 20.0f;
			G.vx = G.speed * sinf (a);
			G.vy = G.speed * cosf (a);
			G.by = PADDLE_Y + PADDLE_HH + BALL_R;
			G.aim_valid = false;
			burst (G.bx, G.by - BALL_R, 5, 2.5f, cyan);
		}

		if (G.by < BOTTOM)
		{
			G.lives--;
			G.state = LOST;
			G.timer = 1.5f;
			G.shake += 0.6f;
			burst (G.bx, PADDLE_Y, 30, 6.0f, red);
			return;
		}
	next_step:
		;
	}
}

static void game_update (float dt, float t)
{
	static const uint8_t trail[3] = {120, 200, 255};
	paddle_ai (dt, t);
	switch (G.state)
	{
	case SERVE:
		G.bx = G.px;
		G.by = PADDLE_Y + PADDLE_HH + BALL_R;
		if ((G.timer -= dt) <= 0.0f)
		{
			launch ();
		}
		break;

	case PLAY:
		ball_physics (dt);
		for (int i = 0; i < 2; i++)
		{
			emit (G.bx, G.by, 0.0f, 0.4f * srand1 (), 0.4f * srand1 (), 0.4f * srand1 (),
			      0.35f, 0.0f, trail);
		}
		break;

	case LOST:
		if ((G.timer -= dt) <= 0.0f)
		{
			if (G.lives > 0)
			{
				G.state = SERVE;
				G.timer = 1.0f;
			}
			else
			{
				G.state = OVER;
				G.timer = 3.0f;
			}
		}
		break;

	case CLEARED:
		if ((G.timer -= dt) <= 0.0f)
		{
			G.level++;
			new_level ();
		}
		break;

	case OVER:
		if ((G.timer -= dt) <= 0.0f)
		{
			new_game ();
		}
		break;
	}
	G.shake *= expf (-6.0f * dt);
}

/* ---- the HUD ------------------------------------------------------------------------ */

static GLint vp[4];			/* the screen (screen_update) */

/* the game at the top left, the perf panel (half size) at the top right */
static void hud_update (bool new_perf, const perf_t *perf)
{
	static int score = -1, lives = -1, level = -1, state = -1;
	if (!new_perf && G.score == score && G.lives == lives && G.level == level && G.state == state)
	{
		return;
	}
	score = G.score;
	lives = G.lives;
	level = G.level;
	state = G.state;

	char s[24];
	hud_begin ();
	snprintf (s, sizeof s, "SCORE %d", G.score);
	hud_rect (0, 0, 12 + 11 * HUD_CHAR_W, 40, HUD_RGBA (0, 0, 20, 140));
	hud_text (6, 4, s, HUD_RGBA (255, 230, 120, 255));
	snprintf (s, sizeof s, "LEVEL %d", G.level);
	hud_text (6, 22, s, HUD_RGBA (235, 235, 235, 255));
	for (int i = 0; i < 3; i++)
	{
		hud_rect (6 + 8 * HUD_CHAR_W + 2 + i * 14, 24, 10, 10,
			  i < G.lives ? HUD_RGBA (110, 230, 255, 255) : HUD_RGBA (60, 60, 70, 255));
	}
	hud_perf (vp[2] - hud_perf_width (0.5f) - 2, 2, 0.5f, perf);

	const char *message = NULL;
	char level_text[16];
	if (G.state == SERVE && G.left == ROWS * COLS)
	{
		snprintf (level_text, sizeof level_text, "LEVEL %d", G.level);
		message = level_text;
	}
	else if (G.state == SERVE)
	{
		message = "READY";
	}
	else if (G.state == CLEARED)
	{
		message = "CLEAR";
	}
	else if (G.state == OVER)
	{
		message = "GAME OVER";
	}
	if (message)
	{
		float w = strlen (message) * HUD_CHAR_W;
		float cx = vp[2] / 2, y = vp[3] * 0.55f;
		hud_rect (cx - w / 2 - 10, y, w + 18, 24, HUD_RGBA (0, 0, 20, 160));
		hud_text (cx - w / 2, y + 5, message, HUD_RGBA (255, 255, 255, 255));
	}
	hud_end ();
}

/* ---- main -------------------------------------------------------------------------- */

/* enable exactly these vertex arrays (-1: none) */
static void arrays (GLint a, GLint b, GLint c)
{
	for (GLint i = 0; i < 4; i++)
	{
		if (i == a || i == b || i == c)
			glEnableVertexAttribArray (i);
		else
			glDisableVertexAttribArray (i);
	}
}

int main (void)
{
	stdio_init_all ();
	pgpu_init ();
	printf ("\nbreakout: waiting for the Zero (READY)...\n");
	while (!pgpu_wait_ready (1000))
	{
	}
	pgpu_set_reply_phase (1);
	int tries = 0;
	while (!pglInit () && ++tries < 5)		/* the first reply can be missed */
	{
	}

	/* programs */
	GLuint blocks = glCreateProgram (), glow = glCreateProgram ();
	glProgramBinaryOES (blocks, PGL_PROGRAM_BINARY_PGPU, &blocks_info, sizeof blocks_info);
	glProgramBinaryOES (glow, PGL_PROGRAM_BINARY_PGPU, &glow_info, sizeof glow_info);
	GLint b_vp = glGetUniformLocation (blocks, "u_vp");
	GLint b_model = glGetUniformLocation (blocks, "u_model");
	GLint b_glow = glGetUniformLocation (blocks, "u_glow");
	GLint b_eye = glGetUniformLocation (blocks, "u_eye");
	GLint b_pos = glGetAttribLocation (blocks, "a_pos");
	GLint b_normal = glGetAttribLocation (blocks, "a_normal");
	GLint b_color = glGetAttribLocation (blocks, "a_color");
	GLint g_vp = glGetUniformLocation (glow, "u_vp");
	GLint g_size = glGetUniformLocation (glow, "u_size");
	GLint g_pos = glGetAttribLocation (glow, "a_pos");
	GLint g_color = glGetAttribLocation (glow, "a_color");
	glUseProgram (blocks);
	const float light[3] = {0.28f, -0.42f, 0.86f};	/* from the front, below: the player's side */
	glUniform3fv (glGetUniformLocation (blocks, "u_light_dir"), 1, light);

	/* meshes */
	static vertex_t v[3 * BOX_VERTICES];
	GLuint walls, paddle, ball, ball_index, stars, sparks;
	GLuint *buffers[] = {&walls, &paddle, &ball, &ball_index, &stars, &sparks, &brick_buffer};
	for (unsigned i = 0; i < sizeof buffers / sizeof buffers[0]; i++)
	{
		glGenBuffers (1, buffers[i]);
	}
	static const uint8_t wall_color[4] = {60, 70, 200, 255}, paddle_color[4] = {220, 230, 255, 255};
	static const uint8_t ball_color[4] = {255, 255, 255, 255};
	const float wall_h = (FIELD_TOP + 1.0f - BOTTOM) / 2, wall_y = (FIELD_TOP + 1.0f + BOTTOM) / 2;
	box (v, (const float[3]) {-FIELD_X - 0.4f, wall_y, 0}, (const float[3]) {0.4f, wall_h, 0.6f}, wall_color);
	box (v + BOX_VERTICES, (const float[3]) {FIELD_X + 0.4f, wall_y, 0},
	     (const float[3]) {0.4f, wall_h, 0.6f}, wall_color);
	box (v + 2 * BOX_VERTICES, (const float[3]) {0, FIELD_TOP + 0.4f, 0},
	     (const float[3]) {FIELD_X + 0.8f, 0.4f, 0.6f}, wall_color);
	glBindBuffer (GL_ARRAY_BUFFER, walls);
	glBufferData (GL_ARRAY_BUFFER, 3 * BOX_VERTICES * sizeof (vertex_t), v, GL_STATIC_DRAW);
	box (v, (const float[3]) {0, 0, 0}, (const float[3]) {PADDLE_HW, PADDLE_HH, 0.55f}, paddle_color);
	glBindBuffer (GL_ARRAY_BUFFER, paddle);
	glBufferData (GL_ARRAY_BUFFER, BOX_VERTICES * sizeof (vertex_t), v, GL_STATIC_DRAW);
	static uint8_t index[SPHERE_INDICES];
	sphere (v, index, ball_color);
	glBindBuffer (GL_ARRAY_BUFFER, ball);
	glBufferData (GL_ARRAY_BUFFER, SPHERE_VERTICES * sizeof (vertex_t), v, GL_STATIC_DRAW);
	glBindBuffer (GL_ELEMENT_ARRAY_BUFFER, ball_index);
	glBufferData (GL_ELEMENT_ARRAY_BUFFER, sizeof index, index, GL_STATIC_DRAW);
	glBindBuffer (GL_ARRAY_BUFFER, brick_buffer);
	glBufferData (GL_ARRAY_BUFFER, ROWS * COLS * BOX_VERTICES * sizeof (vertex_t), NULL, GL_DYNAMIC_DRAW);

	#define N_STARS 160
	static point_t star[N_STARS];
	for (int i = 0; i < N_STARS; i++)
	{
		uint8_t b = 90 + (uint8_t) (140 * frand ());
		star[i] = (point_t) {45.0f * srand1 (), 30.0f * srand1 () + 4.0f, -25.0f - 20.0f * frand (),
				     {(uint8_t) (b * 0.8f), (uint8_t) (b * 0.85f), b, (uint8_t) (60 + 195 * frand ())}};
	}
	glBindBuffer (GL_ARRAY_BUFFER, stars);
	glBufferData (GL_ARRAY_BUFFER, sizeof star, star, GL_STATIC_DRAW);
	glBindBuffer (GL_ARRAY_BUFFER, sparks);
	glBufferData (GL_ARRAY_BUFFER, MAX_PARTICLES * sizeof (point_t), NULL, GL_DYNAMIC_DRAW);

	float projection[16];

	glClearColor (0.01f, 0.01f, 0.05f, 1.0f);
	glDisable (GL_DITHER);
	if (!hud_init ())
	{
		printf ("breakout: the HUD program didn't link\n");
	}
	new_game ();

	printf ("breakout: running\n");
	absolute_time_t start = get_absolute_time (), last = start;
	unsigned windows = 0;
	perf_t perf;
	memset (&perf, 0, sizeof perf);
	bool new_perf = true;
	static point_t spark[MAX_PARTICLES];
	while (true)
	{
		absolute_time_t now = get_absolute_time ();
		float t = absolute_time_diff_us (start, now) / 1e6f;
		float dt = absolute_time_diff_us (last, now) / 1e6f;
		dt = dt > 1.0f / 30 ? 1.0f / 30 : dt;
		last = now;

		if (screen_update ("breakout", vp))
		{
			mat4_perspective (projection, 40.0f, (float) vp[2] / vp[3], 1.0f, 120.0f);
			new_perf = true;		/* the HUD again, for the new size */
		}
		game_update (dt, t);
		particles_update (dt);
		hud_update (new_perf, &perf);

		/* the camera: in front of the field, below it, swaying; shaken by hits */
		float eye[3] = {3.0f * sinf (t * 0.35f) + G.shake * srand1 (), -17.0f + G.shake * srand1 (), 21.0f};
		const float centre[3] = {0.0f, 0.3f, 0.0f}, up[3] = {0.0f, 1.0f, 0.0f};
		float view[16], vpm[16], model[16];
		mat4_look_at (view, eye, centre, up);
		mat4_multiply (vpm, projection, view);

		glClear (GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

		/* stars, behind everything */
		glUseProgram (glow);
		glUniformMatrix4fv (g_vp, 1, GL_FALSE, vpm);
		glDisable (GL_DEPTH_TEST);
		glEnable (GL_BLEND);
		glBlendFunc (GL_ONE, GL_ONE);
		arrays (g_pos, g_color, -1);
		glUniform1f (g_size, 70.0f);
		glBindBuffer (GL_ARRAY_BUFFER, stars);
		glVertexAttribPointer (g_pos, 3, GL_FLOAT, GL_FALSE, sizeof (point_t), (void *) 0);
		glVertexAttribPointer (g_color, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof (point_t), (void *) 12);
		glDrawArrays (GL_POINTS, 0, N_STARS);
		glDisable (GL_BLEND);

		/* the solid things */
		glUseProgram (blocks);
		glUniformMatrix4fv (b_vp, 1, GL_FALSE, vpm);
		glUniform3fv (b_eye, 1, eye);
		glEnable (GL_DEPTH_TEST);
		glEnable (GL_CULL_FACE);
		arrays (b_pos, b_normal, b_color);
		struct { GLuint buffer; GLsizei count; float glow; bool show; } solid[] =
		{
			{walls, 3 * BOX_VERTICES, 0.35f, true},
			{brick_buffer, ROWS * COLS * BOX_VERTICES, 0.08f, true},
			{paddle, BOX_VERTICES, 0.3f, true},
			{ball, SPHERE_INDICES, 1.1f, G.state == SERVE || G.state == PLAY},
		};
		for (unsigned i = 0; i < sizeof solid / sizeof solid[0]; i++)
		{
			if (!solid[i].show)
			{
				continue;
			}
			mat4_identity (model);
			if (solid[i].buffer == paddle)
			{
				mat4_translate (model, G.px, PADDLE_Y, 0.0f);
			}
			else if (solid[i].buffer == ball)
			{
				mat4_translate (model, G.bx, G.by, 0.0f);
				mat4_scale (model, BALL_R, BALL_R, BALL_R);
			}
			glUniformMatrix4fv (b_model, 1, GL_FALSE, model);
			glUniform1f (b_glow, solid[i].glow);
			glBindBuffer (GL_ARRAY_BUFFER, solid[i].buffer);
			glVertexAttribPointer (b_pos, 3, GL_FLOAT, GL_FALSE, sizeof (vertex_t), (void *) 0);
			glVertexAttribPointer (b_normal, 3, GL_FLOAT, GL_FALSE, sizeof (vertex_t), (void *) 12);
			glVertexAttribPointer (b_color, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof (vertex_t), (void *) 24);
			if (solid[i].buffer == ball)
			{
				glDrawElements (GL_TRIANGLES, SPHERE_INDICES, GL_UNSIGNED_BYTE, (void *) 0);
			}
			else
			{
				glDrawArrays (GL_TRIANGLES, 0, solid[i].count);
			}
		}

		/* sparks and the trail: glowing, not hiding each other */
		if (n_particles)
		{
			for (unsigned i = 0; i < n_particles; i++)
			{
				const particle_t *p = &particles[i];
				spark[i] = (point_t) {p->x, p->y, p->z,
						      {p->rgb[0], p->rgb[1], p->rgb[2],
						       (uint8_t) (255.0f * p->life / p->max_life)}};
			}
			glUseProgram (glow);
			arrays (g_pos, g_color, -1);
			glEnable (GL_BLEND);
			glBlendFunc (GL_ONE, GL_ONE);
			glDepthMask (GL_FALSE);
			glUniform1f (g_size, 150.0f);
			glBindBuffer (GL_ARRAY_BUFFER, sparks);
			glBufferSubData (GL_ARRAY_BUFFER, 0, n_particles * sizeof (point_t), spark);
			glVertexAttribPointer (g_pos, 3, GL_FLOAT, GL_FALSE, sizeof (point_t), (void *) 0);
			glVertexAttribPointer (g_color, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof (point_t), (void *) 12);
			glDrawArrays (GL_POINTS, 0, n_particles);
			glDepthMask (GL_TRUE);
			glDisable (GL_BLEND);
		}

		hud_draw ();
		pglSwapBuffers ();
		absolute_time_t wait_start = get_absolute_time ();
		pgpu_wait_frame (100);			/* pace on the panel */
		new_perf = perf_frame (absolute_time_diff_us (wait_start, get_absolute_time ()), &perf);

		if (new_perf && ++windows % 5 == 0)
		{
			GLenum e = glGetError ();
			printf ("breakout: %.1f fps, load GPU %.0f%% CPU-G %.0f%% CPU-H %.0f%%, render %.2f ms, "
				"level %d, score %d, lives %d, %d bricks, %u particles, GL error 0x%x\n",
				perf.fps, perf.gpu * 100, perf.cpu_g * 100, perf.cpu_h * 100, perf.render_ms,
				G.level, G.score, G.lives, G.left, n_particles, (unsigned) e);
		}
	}
}
