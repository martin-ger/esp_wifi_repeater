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

#include "ringbuf.h"
#include "user_config.h"
#include "config_flash.h"

#ifdef REMOTE_MONITORING
#include "pcap.h"
#endif

/* Some stats */
uint32_t Bytes_in = 0, Bytes_out = 0;

/* Hold the system wide configuration */
sysconfig_t config;

/* System Task, for signals refer to user_config.h */
#define user_procTaskPrio        0
#define user_procTaskQueueLen    1
os_event_t    user_procTaskQueue[user_procTaskQueueLen];
static void user_procTask(os_event_t *events);

static ringbuf_t console_rx_buffer, console_tx_buffer;

struct netif *netif_ap;
static netif_input_fn orig_input_ap;
static netif_linkoutput_fn orig_output_ap;

#ifdef REMOTE_MONITORING
static uint8_t monitoring_on;
static ringbuf_t pcap_buffer;
struct espconn *cur_mon_conn;
static uint8_t monitoring_send_ongoing;


static void ICACHE_FLASH_ATTR tcp_monitor_sent_cb(void *arg)
{
    uint16_t len;
    static uint8_t tbuf[1400];

    struct espconn *pespconn = (struct espconn *)arg;
    //os_printf("tcp_client_sent_cb(): Data sent to monitor\n");

    monitoring_send_ongoing = 0;
    if (!monitoring_on) return;

    len = ringbuf_bytes_used(pcap_buffer);
    if (len > 0) {
	 if (len > 1400)
		 len = 1400;

	 ringbuf_memcpy_from(tbuf, pcap_buffer, len);
	 //os_printf("tcp_monitor_sent_cb(): %d Bytes sent to monitor\n", len);
	 if (espconn_sent(pespconn, tbuf, len) != 0) {
		os_printf("TCP send error\r\n");
		return;
	 }
	 monitoring_send_ongoing = 1;
     }
}

static void ICACHE_FLASH_ATTR tcp_monitor_discon_cb(void *arg)
{
    os_printf("tcp_monitor_discon_cb(): client disconnected\n");
    struct espconn *pespconn = (struct espconn *)arg;

    monitoring_on = 0;
}


/* Called when a client connects to the monitor server */
static void ICACHE_FLASH_ATTR tcp_monitor_connected_cb(void *arg)
{
    struct espconn *pespconn = (struct espconn *)arg;
    struct pcap_file_header pcf_hdr;

    os_printf("tcp_monitor_connected_cb(): Client connected\r\n");

    ringbuf_reset(pcap_buffer);

    cur_mon_conn = pespconn;

    espconn_regist_sentcb(pespconn,     tcp_monitor_sent_cb);
    espconn_regist_disconcb(pespconn,   tcp_monitor_discon_cb);
    //espconn_regist_recvcb(pespconn,     tcp_client_recv_cb);
    espconn_regist_time(pespconn,  300, 1);  // Specific to console only

    pcf_hdr.magic 		= PCAP_MAGIC_NUMBER;
    pcf_hdr.version_major 	= PCAP_VERSION_MAJOR;
    pcf_hdr.version_minor	= PCAP_VERSION_MINOR;
    pcf_hdr.thiszone 		= 0;
    pcf_hdr.sigfigs 		= 0;
    pcf_hdr.snaplen 		= 1600;
    pcf_hdr.linktype		= LINKTYPE_ETHERNET;

    espconn_sent(pespconn, (uint8_t *)&pcf_hdr, sizeof(pcf_hdr));
    monitoring_send_ongoing = 1;

    monitoring_on = 1;
}


int ICACHE_FLASH_ATTR put_packet_to_ringbuf(struct pbuf *p) {
  struct pcap_pkthdr pcap_phdr;
  uint32_t t_usecs;
  uint32_t len = p->len;

#ifdef MONITOR_BUFFER_TIGHT
    if (ringbuf_bytes_free(pcap_buffer) < MONITOR_BUFFER_TIGHT) {
       if (len > 60) {
	  len = 60;
          //os_printf("Packet cut\n");
       }
    }
#endif

    if (ringbuf_bytes_free(pcap_buffer) >= sizeof(pcap_phdr)+len) {
       //os_printf("Put %d Bytes into RingBuff\r\n", sizeof(pcap_phdr)+p->len);
       t_usecs = system_get_time();
       pcap_phdr.ts_sec = t_usecs/1000000;
       pcap_phdr.ts_usec = t_usecs%1000000;
       pcap_phdr.caplen = len;
       pcap_phdr.len = p->tot_len;
       ringbuf_memcpy_into(pcap_buffer, (uint8_t*)&pcap_phdr, sizeof(pcap_phdr));
       ringbuf_memcpy_into(pcap_buffer, p->payload, len);
    } else {
       //os_printf("Packet with %d Bytes discarded\r\n", p->len);
       return -1;
    }
  return 0;
}
#endif

static uint8_t columns = 0;
err_t ICACHE_FLASH_ATTR my_input_ap (struct pbuf *p, struct netif *inp) {

    //os_printf("Got packet from STA\r\n");
    Bytes_in += p->tot_len;

#ifdef REMOTE_MONITORING
    if (monitoring_on) {
       system_os_post(user_procTaskPrio, SIG_PACKET, 0 );
       if (put_packet_to_ringbuf(p) != 0) {
#ifdef DROP_PACKET_IF_NOT_RECORDED
               pbuf_free(p);
	       return;
#else 
       	       os_printf("x");
	       if (++columns > 40) {
		  os_printf("\r\n");
		  columns = 0;
	       }
#endif
       }
    }
#endif

    orig_input_ap (p, inp);
}

err_t ICACHE_FLASH_ATTR my_output_ap (struct netif *outp, struct pbuf *p) {

    //os_printf("Send packet to STA\r\n");
    Bytes_out += p->tot_len;

#ifdef REMOTE_MONITORING
    if (monitoring_on) {
       system_os_post(user_procTaskPrio, SIG_PACKET, 0 );
       if (put_packet_to_ringbuf(p) != 0) {
#ifdef DROP_PACKET_IF_NOT_RECORDED
               pbuf_free(p);
	       return;
#else 
       	       os_printf("x");
	       if (++columns > 40) {
		  os_printf("\r\n");
		  columns = 0;
	       }
#endif
       }
    }
#endif

    orig_output_ap (outp, p);
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
                    if(ch <= ' ') break;

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
    uint8_t max_unload, max_length;

    uint8_t bytes_ringbuffer = ringbuf_bytes_used(console_tx_buffer);
    max_length = sizeof(payload)-1;

    if (bytes_ringbuffer)   // If data is available in the ringbuffer
    {
        max_unload = (bytes_ringbuffer<max_length ? bytes_ringbuffer: max_length);

	ringbuf_memcpy_from(payload, console_tx_buffer, max_unload);
        payload[max_unload] = 0;
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

    bytes_count = ringbuf_bytes_used(console_rx_buffer);
    ringbuf_memcpy_from(cmd_line, console_rx_buffer, bytes_count);
    
    cmd_line[bytes_count] = 0;
    nTokens = parse_str_into_tokens(cmd_line, tokens, 5);

    if (strcmp(tokens[0], "save") == 0)
    {
        config_save(0, &config);
        os_sprintf(response, "Done!\r\n");
        ringbuf_memcpy_into(console_tx_buffer, response, os_strlen(response));
        goto command_handled;
    }

    if (strcmp(tokens[0], "help") == 0)
    {
        os_sprintf(response, "save|reset|set [ssid|password|auto_connect|ap_ssid|ap_password|ap_open] <val>|lock|unlock <password>\r\n");
        ringbuf_memcpy_into(console_tx_buffer, response, os_strlen(response));
        goto command_handled;
    }

    if (strcmp(tokens[0], "show") == 0)
    {
	if (config.locked) {
           os_sprintf(response, "STA: SSID %s PW:****** [AutoConnect: %2d] \r\n",
                   config.ssid,
                   config.auto_connect);
           ringbuf_memcpy_into(console_tx_buffer, response, os_strlen(response));
           os_sprintf(response, "AP:  SSID %s PW:****** [Open: %2d] \r\n",
                   config.ap_ssid,
                   config.ap_open);
           ringbuf_memcpy_into(console_tx_buffer, response, os_strlen(response));
        goto command_handled;
        } else {
           os_sprintf(response, "STA: SSID %s PW:%s [AutoConnect: %2d] \r\n",
                   config.ssid,
                   config.password,
                   config.auto_connect);
           ringbuf_memcpy_into(console_tx_buffer, response, os_strlen(response));
           os_sprintf(response, "AP:  SSID %s PW:%s [Open: %2d] \r\n",
                   config.ap_ssid,
                   config.ap_password,
                   config.ap_open);
           ringbuf_memcpy_into(console_tx_buffer, response, os_strlen(response));
           os_sprintf(response, "Bytes in %d Bytes out %d\r\n", Bytes_in, Bytes_out);
           ringbuf_memcpy_into(console_tx_buffer, response, os_strlen(response));
        goto command_handled;
        }
    }

    if (strcmp(tokens[0], "reset") == 0)
    {
        os_sprintf("Restarting ... \r\n");
        ringbuf_memcpy_into(console_tx_buffer, response, os_strlen(response));

        system_restart();
        goto command_handled;
    }

    if (strcmp(tokens[0], "lock") == 0)
    {
	config.locked = 1;
	os_sprintf(response, "Config locked. Please save the configuration using save command\r\n");
	ringbuf_memcpy_into(console_tx_buffer, response, os_strlen(response));
        goto command_handled;
    }

    if (strcmp(tokens[0], "unlock") == 0)
    {
        if (nTokens != 2)
        {
            os_sprintf(response, "Invalid number of arguments\r\n");
            ringbuf_memcpy_into(console_tx_buffer, response, os_strlen(response));
        }
        else if (strcmp(tokens[1],config.password) == 0) {
	    config.locked = 0;
	    os_sprintf(response, "Config unlocked. Please save the configuration using save command\r\n");
            ringbuf_memcpy_into(console_tx_buffer, response, os_strlen(response));
        } else {
	    os_sprintf(response, "Unlock failed. Invalid password\r\n");
	    ringbuf_memcpy_into(console_tx_buffer, response, os_strlen(response));
        }
        goto command_handled;
    }


    if (strcmp(tokens[0], "set") == 0)
    {
        if (config.locked)
        {
            os_sprintf(response, "Invalid set command. Config locked\r\n");
            ringbuf_memcpy_into(console_tx_buffer, response, os_strlen(response));
            goto command_handled;
        }

        /*
         * For set commands atleast 2 tokens "set" "parameter" "value" is needed
         * hence the check
         */
        if (nTokens < 3)
        {
            os_sprintf(response, "Invalid number of arguments\r\n");
            ringbuf_memcpy_into(console_tx_buffer, response, os_strlen(response));
            goto command_handled;
        }
        else
        {
            // atleast 3 tokens, proceed
            if (strcmp(tokens[1],"ssid") == 0)
            {
                os_sprintf(config.ssid, "%s", tokens[2]);
                os_sprintf(response, "SSID Set. Please save the configuration using save command\r\n");
                ringbuf_memcpy_into(console_tx_buffer, response, os_strlen(response));
                goto command_handled;
            }

            if (strcmp(tokens[1],"password") == 0)
            {
                os_sprintf(config.password, "%s", tokens[2]);
                os_sprintf(response, "Password Set. Please save the configuration using save command\r\n");
                ringbuf_memcpy_into(console_tx_buffer, response, os_strlen(response));
                goto command_handled;
            }

            if (strcmp(tokens[1],"auto_connect") == 0)
            {
                config.auto_connect = atoi(tokens[2]);
                os_sprintf(response, "Auto Connect Set. Please save the configuration using save command\r\n");
                ringbuf_memcpy_into(console_tx_buffer, response, os_strlen(response));
                goto command_handled;
            }

            if (strcmp(tokens[1],"ap_ssid") == 0)
            {
                os_sprintf(config.ap_ssid, "%s", tokens[2]);
                os_sprintf(response, "AP SSID Set. Please save the configuration using save command\r\n");
                ringbuf_memcpy_into(console_tx_buffer, response, os_strlen(response));
                goto command_handled;
            }

            if (strcmp(tokens[1],"ap_password") == 0)
            {
                os_sprintf(config.ap_password, "%s", tokens[2]);
                os_sprintf(response, "AP Password Set. Please save the configuration using save command\r\n");
                ringbuf_memcpy_into(console_tx_buffer, response, os_strlen(response));
                goto command_handled;
            }

            if (strcmp(tokens[1],"ap_open") == 0)
            {
                config.ap_open = atoi(tokens[2]);
                os_sprintf(response, "Open Auth Set. Please save the configuration using save command\r\n");
                ringbuf_memcpy_into(console_tx_buffer, response, os_strlen(response));
                goto command_handled;
            }

        }
    }

    /* Control comes here only if the tokens[0] command is not handled */
    os_sprintf(response, "\r\nInvalid Command\r\n");
    ringbuf_memcpy_into(console_tx_buffer, response, os_strlen(response));

command_handled:
    system_os_post(0, SIG_CONSOLE_TX, (ETSParam) pespconn);
    return;
}

#ifdef REMOTE_CONFIG
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
	ringbuf_memcpy_into(console_rx_buffer, &ch, 1);

        // If a complete commandline is received, then signal the main
        // task that command is available for processing
        if (ch == '\n')
            system_os_post(0, SIG_CONSOLE_RX, (ETSParam) arg);
    }

    *(data+length) = 0;
}


static void ICACHE_FLASH_ATTR tcp_client_discon_cb(void *arg)
{
    os_printf("tcp_client_discon_cb(): client disconnected\n");
    struct espconn *pespconn = (struct espconn *)arg;
}


/* Called when a client connects to the console server */
static void ICACHE_FLASH_ATTR tcp_client_connected_cb(void *arg)
{
    char payload[128];
    struct espconn *pespconn = (struct espconn *)arg;

    os_printf("tcp_client_connected_cb(): Client connected\r\n");

    //espconn_regist_sentcb(pespconn,     tcp_client_sent_cb);
    espconn_regist_disconcb(pespconn,   tcp_client_discon_cb);
    espconn_regist_recvcb(pespconn,     tcp_client_recv_cb);
    espconn_regist_time(pespconn,  300, 1);  // Specific to console only
    os_sprintf(payload, "CMD>");
    espconn_sent(pespconn, payload, os_strlen(payload));
}
#endif


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

	/* Switch AP to NAT */
	netif_ap->napt = 1;

	/* Hook into the traffic of the internal AP */
	orig_input_ap = netif_ap->input;
	netif_ap->input = my_input_ap;

	orig_output_ap = netif_ap->linkoutput;
	netif_ap->linkoutput = my_output_ap;

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
#ifdef REMOTE_MONITORING
    case SIG_PACKET:
        {
            if (!monitoring_send_ongoing)
		   tcp_monitor_sent_cb(cur_mon_conn);
        }
        break;
#endif
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

    console_rx_buffer = ringbuf_new(256);
    console_tx_buffer = ringbuf_new(256);

    // Initialize the GPIO subsystem.
    UART_init_console(BIT_RATE_115200, 0, console_rx_buffer, console_tx_buffer);
    //UART_init(BIT_RATE_115200, BIT_RATE_115200, 0);

    os_printf("\r\n\r\nWiFi Repeater starting\r\n");

    // Load WiFi-config
    config_load(0, &config);

    // Start the AP
    wifi_set_opmode(STATIONAP_MODE);
    user_set_softap_config();

#ifdef REMOTE_CONFIG
    os_printf("Starting Console TCP Server on %d port\r\n", CONSOLE_SERVER_PORT);
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
    pCon->proto.tcp->local_port = CONSOLE_SERVER_PORT;

    /* Register callback when clients connect to the server */
    espconn_regist_connectcb(pCon, tcp_client_connected_cb);

    /* Put the connection in accept mode */
    espconn_accept(pCon);
#endif

#ifdef REMOTE_MONITORING
    pcap_buffer = ringbuf_new(MONITOR_BUFFER_SIZE);
    monitoring_on = 0;
    monitoring_send_ongoing = 0;

    os_printf("Starting Monitor TCP Server on %d port\r\n", MONITOR_SERVER_PORT);
    struct espconn *mCon = (struct espconn *)os_zalloc(sizeof(struct espconn));
    if (mCon == NULL)
    {
        os_printf("CONNECT FAIL\r\n");
        return;
    }

    /* Equivalent to bind */
    mCon->type  = ESPCONN_TCP;
    mCon->state = ESPCONN_NONE;
    mCon->proto.tcp = (esp_tcp *)os_zalloc(sizeof(esp_tcp));
    mCon->proto.tcp->local_port = MONITOR_SERVER_PORT;

    /* Register callback when clients connect to the server */
    espconn_regist_connectcb(mCon, tcp_monitor_connected_cb);

    /* Put the connection in accept mode */
    espconn_accept(mCon);
#endif


    // Now start the STA-Mode
    user_set_station_config();

    //Start task
    system_os_task(user_procTask, user_procTaskPrio,user_procTaskQueue, user_procTaskQueueLen);
}
