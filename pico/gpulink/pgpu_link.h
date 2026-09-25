/*
 * pgpu_link.h - the transport under pgpu.c
 *
 * pgpu.c encodes commands into batches and parses reply packets; a transport
 * moves the words. pgpu_pico.c is the I2S link of the Pico (docs/protocol.md
 * 2-4); host/pgpu_host.c streams the same packets over the Zero's USB, for
 * running pgl on a PC (tests).
 */
#ifndef PGPU_LINK_H
#define PGPU_LINK_H

#include <stdint.h>
#include "pgpu.h"

/* ---- provided by the transport -------------------------------------------------- */

void pgpu_link_init (void);
/* send a batch of whole packets; the words must stay untouched until the next
   call returns (the Pico sends by DMA while the next batch is built) */
void pgpu_link_send (const uint32_t *words, uint32_t n);
/* parse replies received so far (the Pico does this in a timer as well) */
void pgpu_link_poll (void);
uint64_t pgpu_link_time_us (void);
/* return when every reply the Zero has sent so far has been delivered */
void pgpu_link_settle (void);

/* ---- provided by pgpu.c ---------------------------------------------------------- */

/* a reply packet with a good CRC */
void pgpu_deliver_reply (uint8_t opcode, const uint32_t *payload, uint32_t length);
uint32_t pgpu_crc32 (const uint32_t *words, uint32_t n);
extern pgpu_stats_t pgpu_link_stats;	/* counters the transport updates too */

#endif
