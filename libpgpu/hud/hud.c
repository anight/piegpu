/*
 * hud.c - text and bars over a GL ES 2.0 scene (hud.h). A 5x7 font (drawn
 * here, 2x) in an alpha texture, one blended draw from a vertex buffer.
 */
#include <string.h>
#include "gles/pgl.h"
#include "hud.h"
#include "hud_program.h"

/* ---- the font: 5x7 glyphs in 8x8 cells of a 128x32 alpha texture ---------- */

#define FONT_W		128
#define FONT_H		32

static const struct { char c; const char *rows[7]; } glyphs[] =
{
	{'0', {".###.", "#...#", "#..##", "#.#.#", "##..#", "#...#", ".###."}},
	{'1', {"..#..", ".##..", "..#..", "..#..", "..#..", "..#..", ".###."}},
	{'2', {".###.", "#...#", "....#", "...#.", "..#..", ".#...", "#####"}},
	{'3', {"#####", "...#.", "..#..", "...#.", "....#", "#...#", ".###."}},
	{'4', {"...#.", "..##.", ".#.#.", "#..#.", "#####", "...#.", "...#."}},
	{'5', {"#####", "#....", "####.", "....#", "....#", "#...#", ".###."}},
	{'6', {"..##.", ".#...", "#....", "####.", "#...#", "#...#", ".###."}},
	{'7', {"#####", "....#", "...#.", "..#..", ".#...", ".#...", ".#..."}},
	{'8', {".###.", "#...#", "#...#", ".###.", "#...#", "#...#", ".###."}},
	{'9', {".###.", "#...#", "#...#", ".####", "....#", "...#.", ".##.."}},
	{'.', {".....", ".....", ".....", ".....", ".....", ".##..", ".##.."}},
	{'%', {"##...", "##..#", "...#.", "..#..", ".#...", "#..##", "...##"}},
	{':', {".....", ".##..", ".##..", ".....", ".##..", ".##..", "....."}},
	{'-', {".....", ".....", ".....", "#####", ".....", ".....", "....."}},
	{'/', {"....#", "....#", "...#.", "..#..", ".#...", "#....", "#...."}},
	{'/', {"....#", "....#", "...#.", "..#..", ".#...", "#....", "#...."}},
	{'A', {".###.", "#...#", "#...#", "#####", "#...#", "#...#", "#...#"}},
	{'C', {".###.", "#...#", "#....", "#....", "#....", "#...#", ".###."}},
	{'D', {"####.", "#...#", "#...#", "#...#", "#...#", "#...#", "####."}},
	{'E', {"#####", "#....", "#....", "####.", "#....", "#....", "#####"}},
	{'F', {"#####", "#....", "#....", "####.", "#....", "#....", "#...."}},
	{'G', {".###.", "#...#", "#....", "#.###", "#...#", "#...#", ".####"}},
	{'I', {".###.", "..#..", "..#..", "..#..", "..#..", "..#..", ".###."}},
	{'L', {"#....", "#....", "#....", "#....", "#....", "#....", "#####"}},
	{'M', {"#...#", "##.##", "#.#.#", "#.#.#", "#...#", "#...#", "#...#"}},
	{'N', {"#...#", "#...#", "##..#", "#.#.#", "#..##", "#...#", "#...#"}},
	{'O', {".###.", "#...#", "#...#", "#...#", "#...#", "#...#", ".###."}},
	{'P', {"####.", "#...#", "#...#", "####.", "#....", "#....", "#...."}},
	{'R', {"####.", "#...#", "#...#", "####.", "#.#..", "#..#.", "#...#"}},
	{'S', {".####", "#....", "#....", ".###.", "....#", "....#", "####."}},
	{'T', {"#####", "..#..", "..#..", "..#..", "..#..", "..#..", "..#.."}},
	{'U', {"#...#", "#...#", "#...#", "#...#", "#...#", "#...#", ".###."}},
	{'W', {"#...#", "#...#", "#...#", "#.#.#", "#.#.#", "#.#.#", ".#.#."}},
	{'B', {"####.", "#...#", "#...#", "####.", "#...#", "#...#", "####."}},
	{'H', {"#...#", "#...#", "#...#", "#####", "#...#", "#...#", "#...#"}},
	{'J', {"..###", "...#.", "...#.", "...#.", "...#.", "#..#.", ".##.."}},
	{'K', {"#...#", "#..#.", "#.#..", "##...", "#.#..", "#..#.", "#...#"}},
	{'Q', {".###.", "#...#", "#...#", "#...#", "#.#.#", "#..#.", ".##.#"}},
	{'V', {"#...#", "#...#", "#...#", "#...#", "#...#", ".#.#.", "..#.."}},
	{'X', {"#...#", "#...#", ".#.#.", "..#..", ".#.#.", "#...#", "#...#"}},
	{'Y', {"#...#", "#...#", ".#.#.", "..#..", "..#..", "..#..", "..#.."}},
	{'Z', {"#####", "....#", "...#.", "..#..", ".#...", "#....", "#####"}},
	{'m', {".....", ".....", "##.#.", "#.#.#", "#.#.#", "#...#", "#...#"}},
	{'s', {".....", ".....", ".###.", "#....", ".###.", "....#", "####."}},
};
#define N_GLYPHS	(sizeof glyphs / sizeof glyphs[0])
#define SOLID		N_GLYPHS		/* a filled cell, for rectangles */

/* ---- state -------------------------------------------------------------------------- */

typedef struct { float x, y, u, v; uint8_t rgba[4]; } hud_vertex_t;

#define MAX_QUADS	160

static hud_vertex_t verts[MAX_QUADS * 6];
static unsigned n_verts, drawn_verts;
static GLuint prog, texture, buffer;
static GLint a_pos, a_uv, a_color;
static GLint u_scale, scale_w, scale_h;	/* the viewport u_scale is for */

static void cell_uv (unsigned cell, float *u0, float *v0)
{
	*u0 = (cell % 16) * 8.0f / FONT_W;
	*v0 = (cell / 16) * 8.0f / FONT_H;
}

bool hud_init (void)
{
	/* the font texture */
	static uint8_t font[FONT_W * FONT_H];
	memset (font, 0, sizeof font);
	for (unsigned g = 0; g <= N_GLYPHS; g++)
	{
		unsigned x0 = (g % 16) * 8, y0 = (g / 16) * 8;
		for (unsigned y = 0; y < 8; y++)
			for (unsigned x = 0; x < 8; x++)
				font[(y0 + y) * FONT_W + x0 + x] =
					  g == SOLID ? 255
					: y < 7 && x < 5 && glyphs[g].rows[y][x] == '#' ? 255 : 0;
	}
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
	a_uv = glGetAttribLocation (prog, "a_uv");
	a_color = glGetAttribLocation (prog, "a_color");
	u_scale = glGetUniformLocation (prog, "u_scale");
	scale_w = scale_h = 0;
	glUseProgram (prog);
	glUniform1i (glGetUniformLocation (prog, "u_font"), 0);

	glGenBuffers (1, &buffer);
	return true;
}

void hud_begin (void)
{
	n_verts = 0;
}

static void quad (float x, float y, float w, float h, float u0, float v0, float u1, float v1,
		  uint32_t rgba)
{
	if (n_verts + 6 > MAX_QUADS * 6)
	{
		return;
	}
	const hud_vertex_t c[4] =
	{
		{x, y, u0, v0, {0}}, {x + w, y, u1, v0, {0}}, {x + w, y + h, u1, v1, {0}}, {x, y + h, u0, v1, {0}},
	};
	static const int order[6] = {0, 1, 2, 0, 2, 3};
	for (int i = 0; i < 6; i++)
	{
		hud_vertex_t *v = &verts[n_verts++];
		*v = c[order[i]];
		memcpy (v->rgba, &rgba, 4);			/* little endian: r g b a */
	}
}

void hud_rect (float x, float y, float w, float h, uint32_t rgba)
{
	/* the centre of the solid cell */
	float u, v;
	cell_uv (SOLID, &u, &v);
	u += 4.0f / FONT_W;
	v += 4.0f / FONT_H;
	quad (x, y, w, h, u, v, u, v, rgba);
}

void hud_text (float x, float y, const char *text, uint32_t rgba)
{
	hud_text_scaled (x, y, text, rgba, 1.0f);
}

void hud_text_scaled (float x, float y, const char *text, uint32_t rgba, float scale)
{
	for (; *text; text++, x += HUD_CHAR_W * scale)
	{
		for (unsigned g = 0; g < N_GLYPHS; g++)
		{
			if (glyphs[g].c == *text)
			{
				float u, v;
				cell_uv (g, &u, &v);
				quad (x, y, 10.0f * scale, 14.0f * scale, u, v, u + 5.0f / FONT_W, v + 7.0f / FONT_H, rgba);
				break;
			}
		}
	}
}

void hud_end (void)
{
	glBindBuffer (GL_ARRAY_BUFFER, buffer);
	glBufferData (GL_ARRAY_BUFFER, n_verts * sizeof (hud_vertex_t), verts, GL_DYNAMIC_DRAW);
	drawn_verts = n_verts;
}

void hud_draw (void)
{
	if (!drawn_verts)
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

	glBindBuffer (GL_ARRAY_BUFFER, buffer);
	glEnableVertexAttribArray (a_pos);
	glEnableVertexAttribArray (a_uv);
	glEnableVertexAttribArray (a_color);
	glVertexAttribPointer (a_pos, 2, GL_FLOAT, GL_FALSE, sizeof (hud_vertex_t), (void *) 0);
	glVertexAttribPointer (a_uv, 2, GL_FLOAT, GL_FALSE, sizeof (hud_vertex_t), (void *) 8);
	glVertexAttribPointer (a_color, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof (hud_vertex_t), (void *) 16);
	glDrawArrays (GL_TRIANGLES, 0, drawn_verts);
	glDisableVertexAttribArray (a_color);
	glDisable (GL_BLEND);
}
