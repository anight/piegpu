/*
 * game.h - Xonix's rules, without its picture or its sound (xonix.c draws and
 * plays them; bot.c plays by itself; sim.c, on a PC, plays without a GPU).
 *
 * The field is land and sea, a grid of cells; land rims it, two cells deep.
 * The player crosses the land freely; out at sea it leaves a trail, and it
 * can't stop there. Back on land the trail becomes land, and so does all the
 * sea it closed off from the sea balls. Sea balls fly diagonally over the sea
 * and bounce off the land: one that meets the trail or the player at sea
 * costs a life, as does the player running into its own trail. Land balls do
 * the same over the land and catch the player there. Land over 75% of what
 * was sea clears the level; the next has another sea ball, and every other
 * level another land ball.
 */
#ifndef XONIX_GAME_H
#define XONIX_GAME_H

#include <stdbool.h>
#include <stdint.h>

#define XN_COLS			80
#define XN_ROWS			56
#define XN_RIM			2		/* the land round the field, cells deep */
#define XN_GOAL			75		/* percent of the sea made land: the level's cleared */
#define XN_MAX_SEA		12
#define XN_MAX_LAND		5
#define XN_LIVES		3

/* a cell: sea, trail, or land (2 .. 255: which capture made it, for its flash) */
enum { XN_SEA = 0, XN_TRAIL = 1, XN_LAND = 2 };
#define XN_IS_LAND(v)		((v) >= XN_LAND)

/* the directions; y goes down the field (row 0 is the top) */
enum { XN_NONE = -1, XN_RIGHT, XN_UP, XN_LEFT, XN_DOWN };

enum { XN_PLAYING, XN_DYING, XN_CLEARED, XN_OVER };

enum						/* events: what happened in the update (for the sounds) */
{
	XN_EV_STEP = 1, XN_EV_OUT = 2, XN_EV_CAPTURE = 4, XN_EV_DIE = 8, XN_EV_CLEARED = 16,
	XN_EV_OVER = 32, XN_EV_BOUNCE = 64, XN_EV_START = 128, XN_EV_LEVEL = 256
};

typedef struct
{
	int x, y, dx, dy;
	int px, py;				/* the cell before (to draw it on its way) */
} xn_ball_t;

typedef struct
{
	uint8_t cell[XN_ROWS][XN_COLS];
	int level, lives, score, state;
	float state_t;				/* seconds in the state */

	int x, y, px, py;			/* the player, the cell before */
	int dir;				/* where it's going (XN_NONE: standing, on land) */
	bool at_sea;
	int out_x, out_y;			/* the land cell it left from */
	int trail;				/* cells of trail */
	float safe;				/* seconds the land balls can't catch it (just back) */

	xn_ball_t sea[XN_MAX_SEA], land[XN_MAX_LAND];
	int n_sea, n_land;
	float t_player, t_sea, t_land;		/* time to their next step */

	int sea_left;				/* sea cells inside the rim (land: the rest) */
	int inside;				/* all the cells inside the rim */
	uint8_t fresh;				/* the last capture's cell value, */
	float fresh_t;				/* ... and seconds since */
	int captured;				/* its cells */

	unsigned events;			/* XN_EV_*: in the last update */
	int bounce_x;				/* where a sea ball bounced last (for its sound) */
	int died_x, died_y;
	int dirty0, dirty1;			/* rows changed since xn_clean (dirty1 < dirty0: none) */
	uint32_t rng;
} xn_t;

/* the player's speed and the balls', cells a second */
float xn_player_speed (const xn_t *g);
float xn_sea_speed (const xn_t *g);
float xn_land_speed (const xn_t *g);

/* a game from level 1 */
void xn_new_game (xn_t *g, uint32_t seed);
/* dt seconds; want: the direction held (XN_NONE: none) */
void xn_update (xn_t *g, int want, float dt);
/* percent of the sea made land */
int xn_filled (const xn_t *g);
/* where a ball goes next (land: a land ball), bouncing off what blocks it; false: boxed in,
   it stays (bounced: it turned) */
bool xn_ball_next (const xn_t *g, xn_ball_t *b, bool land, bool *bounced);
/* the rows changed have been taken */
static inline void xn_clean (xn_t *g)	{ g->dirty0 = XN_ROWS; g->dirty1 = -1; }

static inline int xn_dx (int dir)	{ return dir == XN_RIGHT ? 1 : dir == XN_LEFT ? -1 : 0; }
static inline int xn_dy (int dir)	{ return dir == XN_DOWN ? 1 : dir == XN_UP ? -1 : 0; }
static inline bool xn_inside (int x, int y)	{ return x >= 0 && y >= 0 && x < XN_COLS && y < XN_ROWS; }

/* ---- the game played by itself (bot.c) ------------------------------------------------ */

#define XN_BOT_PATH		96

typedef struct
{
	int8_t path[XN_BOT_PATH][2];		/* the cells it means to go through */
	int n, at;				/* their number; the one it's at */
	float wait;				/* seconds on land without a way out */
	float cool;				/* till it looks for one again */
	int walk;				/* the way it walks along the shore meanwhile */
	uint32_t rng;
} xn_bot_t;

void xn_bot_init (xn_bot_t *b, uint32_t seed);
/* the direction to hold now */
int xn_bot (xn_bot_t *b, const xn_t *g, float dt);

#endif
