#include "user_config.h"

#ifdef REPEATER_MODE

#include "bridge.h"
#include "c_types.h"
#include "lwip/def.h"
#include "lwip/netif.h"
#include "lwip/pbuf.h"
#include "lwip/err.h"
#include "osapi.h"
#include "user_interface.h"
#include "sys_time.h"
#include "config_flash.h"

extern sysconfig_t config;

/* my_channel is set by wifi event handler in user_main.c on STAMODE_CONNECTED */
extern uint8_t my_channel;
extern uint64_t Bytes_in, Bytes_out;
extern uint32_t Packets_in, Packets_out;
extern uint64_t Bytes_per_day;

/* -------------------------------------------------------------------------
 * Netif state
 * ------------------------------------------------------------------------- */

static struct netif        *s_sta_nif;
static struct netif        *s_ap_nif;
static netif_input_fn       s_orig_input_sta;
static netif_input_fn       s_orig_input_ap;
static netif_output_fn      s_orig_output_sta;
static netif_output_fn      s_orig_output_ap;
static netif_linkoutput_fn  s_orig_lo_sta;
static netif_linkoutput_fn  s_orig_lo_ap;
static os_timer_t           s_arp_timer;

/* -------------------------------------------------------------------------
 * Compact packed header types
 * ------------------------------------------------------------------------- */

#define ETHTYPE_IP  0x0800
#define ETHTYPE_ARP 0x0806

typedef struct {
    uint8_t  dst[6];
    uint8_t  src[6];
    uint16_t type;
} __attribute__((packed)) eth_hdr_t;

typedef struct {
    uint16_t hwtype;
    uint16_t prtype;
    uint8_t  hwlen;
    uint8_t  prlen;
    uint16_t op;
    uint8_t  sha[6];
    uint32_t spa;
    uint8_t  tha[6];
    uint32_t tpa;
} __attribute__((packed)) arp_hdr_t;

typedef struct {
    uint8_t  vhl;
    uint8_t  tos;
    uint16_t len;
    uint16_t id;
    uint16_t off;
    uint8_t  ttl;
    uint8_t  proto;
    uint16_t chksum;
    uint32_t src;
    uint32_t dst;
} __attribute__((packed)) ip_hdr_t;

typedef struct {
    uint16_t src_port;
    uint16_t dst_port;
    uint16_t len;
    uint16_t chksum;
} __attribute__((packed)) udp_hdr_t;

#define DHCP_OP_REQUEST  1
#define DHCP_OP_REPLY    2
#define DHCP_MSG_DISCOVER 1
#define DHCP_MSG_REQUEST  3
#define DHCP_MSG_ACK      5
#define DHCP_MAGIC_COOKIE 0x63825363UL

typedef struct {
    uint8_t  op;
    uint8_t  htype;
    uint8_t  hlen;
    uint8_t  hops;
    uint32_t xid;
    uint16_t secs;
    uint16_t flags;
    uint32_t ciaddr;
    uint32_t yiaddr;
    uint32_t siaddr;
    uint32_t giaddr;
    uint8_t  chaddr[16];
    uint8_t  sname[64];
    uint8_t  file[128];
    uint32_t magic;
    uint8_t  options[0];
} __attribute__((packed)) dhcp_msg_t;

/* -------------------------------------------------------------------------
 * Time helper
 * ------------------------------------------------------------------------- */

static uint32_t ICACHE_FLASH_ATTR now_secs(void)
{
    return (uint32_t)(get_long_systime() / 1000000ULL);
}

/* -------------------------------------------------------------------------
 * Forwarding Database (FDB)
 * ------------------------------------------------------------------------- */

#define FDB_SIZE   16
#define FDB_TTL_S 600

typedef struct {
    uint32_t ip;
    uint8_t  mac[6];
    uint32_t expires_s;
} fdb_entry_t;

static fdb_entry_t s_fdb[FDB_SIZE];

static void ICACHE_FLASH_ATTR fdb_insert(uint32_t ip, const uint8_t *mac)
{
    if (ip == 0) return;
    uint32_t now = now_secs();
    int free_idx = -1, oldest_idx = 0;
    uint32_t oldest_exp = 0xFFFFFFFFUL;
    int i;
    for (i = 0; i < FDB_SIZE; i++) {
        if (s_fdb[i].ip == ip) {
            os_memcpy(s_fdb[i].mac, mac, 6); s_fdb[i].expires_s = now + FDB_TTL_S;
            return;
        }
        if (s_fdb[i].ip == 0 || s_fdb[i].expires_s <= now) { if (free_idx < 0) free_idx = i; }
        if (s_fdb[i].expires_s < oldest_exp) { oldest_exp = s_fdb[i].expires_s; oldest_idx = i; }
    }
    int idx = (free_idx >= 0) ? free_idx : oldest_idx;
    s_fdb[idx].ip = ip; os_memcpy(s_fdb[idx].mac, mac, 6); s_fdb[idx].expires_s = now + FDB_TTL_S;
}

static const uint8_t * ICACHE_FLASH_ATTR fdb_lookup(uint32_t ip)
{
    uint32_t now = now_secs();
    int i;
    for (i = 0; i < FDB_SIZE; i++) {
        if (s_fdb[i].ip == ip && s_fdb[i].expires_s > now) return s_fdb[i].mac;
    }
    return NULL;
}

/* -------------------------------------------------------------------------
 * DHCP XID map
 * ------------------------------------------------------------------------- */

#define XID_MAP_SIZE  8
#define XID_TTL_S    30

typedef struct {
    uint32_t xid;
    uint8_t  chaddr[6];
    uint32_t expires_s;
} xid_entry_t;

static xid_entry_t s_xid_map[XID_MAP_SIZE];

static void ICACHE_FLASH_ATTR xid_map_insert(uint32_t xid, const uint8_t *chaddr)
{
    uint32_t now = now_secs();
    int free_idx = -1, oldest_idx = 0;
    uint32_t oldest_exp = 0xFFFFFFFFUL;
    int i;
    for (i = 0; i < XID_MAP_SIZE; i++) {
        if (s_xid_map[i].xid == xid) {
            os_memcpy(s_xid_map[i].chaddr, chaddr, 6); s_xid_map[i].expires_s = now + XID_TTL_S;
            return;
        }
        if (s_xid_map[i].xid == 0 || s_xid_map[i].expires_s <= now) { if (free_idx < 0) free_idx = i; }
        if (s_xid_map[i].expires_s < oldest_exp) { oldest_exp = s_fdb[i].expires_s; oldest_idx = i; }
    }
    int idx = (free_idx >= 0) ? free_idx : oldest_idx;
    s_xid_map[idx].xid = xid; os_memcpy(s_xid_map[idx].chaddr, chaddr, 6); s_xid_map[idx].expires_s = now + XID_TTL_S;
}

static const uint8_t * ICACHE_FLASH_ATTR xid_map_lookup(uint32_t xid)
{
    uint32_t now = now_secs();
    int i;
    for (i = 0; i < XID_MAP_SIZE; i++) {
        if (s_xid_map[i].xid == xid && s_xid_map[i].expires_s > now) return s_xid_map[i].chaddr;
    }
    return NULL;
}

/* -------------------------------------------------------------------------
 * Helpers
 * ------------------------------------------------------------------------- */

static inline void * ICACHE_FLASH_ATTR pkt_at(struct pbuf *p, uint16_t off, uint16_t need)
{
    if (p->tot_len < (uint16_t)(off + need)) return NULL;
    return (uint8_t *)p->payload + off;
}

static void ICACHE_FLASH_ATTR update_ip_chksum(ip_hdr_t *ip)
{
    ip->chksum = 0;
    uint32_t sum = 0;
    uint16_t *p = (uint16_t *)ip;
    int i;
    for (i = 0; i < (ip->vhl & 0x0f) * 2; i++) sum += ntohs(p[i]);
    while (sum >> 16) sum = (sum & 0xffff) + (sum >> 16);
    ip->chksum = htons(~((uint16_t)sum));
}

static uint8_t * ICACHE_FLASH_ATTR dhcp_find_option(uint8_t *opts, uint16_t opts_len, uint8_t tag, uint8_t *out_len)
{
    uint16_t i = 0;
    while (i < opts_len) {
        if (opts[i] == 255) { if (tag == 255) return &opts[i]; break; }
        if (opts[i] == 0)  { i++; continue; } 
        uint8_t t = opts[i], l = (i + 1 < opts_len) ? opts[i + 1] : 0;
        if (t == tag) { if (out_len) *out_len = l; return &opts[i + 2]; }
        i += 2 + l;
    }
    return NULL;
}

/* -------------------------------------------------------------------------
 * Proxy ARP & Maintenance
 * ------------------------------------------------------------------------- */

static void ICACHE_FLASH_ATTR send_proxy_arp_reply(struct netif *tx_nif, netif_linkoutput_fn lo, const arp_hdr_t *req, uint32_t target_ip)
{
    uint16_t pkt_len = sizeof(eth_hdr_t) + sizeof(arp_hdr_t);
    struct pbuf *p = pbuf_alloc(PBUF_RAW, pkt_len, PBUF_RAM);
    if (!p) return;
    eth_hdr_t *eth = (eth_hdr_t *)p->payload;
    arp_hdr_t *arp = (arp_hdr_t *)((uint8_t *)p->payload + sizeof(eth_hdr_t));
    os_memcpy(eth->dst, req->sha, 6); os_memcpy(eth->src, tx_nif->hwaddr, 6); eth->type = htons(ETHTYPE_ARP);
    arp->hwtype = req->hwtype; arp->prtype = req->prtype; arp->hwlen = req->hwlen; arp->prlen = req->prlen;
    arp->op = htons(2); 
    os_memcpy(arp->sha, tx_nif->hwaddr, 6); arp->spa = target_ip;
    os_memcpy(arp->tha, req->sha, 6); arp->tpa = req->spa;
    lo(tx_nif, p); pbuf_free(p);
}

static void ICACHE_FLASH_ATTR send_garp(uint32_t ip)
{
    uint16_t pkt_len = sizeof(eth_hdr_t) + sizeof(arp_hdr_t);
    struct pbuf *p = pbuf_alloc(PBUF_RAW, pkt_len, PBUF_RAM);
    if (!p) return;
    eth_hdr_t *eth = (eth_hdr_t *)p->payload;
    arp_hdr_t *arp = (arp_hdr_t *)((uint8_t *)p->payload + sizeof(eth_hdr_t));
    os_memset(eth->dst, 0xff, 6); os_memcpy(eth->src, s_sta_nif->hwaddr, 6); eth->type = htons(ETHTYPE_ARP);
    arp->hwtype = htons(1); arp->prtype = htons(ETHTYPE_IP); arp->hwlen = 6; arp->prlen = 4;
    arp->op = htons(1); os_memcpy(arp->sha, s_sta_nif->hwaddr, 6); arp->spa = ip;
    os_memset(arp->tha, 0, 6); arp->tpa = ip;
    s_orig_lo_sta(s_sta_nif, p); pbuf_free(p);
}

static void ICACHE_FLASH_ATTR arp_timer_func(void *arg)
{
    uint32_t now = now_secs();
    int i;
    for (i = 0; i < FDB_SIZE; i++) {
        if (s_fdb[i].ip != 0 && s_fdb[i].expires_s > now) send_garp(s_fdb[i].ip);
    }
}

/* -------------------------------------------------------------------------
 * DHCP snooping
 * ------------------------------------------------------------------------- */

static void ICACHE_FLASH_ATTR snoop_dhcp_request(struct pbuf *p, uint16_t eth_ip_udp_hdr_len)
{
    dhcp_msg_t *dhcp = (dhcp_msg_t *)pkt_at(p, eth_ip_udp_hdr_len, sizeof(dhcp_msg_t));
    if (!dhcp || dhcp->op != DHCP_OP_REQUEST || dhcp->hlen != 6 || dhcp->magic != htonl(DHCP_MAGIC_COOKIE)) return;

    uint8_t client_mac[6]; os_memcpy(client_mac, dhcp->chaddr, 6);
    os_memcpy(dhcp->chaddr, s_sta_nif->hwaddr, 6); 
    dhcp->flags |= htons(0x8000); 

    uint16_t opts_len = p->len - eth_ip_udp_hdr_len - (uint16_t)sizeof(dhcp_msg_t);
    uint8_t msg_type = 0, optlen;
    uint8_t *opt = dhcp_find_option(dhcp->options, opts_len, 53, &optlen);
    if (opt && optlen >= 1) msg_type = opt[0];

    if (msg_type == DHCP_MSG_DISCOVER || msg_type == DHCP_MSG_REQUEST) {
        xid_map_insert(dhcp->xid, client_mac);
        uint8_t opt61len;
        uint8_t *opt61 = dhcp_find_option(dhcp->options, opts_len, 61, &opt61len);
        if (!opt61 && opts_len + 9 <= 308) { 
            uint8_t *end = dhcp_find_option(dhcp->options, opts_len, 255, NULL);
            if (end) {
                end[0] = 61; end[1] = 7; end[2] = 1; os_memcpy(&end[3], client_mac, 6); end[9] = 255;
                uint16_t *ip_len_ptr = (uint16_t *)pkt_at(p, sizeof(eth_hdr_t) + 2, 2);
                uint16_t *udp_len_ptr = (uint16_t *)pkt_at(p, eth_ip_udp_hdr_len - 4, 2);
                if (ip_len_ptr) *ip_len_ptr = htons(ntohs(*ip_len_ptr) + 9);
                if (udp_len_ptr) *udp_len_ptr = htons(ntohs(*udp_len_ptr) + 9);
                p->len += 9; p->tot_len += 9;
                ip_hdr_t *ip = (ip_hdr_t *)pkt_at(p, sizeof(eth_hdr_t), sizeof(ip_hdr_t));
                if (ip) update_ip_chksum(ip);
            }
        }
        udp_hdr_t *udp = (udp_hdr_t *)pkt_at(p, eth_ip_udp_hdr_len - (uint16_t)sizeof(udp_hdr_t), sizeof(udp_hdr_t));
        if (udp) udp->chksum = 0;
    }
}

static bool ICACHE_FLASH_ATTR snoop_dhcp_reply(struct pbuf *p, uint16_t eth_ip_udp_hdr_len, uint8_t chaddr_out[6])
{
    dhcp_msg_t *dhcp = (dhcp_msg_t *)pkt_at(p, eth_ip_udp_hdr_len, sizeof(dhcp_msg_t));
    if (!dhcp || dhcp->op != DHCP_OP_REPLY || dhcp->hlen != 6 || dhcp->magic != htonl(DHCP_MAGIC_COOKIE)) return false;

    const uint8_t *orig_mac = xid_map_lookup(dhcp->xid);
    if (orig_mac) {
        os_memcpy(chaddr_out, orig_mac, 6); os_memcpy(dhcp->chaddr, orig_mac, 6);
        udp_hdr_t *udp = (udp_hdr_t *)pkt_at(p, eth_ip_udp_hdr_len - (uint16_t)sizeof(udp_hdr_t), sizeof(udp_hdr_t));
        if (udp) udp->chksum = 0;
    } else { os_memcpy(chaddr_out, dhcp->chaddr, 6); }

    if (dhcp->yiaddr != 0) {
        uint16_t opts_len = p->len - eth_ip_udp_hdr_len - (uint16_t)sizeof(dhcp_msg_t);
        uint8_t msg_type = 0, optlen;
        uint8_t *opt = dhcp_find_option(dhcp->options, opts_len, 53, &optlen);
        if (opt && optlen >= 1 && opt[0] == DHCP_MSG_ACK) {
            uint32_t ttl = FDB_TTL_S;
            opt = dhcp_find_option(dhcp->options, opts_len, 51, &optlen);
            if (opt && optlen >= 4) {
                ttl = ((uint32_t)opt[0] << 24) | ((uint32_t)opt[1] << 16) | ((uint32_t)opt[2] << 8) | (uint32_t)opt[3];
                if (ttl == 0 || ttl > 86400 * 7) ttl = FDB_TTL_S;
            }
            fdb_insert(dhcp->yiaddr, chaddr_out);
        }
    }
    return true;
}

/* -------------------------------------------------------------------------
 * Hooks
 * ------------------------------------------------------------------------- */

static err_t ICACHE_FLASH_ATTR bridge_output_sta(struct netif *netif, struct pbuf *p, ip_addr_t *ipaddr)
{
    const uint8_t *client_mac = fdb_lookup(ipaddr->addr);
    if (client_mac && pbuf_header(p, sizeof(eth_hdr_t)) == 0) {
        eth_hdr_t *eth = (eth_hdr_t *)p->payload;
        os_memcpy(eth->dst, client_mac, 6); os_memcpy(eth->src, s_ap_nif->hwaddr, 6); eth->type = htons(ETHTYPE_IP);
        
        Bytes_out += p->tot_len;
        Packets_out++;
#if DAILY_LIMIT
        Bytes_per_day += p->tot_len;
#endif
        
        err_t err = s_orig_lo_ap(s_ap_nif, p);
        pbuf_header(p, -(s16_t)sizeof(eth_hdr_t)); return err;
    }
    return s_orig_output_sta(netif, p, ipaddr);
}

static err_t ICACHE_FLASH_ATTR bridge_output_ap(struct netif *netif, struct pbuf *p, ip_addr_t *ipaddr)
{
    if (fdb_lookup(ipaddr->addr)) return s_orig_output_ap(netif, p, ipaddr);
    
    return s_orig_output_sta(s_sta_nif, p, ipaddr);
}

static err_t ICACHE_FLASH_ATTR bridge_input_ap(struct pbuf *p, struct netif *inp)
{
    if (os_strcmp(config.ssid, WIFI_SSID) == 0) return s_orig_input_ap(p, inp);

    struct pbuf *q = pbuf_alloc(PBUF_RAW, p->tot_len + 16, PBUF_RAM);

    Bytes_out += p->tot_len;
    Packets_out++;
#if DAILY_LIMIT
    Bytes_per_day += p->tot_len;
#endif

    if (!q) return s_orig_input_ap(p, inp);
    pbuf_copy(q, p);

    eth_hdr_t *eth = (eth_hdr_t *)q->payload;
    bool is_bcast = (eth->dst[0] & 0x01) != 0, is_to_ap_mac = (os_memcmp(eth->dst, s_ap_nif->hwaddr, 6) == 0);
    if (is_to_ap_mac && !is_bcast) { pbuf_free(q); return s_orig_input_ap(p, inp); }

    uint16_t eth_type = ntohs(eth->type);
    bool handled = false;
    os_memcpy(eth->src, s_sta_nif->hwaddr, 6);

    if (eth_type == ETHTYPE_ARP) {
        arp_hdr_t *arp = (arp_hdr_t *)pkt_at(q, sizeof(eth_hdr_t), sizeof(arp_hdr_t));
        if (arp) {
            fdb_insert(arp->spa, ((eth_hdr_t*)p->payload)->src);
            if (ntohs(arp->op) == 1 && s_sta_nif->ip_addr.addr != 0 && arp->tpa == s_sta_nif->ip_addr.addr) {
                send_proxy_arp_reply(s_ap_nif, s_orig_lo_ap, arp, s_sta_nif->ip_addr.addr); handled = true;
            } else {
                os_memcpy(arp->sha, s_sta_nif->hwaddr, 6); s_orig_lo_sta(s_sta_nif, q); handled = true;
            }
        }
    } else if (eth_type == ETHTYPE_IP) {
        ip_hdr_t *ip = (ip_hdr_t *)pkt_at(q, sizeof(eth_hdr_t), sizeof(ip_hdr_t));
        if (ip) {
            fdb_insert(ip->src, ((eth_hdr_t*)p->payload)->src);
            if (ip->proto == 17) {
                uint16_t udp_off = sizeof(eth_hdr_t) + (ip->vhl & 0x0f) * 4;
                udp_hdr_t *udp = (udp_hdr_t *)pkt_at(q, udp_off, sizeof(udp_hdr_t));
                if (udp && ntohs(udp->dst_port) == 67) snoop_dhcp_request(q, udp_off + sizeof(udp_hdr_t));
            }
            s_orig_lo_sta(s_sta_nif, q); handled = true;
        }
    } else { s_orig_lo_sta(s_sta_nif, q); handled = true; }
    
    pbuf_free(q);
    if (is_bcast) return s_orig_input_ap(p, inp);

    if (handled) { pbuf_free(p); return ERR_OK; }
    return s_orig_input_ap(p, inp);
}

static err_t ICACHE_FLASH_ATTR bridge_input_sta(struct pbuf *p, struct netif *inp)
{
    struct pbuf *q = pbuf_alloc(PBUF_RAW, p->tot_len, PBUF_RAM);

    Bytes_in += p->tot_len;
    Packets_in++;
#if DAILY_LIMIT
    Bytes_per_day += p->tot_len;
#endif

    if (!q) return s_orig_input_sta(p, inp);
    pbuf_copy(q, p);

    eth_hdr_t *eth = (eth_hdr_t *)q->payload;
    if (os_memcmp(eth->src, s_ap_nif->hwaddr, 6) == 0) { pbuf_free(q); return s_orig_input_sta(p, inp); }

    uint16_t eth_type = ntohs(eth->type);
    bool is_bcast = (eth->dst[0] & 0x01) != 0, is_to_sta_mac = (os_memcmp(eth->dst, s_sta_nif->hwaddr, 6) == 0);
    bool handled = false;
    os_memcpy(eth->src, s_ap_nif->hwaddr, 6);

    if (eth_type == ETHTYPE_IP) {
        ip_hdr_t *ip = (ip_hdr_t *)pkt_at(q, sizeof(eth_hdr_t), sizeof(ip_hdr_t));
        if (ip) {
            uint8_t ch[6]; bool have_dhcp = false;
            if (ip->proto == 17) {
                uint16_t udp_off = sizeof(eth_hdr_t) + (ip->vhl & 0x0f) * 4;
                udp_hdr_t *udp = (udp_hdr_t *)pkt_at(q, udp_off, sizeof(udp_hdr_t));
                if (udp && ntohs(udp->src_port) == 67 && ntohs(udp->dst_port) == 68) have_dhcp = snoop_dhcp_reply(q, udp_off + sizeof(udp_hdr_t), ch);
            }
            const uint8_t *mac = NULL;
            if (have_dhcp) mac = ch; else if (!is_bcast) { if (s_sta_nif->ip_addr.addr && ip->dst != s_sta_nif->ip_addr.addr) mac = fdb_lookup(ip->dst); }
            if (is_bcast || mac) {
                if (mac) os_memcpy(eth->dst, mac, 6);
                s_orig_lo_ap(s_ap_nif, q); handled = true;
            }
        }
    } else if (eth_type == ETHTYPE_ARP) {
        arp_hdr_t *arp = (arp_hdr_t *)pkt_at(q, sizeof(eth_hdr_t), sizeof(arp_hdr_t));
        if (arp) {
            if (ntohs(arp->op) == 1 && fdb_lookup(arp->tpa)) {
                send_proxy_arp_reply(s_sta_nif, s_orig_lo_sta, arp, arp->tpa);
                handled = true; pbuf_free(q); pbuf_free(p); return ERR_OK;
            }
            os_memcpy(arp->sha, s_ap_nif->hwaddr, 6);
            const uint8_t *mac = NULL;
            if (!is_bcast) { if (s_sta_nif->ip_addr.addr && arp->tpa != s_sta_nif->ip_addr.addr) mac = fdb_lookup(arp->tpa); }
            if (is_bcast || mac) {
                if (mac) { os_memcpy(eth->dst, mac, 6); os_memcpy(arp->tha, mac, 6); }
                s_orig_lo_ap(s_ap_nif, q); handled = true;
            }
        }
    }
    
    pbuf_free(q);

    if (is_bcast || (is_to_sta_mac && !handled)) return s_orig_input_sta(p, inp);
    pbuf_free(p); return ERR_OK;
}
/*
static err_t ICACHE_FLASH_ATTR bridge_input_sta(struct pbuf *p, struct netif *inp)
{
 //   struct pbuf *q = pbuf_alloc(PBUF_RAW, p->tot_len, PBUF_RAM);
 //   if (!q) return s_orig_input_sta(p, inp);
 //   pbuf_copy(q, p);

    eth_hdr_t *eth = (eth_hdr_t *)p->payload;
    if (os_memcmp(eth->src, s_ap_nif->hwaddr, 6) == 0) { return s_orig_input_sta(p, inp); }

    uint16_t eth_type = ntohs(eth->type);
    bool is_bcast = (eth->dst[0] & 0x01) != 0, is_to_sta_mac = (os_memcmp(eth->dst, s_sta_nif->hwaddr, 6) == 0);
    bool handled = false;
    os_memcpy(eth->src, s_ap_nif->hwaddr, 6);

    if (eth_type == ETHTYPE_IP) {
        ip_hdr_t *ip = (ip_hdr_t *)pkt_at(p, sizeof(eth_hdr_t), sizeof(ip_hdr_t));
        if (ip) {
            uint8_t ch[6]; bool have_dhcp = false;
            if (ip->proto == 17) {
                uint16_t udp_off = sizeof(eth_hdr_t) + (ip->vhl & 0x0f) * 4;
                udp_hdr_t *udp = (udp_hdr_t *)pkt_at(p, udp_off, sizeof(udp_hdr_t));
                if (udp && ntohs(udp->src_port) == 67 && ntohs(udp->dst_port) == 68) have_dhcp = snoop_dhcp_reply(p, udp_off + sizeof(udp_hdr_t), ch);
            }
            const uint8_t *mac = NULL;
            if (have_dhcp) mac = ch; else if (!is_bcast) { if (s_sta_nif->ip_addr.addr && ip->dst != s_sta_nif->ip_addr.addr) mac = fdb_lookup(ip->dst); }
            if (is_bcast || mac) {
                if (mac) os_memcpy(eth->dst, mac, 6);
                s_orig_lo_ap(s_ap_nif, p); handled = true;
            }
        }
    } else if (eth_type == ETHTYPE_ARP) {
        arp_hdr_t *arp = (arp_hdr_t *)pkt_at(p, sizeof(eth_hdr_t), sizeof(arp_hdr_t));
        if (arp) {
            if (ntohs(arp->op) == 1 && fdb_lookup(arp->tpa)) {
                send_proxy_arp_reply(s_sta_nif, s_orig_lo_sta, arp, arp->tpa);
                handled = true; pbuf_free(p); return ERR_OK;
            }
            os_memcpy(arp->sha, s_ap_nif->hwaddr, 6);
            const uint8_t *mac = NULL;
            if (!is_bcast) { if (s_sta_nif->ip_addr.addr && arp->tpa != s_sta_nif->ip_addr.addr) mac = fdb_lookup(arp->tpa); }
            if (is_bcast || mac) {
                if (mac) { os_memcpy(eth->dst, mac, 6); os_memcpy(arp->tha, mac, 6); }
                s_orig_lo_ap(s_ap_nif, p); handled = true;
            }
        }
    }
    
    if (is_bcast || (is_to_sta_mac && !handled)) return s_orig_input_sta(p, inp);
    pbuf_free(p); return ERR_OK;
}
*/
void ICACHE_FLASH_ATTR bridge_init(struct netif *sta_nif, struct netif *ap_nif)
{
    s_sta_nif = sta_nif; s_ap_nif = ap_nif;
    s_orig_input_sta = sta_nif->input; sta_nif->input = bridge_input_sta;
    s_orig_input_ap = ap_nif->input; ap_nif->input = bridge_input_ap;
    s_orig_output_sta = sta_nif->output; sta_nif->output = bridge_output_sta;
    s_orig_output_ap = ap_nif->output; ap_nif->output = bridge_output_ap;
    s_orig_lo_sta = sta_nif->linkoutput; s_orig_lo_ap = ap_nif->linkoutput;
    netif_set_default(sta_nif); sta_nif->napt = 0; ap_nif->napt = 0;
    os_memset(s_fdb, 0, sizeof(s_fdb)); os_memset(s_xid_map, 0, sizeof(s_xid_map));
    struct softap_config ap_cfg; wifi_softap_get_config(&ap_cfg); ap_cfg.channel = my_channel; wifi_softap_set_config(&ap_cfg);
    wifi_set_sleep_type(NONE_SLEEP_T);
    os_timer_disarm(&s_arp_timer);
    os_timer_setfn(&s_arp_timer, (os_timer_func_t *)arp_timer_func, NULL);
    os_timer_arm(&s_arp_timer, 30000, 1);
    os_printf("bridge: init done\n");
}

void ICACHE_FLASH_ATTR bridge_show_fdb(void)
{
    uint32_t now = now_secs();
    os_printf("Repeater FDB (IP -> Client MAC):\n");
    int i, count = 0;
    for (i = 0; i < FDB_SIZE; i++) {
        if (s_fdb[i].ip != 0 && s_fdb[i].expires_s > now) {
            os_printf("  " IPSTR " -> %02x:%02x:%02x:%02x:%02x:%02x (expires in %d s)\n",
                      IP2STR(&s_fdb[i].ip),
                      s_fdb[i].mac[0], s_fdb[i].mac[1], s_fdb[i].mac[2],
                      s_fdb[i].mac[3], s_fdb[i].mac[4], s_fdb[i].mac[5],
                      (int)(s_fdb[i].expires_s - now));
            count++;
        }
    }
    if (count == 0) os_printf("  (empty)\n");
}

#endif /* REPEATER_MODE */
