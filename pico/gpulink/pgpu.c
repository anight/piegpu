/*
 * pgpu.c - Pico side of the Pico GPU link
 */
#include "pgpu.h"
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/pio.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/sync.h"
#include "pcm_out.pio.h"

#define PIN_DATA	16
#define PIN_CLOCK_BASE	17		/* BCLK, FS = 18 */
#define PIN_REPLY	19
#define PIN_READY	20
#define PIN_FRAME	21

static PIO pio = pio0;
static uint sm, sm_rx;
static uint offset_tx, offset_rx;
static int idle_dma, packet_dma, rx_dma;

static const uint32_t idle_word = PGPU_IDLE_WORD;

static uint32_t staging[2][PGPU_STAGING_WORDS];
static uint32_t current;		/* staging buffer being filled */
static uint32_t fill;			/* words in it */
static uint32_t open_packet;		/* index of the header of the open packet */

static uint32_t crc_table[256];
static volatile uint32_t frame_count;
static pgpu_stats_t stats;

/* reply receiver: 32 KB DMA ring = 3.5 ms at 75 Mbit/s, parsed every 1 ms */
#define RX_RING_BITS	15
#define RX_RING_WORDS	((1u << RX_RING_BITS) / 4)
static uint32_t rx_ring[RX_RING_WORDS] __attribute__ ((aligned (1u << RX_RING_BITS)));
static uint64_t rx_written;		/* words written by the DMA, unwrapped */
static uint32_t rx_last_index;
static uint64_t rx_bit;			/* parse position, bits, unwrapped */
static repeating_timer_t rx_timer;

#define REPLY_QUEUE	16
static pgpu_reply_t reply_queue[REPLY_QUEUE];
static volatile uint32_t reply_head, reply_tail;	/* tail written by the parser */

static void frame_irq (uint gpio, uint32_t events)
{
	if (gpio == PIN_FRAME)
	{
		frame_count++;
	}
}

static uint32_t crc32_words (const uint32_t *p, uint32_t n);

static inline uint32_t rx_bits (uint64_t bit)		/* 32 stream bits from bit on */
{
	uint32_t word = (uint32_t) (bit >> 5) % RX_RING_WORDS;
	uint32_t shift = bit & 31;
	uint32_t w = rx_ring[word];
	if (shift == 0)
	{
		return w;
	}
	return w << shift | rx_ring[(word + 1) % RX_RING_WORDS] >> (32 - shift);
}

/* The reply stream as sampled has an unknown bit offset. Idle words are 0,
   and a reply header starts with 0x5A (binary 0101 1010), so the first 1
   bit after idle is header bit 30: that gives the alignment of each packet. */
static void rx_parse (void)
{
	uint32_t index = (dma_hw->ch[rx_dma].write_addr - (uintptr_t) rx_ring) / 4;
	rx_written += (index + RX_RING_WORDS - rx_last_index) % RX_RING_WORDS;
	rx_last_index = index;

	uint64_t avail = rx_written * 32;
	if (rx_written - (rx_bit >> 5) > RX_RING_WORDS - 256)
	{
		stats.reply_overruns++;
		rx_bit = (rx_written - RX_RING_WORDS / 2) * 32;
	}

	while (rx_bit + 64 <= avail)
	{
		uint32_t v = rx_ring[(uint32_t) (rx_bit >> 5) % RX_RING_WORDS] << (rx_bit & 31);
		if (v == 0)
		{
			rx_bit = (rx_bit | 31) + 1;		/* rest of this word is idle */
			continue;
		}

		uint64_t one = rx_bit + __builtin_clz (v);
		if (one == 0)
		{
			rx_bit = 1;
			continue;
		}
		uint64_t start = one - 1;
		uint32_t header = rx_bits (start);
		uint32_t length = PGPU_HEADER_LEN (header);
		if (   PGPU_HEADER_SYNC (header) != PGPU_SYNC_REPLY
		    || length > PGPU_MAX_REPLY_PAYLOAD)
		{
			rx_bit = one + 1;			/* not a header: go on searching */
			continue;
		}
		if (start + (length + 2) * 32 > avail)
		{
			break;					/* wait for the rest */
		}

		uint32_t words[PGPU_MAX_REPLY_PAYLOAD + 2];
		for (uint32_t i = 0; i < length + 2; i++)
		{
			words[i] = rx_bits (start + i * 32);
		}
		if (crc32_words (words, length + 1) != words[length + 1])
		{
			stats.reply_crc_errors++;
			rx_bit = one + 1;
			continue;
		}
		rx_bit = start + (length + 2) * 32;
		stats.replies++;

		uint8_t opcode = PGPU_HEADER_OP (header);
		if (opcode == PGPU_REPLY_ERROR && length >= 3)
		{
			stats.zero_errors++;
			memcpy (stats.last_error, &words[1], sizeof stats.last_error);
		}

		uint32_t tail = reply_tail;
		if ((tail + 1) % REPLY_QUEUE == reply_head)
		{
			stats.replies_lost++;
			continue;
		}
		pgpu_reply_t *r = &reply_queue[tail];
		r->opcode = opcode;
		r->length = length;
		memcpy (r->payload, &words[1], length * 4);
		reply_tail = (tail + 1) % REPLY_QUEUE;
	}
}

static bool rx_timer_callback (repeating_timer_t *t)
{
	rx_parse ();
	return true;
}

bool pgpu_poll_reply (pgpu_reply_t *reply)
{
	uint32_t head = reply_head;
	if (head == reply_tail)
	{
		return false;
	}
	*reply = reply_queue[head];
	reply_head = (head + 1) % REPLY_QUEUE;
	return true;
}

bool pgpu_wait_reply (uint8_t opcode, pgpu_reply_t *reply, uint32_t timeout_ms)
{
	absolute_time_t end = make_timeout_time_ms (timeout_ms);
	while (!time_reached (end))
	{
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
	if (!pgpu_wait_reply (PGPU_REPLY_STATUS, &r, timeout_ms) || r.length < 5)
	{
		return false;
	}
	memcpy (status, r.payload, sizeof *status);
	return true;
}

int32_t pgpu_ping_wait (uint32_t cookie, uint32_t timeout_ms)
{
	pgpu_reply_t r;
	pgpu_ping (cookie);
	pgpu_flush ();
	uint64_t start = time_us_64 ();
	absolute_time_t end = make_timeout_time_ms (timeout_ms);
	while (!time_reached (end))
	{
		if (   pgpu_wait_reply (PGPU_REPLY_PONG, &r, 1)
		    && r.length == 1 && r.payload[0] == cookie)
		{
			return (int32_t) (time_us_64 () - start);
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

	/* READY and FRAME from the Zero (READY has an external 10k pull-down) */
	gpio_init (PIN_READY);
	gpio_set_dir (PIN_READY, GPIO_IN);
	gpio_disable_pulls (PIN_READY);
	gpio_init (PIN_FRAME);
	gpio_set_dir (PIN_FRAME, GPIO_IN);
	gpio_pull_down (PIN_FRAME);
	gpio_set_irq_enabled_with_callback (PIN_FRAME, GPIO_IRQ_EDGE_RISE, true, frame_irq);

	/* I2S master: bit clock = sysclk / 2 */
	sm = pio_claim_unused_sm (pio, true);
	offset_tx = pio_add_program (pio, &pcm_out_program);
	pcm_out_program_init (pio, sm, offset_tx, PIN_DATA, PIN_CLOCK_BASE);
	pio_sm_set_clkdiv_int_frac8 (pio, sm, 1, 0);

	/* reply sampler, in lockstep with the bit clock */
	sm_rx = pio_claim_unused_sm (pio, true);
	offset_rx = pio_add_program (pio, &pcm_in_program);
	pio_gpio_init (pio, PIN_REPLY);
	gpio_pull_down (PIN_REPLY);
	pio->input_sync_bypass |= 1u << PIN_REPLY;	/* 75 MHz: no 2-cycle synchroniser */
	pcm_in_program_init (pio, sm_rx, offset_rx, PIN_REPLY, 1);
	pio_sm_set_clkdiv_int_frac8 (pio, sm_rx, 1, 0);

	rx_dma = dma_claim_unused_channel (true);
	dma_channel_config rc = dma_channel_get_default_config (rx_dma);
	channel_config_set_transfer_data_size (&rc, DMA_SIZE_32);
	channel_config_set_read_increment (&rc, false);
	channel_config_set_write_increment (&rc, true);
	channel_config_set_ring (&rc, true, RX_RING_BITS);
	channel_config_set_dreq (&rc, pio_get_dreq (pio, sm_rx, false));
	channel_config_set_high_priority (&rc, true);
	dma_channel_configure (rx_dma, &rc, rx_ring, &pio->rxf[sm_rx],
			       dma_encode_endless_transfer_count (), true);

	/* idle: the same zero word forever */
	idle_dma = dma_claim_unused_channel (true);
	dma_channel_config c = dma_channel_get_default_config (idle_dma);
	channel_config_set_transfer_data_size (&c, DMA_SIZE_32);
	channel_config_set_read_increment (&c, false);
	channel_config_set_write_increment (&c, false);
	channel_config_set_dreq (&c, pio_get_dreq (pio, sm, true));
	dma_channel_configure (idle_dma, &c, &pio->txf[sm], &idle_word,
			       dma_encode_transfer_count_with_self_trigger (0x0FFFFFFF), true);

	/* packets: a staging buffer, then back to idle */
	packet_dma = dma_claim_unused_channel (true);
	c = dma_channel_get_default_config (packet_dma);
	channel_config_set_transfer_data_size (&c, DMA_SIZE_32);
	channel_config_set_read_increment (&c, true);
	channel_config_set_write_increment (&c, false);
	channel_config_set_dreq (&c, pio_get_dreq (pio, sm, true));
	channel_config_set_chain_to (&c, idle_dma);
	dma_channel_configure (packet_dma, &c, &pio->txf[sm], staging[0], 0, false);

	pio_enable_sm_mask_in_sync (pio, (1u << sm) | (1u << sm_rx));

	current = 0;
	fill = 0;

	add_repeating_timer_us (-1000, rx_timer_callback, NULL, &rx_timer);
}

void pgpu_set_reply_phase (unsigned phase)
{
	pgpu_flush ();
	while (dma_channel_is_busy (packet_dma))
	{
		tight_loop_contents ();
	}

	/* restart both state machines in the same cycle (the command stream is
	   idle, so the Zero only sees one short frame of zeros) */
	uint32_t mask = (1u << sm) | (1u << sm_rx);
	pio_set_sm_mask_enabled (pio, mask, false);
	pio_restart_sm_mask (pio, mask);
	pio_sm_exec (pio, sm, pio_encode_jmp (offset_tx + pcm_out_offset_entry_point));
	pio_sm_exec (pio, sm_rx, pio_encode_jmp (offset_rx + (phase ? pcm_in_offset_skip
								     : pcm_in_offset_sample)));
	pio_enable_sm_mask_in_sync (pio, mask);
}

bool pgpu_wait_ready (uint32_t timeout_ms)
{
	absolute_time_t end = make_timeout_time_ms (timeout_ms);
	while (!gpio_get (PIN_READY))
	{
		if (time_reached (end))
		{
			return false;
		}
	}
	return true;
}

bool pgpu_wait_frame (uint32_t timeout_ms)
{
	uint32_t start = frame_count;
	absolute_time_t end = make_timeout_time_ms (timeout_ms);
	while (frame_count == start)
	{
		if (time_reached (end))
		{
			return false;
		}
		tight_loop_contents ();
	}
	return true;
}

static uint32_t crc32_words (const uint32_t *p, uint32_t n)
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

	/* one batch in flight at a time */
	while (dma_channel_is_busy (packet_dma))
	{
		tight_loop_contents ();
	}

	/* READY high: the Zero has room for a maximum-size packet (> one batch) */
	uint64_t wait_start = time_us_64 ();
	while (!gpio_get (PIN_READY))
	{
		tight_loop_contents ();
	}
	stats.ready_wait_us += (uint32_t) (time_us_64 () - wait_start);

	/* stop the idle stream, send the batch; its completion restarts idle */
	dma_channel_abort (idle_dma);
	dma_channel_transfer_from_buffer_now (packet_dma, staging[current], fill);

	stats.words += fill;
	stats.batches++;

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
	staging[current][fill++] = crc32_words (&staging[current][open_packet], 1 + length);
	stats.packets++;
}

pgpu_stats_t pgpu_get_stats (void)
{
	uint32_t irq = save_and_disable_interrupts ();
	pgpu_stats_t s = stats;
	memset (&stats, 0, sizeof stats);
	memcpy (stats.last_error, s.last_error, sizeof stats.last_error);
	restore_interrupts (irq);
	s.frames = frame_count;
	return s;
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

static const uint8_t bytes_per_pixel[] = {4, 2, 2, 2, 1, 1, 2, 0};

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

void pgpu_texture_data (uint32_t id, uint32_t x, uint32_t y, uint32_t width, uint32_t height,
			uint32_t format, const void *pixels)
{
	const uint8_t *src = pixels;

	if (format == PGPU_ETC1)
	{
		/* 4x4 blocks of 8 bytes, block rows bottom-up */
		uint32_t row_bytes = width / 4 * 8;
		uint32_t rows_per_part = MAX_DATA_BYTES / row_bytes;
		for (uint32_t by = 0; by < height / 4; by += rows_per_part)
		{
			uint32_t n = height / 4 - by < rows_per_part ? height / 4 - by : rows_per_part;
			texture_part (id, x, y + by * 4, width, n * 4, src + by * row_bytes,
				      row_bytes, row_bytes, n);
		}
		return;
	}

	uint32_t bpp = bytes_per_pixel[format];
	uint32_t src_stride = (width * bpp + 3) & ~3u;		/* GL unpack alignment 4 */

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

static const uint8_t type_bytes[] = {4, 2, 2, 1, 1, 1, 1, 2, 2};

static struct
{
	const uint8_t *pointer;
	uint32_t size, type, stride;
} client[8];

void pgpu_client_attrib_pointer (uint32_t index, uint32_t size, uint32_t type, uint32_t stride,
				 const void *pointer)
{
	if (index >= 8 || type > PGPU_USHORT_NORM)
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

bool pgpu_draw_arrays_client (uint32_t mode, uint32_t first, uint32_t count)
{
	/* the largest vertex count that fits a packet */
	uint32_t per_vertex = inline_words (1, 0) - inline_words (0, 0) + 1;
	uint32_t max = (PGPU_STAGING_WORDS - 16 - inline_words (0, 0)) / per_vertex;

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
