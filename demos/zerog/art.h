/*
 * art.h - what zerog draws, made at the start (art.c): the textures (drawn
 * texel by texel), the circuit's road, walls and deck, the scenery by it, the
 * ground, the craft and what they fire. All of it in GL's buffers and
 * textures: the host keeps the names and where each part is.
 */
#ifndef ZEROG_ART_H
#define ZEROG_ART_H

#include <stdint.h>
#include "gles/pgl.h"
#include "race.h"

#define WALL_H		1.3f
#define GROUND_Y	0.0f

/* the scenery program's vertex (the road's, the scenery's, the shadows') */
typedef struct
{
	float x, y, z, u, v, shade;
} vertex_t;

/* the craft program's */
typedef struct
{
	float p[3], n[3], uv[2];
} craft_vertex_t;

/* the circuit in pieces of road, each drawn if it's seen: its strips of
   each kind (a texture each) are one after another in the buffer, so
   pieces in a row are one draw */
enum { K_ROAD, K_WALL, K_METAL, KINDS };
#define CHUNK_TILES	8
#define MAX_CHUNKS	32

typedef struct
{
	float c[3], radius;		/* a sphere round it */
	int first[KINDS], count[KINDS];	/* its vertices (the first two join it to the piece before) */
} chunk_t;

/* the scenery, a texture a part */
enum
{
	P_CONCRETE, P_METAL, P_ROOF, P_RIB, P_CROWD, P_SCREEN, P_HAZARD, P_TOWER,
	P_BANNER1, P_BANNER2, P_BANNER3, P_BANNER4, P_BANNER5, PARTS
};

typedef struct
{
	const char *name;
	float base[3], stripe[3], accent[3];
	uint32_t hud;			/* its colour on the HUD */
} team_t;

extern const team_t team[CRAFTS];

typedef struct
{
	GLuint track, props, ground, ground_index, meshes;
	chunk_t chunk[MAX_CHUNKS];
	int chunks;
	int part_first[PARTS], part_count[PARTS], prop_vertices;
	int ground_indices;
	int craft_first, craft_count, shot_first, shot_count;	/* in meshes */
	int mine_first, fire_first, flash_first, mine_count;	/* (the fire and the flash are as the mine: mine_count vertices each) */
	GLuint t_kind[KINDS], t_part[PARTS], t_ground, t_shadow, t_stars, t_flare, t_ordnance, t_livery[CRAFTS];
	int pylons, towers, hoops;
	uint8_t over[MAIN_RINGS];	/* the main route's rings with something over the road (heard as it's passed) */
} art_t;

extern art_t art;

/* sun: towards it (the scenery is lit per vertex, here) */
void art_build (const float sun[3]);

#endif
