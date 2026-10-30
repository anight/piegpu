/*
 * caves.c - rockfall's caves: made the original's way (its generator, its
 * kinds of drawing), designed here. A cave is first filled at random - for
 * each cell inside the border, the next of the original's pseudo-random
 * numbers (Peter Broadribb's "Inside BoulderDash": seed 1 from 0, seed 2 the
 * cave's) is compared with four chances, a later one winning - dirt where
 * none is under; then steel round it; then its drawing: points, lines (the
 * original's 8 directions), rectangles outlined or filled.
 */
#include <string.h>
#include "game.h"

/* the original's NextRandom */
static int next_random (int *seed1, int *seed2)
{
	int temp1 = (*seed1 & 0x01) * 0x80;
	int temp2 = (*seed2 >> 1) & 0x7F;
	int result = *seed2 + (*seed2 & 0x01) * 0x80;
	int carry = result > 0xFF;
	result = (result & 0xFF) + carry + 0x13;
	carry = result > 0xFF;
	*seed2 = result & 0xFF;
	result = *seed1 + carry + temp1;
	carry = result > 0xFF;
	result = (result & 0xFF) + carry + temp2;
	*seed1 = result & 0xFF;
	return *seed1;
}

#define FF(d)		((uint16_t) (O_FIREFLY | (d) << F_DIR_SHIFT))
#define BF(d)		((uint16_t) (O_BUTTERFLY | (d) << F_DIR_SHIFT))
enum { UP, UP_RIGHT, RIGHT, DOWN_RIGHT, DOWN, DOWN_LEFT, LEFT, UP_LEFT };	/* (a line's) */
enum { FACE_LEFT, FACE_UP, FACE_RIGHT, FACE_DOWN };			/* (a creature's) */

static const rf_draw_t cave_dig[] =
{
	{'F', 2, 1, 4, 3, O_DIRT, O_DIRT}, {'P', 3, 2, 0, 0, O_INBOX, 0},
	{'L', 8, 6, 14, RIGHT, O_BRICK, 0}, {'L', 18, 12, 16, RIGHT, O_BRICK, 0}, {'L', 4, 16, 10, RIGHT, O_BRICK, 0},
	{'L', 30, 3, 6, DOWN, O_BRICK, 0},
	{'P', 38, 18, 0, 0, O_PREOUT, 0},
	{0, 0, 0, 0, 0, 0, 0}
};

static const rf_draw_t cave_fireflies[] =
{
	{'R', 10, 4, 6, 4, O_BRICK, 0}, {'F', 11, 5, 4, 2, O_SPACE, O_SPACE}, {'P', 12, 5, 0, 0, FF (FACE_LEFT), 0},
	{'P', 12, 7, 0, 0, O_DIRT, 0},
	{'R', 25, 9, 6, 4, O_BRICK, 0}, {'F', 26, 10, 4, 2, O_SPACE, O_SPACE}, {'P', 27, 10, 0, 0, FF (FACE_LEFT), 0},
	{'P', 25, 11, 0, 0, O_DIRT, 0},
	{'R', 8, 14, 6, 4, O_BRICK, 0}, {'F', 9, 15, 4, 2, O_SPACE, O_SPACE}, {'P', 10, 15, 0, 0, FF (FACE_LEFT), 0},
	{'P', 13, 14, 0, 0, O_DIRT, 0},
	{'F', 1, 18, 4, 3, O_DIRT, O_DIRT}, {'P', 2, 19, 0, 0, O_INBOX, 0},
	{'P', 37, 2, 0, 0, O_PREOUT, 0},
	{0, 0, 0, 0, 0, 0, 0}
};

#define POCKET(x, y)	{'L', x, (y) - 2, 4, RIGHT, O_BOULDER, 0}, {'L', x, (y) - 1, 4, RIGHT, O_DIRT, 0}, \
			{'F', x, y, 4, 2, O_SPACE, O_SPACE}, {'P', (x) + 1, y, 0, 0, BF (FACE_DOWN), 0}, \
			{'L', (x) - 1, (y) + 2, 6, RIGHT, O_BRICK, 0}

static const rf_draw_t cave_butterflies[] =
{
	POCKET (6, 6), POCKET (16, 10), POCKET (26, 6), POCKET (31, 15),
	{'F', 19, 1, 4, 3, O_DIRT, O_DIRT}, {'P', 20, 2, 0, 0, O_INBOX, 0},
	{'P', 2, 19, 0, 0, O_PREOUT, 0},
	{0, 0, 0, 0, 0, 0, 0}
};

static const rf_draw_t cave_amoeba[] =
{
	{'R', 15, 8, 10, 7, O_BRICK, 0}, {'F', 16, 9, 8, 5, O_SPACE, O_SPACE}, {'P', 15, 11, 0, 0, O_DIRT, 0},
	{'P', 20, 11, 0, 0, O_AMOEBA, 0},
	{'F', 1, 1, 4, 3, O_DIRT, O_DIRT}, {'P', 2, 2, 0, 0, O_INBOX, 0},
	{'P', 37, 19, 0, 0, O_PREOUT, 0},
	{0, 0, 0, 0, 0, 0, 0}
};

static const rf_draw_t cave_magic[] =			/* an intermission: 20 x 12, the rest steel */
{
	{'F', 20, 0, 20, 22, O_STEEL, O_STEEL}, {'F', 0, 12, 20, 10, O_STEEL, O_STEEL},
	{'L', 0, 11, 20, RIGHT, O_STEEL, 0}, {'L', 19, 0, 12, DOWN, O_STEEL, 0},
	{'L', 3, 3, 14, RIGHT, O_BOULDER, 0}, {'L', 2, 7, 16, RIGHT, O_MAGIC, 0},
	{'F', 1, 8, 18, 3, O_SPACE, O_SPACE},
	{'P', 1, 10, 0, 0, O_INBOX, 0}, {'P', 18, 10, 0, 0, O_PREOUT, 0},
	{0, 0, 0, 0, 0, 0, 0}
};

/* (the C64's colours, Pepto's measures of them) */
#define C64_ORANGE	0x6F4F25
#define C64_BROWN	0x433900
#define C64_LT_RED	0x9A6759
#define C64_YELLOW	0xB8C76F
#define C64_GREY	0x6C6C6C
#define C64_CYAN	0x70A4B2
#define C64_BLUE	0x352879
#define C64_LT_BLUE	0x6C5EB5
#define C64_LT_GREEN	0x9AD284
#define C64_GREEN	0x588D43
#define C64_DK_GREY	0x444444
#define C64_PURPLE	0x6F3D86
#define C64_LT_GREY	0x959595

const rf_cave_t rf_caves[RF_CAVES] =
{
	{"FIRST DIG", false, 12, 10, 15, 150, 0, 0, 0x0A, {O_BOULDER, O_SPACE, O_DIAMOND, 0}, {0x30, 0x10, 0x0A, 0},
	 {C64_ORANGE, C64_LT_RED, C64_YELLOW}, cave_dig},
	{"FIREFLIES", false, 15, 10, 20, 140, 0, 0, 0x21, {O_BOULDER, O_DIAMOND, 0, 0}, {0x28, 0x0C, 0, 0},
	 {C64_BROWN, C64_GREY, C64_CYAN}, cave_fireflies},
	{"BUTTERFLIES", false, 18, 15, 20, 150, 0, 0, 0x5A, {O_BOULDER, O_DIAMOND, 0, 0}, {0x18, 0x06, 0, 0},
	 {C64_BLUE, C64_LT_BLUE, C64_LT_GREEN}, cave_butterflies},
	{"AMOEBA", false, 15, 10, 15, 160, 0, 30, 0x7F, {O_BOULDER, O_SPACE, O_DIAMOND, 0}, {0x20, 0x0C, 0x0A, 0},
	 {C64_GREEN, C64_DK_GREY, C64_LT_GREEN}, cave_amoeba},
	{"MAGIC", true, 8, 30, 40, 30, 15, 0, 0x33, {O_BOULDER, 0, 0, 0}, {0x10, 0, 0, 0},
	 {C64_PURPLE, C64_LT_GREY, 0xFFFFFF}, cave_magic},
};

static void put (rf_t *g, int x, int y, uint16_t v)
{
	if (x >= 0 && y >= 0 && x < RF_W && y < RF_H)
	{
		g->cell[y][x] = v;
	}
}

void rf_build_cave (rf_t *g, int index)
{
	const rf_cave_t *cave = &rf_caves[index];
	int seed1 = 0, seed2 = cave->seed;
	for (int y = 1; y < RF_H - 1; y++)
	{
		for (int x = 0; x < RF_W; x++)
		{
			uint16_t o = O_DIRT;
			int r = next_random (&seed1, &seed2);
			for (int i = 0; i < 4; i++)
			{
				if (cave->prob[i] && r < cave->prob[i])
				{
					o = cave->fill[i];
				}
			}
			g->cell[y][x] = o;
		}
	}
	for (int x = 0; x < RF_W; x++)
	{
		g->cell[0][x] = g->cell[RF_H - 1][x] = O_STEEL;
	}
	for (int y = 0; y < RF_H; y++)
	{
		g->cell[y][0] = g->cell[y][RF_W - 1] = O_STEEL;
	}
	static const int8_t ldx[8] = {0, 1, 1, 1, 0, -1, -1, -1}, ldy[8] = {-1, -1, 0, 1, 1, 1, 0, -1};
	for (const rf_draw_t *d = cave->draw; d && d->op; d++)
	{
		switch (d->op)
		{
		case 'P':
			put (g, d->x, d->y, d->obj);
			break;
		case 'L':
			for (int k = 0; k < d->a; k++)
			{
				put (g, d->x + ldx[d->b] * k, d->y + ldy[d->b] * k, d->obj);
			}
			break;
		case 'R':
		case 'F':
			for (int y = d->y; y < d->y + d->b; y++)
			{
				for (int x = d->x; x < d->x + d->a; x++)
				{
					bool edge = x == d->x || y == d->y || x == d->x + d->a - 1 || y == d->y + d->b - 1;
					if (edge || d->op == 'F')
					{
						put (g, x, y, edge ? d->obj : d->fill);
					}
				}
			}
			break;
		}
	}
}
