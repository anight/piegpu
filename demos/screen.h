/*
 * screen.h - the demos follow the screen: its size changes when an HDMI
 * monitor is plugged in or out (the Zero's DISPLAY reply; pgl moves a
 * full-screen viewport to the new size at pglSwapBuffers).
 */
#ifndef SCREEN_H
#define SCREEN_H

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "gles/pgl.h"
#include "pgpu.h"

/* what the Zero shows on, on the console */
static inline void screen_print (const char *name, const pgpu_display_t *d)
{
	printf ("%s: screen %ux%u on %s", name, d->width, d->height,
		d->output == PGPU_OUTPUT_HDMI ? "HDMI" : "the panel");
	if (d->hdmi_connected && d->edid)
	{
		printf (", monitor \"%s\" %ux%u at %u.%02u Hz (HDMI sends %ux%u)", d->monitor_name,
			d->monitor_width, d->monitor_height, (unsigned) d->monitor_refresh_mhz / 1000,
			(unsigned) d->monitor_refresh_mhz % 1000 / 10, d->signal_width, d->signal_height);
	}
	else
	{
		printf (d->hdmi_connected ? ", monitor without EDID" : ", no monitor");
	}
	printf ("\n");
}

/* call once a frame (after pglSwapBuffers): the viewport in vp (x, y, width,
   height); true if it has changed (at the first call too) */
static inline bool screen_update (const char *name, GLint vp[4])
{
	static uint32_t displays_seen;
	pgpu_display_t d;
	uint32_t n = pgpu_get_display (&d);
	if (n != displays_seen)
	{
		displays_seen = n;
		screen_print (name, &d);
	}

	GLint v[4];
	glGetIntegerv (GL_VIEWPORT, v);
	if (memcmp (v, vp, sizeof v) == 0)
	{
		return false;
	}
	memcpy (vp, v, sizeof v);
	return true;
}

#endif
