/*
 * aac_enc.h - 16-bit stereo frames to AAC access units (AAC LC, a channel
 * pair): for the recorder's MP4 (gpu/video/mp4_writer.h). Plain C, as aac.c:
 * the RPi and a test on a PC (tools/aacenc) use the same file.
 *
 * A small encoder, not a clever one: long blocks only, no psychoacoustic
 * model (the noise is kept a fixed distance under each band's level, nearer
 * in the high bands), mid/side when the two channels are much alike, the
 * codebook that takes the fewest bits for each band, no bit reservoir: a
 * unit is as big as its sound asks (an MP4 keeps each unit's size).
 */
#ifndef GPU_AUDIO_AAC_ENC_H
#define GPU_AUDIO_AAC_ENC_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AAC_ENC_FRAMES		1024	/* frames a unit */
#define AAC_ENC_MAX_BYTES	1536	/* a unit at most (6144 bits a channel) */

typedef struct aac_enc_s aac_enc_t;

/* an encoder for stereo at 44100 or 48000 Hz; NULL for another rate, or
   without memory */
aac_enc_t *aac_enc_open (unsigned rate);
void aac_enc_close (aac_enc_t *enc);

/* the stream's AudioSpecificConfig (an MP4's esds, an ADTS header's fields) */
void aac_enc_config (const aac_enc_t *enc, uint8_t asc[2]);

/* the next AAC_ENC_FRAMES frames (L, R; NULL: silence, to get the last unit
   out). The unit written to out (its bytes) is of the frames given the call
   before: the first call returns 0. What a decoder makes of the n-th unit are
   the n-th call's... frames: no delay to make up for; the first unit's only
   fade in */
unsigned aac_enc_frame (aac_enc_t *enc, const int16_t *frames, uint8_t *out);

#ifdef __cplusplus
}
#endif

#endif
