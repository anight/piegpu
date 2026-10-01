/*
 * net_servers.h - what a phone needs from the access point the Pico is
 * while it's set up: an address (a small DHCP server), and every name it
 * asks for answered with ours (a DNS that knows one address), so that the
 * phone's test for a way out lands on our page and it offers it (a captive
 * portal). lwIP's raw API: call with the lwIP lock held.
 */
#ifndef NET_SERVERS_H
#define NET_SERVERS_H

#include "lwip/ip4_addr.h"
#include "lwip/netif.h"

/* addresses .16 on to up to 8 phones; our address as their router and DNS,
   and (option 114) the page's URL for those that take it from there */
void dhcpd_start (struct netif *netif, const ip4_addr_t *address);
void dhcpd_stop (void);
unsigned dhcpd_leases (void);		/* given so far */

void dns_start (struct netif *netif, const ip4_addr_t *address);
void dns_stop (void);
unsigned dns_queries (void);		/* answered so far */

#endif
