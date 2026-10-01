/*
 * wifi_setup.h - Wi-Fi for the Pico W without a keyboard: asked of a phone.
 *
 * With a network kept (wifi_store) it's joined. Without one, or when that
 * one can't be joined, the Pico becomes an access point with a name and a
 * password made up on the spot, to be shown as a QR code (wifi_setup_qr: a
 * phone's camera offers to join); a phone that joined is offered the page
 * with the form by itself (net_servers, portal), or opens it by its address
 * (a second QR code). What the form sent is tried: joined, it's kept and the
 * access point is gone; not joined, the access point is back, saying why.
 *
 * wifi_setup_poll moves it on: call it often (every frame). The radio and
 * lwIP run in the CYW43 driver's background context.
 */
#ifndef WIFI_SETUP_H
#define WIFI_SETUP_H

#include <stdbool.h>
#include <stdint.h>

typedef enum
{
	WIFI_OFF,
	WIFI_LOOKING,			/* for the networks around (the form offers them) */
	WIFI_JOINING,			/* the network kept, or the one the form sent */
	WIFI_AP,			/* the access point: waiting for a phone */
	WIFI_AP_PHONE,			/* a phone is on it: the page */
	WIFI_ONLINE,
	WIFI_FAILED			/* no radio */
} wifi_state_t;

typedef struct
{
	wifi_state_t state;
	char ap_ssid[20], ap_password[16];	/* the access point's (capitals and digits) */
	char ssid[33];				/* the network joined, or being joined */
	char note[64];				/* why the last try failed ("": it didn't) */
	uint8_t ip[4];				/* online: our address */
	int rssi;				/* online: the signal, dBm (0: not known) */
	uint32_t time;				/* online: seconds since 1970, from the net (0: not yet) */
	unsigned networks;			/* found around */
} wifi_status_t;

/* setup: don't join the network kept, go straight to the access point */
bool wifi_setup_start (bool setup);
void wifi_setup_poll (void);
const wifi_status_t *wifi_setup_status (void);
/* forget the network kept and start the access point */
void wifi_setup_again (void);

/* what to show as a QR code now: the access point's "WIFI:..." while no phone
   is on it, the page's address while one is; false: nothing */
bool wifi_setup_qr (char *text, unsigned max);
/* the page's address, as text */
const char *wifi_setup_url (void);

#endif
