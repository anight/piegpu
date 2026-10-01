/*
 * render.h - a BSP level drawn with pgl (GL ES 2.0) on the RPi.
 *
 * At the start everything goes to the RPi once: the textures (the palette's
 * colours, RGB565, mipmapped by the GPU), the faces' lightmaps packed into
 * atlas pages (a texel of padding round each, copied from its edge, so the
 * filtering never mixes two faces), and every face as triangles in one vertex
 * buffer, grouped by model, texture and lightmap page (a batch each). The
 * level is read where it is (in flash on a microcontroller): what's sent is
 * made a little at a time (a lightmap's block, a few rows of a texture, some
 * vertices), and what's kept is small (16-bit indices, a bit a face). A frame
 * is then the host's work only: the leaves the eye's leaf can see (the PVS),
 * those in the view (their boxes against the frustum), their faces facing the
 * eye; each batch's such faces as few draws as their runs in the buffer.
 *
 * The shaders (engine/shaders): world, the texture times the lightmap, twice
 * (as Quake's overbright: 128 is the texture's own colour); textures named
 * light* at their own colour (their faces' lightmaps ignored). Liquids (*NAME)
 * warp, as Quake's, unlit. The sky (sky*: Quake's 256 x 128, the front layer
 * on the left, colour 0 see-through, the back layer on the right) is two
 * layers of clouds drifting, chosen by the direction from the eye: a flat
 * deck in perspective (above, and mirrored below), hazy towards the horizon
 * (the back layer's colour, on average); drawn last
 * (what's in front of it hides it early: the V3D's early Z).
 */
#ifndef ENGINE_RENDER_H
#define ENGINE_RENDER_H

#include "bsp.h"
#include "gles/pgl.h"

#define RENDER_PAGE		256		/* lightmap atlas pages: 256 x 256 */
#define RENDER_MAX_PAGES	8

typedef struct
{
	GLuint id;
	GLint u_vp, u_offset, u_eye, u_time, u_size, u_haze, a_pos, a_uv, a_luv;
} render_program_t;

typedef struct
{
	uint16_t model, texture;
	uint8_t page, kind;
	uint16_t first_face, faces;		/* in render_t.order */
} render_batch_t;

typedef struct
{
	unsigned faces, draws, leaves;		/* the last frame's */
} render_stats_t;

typedef struct
{
	const bsp_t *bsp;
	render_program_t programs[3];		/* the world, liquids, the sky */
	GLuint buffer;
	GLuint *textures;			/* a GL texture a miptex (0: none; a sky's back layer) */
	GLuint *fronts;				/* a sky's front layer */
	float (*hazes)[3];			/* a sky's back layer's colour, on average */
	GLuint pages[RENDER_MAX_PAGES];
	int n_pages;
	uint16_t *first;			/* a face's first vertex, by the BSP's face index */
	uint8_t *shown;				/* the faces to draw this frame: a bit each */
	uint16_t *order;			/* face indices, batch by batch */
	render_batch_t *batches;
	int n_batches;
	uint16_t *model_batches;		/* a model's first batch; [n_models] the end */
	uint8_t *pvs;
	int pvs_leaf;				/* the leaf pvs is for */
	float time;				/* seconds: the liquids' and the sky's (the caller's to set) */
	float eye[3];				/* the last render_world's */
	render_stats_t stats;
} render_t;

/* everything to the RPi (the GL context is up); false if something doesn't fit */
bool render_init (render_t *r, const bsp_t *bsp);

/* the world as seen from eye (vp: projection times view, column major) */
void render_world (render_t *r, const float eye[3], const float vp[16]);

/* a brush model (a door, a lift: its entity's "*n") moved by offset */
void render_model (render_t *r, int model, const float offset[3], const float vp[16]);

#endif
