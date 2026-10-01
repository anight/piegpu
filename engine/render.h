/*
 * render.h - a BSP level drawn with pgl (GL ES 2.0) on the RPi.
 *
 * At the start everything goes to the RPi once: the textures (the palette's
 * colours, RGB565, mipmapped by the GPU), the faces' lightmaps packed into
 * atlas pages (a texel of padding round each, copied from its edge, so the
 * filtering never mixes two faces), and every face as triangles in one vertex
 * buffer, grouped by model, texture and lightmap page (a batch each). A frame
 * is then the host's work only: the leaves the eye's leaf can see (the PVS),
 * those in the view (their boxes against the frustum), their faces facing the
 * eye; each batch's such faces as few draws as their runs in the buffer.
 *
 * The shader (engine/shaders/world.*): the texture times the lightmap, twice
 * (as Quake's overbright: 128 is the texture's own colour). Textures named
 * light* are drawn at their own colour (their faces' lightmaps ignored), sky*
 * and *water ones too for now.
 */
#ifndef ENGINE_RENDER_H
#define ENGINE_RENDER_H

#include "bsp.h"
#include "gles/pgl.h"

#define RENDER_PAGE		256		/* lightmap atlas pages: 256 x 256 */
#define RENDER_MAX_PAGES	8

typedef struct
{
	int model, texture, page;
	int first_face, faces;			/* in render_t.order */
} render_batch_t;

typedef struct
{
	int first, count;			/* vertices */
	unsigned frame;				/* marked visible in this frame */
} render_face_t;

typedef struct
{
	unsigned faces, draws, leaves;		/* the last frame's */
} render_stats_t;

typedef struct
{
	const bsp_t *bsp;
	GLuint program, buffer;
	GLint u_vp, u_offset, a_pos, a_uv, a_luv;
	GLuint *textures;			/* a GL texture a miptex (0: none) */
	GLuint pages[RENDER_MAX_PAGES];
	int n_pages;
	render_face_t *faces;			/* by the BSP's face index */
	int *order;				/* face indices, batch by batch */
	render_batch_t *batches;
	int n_batches;
	int *model_batches;			/* a model's first batch; [n_models] the end */
	uint8_t *pvs;
	int pvs_leaf;				/* the leaf pvs is for */
	unsigned frame;
	render_stats_t stats;
} render_t;

/* everything to the RPi (the GL context is up); false if something doesn't fit */
bool render_init (render_t *r, const bsp_t *bsp);

/* the world as seen from eye (vp: projection times view, column major) */
void render_world (render_t *r, const float eye[3], const float vp[16]);

/* a brush model (a door, a lift: its entity's "*n") moved by offset */
void render_model (render_t *r, int model, const float offset[3], const float vp[16]);

#endif
