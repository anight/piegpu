/*
 * portal.h - the page a phone gets from the Pico while it's set up: a form
 * for the Wi-Fi network's name and its password. Every other URL is sent on
 * to it, which is what makes a phone that has just joined offer the page by
 * itself (its test for a way out gets our answer). lwIP's raw API: call
 * with the lwIP lock held; what the form sent is taken with portal_take.
 */
#ifndef PORTAL_H
#define PORTAL_H

#include <stdbool.h>
#include "lwip/ip4_addr.h"

#define PORTAL_SSID_MAX		32
#define PORTAL_PASSWORD_MAX	63
#define PORTAL_NETWORKS		16

/* the networks to offer (their names: found by a scan), and a line to say
   over the form (why the last try failed; "": none). Kept by the caller */
void portal_start (const ip4_addr_t *address, const char (*networks)[PORTAL_SSID_MAX + 1], unsigned count,
		   const char *note);
void portal_stop (void);
/* the form came: its fields (once) */
bool portal_take (char ssid[PORTAL_SSID_MAX + 1], char password[PORTAL_PASSWORD_MAX + 1]);
unsigned portal_requests (void);	/* served so far */

#endif
