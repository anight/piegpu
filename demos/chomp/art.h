/*
 * art.h - chomp's pictures (art.c): drawn by code into one RGBA atlas of
 * 16 x 16 cells, 16 a row, uploaded once
 */
#ifndef CHOMP_ART_H
#define CHOMP_ART_H

#include <stdint.h>

#define ATLAS_W		256
#define ATLAS_H		128

/* the cells */
#define A_PAC(dir, frame)	((dir) * 3 + (frame))	/* frame 0 shut, 1 half, 2 wide */
#define A_DEATH(k)		(12 + (k))		/* 0 .. 10 */
#define A_GHOST(frame)		(32 + (frame))		/* the body, white: tinted */
#define A_EYES(dir)		(34 + (dir))
#define A_FRIGHT(frame)		(38 + (frame))
#define A_FLASH(frame)		(40 + (frame))
#define A_FRUIT(kind)		(48 + (kind))
#define A_GHOST_SCORE(n)	(64 + 2 * (n))		/* 200 400 800 1600: two cells wide */
#define A_FRUIT_SCORE(kind)	(80 + 2 * (kind))	/* 100 .. 5000: two cells wide */

/* the colours (RGB), the arcade's */
extern const uint32_t ch_palette[];			/* by P_* (game.h) */
extern const uint32_t ch_ghost_color[4];

/* the atlas, RGBA8888 */
void ch_draw_atlas (uint8_t *rgba);

#endif
