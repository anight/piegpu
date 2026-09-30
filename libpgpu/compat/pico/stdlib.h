/*
 * pico/stdlib.h - the part of the Pico SDK that the demos and self tests use,
 * for hosts other than the Pico (a PC, an ESP32-P4): time from the transport
 * (pgpu_time_us), sleep from POSIX. So the same sources build on every host.
 */
#ifndef PGPU_COMPAT_PICO_STDLIB_H
#define PGPU_COMPAT_PICO_STDLIB_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include "pgpu.h"

typedef uint64_t absolute_time_t;		/* microseconds */

static inline void stdio_init_all (void)	{ }
static inline void tight_loop_contents (void)	{ }

static inline absolute_time_t get_absolute_time (void)	{ return pgpu_time_us (); }
static inline uint64_t time_us_64 (void)		{ return pgpu_time_us (); }
static inline uint32_t time_us_32 (void)		{ return (uint32_t) pgpu_time_us (); }

static inline int64_t absolute_time_diff_us (absolute_time_t from, absolute_time_t to)
{
	return (int64_t) (to - from);
}

static inline absolute_time_t make_timeout_time_ms (uint32_t ms)
{
	return pgpu_time_us () + ms * 1000ull;
}

static inline bool time_reached (absolute_time_t t)	{ return pgpu_time_us () >= t; }

#define PICO_ERROR_TIMEOUT	(-1)

/* console input, waiting at most us: a character, or PICO_ERROR_TIMEOUT
   (stdin as it is: a terminal in line mode gives the characters after Enter;
   a page's WebAssembly has none) */
#ifdef __EMSCRIPTEN__
static inline int getchar_timeout_us (uint32_t us)	{ (void) us; return PICO_ERROR_TIMEOUT; }
#else
#include <poll.h>
static inline int getchar_timeout_us (uint32_t us)
{
	struct pollfd p = {0, POLLIN, 0};
	unsigned char c;
	if (poll (&p, 1, (int) (us / 1000)) == 1 && (p.revents & POLLIN) && read (0, &c, 1) == 1)
	{
		return c;
	}
	return PICO_ERROR_TIMEOUT;
}
#endif

static inline void sleep_ms (uint32_t ms)	{ usleep (ms * 1000u); }
static inline void sleep_us (uint64_t us)	{ usleep ((useconds_t) us); }

#endif
