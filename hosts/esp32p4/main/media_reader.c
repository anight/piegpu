/*
 * media_reader.c - see media_reader.h.
 *
 * The file is cut into BLOCK-sized blocks; BLOCKS of them are kept in PSRAM
 * (DMA-capable and aligned, esp_dma_capable_malloc: the SD controller then
 * reads into them by DMA, SOC_SDMMC_PSRAM_DMA_CAPABLE). A read copies from the blocks it covers,
 * waiting for any that isn't there yet. The reads' places are followed by up
 * to CURSORS cursors: an MP4's interleaved samples make one, each of its
 * tracks' sample tables (six a track) one more. The task on core 1 keeps
 * each cursor's block loaded, and for a cursor that streams (it has moved on
 * to a next block twice within STREAM_US: the samples; a table's cursor moves
 * on every few minutes) the AHEAD blocks after it; the block a read waits
 * for first. A block in use or in a cursor's window isn't replaced, but for
 * the block a read waits for, which always gets one: else a read could wait
 * for nothing (once the windows took every block, 25 minutes into a film). The task has the file to itself (its own descriptor); the demo's
 * thread only copies.
 */
#include "media_reader.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_dma_utils.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"

#define BLOCK		(256u << 10)
#define BLOCKS		32			/* 8 MB of PSRAM */
#define CURSORS		16
#define AHEAD		4			/* blocks kept loaded after a streaming cursor's */
#define WAIT_US		5000000			/* the most a read waits for its block */
#define SLOW_LOAD_US	50000			/* a block's load counted as slow (normal: 16 ms) */
#define STREAM_US	20000000		/* two moves on within this: a cursor streams */

enum {EMPTY, LOADING, READY, FAILED};

typedef struct
{
	int64_t block;
	int state;
	unsigned pins;				/* reads copying from it */
	uint32_t used;				/* the clock at its last use */
	uint32_t bytes;				/* valid (the last block is shorter) */
	bool read;				/* copied from since it was loaded */
	uint8_t *data;
} slot_t;

typedef struct
{
	int64_t block;				/* -1: unused */
	uint32_t used;
	uint64_t moved_us;			/* when it last moved on to a next block */
	bool streaming;				/* it moved on twice within STREAM_US */
} cursor_t;

typedef struct
{
	int fd;
	uint64_t size;
	int64_t blocks;				/* in the file */
	slot_t slot[BLOCKS];
	cursor_t cursor[CURSORS];
	int64_t want;				/* the block a read waits for (-1: none) */
	uint32_t clock;
	SemaphoreHandle_t lock, loaded;
	TaskHandle_t task;

	/* since the last stats (under lock) */
	unsigned reads, waits, loads, slow_loads, failed, unread;
	uint64_t read_bytes, wait_us, load_us, max_load_us;
} reader_t;

static uint64_t now_us (void)
{
	return (uint64_t) esp_timer_get_time ();
}

static int find (reader_t *r, int64_t block)
{
	for (int i = 0; i < BLOCKS; i++)
	{
		if (r->slot[i].state != EMPTY && r->slot[i].block == block)
		{
			return i;
		}
	}
	return -1;
}

/* a block a read waits for or a cursor keeps loaded */
static bool needed (reader_t *r, int64_t block)
{
	if (block == r->want)
	{
		return true;
	}
	for (int c = 0; c < CURSORS; c++)
	{
		const cursor_t *k = &r->cursor[c];
		if (k->block >= 0 && block >= k->block && block <= k->block + (k->streaming ? AHEAD : 0))
		{
			return true;
		}
	}
	return false;
}

/* the next block to load (-1: none): the one waited for, then each cursor's
   window, the most recently used cursor first */
static int64_t next_load (reader_t *r)
{
	if (r->want >= 0 && find (r, r->want) < 0)
	{
		return r->want;
	}
	bool done[CURSORS] = {false};
	for (int n = 0; n < CURSORS; n++)
	{
		int best = -1;
		for (int c = 0; c < CURSORS; c++)
		{
			if (!done[c] && r->cursor[c].block >= 0 && (best < 0 || r->cursor[c].used > r->cursor[best].used))
			{
				best = c;
			}
		}
		if (best < 0)
		{
			break;
		}
		done[best] = true;
		const cursor_t *k = &r->cursor[best];
		for (int64_t b = k->block; b <= k->block + (k->streaming ? AHEAD : 0) && b < r->blocks; b++)
		{
			if (find (r, b) < 0)
			{
				return b;
			}
		}
	}
	return -1;
}

/* a slot to load block into (-1: none): an empty one, else the least
   recently used one nobody copies from or needs; for the block a read waits
   for, else any nobody copies from */
static int victim (reader_t *r, int64_t block)
{
	int best = -1, any = -1;
	for (int i = 0; i < BLOCKS; i++)
	{
		slot_t *s = &r->slot[i];
		if (s->state == EMPTY)
		{
			return i;
		}
		if (s->state == LOADING || s->pins)
		{
			continue;
		}
		if (!needed (r, s->block) && (best < 0 || s->used < r->slot[best].used))
		{
			best = i;
		}
		if (any < 0 || s->used < r->slot[any].used)
		{
			any = i;
		}
	}
	return best >= 0 || block != r->want ? best : any;
}

static void reader_task (void *param)
{
	reader_t *r = param;
	for (;;)
	{
		xSemaphoreTake (r->lock, portMAX_DELAY);
		int64_t block = next_load (r);
		int i = block >= 0 ? victim (r, block) : -1;
		if (i >= 0)
		{
			r->unread += r->slot[i].state == READY && !r->slot[i].read;	/* (loaded for nothing) */
			r->slot[i].block = block;
			r->slot[i].state = LOADING;
			r->slot[i].read = false;
		}
		xSemaphoreGive (r->lock);
		if (i < 0)
		{
			ulTaskNotifyTake (pdTRUE, pdMS_TO_TICKS (20));	/* (a read's news, or a look again) */
			continue;
		}

		slot_t *s = &r->slot[i];
		uint64_t offset = (uint64_t) block * BLOCK;
		uint32_t bytes = r->size - offset < BLOCK ? (uint32_t) (r->size - offset) : BLOCK;
		uint64_t t = now_us ();
		bool ok = lseek (r->fd, (off_t) offset, SEEK_SET) == (off_t) offset;
		for (uint32_t done = 0; ok && done < bytes; )
		{
			ssize_t n = read (r->fd, s->data + done, bytes - done);
			ok = n > 0;
			done += ok ? (uint32_t) n : 0;
		}
		uint64_t us = now_us () - t;

		xSemaphoreTake (r->lock, portMAX_DELAY);
		s->state = ok ? READY : FAILED;
		s->bytes = bytes;
		s->used = ++r->clock;
		r->loads++;
		r->load_us += us;
		r->max_load_us = us > r->max_load_us ? us : r->max_load_us;
		r->slow_loads += us >= SLOW_LOAD_US;
		r->failed += !ok;
		xSemaphoreGive (r->lock);
		xSemaphoreGive (r->loaded);
	}
}

/* a read at block: the cursor there, or a block before (the samples of two
   tracks interleave), follows it; one that moves on to the next blocks
   streams. Else the least recently used cursor moves there, not streaming */
static void follow (reader_t *r, int64_t block)
{
	int c = -1, oldest = 0;
	for (int i = 0; i < CURSORS; i++)
	{
		cursor_t *k = &r->cursor[i];
		if (k->block >= 0 && block >= k->block - 1 && block <= k->block + 2)
		{
			c = i;
			break;
		}
		if (k->block < 0 || (r->cursor[oldest].block >= 0 && k->used < r->cursor[oldest].used))
		{
			oldest = i;
		}
	}
	cursor_t *k;
	if (c < 0)				/* a new place */
	{
		k = &r->cursor[oldest];
		k->block = block;
		k->streaming = false;
		k->moved_us = 0;
	}
	else
	{
		k = &r->cursor[c];
		if (block > k->block)		/* moved on */
		{
			uint64_t now = now_us ();
			k->block = block;
			k->streaming = k->moved_us && now - k->moved_us < STREAM_US;
			k->moved_us = now;
		}
	}
	k->used = ++r->clock;
}

static bool media_read (void *ctx, uint64_t offset, void *buffer, uint32_t bytes)
{
	reader_t *r = ctx;
	if (offset > r->size || bytes > r->size - offset)
	{
		return false;
	}
	uint8_t *p = buffer;
	uint64_t start = now_us ();
	bool waited = false, ok = true;
	uint32_t total = bytes;
	while (ok && bytes)
	{
		int64_t block = (int64_t) (offset / BLOCK);
		uint32_t in = (uint32_t) (offset % BLOCK);
		uint32_t n = BLOCK - in < bytes ? BLOCK - in : bytes;

		xSemaphoreTake (r->lock, portMAX_DELAY);
		follow (r, block);
		int i;
		while ((i = find (r, block)) < 0 || r->slot[i].state == LOADING)
		{
			r->want = block;
			xSemaphoreGive (r->lock);
			xTaskNotifyGive (r->task);
			waited = true;
			xSemaphoreTake (r->loaded, pdMS_TO_TICKS (100));
			xSemaphoreTake (r->lock, portMAX_DELAY);
			if (now_us () - start > WAIT_US)
			{
				break;
			}
		}
		if (i < 0 || r->slot[i].state != READY)
		{
			bool failed = i >= 0 && r->slot[i].state == FAILED;
			if (failed)
			{
				r->slot[i].state = EMPTY;	/* (the next read tries again) */
			}
			r->want = -1;
			xSemaphoreGive (r->lock);
			printf ("media_reader: a read at %llu (block %lld) failed: %s\n", (unsigned long long) offset,
				(long long) block, failed ? "the card's read failed" : "no block after 5 s");
			ok = false;
			break;
		}
		slot_t *s = &r->slot[i];
		s->pins++;
		s->used = ++r->clock;
		s->read = true;
		if (r->want == block)
		{
			r->want = -1;
		}
		xSemaphoreGive (r->lock);
		xTaskNotifyGive (r->task);		/* (the cursor may have moved: load ahead) */

		memcpy (p, s->data + in, n);

		xSemaphoreTake (r->lock, portMAX_DELAY);
		s->pins--;
		xSemaphoreGive (r->lock);
		p += n;
		offset += n;
		bytes -= n;
	}

	xSemaphoreTake (r->lock, portMAX_DELAY);
	r->reads++;
	r->read_bytes += ok ? total : 0;
	if (waited)
	{
		r->waits++;
		r->wait_us += now_us () - start;
	}
	xSemaphoreGive (r->lock);
	return ok;
}

bool media_reader_open (const char *path, pgpu_read_t *read_fn, void **ctx, uint64_t *size)
{
	int fd = open (path, O_RDONLY);
	off_t end = fd >= 0 ? lseek (fd, 0, SEEK_END) : -1;
	reader_t *r = end > 0 ? calloc (1, sizeof *r) : NULL;
	if (!r)
	{
		if (fd >= 0)
		{
			close (fd);
		}
		return false;
	}
	/* DMA-capable PSRAM, aligned for the SD controller and the cache */
	const esp_dma_mem_info_t psram = {.extra_heap_caps = MALLOC_CAP_SPIRAM, .dma_alignment_bytes = 64};
	for (int i = 0; i < BLOCKS; i++)
	{
		void *data = NULL;
		size_t got = 0;
		esp_dma_capable_malloc (BLOCK, &psram, &data, &got);
		r->slot[i].data = data;
		if (!r->slot[i].data)
		{
			printf ("media_reader: no PSRAM for block %d of %d\n", i, BLOCKS);
			return false;			/* (at start-up: nothing to give back to) */
		}
	}
	r->fd = fd;
	r->size = (uint64_t) end;
	r->blocks = (int64_t) ((r->size + BLOCK - 1) / BLOCK);
	r->want = -1;
	for (int c = 0; c < CURSORS; c++)
	{
		r->cursor[c].block = -1;
	}
	r->lock = xSemaphoreCreateMutex ();
	r->loaded = xSemaphoreCreateBinary ();
	if (!r->lock || !r->loaded
	    || xTaskCreatePinnedToCore (reader_task, "media_reader", 4096, r, 3, &r->task, 1) != pdPASS)
	{
		printf ("media_reader: no task\n");
		return false;
	}
	printf ("media_reader: %s, %llu MB, read ahead in %u KB blocks, %u of them in PSRAM\n", path,
		(unsigned long long) (r->size >> 20), BLOCK >> 10, BLOCKS);
	*read_fn = media_read;
	*ctx = r;
	*size = r->size;
	return true;
}

void media_reader_stats (void *ctx, char *line, size_t bytes)
{
	reader_t *r = ctx;
	xSemaphoreTake (r->lock, portMAX_DELAY);
	snprintf (line, bytes, "%u reads, %u KB; %u blocks loaded (%u slow, %u failed, %u replaced unread), %u ms "
		  "each on average, at most %u ms; reads waited %u times, %u ms", r->reads,
		  (unsigned) (r->read_bytes >> 10), r->loads, r->slow_loads, r->failed, r->unread,
		  r->loads ? (unsigned) (r->load_us / r->loads / 1000) : 0, (unsigned) (r->max_load_us / 1000), r->waits,
		  (unsigned) (r->wait_us / 1000));
	r->reads = r->waits = r->loads = r->slow_loads = r->failed = r->unread = 0;
	r->read_bytes = r->wait_us = r->load_us = r->max_load_us = 0;
	xSemaphoreGive (r->lock);
}
