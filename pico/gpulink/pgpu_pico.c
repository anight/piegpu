/*
 * pgpu_pico.c - the transport of pgpu.c on the Pico (pgpu_link.h): the I2S
 * link (docs/protocol.md 2-4)
 *
 * Batches go out by DMA over PIO, READY is checked before each batch, idle
 * words keep the clock running. Replies are sampled by a second state machine
 * in lockstep with the bit clock, written to a DMA ring, and parsed every
 * millisecond by a timer callback.
 */
#include "pgpu_link.h"
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

static volatile uint32_t frame_count;

/* reply receiver: 32 KB DMA ring = 3.5 ms at 75 Mbit/s, parsed every 1 ms */
#define RX_RING_BITS	15
#define RX_RING_WORDS	((1u << RX_RING_BITS) / 4)
static uint32_t rx_ring[RX_RING_WORDS] __attribute__ ((aligned (1u << RX_RING_BITS)));
static uint64_t rx_written;		/* words written by the DMA, unwrapped */
static uint32_t rx_last_index;
static uint64_t rx_bit;			/* parse position, bits, unwrapped */
static repeating_timer_t rx_timer;


static void frame_irq (uint gpio, uint32_t events)
{
	if (gpio == PIN_FRAME)
	{
		frame_count++;
	}
}

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
		pgpu_link_stats.reply_overruns++;
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
		if (pgpu_crc32 (words, length + 1) != words[length + 1])
		{
			pgpu_link_stats.reply_crc_errors++;
			rx_bit = one + 1;
			continue;
		}
		rx_bit = start + (length + 2) * 32;
		pgpu_deliver_reply (PGPU_HEADER_OP (header), &words[1], length);
	}
}

static bool rx_timer_callback (repeating_timer_t *t)
{
	rx_parse ();
	return true;
}

void pgpu_link_init (void)
{
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
	dma_channel_configure (packet_dma, &c, &pio->txf[sm], NULL, 0, false);

	pio_enable_sm_mask_in_sync (pio, (1u << sm) | (1u << sm_rx));

	add_repeating_timer_us (-1000, rx_timer_callback, NULL, &rx_timer);
}

void pgpu_link_send (const uint32_t *words, uint32_t n)
{
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
	pgpu_link_stats.ready_wait_us += (uint32_t) (time_us_64 () - wait_start);

	/* stop the idle stream, send the batch; its completion restarts idle */
	dma_channel_abort (idle_dma);
	dma_channel_transfer_from_buffer_now (packet_dma, words, n);
}

void pgpu_link_poll (void)
{
	/* the timer parses the replies */
}

uint64_t pgpu_link_time_us (void)
{
	return time_us_64 ();
}

void pgpu_link_settle (void)
{
	sleep_ms (2);			/* the parser runs every millisecond */
}

void pgpu_set_reply_phase (unsigned phase)
{
	pgpu_flush ();			/* and wait for the batch to go out */
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

uint32_t pgpu_frame_count (void)
{
	return frame_count;
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

pgpu_stats_t pgpu_get_stats (void)
{
	uint32_t irq = save_and_disable_interrupts ();
	pgpu_stats_t s = pgpu_link_stats;
	memset (&pgpu_link_stats, 0, sizeof pgpu_link_stats);
	memcpy (pgpu_link_stats.last_error, s.last_error, sizeof pgpu_link_stats.last_error);
	restore_interrupts (irq);
	s.frames = frame_count;
	return s;
}
