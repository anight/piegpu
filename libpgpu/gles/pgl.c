/*
 * pgl.c - OpenGL ES 2.0 / 1.1 API on top of pgpu (see pgl.h)
 *
 * GL objects map to Zero objects with the same id where possible (buffers,
 * textures, framebuffers); programs get Zero ids from a pool. State that
 * depends on several GL objects (framebuffer attachments, texture units,
 * vertex arrays, enables that need a depth or stencil buffer, fixed-function
 * matrices) is sent when a draw, clear or read needs it; other state is sent
 * when it is set.
 */
#include "pgl.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pgpu.h"
#include "pgpu_link.h"
#include "pgpu_program.h"
#include "pgpu_program_info.h"
#include "pgl_compiler.h"

/* ---- limits -------------------------------------------------------------------- */

#define MAX_BUFFER_NAMES	250		/* Zero ids 1 .. 250 */
#ifndef PGL_SHADOW_LIMIT
#define PGL_SHADOW_LIMIT	16384		/* copies of buffers kept up to this size (0: all) */
#endif
#define HW_STREAM_VERTEX	251		/* fixed-function client arrays */
#define HW_STREAM_INDEX		252		/* fixed-function client indices */
#define HW_TEMP_VERTEX		253		/* program draws too large for a packet */
#define HW_TEMP_INDEX		254
#define MAX_TEXTURE_NAMES	109		/* Zero ids 1 .. 109 */
#define HW_SURFACE_TEXTURE	110		/* pglInitSurface: the default framebuffer's colour */
#define TEX_DEFAULT_2D		111		/* texture object 0 of each target */
#define TEX_DEFAULT_CUBE	112
#define HW_SCRATCH_BASE		112		/* colour for framebuffers without: 113 .. 120 */
#define SCRATCH_TEXTURES	8
#define MAX_RENDERBUFFER_NAMES	8		/* colour storage: Zero textures 121 .. 128 */
#define HW_RENDERBUFFER_BASE	120
#define MAX_FRAMEBUFFER_NAMES	15		/* Zero ids 1 .. 15 */
#define HW_SURFACE_FRAMEBUFFER	16		/* pglInitSurface: the default framebuffer */
#define MAX_OBJECT_NAMES	128		/* programs and shaders */
#define MAX_HW_PROGRAMS		64
#define UNITS			PGPU_MAX_TEXTURE_UNITS
#define ATTRIBS			PGPU_MAX_ATTRIBUTES
#define MAX_SIZE		2048		/* textures, renderbuffers, viewport */
#define MAX_LEVELS		12
#define MAX_LIGHTS		4
#define MV_STACK		16
#define PROJ_STACK		4
#define TEX_STACK		4

/* uniform location = index << 16 | array element */
#define LOCATION(index, element)	((GLint) ((index) << 16 | (element)))

/* ---- objects ------------------------------------------------------------------- */

typedef struct
{
	bool used;			/* name generated or bound */
	bool bound;			/* an object (GL: created by the first bind) */
	GLsizeiptr size;
	GLenum usage;
	uint8_t *shadow;		/* copy of index data (ELEMENT_ARRAY_BUFFER uploads) */
} buffer_t;

enum { TEX_NONE, TEX_2D, TEX_CUBE };

typedef struct
{
	bool used;
	uint8_t target;			/* TEX_2D, TEX_CUBE once bound */
	bool storage;			/* created on the Zero */
	uint16_t width, height;		/* level 0 */
	uint8_t format;			/* enum pgpu_format */
	GLenum gl_format, gl_type;	/* of the levels (GL_ETC1_RGB8_OES: compressed) */
	uint32_t generation;		/* TEXTURE_CREATE count (framebuffers follow it) */
	GLenum min_filter, mag_filter, wrap_s, wrap_t;
	bool generate_mipmap;		/* GL ES 1.1 GL_GENERATE_MIPMAP */
	bool empty;			/* level 0 has no texels (0 x n): incomplete */
	uint8_t faces;			/* the faces whose level 0 is defined (2D: bit 0) */
	bool orphan;			/* deleted, still attached to a framebuffer */
	GLuint orphan_name;		/* its GL name then (attachment queries) */
	uint16_t bad_levels;		/* mipmap levels not fitting level 0: incomplete
					   with a mipmap filter (pgl ignores their data) */
} texture_t;

typedef struct
{
	bool used;			/* name generated or bound */
	bool bound;			/* an object (GL: created by the first bind) */
	GLenum format;			/* 0: no storage */
	bool orphan;			/* deleted, still attached to a framebuffer */
	GLuint orphan_name;		/* its GL name then (attachment queries) */
	uint16_t width, height;
	uint32_t generation;
} renderbuffer_t;

enum { ATT_COLOR, ATT_DEPTH, ATT_STENCIL, ATTACHMENTS };

typedef struct
{
	GLenum type;			/* GL_NONE, GL_TEXTURE, GL_RENDERBUFFER */
	GLuint name;
	GLint level;
	uint8_t face;
} attachment_t;

typedef struct
{
	bool used;			/* name generated or bound */
	bool bound;			/* an object (GL: created by the first bind) */
	attachment_t att[ATTACHMENTS];
	bool created;			/* on the Zero, with: */
	uint32_t sent_texture, sent_flags, sent_generation;
	uint8_t scratch;		/* colour texture when none is attached (0: none) */
	uint16_t scratch_width, scratch_height;
} framebuffer_t;

enum { OBJ_NONE, OBJ_SHADER, OBJ_PROGRAM };

typedef struct
{
	uint8_t kind;
	bool delete_pending;
	GLuint name;			/* its GL name: the slot + MAX_OBJECT_NAMES * a generation */
	/* shader */
	GLenum shader_type;
	uint16_t attached;		/* number of programs it is attached to */
	const pgpu_program_info_t *binary;	/* from glShaderBinary */
	char *source;			/* glShaderSource (with a compiler) */
	char *compiled_source;		/* of the last successful compile */
	bool compiled;
	char *log_buf;			/* info log (shader: compile, program: link) */
	/* program */
	uint8_t hw;			/* Zero id, 0 = none */
	bool linked;
	bool validated;
	const char *log;
	const pgpu_program_info_t *info;
	uint32_t *values;		/* uniform words as sent (float, int32, bool 0 / ~0) */
	uint16_t *first;		/* per uniform: index of its first value */
	uint8_t units[PGPU_MAX_SAMPLERS];
	GLuint shaders[2];
	pgpu_program_info_t *compiled_info;	/* linked from source (pglc_free) */
	struct attrib_binding { char *name; GLuint index; } *bindings;	/* glBindAttribLocation */
	unsigned n_bindings;
} object_t;

typedef struct
{
	bool enabled;
	uint8_t size;
	GLenum gl_type;
	bool normalized;
	GLsizei stride;			/* as given */
	uint8_t type;			/* enum pgpu_type */
	const void *pointer;
	GLuint buffer;			/* 0: client memory */
} array_t;

typedef struct
{
	float ambient[4], diffuse[4], specular[4];
	float position[4];		/* eye coordinates */
	float spot_direction[3], spot_exponent, spot_cutoff;
	float attenuation[3];
} light_t;

/* what the Zero has, for state sent when a draw needs it */
typedef struct
{
	bool valid;
	uint32_t buffer, offset, stride, size, type;
} sent_array_t;

static struct
{
	uint16_t width, height;		/* the screen (framebuffer 0) */

	uint32_t errors;		/* GL error flags: bit (error - GL_INVALID_ENUM) */
	uint32_t synced_packets;	/* pgpu_packets_sent () at the last error sync */
	bool debug;			/* PGL_DEBUG set: print the Zero's errors */
	uint32_t zero_error[3];

	/* GL names -> slots (the index of these arrays, and the Zero id where it
	   is the same): GL allows any name, created by glBind* */
	GLuint buffer_names[MAX_BUFFER_NAMES + 1];
	GLuint texture_names[MAX_TEXTURE_NAMES + 1];
	GLuint renderbuffer_names[MAX_RENDERBUFFER_NAMES + 1];
	GLuint framebuffer_names[MAX_FRAMEBUFFER_NAMES + 1];
	GLuint last_name[4];			/* per kind: names only grow (as Mesa's) */
	GLuint last_object_name;		/* shaders and programs */

	buffer_t buffers[MAX_BUFFER_NAMES + 1];
	texture_t textures[TEX_DEFAULT_CUBE + 1];	/* and the default objects */
	GLuint scratch_owner[SCRATCH_TEXTURES];	/* framebuffer using each scratch texture */
	renderbuffer_t renderbuffers[MAX_RENDERBUFFER_NAMES + 1];
	framebuffer_t framebuffers[MAX_FRAMEBUFFER_NAMES + 1];
	object_t objects[MAX_OBJECT_NAMES + 1];
	bool hw_program_used[MAX_HW_PROGRAMS + 1];

	GLuint array_buffer, element_buffer;
	GLuint renderbuffer, framebuffer;
	GLuint program;
	unsigned active_unit, client_active_unit;
	GLuint bound_2d[UNITS], bound_cube[UNITS];

	array_t attribs[ATTRIBS];
	float attrib_current[ATTRIBS][4];
	array_t ff_arrays[4];		/* enum pgpu_attribute order */

	/* enables */
	uint32_t caps;			/* PGPU_CAP_* as the application set them */
	bool texture_2d[UNITS];		/* GL_TEXTURE_2D per unit (unit 0 drives the cap) */
	bool normalize, rescale_normal;
	bool dither, sample_alpha_to_coverage, sample_coverage, point_smooth, line_smooth;
	bool multisample, sample_alpha_to_one;

	/* fragment state */
	GLfloat clear_color[4], clear_depth;
	GLint clear_stencil;
	GLboolean color_mask[4], depth_mask;
	GLenum depth_func, cull_face, front_face;
	GLenum blend_src_rgb, blend_dst_rgb, blend_src_alpha, blend_dst_alpha;
	GLenum blend_eq_rgb, blend_eq_alpha;
	GLfloat blend_color[4];
	GLenum stencil_func[2], stencil_fail[2], stencil_zfail[2], stencil_zpass[2];
	GLint stencil_ref[2];
	GLuint stencil_value_mask[2], stencil_writemask[2];
	GLint viewport[4], scissor[4];
	GLfloat depth_range[2];
	GLfloat line_width, polygon_offset_factor, polygon_offset_units;
	GLfloat sample_coverage_value;
	GLboolean sample_coverage_invert;
	GLenum generate_mipmap_hint, perspective_hint, point_smooth_hint, line_smooth_hint, fog_hint;
	GLint pack_alignment, unpack_alignment;

	/* fixed function */
	GLenum matrix_mode;
	float modelview[MV_STACK][16], projection[PROJ_STACK][16], texture_matrix[TEX_STACK][16];
	int modelview_depth, projection_depth, texture_depth;	/* index of the top */
	bool matrix_dirty[3];
	float current_color[4], current_normal[3], current_texcoord[4];
	light_t lights[MAX_LIGHTS];
	float material_ambient[4], material_diffuse[4], material_specular[4], material_emission[4];
	float material_shininess;
	float light_model_ambient[4];
	bool light_model_two_side;
	GLenum fog_mode;
	float fog_color[4], fog_start, fog_end, fog_density;
	GLenum shade_model;
	GLenum alpha_func;
	GLclampf alpha_ref;
	GLenum tex_env_mode;
	float tex_env_color[4];

	/* sent to the Zero (reset by pglInit) */
	uint32_t sent_caps;
	uint32_t sent_units[UNITS];
	bool sent_units_valid[UNITS];
	sent_array_t sent_attribs[ATTRIBS];
	uint32_t sent_attribs_mask;
	bool sent_attribs_mask_valid;
	sent_array_t sent_ff[4];
	uint32_t sent_ff_mask;
	bool sent_ff_mask_valid;
	GLuint sent_framebuffer;
	bool surface;			/* the default framebuffer is HW_SURFACE_FRAMEBUFFER */
	uint32_t stream_bytes[2];	/* sizes of the stream buffers */
} S;

static const uint8_t type_bytes[] = {4, 2, 2, 1, 1, 1, 1, 2, 2, 4};	/* enum pgpu_type */
static const uint8_t format_bytes[] = {4, 2, 2, 2, 1, 1, 2, 0, 3};	/* enum pgpu_format */

/* ---- helpers ------------------------------------------------------------------- */

static void set_error (GLenum error)
{
	S.errors |= 1u << (error - GL_INVALID_ENUM);
}

#define ERROR(e)		do { set_error (e); return; } while (0)
#define ERROR_RET(e, r)		do { set_error (e); return (r); } while (0)

static uint32_t f2u (float f)
{
	uint32_t u;
	memcpy (&u, &f, sizeof u);
	return u;
}

static float clampf (float v, float lo, float hi)
{
	return v < lo ? lo : v > hi ? hi : v;
}

static uint32_t color_u32 (const float c[4])
{
	uint32_t b[4];
	for (int i = 0; i < 4; i++)
	{
		b[i] = (uint32_t) (clampf (c[i], 0.0f, 1.0f) * 255.0f + 0.5f);
	}
	return PGPU_RGBA (b[0], b[1], b[2], b[3]);
}

static bool is_pot (unsigned n)
{
	return n && !(n & (n - 1));
}

static void mat_identity (float *m)
{
	memset (m, 0, 16 * sizeof (float));
	m[0] = m[5] = m[10] = m[15] = 1.0f;
}

/* r = a * b (column-major); r may be a */
static void mat_multiply (float *r, const float *a, const float *b)
{
	float t[16];
	for (int c = 0; c < 4; c++)
	{
		for (int row = 0; row < 4; row++)
		{
			float s = 0.0f;
			for (int k = 0; k < 4; k++)
			{
				s += a[k * 4 + row] * b[c * 4 + k];
			}
			t[c * 4 + row] = s;
		}
	}
	memcpy (r, t, sizeof t);
}

static void mat_transform (float *r, const float *m, const float *v)	/* r = m * v */
{
	float t[4];
	for (int row = 0; row < 4; row++)
	{
		t[row] = m[row] * v[0] + m[4 + row] * v[1] + m[8 + row] * v[2] + m[12 + row] * v[3];
	}
	memcpy (r, t, sizeof t);
}

/* ---- Zero errors ----------------------------------------------------------------- */

static GLenum map_zero_error (uint32_t code)
{
	switch (code)
	{
	case PGPU_ERR_ENUM:	return GL_INVALID_ENUM;
	case PGPU_ERR_LIMIT:	return GL_INVALID_VALUE;
	case PGPU_ERR_MEMORY:	return GL_OUT_OF_MEMORY;
	default:		return GL_INVALID_OPERATION;
	}
}

/* move ERROR replies into the GL error flags; with program_hw, errors of
   uploading that program are counted instead (return value) */
static unsigned drain_zero_errors (uint32_t program_hw)
{
	unsigned program_errors = 0;
	uint32_t e[3];
	while (pgpu_poll_error (e))
	{
		if (S.debug)
		{
			fprintf (stderr, "pgl: Zero error %lu, opcode %02lx, detail %lu\n", (unsigned long) e[0],
				 (unsigned long) e[1], (unsigned long) e[2]);
		}
		memcpy (S.zero_error, e, sizeof e);
		if (   program_hw
		    && (e[1] == PGPU_OP_PROGRAM_CREATE || e[1] == PGPU_OP_PROGRAM_DATA))
		{
			program_errors++;
			continue;
		}
		set_error (map_zero_error (e[0]));
	}
	return program_errors;
}

/* errors the Zero finds come back as ERROR replies: wait for those of the
   commands sent so far (a PING round trip, only if anything was sent) */
static void sync_errors (void)
{
	if (pgpu_packets_sent () != S.synced_packets)
	{
		static uint32_t cookie = 0x9E000000u;
		pgpu_ping_wait (++cookie, 1000);
		pgpu_link_settle ();
		S.synced_packets = pgpu_packets_sent ();
	}
	drain_zero_errors (0);
}

GLenum glGetError (void)
{
	sync_errors ();
	for (unsigned bit = 0; bit < 7; bit++)
	{
		if (S.errors & (1u << bit))
		{
			S.errors &= ~(1u << bit);
			return GL_INVALID_ENUM + bit;
		}
	}
	return GL_NO_ERROR;
}

void pglGetZeroError (uint32_t error[3])
{
	sync_errors ();
	memcpy (error, S.zero_error, sizeof S.zero_error);
}

void glFlush (void)
{
	pgpu_flush ();
}

void glFinish (void)
{
	/* PONG comes when the Zero has executed everything before it, and the
	   ERROR replies of those commands come before it */
	static uint32_t cookie = 0x9C000000u;
	pgpu_ping_wait (++cookie, 1000);
	pgpu_link_settle ();
	drain_zero_errors (0);
}

/* ---- init ------------------------------------------------------------------------ */

static void free_objects (void)
{
	for (unsigned i = 1; i <= MAX_BUFFER_NAMES; i++)
	{
		free (S.buffers[i].shadow);
	}
	for (unsigned i = 1; i <= MAX_OBJECT_NAMES; i++)
	{
		object_t *o = &S.objects[i];
		free (o->values);
		free (o->first);
		free (o->source);
		free (o->compiled_source);
		free (o->log_buf);
		pglc_free (o->compiled_info);
		for (unsigned b = 0; b < o->n_bindings; b++)
		{
			free (o->bindings[b].name);
		}
		free (o->bindings);
	}
}

static void default_light (light_t *l, int i)
{
	static const float black[4] = {0, 0, 0, 1}, white[4] = {1, 1, 1, 1};
	memcpy (l->ambient, black, sizeof black);
	memcpy (l->diffuse, i == 0 ? white : black, sizeof white);
	memcpy (l->specular, i == 0 ? white : black, sizeof white);
	l->position[0] = l->position[1] = l->position[3] = 0.0f;
	l->position[2] = 1.0f;
	l->spot_direction[0] = l->spot_direction[1] = 0.0f;
	l->spot_direction[2] = -1.0f;
	l->spot_exponent = 0.0f;
	l->spot_cutoff = 180.0f;
	l->attenuation[0] = 1.0f;
	l->attenuation[1] = l->attenuation[2] = 0.0f;
}

static void texture_take (unsigned n);

bool pglInit (void)
{
	pgpu_reset ();
	pgpu_flush ();

	pgpu_reply_t r;
	bool ok = pgpu_wait_reply (PGPU_REPLY_INFO, &r, 200) && r.length >= 7;

	free_objects ();
	memset (&S, 0, sizeof S);
	S.width = ok ? r.payload[1] & 0xFFFF : 320;
	S.height = ok ? r.payload[1] >> 16 : 240;
	uint32_t e[3];
	while (pgpu_poll_error (e))
	{
	}

	/* GL defaults (the Zero's defaults after RESET are the same, §7.11) */
	S.clear_depth = 1.0f;
	for (int i = 0; i < 4; i++)
	{
		S.color_mask[i] = GL_TRUE;
	}
	S.depth_mask = GL_TRUE;
	S.depth_func = GL_LESS;
	S.cull_face = GL_BACK;
	S.front_face = GL_CCW;
	S.blend_src_rgb = S.blend_src_alpha = GL_ONE;
	S.blend_dst_rgb = S.blend_dst_alpha = GL_ZERO;
	S.blend_eq_rgb = S.blend_eq_alpha = GL_FUNC_ADD;
	for (int f = 0; f < 2; f++)
	{
		S.stencil_func[f] = GL_ALWAYS;
		S.stencil_fail[f] = S.stencil_zfail[f] = S.stencil_zpass[f] = GL_KEEP;
		S.stencil_value_mask[f] = S.stencil_writemask[f] = 0xFF;
	}
	S.viewport[2] = S.scissor[2] = S.width;
	S.viewport[3] = S.scissor[3] = S.height;
	S.depth_range[1] = 1.0f;
	S.line_width = 1.0f;
	S.sample_coverage_value = 1.0f;
	S.generate_mipmap_hint = S.perspective_hint = S.point_smooth_hint = GL_DONT_CARE;
	S.line_smooth_hint = S.fog_hint = GL_DONT_CARE;
	S.pack_alignment = S.unpack_alignment = 4;
	S.multisample = S.dither = true;
	S.debug = getenv ("PGL_DEBUG") != NULL;
	/* GL_DITHER has no effect: the V3D's dither moves values by up to two
	   steps (0 becomes 2 of 31, measured), where GL allows one of the two
	   nearest values (GL ES 2.0 4.1.7); without it, the V3D truncates, which
	   is what GL does without dithering */
	pgpu_disable (PGPU_CAP_DITHER);
	S.sent_caps = 0;
	texture_take (TEX_DEFAULT_2D);
	S.textures[TEX_DEFAULT_2D].target = TEX_2D;
	texture_take (TEX_DEFAULT_CUBE);
	S.textures[TEX_DEFAULT_CUBE].target = TEX_CUBE;

	S.matrix_mode = GL_MODELVIEW;
	mat_identity (S.modelview[0]);
	mat_identity (S.projection[0]);
	mat_identity (S.texture_matrix[0]);
	S.current_color[0] = S.current_color[1] = S.current_color[2] = S.current_color[3] = 1.0f;
	S.current_normal[2] = 1.0f;
	S.current_texcoord[3] = 1.0f;
	for (int i = 0; i < MAX_LIGHTS; i++)
	{
		default_light (&S.lights[i], i);
	}
	static const float mat_a[4] = {0.2f, 0.2f, 0.2f, 1}, mat_d[4] = {0.8f, 0.8f, 0.8f, 1};
	static const float black[4] = {0, 0, 0, 1};
	memcpy (S.material_ambient, mat_a, sizeof mat_a);
	memcpy (S.material_diffuse, mat_d, sizeof mat_d);
	memcpy (S.material_specular, black, sizeof black);
	memcpy (S.material_emission, black, sizeof black);
	memcpy (S.light_model_ambient, mat_a, sizeof mat_a);
	S.fog_mode = GL_EXP;
	S.fog_end = S.fog_density = 1.0f;
	S.shade_model = GL_SMOOTH;
	S.alpha_func = GL_ALWAYS;
	S.tex_env_mode = GL_MODULATE;
	for (int i = 0; i < ATTRIBS; i++)
	{
		S.attrib_current[i][3] = 1.0f;
		S.attribs[i].size = 4;
		S.attribs[i].gl_type = GL_FLOAT;
	}
	static const uint8_t ff_sizes[4] = {4, 4, 3, 4};
	for (int i = 0; i < 4; i++)
	{
		S.ff_arrays[i].size = ff_sizes[i];
		S.ff_arrays[i].gl_type = GL_FLOAT;
	}

	return ok;
}

bool pglInitSurface (unsigned width, unsigned height)
{
	if (!pglInit ())
	{
		return false;
	}
	if (width < 1 || height < 1 || width > MAX_SIZE || height > MAX_SIZE)
	{
		return false;
	}
	pgpu_texture_create (HW_SURFACE_TEXTURE, width, height, PGPU_RGBA8888);
	pgpu_framebuffer_create (HW_SURFACE_FRAMEBUFFER, HW_SURFACE_TEXTURE, 0, PGPU_FRAMEBUFFER_DEPTH_STENCIL);
	S.surface = true;
	S.width = width;
	S.height = height;
	/* sent too: the Zero's viewport after RESET is the panel's */
	glViewport (0, 0, (GLsizei) width, (GLsizei) height);
	glScissor (0, 0, (GLsizei) width, (GLsizei) height);
	return true;
}

/* the screen has a new size (a monitor plugged in or out, DISPLAY): a
   viewport and scissor box that covered the whole screen cover the new one,
   as when a window system resizes a window. The Zero changes size between
   frames and tells afterwards, so a frame or two may still use the old size */
static uint32_t display_seen;

static void screen_update (void)
{
	pgpu_display_t d;
	uint32_t n = pgpu_get_display (&d);
	if (n == display_seen || S.surface)
	{
		return;
	}
	display_seen = n;
	if (d.width == 0 || d.height == 0 || (d.width == S.width && d.height == S.height))
	{
		return;
	}

	bool viewport_full =    S.viewport[0] == 0 && S.viewport[1] == 0
			     && S.viewport[2] == (GLint) S.width && S.viewport[3] == (GLint) S.height;
	bool scissor_full =    S.scissor[0] == 0 && S.scissor[1] == 0
			    && S.scissor[2] == (GLint) S.width && S.scissor[3] == (GLint) S.height;
	S.width = d.width;
	S.height = d.height;
	if (viewport_full)
	{
		glViewport (0, 0, (GLsizei) S.width, (GLsizei) S.height);
	}
	if (scissor_full)
	{
		glScissor (0, 0, (GLsizei) S.width, (GLsizei) S.height);
	}
}

void pglGetScreenSize (unsigned *width, unsigned *height)
{
	screen_update ();
	*width = S.width;
	*height = S.height;
}

void pglSwapBuffers (void)
{
	pgpu_frame_end (0);
	screen_update ();
}

/* ---- enum mapping ------------------------------------------------------------------ */

static int map_func (GLenum func)		/* GL_NEVER .. GL_ALWAYS */
{
	return func >= GL_NEVER && func <= GL_ALWAYS ? (int) (func - GL_NEVER) : -1;
}

static int map_blend_factor (GLenum f, bool source)
{
	switch (f)
	{
	case GL_ZERO:				return PGPU_ZERO;
	case GL_ONE:				return PGPU_ONE;
	case GL_SRC_COLOR:			return PGPU_SRC_COLOR;
	case GL_ONE_MINUS_SRC_COLOR:		return PGPU_ONE_MINUS_SRC_COLOR;
	case GL_SRC_ALPHA:			return PGPU_SRC_ALPHA;
	case GL_ONE_MINUS_SRC_ALPHA:		return PGPU_ONE_MINUS_SRC_ALPHA;
	case GL_DST_ALPHA:			return PGPU_DST_ALPHA;
	case GL_ONE_MINUS_DST_ALPHA:		return PGPU_ONE_MINUS_DST_ALPHA;
	case GL_DST_COLOR:			return PGPU_DST_COLOR;
	case GL_ONE_MINUS_DST_COLOR:		return PGPU_ONE_MINUS_DST_COLOR;
	case GL_SRC_ALPHA_SATURATE:		return source ? PGPU_SRC_ALPHA_SATURATE : -1;
	case GL_CONSTANT_COLOR:			return PGPU_CONSTANT_COLOR;
	case GL_ONE_MINUS_CONSTANT_COLOR:	return PGPU_ONE_MINUS_CONSTANT_COLOR;
	case GL_CONSTANT_ALPHA:			return PGPU_CONSTANT_ALPHA;
	case GL_ONE_MINUS_CONSTANT_ALPHA:	return PGPU_ONE_MINUS_CONSTANT_ALPHA;
	default:				return -1;
	}
}

static int map_blend_equation (GLenum mode)
{
	switch (mode)
	{
	case GL_FUNC_ADD:		return PGPU_FUNC_ADD;
	case GL_FUNC_SUBTRACT:		return PGPU_FUNC_SUBTRACT;
	case GL_FUNC_REVERSE_SUBTRACT:	return PGPU_FUNC_REVERSE_SUBTRACT;
	default:			return -1;
	}
}

static int map_stencil_op (GLenum op)
{
	switch (op)
	{
	case GL_KEEP:		return PGPU_KEEP;
	case GL_ZERO:		return PGPU_ZERO_OP;
	case GL_REPLACE:	return PGPU_REPLACE_OP;
	case GL_INCR:		return PGPU_INCR;
	case GL_DECR:		return PGPU_DECR;
	case GL_INVERT:		return PGPU_INVERT;
	case GL_INCR_WRAP:	return PGPU_INCR_WRAP;
	case GL_DECR_WRAP:	return PGPU_DECR_WRAP;
	default:		return -1;
	}
}

static int map_face (GLenum face)
{
	switch (face)
	{
	case GL_FRONT:		return PGPU_FRONT;
	case GL_BACK:		return PGPU_BACK;
	case GL_FRONT_AND_BACK:	return PGPU_FRONT_AND_BACK;
	default:		return -1;
	}
}

static int map_filter (GLenum f)
{
	switch (f)
	{
	case GL_NEAREST:		return PGPU_NEAREST;
	case GL_LINEAR:			return PGPU_LINEAR;
	case GL_NEAREST_MIPMAP_NEAREST:	return PGPU_NEAREST_MIPMAP_NEAREST;
	case GL_LINEAR_MIPMAP_NEAREST:	return PGPU_LINEAR_MIPMAP_NEAREST;
	case GL_NEAREST_MIPMAP_LINEAR:	return PGPU_NEAREST_MIPMAP_LINEAR;
	case GL_LINEAR_MIPMAP_LINEAR:	return PGPU_LINEAR_MIPMAP_LINEAR;
	default:			return -1;
	}
}

static int map_wrap (GLenum w)
{
	switch (w)
	{
	case GL_REPEAT:			return PGPU_REPEAT;
	case GL_CLAMP_TO_EDGE:		return PGPU_CLAMP_TO_EDGE;
	case GL_MIRRORED_REPEAT:	return PGPU_MIRRORED_REPEAT;
	default:			return -1;
	}
}

static int map_array_type (GLenum type, bool normalized)
{
	switch (type)
	{
	case GL_FLOAT:		return PGPU_FLOAT;
	case GL_FIXED:		return PGPU_FIXED;
	case GL_BYTE:		return normalized ? PGPU_BYTE_NORM : PGPU_BYTE;
	case GL_UNSIGNED_BYTE:	return normalized ? PGPU_UBYTE_NORM : PGPU_UBYTE;
	case GL_SHORT:		return normalized ? PGPU_SHORT_NORM : PGPU_SHORT;
	case GL_UNSIGNED_SHORT:	return normalized ? PGPU_USHORT_NORM : PGPU_USHORT;
	default:		return -1;
	}
}

/* format and type of glTexImage2D -> enum pgpu_format; -1 enum error,
   -2 invalid combination */
static int map_texture_format (GLenum format, GLenum type)
{
	switch (format)
	{
	case GL_RGBA: case GL_RGB: case GL_LUMINANCE: case GL_ALPHA: case GL_LUMINANCE_ALPHA:
	case GL_BGRA_EXT:			/* EXT_texture_format_BGRA8888: stored RGBA */
		break;
	default:
		return -1;
	}
	switch (type)
	{
	case GL_UNSIGNED_BYTE:
		switch (format)
		{
		case GL_RGBA:		 return PGPU_RGBA8888;
		case GL_BGRA_EXT:	 return PGPU_RGBA8888;
		case GL_RGB:		 return PGPU_RGB888;
		case GL_LUMINANCE:	 return PGPU_L8;
		case GL_ALPHA:		 return PGPU_A8;
		default:		 return PGPU_LA88;
		}
	case GL_UNSIGNED_SHORT_5_6_5:	return format == GL_RGB ? PGPU_RGB565 : -2;
	case GL_UNSIGNED_SHORT_4_4_4_4:	return format == GL_RGBA ? PGPU_RGBA4444 : -2;
	case GL_UNSIGNED_SHORT_5_5_5_1:	return format == GL_RGBA ? PGPU_RGBA5551 : -2;
	default:			return -1;
	}
}

/* ---- enables ----------------------------------------------------------------------- */

/* GL_LIGHT0 .. 3 and the other caps with a Zero bit */
static uint32_t cap_bit (GLenum cap)
{
	switch (cap)
	{
	case GL_DEPTH_TEST:		return PGPU_CAP_DEPTH_TEST;
	case GL_CULL_FACE:		return PGPU_CAP_CULL_FACE;
	case GL_BLEND:			return PGPU_CAP_BLEND;
	case GL_LIGHTING:		return PGPU_CAP_LIGHTING;
	case GL_LIGHT0:			return PGPU_CAP_LIGHT0;
	case GL_LIGHT1:			return PGPU_CAP_LIGHT1;
	case GL_LIGHT2:			return PGPU_CAP_LIGHT2;
	case GL_LIGHT3:			return PGPU_CAP_LIGHT3;
	case GL_FOG:			return PGPU_CAP_FOG;
	case GL_ALPHA_TEST:		return PGPU_CAP_ALPHA_TEST;
	case GL_COLOR_MATERIAL:		return PGPU_CAP_COLOR_MATERIAL;
	case GL_SCISSOR_TEST:		return PGPU_CAP_SCISSOR_TEST;
	case GL_POLYGON_OFFSET_FILL:	return PGPU_CAP_POLYGON_OFFSET_FILL;
	case GL_STENCIL_TEST:		return PGPU_CAP_STENCIL_TEST;
	default:			return 0;
	}
}

/* the caps without a Zero bit; NULL if not a cap */
static bool *cap_flag (GLenum cap)
{
	switch (cap)
	{
	case GL_TEXTURE_2D:			return &S.texture_2d[S.active_unit];
	case GL_DITHER:				return &S.dither;
	case GL_NORMALIZE:			return &S.normalize;
	case GL_RESCALE_NORMAL:			return &S.rescale_normal;
	case GL_SAMPLE_ALPHA_TO_COVERAGE:	return &S.sample_alpha_to_coverage;
	case GL_SAMPLE_COVERAGE:		return &S.sample_coverage;
	case GL_POINT_SMOOTH:			return &S.point_smooth;
	case GL_LINE_SMOOTH:			return &S.line_smooth;
	case GL_MULTISAMPLE:			return &S.multisample;
	case GL_SAMPLE_ALPHA_TO_ONE:		return &S.sample_alpha_to_one;
	default:				return NULL;
	}
}

static array_t *client_state_array (GLenum array)
{
	switch (array)
	{
	case GL_VERTEX_ARRAY:		return &S.ff_arrays[PGPU_ATTR_POSITION];
	case GL_COLOR_ARRAY:		return &S.ff_arrays[PGPU_ATTR_COLOR];
	case GL_NORMAL_ARRAY:		return &S.ff_arrays[PGPU_ATTR_NORMAL];
	case GL_TEXTURE_COORD_ARRAY:	return S.client_active_unit == 0 ? &S.ff_arrays[PGPU_ATTR_TEXCOORD] : NULL;
	default:			return NULL;
	}
}

static void set_cap (GLenum cap, bool on)
{
	uint32_t bit = cap_bit (cap);
	if (bit)
	{
		S.caps = on ? S.caps | bit : S.caps & ~bit;
		return;
	}
	bool *flag = cap_flag (cap);
	if (!flag)
	{
		ERROR (GL_INVALID_ENUM);
	}
	*flag = on;
}

void glEnable (GLenum cap)	{ set_cap (cap, true); }
void glDisable (GLenum cap)	{ set_cap (cap, false); }

GLboolean glIsEnabled (GLenum cap)
{
	uint32_t bit = cap_bit (cap);
	if (bit)
	{
		return (S.caps & bit) ? GL_TRUE : GL_FALSE;
	}
	bool *flag = cap_flag (cap);
	if (flag)
	{
		return *flag;
	}
	array_t *a = client_state_array (cap);
	if (a)
	{
		return a->enabled;
	}
	ERROR_RET (GL_INVALID_ENUM, GL_FALSE);
}

void glEnableClientState (GLenum array)
{
	array_t *a = client_state_array (array);
	if (!a)
	{
		ERROR (GL_INVALID_ENUM);
	}
	a->enabled = true;
}

void glDisableClientState (GLenum array)
{
	array_t *a = client_state_array (array);
	if (!a)
	{
		ERROR (GL_INVALID_ENUM);
	}
	a->enabled = false;
}

/* ---- framebuffer validation ------------------------------------------------------------ */

static bool rb_is_color (GLenum f)
{
	return    f == GL_RGBA4 || f == GL_RGB5_A1 || f == GL_RGB565 || f == GL_RGBA8_OES || f == GL_RGB8_OES
	       || f == GL_BGRA_EXT || f == GL_BGRA8_EXT;	/* EXT_texture_format_BGRA8888: stored RGBA */
}

static bool rb_has_depth (GLenum f)
{
	return f == GL_DEPTH_COMPONENT16 || f == GL_DEPTH_COMPONENT24_OES || f == GL_DEPTH24_STENCIL8_OES;
}

static bool rb_has_stencil (GLenum f)
{
	return f == GL_STENCIL_INDEX8 || f == GL_DEPTH24_STENCIL8_OES;
}

static bool texture_has_alpha (uint8_t format)
{
	return format == PGPU_RGBA8888 || format == PGPU_RGBA4444 || format == PGPU_RGBA5551;
}

/* colour-renderable texture formats (RGB ones read alpha 1 on the Zero) */
static bool texture_renderable (uint8_t format)
{
	return texture_has_alpha (format) || format == PGPU_RGB888 || format == PGPU_RGB565;
}

/* status of a framebuffer object; its size and colour target */
static GLenum framebuffer_status (const framebuffer_t *fb, unsigned *w, unsigned *h,
				  uint32_t *hw_texture, uint32_t *generation)
{
	const attachment_t *c = &fb->att[ATT_COLOR];
	bool any = false;
	unsigned width = 0, height = 0;

	for (int i = 0; i < ATTACHMENTS; i++)
	{
		const attachment_t *a = &fb->att[i];
		unsigned aw, ah;
		if (a->type == GL_NONE)
		{
			continue;
		}
		any = true;
		if (a->type == GL_TEXTURE)
		{
			const texture_t *t = &S.textures[a->name];
			if (a->name == 0 || a->name > MAX_TEXTURE_NAMES || !t->used || !t->storage || t->empty)
			{
				return GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT;
			}
			if (i != ATT_COLOR || a->level != 0 || !texture_renderable (t->format))
			{
				return GL_FRAMEBUFFER_UNSUPPORTED;
			}
			aw = t->width;
			ah = t->height;
		}
		else
		{
			const renderbuffer_t *rb = &S.renderbuffers[a->name];
			if (!rb->used || !rb->format || !rb->width || !rb->height)
			{
				return GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT;
			}
			if (   (i == ATT_COLOR && !rb_is_color (rb->format))
			    || (i == ATT_DEPTH && !rb_has_depth (rb->format))
			    || (i == ATT_STENCIL && !rb_has_stencil (rb->format)))
			{
				return GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT;
			}
			aw = rb->width;
			ah = rb->height;
		}
		if (width && (aw != width || ah != height))
		{
			return GL_FRAMEBUFFER_INCOMPLETE_DIMENSIONS;
		}
		width = aw;
		height = ah;
	}
	if (!any)
	{
		return GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT;
	}
	if (w)
	{
		*w = width;
		*h = height;
	}
	if (hw_texture)
	{
		if (c->type == GL_NONE)
		{
			*hw_texture = 0;		/* validate_framebuffer adds a scratch colour */
			*generation = 0;
		}
		else if (c->type == GL_TEXTURE)
		{
			*hw_texture = c->name | (uint32_t) c->face << 24;
			*generation = S.textures[c->name].generation;
		}
		else
		{
			*hw_texture = HW_RENDERBUFFER_BASE + c->name;
			*generation = S.renderbuffers[c->name].generation;
		}
	}
	return GL_FRAMEBUFFER_COMPLETE;
}

static bool target_has_depth (void)
{
	if (!S.framebuffer)
	{
		return true;
	}
	const attachment_t *d = &S.framebuffers[S.framebuffer].att[ATT_DEPTH];
	return d->type == GL_RENDERBUFFER && rb_has_depth (S.renderbuffers[d->name].format);
}

static bool target_has_stencil (void)
{
	if (!S.framebuffer)
	{
		return true;
	}
	const attachment_t *s = &S.framebuffers[S.framebuffer].att[ATT_STENCIL];
	return s->type == GL_RENDERBUFFER && rb_has_stencil (S.renderbuffers[s->name].format);
}

static bool target_has_color (void)
{
	return !S.framebuffer || S.framebuffers[S.framebuffer].att[ATT_COLOR].type != GL_NONE;
}

static bool target_has_alpha (void)
{
	if (!S.framebuffer)
	{
		return S.surface;
	}
	const attachment_t *c = &S.framebuffers[S.framebuffer].att[ATT_COLOR];
	if (c->type == GL_NONE)
	{
		return false;
	}
	if (c->type == GL_TEXTURE)
	{
		return texture_has_alpha (S.textures[c->name].format);
	}
	GLenum f = S.renderbuffers[c->name].format;
	return f != GL_RGB565 && f != GL_RGB8_OES;
}

/* the bound framebuffer on the Zero, with its current attachments */
static void release_render_target (uint32_t hw);

/* the Zero renders to a colour target: a framebuffer with only depth and/or
   stencil gets a scratch colour texture (writes to it can't be seen) */
static uint32_t scratch_texture (GLuint name, framebuffer_t *fb, unsigned w, unsigned h)
{
	if (!fb->scratch)
	{
		for (unsigned i = 0; i < SCRATCH_TEXTURES && !fb->scratch; i++)
		{
			if (!S.scratch_owner[i])
			{
				S.scratch_owner[i] = name;
				fb->scratch = HW_SCRATCH_BASE + 1 + i;
				fb->scratch_width = fb->scratch_height = 0;
			}
		}
		if (!fb->scratch)
		{
			return 0;
		}
	}
	if (fb->scratch_width != w || fb->scratch_height != h)
	{
		release_render_target (fb->scratch);
		pgpu_texture_create (fb->scratch, w, h, PGPU_RGBA8888);
		fb->scratch_width = w;
		fb->scratch_height = h;
	}
	return fb->scratch;
}

static void free_scratch (framebuffer_t *fb)
{
	if (fb->scratch)
	{
		release_render_target (fb->scratch);
		pgpu_texture_delete (fb->scratch);
		S.scratch_owner[fb->scratch - HW_SCRATCH_BASE - 1] = 0;
		fb->scratch = 0;
	}
}

static bool validate_framebuffer (void)
{
	if (S.framebuffer)
	{
		framebuffer_t *fb = &S.framebuffers[S.framebuffer];
		uint32_t texture = 0, generation = 0;
		unsigned w = 0, h = 0;
		if (framebuffer_status (fb, &w, &h, &texture, &generation) != GL_FRAMEBUFFER_COMPLETE)
		{
			ERROR_RET (GL_INVALID_FRAMEBUFFER_OPERATION, false);
		}
		if (!texture)
		{
			texture = scratch_texture (S.framebuffer, fb, w, h);
			generation = w | h << 16;
			if (!texture)
			{
				ERROR_RET (GL_OUT_OF_MEMORY, false);
			}
		}
		else
		{
			free_scratch (fb);
		}
		/* depth and stencil live in the renderbuffer: framebuffers with the
		   same one share them (the Zero keeps depth and stencil together:
		   the depth renderbuffer names them, else the stencil one) */
		uint32_t flags = 0;
		if (target_has_depth () || target_has_stencil ())
		{
			GLuint rb = fb->att[target_has_depth () ? ATT_DEPTH : ATT_STENCIL].name;
			flags = PGPU_FRAMEBUFFER_DEPTH_STENCIL | PGPU_FRAMEBUFFER_SHARED (rb);
		}
		if (   !fb->created || fb->sent_texture != texture || fb->sent_flags != flags
		    || fb->sent_generation != generation)
		{
			pgpu_framebuffer_create (S.framebuffer, texture & 0xFFFF, texture >> 24, flags);
			fb->created = true;
			fb->sent_texture = texture;
			fb->sent_flags = flags;
			fb->sent_generation = generation;
		}
	}
	GLuint hw = S.framebuffer ? S.framebuffer : S.surface ? HW_SURFACE_FRAMEBUFFER : 0;
	if (S.sent_framebuffer != hw)
	{
		pgpu_bind_framebuffer (hw);
		S.sent_framebuffer = hw;
	}
	return true;
}

/* enables as the Zero needs them: depth and stencil tests pass without the buffers */
static void validate_caps (void)
{
	uint32_t caps = S.caps;
	if (S.texture_2d[0])
	{
		caps |= PGPU_CAP_TEXTURE_2D;
	}
	if (S.normalize || S.rescale_normal)
	{
		caps |= PGPU_CAP_NORMALIZE;
	}
	if (!target_has_depth ())
	{
		caps &= ~PGPU_CAP_DEPTH_TEST;
	}
	if (!target_has_stencil ())
	{
		caps &= ~PGPU_CAP_STENCIL_TEST;
	}
	if (caps & ~S.sent_caps)
	{
		pgpu_enable (caps & ~S.sent_caps);
	}
	if (S.sent_caps & ~caps)
	{
		pgpu_disable (S.sent_caps & ~caps);
	}
	S.sent_caps = caps;
}

/* ---- buffers ----------------------------------------------------------------------------- */

/* ---- names: a table per object type, slot -> GL name (0: free) ---- */

enum { NAMES_BUFFER, NAMES_TEXTURE, NAMES_RENDERBUFFER, NAMES_FRAMEBUFFER };

static GLuint *name_table (int kind, unsigned *max)
{
	switch (kind)
	{
	case NAMES_BUFFER:	*max = MAX_BUFFER_NAMES;	return S.buffer_names;
	case NAMES_TEXTURE:	*max = MAX_TEXTURE_NAMES;	return S.texture_names;
	case NAMES_RENDERBUFFER: *max = MAX_RENDERBUFFER_NAMES;	return S.renderbuffer_names;
	default:		*max = MAX_FRAMEBUFFER_NAMES;	return S.framebuffer_names;
	}
}

/* the slot of a GL name, 0 if it has none */
static unsigned slot_of (int kind, GLuint name)
{
	unsigned max;
	const GLuint *names = name_table (kind, &max);
	for (unsigned slot = 1; name && slot <= max; slot++)
	{
		if (names[slot] == name)
		{
			return slot;
		}
	}
	return 0;
}

/* the GL name of a slot (0 for slot 0) */
static GLuint name_of (int kind, unsigned slot)
{
	unsigned max;
	return slot ? name_table (kind, &max)[slot] : 0;
}

/* an object without a name still in use (deleted, but attached) */
static bool slot_busy (int kind, unsigned slot)
{
	switch (kind)
	{
	case NAMES_BUFFER:		return S.buffers[slot].used;
	case NAMES_TEXTURE:		return S.textures[slot].used;
	case NAMES_RENDERBUFFER:	return S.renderbuffers[slot].used;
	default:			return S.framebuffers[slot].used;
	}
}

/* the slot of a name, a free one if it has none; 0 if all are taken */
static unsigned slot_for (int kind, GLuint name)
{
	unsigned slot = slot_of (kind, name), max;
	GLuint *names = name_table (kind, &max);
	for (unsigned s = 1; !slot && s <= max; s++)
	{
		if (!names[s] && !slot_busy (kind, s))
		{
			names[s] = name;
			slot = s;
		}
	}
	return slot;
}

static void free_name (int kind, unsigned slot)
{
	unsigned max;
	name_table (kind, &max)[slot] = 0;
}

static void gen_names (GLsizei n, GLuint *out, int kind, void (*take) (unsigned))
{
	if (n < 0)
	{
		ERROR (GL_INVALID_VALUE);
	}
	for (GLsizei i = 0; i < n; i++)
	{
		GLuint name = S.last_name[kind] + 1;
		while (!name || slot_of (kind, name))
		{
			name++;			/* names only grow (as Mesa's): a deleted one stays invalid */
		}
		S.last_name[kind] = name;
		unsigned slot = slot_for (kind, name);
		if (!slot)
		{
			for (GLsizei j = i; j < n; j++)
			{
				out[j] = 0;
			}
			ERROR (GL_OUT_OF_MEMORY);	/* pgl's tables are full */
		}
		take (slot);
		out[i] = name;
	}
}

/* ---- buffers ---- */

static void buffer_take (unsigned n)	{ S.buffers[n].used = true; }

void glGenBuffers (GLsizei n, GLuint *buffers)
{
	gen_names (n, buffers, NAMES_BUFFER, buffer_take);
}

GLboolean glIsBuffer (GLuint buffer)
{
	unsigned slot = slot_of (NAMES_BUFFER, buffer);
	return slot && S.buffers[slot].bound;
}

void glBindBuffer (GLenum target, GLuint buffer)
{
	if (target != GL_ARRAY_BUFFER && target != GL_ELEMENT_ARRAY_BUFFER)
	{
		ERROR (GL_INVALID_ENUM);
	}
	if (buffer)
	{
		unsigned slot = slot_for (NAMES_BUFFER, buffer);
		if (!slot)
		{
			ERROR (GL_OUT_OF_MEMORY);	/* pgl's table is full */
		}
		buffer = slot;
		S.buffers[buffer].used = S.buffers[buffer].bound = true;
	}
	if (target == GL_ARRAY_BUFFER)
	{
		S.array_buffer = buffer;
	}
	else
	{
		S.element_buffer = buffer;
	}
}

static buffer_t *bound_buffer (GLenum target, GLuint *name)
{
	if (target == GL_ARRAY_BUFFER)
	{
		*name = S.array_buffer;
	}
	else if (target == GL_ELEMENT_ARRAY_BUFFER)
	{
		*name = S.element_buffer;
	}
	else
	{
		ERROR_RET (GL_INVALID_ENUM, NULL);
	}
	if (!*name)
	{
		ERROR_RET (GL_INVALID_OPERATION, NULL);
	}
	return &S.buffers[*name];
}

void glBufferData (GLenum target, GLsizeiptr size, const void *data, GLenum usage)
{
	GLuint name;
	buffer_t *b = bound_buffer (target, &name);
	if (!b)
	{
		return;
	}
	if (size < 0)
	{
		ERROR (GL_INVALID_VALUE);
	}
	if (usage != GL_STREAM_DRAW && usage != GL_STATIC_DRAW && usage != GL_DYNAMIC_DRAW)
	{
		ERROR (GL_INVALID_ENUM);
	}

	free (b->shadow);
	b->shadow = NULL;
	if (target == GL_ELEMENT_ARRAY_BUFFER || PGL_SHADOW_LIMIT == 0 || size <= PGL_SHADOW_LIMIT)
	{
		/* for draws that combine client-side vertex arrays with an index
		   buffer: the vertex range is taken from the indices (a buffer
		   uploaded as vertex data may be used as index buffer later) */
		b->shadow = calloc (1, size ? size : 1);
		if (!b->shadow)
		{
			ERROR (GL_OUT_OF_MEMORY);
		}
		if (data)
		{
			memcpy (b->shadow, data, size);
		}
	}
	b->size = size;
	b->usage = usage;
	pgpu_buffer_create (name, size);
	if (data && size)
	{
		pgpu_buffer_data (name, 0, data, size);
	}
}

void glBufferSubData (GLenum target, GLintptr offset, GLsizeiptr size, const void *data)
{
	GLuint name;
	buffer_t *b = bound_buffer (target, &name);
	if (!b)
	{
		return;
	}
	if (offset < 0 || size < 0 || offset + size > b->size)
	{
		ERROR (GL_INVALID_VALUE);
	}
	if (b->shadow)
	{
		memcpy (b->shadow + offset, data, size);
	}
	if (size)
	{
		pgpu_buffer_data (name, offset, data, size);
	}
}

void glDeleteBuffers (GLsizei n, const GLuint *buffers)
{
	if (n < 0)
	{
		ERROR (GL_INVALID_VALUE);
	}
	for (GLsizei i = 0; i < n; i++)
	{
		GLuint name = slot_of (NAMES_BUFFER, buffers[i]);
		if (!name || !S.buffers[name].used)
		{
			continue;
		}
		if (S.array_buffer == name)
		{
			S.array_buffer = 0;
		}
		if (S.element_buffer == name)
		{
			S.element_buffer = 0;
		}
		for (int a = 0; a < ATTRIBS; a++)
		{
			if (S.attribs[a].buffer == name)
			{
				S.attribs[a].buffer = 0;
			}
		}
		for (int a = 0; a < 4; a++)
		{
			if (S.ff_arrays[a].buffer == name)
			{
				S.ff_arrays[a].buffer = 0;
			}
		}
		if (S.buffers[name].size || S.buffers[name].usage)
		{
			pgpu_buffer_delete (name);
		}
		free (S.buffers[name].shadow);
		memset (&S.buffers[name], 0, sizeof S.buffers[name]);
		free_name (NAMES_BUFFER, name);
		for (int a = 0; a < ATTRIBS; a++)
		{
			if (S.sent_attribs[a].buffer == name)
			{
				S.sent_attribs[a].valid = false;
			}
		}
		for (int a = 0; a < 4; a++)
		{
			if (S.sent_ff[a].buffer == name)
			{
				S.sent_ff[a].valid = false;
			}
		}
	}
}

void glGetBufferParameteriv (GLenum target, GLenum pname, GLint *params)
{
	GLuint name;
	buffer_t *b = bound_buffer (target, &name);
	if (!b)
	{
		return;
	}
	switch (pname)
	{
	case GL_BUFFER_SIZE:	*params = b->size;			break;
	case GL_BUFFER_USAGE:	*params = b->usage ? b->usage : GL_STATIC_DRAW;	break;
	default:		ERROR (GL_INVALID_ENUM);
	}
}

/* ---- textures ------------------------------------------------------------------------------ */


static void texture_take (unsigned n)
{
	texture_t *t = &S.textures[n];
	memset (t, 0, sizeof *t);
	t->used = true;
	t->min_filter = GL_NEAREST_MIPMAP_LINEAR;
	t->mag_filter = GL_LINEAR;
	t->wrap_s = t->wrap_t = GL_REPEAT;
}

void glGenTextures (GLsizei n, GLuint *textures)
{
	gen_names (n, textures, NAMES_TEXTURE, texture_take);
}

GLboolean glIsTexture (GLuint texture)
{
	texture = slot_of (NAMES_TEXTURE, texture);
	return texture && S.textures[texture].used
	       && S.textures[texture].target != TEX_NONE;
}

void glActiveTexture (GLenum texture)
{
	if (texture < GL_TEXTURE0 || texture >= GL_TEXTURE0 + UNITS)
	{
		ERROR (GL_INVALID_ENUM);
	}
	S.active_unit = texture - GL_TEXTURE0;
}

void glBindTexture (GLenum target, GLuint texture)
{
	uint8_t tt = target == GL_TEXTURE_2D ? TEX_2D : target == GL_TEXTURE_CUBE_MAP ? TEX_CUBE : TEX_NONE;
	if (tt == TEX_NONE)
	{
		ERROR (GL_INVALID_ENUM);
	}
	if (texture)
	{
		unsigned slot = slot_for (NAMES_TEXTURE, texture);
		if (!slot)
		{
			ERROR (GL_OUT_OF_MEMORY);	/* pgl's table is full */
		}
		texture = slot;
		texture_t *t = &S.textures[texture];
		if (!t->used)
		{
			texture_take (texture);
		}
		if (t->target != TEX_NONE && t->target != tt)
		{
			ERROR (GL_INVALID_OPERATION);
		}
		t->target = tt;
	}
	if (tt == TEX_2D)
	{
		S.bound_2d[S.active_unit] = texture;
	}
	else
	{
		S.bound_cube[S.active_unit] = texture;
	}
}

static void send_texture_params (GLuint name)
{
	const texture_t *t = &S.textures[name];
	pgpu_texture_params (name, map_filter (t->min_filter), map_filter (t->mag_filter),
			     map_wrap (t->wrap_s), map_wrap (t->wrap_t));
}

/* the texture a target of the active unit names; face for cube faces */
static texture_t *texture_for_target (GLenum target, GLuint *name, unsigned *face, bool faces)
{
	uint8_t tt;
	if (target == GL_TEXTURE_2D)
	{
		tt = TEX_2D;
		*face = 0;
	}
	else if (faces && target >= GL_TEXTURE_CUBE_MAP_POSITIVE_X && target <= GL_TEXTURE_CUBE_MAP_NEGATIVE_Z)
	{
		tt = TEX_CUBE;
		*face = target - GL_TEXTURE_CUBE_MAP_POSITIVE_X;
	}
	else if (!faces && target == GL_TEXTURE_CUBE_MAP)
	{
		tt = TEX_CUBE;
		*face = 0;
	}
	else
	{
		ERROR_RET (GL_INVALID_ENUM, NULL);
	}
	*name = tt == TEX_2D ? S.bound_2d[S.active_unit] : S.bound_cube[S.active_unit];
	if (!*name)
	{
		*name = tt == TEX_2D ? TEX_DEFAULT_2D : TEX_DEFAULT_CUBE;	/* texture object 0 */
	}
	return &S.textures[*name];
}

/* before a Zero texture is re-created or deleted: if it is the target of the
   framebuffer bound on the Zero, bind the panel (which renders that job),
   and have framebuffers on it re-created before their next use */
static void release_render_target (uint32_t hw)
{
	for (unsigned i = 1; i <= MAX_FRAMEBUFFER_NAMES; i++)
	{
		framebuffer_t *fb = &S.framebuffers[i];
		if (fb->created && (fb->sent_texture & 0xFFFF) == hw)
		{
			if (S.sent_framebuffer == i)
			{
				pgpu_bind_framebuffer (0);
				S.sent_framebuffer = 0;
			}
			fb->sent_generation = ~0u;
		}
	}
}

/* define level `level` with this size and format: create the Zero texture if
   needed; false if the level can't be stored (inconsistent with level 0:
   the texture is incomplete in GL, and pgl ignores the level) */
static bool define_level (GLuint name, texture_t *t, unsigned face, GLint level, GLsizei w, GLsizei h,
			  uint8_t format, GLenum gl_format, GLenum gl_type)
{
	bool cube = t->target == TEX_CUBE;
	unsigned w0 = (unsigned) w << level, h0 = (unsigned) h << level;
	if (level == 0 || !t->storage)
	{
		if (level > 0 && (w0 > MAX_SIZE || h0 > MAX_SIZE))
		{
			return false;
		}
		if (!t->storage || t->width != w0 || t->height != h0 || t->format != format)
		{
			release_render_target (name);
			pgpu_texture_create (name, w0, h0, format | (cube ? PGPU_TEXTURE_CUBE : 0));
			t->storage = true;
			t->width = w0;
			t->height = h0;
			t->format = format;
			t->gl_format = gl_format;
			t->gl_type = gl_type;
			t->generation++;
			t->bad_levels = 0;
			t->faces = 0;			/* the other faces are gone */
			send_texture_params (name);
		}
		if (level == 0)
		{
			t->empty = false;
			t->faces |= 1u << face;
		}
		return true;
	}

	/* a mipmap level: the Zero has them for power-of-two sizes (a mipmapped
	   non-power-of-two texture is incomplete in GL ES 2.0 anyway) */
	if (!is_pot (t->width) || !is_pot (t->height))
	{
		return false;
	}
	unsigned lw = t->width >> level, lh = t->height >> level;
	bool in_chain = lw || lh;
	bool fits = format == t->format && (lw ? lw : 1) == (unsigned) w && (lh ? lh : 1) == (unsigned) h
		    && in_chain;
	if (in_chain)
	{
		/* a level that doesn't fit makes the texture incomplete (GL ES 2.0
		   3.7.10) until it is specified again */
		t->bad_levels = fits ? t->bad_levels & ~(1u << level) : t->bad_levels | 1u << level;
	}
	return fits;
}

/* a level of zero size (0 x n) */
static void empty_level (texture_t *t, unsigned face, GLint level)
{
	if (level == 0)
	{
		t->empty = true;
		t->faces &= ~(1u << face);
	}
	else if (t->storage && is_pot (t->width) && is_pot (t->height)
		 && (t->width >> level || t->height >> level))
	{
		t->bad_levels |= 1u << level;
	}
}

/* complete as far as pgl knows (the Zero checks the rest: defined levels,
   NPOT rules) */
static bool texture_usable (const texture_t *t)
{
	bool mipmap = t->min_filter != GL_NEAREST && t->min_filter != GL_LINEAR;
	return    t->storage && !t->empty && !(mipmap && t->bad_levels)
	       && t->faces == (t->target == TEX_CUBE ? 0x3F : 1);	/* cube complete (3.7.10) */
}

static bool check_level_size (GLenum target, GLint level, GLsizei w, GLsizei h, GLint border)
{
	if (level < 0 || level >= MAX_LEVELS || w < 0 || h < 0 || border != 0
	    || w > (MAX_SIZE >> level) || h > (MAX_SIZE >> level)
	    || (level > 0 && (!is_pot (w) || !is_pot (h))))		/* GL ES 2.0 3.7.1 */
	{
		ERROR_RET (GL_INVALID_VALUE, false);
	}
	if (target != GL_TEXTURE_2D && w != h)
	{
		ERROR_RET (GL_INVALID_VALUE, false);
	}
	return true;
}

/* texels of the formats with several GL types (RGBA: 8888, 4444, 5551; RGB:
   888, 565) to and from 8-bit RGBA, as the Zero converts them */
static void unpack_texel (int format, const uint8_t *p, uint8_t rgba[4])
{
	uint32_t v = p[0] | (format_bytes[format] > 1 ? p[1] << 8 : 0);
	switch (format)
	{
	case PGPU_RGBA8888:	memcpy (rgba, p, 4);						break;
	case PGPU_RGB888:	memcpy (rgba, p, 3); rgba[3] = 255;				break;
	case PGPU_RGBA4444:
		for (int c = 0; c < 4; c++)
		{
			rgba[c] = ((v >> (12 - 4 * c)) & 15) * 17;
		}
		break;
	case PGPU_RGBA5551:
		for (int c = 0; c < 3; c++)
		{
			uint32_t x = (v >> (11 - 5 * c)) & 31;
			rgba[c] = x << 3 | x >> 2;
		}
		rgba[3] = v & 1 ? 255 : 0;
		break;
	case PGPU_RGB565: {
		uint32_t r = v >> 11, g = (v >> 5) & 63, b = v & 31;
		rgba[0] = r << 3 | r >> 2;
		rgba[1] = g << 2 | g >> 4;
		rgba[2] = b << 3 | b >> 2;
		rgba[3] = 255;
		} break;
	}
}

static uint32_t to_bits (uint8_t v, unsigned bits)
{
	return (v * ((1u << bits) - 1) + 127) / 255;
}

static void pack_texel (int format, const uint8_t rgba[4], uint8_t *p)
{
	uint32_t v = 0;
	switch (format)
	{
	case PGPU_RGBA8888:	memcpy (p, rgba, 4);	return;
	case PGPU_RGB888:	memcpy (p, rgba, 3);	return;
	case PGPU_RGBA4444:
		v = to_bits (rgba[0], 4) << 12 | to_bits (rgba[1], 4) << 8 | to_bits (rgba[2], 4) << 4
		    | to_bits (rgba[3], 4);
		break;
	case PGPU_RGBA5551:
		v = to_bits (rgba[0], 5) << 11 | to_bits (rgba[1], 5) << 6 | to_bits (rgba[2], 5) << 1
		    | (rgba[3] >= 128);
		break;
	case PGPU_RGB565:
		v = to_bits (rgba[0], 5) << 11 | to_bits (rgba[1], 6) << 5 | to_bits (rgba[2], 5);
		break;
	}
	p[0] = v;
	p[1] = v >> 8;
}

static uint32_t unpack_stride (unsigned row_bytes)
{
	unsigned a = S.unpack_alignment;
	return (row_bytes + a - 1) / a * a;
}

/* texel conversions for TEXTURE_DATA, rows at a time (a row is at most 8 KB:
   MAX_SIZE RGBA texels) */
static uint8_t convert_rows[8192];

/* GL_BGRA_EXT pixels (EXT_texture_format_BGRA8888): the Zero stores RGBA, so
   blue and red are swapped on the way */
static void texture_data_bgra (GLuint name, GLint level, unsigned face, GLint x, GLint y,
			       GLsizei width, GLsizei height, const void *pixels)
{
	uint32_t src_stride = unpack_stride (width * 4), row = width * 4;
	uint32_t rows = sizeof convert_rows / row;
	for (GLsizei y0 = 0; y0 < height; y0 += rows)
	{
		uint32_t n = height - y0 < (GLsizei) rows ? height - y0 : rows;
		for (uint32_t r = 0; r < n; r++)
		{
			const uint8_t *src = (const uint8_t *) pixels + (y0 + r) * src_stride;
			uint8_t *dst = convert_rows + r * row;
			for (GLsizei i = 0; i < width; i++, src += 4, dst += 4)
			{
				dst[0] = src[2];
				dst[1] = src[1];
				dst[2] = src[0];
				dst[3] = src[3];
			}
		}
		pgpu_texture_data_stride (name, level, face, x, y + y0, width, n, PGPU_RGBA8888,
					  convert_rows, row);
	}
}

static void auto_mipmap (GLuint name, texture_t *t, GLint level)
{
	if (level == 0 && t->generate_mipmap && is_pot (t->width) && is_pot (t->height))
	{
		pgpu_generate_mipmap (name);
	}
}

void glTexImage2D (GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height,
		   GLint border, GLenum format, GLenum type, const void *pixels)
{
	GLuint name;
	unsigned face;
	texture_t *t = texture_for_target (target, &name, &face, true);
	if (!t)
	{
		return;
	}
	int f = map_texture_format (format, type);
	if (f == -1)
	{
		ERROR (GL_INVALID_ENUM);
	}
	if (internalformat == GL_BGRA8_EXT)
	{
		internalformat = GL_BGRA_EXT;	/* EXT_texture_format_BGRA8888 allows it sized too */
	}
	if (map_texture_format (internalformat, GL_UNSIGNED_BYTE) < 0)
	{
		ERROR (GL_INVALID_VALUE);
	}
	if (!check_level_size (target, level, width, height, border))
	{
		return;
	}
	if (f == -2 || (GLenum) internalformat != format)
	{
		ERROR (GL_INVALID_OPERATION);
	}
	if (width == 0 || height == 0)
	{
		empty_level (t, face, level);	/* the texture is incomplete */
		return;
	}
	if (!define_level (name, t, face, level, width, height, f, format, type))
	{
		return;
	}
	if (pixels && format == GL_BGRA_EXT)
	{
		texture_data_bgra (name, level, face, 0, 0, width, height, pixels);
		auto_mipmap (name, t, level);
	}
	else if (pixels)
	{
		pgpu_texture_data_stride (name, level, face, 0, 0, width, height, f, pixels,
					  unpack_stride (width * format_bytes[f]));
		auto_mipmap (name, t, level);
	}
}

void glTexSubImage2D (GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width,
		      GLsizei height, GLenum format, GLenum type, const void *pixels)
{
	GLuint name;
	unsigned face;
	texture_t *t = texture_for_target (target, &name, &face, true);
	if (!t)
	{
		return;
	}
	int f = map_texture_format (format, type);
	if (f == -1)
	{
		ERROR (GL_INVALID_ENUM);
	}
	if (level < 0 || level >= MAX_LEVELS || xoffset < 0 || yoffset < 0 || width < 0 || height < 0)
	{
		ERROR (GL_INVALID_VALUE);
	}
	if (!t->storage || t->gl_format == GL_ETC1_RGB8_OES)
	{
		ERROR (GL_INVALID_OPERATION);
	}
	unsigned lw = t->width >> level, lh = t->height >> level;
	lw = lw ? lw : 1;
	lh = lh ? lh : 1;
	if ((unsigned) (xoffset + width) > lw || (unsigned) (yoffset + height) > lh)
	{
		ERROR (GL_INVALID_VALUE);
	}
	/* the format must be the texture's (GL ES 2.0 3.7.2); the type may differ */
	if (f == -2 || format != t->gl_format)
	{
		ERROR (GL_INVALID_OPERATION);
	}
	if (width == 0 || height == 0 || !pixels)
	{
		return;
	}
	uint32_t src_stride = unpack_stride (width * format_bytes[f]);
	if (format == GL_BGRA_EXT)
	{
		texture_data_bgra (name, level, face, xoffset, yoffset, width, height, pixels);
	}
	else if (type == t->gl_type)
	{
		pgpu_texture_data_stride (name, level, face, xoffset, yoffset, width, height, f, pixels,
					  src_stride);
	}
	else
	{
		/* TEXTURE_DATA is in the texture's format: convert, rows at a time */
		uint32_t dst_bpp = format_bytes[t->format], src_bpp = format_bytes[f];
		uint32_t dst_row = (width * dst_bpp + 3) & ~3u;
		uint32_t rows = dst_row <= sizeof convert_rows ? sizeof convert_rows / dst_row : 0;
		uint32_t span = rows ? width : sizeof convert_rows / dst_bpp;
		rows = rows ? rows : 1;
		for (GLsizei y = 0; y < height; y += rows)
		{
			uint32_t n = height - y < (GLsizei) rows ? height - y : rows;
			for (GLsizei x = 0; x < width; x += span)
			{
				uint32_t w = width - x < (GLsizei) span ? width - x : span;
				uint32_t stride = (w * dst_bpp + 3) & ~3u;
				for (uint32_t r = 0; r < n; r++)
				{
					const uint8_t *src = (const uint8_t *) pixels + (y + r) * src_stride + x * src_bpp;
					for (uint32_t i = 0; i < w; i++)
					{
						uint8_t rgba[4];
						unpack_texel (f, src + i * src_bpp, rgba);
						pack_texel (t->format, rgba, convert_rows + r * stride + i * dst_bpp);
					}
				}
				pgpu_texture_data_stride (name, level, face, xoffset + x, yoffset + y, w, n,
							  t->format, convert_rows, stride);
			}
		}
	}
	auto_mipmap (name, t, level);
}

void glCompressedTexImage2D (GLenum target, GLint level, GLenum internalformat, GLsizei width,
			     GLsizei height, GLint border, GLsizei imageSize, const void *data)
{
	GLuint name;
	unsigned face;
	texture_t *t = texture_for_target (target, &name, &face, true);
	if (!t)
	{
		return;
	}
	if (internalformat != GL_ETC1_RGB8_OES)
	{
		ERROR (GL_INVALID_ENUM);
	}
	if (!check_level_size (target, level, width, height, border))
	{
		return;
	}
	if (imageSize != (width + 3) / 4 * ((height + 3) / 4) * 8)
	{
		ERROR (GL_INVALID_VALUE);
	}
	if (width == 0 || height == 0)
	{
		empty_level (t, face, level);
		return;
	}
	if (!define_level (name, t, face, level, width, height, PGPU_ETC1, GL_ETC1_RGB8_OES, 0))
	{
		return;
	}
	if (data)
	{
		pgpu_texture_data_level (name, level, face, 0, 0, width, height, PGPU_ETC1, data);
		auto_mipmap (name, t, level);
	}
}

void glCompressedTexSubImage2D (GLenum target, GLint level, GLint xoffset, GLint yoffset,
				GLsizei width, GLsizei height, GLenum format, GLsizei imageSize,
				const void *data)
{
	/* OES_compressed_ETC1_RGB8_texture: no sub-images */
	if (format != GL_ETC1_RGB8_OES)
	{
		ERROR (GL_INVALID_ENUM);
	}
	ERROR (GL_INVALID_OPERATION);
}

static bool copy_format_ok (GLenum internalformat)
{
	if (!target_has_color ())
	{
		return false;
	}
	bool alpha = target_has_alpha ();
	switch (internalformat)
	{
	case GL_RGB: case GL_LUMINANCE:			return true;
	case GL_RGBA: case GL_ALPHA: case GL_LUMINANCE_ALPHA:	return alpha;
	default:					return false;
	}
}

void glCopyTexImage2D (GLenum target, GLint level, GLenum internalformat, GLint x, GLint y,
		       GLsizei width, GLsizei height, GLint border)
{
	GLuint name;
	unsigned face;
	texture_t *t = texture_for_target (target, &name, &face, true);
	if (!t)
	{
		return;
	}
	int f = map_texture_format (internalformat, GL_UNSIGNED_BYTE);
	if (f < 0 || internalformat == GL_BGRA_EXT)	/* BGRA: TexImage2D, TexSubImage2D only */
	{
		ERROR (GL_INVALID_ENUM);
	}
	if (!check_level_size (target, level, width, height, border))
	{
		return;
	}
	if (!validate_framebuffer ())
	{
		return;
	}
	if (!copy_format_ok (internalformat))
	{
		ERROR (GL_INVALID_OPERATION);
	}
	if (width == 0 || height == 0)
	{
		empty_level (t, face, level);
		return;
	}
	if (!define_level (name, t, face, level, width, height, f, internalformat, GL_UNSIGNED_BYTE))
	{
		return;
	}
	pgpu_copy_tex_image (name, level, face, 0, 0, x, y, width, height);
	auto_mipmap (name, t, level);
}

void glCopyTexSubImage2D (GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint x,
			  GLint y, GLsizei width, GLsizei height)
{
	GLuint name;
	unsigned face;
	texture_t *t = texture_for_target (target, &name, &face, true);
	if (!t)
	{
		return;
	}
	if (level < 0 || level >= MAX_LEVELS || xoffset < 0 || yoffset < 0 || width < 0 || height < 0)
	{
		ERROR (GL_INVALID_VALUE);
	}
	if (!t->storage || t->gl_format == GL_ETC1_RGB8_OES)
	{
		ERROR (GL_INVALID_OPERATION);
	}
	unsigned lw = t->width >> level, lh = t->height >> level;
	if ((unsigned) (xoffset + width) > (lw ? lw : 1) || (unsigned) (yoffset + height) > (lh ? lh : 1))
	{
		ERROR (GL_INVALID_VALUE);
	}
	if (!validate_framebuffer ())
	{
		return;
	}
	if (!copy_format_ok (t->gl_format))
	{
		ERROR (GL_INVALID_OPERATION);
	}
	if (width && height)
	{
		pgpu_copy_tex_image (name, level, face, xoffset, yoffset, x, y, width, height);
		auto_mipmap (name, t, level);
	}
}

void glGenerateMipmap (GLenum target)
{
	GLuint name;
	unsigned face;
	texture_t *t = texture_for_target (target, &name, &face, false);
	if (!t)
	{
		return;
	}
	if (   !t->storage || !is_pot (t->width) || !is_pot (t->height) || t->gl_format == GL_ETC1_RGB8_OES
	    || (t->target == TEX_CUBE && t->faces != 0x3F))		/* cube complete (3.7.11) */
	{
		ERROR (GL_INVALID_OPERATION);
	}
	pgpu_generate_mipmap (name);
}

static void tex_parameter (GLenum target, GLenum pname, GLint param)
{
	GLuint name;
	unsigned face;
	texture_t *t = texture_for_target (target, &name, &face, false);
	if (!t)
	{
		return;
	}
	switch (pname)
	{
	case GL_TEXTURE_MIN_FILTER:
		if (map_filter (param) < 0)
		{
			ERROR (GL_INVALID_ENUM);
		}
		t->min_filter = param;
		break;
	case GL_TEXTURE_MAG_FILTER:
		if (param != GL_NEAREST && param != GL_LINEAR)
		{
			ERROR (GL_INVALID_ENUM);
		}
		t->mag_filter = param;
		break;
	case GL_TEXTURE_WRAP_S:
	case GL_TEXTURE_WRAP_T:
		if (map_wrap (param) < 0)
		{
			ERROR (GL_INVALID_ENUM);
		}
		*(pname == GL_TEXTURE_WRAP_S ? &t->wrap_s : &t->wrap_t) = param;
		break;
	case GL_GENERATE_MIPMAP:
		t->generate_mipmap = param != 0;
		return;
	default:
		ERROR (GL_INVALID_ENUM);
	}
	if (t->storage)
	{
		send_texture_params (name);
	}
}

void glTexParameteri (GLenum target, GLenum pname, GLint param)		{ tex_parameter (target, pname, param); }
void glTexParameterf (GLenum target, GLenum pname, GLfloat param)	{ tex_parameter (target, pname, (GLint) param); }
void glTexParameteriv (GLenum target, GLenum pname, const GLint *params)	{ tex_parameter (target, pname, params[0]); }
void glTexParameterfv (GLenum target, GLenum pname, const GLfloat *params)	{ tex_parameter (target, pname, (GLint) params[0]); }

void glGetTexParameteriv (GLenum target, GLenum pname, GLint *params)
{
	GLuint name;
	unsigned face;
	texture_t *t = texture_for_target (target, &name, &face, false);
	if (!t)
	{
		return;
	}
	switch (pname)
	{
	case GL_TEXTURE_MIN_FILTER:	*params = t->min_filter;	break;
	case GL_TEXTURE_MAG_FILTER:	*params = t->mag_filter;	break;
	case GL_TEXTURE_WRAP_S:		*params = t->wrap_s;		break;
	case GL_TEXTURE_WRAP_T:		*params = t->wrap_t;		break;
	case GL_GENERATE_MIPMAP:	*params = t->generate_mipmap;	break;
	default:			ERROR (GL_INVALID_ENUM);
	}
}

void glGetTexParameterfv (GLenum target, GLenum pname, GLfloat *params)
{
	GLint v = 0;
	uint32_t errors = S.errors;
	S.errors = 0;
	glGetTexParameteriv (target, pname, &v);
	if (!S.errors)
	{
		*params = (GLfloat) v;
	}
	S.errors |= errors;
}

/* deleted textures and renderbuffers live on while a framebuffer (not the
   bound one, which lets go at once) has them attached (GL ES 2.0 4.4.3) */
static unsigned attachment_refs (GLenum type, unsigned slot)
{
	unsigned n = 0;
	for (unsigned f = 1; f <= MAX_FRAMEBUFFER_NAMES; f++)
	{
		for (int i = 0; S.framebuffers[f].used && i < ATTACHMENTS; i++)
		{
			n += S.framebuffers[f].att[i].type == type && S.framebuffers[f].att[i].name == slot;
		}
	}
	return n;
}

static void reap_orphans (void)
{
	for (unsigned t = 1; t <= MAX_TEXTURE_NAMES; t++)
	{
		if (S.textures[t].orphan && !attachment_refs (GL_TEXTURE, t))
		{
			if (S.textures[t].storage)
			{
				release_render_target (t);
				pgpu_texture_delete (t);
			}
			memset (&S.textures[t], 0, sizeof S.textures[t]);
		}
	}
	for (unsigned r = 1; r <= MAX_RENDERBUFFER_NAMES; r++)
	{
		if (S.renderbuffers[r].orphan && !attachment_refs (GL_RENDERBUFFER, r))
		{
			if (rb_is_color (S.renderbuffers[r].format))
			{
				release_render_target (HW_RENDERBUFFER_BASE + r);
				pgpu_texture_delete (HW_RENDERBUFFER_BASE + r);
			}
			memset (&S.renderbuffers[r], 0, sizeof S.renderbuffers[r]);
		}
	}
}

static void detach_from_bound_framebuffer (GLenum type, GLuint name)
{
	if (!S.framebuffer)
	{
		return;
	}
	framebuffer_t *fb = &S.framebuffers[S.framebuffer];
	for (int i = 0; i < ATTACHMENTS; i++)
	{
		if (fb->att[i].type == type && fb->att[i].name == name)
		{
			memset (&fb->att[i], 0, sizeof fb->att[i]);
		}
	}
}

void glDeleteTextures (GLsizei n, const GLuint *textures)
{
	if (n < 0)
	{
		ERROR (GL_INVALID_VALUE);
	}
	for (GLsizei i = 0; i < n; i++)
	{
		GLuint name = slot_of (NAMES_TEXTURE, textures[i]);
		if (!name || !S.textures[name].used)
		{
			continue;
		}
		for (int u = 0; u < UNITS; u++)
		{
			if (S.bound_2d[u] == name)
			{
				S.bound_2d[u] = 0;
			}
			if (S.bound_cube[u] == name)
			{
				S.bound_cube[u] = 0;
			}
			if (S.sent_units[u] == name)
			{
				S.sent_units_valid[u] = false;
			}
		}
		detach_from_bound_framebuffer (GL_TEXTURE, name);
		S.textures[name].orphan_name = textures[i];
		free_name (NAMES_TEXTURE, name);
		S.textures[name].orphan = true;		/* freed when no framebuffer has it */
	}
	reap_orphans ();
}

/* ---- renderbuffers and framebuffers ------------------------------------------------------------- */

static void renderbuffer_take (unsigned n)	{ S.renderbuffers[n].used = true; }
static void framebuffer_take (unsigned n)	{ S.framebuffers[n].used = true; }

void glGenRenderbuffers (GLsizei n, GLuint *renderbuffers)
{
	gen_names (n, renderbuffers, NAMES_RENDERBUFFER, renderbuffer_take);
}

void glGenFramebuffers (GLsizei n, GLuint *framebuffers)
{
	gen_names (n, framebuffers, NAMES_FRAMEBUFFER, framebuffer_take);
}

GLboolean glIsRenderbuffer (GLuint rb)
{
	rb = slot_of (NAMES_RENDERBUFFER, rb);
	return rb && S.renderbuffers[rb].bound;
}

GLboolean glIsFramebuffer (GLuint fb)
{
	fb = slot_of (NAMES_FRAMEBUFFER, fb);
	return fb && S.framebuffers[fb].bound;
}

void glBindRenderbuffer (GLenum target, GLuint renderbuffer)
{
	if (target != GL_RENDERBUFFER)
	{
		ERROR (GL_INVALID_ENUM);
	}
	if (renderbuffer)
	{
		unsigned slot = slot_for (NAMES_RENDERBUFFER, renderbuffer);
		if (!slot)
		{
			ERROR (GL_OUT_OF_MEMORY);	/* pgl's table is full */
		}
		renderbuffer = slot;
		S.renderbuffers[renderbuffer].used = S.renderbuffers[renderbuffer].bound = true;
	}
	S.renderbuffer = renderbuffer;
}

void glBindFramebuffer (GLenum target, GLuint framebuffer)
{
	if (target != GL_FRAMEBUFFER)
	{
		ERROR (GL_INVALID_ENUM);
	}
	if (framebuffer)
	{
		unsigned slot = slot_for (NAMES_FRAMEBUFFER, framebuffer);
		if (!slot)
		{
			ERROR (GL_OUT_OF_MEMORY);	/* pgl's table is full */
		}
		framebuffer = slot;
		S.framebuffers[framebuffer].used = S.framebuffers[framebuffer].bound = true;
	}
	S.framebuffer = framebuffer;
}

void glRenderbufferStorage (GLenum target, GLenum internalformat, GLsizei width, GLsizei height)
{
	if (target != GL_RENDERBUFFER)
	{
		ERROR (GL_INVALID_ENUM);
	}
	if (!rb_is_color (internalformat) && !rb_has_depth (internalformat) && !rb_has_stencil (internalformat))
	{
		ERROR (GL_INVALID_ENUM);
	}
	if (width < 0 || height < 0 || width > MAX_SIZE || height > MAX_SIZE)
	{
		ERROR (GL_INVALID_VALUE);
	}
	if (!S.renderbuffer)
	{
		ERROR (GL_INVALID_OPERATION);
	}
	renderbuffer_t *rb = &S.renderbuffers[S.renderbuffer];
	uint32_t hw = HW_RENDERBUFFER_BASE + S.renderbuffer;
	release_render_target (hw);
	if (rb_is_color (rb->format) && !rb_is_color (internalformat))
	{
		pgpu_texture_delete (hw);
	}
	rb->format = internalformat;
	rb->width = width;
	rb->height = height;
	rb->generation++;
	if (rb_is_color (internalformat) && width && height)
	{
		/* colour storage is a texture on the Zero; without alpha, it reads 1 */
		pgpu_texture_create (hw, width, height,
				       internalformat == GL_RGB565 ? PGPU_RGB565
				     : internalformat == GL_RGB8_OES ? PGPU_RGB888 : PGPU_RGBA8888);
		pgpu_texture_params (hw, PGPU_NEAREST, PGPU_NEAREST, PGPU_CLAMP_TO_EDGE, PGPU_CLAMP_TO_EDGE);
	}
}

void glDeleteRenderbuffers (GLsizei n, const GLuint *renderbuffers)
{
	if (n < 0)
	{
		ERROR (GL_INVALID_VALUE);
	}
	for (GLsizei i = 0; i < n; i++)
	{
		GLuint name = slot_of (NAMES_RENDERBUFFER, renderbuffers[i]);
		if (!name || !S.renderbuffers[name].used)
		{
			continue;
		}
		if (S.renderbuffer == name)
		{
			S.renderbuffer = 0;
		}
		detach_from_bound_framebuffer (GL_RENDERBUFFER, name);
		S.renderbuffers[name].orphan_name = renderbuffers[i];
		free_name (NAMES_RENDERBUFFER, name);
		S.renderbuffers[name].orphan = true;	/* freed when no framebuffer has it */
	}
	reap_orphans ();
}

void glDeleteFramebuffers (GLsizei n, const GLuint *framebuffers)
{
	if (n < 0)
	{
		ERROR (GL_INVALID_VALUE);
	}
	for (GLsizei i = 0; i < n; i++)
	{
		GLuint name = slot_of (NAMES_FRAMEBUFFER, framebuffers[i]);
		if (!name || !S.framebuffers[name].used)
		{
			continue;
		}
		if (S.framebuffer == name)
		{
			S.framebuffer = 0;
		}
		free_scratch (&S.framebuffers[name]);
		if (S.framebuffers[name].created)
		{
			pgpu_framebuffer_delete (name);	/* binds the panel if it was bound */
			if (S.sent_framebuffer == name)
			{
				S.sent_framebuffer = 0;
			}
		}
		memset (&S.framebuffers[name], 0, sizeof S.framebuffers[name]);
		free_name (NAMES_FRAMEBUFFER, name);
	}
	reap_orphans ();
}

static attachment_t *attachment_point (GLenum target, GLenum attachment)
{
	if (target != GL_FRAMEBUFFER)
	{
		ERROR_RET (GL_INVALID_ENUM, NULL);
	}
	int i;
	switch (attachment)
	{
	case GL_COLOR_ATTACHMENT0:	i = ATT_COLOR;		break;
	case GL_DEPTH_ATTACHMENT:	i = ATT_DEPTH;		break;
	case GL_STENCIL_ATTACHMENT:	i = ATT_STENCIL;	break;
	default:			ERROR_RET (GL_INVALID_ENUM, NULL);
	}
	if (!S.framebuffer)
	{
		ERROR_RET (GL_INVALID_OPERATION, NULL);
	}
	return &S.framebuffers[S.framebuffer].att[i];
}

void glFramebufferTexture2D (GLenum target, GLenum attachment, GLenum textarget, GLuint texture,
			     GLint level)
{
	attachment_t *a = attachment_point (target, attachment);
	if (!a)
	{
		return;
	}
	if (!texture)
	{
		memset (a, 0, sizeof *a);
		reap_orphans ();
		return;
	}
	unsigned face = 0;
	uint8_t tt;
	if (textarget == GL_TEXTURE_2D)
	{
		tt = TEX_2D;
	}
	else if (textarget >= GL_TEXTURE_CUBE_MAP_POSITIVE_X && textarget <= GL_TEXTURE_CUBE_MAP_NEGATIVE_Z)
	{
		tt = TEX_CUBE;
		face = textarget - GL_TEXTURE_CUBE_MAP_POSITIVE_X;
	}
	else
	{
		ERROR (GL_INVALID_ENUM);
	}
	texture = slot_of (NAMES_TEXTURE, texture);
	if (!texture || !S.textures[texture].used || S.textures[texture].target != tt)
	{
		ERROR (GL_INVALID_OPERATION);
	}
	if (level != 0)
	{
		ERROR (GL_INVALID_VALUE);	/* ES 2.0: level must be 0 */
	}
	a->type = GL_TEXTURE;
	a->name = texture;
	a->level = level;
	a->face = face;
	reap_orphans ();
}

void glFramebufferRenderbuffer (GLenum target, GLenum attachment, GLenum renderbuffertarget,
				GLuint renderbuffer)
{
	attachment_t *a = attachment_point (target, attachment);
	if (!a)
	{
		return;
	}
	if (renderbuffertarget != GL_RENDERBUFFER)
	{
		ERROR (GL_INVALID_ENUM);
	}
	if (!renderbuffer)
	{
		memset (a, 0, sizeof *a);
		reap_orphans ();
		return;
	}
	renderbuffer = slot_of (NAMES_RENDERBUFFER, renderbuffer);
	if (!renderbuffer || !S.renderbuffers[renderbuffer].bound)
	{
		ERROR (GL_INVALID_OPERATION);	/* no such object (a name is not one) */
	}
	a->type = GL_RENDERBUFFER;
	a->name = renderbuffer;
	a->level = 0;
	a->face = 0;
	reap_orphans ();
}

GLenum glCheckFramebufferStatus (GLenum target)
{
	if (target != GL_FRAMEBUFFER)
	{
		ERROR_RET (GL_INVALID_ENUM, 0);
	}
	if (!S.framebuffer)
	{
		return GL_FRAMEBUFFER_COMPLETE;
	}
	return framebuffer_status (&S.framebuffers[S.framebuffer], NULL, NULL, NULL, NULL);
}

void glGetFramebufferAttachmentParameteriv (GLenum target, GLenum attachment, GLenum pname, GLint *params)
{
	attachment_t *a = attachment_point (target, attachment);
	if (!a)
	{
		return;
	}
	switch (pname)
	{
	case GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE:
		*params = a->type;
		return;
	case GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME:
		if (a->type == GL_TEXTURE)
		{
			/* a deleted object keeps its name here (the name may be reused) */
			*params = S.textures[a->name].orphan ? S.textures[a->name].orphan_name
							      : name_of (NAMES_TEXTURE, a->name);
			return;
		}
		if (a->type == GL_RENDERBUFFER)
		{
			*params = S.renderbuffers[a->name].orphan ? S.renderbuffers[a->name].orphan_name
								  : name_of (NAMES_RENDERBUFFER, a->name);
			return;
		}
		break;
	case GL_FRAMEBUFFER_ATTACHMENT_TEXTURE_LEVEL:
		if (a->type == GL_TEXTURE)
		{
			*params = a->level;
			return;
		}
		break;
	case GL_FRAMEBUFFER_ATTACHMENT_TEXTURE_CUBE_MAP_FACE:
		if (a->type == GL_TEXTURE)
		{
			*params = S.textures[a->name].target == TEX_CUBE
				  ? GL_TEXTURE_CUBE_MAP_POSITIVE_X + a->face : 0;
			return;
		}
		break;
	}
	ERROR (GL_INVALID_ENUM);
}

void glGetRenderbufferParameteriv (GLenum target, GLenum pname, GLint *params)
{
	if (target != GL_RENDERBUFFER)
	{
		ERROR (GL_INVALID_ENUM);
	}
	if (!S.renderbuffer)
	{
		ERROR (GL_INVALID_OPERATION);
	}
	const renderbuffer_t *rb = &S.renderbuffers[S.renderbuffer];
	bool color = rb_is_color (rb->format);
	bool alpha = color && rb->format != GL_RGB565 && rb->format != GL_RGB8_OES;
	switch (pname)
	{
	case GL_RENDERBUFFER_WIDTH:		*params = rb->width;				break;
	case GL_RENDERBUFFER_HEIGHT:		*params = rb->height;				break;
	case GL_RENDERBUFFER_INTERNAL_FORMAT:	*params = rb->format ? rb->format : GL_RGBA4;	break;
	/* the resolution the Zero stores (8-bit colour, 24-bit depth, 8-bit stencil) */
	case GL_RENDERBUFFER_RED_SIZE:
	case GL_RENDERBUFFER_GREEN_SIZE:
	case GL_RENDERBUFFER_BLUE_SIZE:		*params = color ? 8 : 0;			break;
	case GL_RENDERBUFFER_ALPHA_SIZE:	*params = alpha ? 8 : 0;			break;
	case GL_RENDERBUFFER_DEPTH_SIZE:	*params = rb_has_depth (rb->format) ? 24 : 0;	break;
	case GL_RENDERBUFFER_STENCIL_SIZE:	*params = rb_has_stencil (rb->format) ? 8 : 0;	break;
	default:				ERROR (GL_INVALID_ENUM);
	}
}

/* ---- fragment state -------------------------------------------------------------------------- */

void glClearColor (GLclampf r, GLclampf g, GLclampf b, GLclampf a)
{
	S.clear_color[0] = clampf (r, 0, 1);
	S.clear_color[1] = clampf (g, 0, 1);
	S.clear_color[2] = clampf (b, 0, 1);
	S.clear_color[3] = clampf (a, 0, 1);
}

void glClearDepthf (GLclampf depth)	{ S.clear_depth = clampf (depth, 0, 1); }
void glClearStencil (GLint s)		{ S.clear_stencil = s; }

void glClear (GLbitfield mask)
{
	if (mask & ~(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT))
	{
		ERROR (GL_INVALID_VALUE);
	}
	if (!validate_framebuffer ())
	{
		return;
	}
	validate_caps ();			/* the scissor test applies */

	uint32_t m = 0;
	if (mask & GL_COLOR_BUFFER_BIT)
	{
		m |= PGPU_CLEAR_COLOR;
	}
	if ((mask & GL_DEPTH_BUFFER_BIT) && target_has_depth ())
	{
		m |= PGPU_CLEAR_DEPTH;
	}
	if ((mask & GL_STENCIL_BUFFER_BIT) && target_has_stencil ())
	{
		m |= PGPU_CLEAR_STENCIL;
	}
	if (!m)
	{
		return;
	}
	if (m & PGPU_CLEAR_STENCIL)
	{
		pgpu_clear_stencil (m, color_u32 (S.clear_color), S.clear_depth, S.clear_stencil & 0xFF);
	}
	else
	{
		pgpu_clear (m, color_u32 (S.clear_color), S.clear_depth);
	}
}

void glColorMask (GLboolean r, GLboolean g, GLboolean b, GLboolean a)
{
	S.color_mask[0] = r != 0;
	S.color_mask[1] = g != 0;
	S.color_mask[2] = b != 0;
	S.color_mask[3] = a != 0;
	pgpu_color_mask (r, g, b, a);
}

void glDepthMask (GLboolean flag)
{
	S.depth_mask = flag != 0;
	pgpu_depth_mask (flag);
}

void glDepthFunc (GLenum func)
{
	int f = map_func (func);
	if (f < 0)
	{
		ERROR (GL_INVALID_ENUM);
	}
	S.depth_func = func;
	pgpu_depth_func (f);
}

void glDepthRangef (GLclampf zNear, GLclampf zFar)
{
	S.depth_range[0] = clampf (zNear, 0, 1);
	S.depth_range[1] = clampf (zFar, 0, 1);
	pgpu_viewport (S.viewport[0], S.viewport[1], S.viewport[2], S.viewport[3],
		       S.depth_range[0], S.depth_range[1]);
}

void glViewport (GLint x, GLint y, GLsizei width, GLsizei height)
{
	if (width < 0 || height < 0)
	{
		ERROR (GL_INVALID_VALUE);
	}
	S.viewport[0] = x;
	S.viewport[1] = y;
	S.viewport[2] = width < MAX_SIZE ? width : MAX_SIZE;
	S.viewport[3] = height < MAX_SIZE ? height : MAX_SIZE;
	pgpu_viewport (S.viewport[0], S.viewport[1], S.viewport[2], S.viewport[3],
		       S.depth_range[0], S.depth_range[1]);
}

void glScissor (GLint x, GLint y, GLsizei width, GLsizei height)
{
	if (width < 0 || height < 0)
	{
		ERROR (GL_INVALID_VALUE);
	}
	S.scissor[0] = x;
	S.scissor[1] = y;
	S.scissor[2] = width;
	S.scissor[3] = height;
	pgpu_scissor (x, y, width, height);
}

void glCullFace (GLenum mode)
{
	int f = map_face (mode);
	if (f < 0)
	{
		ERROR (GL_INVALID_ENUM);
	}
	S.cull_face = mode;
	pgpu_cull_face (f);
}

void glFrontFace (GLenum mode)
{
	if (mode != GL_CW && mode != GL_CCW)
	{
		ERROR (GL_INVALID_ENUM);
	}
	S.front_face = mode;
	pgpu_front_face (mode == GL_CW ? PGPU_CW : PGPU_CCW);
}

void glLineWidth (GLfloat width)
{
	if (!(width > 0.0f))
	{
		ERROR (GL_INVALID_VALUE);
	}
	S.line_width = width;
	pgpu_line_width (width);
}

void glPolygonOffset (GLfloat factor, GLfloat units)
{
	S.polygon_offset_factor = factor;
	S.polygon_offset_units = units;
	pgpu_polygon_offset (factor, units);
}

void glSampleCoverage (GLclampf value, GLboolean invert)
{
	/* no multisample buffer: no effect (GL ES 2.0 4.1.3) */
	S.sample_coverage_value = clampf (value, 0, 1);
	S.sample_coverage_invert = invert != 0;
}

void glBlendFuncSeparate (GLenum srcRGB, GLenum dstRGB, GLenum srcAlpha, GLenum dstAlpha)
{
	int sr = map_blend_factor (srcRGB, true), dr = map_blend_factor (dstRGB, false);
	int sa = map_blend_factor (srcAlpha, true), da = map_blend_factor (dstAlpha, false);
	if (sr < 0 || dr < 0 || sa < 0 || da < 0)
	{
		ERROR (GL_INVALID_ENUM);
	}
	S.blend_src_rgb = srcRGB;
	S.blend_dst_rgb = dstRGB;
	S.blend_src_alpha = srcAlpha;
	S.blend_dst_alpha = dstAlpha;
	pgpu_blend_func_separate (sr, dr, sa, da);
}

void glBlendFunc (GLenum sfactor, GLenum dfactor)
{
	glBlendFuncSeparate (sfactor, dfactor, sfactor, dfactor);
}

void glBlendEquationSeparate (GLenum modeRGB, GLenum modeAlpha)
{
	int r = map_blend_equation (modeRGB), a = map_blend_equation (modeAlpha);
	if (r < 0 || a < 0)
	{
		ERROR (GL_INVALID_ENUM);
	}
	S.blend_eq_rgb = modeRGB;
	S.blend_eq_alpha = modeAlpha;
	pgpu_blend_equation (r, a);
}

void glBlendEquation (GLenum mode)
{
	glBlendEquationSeparate (mode, mode);
}

void glBlendColor (GLclampf r, GLclampf g, GLclampf b, GLclampf a)
{
	S.blend_color[0] = clampf (r, 0, 1);
	S.blend_color[1] = clampf (g, 0, 1);
	S.blend_color[2] = clampf (b, 0, 1);
	S.blend_color[3] = clampf (a, 0, 1);
	pgpu_blend_color (S.blend_color[0], S.blend_color[1], S.blend_color[2], S.blend_color[3]);
}

void glStencilFuncSeparate (GLenum face, GLenum func, GLint ref, GLuint mask)
{
	int fc = map_face (face), f = map_func (func);
	if (fc < 0 || f < 0)
	{
		ERROR (GL_INVALID_ENUM);
	}
	for (int i = 0; i < 2; i++)
	{
		if (fc == PGPU_FRONT_AND_BACK || fc == i)
		{
			S.stencil_func[i] = func;
			S.stencil_ref[i] = ref;
			S.stencil_value_mask[i] = mask;
		}
	}
	/* the reference value is clamped to the stencil range (0 .. 255) */
	pgpu_stencil_func (fc, f, ref < 0 ? 0 : ref > 255 ? 255 : ref, mask & 0xFF);
}

void glStencilFunc (GLenum func, GLint ref, GLuint mask)
{
	glStencilFuncSeparate (GL_FRONT_AND_BACK, func, ref, mask);
}

void glStencilOpSeparate (GLenum face, GLenum fail, GLenum zfail, GLenum zpass)
{
	int fc = map_face (face);
	int f = map_stencil_op (fail), zf = map_stencil_op (zfail), zp = map_stencil_op (zpass);
	if (fc < 0 || f < 0 || zf < 0 || zp < 0)
	{
		ERROR (GL_INVALID_ENUM);
	}
	for (int i = 0; i < 2; i++)
	{
		if (fc == PGPU_FRONT_AND_BACK || fc == i)
		{
			S.stencil_fail[i] = fail;
			S.stencil_zfail[i] = zfail;
			S.stencil_zpass[i] = zpass;
		}
	}
	pgpu_stencil_op (fc, f, zf, zp);
}

void glStencilOp (GLenum fail, GLenum zfail, GLenum zpass)
{
	glStencilOpSeparate (GL_FRONT_AND_BACK, fail, zfail, zpass);
}

void glStencilMaskSeparate (GLenum face, GLuint mask)
{
	int fc = map_face (face);
	if (fc < 0)
	{
		ERROR (GL_INVALID_ENUM);
	}
	for (int i = 0; i < 2; i++)
	{
		if (fc == PGPU_FRONT_AND_BACK || fc == i)
		{
			S.stencil_writemask[i] = mask;
		}
	}
	pgpu_stencil_mask (fc, mask & 0xFF);
}

void glStencilMask (GLuint mask)
{
	glStencilMaskSeparate (GL_FRONT_AND_BACK, mask);
}

void glHint (GLenum target, GLenum mode)
{
	if (mode != GL_FASTEST && mode != GL_NICEST && mode != GL_DONT_CARE)
	{
		ERROR (GL_INVALID_ENUM);
	}
	switch (target)
	{
	case GL_GENERATE_MIPMAP_HINT:		S.generate_mipmap_hint = mode;	break;
	case GL_PERSPECTIVE_CORRECTION_HINT:	S.perspective_hint = mode;	break;
	case GL_POINT_SMOOTH_HINT:		S.point_smooth_hint = mode;	break;
	case GL_LINE_SMOOTH_HINT:		S.line_smooth_hint = mode;	break;
	case GL_FOG_HINT:			S.fog_hint = mode;		break;
	default:				ERROR (GL_INVALID_ENUM);
	}
}

void glPixelStorei (GLenum pname, GLint param)
{
	if (param != 1 && param != 2 && param != 4 && param != 8)
	{
		ERROR (GL_INVALID_VALUE);
	}
	switch (pname)
	{
	case GL_PACK_ALIGNMENT:		S.pack_alignment = param;	break;
	case GL_UNPACK_ALIGNMENT:	S.unpack_alignment = param;	break;
	default:			ERROR (GL_INVALID_ENUM);
	}
}

/* ---- read-back -------------------------------------------------------------------------------- */

void glReadPixels (GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, void *pixels)
{
	if (width < 0 || height < 0)
	{
		ERROR (GL_INVALID_VALUE);
	}
	if (format != GL_RGBA && format != GL_RGB && format != GL_ALPHA && format != GL_LUMINANCE
	    && format != GL_LUMINANCE_ALPHA)
	{
		ERROR (GL_INVALID_ENUM);
	}
	if (   type != GL_UNSIGNED_BYTE && type != GL_UNSIGNED_SHORT_5_6_5
	    && type != GL_UNSIGNED_SHORT_4_4_4_4 && type != GL_UNSIGNED_SHORT_5_5_5_1)
	{
		ERROR (GL_INVALID_ENUM);
	}
	/* RGBA / UNSIGNED_BYTE, which is also the implementation's read format */
	if (format != GL_RGBA || type != GL_UNSIGNED_BYTE)
	{
		ERROR (GL_INVALID_OPERATION);
	}
	if (!validate_framebuffer ())
	{
		return;
	}
	if (!target_has_color ())
	{
		ERROR (GL_INVALID_OPERATION);		/* nothing to read */
	}
	if (!width || !height)
	{
		return;
	}

	bool opaque = !target_has_alpha ();
	uint8_t *dst = pixels;
	/* rows are GL_PACK_ALIGNMENT aligned: straight into the caller's memory
	   when they are packed and word aligned, else through a bounce buffer */
	size_t stride = ((size_t) width * 4 + S.pack_alignment - 1) / S.pack_alignment * S.pack_alignment;
	static uint32_t bounce[1024];
	bool direct = ((uintptr_t) pixels & 3) == 0 && stride == (size_t) width * 4;
	uint32_t rows_per_read = direct ? 262144 / width : sizeof bounce / 4 / width;
	if (rows_per_read == 0)
	{
		rows_per_read = 1;		/* a row wider than the bounce buffer: read in pieces */
	}
	for (GLsizei row = 0; row < height; row += rows_per_read)
	{
		uint32_t n = height - row < (GLsizei) rows_per_read ? height - row : rows_per_read;
		for (GLsizei col = 0; col < width; )
		{
			uint32_t span = width - col;
			if (!direct && span > sizeof bounce / 4)
			{
				span = sizeof bounce / 4;
				n = 1;
			}
			uint32_t *out = direct ? (uint32_t *) (dst + (size_t) row * stride) : bounce;
			uint32_t timeout = 100 + span * n / 1000;
			if (!pgpu_read_pixels (x + col, y + row, span, n, out, timeout))
			{
				ERROR (GL_OUT_OF_MEMORY);	/* no answer from the Zero */
			}
			for (uint32_t r = 0; r < n; r++)
			{
				uint8_t *d = dst + (size_t) (row + r) * stride + (size_t) col * 4;
				if (out == bounce)
				{
					memcpy (d, bounce + r * span, span * 4);
				}
				if (opaque)
				{
					for (uint32_t i = 0; i < span; i++)
					{
						d[i * 4 + 3] = 255;
					}
				}
			}
			col += span;
		}
	}
}

/* ---- programs and shaders --------------------------------------------------------------------- */

/* the object a GL name stands for, or one of another generation */
static object_t *object_at (GLuint name)
{
	return &S.objects[name ? (name - 1) % MAX_OBJECT_NAMES + 1 : 0];
}

static bool object_exists (GLuint name)
{
	const object_t *o = object_at (name);
	return name && o->kind != OBJ_NONE && o->name == name;
}

static object_t *get_object (GLuint name, uint8_t kind)
{
	if (!object_exists (name))
	{
		ERROR_RET (GL_INVALID_VALUE, NULL);
	}
	if (object_at (name)->kind != kind)
	{
		ERROR_RET (GL_INVALID_OPERATION, NULL);
	}
	return object_at (name);
}

/* names only grow, as Mesa's: a deleted name stays invalid (tests rely on
   it), and slots are reused in turn */
static GLuint new_object (uint8_t kind)
{
	const GLuint last = S.last_object_name;
	for (GLuint k = 1; k <= MAX_OBJECT_NAMES; k++)
	{
		GLuint slot = (last + k - 1) % MAX_OBJECT_NAMES + 1;
		if (S.objects[slot].kind == OBJ_NONE)
		{
			/* the next name above the last with this slot */
			GLuint name = last + 1 + (slot - 1 + MAX_OBJECT_NAMES - last % MAX_OBJECT_NAMES) % MAX_OBJECT_NAMES;
			memset (&S.objects[slot], 0, sizeof S.objects[slot]);
			S.objects[slot].kind = kind;
			S.objects[slot].name = name;
			S.last_object_name = name;
			return name;
		}
	}
	ERROR_RET (GL_OUT_OF_MEMORY, 0);
}

GLuint glCreateShader (GLenum type)
{
	if (type != GL_VERTEX_SHADER && type != GL_FRAGMENT_SHADER)
	{
		ERROR_RET (GL_INVALID_ENUM, 0);
	}
	GLuint name = new_object (OBJ_SHADER);
	if (name)
	{
		object_at (name)->shader_type = type;
	}
	return name;
}

GLuint glCreateProgram (void)
{
	return new_object (OBJ_PROGRAM);
}

GLboolean glIsShader (GLuint shader)
{
	return object_exists (shader) && object_at (shader)->kind == OBJ_SHADER;
}

GLboolean glIsProgram (GLuint program)
{
	return object_exists (program) && object_at (program)->kind == OBJ_PROGRAM;
}

/* without a shader compiler (the Pico), these generate INVALID_OPERATION
   (GL ES 2.0 2.10.1); on a PC, pgl compiles with tools/glslc (pgl_compiler.h) */
void glShaderSource (GLuint shader, GLsizei count, const GLchar *const *string, const GLint *length)
{
	if (!pglc_available ())
	{
		ERROR (GL_INVALID_OPERATION);
	}
	object_t *s = get_object (shader, OBJ_SHADER);
	if (!s)
	{
		return;
	}
	if (count < 0)
	{
		ERROR (GL_INVALID_VALUE);
	}
	size_t total = 0;
	for (GLsizei i = 0; i < count; i++)
	{
		total += length && length[i] >= 0 ? (size_t) length[i] : strlen (string[i]);
	}
	char *source = malloc (total + 1);
	if (!source)
	{
		ERROR (GL_OUT_OF_MEMORY);
	}
	size_t pos = 0;
	for (GLsizei i = 0; i < count; i++)
	{
		size_t n = length && length[i] >= 0 ? (size_t) length[i] : strlen (string[i]);
		memcpy (source + pos, string[i], n);
		pos += n;
	}
	source[pos] = 0;
	free (s->source);
	s->source = source;
}

void glCompileShader (GLuint shader)
{
	if (!pglc_available ())
	{
		ERROR (GL_INVALID_OPERATION);
	}
	object_t *s = get_object (shader, OBJ_SHADER);
	if (!s)
	{
		return;
	}
	char *log = NULL;
	bool ok = s->source && pglc_compile (s->shader_type, s->source, &log);
	free (s->log_buf);
	s->log_buf = log;
	free (s->compiled_source);
	s->compiled_source = ok ? strdup (s->source) : NULL;
	s->compiled = ok && s->compiled_source;
	s->binary = NULL;
}

void glReleaseShaderCompiler (void)
{
	if (!pglc_available ())
	{
		ERROR (GL_INVALID_OPERATION);
	}
}

void glGetShaderPrecisionFormat (GLenum shadertype, GLenum precisiontype, GLint *range, GLint *precision)
{
	if (!pglc_available ())
	{
		ERROR (GL_INVALID_OPERATION);
	}
	if (shadertype != GL_VERTEX_SHADER && shadertype != GL_FRAGMENT_SHADER)
	{
		ERROR (GL_INVALID_ENUM);
	}
	/* Mesa's values for vc4 (32-bit floats and integers at every precision) */
	switch (precisiontype)
	{
	case GL_LOW_FLOAT: case GL_MEDIUM_FLOAT: case GL_HIGH_FLOAT:
		range[0] = range[1] = 127;
		*precision = 23;
		break;
	case GL_LOW_INT: case GL_MEDIUM_INT: case GL_HIGH_INT:
		range[0] = 31;
		range[1] = 30;
		*precision = 0;
		break;
	default:
		ERROR (GL_INVALID_ENUM);
	}
}

static const char bad_binary_log[] = "pgl: not a program binary from tools/glslc";

static bool valid_binary (const void *binary, GLint length)
{
	const pgpu_program_info_t *info = binary;
	return    length == (GLint) sizeof (pgpu_program_info_t) && info
	       && info->magic == PGPU_PROGRAM_INFO_MAGIC && info->blob && info->words;
}

static void link_binary (GLuint program, object_t *p, const pgpu_program_info_t *info);

/* the binary format PGL_SHADER_BINARY_PGPU: a program from tools/glslc (its
   NAME_info), an optimized pair of vertex and fragment shaders (GL ES 2.0
   2.10.2). Load it into both shaders, by one call or one per shader;
   glLinkProgram links shaders loaded from the same binary. */
void glShaderBinary (GLsizei n, const GLuint *shaders, GLenum binaryformat, const void *binary, GLsizei length)
{
	if (binaryformat != PGL_SHADER_BINARY_PGPU)
	{
		ERROR (GL_INVALID_ENUM);
	}
	if (n < 0 || length < 0)
	{
		ERROR (GL_INVALID_VALUE);
	}
	bool types[2] = {false, false};
	for (GLsizei i = 0; i < n; i++)
	{
		object_t *s = get_object (shaders[i], OBJ_SHADER);
		if (!s)
		{
			return;
		}
		int t = s->shader_type == GL_VERTEX_SHADER ? 0 : 1;
		if (types[t])
		{
			ERROR (GL_INVALID_OPERATION);	/* two shaders of one type */
		}
		types[t] = true;
	}
	if (!valid_binary (binary, length))
	{
		ERROR (GL_INVALID_VALUE);
	}
	for (GLsizei i = 0; i < n; i++)
	{
		object_at (shaders[i])->binary = binary;
		object_at (shaders[i])->compiled = false;
	}
}

static void free_shader (GLuint name)
{
	object_t *s = object_at (name);
	free (s->source);
	free (s->compiled_source);
	free (s->log_buf);
	memset (s, 0, sizeof *s);
}

void glDeleteShader (GLuint shader)
{
	if (!shader)
	{
		return;
	}
	object_t *s = get_object (shader, OBJ_SHADER);
	if (!s)
	{
		return;
	}
	if (s->attached)
	{
		s->delete_pending = true;
	}
	else
	{
		free_shader (shader);
	}
}

void glAttachShader (GLuint program, GLuint shader)
{
	object_t *p = get_object (program, OBJ_PROGRAM), *s = get_object (shader, OBJ_SHADER);
	if (!p || !s)
	{
		return;
	}
	int slot = s->shader_type == GL_VERTEX_SHADER ? 0 : 1;
	if (p->shaders[slot])
	{
		ERROR (GL_INVALID_OPERATION);	/* already attached, or one of this type */
	}
	p->shaders[slot] = shader;
	s->attached++;
}

static void detach (object_t *p, int slot)
{
	GLuint shader = p->shaders[slot];
	object_t *s = object_at (shader);
	p->shaders[slot] = 0;
	if (--s->attached == 0 && s->delete_pending)
	{
		free_shader (shader);
	}
}

void glDetachShader (GLuint program, GLuint shader)
{
	object_t *p = get_object (program, OBJ_PROGRAM), *s = get_object (shader, OBJ_SHADER);
	if (!p || !s)
	{
		return;
	}
	for (int slot = 0; slot < 2; slot++)
	{
		if (p->shaders[slot] == shader)
		{
			detach (p, slot);
			return;
		}
	}
	ERROR (GL_INVALID_OPERATION);
}

void glGetAttachedShaders (GLuint program, GLsizei maxcount, GLsizei *count, GLuint *shaders)
{
	object_t *p = get_object (program, OBJ_PROGRAM);
	if (!p)
	{
		return;
	}
	if (maxcount < 0)
	{
		ERROR (GL_INVALID_VALUE);
	}
	GLsizei n = 0;
	for (int slot = 0; slot < 2; slot++)
	{
		if (p->shaders[slot] && n < maxcount)
		{
			shaders[n++] = p->shaders[slot];
		}
	}
	if (count)
	{
		*count = n;
	}
}

void glBindAttribLocation (GLuint program, GLuint index, const GLchar *name)
{
	/* takes effect at the next glLinkProgram, which can't succeed without a
	   compiler: program binaries have the locations given to glslc */
	if (index >= ATTRIBS)
	{
		ERROR (GL_INVALID_VALUE);
	}
	if (!get_object (program, OBJ_PROGRAM))
	{
		return;
	}
	if (strncmp (name, "gl_", 3) == 0)
	{
		ERROR (GL_INVALID_OPERATION);
	}

	/* used by the next glLinkProgram from source */
	object_t *p = object_at (program);
	for (unsigned i = 0; i < p->n_bindings; i++)
	{
		if (strcmp (p->bindings[i].name, name) == 0)
		{
			p->bindings[i].index = index;
			return;
		}
	}
	struct attrib_binding *b = realloc (p->bindings, (p->n_bindings + 1) * sizeof *b);
	char *copy = strdup (name);
	if (!b || !copy)
	{
		free (copy);
		if (b)
		{
			p->bindings = b;
		}
		ERROR (GL_OUT_OF_MEMORY);
	}
	p->bindings = b;
	b[p->n_bindings].name = copy;
	b[p->n_bindings].index = index;
	p->n_bindings++;
}

static const char not_a_pair_log[] =
	"pgl: link needs a vertex and a fragment shader loaded by glShaderBinary from the same "
	"binary (no shader compiler)";

void glLinkProgram (GLuint program)
{
	object_t *p = get_object (program, OBJ_PROGRAM);
	if (!p)
	{
		return;
	}
	const object_t *vs = p->shaders[0] ? object_at (p->shaders[0]) : NULL;
	const object_t *fs = p->shaders[1] ? object_at (p->shaders[1]) : NULL;
	pgpu_program_info_t *compiled = NULL;
	const pgpu_program_info_t *info = NULL;
	if (vs && fs && vs->compiled && fs->compiled)
	{
		/* from source (pgl_compiler.h) */
		const char *names[ATTRIBS * 4];
		unsigned indices[ATTRIBS * 4];
		unsigned n = 0;
		for (unsigned i = 0; i < p->n_bindings && n < ATTRIBS * 4; i++, n++)
		{
			names[n] = p->bindings[i].name;
			indices[n] = p->bindings[i].index;
		}
		char *log = NULL;
		compiled = pglc_link (vs->compiled_source, fs->compiled_source, names, indices, n, &log);
		free (p->log_buf);
		p->log_buf = log;
		if (!compiled)
		{
			p->linked = false;
			p->log = p->log_buf;
			return;
		}
		info = compiled;
	}
	else if (vs && fs && vs->binary && vs->binary == fs->binary)
	{
		info = vs->binary;
	}
	else
	{
		/* a failed link; a program in use keeps its executable (GL ES 2.0 2.10.3) */
		p->linked = false;
		p->log = pglc_available () ? "pgl: link needs a compiled vertex and fragment shader"
					   : not_a_pair_log;
		return;
	}
	link_binary (program, p, info);
	if (p->compiled_info != compiled)
	{
		pglc_free (p->compiled_info);
		p->compiled_info = compiled;
	}
	if (p->linked && p->log_buf && !p->log)
	{
		p->log = p->log_buf;		/* warnings */
	}
}

static void release_program (object_t *p)
{
	if (p->hw)
	{
		pgpu_program_delete (p->hw);
		S.hw_program_used[p->hw] = false;
		p->hw = 0;
	}
	free (p->values);
	free (p->first);
	p->values = NULL;
	p->first = NULL;
	p->info = NULL;
	pglc_free (p->compiled_info);
	p->compiled_info = NULL;
	free (p->log_buf);
	p->log_buf = NULL;
	for (unsigned i = 0; i < p->n_bindings; i++)
	{
		free (p->bindings[i].name);
	}
	free (p->bindings);
	p->bindings = NULL;
	p->n_bindings = 0;
}

static void free_program (GLuint name)
{
	object_t *p = object_at (name);
	for (int slot = 0; slot < 2; slot++)
	{
		if (p->shaders[slot])
		{
			detach (p, slot);
		}
	}
	release_program (p);
	memset (p, 0, sizeof *p);
}

void glDeleteProgram (GLuint program)
{
	if (!program)
	{
		return;
	}
	object_t *p = get_object (program, OBJ_PROGRAM);
	if (!p)
	{
		return;
	}
	if (S.program == program)
	{
		p->delete_pending = true;
	}
	else
	{
		free_program (program);
	}
}

static const char rejected_log[] = "pgl: the GPU rejected the program binary";

void glProgramBinaryOES (GLuint program, GLenum binaryFormat, const void *binary, GLint length)
{
	object_t *p = get_object (program, OBJ_PROGRAM);
	if (!p)
	{
		return;
	}
	if (binaryFormat != PGL_PROGRAM_BINARY_PGPU)
	{
		ERROR (GL_INVALID_ENUM);
	}
	p->linked = false;
	if (!valid_binary (binary, length))
	{
		p->log = bad_binary_log;	/* a failed link, not a GL error */
		return;
	}
	link_binary (program, p, binary);
}

/* GL_EXT_debug_marker: for a debugger's trace; none here. They never set an
   error, whatever the arguments (the extension says so) */
void glInsertEventMarkerEXT (GLsizei length, const GLchar *marker)
{
	(void) length;
	(void) marker;
}

void glPushGroupMarkerEXT (GLsizei length, const GLchar *marker)
{
	(void) length;
	(void) marker;
}

void glPopGroupMarkerEXT (void)
{
}

/* load a program binary into a program object, as linking does */
static void link_binary (GLuint program, object_t *p, const pgpu_program_info_t *info)
{
	p->linked = false;

	/* uniform values, zero as after linking */
	uint32_t n_values = 0;
	for (uint32_t i = 0; i < info->n_uniforms; i++)
	{
		n_values += info->uniforms[i].size * info->uniforms[i].components;
	}
	uint32_t *values = calloc (n_values ? n_values : 1, sizeof (uint32_t));
	uint16_t *first = calloc (info->n_uniforms ? info->n_uniforms : 1, sizeof (uint16_t));
	if (!values || !first)
	{
		free (values);
		free (first);
		ERROR (GL_OUT_OF_MEMORY);
	}

	uint8_t hw = p->hw;
	if (!hw)
	{
		for (unsigned i = 1; i <= MAX_HW_PROGRAMS; i++)
		{
			if (!S.hw_program_used[i])
			{
				hw = i;
				break;
			}
		}
		if (!hw)
		{
			free (values);
			free (first);
			ERROR (GL_OUT_OF_MEMORY);
		}
		S.hw_program_used[hw] = true;
	}
	free (p->values);
	free (p->first);
	p->hw = hw;
	p->info = info;
	p->values = values;
	p->first = first;
	for (uint32_t i = 0, v = 0; i < info->n_uniforms; i++)
	{
		first[i] = v;
		v += info->uniforms[i].size * info->uniforms[i].components;
	}

	/* upload, and wait until the Zero has checked it */
	drain_zero_errors (0);
	pgpu_program_create (hw, info->blob, info->words);
	memset (p->units, 0, sizeof p->units);
	for (uint32_t i = 0; i < info->n_uniforms; i++)
	{
		const pgpu_uniform_info_t *u = &info->uniforms[i];
		for (int e = 0; u->sampler >= 0 && e < u->size; e++)
		{
			pgpu_program_sampler (hw, u->sampler + e, 0);	/* GL: samplers start at unit 0 */
		}
	}
	static uint32_t cookie = 0xB1000000u;
	pgpu_ping_wait (++cookie, 1000);
	pgpu_link_settle ();
	if (drain_zero_errors (hw))
	{
		p->log = rejected_log;
		return;
	}
	p->linked = true;
	p->validated = false;
	p->log = NULL;
	if (S.program == program)
	{
		pgpu_use_program (hw);
	}
}

void glGetProgramBinaryOES (GLuint program, GLsizei bufSize, GLsizei *length, GLenum *binaryFormat,
			    void *binary)
{
	object_t *p = get_object (program, OBJ_PROGRAM);
	if (!p)
	{
		return;
	}
	if (!p->linked || bufSize < (GLsizei) sizeof (pgpu_program_info_t))
	{
		ERROR (GL_INVALID_OPERATION);
	}
	memcpy (binary, p->info, sizeof (pgpu_program_info_t));
	if (length)
	{
		*length = sizeof (pgpu_program_info_t);
	}
	*binaryFormat = PGL_PROGRAM_BINARY_PGPU;
}

void glUseProgram (GLuint program)
{
	object_t *p = NULL;
	if (program)
	{
		p = get_object (program, OBJ_PROGRAM);
		if (!p)
		{
			return;
		}
		if (!p->linked)
		{
			ERROR (GL_INVALID_OPERATION);
		}
	}
	GLuint old = S.program;
	S.program = program;
	pgpu_use_program (p ? p->hw : 0);
	if (old && old != program && object_at (old)->delete_pending)
	{
		free_program (old);
	}
}

void glValidateProgram (GLuint program)
{
	object_t *p = get_object (program, OBJ_PROGRAM);
	if (p)
	{
		p->validated = p->linked;
	}
}

static int max_name_length (const object_t *p, bool uniforms)
{
	int max = 0;
	if (!p->linked)
	{
		return 0;
	}
	if (uniforms)
	{
		for (uint32_t i = 0; i < p->info->n_uniforms; i++)
		{
			const pgpu_uniform_info_t *u = &p->info->uniforms[i];
			int n = strlen (u->name) + 1 + (u->size > 1 || u->array ? 3 : 0);	/* "[0]" */
			max = n > max ? n : max;
		}
	}
	else
	{
		for (uint32_t i = 0; i < p->info->n_attribs; i++)
		{
			int n = strlen (p->info->attribs[i].name) + 1;
			max = n > max ? n : max;
		}
	}
	return max;
}

void glGetProgramiv (GLuint program, GLenum pname, GLint *params)
{
	object_t *p = get_object (program, OBJ_PROGRAM);
	if (!p)
	{
		return;
	}
	switch (pname)
	{
	case GL_DELETE_STATUS:			*params = p->delete_pending;			break;
	case GL_LINK_STATUS:			*params = p->linked;				break;
	case GL_VALIDATE_STATUS:		*params = p->validated;				break;
	case GL_INFO_LOG_LENGTH:		*params = p->log ? (GLint) strlen (p->log) + 1 : 0;	break;
	case GL_ATTACHED_SHADERS:		*params = (p->shaders[0] != 0) + (p->shaders[1] != 0);	break;
	case GL_ACTIVE_ATTRIBUTES:		*params = p->linked ? p->info->n_attribs : 0;	break;
	case GL_ACTIVE_ATTRIBUTE_MAX_LENGTH:	*params = max_name_length (p, false);		break;
	case GL_ACTIVE_UNIFORMS:		*params = p->linked ? p->info->n_uniforms : 0;	break;
	case GL_ACTIVE_UNIFORM_MAX_LENGTH:	*params = max_name_length (p, true);		break;
	case GL_PROGRAM_BINARY_LENGTH_OES:	*params = p->linked ? sizeof (pgpu_program_info_t) : 0;	break;
	default:				ERROR (GL_INVALID_ENUM);
	}
}

static void copy_string (const char *s, GLsizei bufsize, GLsizei *length, GLchar *out)
{
	GLsizei n = 0;
	if (bufsize > 0)
	{
		n = s ? strlen (s) : 0;
		if (n > bufsize - 1)
		{
			n = bufsize - 1;
		}
		memcpy (out, s ? s : "", n);
		out[n] = 0;
	}
	if (length)
	{
		*length = n;
	}
}

void glGetProgramInfoLog (GLuint program, GLsizei bufsize, GLsizei *length, GLchar *infolog)
{
	object_t *p = get_object (program, OBJ_PROGRAM);
	if (!p)
	{
		return;
	}
	if (bufsize < 0)
	{
		ERROR (GL_INVALID_VALUE);
	}
	copy_string (p->log, bufsize, length, infolog);
}

void glGetShaderiv (GLuint shader, GLenum pname, GLint *params)
{
	object_t *s = get_object (shader, OBJ_SHADER);
	if (!s)
	{
		return;
	}
	switch (pname)
	{
	case GL_SHADER_TYPE:		*params = s->shader_type;	return;
	case GL_DELETE_STATUS:		*params = s->delete_pending;	return;
	case GL_COMPILE_STATUS:
	case GL_INFO_LOG_LENGTH:
	case GL_SHADER_SOURCE_LENGTH:
		break;
	default:
		ERROR (GL_INVALID_ENUM);
	}
	if (!pglc_available ())
	{
		ERROR (GL_INVALID_OPERATION);	/* without a shader compiler (GL ES 2.0 6.1.8) */
	}
	switch (pname)
	{
	case GL_COMPILE_STATUS:		*params = s->compiled;					break;
	case GL_INFO_LOG_LENGTH:	*params = s->log_buf ? (GLint) strlen (s->log_buf) + 1 : 0;	break;
	default:			*params = s->source ? (GLint) strlen (s->source) + 1 : 0;	break;
	}
}

void glGetShaderInfoLog (GLuint shader, GLsizei bufsize, GLsizei *length, GLchar *infolog)
{
	if (!pglc_available ())
	{
		ERROR (GL_INVALID_OPERATION);	/* no shader compiler (GL ES 2.0 6.1.8) */
	}
	object_t *s = get_object (shader, OBJ_SHADER);
	if (!s)
	{
		return;
	}
	if (bufsize < 0)
	{
		ERROR (GL_INVALID_VALUE);
	}
	copy_string (s->log_buf, bufsize, length, infolog);
}

void glGetShaderSource (GLuint shader, GLsizei bufsize, GLsizei *length, GLchar *source)
{
	if (!pglc_available ())
	{
		ERROR (GL_INVALID_OPERATION);	/* no shader compiler (GL ES 2.0 6.1.8) */
	}
	object_t *s = get_object (shader, OBJ_SHADER);
	if (!s)
	{
		return;
	}
	if (bufsize < 0)
	{
		ERROR (GL_INVALID_VALUE);
	}
	copy_string (s->source, bufsize, length, source);
}

static object_t *linked_program (GLuint program)
{
	object_t *p = get_object (program, OBJ_PROGRAM);
	if (p && !p->linked)
	{
		ERROR_RET (GL_INVALID_OPERATION, NULL);
	}
	return p;
}

GLint glGetAttribLocation (GLuint program, const GLchar *name)
{
	object_t *p = linked_program (program);
	if (!p)
	{
		return -1;
	}
	for (uint32_t i = 0; i < p->info->n_attribs; i++)
	{
		if (strcmp (p->info->attribs[i].name, name) == 0)
		{
			return p->info->attribs[i].location;
		}
	}
	return -1;
}

void glGetActiveAttrib (GLuint program, GLuint index, GLsizei bufsize, GLsizei *length, GLint *size,
			GLenum *type, GLchar *name)
{
	object_t *p = get_object (program, OBJ_PROGRAM);
	if (!p)
	{
		return;
	}
	if (!p->linked || index >= p->info->n_attribs || bufsize < 0)
	{
		ERROR (GL_INVALID_VALUE);
	}
	const pgpu_attrib_info_t *a = &p->info->attribs[index];
	copy_string (a->name, bufsize, length, name);
	*size = a->size;
	*type = a->type;
}

void glGetActiveUniform (GLuint program, GLuint index, GLsizei bufsize, GLsizei *length, GLint *size,
			 GLenum *type, GLchar *name)
{
	object_t *p = get_object (program, OBJ_PROGRAM);
	if (!p)
	{
		return;
	}
	if (!p->linked || index >= p->info->n_uniforms || bufsize < 0)
	{
		ERROR (GL_INVALID_VALUE);
	}
	const pgpu_uniform_info_t *u = &p->info->uniforms[index];
	char buffer[80];
	snprintf (buffer, sizeof buffer, "%s%s", u->name, u->size > 1 || u->array ? "[0]" : "");
	copy_string (buffer, bufsize, length, name);
	*size = u->size;
	*type = u->type;
}

GLint glGetUniformLocation (GLuint program, const GLchar *name)
{
	object_t *p = linked_program (program);
	if (!p)
	{
		return -1;
	}
	size_t n = strlen (name);
	unsigned element = 0;
	if (n > 3 && name[n - 1] == ']')
	{
		/* "name[element]" */
		const char *open = strrchr (name, '[');
		if (!open || open[1] == ']')
		{
			return -1;
		}
		element = 0;
		for (const char *c = open + 1; *c != ']'; c++)
		{
			if (*c < '0' || *c > '9' || element > 65535)
			{
				return -1;
			}
			element = element * 10 + (*c - '0');
		}
		n = open - name;
	}
	for (uint32_t i = 0; i < p->info->n_uniforms; i++)
	{
		const pgpu_uniform_info_t *u = &p->info->uniforms[i];
		if (strlen (u->name) == n && strncmp (u->name, name, n) == 0)
		{
			return element < (unsigned) u->size ? LOCATION (i, element) : -1;
		}
	}
	return -1;
}

/* components and kind ('f' float, 'i' int, 'b' bool, 'm' matrix, 's' sampler)
   of a uniform type */
static char uniform_kind (GLenum type)
{
	switch (type)
	{
	case GL_FLOAT: case GL_FLOAT_VEC2: case GL_FLOAT_VEC3: case GL_FLOAT_VEC4:	return 'f';
	case GL_INT: case GL_INT_VEC2: case GL_INT_VEC3: case GL_INT_VEC4:		return 'i';
	case GL_BOOL: case GL_BOOL_VEC2: case GL_BOOL_VEC3: case GL_BOOL_VEC4:		return 'b';
	case GL_FLOAT_MAT2: case GL_FLOAT_MAT3: case GL_FLOAT_MAT4:			return 'm';
	default:									return 's';
	}
}

/* write words [first, first + n) of a uniform into the program's storage */
static void send_uniform (uint32_t hw, const pgpu_uniform_info_t *u, uint32_t first, uint32_t n,
			  const uint32_t *values)
{
	uint32_t total = u->size * u->components;
	for (uint32_t stage = 0; stage < 2; stage++)
	{
		const uint16_t *o = u->offsets + stage * total + first;
		uint32_t i = 0;
		while (i < n)
		{
			if (o[i] == 0xFFFF)
			{
				i++;
				continue;
			}
			uint32_t run = 1;
			while (i + run < n && o[i + run] == o[i] + run)
			{
				run++;
			}
			uint32_t *p = pgpu_begin (PGPU_OP_PROGRAM_UNIFORM, 2 + run);
			p[0] = hw;
			p[1] = o[i];
			memcpy (&p[2], values + i, run * 4);
			pgpu_end ();
			i += run;
		}
	}
}

/* glUniform*: kind 'f', 'i' or 'm'; comps per element */
static void set_uniform (GLint location, GLsizei count, unsigned comps, char kind, const void *v)
{
	if (!S.program)
	{
		ERROR (GL_INVALID_OPERATION);
	}
	if (count < 0)
	{
		ERROR (GL_INVALID_VALUE);
	}
	if (location == -1)
	{
		return;
	}
	object_t *p = object_at (S.program);
	uint32_t index = (uint32_t) location >> 16, element = location & 0xFFFF;
	if (location < 0 || index >= p->info->n_uniforms || element >= (uint32_t) p->info->uniforms[index].size)
	{
		ERROR (GL_INVALID_OPERATION);
	}
	const pgpu_uniform_info_t *u = &p->info->uniforms[index];
	char uk = uniform_kind (u->type);
	if (count > 1 && u->size == 1 && !u->array)
	{
		ERROR (GL_INVALID_OPERATION);
	}
	uint32_t n = count;
	if (n > u->size - element)
	{
		n = u->size - element;
	}

	if (uk == 's')
	{
		if (kind != 'i' || comps != 1)
		{
			ERROR (GL_INVALID_OPERATION);
		}
		const GLint *units = v;
		for (uint32_t e = 0; e < n; e++)
		{
			if (units[e] < 0 || units[e] >= UNITS)
			{
				ERROR (GL_INVALID_VALUE);
			}
		}
		for (uint32_t e = 0; u->sampler >= 0 && e < n; e++)
		{
			uint32_t sampler = u->sampler + element + e;
			if (sampler >= PGPU_MAX_SAMPLERS)
			{
				break;			/* glslc gives every sampler an index */
			}
			p->units[sampler] = units[e];
			pgpu_program_sampler (p->hw, sampler, units[e]);
		}
		return;
	}
	if (   comps != u->components
	    || (uk == 'm') != (kind == 'm')
	    || (uk == 'f' && kind != 'f')
	    || (uk == 'i' && kind != 'i'))
	{
		ERROR (GL_INVALID_OPERATION);
	}

	uint32_t first = element * comps, scalars = n * comps;
	/* the words as Mesa's vc4 stores them (native integers): float, int32,
	   bool 0 / ~0 */
	uint32_t *dst = p->values + p->first[index] + first;
	for (uint32_t i = 0; i < scalars; i++)
	{
		if (uk == 'b')
		{
			bool on = kind == 'i' ? ((const GLint *) v)[i] != 0 : ((const GLfloat *) v)[i] != 0.0f;
			dst[i] = on ? 0xFFFFFFFFu : 0;
		}
		else if (uk == 'i')
		{
			dst[i] = (uint32_t) ((const GLint *) v)[i];
		}
		else
		{
			dst[i] = f2u (((const GLfloat *) v)[i]);
		}
	}
	send_uniform (p->hw, u, first, scalars, dst);
}

void glUniform1fv (GLint l, GLsizei c, const GLfloat *v)	{ set_uniform (l, c, 1, 'f', v); }
void glUniform2fv (GLint l, GLsizei c, const GLfloat *v)	{ set_uniform (l, c, 2, 'f', v); }
void glUniform3fv (GLint l, GLsizei c, const GLfloat *v)	{ set_uniform (l, c, 3, 'f', v); }
void glUniform4fv (GLint l, GLsizei c, const GLfloat *v)	{ set_uniform (l, c, 4, 'f', v); }
void glUniform1iv (GLint l, GLsizei c, const GLint *v)		{ set_uniform (l, c, 1, 'i', v); }
void glUniform2iv (GLint l, GLsizei c, const GLint *v)		{ set_uniform (l, c, 2, 'i', v); }
void glUniform3iv (GLint l, GLsizei c, const GLint *v)		{ set_uniform (l, c, 3, 'i', v); }
void glUniform4iv (GLint l, GLsizei c, const GLint *v)		{ set_uniform (l, c, 4, 'i', v); }

void glUniform1f (GLint l, GLfloat x)					{ set_uniform (l, 1, 1, 'f', &x); }
void glUniform2f (GLint l, GLfloat x, GLfloat y)			{ GLfloat v[] = {x, y}; set_uniform (l, 1, 2, 'f', v); }
void glUniform3f (GLint l, GLfloat x, GLfloat y, GLfloat z)		{ GLfloat v[] = {x, y, z}; set_uniform (l, 1, 3, 'f', v); }
void glUniform4f (GLint l, GLfloat x, GLfloat y, GLfloat z, GLfloat w)	{ GLfloat v[] = {x, y, z, w}; set_uniform (l, 1, 4, 'f', v); }
void glUniform1i (GLint l, GLint x)					{ set_uniform (l, 1, 1, 'i', &x); }
void glUniform2i (GLint l, GLint x, GLint y)				{ GLint v[] = {x, y}; set_uniform (l, 1, 2, 'i', v); }
void glUniform3i (GLint l, GLint x, GLint y, GLint z)			{ GLint v[] = {x, y, z}; set_uniform (l, 1, 3, 'i', v); }
void glUniform4i (GLint l, GLint x, GLint y, GLint z, GLint w)		{ GLint v[] = {x, y, z, w}; set_uniform (l, 1, 4, 'i', v); }

static void uniform_matrix (GLint l, GLsizei c, GLboolean transpose, unsigned comps, const GLfloat *v)
{
	if (transpose)
	{
		ERROR (GL_INVALID_VALUE);	/* GL ES 2.0 */
	}
	set_uniform (l, c, comps, 'm', v);
}

void glUniformMatrix2fv (GLint l, GLsizei c, GLboolean t, const GLfloat *v)	{ uniform_matrix (l, c, t, 4, v); }
void glUniformMatrix3fv (GLint l, GLsizei c, GLboolean t, const GLfloat *v)	{ uniform_matrix (l, c, t, 9, v); }
void glUniformMatrix4fv (GLint l, GLsizei c, GLboolean t, const GLfloat *v)	{ uniform_matrix (l, c, t, 16, v); }

/* one element of a uniform; NULL on error */
static const pgpu_uniform_info_t *get_uniform (GLuint program, GLint location, object_t **pp,
					       uint32_t *element)
{
	object_t *p = linked_program (program);
	if (!p)
	{
		return NULL;
	}
	uint32_t index = (uint32_t) location >> 16;
	*element = location & 0xFFFF;
	if (location < 0 || index >= p->info->n_uniforms || *element >= (uint32_t) p->info->uniforms[index].size)
	{
		ERROR_RET (GL_INVALID_OPERATION, NULL);
	}
	*pp = p;
	return &p->info->uniforms[index];
}

void glGetUniformfv (GLuint program, GLint location, GLfloat *params)
{
	object_t *p;
	uint32_t element;
	const pgpu_uniform_info_t *u = get_uniform (program, location, &p, &element);
	if (!u)
	{
		return;
	}
	if (u->sampler >= 0)
	{
		params[0] = p->units[u->sampler + element];
		return;
	}
	const uint32_t *w = p->values + p->first[u - p->info->uniforms] + element * u->components;
	char kind = uniform_kind (u->type);
	for (unsigned i = 0; i < u->components; i++)
	{
		float f;
		memcpy (&f, &w[i], sizeof f);
		params[i] = kind == 'i' ? (float) (int32_t) w[i] : kind == 'b' ? (w[i] ? 1.0f : 0.0f) : f;
	}
}

void glGetUniformiv (GLuint program, GLint location, GLint *params)
{
	object_t *p;
	uint32_t element;
	const pgpu_uniform_info_t *u = get_uniform (program, location, &p, &element);
	if (!u)
	{
		return;
	}
	if (u->sampler >= 0)
	{
		params[0] = p->units[u->sampler + element];
		return;
	}
	const uint32_t *w = p->values + p->first[u - p->info->uniforms] + element * u->components;
	char kind = uniform_kind (u->type);
	for (unsigned i = 0; i < u->components; i++)
	{
		float f;
		memcpy (&f, &w[i], sizeof f);
		params[i] = kind == 'i' ? (GLint) w[i] : kind == 'b' ? (w[i] != 0) : (GLint) f;
	}
}

/* ---- vertex attributes ------------------------------------------------------------------------ */

void glEnableVertexAttribArray (GLuint index)
{
	if (index >= ATTRIBS)
	{
		ERROR (GL_INVALID_VALUE);
	}
	S.attribs[index].enabled = true;
}

void glDisableVertexAttribArray (GLuint index)
{
	if (index >= ATTRIBS)
	{
		ERROR (GL_INVALID_VALUE);
	}
	S.attribs[index].enabled = false;
}

static void set_pointer (array_t *a, GLint size, GLenum type, bool normalized, GLsizei stride,
			 const void *ptr)
{
	a->size = size;
	a->gl_type = type;
	a->normalized = normalized;
	a->type = map_array_type (type, normalized);
	a->stride = stride;
	a->pointer = ptr;
	a->buffer = S.array_buffer;
}

void glVertexAttribPointer (GLuint indx, GLint size, GLenum type, GLboolean normalized, GLsizei stride,
			    const void *ptr)
{
	if (indx >= ATTRIBS || size < 1 || size > 4 || stride < 0)
	{
		ERROR (GL_INVALID_VALUE);
	}
	if (map_array_type (type, normalized) < 0)
	{
		ERROR (GL_INVALID_ENUM);
	}
	set_pointer (&S.attribs[indx], size, type, normalized && type != GL_FLOAT && type != GL_FIXED,
		     stride, ptr);
}

static void vertex_attrib (GLuint indx, float x, float y, float z, float w)
{
	if (indx >= ATTRIBS)
	{
		ERROR (GL_INVALID_VALUE);
	}
	float *c = S.attrib_current[indx];
	c[0] = x;
	c[1] = y;
	c[2] = z;
	c[3] = w;
	pgpu_vertex_attrib (indx, x, y, z, w);
}

void glVertexAttrib1f (GLuint i, GLfloat x)				{ vertex_attrib (i, x, 0, 0, 1); }
void glVertexAttrib2f (GLuint i, GLfloat x, GLfloat y)			{ vertex_attrib (i, x, y, 0, 1); }
void glVertexAttrib3f (GLuint i, GLfloat x, GLfloat y, GLfloat z)	{ vertex_attrib (i, x, y, z, 1); }
void glVertexAttrib4f (GLuint i, GLfloat x, GLfloat y, GLfloat z, GLfloat w)	{ vertex_attrib (i, x, y, z, w); }
void glVertexAttrib1fv (GLuint i, const GLfloat *v)			{ vertex_attrib (i, v[0], 0, 0, 1); }
void glVertexAttrib2fv (GLuint i, const GLfloat *v)			{ vertex_attrib (i, v[0], v[1], 0, 1); }
void glVertexAttrib3fv (GLuint i, const GLfloat *v)			{ vertex_attrib (i, v[0], v[1], v[2], 1); }
void glVertexAttrib4fv (GLuint i, const GLfloat *v)			{ vertex_attrib (i, v[0], v[1], v[2], v[3]); }

void glGetVertexAttribiv (GLuint index, GLenum pname, GLint *params)
{
	if (index >= ATTRIBS)
	{
		ERROR (GL_INVALID_VALUE);
	}
	const array_t *a = &S.attribs[index];
	switch (pname)
	{
	case GL_VERTEX_ATTRIB_ARRAY_ENABLED:		*params = a->enabled;		break;
	case GL_VERTEX_ATTRIB_ARRAY_SIZE:		*params = a->size;		break;
	case GL_VERTEX_ATTRIB_ARRAY_STRIDE:		*params = a->stride;		break;
	case GL_VERTEX_ATTRIB_ARRAY_TYPE:		*params = a->gl_type;		break;
	case GL_VERTEX_ATTRIB_ARRAY_NORMALIZED:		*params = a->normalized;	break;
	case GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING:	*params = name_of (NAMES_BUFFER, a->buffer);	break;
	case GL_CURRENT_VERTEX_ATTRIB:
		for (int i = 0; i < 4; i++)
		{
			params[i] = (GLint) S.attrib_current[index][i];
		}
		break;
	default:
		ERROR (GL_INVALID_ENUM);
	}
}

void glGetVertexAttribfv (GLuint index, GLenum pname, GLfloat *params)
{
	if (index < ATTRIBS && pname == GL_CURRENT_VERTEX_ATTRIB)
	{
		memcpy (params, S.attrib_current[index], 4 * sizeof (float));
		return;
	}
	GLint v = 0;
	uint32_t errors = S.errors;
	S.errors = 0;
	glGetVertexAttribiv (index, pname, &v);
	if (!S.errors)
	{
		*params = (GLfloat) v;
	}
	S.errors |= errors;
}

void glGetVertexAttribPointerv (GLuint index, GLenum pname, void **pointer)
{
	if (index >= ATTRIBS)
	{
		ERROR (GL_INVALID_VALUE);
	}
	if (pname != GL_VERTEX_ATTRIB_ARRAY_POINTER)
	{
		ERROR (GL_INVALID_ENUM);
	}
	*pointer = (void *) S.attribs[index].pointer;
}

/* ---- drawing ------------------------------------------------------------------------------------- */

static uint32_t array_stride (const array_t *a)
{
	return a->stride ? (uint32_t) a->stride : a->size * type_bytes[a->type];
}

static void send_attrib_array (unsigned i, uint32_t buffer, uint32_t offset, uint32_t stride,
			       uint32_t size, uint32_t type)
{
	sent_array_t *s = &S.sent_attribs[i];
	if (   !s->valid || s->buffer != buffer || s->offset != offset || s->stride != stride
	    || s->size != size || s->type != type)
	{
		pgpu_attrib_array (i, buffer, offset, stride, size, type);
		*s = (sent_array_t) {true, buffer, offset, stride, size, type};
	}
}

static void send_ff_array (unsigned i, uint32_t buffer, uint32_t offset, uint32_t stride,
			   uint32_t size, uint32_t type)
{
	sent_array_t *s = &S.sent_ff[i];
	if (   !s->valid || s->buffer != buffer || s->offset != offset || s->stride != stride
	    || s->size != size || s->type != type)
	{
		pgpu_array (i, buffer, offset, stride, size, type);
		*s = (sent_array_t) {true, buffer, offset, stride, size, type};
	}
}

/* the texture each unit needs for this draw */
static void validate_textures (void)
{
	uint32_t want[UNITS];
	bool care[UNITS] = {false};

	if (S.program)
	{
		const object_t *p = object_at (S.program);
		for (uint32_t i = 0; i < p->info->n_uniforms; i++)
		{
			const pgpu_uniform_info_t *u = &p->info->uniforms[i];
			for (int e = 0; u->sampler >= 0 && e < u->size; e++)
			{
				unsigned unit = p->units[u->sampler + e];
				bool cube = u->type == GL_SAMPLER_CUBE;
				GLuint name = cube ? S.bound_cube[unit] : S.bound_2d[unit];
				name = name ? name : cube ? TEX_DEFAULT_CUBE : TEX_DEFAULT_2D;
				care[unit] = true;
				want[unit] = texture_usable (&S.textures[name]) ? name : 0;
			}
		}
	}
	else
	{
		GLuint name = S.bound_2d[0] ? S.bound_2d[0] : TEX_DEFAULT_2D;
		care[0] = true;
		want[0] = texture_usable (&S.textures[name]) ? name : 0;
	}

	for (unsigned unit = 0; unit < UNITS; unit++)
	{
		if (care[unit] && (!S.sent_units_valid[unit] || S.sent_units[unit] != want[unit]))
		{
			pgpu_texture_bind_unit (unit, want[unit]);
			S.sent_units[unit] = want[unit];
			S.sent_units_valid[unit] = true;
		}
	}
}

static void validate_matrices (void)
{
	if (S.matrix_dirty[0])
	{
		pgpu_load_matrix (PGPU_MODELVIEW, S.modelview[S.modelview_depth]);
	}
	if (S.matrix_dirty[1])
	{
		pgpu_load_matrix (PGPU_PROJECTION, S.projection[S.projection_depth]);
	}
	if (S.matrix_dirty[2])
	{
		pgpu_load_matrix (PGPU_TEXTURE, S.texture_matrix[S.texture_depth]);
	}
	S.matrix_dirty[0] = S.matrix_dirty[1] = S.matrix_dirty[2] = false;
}

static uint32_t read_index (const void *indices, uint32_t index_type, uint32_t i)
{
	return index_type == PGPU_INDEX_U16 ? ((const uint16_t *) indices)[i] : ((const uint8_t *) indices)[i];
}

static void index_range (const void *indices, uint32_t index_type, uint32_t count, uint32_t *lo, uint32_t *hi)
{
	*lo = 0xFFFFFFFF;
	*hi = 0;
	for (uint32_t i = 0; i < count; i++)
	{
		uint32_t v = read_index (indices, index_type, i);
		*lo = v < *lo ? v : *lo;
		*hi = v > *hi ? v : *hi;
	}
}

static uint32_t mode_step (GLenum mode)
{
	return mode == GL_TRIANGLES ? 3 : mode == GL_LINES ? 2 : mode == GL_POINTS ? 1 : 0;
}

/* a program draw too large for a PROGRAM_DRAW_INLINE packet: the client
   arrays (vertices lo .. hi) and the indices, rebased to lo, go to temporary
   buffers on the Zero (re-creating a buffer that earlier draws of the frame
   use keeps their data) */
static void draw_via_buffers (GLenum mode, GLint first, GLsizei count, const void *indices,
			      uint32_t index_type, uint32_t buffer_mask, uint32_t client_mask)
{
	uint32_t lo = first, hi = first + count - 1;
	if (indices)
	{
		index_range (indices, index_type, count, &lo, &hi);
		if (hi - lo > 65535)
		{
			ERROR (GL_OUT_OF_MEMORY);
		}
	}

	uint32_t offsets[ATTRIBS], total = 0;
	for (unsigned i = 0; i < ATTRIBS; i++)
	{
		const array_t *a = &S.attribs[i];
		if (client_mask & (1u << i))
		{
			offsets[i] = total;
			total += ((hi - lo) * array_stride (a) + a->size * type_bytes[a->type] + 3) & ~3u;
		}
	}
	pgpu_buffer_create (HW_TEMP_VERTEX, total);
	for (unsigned i = 0; i < ATTRIBS; i++)
	{
		const array_t *a = &S.attribs[i];
		uint32_t stride = array_stride (a);
		if (client_mask & (1u << i))
		{
			pgpu_buffer_data (HW_TEMP_VERTEX, offsets[i], (const uint8_t *) a->pointer + lo * stride,
					  (hi - lo) * stride + a->size * type_bytes[a->type]);
			send_attrib_array (i, HW_TEMP_VERTEX, offsets[i], stride, a->size, a->type);
		}
		else if (buffer_mask & (1u << i))
		{
			send_attrib_array (i, a->buffer, (uint32_t) (uintptr_t) a->pointer + lo * stride, stride,
					   a->size, a->type);
		}
	}
	pgpu_attribs_enable (buffer_mask | client_mask);
	S.sent_attribs_mask = buffer_mask | client_mask;
	S.sent_attribs_mask_valid = true;

	if (!indices)
	{
		pgpu_draw_arrays (mode, 0, count);
		return;
	}
	uint32_t index_bytes = index_type + 1;
	pgpu_buffer_create (HW_TEMP_INDEX, count * index_bytes);
	static union { uint8_t u8[2048]; uint16_t u16[1024]; } part;
	uint32_t per_part = sizeof part / index_bytes;
	for (uint32_t c = 0; c < (uint32_t) count; c += per_part)
	{
		uint32_t n = count - c < per_part ? count - c : per_part;
		for (uint32_t i = 0; i < n; i++)
		{
			uint32_t v = read_index (indices, index_type, c + i) - lo;
			if (index_type == PGPU_INDEX_U16)
			{
				part.u16[i] = v;
			}
			else
			{
				part.u8[i] = v;
			}
		}
		pgpu_buffer_data (HW_TEMP_INDEX, c * index_bytes, &part, n * index_bytes);
	}
	pgpu_draw_elements (mode, count, index_type, HW_TEMP_INDEX, 0);
}

/* a draw with a program: buffers directly; client-side arrays and indices
   with PROGRAM_DRAW_INLINE, where buffer arrays start at the first vertex sent */
static void draw_program (GLenum mode, GLint first, GLsizei count, bool indexed, uint32_t index_type,
			  const void *indices)
{
	const object_t *p = object_at (S.program);
	uint32_t used = 0, buffer_mask = 0, client_mask = 0;
	for (uint32_t i = 0; i < p->info->n_attribs; i++)
	{
		/* a matrix takes a location per column */
		const pgpu_attrib_info_t *a = &p->info->attribs[i];
		unsigned columns =   a->type == GL_FLOAT_MAT2 ? 2 : a->type == GL_FLOAT_MAT3 ? 3
				   : a->type == GL_FLOAT_MAT4 ? 4 : 1;
		for (unsigned c = 0; c < columns && a->location + c < ATTRIBS; c++)
		{
			used |= 1u << (a->location + c);
		}
	}
	for (unsigned i = 0; i < ATTRIBS; i++)
	{
		const array_t *a = &S.attribs[i];
		if (!(used & (1u << i)) || !a->enabled)
		{
			continue;
		}
		if (a->buffer)
		{
			buffer_mask |= 1u << i;		/* strides over 255: the Zero copies */
		}
		else
		{
			client_mask |= 1u << i;
		}
	}
	if (!S.sent_attribs_mask_valid || S.sent_attribs_mask != buffer_mask)
	{
		pgpu_attribs_enable (buffer_mask);
		S.sent_attribs_mask = buffer_mask;
		S.sent_attribs_mask_valid = true;
	}

	/* buffer arrays, with vertex `base` as vertex 0 */
	#define SET_BUFFER_ARRAYS(base) \
		for (unsigned i = 0; i < ATTRIBS; i++) \
		{ \
			const array_t *a = &S.attribs[i]; \
			if (buffer_mask & (1u << i)) \
			{ \
				send_attrib_array (i, a->buffer, \
						   (uint32_t) (uintptr_t) a->pointer + (base) * array_stride (a), \
						   array_stride (a), a->size, a->type); \
			} \
		}

	const void *client_indices = NULL;
	if (indexed)
	{
		if (!S.element_buffer)
		{
			client_indices = indices;
		}
		else if (client_mask)
		{
			const buffer_t *b = &S.buffers[S.element_buffer];
			uint32_t offset = (uint32_t) (uintptr_t) indices;
			if (!b->shadow || offset + (uint64_t) count * (index_type + 1) > (uint64_t) b->size)
			{
				ERROR (GL_INVALID_OPERATION);
			}
			client_indices = b->shadow + offset;
		}
		else
		{
			SET_BUFFER_ARRAYS (0);
			pgpu_draw_elements (mode, count, index_type, S.element_buffer, (uint32_t) (uintptr_t) indices);
			return;
		}
	}
	else if (!client_mask)
	{
		SET_BUFFER_ARRAYS (0);
		pgpu_draw_arrays (mode, first, count);
		return;
	}

	for (unsigned i = 0; i < ATTRIBS; i++)
	{
		const array_t *a = &S.attribs[i];
		pgpu_client_attrib_pointer (i, a->size, a->type, array_stride (a),
					    client_mask & (1u << i) ? a->pointer : NULL);
	}

	uint32_t step = mode_step (mode);
	if (!indexed)
	{
		/* pgpu's own limit: past it pgpu would split, and the buffer
		   arrays (set per chunk below) would not follow */
		uint32_t max = pgpu_client_max_vertices ();
		uint32_t chunk = count;
		if ((uint32_t) count > max)
		{
			if (!step)
			{
				/* strips, fans, loops that don't fit a packet */
				draw_via_buffers (mode, first, count, NULL, 0, buffer_mask, client_mask);
				return;
			}
			chunk = max - max % step;
		}
		for (uint32_t c = 0; c < (uint32_t) count; c += chunk)
		{
			uint32_t n = count - c < chunk ? count - c : chunk;
			SET_BUFFER_ARRAYS (first + c);
			pgpu_draw_arrays_client (mode, first + c, n);
		}
		return;
	}

	uint32_t chunk = step && count > 3000 ? 3000 : count;
	uint32_t index_bytes = index_type + 1;
	for (uint32_t c = 0; c < (uint32_t) count; c += chunk)
	{
		uint32_t n = count - c < chunk ? count - c : chunk;
		const uint8_t *part = (const uint8_t *) client_indices + c * index_bytes;
		uint32_t lo, hi;
		index_range (part, index_type, n, &lo, &hi);
		SET_BUFFER_ARRAYS (lo);
		if (!pgpu_draw_elements_client (mode, n, index_type, part))
		{
			/* the vertices don't fit a packet */
			draw_via_buffers (mode, 0, n, part, index_type, buffer_mask, client_mask);
		}
	}
	#undef SET_BUFFER_ARRAYS
}

static void stream_reserve (unsigned which, uint32_t bytes)
{
	if (bytes > S.stream_bytes[which])
	{
		uint32_t size = 4096;
		while (size < bytes)
		{
			size *= 2;
		}
		pgpu_buffer_create (which ? HW_STREAM_INDEX : HW_STREAM_VERTEX, size);
		S.stream_bytes[which] = size;
	}
}

/* fixed function: client arrays and indices go to stream buffers (the Zero
   reads fixed-function vertex data when the draw arrives) */
static void draw_ff (GLenum mode, GLint first, GLsizei count, bool indexed, uint32_t index_type,
		     const void *indices)
{
	validate_matrices ();

	uint32_t mask = 0, client_mask = 0;
	for (unsigned i = 0; i < 4; i++)
	{
		if (S.ff_arrays[i].enabled)
		{
			mask |= 1u << i;
			client_mask |= S.ff_arrays[i].buffer ? 0 : 1u << i;
		}
	}
	if (!(mask & (1u << PGPU_ATTR_POSITION)))
	{
		return;				/* GL draws nothing without the vertex array */
	}
	if (!S.sent_ff_mask_valid || S.sent_ff_mask != mask)
	{
		pgpu_arrays_enable (mask);
		S.sent_ff_mask = mask;
		S.sent_ff_mask_valid = true;
	}

	const void *client_indices = NULL;
	uint32_t lo = first, hi = first + count - 1;
	if (indexed)
	{
		if (!S.element_buffer)
		{
			client_indices = indices;
		}
		else if (client_mask)
		{
			const buffer_t *b = &S.buffers[S.element_buffer];
			uint32_t offset = (uint32_t) (uintptr_t) indices;
			if (!b->shadow || offset + (uint64_t) count * (index_type + 1) > (uint64_t) b->size)
			{
				ERROR (GL_INVALID_OPERATION);
			}
			client_indices = b->shadow + offset;
		}
		if (client_mask)
		{
			index_range (client_indices, index_type, count, &lo, &hi);
		}
		else
		{
			lo = 0;
		}
	}

	/* client arrays: the vertices lo .. hi; arrays in the same memory
	   (interleaved) are sent once */
	uint32_t stream_offset = 0;
	struct { const uint8_t *start, *end; uint32_t offset; } groups[4];
	unsigned n_groups = 0;
	for (unsigned i = 0; i < 4; i++)
	{
		const array_t *a = &S.ff_arrays[i];
		if (!(client_mask & (1u << i)))
		{
			continue;
		}
		uint32_t stride = array_stride (a);
		const uint8_t *start = (const uint8_t *) a->pointer + lo * stride;
		const uint8_t *end = (const uint8_t *) a->pointer + hi * stride + a->size * type_bytes[a->type];
		unsigned g;
		for (g = 0; g < n_groups; g++)
		{
			if (start < groups[g].end && end > groups[g].start)
			{
				break;
			}
		}
		if (g == n_groups)
		{
			groups[n_groups].start = start;
			groups[n_groups].end = end;
			n_groups++;
		}
		else
		{
			groups[g].start = start < groups[g].start ? start : groups[g].start;
			groups[g].end = end > groups[g].end ? end : groups[g].end;
		}
	}
	/* merging may have made groups overlap: merge again */
	for (unsigned g = 0; g < n_groups; g++)
	{
		for (unsigned h = g + 1; h < n_groups; h++)
		{
			if (groups[h].start < groups[g].end && groups[h].end > groups[g].start)
			{
				groups[g].start = groups[h].start < groups[g].start ? groups[h].start : groups[g].start;
				groups[g].end = groups[h].end > groups[g].end ? groups[h].end : groups[g].end;
				groups[h] = groups[--n_groups];
				h = g;
			}
		}
	}
	for (unsigned g = 0; g < n_groups; g++)
	{
		groups[g].offset = stream_offset;
		stream_offset += (groups[g].end - groups[g].start + 3) & ~3u;
	}
	if (n_groups)
	{
		stream_reserve (0, stream_offset);
		for (unsigned g = 0; g < n_groups; g++)
		{
			pgpu_buffer_data (HW_STREAM_VERTEX, groups[g].offset, groups[g].start,
					  groups[g].end - groups[g].start);
		}
	}

	for (unsigned i = 0; i < 4; i++)
	{
		const array_t *a = &S.ff_arrays[i];
		if (!(mask & (1u << i)))
		{
			continue;
		}
		uint32_t stride = array_stride (a);
		if (a->buffer)
		{
			uint32_t base = client_mask ? lo : 0;
			send_ff_array (i, a->buffer, (uint32_t) (uintptr_t) a->pointer + base * stride, stride,
				       a->size, a->type);
			continue;
		}
		const uint8_t *start = (const uint8_t *) a->pointer + lo * stride;
		for (unsigned g = 0; g < n_groups; g++)
		{
			if (start >= groups[g].start && start < groups[g].end)
			{
				send_ff_array (i, HW_STREAM_VERTEX, groups[g].offset + (start - groups[g].start),
					       stride, a->size, a->type);
			}
		}
	}

	if (!indexed)
	{
		pgpu_draw_arrays (mode, client_mask ? 0 : first, count);
		return;
	}
	if (!client_indices)
	{
		pgpu_draw_elements (mode, count, index_type, S.element_buffer, (uint32_t) (uintptr_t) indices);
		return;
	}

	/* the indices, rebased to lo when the vertices were sent from lo */
	uint32_t index_bytes = index_type + 1;
	stream_reserve (1, count * index_bytes);
	static union { uint8_t u8[2048]; uint16_t u16[1024]; } part;
	uint32_t per_part = sizeof part / index_bytes;
	for (uint32_t c = 0; c < (uint32_t) count; c += per_part)
	{
		uint32_t n = count - c < per_part ? count - c : per_part;
		for (uint32_t i = 0; i < n; i++)
		{
			uint32_t v = read_index (client_indices, index_type, c + i) - (client_mask ? lo : 0);
			if (index_type == PGPU_INDEX_U16)
			{
				part.u16[i] = v;
			}
			else
			{
				part.u8[i] = v;
			}
		}
		pgpu_buffer_data (HW_STREAM_INDEX, c * index_bytes, &part, n * index_bytes);
	}
	pgpu_draw_elements (mode, count, index_type, HW_STREAM_INDEX, 0);
}

static bool begin_draw (GLenum mode, GLsizei count)
{
	if (mode > GL_TRIANGLE_FAN)
	{
		ERROR_RET (GL_INVALID_ENUM, false);
	}
	if (count < 0)
	{
		ERROR_RET (GL_INVALID_VALUE, false);
	}
	if (!validate_framebuffer ())
	{
		return false;
	}
	if (count == 0)
	{
		return false;
	}
	validate_caps ();
	validate_textures ();
	return true;
}

/* GL has no vertex limit per draw; the Zero takes 65535 vertices. Lists and
   strips are split (strips overlapping, triangle strips at even vertices to
   keep the winding); fans and loops would need their first vertex repeated. */
#define MAX_DRAW_VERTICES	65535

static bool split_draw (GLenum mode, uint32_t *chunk, uint32_t *overlap)
{
	switch (mode)
	{
	case GL_POINTS:		*chunk = 65535; *overlap = 0; return true;
	case GL_LINES:		*chunk = 65534; *overlap = 0; return true;
	case GL_TRIANGLES:	*chunk = 65535; *overlap = 0; return true;
	case GL_LINE_STRIP:	*chunk = 65535; *overlap = 1; return true;
	case GL_TRIANGLE_STRIP:	*chunk = 65534; *overlap = 2; return true;
	default:		return false;
	}
}

static void draw (GLenum mode, GLint first, GLsizei count, bool indexed, uint32_t index_type,
		  const void *indices)
{
	if (S.program)
	{
		draw_program (mode, first, count, indexed, index_type, indices);
	}
	else
	{
		draw_ff (mode, first, count, indexed, index_type, indices);
	}
}

static void draw_split (GLenum mode, GLint first, GLsizei count, bool indexed, uint32_t index_type,
			const void *indices)
{
	if (count <= MAX_DRAW_VERTICES)
	{
		draw (mode, first, count, indexed, index_type, indices);
		return;
	}
	uint32_t chunk, overlap;
	if (!split_draw (mode, &chunk, &overlap))
	{
		ERROR (GL_OUT_OF_MEMORY);
	}
	uint32_t index_bytes = index_type + 1;
	for (uint32_t start = 0; ; start += chunk - overlap)
	{
		uint32_t n = count - start < chunk ? count - start : chunk;
		if (indexed)
		{
			/* a client pointer or an offset into the index buffer */
			draw (mode, 0, n, true, index_type, (const uint8_t *) indices + start * index_bytes);
		}
		else
		{
			draw (mode, first + start, n, false, 0, NULL);
		}
		if (start + n >= (uint32_t) count)
		{
			break;
		}
	}
}

void glDrawArrays (GLenum mode, GLint first, GLsizei count)
{
	if (first < 0)
	{
		ERROR (GL_INVALID_VALUE);
	}
	if (!begin_draw (mode, count))
	{
		return;
	}
	draw_split (mode, first, count, false, 0, NULL);
}

void glDrawElements (GLenum mode, GLsizei count, GLenum type, const void *indices)
{
	if (type != GL_UNSIGNED_BYTE && type != GL_UNSIGNED_SHORT)
	{
		ERROR (GL_INVALID_ENUM);
	}
	if (!begin_draw (mode, count))
	{
		return;
	}
	draw_split (mode, 0, count, true, type == GL_UNSIGNED_SHORT ? PGPU_INDEX_U16 : PGPU_INDEX_U8,
		    indices);
}

/* ---- GL ES 1.1: arrays -------------------------------------------------------------------------- */

static void ff_pointer (unsigned attribute, GLint size, GLenum type, GLsizei stride, const void *pointer,
			unsigned min_size, unsigned max_size, const GLenum *types, bool normalized)
{
	if (size < (GLint) min_size || size > (GLint) max_size || stride < 0)
	{
		ERROR (GL_INVALID_VALUE);
	}
	bool ok = false;
	for (const GLenum *t = types; *t; t++)
	{
		ok |= *t == type;
	}
	if (!ok)
	{
		ERROR (GL_INVALID_ENUM);
	}
	set_pointer (&S.ff_arrays[attribute], size, type, normalized && type != GL_FLOAT && type != GL_FIXED,
		     stride, pointer);
}

void glVertexPointer (GLint size, GLenum type, GLsizei stride, const void *pointer)
{
	static const GLenum types[] = {GL_BYTE, GL_SHORT, GL_FIXED, GL_FLOAT, 0};
	ff_pointer (PGPU_ATTR_POSITION, size, type, stride, pointer, 2, 4, types, false);
}

void glColorPointer (GLint size, GLenum type, GLsizei stride, const void *pointer)
{
	static const GLenum types[] = {GL_UNSIGNED_BYTE, GL_FIXED, GL_FLOAT, 0};
	ff_pointer (PGPU_ATTR_COLOR, size, type, stride, pointer, 4, 4, types, true);
}

void glNormalPointer (GLenum type, GLsizei stride, const void *pointer)
{
	static const GLenum types[] = {GL_BYTE, GL_SHORT, GL_FIXED, GL_FLOAT, 0};
	ff_pointer (PGPU_ATTR_NORMAL, 3, type, stride, pointer, 3, 3, types, true);
}

void glTexCoordPointer (GLint size, GLenum type, GLsizei stride, const void *pointer)
{
	static const GLenum types[] = {GL_BYTE, GL_SHORT, GL_FIXED, GL_FLOAT, 0};
	if (S.client_active_unit != 0)
	{
		return;				/* one fixed-function texture unit */
	}
	ff_pointer (PGPU_ATTR_TEXCOORD, size, type, stride, pointer, 2, 4, types, false);
}

void glClientActiveTexture (GLenum texture)
{
	if (texture != GL_TEXTURE0)
	{
		ERROR (GL_INVALID_ENUM);	/* GL_MAX_TEXTURE_UNITS is 1 */
	}
	S.client_active_unit = 0;
}

/* ---- GL ES 1.1: current values ------------------------------------------------------------------ */

void glColor4f (GLfloat r, GLfloat g, GLfloat b, GLfloat a)
{
	S.current_color[0] = r;
	S.current_color[1] = g;
	S.current_color[2] = b;
	S.current_color[3] = a;
	pgpu_color (color_u32 (S.current_color));
}

void glColor4ub (GLubyte r, GLubyte g, GLubyte b, GLubyte a)
{
	glColor4f (r / 255.0f, g / 255.0f, b / 255.0f, a / 255.0f);
}

void glNormal3f (GLfloat nx, GLfloat ny, GLfloat nz)
{
	S.current_normal[0] = nx;
	S.current_normal[1] = ny;
	S.current_normal[2] = nz;
	pgpu_normal (nx, ny, nz);
}

void glMultiTexCoord4f (GLenum target, GLfloat s, GLfloat t, GLfloat r, GLfloat q)
{
	if (target != GL_TEXTURE0)
	{
		ERROR (GL_INVALID_ENUM);
	}
	S.current_texcoord[0] = s;
	S.current_texcoord[1] = t;
	S.current_texcoord[2] = r;
	S.current_texcoord[3] = q;
	pgpu_texcoord (s, t);
}

/* ---- GL ES 1.1: matrices ------------------------------------------------------------------------- */

static float *current_matrix (void)
{
	switch (S.matrix_mode)
	{
	case GL_PROJECTION:	return S.projection[S.projection_depth];
	case GL_TEXTURE:	return S.texture_matrix[S.texture_depth];
	default:		return S.modelview[S.modelview_depth];
	}
}

static void matrix_changed (void)
{
	S.matrix_dirty[S.matrix_mode == GL_MODELVIEW ? 0 : S.matrix_mode == GL_PROJECTION ? 1 : 2] = true;
}

void glMatrixMode (GLenum mode)
{
	if (mode != GL_MODELVIEW && mode != GL_PROJECTION && mode != GL_TEXTURE)
	{
		ERROR (GL_INVALID_ENUM);
	}
	S.matrix_mode = mode;
}

void glLoadIdentity (void)
{
	mat_identity (current_matrix ());
	matrix_changed ();
}

void glLoadMatrixf (const GLfloat *m)
{
	memcpy (current_matrix (), m, 16 * sizeof (float));
	matrix_changed ();
}

void glMultMatrixf (const GLfloat *m)
{
	float *c = current_matrix ();
	mat_multiply (c, c, m);
	matrix_changed ();
}

void glPushMatrix (void)
{
	int *depth, max;
	float (*stack)[16];
	switch (S.matrix_mode)
	{
	case GL_PROJECTION:	depth = &S.projection_depth; max = PROJ_STACK; stack = S.projection;	break;
	case GL_TEXTURE:	depth = &S.texture_depth; max = TEX_STACK; stack = S.texture_matrix;	break;
	default:		depth = &S.modelview_depth; max = MV_STACK; stack = S.modelview;	break;
	}
	if (*depth + 1 >= max)
	{
		ERROR (GL_STACK_OVERFLOW);
	}
	memcpy (stack[*depth + 1], stack[*depth], sizeof stack[0]);
	(*depth)++;
}

void glPopMatrix (void)
{
	int *depth;
	switch (S.matrix_mode)
	{
	case GL_PROJECTION:	depth = &S.projection_depth;	break;
	case GL_TEXTURE:	depth = &S.texture_depth;	break;
	default:		depth = &S.modelview_depth;	break;
	}
	if (*depth == 0)
	{
		ERROR (GL_STACK_UNDERFLOW);
	}
	(*depth)--;
	matrix_changed ();
}

void glTranslatef (GLfloat x, GLfloat y, GLfloat z)
{
	float m[16];
	mat_identity (m);
	m[12] = x;
	m[13] = y;
	m[14] = z;
	glMultMatrixf (m);
}

void glScalef (GLfloat x, GLfloat y, GLfloat z)
{
	float m[16];
	mat_identity (m);
	m[0] = x;
	m[5] = y;
	m[10] = z;
	glMultMatrixf (m);
}

void glRotatef (GLfloat angle, GLfloat x, GLfloat y, GLfloat z)
{
	float len = sqrtf (x * x + y * y + z * z);
	if (len == 0.0f)
	{
		return;
	}
	x /= len;
	y /= len;
	z /= len;
	float a = angle * (float) M_PI / 180.0f, c = cosf (a), s = sinf (a), t = 1.0f - c;
	float m[16] = {
		x * x * t + c,     y * x * t + z * s, x * z * t - y * s, 0,
		x * y * t - z * s, y * y * t + c,     y * z * t + x * s, 0,
		x * z * t + y * s, y * z * t - x * s, z * z * t + c,     0,
		0, 0, 0, 1
	};
	glMultMatrixf (m);
}

void glOrthof (GLfloat l, GLfloat r, GLfloat b, GLfloat t, GLfloat n, GLfloat f)
{
	if (l == r || b == t || n == f)
	{
		ERROR (GL_INVALID_VALUE);
	}
	float m[16] = {0};
	m[0] = 2.0f / (r - l);
	m[5] = 2.0f / (t - b);
	m[10] = -2.0f / (f - n);
	m[12] = -(r + l) / (r - l);
	m[13] = -(t + b) / (t - b);
	m[14] = -(f + n) / (f - n);
	m[15] = 1.0f;
	glMultMatrixf (m);
}

void glFrustumf (GLfloat l, GLfloat r, GLfloat b, GLfloat t, GLfloat n, GLfloat f)
{
	if (n <= 0.0f || f <= 0.0f || l == r || b == t || n == f)
	{
		ERROR (GL_INVALID_VALUE);
	}
	float m[16] = {0};
	m[0] = 2.0f * n / (r - l);
	m[5] = 2.0f * n / (t - b);
	m[8] = (r + l) / (r - l);
	m[9] = (t + b) / (t - b);
	m[10] = -(f + n) / (f - n);
	m[11] = -1.0f;
	m[14] = -2.0f * f * n / (f - n);
	glMultMatrixf (m);
}

/* ---- GL ES 1.1: lighting, fog, texture environment --------------------------------------------- */

static void send_light (int i)
{
	const light_t *l = &S.lights[i];
	pgpu_light (i, l->position, color_u32 (l->ambient), color_u32 (l->diffuse),
		    color_u32 (l->specular), l->attenuation);
}

void glLightfv (GLenum light, GLenum pname, const GLfloat *params)
{
	if (light < GL_LIGHT0 || light >= GL_LIGHT0 + MAX_LIGHTS)
	{
		ERROR (GL_INVALID_ENUM);
	}
	light_t *l = &S.lights[light - GL_LIGHT0];
	switch (pname)
	{
	case GL_AMBIENT:	memcpy (l->ambient, params, 4 * sizeof (float));	break;
	case GL_DIFFUSE:	memcpy (l->diffuse, params, 4 * sizeof (float));	break;
	case GL_SPECULAR:	memcpy (l->specular, params, 4 * sizeof (float));	break;
	case GL_POSITION:	mat_transform (l->position, S.modelview[S.modelview_depth], params);	break;
	case GL_SPOT_DIRECTION: {
		float d[4] = {params[0], params[1], params[2], 0.0f};
		mat_transform (d, S.modelview[S.modelview_depth], d);
		memcpy (l->spot_direction, d, 3 * sizeof (float));
		} return;				/* no spot lights on the Zero */
	case GL_SPOT_EXPONENT:
		if (params[0] < 0.0f || params[0] > 128.0f)
		{
			ERROR (GL_INVALID_VALUE);
		}
		l->spot_exponent = params[0];
		return;
	case GL_SPOT_CUTOFF:
		if ((params[0] < 0.0f || params[0] > 90.0f) && params[0] != 180.0f)
		{
			ERROR (GL_INVALID_VALUE);
		}
		l->spot_cutoff = params[0];
		return;
	case GL_CONSTANT_ATTENUATION:
	case GL_LINEAR_ATTENUATION:
	case GL_QUADRATIC_ATTENUATION:
		if (params[0] < 0.0f)
		{
			ERROR (GL_INVALID_VALUE);
		}
		l->attenuation[pname - GL_CONSTANT_ATTENUATION] = params[0];
		break;
	default:
		ERROR (GL_INVALID_ENUM);
	}
	send_light (light - GL_LIGHT0);
}

void glLightf (GLenum light, GLenum pname, GLfloat param)
{
	if (   pname != GL_SPOT_EXPONENT && pname != GL_SPOT_CUTOFF && pname != GL_CONSTANT_ATTENUATION
	    && pname != GL_LINEAR_ATTENUATION && pname != GL_QUADRATIC_ATTENUATION)
	{
		ERROR (GL_INVALID_ENUM);
	}
	glLightfv (light, pname, &param);
}

void glGetLightfv (GLenum light, GLenum pname, GLfloat *params)
{
	if (light < GL_LIGHT0 || light >= GL_LIGHT0 + MAX_LIGHTS)
	{
		ERROR (GL_INVALID_ENUM);
	}
	const light_t *l = &S.lights[light - GL_LIGHT0];
	switch (pname)
	{
	case GL_AMBIENT:		memcpy (params, l->ambient, 4 * sizeof (float));	break;
	case GL_DIFFUSE:		memcpy (params, l->diffuse, 4 * sizeof (float));	break;
	case GL_SPECULAR:		memcpy (params, l->specular, 4 * sizeof (float));	break;
	case GL_POSITION:		memcpy (params, l->position, 4 * sizeof (float));	break;
	case GL_SPOT_DIRECTION:		memcpy (params, l->spot_direction, 3 * sizeof (float));	break;
	case GL_SPOT_EXPONENT:		params[0] = l->spot_exponent;				break;
	case GL_SPOT_CUTOFF:		params[0] = l->spot_cutoff;				break;
	case GL_CONSTANT_ATTENUATION:
	case GL_LINEAR_ATTENUATION:
	case GL_QUADRATIC_ATTENUATION:	params[0] = l->attenuation[pname - GL_CONSTANT_ATTENUATION];	break;
	default:			ERROR (GL_INVALID_ENUM);
	}
}

static void send_material (void)
{
	pgpu_material (color_u32 (S.material_ambient), color_u32 (S.material_diffuse),
		       color_u32 (S.material_specular), color_u32 (S.material_emission),
		       S.material_shininess);
}

void glMaterialfv (GLenum face, GLenum pname, const GLfloat *params)
{
	if (face != GL_FRONT_AND_BACK)
	{
		ERROR (GL_INVALID_ENUM);
	}
	switch (pname)
	{
	case GL_AMBIENT:		memcpy (S.material_ambient, params, 4 * sizeof (float));	break;
	case GL_DIFFUSE:		memcpy (S.material_diffuse, params, 4 * sizeof (float));	break;
	case GL_AMBIENT_AND_DIFFUSE:
		memcpy (S.material_ambient, params, 4 * sizeof (float));
		memcpy (S.material_diffuse, params, 4 * sizeof (float));
		break;
	case GL_SPECULAR:		memcpy (S.material_specular, params, 4 * sizeof (float));	break;
	case GL_EMISSION:		memcpy (S.material_emission, params, 4 * sizeof (float));	break;
	case GL_SHININESS:
		if (params[0] < 0.0f || params[0] > 128.0f)
		{
			ERROR (GL_INVALID_VALUE);
		}
		S.material_shininess = params[0];
		break;
	default:
		ERROR (GL_INVALID_ENUM);
	}
	send_material ();
}

void glMaterialf (GLenum face, GLenum pname, GLfloat param)
{
	if (pname != GL_SHININESS)
	{
		ERROR (GL_INVALID_ENUM);
	}
	glMaterialfv (face, pname, &param);
}

void glGetMaterialfv (GLenum face, GLenum pname, GLfloat *params)
{
	if (face != GL_FRONT && face != GL_BACK)
	{
		ERROR (GL_INVALID_ENUM);
	}
	switch (pname)
	{
	case GL_AMBIENT:	memcpy (params, S.material_ambient, 4 * sizeof (float));	break;
	case GL_DIFFUSE:	memcpy (params, S.material_diffuse, 4 * sizeof (float));	break;
	case GL_SPECULAR:	memcpy (params, S.material_specular, 4 * sizeof (float));	break;
	case GL_EMISSION:	memcpy (params, S.material_emission, 4 * sizeof (float));	break;
	case GL_SHININESS:	params[0] = S.material_shininess;				break;
	default:		ERROR (GL_INVALID_ENUM);
	}
}

void glLightModelfv (GLenum pname, const GLfloat *params)
{
	switch (pname)
	{
	case GL_LIGHT_MODEL_AMBIENT:	memcpy (S.light_model_ambient, params, 4 * sizeof (float));	break;
	case GL_LIGHT_MODEL_TWO_SIDE:	S.light_model_two_side = params[0] != 0.0f;			break;
	default:			ERROR (GL_INVALID_ENUM);
	}
	pgpu_light_model (color_u32 (S.light_model_ambient), S.light_model_two_side);
}

void glLightModelf (GLenum pname, GLfloat param)
{
	if (pname != GL_LIGHT_MODEL_TWO_SIDE)
	{
		ERROR (GL_INVALID_ENUM);
	}
	glLightModelfv (pname, &param);
}

void glFogfv (GLenum pname, const GLfloat *params)
{
	switch (pname)
	{
	case GL_FOG_MODE: {
		GLenum mode = (GLenum) params[0];
		if (mode != GL_LINEAR && mode != GL_EXP && mode != GL_EXP2)
		{
			ERROR (GL_INVALID_ENUM);
		}
		S.fog_mode = mode;
		} break;
	case GL_FOG_DENSITY:
		if (params[0] < 0.0f)
		{
			ERROR (GL_INVALID_VALUE);
		}
		S.fog_density = params[0];
		break;
	case GL_FOG_START:	S.fog_start = params[0];				break;
	case GL_FOG_END:	S.fog_end = params[0];					break;
	case GL_FOG_COLOR:	memcpy (S.fog_color, params, 4 * sizeof (float));	break;
	default:		ERROR (GL_INVALID_ENUM);
	}
	pgpu_fog (S.fog_mode == GL_LINEAR ? PGPU_FOG_LINEAR : S.fog_mode == GL_EXP ? PGPU_FOG_EXP : PGPU_FOG_EXP2,
		  color_u32 (S.fog_color), S.fog_start, S.fog_end, S.fog_density);
}

void glFogf (GLenum pname, GLfloat param)
{
	if (pname == GL_FOG_COLOR)
	{
		ERROR (GL_INVALID_ENUM);
	}
	glFogfv (pname, &param);
}

void glShadeModel (GLenum mode)
{
	if (mode != GL_SMOOTH && mode != GL_FLAT)
	{
		ERROR (GL_INVALID_ENUM);
	}
	S.shade_model = mode;
	pgpu_shade_model (mode == GL_FLAT ? PGPU_FLAT : PGPU_SMOOTH);
}

void glAlphaFunc (GLenum func, GLclampf ref)
{
	int f = map_func (func);
	if (f < 0)
	{
		ERROR (GL_INVALID_ENUM);
	}
	S.alpha_func = func;
	S.alpha_ref = clampf (ref, 0, 1);
	pgpu_alpha_func (f, S.alpha_ref);
}

static void send_tex_env (void)
{
	uint32_t mode =   S.tex_env_mode == GL_REPLACE ? PGPU_REPLACE
			: S.tex_env_mode == GL_DECAL ? PGPU_DECAL
			: S.tex_env_mode == GL_BLEND ? PGPU_BLEND : PGPU_MODULATE;
	pgpu_tex_env (mode, color_u32 (S.tex_env_color));
}

void glTexEnvfv (GLenum target, GLenum pname, const GLfloat *params)
{
	if (target != GL_TEXTURE_ENV)
	{
		ERROR (GL_INVALID_ENUM);
	}
	switch (pname)
	{
	case GL_TEXTURE_ENV_MODE: {
		/* ADD and COMBINE aren't supported by the Zero */
		GLenum mode = (GLenum) params[0];
		if (mode != GL_MODULATE && mode != GL_REPLACE && mode != GL_DECAL && mode != GL_BLEND)
		{
			ERROR (GL_INVALID_ENUM);
		}
		S.tex_env_mode = mode;
		} break;
	case GL_TEXTURE_ENV_COLOR:
		memcpy (S.tex_env_color, params, 4 * sizeof (float));
		break;
	default:
		ERROR (GL_INVALID_ENUM);
	}
	if (S.active_unit == 0)
	{
		send_tex_env ();
	}
}

void glTexEnvf (GLenum target, GLenum pname, GLfloat param)
{
	if (pname == GL_TEXTURE_ENV_COLOR)
	{
		ERROR (GL_INVALID_ENUM);
	}
	glTexEnvfv (target, pname, &param);
}

void glTexEnvi (GLenum target, GLenum pname, GLint param)
{
	glTexEnvf (target, pname, (GLfloat) param);
}

void glGetTexEnvfv (GLenum env, GLenum pname, GLfloat *params)
{
	if (env != GL_TEXTURE_ENV)
	{
		ERROR (GL_INVALID_ENUM);
	}
	switch (pname)
	{
	case GL_TEXTURE_ENV_MODE:	params[0] = (GLfloat) S.tex_env_mode;			break;
	case GL_TEXTURE_ENV_COLOR:	memcpy (params, S.tex_env_color, 4 * sizeof (float));	break;
	default:			ERROR (GL_INVALID_ENUM);
	}
}

/* ---- glGet ------------------------------------------------------------------------------------- */

enum { K_INT, K_FLOAT, K_BOOL, K_NORM };	/* K_NORM: colours and depths, 0 .. 1 */

static void copy_values (double *v, const float *src, int n)
{
	for (int i = 0; i < n; i++)
	{
		v[i] = src[i];
	}
}

/* values of a state variable; 0 = unknown pname (double: 32-bit masks exactly) */
static int get_state (GLenum pname, double *v, int *kind)
{
	*kind = K_INT;
	#define I1(a)		(v[0] = (a), 1)
	#define I2(a, b)	(v[0] = (a), v[1] = (b), 2)
	#define I4(a, b, c, d)	(v[0] = (a), v[1] = (b), v[2] = (c), v[3] = (d), 4)
	#define COPY(src, n)	(copy_values (v, src, n), (n))
	switch (pname)
	{
	/* bits of the bound framebuffer */
	case GL_RED_BITS:	return I1 (!S.framebuffer && !S.surface ? 5 : target_has_color () ? 8 : 0);
	case GL_GREEN_BITS:	return I1 (!S.framebuffer && !S.surface ? 6 : target_has_color () ? 8 : 0);
	case GL_BLUE_BITS:	return I1 (!S.framebuffer && !S.surface ? 5 : target_has_color () ? 8 : 0);
	case GL_ALPHA_BITS:	return I1 (target_has_alpha () ? 8 : 0);
	case GL_DEPTH_BITS:	return I1 (target_has_depth () ? 24 : 0);
	case GL_STENCIL_BITS:	return I1 (target_has_stencil () ? 8 : 0);
	case GL_SUBPIXEL_BITS:	return I1 (4);		/* 12.4 fixed point screen coordinates */
	case GL_SAMPLE_BUFFERS:	return I1 (0);
	case GL_SAMPLES:	return I1 (0);

	/* implementation limits */
	case GL_MAX_TEXTURE_SIZE:
	case GL_MAX_CUBE_MAP_TEXTURE_SIZE:
	case GL_MAX_RENDERBUFFER_SIZE:		return I1 (MAX_SIZE);
	case GL_MAX_VIEWPORT_DIMS:		return I2 (MAX_SIZE, MAX_SIZE);
	case GL_MAX_VERTEX_ATTRIBS:		return I1 (ATTRIBS);
	case GL_MAX_TEXTURE_IMAGE_UNITS:
	case GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS:	return I1 (UNITS);
	case GL_MAX_VERTEX_TEXTURE_IMAGE_UNITS:	return I1 (UNITS);
	/* uniform storage: 4096 words per program, both stages */
	case GL_MAX_VERTEX_UNIFORM_VECTORS:
	case GL_MAX_FRAGMENT_UNIFORM_VECTORS:	return I1 (512);
	case GL_MAX_VARYING_VECTORS:		return I1 (8);		/* Mesa vc4 */
	case GL_ALIASED_POINT_SIZE_RANGE:	*kind = K_FLOAT; return I2 (1, 512);	/* Mesa vc4 */
	case GL_ALIASED_LINE_WIDTH_RANGE:	*kind = K_FLOAT; return I2 (1, 32);
	case GL_MAX_LIGHTS:			return I1 (MAX_LIGHTS);
	case GL_MAX_CLIP_PLANES:		return I1 (0);
	case GL_MAX_TEXTURE_UNITS:		return I1 (1);
	case GL_MAX_MODELVIEW_STACK_DEPTH:	return I1 (MV_STACK);
	case GL_MAX_PROJECTION_STACK_DEPTH:	return I1 (PROJ_STACK);
	case GL_MAX_TEXTURE_STACK_DEPTH:	return I1 (TEX_STACK);
	case GL_NUM_COMPRESSED_TEXTURE_FORMATS:	return I1 (1);
	case GL_COMPRESSED_TEXTURE_FORMATS:	return I1 (GL_ETC1_RGB8_OES);
	case GL_NUM_SHADER_BINARY_FORMATS:	return I1 (1);
	case GL_SHADER_BINARY_FORMATS:		return I1 (PGL_SHADER_BINARY_PGPU);
	case GL_NUM_PROGRAM_BINARY_FORMATS_OES:	return I1 (1);
	case GL_PROGRAM_BINARY_FORMATS_OES:	return I1 (PGL_PROGRAM_BINARY_PGPU);
	case GL_SHADER_COMPILER:		*kind = K_BOOL; return I1 (pglc_available ());
	case GL_IMPLEMENTATION_COLOR_READ_FORMAT:	return I1 (GL_RGBA);
	case GL_IMPLEMENTATION_COLOR_READ_TYPE:	return I1 (GL_UNSIGNED_BYTE);

	/* bindings */
	case GL_ARRAY_BUFFER_BINDING:		return I1 (name_of (NAMES_BUFFER, S.array_buffer));
	case GL_ELEMENT_ARRAY_BUFFER_BINDING:	return I1 (name_of (NAMES_BUFFER, S.element_buffer));
	case GL_FRAMEBUFFER_BINDING:		return I1 (name_of (NAMES_FRAMEBUFFER, S.framebuffer));
	case GL_RENDERBUFFER_BINDING:		return I1 (name_of (NAMES_RENDERBUFFER, S.renderbuffer));
	case GL_CURRENT_PROGRAM:		return I1 (S.program);
	case GL_ACTIVE_TEXTURE:			return I1 (GL_TEXTURE0 + S.active_unit);
	case GL_CLIENT_ACTIVE_TEXTURE:		return I1 (GL_TEXTURE0 + S.client_active_unit);
	case GL_TEXTURE_BINDING_2D:		return I1 (name_of (NAMES_TEXTURE, S.bound_2d[S.active_unit]));
	case GL_TEXTURE_BINDING_CUBE_MAP:	return I1 (name_of (NAMES_TEXTURE, S.bound_cube[S.active_unit]));

	/* fragment state */
	case GL_VIEWPORT:		return I4 (S.viewport[0], S.viewport[1], S.viewport[2], S.viewport[3]);
	case GL_SCISSOR_BOX:		return I4 (S.scissor[0], S.scissor[1], S.scissor[2], S.scissor[3]);
	case GL_DEPTH_RANGE:		*kind = K_NORM; return COPY (S.depth_range, 2);
	case GL_COLOR_CLEAR_VALUE:	*kind = K_NORM; return COPY (S.clear_color, 4);
	case GL_DEPTH_CLEAR_VALUE:	*kind = K_NORM; return I1 (S.clear_depth);
	case GL_STENCIL_CLEAR_VALUE:	return I1 (S.clear_stencil);
	case GL_COLOR_WRITEMASK:	*kind = K_BOOL;
					return I4 (S.color_mask[0], S.color_mask[1], S.color_mask[2], S.color_mask[3]);
	case GL_DEPTH_WRITEMASK:	*kind = K_BOOL; return I1 (S.depth_mask);
	case GL_DEPTH_FUNC:		return I1 (S.depth_func);
	case GL_CULL_FACE_MODE:		return I1 (S.cull_face);
	case GL_FRONT_FACE:		return I1 (S.front_face);
	case GL_LINE_WIDTH:		*kind = K_FLOAT; return I1 (S.line_width);
	case GL_POLYGON_OFFSET_FACTOR:	*kind = K_FLOAT; return I1 (S.polygon_offset_factor);
	case GL_POLYGON_OFFSET_UNITS:	*kind = K_FLOAT; return I1 (S.polygon_offset_units);
	case GL_SAMPLE_COVERAGE_VALUE:	*kind = K_FLOAT; return I1 (S.sample_coverage_value);
	case GL_SAMPLE_COVERAGE_INVERT:	*kind = K_BOOL; return I1 (S.sample_coverage_invert);
	case GL_BLEND_SRC_RGB:		return I1 (S.blend_src_rgb);
	case GL_BLEND_DST_RGB:		return I1 (S.blend_dst_rgb);
	case GL_BLEND_SRC_ALPHA:	return I1 (S.blend_src_alpha);
	case GL_BLEND_DST_ALPHA:	return I1 (S.blend_dst_alpha);
	case GL_BLEND_EQUATION_RGB:	return I1 (S.blend_eq_rgb);
	case GL_BLEND_EQUATION_ALPHA:	return I1 (S.blend_eq_alpha);
	case GL_BLEND_COLOR:		*kind = K_NORM; return COPY (S.blend_color, 4);
	case GL_STENCIL_FUNC:		return I1 (S.stencil_func[0]);
	case GL_STENCIL_REF:		return I1 (S.stencil_ref[0]);
	case GL_STENCIL_VALUE_MASK:	return I1 (S.stencil_value_mask[0]);
	case GL_STENCIL_WRITEMASK:	return I1 (S.stencil_writemask[0]);
	case GL_STENCIL_FAIL:		return I1 (S.stencil_fail[0]);
	case GL_STENCIL_PASS_DEPTH_FAIL: return I1 (S.stencil_zfail[0]);
	case GL_STENCIL_PASS_DEPTH_PASS: return I1 (S.stencil_zpass[0]);
	case GL_STENCIL_BACK_FUNC:	return I1 (S.stencil_func[1]);
	case GL_STENCIL_BACK_REF:	return I1 (S.stencil_ref[1]);
	case GL_STENCIL_BACK_VALUE_MASK: return I1 (S.stencil_value_mask[1]);
	case GL_STENCIL_BACK_WRITEMASK:	return I1 (S.stencil_writemask[1]);
	case GL_STENCIL_BACK_FAIL:	return I1 (S.stencil_fail[1]);
	case GL_STENCIL_BACK_PASS_DEPTH_FAIL: return I1 (S.stencil_zfail[1]);
	case GL_STENCIL_BACK_PASS_DEPTH_PASS: return I1 (S.stencil_zpass[1]);
	case GL_GENERATE_MIPMAP_HINT:	return I1 (S.generate_mipmap_hint);
	case GL_PERSPECTIVE_CORRECTION_HINT: return I1 (S.perspective_hint);
	case GL_POINT_SMOOTH_HINT:	return I1 (S.point_smooth_hint);
	case GL_LINE_SMOOTH_HINT:	return I1 (S.line_smooth_hint);
	case GL_FOG_HINT:		return I1 (S.fog_hint);
	case GL_PACK_ALIGNMENT:		return I1 (S.pack_alignment);
	case GL_UNPACK_ALIGNMENT:	return I1 (S.unpack_alignment);

	/* fixed function */
	case GL_MATRIX_MODE:		return I1 (S.matrix_mode);
	case GL_MODELVIEW_MATRIX:	*kind = K_FLOAT; return COPY (S.modelview[S.modelview_depth], 16);
	case GL_PROJECTION_MATRIX:	*kind = K_FLOAT; return COPY (S.projection[S.projection_depth], 16);
	case GL_TEXTURE_MATRIX:		*kind = K_FLOAT; return COPY (S.texture_matrix[S.texture_depth], 16);
	case GL_MODELVIEW_STACK_DEPTH:	return I1 (S.modelview_depth + 1);
	case GL_PROJECTION_STACK_DEPTH:	return I1 (S.projection_depth + 1);
	case GL_TEXTURE_STACK_DEPTH:	return I1 (S.texture_depth + 1);
	case GL_CURRENT_COLOR:		*kind = K_NORM; return COPY (S.current_color, 4);
	case GL_CURRENT_NORMAL:		*kind = K_FLOAT; return COPY (S.current_normal, 3);
	case GL_CURRENT_TEXTURE_COORDS:	*kind = K_FLOAT; return COPY (S.current_texcoord, 4);
	case GL_SHADE_MODEL:		return I1 (S.shade_model);
	case GL_ALPHA_TEST_FUNC:	return I1 (S.alpha_func);
	case GL_ALPHA_TEST_REF:		*kind = K_NORM; return I1 (S.alpha_ref);
	case GL_FOG_MODE:		return I1 (S.fog_mode);
	case GL_FOG_DENSITY:		*kind = K_FLOAT; return I1 (S.fog_density);
	case GL_FOG_START:		*kind = K_FLOAT; return I1 (S.fog_start);
	case GL_FOG_END:		*kind = K_FLOAT; return I1 (S.fog_end);
	case GL_FOG_COLOR:		*kind = K_NORM; return COPY (S.fog_color, 4);
	case GL_LIGHT_MODEL_AMBIENT:	*kind = K_NORM; return COPY (S.light_model_ambient, 4);
	case GL_LIGHT_MODEL_TWO_SIDE:	*kind = K_BOOL; return I1 (S.light_model_two_side);
	case GL_VERTEX_ARRAY_SIZE:	return I1 (S.ff_arrays[PGPU_ATTR_POSITION].size);
	case GL_VERTEX_ARRAY_TYPE:	return I1 (S.ff_arrays[PGPU_ATTR_POSITION].gl_type);
	case GL_VERTEX_ARRAY_STRIDE:	return I1 (S.ff_arrays[PGPU_ATTR_POSITION].stride);
	case GL_VERTEX_ARRAY_BUFFER_BINDING: return I1 (name_of (NAMES_BUFFER, S.ff_arrays[PGPU_ATTR_POSITION].buffer));
	case GL_COLOR_ARRAY_SIZE:	return I1 (S.ff_arrays[PGPU_ATTR_COLOR].size);
	case GL_COLOR_ARRAY_TYPE:	return I1 (S.ff_arrays[PGPU_ATTR_COLOR].gl_type);
	case GL_COLOR_ARRAY_STRIDE:	return I1 (S.ff_arrays[PGPU_ATTR_COLOR].stride);
	case GL_COLOR_ARRAY_BUFFER_BINDING: return I1 (name_of (NAMES_BUFFER, S.ff_arrays[PGPU_ATTR_COLOR].buffer));
	case GL_NORMAL_ARRAY_TYPE:	return I1 (S.ff_arrays[PGPU_ATTR_NORMAL].gl_type);
	case GL_NORMAL_ARRAY_STRIDE:	return I1 (S.ff_arrays[PGPU_ATTR_NORMAL].stride);
	case GL_NORMAL_ARRAY_BUFFER_BINDING: return I1 (name_of (NAMES_BUFFER, S.ff_arrays[PGPU_ATTR_NORMAL].buffer));
	case GL_TEXTURE_COORD_ARRAY_SIZE: return I1 (S.ff_arrays[PGPU_ATTR_TEXCOORD].size);
	case GL_TEXTURE_COORD_ARRAY_TYPE: return I1 (S.ff_arrays[PGPU_ATTR_TEXCOORD].gl_type);
	case GL_TEXTURE_COORD_ARRAY_STRIDE: return I1 (S.ff_arrays[PGPU_ATTR_TEXCOORD].stride);
	case GL_TEXTURE_COORD_ARRAY_BUFFER_BINDING: return I1 (name_of (NAMES_BUFFER, S.ff_arrays[PGPU_ATTR_TEXCOORD].buffer));
	}

	/* enables */
	uint32_t bit = cap_bit (pname);
	bool *flag = cap_flag (pname);
	array_t *a = client_state_array (pname);
	if (bit || flag || a)
	{
		*kind = K_BOOL;
		return I1 (bit ? (S.caps & bit) != 0 : flag ? *flag : a->enabled);
	}
	return 0;
	#undef I1
	#undef I2
	#undef I4
	#undef COPY
}

void glGetFloatv (GLenum pname, GLfloat *params)
{
	double v[16];
	int kind, n = get_state (pname, v, &kind);
	if (!n)
	{
		ERROR (GL_INVALID_ENUM);
	}
	for (int i = 0; i < n; i++)
	{
		params[i] = (GLfloat) v[i];
	}
}

void glGetIntegerv (GLenum pname, GLint *params)
{
	double v[16];
	int kind, n = get_state (pname, v, &kind);
	if (!n)
	{
		ERROR (GL_INVALID_ENUM);
	}
	for (int i = 0; i < n; i++)
	{
		if (kind == K_NORM)
		{
			/* GL ES 2.0 6.1.2: [-1, 1] maps linearly to the integer range */
			double d = ((double) 0xFFFFFFFFu * v[i] - 1.0) / 2.0;
			params[i] = (GLint) (d >= 2147483647.0 ? 2147483647.0 : d);
		}
		else
		{
			params[i] = (GLint) (int64_t) llround (v[i]);	/* 32-bit masks wrap */
		}
	}
}

void glGetBooleanv (GLenum pname, GLboolean *params)
{
	double v[16];
	int kind, n = get_state (pname, v, &kind);
	if (!n)
	{
		ERROR (GL_INVALID_ENUM);
	}
	for (int i = 0; i < n; i++)
	{
		params[i] = v[i] != 0.0 ? GL_TRUE : GL_FALSE;
	}
}

const GLubyte *glGetString (GLenum name)
{
	switch (name)
	{
	case GL_VENDOR:				return (const GLubyte *) "pico-gpu";
	case GL_RENDERER:			return (const GLubyte *) "VideoCore IV V3D (Pi Zero, pgpu)";
	case GL_VERSION:			return (const GLubyte *) "OpenGL ES 2.0 pgl";
	case GL_SHADING_LANGUAGE_VERSION:	return (const GLubyte *) "OpenGL ES GLSL ES 1.00 (precompiled, tools/glslc)";
	case GL_EXTENSIONS:
		return (const GLubyte *) "GL_OES_get_program_binary GL_OES_compressed_ETC1_RGB8_texture "
					 "GL_OES_rgb8_rgba8 GL_OES_depth24 GL_OES_packed_depth_stencil "
					 "GL_EXT_texture_format_BGRA8888 GL_EXT_debug_marker";
	default:
		ERROR_RET (GL_INVALID_ENUM, NULL);
	}
}
