/*
 * track.h - zerog's circuit: two routes of rings a few metres apart. The
 * main one is a closed loop; the other leaves it at a fork, cuts inside the
 * long curve and comes back to it. A ring is a frame on the centre line:
 * where it is, forward, right, up (banked); the road is HW each side of it.
 *
 * Nothing here draws: the race (race.c) flies over it, zerog.c makes its
 * geometry, sim.c tries it without a GPU.
 */
#ifndef ZEROG_TRACK_H
#define ZEROG_TRACK_H

#include <stdbool.h>
#include <stdint.h>

#define HW		8.0f		/* half the road's width, metres */
#define TILE_RINGS	4		/* rings a tile: a texture's length along the road, a pad */
#define OPEN_SEP	(2 * HW + 1.0f)	/* the two routes' middles closer than this: no wall between them */
#define MAIN_RINGS	704		/* at most */
#define ALT_RINGS	160

enum { ROUTE_MAIN, ROUTE_ALT, ROUTES };

enum					/* a ring's flags: of the road from it to the next */
{
	RING_GAP = 1,			/* no road: the jump */
	RING_OPEN_L = 2, RING_OPEN_R = 4,	/* no wall that side: the other route's road is there */
	RING_TUNNEL = 8,		/* roofed */
	RING_RAMP = 16			/* the ramp up to the jump */
};

enum { PAD_NONE, PAD_BOOST, PAD_WEAPON };	/* half a tile (left, right) */

typedef struct
{
	float p[3], t[3], r[3], u[3];	/* the centre line; forward, right, up */
	float k;			/* curvature, 1/m: > 0 turning right */
	float sep;			/* where the routes' roads meet: how far the other's centre line is */
	uint8_t flags;
} ring_t;

typedef struct
{
	float p[3], t[3], r[3], u[3], k;
} frame_t;

typedef struct
{
	ring_t *rings;
	int n;				/* rings: the loop's, or the fork's (and one more at its end) */
	float step, len;
	bool loop;
	uint8_t (*pads)[2];		/* PAD_*: each tile's left and right half */
	float from, to;			/* the fork's: where on the main route it leaves and rejoins */
	float *main_s;			/* the fork's: each ring's place along the main route */
	int side;			/* the fork's: 1 right of the main route, -1 left */
} route_t;

extern route_t route[ROUTES];

/* what's where on the circuit, for the scenery and the console */
typedef struct
{
	int ramp_first, gap_first, gap_rings;
	int tunnel_first, tunnel_rings;
	float jump_drop;		/* the lip above the landing, m */
} track_features_t;

extern track_features_t track_features;

void track_build (void);

/* the frame at s along a route (between the rings) */
void track_frame (int r, float s, frame_t *f);
const ring_t *track_ring (int r, float s);
int track_pad (int r, float s, float x);		/* PAD_* under there */

/* a place on the circuit: the route and how far along it */
typedef struct
{
	int route;
	float s;
} where_t;

/* w moved to where p is (from near it); the frame there and p's place in it:
   x across, h above the road's plane. laps: +1 when the line is crossed, -1
   back over it */
void track_locate (where_t *w, const float p[3], frame_t *f, float *x, float *h, int *laps);

/* how far across the road goes there (to the walls), and is there road at x? */
void track_limits (const where_t *w, float *lo, float *hi);
bool track_road (const where_t *w, float x);

/* the place dist further on, taking the fork or not */
where_t track_ahead (where_t w, float dist, bool fork);

/* how far round the loop a place is (a place on the fork: where it is beside) */
float track_progress (const where_t *w);

#endif
