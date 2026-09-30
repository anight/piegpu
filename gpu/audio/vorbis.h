/*
 * vorbis.h - Ogg Vorbis to 16-bit stereo frames, with Tremor and libogg
 * (tremor/, BSD-3-Clause): whole Ogg pages in, decoded into the room there
 * is; mono doubled, more channels mixed down to stereo (Vorbis' channel
 * order), the volume applied. Plain C: the RPi's audio (gpu/audio) and a
 * test on a PC use the same file.
 */
#ifndef GPU_AUDIO_VORBIS_H
#define GPU_AUDIO_VORBIS_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VORBIS_CHUNK_FRAMES	1024		/* frames a step decodes (about a packet: 2048-sample blocks give 1024) */
#define VORBIS_MAX_PAGE		65307		/* bytes of an Ogg page (27 + 255 + 255 * 255) */

typedef struct vorbis_s vorbis_t;

/* a decoder for the stream whose header packets (identification, comment,
   setup) are in these Ogg pages; NULL if it doesn't take them. *rate and
   *channels: what it decodes to */
vorbis_t *vorbis_open (const uint8_t *headers, unsigned header_bytes, unsigned *rate, unsigned *channels);
void vorbis_close (vorbis_t *vorbis);

/* volume: 0 (silent) to 65536 (full scale) */
void vorbis_set_volume (vorbis_t *vorbis, unsigned volume);

/* the next Ogg page of the stream (its CRC is checked: a broken page is
   dropped, and the packets it held are lost). The first page may be any of
   the stream's: its sound then starts at that page's time. But a decoder
   needs the packet before to decode a packet's sound in full (the first
   packet of a stream has none: its first page's time says so), so when the
   first page isn't the one after the header pages, its sound is dropped
   (and the pages' up to the one where a packet ends): the next page's sound
   then comes in full from that page's time. To be heard from a page, start
   one page before it. The stream's first page again (the stream looping)
   starts it again, as a new stream */
void vorbis_feed (vorbis_t *vorbis, const uint8_t *page, unsigned bytes);

/* decode what was fed into up to max_frames frames (L, R) at out: the
   frames written. *more: all of it is decoded (a packet that goes on in the
   next page waits for it), so the next page can come */
int vorbis_decode (vorbis_t *vorbis, int16_t *out, unsigned max_frames, bool *more);

/* packets lost so far (broken pages, gaps) */
unsigned vorbis_losses (vorbis_t *vorbis);

#ifdef __cplusplus
}
#endif

#endif
