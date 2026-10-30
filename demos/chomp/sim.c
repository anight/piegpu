/*
 * sim.c - chomp played by the bot without a GPU (a PC: chomp_sim [games]
 * [FRAME FILE.ppm]): whole games, 60 frames a second of game time as fast as
 * it goes. Each: the levels cleared, the score, the ghosts and fruit eaten,
 * the lives lost. After every frame: the dots counted against the maze, the
 * player and the hunting ghosts on open tiles. A picture of the screen
 * (the maze; the actors as squares) at a frame on request.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "game.h"
#include "art.h"

static int tile_of (int p)		{ return p >= 0 ? p / 8 : -((7 - p) / 8); }

static void picture (const ch_t *g, const char *file)
{
	static uint8_t rgb[CH_H][CH_W][3];
	for (int y = 0; y < CH_H; y++)
	{
		for (int x = 0; x < CH_W; x++)
		{
			uint32_t c = ch_palette[g->screen[y][x]];
			rgb[y][x][0] = (uint8_t) (c >> 16), rgb[y][x][1] = (uint8_t) (c >> 8), rgb[y][x][2] = (uint8_t) c;
		}
	}
	for (int i = -1; i < 4; i++)
	{
		const ch_actor_t *a = i < 0 ? &g->pac : &g->ghost[i];
		uint32_t c = i < 0 ? 0xFFFF00 : a->fright ? 0x2121FF : ch_ghost_color[i];
		for (int y = a->y - 6; y <= a->y + 6; y++)
		{
			for (int x = a->x - 6; x <= a->x + 6; x++)
			{
				if (x >= 0 && y >= 0 && x < CH_W && y < CH_H)
				{
					rgb[y][x][0] = (uint8_t) (c >> 16), rgb[y][x][1] = (uint8_t) (c >> 8), rgb[y][x][2] = (uint8_t) c;
				}
			}
		}
	}
	FILE *f = fopen (file, "wb");
	fprintf (f, "P6 %d %d 255\n", CH_W, CH_H);
	fwrite (rgb, 1, sizeof rgb, f);
	fclose (f);
}

int main (int argc, char **argv)
{
	int games = argc > 1 ? atoi (argv[1]) : 5;
	int at = argc > 3 ? atoi (argv[2]) : -1;
	static ch_t g;
	int failures = 0;
	for (int n = 0; n < games; n++)
	{
		ch_new_game (&g, false, 0);
		g.rng ^= (uint32_t) n * 7919u;
		int frames = 0, ghosts = 0, fruits = 0, deaths = 0, best_level = 1;
		while (g.state != CH_GAME_OVER && frames < 60 * 60 * 60)
		{
			ch_frame (&g, ch_bot (&g));
			frames++;
			ghosts += (g.events & CH_EV_GHOST) != 0;
			fruits += (g.events & CH_EV_FRUIT) != 0;
			deaths += (g.events & CH_EV_DIE) != 0;
			best_level = g.level > best_level ? g.level : best_level;
			int left = 0;
			for (int y = 0; y < CH_ROWS; y++)
			{
				for (int x = 0; x < CH_COLS; x++)
				{
					left += g.tile[y][x] == T_DOT || g.tile[y][x] == T_ENERGIZER;
				}
			}
			if (left != CH_DOTS - g.dots_eaten && failures++ < 10)
			{
				printf ("  frame %d: %d dots in the maze, %d by the count\n", frames, left, CH_DOTS - g.dots_eaten);
			}
			int px = tile_of (g.pac.x), py = tile_of (g.pac.y);
			if (!ch_open (&g, px, py, false) && failures++ < 10)
			{
				printf ("  frame %d: the player in a wall at %d,%d (%d,%d)\n", frames, px, py, g.pac.x, g.pac.y);
			}
			for (int i = 0; i < 4; i++)
			{
				const ch_actor_t *gh = &g.ghost[i];
				if ((gh->state == G_ACTIVE || gh->state == G_EYES)
				    && !ch_open (&g, tile_of (gh->x), tile_of (gh->y), false) && failures++ < 10)
				{
					printf ("  frame %d: ghost %d (state %d) in a wall at %d,%d\n", frames, i, gh->state, gh->x, gh->y);
				}
			}
			if (frames == at)
			{
				picture (&g, argv[3]);
			}
			ch_clean (&g);
		}
		printf ("game %d: level %d, score %d, %d ghosts, %d fruit, %d lives lost, %.0f s%s\n", n + 1, best_level, g.score,
			ghosts, fruits, deaths, frames / 60.0, g.state == CH_GAME_OVER ? "" : " (stopped)");
	}
	printf ("%d checks failed\n", failures);
	return failures ? 1 : 0;
}
