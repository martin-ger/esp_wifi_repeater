#ifndef _BRIDGE_H_
#define _BRIDGE_H_

#ifdef REPEATER_MODE

#include "lwip/netif.h"

/* Called once from user_main.c EVENT_STAMODE_GOT_IP when both netifs are ready.
   Installs input hooks on both netifs, saves linkoutput pointers for forwarding,
   and locks the AP channel to the current STA channel. */
void bridge_init(struct netif *sta_netif, struct netif *ap_netif);

#endif /* REPEATER_MODE */
#endif /* _BRIDGE_H_ */
