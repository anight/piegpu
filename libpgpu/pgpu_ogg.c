/*
 * pgpu_ogg.c - see pgpu_ogg.h. RFC 3533 (the Ogg page), the Vorbis I
 * specification (the header packets, 4.2).
 */
#include "pgpu_ogg.h"
#include <string.h>

#define HEADER		27			/* bytes of a page before its segment table */
#define MAX_PAGE	(HEADER + 255 + 255 * 255)
#define MAX_COMMENT	8192			/* bytes of the comment packet looked at */

typedef struct
{
	uint32_t size;				/* the whole page */
	uint32_t serial;
	uint32_t sequence;
	int64_t granule;			/* -1: no packet ends in it */
	uint8_t flags;				/* 1 continued, 2 first page, 4 last page */
	uint8_t segments;
	uint8_t lacing[255];
} page_t;

static uint32_t le32 (const uint8_t *p)
{
	return (uint32_t) p[0] | (uint32_t) p[1] << 8 | (uint32_t) p[2] << 16 | (uint32_t) p[3] << 24;
}

static bool read_file (pgpu_ogg_t *o, uint64_t offset, void *buffer, uint32_t bytes)
{
	if (offset > o->size || bytes > o->size - offset || !o->read (o->ctx, offset, buffer, bytes))
	{
		o->error = true;
		return false;
	}
	return true;
}

/* a page at offset (within the file) */
static bool page_at (pgpu_ogg_t *o, uint64_t offset, page_t *p)
{
	uint8_t h[HEADER];
	if (offset > o->size || o->size - offset < HEADER || !read_file (o, offset, h, HEADER)
	    || memcmp (h, "OggS", 4) || h[4] != 0)
	{
		return false;
	}
	p->flags = h[5];
	p->granule = (int64_t) ((uint64_t) le32 (h + 6) | (uint64_t) le32 (h + 10) << 32);
	p->serial = le32 (h + 14);
	p->sequence = le32 (h + 18);
	p->segments = h[26];
	if (o->size - offset < HEADER + (uint64_t) p->segments || !read_file (o, offset + HEADER, p->lacing, p->segments))
	{
		return false;
	}
	p->size = HEADER + p->segments;
	for (unsigned i = 0; i < p->segments; i++)
	{
		p->size += p->lacing[i];
	}
	return p->size <= o->size - offset;
}

/* the next page at or after offset: one followed by another (or the end) */
static bool find_page (pgpu_ogg_t *o, uint64_t offset, uint64_t *found, page_t *p)
{
	uint8_t w[256];
	while (offset < o->size && o->size - offset >= HEADER)
	{
		uint32_t n = o->size - offset < sizeof w ? (uint32_t) (o->size - offset) : sizeof w;
		if (!read_file (o, offset, w, n))
		{
			return false;
		}
		for (uint32_t i = 0; i + 4 <= n; i++)
		{
			page_t next;
			if (   !memcmp (w + i, "OggS", 4) && page_at (o, offset + i, p)
			    && (offset + i + p->size == o->size || page_at (o, offset + i + p->size, &next)))
			{
				*found = offset + i;
				return true;
			}
		}
		offset += n - 3;			/* (a capture pattern across the windows) */
	}
	return false;
}

/* ---- the header packets ------------------------------------------------------ */

static uint32_t crc_table[256];

/* Ogg's CRC: polynomial 0x04c11db7, not reflected, from 0 */
static uint32_t ogg_crc (const uint8_t *p, uint32_t n)
{
	if (!crc_table[1])
	{
		for (uint32_t i = 0; i < 256; i++)
		{
			uint32_t c = i << 24;
			for (int k = 0; k < 8; k++)
			{
				c = c & 0x80000000u ? c << 1 ^ 0x04c11db7u : c << 1;
			}
			crc_table[i] = c;
		}
	}
	uint32_t crc = 0;
	for (uint32_t i = 0; i < n; i++)
	{
		crc = crc << 8 ^ crc_table[(crc >> 24) ^ p[i]];
	}
	return crc;
}

/* a page of these whole packets into the config; false if it doesn't fit */
static bool put_page (pgpu_ogg_t *o, uint8_t flags, uint32_t sequence, const uint8_t *const *packets,
		      const uint32_t *sizes, unsigned count)
{
	uint8_t lacing[255];
	unsigned segments = 0;
	uint32_t body = 0;
	for (unsigned k = 0; k < count; k++)
	{
		uint32_t left = sizes[k];
		do				/* 255s, then less (0 after an exact multiple) */
		{
			if (segments == 255)
			{
				return false;
			}
			uint8_t l = left >= 255 ? 255 : (uint8_t) left;
			lacing[segments++] = l;
			left -= l;
			if (l < 255)
			{
				break;
			}
		}
		while (1);
		body += sizes[k];
	}
	uint32_t bytes = HEADER + segments + body;
	if (bytes > PGPU_OGG_MAX_CONFIG - o->config_size)
	{
		return false;
	}
	uint8_t *p = o->config + o->config_size;
	memset (p, 0, HEADER);
	memcpy (p, "OggS", 4);
	p[5] = flags;
	for (int i = 0; i < 4; i++)
	{
		p[14 + i] = (uint8_t) (o->serial >> (8 * i));
		p[18 + i] = (uint8_t) (sequence >> (8 * i));
	}
	p[26] = (uint8_t) segments;
	memcpy (p + HEADER, lacing, segments);
	uint8_t *q = p + HEADER + segments;
	for (unsigned k = 0; k < count; k++)
	{
		memcpy (q, packets[k], sizes[k]);
		q += sizes[k];
	}
	uint32_t crc = ogg_crc (p, bytes);
	for (int i = 0; i < 4; i++)
	{
		p[22 + i] = (uint8_t) (crc >> (8 * i));
	}
	o->config_size += bytes;
	return true;
}

/* a comment's value for key (KEY=value, the key in any case) into out */
static void comment (const uint8_t *c, uint32_t n, const char *key, char *out)
{
	size_t k = strlen (key);
	if (out[0] || n <= k || c[k] != '=')
	{
		return;
	}
	for (size_t i = 0; i < k; i++)
	{
		if ((c[i] | 0x20) != (key[i] | 0x20))
		{
			return;
		}
	}
	size_t len = n - k - 1 < PGPU_OGG_MAX_TEXT - 1 ? n - k - 1 : PGPU_OGG_MAX_TEXT - 1;
	while (len && (c[k + 1 + len] & 0xC0) == 0x80 && len < n - k - 1)
	{
		len--;				/* (cut between UTF-8 characters) */
	}
	memcpy (out, c + k + 1, len);
	out[len] = '\0';
}

/* the comment packet's title and artist (as much of it as was kept) */
static void comments (pgpu_ogg_t *o, const uint8_t *c, uint32_t n)
{
	if (n < 11)
	{
		return;
	}
	uint32_t at = 7 + 4 + le32 (c + 7);		/* the vendor string */
	if (at + 4 > n)
	{
		return;
	}
	uint32_t count = le32 (c + at);
	at += 4;
	for (uint32_t i = 0; i < count && at + 4 <= n; i++)
	{
		uint32_t len = le32 (c + at);
		at += 4;
		if (len > n - at)
		{
			return;
		}
		comment (c + at, len, "TITLE", o->title);
		comment (c + at, len, "ARTIST", o->artist);
		at += len;
	}
}

/* the header packets: the stream's first pages, packet by packet */
static bool headers (pgpu_ogg_t *o, uint64_t first)
{
	static uint8_t id[64], text[MAX_COMMENT], setup[PGPU_OGG_MAX_CONFIG];
	uint8_t *const buffers[3] = {id, text, setup};
	const uint32_t capacity[3] = {sizeof id, sizeof text, sizeof setup};
	uint32_t sizes[3] = {0, 0, 0};
	unsigned packet = 0;
	uint64_t at = first;
	page_t p;
	while (packet < 3)
	{
		if (!page_at (o, at, &p))
		{
			return false;
		}
		if (p.serial == o->serial)
		{
			uint64_t data = at + HEADER + p.segments;
			for (unsigned s = 0; s < p.segments && packet < 3; s++)
			{
				uint32_t l = p.lacing[s];
				uint32_t keep = capacity[packet] - sizes[packet] < l ? capacity[packet] - sizes[packet] : l;
				if (packet != 1 && keep < l)
				{
					return false;		/* (the identification or setup doesn't fit) */
				}
				if (keep && !read_file (o, data, buffers[packet] + sizes[packet], keep))
				{
					return false;
				}
				sizes[packet] += keep;
				data += l;
				if (l < 255)
				{
					packet++;
				}
			}
		}
		at += p.size;
	}
	o->start = at;					/* (the setup ends a page) */

	/* the identification: "\1vorbis", version, channels, rate, bitrates */
	if (sizes[0] < 30 || id[0] != 1 || memcmp (id + 1, "vorbis", 6) || le32 (id + 7) != 0 || !id[11] || !le32 (id + 12))
	{
		return false;
	}
	o->channels = id[11];
	o->sample_rate = le32 (id + 12);
	o->bitrate = le32 (id + 20) < 0x80000000u ? le32 (id + 20) : 0;
	if (text[0] == 3 && !memcmp (text + 1, "vorbis", 6))
	{
		comments (o, text, sizes[1]);
	}
	if (setup[0] != 5 || memcmp (setup + 1, "vorbis", 6))
	{
		return false;
	}

	/* the config: the identification alone on the first page, then an empty
	   comment (no vendor, no comments, the framing bit) and the setup, numbered
	   so that the first page of sound follows them (a comment of more pages
	   than one would leave a gap: a packet lost to the decoder) */
	static const uint8_t empty[16] = {3, 'v', 'o', 'r', 'b', 'i', 's', 0, 0, 0, 0, 0, 0, 0, 0, 1};
	const uint8_t *first_page[1] = {id};
	const uint8_t *second_page[2] = {empty, setup};
	const uint32_t first_sizes[1] = {sizes[0]}, second_sizes[2] = {sizeof empty, sizes[2]};
	o->config_size = 0;
	uint32_t last = p.sequence ? p.sequence : 1;	/* (the setup's page) */
	return    put_page (o, 2, last - 1, first_page, first_sizes, 1)
	       && put_page (o, 0, last, second_page, second_sizes, 2);
}

/* the last page of the stream with a granule position: the duration */
static void duration (pgpu_ogg_t *o)
{
	uint8_t w[4096];
	uint64_t end = o->size;
	for (unsigned round = 0; round < 16 && end > o->start; round++)	/* (the last 64 KB at most) */
	{
		uint64_t from = end > o->start + sizeof w ? end - sizeof w : o->start;
		uint32_t n = (uint32_t) (end - from);
		if (!read_file (o, from, w, n))
		{
			return;
		}
		for (uint32_t i = n >= 4 ? n - 4 : 0; i + 1 > 0; i--)
		{
			page_t p;
			if (!memcmp (w + i, "OggS", 4) && page_at (o, from + i, &p) && p.serial == o->serial && p.granule >= 0)
			{
				o->duration_us = (int64_t) ((uint64_t) p.granule * 1000000 / o->sample_rate);
				return;
			}
			if (i == 0)
			{
				break;
			}
		}
		end = from + 3;
	}
}

static bool open_file (pgpu_ogg_t *o)
{
	uint64_t first;
	page_t p;
	if (!find_page (o, 0, &first, &p) || first > 65536 || !(p.flags & 2))
	{
		return false;
	}
	o->serial = p.serial;
	if (!headers (o, first))
	{
		return false;
	}
	duration (o);
	o->error = false;
	pgpu_ogg_rewind (o);
	return true;
}

bool pgpu_ogg_open (pgpu_ogg_t *o, pgpu_read_t read, void *ctx, uint64_t size)
{
	memset (o, 0, sizeof *o);
	o->read = read;
	o->ctx = ctx;
	o->size = size;
	return open_file (o);
}

static bool memory_read (void *ctx, uint64_t offset, void *buffer, uint32_t bytes)
{
	memcpy (buffer, ((pgpu_ogg_t *) ctx)->memory + offset, bytes);	/* (checked by the caller) */
	return true;
}

bool pgpu_ogg_open_memory (pgpu_ogg_t *o, const void *file, size_t size)
{
	memset (o, 0, sizeof *o);
	o->read = memory_read;
	o->ctx = o;
	o->size = size;
	o->memory = (const uint8_t *) file;
	return open_file (o);
}

void pgpu_ogg_rewind (pgpu_ogg_t *o)
{
	o->offset = o->start;
	o->granule = 0;
	o->next = 0;
	o->skipped = 0;
}

bool pgpu_ogg_next (pgpu_ogg_t *o, pgpu_ogg_sample_t *sample)
{
	page_t p;
	while (!o->error)
	{
		uint64_t at = o->offset;
		if (!page_at (o, at, &p))			/* not a page here: the next one */
		{
			if (o->error || !find_page (o, at + 1, &at, &p))
			{
				return false;
			}
			o->skipped += (uint32_t) (at - o->offset);
		}
		o->offset = at + p.size;
		if (p.serial != o->serial)		/* another stream's */
		{
			continue;
		}
		sample->offset = at;
		sample->size = p.size;
		sample->pts_us = (int64_t) (o->granule * 1000000 / o->sample_rate);
		if (p.granule >= 0)
		{
			o->granule = (uint64_t) p.granule;
		}
		o->next++;
		return true;
	}
	return false;
}

bool pgpu_ogg_seek (pgpu_ogg_t *o, int64_t target_us, pgpu_ogg_sample_t *sample)
{
	pgpu_ogg_sample_t s, before;
	bool first = true;
	pgpu_ogg_rewind (o);
	while (true)
	{
		uint64_t offset = o->offset, granule = o->granule;	/* (where the page is read from) */
		uint32_t next = o->next, skipped = o->skipped;
		if (!pgpu_ogg_next (o, &s))
		{
			if (o->error)
			{
				return false;
			}
			pgpu_ogg_rewind (o);
			return pgpu_ogg_next (o, sample);
		}
		if (s.pts_us >= target_us)
		{
			if (first)
			{
				*sample = s;
				return true;
			}
			*sample = before;		/* ... and this page next */
			o->offset = offset;
			o->granule = granule;
			o->next = next;
			o->skipped = skipped;
			return true;
		}
		before = s;
		first = false;
	}
}
