/*
 * pgpu_mp4.c - see pgpu_mp4.h. ISO/IEC 14496-12 (boxes, sample tables) and
 * 14496-15 (avcC). Everything is read through the callback and checked
 * against the sizes the file gives; a failed read or a broken table ends the
 * samples (mp4->error).
 */
#include "pgpu_mp4.h"
#include <string.h>

static uint32_t be16 (const uint8_t *p)	{ return (uint32_t) p[0] << 8 | p[1]; }
static uint32_t be32 (const uint8_t *p)	{ return (uint32_t) p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static uint64_t be64 (const uint8_t *p)	{ return (uint64_t) be32 (p) << 32 | be32 (p + 4); }

#define TYPE(a, b, c, d)	((uint32_t) (a) << 24 | (b) << 16 | (c) << 8 | (d))

bool pgpu_mp4_read (pgpu_mp4_t *m, uint64_t offset, void *buffer, uint32_t bytes)
{
	if (offset > m->size || bytes > m->size - offset || !m->read (m->ctx, offset, buffer, bytes))
	{
		m->error = true;
		return false;
	}
	return true;
}

/* the box of this type in [start, end): its payload's offset and size */
static bool find (pgpu_mp4_t *m, uint64_t start, uint64_t end, uint32_t type, uint64_t *payload,
		  uint64_t *size)
{
	uint64_t p = start;
	while (end - p >= 8)
	{
		uint8_t h[16];
		if (!pgpu_mp4_read (m, p, h, 8))
		{
			return false;
		}
		uint64_t box = be32 (h);
		unsigned header = 8;
		if (box == 1)				/* 64-bit size */
		{
			if (end - p < 16 || !pgpu_mp4_read (m, p + 8, h + 8, 8))
			{
				return false;
			}
			box = be64 (h + 8);
			header = 16;
		}
		else if (box == 0)			/* to the end */
		{
			box = end - p;
		}
		if (box < header || box > end - p)
		{
			return false;
		}
		if (be32 (h + 4) == type)
		{
			*payload = p + header;
			*size = box - header;
			return true;
		}
		p += box;
	}
	return false;
}

/* a full box's table: version and flags, (skip bytes,) the count, the entries */
static bool table (pgpu_mp4_t *m, pgpu_mp4_table_t *t, uint64_t start, uint64_t end, uint32_t type,
		   uint32_t entry, unsigned skip, uint8_t *version)
{
	uint64_t p, size;
	memset (t, 0, sizeof *t);
	if (!find (m, start, end, type, &p, &size))
	{
		return false;
	}
	uint8_t h[16];
	if (size < 8 + skip || !pgpu_mp4_read (m, p, h, 8 + skip))
	{
		return false;
	}
	uint32_t count = be32 (h + 4 + skip);
	if ((uint64_t) count * entry > size - 8 - skip)
	{
		return false;
	}
	if (version)
	{
		*version = h[0];
	}
	t->offset = p + 8 + skip;
	t->count = count;
	t->entry = entry;
	return true;
}

/* word k of entry i of a table, through its window (the tables are read in
   order, so a window is read once) */
static uint32_t entry (pgpu_mp4_t *m, pgpu_mp4_table_t *t, uint32_t i, unsigned k)
{
	if (i >= t->count)
	{
		m->error = true;
		return 0;
	}
	uint64_t at = t->offset + (uint64_t) i * t->entry + k * 4;
	if (at < t->window_offset || at + 4 > t->window_offset + t->window_bytes)
	{
		uint64_t end = t->offset + (uint64_t) t->count * t->entry;
		uint32_t n = end - at < PGPU_MP4_WINDOW ? (uint32_t) (end - at) : PGPU_MP4_WINDOW;
		if (!pgpu_mp4_read (m, at, t->window, n))
		{
			t->window_bytes = 0;
			return 0;
		}
		t->window_offset = at;
		t->window_bytes = n;
	}
	return be32 (t->window + (at - t->window_offset));
}

static bool video_track (pgpu_mp4_t *m, uint64_t trak, uint64_t trak_end)
{
	uint64_t p, size;

	/* the edit list: the media time its first (non-empty) edit starts at */
	m->shift = 0;
	if (find (m, trak, trak_end, TYPE ('e', 'd', 't', 's'), &p, &size))
	{
		pgpu_mp4_table_t elst;
		uint8_t version = 0;
		if (table (m, &elst, p, p + size, TYPE ('e', 'l', 's', 't'), 12, 0, &version))
		{
			elst.entry = version == 1 ? 20 : 12;	/* (the count's check was for 12) */
			for (uint32_t i = 0; i < elst.count && i < 8; i++)
			{
				uint8_t e[20];
				if (!pgpu_mp4_read (m, elst.offset + i * elst.entry, e, elst.entry))
				{
					return false;
				}
				int64_t t = version == 1 ? (int64_t) be64 (e + 8) : (int64_t) (int32_t) be32 (e + 4);
				if (t >= 0)
				{
					m->shift = t;
					break;
				}
			}
		}
	}

	uint64_t mdia, mdia_size;
	if (!find (m, trak, trak_end, TYPE ('m', 'd', 'i', 'a'), &mdia, &mdia_size))
	{
		return false;
	}
	uint64_t mdia_end = mdia + mdia_size;

	uint8_t h[128];
	if (   !find (m, mdia, mdia_end, TYPE ('h', 'd', 'l', 'r'), &p, &size) || size < 12
	    || !pgpu_mp4_read (m, p, h, 12) || be32 (h + 8) != TYPE ('v', 'i', 'd', 'e'))
	{
		return false;
	}

	if (!find (m, mdia, mdia_end, TYPE ('m', 'd', 'h', 'd'), &p, &size) || size < 24)
	{
		return false;
	}
	uint64_t duration;
	if (!pgpu_mp4_read (m, p, h, size < 36 ? 24 : 36))
	{
		return false;
	}
	if (h[0] == 1)
	{
		if (size < 36)
		{
			return false;
		}
		m->timescale = be32 (h + 20);
		duration = be64 (h + 24);
	}
	else
	{
		m->timescale = be32 (h + 12);
		duration = be32 (h + 16);
	}
	if (!m->timescale)
	{
		return false;
	}
	m->duration_us = (int64_t) (duration * 1000000 / m->timescale);

	uint64_t minf, minf_size, stbl, stbl_size;
	if (   !find (m, mdia, mdia_end, TYPE ('m', 'i', 'n', 'f'), &minf, &minf_size)
	    || !find (m, minf, minf + minf_size, TYPE ('s', 't', 'b', 'l'), &stbl, &stbl_size))
	{
		return false;
	}
	uint64_t stbl_end = stbl + stbl_size;

	/* stsd: the first entry, avc1 (or avc3), with an avcC */
	if (   !find (m, stbl, stbl_end, TYPE ('s', 't', 's', 'd'), &p, &size) || size < 8 + 8 + 78
	    || !pgpu_mp4_read (m, p, h, 8 + 8 + 78))
	{
		return false;
	}
	uint32_t entry_size = be32 (h + 8), type = be32 (h + 12);
	if (   (type != TYPE ('a', 'v', 'c', '1') && type != TYPE ('a', 'v', 'c', '3'))
	    || entry_size < 8 + 78 || entry_size > size - 8)
	{
		return false;
	}
	m->width = be16 (h + 16 + 24);
	m->height = be16 (h + 16 + 26);
	uint64_t entry_start = p + 8;
	uint64_t avcc;
	if (   !find (m, entry_start + 8 + 78, entry_start + entry_size, TYPE ('a', 'v', 'c', 'C'), &avcc, &size)
	    || size < 7 || size > PGPU_MP4_MAX_AVCC || !pgpu_mp4_read (m, avcc, m->avcc, (uint32_t) size))
	{
		return false;
	}
	m->avcc_size = (uint32_t) size;

	/* the sample tables */
	uint8_t version = 0;
	if (   !table (m, &m->stts, stbl, stbl_end, TYPE ('s', 't', 't', 's'), 8, 0, NULL)
	    || !table (m, &m->stsc, stbl, stbl_end, TYPE ('s', 't', 's', 'c'), 12, 0, NULL))
	{
		return false;
	}
	if (table (m, &m->ctts, stbl, stbl_end, TYPE ('c', 't', 't', 's'), 8, 0, &version))
	{
		m->ctts_signed = version != 0;
	}
	table (m, &m->stss, stbl, stbl_end, TYPE ('s', 't', 's', 's'), 4, 0, NULL);
	m->co64 = false;
	if (!table (m, &m->stco, stbl, stbl_end, TYPE ('s', 't', 'c', 'o'), 4, 0, NULL))
	{
		if (!table (m, &m->stco, stbl, stbl_end, TYPE ('c', 'o', '6', '4'), 8, 0, NULL))
		{
			return false;
		}
		m->co64 = true;
	}
	/* stsz: version and flags, the size of all samples (0: a table), the count */
	if (   !find (m, stbl, stbl_end, TYPE ('s', 't', 's', 'z'), &p, &size) || size < 12
	    || !pgpu_mp4_read (m, p, h, 12))
	{
		return false;
	}
	m->stsz_fixed = be32 (h + 4);
	m->samples = be32 (h + 8);
	memset (&m->stsz, 0, sizeof m->stsz);
	if (!m->stsz_fixed)
	{
		if (!table (m, &m->stsz, stbl, stbl_end, TYPE ('s', 't', 's', 'z'), 4, 4, NULL))
		{
			return false;
		}
	}

	return    m->stts.count && m->stsc.count && m->stco.count && m->samples
	       && !m->error;
}

static bool open_file (pgpu_mp4_t *m)
{
	uint64_t moov, moov_size;
	if (!find (m, 0, m->size, TYPE ('m', 'o', 'o', 'v'), &moov, &moov_size))
	{
		return false;
	}
	uint64_t p = moov, end = moov + moov_size, trak, trak_size;
	while (find (m, p, end, TYPE ('t', 'r', 'a', 'k'), &trak, &trak_size))
	{
		m->error = false;
		if (video_track (m, trak, trak + trak_size))
		{
			pgpu_mp4_rewind (m);
			return !m->error;
		}
		p = trak + trak_size;
	}
	return false;
}

bool pgpu_mp4_open (pgpu_mp4_t *m, pgpu_read_t read, void *ctx, uint64_t size)
{
	memset (m, 0, sizeof *m);
	m->read = read;
	m->ctx = ctx;
	m->size = size;
	return open_file (m);
}

static bool memory_read (void *ctx, uint64_t offset, void *buffer, uint32_t bytes)
{
	memcpy (buffer, ((pgpu_mp4_t *) ctx)->memory + offset, bytes);	/* (checked by the caller) */
	return true;
}

bool pgpu_mp4_open_memory (pgpu_mp4_t *m, const void *file, size_t size)
{
	memset (m, 0, sizeof *m);
	m->read = memory_read;
	m->ctx = m;
	m->size = size;
	m->memory = (const uint8_t *) file;
	return open_file (m);
}

static uint64_t chunk_offset (pgpu_mp4_t *m, uint32_t chunk)
{
	if (m->co64)
	{
		uint64_t hi = entry (m, &m->stco, chunk, 0);
		return hi << 32 | entry (m, &m->stco, chunk, 1);
	}
	return entry (m, &m->stco, chunk, 0);
}

void pgpu_mp4_rewind (pgpu_mp4_t *m)
{
	m->error = false;
	m->next = 0;
	m->dts = 0;
	m->stts_i = 0;
	m->stts_left = entry (m, &m->stts, 0, 0);
	m->ctts_i = 0;
	m->ctts_left = m->ctts.count ? entry (m, &m->ctts, 0, 0) : 0;
	m->stss_i = 0;
	m->stsc_i = 0;
	m->chunk = 0;
	m->chunk_left = entry (m, &m->stsc, 0, 1);
	m->offset = chunk_offset (m, 0);
}

bool pgpu_mp4_next (pgpu_mp4_t *m, pgpu_mp4_sample_t *s)
{
	if (m->error || m->next >= m->samples)
	{
		return false;
	}

	/* its chunk (stsc: runs of chunks with the same number of samples; its
	   first chunks are 1-based) */
	while (!m->chunk_left)
	{
		m->chunk++;
		if (m->chunk >= m->stco.count)
		{
			m->error = true;
			return false;
		}
		if (m->stsc_i + 1 < m->stsc.count && m->chunk + 1 >= entry (m, &m->stsc, m->stsc_i + 1, 0))
		{
			m->stsc_i++;
		}
		m->chunk_left = entry (m, &m->stsc, m->stsc_i, 1);
		m->offset = chunk_offset (m, m->chunk);
	}

	uint32_t size = m->stsz_fixed ? m->stsz_fixed : entry (m, &m->stsz, m->next, 0);
	if (m->error || m->offset > m->size || size > m->size - m->offset)
	{
		m->error = true;
		return false;
	}
	s->offset = m->offset;
	s->size = size;
	m->offset += size;
	m->chunk_left--;

	/* its times: stts gives the decode time, ctts the offset to the presentation */
	int64_t cts = 0;
	if (m->ctts.count)
	{
		while (!m->ctts_left && m->ctts_i + 1 < m->ctts.count)
		{
			m->ctts_left = entry (m, &m->ctts, ++m->ctts_i, 0);
		}
		uint32_t v = entry (m, &m->ctts, m->ctts_i, 1);
		cts = m->ctts_signed ? (int64_t) (int32_t) v : (int64_t) v;
		if (m->ctts_left)
		{
			m->ctts_left--;
		}
	}
	s->dts_us = ((int64_t) m->dts - m->shift) * 1000000 / m->timescale;
	s->pts_us = ((int64_t) m->dts + cts - m->shift) * 1000000 / m->timescale;
	while (!m->stts_left && m->stts_i + 1 < m->stts.count)
	{
		m->stts_left = entry (m, &m->stts, ++m->stts_i, 0);
	}
	m->dts += entry (m, &m->stts, m->stts_i, 1);
	if (m->stts_left)
	{
		m->stts_left--;
	}

	/* a sync sample (no stss: all are) */
	s->keyframe = !m->stss.count;
	if (m->stss_i < m->stss.count && entry (m, &m->stss, m->stss_i, 0) == m->next + 1)
	{
		s->keyframe = true;
		m->stss_i++;
	}

	m->next++;
	return !m->error;
}
