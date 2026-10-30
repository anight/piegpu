/* sounds.h - Xonix's sound effects: written by demos/tools/make_xonix_sounds.py */
#ifndef XONIX_SOUNDS_H
#define XONIX_SOUNDS_H

#include <stdint.h>

/* (the values are the sounds' ids on the RPi) */
enum
{
	XSND_NONE,
	XSND_STEP,
	XSND_OUT,
	XSND_CAPTURE,
	XSND_BOUNCE,
	XSND_DIE,
	XSND_CLEARED,
	XSND_OVER,
	XSND_START,
	XSND_COUNT
};

typedef struct
{
	const uint8_t *samples;		/* 8 bits unsigned, mono */
	uint32_t frames;
	uint16_t rate;
} xonix_sound_t;

extern const xonix_sound_t xonix_sounds[XSND_COUNT];

#endif
