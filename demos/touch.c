/*
 * touch - the panel's touch screen (the TOUCH reply, pgpu_get_touch): a
 * crosshair where it's touched and a trail behind, the position, the
 * controller's readings (to calibrate by: the RPi's touchcal=), the pressure
 * and the presses so far. The HUD draws it all; the touch is in the panel's
 * pixels (320x240), scaled to the screen (HDMI).
 */
#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "gles/pgl.h"
#include "pgpu.h"
#include "hud.h"
#include "pgpu_perf.h"
#include "screen.h"

#define TRAIL		256		/* the last points touched */
#define PANEL_W		320
#define PANEL_H		240

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
	glClearColor (0.06f, 0.07f, 0.10f, 1.0f);

	GLint vp[4] = {0};
	static uint16_t trail[TRAIL][2];
	unsigned n_trail = 0, last_count = 0, last_presses = 0;
	pgpu_touch_t t;
	memset (&t, 0, sizeof t);
	perf_t m;
	memset (&m, 0, sizeof m);
	bool dirty = true, announced = false;
	while (true)
	{
		if (screen_update ("touch", vp))
		{
			dirty = true;
		}

		uint32_t count = pgpu_get_touch (&t);
		if (count != last_count)
		{
			if (!announced)
			{
				printf ("touch: the RPi has a touch screen\n");
				announced = true;
			}
			if (t.down)
			{
				trail[n_trail % TRAIL][0] = t.x;
				trail[n_trail % TRAIL][1] = t.y;
				n_trail++;
			}
			if (t.presses != last_presses)
			{
				printf ("touch: pressed at %u,%u (readings %u,%u, pressure %u), %u presses\n", t.x, t.y,
					t.raw_x, t.raw_y, t.pressure, t.presses);
				last_presses = t.presses;
			}
			last_count = count;
			dirty = true;
		}

		glClear (GL_COLOR_BUFFER_BIT);
		if (dirty)
		{
			float sx = (float) vp[2] / PANEL_W, sy = (float) vp[3] / PANEL_H, s = sx < sy ? sx : sy;
			float dot = 3 * s > 2 ? 3 * s : 2;
			char line[64];
			hud_begin ();
			unsigned first = n_trail > TRAIL ? n_trail - TRAIL : 0;
			for (unsigned i = first; i < n_trail; i++)
			{
				unsigned age = n_trail - 1 - i, a = 255 - age * 200 / TRAIL;
				hud_rect (trail[i % TRAIL][0] * sx - dot / 2, trail[i % TRAIL][1] * sy - dot / 2, dot, dot,
					  HUD_RGBA (80, 200, 255, a));
			}
			if (count)
			{
				float cx = t.x * sx, cy = t.y * sy, len = 12 * s;
				uint32_t c = t.down ? HUD_RGBA (255, 220, 60, 255) : HUD_RGBA (120, 120, 120, 200);
				hud_rect (cx - len, cy - s / 2, 2 * len, s > 1 ? s : 1, c);
				hud_rect (cx - s / 2, cy - len, s > 1 ? s : 1, 2 * len, c);
			}
			float hs = vp[3] >= 1000 ? 1.5f : vp[3] >= 600 ? 1.0f : 0.5f, lh = HUD_CHAR_H * hs * 1.15f;
			if (!count)
			{
				hud_text_scaled (4, 4, "NO TOUCH SCREEN", HUD_RGBA (255, 120, 120, 255), hs);
			}
			else
			{
				snprintf (line, sizeof line, "%s %3u %3u", t.down ? "DOWN" : "UP  ", t.x, t.y);
				hud_text_scaled (4, 4, line, HUD_RGBA (255, 255, 255, 255), hs);
				snprintf (line, sizeof line, "RAW %4u %4u", t.raw_x, t.raw_y);
				hud_text_scaled (4, 4 + lh, line, HUD_RGBA (200, 200, 200, 255), hs);
				snprintf (line, sizeof line, "PRESSURE %4u", t.pressure);
				hud_text_scaled (4, 4 + 2 * lh, line, HUD_RGBA (200, 200, 200, 255), hs);
				snprintf (line, sizeof line, "PRESSES %u", t.presses);
				hud_text_scaled (4, 4 + 3 * lh, line, HUD_RGBA (200, 200, 200, 255), hs);
			}
			hud_perf (vp[2] - hud_perf_width (0.5f) - 2, 2, 0.5f, &m);
			hud_end ();
			dirty = false;
		}
		hud_draw ();
		pglSwapBuffers ();

		absolute_time_t wait_start = get_absolute_time ();
		pgpu_wait_frame (100);			/* pace on the screen */
		if (perf_frame (absolute_time_diff_us (wait_start, get_absolute_time ()), &m))
		{
			dirty = true;
		}
	}
}
