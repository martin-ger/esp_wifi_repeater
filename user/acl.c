#include "c_types.h"
#include "mem.h"
#include "ets_sys.h"
#include "osapi.h"
#include "os_type.h"
#include "lwip/ip.h"
#include "lwip/udp.h"
#include "lwip/tcp_impl.h"
#include "netif/etharp.h"

#include "user_interface.h"
#include "string.h"

#include "acl.h"

acl_entry acl[MAX_NO_ACLS][MAX_ACL_ENTRIES];
uint8_t acl_freep[MAX_NO_ACLS];
uint32_t acl_allow_count;
uint32_t acl_deny_count;
static packet_deny_cb my_deny_cb;

void ICACHE_FLASH_ATTR acl_init()
{
int i;
    acl_allow_count = acl_deny_count = 0;;
    my_deny_cb = NULL;
    for(i=0; i< MAX_NO_ACLS; i++) {
	acl_freep[i] = 0;
	acl_clear_stats(i);
    }
}

bool ICACHE_FLASH_ATTR acl_is_empty(uint8_t acl_no)
{
    if (acl_no >= MAX_NO_ACLS)
	return true;
    return acl_freep[acl_no] == 0;
}

void ICACHE_FLASH_ATTR acl_clear(uint8_t acl_no)
{
    if (acl_no >= MAX_NO_ACLS)
	return;
    acl_freep[acl_no] = 0;
    acl_clear_stats(acl_no);
}

void ICACHE_FLASH_ATTR acl_clear_stats(uint8_t acl_no)
{
int i;

    if (acl_no >= MAX_NO_ACLS)
	return;
    my_deny_cb = NULL;
    for(i=0; i< MAX_ACL_ENTRIES; i++)
	acl[acl_no][i].hit_count = 0;    
}

bool ICACHE_FLASH_ATTR acl_add(uint8_t acl_no, 
	uint32_t src, uint32_t s_mask, uint32_t dest, uint32_t d_mask, 
	uint8_t proto, uint16_t s_port, uint16_t d_port, uint8_t allow)
{
acl_entry *my_entry;

    if (acl_no >= MAX_NO_ACLS || acl_freep[acl_no] >= MAX_ACL_ENTRIES)
	return false;
    
    my_entry = &acl[acl_no][acl_freep[acl_no]];
    my_entry->src = src & s_mask;
    my_entry->s_mask = s_mask;
    my_entry->dest = dest & d_mask;
    my_entry->d_mask = d_mask;
    my_entry->proto = proto;
    my_entry->s_port = s_port;
    my_entry->d_port = d_port;
    my_entry->allow = allow;
    my_entry->hit_count = 0;

    acl_freep[acl_no]++;
    return true;
}


uint8_t ICACHE_FLASH_ATTR acl_check_packet(uint8_t acl_no, struct pbuf *p)
{
struct eth_hdr *mac_h;
struct ip_hdr *ip_h;
uint8_t proto;
struct udp_hdr *udp_h;
struct tcp_hdr *tcp_h;
uint16_t src_port, dest_port;
uint8_t *packet;
int i;
acl_entry *my_entry;
uint8_t allow;

    if (acl_no >= MAX_NO_ACLS)
	return ACL_DENY;

    if (p->len < sizeof(struct eth_hdr))
	return ACL_DENY;

    mac_h = (struct eth_hdr *)p->payload;

    // Allow ARP
    if (ntohs(mac_h->type) == ETHTYPE_ARP) {
	acl_allow_count++;
	return ACL_ALLOW;
    }

    // Drop anything else if not IPv4
    if (ntohs(mac_h->type) != ETHTYPE_IP) {
	acl_deny_count++;
	return ACL_DENY;
    }
   
    if (p->len < sizeof(struct eth_hdr)+sizeof(struct ip_hdr)) {
	acl_deny_count++;
	return ACL_DENY;
    }

    allow = ACL_DENY;
    packet = (uint8_t*)p->payload;
    ip_h = (struct ip_hdr *)&packet[sizeof(struct eth_hdr)];
    proto = IPH_PROTO(ip_h);

    switch (proto) {
    case IP_PROTO_UDP:
	if (p->len < sizeof(struct eth_hdr)+sizeof(struct ip_hdr)+sizeof(struct udp_hdr))
	    return ACL_DENY;
	udp_h = (struct udp_hdr *)&packet[sizeof(struct eth_hdr)+sizeof(struct ip_hdr)];
	src_port = ntohs(udp_h->src);
	dest_port = ntohs(udp_h->dest);
	break;

    case IP_PROTO_TCP:
	if (p->len < sizeof(struct eth_hdr)+sizeof(struct ip_hdr)+sizeof(struct tcp_hdr))
	    return ACL_DENY;
	tcp_h = (struct tcp_hdr *)&packet[sizeof(struct eth_hdr)+sizeof(struct ip_hdr)];
	src_port = ntohs(tcp_h->src);
	dest_port = ntohs(tcp_h->dest);
	break;

    case IP_PROTO_ICMP:
	src_port = dest_port = 0;
	break;
 
    // Drop anything that is not UDP, TCP, or ICMP
    default:
	acl_deny_count++;
	return ACL_DENY;
    }

//    os_printf("Src: %d.%d.%d.%d Dst: %d.%d.%d.%d Proto: %s SP:%d DP:%d\n", 
//		IP2STR(&ip_h->src), IP2STR(&ip_h->dest), 
//		proto==IP_PROTO_TCP?"TCP":proto==IP_PROTO_UDP?"UDP":"IP4", src_port, dest_port);

    for(i=0; i<acl_freep[acl_no]; i++) {
	my_entry = &acl[acl_no][i];
	if ((my_entry->proto == 0  || proto == my_entry->proto) &&
	    (my_entry->src == 0    || my_entry->src == (ip_h->src.addr&my_entry->s_mask)) &&
	    (my_entry->dest == 0   || my_entry->dest == (ip_h->dest.addr&my_entry->d_mask)) &&
	    (my_entry->s_port == 0 || my_entry->s_port == src_port) &&
	    (my_entry->d_port == 0 || my_entry->d_port == dest_port)) {
		allow = my_entry->allow;
		my_entry->hit_count++;
		goto done;
	}
    }

done:
    if (!(allow & ACL_ALLOW) && my_deny_cb != NULL)
	allow = my_deny_cb(proto, ip_h->src.addr, src_port, ip_h->dest.addr, dest_port, allow);
    if (allow & ACL_ALLOW) acl_allow_count++; else acl_deny_count++;
//    os_printf(" allow: %d\r\n",  allow);
    return allow;
}

void acl_set_deny_cb(packet_deny_cb cb)
{
    my_deny_cb = cb;
}

void ICACHE_FLASH_ATTR addr2str(uint8_t *buf, uint32_t addr, uint32_t mask)
{
uint8_t clidr;

    if (addr == 0 && mask == 0) {
	os_sprintf(buf, "any");
	return;
    }

    mask = ntohl(mask);
    for (clidr = 0; mask; mask <<= 1,clidr++);
    if (clidr < 32)
	os_sprintf(buf, "%d.%d.%d.%d/%d", IP2STR((ip_addr_t*)&addr), clidr);
    else
	os_sprintf(buf, "%d.%d.%d.%d", IP2STR((ip_addr_t*)&addr));
}

void ICACHE_FLASH_ATTR port2str(uint8_t *buf, uint16_t port)
{
    if (port == 0)
	os_sprintf(buf, "any");
    else
	os_sprintf(buf, "%d", port);    
}

void ICACHE_FLASH_ATTR acl_show(uint8_t acl_no, uint8_t *buf)
{
int i;
acl_entry *my_entry;
uint8_t line[80], addr1[21], addr2[21], port1[6], port2[6];

    buf[0] = 0;

    if (acl_no >= MAX_NO_ACLS)
	return;

    for(i=0; i<acl_freep[acl_no]; i++) {
	my_entry = &acl[acl_no][i];
	addr2str(addr1, my_entry->src, my_entry->s_mask);
	port2str(port1, my_entry->s_port);
	addr2str(addr2, my_entry->dest, my_entry->d_mask);
	port2str(port2, my_entry->d_port);
	if (my_entry->proto != 0)
	    os_sprintf(line, "%s %s:%s %s:%s %s%s (%d hits)\r\n",
		my_entry->proto==IP_PROTO_TCP?"TCP":"UDP", 
		addr1, port1, addr2, port2,
		(my_entry->allow & ACL_ALLOW)?"allow":"deny",
		(my_entry->allow & ACL_MONITOR)?"_monitor":"",
		my_entry->hit_count);
	else 
	    os_sprintf(line, "IP %s %s %s%s (%d hits)\r\n",
		addr1, addr2,
		(my_entry->allow & ACL_ALLOW)?"allow":"deny",
		(my_entry->allow & ACL_MONITOR)?"_monitor":"",
		my_entry->hit_count);
        os_memcpy(&buf[os_strlen(buf)], line, os_strlen(line)+1);
    }
}
