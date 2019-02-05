#include <unistd.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/time.h>

#include <lwip/netif.h>
#include <lwip/pbuf.h>
#include <lwip/stats.h>
#include <lwip/ip.h>
#include <lwip/init.h>
#include <lwip/tcp_impl.h>
#include <lwip/dns.h>
#include <mqttif.h>

#include "user_interface.h"

struct mqtt_if_data {
	struct netif netif;
	ip_addr_t ipaddr;
	MQTT_Client *mqttcl;
	uint8_t *topic_pre;
	u_char buf[4096];
};

//static pcap_dumper_t *pcap_dumper;

static err_t ICACHE_FLASH_ATTR
mqtt_if_output(struct netif *netif, struct pbuf *p, ip_addr_t *ipaddr)
{
struct mqtt_if_data *data = netif->state;
struct ip_hdr *iph;
int len;
uint8_t buf[os_strlen(data->topic_pre) + 20];

	len = pbuf_copy_partial(p, data->buf, sizeof(data->buf), 0);
	iph = (struct ip_hdr *)data->buf;

os_printf("packet %d, buf %x\r\n", len, p);
os_printf("to: %x from: %x\r\n", iph->dest, iph->src);

	os_sprintf(buf, "%s/%x", data->topic_pre, iph->dest);
	MQTT_Publish(data->mqttcl, buf, data->buf, len, 0, 1);

	return 0;
}

static err_t ICACHE_FLASH_ATTR
mqtt_if_init(struct netif *netif)
{
	NETIF_INIT_SNMP(netif, snmp_ifType_other, 0);
	netif->name[0] = 'm';
	netif->name[1] = 'q';

	netif->output = mqtt_if_output;
	netif->mtu = 1500;
	netif->flags = NETIF_FLAG_LINK_UP;

	return 0;
}

struct mqtt_if_data ICACHE_FLASH_ATTR *
mqtt_if_add(MQTT_Client *cl, uint8_t *topic_prefix)
{
	struct mqtt_if_data *data;

	data = calloc(1, sizeof(*data));
	data->mqttcl = cl;
	data->topic_pre = topic_prefix;

	netif_add(&data->netif, NULL, NULL, NULL, data, mqtt_if_init, ip_input);
//	netif_set_default(&data->netif);
	return data;
}

void ICACHE_FLASH_ATTR
mqtt_if_del(struct mqtt_if_data *data)
{
	netif_remove(&data->netif);
	free(data);
}

void ICACHE_FLASH_ATTR
mqtt_if_set_ipaddr(struct mqtt_if_data *data, uint32_t addr)
{
	ip_addr_t ipaddr;
	ipaddr.addr = addr;
	netif_set_ipaddr(&data->netif, &ipaddr);
	data->ipaddr = ipaddr;
}

void ICACHE_FLASH_ATTR
mqtt_if_set_netmask(struct mqtt_if_data *data, uint32_t addr)
{
	ip_addr_t ipaddr;
	ipaddr.addr = addr;
	netif_set_netmask(&data->netif, &ipaddr);
}

void ICACHE_FLASH_ATTR
mqtt_if_set_gw(struct mqtt_if_data *data, uint32_t addr)
{
	ip_addr_t ipaddr;
	ipaddr.addr = addr;
	netif_set_gw(&data->netif, &ipaddr);
}

void ICACHE_FLASH_ATTR
mqtt_if_set_up(struct mqtt_if_data *data)
{
uint8_t buf[os_strlen(data->topic_pre) + 20];

	os_sprintf(buf, "%s/%8x", data->ipaddr.addr);
	MQTT_Subscribe(data->mqttcl, buf, 0);
	os_sprintf(buf, "%s/ffffffff", data->topic_pre);
	MQTT_Subscribe(data->mqttcl, buf, 0);

	netif_set_up(&data->netif);
}

void ICACHE_FLASH_ATTR
mqtt_if_set_down(struct mqtt_if_data *data)
{
uint8_t buf[os_strlen(data->topic_pre) + 20];

	os_sprintf(buf, "%s/%8x", data->ipaddr.addr);
	MQTT_UnSubscribe(data->mqttcl, buf);
	os_sprintf(buf, "%s/ffffffff", data->topic_pre);
	MQTT_UnSubscribe(data->mqttcl, buf);

	netif_set_down(&data->netif);
}

void ICACHE_FLASH_ATTR
mqtt_if_set_mtu(struct mqtt_if_data *data, int mtu)
{
	data->netif.mtu = mtu;
}

void ICACHE_FLASH_ATTR
mqtt_if_set_flag(struct mqtt_if_data *data, int flag)
{
	data->netif.flags |= flag;
}

void ICACHE_FLASH_ATTR
mqtt_if_clear_flag(struct mqtt_if_data *data, int flag)
{
	data->netif.flags &= ~flag;
}

static int dns_count;

void ICACHE_FLASH_ATTR
mqtt_if_clear_dns(void)
{
	ip_addr_t addr;
//	addr.addr = INADDR_ANY;
	int i;
	for (i = 0; i < DNS_MAX_SERVERS; i++)
		dns_setserver(i, &addr);
	dns_count = 0;
}

void ICACHE_FLASH_ATTR
mqtt_if_add_dns(uint32_t addr)
{
	ip_addr_t ipaddr;
	ipaddr.addr = addr;
	dns_setserver(dns_count++, &ipaddr);
}
