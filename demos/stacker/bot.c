/*
 * bot.c - the attract mode's player (game.h): as each piece comes, every
 * orientation in every column dropped straight down and the stack it leaves
 * judged (its height, its holes, its bumpiness, the lines it makes: weights
 * as in Yiyuan Lee's well-known tuning); then the presses to get there -
 * turns, steps across, a soft drop - as a player's, a press and a let-go.
 */
#include <string.h>
#include "game.h"

static int target_orient, target_x, plan_for = -1;

static double judge (uint8_t f[ST_ROWS][ST_COLS])
{
	int heights[ST_COLS], holes = 0, lines = 0;
	for (int x = 0; x < ST_COLS; x++)
	{
		heights[x] = 0;
		for (int y = 0; y < ST_ROWS; y++)
		{
			if (f[y][x])
			{
				heights[x] = ST_ROWS - y;
				for (int yy = y + 1; yy < ST_ROWS; yy++)
				{
					holes += !f[yy][x];
				}
				break;
			}
		}
	}
	for (int y = 0; y < ST_ROWS; y++)
	{
		int n = 0;
		for (int x = 0; x < ST_COLS; x++)
		{
			n += f[y][x] != 0;
		}
		lines += n == ST_COLS;
	}
	int aggregate = 0, bump = 0;
	for (int x = 0; x < ST_COLS; x++)
	{
		aggregate += heights[x];
		if (x)
		{
			bump += heights[x] > heights[x - 1] ? heights[x] - heights[x - 1] : heights[x - 1] - heights[x];
		}
	}
	return -0.510066 * aggregate + 0.760666 * lines - 0.35663 * holes - 0.184483 * bump;
}

static void plan (const st_t *g)
{
	double best = -1e9;
	target_orient = g->orient;
	target_x = g->px;
	int o = st_spawn[g->piece];
	for (int turns = 0; turns < 4; turns++, o = st_rotate (o, true))
	{
		for (int x = -2; x < ST_COLS + 2; x++)
		{
			if (!st_fits (g, o, x, 0))
			{
				continue;
			}
			int y = 0;
			while (st_fits (g, o, x, y + 1))
			{
				y++;
			}
			uint8_t f[ST_ROWS][ST_COLS];
			memcpy (f, g->field, sizeof f);
			for (int k = 0; k < 4; k++)
			{
				int cy = y + st_orient[o][k][1];
				if (cy >= 0)
				{
					f[cy][x + st_orient[o][k][0]] = (uint8_t) (g->piece + 1);
				}
			}
			double v = judge (f);
			if (v > best)
			{
				best = v;
				target_orient = o;
				target_x = x;
			}
		}
	}
}

void st_bot (const st_t *g, st_input_t *in)
{
	memset (in, 0, sizeof *in);
	if (g->state != ST_FALLING)
	{
		return;
	}
	if (plan_for != g->spawns)
	{
		plan_for = g->spawns;
		plan (g);
	}
	bool tick = g->frame & 1;			/* (presses need a let-go between them) */
	if (g->orient != target_orient)
	{
		in->rotate_cw = tick;
		return;
	}
	if (g->px != target_x)
	{
		in->left = g->px > target_x && tick;
		in->right = g->px < target_x && tick;
		return;
	}
	in->down = true;
}
