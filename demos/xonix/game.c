/*
 * game.c - Xonix's rules (game.h). Everything moves a cell at a time, each
 * at its own speed: the steps due are taken in the order they fall due, so
 * that what meets what doesn't depend on the frame rate.
 */
#include <string.h>
#include "game.h"

#define DYING_S		1.2f		/* the life lost shown */
#define CLEARED_S	2.6f		/* the level's end shown */
#define SAFE_S		2.5f		/* back after a life lost: the land balls can't catch it (from the loss) */
#define START_S		1.0f		/* the balls wait at a level's start */

static uint32_t next (xn_t *g)
{
	g->rng ^= g->rng << 13;
	g->rng ^= g->rng >> 17;
	g->rng ^= g->rng << 5;
	return g->rng;
}

static int below (xn_t *g, int n)	{ return (int) (next (g) % (uint32_t) n); }
static int either (xn_t *g)		{ return next (g) & 1 ? 1 : -1; }
static int mini (int a, int b)		{ return a < b ? a : b; }
static float minf (float a, float b)	{ return a < b ? a : b; }

float xn_player_speed (const xn_t *g)	{ (void) g; return 18.0f; }
float xn_sea_speed (const xn_t *g)	{ return minf (11.0f + 0.6f * (g->level - 1), 17.0f); }
float xn_land_speed (const xn_t *g)	{ return minf (8.0f + 0.3f * (g->level - 1), 11.0f); }

static void set (xn_t *g, int x, int y, uint8_t v)
{
	g->cell[y][x] = v;
	g->dirty0 = mini (g->dirty0, y);
	g->dirty1 = y > g->dirty1 ? y : g->dirty1;
}

static void all_dirty (xn_t *g)		{ g->dirty0 = 0; g->dirty1 = XN_ROWS - 1; }

int xn_filled (const xn_t *g)
{
	return 100 * (g->inside - g->sea_left) / g->inside;
}

static void setup_level (xn_t *g)
{
	for (int y = 0; y < XN_ROWS; y++)
	{
		for (int x = 0; x < XN_COLS; x++)
		{
			bool rim = x < XN_RIM || y < XN_RIM || x >= XN_COLS - XN_RIM || y >= XN_ROWS - XN_RIM;
			g->cell[y][x] = rim ? XN_LAND : XN_SEA;
		}
	}
	all_dirty (g);
	g->inside = g->sea_left = (XN_COLS - 2 * XN_RIM) * (XN_ROWS - 2 * XN_RIM);
	g->x = g->px = XN_COLS / 2;
	g->y = g->py = 0;
	g->dir = XN_NONE;
	g->at_sea = false;
	g->trail = 0;
	g->safe = 0;
	g->fresh = XN_LAND;
	g->fresh_t = 10.0f;
	g->captured = 0;

	g->n_sea = mini (g->level + 1, XN_MAX_SEA);
	for (int i = 0; i < g->n_sea; i++)
	{
		xn_ball_t *b = &g->sea[i];
		b->x = b->px = XN_RIM + 2 + below (g, XN_COLS - 2 * XN_RIM - 4);
		b->y = b->py = 12 + below (g, XN_ROWS - XN_RIM - 14);	/* (not under the player's start) */
		b->dx = either (g);
		b->dy = either (g);
	}
	g->n_land = mini ((g->level + 1) / 2, XN_MAX_LAND);
	for (int i = 0; i < g->n_land; i++)
	{
		xn_ball_t *b = &g->land[i];
		b->x = b->px = 4 + below (g, XN_COLS - 8);
		b->y = b->py = XN_ROWS - 1 - below (g, XN_RIM);	/* on the bottom's rim */
		b->dx = either (g);
		b->dy = either (g);
	}
	g->t_player = 0;
	g->t_sea = g->t_land = START_S;
	g->state = XN_PLAYING;
	g->state_t = 0;
	g->events |= XN_EV_START;
}

void xn_new_game (xn_t *g, uint32_t seed)
{
	memset (g, 0, sizeof *g);
	g->rng = seed ? seed : 0x5EA5EA5u;
	g->level = 1;
	g->lives = XN_LIVES;
	setup_level (g);
}

/* ---- losing a life, closing off the sea ------------------------------------------------ */

static void die (xn_t *g)
{
	g->events |= XN_EV_DIE;
	g->died_x = g->x;
	g->died_y = g->y;
	g->lives--;
	if (g->trail)
	{
		for (int y = 0; y < XN_ROWS; y++)
		{
			for (int x = 0; x < XN_COLS; x++)
			{
				if (g->cell[y][x] == XN_TRAIL)
				{
					set (g, x, y, XN_SEA);
				}
			}
		}
	}
	g->trail = 0;
	if (g->at_sea)
	{
		g->x = g->out_x;
		g->y = g->out_y;
	}
	g->px = g->x;
	g->py = g->y;
	g->at_sea = false;
	g->dir = XN_NONE;
	g->safe = SAFE_S;
	g->state = XN_DYING;
	g->state_t = 0;
}

static uint8_t reached[XN_ROWS][XN_COLS];
static int16_t stack[XN_ROWS * XN_COLS][2];

/* the sea the balls can't reach becomes land, with the trail */
static void capture (xn_t *g)
{
	g->fresh = g->fresh >= 255 ? XN_LAND + 1 : g->fresh + 1;
	int n = 0;
	memset (reached, 0, sizeof reached);
	int top = 0;
	for (int i = 0; i < g->n_sea; i++)
	{
		const xn_ball_t *b = &g->sea[i];
		if (g->cell[b->y][b->x] == XN_SEA && !reached[b->y][b->x])
		{
			reached[b->y][b->x] = 1;
			stack[top][0] = (int16_t) b->x;
			stack[top++][1] = (int16_t) b->y;
		}
	}
	while (top)
	{
		top--;
		int x = stack[top][0], y = stack[top][1];
		static const int8_t around[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};	/* (the balls bounce off corners: */
		for (int k = 0; k < 4; k++)						/* the sea is joined by its sides) */
		{
			int nx = x + around[k][0], ny = y + around[k][1];
			if (xn_inside (nx, ny) && !reached[ny][nx] && g->cell[ny][nx] == XN_SEA)
			{
				reached[ny][nx] = 1;
				stack[top][0] = (int16_t) nx;
				stack[top++][1] = (int16_t) ny;
			}
		}
	}
	for (int y = XN_RIM; y < XN_ROWS - XN_RIM; y++)
	{
		for (int x = XN_RIM; x < XN_COLS - XN_RIM; x++)
		{
			uint8_t c = g->cell[y][x];
			if (c == XN_TRAIL || (c == XN_SEA && !reached[y][x]))
			{
				set (g, x, y, g->fresh);
				n++;
			}
		}
	}
	g->trail = 0;
	g->sea_left -= n;
	g->captured = n;
	g->fresh_t = 0;
	g->score += n * g->level;
	g->events |= XN_EV_CAPTURE;
	int filled = xn_filled (g);
	if (filled >= XN_GOAL)
	{
		g->score += 1000 * g->level + 100 * g->level * (filled - XN_GOAL);
		g->state = XN_CLEARED;
		g->state_t = 0;
		g->events |= XN_EV_CLEARED;
	}
}

/* ---- the steps ------------------------------------------------------------------------- */

static bool sea_ball_at (const xn_t *g, int x, int y)
{
	for (int i = 0; i < g->n_sea; i++)
	{
		if (g->sea[i].x == x && g->sea[i].y == y)
		{
			return true;
		}
	}
	return false;
}

static bool land_ball_at (const xn_t *g, int x, int y)
{
	for (int i = 0; i < g->n_land; i++)
	{
		if (g->land[i].x == x && g->land[i].y == y)
		{
			return true;
		}
	}
	return false;
}

static void player_step (xn_t *g, int want)
{
	g->px = g->x;
	g->py = g->y;
	int dir = g->dir;
	if (!g->at_sea)
	{
		dir = want;				/* on land: while it's held */
	}
	else if (want != XN_NONE && want != (dir + 2) % 4)
	{
		dir = want;				/* at sea: on, the way it was going, or turned (not back) */
	}
	g->dir = dir;
	if (dir == XN_NONE)
	{
		return;
	}
	int nx = g->x + xn_dx (dir), ny = g->y + xn_dy (dir);
	if (!xn_inside (nx, ny))
	{
		return;
	}
	uint8_t c = g->cell[ny][nx];
	if (c == XN_TRAIL)
	{
		die (g);				/* into its own trail */
		return;
	}
	g->x = nx;
	g->y = ny;
	if (c == XN_SEA)
	{
		if (!g->at_sea)
		{
			g->at_sea = true;
			g->out_x = g->px;
			g->out_y = g->py;
			g->events |= XN_EV_OUT;
		}
		set (g, nx, ny, XN_TRAIL);
		g->trail++;
		g->events |= XN_EV_STEP;
		if (sea_ball_at (g, nx, ny))
		{
			die (g);
		}
		return;
	}
	if (g->at_sea)
	{
		g->at_sea = false;
		g->dir = XN_NONE;
		capture (g);
	}
	if (g->state == XN_PLAYING && g->safe <= 0 && land_ball_at (g, nx, ny))
	{
		die (g);
	}
}

bool xn_ball_next (const xn_t *g, xn_ball_t *b, bool land, bool *bounced)
{
	b->px = b->x;
	b->py = b->y;
#define BLOCKED(x, y)	(!xn_inside (x, y) || (land ? !XN_IS_LAND (g->cell[y][x]) : XN_IS_LAND (g->cell[y][x])))
	int dx = b->dx, dy = b->dy;
	bool hx = BLOCKED (b->x + dx, b->y), hy = BLOCKED (b->x, b->y + dy);
	if (hx)
	{
		dx = -dx;
	}
	if (hy)
	{
		dy = -dy;
	}
	if (!hx && !hy && BLOCKED (b->x + dx, b->y + dy))
	{
		dx = -dx;
		dy = -dy;
		hx = true;
	}
	b->dx = dx;
	b->dy = dy;
	*bounced = hx || hy;
	if (BLOCKED (b->x + dx, b->y + dy))
	{
		return false;
	}
#undef BLOCKED
	b->x += dx;
	b->y += dy;
	return true;
}

/* a ball a step on; true if it moved */
static bool ball_step (xn_t *g, xn_ball_t *b, bool land)
{
	bool bounced;
	bool moved = xn_ball_next (g, b, land, &bounced);
	if (bounced && !land)
	{
		g->events |= XN_EV_BOUNCE;
		g->bounce_x = b->x;
	}
	return moved;
}

static void sea_step (xn_t *g)
{
	for (int i = 0; i < g->n_sea && g->state == XN_PLAYING; i++)
	{
		xn_ball_t *b = &g->sea[i];
		if (ball_step (g, b, false) && g->cell[b->y][b->x] == XN_TRAIL)
		{
			die (g);				/* the trail met (or the player on it) */
		}
	}
}

static void land_step (xn_t *g)
{
	for (int i = 0; i < g->n_land && g->state == XN_PLAYING; i++)
	{
		xn_ball_t *b = &g->land[i];
		if (ball_step (g, b, true) && b->x == g->x && b->y == g->y && !g->at_sea && g->safe <= 0)
		{
			die (g);
		}
	}
}

void xn_update (xn_t *g, int want, float dt)
{
	g->events = 0;
	g->state_t += dt;
	g->fresh_t += dt;
	g->safe -= dt;
	switch (g->state)
	{
	case XN_DYING:
		if (g->state_t >= DYING_S)
		{
			if (g->lives <= 0)
			{
				g->state = XN_OVER;
				g->state_t = 0;
				g->events |= XN_EV_OVER;
			}
			else
			{
				g->state = XN_PLAYING;
				g->state_t = 0;
				g->t_player = 0;
			}
		}
		return;
	case XN_CLEARED:
		if (g->state_t >= CLEARED_S)
		{
			g->level++;
			g->lives = mini (g->lives + 1, 9);
			setup_level (g);
			g->events |= XN_EV_LEVEL;
		}
		return;
	case XN_OVER:
		return;
	}

	const float tp = 1.0f / xn_player_speed (g), ts = 1.0f / xn_sea_speed (g), tl = 1.0f / xn_land_speed (g);
	g->t_player -= dt;
	g->t_sea -= dt;
	g->t_land -= dt;
	if (g->t_player < -tp && !g->at_sea && want == XN_NONE)
	{
		g->t_player = 0;			/* (standing: a step as soon as one is asked) */
	}
	while (g->state == XN_PLAYING)
	{
		float t = minf (g->t_player, minf (g->t_sea, g->t_land));
		if (t > 0)
		{
			break;
		}
		if (t == g->t_player)
		{
			player_step (g, want);
			g->t_player += tp;
		}
		else if (t == g->t_sea)
		{
			sea_step (g);
			g->t_sea += ts;
		}
		else
		{
			land_step (g);
			g->t_land += tl;
		}
	}
}
