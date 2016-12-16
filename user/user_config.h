#ifndef _USER_CONFIG_
#define _USER_CONFIG_

typedef enum {SIG_DO_NOTHING=0, SIG_START_SERVER=1, SIG_SEND_DATA, SIG_UART0, SIG_CONSOLE_RX, SIG_CONSOLE_TX } USER_SIGNALS;

#define WIFI_SSID            "ssid"
#define WIFI_PASSWORD        "password"

#define WIFI_AP_SSID         "MyAP"
#define WIFI_AP_PASSWORD     "none"

#define SERVER_PORT	     7777

#define MAX_CLIENTS	     10

#endif
