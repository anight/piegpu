/*
 * game.h - rockfall's rules: the 1984 cave game, after First Star's, as its
 * engine is documented by Peter Broadribb and the BoulderDash Common File
 * Format's object specifications (elmerproductions.com/sp/peterb): the
 * cave scanned cell by cell, top left to bottom right, each object acting
 * in its turn; boulders and diamonds falling and rolling off rounded things
 * (left first); a push a 1-in-8 chance; fireflies turning left where they
 * can, butterflies right, both exploding on touching the miner or the
 * amoeba (3 x 3, steel spared; butterflies into diamonds); the amoeba
 * growing 4 in 128, 4 in 16 after its time, into boulders at 200 cells and
 * into diamonds enclosed; the magic wall turning what falls through it
 * into the other, two cells down, for its milling time; the uncovering;
 * the caves' generator (its pseudo-random numbers and fill). The caves
 * themselves (caves.c), the pictures (art.c) and the sounds are this one's.
 *
 * A cave is 40 x 22 cells, steel round it. The miner moves a cell a scan;
 * the cave is scanned every few frames (the difficulty level's), 60 frames
 * a second; the cave's time counts in seconds of 60 frames.
 */
#ifndef ROCKFALL_GAME_H
#define ROCKFALL_GAME_H

#include <stdbool.h>
#include <stdint.h>

#define RF_W			40
#define RF_H			22
#define RF_CAVES		5

/* a cell's object (its low 5 bits) and flags */
enum
{
	O_SPACE, O_DIRT, O_BRICK, O_MAGIC, O_STEEL, O_BOULDER, O_DIAMOND, O_FIREFLY, O_BUTTERFLY, O_AMOEBA,
	O_EXPLODE, O_EXPLODE_DIAMOND, O_MINER, O_INBOX, O_PREOUT, O_OUTBOX,
	O_COUNT
};
#define F_FALLING		0x20		/* a boulder's or a diamond's */
#define F_SCANNED		0x40		/* moved down or right already this scan */
#define F_DIR_SHIFT		8		/* a creature's facing (0 left, 1 up, 2 right, 3 down); an explosion's stage */

enum { RF_ATTRACT, RF_UNCOVER, RF_PLAYING, RF_EXITED, RF_DEAD, RF_GAME_OVER };

enum						/* events in the frame (for the sounds) */
{
	RF_EV_BOULDER = 1, RF_EV_DIAMOND_FALL = 2, RF_EV_COLLECT = 4, RF_EV_EXPLODE = 8, RF_EV_PUSH = 16,
	RF_EV_OPEN = 32, RF_EV_TICK = 64, RF_EV_EXTRA = 128, RF_EV_BONUS = 256, RF_EV_MOVE = 512,
	RF_EV_DIE = 1024, RF_EV_GAME_OVER = 2048, RF_EV_UNCOVER = 4096
};

/* a drawing over the fill: a point, a line (dir: 0 up, 1 up-right, 2 right ... 7 up-left, as the original's),
   a rectangle's outline, a filled rectangle (its inside `fill`) */
typedef struct
{
	char op;				/* 'P', 'L', 'R', 'F'; 0: the end */
	uint8_t x, y, a, b;			/* L: a the length, b the direction; R, F: a, b the size */
	uint16_t obj, fill;			/* (a creature's facing in obj's bits 8-) */
} rf_draw_t;

typedef struct
{
	const char *name;
	bool intermission;
	uint8_t needed, value, extra;		/* diamonds; their points, before and after enough */
	uint16_t time;				/* seconds */
	uint8_t magic_time, amoeba_time;	/* seconds */
	uint8_t seed;				/* the random fill's (seed2) */
	uint8_t fill[4], prob[4];		/* objects and their chances (of 256): the fill */
	uint32_t colors[3];			/* the cave's three colours */
	const rf_draw_t *draw;			/* what's drawn over the fill */
} rf_cave_t;

extern const rf_cave_t rf_caves[RF_CAVES];

typedef struct
{
	uint16_t cell[RF_H][RF_W];
	bool covered[RF_H][RF_W];		/* the uncovering */

	int state, frame;			/* frame: since the state began */
	bool demo;
	int cave, difficulty, lives, score, hi_score;
	int next_life;				/* the score for another life */
	int diamonds, needed, value, value_extra;
	int time, tick;				/* the cave's seconds left; frames into the second */
	int scan_wait;				/* frames to the next scan */
	bool open;				/* the exit's out */

	int mx, my;				/* the miner (or its inbox) */
	bool facing_right;
	int anim;				/* its animation: frames walking, idle tapping, blinking */
	bool moving, tapping, blinking;
	int inbox_scans;

	int amoeba_count, last_amoeba;		/* this scan's, the last's */
	bool amoeba_grows, last_grows;
	int amoeba_t;				/* scans since the cave began (its slow time) */
	bool magic_on, magic_done;
	int magic_t;

	int flash;				/* frames of the screen's flash (the exit's out) */
	unsigned events;
	uint32_t rng;				/* (the game's own: pushes, the amoeba, the uncovering) */
} rf_t;

void rf_new_game (rf_t *g, bool demo, int hi_score);
void rf_attract (rf_t *g, int hi_score);
/* the cave g->cave from its start (uncovered again) */
void rf_start_cave (rf_t *g);
/* a frame: dx, dy (-1 .. 1; horizontal wins), fire held */
void rf_frame (rf_t *g, int dx, int dy, bool fire);

static inline int rf_object (uint16_t c)	{ return c & 0x1F; }
static inline int rf_dir (uint16_t c)		{ return (c >> F_DIR_SHIFT) & 0xFF; }

/* the cave's scans: frames between them, by the difficulty */
int rf_scan_frames (int difficulty);

/* caves.c: the cave drawn into the cells (the generator's fill, then the drawing) */
void rf_build_cave (rf_t *g, int cave);

/* the attract mode's player (bot.c): what it asks for */
void rf_bot (const rf_t *g, int *dx, int *dy, bool *fire);

#endif
