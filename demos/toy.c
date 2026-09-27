/*
 * toy - Shadertoy-style demos: one full-screen quad, all the picture made by
 * a fragment shader on the V3D. The shader gets Shadertoy's inputs: iTime
 * (seconds), iResolution (pixels) and the pixel position (fragCoord, as
 * v_coord from shaders/toy.vert), and, if it declares iChannel0, a 64x64
 * texture of random RGBA bytes (repeat, linear), as Shadertoy's noise
 * channel: one texture fetch is much cheaper on the V3D than a hash in
 * arithmetic. The Pico only sends a uniform and a draw a frame.
 *
 * Built once per shader (CMakeLists.txt, toy_demo()): TOY_HEADER is the
 * glslc header of shaders/toy.vert with toy_NAME.frag, TOY_INFO its program,
 * TOY_CAPTION the name shown bottom left. A game (TOY_GAME) adds the Pico's
 * side: game_init and game_frame (toy_game.h) keep and play the game and set
 * the shader's uniforms each frame. The half-size HUD top right shows
 * the frame rate and the loads (hud.c, perf.c), as in gears.
 */
#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "gles/pgl.h"
#include "pgpu.h"
#include "hud.h"
#include "perf.h"
#include TOY_HEADER
#ifdef TOY_GAME
#include "toy_game.h"
#endif

#define NOISE_SIZE	64
#define NOISE_UNIT	1		/* the HUD binds its font to unit 0 each frame */

/* iChannel0: random bytes (xorshift32) */
static void noise_texture (void)
{
	static uint8_t texels[NOISE_SIZE * NOISE_SIZE * 4];
	uint32_t x = 2463534242u;
	for (unsigned i = 0; i < sizeof texels; i++)
	{
		x ^= x << 13;
		x ^= x >> 17;
		x ^= x << 5;
		texels[i] = (uint8_t) (x >> 24);
	}
	GLuint texture;
	glGenTextures (1, &texture);
	glActiveTexture (GL_TEXTURE0 + NOISE_UNIT);
	glBindTexture (GL_TEXTURE_2D, texture);
	glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA, NOISE_SIZE, NOISE_SIZE, 0, GL_RGBA, GL_UNSIGNED_BYTE, texels);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
	glActiveTexture (GL_TEXTURE0);
}

int main (void)
{
	stdio_init_all ();
	pgpu_init ();
	printf ("\ntoy %s: waiting for the Zero (READY)...\n", TOY_CAPTION);
	while (!pgpu_wait_ready (1000))
	{
	}
	pgpu_set_reply_phase (1);
	int tries = 0;
	while (!pglInit () && ++tries < 5)		/* the first reply can be missed */
	{
	}
	if (tries == 5)
	{
		printf ("toy: no answer from the Zero\n");
	}

	GLuint prog = glCreateProgram ();
	glProgramBinaryOES (prog, PGL_PROGRAM_BINARY_PGPU, &TOY_INFO, sizeof TOY_INFO);
	GLint linked = 0;
	glGetProgramiv (prog, GL_LINK_STATUS, &linked);
	if (!linked)
	{
		printf ("toy: the program didn't link\n");
	}
	GLint u_time = glGetUniformLocation (prog, "iTime");
	GLint u_resolution = glGetUniformLocation (prog, "iResolution");
	GLint a_pos = glGetAttribLocation (prog, "a_pos");
	GLint u_channel0 = glGetUniformLocation (prog, "iChannel0");

	/* the screen: two triangles in a buffer */
	static const float quad[12] = {-1, -1, 1, -1, 1, 1, -1, -1, 1, 1, -1, 1};
	GLuint buffer;
	glGenBuffers (1, &buffer);
	glBindBuffer (GL_ARRAY_BUFFER, buffer);
	glBufferData (GL_ARRAY_BUFFER, sizeof quad, quad, GL_STATIC_DRAW);

	GLint vp[4];
	glGetIntegerv (GL_VIEWPORT, vp);
	glUseProgram (prog);
	glUniform2f (u_resolution, (float) vp[2], (float) vp[3]);
	if (u_channel0 >= 0)
	{
		noise_texture ();
		glUniform1i (u_channel0, NOISE_UNIT);
	}
	glDisable (GL_DITHER);
#ifdef TOY_GAME
	game_init (prog, (float) vp[2], (float) vp[3]);
#endif
	if (!hud_init ())
	{
		printf ("toy: the HUD program didn't link\n");
	}

	printf ("toy %s: %dx%d, running\n", TOY_CAPTION, (int) vp[2], (int) vp[3]);
	absolute_time_t start = get_absolute_time ();
	float last = 0.0f;
	unsigned windows = 0;
	perf_t m;
	memset (&m, 0, sizeof m);
	while (true)
	{
		float t = absolute_time_diff_us (start, get_absolute_time ()) / 1e6f;

		/* every pixel is drawn: no clear */
		glUseProgram (prog);
		glUniform1f (u_time, t);
#ifdef TOY_GAME
		game_frame (t, t - last);
#endif
		last = t;
		glBindBuffer (GL_ARRAY_BUFFER, buffer);	/* hud_draw leaves its own bound */
		glEnableVertexAttribArray (a_pos);
		glVertexAttribPointer (a_pos, 2, GL_FLOAT, GL_FALSE, 0, (void *) 0);
		glDrawArrays (GL_TRIANGLES, 0, 6);
		hud_draw ();
		pglSwapBuffers ();

		absolute_time_t wait_start = get_absolute_time ();
		pgpu_wait_frame (100);			/* pace on the panel (swap interval 1) */
		if (perf_frame (absolute_time_diff_us (wait_start, get_absolute_time ()), &m))
		{
			hud_begin ();
			hud_perf (vp[2] - hud_perf_width (0.5f) - 2, 2, 0.5f, &m);	/* half size, top right */
			hud_text_scaled (4, vp[3] - 12, TOY_CAPTION, HUD_RGBA (255, 255, 255, 200), 0.5f);
			hud_end ();
			if (++windows % 5 == 0)
			{
				GLenum e = glGetError ();
				printf ("toy %s: %.1f fps, load GPU %.0f%% ARM %.0f%% Pico %.0f%%, "
					"render %.2f ms, panel wait %.2f ms, GL error 0x%x\n", TOY_CAPTION,
					m.fps, m.gpu * 100, m.arm * 100, m.pico * 100,
					m.render_ms, m.panel_ms, (unsigned) e);
			}
		}
	}
}
