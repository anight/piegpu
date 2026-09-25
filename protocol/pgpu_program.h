/*
 * pgpu_program.h - program blob format (docs/protocol.md section 7.10)
 *
 * A program is precompiled on the PC by tools/glslc (GLSL ES 1.00 through
 * Mesa's vc4 compiler) into QPU code for the V3D's GL shader mode: a
 * fragment shader (FS), a vertex shader (VS) and a coordinate shader (CS,
 * the position-only vertex shader the binner runs), for a few pipeline state
 * variants. The blob is sent with PROGRAM_CREATE / PROGRAM_DATA.
 *
 * All fields are 32-bit little-endian words:
 *
 *	header		PGPU_PROGRAM_HEADER_WORDS words
 *	attributes	one word per attribute (location 0, 1, ...): type | size << 8
 *	variants	two words each: key, shader indices
 *	shaders		PGPU_SHADER_HEADER_WORDS words, then the code (two words per
 *			instruction, low word first), then the uniform stream
 *			description (two words per uniform: kind, data)
 */
#ifndef PGPU_PROGRAM_H
#define PGPU_PROGRAM_H

#define PGPU_PROGRAM_MAGIC		0x31504750u	/* "PGP1" */

/* header */
#define PGPU_PROGRAM_HEADER_WORDS	8
#define PGPU_PH_MAGIC			0
#define PGPU_PH_TOTAL_WORDS		1
#define PGPU_PH_COUNTS			2	/* attributes | samplers << 8 | variants << 16 | shaders << 24 */
#define PGPU_PH_UNIFORM_WORDS		3	/* size of the program's uniform storage */
/* words 4-7 reserved, 0 */

#define PGPU_MAX_ATTRIBUTES		8
#define PGPU_MAX_SAMPLERS		8
#define PGPU_MAX_TEXTURE_UNITS		8

/* attribute word */
#define PGPU_ATTR_TYPE(w)		((w) & 0xFFu)		/* enum pgpu_type */
#define PGPU_ATTR_SIZE(w)		(((w) >> 8) & 0xFFu)	/* components 1-4 */

/* variant key word */
#define PGPU_VK_PRIM(w)			((w) & 0xFFu)		/* enum pgpu_prim_class */
#define PGPU_VK_BLEND(w)		(((w) >> 8) & 0xFFu)	/* enum pgpu_blend_mode */
#define PGPU_VK_POINT_SIZE		(1u << 16)		/* the VS writes the point size */

enum pgpu_prim_class { PGPU_PRIM_TRIANGLES, PGPU_PRIM_LINES, PGPU_PRIM_POINTS };

/* the fragment shader's ending (tools/glslc): how the colour gets to the
   tile buffer */
enum pgpu_blend_mode
{
	PGPU_BLEND_PLAIN,		/* no blending, colour mask all on */
	PGPU_BLEND_GENERIC		/* blending and colour mask from PGPU_U_BLEND uniforms */
};

/* variant shader indices word: FS | VS << 8 | CS << 16 */

/* shader header */
#define PGPU_SHADER_HEADER_WORDS	4
#define PGPU_SH_COUNTS			0	/* instructions | uniforms << 16 */
#define PGPU_SH_INFO			1	/* see below */
#define PGPU_SH_VATTR_OFFSETS		2	/* 2 words: VPM byte offsets of attributes 0-7 */

/* PGPU_SH_INFO: stage in bits 31-28 */
#define PGPU_SHADER_STAGE(w)		((w) >> 28)
enum pgpu_shader_stage { PGPU_STAGE_FS, PGPU_STAGE_VS, PGPU_STAGE_CS };
/* FS: threaded (bit 0) | varyings << 8 */
#define PGPU_SH_FS_THREADED		(1u << 0)
#define PGPU_SH_FS_VARYINGS(w)		(((w) >> 8) & 0xFFu)
/* VS, CS: attribute select bits | total attributes size (bytes) << 8 */
#define PGPU_SH_ATTR_SELECT(w)		((w) & 0xFFu)
#define PGPU_SH_ATTR_SIZE(w)		(((w) >> 8) & 0xFFu)

/* uniform stream entry kinds (values of Mesa's enum quniform_contents,
   checked by tools/glslc against the Mesa source it uses) */
enum pgpu_uniform_kind
{
	PGPU_U_CONSTANT = 0,		/* data is the value */
	PGPU_U_UNIFORM = 1,		/* data is a word offset into the uniform storage */
	PGPU_U_VIEWPORT_X_SCALE = 2,	/* half width in 1/16 pixel */
	PGPU_U_VIEWPORT_Y_SCALE = 3,	/* -half height in 1/16 pixel (y down) */
	PGPU_U_VIEWPORT_Z_OFFSET = 4,
	PGPU_U_VIEWPORT_Z_SCALE = 5,
	PGPU_U_TEXTURE_CONFIG_P0 = 6,	/* data is the sampler index */
	PGPU_U_TEXTURE_CONFIG_P1 = 7,
	PGPU_U_TEXTURE_CONFIG_P2 = 8,
	PGPU_U_TEXTURE_FIRST_LEVEL = 9,
	PGPU_U_STENCIL = 22,		/* TLB stencil setup: data 0 front, 1 back, 2 write masks */
	PGPU_U_UNIFORMS_ADDRESS = 24,	/* bus address of this uniform stream */

	/* GL built-in state (Mesa state variables; data is the component 0-3),
	   values as Mesa defines them for a window framebuffer (rows top down) */
	PGPU_U_FB_Y_TRANSFORM = 32,	/* gl_FragCoord.y: -1, height, 1, 0 */
	PGPU_U_DEPTH_RANGE = 33,	/* gl_DepthRange: near, far, far - near, 1 */
	PGPU_U_POINT_Y_TRANSFORM = 34,	/* gl_PointCoord.y: -1, 1, 0, 0 */

	/* blending: data = channel * 12 + term; per channel (R, G, B, A) the
	   source factor, then the destination factor, each as coefficients of
	   (1, As, Ad, Sc, Dc, min (As, 1 - Ad)); the equation's sign included */
	PGPU_U_BLEND = 35
};

#endif
