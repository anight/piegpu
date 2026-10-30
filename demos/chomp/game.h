/*
 * game.h - chomp's rules: the 1980 maze game after Namco's, as Jamey
 * Pittman's "The Pac-Man Dossier" tells it (pacman.holenet.info): the maze,
 * the speeds, the scatter and chase times, the frightened times, the four
 * ghosts' targets, the ghost house's counters, the fruit, the points are the
 * original's; what the Dossier shows only in pictures (the scatter corners,
 * the zones where ghosts don't turn up) is as reimplementations of it have
 * it; the frightened ghosts' random turns go by a generator of this one's
 * (the original reads its ROM's bytes). The pictures and the sounds are made
 * here (art.c, tools/make_chomp_sounds.py), the names aren't Namco's.
 *
 * The screen is 224 x 288 (28 x 36 tiles of 8): the scores in the top three
 * rows, the maze (28 x 31) under them, the lives and the fruit in the bottom
 * two. Everything moves a pixel at a time, at its speed (a share of 75.76
 * pixels a second); 60 frames a second.
 */
#ifndef CHOMP_GAME_H
#define CHOMP_GAME_H

#include <stdbool.h>
#include <stdint.h>

#define CH_W			224
#define CH_H			288
#define CH_COLS			28
#define CH_ROWS			36
#define CH_MAZE_TOP		3		/* the maze's first tile row */
#define CH_DOTS			244		/* 240 dots, 4 energizers */

enum { CH_UP, CH_LEFT, CH_DOWN, CH_RIGHT, CH_NONE = -1 };	/* (the ghosts' order of preference) */
enum { CH_RED, CH_PINK, CH_CYAN, CH_ORANGE };
enum { T_EMPTY, T_WALL, T_DOT, T_ENERGIZER, T_DOOR };		/* the maze's tiles */
enum { G_HOUSE, G_LEAVING, G_ACTIVE, G_EYES, G_ENTERING };	/* a ghost's */
enum { CH_ATTRACT, CH_READY, CH_PLAYING, CH_EATEN, CH_DYING, CH_LEVEL_DONE, CH_GAME_OVER };

/* the screen's palette (the screen holds these; the shader colours them) */
enum
{
	P_BLACK, P_WALL, P_DOT, P_ENERGIZER, P_DOOR, P_WHITE, P_YELLOW, P_RED, P_PINK, P_CYAN, P_ORANGE, P_PEACH,
	P_COUNT
};

enum						/* events: what happened in the frame (for the sounds) */
{
	CH_EV_DOT = 1, CH_EV_ENERGIZER = 2, CH_EV_GHOST = 4, CH_EV_FRUIT = 8, CH_EV_DIE = 16, CH_EV_EXTRA = 32,
	CH_EV_READY = 64, CH_EV_LEVEL = 128, CH_EV_GAME_OVER = 256
};

typedef struct
{
	int x, y;				/* its middle, in pixels */
	int dir;				/* where it's going */
	int next;				/* a ghost: where it'll turn at this tile's middle, */
	int plan;				/* ... and at the next one's (decided as it came in: one tile ahead) */
	int tx, ty;				/* the tile it's in (a ghost's: when it came in) */
	float acc;				/* pixels owed */
	int state;				/* a ghost's G_* */
	bool fright;				/* frightened (blue) */
	int dots;				/* its own counter in the house */
	int tgx, tgy;				/* its target, last worked out (to show) */
} ch_actor_t;

typedef struct
{
	uint8_t tile[CH_ROWS][CH_COLS];
	uint8_t screen[CH_H][CH_W];		/* P_*: the maze and the text */
	uint8_t dirty[CH_H];			/* rows changed since ch_clean */

	int state, frame;			/* frame: since the state began */
	bool demo;
	int level, score, hi_score, lives;
	bool extra_given;

	ch_actor_t pac;
	int want;				/* the direction asked for */
	int pause;				/* frames it stops (a dot: 1, an energizer: 3) */
	int mouth;				/* its animation */
	ch_actor_t ghost[4];

	int phase, phase_t;			/* scatter/chase: the phase (even: scatter), frames in it */
	int fright_t;				/* frames of fright left */
	int eaten_in_fright;			/* ghosts eaten since the energizer */
	int dots_eaten;
	bool global_counter;			/* (after a life lost) */
	int global_dots;
	int idle_t;				/* frames since a dot was eaten */
	bool elroy_held;			/* (after a life lost, till the orange ghost is out) */

	int fruit_t, fruit_score_t, fruit_shown;	/* frames left: the fruit, its score shown */
	int popup_x, popup_y, popup_value;	/* a ghost's score shown (CH_EATEN) */
	int eaten_ghost;

	unsigned events;
	uint32_t rng;
} ch_t;

/* the speeds of this level, a share of the full speed (75.76 pixels a second) */
float ch_full_speed_px (void);

void ch_new_game (ch_t *g, bool demo, int hi_score);
void ch_attract (ch_t *g, int hi_score);
/* a frame: the direction asked for (CH_NONE: none) */
void ch_frame (ch_t *g, int want);
void ch_clean (ch_t *g);

/* can it go into the tile (the tunnel's ends wrap; ghost: the door too when going in or out) */
bool ch_open (const ch_t *g, int tx, int ty, bool door);
int ch_fright_frames (int level);
int ch_fright_flashes (int level);
int ch_fruit_points (int level);
int ch_fruit_kind (int level);		/* 0 .. 7: cherries .. key */

static inline int ch_dx (int d)		{ return d == CH_RIGHT ? 1 : d == CH_LEFT ? -1 : 0; }
static inline int ch_dy (int d)		{ return d == CH_DOWN ? 1 : d == CH_UP ? -1 : 0; }
static inline int ch_back (int d)	{ return d == CH_NONE ? CH_NONE : (d + 2) % 4; }

/* the attract mode's player (bot.c): the direction to ask for */
int ch_bot (const ch_t *g);

#endif
