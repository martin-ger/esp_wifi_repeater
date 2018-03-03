#ifndef _ACL_H_
#define _ACL_H_

#include "lwip/ip.h"
#include "lwip/pbuf.h"

#define MAX_NO_ACLS 4
#define MAX_ACL_ENTRIES 16

#define ACL_DENY     0x0
#define ACL_ALLOW    0x1
#define ACL_MONITOR  0x2

typedef struct _acl_entry {
uint32_t	src;
uint32_t	s_mask;
uint32_t	dest;
uint32_t	d_mask;
uint16_t	s_port;
uint16_t	d_port;
uint8_t		proto;
uint8_t		allow;
uint32_t	hit_count;
} acl_entry;

extern acl_entry acl[MAX_NO_ACLS][MAX_ACL_ENTRIES];
extern uint8_t acl_freep[MAX_NO_ACLS];
extern uint32_t acl_allow_count;
extern uint32_t acl_deny_count;

typedef uint8_t (*packet_deny_cb)(uint8_t proto, uint32_t saddr, uint16_t s_port, uint32_t daddr, uint16_t d_port, uint8_t allow);

void acl_init();
bool acl_is_empty(uint8_t acl_no);
void acl_clear(uint8_t acl_no);
void acl_clear_stats(uint8_t acl_no);
bool acl_add(uint8_t acl_no, 
	uint32_t src, uint32_t s_mask, uint32_t dest, uint32_t d_mask, 
	uint8_t proto, uint16_t s_port, uint16_t d_port, uint8_t allow);
uint8_t acl_check_packet(uint8_t acl_no, struct pbuf *p);
void acl_set_deny_cb(packet_deny_cb cb);

void addr2str(uint8_t *buf, uint32_t addr, uint32_t mask);
void acl_show(uint8_t acl_no, uint8_t *buf);

#endif /* _ACL_H_ */

