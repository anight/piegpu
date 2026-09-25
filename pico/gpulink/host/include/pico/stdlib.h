/* the part of the Pico SDK that the host build of gltest.c uses */
#ifndef PICO_STDLIB_HOST_H
#define PICO_STDLIB_HOST_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>

static inline void sleep_ms (uint32_t ms)
{
	usleep (ms * 1000u);
}

#endif
