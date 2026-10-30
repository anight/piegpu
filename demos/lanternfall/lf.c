/*
 * lf.c - Lanternfall's game (lf.h). The level's entities:
 *
 *   lf_hidden   brushes: a way that isn't there till the raised lantern has
 *               been near it (radius) a moment; then it stays
 *   lf_break    brushes: a wall Ody breaks when told to (key "anchor": one
 *               of the Tide King's anchors)
 *   lf_gate     brushes: sinks away when its key is met: "beacon" (the
 *               level's beacon lit), "plate" (Ody on plate id), "kill" (the
 *               Hollow of group id all gone)
 *   lf_ody      where Ody is at the start (asleep 1: till the lantern wakes him;
 *               stay 1: he waits there till he's called, or the beacon burns)
 *   lf_lift     where Ody lifts Wren: thrown through its target's place
 *   lf_plate    a plate only Ody is heavy enough for (id)
 *   lf_hollow   one of the Hollow: kind 0 a lurker, 1 a brute, 2 a listener;
 *               wake: how near Wren wakes it; group
 *   lf_shrine   a small fire: fills the lantern, and where Wren comes back to
 *   lf_beacon   the island's beacon: kindled by the raised lantern
 *   lf_exit     where the level's left (radius), its beacon lit
 *   lf_story    lines of the story (line), said once as Wren comes near (radius)
 *   lf_king     the Tide King; lf_choice (which 0, 1): the great lantern, the tide pool
 *   lf_archivist  where she sits
 *   lf_bot      the way through for the game playing itself: n, do
 *   item_ember  an ember to pick up (the engine's items)
 */
#include "lf.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PI		3.14159265f
#define GRAVITY		800.0f
#define STEP		18.0f

/* Wren */
#define HURT_SECONDS	1.2f		/* after a hit: not hurt again */
#define OUT_SECONDS	1.8f		/* the light out: the dark, then back at the shrine */
#define SEA_SECONDS	1.0f
#define EMBER_ITEM	0.34f
#define KINDLE_SECONDS	1.5f		/* the lantern held by a beacon */
#define REVEAL_SECONDS	0.8f
#define WAKE_SECONDS	1.0f

/* Ody */
#define ODY_HULL	2
#define ODY_SPEED	235.0f
#define ODY_NEAR	120.0f		/* he stops this near Wren */
#define ODY_WIDE	52.0f		/* Wren can't be nearer his middle */
#define ODY_REACH	120.0f		/* his arm's */

/* the Hollow */
#define LURK_SPEED	150.0f
#define FLEE_SPEED	190.0f
#define BURN_SECONDS	1.1f
#define BRUTE_SPEED	100.0f
#define BRUTE_RANGE	380.0f		/* it rears this near */
#define WINDUP_SECONDS	1.1f
#define LUNGE_SPEED	560.0f
#define LUNGE_SECONDS	0.8f
#define RECOVER_SECONDS	1.3f
#define STUN_SECONDS	2.6f
#define HEAR_RUN	380.0f		/* a listener hears running this near, */
#define HEAR_LIGHT	520.0f		/* sees the raised lantern this near, */
#define HEAR_ANY	100.0f		/* and anything this near */
#define CHASE_SPEED	360.0f

static void vset (float *r, float x, float y, float z)	{ r[0] = x; r[1] = y; r[2] = z; }
static void vcopy (float *r, const float *a)		{ memcpy (r, a, 3 * sizeof (float)); }
static float clampf (float x, float lo, float hi)	{ return x < lo ? lo : x > hi ? hi : x; }

static float dist2 (const float *a, const float *b)	/* across */
{
	return sqrtf ((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]));
}

static float dist3 (const float *a, const float *b)
{
	return sqrtf ((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2]));
}

static void event (lf_t *s, int type, const float *p)
{
	if (s->n_events < LF_MAX_EVENTS)
	{
		s->event[s->n_events].type = type;
		vcopy (s->event[s->n_events++].p, p ? p : s->g.origin);
	}
}

/* the story's lines of this level with this id, to be shown in their order */
static void say (lf_t *s, int id)
{
	for (int i = 0; i < lf_n_lines; i++)
	{
		if (lf_lines[i].level == s->level && lf_lines[i].id == id && s->n_captions < LF_MAX_CAPTIONS)
		{
			s->caption[s->n_captions++] = &lf_lines[i];
		}
	}
}

const lf_line_t *lf_caption (const lf_t *s)	{ return s->n_captions ? s->caption[0] : NULL; }
float lf_caption_seconds (const lf_line_t *l)	{ return 1.7f + 0.06f * strlen (l->text); }

const char *lf_level_name (int level)
{
	static const char *const names[LF_LEVELS] =
	{
		"THE FERRY DOCKS", "THE DROWNED LIBRARY", "THE BELL FOREST", "THE GLASS MARKET", "THE STAIR OF KINGS", "THE LANTERN PEAK"
	};
	return names[level < 0 ? 0 : level >= LF_LEVELS ? LF_LEVELS - 1 : level];
}

void lf_lantern (const lf_t *s, float p[3])
{
	vset (p, s->g.origin[0] + cosf (s->g.yaw) * 12.0f, s->g.origin[1] + sinf (s->g.yaw) * 12.0f,
	      s->g.origin[2] + 4.0f + 26.0f * s->raised);
}

float lf_glow (const lf_t *s)
{
	return LF_GLOW_LOW + (LF_GLOW_RAISED - LF_GLOW_LOW) * s->raised * (s->ember > 0.0f ? 1.0f : 0.3f);
}

float lf_hollow_reach (const lf_hollow_t *h)
{
	return h->kind == LF_BRUTE ? 62.0f : h->kind == LF_LISTENER ? 46.0f : 40.0f;
}

/* is this in the raised lantern's light (with ember in it: a spent lantern's last spark burns nothing)? */
static bool lit (const lf_t *s, const float *p)
{
	float l[3];
	lf_lantern (s, l);
	return s->raised > 0.5f && s->ember > 0.0f && dist3 (l, p) < LF_GLOW_RAISED * 0.85f;
}

/* ---- walking: a box over the level's floors, down its steps, along its walls ----------------- */

static void clip (float *v, const float *normal)
{
	float d = v[0] * normal[0] + v[1] * normal[1] + v[2] * normal[2];
	for (int k = 0; k < 3; k++)
	{
		v[k] -= normal[k] * d;
	}
}

static void walk_move (const collide_t *c, int hull, float p[3], float v[3], float dt, bool *ground)
{
	float down[3] = {p[0], p[1], p[2] - 2.0f};
	collide_trace_t t = collide_trace (c, hull, p, down, -1);
	*ground = t.fraction < 1.0f && t.normal[2] >= 0.7f;
	if (!*ground)
	{
		v[2] -= GRAVITY * dt;
	}
	else if (v[2] < 0.0f)
	{
		v[2] = 0.0f;
	}
	float left = dt;
	for (int bump = 0; bump < 3 && left > 0.0f; bump++)
	{
		float end[3] = {p[0] + v[0] * left, p[1] + v[1] * left, p[2] + v[2] * left};
		t = collide_trace (c, hull, p, end, -1);
		if (t.all_solid)
		{
			vset (v, 0, 0, 0);
			return;
		}
		if (t.fraction == 1.0f)
		{
			vcopy (p, t.end);
			break;
		}
		if (*ground && t.normal[2] < 0.7f)		/* a step? up, on, down again */
		{
			float up[3] = {p[0], p[1], p[2] + STEP};
			collide_trace_t tu = collide_trace (c, hull, p, up, -1);
			float on[3] = {tu.end[0] + v[0] * left, tu.end[1] + v[1] * left, tu.end[2]};
			collide_trace_t to = collide_trace (c, hull, tu.end, on, -1);
			if (!tu.all_solid && to.fraction > t.fraction + 0.01f)
			{
				float under[3] = {to.end[0], to.end[1], to.end[2] - STEP - 2.0f};
				collide_trace_t td = collide_trace (c, hull, to.end, under, -1);
				if (!td.start_solid && (td.fraction == 1.0f || td.normal[2] >= 0.7f))
				{
					vcopy (p, td.end);
					left -= left * to.fraction;
					if (to.fraction < 1.0f)
					{
						clip (v, to.normal);
					}
					continue;
				}
			}
		}
		vcopy (p, t.end);
		left -= left * t.fraction;
		clip (v, t.normal);
	}
	/* kept to the ground down steps and slopes */
	if (*ground && v[2] <= 0.0f)
	{
		float under[3] = {p[0], p[1], p[2] - STEP};
		t = collide_trace (c, hull, p, under, -1);
		if (!t.start_solid && t.fraction < 1.0f && t.normal[2] >= 0.7f)
		{
			vcopy (p, t.end);
		}
	}
}

/* is there floor a stride ahead (not the sea, not a drop)? */
static bool floor_ahead (const lf_t *s, int hull, const float *p, float dx, float dy)
{
	float l = sqrtf (dx * dx + dy * dy);
	if (l < 1.0f)
	{
		return true;
	}
	float ahead[3] = {p[0] + dx / l * 44.0f, p[1] + dy / l * 44.0f, p[2]}, below[3] = {ahead[0], ahead[1], ahead[2] - 64.0f};
	if (collide_trace (&s->g.collide, hull, p, ahead, -1).fraction < 1.0f)
	{
		return true;				/* (a wall: walked along) */
	}
	collide_trace_t t = collide_trace (&s->g.collide, hull, ahead, below, -1);
	float feet[3] = {t.end[0], t.end[1], t.end[2] - 20.0f};
	return t.fraction < 1.0f && collide_contents (&s->g.collide, 0, feet) != BSP_CONTENTS_WATER;
}

static bool fits (const lf_t *s, int hull, const float *p)
{
	return collide_contents (&s->g.collide, hull, p) != BSP_CONTENTS_SOLID
	       && !collide_trace (&s->g.collide, hull, p, p, -1).start_solid;
}

/* ---- the level's entities ----------------------------------------------------------------- */

static bool find_target (const bsp_t *b, const char *name, bsp_entity_t *out)
{
	const char *cursor = NULL;
	char t[BSP_VALUE];
	while (name[0] && bsp_entity_next (b, &cursor, out))
	{
		if (bsp_entity_value (out, "targetname", t) && strcmp (t, name) == 0)
		{
			return true;
		}
	}
	return false;
}

static void add_brush (lf_t *s, const bsp_entity_t *e, int kind)
{
	char m[BSP_VALUE], key[BSP_VALUE] = "";
	if (!bsp_entity_value (e, "model", m) || m[0] != '*' || s->n_brushes >= LF_MAX_BRUSHES)
	{
		printf ("lf: a brush entity too many (or without its brushes)\n");
		return;
	}
	lf_brush_t *b = &s->brush[s->n_brushes];
	memset (b, 0, sizeof *b);
	b->kind = kind;
	b->model = atoi (m + 1);
	b->solid = collide_add_solid (&s->g.collide, b->model);
	if (b->solid < 0)
	{
		printf ("lf: no room for a brush entity's solid\n");
		return;
	}
	const bsp_model_t *bm = &s->bsp->models[b->model];
	for (int k = 0; k < 3; k++)
	{
		b->centre[k] = (bm->mins[k] + bm->maxs[k]) / 2;
		b->size[k] = bm->maxs[k] - bm->mins[k];
	}
	b->radius = bsp_entity_float (e, "radius", 300.0f);
	bsp_entity_value (e, "key", key);
	b->key = strcmp (key, "beacon") == 0 ? LF_KEY_BEACON : strcmp (key, "plate") == 0 ? LF_KEY_PLATE
	       : strcmp (key, "kill") == 0 ? LF_KEY_KILL : strcmp (key, "anchor") == 0 ? LF_KEY_ANCHOR : LF_KEY_NONE;
	b->key_id = (int) bsp_entity_float (e, "id", 0.0f);
	s->g.collide.solids[b->solid].active = kind != LF_HIDDEN;
	s->n_brushes++;
}

bool lf_load (lf_t *s, const bsp_t *bsp, int level, float ember)
{
	memset (s, 0, sizeof *s);
	s->bsp = bsp;
	s->level = level;
	s->ember = ember;
	s->at_shrine = -1;
	if (!game_init (&s->g, bsp))
	{
		return false;
	}
	const char *cursor = NULL;
	bsp_entity_t e, t;
	char cls[BSP_VALUE], v[BSP_VALUE];
	float origin[3];
	while (bsp_entity_next (bsp, &cursor, &e))
	{
		if (!bsp_entity_value (&e, "classname", cls))
		{
			continue;
		}
		vset (origin, 0, 0, 0);
		bsp_entity_vector (&e, "origin", origin);
		if (strcmp (cls, "lf_hidden") == 0)
			add_brush (s, &e, LF_HIDDEN);
		else if (strcmp (cls, "lf_break") == 0)
			add_brush (s, &e, LF_BREAK);
		else if (strcmp (cls, "lf_gate") == 0)
			add_brush (s, &e, LF_GATE);
		else if (strcmp (cls, "lf_ody") == 0)
		{
			vcopy (s->ody.p, origin);
			s->ody.yaw = bsp_entity_float (&e, "angle", 0.0f) * PI / 180;
			s->ody.state = bsp_entity_float (&e, "asleep", 0.0f) != 0.0f ? LF_ODY_ASLEEP
				     : bsp_entity_float (&e, "stay", 0.0f) != 0.0f ? LF_ODY_STAY : LF_ODY_FOLLOW;
		}
		else if ((strcmp (cls, "lf_lift") == 0 || strcmp (cls, "lf_plate") == 0) && s->n_spots < LF_MAX_SPOTS)
		{
			lf_spot_t *sp = &s->spot[s->n_spots++];
			sp->kind = cls[3] == 'l' ? LF_SPOT_LIFT : LF_SPOT_PLATE;
			sp->id = (int) bsp_entity_float (&e, "id", 0.0f);
			vcopy (sp->p, origin);
			vcopy (sp->top, origin);
			if (sp->kind == LF_SPOT_LIFT && bsp_entity_value (&e, "target", v) && find_target (bsp, v, &t))
			{
				bsp_entity_vector (&t, "origin", sp->top);
			}
		}
		else if (strcmp (cls, "lf_hollow") == 0 && s->n_hollow < LF_MAX_HOLLOW)
		{
			lf_hollow_t *h = &s->hollow[s->n_hollow++];
			h->kind = (int) bsp_entity_float (&e, "kind", 0.0f);
			h->group = (int) bsp_entity_float (&e, "group", 0.0f);
			h->wake = bsp_entity_float (&e, "wake", 450.0f);
			h->hp = h->kind == LF_BRUTE ? 3 : 1;
			vcopy (h->home, origin);
			vcopy (h->p, origin);
		}
		else if (strcmp (cls, "lf_shrine") == 0 && s->n_shrines < LF_MAX_SHRINES)
			vcopy (s->shrine[s->n_shrines++], origin);
		else if (strcmp (cls, "lf_beacon") == 0)
		{
			s->has_beacon = true;
			vcopy (s->beacon, origin);
		}
		else if (strcmp (cls, "lf_exit") == 0)
		{
			s->has_exit = true;
			vcopy (s->exit, origin);
			s->exit_radius = bsp_entity_float (&e, "radius", 90.0f);
		}
		else if (strcmp (cls, "lf_story") == 0 && s->n_stories < LF_MAX_STORIES)
		{
			lf_story_t *st = &s->story[s->n_stories++];
			vcopy (st->p, origin);
			st->radius = bsp_entity_float (&e, "radius", 120.0f);
			st->line = (int) bsp_entity_float (&e, "line", 0.0f);
		}
		else if (strcmp (cls, "lf_king") == 0)
		{
			s->king.here = true;
			vcopy (s->king.home, origin);
			vcopy (s->king.p, origin);
		}
		else if (strcmp (cls, "lf_choice") == 0)
		{
			s->has_choice = true;
			vcopy (s->choice[bsp_entity_float (&e, "which", 0.0f) != 0.0f], origin);
		}
		else if (strcmp (cls, "lf_archivist") == 0)
		{
			s->has_archivist = true;
			vcopy (s->archivist, origin);
			s->archivist_yaw = bsp_entity_float (&e, "angle", 0.0f) * PI / 180;
		}
		else if (strcmp (cls, "lf_bot") == 0)
		{
			int n = (int) bsp_entity_float (&e, "n", 0.0f);
			if (n >= 0 && n < LF_MAX_BOT)
			{
				vcopy (s->step[n].p, origin);
				s->step[n].what[0] = '\0';
				if (bsp_entity_value (&e, "do", v))
				{
					snprintf (s->step[n].what, sizeof s->step[n].what, "%.*s", (int) sizeof s->step[n].what - 1, v);
				}
				s->n_steps = n + 1 > s->n_steps ? n + 1 : s->n_steps;
			}
		}
	}
	for (int i = 0; i < s->n_brushes; i++)			/* the Tide King: a third of him an anchor */
	{
		s->king.hp += s->brush[i].key == LF_KEY_ANCHOR;
	}
	vcopy (s->check, s->g.start);
	s->check_yaw = s->g.start_yaw;
	vcopy (s->safe, s->g.start);
	say (s, LF_LINE_START);
	return true;
}

/* ---- Wren ------------------------------------------------------------------------------------ */

/* struck from `from`: an ember's worth taken, thrown back; with none left the light goes out */
static void hurt (lf_t *s, float amount, const float *from, float knock)
{
	if (s->hurt > 0.0f || s->out > 0.0f || s->ending)
	{
		return;
	}
	if (s->ember <= 0.001f)
	{
		s->out = OUT_SECONDS;
		s->deaths++;
		event (s, LF_EV_OUT, NULL);
		return;
	}
	s->ember = s->ember > amount ? s->ember - amount : 0.0f;
	s->hurt = HURT_SECONDS;
	float d = dist2 (s->g.origin, from);
	d = d < 1.0f ? 1.0f : d;
	s->g.velocity[0] = (s->g.origin[0] - from[0]) / d * knock;
	s->g.velocity[1] = (s->g.origin[1] - from[1]) / d * knock;
	s->g.velocity[2] = 160.0f;
	s->g.on_ground = false;
	event (s, LF_EV_HURT, NULL);
}

/* Ody put where he fits, near a place */
static void ody_place (lf_t *s, const float *near)
{
	static const float off[9][2] = {{-90, 0}, {90, 0}, {0, -90}, {0, 90}, {-70, -70}, {70, 70}, {-70, 70}, {70, -70}, {0, 0}};
	for (int k = 0; k < 9; k++)
	{
		float p[3] = {near[0] + off[k][0], near[1] + off[k][1], near[2] + 2.0f};
		if (fits (s, ODY_HULL, p))
		{
			vcopy (s->ody.p, p);
			vset (s->ody.v, 0, 0, 0);
			return;
		}
	}
}

/* back at the last shrine: half an ember; the Hollow that are left, asleep at their places */
static void back (lf_t *s)
{
	vcopy (s->g.origin, s->check);
	vset (s->g.velocity, 0, 0, 0);
	s->g.yaw = s->check_yaw;
	s->g.view_z = s->g.origin[2] + GAME_EYE;
	s->ember = 0.5f;
	s->hurt = 2.0f;
	s->crumbs = 0;
	for (int i = 0; i < s->n_hollow; i++)
	{
		lf_hollow_t *h = &s->hollow[i];
		if (h->state != LF_H_GONE && h->state != LF_H_DYING)
		{
			h->state = LF_H_ASLEEP;
			h->hp = h->kind == LF_BRUTE ? 3 : 1;
			h->burn = h->t = 0.0f;
			vcopy (h->p, h->home);
			vset (h->v, 0, 0, 0);
		}
	}
	for (int i = 0; i < LF_MAX_BOLTS; i++)
	{
		s->bolt[i].live = false;
	}
	if (s->ody.state > LF_ODY_ASLEEP)
	{
		ody_place (s, s->check);
		s->ody.state = LF_ODY_FOLLOW;
		s->ody.task = LF_DO_NONE;
	}
	s->king.cast = s->king.call = 0.0f;
	if (!s->said_out)
	{
		say (s, LF_LINE_OUT);
		s->said_out = true;
	}
}

static void wren (lf_t *s, const lf_input_t *in, float dt)
{
	game_input_t move = in->move;
	bool frozen = s->out > 0.0f || s->sea > 0.0f;
	if (frozen)
	{
		memset (&move, 0, sizeof move);
	}
	game_update (&s->g, &move, dt);

	/* the light out, the sea: the dark, then back */
	if (s->out > 0.0f && (s->out -= dt) <= 0.0f)
	{
		s->out = 0.0f;
		back (s);
	}
	if (s->sea > 0.0f && (s->sea -= dt) <= 0.0f)
	{
		s->sea = 0.0f;
		vcopy (s->g.origin, s->safe);
		vset (s->g.velocity, 0, 0, 0);
		s->g.view_z = s->g.origin[2] + GAME_EYE;
		s->ember = s->ember > 0.1f ? s->ember - 0.1f : 0.0f;
		s->hurt = 1.5f;
	}
	float knee[3] = {s->g.origin[0], s->g.origin[1], s->g.origin[2] + 2.0f};
	if (!frozen && s->g.contents == BSP_CONTENTS_WATER && collide_contents (&s->g.collide, 0, knee) == BSP_CONTENTS_WATER)
	{
		s->sea = SEA_SECONDS;
		event (s, LF_EV_SEA, NULL);
	}

	/* where she's walked: Ody's way after her, and where the sea gives her back */
	if (s->g.on_ground && s->g.ground < 0 && s->g.contents == BSP_CONTENTS_EMPTY
	    && (s->crumbs == 0 || dist3 (s->g.origin, s->crumb[s->crumbs - 1]) > 56.0f))
	{
		if (s->crumbs == LF_MAX_CRUMBS)
		{
			memmove (s->crumb[0], s->crumb[1], sizeof s->crumb[0] * (LF_MAX_CRUMBS - 1));
			s->crumbs--;
		}
		if (s->crumbs)
		{
			vcopy (s->safe, s->crumb[s->crumbs - 1]);	/* (a stride back from the edge) */
		}
		vcopy (s->crumb[s->crumbs++], s->g.origin);
	}

	/* the lantern: raised while held (spent, its last spark still shows a way
	   and kindles a fire, slowly: it burns nothing); a flare */
	bool can = !frozen && !s->ending;
	if (in->raise && can && s->ember <= 0.0f && !s->spent_said)
	{
		event (s, LF_EV_SPENT, NULL);
		s->spent_said = true;
	}
	s->spent_said = s->spent_said && in->raise;
	float want = in->raise && can ? 1.0f : 0.0f;
	s->raised += (want - s->raised) * (1.0f - expf (-dt * 9.0f));
	if (s->raised > 0.5f)
	{
		s->ember -= dt / LF_RAISED_SECONDS;
		s->ember = s->ember < 0.0f ? 0.0f : s->ember;
	}
	s->flare_cool = s->flare_cool > dt ? s->flare_cool - dt : 0.0f;
	s->flash *= expf (-dt * 3.5f);
	s->hurt = s->hurt > dt ? s->hurt - dt : 0.0f;
	if (in->flare && can && s->ember >= LF_FLARE_COST - 0.001f && s->flare_cool <= 0.0f)	/* (a flare wants its quarter) */
	{
		s->ember -= LF_FLARE_COST;
		s->ember = s->ember < 0.0f ? 0.0f : s->ember;
		s->flare_cool = 1.0f;
		s->flash = 1.0f;
		event (s, LF_EV_FLARE, NULL);
		for (int i = 0; i < s->n_hollow; i++)
		{
			lf_hollow_t *h = &s->hollow[i];
			if (h->state == LF_H_ASLEEP || h->state >= LF_H_DYING || dist3 (h->p, s->g.origin) > LF_FLARE_REACH)
			{
				continue;
			}
			if (h->kind == LF_LURKER)
			{
				h->state = LF_H_DYING;
				h->t = 0.0f;
				event (s, LF_EV_HOLLOW_DIE, h->p);
			}
			else if (h->kind == LF_LISTENER)
			{
				h->state = LF_H_STUN;
				h->t = -1.4f;			/* (longer than a brute's) */
			}
			else if (h->state == LF_H_STUN || h->state == LF_H_RECOVER)
			{
				h->hp--;
				h->state = h->hp > 0 ? LF_H_STUN : LF_H_DYING;
				h->t = 0.0f;
				event (s, h->hp > 0 ? LF_EV_BRUTE_HURT : LF_EV_HOLLOW_DIE, h->p);
			}
		}
	}

	/* an ember picked up */
	if (s->g.picked >= 0)
	{
		s->ember = clampf (s->ember + EMBER_ITEM, 0.0f, 1.0f);
		event (s, LF_EV_EMBER, s->g.items[s->g.picked].origin);
	}
}

/* ---- what the lantern does to the place ------------------------------------------------------------ */

static void ody_come (lf_t *s);

static void place (lf_t *s, float dt)
{
	float lantern[3], spark = s->ember > 0.0f ? 1.0f : 0.5f;	/* (a spent lantern: half as fast) */
	lf_lantern (s, lantern);
	bool up = s->raised > 0.7f;

	/* the shrines: the lantern filled, and here she comes back to */
	int at = -1;
	for (int i = 0; i < s->n_shrines; i++)
	{
		if (dist2 (s->g.origin, s->shrine[i]) < 90.0f && fabsf (s->g.origin[2] - s->shrine[i][2]) < 90.0f)
		{
			at = i;
		}
	}
	if (at >= 0 && at != s->at_shrine)
	{
		vcopy (s->check, s->g.origin);
		s->check_yaw = s->g.yaw;
		s->ember = 1.0f;
		event (s, LF_EV_SHRINE, s->shrine[at]);
	}
	s->at_shrine = at;

	/* the beacon: kindled by the raised lantern */
	if (s->has_beacon && !s->beacon_lit)
	{
		bool near = up && dist3 (lantern, s->beacon) < 170.0f;
		s->kindle = clampf (s->kindle + (near ? spark * dt : -dt) / KINDLE_SECONDS, 0.0f, 1.0f);
		if (s->kindle >= 1.0f)
		{
			s->beacon_lit = true;
			s->ember = 1.0f;
			vcopy (s->check, s->g.origin);
			s->check_yaw = s->g.yaw;
			event (s, LF_EV_BEACON, s->beacon);
			say (s, LF_LINE_BEACON);
			/* the island's light is back: its listeners go; Ody, left waiting, comes */
			for (int i = 0; i < s->n_hollow; i++)
			{
				if (s->hollow[i].kind == LF_LISTENER && s->hollow[i].state < LF_H_DYING)
				{
					s->hollow[i].state = LF_H_DYING;
					s->hollow[i].t = 0.0f;
				}
			}
			if (s->ody.state == LF_ODY_STAY && s->ody.task == LF_DO_NONE)
			{
				s->ody.state = LF_ODY_FOLLOW;
				ody_come (s);
			}
		}
	}

	/* the brushes: ways shown, gates opened */
	for (int i = 0; i < s->n_brushes; i++)
	{
		lf_brush_t *b = &s->brush[i];
		if (b->done)
		{
			continue;
		}
		if (b->kind == LF_HIDDEN)
		{
			bool near = up && dist3 (lantern, b->centre) < b->radius;
			b->open = clampf (b->open + (near ? spark * dt : -0.5f * dt) / REVEAL_SECONDS, 0.0f, 1.0f);
			if (b->open >= 1.0f)
			{
				b->done = true;
				s->g.collide.solids[b->solid].active = true;
				event (s, LF_EV_REVEAL, b->centre);
			}
		}
		else if (b->kind == LF_GATE)
		{
			bool met = false;
			if (b->key == LF_KEY_BEACON)
			{
				met = s->beacon_lit;
			}
			else if (b->key == LF_KEY_PLATE)
			{
				for (int k = 0; k < s->n_spots; k++)
				{
					met = met || (s->spot[k].kind == LF_SPOT_PLATE && s->spot[k].id == b->key_id && s->spot[k].pressed);
				}
			}
			else if (b->key == LF_KEY_KILL)
			{
				met = true;
				for (int k = 0; k < s->n_hollow; k++)
				{
					met = met && (s->hollow[k].group != b->key_id || s->hollow[k].state >= LF_H_DYING);
				}
			}
			if (met && b->open == 0.0f)
			{
				event (s, LF_EV_GATE, b->centre);
				if (b->key == LF_KEY_KILL)
				{
					say (s, LF_LINE_KILL);
				}
			}
			if (met || b->open > 0.0f)			/* (once it starts it goes) */
			{
				b->open = clampf (b->open + dt / 1.6f, 0.0f, 1.0f);
				const float off[3] = {0, 0, -b->size[2] * b->open};
				collide_move_solid (&s->g.collide, b->solid, off);
				if (b->open >= 1.0f)
				{
					b->done = true;
					s->g.collide.solids[b->solid].active = false;
				}
			}
		}
	}

	/* the story's lines, as she comes by; the way out */
	for (int i = 0; i < s->n_stories; i++)
	{
		lf_story_t *st = &s->story[i];
		if (!st->told && dist2 (s->g.origin, st->p) < st->radius && fabsf (s->g.origin[2] - st->p[2]) < 90.0f)
		{
			st->told = true;
			say (s, st->line);
		}
	}
	if (s->has_exit && (!s->has_beacon || s->beacon_lit) && dist2 (s->g.origin, s->exit) < s->exit_radius
	    && fabsf (s->g.origin[2] - s->exit[2]) < 90.0f)
	{
		s->done = true;
	}

	/* the peak's choice: the ember raised at the great lantern, or over the tide pool */
	if (s->has_choice && !s->ending && (!s->king.here || s->king.beaten))
	{
		for (int k = 0; k < 2; k++)
		{
			bool near = up && dist3 (lantern, s->choice[k]) < 170.0f;
			s->choosing[k] = clampf (s->choosing[k] + (near ? dt : -dt) / 2.2f, 0.0f, 1.0f);
			if (s->choosing[k] >= 1.0f)
			{
				s->ending = k + 1;
				s->beacon_lit = s->beacon_lit || k == 0;
				event (s, LF_EV_ENDING, s->choice[k]);
				s->n_captions = 0;
				s->caption_t = 0.0f;
				say (s, k == 0 ? LF_LINE_RELIT : LF_LINE_DARK);
				break;
			}
		}
	}
}

/* ---- Ody ------------------------------------------------------------------------------------- */

/* what Ody would do, told now: the nearest thing that asks for him */
static int ody_can (const lf_t *s, int *index)
{
	const lf_ody_t *o = &s->ody;
	float best = 1e9f;
	int what = LF_DO_NONE;
	*index = -1;
	if (o->state <= LF_ODY_ASLEEP || o->state == LF_ODY_SWING || s->out > 0.0f)
	{
		return LF_DO_NONE;
	}
	if (o->state == LF_ODY_KNEEL || o->state == LF_ODY_STAY || o->state == LF_ODY_GO)
	{
		return LF_DO_COME;
	}
	for (int i = 0; i < s->n_hollow && o->cool <= 0.0f; i++)
	{
		const lf_hollow_t *h = &s->hollow[i];
		float d = dist3 (h->p, s->g.origin);
		if (h->kind == LF_BRUTE && h->state != LF_H_ASLEEP && h->state < LF_H_DYING && d < 650.0f && d < best)
		{
			best = d;
			what = LF_DO_STRIKE;
			*index = i;
		}
	}
	if (what != LF_DO_NONE)
	{
		return what;
	}
	for (int i = 0; i < s->n_brushes; i++)
	{
		const lf_brush_t *b = &s->brush[i];
		float d = dist3 (b->centre, s->g.origin);
		if (b->kind == LF_BREAK && !b->done && d < 340.0f && d < best)
		{
			best = d;
			what = LF_DO_SMASH;
			*index = i;
		}
	}
	for (int i = 0; i < s->n_spots; i++)
	{
		const lf_spot_t *sp = &s->spot[i];
		float d = dist3 (sp->p, s->g.origin);
		if (d < 270.0f && d < best && !(sp->kind == LF_SPOT_PLATE && sp->pressed))
		{
			best = d;
			what = sp->kind == LF_SPOT_LIFT ? LF_DO_LIFT : LF_DO_STAND;
			*index = i;
		}
	}
	return what != LF_DO_NONE ? what : dist3 (o->p, s->g.origin) > 300.0f ? LF_DO_COME : LF_DO_NONE;
}

/* brought to where Wren has been, where he fits, not on top of her */
static void ody_come (lf_t *s)
{
	lf_ody_t *o = &s->ody;
	for (int i = s->crumbs - 1; i >= 0; i--)
	{
		float p[3] = {s->crumb[i][0], s->crumb[i][1], s->crumb[i][2] + 2.0f};
		if (dist2 (p, s->g.origin) > 130.0f && fabsf (p[2] - s->g.origin[2]) < 80.0f && fits (s, ODY_HULL, p))
		{
			vcopy (o->p, p);
			vset (o->v, 0, 0, 0);
			o->stuck = 0.0f;
			s->crumbs = 0;
			event (s, LF_EV_ODY_COME, o->p);
			return;
		}
	}
}

/* towards a place, at his pace; true when there */
static bool ody_walk (lf_t *s, const float *to, float near, float dt)
{
	lf_ody_t *o = &s->ody;
	float dx = to[0] - o->p[0], dy = to[1] - o->p[1], d = sqrtf (dx * dx + dy * dy);
	bool there = d < near;
	float want[2] = {0, 0};
	if (!there && floor_ahead (s, ODY_HULL, o->p, dx, dy))
	{
		want[0] = dx / d * ODY_SPEED;
		want[1] = dy / d * ODY_SPEED;
		float a = atan2f (dy, dx) - o->yaw;
		a -= 2 * PI * floorf ((a + PI) / (2 * PI));
		o->yaw += clampf (a, -5.0f * dt, 5.0f * dt);
	}
	float w = 1.0f - expf (-dt * 8.0f), before[3];
	o->v[0] += (want[0] - o->v[0]) * w;
	o->v[1] += (want[1] - o->v[1]) * w;
	vcopy (before, o->p);
	walk_move (&s->g.collide, ODY_HULL, o->p, o->v, dt, &o->ground);
	float moved = dist2 (before, o->p);
	int strides = (int) (o->stride / 70.0f);
	o->stride += moved;
	if ((int) (o->stride / 70.0f) != strides && o->ground)
	{
		event (s, LF_EV_ODY_STEP, o->p);
	}
	o->stuck = !there && moved < 30.0f * dt ? o->stuck + dt : 0.0f;
	return there;
}

static void ody (lf_t *s, const lf_input_t *in, float dt)
{
	lf_ody_t *o = &s->ody;
	int index;
	s->ody_can = ody_can (s, &index);
	if (o->state == LF_ODY_ABSENT)
	{
		return;
	}
	o->t += dt;
	o->cool = o->cool > dt ? o->cool - dt : 0.0f;
	if (o->state == LF_ODY_ASLEEP)				/* woken by the raised lantern */
	{
		float l[3];
		lf_lantern (s, l);
		bool near = s->raised > 0.7f && dist3 (l, o->p) < 240.0f;
		o->wake = clampf (o->wake + (near ? dt : -dt) / WAKE_SECONDS, 0.0f, 1.0f);
		if (o->wake >= 1.0f)
		{
			o->state = LF_ODY_FOLLOW;
			o->t = 0.0f;
			s->crumbs = 0;
			event (s, LF_EV_ODY_WAKE, o->p);
			say (s, LF_LINE_ODY_WAKE);
		}
		return;
	}

	/* told to */
	if (in->ody && s->ody_can != LF_DO_NONE)
	{
		o->task = s->ody_can;
		o->index = index;
		o->t = o->stuck = 0.0f;
		o->state = LF_ODY_GO;
		if (o->task == LF_DO_COME)
		{
			o->state = LF_ODY_FOLLOW;
			o->task = LF_DO_NONE;
			if (dist3 (o->p, s->g.origin) > 420.0f)
			{
				ody_come (s);
			}
		}
		else if (o->task == LF_DO_SMASH)		/* to the wall's face, his side of it */
		{
			const lf_brush_t *b = &s->brush[index];
			float d[2] = {o->p[0] - b->centre[0], o->p[1] - b->centre[1]};
			if (b->size[0] < b->size[1] * 0.7f)
				vset (o->goal, b->centre[0] + (d[0] > 0 ? 1 : -1) * (b->size[0] / 2 + 50.0f), b->centre[1], o->p[2]);
			else if (b->size[1] < b->size[0] * 0.7f)
				vset (o->goal, b->centre[0], b->centre[1] + (d[1] > 0 ? 1 : -1) * (b->size[1] / 2 + 50.0f), o->p[2]);
			else
			{
				float l = sqrtf (d[0] * d[0] + d[1] * d[1]) + 1.0f, r = (b->size[0] + b->size[1]) / 4 + 56.0f;
				vset (o->goal, b->centre[0] + d[0] / l * r, b->centre[1] + d[1] / l * r, o->p[2]);
			}
		}
		else if (o->task == LF_DO_LIFT || o->task == LF_DO_STAND)
		{
			vcopy (o->goal, s->spot[index].p);
		}
	}

	switch (o->state)
	{
	case LF_ODY_FOLLOW:
	{
		float d = dist3 (o->p, s->g.origin);
		const float *to = s->crumbs ? s->crumb[0] : s->g.origin;
		float aside[3] = {o->p[0] * 2 - s->g.origin[0], o->p[1] * 2 - s->g.origin[1], o->p[2]};
		if (d < ODY_WIDE + 22.0f)			/* in her way: he steps back from her */
		{
			s->crumbs = 0;
			to = aside;
			d = 1e9f;
		}
		else if (d < ODY_NEAR)
		{
			s->crumbs = 0;
			to = o->p;
		}
		else if (s->crumbs && dist2 (o->p, s->crumb[0]) < 48.0f)
		{
			memmove (s->crumb[0], s->crumb[1], sizeof s->crumb[0] * (LF_MAX_CRUMBS - 1));
			s->crumbs--;
			to = s->crumbs ? s->crumb[0] : s->g.origin;
		}
		ody_walk (s, to, d < ODY_NEAR ? 1e9f : 24.0f, dt);
		d = dist3 (o->p, s->g.origin);
		if (d > 950.0f || (o->stuck > 2.5f && d > 420.0f))	/* left behind: he comes by his own ways */
		{
			ody_come (s);
		}
		break;
	}
	case LF_ODY_GO:
	{
		if (o->task == LF_DO_STRIKE)
		{
			const lf_hollow_t *h = &s->hollow[o->index];
			if (h->state >= LF_H_DYING || h->state == LF_H_ASLEEP)
			{
				o->state = LF_ODY_FOLLOW;
				break;
			}
			vcopy (o->goal, h->p);
		}
		bool there = ody_walk (s, o->goal, o->task == LF_DO_STRIKE ? ODY_REACH : 34.0f, dt);
		if (!there && (o->stuck > 3.0f || o->t > 7.0f) && o->task != LF_DO_STRIKE)	/* (he gets there) */
		{
			float p[3] = {o->goal[0], o->goal[1], o->goal[2] + 2.0f};
			if (fits (s, ODY_HULL, p))
			{
				vcopy (o->p, p);
				there = true;
			}
			else
			{
				o->state = LF_ODY_FOLLOW;
			}
		}
		if (there)
		{
			o->t = 0.0f;
			vset (o->v, 0, 0, o->v[2]);
			if (o->task == LF_DO_SMASH || o->task == LF_DO_STRIKE)
			{
				const float *at = o->task == LF_DO_SMASH ? s->brush[o->index].centre : s->hollow[o->index].p;
				o->yaw = atan2f (at[1] - o->p[1], at[0] - o->p[0]);
				o->state = LF_ODY_SWING;
				event (s, LF_EV_ODY_SWING, o->p);
			}
			else if (o->task == LF_DO_LIFT)
			{
				const lf_spot_t *sp = &s->spot[o->index];
				o->yaw = atan2f (sp->top[1] - sp->p[1], sp->top[0] - sp->p[0]);
				o->state = LF_ODY_KNEEL;
				event (s, LF_EV_ODY_KNEEL, o->p);
			}
			else
			{
				o->state = LF_ODY_STAY;
			}
		}
		break;
	}
	case LF_ODY_SWING:				/* the arm up, then down: the blow at 0.45 s */
	{
		float before = o->swing;
		o->swing = clampf (o->t / 0.75f, 0.0f, 1.0f);
		ody_walk (s, o->p, 1e9f, dt);
		if (before < 0.6f && o->swing >= 0.6f)
		{
			if (o->task == LF_DO_SMASH)
			{
				lf_brush_t *b = &s->brush[o->index];
				b->done = true;
				b->open = 1.0f;
				s->g.collide.solids[b->solid].active = false;
				event (s, LF_EV_ODY_SMASH, b->centre);
			}
			else
			{
				lf_hollow_t *h = &s->hollow[o->index];
				if (h->state < LF_H_DYING && dist3 (h->p, o->p) < ODY_REACH + 70.0f)
				{
					h->hp--;
					h->state = h->hp > 0 ? LF_H_STUN : LF_H_DYING;
					h->t = 0.0f;
					event (s, h->hp > 0 ? LF_EV_BRUTE_HURT : LF_EV_HOLLOW_DIE, h->p);
				}
				event (s, LF_EV_ODY_SMASH, h->p);
				o->cool = 2.5f;
			}
		}
		if (o->t > 1.1f)
		{
			o->swing = 0.0f;
			o->state = LF_ODY_FOLLOW;
			o->task = LF_DO_NONE;
		}
		break;
	}
	case LF_ODY_KNEEL:				/* Wren steps to him: thrown up through the arc's top */
	{
		const lf_spot_t *sp = &s->spot[o->index];
		ody_walk (s, o->p, 1e9f, dt);
		if (o->task == LF_DO_LIFT && dist2 (s->g.origin, o->p) < ODY_WIDE + 16.0f && fabsf (s->g.origin[2] - o->p[2]) < 60.0f
		    && s->g.on_ground && o->t > 0.4f)
		{
			float h = sp->top[2] - s->g.origin[2], time = sqrtf ((h > 8.0f ? h : 8.0f) / (0.5f * GRAVITY));
			vset (s->g.velocity, (sp->top[0] - s->g.origin[0]) / time, (sp->top[1] - s->g.origin[1]) / time, time * GRAVITY);
			s->g.on_ground = false;
			s->g.ground = -1;
			s->g.events |= GAME_PUSHED;
			o->task = LF_DO_NONE;
			o->t = 0.0f;
			o->swing = 1.0f;
			event (s, LF_EV_ODY_THROW, o->p);
		}
		if (o->task == LF_DO_NONE)
		{
			o->swing = clampf (1.0f - o->t / 0.8f, 0.0f, 1.0f);
			if (o->t > 1.5f)
			{
				o->state = LF_ODY_FOLLOW;
			}
		}
		break;
	}
	case LF_ODY_STAY:
		ody_walk (s, o->p, 1e9f, dt);
		break;
	}

	/* the plates: his weight only */
	for (int i = 0; i < s->n_spots; i++)
	{
		lf_spot_t *sp = &s->spot[i];
		bool on = sp->kind == LF_SPOT_PLATE && dist2 (o->p, sp->p) < 50.0f && fabsf (o->p[2] - sp->p[2]) < 60.0f;
		if (on && !sp->pressed)
		{
			event (s, LF_EV_PLATE, sp->p);
		}
		sp->pressed = sp->kind == LF_SPOT_PLATE && on;
	}

	/* Wren doesn't walk through him where he's at work (kneeling, standing on
	   a plate, asleep); following her, he gives way, and she gets by */
	float d = dist2 (s->g.origin, o->p);
	if (d < ODY_WIDE && fabsf (s->g.origin[2] - o->p[2]) < 70.0f && s->out <= 0.0f && o->state != LF_ODY_FOLLOW && o->state != LF_ODY_GO)
	{
		float l = d < 1.0f ? 1.0f : d;
		float to[3] = {o->p[0] + (s->g.origin[0] - o->p[0]) / l * ODY_WIDE, o->p[1] + (s->g.origin[1] - o->p[1]) / l * ODY_WIDE,
			       s->g.origin[2]};
		collide_trace_t t = collide_trace (&s->g.collide, 1, s->g.origin, to, -1);
		if (!t.start_solid)
		{
			vcopy (s->g.origin, t.end);
		}
	}
}

/* ---- the Hollow --------------------------------------------------------------------------------- */

static void hollow (lf_t *s, lf_hollow_t *h, float dt)
{
	const lf_ody_t *o = &s->ody;
	int hull = h->kind == LF_BRUTE ? 2 : 1;
	float d = dist3 (h->p, s->g.origin), dx = s->g.origin[0] - h->p[0], dy = s->g.origin[1] - h->p[1];
	float l = sqrtf (dx * dx + dy * dy) + 0.001f, want[2] = {0, 0}, speed = 0.0f;
	bool in_light = lit (s, h->p), hunted = s->out <= 0.0f && s->sea <= 0.0f && !s->ending;
	h->t += dt;
	switch (h->state)
	{
	case LF_H_ASLEEP:
		if (d < h->wake && hunted && h->kind != LF_LISTENER && fabsf (s->g.origin[2] - h->p[2]) < 160.0f)
		{
			h->state = LF_H_STALK;
			h->t = 0.0f;
			event (s, LF_EV_HOLLOW_WAKE, h->p);
			if (!s->said_hollow && h->kind == LF_LURKER)
			{
				s->said_hollow = true;
				say (s, LF_LINE_HOLLOW);
			}
			if (!s->said_brute && h->kind == LF_BRUTE)
			{
				s->said_brute = true;
				say (s, LF_LINE_BRUTE);
			}
		}
		else if (h->kind == LF_LISTENER && hunted && fabsf (s->g.origin[2] - h->p[2]) < 120.0f)
		{
			/* a listener: running feet, a raised lantern, or anything at its side */
			float run = sqrtf (s->g.velocity[0] * s->g.velocity[0] + s->g.velocity[1] * s->g.velocity[1]);
			if ((d < HEAR_RUN && run > 190.0f) || (d < HEAR_LIGHT && s->raised > 0.5f) || d < HEAR_ANY)
			{
				h->state = LF_H_STALK;
				h->t = 0.0f;
				event (s, LF_EV_SHRIEK, h->p);
				say (s, LF_LINE_CAUGHT);
			}
		}
		return;

	case LF_H_STALK:
		if (!hunted)
		{
			h->state = LF_H_BACK;
			break;
		}
		if (h->kind == LF_LURKER)
		{
			if (in_light)
			{
				h->state = LF_H_FLEE;
				break;
			}
			speed = LURK_SPEED;
		}
		else if (h->kind == LF_LISTENER)
		{
			speed = CHASE_SPEED;
			if (h->t > 4.0f)
			{
				h->state = LF_H_BACK;
			}
		}
		else						/* a brute: nearer, till it rears */
		{
			speed = in_light && d < 300.0f ? 0.0f : BRUTE_SPEED;
			if (d < BRUTE_RANGE || h->t > 6.0f)
			{
				h->state = LF_H_WINDUP;
				h->t = 0.0f;
				event (s, LF_EV_WINDUP, h->p);
			}
		}
		want[0] = dx / l * speed;
		want[1] = dy / l * speed;
		h->yaw = atan2f (dy, dx);
		break;

	case LF_H_FLEE:						/* a lurker in the light: away, burning */
		h->burn += dt / BURN_SECONDS;
		if (h->burn >= 1.0f)
		{
			h->state = LF_H_DYING;
			h->t = 0.0f;
			event (s, LF_EV_HOLLOW_DIE, h->p);
			break;
		}
		if (!in_light && h->t > 0.6f)
		{
			h->state = LF_H_STALK;
			h->t = 0.0f;
		}
		want[0] = -dx / l * FLEE_SPEED;
		want[1] = -dy / l * FLEE_SPEED;
		break;

	case LF_H_WINDUP:					/* rearing: it turns with her, then goes where she was */
		h->yaw = atan2f (dy, dx);
		h->dir[0] = dx / l;
		h->dir[1] = dy / l;
		if (h->t > WINDUP_SECONDS)
		{
			h->state = LF_H_LUNGE;
			h->t = 0.0f;
			event (s, LF_EV_LUNGE, h->p);
		}
		break;

	case LF_H_LUNGE:
		want[0] = h->dir[0] * LUNGE_SPEED;
		want[1] = h->dir[1] * LUNGE_SPEED;
		h->v[0] = want[0];
		h->v[1] = want[1];
		if (h->t > LUNGE_SECONDS)
		{
			h->state = LF_H_RECOVER;
			h->t = 0.0f;
		}
		else if (o->state > LF_ODY_ASLEEP && dist3 (h->p, o->p) < 100.0f)	/* into Ody: he doesn't give */
		{
			h->state = LF_H_STUN;
			h->t = 0.0f;
			event (s, LF_EV_THUD, h->p);
		}
		break;

	case LF_H_RECOVER:
		if (h->t > RECOVER_SECONDS)
		{
			h->state = LF_H_STALK;
			h->t = 0.0f;
		}
		break;

	case LF_H_STUN:
		if (h->t > STUN_SECONDS)
		{
			h->state = h->kind == LF_LISTENER ? LF_H_BACK : LF_H_STALK;
			h->t = 0.0f;
		}
		break;

	case LF_H_BACK:						/* home, and asleep again */
	{
		float hx = h->home[0] - h->p[0], hy = h->home[1] - h->p[1], hl = sqrtf (hx * hx + hy * hy) + 0.001f;
		want[0] = hx / hl * LURK_SPEED;
		want[1] = hy / hl * LURK_SPEED;
		h->yaw = atan2f (hy, hx);
		if (hl < 30.0f || h->t > 8.0f)
		{
			h->state = LF_H_ASLEEP;
			vcopy (h->p, h->home);
			vset (h->v, 0, 0, 0);
			return;
		}
		break;
	}
	case LF_H_DYING:
		if (h->t > 0.9f)
		{
			h->state = LF_H_GONE;
		}
		return;
	default:
		return;
	}
	if (h->state != LF_H_FLEE && h->burn > 0.0f)
	{
		h->burn = h->burn > 0.4f * dt ? h->burn - 0.4f * dt : 0.0f;
	}

	/* it moves: not off the floors, not into the sea */
	if (!floor_ahead (s, hull, h->p, want[0], want[1]))
	{
		want[0] = want[1] = 0.0f;
		if (h->state == LF_H_LUNGE)
		{
			h->state = LF_H_RECOVER;
			h->t = 0.0f;
		}
	}
	float w = h->state == LF_H_LUNGE ? 1.0f : 1.0f - expf (-dt * 6.0f), before[3];
	h->v[0] += (want[0] - h->v[0]) * w;
	h->v[1] += (want[1] - h->v[1]) * w;
	vcopy (before, h->p);
	walk_move (&s->g.collide, hull, h->p, h->v, dt, &h->ground);
	if (h->state == LF_H_LUNGE && dist2 (before, h->p) < 0.4f * LUNGE_SPEED * dt)	/* into a wall */
	{
		h->state = LF_H_STUN;
		h->t = 0.0f;
		event (s, LF_EV_THUD, h->p);
	}

	/* its touch */
	if (hunted && dist2 (h->p, s->g.origin) < lf_hollow_reach (h) && fabsf (h->p[2] - s->g.origin[2]) < 60.0f
	    && (h->state == LF_H_STALK || h->state == LF_H_LUNGE))
	{
		bool was = s->hurt > 0.0f;
		hurt (s, h->kind == LF_BRUTE ? 0.3f : h->kind == LF_LISTENER ? 0.4f : 0.2f, h->p, h->kind == LF_BRUTE ? 450.0f : 300.0f);
		if (!was && h->kind != LF_BRUTE)
		{
			h->state = h->kind == LF_LISTENER ? LF_H_BACK : LF_H_FLEE;
			h->t = 0.0f;
		}
	}
}

/* ---- the Tide King ---------------------------------------------------------------------------------- */

static void king (lf_t *s, float dt)
{
	lf_king_t *k = &s->king;
	if (!k->here || k->beaten)
	{
		return;
	}
	float centre[3] = {0, 0, 0};
	int anchors = 0, broken = 0;
	for (int i = 0; i < s->n_brushes; i++)
	{
		if (s->brush[i].key == LF_KEY_ANCHOR)
		{
			for (int c = 0; c < 3; c++)
			{
				centre[c] += s->brush[i].centre[c];
			}
			anchors++;
			broken += s->brush[i].done;
		}
	}
	if (!anchors)
	{
		return;
	}
	vset (centre, centre[0] / anchors, centre[1] / anchors, centre[2] / anchors);
	if (!k->fighting)
	{
		if (dist2 (s->g.origin, centre) < 620.0f)
		{
			k->fighting = true;
			k->cast = 1.5f;
			k->call = 3.5f;
			say (s, LF_LINE_FIGHT);
		}
		return;
	}

	/* an anchor broken: he falls to the middle, and lies there till the lantern burns a third of him */
	if (!k->down && anchors - broken < k->hp)
	{
		k->down = true;
		k->burn = 0.0f;
		event (s, LF_EV_KING_FALL, k->p);
	}
	float to[3], w = 1.0f - expf (-dt * 2.0f);
	vcopy (to, k->down ? centre : k->home);
	to[2] = k->down ? centre[2] - 10.0f : k->home[2] + 14.0f * sinf (s->time * 1.3f);
	for (int c = 0; c < 3; c++)
	{
		k->p[c] += (to[c] - k->p[c]) * w;
	}
	if (k->down)
	{
		float l[3];
		lf_lantern (s, l);
		bool near = s->raised > 0.7f && s->ember > 0.0f && dist3 (l, k->p) < 300.0f;
		k->burn = clampf (k->burn + (near ? dt : -0.5f * dt) / 1.6f, 0.0f, 1.0f);
		if (k->burn >= 1.0f)
		{
			k->hp--;
			k->down = false;
			k->burn = 0.0f;
			k->cast = 1.0f;
			k->call = 3.0f;
			event (s, LF_EV_KING_HURT, k->p);
			float d = dist2 (s->g.origin, k->p) + 1.0f;
			s->g.velocity[0] = (s->g.origin[0] - k->p[0]) / d * 380.0f;
			s->g.velocity[1] = (s->g.origin[1] - k->p[1]) / d * 380.0f;
			s->g.velocity[2] = 150.0f;
			s->g.on_ground = false;
			if (k->hp <= 0)
			{
				k->beaten = true;
				for (int i = 0; i < s->n_hollow; i++)
				{
					if (s->hollow[i].state < LF_H_DYING)
					{
						s->hollow[i].state = s->hollow[i].state == LF_H_ASLEEP ? LF_H_GONE : LF_H_DYING;
						s->hollow[i].t = 0.0f;
					}
				}
				for (int i = 0; i < LF_MAX_BOLTS; i++)
				{
					s->bolt[i].live = false;
				}
				event (s, LF_EV_KING_BEATEN, k->p);
				say (s, LF_LINE_BEATEN);
			}
			else
			{
				say (s, LF_LINE_STAGGER);
			}
		}
		return;
	}
	if (s->out > 0.0f)
	{
		return;
	}

	/* over the peak: his bolts of black water at her, the Hollow called up */
	if ((k->cast += dt) > 3.0f)
	{
		k->cast = 0.0f;
		for (int i = 0; i < LF_MAX_BOLTS; i++)
		{
			lf_bolt_t *b = &s->bolt[i];
			if (!b->live)
			{
				float d = dist3 (s->g.origin, k->p) + 1.0f;
				b->live = true;
				b->age = 0.0f;
				vcopy (b->p, k->p);
				for (int c = 0; c < 3; c++)
				{
					b->v[c] = (s->g.origin[c] - k->p[c]) / d * 300.0f;
				}
				event (s, LF_EV_BOLT, k->p);
				break;
			}
		}
	}
	if ((k->call += dt) > 5.5f)
	{
		k->call = 0.0f;
		int awake = 0;
		for (int i = 0; i < s->n_hollow; i++)
		{
			awake += s->hollow[i].state != LF_H_ASLEEP && s->hollow[i].state < LF_H_DYING;
		}
		for (int i = 0; i < s->n_hollow && awake < 2; i++)
		{
			lf_hollow_t *h = &s->hollow[i];
			if (h->kind == LF_LURKER && (h->state == LF_H_ASLEEP || h->state == LF_H_GONE))
			{
				vcopy (h->p, h->home);
				vset (h->v, 0, 0, 0);
				h->state = LF_H_STALK;
				h->burn = h->t = 0.0f;
				h->hp = 1;
				event (s, LF_EV_HOLLOW_WAKE, h->p);
				break;
			}
		}
	}
}

static void bolts (lf_t *s, float dt)
{
	for (int i = 0; i < LF_MAX_BOLTS; i++)
	{
		lf_bolt_t *b = &s->bolt[i];
		if (!b->live)
		{
			continue;
		}
		float end[3] = {b->p[0] + b->v[0] * dt, b->p[1] + b->v[1] * dt, b->p[2] + b->v[2] * dt};
		collide_trace_t t = collide_trace (&s->g.collide, 0, b->p, end, -1);
		vcopy (b->p, t.end);
		b->age += dt;
		if (dist3 (b->p, s->g.origin) < 44.0f)
		{
			hurt (s, 0.25f, b->p, 260.0f);
			b->live = false;
		}
		else if (t.fraction < 1.0f || b->age > 6.0f || lit (s, b->p))	/* (the raised lantern burns them off) */
		{
			b->live = false;
		}
	}
}

/* ---- a step --------------------------------------------------------------------------------- */

void lf_update (lf_t *s, const lf_input_t *in, float dt)
{
	s->n_events = 0;
	s->time += dt;
	wren (s, in, dt);
	place (s, dt);
	ody (s, in, dt);
	for (int i = 0; i < s->n_hollow; i++)
	{
		hollow (s, &s->hollow[i], dt);
	}
	king (s, dt);
	bolts (s, dt);

	/* the story's lines, one after another */
	if (s->n_captions)
	{
		if (s->caption_t == 0.0f)
		{
			event (s, LF_EV_CAPTION, NULL);
		}
		s->caption_t += dt;
		if (s->caption_t > lf_caption_seconds (s->caption[0]))
		{
			memmove (&s->caption[0], &s->caption[1], sizeof s->caption[0] * (LF_MAX_CAPTIONS - 1));
			s->n_captions--;
			s->caption_t = 0.0f;
		}
	}
}
