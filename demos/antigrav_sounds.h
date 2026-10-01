/* antigrav_sounds.h - antigrav's sound effects: written by demos/tools/make_antigrav_sounds.py */
#ifndef ANTIGRAV_SOUNDS_H
#define ANTIGRAV_SOUNDS_H

#include <stdint.h>

/* (the values are the sounds' ids on the RPi) */
enum
{
	ASND_NONE,
	ASND_ENGINE,	/* a loop */
	ASND_ENGINE_OTHER,	/* a loop */
	ASND_WIND,	/* a loop */
	ASND_CROWD,	/* a loop */
	ASND_BOOST,
	ASND_WHOOSH,
	ASND_BUMP,
	ASND_SCRAPE,
	ASND_BEEP,
	ASND_GO,
	ASND_LAP,
	ASND_FINISH,
	ASND_COUNT
};

typedef struct
{
	const uint8_t *samples;		/* 8 bits unsigned, mono */
	uint32_t frames;
	uint16_t rate;
} antigrav_sound_t;

extern const antigrav_sound_t antigrav_sounds[ASND_COUNT];

#endif
