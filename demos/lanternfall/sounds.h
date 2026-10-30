/* sounds.h - Lanternfall's own sounds: written by demos/lanternfall/tools/make_sounds.py */
#ifndef LANTERNFALL_SOUNDS_H
#define LANTERNFALL_SOUNDS_H

#include <stdint.h>
#include "sounds/sound_data.h"

/* (the values are the sounds' ids on the RPi: after the engine's, which the game plays too) */
enum
{
	LSND_FIRST = SND_COUNT,
	LSND_CELLO = LSND_FIRST,
	LSND_BELL,
	LSND_RAISE,
	LSND_FLARE,
	LSND_REVEAL,
	LSND_BEACON,
	LSND_SHRINE,
	LSND_HURT,
	LSND_OUT,
	LSND_HOLLOW,
	LSND_HOLLOW_DIE,
	LSND_ROAR,
	LSND_THUD,
	LSND_SHRIEK,
	LSND_STONE,
	LSND_SMASH,
	LSND_ODY,
	LSND_THUNDER,
	LSND_BOLT,
	LSND_KING,
	LSND_COUNT
};

typedef struct
{
	const uint8_t *samples;		/* 8 bits unsigned, mono */
	uint32_t frames;
	uint16_t rate;
} lf_sound_t;

extern const lf_sound_t lf_sounds[LSND_COUNT - LSND_FIRST];

#endif
