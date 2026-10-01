/* aquarium_assets.h - the aquarium's caustics and sounds: written by demos/tools/make_aquarium_assets.py */
#ifndef AQUARIUM_ASSETS_H
#define AQUARIUM_ASSETS_H

#include <stdint.h>

#define AQUARIUM_CAUSTICS	128		/* the texture's side: 8 bits a texel, it tiles */
extern const uint8_t aquarium_caustics[AQUARIUM_CAUSTICS * AQUARIUM_CAUSTICS];

/* (the values are the sounds' ids on the RPi) */
enum
{
	ASND_NONE,
	ASND_BUBBLES,	/* a loop */
	ASND_HUM,	/* a loop */
	ASND_PLOP,
	ASND_KNOCK,
	ASND_COUNT
};

typedef struct
{
	const uint8_t *samples;		/* 8 bits unsigned, mono */
	uint32_t frames;
	uint16_t rate;
} aquarium_sound_t;

extern const aquarium_sound_t aquarium_sounds[ASND_COUNT];

#endif
