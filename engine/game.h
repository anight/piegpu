/*
 * game.h - a player in a BSP level: Quake's movement (friction and
 * acceleration on the ground, a little control in the air, gravity, jumping,
 * sliding along walls, climbing steps of 18), the doors (func_door: open when
 * the player comes near, close after their wait) and lifts (func_plat: up while
 * the player stands on them, down after they've left), both carrying the
 * player as they move. The level's entities say where they are and how they
 * move (angle, speed, wait, lip, height), and where the player starts
 * (info_player_start). Trains (func_train) go round their path_corners
 * (target, wait), carrying the player. Triggers: trigger_push throws the
 * player to the top of an arc through its target (as Quake 3's jump pads),
 * trigger_teleport puts them at its target (info_teleport_destination, its
 * angle). Items (any item_* entity: its origin) are picked up by walking
 * into them. What the player's feet are in (water, slime, lava) is the
 * caller's to act on.
 */
#ifndef ENGINE_GAME_H
#define ENGINE_GAME_H

#include "bsp.h"
#include "collide.h"

#define GAME_MAX_MOVERS		16
#define GAME_MAX_ITEMS		32
#define GAME_MAX_TRIGGERS	16
#define GAME_MAX_PATH		8		/* a train's corners */

enum { GAME_DOOR, GAME_PLAT, GAME_TRAIN };
enum { GAME_PUSH, GAME_TELEPORT };
#define GAME_PUSHED		1		/* game_t.events: in the last step */
#define GAME_TELEPORTED		2
#define GAME_JUMPED		4
#define GAME_LANDED		8		/* (from the air onto the ground: land_speed) */
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
	int kind;				/* GAME_DOOR, GAME_PLAT, GAME_TRAIN */
	float pos[2][3];			/* its offsets: closed (at the bottom), open (at the top) */
	float path[GAME_MAX_PATH][3];		/* a train's: its offsets at its corners ... */
	float waits[GAME_MAX_PATH];		/* ... and its waits there */
	int corners, leg;			/* the corner it's going to or at */
	float at[3];				/* where it is */
	float speed, wait;
	int state;				/* 0 closed, 1 opening, 2 open, 3 closing (a train: 1 going, 2 waiting) */
	float timer;				/* in the open state: until it closes */
} game_mover_t;

typedef struct
{
	int kind;				/* GAME_PUSH, GAME_TELEPORT */
	float mins[3], maxs[3];
	float target[3], yaw;			/* the arc's top; where to (facing yaw) */
	bool inside;				/* (a push: once till the player's out) */
} game_trigger_t;

typedef struct
{
	float origin[3];
	bool taken;
} game_item_t;

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
	game_item_t items[GAME_MAX_ITEMS];
	int n_items, n_taken;
	game_trigger_t triggers[GAME_MAX_TRIGGERS];
	int n_triggers;
	unsigned events;			/* GAME_PUSHED, GAME_TELEPORTED, GAME_JUMPED, GAME_LANDED: in the last step */
	float land_speed;			/* how fast it came down, with GAME_LANDED */
	int picked;				/* the item picked up in the last step, else -1 */
	int contents;				/* at the player's feet (BSP_CONTENTS_*) */
	float start[3], start_yaw;		/* info_player_start */
} game_t;

/* the level's movers and the player at its start */
bool game_init (game_t *g, const bsp_t *bsp);

/* a step of dt seconds */
void game_update (game_t *g, const game_input_t *in, float dt);

/* the eye (smoothed over steps) */
void game_eye (const game_t *g, float eye[3]);

/* back to the start, standing */
void game_respawn (game_t *g);

/* the items back, none taken */
void game_items_reset (game_t *g);

/* an autopilot: the input that walks the player through waypoints (their
   origins: 24 over the floor), turning towards each, jumping when stuck;
   where the floor ends before a waypoint no lower: a jump over the gap if
   running and there's floor at the waypoint near enough, else a wait at the
   edge (for a platform to come); waiting under or over one (a lift); on past one when
   thrown or teleported; giving one up after a while; on to the first after
   the last */
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
