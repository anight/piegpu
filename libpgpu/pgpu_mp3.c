/*
 * pgpu_mp3.c - see pgpu_mp3.h. ISO/IEC 11172-3 and 13818-3 (the frame
 * header), MPEG 2.5's lower rates, ID3v2.2 to 2.4 and ID3v1 (the tags), the
 * Xing/Info frame of LAME and others.
 */
#include "pgpu_mp3.h"
#include <string.h>

typedef struct
{
	unsigned version;			/* 3 MPEG-1, 2 MPEG-2, 0 MPEG-2.5 (the header's bits) */
	unsigned layer;				/* 1, 2, 3 */
	unsigned kbps, rate, channels;
	unsigned bytes;				/* of the frame, with its padding */
	unsigned samples;			/* frames of sound */
	bool crc;
} header_t;

/* a frame header: false if it isn't one (or free format) */
static bool parse_header (const uint8_t *h, header_t *f)
{
	static const uint16_t kbps[2][3][15] = {
		{	/* MPEG-1: layers 1, 2, 3 */
			{0, 32, 64, 96, 128, 160, 192, 224, 256, 288, 320, 352, 384, 416, 448},
			{0, 32, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384},
			{0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320}},
		{	/* MPEG-2 and 2.5 */
			{0, 32, 48, 56, 64, 80, 96, 112, 128, 144, 160, 176, 192, 224, 256},
			{0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160},
			{0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160}}};
	static const unsigned rates[3] = {44100, 48000, 32000};

	unsigned version = (h[1] >> 3) & 3, layer_bits = (h[1] >> 1) & 3;
	unsigned bitrate = h[2] >> 4, rate = (h[2] >> 2) & 3;
	if (   h[0] != 0xFF || (h[1] & 0xE0) != 0xE0 || version == 1 || layer_bits == 0
	    || bitrate == 0 || bitrate == 15 || rate == 3)
	{
		return false;
	}
	f->version = version;
	f->layer = 4 - layer_bits;
	f->kbps = kbps[version == 3 ? 0 : 1][f->layer - 1][bitrate];
	f->rate = rates[rate] >> (version == 3 ? 0 : version == 2 ? 1 : 2);
	f->channels = (h[3] >> 6) == 3 ? 1 : 2;
	f->crc = !(h[1] & 1);
	unsigned padding = (h[2] >> 1) & 1;
	if (f->layer == 1)
	{
		f->bytes = (12000 * f->kbps / f->rate + padding) * 4;
		f->samples = 384;
	}
	else
	{
		unsigned per = f->layer == 3 && version != 3 ? 72000 : 144000;
		f->bytes = per * f->kbps / f->rate + padding;
		f->samples = f->layer == 3 && version != 3 ? 576 : 1152;
	}
	return true;
}

static bool read_file (pgpu_mp3_t *m, uint64_t offset, void *buffer, uint32_t bytes)
{
	if (offset > m->size || bytes > m->size - offset || !m->read (m->ctx, offset, buffer, bytes))
	{
		m->error = true;
		return false;
	}
	return true;
}

/* a frame of the stream (the first's version, layer and rate) at offset */
static bool frame_at (pgpu_mp3_t *m, uint64_t offset, header_t *f)
{
	uint8_t h[4];
	header_t first;
	return    offset <= m->end && m->end - offset >= 4 && read_file (m, offset, h, 4) && parse_header (h, f)
	       && f->bytes <= m->end - offset
	       && (!m->header[0] || (   parse_header (m->header, &first) && first.version == f->version
				     && first.layer == f->layer && first.rate == f->rate));
}

/* the next frame at or after offset: one followed by another (or the end),
   so that bytes that only look like a header aren't taken */
static bool find_frame (pgpu_mp3_t *m, uint64_t offset, uint64_t *found, header_t *f)
{
	uint8_t w[256];
	while (offset < m->end && m->end - offset >= 4)
	{
		uint32_t n = m->end - offset < sizeof w ? (uint32_t) (m->end - offset) : sizeof w;
		if (!read_file (m, offset, w, n))
		{
			return false;
		}
		for (uint32_t i = 0; i + 1 < n; i++)
		{
			header_t next;
			if (   w[i] == 0xFF && (w[i + 1] & 0xE0) == 0xE0 && frame_at (m, offset + i, f)
			    && (   offset + i + f->bytes == m->end
				|| frame_at (m, offset + i + f->bytes, &next)))
			{
				*found = offset + i;
				return true;
			}
		}
		offset += n - 1;			/* (a header across the windows) */
	}
	return false;
}

/* ---- ID3 tags ------------------------------------------------------------ */

static uint32_t syncsafe (const uint8_t *p)
{
	return (uint32_t) p[0] << 21 | p[1] << 14 | p[2] << 7 | p[3];
}

/* a code point to UTF-8 at out (n bytes left, one kept for the NUL): false
   when it doesn't fit */
static bool put_utf8 (char **out, size_t *n, uint32_t c)
{
	uint8_t b[3];
	size_t k = 0;
	if (c < 0x80)
	{
		b[k++] = (uint8_t) c;
	}
	else if (c < 0x800)
	{
		b[k++] = (uint8_t) (0xC0 | c >> 6);
		b[k++] = (uint8_t) (0x80 | (c & 0x3F));
	}
	else
	{
		b[k++] = (uint8_t) (0xE0 | c >> 12);
		b[k++] = (uint8_t) (0x80 | ((c >> 6) & 0x3F));
		b[k++] = (uint8_t) (0x80 | (c & 0x3F));
	}
	if (k >= *n)
	{
		return false;
	}
	memcpy (*out, b, k);
	*out += k;
	*n -= k;
	return true;
}

/* an ID3 text (its encoding byte first) to UTF-8, up to its first NUL; cut
   between characters */
static void id3_text (const uint8_t *t, size_t bytes, char *out, size_t out_bytes)
{
	char *o = out;
	size_t n = out_bytes;
	if (bytes)
	{
		unsigned encoding = t[0];
		size_t i = 1;
		bool big = encoding == 2;			/* UTF-16BE; 1: UTF-16 after a BOM */
		if (encoding == 1 && bytes >= 3)
		{
			big = t[1] == 0xFE;
			i = 3;
		}
		while (i < bytes)
		{
			uint32_t c;
			if (encoding == 1 || encoding == 2)
			{
				if (i + 1 >= bytes)
				{
					break;
				}
				c = big ? (uint32_t) t[i] << 8 | t[i + 1] : (uint32_t) t[i + 1] << 8 | t[i];
				i += 2;
				if (c >= 0xD800 && c < 0xE000)
				{
					c = '?';			/* (outside the BMP: not kept) */
					i += 2;
				}
			}
			else if (encoding == 3)			/* UTF-8: as it is, whole sequences */
			{
				size_t k = t[i] < 0x80 ? 1 : t[i] < 0xE0 ? 2 : t[i] < 0xF0 ? 3 : 4;
				if (!t[i] || i + k > bytes || k >= n)
				{
					break;
				}
				memcpy (o, t + i, k);
				o += k;
				n -= k;
				i += k;
				continue;
			}
			else					/* ISO-8859-1 */
			{
				c = t[i++];
			}
			if (!c || !put_utf8 (&o, &n, c))
			{
				break;
			}
		}
	}
	*o = '\0';
}

/* the title and artist of an ID3v2 tag at p (h its 10-byte header) */
static void id3v2_frames (pgpu_mp3_t *m, uint64_t p, const uint8_t *h)
{
	unsigned version = h[3];
	uint64_t end = p + 10 + syncsafe (h + 6);
	if (version == 3 && (h[5] & 0x80))
	{
		return;					/* (unsynchronised as a whole: not read) */
	}
	p += 10;
	if (version >= 3 && (h[5] & 0x40))		/* an extended header */
	{
		uint8_t e[4];
		if (!read_file (m, p, e, 4))
		{
			return;
		}
		p += version == 4 ? syncsafe (e) : 4 + ((uint32_t) e[0] << 24 | e[1] << 16 | e[2] << 8 | e[3]);
	}

	unsigned header = version == 2 ? 6 : 10;
	while (p + header <= end)
	{
		uint8_t f[10];
		if (!read_file (m, p, f, header) || !f[0])	/* (0: the padding) */
		{
			return;
		}
		uint32_t size =   version == 2 ? (uint32_t) f[3] << 16 | f[4] << 8 | f[5]
				: version == 4 ? syncsafe (f + 4)
				: (uint32_t) f[4] << 24 | f[5] << 16 | f[6] << 8 | f[7];
		uint64_t data = p + header;
		p = data + size;
		if (p > end)
		{
			return;
		}
		char *out =   !memcmp (f, version == 2 ? "TT2" : "TIT2", version == 2 ? 3 : 4) ? m->title
			    : !memcmp (f, version == 2 ? "TP1" : "TPE1", version == 2 ? 3 : 4) ? m->artist : NULL;
		if (!out)
		{
			continue;
		}
		if (version == 4 && (f[9] & 0x01))	/* a data length first */
		{
			data += 4;
			size = size >= 4 ? size - 4 : 0;
		}
		if (   (version == 4 && (f[9] & 0x0E))	/* compressed, encrypted, unsynchronised */
		    || (version == 3 && (f[9] & 0xC0)))	/* compressed, encrypted */
		{
			continue;
		}
		uint8_t t[2 * PGPU_MP3_MAX_TEXT + 4];
		uint32_t n = size < sizeof t ? size : sizeof t;
		if (read_file (m, data, t, n))
		{
			id3_text (t, n, out, PGPU_MP3_MAX_TEXT);
		}
	}
}

/* an ID3v1 field: ISO-8859-1, spaces or NULs after it */
static void id3v1_text (const uint8_t *t, size_t bytes, char *out)
{
	uint8_t s[31];
	while (bytes && (t[bytes - 1] == ' ' || !t[bytes - 1]))
	{
		bytes--;
	}
	s[0] = 0;					/* (the encoding: ISO-8859-1) */
	memcpy (s + 1, t, bytes);
	id3_text (s, bytes + 1, out, PGPU_MP3_MAX_TEXT);
}

/* ---- the file ------------------------------------------------------------ */

static bool open_file (pgpu_mp3_t *m)
{
	/* ID3v2 tags, one after another */
	uint64_t p = 0;
	uint8_t h[10];
	while (   m->size - p >= 10 && read_file (m, p, h, 10) && !memcmp (h, "ID3", 3)
	       && h[3] >= 2 && h[3] <= 4 && !((h[6] | h[7] | h[8] | h[9]) & 0x80))
	{
		id3v2_frames (m, p, h);
		p += 10 + syncsafe (h + 6) + (h[3] == 4 && (h[5] & 0x10) ? 10 : 0);	/* (a footer) */
	}
	m->error = false;				/* (a broken tag is fine) */
	m->end = m->size;

	/* an ID3v1 tag at the end */
	uint8_t v1[128];
	if (m->size - p >= 128 && read_file (m, m->size - 128, v1, 128) && !memcmp (v1, "TAG", 3))
	{
		m->end = m->size - 128;
		if (!m->title[0])
		{
			id3v1_text (v1 + 3, 30, m->title);
		}
		if (!m->artist[0])
		{
			id3v1_text (v1 + 33, 30, m->artist);
		}
	}
	if (m->error || p >= m->end)
	{
		return false;
	}

	header_t f;
	uint64_t first;
	if (!find_frame (m, p, &first, &f) || !read_file (m, first, m->header, 4))
	{
		return false;
	}
	m->sample_rate = f.rate;
	m->channels = f.channels;
	m->layer = f.layer;
	m->bitrate_kbps = f.kbps;
	m->frame_samples = f.samples;
	m->start = first;

	/* a Xing or Info frame (LAME's and others', layer 3): after the side
	   information; not sound: skipped, its frame count the duration */
	uint32_t frames = 0;
	if (f.layer == 3)
	{
		unsigned side = f.version == 3 ? (f.channels == 1 ? 17 : 32) : (f.channels == 1 ? 9 : 17);
		uint8_t x[12];
		uint64_t at = first + 4 + (f.crc ? 2 : 0) + side;
		if (   at + sizeof x <= first + f.bytes && read_file (m, at, x, sizeof x)
		    && (!memcmp (x, "Xing", 4) || !memcmp (x, "Info", 4)))
		{
			m->start = first + f.bytes;
			if (x[7] & 1)			/* the frame count's there */
			{
				frames = (uint32_t) x[8] << 24 | x[9] << 16 | x[10] << 8 | x[11];
			}
		}
	}
	if (frames)
	{
		m->duration_us = (int64_t) ((uint64_t) frames * f.samples * 1000000 / f.rate);
		m->duration_exact = true;
	}
	else
	{
		m->duration_us = (int64_t) ((m->end - m->start) * 8000 / f.kbps);
	}

	pgpu_mp3_rewind (m);
	return !m->error;
}

bool pgpu_mp3_open (pgpu_mp3_t *m, pgpu_read_t read, void *ctx, uint64_t size)
{
	memset (m, 0, sizeof *m);
	m->read = read;
	m->ctx = ctx;
	m->size = size;
	return open_file (m);
}

static bool memory_read (void *ctx, uint64_t offset, void *buffer, uint32_t bytes)
{
	memcpy (buffer, ((pgpu_mp3_t *) ctx)->memory + offset, bytes);	/* (checked by the caller) */
	return true;
}

bool pgpu_mp3_open_memory (pgpu_mp3_t *m, const void *file, size_t size)
{
	memset (m, 0, sizeof *m);
	m->read = memory_read;
	m->ctx = m;
	m->size = size;
	m->memory = (const uint8_t *) file;
	return open_file (m);
}

void pgpu_mp3_rewind (pgpu_mp3_t *m)
{
	m->offset = m->start;
	m->next = 0;
	m->skipped = 0;
}

bool pgpu_mp3_next (pgpu_mp3_t *m, pgpu_mp3_sample_t *sample)
{
	if (m->error)
	{
		return false;
	}
	header_t f;
	uint64_t at = m->offset;
	if (!frame_at (m, at, &f))			/* not a frame here: the next one */
	{
		if (m->error || !find_frame (m, at + 1, &at, &f))
		{
			if (!m->error && !m->duration_exact)	/* the end: the duration's known */
			{
				m->duration_us = (int64_t) ((uint64_t) m->next * m->frame_samples * 1000000
							    / m->sample_rate);
				m->duration_exact = true;
			}
			return false;
		}
		m->skipped += (uint32_t) (at - m->offset);
	}
	sample->offset = at;
	sample->size = f.bytes;
	sample->pts_us = (int64_t) ((uint64_t) m->next * m->frame_samples * 1000000 / m->sample_rate);
	m->offset = at + f.bytes;
	m->next++;
	return true;
}
