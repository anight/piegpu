/*
 * pgl_compiler_none.c - no shader compiler (the Pico): see pgl_compiler.h
 */
#include "pgl_compiler.h"
#include <stddef.h>

bool pglc_available (void)
{
	return false;
}

bool pglc_compile (unsigned type, const char *source, char **log)
{
	*log = NULL;
	return false;
}

pgpu_program_info_t *pglc_link (const char *vs, const char *fs, const char *const *names,
				const unsigned *locations, unsigned bindings, char **log)
{
	*log = NULL;
	return NULL;
}

void pglc_free (pgpu_program_info_t *program)
{
}
