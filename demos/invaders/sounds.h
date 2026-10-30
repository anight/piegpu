/* sounds.h - the invaders' sounds: written by demos/tools/make_invaders_sounds.py */
#ifndef INVADERS_SOUNDS_H
#define INVADERS_SOUNDS_H

#include <stdint.h>

/* (the values are the sounds' ids on the RPi) */
enum
{
	ISND_NONE,
	ISND_FLEET1,
	ISND_FLEET2,
	ISND_FLEET3,
	ISND_FLEET4,
	ISND_SHOT,
	ISND_INVADER_HIT,
	ISND_PLAYER_HIT,
	ISND_SAUCER,
	ISND_SAUCER_HIT,
	ISND_EXTRA,
	ISND_COUNT
};

typedef struct
{
	const uint8_t *samples;		/* 8 bits unsigned, mono */
	uint32_t frames;
	uint16_t rate;
} invaders_sound_t;

extern const invaders_sound_t invaders_sounds[ISND_COUNT];

#endif
