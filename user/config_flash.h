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

#include "user_config.h"
#include "acl.h"

#define FLASH_BLOCK_NO 0xc

#define MAGIC_NUMBER    0x014005fc

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
    ip_addr_t	dns_addr;	// Optional: address of the dns server

    ip_addr_t	my_addr;	// Optional (if not DHCP): IP address of the uplink side
    ip_addr_t	my_netmask;	// Optional (if not DHCP): IP netmask of the uplink side
    ip_addr_t	my_gw;		// Optional (if not DHCP): Gateway of the uplink side

    uint16_t	clock_speed;	// Freq of the CPU

#ifdef ALLOW_SLEEP
    int32_t	Vmin;		// Min voltage of battery
    int32_t	Vmin_sleep;	// Sleep time in sec when battery is low
#endif
#ifdef REMOTE_CONFIG
    uint16_t	config_port;	// Port on which the concole listenes (0 if no access)
#endif
#ifdef TOKENBUCKET
    uint32_t 	kbps_ds;	// Average downstream bitrate (0 if no limit);
    uint32_t 	kbps_us;	// Average upstream bitrate (0 if no limit);
#endif
#ifdef MQTT_CLIENT
    uint8_t     mqtt_host[32];	// IP or hostname of the MQTT broker, "none" if empty
    uint16_t	mqtt_port;	// Port of the MQTT broker

    uint8_t     mqtt_user[32];	// Username for broker login, "none" if empty
    uint8_t     mqtt_password[32]; // Password for broker login
    uint8_t	mqtt_id[32];    // MQTT clientId
    uint8_t	mqtt_prefix[64];   // Topic-prefix
    uint8_t	mqtt_command_topic[64];   // Topic on which commands are received, "none" if not subscibed
    uint8_t	mqtt_gpio_out_topic[64];  // Topic on which the status of the gpio_out pin can be set
    bool	gpio_out_status; // Initial status of the gpio_out pin

    uint32_t	mqtt_interval;  // Interval in secs for status messages, 0 means no messages
    uint16_t	mqtt_topic_mask;// Mask for active topics
#endif

    uint16_t	dhcps_entries;	// number of allocated entries in the following table
    struct dhcps_pool dhcps_p[MAX_DHCP];		// DHCP entries
#ifdef ACLS
    acl_entry	acl[MAX_NO_ACLS][MAX_ACL_ENTRIES];	// ACL entries
    uint8_t	acl_freep[MAX_NO_ACLS];			// ACL free pointers
#endif
} sysconfig_t, *sysconfig_p;

int config_load(sysconfig_p config);
void config_save(sysconfig_p config);

void blob_save(uint8_t blob_no, uint32_t *data, uint16_t len);
void blob_load(uint8_t blob_no, uint32_t *data, uint16_t len);
void blob_zero(uint8_t blob_no, uint16_t len);

#endif
