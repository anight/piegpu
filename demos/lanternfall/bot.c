/*
 * bot.c - Lanternfall played by itself (lf.h): along the level's way (its
 * lf_bot entities, in order), doing at each step what it says:
 *
 *   raise   the lantern held up till what's there has taken it: a hidden way
 *           shown, Ody woken, the beacon lit, the Tide King burnt, the choice made
 *   ody     Ody told what the place asks, and waited for (where he lifts:
 *           walked up to, and thrown)
 *   fight   stays till the Hollow about are gone
 *   sneak   (to this step:) softly, the lantern low
 *   wait    a moment
 *
 * and on the way what a player would: the lantern up when a lurker's near, a
 * step aside when a brute rears, a flare when it reels, Ody sent at it. For
 * showing the game with nobody at the stick, and for trying a level: is its
 * way open from end to end?
 */
#include "lf.h"
#include <math.h>
#include <string.h>

static float dist3 (const float *a, const float *b)
{
	return sqrtf ((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2]));
}

void lf_bot_init (lf_bot_t *b, const lf_t *s)
{
	memset (b, 0, sizeof *b);
	for (int i = 0; i < s->n_steps; i++)
	{
		memcpy (b->points[i], s->step[i].p, sizeof b->points[i]);
	}
	game_pilot_init (&b->pilot, (const float (*)[3]) b->points, s->n_steps);
	b->at = -1;
	memcpy (b->check, s->check, sizeof b->check);
}

/* is something here still taking the raised lantern? */
static bool wants_light (const lf_t *s)
{
	float l[3];
	lf_lantern (s, l);
	for (int i = 0; i < s->n_brushes; i++)
	{
		if (s->brush[i].kind == LF_HIDDEN && !s->brush[i].done && dist3 (l, s->brush[i].centre) < s->brush[i].radius)
			return true;
	}
	if (s->has_beacon && !s->beacon_lit && dist3 (l, s->beacon) < 170.0f)
		return true;
	if (s->ody.state == LF_ODY_ASLEEP && dist3 (l, s->ody.p) < 240.0f)
		return true;
	if (s->king.here && s->king.down && dist3 (l, s->king.p) < 300.0f)
		return true;
	if (s->has_choice && !s->ending && (!s->king.here || s->king.beaten))
	{
		for (int k = 0; k < 2; k++)
			if (dist3 (l, s->choice[k]) < 170.0f)
				return true;
	}
	return false;
}

/* turning to a place and walking to it */
static void towards (const lf_t *s, const float *p, float pace, lf_input_t *in)
{
	float a = atan2f (p[1] - s->g.origin[1], p[0] - s->g.origin[0]) - s->g.yaw;
	a -= 6.2831853f * floorf ((a + 3.14159265f) / 6.2831853f);
	in->move.turn = a * 5.0f > 4.0f ? 4.0f : a * 5.0f < -4.0f ? -4.0f : a * 5.0f;
	in->move.forward = fabsf (a) < 0.6f ? pace : 0.0f;
}

void lf_bot (lf_bot_t *b, const lf_t *s, lf_input_t *in, float dt)
{
	memset (in, 0, sizeof *in);
	if (s->n_steps == 0 || s->ending)
	{
		return;
	}
	if (s->out > 0.0f || s->sea > 0.0f)
	{
		b->was_out = b->was_out || s->out > 0.0f;
		return;
	}
	if (b->was_out)					/* back at the last fire: on from the step it was at then */
	{
		b->was_out = false;
		b->pilot.next = b->check_step;
		b->pilot.best = 1e9f;
		b->pilot.since = 0.0f;
		b->at = -1;
		b->deaths++;
	}
	if (memcmp (b->check, s->check, sizeof b->check) != 0)
	{
		memcpy (b->check, s->check, sizeof b->check);
		b->check_step = b->at >= 0 ? b->at : b->pilot.next;
	}
	b->cool = b->cool > dt ? b->cool - dt : 0.0f;

	/* the Hollow about: what a player would do */
	const char *to = s->step[b->pilot.next].what;
	bool sneak = b->at < 0 && strcmp (to, "sneak") == 0, awake = false, aside = false;
	for (int i = 0; i < s->n_hollow; i++)
	{
		const lf_hollow_t *h = &s->hollow[i];
		float d = dist3 (h->p, s->g.origin);
		if (h->state == LF_H_ASLEEP || h->state >= LF_H_DYING)
		{
			continue;
		}
		awake = awake || d < 800.0f;
		if (h->kind == LF_LURKER && d < 330.0f && !sneak)
		{
			in->raise = true;
		}
		if (h->kind == LF_BRUTE)
		{
			aside = aside || ((h->state == LF_H_WINDUP || h->state == LF_H_LUNGE) && d < 520.0f);
			if ((h->state == LF_H_STUN || h->state == LF_H_RECOVER) && d < LF_FLARE_REACH - 30.0f && s->ember >= 0.3f && b->cool <= 0.0f)
			{
				in->flare = true;
				b->cool = 1.2f;
			}
		}
		if (h->kind == LF_LISTENER && h->state == LF_H_STALK && d < 300.0f && s->ember >= 0.3f && b->cool <= 0.0f)
		{
			in->flare = true;
			b->cool = 1.2f;
		}
	}
	for (int i = 0; i < LF_MAX_BOLTS; i++)		/* the Tide King's bolts burn in the raised lantern */
	{
		if (s->bolt[i].live && dist3 (s->bolt[i].p, s->g.origin) < 300.0f)
		{
			in->raise = true;
		}
	}
	if (s->ody_can == LF_DO_STRIKE && b->cool <= 0.0f)
	{
		in->ody = true;
		b->cool = 1.0f;
	}

	if (b->at < 0)					/* on the way */
	{
		unsigned reached = b->pilot.reached;
		game_pilot (&b->pilot, &s->g, &in->move, dt);
		if (sneak)
		{
			in->move.forward = in->move.forward > 0.5f ? 0.5f : in->move.forward;
			in->move.jump = false;
		}
		if (aside)
		{
			in->move.side = 1.0f;
		}
		if (b->pilot.reached != reached)
		{
			int step = (b->pilot.next + s->n_steps - 1) % s->n_steps;
			if (s->step[step].what[0] && strcmp (s->step[step].what, "sneak") != 0)
			{
				b->at = step;
				b->t = 0.0f;
				b->pressed = false;
			}
		}
		return;
	}

	/* at a step: what it asks */
	const char *what = s->step[b->at].what;
	bool done = false;
	b->t += dt;
	if (strcmp (what, "raise") == 0)
	{
		in->raise = true;
		done = (b->t > 0.5f && !wants_light (s)) || b->t > 9.0f;
	}
	else if (strcmp (what, "ody") == 0)
	{
		const lf_ody_t *o = &s->ody;
		if (!b->pressed && s->ody_can != LF_DO_NONE && s->ody_can != LF_DO_COME && s->ody_can != LF_DO_STRIKE)
		{
			in->ody = true;
			b->pressed = true;
			b->since = b->t;
		}
		if (b->pressed && o->state == LF_ODY_KNEEL && o->task == LF_DO_LIFT)	/* to be lifted: to him */
		{
			towards (s, o->p, 0.6f, in);
		}
		done = (b->pressed && b->t - b->since > 0.5f && (o->state == LF_ODY_FOLLOW || o->state == LF_ODY_STAY))
		       || (s->g.events & GAME_PUSHED) || b->t > 16.0f;
	}
	else if (strcmp (what, "fight") == 0)
	{
		if (aside)
		{
			in->move.side = 1.0f;
		}
		else if (dist3 (s->g.origin, s->step[b->at].p) > 220.0f)
		{
			towards (s, s->step[b->at].p, 1.0f, in);
		}
		done = (b->t > 1.5f && !awake) || b->t > 60.0f;
	}
	else
	{
		done = b->t > 2.0f;
	}
	if (done)
	{
		b->at = -1;
	}
}
