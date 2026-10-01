/*
 * hud.c - text and bars over a GL ES 2.0 scene (hud.h). A 5x7 font (drawn in
 * make_font.py, shown 2x) in an alpha texture, one blended draw.
 *
 * The RPi keeps what's drawn, in two buffers: each quad's place, size and
 * colour (six vertices), and the glyph it shows (a byte a vertex: the vertex
 * shader finds the glyph's cell in the font). Here only what the RPi has of
 * each quad is remembered: a hash of the first, and the glyph. So building
 * the same contents again sends nothing, and a number that changed sends its
 * glyphs: six bytes a character. A quad is its place in the order of the
 * calls: text keeps a quad for a space too, so that a string of the same
 * length is the same quads whatever it says.
 */
#include <string.h>
#include "gles/pgl.h"
#include "hud.h"
#include "hud_program.h"
#include "hud_font.h"		/* font: 5x7 glyphs in 8x8 cells of a 128x32 alpha texture */

#define BLANK		(SOLID + 1)	/* an empty cell: a space */

/* ---- state -------------------------------------------------------------------------- */

/* a vertex of the first buffer: where, which texel of the glyph's cell, the colour */
typedef struct { float x, y; uint8_t corner[2], pad[2], rgba[4]; } hud_vertex_t;

#define MAX_QUADS	384		/* glyphs, spaces and rectangles (antigrav's results table and the numbers: 251) */

static uint32_t slot_place[MAX_QUADS];	/* a hash of the quad's vertices as the RPi has them (0: none) */
static uint8_t slot_glyph[MAX_QUADS];	/* and its glyph (0xFF: none) */
static unsigned n_quads, drawn_quads;
static GLuint prog, texture, places, glyphs;
static GLint a_pos, a_corner, a_color, a_glyph;
static GLint u_scale, scale_w, scale_h;	/* the viewport u_scale is for */

/* quads that changed, in a row, on their way to the two buffers */
#define PLACE_RUN	4
#define GLYPH_RUN	32
static hud_vertex_t place_run[PLACE_RUN * 6];
static uint8_t glyph_run[GLYPH_RUN * 6];
static unsigned place_first, place_n, glyph_first, glyph_n;

bool hud_init (void)
{
	/* the font texture */
	glGenTextures (1, &texture);
	glBindTexture (GL_TEXTURE_2D, texture);
	glPixelStorei (GL_UNPACK_ALIGNMENT, 1);
	glTexImage2D (GL_TEXTURE_2D, 0, GL_ALPHA, FONT_W, FONT_H, 0, GL_ALPHA, GL_UNSIGNED_BYTE, font);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

	/* the program */
	prog = glCreateProgram ();
	glProgramBinaryOES (prog, PGL_PROGRAM_BINARY_PGPU, &hud_info, sizeof hud_info);
	GLint linked = 0;
	glGetProgramiv (prog, GL_LINK_STATUS, &linked);
	if (!linked)
	{
		return false;
	}
	a_pos = glGetAttribLocation (prog, "a_pos");
	a_corner = glGetAttribLocation (prog, "a_corner");
	a_color = glGetAttribLocation (prog, "a_color");
	a_glyph = glGetAttribLocation (prog, "a_glyph");
	u_scale = glGetUniformLocation (prog, "u_scale");
	scale_w = scale_h = 0;
	glUseProgram (prog);
	glUniform1i (glGetUniformLocation (prog, "u_font"), 0);

	/* the two buffers, empty: nothing of them is known yet */
	glGenBuffers (1, &places);
	glBindBuffer (GL_ARRAY_BUFFER, places);
	glBufferData (GL_ARRAY_BUFFER, MAX_QUADS * 6 * sizeof (hud_vertex_t), NULL, GL_DYNAMIC_DRAW);
	glGenBuffers (1, &glyphs);
	glBindBuffer (GL_ARRAY_BUFFER, glyphs);
	glBufferData (GL_ARRAY_BUFFER, MAX_QUADS * 6, NULL, GL_DYNAMIC_DRAW);
	memset (slot_place, 0, sizeof slot_place);
	memset (slot_glyph, 0xFF, sizeof slot_glyph);
	n_quads = drawn_quads = place_n = glyph_n = 0;
	return true;
}

void hud_begin (void)
{
	n_quads = 0;
}

static void flush_places (void)
{
	if (place_n)
	{
		glBindBuffer (GL_ARRAY_BUFFER, places);
		glBufferSubData (GL_ARRAY_BUFFER, place_first * 6 * sizeof (hud_vertex_t), place_n * 6 * sizeof (hud_vertex_t),
				 place_run);
		place_n = 0;
	}
}

static void flush_glyphs (void)
{
	if (glyph_n)
	{
		glBindBuffer (GL_ARRAY_BUFFER, glyphs);
		glBufferSubData (GL_ARRAY_BUFFER, glyph_first * 6, glyph_n * 6, glyph_run);
		glyph_n = 0;
	}
}

/* the next quad: the glyph's texels (u0, v0) .. (u1, v1) of its cell over the
   rectangle. Sent only where the RPi has something else */
static void quad (float x, float y, float w, float h, unsigned glyph, unsigned u0, unsigned v0, unsigned u1, unsigned v1,
		  uint32_t rgba)
{
	if (n_quads == MAX_QUADS)
	{
		return;
	}
	unsigned slot = n_quads++;

	const float place[4] = {x, y, w, h};
	uint32_t words[6], hash = 2166136261u;			/* FNV-1a */
	memcpy (words, place, sizeof place);
	words[4] = rgba;
	words[5] = u0 | v0 << 8 | u1 << 16 | v1 << 24;
	for (unsigned i = 0; i < sizeof words; i++)
	{
		hash = (hash ^ ((const uint8_t *) words)[i]) * 16777619u;
	}
	hash = hash ? hash : 1;
	if (hash != slot_place[slot])
	{
		if (place_n && (slot != place_first + place_n || place_n == PLACE_RUN))
		{
			flush_places ();
		}
		if (!place_n)
		{
			place_first = slot;
		}
		const hud_vertex_t c[4] =
		{
			{x, y, {u0, v0}, {0}, {0}}, {x + w, y, {u1, v0}, {0}, {0}},
			{x + w, y + h, {u1, v1}, {0}, {0}}, {x, y + h, {u0, v1}, {0}, {0}},
		};
		static const int order[6] = {0, 1, 2, 0, 2, 3};
		for (int i = 0; i < 6; i++)
		{
			hud_vertex_t *v = &place_run[place_n * 6 + i];
			*v = c[order[i]];
			memcpy (v->rgba, &rgba, 4);		/* little endian: r g b a */
		}
		place_n++;
		slot_place[slot] = hash;
	}
	if (glyph != slot_glyph[slot])
	{
		if (glyph_n && (slot != glyph_first + glyph_n || glyph_n == GLYPH_RUN))
		{
			flush_glyphs ();
		}
		if (!glyph_n)
		{
			glyph_first = slot;
		}
		memset (&glyph_run[glyph_n * 6], (int) glyph, 6);
		glyph_n++;
		slot_glyph[slot] = (uint8_t) glyph;
	}
}

void hud_rect (float x, float y, float w, float h, uint32_t rgba)
{
	quad (x, y, w, h, SOLID, 4, 4, 4, 4, rgba);		/* the centre of the solid cell */
}

void hud_text (float x, float y, const char *text, uint32_t rgba)
{
	hud_text_scaled (x, y, text, rgba, 1.0f);
}

void hud_text_scaled (float x, float y, const char *text, uint32_t rgba, float scale)
{
	for (; *text; text++, x += HUD_CHAR_W * scale)
	{
		const char *g = strchr (font_chars, *text);
		quad (x, y, 10.0f * scale, 14.0f * scale, g ? (unsigned) (g - font_chars) : BLANK, 0, 0, 5, 7, rgba);
	}
}

void hud_end (void)
{
	flush_places ();
	flush_glyphs ();
	drawn_quads = n_quads;
}

void hud_draw (void)
{
	if (!drawn_quads)
	{
		return;
	}
	glUseProgram (prog);
	GLint vp[4];				/* pixels: follows the screen's size */
	glGetIntegerv (GL_VIEWPORT, vp);
	if (vp[2] != scale_w || vp[3] != scale_h)
	{
		scale_w = vp[2];
		scale_h = vp[3];
		glUniform2f (u_scale, 2.0f / vp[2], -2.0f / vp[3]);
	}
	glDisable (GL_DEPTH_TEST);
	glDisable (GL_CULL_FACE);
	glEnable (GL_BLEND);
	glBlendFunc (GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glActiveTexture (GL_TEXTURE0);
	glBindTexture (GL_TEXTURE_2D, texture);

	glEnableVertexAttribArray (a_pos);
	glEnableVertexAttribArray (a_corner);
	glEnableVertexAttribArray (a_color);
	glEnableVertexAttribArray (a_glyph);
	glBindBuffer (GL_ARRAY_BUFFER, places);
	glVertexAttribPointer (a_pos, 2, GL_FLOAT, GL_FALSE, sizeof (hud_vertex_t), (void *) 0);
	glVertexAttribPointer (a_corner, 2, GL_UNSIGNED_BYTE, GL_FALSE, sizeof (hud_vertex_t), (void *) 8);
	glVertexAttribPointer (a_color, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof (hud_vertex_t), (void *) 12);
	glBindBuffer (GL_ARRAY_BUFFER, glyphs);
	glVertexAttribPointer (a_glyph, 1, GL_UNSIGNED_BYTE, GL_FALSE, 1, (void *) 0);
	glDrawArrays (GL_TRIANGLES, 0, drawn_quads * 6);
	glDisableVertexAttribArray (a_color);
	glDisableVertexAttribArray (a_glyph);
	glDisable (GL_BLEND);
}
