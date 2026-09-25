/*
 * compile_test.c - pgl's shader compiler on the PC: GLSL source compiled at
 * run time (glShaderSource, glCompileShader, glLinkProgram), drawn by the Zero
 */
#include <stdio.h>
#include <string.h>
#include "pgpu.h"
#include "pgl.h"

static unsigned failures;
#define CHECK(c, ...) do { if (!(c)) { failures++; printf ("compile_test: FAIL: " __VA_ARGS__); printf ("\n"); } } while (0)

static GLuint shader (GLenum type, const char *src)
{
	GLuint s = glCreateShader (type);
	glShaderSource (s, 1, &src, NULL);
	glCompileShader (s);
	return s;
}

int main (void)
{
	pgpu_init ();
	pglInit ();
	glDisable (GL_DITHER);

	GLboolean compiler = GL_FALSE;
	glGetBooleanv (GL_SHADER_COMPILER, &compiler);
	CHECK (compiler, "GL_SHADER_COMPILER is false");

	/* a compile error, with the info log */
	GLuint bad = shader (GL_VERTEX_SHADER, "void main () { gl_Position = vec4 (x); }\n");
	GLint ok = 1, len = 0;
	glGetShaderiv (bad, GL_COMPILE_STATUS, &ok);
	glGetShaderiv (bad, GL_INFO_LOG_LENGTH, &len);
	char log[512] = "";
	glGetShaderInfoLog (bad, sizeof log, NULL, log);
	CHECK (!ok && len > 1 && strstr (log, "undeclared"), "compile error not reported: %d %d '%s'", ok, len, log);

	/* a program from source, attribute bound to location 3 */
	GLuint vs = shader (GL_VERTEX_SHADER,
		"attribute vec2 pos;\n"
		"uniform vec2 offset;\n"
		"void main () { gl_Position = vec4 (pos + offset, 0.0, 1.0); }\n");
	GLuint fs = shader (GL_FRAGMENT_SHADER,
		"precision mediump float;\n"
		"uniform vec4 color;\n"
		"void main () { gl_FragColor = color; }\n");
	glGetShaderiv (vs, GL_COMPILE_STATUS, &ok);
	CHECK (ok, "vertex shader did not compile");
	GLuint prog = glCreateProgram ();
	glAttachShader (prog, vs);
	glAttachShader (prog, fs);
	glBindAttribLocation (prog, 3, "pos");
	glLinkProgram (prog);
	glGetProgramiv (prog, GL_LINK_STATUS, &ok);
	char plog[512] = "";
	glGetProgramInfoLog (prog, sizeof plog, NULL, plog);
	CHECK (ok, "link failed: %s", plog);
	CHECK (glGetAttribLocation (prog, "pos") == 3, "pos at %d", glGetAttribLocation (prog, "pos"));

	glClearColor (0, 0, 0, 1);
	glClear (GL_COLOR_BUFFER_BIT);
	glUseProgram (prog);
	glUniform2f (glGetUniformLocation (prog, "offset"), 0.25f, 0);
	glUniform4f (glGetUniformLocation (prog, "color"), 1, 0.5f, 0, 1);
	static const float quad[8] = {-0.5f, -0.5f, 0.5f, -0.5f, 0.5f, 0.5f, -0.5f, 0.5f};
	glVertexAttribPointer (3, 2, GL_FLOAT, GL_FALSE, 0, quad);
	glEnableVertexAttribArray (3);
	glDrawArrays (GL_TRIANGLE_FAN, 0, 4);
	uint8_t p[4], q[4];
	glReadPixels (200, 120, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, p);	/* inside (x 0.25 .. 0.75 NDC: 200 .. 280) */
	glReadPixels (100, 120, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, q);	/* outside */
	CHECK (p[0] > 245 && p[1] > 120 && p[1] < 135 && p[2] < 8, "inside %d %d %d", p[0], p[1], p[2]);
	CHECK (q[0] < 8 && q[1] < 8 && q[2] < 8, "outside %d %d %d", q[0], q[1], q[2]);
	glFinish ();
	uint32_t ze[3];
	pglGetZeroError (ze);
	GLenum e = glGetError ();
	CHECK (e == GL_NO_ERROR, "GL error %04x (Zero %u %02x %u)", e, ze[0], ze[1], ze[2]);
	pglSwapBuffers ();

	printf ("compile_test: %u failed\n", failures);
	return failures ? 1 : 0;
}
