/*
 * aac.c - see aac.h
 */
#include "aac.h"
#include "neaacdec.h"
#include <stdlib.h>

struct aac_s
{
	NeAACDecHandle dec;
	unsigned channels;		/* FAAD2's output: 1 or 2 (downmatrix on) */
	unsigned volume;		/* 0..65536 */
	unsigned char last_error;
};

aac_t *aac_open (const uint8_t *asc, unsigned asc_bytes, unsigned *rate, unsigned *channels)
{
	aac_t *aac = (aac_t *) calloc (1, sizeof *aac);
	if (!aac)
	{
		return NULL;
	}
	aac->dec = NeAACDecOpen ();
	if (!aac->dec)
	{
		free (aac);
		return NULL;
	}

	/* float out (the volume is applied in the conversion), more than two
	   channels mixed down to stereo */
	NeAACDecConfigurationPtr config = NeAACDecGetCurrentConfiguration (aac->dec);
	config->outputFormat = FAAD_FMT_FLOAT;
	config->downMatrix = 1;
	NeAACDecSetConfiguration (aac->dec, config);

	unsigned long r = 0;
	unsigned char c = 0;
	if (NeAACDecInit2 (aac->dec, (unsigned char *) asc, asc_bytes, &r, &c) < 0)
	{
		aac_close (aac);
		return NULL;
	}
	aac->channels = c > 2 ? 2 : c;
	aac->volume = 65536;
	*rate = (unsigned) r;
	*channels = c;
	return aac;
}

void aac_close (aac_t *aac)
{
	if (aac)
	{
		NeAACDecClose (aac->dec);
		free (aac);
	}
}

void aac_set_volume (aac_t *aac, unsigned volume)
{
	aac->volume = volume > 65536 ? 65536 : volume;
}

int aac_decode (aac_t *aac, const uint8_t *unit, unsigned bytes, int16_t *out)
{
	NeAACDecFrameInfo info;
	float *pcm = (float *) NeAACDecDecode (aac->dec, &info, (unsigned char *) unit, bytes);
	if (info.error)
	{
		aac->last_error = info.error;
		return -1;
	}
	if (!pcm || !info.samples || !info.channels)
	{
		return 0;
	}

	/* FAAD2's float samples are in ±1.0 (output.c: FLOAT_SCALE) */
	unsigned channels = info.channels;
	unsigned frames = info.samples / channels;
	if (frames > AAC_MAX_FRAMES)
	{
		frames = AAC_MAX_FRAMES;
	}
	float gain = aac->volume / 65536.0f * 32768.0f;
	for (unsigned i = 0; i < frames; i++)
	{
		float l = pcm[i * channels] * gain;
		float r = channels > 1 ? pcm[i * channels + 1] * gain : l;
		l = l > 32767.0f ? 32767.0f : l < -32768.0f ? -32768.0f : l;
		r = r > 32767.0f ? 32767.0f : r < -32768.0f ? -32768.0f : r;
		out[2 * i] = (int16_t) (l < 0 ? l - 0.5f : l + 0.5f);
		out[2 * i + 1] = (int16_t) (r < 0 ? r - 0.5f : r + 0.5f);
	}
	return (int) frames;
}

const char *aac_error (aac_t *aac)
{
	return NeAACDecGetErrorMessage (aac->last_error);
}
