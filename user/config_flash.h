#ifndef _CONFIG_FLASH_H_
#define _CONFIG_FLASH_H_

#include "c_types.h"
#include "mem.h"
#include "ets_sys.h"
#include "osapi.h"
#include "gpio.h"
#include "os_type.h"
#include "spi_flash.h"

#define MAGIC_NUMBER    0x01200340

typedef struct
{
    // To check if the structure is initialized or not in flash
    uint32_t    magic_number;

    // Length of the structure, since this is a evolving library, the variant may change
    // hence used for verification
    uint16_t     length;

    /* Below variables are specific to my code */
    uint8_t     ssid[32];       // SSID of the AP to connect to
    uint8_t     password[64];   // Password of the network
    uint8_t     auto_connect;   // Should we auto connect

    uint8_t     ap_ssid[32];       // SSID of the own AP
    uint8_t     ap_password[64];   // Password of the own network
    uint8_t     ap_open;           // Should we use no WPA

    uint8_t     locked;		// Should we allow for config changes
    uint8_t	network_no;	// Configure the internal network to 192.168.x.0/24
    uint8_t	padding[2];	// to get a multiple of 4

#ifdef MQTT_CONFIG
    /* Specific to MQTT */
   	uint8_t     device_id[16];
	uint8_t     mqtt_host[64];
	uint32_t    mqtt_port;
	uint8_t     mqtt_user[32];
	uint8_t     mqtt_pass[32];
	uint32_t    mqtt_keepalive;
	uint8_t     security;
#endif

} sysconfig_t, *sysconfig_p;

int config_load(int version, sysconfig_p config);
void config_save(int version, sysconfig_p config);

#endif
