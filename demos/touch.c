/*
 * touch - the panel's touch screen (the TOUCH reply; its events in order,
 * pgpu_poll_touch: every place the RPi reported, however long a frame took) as
 * a drawing board: a finger draws round dabs along its way into a canvas (a
 * texture the RPi renders into, kept between frames), shown over the whole
 * screen. Along the bottom the colours (the last one white: a rubber), CAL
 * and CLEAR. Top left where it's touched, the controller's readings and the
 * pressure; each press goes to the log too. The touch is in the panel's
 * pixels (320x240), scaled to the screen (HDMI).
 *
 * With CAL a calibration: a target at each corner and the middle
 * to tap; the controller's readings there give the panel's pixels by an
 * affine map (any rotation, mirroring, skew), fitted by least squares, used
 * from then on instead of the RPi's own (its touchcal=: the nearest such line
 * goes to the log). A fit off by more than CAL_SLACK pixels: again.
 */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "gles/pgl.h"
#include "pgpu.h"
#include "hud.h"
#include "pgpu_perf.h"
#include "screen.h"
#include "brush_program.h"
#include "texview_program.h"

#define PANEL_W		320
#define PANEL_H		240
#define BAR_H		36		/* the colours along the bottom, panel pixels */
#define BRUSH		3.0f		/* the brush's radius, panel pixels */

static const uint8_t colors[][3] =
{
	{20, 20, 24}, {220, 40, 40}, {240, 150, 20}, {40, 170, 60}, {40, 90, 220}, {150, 60, 200}, {255, 255, 255},
};
#define COLORS		(int) (sizeof colors / sizeof colors[0])
#define CLEAR_W		56		/* the CLEAR button, panel pixels, at the right ... */
#define CAL_W		40		/* ... CAL left of it */
#define BUTTONS_W	(CLEAR_W + CAL_W)
#define CAL_SLACK	12.0f		/* the most a fitted target may be off, panel pixels */

/* the calibration's targets, the panel's pixels */
static const float targets[][2] = {{24, 24}, {296, 24}, {296, 216}, {24, 216}, {160, 120}};
#define TARGETS		(int) (sizeof targets / sizeof targets[0])

typedef struct
{
	bool valid;
	float m[2][3];				/* x, y = m[k][0] * raw x + m[k][1] * raw y + m[k][2] */
	int target;				/* while calibrating: the next one */
	float sum[2];				/* its readings while pressed, ... */
	int samples;				/* ... how many */
	float raw[TARGETS][2];			/* the targets' readings */
} cal_t;

static void cal_map (const cal_t *c, unsigned px, unsigned py, unsigned raw_x, unsigned raw_y, float *x, float *y)
{
	if (!c->valid)
	{
		*x = px;
		*y = py;
		return;
	}
	*x = c->m[0][0] * raw_x + c->m[0][1] * raw_y + c->m[0][2];
	*y = c->m[1][0] * raw_x + c->m[1][1] * raw_y + c->m[1][2];
}

/* the affine map from the targets' readings: least squares (the normal
   equations, 3 x 3, by Cramer's rule); false if they don't fit */
static bool cal_fit (cal_t *c)
{
	double a[3][3] = {{0}}, r[2][3] = {{0}};
	for (int i = 0; i < TARGETS; i++)
	{
		double v[3] = {c->raw[i][0], c->raw[i][1], 1.0};
		for (int j = 0; j < 3; j++)
		{
			for (int k = 0; k < 3; k++)
				a[j][k] += v[j] * v[k];
			r[0][j] += v[j] * targets[i][0];
			r[1][j] += v[j] * targets[i][1];
		}
	}
	double det =   a[0][0] * (a[1][1] * a[2][2] - a[1][2] * a[2][1]) - a[0][1] * (a[1][0] * a[2][2] - a[1][2] * a[2][0])
		     + a[0][2] * (a[1][0] * a[2][1] - a[1][1] * a[2][0]);
	if (det == 0.0)
	{
		return false;
	}
	for (int o = 0; o < 2; o++)
		for (int col = 0; col < 3; col++)
		{
			double m[3][3];
			memcpy (m, a, sizeof m);
			for (int row = 0; row < 3; row++)
				m[row][col] = r[o][row];
			double d =   m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0])
				   + m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
			c->m[o][col] = (float) (d / det);
		}
	float worst = 0.0f;
	for (int i = 0; i < TARGETS; i++)
	{
		float x = c->m[0][0] * c->raw[i][0] + c->m[0][1] * c->raw[i][1] + c->m[0][2];
		float y = c->m[1][0] * c->raw[i][0] + c->m[1][1] * c->raw[i][1] + c->m[1][2];
		float e = sqrtf ((x - targets[i][0]) * (x - targets[i][0]) + (y - targets[i][1]) * (y - targets[i][1]));
		printf ("touch: target %d at %.0f,%.0f: readings %.0f,%.0f, fitted %.0f,%.0f (off %.1f)\n", i + 1, targets[i][0],
			targets[i][1], c->raw[i][0], c->raw[i][1], x, y, e);
		worst = e > worst ? e : worst;
	}
	if (worst > CAL_SLACK)
	{
		printf ("touch: the calibration is off by up to %.0f pixels: again\n", worst);
		return false;
	}
	/* the RPi's touchcal= (x from one reading, y from the other): the readings at the edges */
	bool swap = fabsf (c->m[0][1]) > fabsf (c->m[0][0]);
	float xa = swap ? c->m[0][1] : c->m[0][0], ya = swap ? c->m[1][0] : c->m[1][1];
	printf ("touch: calibrated (off by up to %.1f pixels); for the RPi: touchcal=%.0f,%.0f,%.0f,%.0f,%d\n", worst,
		-c->m[0][2] / xa, (PANEL_W - 1 - c->m[0][2]) / xa, -c->m[1][2] / ya, (PANEL_H - 1 - c->m[1][2]) / ya, swap ? 1 : 0);
	return true;
}

typedef struct
{
	GLuint brush, view, quad, fb, canvas;
	GLint b_rect, b_color, b_pos, v_rect, v_mode, v_pos;
	int w, h;				/* the canvas: the screen's size */
} board_t;

static void canvas_clear (board_t *b)
{
	glBindFramebuffer (GL_FRAMEBUFFER, b->fb);
	glViewport (0, 0, b->w, b->h);
	glClearColor (1.0f, 1.0f, 1.0f, 1.0f);
	glClear (GL_COLOR_BUFFER_BIT);
	glBindFramebuffer (GL_FRAMEBUFFER, 0);
}

/* a canvas as big as the screen (again when it changes size: cleared) */
static bool canvas_make (board_t *b, int w, int h)
{
	if (b->canvas)
	{
		glDeleteTextures (1, &b->canvas);
	}
	b->w = w;
	b->h = h;
	glGenTextures (1, &b->canvas);
	glBindTexture (GL_TEXTURE_2D, b->canvas);
	glTexImage2D (GL_TEXTURE_2D, 0, GL_RGB, w, h, 0, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, NULL);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	if (!b->fb)
	{
		glGenFramebuffers (1, &b->fb);
	}
	glBindFramebuffer (GL_FRAMEBUFFER, b->fb);
	glFramebufferTexture2D (GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, b->canvas, 0);
	GLenum status = glCheckFramebufferStatus (GL_FRAMEBUFFER);
	glBindFramebuffer (GL_FRAMEBUFFER, 0);
	if (status != GL_FRAMEBUFFER_COMPLETE)
	{
		printf ("touch: the canvas (%dx%d) can't be drawn into: 0x%x\n", w, h, (unsigned) status);
		return false;
	}
	canvas_clear (b);
	return true;
}

static void board_init (board_t *b)
{
	static const float square[12] = {0, 0, 1, 0, 1, 1, 0, 0, 1, 1, 0, 1};
	memset (b, 0, sizeof *b);
	b->brush = glCreateProgram ();
	glProgramBinaryOES (b->brush, PGL_PROGRAM_BINARY_PGPU, &brush_info, sizeof brush_info);
	b->b_rect = glGetUniformLocation (b->brush, "u_rect");
	b->b_color = glGetUniformLocation (b->brush, "u_color");
	b->b_pos = glGetAttribLocation (b->brush, "a_pos");
	b->view = glCreateProgram ();
	glProgramBinaryOES (b->view, PGL_PROGRAM_BINARY_PGPU, &texview_info, sizeof texview_info);
	b->v_rect = glGetUniformLocation (b->view, "u_rect");
	b->v_mode = glGetUniformLocation (b->view, "u_mode");
	b->v_pos = glGetAttribLocation (b->view, "a_pos");
	glUseProgram (b->view);
	glUniform1i (glGetUniformLocation (b->view, "u_tex"), 0);
	glUniform1i (glGetUniformLocation (b->view, "u_cube"), 1);	/* (not the 2D one's unit: GL ES won't draw) */
	glGenBuffers (1, &b->quad);
	glBindBuffer (GL_ARRAY_BUFFER, b->quad);
	glBufferData (GL_ARRAY_BUFFER, sizeof square, square, GL_STATIC_DRAW);
}

/* dabs along a stroke from (x0, y0) to (x1, y1), panel pixels, into the canvas */
static void stroke (board_t *b, float x0, float y0, float x1, float y1, int color)
{
	float sx = (float) b->w / PANEL_W, sy = (float) b->h / PANEL_H;
	float r = BRUSH * (sx < sy ? sx : sy), rw = r * 2.0f / b->w * 2.0f, rh = r * 2.0f / b->h * 2.0f;
	float len = sqrtf ((x1 - x0) * (x1 - x0) + (y1 - y0) * (y1 - y0));
	int n = (int) (len / (BRUSH * 0.4f)) + 1;
	glBindFramebuffer (GL_FRAMEBUFFER, b->fb);
	glViewport (0, 0, b->w, b->h);
	glUseProgram (b->brush);
	glBindBuffer (GL_ARRAY_BUFFER, b->quad);
	for (GLint i = 0; i < 4; i++)
	{
		if (i == b->b_pos)
			glEnableVertexAttribArray (i);
		else
			glDisableVertexAttribArray (i);
	}
	glVertexAttribPointer (b->b_pos, 2, GL_FLOAT, GL_FALSE, 0, (void *) 0);
	glUniform4f (b->b_color, colors[color][0] / 255.0f, colors[color][1] / 255.0f, colors[color][2] / 255.0f, 1.0f);
	for (int k = 0; k < n; k++)
	{
		float t = n > 1 ? (float) k / (n - 1) : 1.0f;
		float px = (x0 + (x1 - x0) * t) * sx, py = (y0 + (y1 - y0) * t) * sy;
		float cx = px / b->w * 2.0f - 1.0f, cy = 1.0f - py / b->h * 2.0f;
		glUniform4f (b->b_rect, cx - rw / 2, cy - rh / 2, rw, rh);
		glDrawArrays (GL_TRIANGLES, 0, 6);
	}
	glBindFramebuffer (GL_FRAMEBUFFER, 0);
}

/* the canvas over the screen */
static void board_draw (board_t *b, const GLint vp[4])
{
	glViewport (vp[0], vp[1], vp[2], vp[3]);
	glUseProgram (b->view);
	glBindBuffer (GL_ARRAY_BUFFER, b->quad);
	for (GLint i = 0; i < 4; i++)
	{
		if (i == b->v_pos)
			glEnableVertexAttribArray (i);
		else
			glDisableVertexAttribArray (i);
	}
	glVertexAttribPointer (b->v_pos, 2, GL_FLOAT, GL_FALSE, 0, (void *) 0);
	glUniform4f (b->v_rect, -1.0f, -1.0f, 2.0f, 2.0f);
	glUniform1f (b->v_mode, 1.0f);
	glActiveTexture (GL_TEXTURE0);
	glBindTexture (GL_TEXTURE_2D, b->canvas);
	glDrawArrays (GL_TRIANGLES, 0, 6);
}

int main (void)
{
	stdio_init_all ();
	pgpu_init ();
	printf ("\ntouch: waiting for the RPi (READY)...\n");
	while (!pgpu_wait_ready (1000))
	{
	}
	pgpu_set_reply_phase (1);
	int tries = 0;
	while (!pglInit () && ++tries < 5)		/* the first reply can be missed */
	{
	}
	if (!hud_init ())
	{
		printf ("touch: the HUD program didn't link\n");
	}
	static board_t b;
	board_init (&b);
	glDisable (GL_DEPTH_TEST);
	glDisable (GL_CULL_FACE);

	GLint vp[4] = {0};
	unsigned presses = 0;
	pgpu_touch_t t;
	memset (&t, 0, sizeof t);
	perf_t m;
	memset (&m, 0, sizeof m);
	bool dirty = true, announced = false, drawing = false, calibrating = false;
	static cal_t cal;
	float lx = 0, ly = 0;
	int color = 0;
	while (true)
	{
		if (screen_update ("touch", vp) || !b.canvas)
		{
			canvas_make (&b, vp[2], vp[3]);
			dirty = true;
		}

		uint32_t count = pgpu_get_touch (&t);		/* (the last state: the line at the top) */
		if (count && !announced)
		{
			printf ("touch: the RPi has a touch screen\n");
			announced = true;
		}
		pgpu_touch_event_t e;
		while (pgpu_poll_touch (&e))
		{
			bool pressed = e.type == PGPU_TOUCH_EVENT_DOWN, down = e.type != PGPU_TOUCH_EVENT_UP;
			if (pressed)
			{
				printf ("touch: pressed at %u,%u (readings %u,%u, pressure %u), %u presses\n", e.x, e.y,
					e.raw_x, e.raw_y, e.pressure, ++presses);
			}
			float x, y;
			cal_map (&cal, e.x, e.y, e.raw_x, e.raw_y, &x, &y);
			if (calibrating)
			{
				/* the target's readings: while pressed; taken on the let go */
				if (down)
				{
					cal.sum[0] += e.raw_x;
					cal.sum[1] += e.raw_y;
					cal.samples++;
				}
				if (!down && cal.samples)
				{
					cal.raw[cal.target][0] = cal.sum[0] / cal.samples;
					cal.raw[cal.target][1] = cal.sum[1] / cal.samples;
					cal.sum[0] = cal.sum[1] = 0.0f;
					cal.samples = 0;
					if (++cal.target == TARGETS)
					{
						cal.valid = cal_fit (&cal);
						cal.target = 0;
						calibrating = !cal.valid;
					}
				}
			}
			else if (down && y >= PANEL_H - BAR_H)
			{
				/* the bar: a colour, CAL or CLEAR (on the press) */
				if (pressed && x >= PANEL_W - CLEAR_W)
				{
					canvas_clear (&b);
					printf ("touch: cleared\n");
				}
				else if (pressed && x >= PANEL_W - BUTTONS_W)
				{
					calibrating = true;
					cal.target = cal.samples = 0;
					cal.sum[0] = cal.sum[1] = 0.0f;
				}
				else if (x < PANEL_W - BUTTONS_W)
				{
					color = (int) (x < 0 ? 0 : x) * COLORS / (PANEL_W - BUTTONS_W);
				}
				drawing = false;
			}
			else if (down)
			{
				stroke (&b, drawing ? lx : x, drawing ? ly : y, x, y, color);
				lx = x;
				ly = y;
				drawing = true;
			}
			else
			{
				drawing = false;
			}
			dirty = true;
		}

		if (calibrating)
		{
			glViewport (vp[0], vp[1], vp[2], vp[3]);
			glClearColor (0.0f, 0.0f, 0.0f, 1.0f);
			glClear (GL_COLOR_BUFFER_BIT);
		}
		else
		{
			board_draw (&b, vp);
		}
		if (dirty && calibrating)
		{
			float sx = (float) vp[2] / PANEL_W, sy = (float) vp[3] / PANEL_H;
			float hs = vp[3] >= 1000 ? 1.5f : vp[3] >= 600 ? 1.0f : 0.5f;
			char line[48];
			hud_begin ();
			float cx = targets[cal.target][0] * sx, cy = targets[cal.target][1] * sy, len = 14 * sx, th = sx > 1 ? sx : 1;
			hud_rect (cx - len, cy - th / 2, 2 * len, th, HUD_RGBA (255, 255, 255, 255));
			hud_rect (cx - th / 2, cy - len, th, 2 * len, HUD_RGBA (255, 255, 255, 255));
			hud_rect (cx - 2 * th, cy - 2 * th, 4 * th, 4 * th, HUD_RGBA (255, 60, 60, 255));
			snprintf (line, sizeof line, "TAP THE TARGET  %d/%d", cal.target + 1, TARGETS);
			hud_text_scaled ((float) (int) ((vp[2] - strlen (line) * HUD_CHAR_W * hs) / 2), (float) (int) (vp[3] * 0.45f),
					 line, HUD_RGBA (255, 220, 60, 255), hs);
			hud_end ();
			dirty = false;
		}
		if (dirty)
		{
			float sx = (float) vp[2] / PANEL_W, sy = (float) vp[3] / PANEL_H;
			float hs = vp[3] >= 1000 ? 1.5f : vp[3] >= 600 ? 1.0f : 0.5f;
			char line[64];
			hud_begin ();
			/* the bar: the colours, the one in use framed; CLEAR */
			float bar_y = (PANEL_H - BAR_H) * sy, cell = (PANEL_W - BUTTONS_W) * sx / COLORS;
			hud_rect (0, bar_y, vp[2], vp[3] - bar_y, HUD_RGBA (60, 62, 70, 255));
			for (int i = 0; i < COLORS; i++)
			{
				float x = i * cell, pad = 4 * sx;
				if (i == color)
				{
					hud_rect (x + pad / 2, bar_y + pad / 2, cell - pad, vp[3] - bar_y - pad, HUD_RGBA (255, 220, 60, 255));
				}
				hud_rect (x + pad, bar_y + pad, cell - 2 * pad, vp[3] - bar_y - 2 * pad,
					  HUD_RGBA (colors[i][0], colors[i][1], colors[i][2], 255));
			}
			float text_y = (float) (int) (bar_y + (BAR_H * sy - HUD_CHAR_H * hs) / 2);
			hud_text_scaled ((float) (int) ((PANEL_W - BUTTONS_W + 4) * sx), text_y, "CAL", HUD_RGBA (255, 255, 255, 255), hs);
			hud_text_scaled ((float) (int) ((PANEL_W - CLEAR_W + 4) * sx), text_y, "CLEAR", HUD_RGBA (255, 255, 255, 255), hs);
			hud_rect ((PANEL_W - CLEAR_W) * sx, bar_y, sx > 1 ? sx : 1, vp[3] - bar_y, HUD_RGBA (120, 120, 130, 255));
			/* where it's touched */
			if (!count)
			{
				hud_text_scaled (4, 4, "NO TOUCH SCREEN", HUD_RGBA (220, 40, 40, 255), hs);
			}
			else
			{
				float x, y;
				cal_map (&cal, t.x, t.y, t.raw_x, t.raw_y, &x, &y);
				snprintf (line, sizeof line, "%s %3.0f %3.0f  RAW %4u %4u  P %4u", t.down ? "DOWN" : "UP", x, y,
					  t.raw_x, t.raw_y, t.pressure);
				hud_rect (2, 2, strlen (line) * HUD_CHAR_W * hs + 4, HUD_CHAR_H * hs + 2, HUD_RGBA (0, 0, 0, 120));
				hud_text_scaled (4, 3, line, HUD_RGBA (255, 255, 255, 255), hs);
			}
			hud_end ();
			dirty = false;
		}
		hud_draw ();
		pglSwapBuffers ();

		absolute_time_t wait_start = get_absolute_time ();
		pgpu_wait_frame (100);			/* pace on the screen */
		perf_frame (absolute_time_diff_us (wait_start, get_absolute_time ()), &m);
	}
}
