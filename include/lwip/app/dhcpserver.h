#ifndef __DHCPS_H__
#define __DHCPS_H__

#define   dhcps_router_enabled(offer)	((offer & OFFER_ROUTER) != 0)
#include "dhcpserver_common.h"
#define DHCPS_DEBUG          0

extern uint32 dhcps_lease_time;
#define DHCPS_LEASE_TIMER  dhcps_lease_time  //0x05A0

void dhcps_start(struct ip_info *info);
void dhcps_stop(void);

void dhcps_set_DNS(struct ip_addr *dns_ip) ICACHE_FLASH_ATTR;
struct dhcps_pool *dhcps_get_mapping(uint16_t no) ICACHE_FLASH_ATTR;
void dhcps_set_mapping(struct ip_addr *addr, uint8 *mac, uint32 lease_time) ICACHE_FLASH_ATTR;

#endif

