/*
 * sim.c - Xonix played by the bot without a GPU (a PC: xonix_sim [games]
 * [seed]), 60 updates a second of game time, as fast as it goes. Each game to
 * its end (or level 12, or an hour): the level reached, the score, the lives
 * lost and why. After every update the rules' own counts are checked against
 * the field: the trail, the sea left, where the balls and the player are.
 */
#include <stdio.h>
#include <stdlib.h>
#include "game.h"

static int failures;

static void check (const xn_t *g, double t)
{
	int trail = 0, sea = 0;
	for (int y = 0; y < XN_ROWS; y++)
	{
		for (int x = 0; x < XN_COLS; x++)
		{
			uint8_t c = g->cell[y][x];
			bool rim = x < XN_RIM || y < XN_RIM || x >= XN_COLS - XN_RIM || y >= XN_ROWS - XN_RIM;
			trail += c == XN_TRAIL;
			sea += !rim && !XN_IS_LAND (c);
			if (rim && !XN_IS_LAND (c) && failures++ < 10)
			{
				printf ("  %.2f s: the rim at %d,%d isn't land (%u)\n", t, x, y, c);
			}
		}
	}
	if ((trail != g->trail || sea != g->sea_left) && failures++ < 10)
	{
		printf ("  %.2f s: trail %d (counted %d), sea %d (counted %d)\n", t, trail, g->trail, sea, g->sea_left);
	}
	for (int i = 0; i < g->n_sea; i++)
	{
		if (g->cell[g->sea[i].y][g->sea[i].x] != XN_SEA && g->state == XN_PLAYING && failures++ < 10)
		{
			printf ("  %.2f s: sea ball %d on %u\n", t, i, g->cell[g->sea[i].y][g->sea[i].x]);
		}
	}
	for (int i = 0; i < g->n_land; i++)
	{
		if (!XN_IS_LAND (g->cell[g->land[i].y][g->land[i].x]) && failures++ < 10)
		{
			printf ("  %.2f s: land ball %d on %u\n", t, i, g->cell[g->land[i].y][g->land[i].x]);
		}
	}
	if (g->at_sea != (g->cell[g->y][g->x] == XN_TRAIL) && g->state == XN_PLAYING && failures++ < 10)
	{
		printf ("  %.2f s: the player at %d,%d: at sea %d, on %u\n", t, g->x, g->y, g->at_sea, g->cell[g->y][g->x]);
	}
}

int main (int argc, char **argv)
{
	int games = argc > 1 ? atoi (argv[1]) : 10;
	uint32_t seed = argc > 2 ? (uint32_t) strtoul (argv[2], NULL, 0) : 1;
	static xn_t g;
	static xn_bot_t bot;
	int levels = 0, best = 0;
	for (int n = 0; n < games; n++)
	{
		xn_new_game (&g, seed + n * 7919);
		xn_bot_init (&bot, seed * 31 + n);
		double t = 0;
		int deaths[3] = {0}, captures = 0, steps_out = 0;	/* deaths: sea ball, land ball, own trail */
		const double dt = 1.0 / 60;
		while (g.state != XN_OVER && g.level <= 12 && t < 3600)
		{
			int was_lives = g.lives, was_trail = g.trail;
			bool was_sea = g.at_sea;
			int want = xn_bot (&bot, &g, (float) dt);
			xn_update (&g, want, (float) dt);
			t += dt;
			if (g.events & XN_EV_DIE)
			{
				/* (which: at sea, its own trail if it was heading into it; else a ball) */
				deaths[!was_sea ? 1 : 0]++;
				(void) was_lives;
				(void) was_trail;
			}
			captures += (g.events & XN_EV_CAPTURE) != 0;
			steps_out += (g.events & XN_EV_STEP) != 0;
			check (&g, t);
		}
		levels += g.level;
		best = g.level > best ? g.level : best;
		printf ("game %d: level %d, %d%% of it, score %d, %.0f s; %d captures, %d lives lost (%d at sea, %d on land)%s\n",
			n + 1, g.level, xn_filled (&g), g.score, t, captures, deaths[0] + deaths[1], deaths[0], deaths[1],
			g.state == XN_OVER ? "" : " (stopped)");
	}
	printf ("%d games: level %.1f on average, %d at best; %d checks failed\n", games, (double) levels / games, best, failures);
	return failures ? 1 : 0;
}
