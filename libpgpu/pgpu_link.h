/*
 * pgpu_link.h - the transport under pgpu.c
 *
 * pgpu.c encodes commands into batches and parses reply packets; a transport
 * moves the words. pgpu_pico.c is the I2S link of the Pico (docs/protocol.md
 * 2-4); host/pgpu_host.c streams the same packets over the RPi's USB, for
 * running pgl on a PC (tests).
 *
 * A new board (e.g. an ESP32-P4 as the I2S master) needs only a transport:
 * everything else - pgpu.c, pgl, the HUD, the demos' GL code - is shared. An
 * I2S transport keeps the reply stream in a ring and hands it to the shared
 * parser (pgpu_rx_parse below).
 */
#ifndef PGPU_LINK_H
#define PGPU_LINK_H

#include <stdint.h>
#include "pgpu.h"

/* ---- provided by the transport -------------------------------------------------- */

void pgpu_link_init (void);
/* send a batch of whole packets; the words must stay untouched until the next
   call returns (the Pico sends by DMA while the next batch is built). Before
   sending, the transport waits for READY: the RPi then has room for one
   maximum-size batch, not more (docs/protocol.md 3.1) */
void pgpu_link_send (const uint32_t *words, uint32_t n);
/* parse replies received so far (the Pico does this in a timer as well) */
void pgpu_link_poll (void);
uint64_t pgpu_link_time_us (void);
/* return when every reply the RPi has sent so far has been delivered */
void pgpu_link_settle (void);
/* keep the reply parser out (an interrupt, or another core) while pgpu.c
   copies the counters; not nested */
void pgpu_link_lock (void);
void pgpu_link_unlock (void);

/* and, declared in pgpu.h, the side-band signals (docs/protocol.md 3), which
   a transport without them (USB) answers at once: pgpu_wait_ready,
   pgpu_wait_frame, pgpu_frame_count, pgpu_set_reply_phase */

/* ---- provided by pgpu.c ---------------------------------------------------------- */

/* a reply packet with a good CRC; from one context at a time (the parser) */
void pgpu_deliver_reply (uint8_t opcode, const uint32_t *payload, uint32_t length);
uint32_t pgpu_crc32 (const uint32_t *words, uint32_t n);
extern pgpu_stats_t pgpu_link_stats;	/* counters the transport updates too */

/* The reply stream as raw words (docs/protocol.md 9.1), for a transport that
   receives it into a ring (I2S by DMA). The stream may start at any bit: idle
   words are 0 and a reply header starts with 0x5A, so the first 1 after idle
   is header bit 30, which aligns the packet. A receiver locked to FS gets
   word-aligned packets, which is the same search. */
typedef struct
{
	const uint32_t *ring;		/* as received: bit 31 of word 0 first */
	uint32_t words;			/* ring size, a power of two */
	uint64_t written;		/* words received so far, unwrapped (the transport) */
	uint64_t bit;			/* parse position, bits, unwrapped (the parser) */
} pgpu_rx_t;

/* parse up to rx->written: delivers each reply, counts CRC errors and
   overruns (a ring overtaken skips to the recent half). A transport with a
   data cache makes the new words visible first. */
void pgpu_rx_parse (pgpu_rx_t *rx);

#endif
