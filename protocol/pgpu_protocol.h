/*
 * pgpu_protocol.h - Pico -> Zero GPU command protocol, version 1
 *
 * Shared by the Zero (C++, Circle) and the Pico (C, Pico SDK).
 * Normative description: docs/protocol.md
 */
#ifndef PGPU_PROTOCOL_H
#define PGPU_PROTOCOL_H

#define PGPU_VERSION			0x00010000u	/* 1.0 */

/* ---- packet layer (docs/protocol.md section 4) ------------------------- */

#define PGPU_SYNC_COMMAND		0xA5u
#define PGPU_SYNC_REPLY			0x5Au
#define PGPU_MAX_PAYLOAD		16384u		/* words */
#define PGPU_IDLE_WORD			0x00000000u

#define PGPU_HEADER(sync, op, len)	(((unsigned) (sync) << 24) | ((unsigned) (op) << 16) | (unsigned) (len))
#define PGPU_HEADER_SYNC(h)		(((h) >> 24) & 0xFFu)
#define PGPU_HEADER_OP(h)		(((h) >> 16) & 0xFFu)
#define PGPU_HEADER_LEN(h)		((h) & 0xFFFFu)

/* ---- commands (section 7) ---------------------------------------------- */

enum pgpu_opcode
{
	/* system */
	PGPU_OP_RESET		= 0x01,
	PGPU_OP_GET_INFO	= 0x02,
	PGPU_OP_PING		= 0x03,
	PGPU_OP_GET_STATUS	= 0x04,

	/* frame */
	PGPU_OP_CLEAR		= 0x10,
	PGPU_OP_FRAME_END	= 0x11,
	PGPU_OP_VIEWPORT	= 0x12,
	PGPU_OP_FRAMEBUFFER_CREATE = 0x13,
	PGPU_OP_FRAMEBUFFER_DELETE = 0x14,
	PGPU_OP_BIND_FRAMEBUFFER = 0x15,
	PGPU_OP_READ_PIXELS	= 0x16,
	PGPU_OP_COPY_TEX_IMAGE	= 0x17,

	/* buffers */
	PGPU_OP_BUFFER_CREATE	= 0x20,
	PGPU_OP_BUFFER_DATA	= 0x21,
	PGPU_OP_BUFFER_DELETE	= 0x22,

	/* textures */
	PGPU_OP_TEXTURE_CREATE	= 0x28,
	PGPU_OP_TEXTURE_DATA	= 0x29,
	PGPU_OP_TEXTURE_PARAMS	= 0x2A,
	PGPU_OP_TEXTURE_DELETE	= 0x2B,
	PGPU_OP_TEXTURE_BIND	= 0x2C,
	PGPU_OP_TEX_ENV		= 0x2D,
	PGPU_OP_GENERATE_MIPMAP	= 0x2E,

	/* fragment state */
	PGPU_OP_ENABLE		= 0x30,
	PGPU_OP_DISABLE		= 0x31,
	PGPU_OP_DEPTH_FUNC	= 0x32,
	PGPU_OP_DEPTH_MASK	= 0x33,
	PGPU_OP_BLEND_FUNC	= 0x34,
	PGPU_OP_CULL_FACE	= 0x35,
	PGPU_OP_FRONT_FACE	= 0x36,
	PGPU_OP_ALPHA_FUNC	= 0x37,
	PGPU_OP_COLOR_MASK	= 0x38,
	PGPU_OP_SCISSOR		= 0x39,
	PGPU_OP_POLYGON_OFFSET	= 0x3A,
	PGPU_OP_LINE_WIDTH	= 0x3B,
	PGPU_OP_BLEND_FUNC_SEPARATE = 0x3C,
	PGPU_OP_BLEND_EQUATION	= 0x3D,
	PGPU_OP_BLEND_COLOR	= 0x3E,

	/* transform, lighting, fog, current values (0x40-0x4F) */
	PGPU_OP_LOAD_MATRIX	= 0x40,
	PGPU_OP_LIGHT		= 0x41,
	PGPU_OP_MATERIAL	= 0x42,
	PGPU_OP_LIGHT_MODEL	= 0x43,
	PGPU_OP_FOG		= 0x44,
	PGPU_OP_SHADE_MODEL	= 0x45,
	PGPU_OP_COLOR		= 0x46,
	PGPU_OP_NORMAL		= 0x47,
	PGPU_OP_TEXCOORD	= 0x48,

	/* arrays and drawing */
	PGPU_OP_ARRAY		= 0x50,
	PGPU_OP_ARRAYS_ENABLE	= 0x51,
	PGPU_OP_DRAW_ARRAYS	= 0x52,
	PGPU_OP_DRAW_ELEMENTS	= 0x53,
	PGPU_OP_DRAW_INLINE	= 0x54,

	/* stencil */
	PGPU_OP_STENCIL_FUNC	= 0x60,
	PGPU_OP_STENCIL_OP	= 0x61,
	PGPU_OP_STENCIL_MASK	= 0x62,

	/* programs (GL ES 2.0 subset) */
	PGPU_OP_PROGRAM_CREATE	= 0x80,
	PGPU_OP_PROGRAM_DATA	= 0x81,
	PGPU_OP_PROGRAM_DELETE	= 0x82,
	PGPU_OP_USE_PROGRAM	= 0x83,
	PGPU_OP_PROGRAM_UNIFORM	= 0x84,
	PGPU_OP_PROGRAM_SAMPLER	= 0x85,
	PGPU_OP_TEXTURE_BIND_UNIT = 0x86,
	PGPU_OP_VERTEX_ATTRIB	= 0x87,
	PGPU_OP_ATTRIB_ARRAY	= 0x88,
	PGPU_OP_ATTRIBS_ENABLE	= 0x89,
	PGPU_OP_PROGRAM_DRAW_INLINE = 0x8A,

	/* debug */
	PGPU_OP_DEBUG_SCREENSHOT = 0xF0		/* dump the last presented frame to the Zero's USB log */
};

/* ---- replies (section 9) ------------------------------------------------- */

enum pgpu_reply
{
	PGPU_REPLY_INFO		= 0x02,
	PGPU_REPLY_PONG		= 0x03,
	PGPU_REPLY_STATUS	= 0x04,
	PGPU_REPLY_FRAME_DONE	= 0x11,
	PGPU_REPLY_PIXELS	= 0x16,
	PGPU_REPLY_CREDIT	= 0x7E,		/* USB stream only: bytes received so far */
	PGPU_REPLY_ERROR	= 0x7F
};

enum pgpu_error
{
	PGPU_ERR_CRC		= 1,
	PGPU_ERR_OPCODE		= 2,
	PGPU_ERR_LENGTH		= 3,
	PGPU_ERR_ID		= 4,
	PGPU_ERR_ENUM		= 5,
	PGPU_ERR_OBJECT		= 6,
	PGPU_ERR_MEMORY		= 7,
	PGPU_ERR_CLEAR_AFTER_DRAW = 8,
	PGPU_ERR_LIMIT		= 9,
	PGPU_ERR_PROGRAM	= 10
};

/* ---- enumerations (section 10) ------------------------------------------- */

/* CLEAR mask */
#define PGPU_CLEAR_COLOR		(1u << 0)
#define PGPU_CLEAR_DEPTH		(1u << 1)
#define PGPU_CLEAR_STENCIL		(1u << 2)

/* FRAMEBUFFER_CREATE flags */
#define PGPU_FRAMEBUFFER_DEPTH_STENCIL	(1u << 0)
/* bits 15-8: 1 .. 16 = a depth and stencil buffer shared by the framebuffers
   with the same number (a GL renderbuffer), 0 = the framebuffer's own */
#define PGPU_FRAMEBUFFER_SHARED(n)	((n) << 8)
#define PGPU_FRAMEBUFFER_SHARED_ZS(f)	(((f) >> 8) & 0xFFu)

/* FRAME_END flags */
#define PGPU_FRAME_REPLY		(1u << 0)

/* ENABLE / DISABLE capabilities */
#define PGPU_CAP_DEPTH_TEST		(1u << 0)
#define PGPU_CAP_CULL_FACE		(1u << 1)
#define PGPU_CAP_BLEND			(1u << 2)
#define PGPU_CAP_TEXTURE_2D		(1u << 3)
#define PGPU_CAP_LIGHTING		(1u << 4)
#define PGPU_CAP_LIGHT0			(1u << 5)
#define PGPU_CAP_LIGHT1			(1u << 6)
#define PGPU_CAP_LIGHT2			(1u << 7)
#define PGPU_CAP_LIGHT3			(1u << 8)
#define PGPU_CAP_FOG			(1u << 9)
#define PGPU_CAP_ALPHA_TEST		(1u << 10)
#define PGPU_CAP_COLOR_MATERIAL		(1u << 11)
#define PGPU_CAP_NORMALIZE		(1u << 12)
#define PGPU_CAP_SCISSOR_TEST		(1u << 13)
#define PGPU_CAP_POLYGON_OFFSET_FILL	(1u << 14)
#define PGPU_CAP_STENCIL_TEST		(1u << 15)
#define PGPU_CAP_DITHER			(1u << 16)	/* panel: RGB565 dithered (default on) */
#define PGPU_CAP_ALL			0x1FFFFu

/* compare functions (same order as the V3D depth-test field) */
enum pgpu_func
{
	PGPU_NEVER, PGPU_LESS, PGPU_EQUAL, PGPU_LEQUAL,
	PGPU_GREATER, PGPU_NOTEQUAL, PGPU_GEQUAL, PGPU_ALWAYS
};

/* LOAD_MATRIX */
enum pgpu_matrix { PGPU_MODELVIEW, PGPU_PROJECTION, PGPU_TEXTURE };

/* CULL_FACE, FRONT_FACE */
enum pgpu_face { PGPU_FRONT, PGPU_BACK, PGPU_FRONT_AND_BACK };
enum pgpu_winding { PGPU_CCW, PGPU_CW };

/* attributes (ARRAY, ARRAYS_ENABLE, DRAW_INLINE mask bit n = attribute n) */
enum pgpu_attribute { PGPU_ATTR_POSITION, PGPU_ATTR_COLOR, PGPU_ATTR_NORMAL, PGPU_ATTR_TEXCOORD };
#define PGPU_ATTRIB(a)			(1u << (a))

/* primitive modes (same values as GL) */
enum pgpu_mode
{
	PGPU_POINTS, PGPU_LINES, PGPU_LINE_LOOP, PGPU_LINE_STRIP,
	PGPU_TRIANGLES, PGPU_TRIANGLE_STRIP, PGPU_TRIANGLE_FAN
};

/* blend factors (10.3) */
enum pgpu_blend
{
	PGPU_ZERO, PGPU_ONE, PGPU_SRC_COLOR, PGPU_ONE_MINUS_SRC_COLOR,
	PGPU_SRC_ALPHA, PGPU_ONE_MINUS_SRC_ALPHA, PGPU_DST_ALPHA, PGPU_ONE_MINUS_DST_ALPHA,
	PGPU_DST_COLOR, PGPU_ONE_MINUS_DST_COLOR, PGPU_SRC_ALPHA_SATURATE,
	PGPU_CONSTANT_COLOR, PGPU_ONE_MINUS_CONSTANT_COLOR,
	PGPU_CONSTANT_ALPHA, PGPU_ONE_MINUS_CONSTANT_ALPHA
};

/* STENCIL_OP */
enum pgpu_stencil_op
{
	PGPU_KEEP, PGPU_ZERO_OP, PGPU_REPLACE_OP, PGPU_INCR, PGPU_DECR, PGPU_INVERT,
	PGPU_INCR_WRAP, PGPU_DECR_WRAP
};

/* BLEND_EQUATION */
enum pgpu_blend_equation { PGPU_FUNC_ADD, PGPU_FUNC_SUBTRACT, PGPU_FUNC_REVERSE_SUBTRACT };

/* array component types (10.5), DRAW_ELEMENTS index types */
enum pgpu_type
{
	PGPU_FLOAT, PGPU_SHORT, PGPU_SHORT_NORM, PGPU_UBYTE_NORM, PGPU_BYTE_NORM,
	PGPU_UBYTE, PGPU_BYTE, PGPU_USHORT, PGPU_USHORT_NORM, PGPU_FIXED
};
enum pgpu_index_type { PGPU_INDEX_U8, PGPU_INDEX_U16 };

/* textures (10.6) */
enum pgpu_format
{
	PGPU_RGBA8888, PGPU_RGB565, PGPU_RGBA4444, PGPU_RGBA5551,
	PGPU_L8, PGPU_A8, PGPU_LA88, PGPU_ETC1, PGPU_RGB888
};
#define PGPU_TEXTURE_CUBE		(1u << 8)	/* TEXTURE_CREATE format flag */
/* TEXTURE_DATA first word: id | level << 16 | cube face (+X, -X, +Y, -Y, +Z, -Z) << 24 */
#define PGPU_TEXTURE_TARGET(id, level, face)	((id) | (level) << 16 | (face) << 24)
enum pgpu_filter
{
	PGPU_NEAREST, PGPU_LINEAR, PGPU_NEAREST_MIPMAP_NEAREST, PGPU_LINEAR_MIPMAP_NEAREST,
	PGPU_NEAREST_MIPMAP_LINEAR, PGPU_LINEAR_MIPMAP_LINEAR
};
enum pgpu_wrap { PGPU_REPEAT, PGPU_CLAMP_TO_EDGE, PGPU_MIRRORED_REPEAT };

/* TEX_ENV, FOG, SHADE_MODEL */
enum pgpu_tex_env { PGPU_MODULATE, PGPU_REPLACE, PGPU_DECAL, PGPU_BLEND };
enum pgpu_fog_mode { PGPU_FOG_LINEAR, PGPU_FOG_EXP, PGPU_FOG_EXP2 };
enum pgpu_shade_model { PGPU_SMOOTH, PGPU_FLAT };

/* colors: 0xAABBGGRR */
#define PGPU_RGBA(r, g, b, a)		(  ((unsigned) (r) & 0xFFu) | (((unsigned) (g) & 0xFFu) << 8) \
					 | (((unsigned) (b) & 0xFFu) << 16) | (((unsigned) (a) & 0xFFu) << 24))

/* ---- CRC-32 (zlib / IEEE 802.3, reflected, init and final XOR 0xFFFFFFFF) ---- */

#define PGPU_CRC_INIT			0xFFFFFFFFu
#define PGPU_CRC_POLY_REFLECTED		0xEDB88320u

#endif
