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
#include "lwip/ip_route.h"

#include "user_config.h"
#include "acl.h"

#define FLASH_BLOCK_NO 0x60

#define MAGIC_NUMBER    0x13f43dad

typedef enum {
        AUTOMESH_OFF = 0, AUTOMESH_LEARNING, AUTOMESH_OPERATIONAL
} automeshmode;

#if GPIO_CMDS
typedef enum {
        UNDEFINED = 0, OUT, IN, IN_PULLUP
} gpio_mode;
#endif

typedef struct {
        // To check if the structure is initialized or not in flash
        uint32_t magic_number;

        // Length of the structure, since this is a evolving library, the variant may change
        // hence used for verification
        uint16_t length;

        /* Below variables are specific to my code */
        uint8_t ssid[32]; // SSID of the AP to connect to
        uint8_t password[64]; // Password of the network
        uint8_t auto_connect; // Should we auto connect
        uint8_t bssid[6]; // Optional: BSSID the AP
        uint8_t sta_hostname[32]; // Name of the station

        uint8_t ap_ssid[32]; // SSID of the own AP
        uint8_t ap_password[64]; // Password of the own network
        uint8_t ap_open; // Should we use no WPA?
        uint8_t ap_on; // AP enabled?
        uint8_t ssid_hidden; // Hidden SSID?
        uint8_t max_clients; // Max number of STAs on the SoftAP
#if WPA2_PEAP
        uint8_t use_PEAP; // WPA2 PEAP Authentication
        uint8_t PEAP_identity[64]; // PEAP enterprise outer identity
        uint8_t PEAP_username[64]; // PEAP enterprise username
        uint8_t PEAP_password[32]; // PEAP enterprise password
#endif
        uint8_t lock_password[32]; // Password of config lock
        uint8_t locked; // Should we allow for config changes

        int32_t ap_watchdog; // Seconds without ap traffic will cause reset (-1 off, default)
        int32_t client_watchdog; // Seconds without client traffic will cause reset (-1 off, default)

        automeshmode automesh_mode; // The AutoMesh mode
        uint8_t automesh_checked; // Flag that it has worked once
        uint8_t automesh_tries; // Counter of disconnects
        int8_t automesh_threshold; // RSSI limit
        uint32_t am_scan_time; // Seconds for scanning
        uint32_t am_sleep_time; // Seconds for sleeping

        uint8_t nat_enable; // Enable NAT on the AP netif;
	uint32_t tcp_timeout; // NAT timeout of TCP connections
	uint32_t udp_timeout; // NAT timeout of UDO 'connections'

        ip_addr_t network_addr; // Address of the internal network
        ip_addr_t dns_addr; // Optional: address of the dns server

        ip_addr_t my_addr; // Optional (if not DHCP): IP address of the STA interface
        ip_addr_t my_netmask; // Optional (if not DHCP): IP netmask of the STA interface
        ip_addr_t my_gw; // Optional (if not DHCP): Gateway of the STA interface
#if PHY_MODE
        uint16_t phy_mode; // WiFi PHY mode
#endif
        uint16_t clock_speed; // Freq of the CPU
        uint16_t status_led; // GPIO pin os the status LED (>16 disabled)
        uint16_t hw_reset; // GPIO pin that issues a hw factory reset (>16 disabled)
#if DAILY_LIMIT
        uint32_t daily_limit; // max KBytes per day (0 if no limit)
        int32_t ntp_timezone; // local timezone (to know when a day is over)
#endif
#if ALLOW_SLEEP
        int32_t Vmin; // Min voltage of battery
        int32_t Vmin_sleep; // Sleep time in sec when battery is low
#endif
#if REMOTE_CONFIG
        uint16_t config_port; // Port on which the concole listenes (0 if no access)
#endif
#if WEB_CONFIG
        uint16_t web_port; // Port on which the concole listenes (0 if no access)
#endif
        uint8_t config_access; // Controls the interfaces that allow config access (default LOCAL_ACCESS | REMOTE_ACCESS)
#if TOKENBUCKET
        uint32_t kbps_ds; // Average downstream bitrate (0 if no limit);
        uint32_t kbps_us; // Average upstream bitrate (0 if no limit);
#endif
#if MQTT_CLIENT
        uint8_t mqtt_host[32]; // IP or hostname of the MQTT broker, "none" if empty
        uint16_t mqtt_port; // Port of the MQTT broker

        uint8_t mqtt_user[32]; // Username for broker login, "none" if empty
        uint8_t mqtt_password[32]; // Password for broker login
        uint8_t mqtt_id[32]; // MQTT clientId
        uint8_t mqtt_prefix[64]; // Topic-prefix
        uint8_t mqtt_command_topic[64]; // Topic on which commands are received, "none" if not subscibed
        uint8_t mqtt_gpio_out_topic[64]; // Topic on which the status of the gpio_out pin can be set
        uint8_t mqtt_qos;
        bool gpio_out_status; // Initial status of the gpio_out pin

        uint32_t mqtt_interval; // Interval in secs for status messages, 0 means no messages
        uint16_t mqtt_topic_mask; // Mask for active topics
#endif

        uint8_t AP_MAC_address[6]; // MAC address of the AP
        uint8_t STA_MAC_address[6]; // MAC address of the STA
#if HAVE_ENC28J60
        ip_addr_t eth_addr; // Optional (if not DHCP): IP address of the ENC28J60 ETH interface
        ip_addr_t eth_netmask; // Optional (if not DHCP): IP netmask of the ENC28J60 ETH interface
        ip_addr_t eth_gw; // Optional (if not DHCP): Gateway of the ENC28J60 ETH interface
        uint8_t ETH_MAC_address[6]; // MAC address of the ENC28J60 ETH interface
        bool eth_enable; // Do we really have and use an ENC28J60 ETH interface
#if DCHPSERVER_ENC28J60
        bool enc_DHCPserver; // run DHCP _server_ on ETH interface if static IP
#endif
#endif
        uint32_t no_routes; // Number of static routing entires
        struct route_entry rt_table[MAX_ROUTES]; // The routing entries

        uint16_t dhcps_entries; // number of allocated entries in the following table
        struct dhcps_pool dhcps_p[MAX_DHCP]; // DHCP entries
#if ACLS
        acl_entry acl[MAX_NO_ACLS][MAX_ACL_ENTRIES]; // ACL entries
        uint8_t acl_freep[MAX_NO_ACLS]; // ACL free pointers
#endif
#if OTAUPDATE
        uint8_t ota_host[64];
        uint16_t ota_port;
#endif
#if GPIO_CMDS
        gpio_mode gpiomode[17];
#endif
} sysconfig_t, *sysconfig_p;

int config_load(sysconfig_p config);
void config_load_default(sysconfig_p config);
void config_save(sysconfig_p config);

void blob_save(uint8_t blob_no, uint32_t *data, uint16_t len);
void blob_load(uint8_t blob_no, uint32_t *data, uint16_t len);
void blob_zero(uint8_t blob_no, uint16_t len);

#endif

