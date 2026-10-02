/*
 * race.c - zerog's race (race.h).
 *
 * A craft is a point with a velocity and a nose. Its nose turns with the
 * stick (and an air brake turns it more, that side); the engine pushes along
 * the nose; the air holds it back, more the faster it goes; what it moves
 * sideways of its nose is taken off it by its grip, up to so much: over
 * that it slides wide. So a curve is flown by turning the nose in and waiting
 * for the craft to follow, and too fast it ends in the wall. Over the road a
 * spring holds it at its height (and a magnet brings it back down to it);
 * with no road under it there is only gravity: off the ramp it flies, and
 * past the edge it falls, and is put back.
 *
 * The craft that fly themselves use the same controls: they aim at a point
 * ahead on the line they want (the inside of the curve coming, a pad, round
 * the craft in the way), brake to what the curves ahead allow, and fire when
 * it's worth it.
 */
#include "race.h"
#include <stdlib.h>
#include "vec.h"

/* the flight model: metres, seconds */
#define GRAVITY		20.0f
#define HOVER_RANGE	3.0f		/* the road holds it up to here */
#define SPRING		140.0f
#define DAMP		17.0f
#define THRUST		30.0f
#define DRAG_1		0.05f		/* the air: the speed times (this + DRAG_2 * speed) */
#define DRAG_2		0.00385f	/* (full thrust: 82 m/s) */
#define GRIP		4.2f		/* of its sideways speed, a second */
#define GRIP_MAX	62.0f		/* m/s^2: over that it slides */
#define TURN		1.3f		/* rad/s, the stick over */
#define TURN_QUICK	7.0f		/* how fast the turning follows the stick, 1/s */
#define BRAKE_TURN	0.75f		/* rad/s more, an air brake out */
#define BRAKE_DRAG	24.0f		/* m/s^2, each */
#define BOOST_KICK	20.0f		/* m/s, a speed pad's */
#define TURBO_KICK	32.0f
#define CRAFT_R		1.5f		/* its middle from a wall */
#define CRAFT_LONG	5.4f		/* two craft's middles: nose to tail, */
#define CRAFT_WIDE	3.3f		/* side by side */
#define WALL_BOUNCE	0.35f
#define STEP		0.008f		/* the longest step flown at once */

/* the weapons */
#define ROCKET_SPEED	75.0f		/* over the craft's own */
#define MISSILE_SPEED	125.0f
#define MISSILE_TURN	2.6f		/* rad/s */
#define MINES		5
#define MINE_LIFE	45.0f
#define SHIELD_SECONDS	7.0f
#define HIT_R		2.6f

race_t race;

static uint32_t rng = 0x2545F491u;

static float rnd (void)
{
	rng ^= rng << 13;
	rng ^= rng >> 17;
	rng ^= rng << 5;
	return (rng & 0xFFFFFF) / 16777216.0f;
}

float craft_speed (const craft_t *c)	{ return v_len (c->v); }

float craft_total (const craft_t *c)
{
	return c->lap * route[ROUTE_MAIN].len + track_progress (&c->at);
}

/* how far ahead of a place (the short way round the loop) another is: < 0 behind */
static float gap_from (const where_t *a, const where_t *b)
{
	float len = route[ROUTE_MAIN].len, d = track_progress (b) - track_progress (a);
	return d - len * roundf (d / len);
}

static float gap (const craft_t *a, const where_t *b)	{ return gap_from (&a->at, b); }

const char *weapon_name (int w)
{
	static const char *const names[WEAPONS] = {"", "ROCKET", "MISSILE", "MINES", "SHIELD", "TURBO"};
	return names[w];
}

bool race_hunted (int craft)
{
	for (int i = 0; i < SHOTS; i++)
	{
		if (race.shot[i].live && race.shot[i].type == SHOT_MISSILE && race.shot[i].target == craft)
		{
			return true;
		}
	}
	return false;
}

void race_standings (int order[CRAFTS])
{
	for (int i = 0; i < CRAFTS; i++)
	{
		order[i] = i;
	}
	for (int i = 1; i < CRAFTS; i++)
		for (int j = i; j > 0; j--)
		{
			const craft_t *a = &race.craft[order[j - 1]], *b = &race.craft[order[j]];
			bool swap = a->place && b->place ? b->place < a->place : b->place ? true : a->place ? false
				  : craft_total (b) > craft_total (a);
			if (!swap)
			{
				break;
			}
			int t = order[j];
			order[j] = order[j - 1];
			order[j - 1] = t;
		}
}

static void blast (const float *p)
{
	blast_t *b = &race.blast[0];
	for (int i = 1; i < BLASTS; i++)
	{
		if (race.blast[i].age > b->age)
		{
			b = &race.blast[i];
		}
	}
	v_copy (b->p, p);
	b->age = 0.0f;
	b->fresh = true;
}

/* ---- the grid ------------------------------------------------------------------------ */

/* a craft put on the road at a place, x across, going on at v */
static void put (craft_t *c, where_t at, float x, float v)
{
	frame_t f;
	track_frame (at.route, at.s, &f);
	v_mad (c->p, f.p, f.r, x);
	v_mad (c->p, c->p, f.u, HOVER);
	v_copy (c->fwd, f.t);
	v_set (c->v, f.t[0] * v, f.t[1] * v, f.t[2] * v);
	c->at = at;
	c->spin = c->air_time = 0.0f;
	c->air = false;
	track_locate (&c->at, c->p, &c->f, &c->x, &c->h, NULL);
}

void race_reset (float now, bool human)
{
	const route_t *m = &route[ROUTE_MAIN];
	for (int i = 0; i < SHOTS; i++)
	{
		race.shot[i].live = false;
	}
	for (int i = 0; i < BLASTS; i++)
	{
		race.blast[i].age = 100.0f;
		race.blast[i].fresh = false;
	}
	for (int i = 0; i < CRAFTS; i++)
	{
		/* two by two behind the line, the player's at the back */
		craft_t *c = &race.craft[i];
		int slot = i == race.player ? CRAFTS - 1 : i < race.player ? i : i - 1;
		memset (c, 0, sizeof *c);
		put (c, (where_t) {ROUTE_MAIN, m->len - 14.0f - (slot / 2) * 12.0f - (slot % 2) * 4.0f}, slot % 2 ? 3.6f : -3.6f, 0.0f);
		c->lap = -1;
		c->last_tile = -1;
		c->human = human && i == race.player;
		c->skill = i == race.player ? 1.0f : 0.985f - 0.011f * slot + 0.02f * rnd ();	/* each race its own */
		c->lane = (rnd () - 0.5f) * 5.0f;
		c->fork = rnd () < 0.5f;
	}
	race.finished = 0;
	race.phase = GRID;
	race.phase_start = race.now = now;
	race.races++;
}

/* fallen: back on the road where it left it (past the jump, if into that) */
static void rescue (craft_t *c)
{
	where_t at = c->at;
	for (int i = 0; i < 40 && (track_ring (at.route, at.s)->flags & RING_GAP || i < 2); i++)
	{
		at = track_ahead (at, route[at.route].step, false);
	}
	put (c, at, 0.0f, 22.0f);
	c->stun = c->twirl = c->lost = 0.0f;
	c->shield = fmaxf (c->shield, 1.5f);			/* (it's left alone a moment) */
	c->events |= EV_RESCUED;
}

/* ---- the weapons --------------------------------------------------------------------- */

/* what a pad gives: the leaders what's left behind them, the others what's sent ahead */
static int pick (const craft_t *c)
{
	int ahead = 0;
	for (int i = 0; i < CRAFTS; i++)
	{
		ahead += craft_total (&race.craft[i]) > craft_total (c);
	}
	const float weight[WEAPONS] = {0, 3.0f, 1.0f + 0.5f * ahead, ahead ? 2.5f - 0.3f * ahead : 4.0f, 2.0f, 1.0f + 0.4f * ahead};
	float sum = 0.0f, r;
	for (int w = 1; w < WEAPONS; w++)
	{
		sum += fmaxf (weight[w], 0.3f);
	}
	r = rnd () * sum;
	for (int w = 1; w < WEAPONS; w++)
	{
		r -= fmaxf (weight[w], 0.3f);
		if (r <= 0.0f)
		{
			return w;
		}
	}
	return W_ROCKET;
}

static shot_t *new_shot (int type, int owner)
{
	shot_t *s = &race.shot[0];
	for (int i = 0; i < SHOTS; i++)			/* a free one, or the oldest */
	{
		if (!race.shot[i].live)
		{
			s = &race.shot[i];
			break;
		}
		if (race.shot[i].age > s->age)
		{
			s = &race.shot[i];
		}
	}
	memset (s, 0, sizeof *s);
	s->live = true;
	s->type = type;
	s->owner = owner;
	s->target = -1;
	return s;
}

static void fire (craft_t *c)
{
	int me = (int) (c - race.craft);
	float rt[3];
	v_cross (rt, c->fwd, c->f.u);
	switch (c->weapon)
	{
	case W_ROCKET: case W_MISSILE:
	{
		shot_t *s = new_shot (c->weapon == W_ROCKET ? SHOT_ROCKET : SHOT_MISSILE, me);
		v_mad (s->p, c->p, c->fwd, 3.5f);
		float v = c->weapon == W_ROCKET ? fmaxf (v_dot (c->v, c->fwd), 20.0f) + ROCKET_SPEED : MISSILE_SPEED;
		v_set (s->v, c->fwd[0] * v, c->fwd[1] * v, c->fwd[2] * v);
		s->at = c->at;
		if (c->weapon == W_MISSILE)		/* after the nearest ahead */
		{
			float nearest = 320.0f;
			for (int i = 0; i < CRAFTS; i++)
			{
				float d = gap (c, &race.craft[i].at);
				if (i != me && !race.craft[i].lost && d > 4.0f && d < nearest)
				{
					nearest = d;
					s->target = i;
				}
			}
		}
		break;
	}
	case W_MINES:
		c->mines = MINES;
		c->mine_wait = 0.0f;
		break;
	case W_SHIELD:
		c->shield = SHIELD_SECONDS;
		c->events |= EV_SHIELD;
		break;
	case W_TURBO:
		v_mad (c->v, c->v, c->fwd, TURBO_KICK);
		c->boost = 2.0f;
		c->events |= EV_TURBO;
		break;
	}
	if (c->weapon != W_SHIELD && c->weapon != W_TURBO)
	{
		c->events |= EV_FIRE;
	}
	c->weapon = W_NONE;
	c->fire_wait = 0.4f;
}

/* a craft struck (by a shot, there) */
static void strike (craft_t *c, const float *p)
{
	blast (p);
	if (c->shield > 0.0f)
	{
		c->events |= EV_BLOCKED;
		return;
	}
	v_set (c->v, c->v[0] * 0.6f, c->v[1] * 0.6f, c->v[2] * 0.6f);
	c->stun = 0.7f;
	c->twirl = 2 * PI;
	c->hits++;
	c->events |= EV_HIT;
}

static void shots_step (float dt)
{
	for (int i = 0; i < SHOTS; i++)
	{
		shot_t *s = &race.shot[i];
		if (!s->live)
		{
			continue;
		}
		s->age += dt;
		if (s->type != SHOT_MINE)
		{
			frame_t f;
			float x, h, lo, hi;
			if (s->type == SHOT_MISSILE && s->target >= 0)
			{
				/* at its target when it's near, else along the road towards it */
				const craft_t *t = &race.craft[s->target];
				float aim[3], d[3], dir[3], v = v_len (s->v), ahead = gap_from (&s->at, &t->at);
				if (ahead > 0.0f && ahead < 45.0f)
				{
					v_copy (aim, t->p);
				}
				else
				{
					where_t a = track_ahead (s->at, 30.0f, t->at.route == ROUTE_ALT);
					track_frame (a.route, a.s, &f);
					v_mad (aim, f.p, f.r, clampf (t->x, -(HW - 2.0f), HW - 2.0f));
					v_mad (aim, aim, f.u, HOVER);
				}
				v_sub (d, aim, s->p);
				v_norm (d);
				v_set (dir, s->v[0] / v, s->v[1] / v, s->v[2] / v);
				v_mad (dir, dir, d, MISSILE_TURN * dt);
				v_norm (dir);
				v_set (s->v, dir[0] * v, dir[1] * v, dir[2] * v);
				if (ahead < -30.0f || t->lost > 0.0f)	/* missed: on as a rocket */
				{
					s->target = -1;
				}
			}
			v_mad (s->p, s->p, s->v, dt);
			track_locate (&s->at, s->p, &f, &x, &h, NULL);
			track_limits (&s->at, &lo, &hi);
			bool gapped = track_ring (s->at.route, s->at.s)->flags & RING_GAP;
			if (!gapped)			/* over the road: at a craft's height, along it */
			{
				float v = v_len (s->v);
				v_mad (s->p, s->p, f.u, (HOVER - h) * fminf (1.0f, 12.0f * dt));
				v_mad (s->v, s->v, f.u, -v_dot (s->v, f.u));
				v_norm (s->v);
				v_set (s->v, s->v[0] * v, s->v[1] * v, s->v[2] * v);
			}
			if ((!gapped && (x < lo + 0.3f || x > hi - 0.3f)) || s->age > (s->type == SHOT_ROCKET ? 3.5f : 7.0f))
			{
				blast (s->p);
				s->live = false;
				continue;
			}
		}
		else if (s->age > MINE_LIFE)
		{
			s->live = false;
			continue;
		}
		for (int j = 0; j < CRAFTS; j++)
		{
			craft_t *c = &race.craft[j];
			float d[3];
			v_sub (d, c->p, s->p);
			if (c->lost > 0.0f || v_dot (d, d) > HIT_R * HIT_R || (j == s->owner && s->age < (s->type == SHOT_MINE ? 1.5f : 0.6f)))
			{
				continue;
			}
			strike (c, s->p);
			if (s->type == SHOT_MINE)
			{
				c->events |= EV_MINE;
			}
			s->live = false;
			break;
		}
	}
	for (int i = 0; i < BLASTS; i++)
	{
		race.blast[i].age += dt;
	}
}

/* ---- flying -------------------------------------------------------------------------- */

static void scale_v (craft_t *c, float k)	{ v_set (c->v, c->v[0] * k, c->v[1] * k, c->v[2] * k); }

static void fly (craft_t *c, const input_t *in, float dt, float t_race)
{
	const frame_t *f = &c->f;
	if (c->lost > 0.0f)				/* falling, till it's fetched */
	{
		c->v[1] -= GRAVITY * dt;
		v_mad (c->p, c->p, c->v, dt);
		c->lost -= dt;
		if (c->lost <= 0.0f)
		{
			rescue (c);
		}
		return;
	}

	int laps = 0;
	track_locate (&c->at, c->p, &c->f, &c->x, &c->h, &laps);
	const ring_t *g = track_ring (c->at.route, c->at.s);
	bool road = track_road (&c->at, c->x) && c->h > -1.0f;
	bool held = road && c->h < HOVER_RANGE;

	/* the line: laps (on the grid it's behind it: lap -1) */
	if (laps > 0)
	{
		c->lap++;
		if (c->lap >= 1)
		{
			float lap = t_race - c->lap_start;
			c->best_lap = c->best_lap == 0.0f || lap < c->best_lap ? lap : c->best_lap;
		}
		c->lap_start = t_race;
		if (c->lap == LAPS && !c->place)
		{
			c->place = ++race.finished;
			c->finish = t_race;
			c->events |= EV_FINISH;
		}
		else if (c->lap >= 1 && c->lap < LAPS)
		{
			c->events |= EV_LAP;
		}
		c->fork = rnd () < 0.5f;
	}
	else if (laps < 0)
	{
		c->lap--;
	}

	/* the nose: in the road's plane, turned by the stick and the air brakes */
	bool stunned = c->stun > 0.0f;
	float steer = stunned ? 0.0f : clampf (in->steer, -1.0f, 1.0f), thrust = stunned ? 0.0f : clampf (in->thrust, 0.0f, 1.0f);
	float bl = stunned ? 0.0f : clampf (in->brake_l, 0.0f, 1.0f), br = stunned ? 0.0f : clampf (in->brake_r, 0.0f, 1.0f);
	float rt[3];
	c->throttle = thrust;
	v_mad (c->fwd, c->fwd, f->u, -v_dot (c->fwd, f->u));
	v_norm (c->fwd);
	v_cross (rt, c->fwd, f->u);
	c->spin += (steer * TURN + (br - bl) * BRAKE_TURN - c->spin) * (1.0f - expf (-dt * TURN_QUICK));
	float turn = c->spin * dt;
	v_set (c->fwd, c->fwd[0] + rt[0] * turn, c->fwd[1] + rt[1] * turn, c->fwd[2] + rt[2] * turn);
	v_norm (c->fwd);
	v_cross (rt, c->fwd, f->u);

	/* what pushes it: gravity, the engine, the air (and the brakes in it), its grip, the road */
	float acc[3] = {0.0f, -GRAVITY, 0.0f}, speed = v_len (c->v), slip = v_dot (c->v, rt), vh = v_dot (c->v, f->u);
	v_mad (acc, acc, c->fwd, thrust * THRUST * c->skill);
	v_mad (acc, acc, c->v, -(DRAG_1 + DRAG_2 * speed) - (stunned ? 0.8f : 0.0f)
			       - fminf ((bl + br) * BRAKE_DRAG, 0.5f * speed / dt) / fmaxf (speed, 1.0f));
	v_mad (acc, acc, rt, -clampf (GRIP * slip, -GRIP_MAX, GRIP_MAX) * (held ? 1.0f : 0.15f));
	if (held)
	{
		float fade = 1.0f - smooth (0.5f * HOVER_RANGE, HOVER_RANGE, c->h);
		v_mad (acc, acc, f->u, fade * (SPRING * (HOVER - c->h) - DAMP * vh + GRAVITY * f->u[1]));
	}
	v_mad (c->v, c->v, acc, dt);
	v_mad (c->p, c->p, c->v, dt);

	/* where that took it: the road, the walls */
	float d[3];
	v_sub (d, c->p, f->p);
	float x = v_dot (d, f->r), h = v_dot (d, f->u);
	if (road && h < 0.25f)				/* down on the road */
	{
		vh = v_dot (c->v, f->u);
		v_mad (c->p, c->p, f->u, 0.25f - h);
		if (vh < 0.0f)
		{
			v_mad (c->v, c->v, f->u, -1.3f * vh);
			c->events |= vh < -5.0f ? EV_BUMP : 0;
		}
	}
	c->wall_time += dt;
	if (!(g->flags & RING_GAP) && h > -1.0f && h < 6.0f)
	{
		float lo, hi;
		track_limits (&c->at, &lo, &hi);
		float over = x > hi - CRAFT_R ? x - (hi - CRAFT_R) : x < lo + CRAFT_R ? x - (lo + CRAFT_R) : 0.0f;
		if (over != 0.0f && fabsf (over) < 4.0f)	/* (further out it's outside, and falls) */
		{
			float side = over > 0.0f ? 1.0f : -1.0f, into = v_dot (c->v, f->r) * side;
			v_mad (c->p, c->p, f->r, -over);
			if (into > 0.0f)			/* bounced, and slowed by the blow */
			{
				v_mad (c->v, c->v, f->r, -side * into * (1.0f + WALL_BOUNCE));
				scale_v (c, 1.0f - clampf (0.012f * into, 0.0f, 0.35f));
				if (into > 2.5f && c->wall_time > 0.4f)
				{
					c->events |= EV_WALL;
					c->walls++;
				}
			}
			scale_v (c, 1.0f - 0.7f * dt);		/* scraping along it */
			into = v_dot (c->fwd, f->r) * side;	/* the nose brought round, along it */
			if (into > 0.0f)
			{
				v_mad (c->fwd, c->fwd, f->r, -side * into * fminf (1.0f, 5.0f * dt));
				v_norm (c->fwd);
			}
			c->wall_time = 0.0f;
		}
	}

	/* in the air: off the ramp, or over the edge */
	if (!held)
	{
		if (c->air_time == 0.0f && (g->flags & RING_GAP))
		{
			c->events |= EV_JUMP;
		}
		c->air_time += dt;
	}
	else
	{
		c->events |= c->air_time > 0.3f ? EV_LAND : 0;
		c->air_time = 0.0f;
	}
	c->air = !held;
	if ((!road && h < -5.0f) || c->air_time > 5.0f)
	{
		c->lost = 1.1f;
		c->falls++;
		c->events |= EV_FALL;
		return;
	}

	/* the pads: once each, as it comes onto one */
	if (held && c->h < 1.8f)
	{
		int tile = ((int) (c->at.s / route[c->at.route].step) / TILE_RINGS) * 4 + c->at.route * 2 + (c->x > 0.0f);
		if (tile != c->last_tile)
		{
			int pad = track_pad (c->at.route, c->at.s, c->x);
			c->last_tile = tile;
			if (pad == PAD_BOOST)
			{
				v_mad (c->v, c->v, f->t, BOOST_KICK);
				c->boost = 1.4f;
				c->events |= EV_BOOST;
			}
			else if (pad == PAD_WEAPON && c->weapon == W_NONE && !c->mines)
			{
				c->weapon = pick (c);
				c->fire_wait = c->human ? 0.0f : 0.6f + 1.8f * rnd ();
				c->pickups++;
				c->events |= EV_PICKUP;
			}
		}
	}

	/* the weapon: fired as the button goes down; the mines one after another */
	c->fire_wait -= dt;
	if (in->fire && !c->fired && c->weapon && c->fire_wait <= 0.0f && !stunned)
	{
		fire (c);
	}
	c->fired = in->fire;
	if (c->mines && (c->mine_wait -= dt) <= 0.0f)
	{
		shot_t *s = new_shot (SHOT_MINE, (int) (c - race.craft));
		v_mad (s->p, c->p, c->fwd, -3.0f);
		v_mad (s->p, s->p, f->u, 0.45f - c->h);
		s->at = c->at;
		c->mines--;
		c->mine_wait = 0.2f;
	}

	/* how it sits: rolled into its turn, the nose up as it climbs */
	float w = 1.0f - expf (-dt * 6.0f);
	c->roll += (clampf (0.42f * c->spin + 0.18f * (br - bl) + 0.012f * slip, -0.7f, 0.7f) - c->roll) * w;
	c->pitch += (clampf (0.035f * v_dot (c->v, f->u), -0.3f, 0.3f) - c->pitch) * w;
	c->boost = fmaxf (c->boost - dt, 0.0f);
	c->stun = fmaxf (c->stun - dt, 0.0f);
	c->shield = fmaxf (c->shield - dt, 0.0f);
	c->twirl = fmaxf (c->twirl - 2 * PI * dt / 0.9f, 0.0f);
	c->wrong_way = v_dot (c->v, f->t) < -8.0f;
}

/* craft against craft (each as long and as wide as it is, along the road):
   pushed apart the short way, nose to tail or sideways, and what they close
   at that way given back */
static void collide (void)
{
	for (int i = 0; i < CRAFTS; i++)
		for (int j = i + 1; j < CRAFTS; j++)
		{
			craft_t *a = &race.craft[i], *b = &race.craft[j];
			float d[3];
			v_sub (d, b->p, a->p);
			float along = v_dot (d, a->f.t), across = v_dot (d, a->f.r);
			if (fabsf (along) >= CRAFT_LONG || fabsf (across) >= CRAFT_WIDE || fabsf (v_dot (d, a->f.u)) > 1.6f
			    || a->lost > 0.0f || b->lost > 0.0f)
			{
				continue;
			}
			bool tail = CRAFT_LONG - fabsf (along) < CRAFT_WIDE - fabsf (across);
			float depth = tail ? CRAFT_LONG - fabsf (along) : CRAFT_WIDE - fabsf (across), n[3], rel[3], closing;
			v_copy (n, tail ? a->f.t : a->f.r);
			if ((tail ? along : across) < 0.0f)
			{
				v_set (n, -n[0], -n[1], -n[2]);
			}
			v_mad (a->p, a->p, n, -depth / 2);
			v_mad (b->p, b->p, n, depth / 2);
			v_sub (rel, b->v, a->v);
			closing = v_dot (rel, n);
			if (closing < 0.0f)
			{
				v_mad (a->v, a->v, n, 0.75f * closing);
				v_mad (b->v, b->v, n, -0.75f * closing);
				if (closing < -1.5f)
				{
					a->events |= EV_BUMP;
					b->events |= EV_BUMP;
				}
			}
		}
}

/* ---- the craft that fly themselves ------------------------------------------------------ */

static void autopilot (craft_t *c, input_t *in)
{
	int me = (int) (c - race.craft);
	float speed = craft_speed (c), rt[3];
	v_cross (rt, c->fwd, c->f.u);
	memset (in, 0, sizeof *in);

	/* the speed the curves ahead allow (not on the ramp: the jump wants all of it) */
	float allowed = 1e9f, grip = (50.0f + 8.0f * (c->skill - 0.9f) / 0.1f), reach = speed * speed / 44.0f + 24.0f;
	bool jump = false;
	for (float d = 0.0f; d <= reach; d += 8.0f)
	{
		where_t a = track_ahead (c->at, d, c->fork);
		const ring_t *g = track_ring (a.route, a.s);
		if (g->flags & (RING_RAMP | RING_GAP))
		{
			jump = jump || d < 120.0f;
			continue;
		}
		float vk = grip / fmaxf (fabsf (g->k), 1e-4f);
		allowed = fminf (allowed, sqrtf (vk + 44.0f * d));
	}

	/* the line: the inside of the curve ahead, its own side on the straights;
	   onto a pad coming; round a craft in the way, and a mine */
	where_t a = track_ahead (c->at, 14.0f + 0.42f * speed, c->fork);
	frame_t f;
	track_frame (a.route, a.s, &f);
	float lim = HW - 2.6f, xt = clampf (f.k * 560.0f + c->lane * (1.0f - clampf (fabsf (f.k) * 150.0f, 0.0f, 1.0f)), -lim, lim);
	if (jump)
	{
		xt = clampf (c->x, -lim + 1.5f, lim - 1.5f);		/* straight at it */
	}
	else
	{
		for (float d = 24.0f; d < 110.0f; d += 16.0f)
		{
			where_t pa = track_ahead (c->at, d, c->fork);
			const route_t *m = &route[pa.route];
			int tile = (int) (pa.s / m->step) / TILE_RINGS;
			for (int side = 0; side < 2 && tile < m->n / TILE_RINGS; side++)
			{
				int pad = m->pads[tile][side];
				if (pad == PAD_BOOST || (pad == PAD_WEAPON && !c->weapon && !c->mines))
				{
					xt = side ? HW / 2 : -HW / 2;
					d = 1e3f;
					break;
				}
			}
		}
	}
	for (int i = 0; i < CRAFTS; i++)
	{
		const craft_t *o = &race.craft[i];
		float ds = gap (c, &o->at);
		if (i == me || o->lost > 0.0f || o->at.route != a.route || ds < 0.0f || ds > 34.0f)
		{
			continue;
		}
		if (fabsf (o->x - xt) < 3.8f && speed > craft_speed (o) - 2.0f)
		{
			xt = clampf (o->x + (o->x > c->x ? -4.4f : 4.4f), -lim, lim);
			if (fabsf (xt - o->x) < 3.6f)			/* (no room that side) */
			{
				xt = clampf (o->x + (o->x > c->x ? 4.4f : -4.4f), -lim, lim);
			}
		}
	}
	for (int i = 0; i < SHOTS; i++)
	{
		const shot_t *s = &race.shot[i];
		float ds = s->live && s->type == SHOT_MINE ? gap (c, &s->at) : -1.0f;
		if (ds > 0.0f && ds < 14.0f + 0.9f * speed && s->at.route == a.route)
		{
			frame_t mf;
			float d[3], mx;
			track_frame (s->at.route, s->at.s, &mf);
			v_sub (d, s->p, mf.p);
			mx = v_dot (d, mf.r);
			if (fabsf (mx - xt) < 3.2f)
			{
				xt = clampf (mx + (mx > c->x ? -4.2f : 4.2f), -lim, lim);
			}
		}
	}

	/* the stick: the nose towards that point, and more while the craft still
	   slides the other way; an air brake when it's far off */
	float aim[3], d[3];
	v_mad (aim, f.p, f.r, xt);
	v_sub (d, aim, c->p);
	float err = atan2f (v_dot (d, rt), v_dot (d, c->fwd)), slide = atan2f (v_dot (c->v, rt), fmaxf (v_dot (c->v, c->fwd), 8.0f));
	in->steer = clampf (1.7f * err + 1.3f * (err - slide), -1.0f, 1.0f);
	if (fabsf (err) > 0.22f && speed > 35.0f && !c->air)
	{
		*(err > 0.0f ? &in->brake_r : &in->brake_l) = clampf ((fabsf (err) - 0.22f) * 5.0f, 0.0f, 0.8f);
	}

	/* the engine, or both brakes */
	if (c->place)
	{
		allowed = fminf (allowed, 45.0f);			/* a lap of honour */
	}
	if (speed > allowed + 1.0f && !jump)
	{
		float b = clampf ((speed - allowed) / 10.0f, 0.0f, 1.0f);
		in->brake_l = fmaxf (in->brake_l, b);
		in->brake_r = fmaxf (in->brake_r, b);
	}
	else
	{
		in->thrust = speed > allowed - 1.0f && !jump ? 0.4f : 1.0f;
	}

	/* the weapon: when it's worth it */
	if (c->weapon && c->fire_wait <= 0.0f && !c->place)
	{
		bool before = false, behind = false, lined = false;
		for (int i = 0; i < CRAFTS; i++)
		{
			const craft_t *o = &race.craft[i];
			float ds = gap (c, &o->at), od[3];
			if (i == me || o->lost > 0.0f)
			{
				continue;
			}
			v_sub (od, o->p, c->p);
			before = before || (ds > 6.0f && ds < 260.0f);
			behind = behind || (ds < -4.0f && ds > -70.0f);
			lined = lined || (v_dot (od, c->fwd) > 8.0f && v_dot (od, c->fwd) < 110.0f && fabsf (v_dot (od, rt)) < 2.6f);
		}
		bool threat = race_hunted (me);
		switch (c->weapon)
		{
		case W_ROCKET:	in->fire = lined || (before && c->fire_wait < -9.0f); break;
		case W_MISSILE:	in->fire = before; break;
		case W_MINES:	in->fire = behind || c->fire_wait < -8.0f; break;
		case W_SHIELD:	in->fire = threat || c->fire_wait < -10.0f; break;
		case W_TURBO:	in->fire = fabsf (f.k) < 1.0f / 400 && allowed > 90.0f && !jump && !c->air; break;
		}
	}
}

/* ---- the race --------------------------------------------------------------------------- */

bool race_step (float dt, float now)
{
	float in_phase = now - race.phase_start;
	race.now = now;
	if (race.phase == GRID)
	{
		if (in_phase < GRID_SECONDS)
		{
			return false;
		}
		race.phase = RACE;
		race.phase_start = race.start = now;
	}
	const craft_t *me = &race.craft[race.player];
	if (race.phase == RACE && me->place && (race.finished == CRAFTS || now - race.start - me->finish > 8.0f))
	{
		race.phase = RESULTS;
		race.phase_start = now;
	}
	else if (race.phase == RESULTS && in_phase >= RESULTS_SECONDS)
	{
		return true;
	}

	/* the controls once, then the flying in short steps */
	input_t in[CRAFTS];
	for (int i = 0; i < CRAFTS; i++)
	{
		craft_t *c = &race.craft[i];
		if (c->human && !c->place)
		{
			in[i] = c->in;
		}
		else if (!c->lost)
		{
			autopilot (c, &in[i]);
			/* a craft far ahead of the stick's eases off, one far behind it presses on */
			const craft_t *me = &race.craft[race.player];
			if (me->human && !me->place && c != me)
			{
				float lead = craft_total (c) - craft_total (me);
				in[i].thrust *= 1.0f - 0.28f * smooth (120.0f, 700.0f, lead) + 0.06f * smooth (120.0f, 500.0f, -lead);
			}
		}
		else
		{
			memset (&in[i], 0, sizeof in[i]);
		}
	}
	int n = (int) ceilf (dt / STEP);
	for (int k = 0; k < n; k++)
	{
		for (int i = 0; i < CRAFTS; i++)
		{
			fly (&race.craft[i], &in[i], dt / n, now - race.start);
		}
		collide ();
		shots_step (dt / n);
	}
	return false;
}
