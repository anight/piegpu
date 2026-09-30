/*
 * sdbench.c - the microSD card's reads, timed (PGPU_APP=sdbench): the media
 * demo's file read from its start, as that demo reads it (lseek, then read,
 * through ESP-IDF's FatFs), with 2 KB, 16 KB and 256 KB reads into
 * DMA-capable memory, round after round. Every slow read (SLOW_US more than
 * its bytes take at NORMAL_BYTES_PER_US) is printed (its offset, size and
 * time), each round's rate at its end: do the card's stalls (reads of about
 * 25 ms, whatever their size) come back at the same places in the file, or at
 * times of their own, and do big reads escape them?
 */
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>
#include "esp_heap_caps.h"
#include "esp_timer.h"

#ifndef SDBENCH_FILE
#define SDBENCH_FILE	"/sdcard/video1.mp4"
#endif
#define REGION		(96u << 20)		/* bytes of the file each round reads */
#define SLOW_US		10000
#define NORMAL_BYTES_PER_US	10		/* 10 MB/s: 256 KB in 26 ms */
#define MAX_READ	(256u << 10)

static uint64_t now_us (void)
{
	return (uint64_t) esp_timer_get_time ();
}

/* one round: the region in reads of size bytes; false if a read failed */
static bool round_of (int fd, uint8_t *buffer, uint32_t size, unsigned round)
{
	unsigned reads = 0, slow = 0;
	uint64_t slow_us = 0, start = now_us ();
	uint32_t first_slow = 0, last_slow = 0;
	for (uint32_t offset = 0; offset < REGION; offset += size)
	{
		uint64_t t = now_us ();
		if (lseek (fd, (off_t) offset, SEEK_SET) != (off_t) offset || read (fd, buffer, size) != (ssize_t) size)
		{
			printf ("sdbench: read failed at %u\n", (unsigned) offset);
			return false;
		}
		uint64_t us = now_us () - t;
		reads++;
		if (us >= SLOW_US + size / NORMAL_BYTES_PER_US)
		{
			if (!slow)
			{
				first_slow = offset;
			}
			last_slow = offset;
			slow++;
			slow_us += us;
			if (slow <= 40 || slow % 100 == 0)
			{
				printf ("sdbench: round %u, %u KB reads: slow read at %.3f MB (sector %u), %u ms, "
					"%.1f s since start\n", round, (unsigned) (size >> 10), offset / 1048576.0,
					(unsigned) (offset / 512), (unsigned) (us / 1000), t / 1e6);
			}
		}
	}
	uint64_t us = now_us () - start;
	printf ("sdbench: round %u, %u KB reads: %u MB in %u ms, %.2f MB/s, %u reads, %u slow (%u ms of them",
		round, (unsigned) (size >> 10), (unsigned) (REGION >> 20), (unsigned) (us / 1000),
		REGION / 1048576.0 / (us / 1e6), reads, slow, (unsigned) (slow_us / 1000));
	if (slow)
	{
		printf ("; from %.3f MB to %.3f MB", first_slow / 1048576.0, last_slow / 1048576.0);
	}
	printf (")\n");
	return true;
}

int main (void)
{
	int fd = open (SDBENCH_FILE, O_RDONLY);
	off_t size = fd >= 0 ? lseek (fd, 0, SEEK_END) : -1;
	uint8_t *buffer = heap_caps_aligned_alloc (64, MAX_READ, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
	if (fd < 0 || size < (off_t) REGION || !buffer)
	{
		printf ("sdbench: can't use %s (%s)\n", SDBENCH_FILE, fd < 0 ? "no file" : !buffer ? "no buffer"
			: "shorter than the region");
		return 1;
	}
	printf ("sdbench: %s, %u MB; each round reads its first %u MB; slow: %u ms more than at %u MB/s\n",
		SDBENCH_FILE, (unsigned) (size >> 20), (unsigned) (REGION >> 20), SLOW_US / 1000, NORMAL_BYTES_PER_US);
	static const uint32_t sizes[] = {16u << 10, 2u << 10, 256u << 10};
	for (unsigned round = 1; ; round++)
	{
		for (unsigned i = 0; i < sizeof sizes / sizeof sizes[0]; i++)
		{
			if (!round_of (fd, buffer, sizes[i], round))
			{
				return 1;
			}
		}
	}
}
