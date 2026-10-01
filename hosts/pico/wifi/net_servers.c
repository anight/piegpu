/*
 * net_servers.c - see net_servers.h
 */
#include "net_servers.h"
#include <stdio.h>
#include <string.h>
#include "lwip/pbuf.h"
#include "lwip/udp.h"

/* ---- DHCP (RFC 2131): DISCOVER -> OFFER, REQUEST -> ACK ----------------------------------- */

#define DHCP_SERVER_PORT	67
#define DHCP_CLIENT_PORT	68
#define DHCP_DISCOVER		1
#define DHCP_OFFER		2
#define DHCP_REQUEST		3
#define DHCP_ACK		5
#define DHCP_NAK		6
#define DHCP_OPTIONS		240		/* after the fixed part and the magic */
#define DHCP_LEASES		8
#define DHCP_FIRST		16		/* the last byte of the first address given */
#define DHCP_LEASE_S		3600

static struct udp_pcb *dhcp_pcb;
static struct netif *dhcp_netif;
static ip4_addr_t dhcp_address;
static uint8_t dhcp_mac[DHCP_LEASES][6];
static unsigned dhcp_used, dhcp_next, dhcp_given;
static char dhcp_url[32];

static const uint8_t *dhcp_option (const uint8_t *msg, unsigned len, uint8_t code)
{
	for (unsigned i = DHCP_OPTIONS; i + 1 < len && msg[i] != 255; )
	{
		if (msg[i] == 0)
		{
			i++;
			continue;
		}
		if (msg[i] == code && i + 2 + msg[i + 1] <= len)
		{
			return &msg[i];
		}
		i += 2 + msg[i + 1];
	}
	return NULL;
}

/* the address's last byte for this phone: the one it had, or the next (the oldest goes) */
static unsigned dhcp_lease (const uint8_t *mac)
{
	for (unsigned i = 0; i < dhcp_used; i++)
	{
		if (memcmp (dhcp_mac[i], mac, 6) == 0)
		{
			return DHCP_FIRST + i;
		}
	}
	unsigned i = dhcp_used < DHCP_LEASES ? dhcp_used++ : dhcp_next++ % DHCP_LEASES;
	memcpy (dhcp_mac[i], mac, 6);
	return DHCP_FIRST + i;
}

static void dhcp_recv (void *arg, struct udp_pcb *pcb, struct pbuf *p, const ip_addr_t *from, u16_t port)
{
	static uint8_t msg[600];
	unsigned len = p->tot_len < sizeof msg ? p->tot_len : sizeof msg;
	pbuf_copy_partial (p, msg, len, 0);
	pbuf_free (p);
	static const uint8_t magic[4] = {99, 130, 83, 99};
	const uint8_t *type = len > DHCP_OPTIONS ? dhcp_option (msg, len, 53) : NULL;
	if (!type || msg[0] != 1 || memcmp (&msg[236], magic, 4) != 0)
	{
		return;
	}
	uint8_t reply = type[2] == DHCP_DISCOVER ? DHCP_OFFER : type[2] == DHCP_REQUEST ? DHCP_ACK : 0;
	if (!reply)
	{
		return;
	}
	uint32_t ours = ip4_addr_get_u32 (&dhcp_address);		/* (network order: bytes as on the wire) */
	uint8_t yours[4];
	memcpy (yours, &ours, 4);
	yours[3] = (uint8_t) dhcp_lease (&msg[28]);
	const uint8_t *asked = dhcp_option (msg, len, 50);		/* a REQUEST for another address: no */
	const uint8_t *server = dhcp_option (msg, len, 54);
	if (reply == DHCP_ACK && (   (asked && memcmp (&asked[2], yours, 4) != 0)
				  || (server && memcmp (&server[2], &ours, 4) != 0)
				  || (!asked && memcmp (&msg[12], yours, 4) != 0)))
	{
		reply = DHCP_NAK;
	}

	msg[0] = 2;							/* a reply: the rest of the fixed part as it came */
	memset (&msg[12], 0, 12);
	if (reply != DHCP_NAK)
	{
		memcpy (&msg[16], yours, 4);
		memcpy (&msg[20], &ours, 4);
	}
	memset (&msg[44], 0, 192);
	uint8_t *o = &msg[DHCP_OPTIONS];
	*o++ = 53; *o++ = 1; *o++ = reply;
	*o++ = 54; *o++ = 4; memcpy (o, &ours, 4); o += 4;
	if (reply != DHCP_NAK)
	{
		*o++ = 51; *o++ = 4; *o++ = 0; *o++ = 0; *o++ = (uint8_t) (DHCP_LEASE_S >> 8); *o++ = (uint8_t) DHCP_LEASE_S;
		*o++ = 1; *o++ = 4; *o++ = 255; *o++ = 255; *o++ = 255; *o++ = 0;
		*o++ = 3; *o++ = 4; memcpy (o, &ours, 4); o += 4;
		*o++ = 6; *o++ = 4; memcpy (o, &ours, 4); o += 4;
		unsigned n = (unsigned) strlen (dhcp_url);		/* RFC 8910: where the portal is */
		*o++ = 114; *o++ = (uint8_t) n; memcpy (o, dhcp_url, n); o += n;
	}
	*o++ = 255;
	unsigned out = (unsigned) (o - msg);
	out = out < 300 ? 300 : out;					/* (BOOTP's least) */
	memset (o, 0, &msg[out] - o);

	struct pbuf *q = pbuf_alloc (PBUF_TRANSPORT, (u16_t) out, PBUF_RAM);
	if (!q)
	{
		return;
	}
	memcpy (q->payload, msg, out);
	udp_sendto_if (pcb, q, IP_ADDR_BROADCAST, DHCP_CLIENT_PORT, dhcp_netif);
	pbuf_free (q);
	if (reply == DHCP_ACK)
	{
		dhcp_given++;
		printf ("wifi: a phone got 192.168.%u.%u\n", yours[2], yours[3]);
	}
}

void dhcpd_start (struct netif *netif, const ip4_addr_t *address)
{
	dhcpd_stop ();
	dhcp_netif = netif;
	dhcp_address = *address;
	dhcp_used = dhcp_next = dhcp_given = 0;
	snprintf (dhcp_url, sizeof dhcp_url, "http://%s/", ip4addr_ntoa (address));
	dhcp_pcb = udp_new ();
	if (dhcp_pcb)
	{
		ip_set_option (dhcp_pcb, SOF_BROADCAST);
		udp_bind (dhcp_pcb, IP_ANY_TYPE, DHCP_SERVER_PORT);
		udp_bind_netif (dhcp_pcb, netif);
		udp_recv (dhcp_pcb, dhcp_recv, NULL);
	}
}

void dhcpd_stop (void)
{
	if (dhcp_pcb)
	{
		udp_remove (dhcp_pcb);
		dhcp_pcb = NULL;
	}
}

unsigned dhcpd_leases (void)	{ return dhcp_given; }

/* ---- DNS: every name is us ---------------------------------------------------------------- */

static struct udp_pcb *dns_pcb;
static ip4_addr_t dns_address;
static unsigned dns_answered;

static void dns_recv (void *arg, struct udp_pcb *pcb, struct pbuf *p, const ip_addr_t *from, u16_t port)
{
	static uint8_t msg[300];
	unsigned len = p->tot_len < 256 ? p->tot_len : 256;
	pbuf_copy_partial (p, msg, len, 0);
	pbuf_free (p);
	if (len < 17 || (msg[2] & 0x80) || msg[4] != 0 || msg[5] != 1)		/* one question */
	{
		return;
	}
	unsigned i = 12;
	while (i < len && msg[i] != 0 && msg[i] < 64)				/* its name's labels */
	{
		i += 1 + msg[i];
	}
	if (i + 5 > len || msg[i] != 0)
	{
		return;
	}
	bool a = msg[i + 1] == 0 && msg[i + 2] == 1 && msg[i + 3] == 0 && msg[i + 4] == 1;	/* A, IN */
	unsigned out = i + 5;
	msg[2] = (uint8_t) (0x84 | (msg[2] & 0x01));				/* a response, ours to give */
	msg[3] = 0x80;
	msg[6] = 0; msg[7] = a ? 1 : 0;						/* (another type: nothing, no error) */
	msg[8] = msg[9] = msg[10] = msg[11] = 0;
	if (a)
	{
		static const uint8_t answer[12] = {0xC0, 0x0C, 0, 1, 0, 1, 0, 0, 0, 30, 0, 4};
		memcpy (&msg[out], answer, sizeof answer);
		uint32_t ours = ip4_addr_get_u32 (&dns_address);
		memcpy (&msg[out + 12], &ours, 4);
		out += 16;
	}
	struct pbuf *q = pbuf_alloc (PBUF_TRANSPORT, (u16_t) out, PBUF_RAM);
	if (!q)
	{
		return;
	}
	memcpy (q->payload, msg, out);
	udp_sendto (pcb, q, from, port);
	pbuf_free (q);
	dns_answered++;
}

void dns_start (struct netif *netif, const ip4_addr_t *address)
{
	dns_stop ();
	dns_address = *address;
	dns_answered = 0;
	dns_pcb = udp_new ();
	if (dns_pcb)
	{
		udp_bind (dns_pcb, IP_ANY_TYPE, 53);
		udp_bind_netif (dns_pcb, netif);
		udp_recv (dns_pcb, dns_recv, NULL);
	}
}

void dns_stop (void)
{
	if (dns_pcb)
	{
		udp_remove (dns_pcb);
		dns_pcb = NULL;
	}
}

unsigned dns_queries (void)	{ return dns_answered; }
