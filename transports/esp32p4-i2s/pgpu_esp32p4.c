/*
 * pgpu_esp32p4.c - the transport of pgpu.c on an ESP32-P4 (pgpu_link.h): the
 * I2S link (docs/protocol.md 2-4), the P4's I2S0 as master in full duplex
 *
 * The TX channel runs forever over its DMA buffers. Batches (sent after
 * READY) go into a ring; each time the DMA has sent a buffer, the "sent"
 * interrupt refills that buffer from the ring and pads it with idle words (0)
 * - it goes out again a lap of the buffers later. Not i2s_channel_write:
 * made for continuous audio, it keeps filling a partly written buffer across
 * calls and takes the oldest sent buffer next, which on a link that idles
 * between batches can be the one the DMA sends at that moment - measured:
 * stray words at the Zero and packets lost without a CRC error.
 * The RX channel, on the same BCLK and WS, receives the reply stream: a task
 * on core 1 copies it into a ring and runs the shared parser (pgpu_rx_parse,
 * pgpu.c). FRAME counts in a GPIO interrupt.
 *
 * Pins (Waveshare ESP32-P4-Module-DEV-KIT, 40-pin header, Pi numbering):
 *
 *	BCLK	GPIO20	pin 13	-> Zero pin 12 (GPIO18, PCM_CLK)
 *	DATA	GPIO21	pin 11	-> Zero pin 38 (GPIO20, PCM_DIN)
 *	FS	GPIO22	pin 12	-> Zero pin 35 (GPIO19, PCM_FS)
 *	REPLY	GPIO23	pin 7	<- Zero pin 40 (GPIO21, PCM_DOUT)
 *	READY	GPIO4	pin 18	<- Zero pin 36 (GPIO16), 10k pull-down to GND
 *	FRAME	GPIO5	pin 16	<- Zero pin 37 (GPIO26)
 *	GND		9, 14, 20, 25	-- Zero 39, 34
 *
 * Bit clock: the I2S driver divides its source by at least 2 to MCLK, and
 * MCLK by 2 (mclk_multiple 128) to BCLK. Chips from revision v3.0 on: the
 * 160 MHz PLL, so up to 40 MHz. Before v3.0 (the Waveshare board's is v1.0),
 * I2S can't use that PLL (i2s_ll_get_clk_src) and runs from the APLL, which
 * the driver sets to 2 x MCLK, at most 125 MHz: up to 31.25 MHz.
 *
 * Flow control: READY promises room for one maximum-size batch (16 KB). What
 * the TX ring and DMA still hold (at most 32 + 4 KB) is on its way on top of
 * that; the Zero drops READY while 128 KB are still free, so that is covered.
 */
#include "pgpu_link.h"
#include <string.h>
#include <unistd.h>
#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "esp_attr.h"
#include "esp_cache.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "sdkconfig.h"
#if CONFIG_ESP32P4_SELECTS_REV_LESS_V3
#define I2S_SOURCE		I2S_CLK_SRC_APLL
#ifndef PGPU_P4_BCLK_HZ
#define PGPU_P4_BCLK_HZ		25000000	/* APLL 100 MHz */
#endif
#else
#define I2S_SOURCE		I2S_CLK_SRC_PLL_160M
#ifndef PGPU_P4_BCLK_HZ
#define PGPU_P4_BCLK_HZ		40000000
#endif
#endif
#define PIN_BCLK		GPIO_NUM_20
#define PIN_DATA		GPIO_NUM_21
#define PIN_FS			GPIO_NUM_22
#define PIN_REPLY		GPIO_NUM_23
#define PIN_READY		GPIO_NUM_4
#define PIN_FRAME		GPIO_NUM_5

/* TX: 8 buffers of 512 bytes (164 us at 25 MHz): a buffer refilled by the
   interrupt goes out 1.3 ms later */
#define DMA_BUFFERS		8
#define DMA_FRAMES		64

/* the commands on their way to the DMA: 32 KB */
#define TX_RING_WORDS		8192

/* the reply stream: 64 KB = 21 ms at 25 MHz, read in chunks of 4 buffers */
#define RX_RING_WORDS		16384
#define RX_CHUNK_WORDS		(DMA_FRAMES * 2 * 4)

#define LOAD(x)			__atomic_load_n (&(x), __ATOMIC_ACQUIRE)
#define STORE(x, v)		__atomic_store_n (&(x), (v), __ATOMIC_RELEASE)

static const char *TAG = "pgpu";

static i2s_chan_handle_t tx_chan, rx_chan;
static uint32_t rx_ring[RX_RING_WORDS];
static pgpu_rx_t rx = {rx_ring, RX_RING_WORDS, 0, 0};
static SemaphoreHandle_t parser_lock;
static volatile uint32_t frame_count;

/* written by pgpu_link_send (head), read by the "sent" interrupt (tail) */
static DRAM_ATTR uint32_t tx_ring[TX_RING_WORDS];
static uint32_t tx_head, tx_tail;

/* the DMA has sent this buffer: refill it with what is queued, then idle */
static bool IRAM_ATTR tx_sent (i2s_chan_handle_t chan, i2s_event_data_t *event, void *ctx)
{
	uint32_t *buf = event->dma_buf;
	uint32_t words = event->size / 4;
	uint32_t tail = tx_tail;
	uint32_t n = LOAD (tx_head) - tail;
	if (n > words)
	{
		n = words;
	}
	for (uint32_t i = 0; i < n; i++)
	{
		buf[i] = tx_ring[(tail + i) & (TX_RING_WORDS - 1)];
	}
	for (uint32_t i = n; i < words; i++)
	{
		buf[i] = 0;
	}
	STORE (tx_tail, tail + n);
	esp_cache_msync (buf, event->size, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
	return false;
}

static void IRAM_ATTR frame_isr (void *arg)
{
	frame_count++;
}

/* the reply stream into the ring, then the parser (under the lock, which
   pgpu_get_stats takes too) */
static void rx_task (void *arg)
{
	static uint32_t chunk[RX_CHUNK_WORDS];
	for (;;)
	{
		size_t got = 0;
		if (i2s_channel_read (rx_chan, chunk, sizeof chunk, &got, portMAX_DELAY) != ESP_OK)
		{
			continue;
		}
		uint32_t n = got / 4;
		uint32_t at = (uint32_t) rx.written & (RX_RING_WORDS - 1);
		uint32_t first = n < RX_RING_WORDS - at ? n : RX_RING_WORDS - at;
		memcpy (&rx_ring[at], chunk, first * 4);
		memcpy (rx_ring, chunk + first, (n - first) * 4);

		xSemaphoreTake (parser_lock, portMAX_DELAY);
		rx.written += n;
		pgpu_rx_parse (&rx);
		xSemaphoreGive (parser_lock);
	}
}

void pgpu_link_init (void)
{
	parser_lock = xSemaphoreCreateMutex ();

	/* READY and FRAME from the Zero (READY has an external 10k pull-down) */
	gpio_config_t in = {
		.pin_bit_mask = 1ull << PIN_READY,
		.mode = GPIO_MODE_INPUT,
		.pull_up_en = GPIO_PULLUP_DISABLE,
		.pull_down_en = GPIO_PULLDOWN_DISABLE,
		.intr_type = GPIO_INTR_DISABLE,
	};
	ESP_ERROR_CHECK (gpio_config (&in));
	in.pin_bit_mask = 1ull << PIN_FRAME;
	in.pull_down_en = GPIO_PULLDOWN_ENABLE;
	in.intr_type = GPIO_INTR_POSEDGE;
	ESP_ERROR_CHECK (gpio_config (&in));
	ESP_ERROR_CHECK (gpio_install_isr_service (0));
	ESP_ERROR_CHECK (gpio_isr_handler_add (PIN_FRAME, frame_isr, NULL));

	/* I2S master, full duplex: TX commands, RX replies on the same clocks */
	i2s_chan_config_t chan = I2S_CHANNEL_DEFAULT_CONFIG (I2S_NUM_0, I2S_ROLE_MASTER);
	chan.dma_desc_num = DMA_BUFFERS;
	chan.dma_frame_num = DMA_FRAMES;
	ESP_ERROR_CHECK (i2s_new_channel (&chan, &tx_chan, &rx_chan));

	/* two 32-bit slots a frame (docs/protocol.md 2.2): BCLK = 64 x the rate,
	   MCLK = 128 x the rate */
	i2s_std_config_t std = {
		.clk_cfg = {
			.sample_rate_hz = PGPU_P4_BCLK_HZ / 64,
			.clk_src = I2S_SOURCE,
			.mclk_multiple = I2S_MCLK_MULTIPLE_128,
		},
		.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG (I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO),
		.gpio_cfg = {
			.mclk = I2S_GPIO_UNUSED,
			.bclk = PIN_BCLK,
			.ws = PIN_FS,
			.dout = PIN_DATA,
			.din = PIN_REPLY,
		},
	};
	ESP_ERROR_CHECK (i2s_channel_init_std_mode (tx_chan, &std));
	ESP_ERROR_CHECK (i2s_channel_init_std_mode (rx_chan, &std));
#ifdef PGPU_P4_DRIVE
	/* the outputs' drive strength (0 weakest .. 3 strongest; ESP-IDF's default
	   is 2): lower rings less on unterminated wires */
	gpio_set_drive_capability (PIN_BCLK, (gpio_drive_cap_t) PGPU_P4_DRIVE);
	gpio_set_drive_capability (PIN_DATA, (gpio_drive_cap_t) PGPU_P4_DRIVE);
	gpio_set_drive_capability (PIN_FS, (gpio_drive_cap_t) PGPU_P4_DRIVE);
#endif
	/* the DMA buffers start as idle words; from then on tx_sent fills them */
	static const uint32_t zeros[DMA_FRAMES * 2];
	for (unsigned i = 0; i < DMA_BUFFERS; i++)
	{
		size_t loaded = 0;
		i2s_channel_preload_data (tx_chan, zeros, sizeof zeros, &loaded);
	}
	i2s_event_callbacks_t callbacks = {.on_sent = tx_sent};
	ESP_ERROR_CHECK (i2s_channel_register_event_callback (tx_chan, &callbacks, NULL));
	ESP_ERROR_CHECK (i2s_channel_enable (rx_chan));
	ESP_ERROR_CHECK (i2s_channel_enable (tx_chan));

	i2s_chan_info_t info;
	if (i2s_channel_get_info (tx_chan, &info) == ESP_OK)
	{
		ESP_LOGI (TAG, "I2S master: BCLK %lu Hz (MCLK %lu Hz from %lu Hz)",
			  (unsigned long) info.bclk_hz, (unsigned long) info.mclk_hz,
			  (unsigned long) info.sclk_hz);
	}

	/* the parser on the other core than the app (app_main runs on core 0) */
	xTaskCreatePinnedToCore (rx_task, "pgpu_rx", 4096, NULL, configMAX_PRIORITIES - 2, NULL, 1);
}

void pgpu_link_send (const uint32_t *words, uint32_t n)
{
	/* READY high: the Zero has room for a maximum-size batch */
	uint64_t wait_start = esp_timer_get_time ();
	while (!gpio_get_level (PIN_READY))
	{
	}
	pgpu_link_stats.ready_wait_us += (uint32_t) (esp_timer_get_time () - wait_start);

	/* into the ring (the interrupt takes it from there): the words are free
	   when this returns */
	uint32_t head = tx_head;
	while (TX_RING_WORDS - (head - LOAD (tx_tail)) < n)
	{
	}
	for (uint32_t i = 0; i < n; i++)
	{
		tx_ring[(head + i) & (TX_RING_WORDS - 1)] = words[i];
	}
	STORE (tx_head, head + n);
}

void pgpu_link_poll (void)
{
	/* the RX task parses the replies */
}

uint64_t pgpu_link_time_us (void)
{
	return (uint64_t) esp_timer_get_time ();
}

void pgpu_link_settle (void)
{
	usleep (3000);			/* a DMA buffer of replies and the parse */
}

void pgpu_link_lock (void)
{
	xSemaphoreTake (parser_lock, portMAX_DELAY);
}

void pgpu_link_unlock (void)
{
	xSemaphoreGive (parser_lock);
}

void pgpu_set_reply_phase (unsigned phase)
{
	/* the I2S receiver samples at the BCLK edge the standard gives */
	(void) phase;
}

bool pgpu_wait_ready (uint32_t timeout_ms)
{
	uint64_t end = esp_timer_get_time () + timeout_ms * 1000ull;
	while (!gpio_get_level (PIN_READY))
	{
		if ((uint64_t) esp_timer_get_time () >= end)
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
	uint64_t end = esp_timer_get_time () + timeout_ms * 1000ull;
	while (frame_count == start)
	{
		if ((uint64_t) esp_timer_get_time () >= end)
		{
			return false;
		}
	}
	return true;
}
