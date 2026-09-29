/*
 * pgpu_mp4.h - the H.264 track of an MP4 file, sample by sample, for the
 * GPU's video (pgpu_video_*): the samples in decode order with their
 * presentation times, where they are in the file, and the avcC (the decoder's
 * configuration, for pgpu_video_open with PGPU_VIDEO_AVCC). The samples go to
 * the RPi as they are (NAL units with length prefixes).
 *
 * The file is read through a callback (pgpu_read_t: an offset and a size, so a
 * file on an SD card is read by the filesystem as it's needed): the boxes by
 * their headers (the moov may come after the media data), the sample tables
 * (stts, ctts, stss, stsz, stsc, stco/co64) through small windows, however
 * long the file. A file in memory: pgpu_mp4_open_memory. Reads the first video
 * track with an avcC and the start of its edit list (elst: the times as
 * players show them); no fragmented MP4.
 */
#ifndef PGPU_MP4_H
#define PGPU_MP4_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "pgpu.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PGPU_MP4_MAX_AVCC	256		/* bytes: SPS and PPS of any usual stream */
#define PGPU_MP4_WINDOW		256		/* bytes of a sample table read at a time */
#define PGPU_MP4_MAX_TITLE	64		/* bytes of the title kept, with its NUL */

typedef struct
{
	uint64_t offset;			/* of the first entry in the file */
	uint32_t count;
	uint32_t entry;				/* bytes an entry */
	uint64_t window_offset;			/* what window holds */
	uint32_t window_bytes;
	uint8_t window[PGPU_MP4_WINDOW];
} pgpu_mp4_table_t;

typedef struct
{
	pgpu_read_t read;
	void *ctx;
	uint64_t size;				/* of the file */
	const uint8_t *memory;			/* pgpu_mp4_open_memory's */

	uint32_t width, height;			/* coded (from avc1) */
	uint32_t timescale;			/* the track's, ticks a second */
	uint32_t samples;
	int64_t duration_us;			/* of the track */
	int64_t shift;				/* the edit list's start, ticks (subtracted) */
	uint8_t avcc[PGPU_MP4_MAX_AVCC];	/* the avcC box's payload */
	uint32_t avcc_size;
	char title[PGPU_MP4_MAX_TITLE];		/* the metadata's (UTF-8, cut), or "" */
	bool error;				/* a read failed or the tables are broken */

	pgpu_mp4_table_t stts, ctts, stss, stsz, stsc, stco;
	uint32_t stsz_fixed;			/* all samples this size (then no stsz table) */
	bool co64;
	bool ctts_signed;

	/* where pgpu_mp4_next is (runs of the tables) */
	uint32_t next;				/* the next sample */
	uint64_t dts;				/* its decode time, ticks */
	uint32_t stts_i, stts_left;
	uint32_t ctts_i, ctts_left;
	uint32_t stss_i;
	uint32_t stsc_i, chunk, chunk_left;	/* the chunk and the samples left in it */
	uint64_t offset;			/* the next sample's in the file */
} pgpu_mp4_t;

typedef struct
{
	uint64_t offset;			/* in the file */
	uint32_t size;
	int64_t pts_us, dts_us;
	bool keyframe;
} pgpu_mp4_sample_t;

/* false if it's not an MP4 with an H.264 track this reads (or a read failed) */
bool pgpu_mp4_open (pgpu_mp4_t *mp4, pgpu_read_t read, void *ctx, uint64_t size);
bool pgpu_mp4_open_memory (pgpu_mp4_t *mp4, const void *file, size_t size);
/* the next sample in decode order; false at the end (or mp4->error) */
bool pgpu_mp4_next (pgpu_mp4_t *mp4, pgpu_mp4_sample_t *sample);
/* back to the first sample */
void pgpu_mp4_rewind (pgpu_mp4_t *mp4);
/* read from the file (the callback of the open) */
bool pgpu_mp4_read (pgpu_mp4_t *mp4, uint64_t offset, void *buffer, uint32_t bytes);

#ifdef __cplusplus
}
#endif

#endif
