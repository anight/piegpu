/* sounds.h - rockfall's sounds: written by demos/tools/make_rockfall_sounds.py */
#ifndef ROCKFALL_SOUNDS_H
#define ROCKFALL_SOUNDS_H

#include <stdint.h>

/* (the values are the sounds' ids on the RPi) */
enum
{
	RSND_NONE,
	RSND_BOULDER,
	RSND_DIAMOND,
	RSND_COLLECT,
	RSND_EXPLODE,
	RSND_OPEN,
	RSND_TICK,
	RSND_EXTRA,
	RSND_BONUS,
	RSND_STEP,
	RSND_UNCOVER,
	RSND_AMOEBA,
	RSND_MAGIC,
	RSND_COUNT
};

typedef struct
{
	const uint8_t *samples;		/* 8 bits unsigned, mono */
	uint32_t frames;
	uint16_t rate;
} rockfall_sound_t;

extern const rockfall_sound_t rockfall_sounds[RSND_COUNT];

#endif
