/*
 * game.h - the invaders' rules, after Taito's 1978 arcade game as its
 * commented disassembly tells it (computerarcheology.com, Space Invaders):
 * the timings, speeds, tables and the order things happen in are the
 * original's; the art (art.c) and the sounds are drawn and made afresh in its
 * sizes and manner.
 *
 * As the original, the game draws into a one-bit screen, 224 x 256 (the
 * monitor turned upright), and what a shot hits is what's on that screen
 * where it lands: the shields wear away a few pixels at a time, the invaders
 * wipe them out as they come down over them, the shots chip the ground.
 *
 * Each frame (60 a second): the cannon moves a pixel if asked; its shot 4
 * pixels; one of the invaders' three shots (the rolling shot aimed at the
 * cannon, the plunger and the squiggly from their column tables) 4 pixels, 5
 * with under 9 invaders; one invader is moved, 2 pixels, in its turn - so the
 * rack ripples across, and the fewer they are the faster it goes. At a side
 * the rack drops 8. Every 0x600 frames, with 8 invaders or more and the rack
 * come down, the saucer crosses; what it's worth depends on how many shots
 * have been fired. 10, 20, 30 points an invader by its row, a cannon more at
 * 1500, the game over when the invaders reach the cannon's row.
 */
#ifndef INVADERS_GAME_H
#define INVADERS_GAME_H

#include <stdbool.h>
#include <stdint.h>

#define IV_W			224
#define IV_H			256
#define IV_ROWS			5
#define IV_COLS			11
#define IV_SHIELDS		4

enum { IV_ATTRACT, IV_PLAYING, IV_PLAYER_HIT, IV_ROUND_OVER, IV_GAME_OVER };

enum						/* events: what happened in the frame (for the sounds) */
{
	IV_EV_SHOT = 1, IV_EV_INVADER_HIT = 2, IV_EV_PLAYER_HIT = 4, IV_EV_SAUCER_HIT = 8, IV_EV_EXTRA = 16,
	IV_EV_FLEET = 32,			/* a note of the march: iv_t.fleet_note */
	IV_EV_SAUCER_ON = 64, IV_EV_SAUCER_OFF = 128, IV_EV_GAME_OVER = 256, IV_EV_ROUND = 512
};

typedef struct
{
	bool on;
	int x, y;				/* its sprite's top left */
	int steps;				/* moves since it was fired */
	int frame;				/* its animation */
	int column;				/* the plunger's and squiggly's: where in their column table */
	int blow;				/* frames of its explosion left (0: none) */
} iv_shot_t;

typedef struct
{
	uint8_t screen[IV_H][IV_W];		/* 0 or 255: the one-bit screen, a byte a pixel */
	uint8_t dirty[IV_H];			/* rows changed since iv_clean */

	int state, frame;			/* frame: since the state began */
	bool demo;				/* the attract mode's game, played by the bot */
	int score, hi_score, lives, round;
	bool extra_given;

	/* the cannon */
	int px;					/* its left (top: IV_PLAYER_Y) */
	bool alive;
	int blast;				/* its explosion: frames left */
	bool fire_held;				/* (a shot needs the button let go first) */

	/* the cannon's shot */
	iv_shot_t shot;
	int shots_fired;			/* (the saucer's score goes by it) */

	/* the rack: the bottom-left invader's place; the one moving next */
	bool alive_inv[IV_ROWS][IV_COLS];	/* [0]: the bottom row */
	int n_alive;
	int ref_x, ref_y;			/* the bottom-left invader's sprite: its left, its top */
	int dx;					/* +2, -2 (+3: one invader left, going right) */
	bool drop;				/* the next move of the rack goes down too */
	bool anim;				/* the rack's two frames */
	int cursor;				/* the invader to move next: row * IV_COLS + column */
	int inv_x[IV_ROWS][IV_COLS], inv_y[IV_ROWS][IV_COLS];	/* where each is drawn now */
	int exploding, exp_x, exp_y, exp_timer;	/* an invader's explosion (one at a time) */

	/* the invaders' shots: rolling, plunger, squiggly */
	iv_shot_t ashot[3];
	int shot_turn;				/* which moves this frame */
	int reload;				/* frames-steps another shot waits: by the score */
	int fire_delay;				/* the invaders don't fire until this is 0 */

	/* the saucer */
	bool saucer;
	int saucer_x, saucer_dx, saucer_timer, saucer_score, saucer_show;
	int saucer_pointer;			/* into the score table */

	/* the march */
	int fleet_timer, fleet_note;

	unsigned events;
	int event_x;				/* where (for the sound's side) */
	uint32_t rng;
} iv_t;

/* where the cannon is (its top), the saucer (its top), the shields (their tops) */
#define IV_PLAYER_Y		216
#define IV_SAUCER_Y		40
#define IV_SHIELD_Y		192
#define IV_GROUND_Y		239

/* a new game (demo: the attract mode's, by the bot) */
void iv_new_game (iv_t *g, bool demo, int hi_score);
/* the attract screen (its text drawn as time goes, by iv_frame) */
void iv_attract (iv_t *g, int hi_score);
/* a frame: dir -1, 0, 1; fire: the button held */
void iv_frame (iv_t *g, int dir, bool fire);
/* the rows changed have been taken */
void iv_clean (iv_t *g);

/* ---- the art (art.c) -------------------------------------------------------------------- */

typedef struct
{
	uint8_t w, h;
	const char *rows;			/* h rows of w characters: '#' a pixel */
} iv_sprite_t;

extern const iv_sprite_t iv_invader[3][2];	/* by kind (0: the 10-point, 1: 20, 2: 30), two frames */
extern const iv_sprite_t iv_cannon, iv_cannon_blast[2], iv_saucer, iv_saucer_blast;
extern const iv_sprite_t iv_invader_blast, iv_shot_blast, iv_ashot_blast, iv_shield;
extern const iv_sprite_t iv_ashot[3][4];	/* rolling, plunger, squiggly: four frames each */
/* the font: 5 x 7 in 8 x 8 cells; NULL: a space */
const iv_sprite_t *iv_glyph (char c);

/* ---- the attract mode's player (bot.c) ---------------------------------------------------- */

/* what the bot does this frame */
void iv_bot (const iv_t *g, int *dir, bool *fire);

#endif
