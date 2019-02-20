#ifndef __MQTT_IF_H__
#define __MQTT_IF_H__

#include "c_types.h"
#include "lwip/opt.h"
#include "lwip/netif.h"

#include "mqtt.h"

#define NETIF_FLAG_BROADCAST    0x02U
#define NETIF_FLAG_POINTTOPOINT 0x04U

struct mqtt_if_data;
struct event_base;

struct mqtt_if_data *mqtt_if_add(MQTT_Client *cl, uint8_t *topic_pre, uint8_t *password);
void mqtt_if_del(struct mqtt_if_data *data);

void mqtt_if_input(struct mqtt_if_data *data, const char* topic, uint32_t topic_len, const char *mqtt_data, uint32_t mqtt_data_len);
void mqtt_if_subscribe(struct mqtt_if_data *data);
void mqtt_if_unsubscribe(struct mqtt_if_data *data);

void mqtt_if_set_ipaddr(struct mqtt_if_data *data, uint32_t addr);
void mqtt_if_set_netmask(struct mqtt_if_data *data, uint32_t addr);
void mqtt_if_set_gw(struct mqtt_if_data *data, uint32_t addr);
void mqtt_if_set_up(struct mqtt_if_data *data);
void mqtt_if_set_down(struct mqtt_if_data *data);
void mqtt_if_set_mtu(struct mqtt_if_data *data, int mtu);
void mqtt_if_set_flag(struct mqtt_if_data *data, int flag);
void mqtt_if_clear_flag(struct mqtt_if_data *data, int flag);
void mqtt_if_clear_dns(void);
void mqtt_if_add_dns(uint32_t addr);

#endif
