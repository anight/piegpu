/*
 * mp3.c - see mp3.h
 */
#include "mp3.h"
#include <stdlib.h>

#define MINIMP3_IMPLEMENTATION
#define MINIMP3_FLOAT_OUTPUT		/* the volume is applied in the conversion */
#include "minimp3/minimp3.h"

struct mp3_s
{
	mp3dec_t dec;
	unsigned rate;			/* the stream's (a frame at another is broken) */
	unsigned volume;		/* 0..65536 */
	const char *last_error;
	float pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
};

/* a frame header's sample rate: 0 if it isn't one (ISO 11172-3 2.4.2.3, and
   13818-3's lower rates; MPEG 2.5's, a quarter) */
static unsigned header_rate (const uint8_t *h)
{
	static const unsigned rates[3] = {44100, 48000, 32000};
	unsigned version = (h[1] >> 3) & 3;		/* 3 MPEG-1, 2 MPEG-2, 0 MPEG-2.5 */
	unsigned layer = (h[1] >> 1) & 3;		/* 3 layer 1, 2 layer 2, 1 layer 3 */
	unsigned bitrate = h[2] >> 4;
	unsigned rate = (h[2] >> 2) & 3;
	if (   h[0] != 0xFF || (h[1] & 0xE0) != 0xE0 || version == 1 || layer == 0
	    || bitrate == 0 || bitrate == 15 || rate == 3)	/* (0: free format) */
	{
		return 0;
	}
	return rates[rate] >> (version == 3 ? 0 : version == 2 ? 1 : 2);
}

mp3_t *mp3_open (const uint8_t *header, unsigned header_bytes, unsigned *rate, unsigned *channels)
{
	if (header_bytes < 4 || !header_rate (header))
	{
		return NULL;
	}
	mp3_t *mp3 = (mp3_t *) calloc (1, sizeof *mp3);
	if (!mp3)
	{
		return NULL;
	}
	mp3dec_init (&mp3->dec);
	mp3->rate = header_rate (header);
	mp3->volume = 65536;
	mp3->last_error = "";
	*rate = mp3->rate;
	*channels = (header[3] >> 6) == 3 ? 1 : 2;
	return mp3;
}

void mp3_close (mp3_t *mp3)
{
	if (mp3)
	{
		free (mp3);
	}
}

void mp3_set_volume (mp3_t *mp3, unsigned volume)
{
	mp3->volume = volume > 65536 ? 65536 : volume;
}

int mp3_decode (mp3_t *mp3, const uint8_t *frame, unsigned bytes, int16_t *out)
{
	if (bytes < 4 || header_rate (frame) != mp3->rate)
	{
		mp3->last_error = "not a frame of the stream (its header, or another sample rate)";
		return -1;
	}

	/* a whole frame alone: minimp3 takes it as it is (mp3d_find_frame: a
	   frame at 0 exactly as long as the data) */
	mp3dec_frame_info_t info;
	int frames = mp3dec_decode_frame (&mp3->dec, frame, (int) bytes, mp3->pcm, &info);
	if (!info.frame_bytes || info.frame_offset || info.frame_bytes != (int) bytes)
	{
		mp3->last_error = "the frame's length doesn't match its header";
		return -1;
	}
	if (frames <= 0)
	{
		return 0;
	}
	if (frames > MP3_MAX_FRAMES)
	{
		frames = MP3_MAX_FRAMES;
	}

	/* minimp3's float samples are in ±1.0 (mp3d_scale_pcm: 1/32768) */
	unsigned channels = (unsigned) info.channels;
	float gain = mp3->volume / 65536.0f * 32768.0f;
	for (int i = 0; i < frames; i++)
	{
		float l = mp3->pcm[i * channels] * gain;
		float r = channels > 1 ? mp3->pcm[i * channels + 1] * gain : l;
		l = l > 32767.0f ? 32767.0f : l < -32768.0f ? -32768.0f : l;
		r = r > 32767.0f ? 32767.0f : r < -32768.0f ? -32768.0f : r;
		out[2 * i] = (int16_t) (l < 0 ? l - 0.5f : l + 0.5f);
		out[2 * i + 1] = (int16_t) (r < 0 ? r - 0.5f : r + 0.5f);
	}
	return frames;
}

const char *mp3_error (mp3_t *mp3)
{
	return mp3->last_error;
}
