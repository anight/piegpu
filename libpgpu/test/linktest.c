/*
 * linktest - the link at full speed, both ways, checked word by word:
 *
 * - down: 64 KB buffer uploads (BUFFER_DATA) back to back, random data; the
 *   RPi checks each packet's CRC (STATUS: crc_errors, command_errors)
 * - round trip: a 256x256 RGBA8888 texture of random data (a new seed each
 *   round) uploaded, then read back from a framebuffer on it (READ_PIXELS)
 *   and compared with what was sent; reply CRC errors are counted here
 *   (LINKTEST_SIZE: its side; two copies are kept, 256 KB each at 256, which
 *   a Pico hasn't got: 128 there)
 *
 * Every LINKTEST_REPORT_S seconds a line with the rates and the error counts,
 * and the totals so far; LINKTEST_SECONDS in all (0: for ever).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pico/stdlib.h"
#include "pgpu.h"

#ifndef LINKTEST_SECONDS
#define LINKTEST_SECONDS	120
#endif
#define LINKTEST_REPORT_S	10

#define BUF_ID		1
#define TEX_ID		1
#define FB_ID		1
#ifndef LINKTEST_SIZE
#define LINKTEST_SIZE	256
#endif
#define SIZE		LINKTEST_SIZE
#define UPLOAD_BYTES	65536

static uint32_t rng;

static uint32_t next (void)
{
	rng ^= rng << 13;			/* xorshift32 */
	rng ^= rng >> 17;
	rng ^= rng << 5;
	return rng;
}

static void fill (uint32_t *p, uint32_t words, uint32_t seed)
{
	rng = seed * 2654435761u | 1;
	for (uint32_t i = 0; i < words; i++)
	{
		p[i] = next ();
	}
}

static uint32_t upload[UPLOAD_BYTES / 4];
static uint32_t *sent, *back;			/* SIZE x SIZE words each: the heap (PSRAM on the P4) */

int main (void)
{
	stdio_init_all ();
	pgpu_init ();
	printf ("\nlinktest: waiting for the RPi (READY)...\n");
	while (!pgpu_wait_ready (1000))
	{
	}
	pgpu_set_reply_phase (1);
	sent = malloc (SIZE * SIZE * 4);
	back = malloc (SIZE * SIZE * 4);
	if (!sent || !back)
	{
		printf ("linktest: no memory for the texels\n");
		return 1;
	}

	pgpu_status_t st0;
	bool have_st0 = false;
	for (int i = 0; i < 5 && !(have_st0 = pgpu_get_status (&st0, 200)); i++)
	{
	}
	printf ("linktest: %s; the RPi's counts so far: CRC errors %u, command errors %u\n",
		have_st0 ? "STATUS read" : "no STATUS", have_st0 ? (unsigned) st0.crc_errors : 0,
		have_st0 ? (unsigned) st0.command_errors : 0);

	pgpu_buffer_create (BUF_ID, UPLOAD_BYTES);
	pgpu_texture_create (TEX_ID, SIZE, SIZE, PGPU_RGBA8888);
	pgpu_framebuffer_create (FB_ID, TEX_ID, 0, 0);
	pgpu_flush ();
	(void) pgpu_get_stats ();			/* from here */

	uint64_t start = time_us_64 (), report = start;
	uint64_t down_bytes = 0, down_us = 0, up_bytes = 0, up_us = 0, rt_bytes = 0, rt_us = 0;
	uint64_t t_words = 0, t_bad = 0, t_rounds = 0, t_failed = 0, t_crc = 0, t_lost = 0;
	uint32_t words = 0, bad = 0, rounds = 0, failed = 0, seed = 1;
	while (LINKTEST_SECONDS == 0 || time_us_64 () - start < LINKTEST_SECONDS * 1000000ull)
	{
		/* down: 1 MB of uploads (the data changes a little each time) */
		uint64_t t = time_us_64 ();
		for (int i = 0; i < 16; i++)
		{
			fill (upload, 16, seed + i);	/* a new head: not the same packets */
			pgpu_buffer_data (BUF_ID, 0, upload, UPLOAD_BYTES);
		}
		pgpu_flush ();
		down_us += time_us_64 () - t;
		down_bytes += 16 * UPLOAD_BYTES;

		/* round trip: random texels there and back */
		fill (sent, SIZE * SIZE, seed++);
		t = time_us_64 ();
		pgpu_texture_data (TEX_ID, 0, 0, SIZE, SIZE, PGPU_RGBA8888, sent);
		pgpu_bind_framebuffer (FB_ID);
		pgpu_flush ();
		uint64_t t_up = time_us_64 ();
		memset (back, 0, SIZE * SIZE * 4);
		bool ok = pgpu_read_pixels (0, 0, SIZE, SIZE, back, 2000);
		uint64_t t_end = time_us_64 ();
		up_us += t_end - t_up;
		up_bytes += SIZE * SIZE * 4;
		rt_us += t_end - t;
		rt_bytes += 2 * SIZE * SIZE * 4;
		pgpu_bind_framebuffer (0);
		rounds++;
		if (!ok)
		{
			failed++;
		}
		uint32_t round_bad = 0;
		for (uint32_t i = 0; i < SIZE * SIZE; i++)
		{
			round_bad += back[i] != sent[i];
		}
		if (ok && round_bad && bad == 0)
		{
			uint32_t i = 0;
			while (back[i] == sent[i])
			{
				i++;
			}
			printf ("linktest: round %u: first mismatch at word %u: sent %08lx got %08lx\n",
				(unsigned) rounds, (unsigned) i, (unsigned long) sent[i], (unsigned long) back[i]);
		}
		bad += ok ? round_bad : 0;
		words += SIZE * SIZE;

		uint64_t now = time_us_64 ();
		if (now - report >= LINKTEST_REPORT_S * 1000000ull)
		{
			pgpu_stats_t s = pgpu_get_stats ();
			pgpu_status_t st;
			bool have = pgpu_get_status (&st, 500);
			t_words += words;
			t_bad += bad;
			t_rounds += rounds;
			t_failed += failed;
			t_crc += s.reply_crc_errors;
			t_lost += s.reply_overruns + s.replies_lost;
			printf ("linktest: %3u s: down %u KB/s, up %u KB/s, round trip %u KB/s; %u rounds, "
				"%u words compared, %u wrong, %u reads failed; reply CRC errors %u, overruns %u, "
				"lost %u; READY wait %u ms; RPi: CRC errors %d, command errors %d, CPU %d%%\n",
				(unsigned) ((now - start) / 1000000),
				(unsigned) (down_us ? down_bytes * 1000 / down_us : 0),
				(unsigned) (up_us ? up_bytes * 1000 / up_us : 0),
				(unsigned) (rt_us ? rt_bytes * 1000 / rt_us : 0), (unsigned) rounds,
				(unsigned) words, (unsigned) bad, (unsigned) failed,
				(unsigned) s.reply_crc_errors, (unsigned) s.reply_overruns,
				(unsigned) s.replies_lost, (unsigned) (s.ready_wait_us / 1000),
				have ? (int) (st.crc_errors - st0.crc_errors) : -1,
				have ? (int) (st.command_errors - st0.command_errors) : -1,
				have && st.window_us ? (int) ((uint64_t) st.arm_busy_us * 100 / st.window_us) : -1);
			printf ("linktest: totals: %llu rounds, %llu words compared, %llu wrong, %llu reads failed, "
				"%llu reply CRC errors, %llu replies lost\n", (unsigned long long) t_rounds,
				(unsigned long long) t_words, (unsigned long long) t_bad,
				(unsigned long long) t_failed, (unsigned long long) t_crc,
				(unsigned long long) t_lost);
			down_bytes = down_us = up_bytes = up_us = rt_bytes = rt_us = 0;
			words = bad = rounds = failed = 0;
			report = now;
		}
	}
	printf ("linktest: done\n");
	while (true)
	{
		sleep_ms (1000);
	}
}
