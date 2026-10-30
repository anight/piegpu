/*
 * art.c - rockfall's tiles, drawn by code in the manner of the C64's (two
 * pixels wide each, the multicolour mode's): dirt in the cave's first
 * colour, speckled; brick in its second; boulders round and shaded; the
 * diamonds turning and glinting in its third; steel riveted; fireflies a
 * spinning square, butterflies flapping, the amoeba bubbling; the
 * explosion's stages; the miner standing, blinking, tapping, walking. The
 * letters for the status line from arcade_font, twice as wide.
 */
#include <math.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include "art.h"
#include "arcade_font.h"

const uint32_t rf_fixed_colors[K_COUNT] =
{
	[K_BLACK] = 0x000000, [K_WHITE] = 0xFFFFFF, [K_GREY] = 0x959595, [K_DARK] = 0x444444,
	[K_YELLOW] = 0xEEEE77, [K_ORANGE] = 0xDD8855, [K_BLUE] = 0x6C5EB5, [K_GREEN] = 0x9AD284,
	[K_MAGENTA] = 0xCC44CC, [K_RED] = 0x9A3B2B, [K_SKIN] = 0xD8A27F
};

const char rf_glyph_chars[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ<>=*-?!. ";

static uint8_t *atlas;

/* a double-wide pixel (the C64's): x 0 .. 7, y 0 .. 15 */
static void dot (int tile, int x, int y, int k)
{
	if (x < 0 || y < 0 || x >= 8 || y >= 16)
	{
		return;
	}
	int ax = (tile % 16) * 16 + x * 2, ay = (tile / 16) * 16 + y;
	atlas[ay * ATLAS_SIZE + ax] = (uint8_t) k;
	atlas[ay * ATLAS_SIZE + ax + 1] = (uint8_t) k;
}

static void fill (int tile, int k)
{
	for (int y = 0; y < 16; y++)
	{
		for (int x = 0; x < 8; x++)
		{
			dot (tile, x, y, k);
		}
	}
}

/* rows of 8 characters: '.' nothing, '1' '2' '3' the cave's colours, 'w' white, 'g' grey, 'd' dark,
   'y' yellow, 'o' orange, 'b' blue, 'n' green, 'm' magenta, 'r' red, 's' skin */
static void art (int tile, const char *rows)
{
	static const char keys[] = ".123wgdyobnmrs";
	for (int y = 0; y < 16; y++)
	{
		for (int x = 0; x < 8; x++)
		{
			const char *k = strchr (keys, rows[y * 8 + x]);
			if (k && *k != '.')
			{
				dot (tile, x, y, (int) (k - keys));
			}
		}
	}
}

static void boulder (int tile)
{
	art (tile,
	     "..2222.."
	     ".222222."
	     "22233222"
	     "22333322"
	     "22233222"
	     "22222222"
	     "22222222"
	     "22222222"
	     "22222222"
	     "22222222"
	     "22222222"
	     "22222222"
	     ".222222d"
	     ".22222dd"
	     "..ddddd."
	     "........");
}

static void diamond (int tile, int k)
{
	/* a diamond turning: its facets moved round, a glint where it catches */
	static const char *const facet[2] =
	{
		"...33..."
		"..3ww3.."
		".3w33w3."
		"3w3333w3"
		"w333333w"
		"3w3333w3"
		".3w33w3."
		"..3ww3.."
		"...33...",
		"...33..."
		"..3333.."
		".33ww33."
		"333ww333"
		"3333333w"
		"33ww3333"
		".3333w3."
		"..3w33.."
		"...33..."
	};
	const char *f = facet[k & 1];
	for (int y = 0; y < 9; y++)
	{
		for (int x = 0; x < 8; x++)
		{
			char c = f[y * 8 + x];
			if (c != '.')
			{
				dot (tile, x, y + 3, c == 'w' ? ((k / 2) % 2 ? K_C3 : K_WHITE) : K_C3);
			}
		}
	}
	if (k == 3 || k == 6)
	{
		dot (tile, 6, 2, K_WHITE);		/* (the glint) */
		dot (tile, 7, 3, K_WHITE);
	}
}

void rf_draw_atlas (uint8_t *a)
{
	atlas = a;
	memset (atlas, 0, ATLAS_SIZE * ATLAS_SIZE);
	for (int y = 0; y < 16; y++)		/* dirt: speckled */
	{
		for (int x = 0; x < 8; x++)
		{
			dot (T_DIRT, x, y, ((x * 5 + y * 3) % 7 == 0) ? K_BLACK : ((x + y * 2) % 5 == 0) ? K_C2 : K_C1);
		}
	}
	for (int y = 0; y < 16; y++)		/* brick */
	{
		for (int x = 0; x < 8; x++)
		{
			bool mortar = y % 4 == 3 || x == ((y / 4) % 2 ? 1 : 5);
			dot (T_BRICK, x, y, mortar ? K_DARK : K_C2);
		}
	}
	for (int f = 0; f < 4; f++)		/* the magic wall: brick, sparkling */
	{
		for (int y = 0; y < 16; y++)
		{
			for (int x = 0; x < 8; x++)
			{
				bool mortar = y % 4 == 3 || x == ((y / 4) % 2 ? 1 : 5);
				bool spark = !mortar && ((x * 3 + y + f * 5) % 9 == 0);
				dot (T_MAGIC0 + f, x, y, mortar ? K_DARK : spark ? K_WHITE : (y / 4 + f) % 2 ? K_C2 : K_C3);
			}
		}
	}
	for (int y = 0; y < 16; y++)		/* steel: riveted */
	{
		for (int x = 0; x < 8; x++)
		{
			bool rivet = (x == 1 || x == 6) && (y == 2 || y == 13);
			bool edge = x == 7 || y == 15;
			dot (T_STEEL, x, y, rivet ? K_WHITE : edge ? K_DARK : K_GREY);
		}
	}
	boulder (T_BOULDER);
	for (int k = 0; k < 8; k++)
	{
		diamond (T_DIAMOND0 + k, k);
	}
	for (int f = 0; f < 4; f++)		/* a firefly: a square spinning, in and out */
	{
		int r = 2 + (f % 2), ins = f < 2 ? 0 : 1;
		for (int y = 0; y < 16; y++)
		{
			for (int x = 0; x < 8; x++)
			{
				int dx = x * 2 + 1 - 8, dy = y - 8;
				int m = abs (dx) > abs (dy) ? abs (dx) : abs (dy);
				if (m >= 2 * r && m < 2 * r + 2)
				{
					dot (T_FIREFLY0 + f, x, y, ins ? K_ORANGE : K_YELLOW);
				}
				else if (m >= 2 * r - 4 && m < 2 * r - 2)
				{
					dot (T_FIREFLY0 + f, x, y, ins ? K_YELLOW : K_ORANGE);
				}
			}
		}
	}
	for (int f = 0; f < 4; f++)		/* a butterfly: its wings opening and closing */
	{
		int span = 1 + (f < 2 ? f : 3 - f) * 1;
		for (int y = 2; y < 14; y++)
		{
			for (int x = 0; x < 8; x++)
			{
				int dx = abs (x * 2 + 1 - 8), dy = abs (y - 8);
				if (dx <= 1)
				{
					dot (T_BUTTERFLY0 + f, x, y, K_WHITE);
				}
				else if (dx <= 2 + 2 * span && dy <= 5 - (dx > 4 ? 1 : 0))
				{
					dot (T_BUTTERFLY0 + f, x, y, dy < 2 ? K_C3 : K_BLUE);
				}
			}
		}
	}
	for (int f = 0; f < 4; f++)		/* the amoeba: a blob, bubbling */
	{
		for (int y = 0; y < 16; y++)
		{
			for (int x = 0; x < 8; x++)
			{
				float v = sinf ((x + f) * 1.3f) + sinf ((y + f * 2) * 0.9f);
				dot (T_AMOEBA0 + f, x, y, v > 0.8f ? K_WHITE : v > -0.6f ? K_GREEN : K_DARK);
			}
		}
	}
	for (int s = 0; s < 5; s++)		/* the explosion: a burst growing, then thinning */
	{
		for (int y = 0; y < 16; y++)
		{
			for (int x = 0; x < 8; x++)
			{
				int dx = abs (x * 2 + 1 - 8), dy = abs (y - 8), d = dx > dy ? dx : dy;
				int r = s < 3 ? 2 + 2 * s : 8;
				if (d <= r && ((x * 7 + y * 3 + s) % (s < 3 ? 2 : 3 + s)) == 0)
				{
					dot (T_EXPLODE0 + s, x, y, d < r / 2 ? K_WHITE : s < 2 ? K_YELLOW : K_ORANGE);
				}
			}
		}
	}
	for (int f = 0; f < 2; f++)		/* the inbox and the exit: steel, flashing to an open door */
	{
		for (int y = 0; y < 16; y++)
		{
			for (int x = 0; x < 8; x++)
			{
				bool rim = x == 0 || x == 7 || y == 0 || y == 15;
				dot (T_OUTBOX0 + f, x, y, f ? (rim ? K_WHITE : K_BLACK) : (rim ? K_DARK : K_GREY));
			}
		}
	}
	for (int y = 0; y < 16; y++)		/* the cover the cave starts under */
	{
		for (int x = 0; x < 8; x++)
		{
			dot (T_COVER, x, y, (x + y) % 3 == 0 ? K_DARK : K_GREY);
		}
	}
	/* the miner */
	static const char *const miner_stand =
		"..dddd.."
		".dsssss."
		".swsws.."
		".sssss.."
		"..sss..."
		"..bb...."
		".bbbbb.."
		"bbbbbbb."
		"s.bbb.s."
		"..bbb..."
		"..b.b..."
		"..b.b..."
		"..b.b..."
		".dd.dd.."
		"........"
		"........";
	art (T_MINER, miner_stand);
	art (T_BLINK, miner_stand);
	dot (T_BLINK, 2, 2, K_SKIN);
	dot (T_BLINK, 4, 2, K_SKIN);
	art (T_TAP0, miner_stand);
	art (T_TAP1, miner_stand);
	dot (T_TAP1, 4, 13, K_BLACK);
	dot (T_TAP1, 5, 13, K_BLACK);
	dot (T_TAP1, 4, 12, K_DARK);
	dot (T_TAP1, 5, 12, K_DARK);
	for (int f = 0; f < 4; f++)
	{
		static const char *const legs[4] = {"..b.b...", ".b...b..", "..bb....", ".b..b..."};
		static const char *const feet[4] = {".dd.dd..", "dd...dd.", "..ddd...", "dd..dd.."};
		for (int side = 0; side < 2; side++)
		{
			int t = (side ? T_RIGHT0 : T_LEFT0) + f;
			art (t, miner_stand);
			for (int y = 10; y < 14; y++)
			{
				for (int x = 0; x < 8; x++)
				{
					dot (t, x, y, K_BLACK);
				}
			}
			for (int y = 10; y < 13; y++)
			{
				for (int x = 0; x < 8; x++)
				{
					if (legs[f][x] == 'b')
					{
						dot (t, x, y, K_BLUE);
					}
				}
			}
			for (int x = 0; x < 8; x++)
			{
				if (feet[f][x] == 'd')
				{
					dot (t, x, 13, K_DARK);
				}
			}
			dot (t, side ? 5 : 1, 2, K_SKIN);	/* (looking the way it goes) */
			dot (t, side ? 2 : 4, 2, K_WHITE);
		}
	}
	diamond (T_DIAMOND_ICON, 0);

	/* the status line's letters: arcade_font's, twice as wide and twice as high, white and yellow */
	for (int i = 0; rf_glyph_chars[i]; i++)
	{
		const char *rows = arcade_glyph (rf_glyph_chars[i]);
		for (int yellow = 0; yellow < 2; yellow++)
		{
			int t = T_GLYPH (i, yellow);
			for (int r = 0; rows && r < 7; r++)
			{
				for (int c = 0; c < 5; c++)
				{
					if (rows[r * 5 + c] == '#')
					{
						int ax = (t % 16) * 16 + 2 + c * 2, ay = (t / 16) * 16 + 1 + r * 2;
						for (int k = 0; k < 4; k++)
						{
							atlas[(ay + k / 2) * ATLAS_SIZE + ax + k % 2] = yellow ? K_YELLOW : K_WHITE;
						}
					}
				}
			}
		}
	}
}
