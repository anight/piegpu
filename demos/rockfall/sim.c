/*
 * sim.c - rockfall without a GPU (a PC: rockfall_sim [tries] [CAVE FRAME FILE.ppm]): each cave tried by the
 * bot, a game a try, the rest of its lives in that cave: how often it got
 * out, the diamonds it took, the lives it lost, how. After every frame:
 * one miner at most, every cell a known object. A picture of a cave at a
 * frame on request.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "game.h"

static void picture (const rf_t *g, const char *file)
{
	static const uint32_t col[O_COUNT] =
	{
		0x000000, 0x6F4F25, 0x9A6759, 0xB0B0FF, 0x959595, 0x808080, 0x40FFFF, 0xFF8000, 0xFF40FF, 0x40C040,
		0xFFFFFF, 0xFFFFFF, 0xFFFF00, 0xFF0000, 0x959595, 0x00FF00
	};
	FILE *f = fopen (file, "wb");
	fprintf (f, "P6 %d %d 255\n", RF_W * 8, RF_H * 8);
	for (int y = 0; y < RF_H * 8; y++)
	{
		for (int x = 0; x < RF_W * 8; x++)
		{
			uint32_t c = col[rf_object (g->cell[y / 8][x / 8])];
			int o = rf_object (g->cell[y / 8][x / 8]);
			bool dot = (o == O_BOULDER || o == O_DIAMOND || o >= O_FIREFLY) && ((x % 8 == 0) || (y % 8 == 0));
			if (dot)
			{
				c = 0;
			}
			fputc ((int) (c >> 16), f), fputc ((int) (c >> 8) & 255, f), fputc ((int) c & 255, f);
		}
	}
	fclose (f);
}

int main (int argc, char **argv)
{
	int tries = argc > 1 ? atoi (argv[1]) : 5;
	int pic_cave = argc > 4 ? atoi (argv[2]) : -1, pic_frame = argc > 4 ? atoi (argv[3]) : -1;
	static rf_t g;
	int failures = 0;
	for (int cave = 0; cave < RF_CAVES; cave++)
	{
		int outs = 0, deaths = 0, timeouts = 0, best = 0;
		for (int t = 0; t < tries; t++)
		{
			g.rng = 0x1234u + (uint32_t) (t * 7919 + cave);
			rf_new_game (&g, false, 0);
			g.cave = cave;
			g.lives = 1;
			rf_start_cave (&g);
			int frames = 0;
			while (frames < 60 * 300)
			{
				int dx, dy;
				bool fire;
				rf_bot (&g, &dx, &dy, &fire);
				rf_frame (&g, dx, dy, fire);
				frames++;
				if (cave == pic_cave && frames == pic_frame && t == 0)
				{
					picture (&g, argv[4]);
				}
				int miners = 0;
				for (int y = 0; y < RF_H; y++)
				{
					for (int x = 0; x < RF_W; x++)
					{
						int o = rf_object (g.cell[y][x]);
						miners += o == O_MINER;
						if (o >= O_COUNT && failures++ < 10)
						{
							printf ("  cave %d frame %d: object %d at %d,%d\n", cave, frames, o, x, y);
						}
					}
				}
				if (miners > 1 && failures++ < 10)
				{
					printf ("  cave %d frame %d: %d miners\n", cave, frames, miners);
				}
				if (g.cave != cave || g.state == RF_GAME_OVER)
				{
					break;
				}
				if (g.state == RF_DEAD && g.frame == 1)
				{
					*(g.time == 0 ? &timeouts : &deaths) += 1;
				}
				best = g.diamonds > best ? g.diamonds : best;
			}
			outs += g.cave != cave;
		}
		printf ("cave %d %-12s: out %d of %d, %d lives lost (%d out of time), most diamonds %d (%d needed)\n", cave + 1,
			rf_caves[cave].name, outs, tries, deaths + timeouts, timeouts, best, rf_caves[cave].needed);
	}
	printf ("%d checks failed\n", failures);
	return failures ? 1 : 0;
}
