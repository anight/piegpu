/*
 * Console output that is safe to call from an interrupt.
 *
 * printf is not. The SDK serialises stdout with a mutex
 * (pico_stdio/stdio.c: stdout_serialize_begin), and a mutex cannot be taken from
 * an exception handler - PICO_STDIO_DEADLOCK_TIMEOUT_MS exists precisely because
 * of this, and its own comment says "assume stdio_usb is deadlocked by use in
 * IRQ". When the lock cannot be had, stdio waits up to a second *inside the
 * interrupt* and then writes anyway, unserialised.
 *
 * That is not theoretical here. BTstack runs off the CYW43 async_context in
 * threadsafe_background mode, so every BTstack callback - link up, link down, and
 * every HID report - is an interrupt. Printing from them against a game loop that
 * also prints produced exactly what it should:
 *
 *   sys clock 12Init: displaLCD: PIO programs created. 13 instrs
 *   picosdl: , canvas 320x200Joystick: center x=1993 y=1974picosdl: inpuoth ...
 *
 * Fragments interleaved and lost. Once a keyboard is connected and reports are
 * arriving the console stops being readable at all, which is how this was found:
 * a HID trace that could not be read.
 *
 * So: format into a ring under a spin lock, which *is* IRQ-safe, and let core 0
 * do the actual printf from the main loop where the mutex is available. Output is
 * then always whole and always in order, at the cost of arriving a frame late.
 *
 * Overflow drops whole lines and counts them, rather than emitting a partial one.
 * A dropped line says so; a torn line is a bug hunt.
 */
#include "psdl_pico_log.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "hardware/sync.h"
#include "pico/multicore.h"

#define PSDL_LOG_BYTES 4096

static char          s_ring[PSDL_LOG_BYTES];
static volatile uint s_head;          /* write cursor, producer */
static volatile uint s_tail;          /* read cursor, core 0 only */
static volatile uint s_dropped;
static spin_lock_t  *s_lock;
static int           s_lock_num = -1;

void psdl_log_init(void)
{
	if (s_lock_num >= 0)
		return;
	s_lock_num = (int)spin_lock_claim_unused(true);
	s_lock     = spin_lock_instance((uint)s_lock_num);
	s_head = s_tail = s_dropped = 0;
}

static uint ring_free(void)
{
	/* One byte held back so head == tail always means empty. */
	return PSDL_LOG_BYTES - 1 - ((s_head - s_tail) & (PSDL_LOG_BYTES - 1));
}

void psdl_log(const char *fmt, ...)
{
	char    line[256];
	va_list ap;

	va_start(ap, fmt);
	int n = vsnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);

	if (n <= 0)
		return;
	if (n > (int)sizeof(line) - 1)
		n = (int)sizeof(line) - 1;   /* truncated; better than nothing */

	if (s_lock == NULL) {
		/* Before init: only core 0 is running, so plain printf is safe. */
		fwrite(line, 1, (size_t)n, stdout);
		return;
	}

	uint32_t save = spin_lock_blocking(s_lock);
	if ((uint)n <= ring_free()) {
		for (int i = 0; i < n; ++i)
			s_ring[(s_head + (uint)i) & (PSDL_LOG_BYTES - 1)] = line[i];
		s_head = (s_head + (uint)n) & (PSDL_LOG_BYTES - 1);
	} else {
		s_dropped++;
	}
	spin_unlock(s_lock, save);
}

void psdl_log_drain(void)
{
	if (s_lock == NULL)
		return;

	for (;;) {
		char chunk[128];
		uint got = 0;

		uint32_t save = spin_lock_blocking(s_lock);
		while (got < sizeof(chunk) && s_tail != s_head) {
			chunk[got++] = s_ring[s_tail];
			s_tail = (s_tail + 1) & (PSDL_LOG_BYTES - 1);
		}
		uint dropped = s_dropped;
		s_dropped = 0;
		spin_unlock(s_lock, save);

		if (got > 0)
			fwrite(chunk, 1, got, stdout);
		if (dropped > 0)
			printf("[log] %u line(s) dropped - ring full\n", dropped);
		if (got < sizeof(chunk))
			break;
	}
	fflush(stdout);
}

/* The backend interface's logger is this ring. */
void psdl_backend_log(const char *fmt, ...)
{
	char    line[256];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);
	psdl_log("%s", line);
}
