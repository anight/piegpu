/*
 * bsp.h - a Quake level (BSP version 29, as ericw-tools' qbsp writes it) read
 * in place: the file's bytes (in memory, or linked into flash) stay where they
 * are, the structures point into them. The level's visibility (PVS), its
 * clipping hulls, its brush models and entities; the textures are 8-bit, in
 * the palette bsp_palette (palette.c, made with them).
 *
 * Quake's units and axes: z up, a player 32 wide and 56 tall.
 */
#ifndef ENGINE_BSP_H
#define ENGINE_BSP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define BSP_VERSION		29

/* contents (a leaf's, a clip node's child when negative) */
#define BSP_CONTENTS_EMPTY	-1
#define BSP_CONTENTS_SOLID	-2
#define BSP_CONTENTS_WATER	-3
#define BSP_CONTENTS_SLIME	-4
#define BSP_CONTENTS_LAVA	-5
#define BSP_CONTENTS_SKY	-6

typedef struct { float normal[3]; float dist; int32_t type; } bsp_plane_t;
typedef struct { int32_t plane; int16_t children[2]; int16_t mins[3], maxs[3]; uint16_t first_face, faces; } bsp_node_t;
typedef struct { int32_t plane; int16_t children[2]; } bsp_clipnode_t;
typedef struct { float vecs[2][4]; int32_t miptex; int32_t flags; } bsp_texinfo_t;
typedef struct { int16_t plane, side; int32_t first_edge; int16_t edges, texinfo; uint8_t styles[4]; int32_t light_ofs; } bsp_face_t;
typedef struct { int32_t contents, vis_ofs; int16_t mins[3], maxs[3]; uint16_t first_mark, marks; uint8_t ambient[4]; } bsp_leaf_t;
typedef struct { uint16_t v[2]; } bsp_edge_t;
typedef struct { float mins[3], maxs[3], origin[3]; int32_t headnode[4]; int32_t visleafs, first_face, faces; } bsp_model_t;
typedef struct { char name[16]; uint32_t width, height, offsets[4]; } bsp_miptex_t;

typedef struct
{
	const uint8_t *data;
	size_t size;
	const char *entities;			/* the entity text (NUL terminated in the file) */
	size_t entities_size;
	const bsp_plane_t *planes;		int n_planes;
	const float (*vertices)[3];		int n_vertices;
	const uint8_t *vis;			size_t vis_size;
	const bsp_node_t *nodes;		int n_nodes;
	const bsp_texinfo_t *texinfo;		int n_texinfo;
	const bsp_face_t *faces;		int n_faces;
	const uint8_t *lighting;		size_t lighting_size;
	const bsp_clipnode_t *clipnodes;	int n_clipnodes;
	const bsp_leaf_t *leaves;		int n_leaves;
	const uint16_t *marks;			int n_marks;
	const bsp_edge_t *edges;		int n_edges;
	const int32_t *surfedges;		int n_surfedges;
	const bsp_model_t *models;		int n_models;
	int n_miptex;
	const int32_t *miptex_ofs;		/* from the texture lump's start: -1 none */
	const uint8_t *miptex_lump;
} bsp_t;

extern const uint8_t bsp_palette[256][3];

/* the level in data (size bytes: a BSP29 file); false (said on stdout) if it isn't one */
bool bsp_load (bsp_t *bsp, const void *data, size_t size);

/* a texture: its header, or NULL (none in the file) */
const bsp_miptex_t *bsp_miptex (const bsp_t *bsp, int index);

/* the leaf a point is in (model 0's tree; leaf 0 is the outside, solid) */
int bsp_point_leaf (const bsp_t *bsp, const float p[3]);

/* the leaves visible from a leaf: (n_leaves + 7) / 8 bytes at out, bit i-1 for
   leaf i (leaves 1 .. model 0's visleafs); all of them without vis data */
void bsp_leaf_pvs (const bsp_t *bsp, int leaf, uint8_t *out);

/* a face's vertex k (0 .. edges - 1) */
const float *bsp_face_vertex (const bsp_t *bsp, const bsp_face_t *face, int k);

/* entities: key and value pairs between braces, as the text has them; an
   entity is where it is in the file (nothing copied), its values read out
   as they're asked for */
#define BSP_VALUE	64		/* a value's room, with its '\0' (longer ones cut short) */
typedef struct
{
	const char *start, *end;		/* its pairs, in the entity lump */
} bsp_entity_t;

/* the next entity after *cursor (NULL: the first); false at the end */
bool bsp_entity_next (const bsp_t *bsp, const char **cursor, bsp_entity_t *entity);
/* a key's value into out (BSP_VALUE bytes); false if the key isn't there */
bool bsp_entity_value (const bsp_entity_t *entity, const char *key, char out[BSP_VALUE]);
/* a key's value as a number, or fallback */
float bsp_entity_float (const bsp_entity_t *entity, const char *key, float fallback);
/* "x y z" */
bool bsp_entity_vector (const bsp_entity_t *entity, const char *key, float v[3]);

#endif
