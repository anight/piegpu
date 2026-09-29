/*
 * pgpu.c - command encoding and reply parsing of the piegpu link, on top of
 * a transport (pgpu_link.h). Board independent: a new host board only needs
 * its transport.
 *
 * Replies are delivered by the transport's parser, which may run in an
 * interrupt or on another core: the queues are single-producer,
 * single-consumer rings whose indices are published with release stores and
 * read with acquire loads.
 */
#include "pgpu.h"
#include <string.h>
#include "pgpu_link.h"

/* aligned: data read straight into a packet can be (pgpu_video_sample_read) */
static uint32_t staging[2][PGPU_STAGING_WORDS] __attribute__ ((aligned (PGPU_READ_ALIGN)));
static uint32_t current;		/* staging buffer being filled */
static uint32_t fill;			/* words in it */
static uint32_t open_packet;		/* index of the header of the open packet */
static uint32_t packets_sent;		/* since pgpu_init () */

static uint32_t crc_table[256];
pgpu_stats_t pgpu_link_stats;

uint32_t pgpu_packets_sent (void)
{
	return packets_sent;
}

#define LOAD(x)		__atomic_load_n (&(x), __ATOMIC_ACQUIRE)
#define STORE(x, v)	__atomic_store_n (&(x), (v), __ATOMIC_RELEASE)

#define REPLY_QUEUE	16

/* PIXELS replies go straight here (pgpu_read_pixels) */
static uint32_t *pixel_sink;
static uint32_t pixel_sink_words;
static uint32_t pixel_words_received;
static pgpu_reply_t reply_queue[REPLY_QUEUE];
static uint32_t reply_head, reply_tail;	/* tail written by the parser */

/* ERROR replies go to their own queue (pgpu_poll_error) */
#define ERROR_QUEUE	16
static uint32_t error_queue[ERROR_QUEUE][3];
static uint32_t error_head, error_tail;

/* VIDEO_STATUS replies: the latest one a stream, under a sequence count (as
   DISPLAY); the bytes sent, for pgpu_video_room () */
static uint32_t video_words[PGPU_VIDEO_STREAMS + 1][PGPU_VIDEO_STATUS_WORDS];
static uint32_t video_seq[PGPU_VIDEO_STREAMS + 1];
static uint32_t video_bytes_sent[PGPU_VIDEO_STREAMS + 1];
static uint32_t video_samples_sent[PGPU_VIDEO_STREAMS + 1];

uint32_t pgpu_video_get_status (uint32_t stream, pgpu_video_status_t *status)
{
	pgpu_link_poll ();			/* replies that have come */
	memset (status, 0, sizeof *status);
	status->shown_pts = PGPU_VIDEO_TIME_NONE;
	if (stream < 1 || stream > PGPU_VIDEO_STREAMS)
	{
		return 0;
	}

	uint32_t w[PGPU_VIDEO_STATUS_WORDS], seq;
	do
	{
		while ((seq = LOAD (video_seq[stream])) & 1)
		{
		}
		for (int i = 0; i < PGPU_VIDEO_STATUS_WORDS; i++)
		{
			w[i] = __atomic_load_n (&video_words[stream][i], __ATOMIC_RELAXED);
		}
		__atomic_thread_fence (__ATOMIC_ACQUIRE);
	}
	while (__atomic_load_n (&video_seq[stream], __ATOMIC_RELAXED) != seq);
	if (!seq)
	{
		return 0;
	}

	status->flags = w[1];
	status->bytes_done = w[2];
	status->ring_bytes = w[3];
	status->decoded = w[4];
	status->shown = w[5];
	status->dropped = w[6];
	status->shown_pts = (int64_t) ((uint64_t) w[8] << 32 | w[7]);
	status->waiting = w[9];
	status->samples_done = w[10];
	status->max_samples = w[11];
	return seq / 2;
}

uint32_t pgpu_video_room (uint32_t stream)
{
	pgpu_video_status_t st;
	if (!pgpu_video_get_status (stream, &st) || !(st.flags & PGPU_VIDEO_OPEN_FLAG))
	{
		return 0;
	}
	if (video_samples_sent[stream] - st.samples_done >= st.max_samples)
	{
		return 0;
	}
	uint32_t in_flight = video_bytes_sent[stream] - st.bytes_done;
	return in_flight < st.ring_bytes ? st.ring_bytes - in_flight : 0;
}

/* STATUS replies: a copy of the latest one, under a sequence count (odd while
   the parser writes it); the reply is queued as well (pgpu_get_status) */
#define STATUS_WORDS	(sizeof (pgpu_status_t) / 4)
static uint32_t status_words[STATUS_WORDS];
static uint32_t status_seq;

uint32_t pgpu_last_status (pgpu_status_t *status)
{
	uint32_t w[STATUS_WORDS], seq;
	do
	{
		while ((seq = LOAD (status_seq)) & 1)
		{
		}
		for (unsigned i = 0; i < STATUS_WORDS; i++)
		{
			w[i] = __atomic_load_n (&status_words[i], __ATOMIC_RELAXED);
		}
		__atomic_thread_fence (__ATOMIC_ACQUIRE);
	}
	while (__atomic_load_n (&status_seq, __ATOMIC_RELAXED) != seq);

	memcpy (status, w, sizeof *status);
	return seq / 2;
}

/* DISPLAY replies: the latest one, under a sequence count (odd while the
   parser writes it) */
static uint32_t display_words[PGPU_DISPLAY_WORDS];
static uint32_t display_seq;

uint32_t pgpu_get_display (pgpu_display_t *display)
{
	uint32_t w[PGPU_DISPLAY_WORDS], seq;
	do
	{
		while ((seq = LOAD (display_seq)) & 1)
		{
		}
		for (int i = 0; i < PGPU_DISPLAY_WORDS; i++)
		{
			w[i] = __atomic_load_n (&display_words[i], __ATOMIC_RELAXED);
		}
		__atomic_thread_fence (__ATOMIC_ACQUIRE);
	}
	while (__atomic_load_n (&display_seq, __ATOMIC_RELAXED) != seq);

	memset (display, 0, sizeof *display);
	display->output = PGPU_DISPLAY_OUTPUT (w[0]);
	display->hdmi_connected = (w[0] & PGPU_DISPLAY_HDMI_CONNECTED) != 0;
	display->panel_present = (w[0] & PGPU_DISPLAY_PANEL_PRESENT) != 0;
	display->edid = (w[0] & PGPU_DISPLAY_EDID) != 0;
	display->width = w[1] & 0xFFFF;
	display->height = w[1] >> 16;
	display->monitor_width = w[2] & 0xFFFF;
	display->monitor_height = w[2] >> 16;
	display->monitor_refresh_mhz = w[3];
	display->signal_width = w[4] & 0xFFFF;
	display->signal_height = w[4] >> 16;
	memcpy (display->monitor_name, &w[5], 13);
	return seq / 2;
}

bool pgpu_poll_reply (pgpu_reply_t *reply)
{
	uint32_t head = reply_head;
	if (head == LOAD (reply_tail))
	{
		return false;
	}
	*reply = reply_queue[head];
	STORE (reply_head, (head + 1) % REPLY_QUEUE);
	return true;
}

bool pgpu_poll_error (uint32_t error[3])
{
	uint32_t head = error_head;
	if (head == LOAD (error_tail))
	{
		return false;
	}
	memcpy (error, error_queue[head], sizeof error_queue[head]);
	STORE (error_head, (head + 1) % ERROR_QUEUE);
	return true;
}

uint64_t pgpu_time_us (void)
{
	return pgpu_link_time_us ();
}

pgpu_stats_t pgpu_get_stats (void)
{
	pgpu_link_lock ();
	pgpu_stats_t s = pgpu_link_stats;
	memset (&pgpu_link_stats, 0, sizeof pgpu_link_stats);
	memcpy (pgpu_link_stats.last_error, s.last_error, sizeof pgpu_link_stats.last_error);
	pgpu_link_unlock ();
	s.frames = pgpu_frame_count ();
	return s;
}

bool pgpu_wait_reply (uint8_t opcode, pgpu_reply_t *reply, uint32_t timeout_ms)
{
	uint64_t end = pgpu_link_time_us () + timeout_ms * 1000ull;
	while (pgpu_link_time_us () < end)
	{
		pgpu_link_poll ();
		if (pgpu_poll_reply (reply) && reply->opcode == opcode)
		{
			return true;
		}
	}
	return false;
}

bool pgpu_get_info (pgpu_info_t *info, uint32_t timeout_ms)
{
	pgpu_reply_t r;
	pgpu_request_info ();
	pgpu_flush ();
	if (!pgpu_wait_reply (PGPU_REPLY_INFO, &r, timeout_ms) || r.length < 7)
	{
		return false;
	}
	info->version = r.payload[0];
	info->width = r.payload[1] & 0xFFFF;
	info->height = r.payload[1] >> 16;
	info->max_texture_size = r.payload[2];
	info->max_buffers = r.payload[3];
	info->max_textures = r.payload[4];
	info->max_lights = r.payload[5];
	info->ring_bytes = r.payload[6];
	return true;
}

bool pgpu_get_status (pgpu_status_t *status, uint32_t timeout_ms)
{
	pgpu_reply_t r;
	pgpu_request_status ();
	pgpu_flush ();
	return pgpu_wait_reply (PGPU_REPLY_STATUS, &r, timeout_ms) && pgpu_status_from_reply (&r, status);
}

bool pgpu_status_from_reply (const pgpu_reply_t *reply, pgpu_status_t *status)
{
	if (reply->opcode != PGPU_REPLY_STATUS || reply->length < 5)
	{
		return false;
	}
	uint32_t n = reply->length * 4 < sizeof *status ? reply->length * 4 : sizeof *status;
	memset (status, 0, sizeof *status);
	memcpy (status, reply->payload, n);
	return true;
}

int32_t pgpu_ping_wait (uint32_t cookie, uint32_t timeout_ms)
{
	pgpu_reply_t r;
	pgpu_ping (cookie);
	pgpu_flush ();
	uint64_t start = pgpu_link_time_us ();
	uint64_t end = start + timeout_ms * 1000ull;
	while (pgpu_link_time_us () < end)
	{
		if (   pgpu_wait_reply (PGPU_REPLY_PONG, &r, 1)
		    && r.length == 1 && r.payload[0] == cookie)
		{
			return (int32_t) (pgpu_link_time_us () - start);
		}
	}
	return -1;
}

void pgpu_init (void)
{
	for (uint32_t i = 0; i < 256; i++)
	{
		uint32_t c = i;
		for (int k = 0; k < 8; k++)
		{
			c = c & 1 ? PGPU_CRC_POLY_REFLECTED ^ (c >> 1) : c >> 1;
		}
		crc_table[i] = c;
	}
	current = 0;
	fill = 0;
	pgpu_link_init ();
}

/* a reply packet from the transport: PIXELS into the sink, ERROR into the
   error queue, others into the reply queue */
void pgpu_deliver_reply (uint8_t opcode, const uint32_t *payload, uint32_t length)
{
	pgpu_link_stats.replies++;

	if (opcode == PGPU_REPLY_PIXELS && length >= 1)
	{
		uint32_t *sink = LOAD (pixel_sink);	/* its size was stored before it */
		uint32_t offset = payload[0], n = length - 1;
		if (sink && offset < pixel_sink_words)
		{
			if (n > pixel_sink_words - offset)
				n = pixel_sink_words - offset;
			memcpy (sink + offset, &payload[1], n * 4);
			STORE (pixel_words_received, pixel_words_received + n);
		}
		return;
	}
	if (opcode == PGPU_REPLY_ERROR && length >= 3)
	{
		pgpu_link_stats.rpi_errors++;
		memcpy (pgpu_link_stats.last_error, payload, sizeof pgpu_link_stats.last_error);

		uint32_t tail = error_tail;
		if ((tail + 1) % ERROR_QUEUE == LOAD (error_head))
		{
			pgpu_link_stats.errors_lost++;
		}
		else
		{
			memcpy (error_queue[tail], payload, sizeof error_queue[tail]);
			STORE (error_tail, (tail + 1) % ERROR_QUEUE);
		}
		return;
	}
	if (   opcode == PGPU_REPLY_VIDEO_STATUS && length >= PGPU_VIDEO_STATUS_WORDS
	    && payload[0] >= 1 && payload[0] <= PGPU_VIDEO_STREAMS)
	{
		uint32_t stream = payload[0], seq = video_seq[stream];
		STORE (video_seq[stream], seq + 1);
		__atomic_thread_fence (__ATOMIC_RELEASE);
		for (int i = 0; i < PGPU_VIDEO_STATUS_WORDS; i++)
		{
			__atomic_store_n (&video_words[stream][i], payload[i], __ATOMIC_RELAXED);
		}
		STORE (video_seq[stream], seq + 2);
		return;
	}
	if (opcode == PGPU_REPLY_STATUS && length >= 5)	/* (and queued, below) */
	{
		uint32_t seq = status_seq;
		STORE (status_seq, seq + 1);
		__atomic_thread_fence (__ATOMIC_RELEASE);
		for (unsigned i = 0; i < STATUS_WORDS; i++)
		{
			__atomic_store_n (&status_words[i], i < length ? payload[i] : 0, __ATOMIC_RELAXED);
		}
		STORE (status_seq, seq + 2);
	}
	if (opcode == PGPU_REPLY_DISPLAY && length >= PGPU_DISPLAY_WORDS)
	{
		uint32_t seq = display_seq;
		STORE (display_seq, seq + 1);
		__atomic_thread_fence (__ATOMIC_RELEASE);
		for (int i = 0; i < PGPU_DISPLAY_WORDS; i++)
		{
			__atomic_store_n (&display_words[i], payload[i], __ATOMIC_RELAXED);
		}
		STORE (display_seq, seq + 2);
		return;
	}
	if (length > PGPU_MAX_REPLY_PAYLOAD)
	{
		return;
	}

	uint32_t tail = reply_tail;
	if ((tail + 1) % REPLY_QUEUE == LOAD (reply_head))
	{
		pgpu_link_stats.replies_lost++;
		return;
	}
	pgpu_reply_t *r = &reply_queue[tail];
	r->opcode = opcode;
	r->length = length;
	memcpy (r->payload, payload, length * 4);
	STORE (reply_tail, (tail + 1) % REPLY_QUEUE);
}

/* ---- the reply stream parser (pgpu_link.h, docs/protocol.md 9.1) ----------- */

static inline uint32_t rx_word (const pgpu_rx_t *rx, uint64_t word)
{
	return rx->ring[(uint32_t) word & (rx->words - 1)];
}

static inline uint32_t rx_bits (const pgpu_rx_t *rx, uint64_t bit)	/* 32 stream bits from bit on */
{
	uint32_t shift = bit & 31;
	uint32_t w = rx_word (rx, bit >> 5);
	if (shift == 0)
	{
		return w;
	}
	return w << shift | rx_word (rx, (bit >> 5) + 1) >> (32 - shift);
}

void pgpu_rx_parse (pgpu_rx_t *rx)
{
	uint64_t avail = rx->written * 32;
	if (rx->written - (rx->bit >> 5) > rx->words - 256)
	{
		pgpu_link_stats.reply_overruns++;
		rx->bit = (rx->written - rx->words / 2) * 32;
	}

	while (rx->bit + 64 <= avail)
	{
		uint32_t v = rx_word (rx, rx->bit >> 5) << (rx->bit & 31);
		if (v == 0)
		{
			rx->bit = (rx->bit | 31) + 1;		/* rest of this word is idle */
			continue;
		}

		uint64_t one = rx->bit + __builtin_clz (v);
		if (one == 0)
		{
			rx->bit = 1;
			continue;
		}
		uint64_t start = one - 1;
		uint32_t header = rx_bits (rx, start);
		uint32_t length = PGPU_HEADER_LEN (header);
		if (   PGPU_HEADER_SYNC (header) != PGPU_SYNC_REPLY
		    || length > PGPU_MAX_REPLY_PAYLOAD)
		{
			rx->bit = one + 1;			/* not a header: go on searching */
			continue;
		}
		if (start + (length + 2) * 32 > avail)
		{
			break;					/* wait for the rest */
		}

		uint32_t words[PGPU_MAX_REPLY_PAYLOAD + 2];
		for (uint32_t i = 0; i < length + 2; i++)
		{
			words[i] = rx_bits (rx, start + i * 32);
		}
		if (pgpu_crc32 (words, length + 1) != words[length + 1])
		{
			pgpu_link_stats.reply_crc_errors++;
			rx->bit = one + 1;
			continue;
		}
		rx->bit = start + (length + 2) * 32;
		pgpu_deliver_reply (PGPU_HEADER_OP (header), &words[1], length);
	}
}

uint32_t pgpu_crc32 (const uint32_t *p, uint32_t n)
{
	uint32_t crc = PGPU_CRC_INIT;
	for (uint32_t i = 0; i < n; i++)
	{
		uint32_t w = p[i];
		for (int b = 0; b < 4; b++, w >>= 8)	/* little endian bytes */
		{
			crc = crc_table[(crc ^ w) & 0xFF] ^ (crc >> 8);
		}
	}
	return crc ^ 0xFFFFFFFF;
}

void pgpu_flush (void)
{
	if (fill == 0)
	{
		return;
	}

	/* the transport may read the buffer after returning (DMA): the next
	   batch goes into the other one */
	pgpu_link_send (staging[current], fill);

	pgpu_link_stats.words += fill;
	pgpu_link_stats.batches++;

	current ^= 1;
	fill = 0;
}

uint32_t *pgpu_begin (uint8_t opcode, uint32_t payload_words)
{
	if (payload_words + 2 > PGPU_STAGING_WORDS)
	{
		return NULL;			/* split large uploads */
	}

	if (fill + payload_words + 2 > PGPU_STAGING_WORDS)
	{
		pgpu_flush ();
	}

	/* pgpu_flush () waits for the previous batch before sending the next, so
	   the buffer being filled is never the one being sent */

	open_packet = fill;
	staging[current][fill] = PGPU_HEADER (PGPU_SYNC_COMMAND, opcode, payload_words);
	fill += 1 + payload_words;

	return &staging[current][open_packet + 1];
}

void pgpu_end (void)
{
	uint32_t length = PGPU_HEADER_LEN (staging[current][open_packet]);
	staging[current][fill++] = pgpu_crc32 (&staging[current][open_packet], 1 + length);
	packets_sent++;
	pgpu_link_stats.packets++;
}

/* ---- commands ------------------------------------------------------------ */

static uint32_t f2u (float f)
{
	uint32_t u;
	memcpy (&u, &f, sizeof u);
	return u;
}

static void cmd1 (uint8_t op, uint32_t a)
{
	uint32_t *p = pgpu_begin (op, 1);
	p[0] = a;
	pgpu_end ();
}

void pgpu_reset (void)
{
	pgpu_begin (PGPU_OP_RESET, 0);
	pgpu_end ();
}

void pgpu_ping (uint32_t cookie)		{ cmd1 (PGPU_OP_PING, cookie); }
void pgpu_enable (uint32_t caps)		{ cmd1 (PGPU_OP_ENABLE, caps); }
void pgpu_disable (uint32_t caps)		{ cmd1 (PGPU_OP_DISABLE, caps); }
void pgpu_depth_func (uint32_t func)		{ cmd1 (PGPU_OP_DEPTH_FUNC, func); }
void pgpu_cull_face (uint32_t face)		{ cmd1 (PGPU_OP_CULL_FACE, face); }
void pgpu_front_face (uint32_t winding)		{ cmd1 (PGPU_OP_FRONT_FACE, winding); }
void pgpu_color (uint32_t color)		{ cmd1 (PGPU_OP_COLOR, color); }

void pgpu_clear (uint32_t mask, uint32_t color, float depth)
{
	uint32_t *p = pgpu_begin (PGPU_OP_CLEAR, 3);
	p[0] = mask;
	p[1] = color;
	p[2] = f2u (depth);
	pgpu_end ();
}

void pgpu_frame_end (uint32_t flags)
{
	cmd1 (PGPU_OP_FRAME_END, flags);
	pgpu_flush ();
}

void pgpu_viewport (int32_t x, int32_t y, uint32_t width, uint32_t height, float near, float far)
{
	uint32_t *p = pgpu_begin (PGPU_OP_VIEWPORT, 6);
	p[0] = (uint32_t) x;
	p[1] = (uint32_t) y;
	p[2] = width;
	p[3] = height;
	p[4] = f2u (near);
	p[5] = f2u (far);
	pgpu_end ();
}

void pgpu_load_matrix (uint32_t which, const float m[16])
{
	uint32_t *p = pgpu_begin (PGPU_OP_LOAD_MATRIX, 17);
	p[0] = which;
	memcpy (&p[1], m, 16 * sizeof (float));
	pgpu_end ();
}

void pgpu_draw_inline (uint32_t mode, uint32_t count, uint32_t attrib_mask,
		       const uint32_t *vertex_words, uint32_t words_per_vertex)
{
	uint32_t *p = pgpu_begin (PGPU_OP_DRAW_INLINE, 3 + count * words_per_vertex);
	if (!p)
	{
		return;
	}
	p[0] = mode;
	p[1] = count;
	p[2] = attrib_mask;
	memcpy (&p[3], vertex_words, count * words_per_vertex * sizeof (uint32_t));
	pgpu_end ();
}

void pgpu_request_info (void)
{
	pgpu_begin (PGPU_OP_GET_INFO, 0);
	pgpu_end ();
}

void pgpu_request_status (void)
{
	pgpu_begin (PGPU_OP_GET_STATUS, 0);
	pgpu_end ();
}

static void cmd2 (uint8_t op, uint32_t a, uint32_t b)
{
	uint32_t *p = pgpu_begin (op, 2);
	p[0] = a;
	p[1] = b;
	pgpu_end ();
}

/* largest data part of one packet (the staging buffer holds header, CRC and
   a few parameter words besides) */
#define MAX_DATA_BYTES	((PGPU_STAGING_WORDS - 16) * 4)

/* ---- buffers ------------------------------------------------------------- */

void pgpu_buffer_create (uint32_t id, uint32_t size_bytes)	{ cmd2 (PGPU_OP_BUFFER_CREATE, id, size_bytes); }
void pgpu_buffer_delete (uint32_t id)				{ cmd1 (PGPU_OP_BUFFER_DELETE, id); }

void pgpu_buffer_data (uint32_t id, uint32_t offset_bytes, const void *data, uint32_t length_bytes)
{
	const uint8_t *src = data;
	while (length_bytes)
	{
		uint32_t n = length_bytes < MAX_DATA_BYTES ? length_bytes : MAX_DATA_BYTES;
		uint32_t *p = pgpu_begin (PGPU_OP_BUFFER_DATA, 3 + (n + 3) / 4);
		p[0] = id;
		p[1] = offset_bytes;
		p[2] = n;
		p[3 + (n + 3) / 4 - 1] = 0;		/* zero padding */
		memcpy (&p[3], src, n);
		pgpu_end ();

		src += n;
		offset_bytes += n;
		length_bytes -= n;
	}
}

/* ---- textures ------------------------------------------------------------ */

static const uint8_t bytes_per_pixel[] = {4, 2, 2, 2, 1, 1, 2, 0, 3};

void pgpu_texture_create (uint32_t id, uint32_t width, uint32_t height, uint32_t format)
{
	uint32_t *p = pgpu_begin (PGPU_OP_TEXTURE_CREATE, 3);
	p[0] = id;
	p[1] = width | height << 16;
	p[2] = format;
	pgpu_end ();
}

static void texture_part (uint32_t id, uint32_t x, uint32_t y, uint32_t w, uint32_t h,
			  const uint8_t *rows, uint32_t src_stride, uint32_t row_bytes, uint32_t rows_n)
{
	uint32_t dst_stride = (row_bytes + 3) & ~3u;
	uint32_t words = dst_stride * rows_n / 4;
	uint32_t *p = pgpu_begin (PGPU_OP_TEXTURE_DATA, 3 + words);
	p[0] = id;
	p[1] = x | y << 16;
	p[2] = w | h << 16;
	uint8_t *dst = (uint8_t *) &p[3];
	for (uint32_t r = 0; r < rows_n; r++)
	{
		memcpy (dst + r * dst_stride, rows + r * src_stride, row_bytes);
		memset (dst + r * dst_stride + row_bytes, 0, dst_stride - row_bytes);
	}
	pgpu_end ();
}

void pgpu_texture_create_cube (uint32_t id, uint32_t size, uint32_t format)
{
	pgpu_texture_create (id, size, size, format | PGPU_TEXTURE_CUBE);
}

void pgpu_generate_mipmap (uint32_t id)		{ cmd1 (PGPU_OP_GENERATE_MIPMAP, id); }

void pgpu_texture_data (uint32_t id, uint32_t x, uint32_t y, uint32_t width, uint32_t height,
			uint32_t format, const void *pixels)
{
	pgpu_texture_data_level (id, 0, 0, x, y, width, height, format, pixels);
}

void pgpu_texture_data_level (uint32_t id, uint32_t level, uint32_t face, uint32_t x, uint32_t y,
			      uint32_t width, uint32_t height, uint32_t format, const void *pixels)
{
	uint32_t row_bytes = format == PGPU_ETC1 ? (width + 3) / 4 * 8 : width * bytes_per_pixel[format];
	pgpu_texture_data_stride (id, level, face, x, y, width, height, format, pixels,
				  (row_bytes + 3) & ~3u);
}

void pgpu_texture_data_stride (uint32_t id, uint32_t level, uint32_t face, uint32_t x, uint32_t y,
			       uint32_t width, uint32_t height, uint32_t format, const void *pixels,
			       uint32_t src_stride)
{
	id = PGPU_TEXTURE_TARGET (id, level, face);
	const uint8_t *src = pixels;

	if (format == PGPU_ETC1)
	{
		/* 4x4 blocks of 8 bytes, block rows bottom-up, tightly packed */
		uint32_t row_bytes = (width + 3) / 4 * 8;
		uint32_t rows_per_part = MAX_DATA_BYTES / row_bytes;
		uint32_t block_rows = (height + 3) / 4;
		for (uint32_t by = 0; by < block_rows; by += rows_per_part)
		{
			uint32_t n = block_rows - by < rows_per_part ? block_rows - by : rows_per_part;
			uint32_t h = by * 4 + n * 4 > height ? height - by * 4 : n * 4;
			texture_part (id, x, y + by * 4, width, h, src + by * row_bytes,
				      row_bytes, row_bytes, n);
		}
		return;
	}

	uint32_t bpp = bytes_per_pixel[format];

	/* column spans that fit a packet, then as many rows as fit */
	uint32_t span_max = MAX_DATA_BYTES / bpp;
	for (uint32_t sx = 0; sx < width; sx += span_max)
	{
		uint32_t span = width - sx < span_max ? width - sx : span_max;
		uint32_t row_bytes = span * bpp;
		uint32_t rows_per_part = MAX_DATA_BYTES / ((row_bytes + 3) & ~3u);
		for (uint32_t row = 0; row < height; row += rows_per_part)
		{
			uint32_t n = height - row < rows_per_part ? height - row : rows_per_part;
			texture_part (id, x + sx, y + row, span, n, src + row * src_stride + sx * bpp,
				      src_stride, row_bytes, n);
		}
	}
}

void pgpu_texture_params (uint32_t id, uint32_t min_filter, uint32_t mag_filter,
			  uint32_t wrap_s, uint32_t wrap_t)
{
	uint32_t *p = pgpu_begin (PGPU_OP_TEXTURE_PARAMS, 5);
	p[0] = id;
	p[1] = min_filter;
	p[2] = mag_filter;
	p[3] = wrap_s;
	p[4] = wrap_t;
	pgpu_end ();
}

void pgpu_texture_delete (uint32_t id)			{ cmd1 (PGPU_OP_TEXTURE_DELETE, id); }
void pgpu_texture_bind (uint32_t id)			{ cmd1 (PGPU_OP_TEXTURE_BIND, id); }
void pgpu_tex_env (uint32_t mode, uint32_t env_color)	{ cmd2 (PGPU_OP_TEX_ENV, mode, env_color); }

/* ---- fragment state ------------------------------------------------------ */

void pgpu_depth_mask (bool write)			{ cmd1 (PGPU_OP_DEPTH_MASK, write); }
void pgpu_blend_func (uint32_t src, uint32_t dst)	{ cmd2 (PGPU_OP_BLEND_FUNC, src, dst); }
void pgpu_alpha_func (uint32_t func, float ref)		{ cmd2 (PGPU_OP_ALPHA_FUNC, func, f2u (ref)); }

void pgpu_color_mask (bool r, bool g, bool b, bool a)
{
	cmd1 (PGPU_OP_COLOR_MASK, (r ? 1 : 0) | (g ? 2 : 0) | (b ? 4 : 0) | (a ? 8 : 0));
}

/* ---- transform, lighting, fog, current values ----------------------------- */

void pgpu_light (uint32_t light, const float position[4], uint32_t ambient, uint32_t diffuse,
		 uint32_t specular, const float attenuation[3])
{
	uint32_t *p = pgpu_begin (PGPU_OP_LIGHT, 11);
	p[0] = light;
	memcpy (&p[1], position, 4 * sizeof (float));
	p[5] = ambient;
	p[6] = diffuse;
	p[7] = specular;
	memcpy (&p[8], attenuation, 3 * sizeof (float));
	pgpu_end ();
}

void pgpu_material (uint32_t ambient, uint32_t diffuse, uint32_t specular, uint32_t emission,
		    float shininess)
{
	uint32_t *p = pgpu_begin (PGPU_OP_MATERIAL, 5);
	p[0] = ambient;
	p[1] = diffuse;
	p[2] = specular;
	p[3] = emission;
	p[4] = f2u (shininess);
	pgpu_end ();
}

void pgpu_light_model (uint32_t ambient, bool two_side)	{ cmd2 (PGPU_OP_LIGHT_MODEL, ambient, two_side); }

void pgpu_fog (uint32_t mode, uint32_t color, float start, float end, float density)
{
	uint32_t *p = pgpu_begin (PGPU_OP_FOG, 5);
	p[0] = mode;
	p[1] = color;
	p[2] = f2u (start);
	p[3] = f2u (end);
	p[4] = f2u (density);
	pgpu_end ();
}

void pgpu_shade_model (uint32_t mode)			{ cmd1 (PGPU_OP_SHADE_MODEL, mode); }

void pgpu_normal (float x, float y, float z)
{
	uint32_t *p = pgpu_begin (PGPU_OP_NORMAL, 3);
	p[0] = f2u (x);
	p[1] = f2u (y);
	p[2] = f2u (z);
	pgpu_end ();
}

void pgpu_texcoord (float s, float t)			{ cmd2 (PGPU_OP_TEXCOORD, f2u (s), f2u (t)); }

/* ---- arrays and drawing -------------------------------------------------- */

void pgpu_array (uint32_t attribute, uint32_t buffer, uint32_t offset_bytes, uint32_t stride_bytes,
		 uint32_t size, uint32_t type)
{
	uint32_t *p = pgpu_begin (PGPU_OP_ARRAY, 6);
	p[0] = attribute;
	p[1] = buffer;
	p[2] = offset_bytes;
	p[3] = stride_bytes;
	p[4] = size;
	p[5] = type;
	pgpu_end ();
}

void pgpu_arrays_enable (uint32_t mask)			{ cmd1 (PGPU_OP_ARRAYS_ENABLE, mask); }

void pgpu_draw_arrays (uint32_t mode, uint32_t first, uint32_t count)
{
	uint32_t *p = pgpu_begin (PGPU_OP_DRAW_ARRAYS, 3);
	p[0] = mode;
	p[1] = first;
	p[2] = count;
	pgpu_end ();
}

void pgpu_draw_elements (uint32_t mode, uint32_t count, uint32_t index_type, uint32_t buffer,
			 uint32_t offset_bytes)
{
	uint32_t *p = pgpu_begin (PGPU_OP_DRAW_ELEMENTS, 5);
	p[0] = mode;
	p[1] = count;
	p[2] = index_type;
	p[3] = buffer;
	p[4] = offset_bytes;
	pgpu_end ();
}

/* ---- programs ------------------------------------------------------------ */

void pgpu_program_create (uint32_t id, const uint32_t *blob, uint32_t words)
{
	cmd2 (PGPU_OP_PROGRAM_CREATE, id, words);

	const uint32_t max = PGPU_STAGING_WORDS - 16;
	for (uint32_t offset = 0; offset < words; offset += max)
	{
		uint32_t n = words - offset < max ? words - offset : max;
		uint32_t *p = pgpu_begin (PGPU_OP_PROGRAM_DATA, 2 + n);
		p[0] = id;
		p[1] = offset;
		memcpy (&p[2], blob + offset, n * 4);
		pgpu_end ();
	}
}

void pgpu_program_delete (uint32_t id)		{ cmd1 (PGPU_OP_PROGRAM_DELETE, id); }
void pgpu_use_program (uint32_t id)		{ cmd1 (PGPU_OP_USE_PROGRAM, id); }

/* one PROGRAM_UNIFORM per run of consecutive storage words */
void pgpu_program_uniform (uint32_t id, const uint16_t *offsets, uint32_t scalars, const float *values)
{
	for (uint32_t stage = 0; stage < 2; stage++)
	{
		const uint16_t *o = offsets + stage * scalars;
		uint32_t i = 0;
		while (i < scalars)
		{
			if (o[i] == 0xFFFF)
			{
				i++;
				continue;
			}
			uint32_t n = 1;
			while (i + n < scalars && o[i + n] == o[i] + n)
			{
				n++;
			}
			uint32_t *p = pgpu_begin (PGPU_OP_PROGRAM_UNIFORM, 2 + n);
			p[0] = id;
			p[1] = o[i];
			memcpy (&p[2], values + i, n * 4);
			pgpu_end ();
			i += n;
		}
	}
}

void pgpu_program_uniform1f (uint32_t id, const uint16_t *offsets, float value)
{
	pgpu_program_uniform (id, offsets, 1, &value);
}

void pgpu_program_sampler (uint32_t id, uint32_t sampler, uint32_t unit)
{
	uint32_t *p = pgpu_begin (PGPU_OP_PROGRAM_SAMPLER, 3);
	p[0] = id;
	p[1] = sampler;
	p[2] = unit;
	pgpu_end ();
}

void pgpu_texture_bind_unit (uint32_t unit, uint32_t texture)	{ cmd2 (PGPU_OP_TEXTURE_BIND_UNIT, unit, texture); }

void pgpu_vertex_attrib (uint32_t index, float x, float y, float z, float w)
{
	uint32_t *p = pgpu_begin (PGPU_OP_VERTEX_ATTRIB, 5);
	p[0] = index;
	p[1] = f2u (x);
	p[2] = f2u (y);
	p[3] = f2u (z);
	p[4] = f2u (w);
	pgpu_end ();
}

void pgpu_attrib_array (uint32_t index, uint32_t buffer, uint32_t offset_bytes, uint32_t stride_bytes,
			uint32_t size, uint32_t type)
{
	uint32_t *p = pgpu_begin (PGPU_OP_ATTRIB_ARRAY, 6);
	p[0] = index;
	p[1] = buffer;
	p[2] = offset_bytes;
	p[3] = stride_bytes;
	p[4] = size;
	p[5] = type;
	pgpu_end ();
}

void pgpu_attribs_enable (uint32_t mask)			{ cmd1 (PGPU_OP_ATTRIBS_ENABLE, mask); }

void pgpu_scissor (int32_t x, int32_t y, uint32_t width, uint32_t height)
{
	uint32_t *p = pgpu_begin (PGPU_OP_SCISSOR, 4);
	p[0] = (uint32_t) x;
	p[1] = (uint32_t) y;
	p[2] = width;
	p[3] = height;
	pgpu_end ();
}

void pgpu_polygon_offset (float factor, float units)	{ cmd2 (PGPU_OP_POLYGON_OFFSET, f2u (factor), f2u (units)); }
void pgpu_line_width (float width)			{ cmd1 (PGPU_OP_LINE_WIDTH, f2u (width)); }

/* ---- client-side arrays ------------------------------------------------------ */

static const uint8_t type_bytes[] = {4, 2, 2, 1, 1, 1, 1, 2, 2, 4};

static struct
{
	const uint8_t *pointer;
	uint32_t size, type, stride;
} client[8];

void pgpu_client_attrib_pointer (uint32_t index, uint32_t size, uint32_t type, uint32_t stride,
				 const void *pointer)
{
	if (index >= 8 || type > PGPU_FIXED)
	{
		return;
	}
	client[index].pointer = pointer;
	client[index].size = size;
	client[index].type = type;
	client[index].stride = stride ? stride : size * type_bytes[type];
}

/* words of one packet for n vertices (and indices) */
static uint32_t inline_words (uint32_t vertices, uint32_t index_bytes)
{
	uint32_t words = 5 + (index_bytes + 3) / 4;
	for (int i = 0; i < 8; i++)
	{
		if (client[i].pointer)
		{
			uint32_t bytes = client[i].size * type_bytes[client[i].type] * vertices;
			words += 1 + (bytes + 3) / 4;
		}
	}
	return words;
}

/* one packet: vertices first .. first+count-1 [and 16-bit indices] */
static bool draw_inline_packet (uint32_t mode, uint32_t first, uint32_t count,
				const uint16_t *indices, uint32_t index_count)
{
	uint32_t index_bytes = index_count * 2;
	uint32_t words = inline_words (count, index_bytes);
	uint32_t *p = pgpu_begin (PGPU_OP_PROGRAM_DRAW_INLINE, words);
	if (!p)
	{
		return false;			/* larger than a packet */
	}

	uint32_t mask = 0;
	for (int i = 0; i < 8; i++)
	{
		mask |= client[i].pointer ? 1u << i : 0;
	}
	p[0] = mode;
	p[1] = count;
	p[2] = mask;
	p[3] = index_count;
	p[4] = PGPU_INDEX_U16;
	uint32_t w = 5;
	for (int i = 0; i < 8; i++)
	{
		if (!client[i].pointer)
		{
			continue;
		}
		uint32_t element = client[i].size * type_bytes[client[i].type];
		p[w++] = client[i].type | client[i].size << 8;
		uint8_t *dst = (uint8_t *) &p[w];
		const uint8_t *src = client[i].pointer + first * client[i].stride;
		for (uint32_t v = 0; v < count; v++, src += client[i].stride, dst += element)
		{
			memcpy (dst, src, element);
		}
		uint32_t bytes = element * count;
		memset ((uint8_t *) &p[w] + bytes, 0, (4 - bytes % 4) % 4);
		w += (bytes + 3) / 4;
	}
	if (index_count)
	{
		memcpy (&p[w], indices, index_bytes);
		memset ((uint8_t *) &p[w] + index_bytes, 0, (4 - index_bytes % 4) % 4);
	}
	pgpu_end ();
	return true;
}

/* the largest vertex count of the current client arrays that fits a packet */
uint32_t pgpu_client_max_vertices (void)
{
	uint32_t per_vertex = inline_words (1, 0) - inline_words (0, 0) + 1;
	return (PGPU_STAGING_WORDS - 16 - inline_words (0, 0)) / per_vertex;
}

bool pgpu_draw_arrays_client (uint32_t mode, uint32_t first, uint32_t count)
{
	uint32_t max = pgpu_client_max_vertices ();

	if (count <= max)
	{
		return draw_inline_packet (mode, first, count, NULL, 0);
	}

	/* split lists at primitive boundaries */
	uint32_t step =   mode == PGPU_TRIANGLES ? 3
			: mode == PGPU_LINES ? 2
			: mode == PGPU_POINTS ? 1 : 0;
	if (!step)
	{
		return false;
	}
	max -= max % step;
	while (count)
	{
		uint32_t n = count < max ? count : max;
		if (!draw_inline_packet (mode, first, n, NULL, 0))
		{
			return false;
		}
		first += n;
		count -= n;
	}
	return true;
}

/* sends the vertices min .. max index, indices rebased to 16 bits */
bool pgpu_draw_elements_client (uint32_t mode, uint32_t count, uint32_t index_type, const void *indices)
{
	static uint16_t rebased[PGPU_STAGING_WORDS * 2];
	if (count == 0 || count > sizeof rebased / sizeof rebased[0])
	{
		return count == 0;
	}

	uint32_t lo = 0xFFFFFFFF, hi = 0;
	for (uint32_t i = 0; i < count; i++)
	{
		uint32_t index = index_type == PGPU_INDEX_U16 ? ((const uint16_t *) indices)[i]
							      : ((const uint8_t *) indices)[i];
		rebased[i] = (uint16_t) index;
		lo = index < lo ? index : lo;
		hi = index > hi ? index : hi;
	}
	for (uint32_t i = 0; i < count; i++)
	{
		rebased[i] -= lo;
	}

	return draw_inline_packet (mode, lo, hi - lo + 1, rebased, count);
}

void pgpu_blend_func_separate (uint32_t src_rgb, uint32_t dst_rgb, uint32_t src_alpha, uint32_t dst_alpha)
{
	uint32_t *p = pgpu_begin (PGPU_OP_BLEND_FUNC_SEPARATE, 4);
	p[0] = src_rgb;
	p[1] = dst_rgb;
	p[2] = src_alpha;
	p[3] = dst_alpha;
	pgpu_end ();
}

void pgpu_blend_equation (uint32_t mode_rgb, uint32_t mode_alpha)	{ cmd2 (PGPU_OP_BLEND_EQUATION, mode_rgb, mode_alpha); }

void pgpu_blend_color (float r, float g, float b, float a)
{
	uint32_t *p = pgpu_begin (PGPU_OP_BLEND_COLOR, 4);
	p[0] = f2u (r);
	p[1] = f2u (g);
	p[2] = f2u (b);
	p[3] = f2u (a);
	pgpu_end ();
}

static void cmd4 (uint8_t op, uint32_t a, uint32_t b, uint32_t c, uint32_t d)
{
	uint32_t *p = pgpu_begin (op, 4);
	p[0] = a;
	p[1] = b;
	p[2] = c;
	p[3] = d;
	pgpu_end ();
}

void pgpu_stencil_func (uint32_t face, uint32_t func, uint32_t ref, uint32_t mask)
{
	cmd4 (PGPU_OP_STENCIL_FUNC, face, func, ref, mask);
}

void pgpu_stencil_op (uint32_t face, uint32_t fail, uint32_t zfail, uint32_t zpass)
{
	cmd4 (PGPU_OP_STENCIL_OP, face, fail, zfail, zpass);
}

void pgpu_stencil_mask (uint32_t face, uint32_t mask)	{ cmd2 (PGPU_OP_STENCIL_MASK, face, mask); }

void pgpu_clear_stencil (uint32_t mask, uint32_t color, float depth, uint32_t stencil)
{
	cmd4 (PGPU_OP_CLEAR, mask, color, f2u (depth), stencil);
}

/* ---- framebuffers ---------------------------------------------------------- */

void pgpu_framebuffer_create (uint32_t id, uint32_t texture, uint32_t face, uint32_t flags)
{
	uint32_t *p = pgpu_begin (PGPU_OP_FRAMEBUFFER_CREATE, 3);
	p[0] = id;
	p[1] = texture | face << 24;
	p[2] = flags;
	pgpu_end ();
}

void pgpu_framebuffer_delete (uint32_t id)	{ cmd1 (PGPU_OP_FRAMEBUFFER_DELETE, id); }
void pgpu_bind_framebuffer (uint32_t id)	{ cmd1 (PGPU_OP_BIND_FRAMEBUFFER, id); }

bool pgpu_read_pixels (int32_t x, int32_t y, uint32_t width, uint32_t height, uint32_t *pixels,
		       uint32_t timeout_ms)
{
	pixel_sink_words = width * height;
	STORE (pixel_words_received, 0);
	STORE (pixel_sink, pixels);		/* published last: the parser may run now */

	cmd4 (PGPU_OP_READ_PIXELS, (uint32_t) x, (uint32_t) y, width, height);
	pgpu_flush ();

	uint64_t end = pgpu_link_time_us () + timeout_ms * 1000ull;
	while (LOAD (pixel_words_received) < pixel_sink_words && pgpu_link_time_us () < end)
	{
		pgpu_link_poll ();
	}
	STORE (pixel_sink, NULL);
	return LOAD (pixel_words_received) >= pixel_sink_words;
}

void pgpu_copy_tex_image (uint32_t texture, uint32_t level, uint32_t face, uint32_t xoffset,
			  uint32_t yoffset, int32_t x, int32_t y, uint32_t width, uint32_t height)
{
	uint32_t *p = pgpu_begin (PGPU_OP_COPY_TEX_IMAGE, 6);
	p[0] = PGPU_TEXTURE_TARGET (texture, level, face);
	p[1] = xoffset | yoffset << 16;
	p[2] = (uint32_t) x;
	p[3] = (uint32_t) y;
	p[4] = width;
	p[5] = height;
	pgpu_end ();
}

/* ---- video (docs/protocol.md 7.12) ----------------------------------------------- */

void pgpu_video_open (uint32_t stream, uint32_t texture, uint32_t width, uint32_t height,
		      uint32_t coded_width, uint32_t coded_height, const void *avcc, uint32_t avcc_bytes)
{
	if (!avcc)
	{
		avcc_bytes = 0;
	}
	uint32_t words = (avcc_bytes + 3) / 4;
	uint32_t *p = pgpu_begin (PGPU_OP_VIDEO_OPEN, PGPU_VIDEO_OPEN_WORDS + words);
	if (!p)
	{
		return;
	}
	p[0] = stream;
	p[1] = PGPU_VIDEO_H264;
	p[2] = texture;
	p[3] = width | height << 16;
	p[4] = coded_width | coded_height << 16;
	p[5] = avcc ? PGPU_VIDEO_AVCC : PGPU_VIDEO_ANNEXB;
	p[6] = avcc_bytes;
	if (words)
	{
		p[PGPU_VIDEO_OPEN_WORDS + words - 1] = 0;
		memcpy (p + PGPU_VIDEO_OPEN_WORDS, avcc, avcc_bytes);
	}
	pgpu_end ();
	if (stream >= 1 && stream <= PGPU_VIDEO_STREAMS)
	{
		/* the RPi counts from 0 again; until its first status (the old
		   stream's gone) there's no room */
		video_bytes_sent[stream] = 0;
		video_samples_sent[stream] = 0;
		STORE (video_seq[stream], 0);
	}
}

/* whole sectors: with the padding, the header and the CRC, a packet fits in a
   staging buffer */
#define VIDEO_CHUNK	((PGPU_STAGING_WORDS - PGPU_READ_ALIGN / 4 - 2 - PGPU_VIDEO_DATA_HEADER) * 4 \
			 / PGPU_READ_SECTOR * PGPU_READ_SECTOR)

/* pgpu_begin, with idle words before the packet so that its payload word `at`
   is PGPU_READ_ALIGN aligned */
static uint32_t *begin_aligned (uint8_t opcode, uint32_t payload_words, uint32_t at)
{
	const uint32_t align = PGPU_READ_ALIGN / 4;
	uint32_t pad = (align - (fill + 1 + at) % align) % align;
	if (fill + pad + payload_words + 2 > PGPU_STAGING_WORDS)
	{
		pgpu_flush ();
		pad = (align - (1 + at) % align) % align;
	}
	while (pad--)
	{
		staging[current][fill++] = PGPU_IDLE_WORD;
	}
	return pgpu_begin (opcode, payload_words);
}

/* a sample in VIDEO_DATA packets, each packet's data from read (memory or a
   file: straight into the packet). With aligned: the first packet up to the
   file's next sector boundary (if the sample has a whole sector), then whole
   sectors into aligned packet data (see pgpu_read_t) */
static bool video_sample (uint32_t stream, uint32_t flags, int64_t pts, uint32_t bytes,
			  pgpu_read_t read, void *ctx, uint64_t offset, bool aligned)
{
	uint32_t done = 0;
	bool ok = true;
	do
	{
		uint32_t n = bytes - done < VIDEO_CHUNK ? bytes - done : VIDEO_CHUNK;
		uint32_t head = (uint32_t) (-offset % PGPU_READ_SECTOR);
		if (aligned && done == 0 && head && head + PGPU_READ_SECTOR <= bytes)
		{
			n = head;			/* the partial sector alone */
		}
		uint32_t words = PGPU_VIDEO_DATA_HEADER + (n + 3) / 4;
		uint32_t *p = aligned ? begin_aligned (PGPU_OP_VIDEO_DATA, words, PGPU_VIDEO_DATA_HEADER)
				      : pgpu_begin (PGPU_OP_VIDEO_DATA, words);
		p[0] = stream;
		p[1] =   (flags & ~(PGPU_VIDEO_FIRST | PGPU_VIDEO_LAST))
		       | (done == 0 ? PGPU_VIDEO_FIRST : 0)
		       | (done + n == bytes ? PGPU_VIDEO_LAST : 0);
		p[2] = (uint32_t) pts;
		p[3] = (uint32_t) ((uint64_t) pts >> 32);
		p[4] = n;
		p[5] = bytes;					/* the whole sample's */
		if (n)
		{
			p[PGPU_VIDEO_DATA_HEADER + (n + 3) / 4 - 1] = 0;	/* the last word's padding */
			ok = read (ctx, offset + done, p + PGPU_VIDEO_DATA_HEADER, n);
		}
		if (!ok)
		{
			p[4] = 0;				/* this chunk empty, the sample unfinished */
			p[1] &= ~PGPU_VIDEO_LAST;
		}
		pgpu_end ();
		done += n;
	}
	while (ok && done < bytes);

	if (ok && stream >= 1 && stream <= PGPU_VIDEO_STREAMS)
	{
		video_bytes_sent[stream] += bytes;
		video_samples_sent[stream]++;
	}
	return ok;
}

static bool memory_read (void *ctx, uint64_t offset, void *buffer, uint32_t bytes)
{
	memcpy (buffer, (const uint8_t *) ctx + offset, bytes);
	return true;
}

void pgpu_video_sample (uint32_t stream, uint32_t flags, int64_t pts, const void *data, uint32_t bytes)
{
	video_sample (stream, flags, pts, bytes, memory_read, (void *) data, 0, false);
}

bool pgpu_video_sample_read (uint32_t stream, uint32_t flags, int64_t pts, uint32_t bytes,
			     pgpu_read_t read, void *ctx, uint64_t offset)
{
	return video_sample (stream, flags, pts, bytes, read, ctx, offset, true);
}

void pgpu_video_control (uint32_t stream, uint32_t op, int64_t arg)
{
	uint32_t *p = pgpu_begin (PGPU_OP_VIDEO_CONTROL, 4);
	p[0] = stream;
	p[1] = op;
	p[2] = (uint32_t) arg;
	p[3] = (uint32_t) ((uint64_t) arg >> 32);
	pgpu_end ();
}

void pgpu_video_resize (uint32_t stream, uint32_t width, uint32_t height)
{
	pgpu_video_control (stream, PGPU_VIDEO_RESIZE, (int64_t) (width | height << 16));
}

void pgpu_video_request_status (uint32_t stream)	{ cmd1 (PGPU_OP_VIDEO_GET_STATUS, stream); }
