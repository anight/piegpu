/* sounds.h - stacker's sounds: written by demos/tools/make_stacker_sounds.py */
#ifndef STACKER_SOUNDS_H
#define STACKER_SOUNDS_H

#include <stdint.h>

/* (the values are the sounds' ids on the RPi) */
enum
{
	SSND_NONE,
	SSND_MOVE,
	SSND_ROTATE,
	SSND_LOCK,
	SSND_LINES,
	SSND_FOUR,
	SSND_LEVEL,
	SSND_OVER,
	SSND_MUSIC,
	SSND_COUNT
};

typedef struct
{
	const uint8_t *samples;		/* 8 bits unsigned, mono */
	uint32_t frames;
	uint16_t rate;
} stacker_sound_t;

extern const stacker_sound_t stacker_sounds[SSND_COUNT];

#endif
