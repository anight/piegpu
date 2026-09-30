/*
 * mp3.h - MPEG audio frames (MP3, and layers 1 and 2) to 16-bit stereo
 * frames, with minimp3 (minimp3/, CC0): mono doubled, the volume applied.
 * Plain C: the RPi's audio (gpu/audio) and a test on a PC use the same file.
 */
#ifndef GPU_AUDIO_MP3_H
#define GPU_AUDIO_MP3_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MP3_MAX_FRAMES	1152		/* frames a frame decodes to (MPEG-1 layers 2 and 3) */

typedef struct mp3_s mp3_t;

/* a decoder for the stream whose first frame has this 4-byte header; NULL if
   it isn't one this decodes (free format, a reserved field). *rate and
   *channels: what it decodes to */
mp3_t *mp3_open (const uint8_t *header, unsigned header_bytes, unsigned *rate, unsigned *channels);
void mp3_close (mp3_t *mp3);

/* volume: 0 (silent) to 65536 (full scale) */
void mp3_set_volume (mp3_t *mp3, unsigned volume);

/* one frame (its header, then the rest) into up to MP3_MAX_FRAMES frames (L,
   R) at out: the frames written, 0 if it gave none (layer 3: its bit
   reservoir's data was in frames before the first), -1 if it's broken
   (mp3_error says why; the next one decodes on its own) */
int mp3_decode (mp3_t *mp3, const uint8_t *frame, unsigned bytes, int16_t *out);
const char *mp3_error (mp3_t *mp3);

#ifdef __cplusplus
}
#endif

#endif
