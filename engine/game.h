/*
 * game.h - a player in a BSP level: Quake's movement (friction and
 * acceleration on the ground, a little control in the air, gravity, jumping,
 * sliding along walls, climbing steps of 18), the doors (func_door: open when
 * the player comes near, close after their wait) and lifts (func_plat: up while
 * the player stands on them, down after they've left), both carrying the
 * player as they move. The level's entities say where they are and how they
 * move (angle, speed, wait, lip, height), and where the player starts
 * (info_player_start).
 */
#ifndef ENGINE_GAME_H
#define ENGINE_GAME_H

#include "bsp.h"
#include "collide.h"

#define GAME_MAX_MOVERS		16
#define GAME_EYE		22.0f		/* the eye above the player's origin */

typedef struct
{
	float forward, side;			/* -1 .. 1: back to forward, left to right */
	float turn;				/* radians a second, left positive */
	float look;				/* radians a second, up positive */
	bool jump;
} game_input_t;

typedef struct
{
	int model, solid;
	bool plat;				/* a lift (func_plat), else a door */
	float pos[2][3];			/* its offsets: closed (at the bottom), open (at the top) */
	float at[3];				/* where it is */
	float speed, wait;
	int state;				/* 0 closed, 1 opening, 2 open, 3 closing */
	float timer;				/* in the open state: until it closes */
} game_mover_t;

typedef struct
{
	const bsp_t *bsp;
	collide_t collide;
	float origin[3], velocity[3];
	float yaw, pitch;			/* radians: yaw 0 along +x, turning left; pitch up */
	bool on_ground;
	int ground;				/* the solid it stands on: -1 the world */
	bool jump_held;
	float view_z;				/* the eye's height, smoothed on steps */
	game_mover_t movers[GAME_MAX_MOVERS];
	int n_movers;
} game_t;

/* the level's movers and the player at its start */
bool game_init (game_t *g, const bsp_t *bsp);

/* a step of dt seconds */
void game_update (game_t *g, const game_input_t *in, float dt);

/* the eye (smoothed over steps) */
void game_eye (const game_t *g, float eye[3]);

/* an autopilot: the input that walks the player through waypoints (their
   origins: 24 over the floor), turning towards each, jumping when stuck,
   giving one up after a while; on to the first after the last */
typedef struct
{
	const float (*points)[3];
	int n, next;
	float best, since;			/* the nearest it got to the next, and the time since */
	unsigned reached;			/* waypoints reached so far */
} game_pilot_t;

void game_pilot_init (game_pilot_t *p, const float (*points)[3], int n);
void game_pilot (game_pilot_t *p, const game_t *g, game_input_t *in, float dt);

#endif
