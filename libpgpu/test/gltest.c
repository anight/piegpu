/*
 * gltest.c - self test 8: the GL API (gles/pgl.h), only gl* calls.
 *
 * Draws a test pattern (rows bottom to top):
 *	y  10 .. 60	programs: client arrays, uniforms by name, constant attribute,
 *			buffer + client indices, texture (RGB, unpack alignment 1),
 *			buffer + client arrays with first > 0
 *	y  70 .. 120	programs: index buffer + client arrays, 70000-vertex strip and list
 *			(split into several draws), a line
 *	y 130 .. 180	GL ES 1.1: interleaved client arrays, client indices, lighting,
 *			texture
 *	y 185 .. 235	framebuffer object (texture + depth renderbuffer), glCopyTexImage2D,
 *			control flow (loops, dynamic uniform indexing, discard)
 * and checks pixels (glReadPixels), state (glGet*) and errors (glGetError).
 */
#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "gles/pgl.h"
#include "pgpu.h"
#include "gltest_program.h"
#include "ctrl_program.h"
#include "builtin_program.h"

static unsigned checks, failures;

#define CHECK(cond, ...) \
	do { \
		checks++; \
		if (!(cond)) \
		{ \
			failures++; \
			printf ("gltest: FAIL line %d: ", __LINE__); \
			printf (__VA_ARGS__); \
			printf ("\n"); \
		} \
	} while (0)

static void ortho (float *m, float l, float r, float b, float t)
{
	memset (m, 0, 16 * sizeof (float));
	m[0] = 2.0f / (r - l);
	m[5] = 2.0f / (t - b);
	m[10] = -1.0f;
	m[12] = -(r + l) / (r - l);
	m[13] = -(t + b) / (t - b);
	m[15] = 1.0f;
}

static uint32_t pixel (int x, int y)
{
	uint8_t p[4] = {0};
	glReadPixels (x, y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, p);
	return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t) p[3] << 24;
}

/* colours within tol per channel (the panel is RGB565) */
static bool near (uint32_t c, int r, int g, int b, int a, int tol)
{
	int v[4] = {r, g, b, a};
	for (int i = 0; i < 4; i++)
	{
		int d = (int) ((c >> (i * 8)) & 0xFF) - v[i];
		if (d < -tol || d > tol)
		{
			return false;
		}
	}
	return true;
}

#define CHECK_PIXEL(x, y, r, g, b, a, tol) \
	do { \
		uint32_t c_ = pixel (x, y); \
		CHECK (near (c_, r, g, b, a, tol), "pixel (%d, %d) = %08lx, expected (%d, %d, %d, %d)", \
		       x, y, (unsigned long) c_, r, g, b, a); \
	} while (0)

#define CHECK_ERROR(expected, what) \
	do { \
		GLenum e_ = glGetError (); \
		CHECK (e_ == (expected), "%s: error %04x, expected %04x", what, e_, (unsigned) (expected)); \
	} while (0)

#define MAX_TEST_TEXTURE	128	/* an RPi texture id pgl doesn't use here */

struct pc_vertex { float x, y; uint8_t rgba[4]; };

/* a buffer of `count` 2D float vertices at (0, 0), then the given ones */
static void big_buffer (GLuint buffer, uint32_t count, const float *last, uint32_t n_last)
{
	static const float zeros[1024] = {0};
	uint32_t bytes = count * 8, tail = n_last * 8;
	glBindBuffer (GL_ARRAY_BUFFER, buffer);
	glBufferData (GL_ARRAY_BUFFER, bytes, NULL, GL_STATIC_DRAW);
	for (uint32_t offset = 0; offset < bytes - tail; offset += sizeof zeros)
	{
		uint32_t n = bytes - tail - offset < sizeof zeros ? bytes - tail - offset : sizeof zeros;
		glBufferSubData (GL_ARRAY_BUFFER, offset, n, zeros);
	}
	glBufferSubData (GL_ARRAY_BUFFER, bytes - tail, tail, last);
}
struct ff_vertex { int16_t x, y; uint8_t rgba[4]; };

unsigned gl_self_test (void)
{
	checks = failures = 0;
	if (!pglInit ())
	{
		printf ("gltest: no INFO reply to RESET\n");
	}

	/* ---- state and errors ---- */
	GLint v[4];
	glGetIntegerv (GL_VIEWPORT, v);
	CHECK (v[0] == 0 && v[1] == 0 && v[2] == 320 && v[3] == 240, "GL_VIEWPORT %ld %ld %ld %ld",
	       (long) v[0], (long) v[1], (long) v[2], (long) v[3]);
	glGetIntegerv (GL_MAX_TEXTURE_SIZE, v);
	CHECK (v[0] == 2048, "GL_MAX_TEXTURE_SIZE %ld", (long) v[0]);
	glGetIntegerv (GL_RED_BITS, v);
	glGetIntegerv (GL_GREEN_BITS, v + 1);
	glGetIntegerv (GL_DEPTH_BITS, v + 2);
	glGetIntegerv (GL_STENCIL_BITS, v + 3);
	CHECK (v[0] == 5 && v[1] == 6 && v[2] == 24 && v[3] == 8, "bits %ld %ld %ld %ld",
	       (long) v[0], (long) v[1], (long) v[2], (long) v[3]);
	GLboolean compiler = GL_TRUE;		/* none on the Pico; on a PC, glslc */
	glGetBooleanv (GL_SHADER_COMPILER, &compiler);
	printf ("gltest: shader compiler: %s\n", compiler ? "yes" : "no");
	CHECK_ERROR (GL_NO_ERROR, "initial");
	glEnable (0x1234);
	CHECK_ERROR (GL_INVALID_ENUM, "glEnable (0x1234)");
	CHECK (glIsEnabled (GL_DITHER), "GL_DITHER is enabled by default");
	glDisable (GL_DITHER);			/* exact colours for the pixel checks */
	glEnable (GL_CULL_FACE);
	CHECK (glIsEnabled (GL_CULL_FACE), "glIsEnabled");
	glDisable (GL_CULL_FACE);
	glClearColor (0.25f, 0.5f, 0.75f, 1.0f);
	glGetIntegerv (GL_COLOR_CLEAR_VALUE, v);
	CHECK (v[1] > 1073000000 && v[1] < 1074000000, "GL_COLOR_CLEAR_VALUE as integer %ld", (long) v[1]);
	printf ("gltest: %s / %s / %s\n", glGetString (GL_VENDOR), glGetString (GL_RENDERER),
		glGetString (GL_VERSION));

	glClearColor (0.1f, 0.1f, 0.2f, 1.0f);
	glClear (GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

	/* ---- programs ---- */
	/* GL ES 2.0 without a compiler: glShaderBinary into both shaders, link */
	glGetIntegerv (GL_NUM_SHADER_BINARY_FORMATS, v);
	glGetIntegerv (GL_SHADER_BINARY_FORMATS, v + 1);
	CHECK (v[0] == 1 && v[1] == PGL_SHADER_BINARY_PGPU, "shader binary formats %ld %lx",
	       (long) v[0], (unsigned long) v[1]);
	GLuint shaders[2] = {glCreateShader (GL_VERTEX_SHADER), glCreateShader (GL_FRAGMENT_SHADER)};
	GLuint prog = glCreateProgram ();
	glAttachShader (prog, shaders[0]);
	glAttachShader (prog, shaders[1]);
	glLinkProgram (prog);
	GLint linked = 1;
	glGetProgramiv (prog, GL_LINK_STATUS, &linked);
	CHECK (!linked, "glLinkProgram without binaries linked");
	glShaderBinary (2, shaders, 0x1234, &gltest_info, sizeof gltest_info);
	CHECK_ERROR (GL_INVALID_ENUM, "glShaderBinary with a bad format");
	glShaderBinary (2, shaders, PGL_SHADER_BINARY_PGPU, &gltest_info, sizeof gltest_info);
	glLinkProgram (prog);
	glGetProgramiv (prog, GL_LINK_STATUS, &linked);
	CHECK (linked, "glShaderBinary + glLinkProgram: not linked");
	glGetShaderiv (shaders[0], GL_COMPILE_STATUS, v);
	CHECK_ERROR (compiler ? GL_NO_ERROR : GL_INVALID_OPERATION, "GL_COMPILE_STATUS");
	glDeleteShader (shaders[0]);
	glDeleteShader (shaders[1]);
	glUseProgram (prog);

	/* the same with GL_OES_get_program_binary */
	GLuint prog2 = glCreateProgram ();
	glProgramBinaryOES (prog2, PGL_PROGRAM_BINARY_PGPU, &gltest_info, sizeof gltest_info);
	glGetProgramiv (prog2, GL_LINK_STATUS, &linked);
	CHECK (linked, "glProgramBinaryOES: not linked");
	glDeleteProgram (prog2);
	CHECK_ERROR (GL_NO_ERROR, "program loading");

	GLint l_mvp = glGetUniformLocation (prog, "u_mvp");
	GLint l_offset0 = glGetUniformLocation (prog, "u_offset");
	GLint l_offset1 = glGetUniformLocation (prog, "u_offset[1]");
	GLint l_which = glGetUniformLocation (prog, "u_which");
	GLint l_tint = glGetUniformLocation (prog, "u_tint");
	GLint l_tex = glGetUniformLocation (prog, "u_tex");
	GLint l_use_tex = glGetUniformLocation (prog, "u_use_tex");
	GLint a_pos = glGetAttribLocation (prog, "a_pos");
	GLint a_uv = glGetAttribLocation (prog, "a_uv");
	GLint a_color = glGetAttribLocation (prog, "a_color");
	CHECK (l_mvp >= 0 && l_offset0 >= 0 && l_offset1 >= 0 && l_which >= 0 && l_tint >= 0
	       && l_tex >= 0 && l_use_tex >= 0, "uniform locations");
	CHECK (glGetUniformLocation (prog, "u_offset[2]") == -1 && glGetUniformLocation (prog, "nope") == -1,
	       "invalid uniform names");
	CHECK (a_pos == 0 && a_uv == 1 && a_color == 2, "attribute locations %ld %ld %ld",
	       (long) a_pos, (long) a_uv, (long) a_color);
	GLint n_uniforms = 0;
	glGetProgramiv (prog, GL_ACTIVE_UNIFORMS, &n_uniforms);
	CHECK (n_uniforms == 6, "GL_ACTIVE_UNIFORMS %ld", (long) n_uniforms);
	char name[32] = "";
	GLint size = 0;
	GLenum type = 0;
	for (GLint i = 0; i < n_uniforms; i++)
	{
		glGetActiveUniform (prog, i, sizeof name, NULL, &size, &type, name);
		if (strncmp (name, "u_offset", 8) == 0)
		{
			break;
		}
	}
	CHECK (strcmp (name, "u_offset[0]") == 0 && size == 2 && type == GL_FLOAT_VEC2,
	       "glGetActiveUniform %s %ld %04x", name, (long) size, (unsigned) type);

	float mvp[16];
	ortho (mvp, 0, 320, 0, 240);
	glUniformMatrix4fv (l_mvp, 1, GL_FALSE, mvp);
	float offsets[4] = {0, 0, 60, 0};
	glUniform2fv (l_offset0, 2, offsets);
	glUniform1i (l_which, 0);
	glUniform4f (l_tint, 1, 1, 1, 1);
	glUniform1i (l_use_tex, 0);
	glUniform1i (l_tex, 0);
	float got[16];
	glGetUniformfv (prog, l_offset1, got);
	CHECK (got[0] == 60.0f && got[1] == 0.0f, "glGetUniformfv u_offset[1] %f %f", got[0], got[1]);
	CHECK_ERROR (GL_NO_ERROR, "uniforms");

	/* A: client arrays, red */
	static const float quad[8] = {10, 10, 60, 10, 60, 60, 10, 60};
	static const uint8_t red4[16] = {255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255};
	glVertexAttribPointer (a_pos, 2, GL_FLOAT, GL_FALSE, 0, quad);
	glVertexAttribPointer (a_color, 4, GL_UNSIGNED_BYTE, GL_TRUE, 0, red4);
	glEnableVertexAttribArray (a_pos);
	glEnableVertexAttribArray (a_color);
	glDrawArrays (GL_TRIANGLE_FAN, 0, 4);

	/* B: the same quad moved by u_offset[1] (int uniform), constant green attribute */
	glUniform1i (l_which, 1);
	glDisableVertexAttribArray (a_color);
	glVertexAttrib4f (a_color, 0, 1, 0, 1);
	glDrawArrays (GL_TRIANGLE_FAN, 0, 4);
	glUniform1i (l_which, 0);
	glEnableVertexAttribArray (a_color);

	/* C: vertex buffer (interleaved), client indices, blue by the tint */
	static const struct pc_vertex c_vertices[4] = {
		{130, 10, {255, 255, 255, 255}}, {180, 10, {255, 255, 255, 255}},
		{180, 60, {255, 255, 255, 255}}, {130, 60, {255, 255, 255, 255}}};
	static const uint8_t c_indices[6] = {0, 1, 2, 0, 2, 3};
	GLuint buffers[3];
	glGenBuffers (3, buffers);
	glBindBuffer (GL_ARRAY_BUFFER, buffers[0]);
	glBufferData (GL_ARRAY_BUFFER, sizeof c_vertices, c_vertices, GL_STATIC_DRAW);
	glVertexAttribPointer (a_pos, 2, GL_FLOAT, GL_FALSE, sizeof (struct pc_vertex), (void *) 0);
	glVertexAttribPointer (a_color, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof (struct pc_vertex), (void *) 8);
	glBindBuffer (GL_ARRAY_BUFFER, 0);
	glUniform4f (l_tint, 0, 0, 1, 1);
	glDrawElements (GL_TRIANGLES, 6, GL_UNSIGNED_BYTE, c_indices);
	glUniform4f (l_tint, 1, 1, 1, 1);

	/* D: texture 3x2 RGB, rows of 9 bytes (unpack alignment 1) */
	static const uint8_t texels[18] = {
		255, 0, 0,  0, 255, 0,  0, 0, 255,		/* bottom row: red, green, blue */
		255, 255, 255,  255, 255, 0,  255, 0, 255};	/* top row: white, yellow, magenta */
	GLuint textures[3];
	glGenTextures (3, textures);
	glActiveTexture (GL_TEXTURE0);
	glBindTexture (GL_TEXTURE_2D, textures[0]);
	glPixelStorei (GL_UNPACK_ALIGNMENT, 1);
	glTexImage2D (GL_TEXTURE_2D, 0, GL_RGB, 3, 2, 0, GL_RGB, GL_UNSIGNED_BYTE, texels);
	glPixelStorei (GL_UNPACK_ALIGNMENT, 4);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	static const float d_pos[8] = {190, 10, 250, 10, 250, 60, 190, 60};
	static const float d_uv[8] = {0, 0, 1, 0, 1, 1, 0, 1};
	static const uint8_t white4[16] = {255, 255, 255, 255, 255, 255, 255, 255,
					   255, 255, 255, 255, 255, 255, 255, 255};
	glVertexAttribPointer (a_pos, 2, GL_FLOAT, GL_FALSE, 0, d_pos);
	glVertexAttribPointer (a_uv, 2, GL_FLOAT, GL_FALSE, 0, d_uv);
	glVertexAttribPointer (a_color, 4, GL_UNSIGNED_BYTE, GL_TRUE, 0, white4);
	glEnableVertexAttribArray (a_uv);
	glUniform1i (l_use_tex, 1);
	glDrawArrays (GL_TRIANGLE_FAN, 0, 4);
	glUniform1i (l_use_tex, 0);
	glDisableVertexAttribArray (a_uv);

	/* E: positions from a buffer, colours client-side, first = 4 */
	static const float e_pos[16] = {0, 0, 1, 0, 1, 1, 0, 1,  260, 10, 310, 10, 310, 60, 260, 60};
	static const uint8_t e_color[32] = {
		0, 0, 0, 255, 0, 0, 0, 255, 0, 0, 0, 255, 0, 0, 0, 255,
		255, 255, 0, 255, 255, 255, 0, 255, 255, 255, 0, 255, 255, 255, 0, 255};
	glBindBuffer (GL_ARRAY_BUFFER, buffers[1]);
	glBufferData (GL_ARRAY_BUFFER, sizeof e_pos, e_pos, GL_STATIC_DRAW);
	glVertexAttribPointer (a_pos, 2, GL_FLOAT, GL_FALSE, 0, (void *) 0);
	glBindBuffer (GL_ARRAY_BUFFER, 0);
	glVertexAttribPointer (a_color, 4, GL_UNSIGNED_BYTE, GL_TRUE, 0, e_color);
	glDrawArrays (GL_TRIANGLE_FAN, 4, 4);

	/* F: index buffer, client arrays, magenta */
	static const float f_pos[10] = {0, 0, 10, 70, 60, 70, 60, 120, 10, 120};
	static const uint8_t magenta5[20] = {0, 0, 0, 0, 255, 0, 255, 255, 255, 0, 255, 255,
					     255, 0, 255, 255, 255, 0, 255, 255};
	static const uint16_t f_indices[6] = {1, 2, 3, 1, 3, 4};
	glBindBuffer (GL_ELEMENT_ARRAY_BUFFER, buffers[2]);
	glBufferData (GL_ELEMENT_ARRAY_BUFFER, sizeof f_indices, f_indices, GL_STATIC_DRAW);
	glVertexAttribPointer (a_pos, 2, GL_FLOAT, GL_FALSE, 0, f_pos);
	glVertexAttribPointer (a_color, 4, GL_UNSIGNED_BYTE, GL_TRUE, 0, magenta5);
	glDrawElements (GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, (void *) 0);
	glBindBuffer (GL_ELEMENT_ARRAY_BUFFER, 0);

	/* more vertices than the RPi takes per draw (65535): degenerate
	   triangles at (0, 0), then a quad; a strip (split with overlap) and a list */
	GLuint big[2];
	glGenBuffers (2, big);
	static const float strip_end[12] = {70, 70, 70, 70,	/* degenerate joins */
					    70, 70, 120, 70, 70, 120, 120, 120};
	static const float list_end[12] = {130, 70, 180, 70, 180, 120, 130, 70, 180, 120, 130, 120};
	big_buffer (big[0], 70000, strip_end, 6);
	big_buffer (big[1], 70002, list_end, 6);
	glDisableVertexAttribArray (a_color);
	glVertexAttrib4f (a_color, 0.5f, 1, 0.5f, 1);
	glBindBuffer (GL_ARRAY_BUFFER, big[0]);
	glVertexAttribPointer (a_pos, 2, GL_FLOAT, GL_FALSE, 0, (void *) 0);
	glDrawArrays (GL_TRIANGLE_STRIP, 0, 70000);
	glVertexAttrib4f (a_color, 1, 0.5f, 0.5f, 1);
	glBindBuffer (GL_ARRAY_BUFFER, big[1]);
	glVertexAttribPointer (a_pos, 2, GL_FLOAT, GL_FALSE, 0, (void *) 0);
	glDrawArrays (GL_TRIANGLES, 0, 70002);
	glBindBuffer (GL_ARRAY_BUFFER, 0);

	/* a line (the program's lines variant), white */
	static const float line[4] = {190, 90.5f, 250, 90.5f};
	glVertexAttrib4f (a_color, 1, 1, 1, 1);
	glVertexAttribPointer (a_pos, 2, GL_FLOAT, GL_FALSE, 0, line);
	glDrawArrays (GL_LINES, 0, 2);
	glEnableVertexAttribArray (a_color);
	CHECK_ERROR (GL_NO_ERROR, "program draws");

	/* ---- GL ES 1.1 fixed function ---- */
	glUseProgram (0);
	glMatrixMode (GL_PROJECTION);
	glLoadIdentity ();
	glOrthof (0, 320, 0, 240, -1, 1);
	glMatrixMode (GL_MODELVIEW);
	glLoadIdentity ();
	glEnableClientState (GL_VERTEX_ARRAY);
	glEnableClientState (GL_COLOR_ARRAY);

	/* a diamond: interleaved short positions and colours, rotated */
	static const struct ff_vertex diamond[4] = {
		{-18, -18, {200, 200, 200, 255}}, {18, -18, {200, 200, 200, 255}},
		{18, 18, {200, 200, 200, 255}}, {-18, 18, {200, 200, 200, 255}}};
	glPushMatrix ();
	glTranslatef (35, 155, 0);
	glRotatef (45, 0, 0, 1);
	glVertexPointer (2, GL_SHORT, sizeof (struct ff_vertex), &diamond[0].x);
	glColorPointer (4, GL_UNSIGNED_BYTE, sizeof (struct ff_vertex), diamond[0].rgba);
	glDrawArrays (GL_TRIANGLE_FAN, 0, 4);
	glPopMatrix ();
	glGetIntegerv (GL_MODELVIEW_STACK_DEPTH, v);
	CHECK (v[0] == 1, "GL_MODELVIEW_STACK_DEPTH %ld", (long) v[0]);

	/* client indices, cyan */
	static const float sq_pos[18] = {0, 0, 0,  70, 130, 0,  120, 130, 0,  120, 180, 0,  70, 180, 0,  0, 0, 0};
	static const uint8_t cyan6[24] = {0, 0, 0, 0, 0, 255, 255, 255, 0, 255, 255, 255,
					  0, 255, 255, 255, 0, 255, 255, 255, 0, 0, 0, 0};
	static const uint8_t sq_indices[6] = {1, 2, 3, 1, 3, 4};
	glVertexPointer (3, GL_FLOAT, 0, sq_pos);
	glColorPointer (4, GL_UNSIGNED_BYTE, 0, cyan6);
	glDrawElements (GL_TRIANGLES, 6, GL_UNSIGNED_BYTE, sq_indices);
	glDisableClientState (GL_COLOR_ARRAY);

	/* lighting: directional light 0 (default), material (1, 0.5, 0) */
	static const float lit_pos[8] = {130, 130, 180, 130, 180, 180, 130, 180};
	static const float orange[4] = {1, 0.5f, 0, 1};
	glEnable (GL_LIGHTING);
	glEnable (GL_LIGHT0);
	glMaterialfv (GL_FRONT_AND_BACK, GL_AMBIENT_AND_DIFFUSE, orange);
	glNormal3f (0, 0, 1);
	glVertexPointer (2, GL_FLOAT, 0, lit_pos);
	glDrawArrays (GL_TRIANGLE_FAN, 0, 4);
	glDisable (GL_LIGHTING);

	/* texture (unit 0, REPLACE) */
	static const float tex_pos[8] = {190, 130, 250, 130, 250, 180, 190, 180};
	glEnable (GL_TEXTURE_2D);
	glTexEnvi (GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
	glEnableClientState (GL_TEXTURE_COORD_ARRAY);
	glVertexPointer (2, GL_FLOAT, 0, tex_pos);
	glTexCoordPointer (2, GL_FLOAT, 0, d_uv);
	glDrawArrays (GL_TRIANGLE_FAN, 0, 4);
	glDisableClientState (GL_TEXTURE_COORD_ARRAY);
	glDisable (GL_TEXTURE_2D);
	CHECK_ERROR (GL_NO_ERROR, "fixed-function draws");

	/* ---- framebuffer object: texture + depth renderbuffer ---- */
	GLuint fbo, rb;
	glGenFramebuffers (1, &fbo);
	glGenRenderbuffers (1, &rb);
	glBindTexture (GL_TEXTURE_2D, textures[1]);
	glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA, 64, 64, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glBindRenderbuffer (GL_RENDERBUFFER, rb);
	glRenderbufferStorage (GL_RENDERBUFFER, GL_DEPTH_COMPONENT16, 64, 64);
	glBindFramebuffer (GL_FRAMEBUFFER, fbo);
	CHECK (glCheckFramebufferStatus (GL_FRAMEBUFFER) == GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT,
	       "empty framebuffer status");
	glFramebufferTexture2D (GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, textures[1], 0);
	glFramebufferRenderbuffer (GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, rb);
	GLenum status = glCheckFramebufferStatus (GL_FRAMEBUFFER);
	CHECK (status == GL_FRAMEBUFFER_COMPLETE, "framebuffer status %04x", (unsigned) status);
	glGetIntegerv (GL_ALPHA_BITS, v);
	glGetIntegerv (GL_DEPTH_BITS, v + 1);
	glGetIntegerv (GL_STENCIL_BITS, v + 2);
	CHECK (v[0] == 8 && v[1] == 24 && v[2] == 0, "framebuffer bits %ld %ld %ld",
	       (long) v[0], (long) v[1], (long) v[2]);
	glViewport (0, 0, 64, 64);
	glClearColor (0, 0.5f, 0, 1);
	glClear (GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	glUseProgram (prog);
	ortho (mvp, 0, 64, 0, 64);
	glUniformMatrix4fv (l_mvp, 1, GL_FALSE, mvp);
	static const float fb_quad[8] = {16, 16, 48, 16, 48, 48, 16, 48};
	glVertexAttribPointer (a_pos, 2, GL_FLOAT, GL_FALSE, 0, fb_quad);
	glVertexAttribPointer (a_color, 4, GL_UNSIGNED_BYTE, GL_TRUE, 0, red4);
	glEnable (GL_DEPTH_TEST);
	glDrawArrays (GL_TRIANGLE_FAN, 0, 4);
	glDisable (GL_DEPTH_TEST);
	CHECK_PIXEL (32, 32, 255, 0, 0, 255, 0);
	CHECK_PIXEL (2, 2, 0, 128, 0, 255, 0);

	/* back to the panel: show the texture with the fixed-function pipeline */
	glBindFramebuffer (GL_FRAMEBUFFER, 0);
	glViewport (0, 0, 320, 240);
	glUseProgram (0);
	static const float show_pos[8] = {10, 185, 60, 185, 60, 235, 10, 235};
	glEnable (GL_TEXTURE_2D);
	glEnableClientState (GL_TEXTURE_COORD_ARRAY);
	glVertexPointer (2, GL_FLOAT, 0, show_pos);
	glTexCoordPointer (2, GL_FLOAT, 0, d_uv);
	glDrawArrays (GL_TRIANGLE_FAN, 0, 4);

	/* copy 64x64 of the panel from (190, 10) (quad D) into a texture, show it */
	glBindTexture (GL_TEXTURE_2D, textures[2]);
	glCopyTexImage2D (GL_TEXTURE_2D, 0, GL_RGB, 190, 10, 64, 64, 0);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	static const float copy_pos[8] = {70, 185, 120, 185, 120, 235, 70, 235};
	glVertexPointer (2, GL_FLOAT, 0, copy_pos);
	glDrawArrays (GL_TRIANGLE_FAN, 0, 4);
	glDisableClientState (GL_TEXTURE_COORD_ARRAY);
	glDisable (GL_TEXTURE_2D);
	CHECK_ERROR (GL_NO_ERROR, "framebuffer object");

	/* ---- control flow: a loop with break (uniform bound), dynamic indexing
	   of a uniform array (read by the TMU), discard, a per-pixel loop ---- */
	GLuint cs[2] = {glCreateShader (GL_VERTEX_SHADER), glCreateShader (GL_FRAGMENT_SHADER)};
	glShaderBinary (2, cs, PGL_SHADER_BINARY_PGPU, &ctrl_info, sizeof ctrl_info);
	GLuint ctrl = glCreateProgram ();
	glAttachShader (ctrl, cs[0]);
	glAttachShader (ctrl, cs[1]);
	glLinkProgram (ctrl);
	glGetProgramiv (ctrl, GL_LINK_STATUS, &linked);
	CHECK (linked, "control flow program: not linked");
	glUseProgram (ctrl);
	ortho (mvp, 0, 320, 0, 240);
	glUniformMatrix4fv (glGetUniformLocation (ctrl, "u_mvp"), 1, GL_FALSE, mvp);
	float k[32];
	for (int i = 0; i < 8; i++)
	{
		k[i * 4] = 0.1f;
		k[i * 4 + 1] = i / 7.0f;
		k[i * 4 + 2] = 1.0f - i / 7.0f;
		k[i * 4 + 3] = 1.0f;
	}
	glUniform4fv (glGetUniformLocation (ctrl, "u_k"), 8, k);
	GLint l_n = glGetUniformLocation (ctrl, "u_n"), l_sel = glGetUniformLocation (ctrl, "u_sel");
	GLint l_t = glGetUniformLocation (ctrl, "u_t"), l_mix = glGetUniformLocation (ctrl, "u_mix");
	static const struct { int n, sel; float t, mix; } params[4] = {
		{3, 2, 0, 0},		/* x 0.3, (y, z) from u_k[2] */
		{8, 6, 0, 0},		/* x 0.8, (y, z) from u_k[6] */
		{1, 2, 0.5f, 0},	/* x 0.1 < 0.5: discarded */
		{3, 2, 0, 1}};		/* z = (x % 8 + 1) / 8 per pixel */
	for (int q = 0; q < 4; q++)
	{
		float x0 = 130 + q * 50;
		float quad_q[8] = {x0, 185, x0 + 40, 185, x0 + 40, 235, x0, 235};
		glUniform1i (l_n, params[q].n);
		glUniform1i (l_sel, params[q].sel);
		glUniform1f (l_t, params[q].t);
		glUniform1f (l_mix, params[q].mix);
		glVertexAttribPointer (0, 2, GL_FLOAT, GL_FALSE, 0, quad_q);
		glDrawArrays (GL_TRIANGLE_FAN, 0, 4);
	}
	static const float centre_quad[8] = {260, 70, 310, 70, 310, 120, 260, 120};
	glUniform1f (l_mix, 2.0f);			/* fract (gl_FragCoord.xy) */
	glVertexAttribPointer (0, 2, GL_FLOAT, GL_FALSE, 0, centre_quad);
	glDrawArrays (GL_TRIANGLE_FAN, 0, 4);
	glDeleteShader (cs[0]);
	glDeleteShader (cs[1]);
	CHECK_ERROR (GL_NO_ERROR, "control flow");

	/* ---- built-ins on the panel and in a framebuffer object: gl_PointCoord
	   (origin upper left), gl_FragCoord (pixel centres, y up) ---- */
	GLuint bs[2] = {glCreateShader (GL_VERTEX_SHADER), glCreateShader (GL_FRAGMENT_SHADER)};
	glShaderBinary (2, bs, PGL_SHADER_BINARY_PGPU, &builtin_info, sizeof builtin_info);
	GLuint bprog = glCreateProgram ();
	glAttachShader (bprog, bs[0]);
	glAttachShader (bprog, bs[1]);
	glLinkProgram (bprog);
	glUseProgram (bprog);
	GLint l_rect = glGetUniformLocation (bprog, "u_rect"), l_mode = glGetUniformLocation (bprog, "u_mode");
	glUniform1f (glGetUniformLocation (bprog, "u_point_size"), 32.0f);
	static const float origin[2] = {0, 0};
	glVertexAttribPointer (0, 2, GL_FLOAT, GL_FALSE, 0, origin);
	glUniform4f (l_rect, 285 / 160.0f - 1, 155 / 120.0f - 1, 0, 0);	/* a point at (285, 155) */
	glUniform1f (l_mode, 1.0f);
	glDrawArrays (GL_POINTS, 0, 1);
	glDeleteShader (bs[0]);
	glDeleteShader (bs[1]);

	glBindFramebuffer (GL_FRAMEBUFFER, fbo);
	glViewport (0, 0, 64, 64);
	glClearColor (0, 0, 0, 1);
	glClear (GL_COLOR_BUFFER_BIT);
	glUniform4f (l_rect, -1, -1, 2, 2);		/* the whole target: gl_FragCoord */
	glUniform1f (l_mode, 0.0f);
	static const float unit_quad[8] = {0, 0, 1, 0, 1, 1, 0, 1};
	glVertexAttribPointer (0, 2, GL_FLOAT, GL_FALSE, 0, unit_quad);
	glDrawArrays (GL_TRIANGLE_FAN, 0, 4);
	CHECK_PIXEL (40, 10, 32, 11, 0, 255, 1);		/* (40.5 / 320, 10.5 / 240) */
	glUniform4f (l_rect, 0, 0, 0, 0);			/* a point at (32, 32): x, y 16 .. 48 */
	glUniform1f (l_mode, 1.0f);
	glVertexAttribPointer (0, 2, GL_FLOAT, GL_FALSE, 0, origin);
	glDrawArrays (GL_POINTS, 0, 1);
	CHECK_PIXEL (20, 44, 36, 28, 0, 255, 3);		/* near its top left: s 0.14, t 0.11 */
	CHECK_PIXEL (20, 20, 36, 219, 0, 255, 3);		/* bottom left: t 0.86 */
	glBindFramebuffer (GL_FRAMEBUFFER, 0);
	glViewport (0, 0, 320, 240);
	CHECK_ERROR (GL_NO_ERROR, "built-ins");

	/* ---- a target without alpha, texture object 0, glTexSubImage2D with
	   another type, a depth-only framebuffer sharing a depth renderbuffer ---- */
	{
		GLuint t2[2], fbs[3], rbd;
		glGenTextures (2, t2);
		glGenFramebuffers (3, fbs);
		glGenRenderbuffers (1, &rbd);
		for (int i = 0; i < 2; i++)
		{
			glBindTexture (GL_TEXTURE_2D, t2[i]);
			GLenum f = i ? GL_RGBA : GL_RGB;
			glTexImage2D (GL_TEXTURE_2D, 0, f, 16, 16, 0, f, GL_UNSIGNED_BYTE, NULL);
			glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
			glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		}
		glUseProgram (prog);
		ortho (mvp, -1, 1, -1, 1);
		glUniformMatrix4fv (l_mvp, 1, GL_FALSE, mvp);
		glUniform1i (l_which, 0);
		float zero[4] = {0, 0, 0, 0};
		glUniform2fv (l_offset0, 2, zero);
		glUniform4f (l_tint, 1, 1, 1, 1);
		glUniform1i (l_use_tex, 0);
		static const float full[8] = {-1, -1, 1, -1, 1, 1, -1, 1};
		static const float uv01[8] = {0, 0, 1, 0, 1, 1, 0, 1};
		glVertexAttribPointer (a_pos, 2, GL_FLOAT, GL_FALSE, 0, full);
		glVertexAttribPointer (a_uv, 2, GL_FLOAT, GL_FALSE, 0, uv01);
		glDisableVertexAttribArray (a_color);

		/* RGB target: red with alpha 0.5 (stored, but not visible) */
		glBindFramebuffer (GL_FRAMEBUFFER, fbs[0]);
		glFramebufferTexture2D (GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t2[0], 0);
		CHECK (glCheckFramebufferStatus (GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE, "RGB target");
		glViewport (0, 0, 16, 16);
		glVertexAttrib4f (a_color, 1, 0, 0, 0.5f);
		glDrawArrays (GL_TRIANGLE_FAN, 0, 4);
		CHECK_PIXEL (8, 8, 255, 0, 0, 255, 0);
		/* DST_ALPHA reads 1 there: white * 1 (alpha not written) */
		glEnable (GL_BLEND);
		glBlendFunc (GL_DST_ALPHA, GL_ZERO);
		glColorMask (GL_TRUE, GL_TRUE, GL_TRUE, GL_FALSE);
		glVertexAttrib4f (a_color, 1, 1, 1, 1);
		glDrawArrays (GL_TRIANGLE_FAN, 0, 4);
		glColorMask (GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
		glDisable (GL_BLEND);
		glBlendFunc (GL_ONE, GL_ZERO);
		CHECK_PIXEL (8, 8, 255, 255, 255, 255, 0);

		/* sampling the RGB texture reads alpha 1 (its storage holds 0.5) */
		glBindFramebuffer (GL_FRAMEBUFFER, fbs[1]);
		glFramebufferTexture2D (GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t2[1], 0);
		glClearColor (0, 0, 0, 0);
		glClear (GL_COLOR_BUFFER_BIT);
		glBindTexture (GL_TEXTURE_2D, t2[0]);
		glEnableVertexAttribArray (a_uv);
		glUniform1i (l_use_tex, 1);
		glDrawArrays (GL_TRIANGLE_FAN, 0, 4);
		CHECK_PIXEL (8, 8, 255, 255, 255, 255, 0);

		/* texture object 0; texel 1 replaced with RGBA 4444 data */
		static const uint8_t black2[8] = {0, 0, 0, 255, 0, 0, 0, 255};
		static const uint16_t green4444 = 0x0F0F;
		glBindTexture (GL_TEXTURE_2D, 0);
		glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA, 2, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, black2);
		glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		glTexSubImage2D (GL_TEXTURE_2D, 0, 1, 0, 1, 1, GL_RGBA, GL_UNSIGNED_SHORT_4_4_4_4, &green4444);
		glDrawArrays (GL_TRIANGLE_FAN, 0, 4);
		CHECK_PIXEL (4, 8, 0, 0, 0, 255, 0);
		CHECK_PIXEL (12, 8, 0, 255, 0, 255, 0);
		glUniform1i (l_use_tex, 0);
		glDisableVertexAttribArray (a_uv);

		/* depth renderbuffer shared by fbs[1] and fbs[2] (depth only) */
		glBindRenderbuffer (GL_RENDERBUFFER, rbd);
		glRenderbufferStorage (GL_RENDERBUFFER, GL_DEPTH_COMPONENT16, 16, 16);
		glFramebufferRenderbuffer (GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, rbd);
		glClearDepthf (1.0f);
		glClear (GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
		glBindFramebuffer (GL_FRAMEBUFFER, fbs[2]);
		glFramebufferRenderbuffer (GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, rbd);
		CHECK (glCheckFramebufferStatus (GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE, "depth-only framebuffer");
		glClearDepthf (0.2f);
		glClear (GL_DEPTH_BUFFER_BIT);
		glBindFramebuffer (GL_FRAMEBUFFER, fbs[1]);
		glEnable (GL_DEPTH_TEST);
		glVertexAttrib4f (a_color, 1, 0, 0, 1);
		glDrawArrays (GL_TRIANGLE_FAN, 0, 4);		/* depth 0.5, not < 0.2 */
		CHECK_PIXEL (8, 8, 0, 0, 0, 0, 0);
		glBindFramebuffer (GL_FRAMEBUFFER, fbs[2]);
		glClearDepthf (1.0f);
		glClear (GL_DEPTH_BUFFER_BIT);
		glBindFramebuffer (GL_FRAMEBUFFER, fbs[1]);
		glDrawArrays (GL_TRIANGLE_FAN, 0, 4);		/* 0.5 < 1 */
		CHECK_PIXEL (8, 8, 255, 0, 0, 255, 0);
		glDisable (GL_DEPTH_TEST);

		glEnableVertexAttribArray (a_color);
		glBindFramebuffer (GL_FRAMEBUFFER, 0);
		glViewport (0, 0, 320, 240);
		CHECK_ERROR (GL_NO_ERROR, "targets, texture 0, depth sharing");
	}

	/* ---- errors ---- */
	glUseProgram (prog);
	glUniform1f (l_which, 1.0f);
	CHECK_ERROR (GL_INVALID_OPERATION, "glUniform1f on an int");
	glDrawArrays (99, 0, 3);
	CHECK_ERROR (GL_INVALID_ENUM, "glDrawArrays (99)");
	GLuint shader = glCreateShader (GL_VERTEX_SHADER);
	glCompileShader (shader);
	CHECK_ERROR (compiler ? GL_NO_ERROR : GL_INVALID_OPERATION, "glCompileShader");
	glDeleteShader (shader);
	pgpu_texture_params (MAX_TEST_TEXTURE, 0, 0, 0, 0);	/* no such texture: the RPi reports it */
	glFinish ();
	uint32_t rpi_error[3];
	pglGetRPiError (rpi_error);
	CHECK_ERROR (GL_INVALID_OPERATION, "an ERROR reply");
	CHECK (rpi_error[0] == PGPU_ERR_OBJECT && rpi_error[1] == PGPU_OP_TEXTURE_PARAMS,
	       "RPi error %lu %02lx %lu", (unsigned long) rpi_error[0],
	       (unsigned long) rpi_error[1], (unsigned long) rpi_error[2]);
	glUseProgram (0);

	/* ---- the panel ---- */
	CHECK_PIXEL (35, 35, 255, 0, 0, 255, 8);		/* A */
	CHECK_PIXEL (95, 35, 0, 255, 0, 255, 8);		/* B */
	CHECK_PIXEL (155, 35, 0, 0, 255, 255, 8);		/* C */
	CHECK_PIXEL (200, 20, 255, 0, 0, 255, 8);		/* D: texels */
	CHECK_PIXEL (220, 20, 0, 255, 0, 255, 8);
	CHECK_PIXEL (240, 45, 255, 0, 255, 255, 8);
	CHECK_PIXEL (285, 35, 255, 255, 0, 255, 8);		/* E */
	CHECK_PIXEL (35, 95, 255, 0, 255, 255, 8);		/* F */
	CHECK_PIXEL (95, 95, 128, 255, 128, 255, 8);		/* 70000-vertex strip */
	CHECK_PIXEL (155, 95, 255, 128, 128, 255, 8);		/* 70002-vertex list */
	CHECK_PIXEL (65, 64, 25, 25, 51, 255, 8);		/* nothing between (0, 0) and the strip */
	CHECK_PIXEL (220, 90, 255, 255, 255, 255, 8);		/* line */
	CHECK_PIXEL (220, 92, 25, 25, 51, 255, 8);		/* one pixel wide: the clear colour above */
	CHECK_PIXEL (35, 155, 200, 200, 200, 255, 8);		/* diamond */
	CHECK_PIXEL (35, 177, 200, 200, 200, 255, 8);		/* its top corner (rotated) */
	CHECK_PIXEL (95, 155, 0, 255, 255, 255, 8);		/* client indices */
	CHECK_PIXEL (155, 155, 255, 153, 0, 255, 8);		/* lighting: 0.2 * 0.2 ambient + diffuse */
	CHECK_PIXEL (200, 140, 255, 0, 0, 255, 8);		/* texture */
	CHECK_PIXEL (35, 210, 255, 0, 0, 255, 8);		/* framebuffer texture: the red quad */
	CHECK_PIXEL (12, 187, 0, 128, 0, 255, 8);		/* its clear colour */
	CHECK_PIXEL (95, 210, 255, 255, 0, 255, 8);		/* the copy: texel (1, 1) of D */
	CHECK_PIXEL (150, 210, 77, 73, 182, 255, 8);		/* control flow */
	CHECK_PIXEL (200, 210, 204, 219, 36, 255, 8);
	CHECK_PIXEL (250, 210, 25, 25, 51, 255, 8);		/* discarded */
	CHECK_PIXEL (288, 210, 77, 73, 32, 255, 8);		/* per-pixel loop: 1, 4, 8 iterations */
	CHECK_PIXEL (291, 210, 77, 73, 128, 255, 8);
	CHECK_PIXEL (295, 210, 77, 73, 255, 255, 8);
	CHECK_PIXEL (285, 95, 128, 128, 0, 255, 8);		/* gl_FragCoord at pixel centres */
	CHECK_PIXEL (273, 167, 36, 28, 0, 255, 8);		/* gl_PointCoord: top left of the point */
	CHECK_PIXEL (273, 143, 36, 219, 0, 255, 8);		/* bottom left */
	CHECK_ERROR (GL_NO_ERROR, "end");

	pglSwapBuffers ();
	pgpu_begin (PGPU_OP_DEBUG_SCREENSHOT, 0);
	pgpu_end ();
	pgpu_flush ();
	printf ("gltest: %u checks, %u failed; screenshot requested\n", checks, failures);
	sleep_ms (3000);
	return failures;
}
