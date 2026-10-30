/*
 * arcade_font.h - the old games' font, drawn here: 5 x 7 pixels in 8 x 8
 * cells, the digits, A-Z and < > = * - ? ! . (the invaders, chomp)
 */
#ifndef ARCADE_FONT_H
#define ARCADE_FONT_H

/* the glyph's 7 rows of 5 characters ('#' a pixel), or NULL (a space, or none) */
const char *arcade_glyph (char c);

#endif
