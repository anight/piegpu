//
// pcmsrc - streams a known pattern as I2S master (32-bit channels) to the
// RPi's PCM block in slave mode, stepping the bit clock.
//
//   GP16 (pin 21) data  -> RPi pin 38 (PCM_DIN)
//   GP17 (pin 22) BCLK  -> RPi pin 12 (PCM_CLK)
//   GP18 (pin 24) FS    -> RPi pin 35 (PCM_FS)
//   GND  (pin 23)       -- RPi pin 39
//
// Pattern word k (k = 0..8191, repeating): (k * 2654435761) ^ 0x5A5A0001.
// DMA re-reads the 32 KB buffer forever (read ring + self-trigger), no CPU.
//
#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/pio.h"
#include "hardware/dma.h"
#include "hardware/clocks.h"
#include "pcm_out.pio.h"

#define DATA_PIN	16
#define CLOCK_PIN_BASE	17		// BCLK, FS = 18
#define PATTERN_WORDS	8192		// 32 KB, the largest DMA read ring
#define STEP_MS		20000

static uint32_t pattern[PATTERN_WORDS] __attribute__ ((aligned (PATTERN_WORDS * 4)));

// PIO clock dividers: bit clock = sysclk / 2 / div
static const uint dividers[] = {75, 15, 8, 4, 3, 2, 1};

int main (void)
{
	stdio_init_all ();

	for (uint k = 0; k < PATTERN_WORDS; k++)
	{
		pattern[k] = (k * 2654435761u) ^ 0x5A5A0001u;
	}

	PIO pio = pio0;
	uint sm = pio_claim_unused_sm (pio, true);
	uint offset = pio_add_program (pio, &pcm_out_program);
	pcm_out_program_init (pio, sm, offset, DATA_PIN, CLOCK_PIN_BASE);

	// endless DMA: read ring over the pattern, re-trigger itself at the end
	int dma = dma_claim_unused_channel (true);
	dma_channel_config c = dma_channel_get_default_config (dma);
	channel_config_set_transfer_data_size (&c, DMA_SIZE_32);
	channel_config_set_read_increment (&c, true);
	channel_config_set_write_increment (&c, false);
	channel_config_set_ring (&c, false, 15);		// 2^15 = 32 KB read ring
	channel_config_set_dreq (&c, pio_get_dreq (pio, sm, true));
	dma_channel_configure (dma, &c, &pio->txf[sm], pattern,
			       dma_encode_transfer_count_with_self_trigger (PATTERN_WORDS), true);

	uint sysclk = clock_get_hz (clk_sys);
	printf ("\npcmsrc: sysclk %u Hz, pattern %u words, pins data GP%u BCLK GP%u FS GP%u\n",
		sysclk, PATTERN_WORDS, DATA_PIN, CLOCK_PIN_BASE, CLOCK_PIN_BASE + 1);

	for (uint n = 0; ; n++)
	{
		uint div = dividers[n % count_of (dividers)];

		pio_sm_set_enabled (pio, sm, false);
		pio_sm_set_clkdiv_int_frac8 (pio, sm, div, 0);
		pio_sm_clkdiv_restart (pio, sm);
		pio_sm_set_enabled (pio, sm, true);

		uint bclk_khz = sysclk / 2 / div / 1000;
		printf ("pcmsrc: STEP %u: BCLK %u.%03u MHz (div %u), FS %u Hz\n",
			n + 1, bclk_khz / 1000, bclk_khz % 1000, div, sysclk / 2 / div / 64);

		sleep_ms (STEP_MS);
	}
}
