/*
 * art.c - chomp's pictures, drawn by code (shapes, not bitmaps copied from
 * anywhere) in the arcade's sizes and colours: the player a 13-pixel disc
 * with its mouth, its death opening the mouth till it's gone; a ghost 14 x
 * 14, a dome on a wavy skirt, its eyes looking the way it goes; frightened,
 * blue with a peach face, flashing white and red; the eight fruits; the
 * scores.
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "game.h"
#include "art.h"
#include "arcade_font.h"

const uint32_t ch_palette[P_COUNT] =
{
	[P_BLACK] = 0x000000, [P_WALL] = 0x2121DE, [P_DOT] = 0xFFB8AE, [P_ENERGIZER] = 0xFFB8AE,
	[P_DOOR] = 0xFFB8DE, [P_WHITE] = 0xDEDEDE, [P_YELLOW] = 0xFFFF00, [P_RED] = 0xFF0000,
	[P_PINK] = 0xFFB8FF, [P_CYAN] = 0x00FFFF, [P_ORANGE] = 0xFFB852, [P_PEACH] = 0xFFB8AE
};
const uint32_t ch_ghost_color[4] = {0xFF0000, 0xFFB8FF, 0x00FFFF, 0xFFB852};

static uint8_t *atlas;

static void put (int cell, int x, int y, uint32_t rgb)
{
	if (x < 0 || y < 0 || x >= 16 || y >= 16)
	{
		return;
	}
	uint8_t *p = &atlas[(((cell / 16) * 16 + y) * ATLAS_W + (cell % 16) * 16 + x) * 4];
	p[0] = (uint8_t) (rgb >> 16);
	p[1] = (uint8_t) (rgb >> 8);
	p[2] = (uint8_t) rgb;
	p[3] = 255;
}

static void disc (int cell, float cx, float cy, float r, uint32_t rgb)
{
	for (int y = 0; y < 16; y++)
	{
		for (int x = 0; x < 16; x++)
		{
			float dx = x + 0.5f - cx, dy = y + 0.5f - cy;
			if (dx * dx + dy * dy <= r * r)
			{
				put (cell, x, y, rgb);
			}
		}
	}
}

static void box (int cell, int x0, int y0, int w, int h, uint32_t rgb)
{
	for (int y = y0; y < y0 + h; y++)
	{
		for (int x = x0; x < x0 + w; x++)
		{
			put (cell, x, y, rgb);
		}
	}
}

/* the player: a disc with a wedge out of it, the mouth (half its angle, degrees) the way it faces */
static void pac (int cell, int dir, float mouth)
{
	static const float ax[4] = {0, -1, 0, 1}, ay[4] = {-1, 0, 1, 0};
	float c = cosf (mouth * 3.14159265f / 180.0f);
	for (int y = 0; y < 16; y++)
	{
		for (int x = 0; x < 16; x++)
		{
			float dx = x + 0.5f - 8.0f, dy = y + 0.5f - 8.0f, d = sqrtf (dx * dx + dy * dy);
			if (d > 6.6f)
			{
				continue;
			}
			bool in_mouth = mouth > 0 && d > 0.4f && (dx * ax[dir] + dy * ay[dir]) / d > c;
			if (mouth >= 180.0f || !in_mouth)
			{
				if (mouth < 180.0f)
				{
					put (cell, x, y, 0xFFFF00);
				}
			}
		}
	}
}

/* a ghost's body: a dome over a skirt; its feet, two ways */
static void body (int cell, int frame, uint32_t rgb)
{
	disc (cell, 8.0f, 7.5f, 6.5f, rgb);
	box (cell, 2, 7, 13, 6, rgb);
	static const char *const feet[2] = {"##.###..###.##", "#.###.##.###.#"};	/* (14 wide, at x 1) */
	for (int x = 0; x < 14; x++)
	{
		if (feet[frame][x] == '#')
		{
			box (cell, 1 + x, 13, 1, 1, rgb);
		}
		box (cell, 1 + x, 7, 1, 6, rgb);
	}
}

static void eyes (int cell, int dir)
{
	static const int px[4] = {0, -1, 0, 1}, py[4] = {-1, 0, 1, 0};
	for (int e = 0; e < 2; e++)
	{
		int ex = e ? 9 : 4, ey = 5;
		box (cell, ex + px[dir], ey + py[dir], 4, 5, 0xDEDEDE);
		box (cell, ex - 1 + px[dir], ey + 1 + py[dir], 6, 3, 0xDEDEDE);
		box (cell, ex + 1 + 2 * px[dir], ey + 2 + 2 * py[dir], 2, 2, 0x2121DE);
	}
}

static void frightened (int cell, int frame, uint32_t body_rgb, uint32_t face)
{
	body (cell, frame, body_rgb);
	box (cell, 5, 5, 2, 2, face);
	box (cell, 9, 5, 2, 2, face);
	static const char mouth[2][12] = {".#.#.#.#.#.", "#.#.#.#.#.#"};	/* (a zigzag) */
	for (int x = 0; x < 11; x++)
	{
		box (cell, 3 + x, mouth[0][x] == '#' ? 9 : 10, 1, 1, face);
	}
	(void) mouth[1];
}

static void line (int cell, int x0, int y0, int x1, int y1, uint32_t rgb)
{
	int n = abs (x1 - x0) > abs (y1 - y0) ? abs (x1 - x0) : abs (y1 - y0);
	for (int i = 0; i <= n; i++)
	{
		put (cell, x0 + (x1 - x0) * i / (n ? n : 1), y0 + (y1 - y0) * i / (n ? n : 1), rgb);
	}
}

static void fruit (int cell, int kind)
{
	switch (kind)
	{
	case 0:					/* cherries */
		line (cell, 5, 9, 11, 2, 0xDE9751);
		line (cell, 10, 8, 11, 2, 0xDE9751);
		disc (cell, 5.0f, 11.0f, 3.2f, 0xFF0000);
		disc (cell, 10.5f, 10.0f, 3.2f, 0xFF0000);
		put (cell, 4, 10, 0xDEDEDE);
		put (cell, 9, 9, 0xDEDEDE);
		break;
	case 1:					/* a strawberry */
		disc (cell, 8.0f, 8.5f, 5.0f, 0xFF0000);
		for (int y = 12; y < 15; y++)
		{
			box (cell, 8 - (14 - y), y, 2 * (14 - y) + 1, 1, 0xFF0000);
		}
		box (cell, 5, 3, 7, 2, 0x00DE00);
		box (cell, 7, 2, 3, 1, 0x00DE00);
		for (int k = 0; k < 6; k++)
		{
			put (cell, 5 + (k * 3) % 7, 7 + (k * 5) % 6, 0xDEDEDE);
		}
		break;
	case 2:					/* a peach */
		disc (cell, 8.0f, 9.0f, 5.5f, 0xFFB852);
		line (cell, 8, 4, 8, 7, 0xDE9751);
		box (cell, 9, 3, 3, 2, 0x00DE00);
		break;
	case 3:					/* an apple */
		disc (cell, 6.0f, 9.5f, 4.6f, 0xFF0000);
		disc (cell, 10.0f, 9.5f, 4.6f, 0xFF0000);
		line (cell, 8, 3, 8, 6, 0xDE9751);
		box (cell, 9, 3, 3, 2, 0x00DE00);
		put (cell, 5, 8, 0xDEDEDE);
		break;
	case 4:					/* grapes */
		for (int k = 0; k < 9; k++)
		{
			static const int8_t gx[9] = {5, 8, 11, 6, 9, 12, 7, 10, 8}, gy[9] = {6, 6, 6, 9, 9, 9, 12, 12, 14};
			disc (cell, gx[k] - 0.5f, gy[k], 1.9f, 0x00DE00);
		}
		line (cell, 8, 2, 8, 4, 0xDE9751);
		break;
	case 5:					/* a ship */
		box (cell, 7, 2, 2, 11, 0xDEDEDE);
		box (cell, 4, 8, 8, 3, 0x2121DE);
		box (cell, 2, 10, 2, 4, 0xFF0000);
		box (cell, 12, 10, 2, 4, 0xFF0000);
		box (cell, 7, 13, 2, 2, 0xFFFF00);
		break;
	case 6:					/* a bell */
		disc (cell, 8.0f, 7.0f, 4.5f, 0xFFFF00);
		box (cell, 3, 7, 10, 5, 0xFFFF00);
		box (cell, 2, 12, 12, 1, 0xFFFF00);
		box (cell, 7, 13, 2, 2, 0x00FFFF);
		break;
	default:				/* a key */
		disc (cell, 8.0f, 4.5f, 3.5f, 0x00FFFF);
		disc (cell, 8.0f, 4.5f, 1.6f, 0x000000);
		box (cell, 7, 7, 2, 8, 0xDEDEDE);
		box (cell, 9, 10, 2, 1, 0xDEDEDE);
		box (cell, 9, 13, 2, 1, 0xDEDEDE);
		break;
	}
}

/* a score, two cells wide, in the font */
static void score (int cell, int value, uint32_t rgb)
{
	char s[8];
	int n = 0;
	for (int v = value; v; v /= 10)
	{
		n++;
	}
	for (int i = n - 1, v = value; i >= 0; i--, v /= 10)
	{
		s[i] = (char) ('0' + v % 10);
	}
	int x0 = 16 - n * 3;				/* (centred, 6 a digit, narrowed from the font's 8) */
	for (int i = 0; i < n; i++)
	{
		const char *rows = arcade_glyph (s[i]);
		for (int r = 0; rows && r < 7; r++)
		{
			for (int c = 0; c < 5; c++)
			{
				if (rows[r * 5 + c] == '#')
				{
					int x = x0 + i * 6 + c;
					put (cell + x / 16, x % 16, 5 + r, rgb);
				}
			}
		}
	}
}

void ch_draw_atlas (uint8_t *rgba)
{
	atlas = rgba;
	memset (atlas, 0, ATLAS_W * ATLAS_H * 4);
	for (int d = 0; d < 4; d++)
	{
		pac (A_PAC (d, 0), d, 0.0f);
		pac (A_PAC (d, 1), d, 28.0f);
		pac (A_PAC (d, 2), d, 52.0f);
	}
	for (int k = 0; k < 10; k++)
	{
		pac (A_DEATH (k), CH_UP, 20.0f + 16.0f * k);
	}
	for (int k = 0; k < 8; k++)			/* the last: a little burst */
	{
		float a = k * 3.14159265f / 4;
		line (A_DEATH (10), 8 + (int) lrintf (3 * cosf (a)), 8 + (int) lrintf (3 * sinf (a)),
		      8 + (int) lrintf (6 * cosf (a)), 8 + (int) lrintf (6 * sinf (a)), 0xFFFF00);
	}
	body (A_GHOST (0), 0, 0xFFFFFF);
	body (A_GHOST (1), 1, 0xFFFFFF);
	for (int d = 0; d < 4; d++)
	{
		eyes (A_EYES (d), d);
	}
	frightened (A_FRIGHT (0), 0, 0x2121FF, 0xFFB8AE);
	frightened (A_FRIGHT (1), 1, 0x2121FF, 0xFFB8AE);
	frightened (A_FLASH (0), 0, 0xDEDEFF, 0xFF0000);
	frightened (A_FLASH (1), 1, 0xDEDEFF, 0xFF0000);
	for (int k = 0; k < 8; k++)
	{
		fruit (A_FRUIT (k), k);
	}
	static const int ghost_points[4] = {200, 400, 800, 1600};
	for (int n = 0; n < 4; n++)
	{
		score (A_GHOST_SCORE (n), ghost_points[n], 0x00FFFF);
	}
	static const int fruit_points[8] = {100, 300, 500, 700, 1000, 2000, 3000, 5000};
	for (int k = 0; k < 8; k++)
	{
		score (A_FRUIT_SCORE (k), fruit_points[k], 0xFFB8FF);
	}
}
