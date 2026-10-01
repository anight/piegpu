/*
 * pgpu_perf.h - frame rate and load for the demos' HUD: the Pico's own numbers,
 * the RPi's from its STATUS reply (docs/protocol.md 9), and the link's
 * (pgpu_get_link_counts), once a second.
 */
#ifndef PGPU_PERF_H
#define PGPU_PERF_H

#include <stdbool.h>
#include <stdint.h>

typedef struct
{
	float fps;
	float gpu;				/* loads, 0..1: the V3D, */
	float cpu_g, cpu_h;			/* the GPU board's CPU (the RPi's ARM), the host's CPU */
	float render_ms, panel_ms;		/* V3D time and panel wait per frame */
	float link_tx, link_rx;			/* the link's bytes a second: to the RPi, back */
	float link_use;				/* 0..1: to the RPi, of the link's capacity (< 0: not known) */
	float link_wait;			/* 0..1: the host's time held back by the link */
} perf_t;

/* after each frame, with the time spent waiting for the frame pulse (the
   Pico is idle then); takes the replies (STATUS). true when *m has the
   numbers of a new second */
bool perf_frame (uint32_t wait_us, perf_t *m);

/* the perf panel into the HUD being built (hud_begin .. hud_end): at x, y,
   scale 1 (as glxgears' HUD) or 0.5: the frame rate, the loads, the render
   and panel times, the host's link */
void hud_perf (float x, float y, float scale, const perf_t *m);
float hud_perf_width (float scale);

/* the link's numbers (bytes a second each way, its use, the host's time held
   back by it) as a line of the log: "who: link: ..." */
void perf_log_link (const char *who, const perf_t *m);

#endif
