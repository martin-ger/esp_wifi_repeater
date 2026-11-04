#ifndef _BRIDGE_H_
#define _BRIDGE_H_

#ifdef REPEATER_MODE

#include "lwip/netif.h"

/* Called once from user_main.c EVENT_STAMODE_GOT_IP when both netifs are ready.
   Installs input hooks on both netifs, saves linkoutput pointers for forwarding,
   and locks the AP channel to the current STA channel. */
void bridge_init(struct netif *sta_netif, struct netif *ap_netif);
void bridge_show_fdb(void);
/* Number of distinct client MACs with a live FDB entry. */
int bridge_fdb_clients(void);
/* SoftAP association events: keep the station list, drop FDB entries of leavers. */
void bridge_station_join(const uint8_t *mac);
void bridge_station_leave(const uint8_t *mac);
/* Diagnostic counters (see bridge.c). */
extern uint32_t bridge_tx_err_sta, bridge_tx_err_ap, bridge_alloc_fail;

#endif /* REPEATER_MODE */
#endif /* _BRIDGE_H_ */
