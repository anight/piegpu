/*
 * sim.c - the invaders played by the bot without a GPU (a PC: invaders_sim
 * [games]): whole games, 60 frames a second of game time as fast as it goes.
 * Each: the rounds cleared, the score, the saucers hit, the frames; after
 * every frame the count of invaders left is checked against the rack, and
 * the screen's pixels against 0 and 255. An .pgm of the screen at a frame
 * on request (invaders_sim 1 FRAME FILE.pgm), to look at.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "game.h"

int main (int argc, char **argv)
{
	int games = argc > 1 ? atoi (argv[1]) : 5;
	int shot_at = argc > 3 ? atoi (argv[2]) : -1;
	const char *shot_file = argc > 3 ? argv[3] : NULL;
	static iv_t g;
	int failures = 0, hi = 0;
	for (int n = 0; n < games; n++)
	{
		g.rng = 1234u + (unsigned) n;
		iv_new_game (&g, false, hi);
		int frames = 0, saucers = 0, shots = 0, max_round = 0;
		while (g.state != IV_GAME_OVER && frames < 60 * 60 * 30)
		{
			int dir;
			bool fire;
			iv_bot (&g, &dir, &fire);
			iv_frame (&g, dir, fire);
			frames++;
			saucers += (g.events & IV_EV_SAUCER_HIT) != 0;
			shots += (g.events & IV_EV_SHOT) != 0;
			max_round = g.round > max_round ? g.round : max_round;
			int alive = 0;
			for (int r = 0; r < IV_ROWS; r++)
			{
				for (int c = 0; c < IV_COLS; c++)
				{
					alive += g.alive_inv[r][c];
				}
			}
			if (alive != g.n_alive && g.state == IV_PLAYING && failures++ < 10)
			{
				printf ("  frame %d: %d invaders counted, %d in the rack\n", frames, g.n_alive, alive);
			}
			if (frames == shot_at && shot_file)
			{
				FILE *f = fopen (shot_file, "wb");
				fprintf (f, "P5 %d %d 255\n", IV_W, IV_H);
				fwrite (g.screen, 1, sizeof g.screen, f);
				fclose (f);
			}
			iv_clean (&g);
		}
		for (int y = 0; y < IV_H; y++)
		{
			for (int x = 0; x < IV_W; x++)
			{
				if (g.screen[y][x] != 0 && g.screen[y][x] != 255 && failures++ < 10)
				{
					printf ("  a pixel at %d,%d is %u\n", x, y, g.screen[y][x]);
				}
			}
		}
		hi = g.score > hi ? g.score : hi;
		printf ("game %d: %d rounds cleared, score %d, %d shots, %d saucers hit, %.0f s%s\n", n + 1, max_round, g.score,
			shots, saucers, frames / 60.0, g.state == IV_GAME_OVER ? "" : " (stopped)");
	}
	printf ("%d checks failed\n", failures);
	return failures ? 1 : 0;
}
