#include "c_types.h"
#include "mem.h"
#include "ets_sys.h"
#include "osapi.h"
#include "gpio.h"
#include "os_type.h"
#include "lwip/ip_addr.h"
#include "lwip/netif.h"
#include "lwip/dns.h"
#include "lwip/app/dhcpserver.h"
#include "lwip/app/espconn.h"
#include "lwip/app/espconn_tcp.h"

#include "user_interface.h"
#include "string.h"
#include "driver/uart.h"

#include "user_config.h"
#include "ring_buffer.h"
#include "config_flash.h"

/* Hold the sytem wise configuration */
sysconfig_t config;

/* System Task, for signals refer to user_config.h */
#define user_procTaskPrio        0
#define user_procTaskQueueLen    1
os_event_t    user_procTaskQueue[user_procTaskQueueLen];
static void user_procTask(os_event_t *events);

static ring_buffer_t *console_rx_buffer, *console_tx_buffer;
static char hwaddr[6];



int fputs_into_ringbuff(ring_buffer_t *buffer, uint8_t *strbuffer, int length)
{
    uint8_t index = 0, ch;
    for (index = 0; index < length; index++)
    {
        ch = *(strbuffer+index);
        ring_buffer_enqueue(buffer, ch);
    }

    return index;
}

int fgets_from_ringbuff(ring_buffer_t *buffer, uint8_t *strbuffer, int max_length)
{
    uint8_t max_unload, i, index = 0;
    uint8_t bytes_ringbuffer = buffer->data_present;
    uint8_t *str  = strbuffer;

    if (bytes_ringbuffer)   // If data is available in the ringbuffer
    {
        max_unload = (bytes_ringbuffer<max_length ? bytes_ringbuffer: max_length);

        for (index = 0; index < max_unload; index++)
        {
            *(str) = ring_buffer_dequeue(buffer);

            if (*str  == '\r') break;
            if ((*str != '\n') && (*str != '\r')) str++;
        }
    }
    *str = 0;
    return (str - strbuffer);
}

/* Similar to strtok */
int parse_str_into_tokens(char *str, char **tokens, int max_tokens)
{
    char    *tempstr, *tempstr1;
    int     token_count = 0;
    char    special = 0;
    if (max_tokens <= 0) return 0;
    tempstr = tokens[token_count] = str;
    while (tempstr != NULL)
    {
        {
            uint8_t ch;
            tempstr1 = tempstr;
            do
            {
                ch = *tempstr1;
                if (ch == 0) break;
                if (special != 0)
                {
                    if(ch == special)
                    {
                        break;
                    }
                }
                else
                    if((ch == ' ')||(ch=='\t')) break;

                tempstr1 ++;
            }
            while(ch != 0);
            if (ch == 0) tempstr1 = 0;
        }

        if (tempstr1 != NULL)
        {
            *tempstr1 = 0;
            tempstr1++;
            if (*tempstr1 == '"')
            {
                special = '"';
                tempstr1 ++;
            }
            else
                special = 0;

            if (token_count == max_tokens) break;

            if(*tempstr1 != 0)
            {
                token_count++;
                tokens[token_count] = tempstr1;
            }
        }

        tempstr = tempstr1;
    }

    return token_count + 1;
}


void console_send_response(struct espconn *pespconn)
{
    char payload[128];
    uint8_t max_unload, i, index = 0, max_length;

    uint8_t bytes_ringbuffer = console_tx_buffer->data_present;
    max_length = sizeof(payload);

    if (bytes_ringbuffer)   // If data is available in the ringbuffer
    {
        max_unload = (bytes_ringbuffer<max_length ? bytes_ringbuffer: max_length);

        for (index = 0; index < max_unload; index++)
        {
            payload[index] = ring_buffer_dequeue(console_tx_buffer);
        }
        payload[index] = 0;
        os_sprintf(payload, "%sCMD>", payload);

        //os_printf("Payload: %s\n", payload);

        if (pespconn != NULL)
            espconn_sent(pespconn, payload, os_strlen(payload));
        else
            UART_Send(0, &payload, os_strlen(payload));
    }
}

void console_handle_command(struct espconn *pespconn)
{
    char cmd_line[255];
    char response[255];
    char *tokens[5];

    int bytes_count, nTokens, i;

    bytes_count = fgets_from_ringbuff(console_rx_buffer, cmd_line, 255);
    cmd_line[bytes_count] = 0;
    nTokens = parse_str_into_tokens(cmd_line, tokens, 5);

#if 0
    os_printf("Command is %s [%d]\n", cmd_line, os_strlen(cmd_line));
    for (i=0; i< nTokens; i++)
        os_printf("Token[%2d] : %s \n", i, tokens[i]);
#endif

    if (strcmp(tokens[0], "save") == 0)
    {
        config_save(0, &config);
        os_sprintf(response, "Done!\r\n");
        fputs_into_ringbuff(console_tx_buffer, response, os_strlen(response));
        goto command_handled;
        /* enque the result into the tx buffer, and signal the mail task */
        //espconn_sent(pespconn, response, os_strlen(response));
    }

    if (strcmp(tokens[0], "help") == 0)
    {
        os_sprintf(response, "save|reset|set [ssid|password|auto_connect|ap_ssid|ap_password|ap_open] <val>|lock|unlock <password>\r\n");
        fputs_into_ringbuff(console_tx_buffer, response, os_strlen(response));
        goto command_handled;
    }

    if (strcmp(tokens[0], "show") == 0)
    {
	if (config.locked) {
           os_sprintf(response, "STA: SSID %s PW:****** [AutoConnect: %2d] \r\n",
                   config.ssid,
                   config.auto_connect);
           fputs_into_ringbuff(console_tx_buffer, response, os_strlen(response));
           os_sprintf(response, "AP:  SSID %s PW:****** [Open: %2d] \r\n",
                   config.ap_ssid,
                   config.ap_open);
           fputs_into_ringbuff(console_tx_buffer, response, os_strlen(response));
        goto command_handled;
        } else {
           os_sprintf(response, "STA: SSID %s PW:%s [AutoConnect: %2d] \r\n",
                   config.ssid,
                   config.password,
                   config.auto_connect);
           fputs_into_ringbuff(console_tx_buffer, response, os_strlen(response));
           os_sprintf(response, "AP:  SSID %s PW:%s [Open: %2d] \r\n",
                   config.ap_ssid,
                   config.ap_password,
                   config.ap_open);
           fputs_into_ringbuff(console_tx_buffer, response, os_strlen(response));
        goto command_handled;
        }
    }

    if (strcmp(tokens[0], "reset") == 0)
    {
        os_sprintf("Restarting ... \r\n");
        fputs_into_ringbuff(console_tx_buffer, response, os_strlen(response));

        system_restart();
        goto command_handled;
    }

    if (strcmp(tokens[0], "lock") == 0)
    {
	config.locked = 1;
	os_sprintf(response, "Config locked. Please save the configuration using save command\r\n");
	fputs_into_ringbuff(console_tx_buffer, response, os_strlen(response));
        goto command_handled;
    }

    if (strcmp(tokens[0], "unlock") == 0)
    {
        if (nTokens != 2)
        {
            os_sprintf(response, "Invalid number of arguments\r\n");
            fputs_into_ringbuff(console_tx_buffer, response, os_strlen(response));
        }
        else if (strcmp(tokens[1],config.password) == 0) {
	    config.locked = 0;
	    os_sprintf(response, "Config unlocked. Please save the configuration using save command\r\n");
            fputs_into_ringbuff(console_tx_buffer, response, os_strlen(response));
        } else {
	    os_sprintf(response, "Unlock failed. Invalid password\r\n");
	    fputs_into_ringbuff(console_tx_buffer, response, os_strlen(response));
        }
        goto command_handled;
    }


    if (strcmp(tokens[0], "set") == 0)
    {
        if (config.locked)
        {
            os_sprintf(response, "Invalid set command. Config locked\r\n");
            fputs_into_ringbuff(console_tx_buffer, response, os_strlen(response));
            goto command_handled;
        }

        /*
         * For set commands atleast 2 tokens "set" "parameter" "value" is needed
         * hence the check
         */
        if (nTokens < 3)
        {
            os_sprintf(response, "Invalid number of arguments\r\n");
            fputs_into_ringbuff(console_tx_buffer, response, os_strlen(response));
            goto command_handled;
        }
        else
        {
            // atleast 3 tokens, proceed
            if (strcmp(tokens[1],"ssid") == 0)
            {
                os_sprintf(config.ssid, "%s", tokens[2]);
                os_sprintf(response, "SSID Set. Please save the configuration using save command\r\n");
                fputs_into_ringbuff(console_tx_buffer, response, os_strlen(response));
                goto command_handled;
            }

            if (strcmp(tokens[1],"password") == 0)
            {
                os_sprintf(config.password, "%s", tokens[2]);
                os_sprintf(response, "Password Set. Please save the configuration using save command\r\n");
                fputs_into_ringbuff(console_tx_buffer, response, os_strlen(response));
                goto command_handled;
            }

            if (strcmp(tokens[1],"auto_connect") == 0)
            {
                config.auto_connect = atoi(tokens[2]);
                os_sprintf(response, "Auto Connect Set. Please save the configuration using save command\r\n");
                fputs_into_ringbuff(console_tx_buffer, response, os_strlen(response));
                goto command_handled;
            }

            if (strcmp(tokens[1],"ap_ssid") == 0)
            {
                os_sprintf(config.ap_ssid, "%s", tokens[2]);
                os_sprintf(response, "AP SSID Set. Please save the configuration using save command\r\n");
                fputs_into_ringbuff(console_tx_buffer, response, os_strlen(response));
                goto command_handled;
            }

            if (strcmp(tokens[1],"ap_password") == 0)
            {
                os_sprintf(config.ap_password, "%s", tokens[2]);
                os_sprintf(response, "AP Password Set. Please save the configuration using save command\r\n");
                fputs_into_ringbuff(console_tx_buffer, response, os_strlen(response));
                goto command_handled;
            }

            if (strcmp(tokens[1],"ap_open") == 0)
            {
                config.ap_open = atoi(tokens[2]);
                os_sprintf(response, "Open Auth Set. Please save the configuration using save command\r\n");
                fputs_into_ringbuff(console_tx_buffer, response, os_strlen(response));
                goto command_handled;
            }

        }
    }

    /* Control comes here only if the tokens[0] command is not handled */
    os_sprintf(response, "\r\nInvalid Command\r\n");
    fputs_into_ringbuff(console_tx_buffer, response, os_strlen(response));

command_handled:
    system_os_post(0, SIG_CONSOLE_TX, (ETSParam) pespconn);
    return;
}


static void ICACHE_FLASH_ATTR tcp_client_recv_cb(void *arg,
                                                 char *data,
                                                 unsigned short length)
{
    struct espconn *pespconn = (struct espconn *)arg;
    int            index = 0;
    uint8_t         ch;

    for (index=0; index <length; index++)
    {
        ch = *(data+index);
        ring_buffer_enqueue(console_rx_buffer, ch);

        // If a complete commandline is received, then signal the main
        // task that command is available for processing
        if (ch == '\n')
            system_os_post(0, SIG_CONSOLE_RX, (ETSParam) arg);
    }

    *(data+length) = 0;
}

static void ICACHE_FLASH_ATTR tcp_client_sent_cb(void *arg)
{
    struct espconn *pespconn = (struct espconn *)arg;
    os_printf("tcp_client_sent_cb(): Data sent to client\n");
}

static void ICACHE_FLASH_ATTR tcp_client_discon_cb(void *arg)
{
    os_printf("tcp_client_discon_cb(): client disconnected\n");
    struct espconn *pespconn = (struct espconn *)arg;
}

/* Called wen a client connects to server */
static void ICACHE_FLASH_ATTR tcp_client_connected_cb(void *arg)
{
    char payload[128];
    struct espconn *pespconn = (struct espconn *)arg;

    os_printf("tcp_client_connected_cb(): Client connected\r\n");

	wifi_get_macaddr(0, hwaddr);

    //espconn_regist_sentcb(pespconn,     tcp_client_sent_cb);
    espconn_regist_disconcb(pespconn,   tcp_client_discon_cb);
    espconn_regist_recvcb(pespconn,     tcp_client_recv_cb);
    espconn_regist_time(pespconn,  15, 1);  // Specific to console only
    os_sprintf(payload, "CMD>");
    espconn_sent(pespconn, payload, os_strlen(payload));
}


//Priority 0 Task
static void ICACHE_FLASH_ATTR user_procTask(os_event_t *events)
{
    struct netif *netif_ap, *netif_sta;

    switch(events->sig)
    {
    case SIG_START_SERVER:
	netif_ap = netif_list;
	//os_printf("AP: %c%c%d\r\n", netif_ap->name[0], netif_ap->name[1], netif_ap->num);

	netif_sta = netif_list->next;
	//os_printf("STA: %c%c%d\r\n", netif_sta->name[0], netif_sta->name[1], netif_sta->num);

	netif_ap->napt = 1;

        break;

    case SIG_CONSOLE_TX:
        {
            struct espconn *pespconn = (struct espconn *) events->par;
            console_send_response(pespconn);
        }
        break;

    case SIG_CONSOLE_RX:
        {
            struct espconn *pespconn = (struct espconn *) events->par;
            console_handle_command(pespconn);
        }
        break;

    case SIG_UART0:
        os_printf("SIG_UART0\r\n");
        break;

    case SIG_DO_NOTHING:
    default:
        // Intentionally ignoring these signals
        os_printf("Indication of command being received\r\n");
        break;

    }
}

/* Callback called when the connection state of the module with an Access Point changes */
void wifi_handle_event_cb(System_Event_t *evt)
{
    ip_addr_t dns_ip;

    //os_printf("wifi_handle_event_cb: ");
    switch (evt->event)
    {
    case EVENT_STAMODE_CONNECTED:
        os_printf("connect to ssid %s, channel %d\n", evt->event_info.connected.ssid, evt->event_info.connected.channel);
        break;

    case EVENT_STAMODE_DISCONNECTED:
        os_printf("disconnect from ssid %s, reason %d\n", evt->event_info.disconnected.ssid, evt->event_info.disconnected.reason);
        break;

    case EVENT_STAMODE_AUTHMODE_CHANGE:
        os_printf("mode: %d -> %d\n", evt->event_info.auth_change.old_mode, evt->event_info.auth_change.new_mode);
        break;

    case EVENT_STAMODE_GOT_IP:
	dns_ip = dns_getserver(0);
	dhcps_set_DNS(&dns_ip);

        os_printf("ip:" IPSTR ",mask:" IPSTR ",gw:" IPSTR ",dns:" IPSTR "\n", IP2STR(&evt->event_info.got_ip.ip), IP2STR(&evt->event_info.got_ip.mask), IP2STR(&evt->event_info.got_ip.gw), IP2STR(&dns_ip));

        // Post a Server Start message as the IP has been acquired to Task with priority 0
        system_os_post(user_procTaskPrio, SIG_START_SERVER, 0 );
        break;

    case EVENT_SOFTAPMODE_STACONNECTED:
        os_printf("station: " MACSTR "join, AID = %d\n", MAC2STR(evt->event_info.sta_connected.mac),
        evt->event_info.sta_connected.aid);
        break;

    case EVENT_SOFTAPMODE_STADISCONNECTED:
        os_printf("station: " MACSTR "leave, AID = %d\n", MAC2STR(evt->event_info.sta_disconnected.mac),
        evt->event_info.sta_disconnected.aid);
        break;

    default:
        break;
    }
}


void ICACHE_FLASH_ATTR
user_set_softap_config(void)
{
   struct softap_config apConfig;
 
   wifi_softap_get_config(&apConfig); // Get config first.
    
   os_memset(apConfig.ssid, 0, 32);
   os_sprintf(apConfig.ssid, "%s", config.ap_ssid);
   os_memset(apConfig.password, 0, 64);
   os_sprintf(apConfig.password, "%s", config.ap_password);
   if (!config.ap_open)
      apConfig.authmode = AUTH_WPA_WPA2_PSK;
   else
      apConfig.authmode = AUTH_OPEN;
   apConfig.ssid_len = 0;// or its actual length
   apConfig.max_connection = MAX_CLIENTS; // how many stations can connect to ESP8266 softAP at most.
 
   wifi_softap_set_config(&apConfig);// Set ESP8266 softap config .
    
}


void ICACHE_FLASH_ATTR user_set_station_config(void)
{
    struct station_config stationConf;

    /* Setup AP credentials */
    stationConf.bssid_set = 0;
    os_sprintf(stationConf.ssid, "%s", config.ssid);
    os_sprintf(stationConf.password, "%s", config.password);
    wifi_station_set_config(&stationConf);

    wifi_set_event_handler_cb(wifi_handle_event_cb);

    /* Configure the module to Auto Connect to AP*/
    if (config.auto_connect)
        wifi_station_set_auto_connect(1);
    else
        wifi_station_set_auto_connect(0);

}

void ICACHE_FLASH_ATTR user_init()
{
    gpio_init();

    console_rx_buffer = ring_buffer_init(256);
    console_tx_buffer = ring_buffer_init(256);

    // Initialize the GPIO subsystem.
    UART_init_console(BIT_RATE_115200, 0, console_rx_buffer, console_tx_buffer);
    //UART_init(BIT_RATE_115200, BIT_RATE_115200, 0);

    // Load WiFi-config
    config_load(0, &config);

    // Start the AP
    wifi_set_opmode(STATIONAP_MODE);
    user_set_softap_config();

    os_printf("Starting TCP Server on %d port\r\n", SERVER_PORT);
    struct espconn *pCon = (struct espconn *)os_zalloc(sizeof(struct espconn));
    if (pCon == NULL)
    {
        os_printf("CONNECT FAIL\r\n");
        return;
    }

    /* Equivalent to bind */
    pCon->type  = ESPCONN_TCP;
    pCon->state = ESPCONN_NONE;
    pCon->proto.tcp = (esp_tcp *)os_zalloc(sizeof(esp_tcp));
    pCon->proto.tcp->local_port = SERVER_PORT;

    /* Register callback when clients connect to the server */
    espconn_regist_connectcb(pCon, tcp_client_connected_cb);

    /* Put the connection in accept mode */
    espconn_accept(pCon);

    // Now start the STA-Mode
    user_set_station_config();

    //Start task
    system_os_task(user_procTask, user_procTaskPrio,user_procTaskQueue, user_procTaskQueueLen);
}
