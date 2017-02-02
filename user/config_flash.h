#ifndef _CONFIG_FLASH_H_
#define _CONFIG_FLASH_H_

#include "c_types.h"
#include "mem.h"
#include "ets_sys.h"
#include "osapi.h"
#include "gpio.h"
#include "os_type.h"
#include "spi_flash.h"
#include "lwip/app/dhcpserver.h"

#define FLASH_BLOCK_NO 0xc

#define MAGIC_NUMBER    0x012005fe

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
    uint8_t     ap_open;           // Should we use no WPA?
    uint8_t	ap_on;		   // AP enabled?

    uint8_t     locked;		// Should we allow for config changes
    ip_addr_t	network_addr;	// Address of the internal network

    uint16_t	clock_speed;	// Freq of the CPU

    uint16_t	dhcps_entries;	// number of allocated entries in the following table
    struct dhcps_pool dhcps_p[MAX_DHCP];
} sysconfig_t, *sysconfig_p;

int config_load(sysconfig_p config);
void config_save(sysconfig_p config);

void blob_save(uint8_t blob_no, uint32_t *data, uint16_t len);
void blob_load(uint8_t blob_no, uint32_t *data, uint16_t len);
void blob_zero(uint8_t blob_no, uint16_t len);

#endif
