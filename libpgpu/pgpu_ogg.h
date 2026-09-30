/*
 * pgpu_ogg.h - an Ogg Vorbis file, page by page, for the GPU's audio stream
 * (pgpu_audio_open with PGPU_AUDIO_VORBIS): the Ogg pages of its Vorbis
 * stream in order, with the time of each one's first sample and where it is
 * in the file, and the stream's header packets (identification, comment,
 * setup) as the open's config. The pages go to the RPi as they are.
 *
 * The file is read through a callback (pgpu_read_t, as pgpu_mp4's): a page's
 * header at a time, no index. The first logical stream is taken (its first
 * page must begin a Vorbis stream); pages of other streams in the file are
 * skipped, and bytes that aren't a page are skipped to the next one. The
 * config carries the comment header emptied (its title and artist are kept
 * here: it may hold whole pictures), in pages of its own with their CRCs.
 * The duration is the last page's granule position.
 */
#ifndef PGPU_OGG_H
#define PGPU_OGG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "pgpu.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PGPU_OGG_MAX_TEXT	64		/* bytes of a title or artist kept, with its NUL */
#define PGPU_OGG_MAX_CONFIG	16256		/* bytes of the header pages (AUDIO_OPEN's, in one packet) */

typedef struct
{
	pgpu_read_t read;
	void *ctx;
	uint64_t size;				/* of the file */
	const uint8_t *memory;			/* pgpu_ogg_open_memory's */

	uint32_t serial;			/* the Vorbis stream's */
	uint32_t sample_rate, channels;
	uint32_t bitrate;			/* nominal, bits a second (0: not given) */
	uint8_t config[PGPU_OGG_MAX_CONFIG];	/* the header pages (pgpu_audio_open's config) */
	uint32_t config_size;
	uint64_t start;				/* the first page of sound */
	int64_t duration_us;
	char title[PGPU_OGG_MAX_TEXT];		/* the comments' TITLE and ARTIST (UTF-8, cut), or "" */
	char artist[PGPU_OGG_MAX_TEXT];
	bool error;				/* a read failed */

	/* where pgpu_ogg_next is */
	uint64_t offset;			/* the next page's, or where to look for it */
	uint64_t granule;			/* samples up to the page before */
	uint32_t next;				/* pages returned */
	uint32_t skipped;			/* bytes skipped between pages */
} pgpu_ogg_t;

typedef struct
{
	uint64_t offset;			/* in the file */
	uint32_t size;				/* the whole page */
	int64_t pts_us;				/* its first sample's */
} pgpu_ogg_sample_t;

/* false if it's not an Ogg Vorbis file this reads (or a read failed, or its
   header packets need more than PGPU_OGG_MAX_CONFIG) */
bool pgpu_ogg_open (pgpu_ogg_t *ogg, pgpu_read_t read, void *ctx, uint64_t size);
bool pgpu_ogg_open_memory (pgpu_ogg_t *ogg, const void *file, size_t size);
/* the next page of sound; false at the end (or ogg->error) */
bool pgpu_ogg_next (pgpu_ogg_t *ogg, pgpu_ogg_sample_t *sample);
/* back to the first page of sound */
void pgpu_ogg_rewind (pgpu_ogg_t *ogg);
/* to be heard from target (the file's time, microseconds) on: *sample is the
   page before the first page at or after target (the RPi's decoder only
   primes itself on it: a packet's sound needs the one before; the first
   page of sound is its own), pgpu_ogg_next goes on from there. Past the end:
   the first page. False if there's no page (or ogg->error) */
bool pgpu_ogg_seek (pgpu_ogg_t *ogg, int64_t target_us, pgpu_ogg_sample_t *sample);

#ifdef __cplusplus
}
#endif

#endif
