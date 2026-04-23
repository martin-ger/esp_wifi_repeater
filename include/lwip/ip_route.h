#ifndef __LWIP_IP_ROUTE_H__
#define __LWIP_IP_ROUTE_H__

#include "lwip/opt.h"
#include "lwip/ip_addr.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MAX_ROUTES 10

struct route_entry {
    ip_addr_t ip;
    ip_addr_t mask;
    ip_addr_t gw;
};

extern struct route_entry ip_rt_table[MAX_ROUTES];
extern int ip_route_max;

/* Add a static route, true on success */
bool ip_add_route(ip_addr_t ip, ip_addr_t mask, ip_addr_t gw);

/* Remove a static route, true on success */
bool ip_rm_route(ip_addr_t ip, ip_addr_t mask);

/* Finds a route entry for an address, NULL if none */
struct route_entry *ip_find_route(ip_addr_t ip);

/* Delete all static routes */
void ip_delete_routes(void);

/* Returns the n_th entry of the routing table, true on success */
bool ip_get_route(uint32_t no, ip_addr_t *ip, ip_addr_t *mask, ip_addr_t *gw);

#ifdef __cplusplus
}
#endif

#endif /* __LWIP_IP_ROUTE_H__ */
