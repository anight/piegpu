/*
 * pgpu_perf.h - frame rate and load for the demos' HUD: the Pico's own numbers,
 * the Zero's from its STATUS reply (docs/protocol.md 9), once a second.
 */
#ifndef PGPU_PERF_H
#define PGPU_PERF_H

#include <stdbool.h>
#include <stdint.h>

typedef struct
{
	float fps;
	float gpu;				/* loads, 0..1: the V3D, */
	float cpu_g, cpu_h;			/* the GPU board's CPU (the Zero's ARM), the host's CPU */
	float render_ms, panel_ms;		/* V3D time and panel wait per frame */
} perf_t;

/* after each frame, with the time spent waiting for the frame pulse (the
   Pico is idle then); takes the replies (STATUS). true when *m has the
   numbers of a new second */
bool perf_frame (uint32_t wait_us, perf_t *m);

/* the perf panel into the HUD being built (hud_begin .. hud_end): at x, y,
   scale 1 (as glxgears' HUD) or 0.5 */
void hud_perf (float x, float y, float scale, const perf_t *m);
float hud_perf_width (float scale);

#endif
