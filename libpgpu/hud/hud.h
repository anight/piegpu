/*
 * hud.h - text and bars over a GL ES 2.0 scene (pgl), in pixels from the top
 * left. Build the contents with hud_begin, hud_rect and hud_text, then
 * hud_end; hud_draw draws the last contents (every frame). The RPi keeps
 * them: built again, only what changed is sent, and where the calls are the
 * same and only the text differs (the same length), that's its characters.
 */
#ifndef HUD_H
#define HUD_H

#include <stdbool.h>
#include <stdint.h>

#define HUD_CHAR_W	12		/* advance, 2x the 5x7 font in 6x8 cells */
#define HUD_CHAR_H	16

#define HUD_RGBA(r, g, b, a)	((uint32_t) (r) | (uint32_t) (g) << 8 | (uint32_t) (b) << 16 | (uint32_t) (a) << 24)

bool hud_init (void);
void hud_begin (void);
void hud_rect (float x, float y, float w, float h, uint32_t rgba);
/* the font has digits, . % : - / A-Z and m s (other characters leave a space) */
void hud_text (float x, float y, const char *text, uint32_t rgba);
/* scale 1: as hud_text (the font 2x), 0.5: the font 1x; keep x and y whole
   pixels for sharp glyphs */
void hud_text_scaled (float x, float y, const char *text, uint32_t rgba, float scale);
void hud_end (void);
/* blended over the frame; leaves blending, depth test and culling off */
void hud_draw (void);

#endif
