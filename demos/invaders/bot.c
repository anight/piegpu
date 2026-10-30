/*
 * bot.c - the attract mode's player (game.h): under the saucer when there is
 * one, else under the nearest of the lowest invaders, firing when it's
 * there; out of the way of a shot coming down on it.
 */
#include <stdlib.h>
#include "game.h"

void iv_bot (const iv_t *g, int *dir, bool *fire)
{
	*dir = 0;
	*fire = false;
	if (!g->alive)
	{
		return;
	}
	int me = g->px + 8;
	/* a shot coming down near: away from it */
	for (int k = 0; k < 3; k++)
	{
		const iv_shot_t *s = &g->ashot[k];
		if (s->on && s->y > IV_PLAYER_Y - 60 && abs (s->x + 1 - me) < 12)
		{
			*dir = s->x + 1 > me ? -1 : 1;
			if ((*dir < 0 && g->px <= 17) || (*dir > 0 && g->px >= 184))
			{
				*dir = -*dir;
			}
			return;
		}
	}
	int target = -1;
	if (g->saucer)
	{
		target = g->saucer_x + 8 + g->saucer_dx * 12;	/* (where it'll be when the shot gets up there) */
	}
	else
	{
		int best = 1000;
		for (int c = 0; c < IV_COLS; c++)
		{
			for (int r = 0; r < IV_ROWS; r++)
			{
				if (g->alive_inv[r][c])
				{
					int x = g->inv_x[r][c] + 8, d = abs (x - me) + 4 * r;
					if (d < best)
					{
						best = d;
						target = x;
					}
					break;
				}
			}
		}
	}
	if (target < 0)
	{
		return;
	}
	if (abs (target - me) > 2)
	{
		*dir = target > me ? 1 : -1;
	}
	*fire = abs (target - me) <= 3 && !g->shot.on && (g->frame & 1);	/* (pressed, let go, pressed) */
}
