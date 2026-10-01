/*
 * pickups.h - the BSP engine's demos' items (engine/game.h: item_*) drawn:
 * spinning and bobbing, faceted and lit by the sun (demos/shaders/gem.*),
 * a glow round each added (halo.*). Gems (keep) or coins (isles).
 */
#ifndef DEMOS_PICKUPS_H
#define DEMOS_PICKUPS_H

#include "gles/pgl.h"
#include "game.h"

enum { PICKUPS_GEM, PICKUPS_COIN };

typedef struct
{
	int shape, vertices;
	GLuint gem, halo, mesh, quad;
	GLint g_vp, g_model, g_sun, g_eye, g_color, g_pos, g_normal;
	GLint h_vp, h_center, h_right, h_up, h_size, h_color, h_corner;
} pickups_t;

void pickups_init (pickups_t *s, int shape);

/* an item's colour (gems: one of several; coins: gold) */
const float *pickups_color (const pickups_t *s, int item);

/* the items not taken; sun: towards it; t: seconds */
void pickups_draw (const pickups_t *s, const game_t *g, const float vp[16], const float eye[3], float yaw, float pitch,
		   float t, const float sun[3]);

#endif
