/* sounds.h - chomp's sounds: written by demos/tools/make_chomp_sounds.py */
#ifndef CHOMP_SOUNDS_H
#define CHOMP_SOUNDS_H

#include <stdint.h>

/* (the values are the sounds' ids on the RPi) */
enum
{
	CSND_NONE,
	CSND_WAKA1,
	CSND_WAKA2,
	CSND_SIREN1,
	CSND_SIREN2,
	CSND_SIREN3,
	CSND_SIREN4,
	CSND_SIREN5,
	CSND_FRIGHT,
	CSND_EYES,
	CSND_DEATH,
	CSND_GHOST,
	CSND_FRUIT,
	CSND_EXTRA,
	CSND_START,
	CSND_COUNT
};

typedef struct
{
	const uint8_t *samples;		/* 8 bits unsigned, mono */
	uint32_t frames;
	uint16_t rate;
} chomp_sound_t;

extern const chomp_sound_t chomp_sounds[CSND_COUNT];

#endif
