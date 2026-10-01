/*
 * keys.h - the player's keys, from the console (stdio: a PC's terminal, the
 * P4's serial port): w s forward and back, a d sideways, the arrows or q e to
 * turn (up and down: walk), space to jump. A terminal sends no key-up, only a
 * key's repeats: a key holds for KEYS_HOLD_S after each. And from the board's
 * stick and game controller, where it has them (pad.h): the stick forward and
 * back and to turn, the controller's Y and A (its left and right buttons) to
 * step sideways, B, X or the stick pressed to jump. On a PC
 * (ENGINE_RAW_TERMINAL) the terminal takes keys as they're pressed, not
 * echoed, and is put back at the end (on exit, and on the signals the
 * transport ends the session on).
 */
#ifndef ENGINE_KEYS_H
#define ENGINE_KEYS_H

#include "game.h"

#define KEYS_HOLD_S		0.3f
#define KEYS_TURN		2.2f		/* radians a second */

enum { KEYS_FORWARD, KEYS_BACK, KEYS_LEFT, KEYS_RIGHT, KEYS_TURN_LEFT, KEYS_TURN_RIGHT, KEYS_JUMP, KEYS_N };

typedef struct
{
	float held[KEYS_N];			/* seconds each is held still */
	float idle;				/* since a key was last pressed (or the stick moved) */
	bool pad;				/* the board has a stick or a game controller */
} keys_t;

/* after pgpu_init (its signal handlers are chained) */
void keys_init (keys_t *k);

/* the keys pressed since: the input they make (the pitch eased level) */
void keys_input (keys_t *k, const game_t *g, game_input_t *in, float dt);

#endif
