#ifndef _USER_CONFIG_
#define _USER_CONFIG_

typedef enum {SIG_DO_NOTHING = 0, SIG_START_SERVER = 1, SIG_SEND_DATA, SIG_UART0, SIG_CONSOLE_RX, SIG_CONSOLE_TX, SIG_PACKET } USER_SIGNALS;

#define WIFI_SSID  "ssid"
#define WIFI_PASSWORD "password"

#define WIFI_AP_SSID "ESP"
#define WIFI_AP_PASSWORD "repeator"

#define MAX_CLIENTS 8
#define MAX_DHCP 8

//
// Size of the console send buffer
//
#define MAX_CON_SEND_SIZE 1024

//
// Define this if you have a status LED connected to GPIO LED_NO
//
#define STATUS_LED  1
#define LED_NO  2

// Select the GPIO init function according to your selected GPIO
//#define SET_LED_GPIO  PIN_FUNC_SELECT(PERIPHS_IO_MUX_U0TXD_U, FUNC_GPIO1)
#define SET_LED_GPIO  PIN_FUNC_SELECT(PERIPHS_IO_MUX_GPIO2_U, FUNC_GPIO2)
//#define SET_LED_GPIO  PIN_FUNC_SELECT(PERIPHS_IO_MUX_MTDI_U, FUNC_GPIO4)

//
// Define this if you to support the "scan" command for AP search
//
#define ALLOW_SCANNING  1

//
// Define this if you want to have access to the config console via TCP.
// Ohterwise only local access via serial is possible
//
#define REMOTE_CONFIG 1
#define CONSOLE_SERVER_PORT 7777

//
// Define this if you want to offer monitoring access to all transmitted data between the soft AP and all STAs.
// Packets are mirrored in pcap format to the given port.
// CAUTION: this might be a privacy issue!!!
//
#define REMOTE_MONITORING 1

#define MONITOR_BUFFER_SIZE 0x3c00

// Define this if you want to cut packets short in case of too high data rate
//#define MONITOR_BUFFER_TIGHT  0x1000

// Define this if you want to silently drop any packet that cannot be send to the monitor
//#define DROP_PACKET_IF_NOT_RECORDED 1

#endif

