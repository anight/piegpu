/*
 * sound.c - see sound.h
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "sound.h"
#include "pgpu.h"

#define CLIP_DISTANCE		1000.0f		/* a sound of attenuation 1 is silent from here */
#define STRIDE			128.0f		/* a step's length: 2.5 a second at a run */
#define LAND_SPEED		250.0f		/* a landing heard */
#define LAND_HARD		600.0f
#define LIQUID_VOLUME		0.7f
#define LIQUID_ATTENUATION	1.6f
#define MOVER_VOLUME		0.75f
#define LOOP_AIR		0
#define LOOP_LIQUID		1
#define LOOP_MOVERS		2		/* the first of the movers' */

/* what's heard of a sound at a place: left and right, 0 .. 256 */
static void spatialize (const sound_t *s, const float *origin, float volume, float attenuation, int *left, int *right)
{
	float l = volume, r = volume;
	if (origin)
	{
		float d[3] = {origin[0] - s->eye[0], origin[1] - s->eye[1], origin[2] - s->eye[2]};
		float dist = sqrtf (d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
		float side = dist > 1.0f ? (d[0] * s->right[0] + d[1] * s->right[1]) / dist : 0.0f;
		float near = 1.0f - dist * attenuation / CLIP_DISTANCE;
		near = near < 0.0f ? 0.0f : near;
		l = volume * near * (1.0f - side);
		r = volume * near * (1.0f + side);
	}
	*left = l >= 1.0f ? PGPU_SOUND_FULL : (int) (l * PGPU_SOUND_FULL);
	*right = r >= 1.0f ? PGPU_SOUND_FULL : (int) (r * PGPU_SOUND_FULL);
}

void sound_init (sound_t *s, const bsp_t *bsp, int air)
{
	memset (s, 0, sizeof *s);
	for (int id = 1; id < SND_COUNT; id++)
	{
		pgpu_sound_data (id, sound_data[id].rate, PGPU_SOUND_U8, sound_data[id].samples, sound_data[id].frames);
	}
	for (int k = 0; k < SOUND_DYNAMIC; k++)
	{
		s->owner[k] = SOUND_ANY;
	}
	for (int k = 0; k < SOUND_LOOPS; k++)
	{
		s->loop_mover[k] = -1;
	}
	s->air = air;
	s->contents = BSP_CONTENTS_EMPTY;

	/* the level's liquid: the middles of its faces with a liquid's texture
	   ("*..."), those too near one already kept left out */
	bool lava = false;
	const bsp_model_t *world = &bsp->models[0];
	for (int i = 0; i < world->faces; i++)
	{
		const bsp_face_t *f = &bsp->faces[world->first_face + i];
		const bsp_miptex_t *t = bsp_miptex (bsp, bsp->texinfo[f->texinfo].miptex);
		if (!t || t->name[0] != '*' || f->edges < 3)
		{
			continue;
		}
		lava = lava || strstr (t->name, "lava") != NULL;
		float mid[3] = {0, 0, 0};
		for (int k = 0; k < f->edges; k++)
		{
			const float *v = bsp_face_vertex (bsp, f, k);
			mid[0] += v[0] / f->edges;
			mid[1] += v[1] / f->edges;
			mid[2] += v[2] / f->edges;
		}
		bool near = false;
		for (int k = 0; k < s->n_liquids && !near; k++)
		{
			near = fabsf (mid[0] - s->liquids[k][0]) < 192.0f && fabsf (mid[1] - s->liquids[k][1]) < 192.0f
			       && fabsf (mid[2] - s->liquids[k][2]) < 192.0f;
		}
		if (!near && s->n_liquids < SOUND_LIQUIDS)
		{
			memcpy (s->liquids[s->n_liquids++], mid, sizeof mid);
		}
	}
	s->liquid = !s->n_liquids ? SND_NONE : lava ? SND_AMB_LAVA : SND_AMB_WATER;
}

void sound_play (sound_t *s, int id, const float *origin, int owner, float volume, float attenuation)
{
	int left, right;
	spatialize (s, origin, volume, attenuation, &left, &right);
	if (id <= SND_NONE || id >= SND_COUNT || (left == 0 && right == 0))
	{
		return;
	}
	/* its owner's channel; else a free one; else the one nearest its end */
	int channel = 0;
	for (int k = 0; k < SOUND_DYNAMIC; k++)
	{
		if (owner != SOUND_ANY && s->owner[k] == owner)
		{
			channel = k;
			break;
		}
		if (s->left_s[k] < s->left_s[channel])
		{
			channel = k;
		}
	}
	s->owner[channel] = owner;
	s->left_s[channel] = (float) sound_data[id].frames / sound_data[id].rate;
	pgpu_sound_play (channel, id, left, right, 0);
}

/* a loop's channel: what loops there, how loud (sent when it changed) */
static void loop (sound_t *s, int k, int id, const float *origin, float volume, float attenuation)
{
	int left = 0, right = 0;
	if (id != SND_NONE)
	{
		spatialize (s, origin, volume, attenuation, &left, &right);
	}
	if (id != s->loop[k])
	{
		if (id == SND_NONE)
		{
			pgpu_sound_stop (SOUND_DYNAMIC + k);
		}
		else
		{
			pgpu_sound_play (SOUND_DYNAMIC + k, id, left, right, PGPU_SOUND_LOOP);
		}
		s->loop[k] = id;
	}
	else if (id != SND_NONE && (abs (left - s->loop_volume[k][0]) > 2 || abs (right - s->loop_volume[k][1]) > 2
				    || ((left == 0) != (s->loop_volume[k][0] == 0)) || ((right == 0) != (s->loop_volume[k][1] == 0))))
	{
		pgpu_sound_volume (SOUND_DYNAMIC + k, left, right);
	}
	else
	{
		return;
	}
	s->loop_volume[k][0] = left;
	s->loop_volume[k][1] = right;
}

/* where a mover is: its model's middle, moved */
static void mover_place (const game_t *g, const game_mover_t *v, float at[3])
{
	const bsp_model_t *m = &g->bsp->models[v->model];
	for (int k = 0; k < 3; k++)
	{
		at[k] = (m->mins[k] + m->maxs[k]) * 0.5f + v->at[k];
	}
}

void sound_game (sound_t *s, const game_t *g, float dt)
{
	game_eye (g, s->eye);
	s->right[0] = sinf (g->yaw);
	s->right[1] = -cosf (g->yaw);
	for (int k = 0; k < SOUND_DYNAMIC; k++)
	{
		s->left_s[k] = s->left_s[k] > dt ? s->left_s[k] - dt : 0.0f;
	}
	if (!s->started)				/* (the first frame: nothing to compare with) */
	{
		memcpy (s->last, g->origin, sizeof s->last);
		s->contents = g->contents;
		for (int i = 0; i < g->n_movers; i++)
		{
			s->moving[i] = 0;
		}
		s->started = true;
	}

	/* the player: steps on the ground, the jump, the landing */
	float dx = g->origin[0] - s->last[0], dy = g->origin[1] - s->last[1];
	float moved = sqrtf (dx * dx + dy * dy);
	memcpy (s->last, g->origin, sizeof s->last);
	if (g->events & (GAME_TELEPORTED | GAME_PUSHED))
	{
		sound_play (s, g->events & GAME_TELEPORTED ? SND_TELEPORT : SND_PUSH, NULL, SOUND_ANY, 0.9f, 0.0f);
		s->stride = 0.0f;
	}
	else if (g->on_ground && moved < 64.0f)
	{
		s->stride += moved;
		if (s->stride >= STRIDE)
		{
			s->stride = 0.0f;
			sound_play (s, SND_STEP1 + s->step++ % 4, NULL, SOUND_FEET, 0.45f, 0.0f);
		}
	}
	if (g->events & GAME_JUMPED)
	{
		sound_play (s, SND_JUMP, NULL, SOUND_PLAYER, 0.8f, 0.0f);
	}
	if ((g->events & GAME_LANDED) && g->land_speed >= LAND_SPEED)
	{
		sound_play (s, g->land_speed >= LAND_HARD ? SND_LAND_HARD : SND_LAND, NULL, SOUND_FEET, 0.9f, 0.0f);
		s->stride = 0.0f;
	}

	/* into water (or slime), and out of it */
	bool wet = g->contents == BSP_CONTENTS_WATER || g->contents == BSP_CONTENTS_SLIME;
	bool was_wet = s->contents == BSP_CONTENTS_WATER || s->contents == BSP_CONTENTS_SLIME;
	if (wet != was_wet)
	{
		sound_play (s, SND_SPLASH, NULL, SOUND_ANY, wet ? 0.9f : 0.5f, 0.0f);
	}
	s->contents = g->contents;

	/* the movers: a motor while one moves (a channel, if there's one left), a clunk as it stops */
	for (int i = 0; i < g->n_movers; i++)
	{
		const game_mover_t *v = &g->movers[i];
		bool moving = v->kind == GAME_TRAIN ? v->state == 1 && v->corners >= 2 : v->state == 1 || v->state == 3;
		float at[3];
		mover_place (g, v, at);
		int channel = -1, spare = -1;
		for (int k = LOOP_MOVERS; k < SOUND_LOOPS; k++)
		{
			if (s->loop_mover[k] == i)
			{
				channel = k;
			}
			else if (s->loop_mover[k] < 0 && spare < 0)
			{
				spare = k;
			}
		}
		if (moving)
		{
			channel = channel >= 0 ? channel : spare;
			if (channel >= 0)
			{
				s->loop_mover[channel] = i;
				loop (s, channel, v->kind == GAME_DOOR ? SND_DOOR_MOVE : SND_PLAT_MOVE, at, MOVER_VOLUME, 1.0f);
			}
		}
		else
		{
			if (channel >= 0)
			{
				loop (s, channel, SND_NONE, NULL, 0.0f, 0.0f);
				s->loop_mover[channel] = -1;
			}
			if (s->moving[i])
			{
				sound_play (s, v->kind == GAME_DOOR ? SND_DOOR_STOP : SND_PLAT_STOP, at, SOUND_MOVER + i, 0.9f, 1.0f);
			}
		}
		s->moving[i] = moving;
	}

	/* the air; the liquid: the nearest of it */
	loop (s, LOOP_AIR, s->air, NULL, s->air == SND_AMB_HUM ? 0.10f : 0.22f, 0.0f);	/* (a hum soon tires: low) */
	if (s->liquid != SND_NONE)
	{
		int nearest = 0;
		float best = 1e30f;
		for (int k = 0; k < s->n_liquids; k++)
		{
			float d[3] = {s->liquids[k][0] - s->eye[0], s->liquids[k][1] - s->eye[1], s->liquids[k][2] - s->eye[2]};
			float d2 = d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
			if (d2 < best)
			{
				best = d2;
				nearest = k;
			}
		}
		loop (s, LOOP_LIQUID, s->liquid, s->liquids[nearest], LIQUID_VOLUME, LIQUID_ATTENUATION);
	}
}
