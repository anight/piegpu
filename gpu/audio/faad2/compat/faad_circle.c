/*
 * faad_circle.c - the C library functions FAAD2 uses that Circle hasn't:
 * qsort (only on small tables: SBR's frequency bands, at most 64 entries,
 * so an insertion sort) and abs.
 */
#include <stdlib.h>
#include <string.h>

int abs (int n)
{
	return n < 0 ? -n : n;
}

void qsort (void *base, size_t count, size_t size, int (*compare) (const void *, const void *))
{
	unsigned char *a = (unsigned char *) base, t[16];
	if (size > sizeof t)
	{
		return;				/* (FAAD2's elements are 1 or 4 bytes) */
	}
	for (size_t i = 1; i < count; i++)
	{
		memcpy (t, a + i * size, size);
		size_t j = i;
		while (j > 0 && compare (a + (j - 1) * size, t) > 0)
		{
			memcpy (a + j * size, a + (j - 1) * size, size);
			j--;
		}
		memcpy (a + j * size, t, size);
	}
}
