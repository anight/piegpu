/*
 * race.h - zerog's race: the craft fly (a flight model: thrust, drag, grip,
 * the hover over the road, gravity where there's none), they are flown (by
 * the stick or by themselves: the same controls either way), they pick up
 * weapons and use them. Nothing here draws or sounds: what happened to a
 * craft is in its events, for zerog.c to show and play.
 */
#ifndef ZEROG_RACE_H
#define ZEROG_RACE_H

#include "track.h"

#define CRAFTS		8
#define LAPS		3
#define SHOTS		24
#define BLASTS		8
#define HOVER		0.8f		/* metres over the road */
#define GRID_SECONDS	5.0f
#define RESULTS_SECONDS	9.0f

enum { W_NONE, W_ROCKET, W_MISSILE, W_MINES, W_SHIELD, W_TURBO, WEAPONS };

enum					/* a craft's events */
{
	EV_BOOST = 1 << 0, EV_WALL = 1 << 1, EV_BUMP = 1 << 2, EV_LAP = 1 << 3, EV_FINISH = 1 << 4,
	EV_PICKUP = 1 << 5, EV_FIRE = 1 << 6, EV_HIT = 1 << 7, EV_FALL = 1 << 8, EV_LAND = 1 << 9,
	EV_SHIELD = 1 << 10, EV_TURBO = 1 << 11, EV_JUMP = 1 << 12, EV_BLOCKED = 1 << 13, EV_MINE = 1 << 14,
	EV_RESCUED = 1 << 15
};

/* the controls: the stick's, or the craft's own */
typedef struct
{
	float steer;			/* -1 left .. 1 right */
	float thrust;			/* 0 .. 1 */
	float brake_l, brake_r;		/* the air brakes, 0 .. 1: one turns it, both slow it */
	bool fire;
} input_t;

typedef struct
{
	/* flying */
	float p[3], v[3], fwd[3];	/* where, its velocity, where its nose points (in the road's plane) */
	float spin;			/* turning, rad/s */
	where_t at;			/* on the circuit, */
	frame_t f;			/* the frame there, */
	float x, h;			/* and its place in it: across, above the road */
	bool air;			/* nothing holds it up */
	float air_time;
	float roll, pitch;		/* how it sits (seen) */
	float throttle;			/* the engine now, 0 .. 1 (seen, heard) */
	float boost;			/* seconds of a pad's or a turbo's kick left (seen, heard) */
	float stun, twirl;		/* hit: seconds without controls; the spin it's sent into, rad left */
	float shield;			/* seconds left */
	float lost;			/* fallen: seconds till it's put back; 0 flying */
	int last_tile;

	/* its weapon */
	int weapon, mines;
	float mine_wait, fire_wait;
	bool fired;			/* the fire button, last step */

	/* its race */
	int lap;			/* laps done: -1 on the grid, behind the line */
	float lap_start, best_lap, finish;
	int place;			/* at the finish, 1 ..; 0 racing */
	bool wrong_way;
	unsigned events;		/* EV_*, since they were last taken */
	int walls, falls, hits, pickups, forks;		/* counted in a race */

	/* flown by */
	bool human;
	input_t in;			/* the stick's (human) */
	float skill;			/* its own flying: about 1 */
	float lane;			/* the side of the road it likes, m */
	bool fork;			/* takes the fork this lap */
	float wall_time;
} craft_t;

enum { SHOT_ROCKET, SHOT_MISSILE, SHOT_MINE };

typedef struct
{
	bool live;
	int type, owner, target;
	float p[3], v[3], age;
	where_t at;
} shot_t;

typedef struct
{
	float p[3], age;		/* an explosion: seconds since */
	bool fresh;			/* not heard yet */
} blast_t;

enum { GRID, RACE, RESULTS };

typedef struct
{
	craft_t craft[CRAFTS];
	shot_t shot[SHOTS];
	blast_t blast[BLASTS];
	int phase, finished, player;
	float phase_start, start;	/* the phase's, the race's (seconds, as race_step's now) */
	float now;
	int races;
} race_t;

extern race_t race;

/* the craft on the grid, the countdown started; the player's flown by the stick or by itself */
void race_reset (float now, bool human);
/* dt on (the countdown, the race, the results; then true: time for another) */
bool race_step (float dt, float now);

float craft_speed (const craft_t *c);
/* how far a craft has come: laps and the way round */
float craft_total (const craft_t *c);
/* the order now: by places at the finish, then by distance */
void race_standings (int order[CRAFTS]);
const char *weapon_name (int w);
/* is a missile after this craft? */
bool race_hunted (int craft);

#endif
