/*
 * art.h - rockfall's tiles (art.c): 16 x 16, drawn by code into a 256 x 256
 * atlas of colour numbers (0 black, 1 - 3 the cave's own colours, then
 * fixed ones), 16 tiles a row
 */
#ifndef ROCKFALL_ART_H
#define ROCKFALL_ART_H

#include <stdint.h>

#define ATLAS_SIZE	256

enum					/* the colour numbers past the cave's three */
{
	K_BLACK, K_C1, K_C2, K_C3, K_WHITE, K_GREY, K_DARK, K_YELLOW, K_ORANGE, K_BLUE, K_GREEN, K_MAGENTA, K_RED,
	K_SKIN, K_COUNT
};
extern const uint32_t rf_fixed_colors[K_COUNT];

/* the tiles */
enum
{
	T_SPACE, T_DIRT, T_BRICK, T_MAGIC0 = 3, T_STEEL = 7, T_BOULDER, T_DIAMOND0, T_FIREFLY0 = 17,
	T_BUTTERFLY0 = 21, T_AMOEBA0 = 25, T_EXPLODE0 = 29, T_OUTBOX0 = 34, T_COVER = 36,
	T_MINER = 37, T_BLINK, T_TAP0, T_TAP1, T_LEFT0 = 41, T_RIGHT0 = 45, T_DIAMOND_ICON = 49,
	T_GLYPHS = 64				/* the status line's letters: 64 white, 128 yellow, by arcade_glyph's order */
};
#define T_GLYPH(i, yellow)	(T_GLYPHS + (yellow) * 64 + (i))
/* the status line's characters, in their tiles' order */
extern const char rf_glyph_chars[];

void rf_draw_atlas (uint8_t *atlas);

#endif
