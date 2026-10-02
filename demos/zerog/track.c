/*
 * track.c - zerog's circuit (track.h): a closed Catmull-Rom spline through
 * CONTROL points, cut into rings and banked into its curves, as antigrav's;
 * then what antigrav's hasn't:
 *
 *   the jump   the road climbs a ramp to a lip, there is none for GAP_RINGS,
 *              and it goes on lower: fast enough, a craft flies over
 *   the fork   a second road leaves the long curve on its inside, cuts it
 *              short through a dip and a few bends, and joins it again.
 *              It is the main road moved aside (and down): where it leaves
 *              and where it comes back the two lie in one plane and have no
 *              wall between them
 *   the pads   half a tile each: speed (the chevrons) and weapons
 */
#include "track.h"
#include <stdlib.h>
#include "vec.h"

#define RING_STEP	4.0f		/* about: the rings are spaced evenly */
#define SUB		48		/* spline samples a span between control points */
#define BANK_MAX	0.34f		/* radians */

/* the jump: where, and its shape */
#define JUMP_CONTROL	7		/* the lip this far after that control point */
#define JUMP_AFTER	96.0f
#define RAMP_RINGS	16
#define RAMP_RISE	3.2f		/* the lip above the road's own line (the landing is on the line) */
#define GAP_RINGS	8

/* the fork: from before one control point to after another, on the right */
#define FORK_FROM	3
#define FORK_FROM_BACK	40.0f
#define FORK_TO		6
#define FORK_TO_ON	10.0f
#define FORK_SIDE	1
#define FORK_ASIDE	36.0f		/* how far inside the main road it runs */
#define FORK_WEAVE	7.0f		/* and the bends it makes there */
#define FORK_BENDS	3.0f
#define FORK_DIP	5.0f		/* how far down, in the middle */
#define FORK_EASE	0.26f		/* of its length: leaving, and coming back */

/* the tunnel */
#define TUNNEL_CONTROL	11
#define TUNNEL_RINGS	36

/* control points: x, z, height (the start line at the first) */
static const float control[][3] =
{
	{0, 0, 3}, {230, 0, 3}, {420, 40, 7}, {540, 170, 14}, {540, 360, 20}, {440, 500, 15}, {270, 560, 10},
	{130, 560, 8}, {-200, 540, 6}, {-325, 450, 5}, {-325, 320, 4}, {-400, 200, 4}, {-420, 60, 8}, {-330, -30, 6},
	{-160, -8, 3},
};
#define CONTROL		(int) (sizeof control / sizeof control[0])

static ring_t main_rings[MAIN_RINGS], alt_rings[ALT_RINGS + 1];
static uint8_t main_pads[MAIN_RINGS / TILE_RINGS][2], alt_pads[ALT_RINGS / TILE_RINGS + 1][2];
static float alt_main_s[ALT_RINGS + 1];
static float bank[MAIN_RINGS];
static int control_ring[CONTROL];

route_t route[ROUTES] =
{
	{.rings = main_rings, .loop = true, .pads = main_pads},
	{.rings = alt_rings, .pads = alt_pads, .main_s = alt_main_s, .side = FORK_SIDE},
};
track_features_t track_features;

static const float up[3] = {0, 1, 0};

static void catmull (const float *p0, const float *p1, const float *p2, const float *p3, float t, float *out)
{
	for (int i = 0; i < 3; i++)
	{
		out[i] = 0.5f * (2 * p1[i] + (p2[i] - p0[i]) * t + (2 * p0[i] - 5 * p1[i] + 4 * p2[i] - p3[i]) * t * t
				 + (3 * p1[i] - p0[i] - 3 * p2[i] + p3[i]) * t * t * t);
	}
}

static int wrap (int i, int n)	{ return ((i % n) + n) % n; }

/* ---- the main route --------------------------------------------------------------- */

static void build_main (void)
{
	route_t *m = &route[ROUTE_MAIN];

	/* the spline, sampled densely, and the length along the samples */
	static float dense[CONTROL * SUB + 1][3], along[CONTROL * SUB + 1];
	float pts[CONTROL][3];
	for (int i = 0; i < CONTROL; i++)
	{
		v_set (pts[i], control[i][0], control[i][2], control[i][1]);
	}
	int n = CONTROL * SUB;
	for (int i = 0; i < CONTROL; i++)
		for (int j = 0; j < SUB; j++)
		{
			catmull (pts[(i + CONTROL - 1) % CONTROL], pts[i], pts[(i + 1) % CONTROL],
				 pts[(i + 2) % CONTROL], (float) j / SUB, dense[i * SUB + j]);
		}
	v_copy (dense[n], dense[0]);
	along[0] = 0.0f;
	for (int i = 1; i <= n; i++)
	{
		float d[3];
		v_sub (d, dense[i], dense[i - 1]);
		along[i] = along[i - 1] + v_len (d);
	}
	m->len = along[n];

	/* rings evenly along it, a whole number of tiles */
	m->n = TILE_RINGS * (int) lroundf (m->len / (RING_STEP * TILE_RINGS));
	m->n = m->n > MAIN_RINGS ? MAIN_RINGS : m->n;
	m->step = m->len / m->n;
	ring_t *g = m->rings;
	for (int i = 0, j = 0; i < m->n; i++)
	{
		float s = i * m->step;
		while (j + 1 < n && along[j + 1] < s)
		{
			j++;
		}
		v_mix (g[i].p, dense[j], dense[j + 1], (s - along[j]) / (along[j + 1] - along[j]));
	}
	for (int i = 0; i < CONTROL; i++)
	{
		control_ring[i] = (int) lroundf (along[i * SUB] / m->step) % m->n;
	}

	/* the jump: the ramp up to the lip (steepest at the lip), the gap (its
	   rings come down to the landing: nothing is there) */
	int lip = control_ring[JUMP_CONTROL] + (int) (JUMP_AFTER / m->step);
	lip -= lip % TILE_RINGS;
	for (int i = 0; i <= RAMP_RINGS; i++)
	{
		float q = (float) i / RAMP_RINGS;
		g[lip - RAMP_RINGS + i].p[1] += RAMP_RISE * q * q;
		if (i < RAMP_RINGS)
		{
			g[lip - RAMP_RINGS + i].flags |= RING_RAMP;
		}
	}
	for (int i = 0; i < GAP_RINGS; i++)
	{
		g[lip + i].flags |= RING_GAP;
		if (i)
		{
			g[lip + i].p[1] += RAMP_RISE * (1.0f - (float) i / GAP_RINGS);
		}
	}
	track_features.ramp_first = lip - RAMP_RINGS;
	track_features.gap_first = lip;
	track_features.gap_rings = GAP_RINGS;
	track_features.jump_drop = RAMP_RISE;

	/* the frames: forward (the lip's from the ramp, the landing's from what
	   follows), the level right (in r for now), the curvature */
	for (int i = 0; i < m->n; i++)
	{
		const float *a = g[i == lip + GAP_RINGS ? i : wrap (i - 1, m->n)].p, *b = g[i == lip ? i : wrap (i + 1, m->n)].p;
		v_sub (g[i].t, b, a);
		v_norm (g[i].t);
		v_cross (g[i].r, g[i].t, up);
		v_norm (g[i].r);
	}
	for (int i = 0; i < m->n; i++)
	{
		const float *a = g[wrap (i - 1, m->n)].t, *b = g[wrap (i + 1, m->n)].t;
		float d[3];
		v_sub (d, b, a);
		g[i].k = v_dot (d, g[i].r) / (2 * m->step);
	}

	/* banked into the curves (the curvature smoothed over 50 m), the right
	   side down in a right-hand curve */
	int half = (int) (25.0f / m->step);
	for (int i = 0; i < m->n; i++)
	{
		float k = 0.0f;
		for (int j = -half; j <= half; j++)
		{
			k += g[wrap (i + j, m->n)].k;
		}
		k /= 2 * half + 1;
		bank[i] = clampf (atanf (k * 800.0f / 9.81f), -BANK_MAX, BANK_MAX);
	}
	for (int i = 0; i < m->n; i++)
	{
		float level[3], u0[3];
		v_copy (level, g[i].r);
		v_cross (u0, level, g[i].t);
		for (int c = 0; c < 3; c++)
		{
			g[i].r[c] = level[c] * cosf (bank[i]) - u0[c] * sinf (bank[i]);
		}
		v_cross (g[i].u, g[i].r, g[i].t);
	}

	/* the tunnel */
	track_features.tunnel_first = control_ring[TUNNEL_CONTROL] - TUNNEL_RINGS / 2;
	track_features.tunnel_first -= track_features.tunnel_first % TILE_RINGS;
	track_features.tunnel_rings = TUNNEL_RINGS;
	for (int i = 0; i < TUNNEL_RINGS; i++)
	{
		g[wrap (track_features.tunnel_first + i, m->n)].flags |= RING_TUNNEL;
	}
}

/* ---- the fork ------------------------------------------------------------------------ */

/* how far aside (and down) the fork runs, at u (0 .. 1) of its way */
static float fork_aside (float u)
{
	float w = smooth (0.0f, FORK_EASE, u) * (1.0f - smooth (1.0f - FORK_EASE, 1.0f, u));
	return FORK_ASIDE * w + FORK_WEAVE * w * w * sinf (2 * PI * FORK_BENDS * (u - 0.5f));
}

static float fork_open_u;		/* where it's clear of the main road: of its way, */
static float fork_open_s;		/* and along the main road */

static float fork_down (float u)
{
	float a = fork_open_u, b = fork_open_u + 0.22f;
	return FORK_DIP * smooth (a, b, u) * (1.0f - smooth (1.0f - b, 1.0f - a, u));
}

static void build_alt (void)
{
	route_t *m = &route[ROUTE_MAIN], *a = &route[ROUTE_ALT];
	int ring_a = control_ring[FORK_FROM] - (int) (FORK_FROM_BACK / m->step);
	int ring_b = control_ring[FORK_TO] + (int) (FORK_TO_ON / m->step);
	a->from = ring_a * m->step;
	a->to = ring_b * m->step;

	for (fork_open_u = 0.0f; fork_open_u < 0.5f && fork_aside (fork_open_u) < OPEN_SEP + 1.5f; fork_open_u += 0.002f)
	{
	}
	fork_open_s = a->from + (a->to - a->from) * fork_open_u;

	/* the main road moved aside: in its (banked) plane while they touch, level
	   beyond; sampled a metre apart, and the length along the samples */
	enum { SAMPLES = 4 * (MAIN_RINGS / 2) };
	static float pt[SAMPLES + 1][3], along[SAMPLES + 1];
	int n = (ring_b - ring_a) * 4;
	n = n > SAMPLES ? SAMPLES : n;
	for (int j = 0; j <= n; j++)
	{
		float u = (float) j / n, dx = fork_aside (u), in_plane = fminf (dx, OPEN_SEP + 1.5f);
		frame_t f;
		track_frame (ROUTE_MAIN, a->from + (a->to - a->from) * u, &f);
		float level[3] = {f.r[0], 0.0f, f.r[2]};
		v_norm (level);
		v_mad (pt[j], f.p, f.r, FORK_SIDE * in_plane);
		v_mad (pt[j], pt[j], level, FORK_SIDE * (dx - in_plane));
		pt[j][1] -= fork_down (u);
		if (j)
		{
			float d[3];
			v_sub (d, pt[j], pt[j - 1]);
			along[j] = along[j - 1] + v_len (d);
		}
	}
	a->len = along[n];
	a->n = TILE_RINGS * (int) lroundf (a->len / (RING_STEP * TILE_RINGS));
	a->n = a->n > ALT_RINGS ? ALT_RINGS : a->n;
	a->step = a->len / a->n;
	ring_t *g = a->rings;
	for (int i = 0, j = 0; i <= a->n; i++)
	{
		float s = i * a->step;
		while (j + 1 < n && along[j + 1] < s)
		{
			j++;
		}
		float w = clampf ((s - along[j]) / (along[j + 1] - along[j]), 0.0f, 1.0f), u = (j + w) / n;
		v_mix (g[i].p, pt[j], pt[j + 1], w);
		a->main_s[i] = a->from + (a->to - a->from) * u;
		g[i].sep = fork_aside (u);
		g[i].flags = g[i].sep < OPEN_SEP ? (FORK_SIDE > 0 ? RING_OPEN_L : RING_OPEN_R) : 0;
	}

	/* the frames: at its ends the main road's; banked as the main road
	   beside it while they touch, into its own bends beyond */
	frame_t f;
	for (int i = 0; i <= a->n; i++)
	{
		if (i == 0 || i == a->n)
		{
			track_frame (ROUTE_MAIN, i ? a->to : a->from, &f);
			v_copy (g[i].t, f.t);
		}
		else
		{
			v_sub (g[i].t, g[i + 1].p, g[i - 1].p);
			v_norm (g[i].t);
		}
		v_cross (g[i].r, g[i].t, up);
		v_norm (g[i].r);
	}
	for (int i = 0; i <= a->n; i++)
	{
		float d[3];
		v_sub (d, g[i < a->n ? i + 1 : i].t, g[i ? i - 1 : i].t);
		g[i].k = v_dot (d, g[i].r) / ((i && i < a->n ? 2 : 1) * a->step);
	}
	int half = (int) (16.0f / a->step);
	for (int i = 0; i <= a->n; i++)
	{
		float k = 0.0f, level[3], u0[3];
		for (int j = -half; j <= half; j++)
		{
			k += g[i + j < 0 ? 0 : i + j > a->n ? a->n : i + j].k;
		}
		k /= 2 * half + 1;
		float own = clampf (atanf (k * 800.0f / 9.81f), -BANK_MAX, BANK_MAX);
		float beside = bank[wrap ((int) lroundf (a->main_s[i] / m->step), m->n)];
		float b = beside + (own - beside) * smooth (OPEN_SEP + 1.5f, OPEN_SEP + 14.0f, g[i].sep);
		v_copy (level, g[i].r);
		v_cross (u0, level, g[i].t);
		for (int c = 0; c < 3; c++)
		{
			g[i].r[c] = level[c] * cosf (b) - u0[c] * sinf (b);
		}
		v_cross (g[i].u, g[i].r, g[i].t);
	}

	/* the main road's rings beside it: no wall where they touch */
	for (int i = ring_a; i < ring_b; i++)
	{
		float sep = fork_aside ((float) (i - ring_a) / (ring_b - ring_a));
		float next = fork_aside ((float) (i + 1 - ring_a) / (ring_b - ring_a));
		if (fminf (sep, next) < OPEN_SEP)
		{
			m->rings[i].sep = sep;
			m->rings[i].flags |= FORK_SIDE > 0 ? RING_OPEN_R : RING_OPEN_L;
		}
	}
	for (int i = 0; i < a->n; i++)			/* (a ring's flags are the road's on to the next) */
	{
		if (!(g[i].flags & (RING_OPEN_L | RING_OPEN_R)) && g[i + 1].sep < OPEN_SEP)
		{
			g[i].flags |= FORK_SIDE > 0 ? RING_OPEN_L : RING_OPEN_R;
		}
	}
}

/* ---- the pads -------------------------------------------------------------------------- */

static bool plain (int tile, float kmax, int tiles_on)
{
	const route_t *m = &route[ROUTE_MAIN];
	for (int i = tile * TILE_RINGS; i < (tile + tiles_on) * TILE_RINGS; i++)
	{
		const ring_t *g = &m->rings[wrap (i, m->n)];
		if (g->flags & (RING_GAP | RING_OPEN_L | RING_OPEN_R) || fabsf (g->k) > kmax)
		{
			return false;
		}
	}
	return true;
}

static void place_pads (void)
{
	route_t *m = &route[ROUTE_MAIN];
	int tiles = m->n / TILE_RINGS, side = 0;
	float last = -1e9f;

	/* speed: where the road runs straight for 50 m, 230 m apart at least;
	   and both halves before the ramp */
	int ramp = track_features.ramp_first / TILE_RINGS - 2;
	m->pads[ramp][0] = m->pads[ramp][1] = PAD_BOOST;
	for (int tile = 4; tile < tiles - 4; tile++)
	{
		float s = tile * TILE_RINGS * m->step;
		if (plain (tile, 1.0f / 260, 3) && s - last > 230.0f && abs (tile - ramp) > 30 && !m->pads[tile][0])
		{
			m->pads[tile][side] = PAD_BOOST;
			side ^= 1;
			last = s;
		}
	}
	/* weapons: 520 m apart, clear of the speed pads, the other half each time */
	last = -1e9f;
	for (int tile = 14; tile < tiles - 6; tile++)
	{
		float s = tile * TILE_RINGS * m->step;
		bool clear = true;
		for (int j = -3; j <= 3; j++)
		{
			clear = clear && !m->pads[wrap (tile + j, tiles)][0] && !m->pads[wrap (tile + j, tiles)][1];
		}
		if (clear && plain (tile, 1.0f / 120, 1) && !(m->rings[tile * TILE_RINGS].flags & RING_RAMP) && s - last > 520.0f)
		{
			m->pads[tile][side] = PAD_WEAPON;
			side ^= 1;
			last = s;
		}
	}
}

void track_build (void)
{
	build_main ();
	build_alt ();
	place_pads ();
}

/* ---- places -------------------------------------------------------------------------- */

static float wrap_s (const route_t *m, float s)
{
	if (m->loop)
	{
		s = fmodf (s, m->len);
		return s < 0.0f ? s + m->len : s;
	}
	return clampf (s, 0.0f, m->len);
}

void track_frame (int r, float s, frame_t *f)
{
	const route_t *m = &route[r];
	s = wrap_s (m, s);
	int i = (int) (s / m->step);
	i = i >= m->n ? m->n - 1 : i;
	float w = s / m->step - i;
	const ring_t *a = &m->rings[i], *b = &m->rings[m->loop ? (i + 1) % m->n : i + 1];
	v_mix (f->p, a->p, b->p, w);
	v_mix (f->t, a->t, b->t, w);
	v_mix (f->r, a->r, b->r, w);
	v_mix (f->u, a->u, b->u, w);
	v_norm (f->t);
	v_norm (f->r);
	v_norm (f->u);
	f->k = a->k + (b->k - a->k) * w;
}

const ring_t *track_ring (int r, float s)
{
	const route_t *m = &route[r];
	int i = (int) (wrap_s (m, s) / m->step);
	return &m->rings[i >= m->n ? m->n - 1 : i];
}

int track_pad (int r, float s, float x)
{
	const route_t *m = &route[r];
	int i = (int) (wrap_s (m, s) / m->step);
	i = i >= m->n ? m->n - 1 : i;
	return m->pads[i / TILE_RINGS][x > 0.0f];
}

/* the fork's place beside a place on the main route, and back */
static float alt_beside (float main_s)
{
	const route_t *a = &route[ROUTE_ALT];
	return a->len * clampf ((main_s - a->from) / (a->to - a->from), 0.0f, 1.0f);
}

static float main_beside (float alt_s)
{
	const route_t *a = &route[ROUTE_ALT];
	float i = clampf (alt_s / a->step, 0.0f, (float) a->n);
	int j = (int) i >= a->n ? a->n - 1 : (int) i;
	return a->main_s[j] + (a->main_s[j + 1] - a->main_s[j]) * (i - j);
}

/* p's place in the frame at w, w moved along to it */
static void settle (where_t *w, const float p[3], frame_t *f, float *x, float *h, int *laps)
{
	for (int pass = 0; pass < 3; pass++)
	{
		float d[3];
		const route_t *m = &route[w->route];
		track_frame (w->route, w->s, f);
		v_sub (d, p, f->p);
		*x = v_dot (d, f->r);
		*h = v_dot (d, f->u);
		if (pass == 2)
		{
			break;
		}
		w->s += v_dot (d, f->t) / clampf (1.0f - f->k * *x, 0.6f, 1.6f);
		if (m->loop)
		{
			if (w->s >= m->len)
			{
				w->s -= m->len;
				*laps += 1;
			}
			else if (w->s < 0.0f)
			{
				w->s += m->len;
				*laps -= 1;
			}
		}
		else if (w->s < 0.0f)			/* off the fork's ends: on the main road */
		{
			w->s = m->from + w->s;
			w->route = ROUTE_MAIN;
		}
		else if (w->s > m->len)
		{
			w->s = m->to + (w->s - m->len);
			w->route = ROUTE_MAIN;
		}
	}
}

void track_locate (where_t *w, const float p[3], frame_t *f, float *x, float *h, int *laps)
{
	int none = 0;
	laps = laps ? laps : &none;
	settle (w, p, f, x, h, laps);

	/* where the two roads touch: on the one whose middle is nearer */
	if (track_ring (w->route, w->s)->flags & (RING_OPEN_L | RING_OPEN_R))
	{
		where_t o = {!w->route, w->route == ROUTE_MAIN ? alt_beside (w->s) : main_beside (w->s)};
		frame_t of;
		float ox, oh;
		int olaps = 0;
		settle (&o, p, &of, &ox, &oh, &olaps);
		if (o.route != w->route && fabsf (ox) < fabsf (*x) - 0.25f)
		{
			*w = o;
			*f = of;
			*x = ox;
			*h = oh;
		}
	}
}

void track_limits (const where_t *w, float *lo, float *hi)
{
	const ring_t *g = track_ring (w->route, w->s);
	*lo = g->flags & RING_OPEN_L ? -(HW + g->sep) : -HW;
	*hi = g->flags & RING_OPEN_R ? HW + g->sep : HW;
}

bool track_road (const where_t *w, float x)
{
	float lo, hi;
	track_limits (w, &lo, &hi);
	return !(track_ring (w->route, w->s)->flags & RING_GAP) && x > lo - 0.3f && x < hi + 0.3f;
}

where_t track_ahead (where_t w, float dist, bool fork)
{
	const route_t *m = &route[ROUTE_MAIN], *a = &route[ROUTE_ALT];
	if (w.route == ROUTE_ALT)
	{
		w.s += dist;
		if (w.s > a->len)
		{
			w.route = ROUTE_MAIN;
			w.s = wrap_s (m, a->to + (w.s - a->len));
		}
		return w;
	}
	float on = w.s + dist;
	if (fork && w.s < fork_open_s && on > a->from)
	{
		w.route = ROUTE_ALT;
		w.s = fminf (alt_beside (on), a->len);
		return w;
	}
	w.s = wrap_s (m, on);
	return w;
}

float track_progress (const where_t *w)
{
	return w->route == ROUTE_MAIN ? w->s : main_beside (w->s);
}
