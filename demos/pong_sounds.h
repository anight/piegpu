/* pong_sounds.h - toy-pong's sounds: written by demos/tools/make_pong_sounds.py */
#ifndef PONG_SOUNDS_H
#define PONG_SOUNDS_H

#include <stdint.h>

/* (the values are the sounds' ids on the RPi) */
enum
{
	PSND_NONE,
	PSND_HIT,
	PSND_WALL,
	PSND_POINT,
	PSND_JOIN,
	PSND_START,
	PSND_WIN,
	PSND_COUNT
};

typedef struct
{
	const uint8_t *samples;		/* 8 bits unsigned, mono */
	uint32_t frames;
	uint16_t rate;
} pong_sound_t;

extern const pong_sound_t pong_sounds[PSND_COUNT];

#endif
