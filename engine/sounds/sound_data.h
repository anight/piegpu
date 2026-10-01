/* sound_data.h - the BSP engine's sound effects: written by engine/tools/make_sounds.py */
#ifndef ENGINE_SOUND_DATA_H
#define ENGINE_SOUND_DATA_H

#include <stdbool.h>
#include <stdint.h>

enum
{
	SND_NONE,
	SND_STEP1,
	SND_STEP2,
	SND_STEP3,
	SND_STEP4,
	SND_JUMP,
	SND_LAND,
	SND_LAND_HARD,
	SND_ITEM,
	SND_COMPLETE,
	SND_PLAT_MOVE,
	SND_PLAT_STOP,
	SND_DOOR_MOVE,
	SND_DOOR_STOP,
	SND_TELEPORT,
	SND_PUSH,
	SND_BURN,
	SND_SPLASH,
	SND_AMB_LAVA,
	SND_AMB_WIND,
	SND_AMB_WATER,
	SND_AMB_HUM,
	SND_COUNT
};

typedef struct
{
	const uint8_t *samples;		/* 8 bits unsigned, mono */
	uint32_t frames;
	uint16_t rate;
	bool loop;			/* made to go round */
} sound_data_t;

extern const sound_data_t sound_data[SND_COUNT];

#endif
