/*
 * pgpu_host.c - the transport of pgpu.c on a PC (pgpu_link.h): the packets go
 * to the RPi over USB, replies come back the same way. For running pgl on
 * the PC (tests such as dEQP, demos): the RPi executes the same commands as
 * from the Pico.
 *
 * Two ways over the same cable:
 * - the RPi's GL interface (devtools/pgpugadget: vendor class, subclass 'P',
 *   protocol 'G'; a bulk endpoint each way), with libusb: USB's own flow
 *   control, large transfers. Needs access to the device (a udev rule,
 *   README);
 * - else its serial port (the devlink of the gpu app, switched to its binary
 *   stream), with CREDIT replies for flow control.
 *
 * Built with Emscripten (hosts/web), the serial port is a web page's (Web
 * Serial): the page's JavaScript moves the bytes (Module.glIO, web/installer/
 * gl.js), and the program waits for them with Asyncify.
 *
 * Environment:
 *	PGPU_TTY	use the serial port, this device (default: the gpu app's)
 *	PGPU_TEXT_LOG	a file for the text the RPi sends besides the replies on
 *			the serial port (the log; DEBUG_SCREENSHOT dumps, for
 *			devtools/screenshot.py)
 *	PGPU_WINDOW	the serial port's flow-control window, bytes (tests)
 *	PGPU_SHOT_FRAME	after this many frames (pgpu_wait_frame), ask the RPi once
 *			for a screenshot (DEBUG_SCREENSHOT: it goes to the log, so
 *			with PGPU_TEXT_LOG over the serial port)
 */
#define _GNU_SOURCE
#include "pgpu_link.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <glob.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#include <signal.h>
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#else
#include <libusb.h>
#endif

/* the gpu app's serial port (devtools/pgpugadget: "piegpu", the board's
   serial number), or an older one's (Circle's CDC gadget) */
#define DEFAULT_TTY	"/dev/serial/by-id/usb-piegpu_piegpu_*-if00"
#define OLD_TTY		"/dev/serial/by-id/usb-Circle_CDC_Gadget-if00"
#define STREAM_MAGIC	"piegpu-stream"
#define STREAM_ACK	"\n#STREAM\n"

static int fd = -1;
static FILE *text_log;

static uint8_t rx[1024 * 1024];
static size_t rx_bytes;

#ifndef __EMSCRIPTEN__
/* the GL interface (libusb), when usb_dev is open */
#define USB_VENDOR_ID	0x1d50
#define USB_PRODUCT_ID	0x614d
#define USB_SUBCLASS	'P'
#define USB_PROTOCOL	'G'
#define USB_XFER_BYTES	(64 * 1024)
#define USB_READS	4		/* always queued */
#define USB_WRITES	4
static libusb_context *usb_ctx;
static libusb_device_handle *usb_dev;
static struct libusb_transfer *usb_read[USB_READS], *usb_write[USB_WRITES];
static volatile int usb_read_done[USB_READS], usb_write_busy[USB_WRITES];
static unsigned usb_next_read, usb_next_write;	/* (they complete in order) */
#endif

/* flow control: the RPi's USB gadget buffers 64 KB and drops what doesn't
   fit; CREDIT replies say how many stream bytes the RPi has taken. Larger
   windows send faster but read back slower (linktest, 2026-09-29: 6 KB 8.3
   and 6.1 MB/s, 60 KB 11.0 and 3.2 MB/s) */
#define WINDOW		6144
static uint32_t window = WINDOW;		/* PGPU_WINDOW overrides it (tests) */
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

uint32_t pgpu_link_capacity (void)
{
	return 0;					/* (USB's depends on the PC and the path) */
}

#ifdef __EMSCRIPTEN__
/* the page's serial port: write (awaits the port), read what has come,
   wait (for data, or us microseconds); < 0 once the page stops the program */
EM_ASYNC_JS (int, web_write, (const void *p, int n), {
	return await Module.glIO.write (HEAPU8.slice (p, p + n));
});
EM_JS (int, web_read, (void *p, int n), {
	return Module.glIO.read (HEAPU8.subarray (p, p + n));
});
EM_ASYNC_JS (int, web_wait, (int us), {
	return await Module.glIO.wait (us);
});

static void write_all (const void *data, size_t n)
{
	if (web_write (data, n) < 0)
	{
		exit (0);				/* stopped */
	}
}
#else
static void write_all (const void *data, size_t n)
{
	const uint8_t *p = data;
	while (n)
	{
		ssize_t r = write (fd, p, n);
		if (r < 0)
		{
			if (errno == EAGAIN)		/* (non-blocking) wait for room */
			{
				struct pollfd w = {fd, POLLOUT, 0};
				poll (&w, 1, 100);
				continue;
			}
			if (errno == EINTR)
			{
				continue;
			}
			fail ("write");
		}
		p += r;
		n -= r;
	}
}
#endif

#ifndef __EMSCRIPTEN__
static void LIBUSB_CALL usb_read_done_cb (struct libusb_transfer *t)
{
	usb_read_done[(uintptr_t) t->user_data] = 1;
}

static void LIBUSB_CALL usb_write_done_cb (struct libusb_transfer *t)
{
	if (t->status != LIBUSB_TRANSFER_COMPLETED)
	{
		fprintf (stderr, "pgpu_host: a transfer to the RPi failed (%s)\n", libusb_error_name (t->status));
		exit (1);
	}
	usb_write_busy[(uintptr_t) t->user_data] = 0;
}

/* the RPi's GL interface: true if it's there and open */
static bool usb_open (void)
{
	if (libusb_init (&usb_ctx) < 0)
	{
		return false;
	}
	libusb_device **list;
	ssize_t n = libusb_get_device_list (usb_ctx, &list);
	int result = LIBUSB_ERROR_NOT_FOUND, interface = -1;
	uint8_t ep_out = 0, ep_in = 0;
	for (ssize_t i = 0; i < n && !usb_dev; i++)
	{
		struct libusb_device_descriptor dd;
		struct libusb_config_descriptor *cd;
		if (   libusb_get_device_descriptor (list[i], &dd) < 0
		    || dd.idVendor != USB_VENDOR_ID || dd.idProduct != USB_PRODUCT_ID
		    || libusb_get_active_config_descriptor (list[i], &cd) < 0)
		{
			continue;
		}
		for (int k = 0; k < cd->bNumInterfaces && interface < 0; k++)
		{
			const struct libusb_interface_descriptor *id = &cd->interface[k].altsetting[0];
			if (   id->bInterfaceClass == LIBUSB_CLASS_VENDOR_SPEC
			    && id->bInterfaceSubClass == USB_SUBCLASS && id->bInterfaceProtocol == USB_PROTOCOL)
			{
				interface = id->bInterfaceNumber;
				for (int e = 0; e < id->bNumEndpoints; e++)
				{
					uint8_t a = id->endpoint[e].bEndpointAddress;
					*(a & LIBUSB_ENDPOINT_IN ? &ep_in : &ep_out) = a;
				}
			}
		}
		libusb_free_config_descriptor (cd);
		if (interface >= 0 && (result = libusb_open (list[i], &usb_dev)) < 0)
		{
			usb_dev = NULL;
		}
	}
	libusb_free_device_list (list, 1);
	if (usb_dev && (result = libusb_claim_interface (usb_dev, interface)) < 0)
	{
		libusb_close (usb_dev);
		usb_dev = NULL;
	}
	if (!usb_dev)
	{
		if (interface >= 0)		/* (an older RPi has none: quiet) */
		{
			fprintf (stderr, "pgpu_host: the RPi's GL interface: %s%s; using its serial port\n",
				 libusb_error_name (result),
				 result == LIBUSB_ERROR_ACCESS ? " (the udev rule in README)" : "");
		}
		libusb_exit (usb_ctx);
		return false;
	}

	for (unsigned i = 0; i < USB_READS; i++)
	{
		usb_read[i] = libusb_alloc_transfer (0);
		libusb_fill_bulk_transfer (usb_read[i], usb_dev, ep_in, malloc (USB_XFER_BYTES), USB_XFER_BYTES,
					   usb_read_done_cb, (void *) (uintptr_t) i, 0);
		if (libusb_submit_transfer (usb_read[i]) < 0)
		{
			fail ("libusb_submit_transfer");
		}
	}
	for (unsigned i = 0; i < USB_WRITES; i++)
	{
		usb_write[i] = libusb_alloc_transfer (0);
		libusb_fill_bulk_transfer (usb_write[i], usb_dev, ep_out, malloc (USB_XFER_BYTES), 0,
					   usb_write_done_cb, (void *) (uintptr_t) i, 0);
		/* a transfer of a multiple of 512 bytes ends with a zero-length
		   packet: the RPi's receive ends there */
		usb_write[i]->flags = LIBUSB_TRANSFER_ADD_ZERO_PACKET;
	}
	return true;
}

/* completed reads into rx (in order, while there's room), queued again */
static void usb_poll (int timeout_us)
{
	struct timeval tv = {0, timeout_us};
	libusb_handle_events_timeout_completed (usb_ctx, &tv, NULL);
	while (usb_read_done[usb_next_read])
	{
		struct libusb_transfer *t = usb_read[usb_next_read];
		if (t->status != LIBUSB_TRANSFER_COMPLETED)
		{
			fprintf (stderr, "pgpu_host: the RPi is gone (%s)\n", libusb_error_name (t->status));
			exit (1);
		}
		if (rx_bytes + t->actual_length > sizeof rx)
		{
			break;			/* after the parser has taken some */
		}
		memcpy (rx + rx_bytes, t->buffer, t->actual_length);
		rx_bytes += t->actual_length;
		usb_read_done[usb_next_read] = 0;
		if (libusb_submit_transfer (t) < 0)
		{
			fail ("libusb_submit_transfer");
		}
		usb_next_read = (usb_next_read + 1) % USB_READS;
	}
}

static void usb_send (const uint8_t *p, uint32_t bytes)
{
	uint64_t wait_start = pgpu_link_time_us ();
	while (bytes)
	{
		if (usb_write_busy[usb_next_write])
		{
			pgpu_link_poll ();		/* replies meanwhile: the RPi may wait for room for them */
			if (pgpu_link_time_us () > wait_start + 10000000)
			{
				fprintf (stderr, "pgpu_host: the RPi takes no commands for 10 s\n");
				exit (1);
			}
			continue;
		}
		wait_start = pgpu_link_time_us ();
		struct libusb_transfer *t = usb_write[usb_next_write];
		uint32_t chunk = bytes < USB_XFER_BYTES ? bytes : USB_XFER_BYTES;
		memcpy (t->buffer, p, chunk);
		t->length = chunk;
		usb_write_busy[usb_next_write] = 1;
		if (libusb_submit_transfer (t) < 0)
		{
			fail ("libusb_submit_transfer");
		}
		usb_next_write = (usb_next_write + 1) % USB_WRITES;
		p += chunk;
		bytes -= chunk;
	}
}

#endif

static void send_bytes (const uint8_t *p, uint32_t bytes);

/* a new session: whatever an earlier program left half sent (killed in the
   middle of a packet), zeros complete it (the RPi then finds its CRC wrong
   and skips zeros); then a PING, and replies before its PONG are an earlier
   program's */
#define START_COOKIE	0x5D000000u
static uint32_t start_cookie, start_pong;

static void start_session (void)
{
	static const uint8_t zeros[(PGPU_MAX_PAYLOAD + 2) * 4];
	send_bytes (zeros, sizeof zeros);
	start_cookie = START_COOKIE | (getpid () & 0xFFFFFF);
	uint32_t ping[3] = {PGPU_HEADER (PGPU_SYNC_COMMAND, PGPU_OP_PING, 1), start_cookie, 0};
	ping[2] = pgpu_crc32 (ping, 2);
	send_bytes ((const uint8_t *) ping, sizeof ping);
	uint64_t end = pgpu_link_time_us () + 5000000;
	while (start_pong != start_cookie && pgpu_link_time_us () < end)
	{
		pgpu_link_poll ();
	}
	if (start_pong != start_cookie)
	{
		fprintf (stderr, "pgpu_host: no answer from the RPi (is the gpu app running?)\n");
		exit (1);
	}
}

/* the serial port's bytes so far (at most n), without waiting; and a wait for
   more (at most us microseconds) */
#ifdef __EMSCRIPTEN__
static ssize_t tty_read (void *p, size_t n)
{
	int r = web_read (p, n);
	if (r < 0)
	{
		exit (0);				/* stopped */
	}
	return r;
}

static void tty_wait (int us)
{
	if (web_wait (us) < 0)
	{
		exit (0);
	}
}
#else
static ssize_t tty_read (void *p, size_t n)
{
	ssize_t r = read (fd, p, n);
	if (r < 0 && errno != EAGAIN && errno != EINTR)
	{
		fail ("read (the RPi is gone)");
	}
	return r > 0 ? r : 0;
}

static void tty_wait (int us)
{
	struct pollfd p = {fd, POLLIN, 0};
	struct timespec wait = {0, us * 1000};
	if (ppoll (&p, 1, &wait, NULL) > 0 && (p.revents & (POLLHUP | POLLERR)))
	{
		fprintf (stderr, "pgpu_host: the RPi is gone\n");
		exit (1);
	}
}
#endif

static void tty_open (void)
{
#ifdef __EMSCRIPTEN__
	const char *tty = "the page's serial port";	/* (open already) */
#else
	const char *tty = getenv ("PGPU_TTY");
	glob_t g;
	if (!tty)
	{
		tty = glob (DEFAULT_TTY, 0, NULL, &g) == 0 ? g.gl_pathv[0] : OLD_TTY;
	}
	/* reads without waiting by O_NONBLOCK, not by VMIN 0: the setting
	   outlives the program, and Chrome's Web Serial (the installer page)
	   takes a read of nothing on a tty with VMIN 0 for "the device has been
	   lost" */
	fd = open (tty, O_RDWR | O_NOCTTY | O_NONBLOCK);
	if (fd < 0)
	{
		fail (tty);
	}
	struct termios t;
	if (tcgetattr (fd, &t) == 0)
	{
		cfmakeraw (&t);			/* (VMIN 1, VTIME 0) */
		tcsetattr (fd, TCSANOW, &t);
	}
	if (getenv ("PGPU_WINDOW"))
	{
		window = strtoul (getenv ("PGPU_WINDOW"), NULL, 0);
	}
	if (getenv ("PGPU_TEXT_LOG"))
	{
		text_log = fopen (getenv ("PGPU_TEXT_LOG"), "w");
	}
#endif

	/* switch the link to the binary stream: wait for the acknowledgement
	   (the log the RPi sends before it is text) */
	write_all (STREAM_MAGIC, sizeof STREAM_MAGIC - 1);
	char seen[sizeof STREAM_ACK] = {0};
	uint64_t end = pgpu_link_time_us () + 3000000;
	while (pgpu_link_time_us () < end)
	{
		char c;
		if (tty_read (&c, 1) == 1)
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
			tty_wait (1000);
		}
	}
	fprintf (stderr, "pgpu_host: no answer from the RPi on %s (is the gpu app running?)\n", tty);
	exit (1);
}

#ifndef __EMSCRIPTEN__
/* a program interrupted or killed (Ctrl-C, SIGTERM, the terminal gone) ends
   its session over the serial port: STREAM_END (docs/protocol.md 13), so the
   RPi resets and takes text again. The packet is written as it is (write is
   safe in a handler); a packet cut short before it is garbage the RPi skips.
   (The GL interface's libusb can't be used here: there the next program's
   session, or a restart, ends it.) */
static const uint8_t stream_end_packet[8] = {0x00, 0x00, 0x05, 0xa5, 0x3e, 0x7c, 0x8f, 0xfa};

static void end_on_signal (int sig)
{
	if (fd >= 0)
	{
		ssize_t r = write (fd, stream_end_packet, sizeof stream_end_packet);
		(void) r;
		tcdrain (fd);
	}
	_exit (128 + sig);
}

static void end_at_exit (void)
{
	if (fd >= 0)
	{
		pgpu_stream_end ();
		tcdrain (fd);
	}
}
#endif

void pgpu_link_init (void)
{
#ifdef __EMSCRIPTEN__
	tty_open ();
#else
	if (getenv ("PGPU_TTY") || !usb_open ())
	{
		tty_open ();
		struct sigaction sa;
		memset (&sa, 0, sizeof sa);
		sa.sa_handler = end_on_signal;
		sigaction (SIGINT, &sa, NULL);
		sigaction (SIGTERM, &sa, NULL);
		sigaction (SIGHUP, &sa, NULL);
		atexit (end_at_exit);
	}
#endif
	start_session ();
}

#define SYNC_COOKIE	0x5C000000u		/* the PINGs of PGPU_SYNC (high byte) */
static uint32_t sync_pong;

/* PGPU_SYNC set: each packet alone, then a PING; reports the packet the RPi
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
			fprintf (stderr, "pgpu_host: the RPi hangs in opcode %02x, %u words:",
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
#ifndef __EMSCRIPTEN__
	if (usb_dev)
	{
		usb_send (p, bytes);
		return;
	}
#endif
	uint64_t credit_wait_start = pgpu_link_time_us ();
	while (bytes)
	{
		uint32_t space = window - (sent - credited);
		if (space < 512 && space < bytes)
		{
			pgpu_link_poll ();		/* wait for credit */
			if (pgpu_link_time_us () > credit_wait_start + 10000000)
			{
				fprintf (stderr, "pgpu_host: no credit from the RPi for 10 s\n");
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
#ifndef __EMSCRIPTEN__
	if (usb_dev)
	{
		usb_poll (100);			/* (the callers poll in a loop: don't spin) */
	}
	else
#endif
	{
		ssize_t r = tty_read (rx + rx_bytes, sizeof rx - rx_bytes);
		rx_bytes += r;
		if (r == 0)
		{
			tty_wait (100);		/* nothing yet: the callers poll in a loop, don't spin */
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
		if (start_pong != start_cookie)		/* an earlier program's, or ours */
		{
			if (PGPU_HEADER_OP (header) == PGPU_REPLY_PONG && length >= 1 && words[1] == start_cookie)
			{
				start_pong = start_cookie;
			}
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
/* no FRAME line over USB: a PING after the frame instead. The RPi runs the
   commands in order, so its PONG comes once it has executed everything
   before: the frame, with its wait for the screen (the panel's DMA of the
   frame before, or HDMI's vertical sync). Programs pace on the RPi's frames
   as on the Pico's FRAME pulses; frame_count counts these. */
static uint32_t frames_waited;

bool pgpu_wait_frame (uint32_t timeout_ms)
{
	if (pgpu_ping_wait (0x46524d00u + (frames_waited & 0xff), timeout_ms) < 0)
	{
		return false;
	}
	frames_waited++;

	/* PGPU_SHOT_FRAME: between two frames, no packet is half sent */
	static long shot_frame = -1;
	if (shot_frame == -1)
	{
		shot_frame = getenv ("PGPU_SHOT_FRAME") ? strtol (getenv ("PGPU_SHOT_FRAME"), NULL, 0) : 0;
	}
	if (shot_frame > 0 && frames_waited == (uint32_t) shot_frame)
	{
		uint32_t shot[2] = {PGPU_HEADER (PGPU_SYNC_COMMAND, PGPU_OP_DEBUG_SCREENSHOT, 0), 0};
		shot[1] = pgpu_crc32 (shot, 1);
		send_bytes ((const uint8_t *) shot, sizeof shot);
	}
	return true;
}

uint32_t pgpu_frame_count (void)		{ return frames_waited; }
const char *pgpu_link_name (void)		{ return "USB"; }
void pgpu_set_reply_phase (unsigned phase)	{ }

/* replies are parsed by the caller's own pgpu_link_poll: nothing to keep out */
void pgpu_link_lock (void)		{ }
void pgpu_link_unlock (void)		{ }
