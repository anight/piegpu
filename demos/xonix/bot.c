/*
 * bot.c - Xonix played by itself (game.h): from the land it goes out into
 * the sea in a U, out, along and back, closing off the sea inside it; the
 * one it takes is the largest whose trail no sea ball can reach before it's
 * back. The balls go where the field sends them (the trail doesn't turn
 * them), so where each will be is worked out step by step, with the times
 * the player reaches each cell of the U. Without a way out that's safe it
 * waits, walks along the shore after a while, and keeps away from the land
 * balls. At sea, if the way back becomes unsafe, it takes the shortest way
 * to land.
 */
#include <string.h>
#include "game.h"

#define MAX_OUT		14		/* the U: how far out */
#define MAX_ALONG	30		/* ... and along */
#define BACK_EXTRA	4		/* the way back may be this much longer than out */
#define TRIED		40		/* the largest ones tested */
#define SLACK		1.5f		/* player steps: the times are this much earlier, to be safe */
#define LOOK_S		0.15f		/* without a way out: looked for again after this */
#define WALK_S		1.5f		/* ... after this long, along the shore */
#define LAND_NEAR	4		/* a land ball this near (cells): away from it */

typedef int8_t cell_t[2];

static int16_t stamp[XN_ROWS][XN_COLS];	/* a path's cells: their step, from 1 (with the generation) */
static uint16_t stamp_gen[XN_ROWS][XN_COLS], gen;

static int absi (int a)			{ return a < 0 ? -a : a; }
static int maxi (int a, int b)		{ return a > b ? a : b; }
static int cheb (int x0, int y0, int x1, int y1)	{ return maxi (absi (x0 - x1), absi (y0 - y1)); }

static uint32_t next (xn_bot_t *b)
{
	b->rng ^= b->rng << 13;
	b->rng ^= b->rng >> 17;
	b->rng ^= b->rng << 5;
	return b->rng;
}

void xn_bot_init (xn_bot_t *b, uint32_t seed)
{
	memset (b, 0, sizeof *b);
	b->rng = seed ? seed : 0xB07B07u;
	b->walk = XN_RIGHT;
}

static int dir_to (int x0, int y0, int x1, int y1)
{
	return x1 > x0 ? XN_RIGHT : x1 < x0 ? XN_LEFT : y1 > y0 ? XN_DOWN : y1 < y0 ? XN_UP : XN_NONE;
}

/* the player from where it is through path[0 .. n-1] (the last a land cell), a cell a step:
   can a sea ball meet its trail, or it, first? And is a land ball at the last cell when it gets there? */
static bool path_safe (const xn_t *g, const cell_t *path, int n)
{
	if (++gen == 0)
	{
		memset (stamp_gen, 0, sizeof stamp_gen);
		gen = 1;
	}
	for (int k = 0; k < n - 1; k++)
	{
		stamp[path[k][1]][path[k][0]] = (int16_t) (k + 1);
		stamp_gen[path[k][1]][path[k][0]] = gen;
	}
	const float tp = 1.0f / xn_player_speed (g), ts = 1.0f / xn_sea_speed (g), tl = 1.0f / xn_land_speed (g);
	const float arrive = g->t_player + (n - 1) * tp;
	for (int i = 0; i < g->n_sea; i++)
	{
		xn_ball_t b = g->sea[i];
		float from = 0, to = g->t_sea;		/* it's in its cell from .. to */
		while (from <= arrive)
		{
			if (g->cell[b.y][b.x] == XN_TRAIL)
			{
				return false;		/* (the trail there already) */
			}
			if (stamp_gen[b.y][b.x] == gen)
			{
				float enters = g->t_player + (stamp[b.y][b.x] - 1) * tp - SLACK * tp;
				if (enters < to + SLACK * tp)
				{
					return false;	/* the player there, or its trail, before the ball's gone */
				}
			}
			bool bounced;
			xn_ball_next (g, &b, false, &bounced);
			from = to;
			to += ts;
		}
	}
	const int lx = path[n - 1][0], ly = path[n - 1][1];
	for (int i = 0; i < g->n_land; i++)
	{
		xn_ball_t b = g->land[i];
		float from = 0, to = g->t_land;
		while (from <= arrive + 3 * tl)
		{
			if (to >= arrive - tl && cheb (b.x, b.y, lx, ly) <= 1)
			{
				return false;
			}
			bool bounced;
			xn_ball_next (g, &b, true, &bounced);
			from = to;
			to += tl;
		}
	}
	return true;
}

typedef struct { int8_t d, turn, out, along; int16_t area; } plan_t;

/* the U out from (x, y) that way: false if it isn't all sea */
static int build_u (const xn_t *g, const plan_t *p, cell_t *path)
{
	int dx = xn_dx (p->d), dy = xn_dy (p->d), tx = xn_dx (p->turn), ty = xn_dy (p->turn);
	int x = g->x, y = g->y, n = 0;
	for (int k = 0; k < p->out; k++)
	{
		x += dx, y += dy;
		path[n][0] = (int8_t) x, path[n++][1] = (int8_t) y;
	}
	for (int k = 0; k < p->along; k++)
	{
		x += tx, y += ty;
		path[n][0] = (int8_t) x, path[n++][1] = (int8_t) y;
	}
	for (int k = 0; k < p->out + BACK_EXTRA && n < XN_BOT_PATH; k++)
	{
		x -= dx, y -= dy;
		if (!xn_inside (x, y) || g->cell[y][x] == XN_TRAIL)
		{
			return 0;
		}
		path[n][0] = (int8_t) x, path[n++][1] = (int8_t) y;
		if (XN_IS_LAND (g->cell[y][x]))
		{
			return n;		/* (back on land) */
		}
	}
	return 0;
}

/* the largest safe U from where the player stands */
static bool plan_u (xn_bot_t *b, const xn_t *g)
{
	plan_t best[TRIED];
	int n_best = 0;
	for (int d = 0; d < 4; d++)
	{
		int dx = xn_dx (d), dy = xn_dy (d);
		for (int out = 1; out <= MAX_OUT; out++)
		{
			int ox = g->x + dx * out, oy = g->y + dy * out;
			if (!xn_inside (ox, oy) || g->cell[oy][ox] != XN_SEA)
			{
				break;
			}
			for (int side = 0; side < 2; side++)
			{
				int turn = (d + (side ? 1 : 3)) % 4, tx = xn_dx (turn), ty = xn_dy (turn);
				for (int along = 1; along <= MAX_ALONG; along++)
				{
					int ax = ox + tx * along, ay = oy + ty * along;
					if (!xn_inside (ax, ay) || g->cell[ay][ax] != XN_SEA)
					{
						break;
					}
					plan_t p = {(int8_t) d, (int8_t) turn, (int8_t) out, (int8_t) along,
						    (int16_t) (out * along + (int) (next (b) % 4))};
					if (n_best == TRIED && p.area <= best[n_best - 1].area)
					{
						continue;
					}
					int i = n_best < TRIED ? n_best++ : TRIED - 1;	/* (in, largest first) */
					while (i > 0 && best[i - 1].area < p.area)
					{
						best[i] = best[i - 1];
						i--;
					}
					best[i] = p;
				}
			}
		}
	}
	for (int i = 0; i < n_best; i++)
	{
		int n = build_u (g, &best[i], b->path);
		if (n && path_safe (g, b->path, n))
		{
			b->n = n;
			b->at = 0;
			return true;
		}
	}
	return false;
}

/* at sea: the shortest way to land (never straight back) */
static bool plan_back (xn_bot_t *b, const xn_t *g)
{
	static int16_t from[XN_ROWS][XN_COLS];
	static int16_t queue[XN_ROWS * XN_COLS][2];
	for (int y = 0; y < XN_ROWS; y++)
	{
		for (int x = 0; x < XN_COLS; x++)
		{
			from[y][x] = -1;
		}
	}
	int head = 0, tail = 0;
	from[g->y][g->x] = (int16_t) (g->y * XN_COLS + g->x);
	queue[tail][0] = (int16_t) g->x, queue[tail++][1] = (int16_t) g->y;
	while (head < tail)
	{
		int x = queue[head][0], y = queue[head++][1];
		for (int d = 0; d < 4; d++)
		{
			if (x == g->x && y == g->y && g->dir != XN_NONE && d == (g->dir + 2) % 4)
			{
				continue;		/* (it can't turn back) */
			}
			int nx = x + xn_dx (d), ny = y + xn_dy (d);
			if (!xn_inside (nx, ny) || from[ny][nx] >= 0 || g->cell[ny][nx] == XN_TRAIL)
			{
				continue;
			}
			from[ny][nx] = (int16_t) (y * XN_COLS + x);
			if (XN_IS_LAND (g->cell[ny][nx]))
			{
				int n = 0, cx = nx, cy = ny;	/* (backwards from there, then turned round) */
				while (!(cx == g->x && cy == g->y) && n < XN_BOT_PATH)
				{
					b->path[n][0] = (int8_t) cx, b->path[n++][1] = (int8_t) cy;
					int f = from[cy][cx];
					cx = f % XN_COLS, cy = f / XN_COLS;
				}
				for (int i = 0; i < n / 2; i++)
				{
					cell_t t = {b->path[i][0], b->path[i][1]};
					b->path[i][0] = b->path[n - 1 - i][0], b->path[i][1] = b->path[n - 1 - i][1];
					b->path[n - 1 - i][0] = t[0], b->path[n - 1 - i][1] = t[1];
				}
				b->n = n;
				b->at = 0;
				return n > 0;
			}
			queue[tail][0] = (int16_t) nx, queue[tail++][1] = (int16_t) ny;
		}
	}
	return false;
}

static bool land_at (const xn_t *g, int x, int y)
{
	return xn_inside (x, y) && XN_IS_LAND (g->cell[y][x]);
}

static int nearest_land_ball (const xn_t *g, int x, int y)
{
	int best = 999;
	for (int i = 0; i < g->n_land; i++)
	{
		int d = cheb (g->land[i].x, g->land[i].y, x, y);
		best = d < best ? d : best;
	}
	return best;
}

/* land with sea beside it: a U can start there */
static bool shore (const xn_t *g, int x, int y)
{
	if (!land_at (g, x, y))
	{
		return false;
	}
	for (int d = 0; d < 4; d++)
	{
		int nx = x + xn_dx (d), ny = y + xn_dy (d);
		if (xn_inside (nx, ny) && g->cell[ny][nx] == XN_SEA)
		{
			return true;
		}
	}
	return false;
}

/* over the land to the nearest shore but where it is: the way to go first */
static int to_shore (const xn_t *g)
{
	static int8_t first[XN_ROWS][XN_COLS];		/* the first step there (-1: not reached) */
	static int16_t queue[XN_ROWS * XN_COLS][2];
	memset (first, -1, sizeof first);
	int head = 0, tail = 0;
	first[g->y][g->x] = 4;
	queue[tail][0] = (int16_t) g->x, queue[tail++][1] = (int16_t) g->y;
	while (head < tail)
	{
		int x = queue[head][0], y = queue[head++][1];
		for (int d = 0; d < 4; d++)
		{
			int nx = x + xn_dx (d), ny = y + xn_dy (d);
			if (!land_at (g, nx, ny) || first[ny][nx] >= 0 || nearest_land_ball (g, nx, ny) <= 1)
			{
				continue;
			}
			first[ny][nx] = (int8_t) (first[y][x] == 4 ? d : first[y][x]);
			if (shore (g, nx, ny))
			{
				return first[ny][nx];
			}
			queue[tail][0] = (int16_t) nx, queue[tail++][1] = (int16_t) ny;
		}
	}
	return XN_NONE;
}

/* on land: away from a land ball that's near; to the shore; along it after a while */
static int on_land (xn_bot_t *b, const xn_t *g, float dt)
{
	int near = nearest_land_ball (g, g->x, g->y);
	if (near <= LAND_NEAR && g->safe <= 0)
	{
		int best = XN_NONE, far = near;
		for (int d = 0; d < 4; d++)
		{
			int nx = g->x + xn_dx (d), ny = g->y + xn_dy (d);
			if (land_at (g, nx, ny) && nearest_land_ball (g, nx, ny) > far)
			{
				far = nearest_land_ball (g, nx, ny);
				best = d;
			}
		}
		if (best != XN_NONE)
		{
			return best;
		}
	}
	if (!shore (g, g->x, g->y))
	{
		return to_shore (g);
	}
	b->wait += dt;
	if (b->wait < WALK_S)
	{
		return XN_NONE;
	}
	static const int order[4] = {0, 1, 3, 2};	/* on, turned one way, the other, back */
	for (int i = 0; i < 4; i++)
	{
		int d = (b->walk + order[i]) % 4, nx = g->x + xn_dx (d), ny = g->y + xn_dy (d);
		if (shore (g, nx, ny) && nearest_land_ball (g, nx, ny) > 1)
		{
			b->walk = d;
			return d;
		}
	}
	return to_shore (g);
}

int xn_bot (xn_bot_t *b, const xn_t *g, float dt)
{
	if (g->state != XN_PLAYING)
	{
		b->n = 0;
		return XN_NONE;
	}
	/* the plan: still where it goes? */
	if (b->n)
	{
		if (b->at < b->n && g->x == b->path[b->at][0] && g->y == b->path[b->at][1])
		{
			b->at++;
		}
		if (b->at >= b->n || cheb (g->x, g->y, b->path[b->at][0], b->path[b->at][1]) != 1)
		{
			b->n = 0;		/* (done, or off it) */
		}
	}
	if (g->at_sea)
	{
		if (!b->n || !path_safe (g, b->path + b->at, b->n - b->at))
		{
			plan_back (b, g);
		}
	}
	else if (!b->n)
	{
		b->cool -= dt;
		if (b->cool <= 0)
		{
			b->cool = LOOK_S;
			if (plan_u (b, g))
			{
				b->wait = 0;
			}
		}
		if (!b->n)
		{
			return on_land (b, g, dt);
		}
	}
	if (!b->n)
	{
		return g->dir;
	}
	return dir_to (g->x, g->y, b->path[b->at][0], b->path[b->at][1]);
}
