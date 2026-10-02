/* sounds.h - zerog's own sound effects: written by demos/tools/make_zerog_sounds.py */
#ifndef ZEROG_SOUNDS_H
#define ZEROG_SOUNDS_H

#include "antigrav_sounds.h"

/* (the values are the sounds' ids on the RPi: after antigrav's, which zerog plays too) */
enum
{
	ZSND_FIRST = ASND_COUNT,
	ZSND_FIRE = ZSND_FIRST,
	ZSND_BLAST,
	ZSND_PICKUP,
	ZSND_MINE,
	ZSND_SHIELD,
	ZSND_FALL,
	ZSND_COUNT
};

extern const antigrav_sound_t zerog_sounds[ZSND_COUNT - ZSND_FIRST];

#endif
