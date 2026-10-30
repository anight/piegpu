/*
 * sim.c - stacker played by the bot without a GPU (a PC: stacker_sim [games] [start level] [FRAME FILE.pgm]):
 * whole games, 60 frames a second of game time as fast as it goes. Each: the
 * lines, the score, the level reached, the pieces of each kind. After every
 * frame: the falling piece where it fits. A picture of the screen (colour
 * numbers, stretched to grey) at a frame on request.
 */
#include <stdio.h>
#include <stdlib.h>
#include "game.h"

int main (int argc, char **argv)
{
	int games = argc > 1 ? atoi (argv[1]) : 5, start = argc > 2 ? atoi (argv[2]) : 0;
	int at = argc > 4 ? atoi (argv[3]) : -1;
	static st_t g;
	int failures = 0;
	for (int n = 0; n < games; n++)
	{
		g.lfsr = (uint16_t) (0x8988 + n * 4099);
		st_new_game (&g, false, start, 0);
		long frames = 0;
		while (g.state != ST_OVER && frames < 60L * 60 * 60)
		{
			st_input_t in;
			st_bot (&g, &in);
			st_frame (&g, &in);
			frames++;
			if (g.state == ST_FALLING && !st_fits (&g, g.orient, g.px, g.py) && failures++ < 10)
			{
				printf ("  frame %ld: the piece where it doesn't fit\n", frames);
			}
			if (frames == at)
			{
				FILE *f = fopen (argv[4], "wb");
				fprintf (f, "P5 %d %d 255\n", ST_W, ST_H);
				for (int y = 0; y < ST_H; y++)
				{
					for (int x = 0; x < ST_W; x++)
					{
						fputc (g.screen[y][x] * 255 / (C_COUNT - 1), f);
					}
				}
				fclose (f);
			}
			st_clean (&g);
		}
		printf ("game %d: %d lines, score %d, level %d, %ld s; pieces T %d J %d Z %d O %d S %d L %d I %d%s\n", n + 1,
			g.lines, g.score, g.level, frames / 60, g.stats[0], g.stats[1], g.stats[2], g.stats[3], g.stats[4],
			g.stats[5], g.stats[6], g.state == ST_OVER ? "" : " (stopped)");
	}
	printf ("%d checks failed\n", failures);
	return failures ? 1 : 0;
}
