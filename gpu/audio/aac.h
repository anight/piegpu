/*
 * aac.h - AAC access units to 16-bit stereo frames, with FAAD2 (faad2/, GPL
 * 2 or later): 5.1 mixed down to stereo (FAAD2's downmatrix), mono doubled,
 * the volume applied. Plain C: the RPi's audio (gpu/audio) and a test on a PC
 * use the same file.
 */
#ifndef GPU_AUDIO_AAC_H
#define GPU_AUDIO_AAC_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AAC_MAX_FRAMES	2048		/* frames an access unit decodes to (HE-AAC's) */

typedef struct aac_s aac_t;

/* a decoder for this AudioSpecificConfig; NULL if FAAD2 won't take it.
   *rate and *channels: what it decodes to (HE-AAC: twice the core's rate) */
aac_t *aac_open (const uint8_t *asc, unsigned asc_bytes, unsigned *rate, unsigned *channels);
void aac_close (aac_t *aac);

/* volume: 0 (silent) to 65536 (full scale) */
void aac_set_volume (aac_t *aac, unsigned volume);

/* one access unit into up to AAC_MAX_FRAMES frames (L, R) at out: the frames
   written, 0 if it held none (the decoder's delay), -1 if it's broken
   (aac_error says why; the next one decodes on its own) */
int aac_decode (aac_t *aac, const uint8_t *unit, unsigned bytes, int16_t *out);
const char *aac_error (aac_t *aac);

#ifdef __cplusplus
}
#endif

#endif
