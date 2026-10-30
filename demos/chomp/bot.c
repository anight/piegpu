/*
 * bot.c - the attract mode's player (game.h): the shortest way, over the
 * maze's tiles, to the nearest dot, never through a tile near a ghost that
 * hunts; after a frightened ghost near enough while there's time; with no
 * safe way, away from the nearest ghost.
 */
#include <stdlib.h>
#include <string.h>
#include "game.h"

#define DANGER		3		/* tiles: around a hunting ghost, and ahead of it */
#define CHASE_TILES	10		/* a frightened ghost this near (the way there): after it */

static int tile_of (int p)		{ return p >= 0 ? p / 8 : -((7 - p) / 8); }

int ch_bot (const ch_t *g)
{
	static uint8_t danger[CH_ROWS][CH_COLS];
	static int16_t dist[CH_ROWS][CH_COLS];
	static int8_t first[CH_ROWS][CH_COLS];
	static int16_t queue[CH_ROWS * CH_COLS][2];
	int px = tile_of (g->pac.x), py = tile_of (g->pac.y);
	if (px < 0 || px >= CH_COLS)
	{
		return g->pac.dir;			/* (in the tunnel: on) */
	}
	memset (danger, 0, sizeof danger);
	int nearest = 1000, away = CH_NONE;
	for (int i = 0; i < 4; i++)
	{
		const ch_actor_t *gh = &g->ghost[i];
		if (gh->state != G_ACTIVE || gh->fright)
		{
			continue;
		}
		int gx = tile_of (gh->x), gy = tile_of (gh->y);
		for (int y = gy - DANGER; y <= gy + DANGER; y++)
		{
			for (int x = gx - DANGER; x <= gx + DANGER; x++)
			{
				if (x >= 0 && y >= 0 && x < CH_COLS && y < CH_ROWS && abs (x - gx) + abs (y - gy) <= DANGER)
				{
					danger[y][x] = 1;
				}
			}
		}
		for (int k = 1; k <= DANGER + 1; k++)	/* (where it's going) */
		{
			int x = gx + ch_dx (gh->dir) * k, y = gy + ch_dy (gh->dir) * k;
			if (x >= 0 && y >= 0 && x < CH_COLS && y < CH_ROWS)
			{
				danger[y][x] = 1;
			}
		}
		int d = abs (gx - px) + abs (gy - py);
		if (d < nearest)
		{
			nearest = d;
			away = abs (gx - px) > abs (gy - py) ? (gx > px ? CH_LEFT : CH_RIGHT) : (gy > py ? CH_UP : CH_DOWN);
		}
	}
	danger[py][px] = 0;

	/* the shortest ways from here, through safe tiles */
	for (int y = 0; y < CH_ROWS; y++)
	{
		for (int x = 0; x < CH_COLS; x++)
		{
			dist[y][x] = -1;
		}
	}
	int head = 0, tail = 0;
	dist[py][px] = 0;
	first[py][px] = CH_NONE;
	queue[tail][0] = (int16_t) px, queue[tail++][1] = (int16_t) py;
	int best_dot = -1, best_ghost = -1;
	while (head < tail)
	{
		int x = queue[head][0], y = queue[head++][1];
		uint8_t t = g->tile[y][x];
		if (best_dot < 0 && (t == T_DOT || t == T_ENERGIZER) && dist[y][x] > 0)
		{
			best_dot = y * CH_COLS + x;
		}
		for (int i = 0; i < 4 && best_ghost < 0 && g->fright_t > 90 && dist[y][x] <= CHASE_TILES; i++)
		{
			const ch_actor_t *gh = &g->ghost[i];
			if (gh->state == G_ACTIVE && gh->fright && tile_of (gh->x) == x && tile_of (gh->y) == y && dist[y][x] > 0)
			{
				best_ghost = y * CH_COLS + x;
			}
		}
		for (int d = 0; d < 4; d++)
		{
			int nx = x + ch_dx (d), ny = y + ch_dy (d);
			if (nx < 0 || nx >= CH_COLS)
			{
				continue;			/* (the tunnel: not worth working out) */
			}
			if (!ch_open (g, nx, ny, false) || dist[ny][nx] >= 0 || danger[ny][nx])
			{
				continue;
			}
			dist[ny][nx] = (int16_t) (dist[y][x] + 1);
			first[ny][nx] = (int8_t) (dist[y][x] == 0 ? d : first[y][x]);
			queue[tail][0] = (int16_t) nx, queue[tail++][1] = (int16_t) ny;
		}
	}
	int goal = best_ghost >= 0 ? best_ghost : best_dot;
	if (best_ghost < 0 && g->fruit_t > 60)		/* the fruit, while it's there, if the way's safe */
	{
		for (int x = 13; x <= 14; x++)
		{
			if (dist[20][x] > 0 && (best_dot < 0 || dist[20][x] < 2 * dist[best_dot / CH_COLS][best_dot % CH_COLS] + 12))
			{
				goal = 20 * CH_COLS + x;
			}
		}
	}
	if (goal >= 0)
	{
		return first[goal / CH_COLS][goal % CH_COLS];
	}
	return away;
}
