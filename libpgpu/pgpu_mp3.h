/*
 * pgpu_mp3.h - an MP3 file (MPEG audio: layer 3, also layers 1 and 2), frame
 * by frame, for the GPU's audio stream (pgpu_audio_open with PGPU_AUDIO_MP3):
 * the frames in order with their presentation times and where they are in
 * the file, and the first frame's header (the open's config). The frames go
 * to the RPi as they are.
 *
 * The file is read through a callback (pgpu_read_t, as pgpu_mp4's: an offset
 * and a size), a frame's header at a time: no index, however long the file.
 * Skips ID3v2 tags at the start (their title and artist are kept), an ID3v1
 * tag at the end and a Xing or Info frame (a VBR file's frame count: the
 * duration); bytes that aren't a frame of the stream (the same version, layer
 * and sample rate) are skipped to the next one. The duration of a file
 * without a frame count is estimated from its bitrate until pgpu_mp3_next has
 * reached its end; then it's exact. Free-format streams aren't read.
 */
#ifndef PGPU_MP3_H
#define PGPU_MP3_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "pgpu.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PGPU_MP3_MAX_TEXT	64		/* bytes of a title or artist kept, with its NUL */

typedef struct
{
	pgpu_read_t read;
	void *ctx;
	uint64_t size;				/* of the file */
	const uint8_t *memory;			/* pgpu_mp3_open_memory's */

	uint8_t header[4];			/* the first frame's (pgpu_audio_open's config) */
	uint32_t sample_rate, channels;
	uint32_t layer;				/* 1, 2 or 3 */
	uint32_t bitrate_kbps;			/* the first frame's */
	uint32_t frame_samples;			/* frames of sound a frame (1152, 576, 384) */
	uint64_t start, end;			/* the frames' bytes in the file */
	int64_t duration_us;
	bool duration_exact;			/* a frame count or a pass to the end (else the bitrate's) */
	char title[PGPU_MP3_MAX_TEXT];		/* the ID3 tag's (UTF-8, cut), or "" */
	char artist[PGPU_MP3_MAX_TEXT];
	bool error;				/* a read failed */

	/* where pgpu_mp3_next is */
	uint64_t offset;			/* the next frame's, or where to look for it */
	uint32_t next;				/* frames returned */
	uint32_t skipped;			/* bytes skipped between frames */
} pgpu_mp3_t;

typedef struct
{
	uint64_t offset;			/* in the file */
	uint32_t size;
	int64_t pts_us;
} pgpu_mp3_sample_t;

/* false if it's not an MPEG audio file this reads (or a read failed) */
bool pgpu_mp3_open (pgpu_mp3_t *mp3, pgpu_read_t read, void *ctx, uint64_t size);
bool pgpu_mp3_open_memory (pgpu_mp3_t *mp3, const void *file, size_t size);
/* the next frame; false at the end (or mp3->error) */
bool pgpu_mp3_next (pgpu_mp3_t *mp3, pgpu_mp3_sample_t *sample);
/* back to the first frame */
void pgpu_mp3_rewind (pgpu_mp3_t *mp3);

#ifdef __cplusplus
}
#endif

#endif
