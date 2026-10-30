/*
 * game.h - stacker's rules: the falling-blocks game as Nintendo's 1989 NES
 * version plays it, from TetrisWiki and meatfighter's study of the ROM
 * ("Applying Artificial Intelligence to Nintendo Tetris"): a 10 x 20 field;
 * the seven pieces in the right-handed Nintendo rotation system, no wall
 * kicks, spawned at (5, 0) in their spawn orientations; gravity by level
 * (48 frames a row at 0 ... 1 at 29); a piece locking when it can't go down
 * at its gravity step; DAS 16 frames, then every 6; soft drop at half a row
 * a frame, a point a row of the last soft drop into the lock; the entry
 * delay 10 - 18 frames by the height it locked at; the line clear in 5
 * steps, a frame in 4, from the middle out, the screen flashing on four;
 * 40 / 100 / 300 / 1200 x (level + 1); the level after (start x 10 + 10) or
 * max (100, start x 10 - 50) lines, then every 10; the next piece reroll
 * once if it repeats the last. The name, the pictures, the colours of the
 * levels and the sounds are this one's (the music: the folk song the NES's
 * first was, arranged here).
 *
 * The screen is the NES's, 256 x 224, 32 x 28 tiles of 8.
 */
#ifndef STACKER_GAME_H
#define STACKER_GAME_H

#include <stdbool.h>
#include <stdint.h>

#define ST_W		256
#define ST_H		224
#define ST_COLS		10
#define ST_ROWS		20

enum { P_T, P_J, P_Z, P_O, P_S, P_L, P_I, P_NONE = 7 };	/* (the NES's order) */
enum { ST_TITLE, ST_FALLING, ST_CLEARING, ST_ENTRY, ST_TOPPED, ST_OVER };

/* the screen's palette: 0 black, 1 white, 2 the frame, 3 its shade, 4 and 5 the level's colours,
   6 the field's background (white in a four's flash) */
enum { C_BLACK, C_WHITE, C_FRAME, C_SHADE, C_LEVEL1, C_LEVEL2, C_FIELD, C_RED, C_COUNT };

enum
{
	ST_EV_MOVE = 1, ST_EV_ROTATE = 2, ST_EV_LOCK = 4, ST_EV_LINES = 8, ST_EV_FOUR = 16, ST_EV_LEVEL = 32,
	ST_EV_OVER = 64, ST_EV_START = 128
};

typedef struct
{
	bool left, right, down, rotate_cw, rotate_ccw;	/* held */
} st_input_t;

typedef struct
{
	uint8_t field[ST_ROWS][ST_COLS];	/* 0 empty, else the piece + 1 */
	uint8_t screen[ST_H][ST_W];
	uint8_t dirty[ST_H];

	int state, frame;
	bool demo;
	int start_level, level, lines, score, top_score;
	int piece, orient, px, py;		/* the falling piece: its kind, its orientation's id, its pivot */
	int next;
	int gravity_t, das, down_t, push_down;	/* frames */
	bool was_left, was_right, was_down, was_cw, was_ccw;
	bool first;				/* (the opening wait) */
	int entry_t;
	int clear_rows[4], n_clear, clear_step;
	int stats[7];
	int spawns;				/* pieces so far (the bot sees a new one) */
	uint16_t lfsr;
	unsigned events;
} st_t;

/* the cells of an orientation (the ROM's table: x, y from the pivot) */
extern const int8_t st_orient[19][4][2];
extern const uint8_t st_spawn[7];		/* each piece's spawn orientation */
int st_piece_of (int orient);
int st_rotate (int orient, bool clockwise);
int st_gravity (int level);

void st_title (st_t *g, int top_score, int start_level);
void st_new_game (st_t *g, bool demo, int start_level, int top_score);
void st_frame (st_t *g, const st_input_t *in);
void st_clean (st_t *g);
bool st_fits (const st_t *g, int orient, int px, int py);

/* the level colours (this one's, after the NES's two a level, ten in turn) */
extern const uint32_t st_level_colors[10][2];

/* the attract mode's player (bot.c) */
void st_bot (const st_t *g, st_input_t *in);

#endif
