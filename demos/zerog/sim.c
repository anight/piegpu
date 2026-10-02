/*
 * sim.c - zerog without a GPU: races flown by the craft themselves, as fast
 * as the PC goes, and what happened in them on the console. For tuning the
 * flight model and the craft's own flying (hosts/pc builds it: zerog_sim).
 *
 *   zerog_sim [RACES [TRACE.txt [PILOT]]]	(TRACE: where the player's craft went, a line a frame;
 *						 PILOT: the player's craft flown by the engine alone, full on
 *						 (1), or so and the stick thrown about (2): what a bad pilot
 *						 gets)
 */
#include <stdio.h>
#include <stdlib.h>
#include "race.h"

int main (int argc, char **argv)
{
	int races = argc > 1 ? atoi (argv[1]) : 3;
	FILE *trace = argc > 2 ? fopen (argv[2], "w") : NULL;
	int pilot = argc > 3 ? atoi (argv[3]) : 0;
	track_build ();
	printf ("the loop %.0f m (%d rings), the fork %.0f m for %.0f m of it\n", route[ROUTE_MAIN].len, route[ROUTE_MAIN].n,
		route[ROUTE_ALT].len, route[ROUTE_ALT].to - route[ROUTE_ALT].from);
	float now = 0.0f;
	for (int r = 0; r < races; r++)
	{
		float vmax = 0.0f, begun = now;
		race_reset (now, pilot != 0);
		while (!race_step (1.0f / 60, now) && now - begun < 600.0f)
		{
			craft_t *c = &race.craft[race.player];
			c->in.thrust = 1.0f;
			c->in.steer = pilot == 2 ? ((int) (now / 0.7f) * 7919 % 5 - 2) / 2.0f : 0.0f;
			c->in.fire = pilot == 2 && ((int) (now * 4) & 1);
			now += 1.0f / 60;
			vmax = craft_speed (c) > vmax ? craft_speed (c) : vmax;
			if (trace && r == 0)
			{
				fprintf (trace, "%.3f %.2f %.2f %.2f %.2f %d %.1f %.2f %.2f %d %d\n", now, c->p[0], c->p[1], c->p[2],
					 craft_speed (c), c->at.route, c->at.s, c->x, c->h, c->air, c->lap);
			}
			for (int i = 0; i < CRAFTS; i++)
			{
				race.craft[i].events = 0;
			}
		}
		int order[CRAFTS];
		race_standings (order);
		printf ("race %d: %.1f s, the player's top speed %.1f m/s\n", r + 1, now - begun, vmax);
		for (int i = 0; i < CRAFTS; i++)
		{
			const craft_t *c = &race.craft[order[i]];
			printf ("  %d craft %d skill %.3f: %7.2f s, best lap %6.2f; walls %2d, falls %d, hit %d, pickups %d\n", i + 1,
				order[i], c->skill, c->place ? c->finish : -1.0f, c->best_lap, c->walls, c->falls, c->hits, c->pickups);
		}
	}
	return 0;
}
