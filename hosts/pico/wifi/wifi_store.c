/*
 * wifi_store.c - see wifi_store.h
 */
#include "wifi_store.h"
#include <string.h>
#include "hardware/flash.h"
#include "pico/flash.h"
#include "pico/stdlib.h"

#define STORE_OFFSET	(PICO_FLASH_SIZE_BYTES - 3 * FLASH_SECTOR_SIZE)
#define STORE_MAGIC	0x46574750u		/* "PGWF" */

typedef struct
{
	uint32_t magic;
	wifi_network_t network;
	uint32_t sum;				/* of the network's bytes */
} record_t;

static uint32_t sum_of (const wifi_network_t *n)
{
	uint32_t sum = 2166136261u;		/* FNV-1a */
	for (unsigned i = 0; i < sizeof *n; i++)
	{
		sum = (sum ^ ((const uint8_t *) n)[i]) * 16777619u;
	}
	return sum;
}

bool wifi_store_load (wifi_network_t *n)
{
	const record_t *r = (const record_t *) (XIP_BASE + STORE_OFFSET);
	if (r->magic != STORE_MAGIC || r->sum != sum_of (&r->network) || !r->network.ssid[0])
	{
		return false;
	}
	*n = r->network;
	n->ssid[sizeof n->ssid - 1] = n->password[sizeof n->password - 1] = '\0';
	return true;
}

/* with the flash out of use (interrupts off: tens of milliseconds) */
static void write_sector (void *page)
{
	flash_range_erase (STORE_OFFSET, FLASH_SECTOR_SIZE);
	if (page)
	{
		flash_range_program (STORE_OFFSET, page, FLASH_PAGE_SIZE);
	}
}

bool wifi_store_save (const wifi_network_t *n)
{
	static uint8_t page[FLASH_PAGE_SIZE];
	record_t r;
	memset (&r, 0, sizeof r);
	r.magic = STORE_MAGIC;
	strncpy (r.network.ssid, n->ssid, sizeof r.network.ssid - 1);
	strncpy (r.network.password, n->password, sizeof r.network.password - 1);
	r.sum = sum_of (&r.network);
	memset (page, 0xFF, sizeof page);
	memcpy (page, &r, sizeof r);
	return flash_safe_execute (write_sector, page, 500) == PICO_OK;
}

bool wifi_store_clear (void)
{
	return flash_safe_execute (write_sector, NULL, 500) == PICO_OK;
}
