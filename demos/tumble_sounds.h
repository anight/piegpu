/* tumble_sounds.h - tumble's sound effects: written by demos/tools/make_tumble_sounds.py */
#ifndef TUMBLE_SOUNDS_H
#define TUMBLE_SOUNDS_H

#include <stdint.h>

/* (the values are the sounds' ids on the RPi) */
enum
{
	TSND_NONE,
	TSND_CLACK,
	TSND_THOCK,
	TSND_THUD,
	TSND_POP,
	TSND_BOOM,
	TSND_COUNT
};

typedef struct
{
	const uint8_t *samples;		/* 8 bits unsigned, mono */
	uint32_t frames;
	uint16_t rate;
} tumble_sound_t;

extern const tumble_sound_t tumble_sounds[TSND_COUNT];

#endif
