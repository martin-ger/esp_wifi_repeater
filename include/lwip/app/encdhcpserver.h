#ifndef __enc_DHCPS_H__
#define __enc_DHCPS_H__

#define   enc_dhcps_router_enabled(offer)	((offer & OFFER_ROUTER) != 0)
#include "dhcpserver_common.h"

extern uint32 enc_dhcps_lease_time;
#define enc_DHCPS_LEASE_TIMER  enc_dhcps_lease_time  //0x05A0
#define ENCDHCPS_DEBUG          0


void enc_dhcps_start(struct netif* enetif);
void enc_dhcps_stop(void);

void enc_dhcps_set_DNS(struct ip_addr *dns_ip) ICACHE_FLASH_ATTR;
struct dhcps_pool *enc_dhcps_get_mapping(uint16_t no) ICACHE_FLASH_ATTR;
void enc_dhcps_set_mapping(struct ip_addr *addr, uint8 *mac, uint32 lease_time) ICACHE_FLASH_ATTR;

#endif

