/*
 * gltest_main.c - self test 8 (gltest.c) on a PC, through the Zero's USB
 */
#include <stdio.h>
#include "pgpu.h"

unsigned gl_self_test (void);

int main (void)
{
	pgpu_init ();
	unsigned failures = gl_self_test ();

	return failures ? 1 : 0;
}
