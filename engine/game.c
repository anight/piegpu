/*
 * game.c - see game.h. The movement after Quake's (its constants): a slide
 * along the planes hit (up to 4 a step), on the ground the better of that and
 * the same raised by a step and put down again.
 */
#include "game.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GRAVITY		800.0f
#define MAX_SPEED	320.0f
#define ACCEL		10.0f
#define AIR_ACCEL	10.0f
#define AIR_WISH	30.0f		/* the most speed wished in the air */
#define FRICTION	4.0f
#define STOP_SPEED	100.0f
#define JUMP_SPEED	270.0f
#define STEP		18.0f
#define WALKABLE	0.7f		/* a floor's normal's z at least */
#define HULL		1		/* the player's */
#define DOOR_REACH	60.0f		/* a door opens with the player this near */

static void vset (float *r, float x, float y, float z)	{ r[0] = x; r[1] = y; r[2] = z; }
static float vdot (const float *a, const float *b)	{ return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

/* ---- the movers ------------------------------------------------------------------- */

static void add_mover (game_t *g, const bsp_entity_t *e, bool plat)
{
	const char *m = bsp_entity_value (e, "model");
	if (!m || m[0] != '*' || g->n_movers >= GAME_MAX_MOVERS)
	{
		return;
	}
	game_mover_t *v = &g->movers[g->n_movers];
	memset (v, 0, sizeof *v);
	v->model = atoi (m + 1);
	v->solid = collide_add_solid (&g->collide, v->model);
	if (v->solid < 0)
	{
		return;
	}
	v->plat = plat;
	const char *s = bsp_entity_value (e, "speed");
	v->speed = s ? (float) atof (s) : plat ? 150.0f : 100.0f;
	s = bsp_entity_value (e, "wait");
	v->wait = s ? (float) atof (s) : 3.0f;
	const bsp_model_t *bm = &g->bsp->models[v->model];
	if (plat)			/* drawn at the top; starts at the bottom */
	{
		s = bsp_entity_value (e, "height");
		float h = s ? (float) atof (s) : bm->maxs[2] - bm->mins[2] - 8;
		vset (v->pos[0], 0, 0, -h);
		vset (v->pos[1], 0, 0, 0);
	}
	else				/* drawn closed; opens along its angle by its size less lip */
	{
		s = bsp_entity_value (e, "angle");
		float angle = s ? (float) atof (s) : 0.0f, dir[3];
		if (angle == -1.0f)
			vset (dir, 0, 0, 1);
		else if (angle == -2.0f)
			vset (dir, 0, 0, -1);
		else
			vset (dir, cosf (angle * 3.14159265f / 180), sinf (angle * 3.14159265f / 180), 0);
		float size[3] = {bm->maxs[0] - bm->mins[0], bm->maxs[1] - bm->mins[1], bm->maxs[2] - bm->mins[2]};
		s = bsp_entity_value (e, "lip");
		float lip = s ? (float) atof (s) : 8.0f;
		float dist = fabsf (dir[0]) * size[0] + fabsf (dir[1]) * size[1] + fabsf (dir[2]) * size[2] - lip;
		vset (v->pos[0], 0, 0, 0);
		vset (v->pos[1], dir[0] * dist, dir[1] * dist, dir[2] * dist);
	}
	memcpy (v->at, v->pos[0], sizeof v->at);
	collide_move_solid (&g->collide, v->solid, v->at);
	g->n_movers++;
}

/* is the player by this mover: at a door, on a lift */
static bool player_near (const game_t *g, const game_mover_t *v)
{
	const bsp_model_t *bm = &g->bsp->models[v->model];
	float r = v->plat ? -8.0f : DOOR_REACH;		/* (on a lift: over its top, inside its edges) */
	for (int k = 0; k < 2; k++)
	{
		if (g->origin[k] < bm->mins[k] + v->at[k] - r - 16 || g->origin[k] > bm->maxs[k] + v->at[k] + r + 16)
		{
			return false;
		}
	}
	float bottom = bm->mins[2] + v->at[2], top = bm->maxs[2] + v->at[2];
	return v->plat ? g->origin[2] - 24 >= top - 2 && g->origin[2] - 24 <= top + 36
		       : g->origin[2] + 32 >= bottom - DOOR_REACH && g->origin[2] - 24 <= top + DOOR_REACH;
}

static void update_mover (game_t *g, game_mover_t *v, float dt)
{
	bool near = player_near (g, v);
	switch (v->state)
	{
	case 0:
		if (near)
			v->state = 1;
		break;
	case 2:
		if (v->plat && near)
			v->timer = v->wait;		/* (held up while stood on) */
		if ((v->timer -= dt) <= 0.0f)
			v->state = 3;
		break;
	}
	if (v->state != 1 && v->state != 3)
	{
		return;
	}

	/* towards the end its state goes to */
	const float *to = v->pos[v->state == 1];
	float d[3] = {to[0] - v->at[0], to[1] - v->at[1], to[2] - v->at[2]}, len = sqrtf (vdot (d, d)), step = v->speed * dt;
	float move[3];
	bool arrived = len <= step;
	for (int k = 0; k < 3; k++)
	{
		move[k] = arrived ? d[k] : d[k] / len * step;
	}

	/* the player on it, or in its way, goes with it; if it can't, the mover
	   gives way (a door goes back up; a lift waits) */
	float was[3];
	memcpy (was, v->at, sizeof was);
	for (int k = 0; k < 3; k++)
	{
		v->at[k] += move[k];
	}
	collide_move_solid (&g->collide, v->solid, v->at);
	bool carried = g->on_ground && g->ground == v->solid;
	if (carried || collide_trace_solid (&g->collide, v->solid, HULL, g->origin, g->origin).start_solid)
	{
		float to_p[3] = {g->origin[0] + move[0], g->origin[1] + move[1], g->origin[2] + move[2]};
		collide_trace_t t = collide_trace (&g->collide, HULL, g->origin, to_p, v->solid);
		if (!t.start_solid && !collide_trace_solid (&g->collide, v->solid, HULL, t.end, t.end).start_solid)
		{
			memcpy (g->origin, t.end, sizeof g->origin);
		}
		else
		{
			memcpy (v->at, was, sizeof v->at);	/* blocked */
			collide_move_solid (&g->collide, v->solid, v->at);
			if (v->state == 3 && !v->plat)
			{
				v->state = 1;
			}
			return;
		}
	}
	if (arrived)
	{
		v->state = v->state == 1 ? 2 : 0;
		v->timer = v->wait;
	}
}

/* ---- the player ------------------------------------------------------------------- */

bool game_init (game_t *g, const bsp_t *b)
{
	memset (g, 0, sizeof *g);
	g->bsp = b;
	g->ground = -1;
	if (!collide_init (&g->collide, b))
	{
		return false;
	}
	const char *cursor = NULL;
	bsp_entity_t e;
	bool start = false;
	while (bsp_entity_next (b, &cursor, &e))
	{
		const char *cls = bsp_entity_value (&e, "classname");
		if (!cls)
		{
			continue;
		}
		if (strcmp (cls, "info_player_start") == 0 && bsp_entity_vector (&e, "origin", g->origin))
		{
			const char *a = bsp_entity_value (&e, "angle");
			g->yaw = a ? (float) atof (a) * 3.14159265f / 180 : 0.0f;
			start = true;
		}
		else if (strcmp (cls, "func_door") == 0)
		{
			add_mover (g, &e, false);
		}
		else if (strcmp (cls, "func_plat") == 0)
		{
			add_mover (g, &e, true);
		}
	}
	if (!start)
	{
		printf ("game: the level has no info_player_start\n");
	}
	g->view_z = g->origin[2] + GAME_EYE;
	return true;
}

static void clip_velocity (const float *in, const float *normal, float *out, float overbounce)
{
	float back = vdot (in, normal) * overbounce;
	for (int k = 0; k < 3; k++)
	{
		out[k] = in[k] - normal[k] * back;
		if (out[k] > -0.1f && out[k] < 0.1f)
		{
			out[k] = 0.0f;
		}
	}
}

/* along velocity for dt, sliding along what's hit */
static void fly_move (game_t *g, float dt)
{
	float planes[5][3], primal[3], left = dt;
	int n = 0;
	memcpy (primal, g->velocity, sizeof primal);
	for (int bump = 0; bump < 4 && left > 0.0f; bump++)
	{
		float end[3] = {g->origin[0] + g->velocity[0] * left, g->origin[1] + g->velocity[1] * left,
				g->origin[2] + g->velocity[2] * left};
		collide_trace_t t = collide_trace (&g->collide, HULL, g->origin, end, -1);
		if (t.all_solid)
		{
			vset (g->velocity, 0, 0, 0);	/* (stuck) */
			return;
		}
		if (t.fraction > 0.0f)
		{
			memcpy (g->origin, t.end, sizeof g->origin);
			n = 0;
		}
		if (t.fraction == 1.0f)
		{
			return;
		}
		left -= left * t.fraction;
		if (n >= 5)
		{
			vset (g->velocity, 0, 0, 0);
			return;
		}
		memcpy (planes[n++], t.normal, sizeof planes[0]);

		/* a velocity along all the planes hit, else along their crease, else none */
		int i;
		float v[3];
		for (i = 0; i < n; i++)
		{
			clip_velocity (primal, planes[i], v, 1.0f);
			int j;
			for (j = 0; j < n; j++)
			{
				if (j != i && vdot (v, planes[j]) < 0.0f)
					break;
			}
			if (j == n)
				break;
		}
		if (i < n)
		{
			memcpy (g->velocity, v, sizeof v);
		}
		else if (n == 2)
		{
			float dir[3] = {planes[0][1] * planes[1][2] - planes[0][2] * planes[1][1],
					planes[0][2] * planes[1][0] - planes[0][0] * planes[1][2],
					planes[0][0] * planes[1][1] - planes[0][1] * planes[1][0]};
			float d = vdot (dir, g->velocity);
			vset (g->velocity, dir[0] * d, dir[1] * d, dir[2] * d);
		}
		else
		{
			vset (g->velocity, 0, 0, 0);
			return;
		}
		if (vdot (g->velocity, primal) <= 0.0f)
		{
			vset (g->velocity, 0, 0, 0);	/* (it would turn back: stop) */
			return;
		}
	}
}

/* on the ground: the slide, or the slide raised by a step (and put down), whichever gets further */
static void ground_move (game_t *g, float dt)
{
	float o0[3], v0[3];
	memcpy (o0, g->origin, sizeof o0);
	memcpy (v0, g->velocity, sizeof v0);
	fly_move (g, dt);
	float o1[3], v1[3];
	memcpy (o1, g->origin, sizeof o1);
	memcpy (v1, g->velocity, sizeof v1);

	memcpy (g->origin, o0, sizeof o0);
	memcpy (g->velocity, v0, sizeof v0);
	float up[3] = {o0[0], o0[1], o0[2] + STEP};
	collide_trace_t t = collide_trace (&g->collide, HULL, o0, up, -1);
	if (!t.all_solid)
	{
		memcpy (g->origin, t.end, sizeof g->origin);
	}
	fly_move (g, dt);
	float down[3] = {g->origin[0], g->origin[1], g->origin[2] - STEP};
	t = collide_trace (&g->collide, HULL, g->origin, down, -1);
	if (!t.all_solid)
	{
		memcpy (g->origin, t.end, sizeof g->origin);
	}
	float d1 = (o1[0] - o0[0]) * (o1[0] - o0[0]) + (o1[1] - o0[1]) * (o1[1] - o0[1]);
	float d2 = (g->origin[0] - o0[0]) * (g->origin[0] - o0[0]) + (g->origin[1] - o0[1]) * (g->origin[1] - o0[1]);
	if (d1 >= d2 || (t.fraction < 1.0f && t.normal[2] < WALKABLE))
	{
		memcpy (g->origin, o1, sizeof o1);	/* the plain slide was as good */
		memcpy (g->velocity, v1, sizeof v1);
	}
	else
	{
		g->velocity[2] = v1[2];
	}
}

static void categorize (game_t *g)
{
	float down[3] = {g->origin[0], g->origin[1], g->origin[2] - 1.0f};
	collide_trace_t t = collide_trace (&g->collide, HULL, g->origin, down, -1);
	g->on_ground = g->velocity[2] <= 180.0f && t.fraction < 1.0f && t.normal[2] >= WALKABLE;
	g->ground = g->on_ground ? t.entity : -1;
	if (g->on_ground && !t.start_solid)
	{
		memcpy (g->origin, t.end, sizeof g->origin);
	}
}

void game_update (game_t *g, const game_input_t *in, float dt)
{
	for (int i = 0; i < g->n_movers; i++)
	{
		update_mover (g, &g->movers[i], dt);
	}

	g->yaw += in->turn * dt;
	g->pitch += in->look * dt;
	g->pitch = g->pitch > 1.2f ? 1.2f : g->pitch < -1.2f ? -1.2f : g->pitch;
	categorize (g);

	/* friction on the ground */
	float speed = sqrtf (g->velocity[0] * g->velocity[0] + g->velocity[1] * g->velocity[1]);
	if (g->on_ground && speed > 0.0f)
	{
		float drop = (speed < STOP_SPEED ? STOP_SPEED : speed) * FRICTION * dt;
		float scale = speed - drop > 0.0f ? (speed - drop) / speed : 0.0f;
		g->velocity[0] *= scale;
		g->velocity[1] *= scale;
	}

	/* the speed wished, along the yaw */
	float cy = cosf (g->yaw), sy = sinf (g->yaw);
	float wish[3] = {(cy * in->forward + sy * in->side) * MAX_SPEED, (sy * in->forward - cy * in->side) * MAX_SPEED, 0};
	float wishspeed = sqrtf (wish[0] * wish[0] + wish[1] * wish[1]);
	if (wishspeed > MAX_SPEED)
	{
		wish[0] *= MAX_SPEED / wishspeed;
		wish[1] *= MAX_SPEED / wishspeed;
		wishspeed = MAX_SPEED;
	}
	if (wishspeed > 0.0f)
	{
		float dir[3] = {wish[0] / wishspeed, wish[1] / wishspeed, 0};
		float cap = g->on_ground ? wishspeed : (wishspeed < AIR_WISH ? wishspeed : AIR_WISH);
		float add = cap - vdot (g->velocity, dir);
		if (add > 0.0f)
		{
			float acc = (g->on_ground ? ACCEL : AIR_ACCEL) * wishspeed * dt;
			acc = acc > add ? add : acc;
			g->velocity[0] += acc * dir[0];
			g->velocity[1] += acc * dir[1];
		}
	}

	/* jumping, gravity */
	if (in->jump && g->on_ground && !g->jump_held)
	{
		g->velocity[2] = JUMP_SPEED;
		g->on_ground = false;
	}
	g->jump_held = in->jump;
	if (!g->on_ground)
	{
		g->velocity[2] -= GRAVITY * dt;
	}
	else
	{
		g->velocity[2] = 0.0f;
	}

	if (g->on_ground)
	{
		ground_move (g, dt);
	}
	else
	{
		fly_move (g, dt);
	}
	categorize (g);

	/* the eye: straight to the origin, but eased up a step */
	float target = g->origin[2] + GAME_EYE;
	if (g->on_ground && target > g->view_z && target - g->view_z <= STEP + 1.0f)
	{
		g->view_z += (target - g->view_z) * (1.0f - expf (-dt * 14.0f));
		if (target - g->view_z > STEP)
			g->view_z = target - STEP;
	}
	else
	{
		g->view_z = target;
	}
}

void game_eye (const game_t *g, float eye[3])
{
	vset (eye, g->origin[0], g->origin[1], g->view_z);
}

/* ---- the autopilot ---------------------------------------------------------------- */

void game_pilot_init (game_pilot_t *p, const float (*points)[3], int n)
{
	memset (p, 0, sizeof *p);
	p->points = points;
	p->n = n;
	p->best = 1e9f;
}

void game_pilot (game_pilot_t *p, const game_t *g, game_input_t *in, float dt)
{
	memset (in, 0, sizeof *in);
	if (p->n <= 0)
	{
		return;
	}
	const float *w = p->points[p->next];
	float dx = w[0] - g->origin[0], dy = w[1] - g->origin[1], dist = sqrtf (dx * dx + dy * dy);
	if (dist < 40.0f && fabsf (w[2] - g->origin[2]) < 64.0f)
	{
		p->next = (p->next + 1) % p->n;
		p->best = 1e9f;
		p->since = 0.0f;
		p->reached++;
		return;
	}

	/* turn towards it, walk when facing it */
	float diff = atan2f (dy, dx) - g->yaw;
	diff -= 6.2831853f * floorf ((diff + 3.14159265f) / 6.2831853f);
	float turn = diff * 5.0f;
	in->turn = turn > 4.0f ? 4.0f : turn < -4.0f ? -4.0f : turn;
	in->forward = fabsf (diff) < 0.5f ? 1.0f : 0.2f;
	in->look = -g->pitch * 2.0f;

	/* stuck: a jump; given up after 5 s */
	if (dist < p->best - 8.0f)
	{
		p->best = dist;
		p->since = 0.0f;
	}
	p->since += dt;
	in->jump = p->since > 1.5f && p->since < 1.7f;
	if (p->since > 5.0f)
	{
		p->next = (p->next + 1) % p->n;
		p->best = 1e9f;
		p->since = 0.0f;
	}
}
