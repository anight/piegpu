/*
 * bot.c - the attract mode's miner (game.h): the shortest way to the nearest
 * diamond, to the exit once it's out; never down from under a boulder or a
 * diamond (it would follow), never under a falling one, never next to a
 * firefly or a butterfly. With no way: a push, or it waits.
 */
#include <stdlib.h>
#include <string.h>
#include "game.h"

static int obj (const rf_t *g, int x, int y)
{
	return x < 0 || y < 0 || x >= RF_W || y >= RF_H ? O_STEEL : rf_object (g->cell[y][x]);
}

static bool heavy (const rf_t *g, int x, int y)
{
	int o = obj (g, x, y);
	return o == O_BOULDER || o == O_DIAMOND;
}

static bool unsafe (const rf_t *g, int x, int y)
{
	for (int yy = y - 1; yy <= y + 1; yy++)
	{
		for (int xx = x - 1; xx <= x + 1; xx++)
		{
			int o = obj (g, xx, yy);
			if (o == O_FIREFLY || o == O_BUTTERFLY)
			{
				return true;
			}
		}
	}
	return y > 0 && (g->cell[y - 1][x] & F_FALLING) && heavy (g, x, y - 1);
}

void rf_bot (const rf_t *g, int *dx, int *dy, bool *fire)
{
	static const int8_t ddx[4] = {-1, 0, 1, 0}, ddy[4] = {0, -1, 0, 1};
	static int8_t first[RF_H][RF_W];
	static int16_t queue[RF_W * RF_H][2];
	*dx = *dy = 0;
	*fire = false;
	if (g->state != RF_PLAYING || obj (g, g->mx, g->my) != O_MINER)
	{
		return;
	}
	memset (first, -1, sizeof first);
	int head = 0, tail = 0;
	first[g->my][g->mx] = 4;
	queue[tail][0] = (int16_t) g->mx, queue[tail++][1] = (int16_t) g->my;
	while (head < tail)
	{
		int x = queue[head][0], y = queue[head++][1];
		int o = obj (g, x, y);
		if (first[y][x] != 4 && (o == O_DIAMOND || (o == O_OUTBOX && g->open)))
		{
			*dx = ddx[first[y][x]];
			*dy = ddy[first[y][x]];
			return;
		}
		for (int d = 0; d < 4; d++)
		{
			int nx = x + ddx[d], ny = y + ddy[d], n = obj (g, nx, ny);
			if (nx < 0 || ny < 0 || nx >= RF_W || ny >= RF_H || first[ny][nx] >= 0)
			{
				continue;
			}
			if (!(n == O_SPACE || n == O_DIRT || n == O_DIAMOND || (n == O_OUTBOX && g->open)))
			{
				continue;
			}
			if (unsafe (g, nx, ny) || (d == 3 && heavy (g, x, y - 1)))
			{
				continue;
			}
			first[ny][nx] = (int8_t) (first[y][x] == 4 ? d : first[y][x]);
			queue[tail][0] = (int16_t) nx, queue[tail++][1] = (int16_t) ny;
		}
	}
	/* no way: push a boulder that can go */
	for (int d = 0; d < 4; d += 2)
	{
		if (obj (g, g->mx + ddx[d], g->my) == O_BOULDER && obj (g, g->mx + 2 * ddx[d], g->my) == O_SPACE)
		{
			*dx = ddx[d];
			return;
		}
	}
}
