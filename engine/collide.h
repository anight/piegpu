/*
 * collide.h - moving boxes through a BSP level: its clipping hulls, which the
 * compiler made by growing every brush by a box's size, so that a box moving
 * is a point moving through the grown brushes. Hull 0 is a point (the render
 * tree), hull 1 a player (32 x 32 x 56: -16 -16 -24 to 16 16 32), hull 2 a
 * large monster (64 x 64 x 88). Brush models (doors, lifts) have their own
 * hulls, moved by where they are.
 */
#ifndef ENGINE_COLLIDE_H
#define ENGINE_COLLIDE_H

#include "bsp.h"

typedef struct
{
	float fraction;				/* of the way done: 1 nothing hit */
	float end[3];				/* where it stopped */
	float normal[3];			/* the plane it hit */
	bool start_solid;			/* it started inside something */
	bool all_solid;				/* ... and never left */
	int entity;				/* what it hit: -1 the world, else the solid's index */
} collide_trace_t;

typedef struct
{
	int model;				/* the brush model */
	float offset[3];			/* moved by */
	bool active;
} collide_solid_t;

#define COLLIDE_MAX_SOLIDS	32

typedef struct
{
	const bsp_t *bsp;
	bsp_clipnode_t *hull0;			/* the render tree as clip nodes (leaves: their contents) */
	collide_solid_t solids[COLLIDE_MAX_SOLIDS];	/* the brush models that block */
	int n_solids;
} collide_t;

bool collide_init (collide_t *c, const bsp_t *bsp);

/* a brush model that blocks, moved by offset (set it with collide_move_solid); its index */
int collide_add_solid (collide_t *c, int model);
void collide_move_solid (collide_t *c, int solid, const float offset[3]);

/* a box of hull (0 point, 1 player, 2 large) from start to end: through the
   world and the solids (ignore: one of them not, -1 none) */
collide_trace_t collide_trace (const collide_t *c, int hull, const float start[3], const float end[3], int ignore);

/* the contents at a point for a box of hull (BSP_CONTENTS_SOLID if any solid there) */
int collide_contents (const collide_t *c, int hull, const float p[3]);

/* only one solid (a lift: does it hold the player?) */
collide_trace_t collide_trace_solid (const collide_t *c, int solid, int hull, const float start[3], const float end[3]);

#endif
