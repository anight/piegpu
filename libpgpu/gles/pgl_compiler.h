/*
 * pgl_compiler.h - pgl's shader compiler, where there is one: the host build
 * (host/pgl_compiler_glslc.c) runs tools/glslc; the Pico has none
 * (pgl_compiler_none.c: GL_SHADER_COMPILER is false, programs are loaded as
 * binaries).
 */
#ifndef PGL_COMPILER_H
#define PGL_COMPILER_H

#include <stdbool.h>
#include "pgpu_program_info.h"

bool pglc_available (void);

/* compile one shader (GL_VERTEX_SHADER or GL_FRAGMENT_SHADER): true if it
   compiles; *log is a malloc'ed info log or NULL */
bool pglc_compile (unsigned type, const char *source, char **log);

/* link two compiled shaders, with attribute locations bound by name (others
   get the lowest free ones): a malloc'ed program (pglc_free), or NULL and *log */
pgpu_program_info_t *pglc_link (const char *vs, const char *fs, const char *const *names,
				const unsigned *locations, unsigned bindings, char **log);
void pglc_free (pgpu_program_info_t *program);

#endif
