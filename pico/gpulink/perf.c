/*
 * perf.c - frame rate and load for the demos' HUD (perf.h)
 */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "pgpu.h"
#include "hud.h"
#include "perf.h"

static absolute_time_t window_start;
static bool started;
static unsigned frames;
static uint64_t wait_us;
static pgpu_status_t status;

bool perf_frame (uint32_t wait, perf_t *m)
{
	if (!started)
	{
		window_start = get_absolute_time ();
		started = true;
	}
	frames++;
	wait_us += wait;

	pgpu_reply_t r;
	while (pgpu_poll_reply (&r))
	{
		pgpu_status_from_reply (&r, &status);	/* the Zero's last second */
	}

	int64_t window = absolute_time_diff_us (window_start, get_absolute_time ());
	if (window < 1000000)
	{
		return false;
	}
	m->fps = frames * 1e6f / window;
	m->pico = 1.0f - (float) wait_us / window;
	if (status.window_us)
	{
		uint32_t n = status.window_frames ? status.window_frames : 1;
		m->gpu = (float) status.v3d_busy_us / status.window_us;
		m->arm = (float) status.arm_busy_us / status.window_us;
		m->render_ms = status.v3d_busy_us / 1000.0f / n;
		m->panel_ms = status.panel_wait_us / 1000.0f / n;
	}

	/* the Zero's numbers for the next second */
	pgpu_request_status ();
	pgpu_flush ();
	frames = 0;
	wait_us = 0;
	window_start = get_absolute_time ();
	return true;
}

/* ---- the panel ------------------------------------------------------------------ */

#define LINE		17		/* at scale 1 */
#define BAR_W		64
#define TEXT_CHARS	9		/* "PICO 100%" */

float hud_perf_width (float scale)
{
	return roundf ((6 + TEXT_CHARS * HUD_CHAR_W + 4 + BAR_W + 6) * scale);
}

static uint32_t load_color (float load)
{
	return load < 0.5f ? HUD_RGBA (60, 220, 90, 255)
	     : load < 0.8f ? HUD_RGBA (240, 200, 40, 255) : HUD_RGBA (240, 60, 50, 255);
}

void hud_perf (float x, float y, float scale, const perf_t *m)
{
	const uint32_t text = HUD_RGBA (235, 235, 235, 255);
	float left = x + roundf (6 * scale), bar_x = x + roundf ((6 + TEXT_CHARS * HUD_CHAR_W + 4) * scale);
	float bar_w = roundf (BAR_W * scale), bar_h = roundf (10 * scale);
	char s[24];
#define Y(line)	(y + roundf ((4 + (line) * LINE) * scale))

	hud_rect (x, y, hud_perf_width (scale), roundf ((6 * LINE + 6) * scale), HUD_RGBA (0, 0, 0, 150));
	snprintf (s, sizeof s, "FPS  %4.1f", m->fps);
	hud_text_scaled (left, Y (0), s, HUD_RGBA (255, 230, 120, 255), scale);

	const struct { const char *label; float load; } loads[3] =
	{
		{"GPU ", m->gpu}, {"ARM ", m->arm}, {"PICO", m->pico},
	};
	for (int i = 0; i < 3; i++)
	{
		float ly = Y (i + 1), load = loads[i].load < 1.0f ? loads[i].load : 1.0f;
		snprintf (s, sizeof s, "%s %3d%%", loads[i].label, (int) (loads[i].load * 100.0f + 0.5f));
		hud_text_scaled (left, ly, s, text, scale);
		hud_rect (bar_x, ly + roundf (2 * scale), bar_w, bar_h, HUD_RGBA (70, 70, 80, 200));
		hud_rect (bar_x, ly + roundf (2 * scale), roundf (bar_w * load), bar_h, load_color (load));
	}
	snprintf (s, sizeof s, "RENDER %4.1fms", m->render_ms);
	hud_text_scaled (left, Y (4), s, text, scale);
	snprintf (s, sizeof s, "PANEL  %4.1fms", m->panel_ms);
	hud_text_scaled (left, Y (5), s, text, scale);
#undef Y
}
