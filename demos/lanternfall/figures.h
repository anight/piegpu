/*
 * figures.h - Lanternfall's people and things, drawn (figures.c): meshes of
 * flat-faced blocks made at the start, each drawn where a matrix puts it, lit
 * by shaders/figure. Their axes are the level's: x forward, y left, z up.
 */
#ifndef LANTERNFALL_FIGURES_H
#define LANTERNFALL_FIGURES_H

#include <stdint.h>
#include "gles/pgl.h"
#include "bsp.h"

enum
{
	M_WREN_BODY, M_WREN_HEAD, M_WREN_LEG, M_WREN_ARM, M_LANTERN,
	M_ODY_TORSO, M_ODY_HEAD, M_ODY_EYE, M_ODY_LEG, M_ODY_ARM, M_ODY_STUMP,
	M_BLOB, M_EYES, M_HORNS, M_LISTENER, M_KING, M_KING_EYES, M_ARCHIVIST,
	M_BRAZIER, M_COALS, M_EMBER, M_CHUNK,
	M_COUNT
};

/* the lights that move: the lantern, the nearest fire; the fog; lightning */
typedef struct
{
	float lantern[4], gold[3];		/* where, 1 / reach squared; colour times strength */
	float fire[4], fire_gold[3];
	float fog[4];				/* colour, 1 / the distance of all fog */
	float storm;
} lights_t;

typedef struct
{
	GLuint program, buffer;
	GLint u_vp, u_model, u_eye, u_ambient, u_lantern, u_gold, u_fire, u_fire_gold, u_rim, u_tint, u_fog, u_time, u_wobble;
	GLint a_pos, a_normal, a_color;
	int first[M_COUNT], count[M_COUNT];
} figures_t;

bool figures_init (figures_t *f);
/* a frame's figures to come */
void figures_begin (const figures_t *f, const float vp[16], const float eye[3], const lights_t *l, float time);
/* a mesh where model puts it; ambient: the light where it is (light_at); tint: its colour times this;
   rim: a sheen at its edges; wobble: its skin moving this far */
void figures_draw (const figures_t *f, int mesh, const float model[16], const float ambient[3], const float tint[3],
		   const float rim[3], float wobble);

/* the level's own light at a place (the lightmap of the floor under it), as the world's shader colours it */
void light_at (const bsp_t *bsp, const float p[3], float storm, float rgb[3]);

#endif
