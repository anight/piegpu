/*
 * vorbis.c - see vorbis.h. libogg takes the pages apart (ogg_sync, the page
 * CRCs; ogg_stream, the packets), Tremor decodes the packets (its synthesis
 * layer, as libvorbis' is used).
 */
#include "vorbis.h"
#include <stdlib.h>
#include <string.h>
#include "tremor/ivorbiscodec.h"

struct vorbis_s
{
	ogg_sync_state sync;
	ogg_stream_state stream;
	vorbis_info info;
	vorbis_comment comment;
	vorbis_dsp_state dsp;
	vorbis_block block;
	unsigned channels;
	unsigned volume;			/* 0..65536 */
	unsigned losses;
	long first_page;			/* the first page of sound's number (after the header pages') */
	bool fed;				/* a page of sound has come */
	bool priming;				/* the sound is dropped until a packet is decoded (see vorbis.h) */
	bool decoded;				/* ... and one has been */
};

/* the pages fed so far into the stream (another stream's is refused) */
static void pages_in (vorbis_t *v)
{
	ogg_page page;
	int r;
	while ((r = ogg_sync_pageout (&v->sync, &page)) != 0)
	{
		if (r < 0)
		{
			v->losses++;			/* (bytes skipped to a page) */
			continue;
		}
		if (ogg_stream_pagein (&v->stream, &page) != 0)
		{
			continue;			/* (another stream's) */
		}
		if (!v->fed)
		{
			v->first_page = ogg_page_pageno (&page) + 1;
		}
	}
}

vorbis_t *vorbis_open (const uint8_t *headers, unsigned header_bytes, unsigned *rate, unsigned *channels)
{
	vorbis_t *v = (vorbis_t *) calloc (1, sizeof *v);
	if (!v)
	{
		return NULL;
	}
	ogg_sync_init (&v->sync);
	vorbis_info_init (&v->info);
	vorbis_comment_init (&v->comment);

	/* the header pages: the stream's serial from the first */
	char *buffer = ogg_sync_buffer (&v->sync, (long) header_bytes);
	memcpy (buffer, headers, header_bytes);
	ogg_sync_wrote (&v->sync, (long) header_bytes);
	ogg_page page;
	bool stream = false, ok = false;
	if (ogg_sync_pageout (&v->sync, &page) == 1)
	{
		ogg_stream_init (&v->stream, ogg_page_serialno (&page));
		stream = true;
		ogg_stream_pagein (&v->stream, &page);
		v->first_page = ogg_page_pageno (&page) + 1;
		pages_in (v);
		ogg_packet packet;
		ok = true;
		for (int i = 0; i < 3 && ok; i++)
		{
			int r;
			while ((r = ogg_stream_packetout (&v->stream, &packet)) < 0)
			{
				;			/* (the first page's number isn't 0: a "gap" before it) */
			}
			ok = r == 1 && vorbis_synthesis_headerin (&v->info, &v->comment, &packet) == 0;
		}
	}
	if (   !ok || v->info.channels < 1 || vorbis_synthesis_init (&v->dsp, &v->info) != 0)
	{
		if (stream)
		{
			ogg_stream_clear (&v->stream);
		}
		vorbis_comment_clear (&v->comment);
		vorbis_info_clear (&v->info);
		ogg_sync_clear (&v->sync);
		free (v);
		return NULL;
	}
	vorbis_block_init (&v->dsp, &v->block);
	v->channels = (unsigned) v->info.channels;
	v->volume = 65536;
	*rate = (unsigned) v->info.rate;
	*channels = v->channels;
	return v;
}

void vorbis_close (vorbis_t *v)
{
	if (v)
	{
		vorbis_block_clear (&v->block);
		vorbis_dsp_clear (&v->dsp);
		ogg_stream_clear (&v->stream);
		vorbis_comment_clear (&v->comment);
		vorbis_info_clear (&v->info);
		ogg_sync_clear (&v->sync);
		free (v);
	}
}

void vorbis_set_volume (vorbis_t *v, unsigned volume)
{
	v->volume = volume > 65536 ? 65536 : volume;
}

void vorbis_feed (vorbis_t *v, const uint8_t *page, unsigned bytes)
{
	long number = bytes >= 22 ? (long) ((uint32_t) page[18] | (uint32_t) page[19] << 8 | (uint32_t) page[20] << 16
					    | (uint32_t) page[21] << 24) : -1;
	if (!v->fed)					/* the first page of sound: the stream's first? */
	{
		v->fed = true;
		v->priming = number != v->first_page;
	}
	else if (number == v->first_page)		/* the stream from its start again (a loop): as a new one */
	{
		ogg_stream_reset (&v->stream);
		vorbis_synthesis_restart (&v->dsp);
		v->priming = false;
	}
	char *buffer = ogg_sync_buffer (&v->sync, (long) bytes);
	if (buffer)
	{
		memcpy (buffer, page, bytes);
		ogg_sync_wrote (&v->sync, (long) bytes);
		pages_in (v);
	}
}

static int16_t clamp (float x)
{
	x = x > 32767.0f ? 32767.0f : x < -32768.0f ? -32768.0f : x;
	return (int16_t) (x < 0 ? x - 0.5f : x + 0.5f);
}

/* frames as stereo. Tremor's samples: 16-bit ones shifted left 9 (its
   vorbisfile's ov_read takes them >> 9). Vorbis' order is L R (2), L C R (3),
   FL FR RL RR (4), FL C FR RL RR (5), FL C FR RL RR LFE (6): the centre and
   the rears at -3 dB, the LFE left out, scaled so that nothing clips more
   than the channels would */
static void to_stereo (vorbis_t *v, ogg_int32_t **pcm, int frames, int16_t *out)
{
	const float h = 0.70710678f;
	float gain = v->volume / 65536.0f / 512.0f;
	for (int i = 0; i < frames; i++)
	{
		float l, r;
		switch (v->channels)
		{
		case 1:
			l = r = (float) pcm[0][i];
			break;
		case 2:
			l = (float) pcm[0][i];
			r = (float) pcm[1][i];
			break;
		case 3:
			l = (pcm[0][i] + h * pcm[1][i]) / (1 + h);
			r = (pcm[2][i] + h * pcm[1][i]) / (1 + h);
			break;
		case 4:
			l = (pcm[0][i] + h * pcm[2][i]) / (1 + h);
			r = (pcm[1][i] + h * pcm[3][i]) / (1 + h);
			break;
		default:				/* 5 or more: the first five (the LFE and any more left out) */
			l = (pcm[0][i] + h * pcm[1][i] + h * pcm[3][i]) / (1 + 2 * h);
			r = (pcm[2][i] + h * pcm[1][i] + h * pcm[4][i]) / (1 + 2 * h);
			break;
		}
		out[2 * i] = clamp (l * gain);
		out[2 * i + 1] = clamp (r * gain);
	}
}

int vorbis_decode (vorbis_t *v, int16_t *out, unsigned max_frames, bool *more)
{
	unsigned written = 0;
	*more = false;
	while (written < max_frames)
	{
		/* what's decoded, as much as fits */
		ogg_int32_t **pcm;
		int frames = vorbis_synthesis_pcmout (&v->dsp, &pcm);
		if (frames > 0 && v->priming)
		{
			vorbis_synthesis_read (&v->dsp, frames);
			continue;
		}
		if (frames > 0)
		{
			if ((unsigned) frames > max_frames - written)
			{
				frames = (int) (max_frames - written);
			}
			to_stereo (v, pcm, frames, out + 2 * written);
			vorbis_synthesis_read (&v->dsp, frames);
			written += (unsigned) frames;
			continue;
		}

		/* the next packet */
		ogg_packet packet;
		int r = ogg_stream_packetout (&v->stream, &packet);
		if (r == 0)
		{
			*more = true;			/* (the rest waits for the next page) */
			v->priming = v->priming && !v->decoded;
			break;
		}
		if (r < 0)
		{
			v->losses += !v->priming;	/* (a gap: pages lost; before a stream started mid-way, not) */
			continue;
		}
		if (vorbis_synthesis (&v->block, &packet) == 0)
		{
			vorbis_synthesis_blockin (&v->dsp, &v->block);
			v->decoded = true;
		}
		else
		{
			v->losses++;
		}
	}
	return (int) written;
}

unsigned vorbis_losses (vorbis_t *v)
{
	return v->losses;
}
