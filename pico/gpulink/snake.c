/*
 * snake.c - the Pico's side of toy-snake (toy_game.h): the computer plays
 * Snake on a 32 x 24 grid. Each step it takes the shortest way to the food
 * (breadth-first search) when, after that move, the snake could still reach
 * as many free cells as it is long; otherwise the move that leaves the most
 * room (flood fill). When it dies the board flashes and it starts again.
 * The grid goes to the shader as a 32 x 24 RGBA texture (R body shade, G
 * food, B head, A segment number; shaders/toy_snake.frag), sent again after
 * each step.
 */
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include "toy_game.h"

#define GW		32
#define GH		24
#define CELLS		(GW * GH)
#define STEP		0.075f		/* seconds per move */
#define GRID_UNIT	2		/* texture unit (the HUD uses 0) */

static int16_t body[CELLS];		/* cell -> segment number + 1 (0: free) */
static int16_t snake[CELLS];		/* cells, head first */
static int length, food;
static float clock, dead = 99.0f;
static uint8_t texels[CELLS * 4];
static GLuint texture;
static GLint u_dead, u_score;

static const int dx[4] = {1, -1, 0, 0}, dy[4] = {0, 0, 1, -1};

static int neighbour (int c, int d)
{
	int x = c % GW + dx[d], y = c / GW + dy[d];
	return x < 0 || y < 0 || x >= GW || y >= GH ? -1 : y * GW + x;
}

static void place_food (void)
{
	do
	{
		food = rand () % CELLS;
	}
	while (body[food]);
}

static void reset (void)
{
	memset (body, 0, sizeof body);
	length = 4;
	for (int i = 0; i < length; i++)
	{
		snake[i] = (GH / 2) * GW + GW / 2 - i;
		body[snake[i]] = 1;
	}
	place_food ();
}

/* free cells reachable from c (the tail's cell counts as free: it moves on) */
static int room (int c, const int16_t *occupied)
{
	static int16_t queue[CELLS];
	static uint8_t seen[CELLS];
	memset (seen, 0, sizeof seen);
	int head = 0, tail = 0, n = 0;
	queue[tail++] = c;
	seen[c] = 1;
	while (head < tail)
	{
		int u = queue[head++];
		n++;
		for (int d = 0; d < 4; d++)
		{
			int v = neighbour (u, d);
			if (v >= 0 && !seen[v] && !occupied[v])
			{
				seen[v] = 1;
				queue[tail++] = v;
			}
		}
	}
	return n;
}

/* the first move of a shortest way from the head to the food, or -1 */
static int towards_food (void)
{
	static int16_t from[CELLS], queue[CELLS];
	memset (from, -1, sizeof from);
	int head = 0, tail = 0, start = snake[0];
	queue[tail++] = start;
	from[start] = start;
	while (head < tail)
	{
		int u = queue[head++];
		if (u == food)
		{
			while (from[u] != start)
			{
				u = from[u];
			}
			for (int d = 0; d < 4; d++)
			{
				if (neighbour (start, d) == u)
				{
					return d;
				}
			}
		}
		for (int d = 0; d < 4; d++)
		{
			int v = neighbour (u, d);
			if (v >= 0 && from[v] < 0 && (!body[v] || v == snake[length - 1]))
			{
				from[v] = u;
				queue[tail++] = v;
			}
		}
	}
	return -1;
}

/* the room left after moving the head to c */
static int room_after (int c)
{
	static int16_t occupied[CELLS];
	memcpy (occupied, body, sizeof occupied);
	if (c != food)
	{
		occupied[snake[length - 1]] = 0;
	}
	occupied[c] = 1;
	int best = 0;
	for (int d = 0; d < 4; d++)
	{
		int v = neighbour (c, d);
		if (v >= 0 && !occupied[v])
		{
			int r = room (v, occupied);
			best = r > best ? r : best;
		}
	}
	return best;
}

static void step (void)
{
	int d = towards_food ();
	if (d >= 0 && room_after (neighbour (snake[0], d)) < length + 1)
	{
		d = -1;					/* a trap: play for room instead */
	}
	if (d < 0)
	{
		int best = -1;
		for (int k = 0; k < 4; k++)
		{
			int v = neighbour (snake[0], k);
			if (v >= 0 && (!body[v] || v == snake[length - 1]))
			{
				int r = room_after (v);
				if (r > best)
				{
					best = r;
					d = k;
				}
			}
		}
	}
	int next = d >= 0 ? neighbour (snake[0], d) : -1;
	if (next < 0 || (body[next] && next != snake[length - 1]))
	{
		dead = 0;				/* no way on */
		return;
	}
	bool eat = next == food;
	if (!eat)
	{
		body[snake[length - 1]] = 0;
	}
	else if (length < CELLS)
	{
		length++;
	}
	memmove (snake + 1, snake, (length - 1) * sizeof snake[0]);
	snake[0] = next;
	body[next] = 1;
	if (eat)
	{
		if (length == CELLS)
		{
			dead = 0;			/* the board is full: a win, start again */
			return;
		}
		place_food ();
	}
}

static void upload (void)
{
	memset (texels, 0, sizeof texels);
	for (int i = 0; i < length; i++)
	{
		texels[snake[i] * 4] = (uint8_t) (255 - i * 175 / (length > 1 ? length - 1 : 1));
		texels[snake[i] * 4 + 3] = (uint8_t) i;
	}
	texels[snake[0] * 4 + 2] = 255;
	texels[food * 4 + 1] = 255;
	glActiveTexture (GL_TEXTURE0 + GRID_UNIT);
	glBindTexture (GL_TEXTURE_2D, texture);
	glTexSubImage2D (GL_TEXTURE_2D, 0, 0, 0, GW, GH, GL_RGBA, GL_UNSIGNED_BYTE, texels);
	glActiveTexture (GL_TEXTURE0);
}

void game_init (GLuint prog, float width, float height)
{
	(void) width;
	(void) height;
	u_dead = glGetUniformLocation (prog, "uDead");
	u_score = glGetUniformLocation (prog, "uScore");
	glGenTextures (1, &texture);
	glActiveTexture (GL_TEXTURE0 + GRID_UNIT);
	glBindTexture (GL_TEXTURE_2D, texture);
	glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA, GW, GH, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glActiveTexture (GL_TEXTURE0);
	glUniform1i (glGetUniformLocation (prog, "uGrid"), GRID_UNIT);
	srand (4321);
	reset ();
	upload ();
}

void game_frame (float t, float dt)
{
	(void) t;
	if (dt > 0.05f)
	{
		dt = 0.05f;
	}
	if (dead < 1.5f)
	{
		dead += dt;
		if (dead >= 1.5f)
		{
			reset ();
			dead = 99.0f;
			upload ();
		}
	}
	else
	{
		clock += dt;
		bool moved = false;
		while (clock >= STEP && dead > 1.5f)
		{
			clock -= STEP;
			step ();
			moved = true;
		}
		if (moved)
		{
			upload ();
		}
	}
	glUniform1f (u_dead, dead);
	glUniform2f (u_score, (float) (length / 10 % 10), (float) (length % 10));
}
