/*
 * wifi_store.h - the Wi-Fi network the Pico joins, kept in its flash (one
 * sector, the third from the end: BTstack keeps its keys in the last two).
 * As it is: the name and the password are readable by whoever reads the
 * flash.
 */
#ifndef WIFI_STORE_H
#define WIFI_STORE_H

#include <stdbool.h>

typedef struct
{
	char ssid[33];
	char password[64];
} wifi_network_t;

bool wifi_store_load (wifi_network_t *n);		/* false: none kept */
bool wifi_store_save (const wifi_network_t *n);
bool wifi_store_clear (void);

#endif
