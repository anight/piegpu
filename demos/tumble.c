/*
 * tumble - 2D physics: balls and boxes in a box that the stick tilts. Rigid
 * bodies with mass, spin and friction, stacking and rolling; each meeting is
 * heard (the RPi mixes the sounds: pgpu.h's sound effects), louder the harder
 * it was, higher for a small body and lower for a big one, on the side it
 * happened.
 *
 * The stick (engine/pad.h: the Pico's analog stick and its game controller)
 * says which way is down: left and right tilt the box, forward lifts
 * everything to the top. A adds a ball, B a box, X or the stick pressed kicks
 * them all from the middle, Y starts over. A finger on the panel puts a ball
 * where it touches. The console's keys: a d w s tilt, b a ball, x a box, the
 * space bar kicks, r starts over. Left alone for 10 s the box rocks by itself.
 *
 * The physics, made here: every pair of bodies near each other is tested
 * (balls by their distance, boxes by their separating axes, clipped to the
 * points they touch at: Box2D Lite's way), and the contacts are solved one
 * after another, a few times over, by impulses along the normal (never
 * pulling) and across it (friction, up to its share of the normal's), with a
 * push apart where they've sunk into each other and a bounce where they met
 * fast. Two steps a frame.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pico/stdlib.h"
#include "gles/pgl.h"
#include "pgpu.h"
#include "hud.h"
#include "pgpu_perf.h"
#include "screen.h"
#include "pad.h"
#include "tumble_program.h"
#include "tumble_sounds.h"

#define WORLD_W		16.0f		/* the box's width; its height by the screen's shape */
#define MAX_BODIES	40
#define FIRST_BODIES	14
#define MAX_CONTACTS	200
#define GRAVITY		24.0f
#define STEPS		2		/* a frame */
#define ITERATIONS	8
#define SLOP		0.02f		/* how far bodies may sink into each other */
#define PUSH		0.2f		/* of what they've sunk beyond that, taken back a step */
#define FRICTION	0.45f
#define BOUNCE_OVER	1.5f		/* meeting faster than this, they bounce */
#define IDLE_S		10.0f		/* without input: the box rocks by itself */
#define KEY_HOLD_S	0.3f		/* a console key holds this long (a terminal sends no key-up) */

typedef struct
{
	float x, y, a;			/* where, turned how far */
	float vx, vy, w;		/* its speed, its spin */
	float im, ii;			/* 1 / its mass, 1 / its moment of inertia (0: it can't be moved) */
	float hx, hy;			/* a box's half extents; a ball's radius in both */
	float bounce;
	float heard;			/* when its last sound was */
	bool ball;
	uint8_t color;
} body_t;

typedef struct
{
	int a, b;			/* the bodies (b may be the box itself: n_bodies) */
	float px, py, nx, ny;		/* where, and the normal from a to b */
	float sunk;			/* how deep */
	float rax, ray, rbx, rby;	/* from each body's middle */
	float mn, mt, bias;		/* the solver's: the masses along and across, the speed asked for */
	float pn, pt;			/* the impulses so far */
} contact_t;

static body_t body[MAX_BODIES + 1];	/* (one more: the box they're in) */
static int n_bodies;
static contact_t contact[MAX_CONTACTS];
static int n_contacts;
static float world_h = 12.0f;
static float now;

static const uint8_t colors[][3] =
{
	{235, 80, 70}, {245, 160, 50}, {245, 215, 70}, {110, 200, 90}, {70, 190, 200}, {90, 140, 240}, {170, 110, 230},
	{235, 120, 180},
};
#define COLORS		(int) (sizeof colors / sizeof colors[0])

static uint32_t rng = 0x2F6E2B1u;
static float frand (void)		/* 0 .. 1 */
{
	rng ^= rng << 13;
	rng ^= rng >> 17;
	rng ^= rng << 5;
	return (rng >> 8) / 16777216.0f;
}

static float clampf (float x, float lo, float hi)	{ return x < lo ? lo : x > hi ? hi : x; }

/* ---- the sound ---------------------------------------------------------------------- */

#define HIT_CHANNELS	8		/* the meetings', in turn; then the others' */
#define CH_POP		8
#define CH_BOOM		9
#define HITS_A_FRAME	3
#define HIT_PAUSE	0.07f		/* a body's sounds are this far apart at least */
#define HIT_OVER	2.2f		/* a meeting slower than this isn't heard (bodies resting, sliding) */

static unsigned hit_channel, hits_now, hits_heard;

static void play (unsigned channel, int sound, float volume, float pan, float pitch)
{
	float l = clampf (volume * fminf (1.0f, 1.0f - pan), 0.0f, 1.0f), r = clampf (volume * fminf (1.0f, 1.0f + pan), 0.0f, 1.0f);
	pgpu_sound_play (channel, sound, (uint32_t) (l * PGPU_SOUND_FULL), (uint32_t) (r * PGPU_SOUND_FULL), 0);
	pgpu_sound_pitch (channel, pitch);
}

/* two bodies met at this speed */
static void hit (contact_t *c, float speed)
{
	body_t *a = &body[c->a], *b = &body[c->b];
	bool wall = c->b == n_bodies;
	if (speed < HIT_OVER || hits_now >= HITS_A_FRAME || now - a->heard < HIT_PAUSE || (!wall && now - b->heard < HIT_PAUSE))
	{
		return;
	}
	a->heard = b->heard = now;
	hits_now++;
	hits_heard++;
	float size = wall ? a->hx + a->hy : fminf (a->hx + a->hy, b->hx + b->hy);	/* (the smaller one's note) */
	int sound = wall ? TSND_THUD : a->ball && b->ball ? TSND_CLACK : TSND_THOCK;
	play (hit_channel++ % HIT_CHANNELS, sound, 0.10f + 0.80f * clampf ((speed - HIT_OVER) / 18.0f, 0.0f, 1.0f),
	      clampf (c->px / WORLD_W * 2.0f - 1.0f, -0.9f, 0.9f), clampf (1.75f - 0.62f * size, 0.6f, 1.6f) * (0.96f + 0.08f * frand ()));
}

/* ---- the bodies --------------------------------------------------------------------- */

static void add_body (bool ball, float x, float y)
{
	if (n_bodies == MAX_BODIES)
	{
		return;
	}
	body[n_bodies + 1] = body[n_bodies];		/* (the box moves up) */
	body_t *b = &body[n_bodies++];
	memset (b, 0, sizeof *b);
	b->ball = ball;
	b->hx = ball ? 0.38f + 0.45f * frand () : 0.42f + 0.50f * frand ();
	b->hy = ball ? b->hx : 0.42f + 0.50f * frand ();
	b->x = clampf (x, b->hx, WORLD_W - b->hx);
	b->y = clampf (y, b->hy, world_h - b->hy);
	b->a = ball ? 0.0f : (frand () - 0.5f);
	b->w = (frand () - 0.5f) * 4.0f;
	float mass = ball ? 3.1416f * b->hx * b->hx : 4.0f * b->hx * b->hy;
	b->im = 1.0f / mass;
	b->ii = 1.0f / (ball ? mass * b->hx * b->hx * 0.5f : mass * (b->hx * b->hx + b->hy * b->hy) / 3.0f);
	b->bounce = ball ? 0.45f : 0.2f;
	b->color = (uint8_t) (frand () * COLORS);
	b->heard = -1.0f;
}

static void start_over (void)
{
	n_bodies = 0;
	memset (&body[0], 0, sizeof body[0]);		/* the box: it can't be moved */
	body[0].bounce = 0.3f;
	for (int i = 0; i < FIRST_BODIES; i++)
	{
		add_body (i % 2 == 0, 1.5f + (WORLD_W - 3.0f) * frand (), world_h * (0.35f + 0.6f * frand ()));
	}
}

/* everything thrown from a point */
static void kick (float x, float y)
{
	for (int i = 0; i < n_bodies; i++)
	{
		body_t *b = &body[i];
		float dx = b->x - x, dy = b->y - y, d = sqrtf (dx * dx + dy * dy) + 0.3f;
		float push = 42.0f / (1.0f + 0.25f * d);
		b->vx += dx / d * push;
		b->vy += dy / d * push + 6.0f;
		b->w += (frand () - 0.5f) * 12.0f;
	}
	play (CH_BOOM, TSND_BOOM, 0.85f, clampf (x / WORLD_W * 2.0f - 1.0f, -0.8f, 0.8f), 1.0f);
}

/* ---- where they touch --------------------------------------------------------------- */

static void touch (int a, int b, float px, float py, float nx, float ny, float sunk)
{
	if (n_contacts < MAX_CONTACTS)
	{
		contact_t *c = &contact[n_contacts++];
		memset (c, 0, sizeof *c);
		c->a = a;
		c->b = b;
		c->px = px;
		c->py = py;
		c->nx = nx;
		c->ny = ny;
		c->sunk = sunk;
	}
}

static void ball_ball (int ia, int ib)
{
	const body_t *a = &body[ia], *b = &body[ib];
	float dx = b->x - a->x, dy = b->y - a->y, r = a->hx + b->hx, d2 = dx * dx + dy * dy;
	if (d2 >= r * r)
	{
		return;
	}
	float d = sqrtf (d2), nx = d > 1e-5f ? dx / d : 1.0f, ny = d > 1e-5f ? dy / d : 0.0f;
	touch (ia, ib, a->x + nx * a->hx, a->y + ny * a->hx, nx, ny, r - d);
}

/* (the box first) */
static void box_ball (int ia, int ib)
{
	const body_t *a = &body[ia], *b = &body[ib];
	float c = cosf (a->a), s = sinf (a->a), dx = b->x - a->x, dy = b->y - a->y;
	float lx = c * dx + s * dy, ly = -s * dx + c * dy;			/* the ball, as the box sees it */
	float qx = clampf (lx, -a->hx, a->hx), qy = clampf (ly, -a->hy, a->hy);	/* the box's point nearest to it */
	float ex = lx - qx, ey = ly - qy, d2 = ex * ex + ey * ey, nx, ny, sunk;
	if (d2 >= b->hx * b->hx)
	{
		return;
	}
	if (d2 > 1e-10f)
	{
		float d = sqrtf (d2);
		nx = ex / d;
		ny = ey / d;
		sunk = b->hx - d;
	}
	else							/* its middle is inside: out by the nearest side */
	{
		float ox = a->hx - fabsf (lx), oy = a->hy - fabsf (ly);
		nx = ox < oy ? (lx > 0 ? 1.0f : -1.0f) : 0.0f;
		ny = ox < oy ? 0.0f : (ly > 0 ? 1.0f : -1.0f);
		qx = ox < oy ? nx * a->hx : lx;
		qy = ox < oy ? ly : ny * a->hy;
		sunk = b->hx + fminf (ox, oy);
	}
	touch (ia, ib, a->x + c * qx - s * qy, a->y + s * qx + c * qy, c * nx - s * ny, s * nx + c * ny, sunk);
}

typedef struct { float x, y; } vec_t;

/* a segment's part behind a line */
static int clip (vec_t out[2], const vec_t in[2], float nx, float ny, float offset)
{
	int n = 0;
	float d0 = nx * in[0].x + ny * in[0].y - offset, d1 = nx * in[1].x + ny * in[1].y - offset;
	if (d0 <= 0.0f)
	{
		out[n++] = in[0];
	}
	if (d1 <= 0.0f)
	{
		out[n++] = in[1];
	}
	if (d0 * d1 < 0.0f)
	{
		float t = d0 / (d0 - d1);
		out[n].x = in[0].x + t * (in[1].x - in[0].x);
		out[n++].y = in[0].y + t * (in[1].y - in[0].y);
	}
	return n;
}

/* the edge of a box that faces a normal the most */
static void facing_edge (vec_t e[2], const body_t *b, float c, float s, float nx, float ny)
{
	float lx = -(c * nx + s * ny), ly = -(-s * nx + c * ny);		/* the normal, as the box sees it, turned round */
	if (fabsf (lx) > fabsf (ly))
	{
		e[0] = lx > 0 ? (vec_t) {b->hx, -b->hy} : (vec_t) {-b->hx, b->hy};
		e[1] = lx > 0 ? (vec_t) {b->hx, b->hy} : (vec_t) {-b->hx, -b->hy};
	}
	else
	{
		e[0] = ly > 0 ? (vec_t) {b->hx, b->hy} : (vec_t) {-b->hx, -b->hy};
		e[1] = ly > 0 ? (vec_t) {-b->hx, b->hy} : (vec_t) {b->hx, -b->hy};
	}
	for (int i = 0; i < 2; i++)
	{
		vec_t v = e[i];
		e[i].x = b->x + c * v.x - s * v.y;
		e[i].y = b->y + s * v.x + c * v.y;
	}
}

static void box_box (int ia, int ib)
{
	const body_t *a = &body[ia], *b = &body[ib];
	float ca = cosf (a->a), sa = sinf (a->a), cb = cosf (b->a), sb = sinf (b->a);
	float dx = b->x - a->x, dy = b->y - a->y;
	float dax = ca * dx + sa * dy, day = -sa * dx + ca * dy;		/* b's middle, as a sees it; and a's, as b does */
	float dbx = cb * dx + sb * dy, dby = -sb * dx + cb * dy;
	float c11 = fabsf (ca * cb + sa * sb), c12 = fabsf (-ca * sb + sa * cb);	/* b's axes in a's, their sizes */
	float c21 = fabsf (-sa * cb + ca * sb), c22 = fabsf (sa * sb + ca * cb);

	/* how far apart along each box's two axes: apart along one, they don't touch */
	float sep[4] =
	{
		fabsf (dax) - a->hx - (c11 * b->hx + c12 * b->hy), fabsf (day) - a->hy - (c21 * b->hx + c22 * b->hy),
		fabsf (dbx) - b->hx - (c11 * a->hx + c21 * a->hy), fabsf (dby) - b->hy - (c12 * a->hx + c22 * a->hy),
	};
	if (sep[0] > 0.0f || sep[1] > 0.0f || sep[2] > 0.0f || sep[3] > 0.0f)
	{
		return;
	}
	/* the axis they've sunk along the least (a's sooner than b's, so it doesn't flip) */
	const float half[4] = {a->hx, a->hy, b->hx, b->hy};
	int axis = 0;
	for (int i = 1; i < 4; i++)
	{
		if (sep[i] > 0.95f * sep[axis] + 0.01f * half[i])
		{
			axis = i;
		}
	}
	const float ax[4][2] = {{ca, sa}, {-sa, ca}, {cb, sb}, {-sb, cb}};	/* the four axes */
	const float along[4] = {dax, day, dbx, dby};
	float nx = along[axis] > 0.0f ? ax[axis][0] : -ax[axis][0], ny = along[axis] > 0.0f ? ax[axis][1] : -ax[axis][1];

	/* the face the normal is of (a's or b's), the other box's edge facing it,
	   cut to the face's width: what's behind the face touches */
	const body_t *ref = axis < 2 ? a : b, *inc = axis < 2 ? b : a;
	float fx = axis < 2 ? nx : -nx, fy = axis < 2 ? ny : -ny;		/* out of the face */
	float front = ref->x * fx + ref->y * fy + half[axis];
	int side_axis = axis ^ 1;
	float sx = ax[side_axis][0], sy = ax[side_axis][1], side = ref->x * sx + ref->y * sy, width = half[side_axis];
	vec_t edge[2], cut[2], points[2];
	facing_edge (edge, inc, axis < 2 ? cb : ca, axis < 2 ? sb : sa, fx, fy);
	if (clip (cut, edge, -sx, -sy, -side + width) < 2 || clip (points, cut, sx, sy, side + width) < 2)
	{
		return;
	}
	for (int i = 0; i < 2; i++)
	{
		float s = fx * points[i].x + fy * points[i].y - front;
		if (s <= 0.0f)
		{
			touch (ia, ib, points[i].x - s * fx, points[i].y - s * fy, nx, ny, -s);
		}
	}
}

/* a body and the box's four sides */
static void body_walls (int i)
{
	static const float planes[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};	/* into the box */
	const float offsets[4] = {0.0f, -WORLD_W, 0.0f, -world_h};
	const body_t *b = &body[i];
	float c = cosf (b->a), s = sinf (b->a);
	for (int p = 0; p < 4; p++)
	{
		float nx = planes[p][0], ny = planes[p][1];
		if (b->ball)
		{
			float d = nx * b->x + ny * b->y - offsets[p] - b->hx;
			if (d < 0.0f)
			{
				touch (i, n_bodies, b->x - nx * b->hx, b->y - ny * b->hx, -nx, -ny, -d);
			}
			continue;
		}
		for (int k = 0; k < 4; k++)			/* its corners */
		{
			float lx = k & 1 ? b->hx : -b->hx, ly = k & 2 ? b->hy : -b->hy;
			float x = b->x + c * lx - s * ly, y = b->y + s * lx + c * ly, d = nx * x + ny * y - offsets[p];
			if (d < 0.0f)
			{
				touch (i, n_bodies, x, y, -nx, -ny, -d);
			}
		}
	}
}

static void find_contacts (void)
{
	n_contacts = 0;
	for (int i = 0; i < n_bodies; i++)
	{
		const body_t *a = &body[i];
		float ra = a->ball ? a->hx : sqrtf (a->hx * a->hx + a->hy * a->hy);
		body_walls (i);
		for (int j = i + 1; j < n_bodies; j++)
		{
			const body_t *b = &body[j];
			float rb = b->ball ? b->hx : sqrtf (b->hx * b->hx + b->hy * b->hy);
			float dx = b->x - a->x, dy = b->y - a->y, r = ra + rb;
			if (dx * dx + dy * dy >= r * r)		/* (too far apart to touch) */
			{
				continue;
			}
			if (a->ball && b->ball)
			{
				ball_ball (i, j);
			}
			else if (a->ball)
			{
				box_ball (j, i);
			}
			else if (b->ball)
			{
				box_ball (i, j);
			}
			else
			{
				box_box (i, j);
			}
		}
	}
}

/* ---- a step ------------------------------------------------------------------------- */

static void step (float dt, float gx, float gy)
{
	find_contacts ();
	for (int i = 0; i < n_bodies; i++)
	{
		body[i].vx += gx * dt;
		body[i].vy += gy * dt;
		body[i].w *= 1.0f - 0.2f * dt;			/* (the air) */
	}

	/* each contact: what an impulse does there, and the speed to end with */
	for (int i = 0; i < n_contacts; i++)
	{
		contact_t *c = &contact[i];
		const body_t *a = &body[c->a], *b = &body[c->b];
		c->rax = c->px - a->x;
		c->ray = c->py - a->y;
		c->rbx = c->px - b->x;
		c->rby = c->py - b->y;
		float tx = c->ny, ty = -c->nx;
		float ran = c->rax * c->ny - c->ray * c->nx, rbn = c->rbx * c->ny - c->rby * c->nx;
		float rat = c->rax * ty - c->ray * tx, rbt = c->rbx * ty - c->rby * tx;
		c->mn = 1.0f / (a->im + b->im + a->ii * ran * ran + b->ii * rbn * rbn);
		c->mt = 1.0f / (a->im + b->im + a->ii * rat * rat + b->ii * rbt * rbt);
		float vx = b->vx - b->w * c->rby - a->vx + a->w * c->ray, vy = b->vy + b->w * c->rbx - a->vy - a->w * c->rax;
		float closing = -(vx * c->nx + vy * c->ny);
		c->bias = PUSH / dt * fmaxf (0.0f, c->sunk - SLOP);
		if (closing > BOUNCE_OVER)
		{
			c->bias += 0.5f * (a->bounce + b->bounce) * (closing - BOUNCE_OVER);
		}
		hit (c, closing);
	}

	for (int k = 0; k < ITERATIONS; k++)
	{
		for (int i = 0; i < n_contacts; i++)
		{
			contact_t *c = &contact[i];
			body_t *a = &body[c->a], *b = &body[c->b];
			float tx = c->ny, ty = -c->nx;

			/* along the normal: never pulling */
			float vx = b->vx - b->w * c->rby - a->vx + a->w * c->ray, vy = b->vy + b->w * c->rbx - a->vy - a->w * c->rax;
			float p = c->mn * (-(vx * c->nx + vy * c->ny) + c->bias), was = c->pn;
			c->pn = fmaxf (was + p, 0.0f);
			p = c->pn - was;
			float px = p * c->nx, py = p * c->ny;
			a->vx -= a->im * px;
			a->vy -= a->im * py;
			a->w -= a->ii * (c->rax * py - c->ray * px);
			b->vx += b->im * px;
			b->vy += b->im * py;
			b->w += b->ii * (c->rbx * py - c->rby * px);

			/* across it: friction, up to its share of the normal's */
			vx = b->vx - b->w * c->rby - a->vx + a->w * c->ray;
			vy = b->vy + b->w * c->rbx - a->vy - a->w * c->rax;
			p = c->mt * -(vx * tx + vy * ty);
			was = c->pt;
			c->pt = clampf (was + p, -FRICTION * c->pn, FRICTION * c->pn);
			p = c->pt - was;
			px = p * tx;
			py = p * ty;
			a->vx -= a->im * px;
			a->vy -= a->im * py;
			a->w -= a->ii * (c->rax * py - c->ray * px);
			b->vx += b->im * px;
			b->vy += b->im * py;
			b->w += b->ii * (c->rbx * py - c->rby * px);
		}
	}

	for (int i = 0; i < n_bodies; i++)
	{
		body_t *b = &body[i];
		float v2 = b->vx * b->vx + b->vy * b->vy;
		if (v2 > 60.0f * 60.0f)				/* (nothing through a wall in a step) */
		{
			float k = 60.0f / sqrtf (v2);
			b->vx *= k;
			b->vy *= k;
		}
		b->x += b->vx * dt;
		b->y += b->vy * dt;
		b->a += b->w * dt;
	}
}

/* ---- the picture -------------------------------------------------------------------- */

typedef struct { float x, y; int8_t lx, ly; uint8_t ex, ey; uint8_t rgba[4]; } vertex_t;

static vertex_t verts[MAX_BODIES * 6];

static int build (void)
{
	static const int8_t corner[6][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, -1}, {1, 1}, {-1, 1}};
	vertex_t *v = verts;
	for (int i = 0; i < n_bodies; i++)
	{
		const body_t *b = &body[i];
		float c = cosf (b->a), s = sinf (b->a);
		for (int k = 0; k < 6; k++, v++)
		{
			float lx = corner[k][0] * b->hx, ly = corner[k][1] * b->hy;
			v->x = b->x + c * lx - s * ly;
			v->y = b->y + s * lx + c * ly;
			v->lx = corner[k][0];
			v->ly = corner[k][1];
			v->ex = (uint8_t) (255.0f * (1.0f - 0.09f / b->hx));		/* (a box's border: the same width all round) */
			v->ey = (uint8_t) (255.0f * (1.0f - 0.09f / b->hy));
			memcpy (v->rgba, colors[b->color], 3);
			v->rgba[3] = b->ball ? 255 : 0;
		}
	}
	return n_bodies * 6;
}

int main (void)
{
	stdio_init_all ();
	pgpu_init ();
	printf ("\ntumble: waiting for the RPi (READY)...\n");
	while (!pgpu_wait_ready (1000))
	{
	}
	pgpu_set_reply_phase (1);
	int tries = 0;
	while (!pglInit () && ++tries < 5)		/* the first reply can be missed */
	{
	}
	bool pad_there = pad_init ();

	GLuint prog = glCreateProgram ();
	glProgramBinaryOES (prog, PGL_PROGRAM_BINARY_PGPU, &tumble_info, sizeof tumble_info);
	GLint linked = 0;
	glGetProgramiv (prog, GL_LINK_STATUS, &linked);
	if (!linked)
	{
		printf ("tumble: the program didn't link\n");
	}
	GLint a_pos = glGetAttribLocation (prog, "a_pos"), a_local = glGetAttribLocation (prog, "a_local");
	GLint a_edge = glGetAttribLocation (prog, "a_edge"), a_color = glGetAttribLocation (prog, "a_color");
	GLint u_view = glGetUniformLocation (prog, "u_view");
	if (!hud_init ())
	{
		printf ("tumble: the HUD program didn't link\n");
	}
	glDisable (GL_DEPTH_TEST);
	glDisable (GL_CULL_FACE);
	for (int i = 1; i < TSND_COUNT; i++)
	{
		pgpu_sound_data (i, tumble_sounds[i].rate, PGPU_SOUND_U8, tumble_sounds[i].samples, tumble_sounds[i].frames);
	}
	printf ("tumble: the stick tilts the box%s; keys: a d w s tilt, b a ball, x a box, space kicks, r starts over\n",
		pad_there ? " (A a ball, B a box, X kicks, Y starts over)" : " (no stick here)");

	GLint vp[4] = {0};
	perf_t perf;
	memset (&perf, 0, sizeof perf);
	bool new_perf = true;
	absolute_time_t last = get_absolute_time ();
	float idle = IDLE_S, key_x = 0, key_y = 0, key_x_left = 0, key_y_left = 0, auto_kick = 6.0f, said = 0;
	unsigned was_buttons = 0, frame = 0;
	uint64_t physics_us = 0;
	while (true)
	{
		if (screen_update ("tumble", vp))		/* the box has the screen's shape */
		{
			world_h = WORLD_W * vp[3] / (vp[2] ? vp[2] : 1);
			start_over ();
		}
		absolute_time_t t0 = get_absolute_time ();
		float dt = absolute_time_diff_us (last, t0) / 1e6f;
		dt = dt > 1.0f / 30 ? 1.0f / 30 : dt;
		last = t0;
		now += dt;

		/* what's asked: the stick, the controller, the keys, a finger */
		float tilt_x = 0, tilt_y = 0;
		bool want_ball = false, want_box = false, want_kick = false, want_over = false, any = false;
		pad_t pad;
		if (pad_there && pad_read (&pad))
		{
			unsigned pressed = pad.buttons & ~was_buttons;
			was_buttons = pad.buttons;
			tilt_x = pad.x;
			tilt_y = pad.y;
			want_ball = pressed & PAD_A;
			want_box = pressed & PAD_B;
			want_kick = pressed & (PAD_X | PAD_STICK);
			want_over = pressed & PAD_Y;
			any = pad.x != 0.0f || pad.y != 0.0f || pressed;
		}
		for (int c; (c = getchar_timeout_us (0)) != PICO_ERROR_TIMEOUT; any = true)
		{
			switch (c)
			{
			case 'a': case 'A': key_x = -1; key_x_left = KEY_HOLD_S; break;
			case 'd': case 'D': key_x = 1; key_x_left = KEY_HOLD_S; break;
			case 'w': case 'W': key_y = 1; key_y_left = KEY_HOLD_S; break;
			case 's': case 'S': key_y = -1; key_y_left = KEY_HOLD_S; break;
			case 'b': case 'B': want_ball = true; break;
			case 'x': case 'X': want_box = true; break;
			case ' ': want_kick = true; break;
			case 'r': case 'R': want_over = true; break;
			}
		}
		key_x_left -= dt;
		key_y_left -= dt;
		tilt_x = clampf (tilt_x + (key_x_left > 0 ? key_x : 0), -1.0f, 1.0f);
		tilt_y = clampf (tilt_y + (key_y_left > 0 ? key_y : 0), -1.0f, 1.0f);
		any = any || key_x_left > 0 || key_y_left > 0;
		pgpu_touch_event_t finger;
		while (pgpu_poll_touch (&finger))
		{
			if (finger.type == PGPU_TOUCH_EVENT_DOWN && n_bodies < MAX_BODIES)	/* (the panel: 320 x 240, y down) */
			{
				add_body (true, finger.x / 320.0f * WORLD_W, (1.0f - finger.y / 240.0f) * world_h);
				play (CH_POP, TSND_POP, 0.6f, finger.x / 160.0f - 1.0f, 1.0f);
				any = true;
			}
		}
		idle = any ? 0.0f : idle + dt;
		bool by_itself = idle >= IDLE_S;

		/* which way is down */
		float gx = GRAVITY * 1.5f * tilt_x, gy = -GRAVITY + 2.0f * GRAVITY * tilt_y;
		if (by_itself)
		{
			float lean = 0.95f * sinf (now * 0.45f);
			gx = GRAVITY * sinf (lean);
			gy = -GRAVITY * cosf (lean);
			auto_kick -= dt;
			if (auto_kick < 0)
			{
				auto_kick = 9.0f + 4.0f * frand ();
				want_kick = true;
			}
		}
		if (want_over)
		{
			start_over ();
			play (CH_POP, TSND_POP, 0.6f, 0.0f, 0.6f);
		}
		if ((want_ball || want_box) && n_bodies < MAX_BODIES)
		{
			add_body (want_ball, WORLD_W * (0.3f + 0.4f * frand ()), world_h - 1.0f);
			play (CH_POP, TSND_POP, 0.6f, 0.0f, want_ball ? 1.0f : 0.75f);
		}
		if (want_kick)
		{
			kick (WORLD_W * (0.35f + 0.3f * frand ()), world_h * 0.2f);
		}

		hits_now = 0;
		uint64_t before = time_us_64 ();
		for (int i = 0; i < STEPS; i++)
		{
			step (dt / STEPS, gx, gy);
		}
		physics_us += time_us_64 () - before;
		if (now - said >= 5.0f)
		{
			printf ("tumble: %d bodies, %d contacts, %u meetings heard, the physics %u us a frame, %s\n", n_bodies, n_contacts,
				hits_heard, (unsigned) (physics_us / (frame ? frame : 1)), by_itself ? "by itself" : "played");
			said = now;
			physics_us = 0;
			frame = 0;
		}
		frame++;

		/* the picture: the bodies in one draw, from here */
		glViewport (vp[0], vp[1], vp[2], vp[3]);
		glClearColor (0.07f, 0.08f, 0.11f, 1.0f);
		glClear (GL_COLOR_BUFFER_BIT);
		int n = build ();
		glUseProgram (prog);
		glUniform4f (u_view, WORLD_W / 2, world_h / 2, 2.0f / WORLD_W, 2.0f / world_h);
		glBindBuffer (GL_ARRAY_BUFFER, 0);
		glEnableVertexAttribArray (a_pos);
		glEnableVertexAttribArray (a_local);
		glEnableVertexAttribArray (a_edge);
		glEnableVertexAttribArray (a_color);
		glVertexAttribPointer (a_pos, 2, GL_FLOAT, GL_FALSE, sizeof (vertex_t), &verts[0].x);
		glVertexAttribPointer (a_local, 2, GL_BYTE, GL_FALSE, sizeof (vertex_t), &verts[0].lx);
		glVertexAttribPointer (a_edge, 2, GL_UNSIGNED_BYTE, GL_TRUE, sizeof (vertex_t), &verts[0].ex);
		glVertexAttribPointer (a_color, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof (vertex_t), verts[0].rgba);
		if (n)
		{
			glDrawArrays (GL_TRIANGLES, 0, n);
		}
		glDisableVertexAttribArray (a_edge);
		glDisableVertexAttribArray (a_color);

		if (new_perf || frame % 30 == 1)
		{
			char s[32];
			hud_begin ();
			snprintf (s, sizeof s, "TUMBLE %2d", n_bodies);
			hud_rect (0, 0, 12 + 9 * HUD_CHAR_W, 22, HUD_RGBA (0, 0, 20, 140));
			hud_text (6, 4, s, HUD_RGBA (255, 230, 120, 255));
			hud_text_scaled (6, 26, by_itself ? "BY ITSELF" : "PLAYED   ", HUD_RGBA (200, 205, 215, 255), 0.5f);
			hud_perf (vp[2] - hud_perf_width (hud_perf_scale (vp[3])) - 2, 2, hud_perf_scale (vp[3]), &perf);
			hud_end ();
		}
		hud_draw ();
		pglSwapBuffers ();

		absolute_time_t wait_start = get_absolute_time ();
		pgpu_wait_frame (100);			/* pace on the screen */
		new_perf = perf_frame (absolute_time_diff_us (wait_start, get_absolute_time ()), &perf);
	}
}
