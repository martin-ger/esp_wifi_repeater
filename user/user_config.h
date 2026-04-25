#ifndef _USER_CONFIG_
#define _USER_CONFIG_

#define		ESP_REPEATER_VERSION "V2.2.17"

#define		LOCAL_ACCESS 0x01
#define		REMOTE_ACCESS 0x02

#define		WIFI_SSID "ssid"
#define		WIFI_PASSWORD "password"

#define		WIFI_AP_SSID "MyAP"
#define		WIFI_AP_PASSWORD "none"

#ifndef MAX_CLIENTS
#define		MAX_CLIENTS 8
#endif
#ifndef MAX_DHCP
#define		MAX_DHCP 8
#endif

//
// Docker SDK comes with a user_rf_cal_sector_set() in libmain.a.
// Define this to 1 if you use an official SDK (and need the user_rf_cal_sector_set() here)
//
#ifndef USER_RF_CAL
#define		USER_RF_CAL 0
#endif

//
// Size of the console buffers
//
#define		MAX_CON_SEND_SIZE 1500
#define		MAX_CON_CMD_SIZE 120

//
// Defines the default GPIO pin if you have a status LED connected to a GPIO pin
// Any value > 16 disables this feature
// (I don't know of any that don't --AJK)
//
#ifndef STATUS_LED_GPIO
#define		STATUS_LED_GPIO 2
#endif

//
// Defines the default GPIO pin for HW factory reset (when this GPIO is pulled low for more than 3 secs)
// Any value > 16 disables this feature
//
#ifndef FACTORY_RESET_PIN
#define		FACTORY_RESET_PIN 255
#endif

//
// Define this to 1 to support the "scan" command for AP search
//
#ifndef ALLOW_SCANNING
#define		ALLOW_SCANNING 1
#endif

//
// Define this to 1 to support the "ping" command for IP connectivity check
//
#ifndef ALLOW_PING
#define		ALLOW_PING 1
#endif

//
// Define this to 1 to support the "sleep" command for power management and deep sleep
// Requires a connection of GPIO16 and RST (probably not available on ESP01 modules)
//
#ifndef ALLOW_SLEEP
#define		ALLOW_SLEEP 1
#endif

//
// Define this to 1 to support a daily traffic limit
//
#ifndef DAILY_LIMIT
#define		DAILY_LIMIT 0
#endif

//
// Define this to support the setting of the WiFi PHY mode
//
#ifndef PHY_MODE
#define		PHY_MODE 1
#endif

//
// Define this to 1 to support a loopback device (127.0.0.1)
//
#ifndef HAVE_LOOPBACK
#define		HAVE_LOOPBACK 1
#endif

//
// Define this to 1 if you want to have access to the config console via TCP.
// Ohterwise only local access via serial is possible
//
#ifndef REMOTE_CONFIG
#define		REMOTE_CONFIG 1
#endif
#define		CONSOLE_SERVER_PORT 7777

//
// Define this to 1 if you want to have access to the config via Web.
//
#ifndef WEB_CONFIG
#define		WEB_CONFIG 1
#endif
#define		WEB_CONFIG_PORT 80

//
// Define this to 1 if you want to have ACLs for the SoftAP.
//
#ifndef ACLS
#define		ACLS 1
#endif

//
// Define this to 1 if you want to have OTA (Over the air) updates
//
#ifndef OTAUPDATE
#define		OTAUPDATE 1
#endif

//
// Define this to 1 if you want to have QoS for the SoftAP.
//
#ifndef TOKENBUCKET
#define		TOKENBUCKET 1
#endif
// Burst size (token bucket size) in seconds of average bitrate
#ifndef MAX_TOKEN_RATIO
#define		MAX_TOKEN_RATIO 4
#endif

//
// Define this to 1 if you want to offer monitoring access to all transmitted data between the soft AP and all STAs.
// Packets are mirrored in pcap format to the given port.
// CAUTION: this might be a privacy issue!!!
//
#ifndef REMOTE_MONITORING
#define		REMOTE_MONITORING 1
#endif

#ifndef MONITOR_BUFFER_SIZE
#define		MONITOR_BUFFER_SIZE 0x3c00
#endif

// Define this if you want to cut packets short in case of too high data rate
#ifndef MONITOR_BUFFER_TIGHT
#define		MONITOR_BUFFER_TIGHT 0x1000
#endif

// Define this to 1 if you want to silently drop any packet that cannot be send to the monitor
#ifndef DROP_PACKET_IF_NOT_RECORDED
#define		DROP_PACKET_IF_NOT_RECORDED 1
#endif

//
//
// Define this to 1 if you want to have it work as a MQTT client
//
#ifndef MQTT_CLIENT
#define		MQTT_CLIENT 1
#endif

#define		MQTT_BUF_SIZE 2048
#define		MQTT_KEEPALIVE 120  /*seconds*/
#define		MQTT_RECONNECT_TIMEOUT 5 /*seconds*/
#define		PROTOCOL_NAMEv31 /*MQTT version 3.1 compatible with Mosquitto v0.15*/
//#define           PROTOCOL_NAMEv311 /*MQTT version 3.11 compatible with https://eclipse.org/paho/clients/testing/*/

#define		MQTT_PREFIX "/WiFi"
#define		MQTT_ID "ESPRouter"
#define		MQTT_REPORT_INTERVAL 15 /*seconds*/

// Define this if you want to get messages about GPIO pin status changes
//               #define USER_GPIO_IN 0

// Define this if you want to set an output signal
//              #define USER_GPIO_OUT 12

// Define this to 1 support WPA2 PEAP authentication (experimental)
//
#ifndef WPA2_PEAP
#define		WPA2_PEAP 1
#endif

//
// Define this to 1 to support an ENC28J60 Ethernet interface
// Experimental feature - not yet stable
//
#ifndef HAVE_ENC28J60
#define		HAVE_ENC28J60 0
#endif

//
// Define this to 1 to support ENC28J60 DHCP server
// Experimental feature - might not yet be stable
//
#ifndef DCHPSERVER_ENC28J60
#define         DCHPSERVER_ENC28J60 1
#endif

//
// Define this ESP GPIO, if you have the HW-RESET pin of the ENC28J60 connected to it
// Undefine it, if you have no HW-RESET
//
#define		ENC28J60_HW_RESET 4

//
// Define this to 1 if you want to be able to control GPIO pins from the command line
//
#ifndef GPIO_CMDS
#define		GPIO_CMDS 1
#endif

//
// Define this to 1 to advertise the repeater via mDNS on both STA and AP interfaces.
// The device will be reachable as <sta_hostname>.local
//
#ifndef MDNS_REPEATER
#define		MDNS_REPEATER 1
#endif

// Internal

typedef enum {
        SIG_DO_NOTHING = 0, SIG_START_SERVER = 1, SIG_SEND_DATA, SIG_UART0, SIG_CONSOLE_RX, SIG_CONSOLE_TX, SIG_CONSOLE_TX_RAW, SIG_GPIO_INT, SIG_LOOPBACK
} USER_SIGNALS;

#endif
