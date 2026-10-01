/*
 * wifi_setup.c - see wifi_setup.h
 */
#include "wifi_setup.h"
#include <stdio.h>
#include <string.h>
#include "pico/cyw43_arch.h"
#include "pico/rand.h"
#include "pico/stdlib.h"
#include "lwip/apps/sntp.h"
#include "lwip/netif.h"
#include "net_servers.h"
#include "portal.h"
#include "wifi_store.h"

#define LOOK_US		10000000	/* a scan at most */
#define JOIN_US		30000000	/* a try to join at most */
#define ANSWER_US	1500000		/* the form's answer is on its way to the phone: then we leave */
#define RETRY_US	60000000	/* the network kept, tried again while no phone is here */
#define CHECK_US	1000000		/* the phones here, the signal: how often they're looked at */

static wifi_status_t st;
static wifi_network_t wanted;		/* being joined */
static bool from_form;			/* it came from the form (else it's the one kept) */
static bool no_kept;			/* don't join the one kept (set-up asked for) */
static bool sntp_on;
static uint64_t since, checked, leave_at;
static volatile uint32_t net_time;	/* from the net: seconds since 1970 ... */
static volatile uint64_t net_time_us;	/* ... at this time_us_64 () */

/* the networks found, the strongest first */
static char networks[PORTAL_NETWORKS][PORTAL_SSID_MAX + 1];
static int strength[PORTAL_NETWORKS];
static volatile unsigned n_networks;

static void set_state (wifi_state_t state)
{
	st.state = state;
	since = checked = time_us_64 ();
}

/* lwIP's, when its answer came (lwipopts.h) */
void wifi_setup_time (unsigned long seconds)
{
	net_time_us = time_us_64 ();
	net_time = (uint32_t) seconds;
}

/* a network heard (the driver's context): kept if it's new and among the strongest */
static int scan_result (void *env, const cyw43_ev_scan_result_t *r)
{
	char name[PORTAL_SSID_MAX + 1];
	unsigned len = r->ssid_len < PORTAL_SSID_MAX ? r->ssid_len : PORTAL_SSID_MAX;
	memcpy (name, r->ssid, len);
	name[len] = '\0';
	if (!len || strlen (name) != len)		/* (hidden, or not text) */
	{
		return 0;
	}
	unsigned n = n_networks, weakest = 0;
	for (unsigned i = 0; i < n; i++)
	{
		if (strcmp (networks[i], name) == 0)
		{
			strength[i] = r->rssi > strength[i] ? r->rssi : strength[i];
			return 0;
		}
		weakest = strength[i] < strength[weakest] ? i : weakest;
	}
	unsigned at = n < PORTAL_NETWORKS ? n : weakest;
	if (n == PORTAL_NETWORKS && r->rssi <= strength[weakest])
	{
		return 0;
	}
	strcpy (networks[at], name);
	strength[at] = r->rssi;
	n_networks = n < PORTAL_NETWORKS ? n + 1 : n;
	return 0;
}

static void sort_networks (void)
{
	for (unsigned i = 1; i < n_networks; i++)
	{
		for (unsigned j = i; j > 0 && strength[j] > strength[j - 1]; j--)
		{
			char name[PORTAL_SSID_MAX + 1];
			int s = strength[j];
			strcpy (name, networks[j]);
			strcpy (networks[j], networks[j - 1]);
			strength[j] = strength[j - 1];
			strcpy (networks[j - 1], name);
			strength[j - 1] = s;
		}
	}
	st.networks = n_networks;
}

static void join (bool form)
{
	from_form = form;
	strncpy (st.ssid, wanted.ssid, sizeof st.ssid - 1);
	printf ("wifi: joining \"%s\" (%s, a password of %u characters)\n", wanted.ssid,
		form ? "from the form" : "the one kept", (unsigned) strlen (wanted.password));
	cyw43_arch_wifi_connect_async (wanted.ssid, wanted.password[0] ? wanted.password : NULL,
				       wanted.password[0] ? CYW43_AUTH_WPA2_MIXED_PSK : CYW43_AUTH_OPEN);
	set_state (WIFI_JOINING);
}

static void start_ap (void)
{
	if (!st.ap_ssid[0])				/* made up once: the same till the power goes */
	{
		static const char letters[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
		strcpy (st.ap_ssid, "PIEGPU-");
		for (unsigned i = 0; i < 4; i++)
		{
			st.ap_ssid[7 + i] = letters[get_rand_32 () % 32];
		}
		for (unsigned i = 0; i < 10; i++)
		{
			st.ap_password[i] = letters[get_rand_32 () % 32];
		}
	}
	cyw43_arch_enable_ap_mode (st.ap_ssid, st.ap_password, CYW43_AUTH_WPA2_AES_PSK);
	struct netif *netif = &cyw43_state.netif[CYW43_ITF_AP];
	cyw43_arch_lwip_begin ();
	dhcpd_start (netif, netif_ip4_addr (netif));
	dns_start (netif, netif_ip4_addr (netif));
	portal_start (netif_ip4_addr (netif), networks, n_networks, st.note);
	cyw43_arch_lwip_end ();
	printf ("wifi: the access point \"%s\" is up, the page at %s\n", st.ap_ssid, wifi_setup_url ());
	set_state (WIFI_AP);
}

static void stop_ap (void)
{
	cyw43_arch_lwip_begin ();
	portal_stop ();
	dns_stop ();
	dhcpd_stop ();
	cyw43_arch_lwip_end ();
	cyw43_arch_disable_ap_mode ();
}

/* the try failed: say why, and back to the access point */
static void failed (const char *why)
{
	snprintf (st.note, sizeof st.note, "%.32s: %s", wanted.ssid, why);
	printf ("wifi: not joined: %s\n", st.note);
	cyw43_wifi_leave (&cyw43_state, CYW43_ITF_STA);
	memset (wanted.password, 0, sizeof wanted.password);
	start_ap ();
}

bool wifi_setup_start (bool setup)
{
	memset (&st, 0, sizeof st);
	no_kept = setup;
	if (cyw43_arch_init ())
	{
		printf ("wifi: no radio\n");
		set_state (WIFI_FAILED);
		return false;
	}
	cyw43_arch_enable_sta_mode ();
	cyw43_wifi_scan_options_t options = {0};
	n_networks = 0;
	if (cyw43_wifi_scan (&cyw43_state, &options, NULL, scan_result) != 0)
	{
		printf ("wifi: the scan didn't start\n");
	}
	set_state (WIFI_LOOKING);
	return true;
}

void wifi_setup_again (void)
{
	if (st.state == WIFI_OFF || st.state == WIFI_FAILED || st.state == WIFI_LOOKING)
	{
		no_kept = true;
		return;
	}
	wifi_store_clear ();
	no_kept = true;
	st.note[0] = '\0';
	if (st.state == WIFI_ONLINE || st.state == WIFI_JOINING)
	{
		cyw43_wifi_leave (&cyw43_state, CYW43_ITF_STA);
		start_ap ();
	}
}

void wifi_setup_poll (void)
{
	uint64_t now = time_us_64 ();
	switch (st.state)
	{
	case WIFI_LOOKING:
		if (!cyw43_wifi_scan_active (&cyw43_state) || now - since > LOOK_US)
		{
			sort_networks ();
			printf ("wifi: %u networks around\n", st.networks);
			if (!no_kept && wifi_store_load (&wanted))
			{
				join (false);
			}
			else
			{
				start_ap ();
			}
		}
		break;

	case WIFI_JOINING:
	{
		int link = cyw43_tcpip_link_status (&cyw43_state, CYW43_ITF_STA);
		if (link == CYW43_LINK_UP)
		{
			const ip4_addr_t *ip = netif_ip4_addr (&cyw43_state.netif[CYW43_ITF_STA]);
			memcpy (st.ip, &ip->addr, 4);
			st.note[0] = '\0';
			printf ("wifi: joined \"%s\": %u.%u.%u.%u\n", wanted.ssid, st.ip[0], st.ip[1], st.ip[2], st.ip[3]);
			if (from_form)
			{
				printf ("wifi: the network is %s\n", wifi_store_save (&wanted) ? "kept in the flash" : "NOT kept: the flash didn't take it");
			}
			memset (wanted.password, 0, sizeof wanted.password);
			if (!sntp_on)
			{
				cyw43_arch_lwip_begin ();
				sntp_setoperatingmode (SNTP_OPMODE_POLL);
				sntp_setservername (0, "pool.ntp.org");
				sntp_init ();
				cyw43_arch_lwip_end ();
				sntp_on = true;
			}
			set_state (WIFI_ONLINE);
		}
		else if (link == CYW43_LINK_BADAUTH)
		{
			failed ("wrong password");
		}
		else if (link == CYW43_LINK_NONET)
		{
			failed ("no such network in reach");
		}
		else if (link == CYW43_LINK_FAIL)
		{
			failed ("it didn't let us in");
		}
		else if (now - since > JOIN_US)
		{
			failed ("no answer in 30 s");
		}
		break;
	}

	case WIFI_AP:
	case WIFI_AP_PHONE:
		if (leave_at)					/* the form came: its answer is going out */
		{
			if (now >= leave_at)
			{
				leave_at = 0;
				stop_ap ();
				join (true);
			}
			break;
		}
		if (portal_take (wanted.ssid, wanted.password))
		{
			leave_at = now + ANSWER_US;
			break;
		}
		if (now - checked >= CHECK_US)
		{
			checked = now;
			uint8_t macs[6 * 8];
			int phones = 8;
			cyw43_wifi_ap_get_stas (&cyw43_state, &phones, macs);
			if ((phones > 0) != (st.state == WIFI_AP_PHONE))
			{
				printf ("wifi: %s\n", phones > 0 ? "a phone joined the access point" : "the phone left");
				st.state = phones > 0 ? WIFI_AP_PHONE : WIFI_AP;
				since = now;
			}
		}
		/* nobody here: the network kept may be back (a router switched on later) */
		if (st.state == WIFI_AP && !no_kept && now - since > RETRY_US && wifi_store_load (&wanted))
		{
			stop_ap ();
			join (false);
		}
		break;

	case WIFI_ONLINE:
		if (now - checked >= CHECK_US)
		{
			checked = now;
			int32_t rssi = 0;
			st.rssi = cyw43_wifi_get_rssi (&cyw43_state, &rssi) == 0 ? (int) rssi : 0;
			uint32_t at = net_time;
			st.time = at ? at + (uint32_t) ((now - net_time_us) / 1000000) : 0;
			if (cyw43_tcpip_link_status (&cyw43_state, CYW43_ITF_STA) != CYW43_LINK_UP && wifi_store_load (&wanted))
			{
				printf ("wifi: the network is gone: joining it again\n");
				join (false);
			}
		}
		break;

	default:
		break;
	}
}

const wifi_status_t *wifi_setup_status (void)
{
	return &st;
}

const char *wifi_setup_url (void)
{
	static char url[32];
	snprintf (url, sizeof url, "http://%s/", ip4addr_ntoa (netif_ip4_addr (&cyw43_state.netif[CYW43_ITF_AP])));
	return url;
}

bool wifi_setup_qr (char *text, unsigned max)
{
	if (st.state == WIFI_AP)
	{
		snprintf (text, max, "WIFI:T:WPA;S:%s;P:%s;;", st.ap_ssid, st.ap_password);
		return true;
	}
	if (st.state == WIFI_AP_PHONE)
	{
		snprintf (text, max, "%s", wifi_setup_url ());
		return true;
	}
	return false;
}
