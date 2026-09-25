/*
 * pgpu_host.c - the transport of pgpu.c on a PC (pgpu_link.h): the packets go
 * to the Zero over its USB serial link (the devlink of the gpu app, switched
 * to its binary stream), replies come back the same way. For running pgl on
 * the PC (tests such as dEQP): the Zero executes the same commands as from
 * the Pico.
 *
 * Environment:
 *	PGPU_TTY	the Zero's serial device (default: the Circle CDC gadget)
 *	PGPU_TEXT_LOG	a file for the text the Zero sends besides the replies
 *			(e.g. DEBUG_SCREENSHOT dumps, for devtools/screenshot.py)
 */
#define _GNU_SOURCE
#include "pgpu_link.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define DEFAULT_TTY	"/dev/serial/by-id/usb-Circle_CDC_Gadget-if00"
#define STREAM_MAGIC	"pico-gpu-stream"
#define STREAM_ACK	"\n#STREAM\n"

static int fd = -1;
static FILE *text_log;

static uint8_t rx[64 * 1024];
static size_t rx_bytes;

/* flow control: the Zero's USB gadget buffers 8 KB and drops what doesn't
   fit; CREDIT replies say how many stream bytes the Zero has taken */
#define WINDOW		6144
static uint32_t sent, credited;

static void fail (const char *what)
{
	fprintf (stderr, "pgpu_host: %s: %s\n", what, strerror (errno));
	exit (1);
}

uint64_t pgpu_link_time_us (void)
{
	struct timespec ts;
	clock_gettime (CLOCK_MONOTONIC, &ts);
	return (uint64_t) ts.tv_sec * 1000000u + ts.tv_nsec / 1000;
}

static void write_all (const void *data, size_t n)
{
	const uint8_t *p = data;
	while (n)
	{
		ssize_t r = write (fd, p, n);
		if (r < 0)
		{
			if (errno == EINTR || errno == EAGAIN)
			{
				continue;
			}
			fail ("write");
		}
		p += r;
		n -= r;
	}
}

void pgpu_link_init (void)
{
	const char *tty = getenv ("PGPU_TTY") ? getenv ("PGPU_TTY") : DEFAULT_TTY;
	fd = open (tty, O_RDWR | O_NOCTTY);
	if (fd < 0)
	{
		fail (tty);
	}
	struct termios t;
	if (tcgetattr (fd, &t) == 0)
	{
		cfmakeraw (&t);
		t.c_cc[VMIN] = 0;
		t.c_cc[VTIME] = 0;
		tcsetattr (fd, TCSANOW, &t);
	}
	if (getenv ("PGPU_TEXT_LOG"))
	{
		text_log = fopen (getenv ("PGPU_TEXT_LOG"), "w");
	}

	/* switch the link to the binary stream: wait for the acknowledgement
	   (the log the Zero sends before it is text) */
	write_all (STREAM_MAGIC, sizeof STREAM_MAGIC - 1);
	char seen[sizeof STREAM_ACK] = {0};
	uint64_t end = pgpu_link_time_us () + 3000000;
	while (pgpu_link_time_us () < end)
	{
		char c;
		if (read (fd, &c, 1) == 1)
		{
			memmove (seen, seen + 1, sizeof seen - 2);
			seen[sizeof seen - 2] = c;
			if (memcmp (seen, STREAM_ACK, sizeof STREAM_ACK - 1) == 0)
			{
				return;
			}
		}
		else
		{
			usleep (1000);
		}
	}
	fprintf (stderr, "pgpu_host: no answer from the Zero on %s (is the gpu app running?)\n", tty);
	exit (1);
}

static void send_bytes (const uint8_t *p, uint32_t bytes);

#define SYNC_COOKIE	0x5C000000u		/* the PINGs of PGPU_SYNC (high byte) */
static uint32_t sync_pong;

/* PGPU_SYNC set: each packet alone, then a PING; reports the packet the Zero
   doesn't get past (debugging hangs) */
static void send_synced (const uint32_t *words, uint32_t n)
{
	static uint32_t cookie = SYNC_COOKIE;
	for (uint32_t i = 0; i < n; )
	{
		uint32_t len = PGPU_HEADER_LEN (words[i]) + 2;
		send_bytes ((const uint8_t *) &words[i], len * 4);
		uint32_t ping[3] = {PGPU_HEADER (PGPU_SYNC_COMMAND, PGPU_OP_PING, 1), ++cookie, 0};
		ping[2] = pgpu_crc32 (ping, 2);
		send_bytes ((const uint8_t *) ping, sizeof ping);
		uint64_t end = pgpu_link_time_us () + 5000000;
		while (sync_pong != cookie && pgpu_link_time_us () < end)
		{
			pgpu_link_poll ();
		}
		if (sync_pong != cookie)
		{
			fprintf (stderr, "pgpu_host: the Zero hangs in opcode %02x, %u words:",
				 PGPU_HEADER_OP (words[i]), PGPU_HEADER_LEN (words[i]));
			for (uint32_t k = 1; k < len - 1 && k <= 12; k++)
			{
				fprintf (stderr, " %08x", words[i + k]);
			}
			fprintf (stderr, "\n");
			exit (1);
		}
		i += len;
	}
}

void pgpu_link_send (const uint32_t *words, uint32_t n)
{
	static int sync = -1;
	if (sync < 0)
	{
		sync = getenv ("PGPU_SYNC") != NULL;
	}
	if (sync)
	{
		send_synced (words, n);
		return;
	}
	send_bytes ((const uint8_t *) words, n * 4);
}

static void send_bytes (const uint8_t *p, uint32_t bytes)
{
	uint64_t credit_wait_start = pgpu_link_time_us ();
	while (bytes)
	{
		uint32_t space = WINDOW - (sent - credited);
		if (space < 512 && space < bytes)
		{
			pgpu_link_poll ();		/* wait for credit */
			if (pgpu_link_time_us () > credit_wait_start + 10000000)
			{
				fprintf (stderr, "pgpu_host: no credit from the Zero for 10 s\n");
				exit (1);
			}
			continue;
		}
		credit_wait_start = pgpu_link_time_us ();
		uint32_t chunk = bytes < space ? bytes : space;
		write_all (p, chunk);
		sent += chunk;
		p += chunk;
		bytes -= chunk;
	}
}

/* reply packets are found by their header (SYNC 0x5A, a valid length) and
   the CRC; other bytes are text */
void pgpu_link_poll (void)
{
	ssize_t r = read (fd, rx + rx_bytes, sizeof rx - rx_bytes);
	if (r > 0)
	{
		rx_bytes += r;
	}
	else if (r < 0 && errno != EAGAIN && errno != EINTR)
	{
		fail ("read (the Zero is gone)");
	}
	else
	{
		/* nothing yet: the callers poll in a loop, don't spin */
		struct pollfd p = {fd, POLLIN, 0};
		struct timespec wait = {0, 100000};
		if (ppoll (&p, 1, &wait, NULL) > 0 && (p.revents & (POLLHUP | POLLERR)))
		{
			fprintf (stderr, "pgpu_host: the Zero is gone\n");
			exit (1);
		}
	}

	size_t pos = 0;
	while (rx_bytes - pos >= 8)
	{
		uint32_t header;
		memcpy (&header, rx + pos, 4);
		uint32_t length = PGPU_HEADER_LEN (header);
		if (PGPU_HEADER_SYNC (header) != PGPU_SYNC_REPLY || length > PGPU_MAX_REPLY_PAYLOAD)
		{
			if (text_log)
			{
				fputc (rx[pos], text_log);
			}
			pos++;
			continue;
		}
		if (rx_bytes - pos < (length + 2) * 4)
		{
			break;				/* wait for the rest */
		}
		uint32_t words[PGPU_MAX_REPLY_PAYLOAD + 2];
		memcpy (words, rx + pos, (length + 2) * 4);
		if (pgpu_crc32 (words, length + 1) != words[length + 1])
		{
			pgpu_link_stats.reply_crc_errors++;
			if (text_log)
			{
				fputc (rx[pos], text_log);
			}
			pos++;
			continue;
		}
		pos += (length + 2) * 4;
		if (PGPU_HEADER_OP (header) == PGPU_REPLY_CREDIT && length == 1)
		{
			credited = words[1];
			continue;
		}
		if (PGPU_HEADER_OP (header) == PGPU_REPLY_PONG && length >= 1
		    && (words[1] & 0xFF000000u) == SYNC_COOKIE)
		{
			sync_pong = words[1];		/* not for pgpu.c */
			continue;
		}
		pgpu_deliver_reply (PGPU_HEADER_OP (header), &words[1], length);
	}
	memmove (rx, rx + pos, rx_bytes - pos);
	rx_bytes -= pos;
	if (text_log)
	{
		fflush (text_log);
	}
}

void pgpu_link_settle (void)
{
	/* replies arrive in order: after a PONG, the earlier ones are parsed */
	pgpu_link_poll ();
}

bool pgpu_wait_ready (uint32_t timeout_ms)	{ return true; }	/* USB flow control */
bool pgpu_wait_frame (uint32_t timeout_ms)	{ return true; }
void pgpu_set_reply_phase (unsigned phase)	{ }

pgpu_stats_t pgpu_get_stats (void)
{
	pgpu_stats_t s = pgpu_link_stats;
	memset (&pgpu_link_stats, 0, sizeof pgpu_link_stats);
	memcpy (pgpu_link_stats.last_error, s.last_error, sizeof pgpu_link_stats.last_error);
	return s;
}
