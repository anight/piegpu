/*
 * pgpu_program_info.h - what tools/glslc knows about a program, for the GL
 * layer (gles/pgl.c): attribute and uniform names, GL types and locations.
 * The generated header defines NAME_info; glProgramBinaryOES takes a pointer
 * to it as the binary (format PGL_PROGRAM_BINARY_PGPU).
 */
#ifndef PGPU_PROGRAM_INFO_H
#define PGPU_PROGRAM_INFO_H

#include <stdint.h>

#define PGPU_PROGRAM_INFO_MAGIC		0x49504750u	/* "PGPI" */

typedef struct
{
	const char *name;
	uint32_t location;		/* generic attribute index */
	uint32_t type;			/* GL type (GL_FLOAT_VEC3, ...) */
	int32_t size;			/* array size (1) */
} pgpu_attrib_info_t;

typedef struct
{
	const char *name;		/* without "[0]" */
	uint32_t type;			/* GL type (GL_FLOAT_MAT4, GL_SAMPLER_2D, ...) */
	int32_t size;			/* array elements */
	uint16_t components;		/* scalars per element (samplers: 0) */
	int16_t sampler;		/* sampler index, -1 for other uniforms */
	const uint16_t *offsets;	/* size * components storage words for the vertex
					   shaders, then as many for the fragment shader;
					   0xffff = not used there (samplers: NULL) */
} pgpu_uniform_info_t;

typedef struct
{
	uint32_t magic;
	const uint32_t *blob;
	uint32_t words;
	const pgpu_attrib_info_t *attribs;
	uint32_t n_attribs;
	const pgpu_uniform_info_t *uniforms;
	uint32_t n_uniforms;
} pgpu_program_info_t;

#endif
