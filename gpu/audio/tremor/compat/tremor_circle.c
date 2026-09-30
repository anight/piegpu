/*
 * tremor_circle.c - the C library functions Tremor and libogg use that
 * Circle hasn't (see tremor_circle.h).
 */
#include "tremor_circle.h"
#include <stdlib.h>
#include <string.h>

int toupper (int c)
{
	return c >= 'a' && c <= 'z' ? c - 'a' + 'A' : c;
}

long labs (long n)
{
	return n < 0 ? -n : n;
}

void *memchr (const void *s, int c, size_t n)
{
	const unsigned char *p = (const unsigned char *) s;
	for (size_t i = 0; i < n; i++)
	{
		if (p[i] == (unsigned char) c)
		{
			return (void *) (p + i);
		}
	}
	return NULL;
}
