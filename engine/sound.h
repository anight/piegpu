/*
 * sound.h - the BSP engine's sounds, in Quake's manner: the RPi keeps them
 * and mixes them (docs/protocol.md 7.14: pgpu_sound_*); here they are placed.
 * A sound has a place in the level (or none: in the player's head), a volume
 * and an attenuation, and is heard from the player's eye: quieter with the
 * distance (silent 1000 units away, at attenuation 1), louder on the side
 * it's on.
 *
 * One-shot sounds take one of SOUND_DYNAMIC channels: a free one, or the one
 * their owner already has (the player's feet, a mover), or the one nearest
 * its end. Loops have their own: the level's air (wind, a base's hum), its
 * liquid (lava or water: the nearest of it, found from the level's faces),
 * and the movers that are moving (lifts, doors, trains), their volumes
 * following the player.
 *
 * sound_game makes the game's sounds from its state, a frame after a frame:
 * steps (a stride of 128 units, soft: the sole's brush, no thud), the jump's grunt, landings (hard ones over
 * 600 units a second), a splash into water and out of it, the jump pads and
 * the teleporters, each mover's motor while it moves and its clunk when it
 * stops. The rest (pickups, a death) is the demo's: sound_play.
 *
 * The sounds: engine/sounds/sound_data.c (engine/tools/make_sounds.py).
 */
#ifndef ENGINE_SOUND_H
#define ENGINE_SOUND_H

#include "bsp.h"
#include "game.h"
#include "sounds/sound_data.h"

#define SOUND_DYNAMIC		10		/* channels 0 .. 9: one-shot sounds */
#define SOUND_LOOPS		6		/* channels 10 .. 15: the air, the liquid, four movers */
#define SOUND_LIQUIDS		24		/* places of liquid remembered */

#define SOUND_PLAYER		0		/* owners: the player's voice ... */
#define SOUND_FEET		1		/* ... and feet */
#define SOUND_MOVER		16		/* ... a mover: + its index */
#define SOUND_ANY		-1		/* nobody's: never taken over for its owner's next */

typedef struct
{
	float eye[3], right[2];			/* the listener */
	float left_s[SOUND_DYNAMIC];		/* seconds each channel's sound still plays */
	int owner[SOUND_DYNAMIC];
	int loop[SOUND_LOOPS];			/* the sound looping on each (SND_NONE: silent) */
	int loop_volume[SOUND_LOOPS][2];	/* as last sent */
	int loop_mover[SOUND_LOOPS];		/* the mover it's for (the movers' channels) */
	int air, liquid;			/* the level's loops */
	float liquids[SOUND_LIQUIDS][3];
	int n_liquids;
	/* the game, the frame before */
	float last[3];
	float stride;
	int step;
	int contents;
	unsigned char moving[GAME_MAX_MOVERS];
	bool started;
} sound_t;

/* the sounds sent to the RPi; the level's liquid found; air: the level's
   ambient loop (SND_AMB_WIND, SND_AMB_HUM, or SND_NONE) */
void sound_init (sound_t *s, const bsp_t *bsp, int air);

/* a sound at a place (NULL: in the player's head): volume 0 .. 1,
   attenuation 1 normal (0: heard everywhere) */
void sound_play (sound_t *s, int id, const float *origin, int owner, float volume, float attenuation);

/* the game's sounds for the step just made (after game_update) */
void sound_game (sound_t *s, const game_t *g, float dt);

#endif
