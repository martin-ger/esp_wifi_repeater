#include "c_types.h"
#include "mem.h"
#include "ets_sys.h"
#include "osapi.h"
#include "gpio.h"
#include "os_type.h"
#include "lwip/ip.h"
#include "lwip/netif.h"
#include "lwip/dns.h"
#include "lwip/lwip_napt.h"
#include "lwip/app/dhcpserver.h"
#include "lwip/app/espconn.h"
#include "lwip/app/espconn_tcp.h"

#include "user_interface.h"
#include "string.h"
#include "driver/uart.h"

#include "ringbuf.h"
#include "user_config.h"
#include "config_flash.h"
#include "sys_time.h"

#include "easygpio.h"

#ifdef WEB_CONFIG
#include "web.h"
#endif

#ifdef ACLS
#include "acl.h"
#endif

#ifdef REMOTE_MONITORING
#include "pcap.h"
#endif

#ifdef MQTT_CLIENT
#include "mqtt.h"
#endif

uint32_t readvdd33(void);
uint32_t Vdd;

/* System Task, for signals refer to user_config.h */
#define user_procTaskPrio        0
#define user_procTaskQueueLen    1
os_event_t    user_procTaskQueue[user_procTaskQueueLen];
static void user_procTask(os_event_t *events);

static os_timer_t ptimer;	

/* Some stats */
uint64_t Bytes_in, Bytes_out, Bytes_in_last, Bytes_out_last;
uint32_t Packets_in, Packets_out, Packets_in_last, Packets_out_last;
uint64_t t_old;

#ifdef TOKENBUCKET
uint64_t t_old_tb;
uint32_t token_bucket_ds, token_bucket_us;
#endif

/* Hold the system wide configuration */
sysconfig_t config;

static ringbuf_t console_rx_buffer, console_tx_buffer;

static ip_addr_t my_ip;
static ip_addr_t dns_ip;
bool connected;
uint8_t my_channel;
bool do_ip_config;

static netif_input_fn orig_input_ap;
static netif_linkoutput_fn orig_output_ap;

uint8_t remote_console_disconnect;

void ICACHE_FLASH_ATTR user_set_softap_wifi_config(void);
void ICACHE_FLASH_ATTR user_set_softap_ip_config(void);

#ifdef MQTT_CLIENT

#define MQTT_TOPIC_RESPONSE	0x0001
#define MQTT_TOPIC_IP		0x0002
#define MQTT_TOPIC_SCANRESULT	0x0004
#define MQTT_TOPIC_JOIN		0x0008
#define MQTT_TOPIC_LEAVE	0x0010
#define MQTT_TOPIC_UPTIME	0x0020
#define MQTT_TOPIC_VDD		0x0040
#define MQTT_TOPIC_ACLDENY	0x0080
#define MQTT_TOPIC_BYTES	0x0100
#define MQTT_TOPIC_PIN		0x0200
#define MQTT_TOPIC_POUT		0x0400
#define MQTT_TOPIC_BPSIN	0x0800
#define MQTT_TOPIC_BPSOUT	0x1000
#define MQTT_TOPIC_NOSTATIONS	0x2000
#define MQTT_TOPIC_GPIOIN	0x4000
#define MQTT_TOPIC_GPIOOUT	0x8000

MQTT_Client mqttClient;
bool mqtt_enabled;

void ICACHE_FLASH_ATTR mqtt_publish_str(uint16_t mask, uint8_t *sub_topic, uint8_t *str)
{
uint8_t buf[256];
  if (!mqtt_enabled || (config.mqtt_topic_mask & mask) == 0) return;

  os_sprintf(buf, "%s/%s", config.mqtt_prefix, sub_topic);
//os_printf("Publish: %s %s\r\n", buf, str);
  MQTT_Publish(&mqttClient, buf, str, os_strlen(str), 0, 0);
}

void ICACHE_FLASH_ATTR mqtt_publish_int(uint16_t mask, uint8_t *sub_topic, uint8_t *format, uint32_t val)
{
uint8_t buf[32];
  if (!mqtt_enabled || (config.mqtt_topic_mask & mask) == 0) return;

  os_sprintf(buf, format, val);
  mqtt_publish_str(mask, sub_topic, buf);
}

static void ICACHE_FLASH_ATTR mqttConnectedCb(uint32_t *args)
{
uint8_t ip_str[16];

  MQTT_Client* client = (MQTT_Client*)args;
  os_printf("MQTT: Connected\r\n");

  os_sprintf(ip_str, IPSTR, IP2STR(&my_ip));
  mqtt_publish_str(MQTT_TOPIC_IP, "IP", ip_str);

  if (os_strcmp(config.mqtt_command_topic, "none") != 0) {
    MQTT_Subscribe(client, config.mqtt_command_topic, 0);
  }
#ifdef USER_GPIO_OUT
  if (os_strcmp(config.mqtt_command_topic, "none") != 0) {
    MQTT_Subscribe(client, config.mqtt_gpio_out_topic, 0);
  }
#endif
}

static void ICACHE_FLASH_ATTR mqttDisconnectedCb(uint32_t *args)
{
  MQTT_Client* client = (MQTT_Client*)args;
  os_printf("MQTT: Disconnected\r\n");
}

static void ICACHE_FLASH_ATTR mqttPublishedCb(uint32_t *args)
{
  MQTT_Client* client = (MQTT_Client*)args;
//  os_printf("MQTT: Published\r\n");
}

static void ICACHE_FLASH_ATTR mqttDataCb(uint32_t *args, const char* topic, uint32_t topic_len, const char *data, uint32_t data_len)
{
  MQTT_Client* client = (MQTT_Client*)args;

  if (topic_len == os_strlen(config.mqtt_command_topic) && os_strncmp(topic, config.mqtt_command_topic, topic_len) == 0) {
    ringbuf_memcpy_into(console_rx_buffer, data, data_len);
    ringbuf_memcpy_into(console_rx_buffer, "\n", 1);
    // signal the main task that command is available for processing
    system_os_post(0, SIG_CONSOLE_RX, 0);
    return;
  }
#ifdef USER_GPIO_OUT
  if (topic_len == os_strlen(config.mqtt_gpio_out_topic) && os_strncmp(topic, config.mqtt_gpio_out_topic, topic_len) == 0) {
    if (data_len > 0 && data[0] == '0')
	config.gpio_out_status = 0;
    else
	config.gpio_out_status = 1;
    easygpio_outputSet(USER_GPIO_OUT, config.gpio_out_status);
    mqtt_publish_int(MQTT_TOPIC_GPIOOUT, "GpioOut", "%d", (uint32_t)config.gpio_out_status);
    return;
  }
#endif
}
#endif /* MQTT_CLIENT */


#ifdef REMOTE_MONITORING
static uint8_t monitoring_on;
static uint16_t monitor_port;
static ringbuf_t pcap_buffer;
struct espconn *cur_mon_conn;
static uint8_t monitoring_send_ongoing;
static uint8_t acl_monitoring;

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


static void ICACHE_FLASH_ATTR start_monitor(uint16_t portno)
{
    if (monitoring_on) return;

    pcap_buffer = ringbuf_new(MONITOR_BUFFER_SIZE);
    monitoring_send_ongoing = 0;

    os_printf("Starting Monitor TCP Server on %d port\r\n", portno);
    struct espconn *mCon = (struct espconn *)os_zalloc(sizeof(struct espconn));
    if (mCon == NULL) {
        os_printf("CONNECT FAIL\r\n");
        return;
    }

    /* Equivalent to bind */
    mCon->type  = ESPCONN_TCP;
    mCon->state = ESPCONN_NONE;
    mCon->proto.tcp = (esp_tcp *)os_zalloc(sizeof(esp_tcp));
    mCon->proto.tcp->local_port = portno;

    /* Register callback when clients connect to the server */
    espconn_regist_connectcb(mCon, tcp_monitor_connected_cb);

    /* Put the connection in accept mode */
    espconn_accept(mCon);
}

static void ICACHE_FLASH_ATTR stop_monitor(void)
{
    if (monitoring_on = 1) {
	os_printf("Stopping Monitor TCP Server\r\n");
	espconn_disconnect(cur_mon_conn);
    }

    monitoring_on = 0;
    monitor_port = 0;
    ringbuf_free(&pcap_buffer);
}

int ICACHE_FLASH_ATTR put_packet_to_ringbuf(struct pbuf *p) {
  struct pcap_pkthdr pcap_phdr;
  uint64_t t_usecs;
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
       t_usecs = get_long_systime();
       pcap_phdr.ts_sec = (uint32_t)t_usecs/1000000;
       pcap_phdr.ts_usec = (uint32_t)t_usecs%1000000;
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
#endif /* REMOTE_MONITORING */

err_t ICACHE_FLASH_ATTR my_input_ap (struct pbuf *p, struct netif *inp) {

//  os_printf("Got packet from STA\r\n");

    if (config.status_led <= 16)
	GPIO_OUTPUT_SET (config.status_led, 1);

#ifdef ACLS
    // Check ACLs - store result
    uint8_t acl_check = ACL_ALLOW;
    if (!acl_is_empty(0))
       acl_check = acl_check_packet(0, p);
#endif

#ifdef REMOTE_MONITORING
    if (monitoring_on && !acl_monitoring) {
       if (put_packet_to_ringbuf(p) != 0) {
#ifdef DROP_PACKET_IF_NOT_RECORDED
               pbuf_free(p);
	       return;
#endif
       }
       if (!monitoring_send_ongoing)
	       tcp_monitor_sent_cb(cur_mon_conn);
    }
#ifdef ACLS
    // Check if packet should be monitored by ACL
    if (monitoring_on && acl_monitoring && (acl_check&ACL_MONITOR)) {
       put_packet_to_ringbuf(p);
       if (!monitoring_send_ongoing)
	       tcp_monitor_sent_cb(cur_mon_conn);
    }
#endif
#endif /* REMOTE_MONITORING */

#ifdef ACLS
    // If not allowed, drop packet
    if (!(acl_check&ACL_ALLOW)) {
	pbuf_free(p);
	return;
    };
#endif

#ifdef TOKENBUCKET
    if (config.kbps_us != 0) {
        if (p->tot_len <= token_bucket_us) {
	    token_bucket_us -= p->tot_len;
        } else {
	    pbuf_free(p);
	    return;
	}
    }
#endif

    Bytes_in += p->tot_len;
    Packets_in++;

    orig_input_ap (p, inp);
}

err_t ICACHE_FLASH_ATTR my_output_ap (struct netif *outp, struct pbuf *p) {

//  os_printf("Send packet to STA\r\n");

    if (config.status_led <= 16)
	GPIO_OUTPUT_SET (config.status_led, 0);

#ifdef ACLS
    // Check ACLs - store result
    uint8_t acl_check = ACL_ALLOW;
    if (!acl_is_empty(1))
       acl_check = acl_check_packet(1, p);
#endif

#ifdef REMOTE_MONITORING
    if (monitoring_on && !acl_monitoring) {
       if (put_packet_to_ringbuf(p) != 0) {
#ifdef DROP_PACKET_IF_NOT_RECORDED
               pbuf_free(p);
	       return;
#endif
       }
       if (!monitoring_send_ongoing)
	       tcp_monitor_sent_cb(cur_mon_conn);
    }

#ifdef ACLS
    // Check if packet should be monitored by ACL
    if (monitoring_on && acl_monitoring && (acl_check&ACL_MONITOR)) {
       put_packet_to_ringbuf(p);
       if (!monitoring_send_ongoing)
	       tcp_monitor_sent_cb(cur_mon_conn);
    }
#endif
#endif /* REMOTE_MONITORING */

#ifdef ACLS
    // If not allowed, drop packet
    if (!(acl_check&ACL_ALLOW)) {
	pbuf_free(p);
	return;
    };
#endif

#ifdef TOKENBUCKET
    if (config.kbps_ds != 0) {
        if (p->tot_len <= token_bucket_ds) {
	    token_bucket_ds -= p->tot_len;
        } else {
	    pbuf_free(p);
	    return;
	}
    }
#endif

    Bytes_out += p->tot_len;
    Packets_out++;

    orig_output_ap (outp, p);
}


static void ICACHE_FLASH_ATTR patch_netif_ap(netif_input_fn ifn, netif_linkoutput_fn ofn, bool nat)
{	
struct netif *nif;
ip_addr_t ap_ip;

	ap_ip = config.network_addr;
	ip4_addr4(&ap_ip) = 1;
	
	for (nif = netif_list; nif != NULL && nif->ip_addr.addr != ap_ip.addr; nif = nif->next);
	if (nif == NULL) return;

	nif->napt = nat?1:0;
	if (nif->input != ifn) {
	  orig_input_ap = nif->input;
	  nif->input = ifn;
	}
	if (nif->linkoutput != ofn) {
	  orig_output_ap = nif->linkoutput;
	  nif->linkoutput = ofn;
	}
}


int ICACHE_FLASH_ATTR parse_str_into_tokens(char *str, char **tokens, int max_tokens)
{
char    *p, *q, *end;
int     token_count = 0;
bool    in_token = false;

   // preprocessing
   for (p = q = str; *p != 0; p++) {
	if (*(p) == '%' && *(p+1) != 0 && *(p+2) != 0) {
	   // quoted hex
		uint8_t a;
		p++;
		if (*p <= '9')
		    a = *p - '0';
		else
		    a = toupper(*p) - 'A' + 10;
		a <<= 4;
		p++;
		if (*p <= '9')
		    a += *p - '0';
		else
		    a += toupper(*p) - 'A' + 10;
		*q++ = a;
	} else if (*p == '\\' && *(p+1) != 0) {
	   // next char is quoted - just copy it, skip this one
	   *q++ = *++p;
	} else if (*p == 8) {
	   // backspace - delete previous char
	   if (q != str) q--;
	} else if (*p <= ' ') {
	   // mark this as whitespace
	   *q++ = 0;
	} else {
	   *q++ = *p;
	}
   }

   end = q;
   *q = 0;

   // cut into tokens
   for (p = str; p != end; p++) {
	if (*p == 0) {
	   if (in_token) {
		in_token = false;
	   }
	} else {
	   if (!in_token) {
		tokens[token_count++] = p;
		if (token_count == max_tokens)
		   return token_count;
		in_token = true;
	   }  
	}
   }
   return token_count;
}


void console_send_response(struct espconn *pespconn)
{
    char payload[MAX_CON_SEND_SIZE+4];
    uint16_t len = ringbuf_bytes_used(console_tx_buffer);

    ringbuf_memcpy_from(payload, console_tx_buffer, len);
#ifdef MQTT_CLIENT
    payload[len] = 0;
    if (os_strcmp(config.mqtt_command_topic, "none") != 0) {
	mqtt_publish_str(MQTT_TOPIC_RESPONSE, "response", payload);
    }
#endif
    os_memcpy(&payload[len], "CMD>", 4);

    if (pespconn != NULL)
	espconn_sent(pespconn, payload, len+4);
    else
	UART_Send(0, &payload, len+4);
}


#ifdef ALLOW_SCANNING
struct espconn *scanconn;
void ICACHE_FLASH_ATTR scan_done(void *arg, STATUS status)
{
  char response[128];

  if (status == OK)
  {
    struct bss_info *bss_link = (struct bss_info *)arg;

    ringbuf_memcpy_into(console_tx_buffer, "\r", 1);
    while (bss_link != NULL)
    {
      os_sprintf(response, "%d,\"%s\",%d,\""MACSTR"\",%d\r\n",
                 bss_link->authmode, bss_link->ssid, bss_link->rssi,
                 MAC2STR(bss_link->bssid),bss_link->channel);
      ringbuf_memcpy_into(console_tx_buffer, response, os_strlen(response));
#ifdef MQTT_CLIENT
      mqtt_publish_str(MQTT_TOPIC_SCANRESULT, "ScanResult", response);
#endif
      bss_link = bss_link->next.stqe_next;
    }
  }
  else
  {
     os_sprintf(response, "scan fail !!!\r\n");
     ringbuf_memcpy_into(console_tx_buffer, response, os_strlen(response));
  }
  system_os_post(0, SIG_CONSOLE_TX, (ETSParam) scanconn);
}
#endif

#ifdef ACLS
void ICACHE_FLASH_ATTR parse_IP_addr(uint8_t *str, uint32_t *addr, uint32_t *mask)
{
int i;
uint32_t net;
    if (strcmp(str, "any") == 0) {
	*addr = 0;
	*mask = 0;
	return;
    }

    for(i=0; str[i]!=0 && str[i]!='/'; i++);

    *mask = 0xffffffff;
    if (str[i]!=0) {
	str[i]=0;
	*mask <<= (32 - atoi(&str[i+1]));
    }
    *mask = htonl(*mask);
    *addr = ipaddr_addr(str);
}

struct espconn *deny_cb_conn = 0;
uint8_t acl_debug = 0;

uint8_t acl_deny_cb(uint8_t proto, uint32_t saddr, uint16_t s_port, uint32_t daddr, uint16_t d_port, uint8_t allow)
{
char response[128];

    if (!acl_debug 
#ifdef MQTT_CLIENT
	&& !mqtt_enabled
#endif
	) return allow;

    os_sprintf(response, "\rdeny: %s Src: %d.%d.%d.%d:%d Dst: %d.%d.%d.%d:%d\r\n", 
	proto==IP_PROTO_TCP?"TCP":proto==IP_PROTO_UDP?"UDP":"IP4",
	IP2STR((ip_addr_t *)&saddr), s_port, IP2STR((ip_addr_t *)&daddr), d_port);

#ifdef MQTT_CLIENT
    mqtt_publish_str(MQTT_TOPIC_ACLDENY, "ACLDeny", response);
#endif
    if (acl_debug) {
	ringbuf_memcpy_into(console_tx_buffer, response, os_strlen(response));
	system_os_post(0, SIG_CONSOLE_TX, (ETSParam) deny_cb_conn);
    }
    return allow;
}
#endif /* ACLS */

bool parse_mac(uint8_t *mac, uint8_t *inp)
{
int i;

    if (os_strlen(inp) != 17) return false;
    for (i=0; i<17; i++) {
	if (inp[i] == ':') continue;
	inp[i] = toupper(inp[i]);
        inp[i] = inp[i] <= '9'? inp[i]-'0' : (inp[i]-'A')+10;
	if (inp[i] >= 16) return false;
    }

    for (i=0; i<17; i+=3) {
	*mac++ = inp[i]*16+inp[i+1];
    }
    return true;
}

static char INVALID_LOCKED[] = "Invalid command. Config locked\r\n";
static char INVALID_NUMARGS[] = "Invalid number of arguments\r\n";
static char INVALID_ARG[] = "Invalid argument\r\n";

void ICACHE_FLASH_ATTR console_handle_command(struct espconn *pespconn)
{
#define MAX_CMD_TOKENS 9

    char cmd_line[MAX_CON_CMD_SIZE+1];
    char response[256];
    char *tokens[MAX_CMD_TOKENS];

    int bytes_count, nTokens;

    bytes_count = ringbuf_bytes_used(console_rx_buffer);
    ringbuf_memcpy_from(cmd_line, console_rx_buffer, bytes_count);

    cmd_line[bytes_count] = 0;
    response[0] = 0;

    nTokens = parse_str_into_tokens(cmd_line, tokens, MAX_CMD_TOKENS);

    if (nTokens == 0) {
	char c = '\n';
	ringbuf_memcpy_into(console_tx_buffer, &c, 1);
	goto command_handled_2;
    }

    if (strcmp(tokens[0], "help") == 0)
    {
        os_sprintf(response, "show [config|stats%s]\r\n|set [ssid|password|auto_connect|ap_ssid|ap_password|network|dns|ip|netmask|gw|ap_mac|sta_mac|ap_on|ap_open|ssid_hidden|vmin|vmin_sleep|speed|status_led|config_port|config_access|web_port] <val>\r\n|portmap [add|remove] [TCP|UDP] <ext_port> <int_addr> <int_port>\r\n|quit|save [config|dhcp]|reset [factory]|lock|unlock <password>",
#ifdef MQTT_CLIENT
		"|mqtt"
#else
		""
#endif
	);
        ringbuf_memcpy_into(console_tx_buffer, response, os_strlen(response));
#ifdef ALLOW_SCANNING
        os_sprintf(response, "|scan");
        ringbuf_memcpy_into(console_tx_buffer, response, os_strlen(response));
#endif
#ifdef PHY_MODE
        os_sprintf(response, "|set phy_mode [1|2|3]");
        ringbuf_memcpy_into(console_tx_buffer, response, os_strlen(response));
#endif
#ifdef ALLOW_SLEEP
        os_sprintf(response, "|sleep");
        ringbuf_memcpy_into(console_tx_buffer, response, os_strlen(response));
#endif
#ifdef REMOTE_MONITORING
        os_sprintf(response, "|monitor [on|off] <portnumber>");
        ringbuf_memcpy_into(console_tx_buffer, response, os_strlen(response));
#endif
	ringbuf_memcpy_into(console_tx_buffer, "\r\n", 2);
#ifdef TOKENBUCKET
        os_sprintf(response, "|set [upstream_kbps|downstream_kbps] <val>\r\n");
        ringbuf_memcpy_into(console_tx_buffer, response, os_strlen(response));
#endif
#ifdef ACLS
        os_sprintf(response, "|acl [from_sta|to_sta] clear\r\n|acl [from_sta|to_sta] [IP|TCP|UDP] <src_addr> [<src_port>] <dest_addr> [<dest_port>] [allow|deny|allow_monitor|deny_monitor]\r\n");
        ringbuf_memcpy_into(console_tx_buffer, response, os_strlen(response));
#endif
#ifdef MQTT_CLIENT
        os_sprintf(response, "|set [mqtt_host|mqtt_port|mqtt_user|mqtt_password|mqtt_id|mqtt_prefix|mqtt_command_topic|mqtt_interval] <val>\r\n");
        ringbuf_memcpy_into(console_tx_buffer, response, os_strlen(response));
#endif
        goto command_handled_2;
    }

    if (strcmp(tokens[0], "show") == 0)
    {
      int16_t i;
      struct portmap_table *p;
      ip_addr_t i_ip;

      if (nTokens == 1 || (nTokens == 2 && strcmp(tokens[1], "config") == 0)) {
        os_sprintf(response, "STA: SSID:%s PW:%s%s\r\n",
                   config.ssid,
                   config.locked?"***":(char*)config.password,
                   config.auto_connect?"":" [AutoConnect:0]");
        ringbuf_memcpy_into(console_tx_buffer, response, os_strlen(response));
	if (*(int*)config.bssid != 0) {
		os_sprintf(response, "BSSID: %02x:%02x:%02x:%02x:%02x:%02x\r\n", 
		    config.bssid[0], config.bssid[1], config.bssid[2],
		    config.bssid[3], config.bssid[4], config.bssid[5]);
		ringbuf_memcpy_into(console_tx_buffer, response, os_strlen(response));
	}
        os_sprintf(response, "AP:  SSID:%s %s PW:%s%s%s IP:%d.%d.%d.%d/24",
                   config.ap_ssid,
		   config.ssid_hidden?"[hidden]":"",
                   config.locked?"***":(char*)config.ap_password,
                   config.ap_open?" [open]":"",
                   config.ap_on?"":" [disabled]",
		   IP2STR(&config.network_addr));
        ringbuf_memcpy_into(console_tx_buffer, response, os_strlen(response));
	// if static DNS, add it
	os_sprintf(response, config.dns_addr.addr?" DNS: %d.%d.%d.%d\r\n":"\r\n", IP2STR(&config.dns_addr));
        ringbuf_memcpy_into(console_tx_buffer, response, os_strlen(response));
	// if static IP, add it
	os_sprintf(response, config.my_addr.addr?"Static IP: %d.%d.%d.%d Netmask: %d.%d.%d.%d Gateway: %d.%d.%d.%d\r\n":"", 
		IP2STR(&config.my_addr), IP2STR(&config.my_netmask), IP2STR(&config.my_gw));
        ringbuf_memcpy_into(console_tx_buffer, response, os_strlen(response));

	os_sprintf(response, "STA MAC: %02x:%02x:%02x:%02x:%02x:%02x\r\nAP MAC:  %02x:%02x:%02x:%02x:%02x:%02x\r\n",  
		   config.STA_MAC_address[0], config.STA_MAC_address[1], config.STA_MAC_address[2],
		   config.STA_MAC_address[3], config.STA_MAC_address[4], config.STA_MAC_address[5],
		   config.AP_MAC_address[0], config.AP_MAC_address[1], config.AP_MAC_address[2],
		   config.AP_MAC_address[3], config.AP_MAC_address[4], config.AP_MAC_address[5]);
	ringbuf_memcpy_into(console_tx_buffer, response, os_strlen(response));

#ifdef REMOTE_CONFIG
	if (config.config_port == 0 || config.config_access == 0) {
		os_sprintf(response, "No network console access\r\n");
	} else {
		os_sprintf(response, "Network console access on port %d (mode %d)\r\n", config.config_port, config.config_access);
	}
	ringbuf_memcpy_into(console_tx_buffer, response, os_strlen(response));
#endif

        os_sprintf(response, "Clock speed: %d\r\n", config.clock_speed);
        ringbuf_memcpy_into(console_tx_buffer, response, os_strlen(response));
#ifdef TOKENBUCKET
	if (config.kbps_ds != 0) {
            os_sprintf(response, "Downstream limit: %d kbps\r\n", config.kbps_ds);
            ringbuf_memcpy_into(console_tx_buffer, response, os_strlen(response));
	}
	if (config.kbps_us != 0) {
            os_sprintf(response, "Upstream limit: %d kbps\r\n", config.kbps_us);
            ringbuf_memcpy_into(console_tx_buffer, response, os_strlen(response));
	}
#endif
#ifdef MQTT_CLIENT
        os_sprintf(response, "MQTT: %s\r\n", mqtt_enabled?"enabled":"disabled");
        ringbuf_memcpy_into(console_tx_buffer, response, os_strlen(response));      
#endif
#ifdef ALLOW_SLEEP
	if (config.Vmin != 0) {
            os_sprintf(response, "Vmin: %d mV Sleep time: %d s\r\n", config.Vmin, config.Vmin_sleep);
            ringbuf_memcpy_into(console_tx_buffer, response, os_strlen(response));
	}
#endif
	for (i = 0; i<IP_PORTMAP_MAX; i++) {
	    p = &ip_portmap_table[i];
	    if(p->valid) {
		i_ip.addr = p->daddr;
		os_sprintf(response, "Portmap: %s: " IPSTR ":%d -> "  IPSTR ":%d\r\n", 
		   p->proto==IP_PROTO_TCP?"TCP":p->proto==IP_PROTO_UDP?"UDP":"???",
		   IP2STR(&my_ip), ntohs(p->mport), IP2STR(&i_ip), ntohs(p->dport));
		ringbuf_memcpy_into(console_tx_buffer, response, os_strlen(response));
	    }
	}
#ifdef REMOTE_MONITORING
	if (!config.locked&&monitor_port != 0) {
           	os_sprintf(response, "Monitor (mode %s) started on port %d\r\n", acl_monitoring?"acl":"all", monitor_port);
		ringbuf_memcpy_into(console_tx_buffer, response, os_strlen(response));
	}
#endif
	goto command_handled_2;
      }

      if (nTokens == 2 && strcmp(tokens[1], "stats") == 0) {
           uint32_t time = (uint32_t)(get_long_systime()/1000000);
	   int16_t i;
	   enum phy_mode phy;
	   struct dhcps_pool *p;

           os_sprintf(response, "System uptime: %d:%02d:%02d\r\nPower supply: %d.%03d V\r\n", 
	      time/3600, (time%3600)/60, time%60, Vdd/1000, Vdd%1000);
	   ringbuf_memcpy_into(console_tx_buffer, response, os_strlen(response));
#ifdef USER_GPIO_OUT
	   os_sprintf(response, "GPIO output status: %d\r\n", config.gpio_out_status);
           ringbuf_memcpy_into(console_tx_buffer, response, os_strlen(response));
#endif
	   os_sprintf(response, "%d KiB in (%d packets)\r\n%d KiB out (%d packets)\r\n", 
			(uint32_t)(Bytes_in/1024), Packets_in, 
			(uint32_t)(Bytes_out/1024), Packets_out);
           ringbuf_memcpy_into(console_tx_buffer, response, os_strlen(response));
#ifdef PHY_MODE
	   phy = wifi_get_phy_mode();
	   os_sprintf(response, "Phy mode: %c\r\n", phy == PHY_MODE_11B?'b':phy == PHY_MODE_11G?'g':'n');
           ringbuf_memcpy_into(console_tx_buffer, response, os_strlen(response));
#endif
	   if (connected) {
		os_sprintf(response, "External IP-address: " IPSTR "\r\n", IP2STR(&my_ip));
	   } else {
		os_sprintf(response, "Not connected to AP\r\n");
	   }
	   ringbuf_memcpy_into(console_tx_buffer, response, os_strlen(response));
	   if (config.ap_on)
		os_sprintf(response, "%d Station%s connected to AP\r\n", wifi_softap_get_station_num(),
		  wifi_softap_get_station_num()==1?"":"s");
	   else
		os_sprintf(response, "AP disabled\r\n");
           ringbuf_memcpy_into(console_tx_buffer, response, os_strlen(response));
           for (i = 0; p = dhcps_get_mapping(i); i++) {
		os_sprintf(response, "Station: %02x:%02x:%02x:%02x:%02x:%02x - "  IPSTR "\r\n", 
		   p->mac[0], p->mac[1], p->mac[2], p->mac[3], p->mac[4], p->mac[5], 
		   IP2STR(&p->ip));
		ringbuf_memcpy_into(console_tx_buffer, response, os_strlen(response));
	   }
	   goto command_handled_2;
      }
#ifdef ACLS
      if (nTokens == 2 && strcmp(tokens[1], "acl") == 0) {
	   if (!acl_is_empty(0)) {
		ringbuf_memcpy_into(console_tx_buffer, "From STA:\r\n", 11);
		acl_show(0, response);
		ringbuf_memcpy_into(console_tx_buffer, response, os_strlen(response));
	   }
	   if (!acl_is_empty(1)) {
		ringbuf_memcpy_into(console_tx_buffer, "To STA:\r\n", 9);
		acl_show(1, response);
		ringbuf_memcpy_into(console_tx_buffer, response, os_strlen(response));
	   }
	   os_sprintf(response, "Packets denied: %d Packets allowed: %d\r\n", 
			acl_deny_count, acl_allow_count);
	   ringbuf_memcpy_into(console_tx_buffer, response, os_strlen(response));
	   goto command_handled_2;
      }
#endif
#ifdef MQTT_CLIENT
      if (nTokens == 2 && strcmp(tokens[1], "mqtt") == 0) {
	   if (os_strcmp(config.mqtt_host, "none") == 0) {
	     os_sprintf(response, "MQTT not enabled (no mqtt_host)\r\n");
	     ringbuf_memcpy_into(console_tx_buffer, response, os_strlen(response));
	     goto command_handled_2;
	   }
           os_sprintf(response, "MQTT host: %s\r\nMQTT port: %d\r\nMQTT user: %s\r\nMQTT password: %s\r\n",
		config.mqtt_host, config.mqtt_port, config.mqtt_user, config.locked?"***":(char*)config.mqtt_password);
	   ringbuf_memcpy_into(console_tx_buffer, response, os_strlen(response));
           os_sprintf(response, "MQTT id: %s\r\nMQTT prefix: %s\r\nMQTT command topic: %s\r\nMQTT gpio_out topic: %s\r\nMQTT interval: %d s\r\nMQTT mask: %04x\r\n",
		config.mqtt_id, config.mqtt_prefix, config.mqtt_command_topic, config.mqtt_gpio_out_topic, config.mqtt_interval, config.mqtt_topic_mask);
	   ringbuf_memcpy_into(console_tx_buffer, response, os_strlen(response));

	   goto command_handled_2;
      }
#endif
    }
#ifdef ACLS
    if (strcmp(tokens[0], "acl") == 0)
    {
    uint8_t acl_no;
    uint8_t proto;
    uint32_t saddr;
    uint32_t smask;
    uint16_t sport;
    uint32_t daddr;
    uint32_t dmask;
    uint16_t dport;
    uint8_t allow;
    uint8_t last_arg;

        if (config.locked)
        {
            os_sprintf(response, INVALID_LOCKED);
            goto command_handled;
        }

        if (nTokens < 3)
        {
            os_sprintf(response, INVALID_NUMARGS);
	    goto command_handled;
        }

        if (strcmp(tokens[1],"from_sta")==0)
	    acl_no = 0;
        else if (strcmp(tokens[1],"to_sta")==0)
	    acl_no = 1;
	else {
	    os_sprintf(response, INVALID_ARG);
	    goto command_handled;
	}

	if (strcmp(tokens[2],"clear")==0) {
	    acl_clear(acl_no);
	    os_sprintf(response, "ACL cleared\r\n");
	    goto command_handled;
	}

	last_arg = 7;	
	if (strcmp(tokens[2],"IP") == 0) {
	    proto = 0;
	    last_arg = 5;
	}
	else if (strcmp(tokens[2],"TCP") == 0) proto = IP_PROTO_TCP;
	else if (strcmp(tokens[2],"UDP") == 0) proto = IP_PROTO_UDP;
        else {
	    os_sprintf(response, INVALID_ARG);
	    goto command_handled;
	}

        if (nTokens != last_arg+1)
        {
            os_sprintf(response, INVALID_NUMARGS);
	    goto command_handled;
        }

	if (proto == 0) {
	    parse_IP_addr(tokens[3], &saddr, &smask);
	    parse_IP_addr(tokens[4], &daddr, &dmask);
	    sport = dport = 0;
	} else {
	    parse_IP_addr(tokens[3], &saddr, &smask);
            sport = (uint16_t)atoi(tokens[4]);
	    parse_IP_addr(tokens[5], &daddr, &dmask);
            dport = (uint16_t)atoi(tokens[6]);
	}

	if (strcmp(tokens[last_arg],"allow") == 0) allow = ACL_ALLOW;
	else if (strcmp(tokens[last_arg],"deny") == 0) allow = ACL_DENY;
#ifdef REMOTE_MONITORING
	else if (strcmp(tokens[last_arg],"allow_monitor") == 0) allow = ACL_ALLOW|ACL_MONITOR;
	else if (strcmp(tokens[last_arg],"deny_monitor") == 0) allow = ACL_DENY|ACL_MONITOR;
#endif
        else {
	    os_sprintf(response, INVALID_ARG);
	    goto command_handled;
	}

	if (acl_add(acl_no, saddr, smask, daddr, dmask, proto, sport, dport, allow)) {
	    os_sprintf(response, "ACL added\r\n");
	} else {
	    os_sprintf(response, "ACL add failed\r\n");
	}
        goto command_handled;
    }
#endif /* ACLS */

    if (strcmp(tokens[0], "portmap") == 0)
    {
    uint32_t daddr;
    uint16_t mport;
    uint16_t dport;
    uint8_t proto;
    bool add;
    uint8_t retval;

        if (config.locked)
        {
            os_sprintf(response, INVALID_LOCKED);
            goto command_handled;
        }

        if (nTokens < 4 || (strcmp(tokens[1],"add")==0 && nTokens != 6))
        {
            os_sprintf(response, INVALID_NUMARGS);
	    goto command_handled;
        }

        add = strcmp(tokens[1],"add")==0;
	if (!add && strcmp(tokens[1],"remove")!=0) {
	    os_sprintf(response, INVALID_ARG);
	    goto command_handled;
	}

	if (strcmp(tokens[2],"TCP") == 0) proto = IP_PROTO_TCP;
	else if (strcmp(tokens[2],"UDP") == 0) proto = IP_PROTO_UDP;
        else {
	    os_sprintf(response, INVALID_ARG);
	    goto command_handled;
	}

        mport = (uint16_t)atoi(tokens[3]);
	if (add) {
	    daddr = ipaddr_addr(tokens[4]);
            dport = atoi(tokens[5]);
	    retval = ip_portmap_add(proto, my_ip.addr, mport, daddr, dport);
	} else {
            retval = ip_portmap_remove(proto, mport);
	}

	if (retval) {
	    os_sprintf(response, "Portmap %s\r\n", add?"set":"deleted");
	} else {
	    os_sprintf(response, "Portmap failed\r\n");
	}
        goto command_handled;
    }


    if (strcmp(tokens[0], "save") == 0)
    {
      if (config.locked) {
        os_sprintf(response, INVALID_LOCKED);
        goto command_handled;
      }

      if (nTokens == 1 || (nTokens == 2 && strcmp(tokens[1], "config") == 0)) {
        config_save(&config);
	// also save the portmap table
	blob_save(0, (uint32_t *)ip_portmap_table, sizeof(struct portmap_table) * IP_PORTMAP_MAX);
        os_sprintf(response, "Config saved\r\n");
        goto command_handled;
      }

      if (nTokens == 2 && strcmp(tokens[1], "dhcp") == 0) {
	int16_t i;
	struct dhcps_pool *p;

	for (i = 0; i<MAX_DHCP && (p = dhcps_get_mapping(i)); i++) {
	  os_memcpy(&config.dhcps_p[i], p, sizeof(struct dhcps_pool));
	}
	config.dhcps_entries = i;
        config_save(&config);
	// also save the portmap table
	blob_save(0, (uint32_t *)ip_portmap_table, sizeof(struct portmap_table) * IP_PORTMAP_MAX);
        os_sprintf(response, "Config and DHCP table saved\r\n");
        goto command_handled;
      }
    }
#ifdef ALLOW_SCANNING
    if (strcmp(tokens[0], "scan") == 0)
    {
        scanconn = pespconn;
        wifi_station_scan(NULL,scan_done);
        os_sprintf(response, "Scanning...\r\n");
        goto command_handled;
    }
#endif
    if (strcmp(tokens[0], "reset") == 0) {
	if (config.locked && pespconn != NULL) {
	    os_sprintf(response, INVALID_LOCKED);
	    goto command_handled;
	}
	if (nTokens == 2 && strcmp(tokens[1], "factory") == 0) {
           config_load_default(&config);
           config_save(&config);
	   // clear saved portmap table
	   blob_zero(0, sizeof(struct portmap_table) * IP_PORTMAP_MAX);
	}
        os_printf("Restarting ... \r\n");
	system_restart(); // if it works this will not return

	os_sprintf(response, "Reset failed\r\n");
        goto command_handled;
    }

    if (strcmp(tokens[0], "quit") == 0)
    {
	remote_console_disconnect = 1;
	os_sprintf(response, "Quitting console\r\n");
        goto command_handled;
    }
#ifdef ALLOW_SLEEP
    if (strcmp(tokens[0], "sleep") == 0)
    {
	uint32_t sleeptime = 10; // seconds
	if (nTokens == 2) sleeptime = atoi(tokens[1]);

	system_deep_sleep(sleeptime * 1000000);

	os_sprintf(response, "Going to deep sleep for %ds\r\n", sleeptime);
        goto command_handled;
    }
#endif
    if (strcmp(tokens[0], "lock") == 0) {
	if (config.locked) {
	    os_sprintf(response, "Config already locked\r\n");
	    goto command_handled;
	}
	if (nTokens == 1) {
	    if (os_strlen(config.lock_password) == 0) {
		os_sprintf(response, "No password defined\r\n");
		goto command_handled;
	    }
	}
	else if (nTokens == 2) {
	    os_sprintf(config.lock_password, "%s", tokens[1]);
	}
	else {
	    os_sprintf(response, INVALID_NUMARGS);
	    goto command_handled;
	}
	config.locked = 1;
	config_save(&config);
	os_sprintf(response, "Config locked (pw: %s)\r\n", config.lock_password);
	goto command_handled;
    }

    if (strcmp(tokens[0], "unlock") == 0) {
	if (nTokens != 2) {
	    os_sprintf(response, INVALID_NUMARGS);
	} else if (os_strcmp(tokens[1], config.lock_password) == 0) {
	    config.locked = 0;
	    config_save(&config);
	    os_sprintf(response, "Config unlocked\r\n");
	} else {
	    os_sprintf(response, "Unlock failed. Invalid password\r\n");
	}
	goto command_handled;
    }

#ifdef REMOTE_MONITORING
    if (strcmp(tokens[0],"monitor") == 0) {
        if (nTokens < 2) {
            os_sprintf(response, INVALID_NUMARGS);
	    goto command_handled;
        }
        if (config.locked) {
            os_sprintf(response, INVALID_LOCKED);
            goto command_handled;
        }
	if (strcmp(tokens[1],"on") == 0
#ifdef ACLS
	    || strcmp(tokens[1],"acl") == 0
#endif
					) {
  	    if (nTokens != 3) {
        	os_sprintf(response, "Port number missing\r\n");
		goto command_handled;
            }
	    if (monitor_port != 0) {
		os_sprintf(response, "Monitor already started\r\n");
		goto command_handled;
	    }
	    
            monitor_port = atoi(tokens[2]);
	    if (monitor_port != 0) {
#ifdef ACLS
		acl_monitoring = (strcmp(tokens[1],"acl") == 0);
#endif
		start_monitor(monitor_port);
		os_sprintf(response, "Started monitor on port %d\r\n", monitor_port);       
                goto command_handled;
            } else {
		os_sprintf(response, "Invalid monitor port\r\n");
		goto command_handled;
	    }
	}

	if (strcmp(tokens[1],"off") == 0) {
	    if (monitor_port == 0) {
		os_sprintf(response, "Monitor already stopped\r\n");
		goto command_handled;
	    }
	    monitor_port = 0;
	    stop_monitor();
	    os_sprintf(response, "Stopped monitor\r\n");
	    goto command_handled;
	}
    }
#endif

    if (strcmp(tokens[0], "set") == 0)
    {
        if (config.locked)
        {
            os_sprintf(response, INVALID_LOCKED);
            goto command_handled;
        }

        /*
         * For set commands atleast 2 tokens "set" "parameter" "value" is needed
         * hence the check
         */
        if (nTokens < 3)
        {
            os_sprintf(response, INVALID_NUMARGS);
            goto command_handled;
        }
        else
        {
            // atleast 3 tokens, proceed
            if (strcmp(tokens[1],"ssid") == 0)
            {
                os_sprintf(config.ssid, "%s", tokens[2]);
                os_sprintf(response, "SSID set\r\n");
                goto command_handled;
            }

            if (strcmp(tokens[1],"password") == 0)
            {
                os_sprintf(config.password, "%s", tokens[2]);

		// WiFi pw of the uplink network is also the default lock pw (backward compatibility)
		os_sprintf(config.lock_password, "%s", tokens[2]);

                os_sprintf(response, "Password set\r\n");
                goto command_handled;
            }

            if (strcmp(tokens[1],"auto_connect") == 0)
            {
                config.auto_connect = atoi(tokens[2]);
                os_sprintf(response, "Auto Connect set\r\n");
                goto command_handled;
            }

            if (strcmp(tokens[1],"ap_ssid") == 0)
            {
                os_sprintf(config.ap_ssid, "%s", tokens[2]);
                os_sprintf(response, "AP SSID set\r\n");
                goto command_handled;
            }

            if (strcmp(tokens[1],"ap_password") == 0)
            {
		if (os_strlen(tokens[2])<8) {
		    os_sprintf(response, "Password to short (min. 8)\r\n");
		} else {
                    os_sprintf(config.ap_password, "%s", tokens[2]);
		    config.ap_open = 0;
                    os_sprintf(response, "AP Password set\r\n");
		}
                goto command_handled;
            }

            if (strcmp(tokens[1],"ap_open") == 0)
            {
                config.ap_open = atoi(tokens[2]);
                os_sprintf(response, "Open Auth set\r\n");
                goto command_handled;
            }
            if (strcmp(tokens[1],"ssid_hidden") == 0)
            {
                config.ssid_hidden = atoi(tokens[2]);
                os_sprintf(response, "Hidden SSID set\r\n");
                goto command_handled;
            }
#ifdef ACLS
            if (strcmp(tokens[1],"acl_debug") == 0)
            {
		acl_debug = atoi(tokens[2]);
                os_sprintf(response, "ACL debug set\r\n");
                goto command_handled;
            }
#endif
#ifdef REMOTE_CONFIG
            if (strcmp(tokens[1],"config_port") == 0)
            {
                config.config_port = atoi(tokens[2]);
		if (config.config_port == 0) 
		  os_sprintf(response, "WARNING: if you save this, remote console access will be disabled!\r\n");
		else
		  os_sprintf(response, "Config port set to %d\r\n", config.config_port);
                goto command_handled;
            }
#endif
	    if (strcmp(tokens[1], "config_access") == 0) {
		config.config_access = atoi(tokens[2]) & (LOCAL_ACCESS | REMOTE_ACCESS);
		if (config.config_access == 0)
		    os_sprintf(response, "WARNING: if you save this, remote console and web access will be disabled!\r\n");
		else
		    os_sprintf(response, "Config access set\r\n", config.config_port);
		goto command_handled;
	    }

#ifdef WEB_CONFIG
            if (strcmp(tokens[1],"web_port") == 0)
            {
                config.web_port = atoi(tokens[2]);
		if (config.web_port == 0) 
		  os_sprintf(response, "WARNING: if you save this, web config will be disabled!\r\n");
		else
		  os_sprintf(response, "Web port set to %d\r\n", config.web_port);
                goto command_handled;
            }
#endif
#ifdef TOKENBUCKET
            if (strcmp(tokens[1],"downstream_kbps") == 0)
            {
                config.kbps_ds = atoi(tokens[2]);
                os_sprintf(response, "Bitrate set\r\n");
                goto command_handled;
            }
            if (strcmp(tokens[1],"upstream_kbps") == 0)
            {
                config.kbps_us = atoi(tokens[2]);
                os_sprintf(response, "Bitrate set\r\n");
                goto command_handled;
            }
#endif
#ifdef ALLOW_SLEEP
            if (strcmp(tokens[1],"vmin") == 0)
            {
                config.Vmin = atoi(tokens[2]);
                os_sprintf(response, "Vmin set\r\n");
                goto command_handled;
            }

            if (strcmp(tokens[1],"vmin_sleep") == 0)
            {
                config.Vmin_sleep = atoi(tokens[2]);
                os_sprintf(response, "Vmin sleep time set\r\n");
                goto command_handled;
            }
#endif
            if (strcmp(tokens[1],"ap_on") == 0)
            {
                if (atoi(tokens[2])) {
			if (!config.ap_on) {
				wifi_set_opmode(STATIONAP_MODE);
				user_set_softap_wifi_config();
				do_ip_config = true;
				config.ap_on = true;
	                	os_sprintf(response, "AP on\r\n");
			} else {
				os_sprintf(response, "AP already on\r\n");
			}

		} else {
			if (config.ap_on) {
				wifi_set_opmode(STATION_MODE);
				config.ap_on = false;
                		os_sprintf(response, "AP off\r\n");
			} else {
				os_sprintf(response, "AP already off\r\n");
			}
		}
                goto command_handled;
            }

	    if (strcmp(tokens[1], "speed") == 0)
	    {
		uint16_t speed = atoi(tokens[2]);
		bool succ = system_update_cpu_freq(speed);
		if (succ) 
		    config.clock_speed = speed;
		os_sprintf(response, "Clock speed update %s\r\n",
		  succ?"successful":"failed");
        	goto command_handled;
	    }

	    if (strcmp(tokens[1], "status_led") == 0)
	    {
	    	if (config.status_led <= 16) {
		    GPIO_OUTPUT_SET (config.status_led, 1);
		}
		if (config.status_led == 1) {
		    // Enable output if serial pin was used as status LED
		    system_set_os_print(1);
		}
		config.status_led = atoi(tokens[2]);
		if (config.status_led > 16) {
		    os_sprintf(response, "Status led disabled\r\n");
		    goto command_handled;
		}
		if (config.status_led == 1) {
		    // Disable output if serial pin is used as status LED
		    system_set_os_print(0);
		}
		easygpio_pinMode(config.status_led, EASYGPIO_NOPULL, EASYGPIO_OUTPUT);
		GPIO_OUTPUT_SET (config.status_led, 0);
		os_sprintf(response, "Status led set to GPIO %d\r\n", config.status_led);
        	goto command_handled;
	    }
#ifdef PHY_MODE
	    if (strcmp(tokens[1], "phy_mode") == 0)
	    {
		uint16_t mode = atoi(tokens[2]);
		bool succ = wifi_set_phy_mode(mode);
		if (succ) 
		    config.phy_mode = mode;
		os_sprintf(response, "Phy mode setting %s\r\n",
		  succ?"successful":"failed");
        	goto command_handled;
	    }
#endif
            if (strcmp(tokens[1],"network") == 0)
            {
                config.network_addr.addr = ipaddr_addr(tokens[2]);
		ip4_addr4(&config.network_addr) = 0;
                os_sprintf(response, "Network set to %d.%d.%d.%d/24\r\n", 
			IP2STR(&config.network_addr));
                goto command_handled;
            }

            if (strcmp(tokens[1],"dns") == 0)
            {
		if (os_strcmp(tokens[2], "dhcp") == 0) {
		    config.dns_addr.addr = 0;
		    os_sprintf(response, "DNS from DHCP\r\n");
		} else {
		    config.dns_addr.addr = ipaddr_addr(tokens[2]);
		    os_sprintf(response, "DNS set to %d.%d.%d.%d\r\n", 
			IP2STR(&config.dns_addr));
		    if (config.dns_addr.addr) {
			dns_ip.addr = config.dns_addr.addr;
			dhcps_set_DNS(&dns_ip);
		    }
		}
                goto command_handled;
            }

            if (strcmp(tokens[1],"ip") == 0)
            {
		if (os_strcmp(tokens[2], "dhcp") == 0) {
		    config.my_addr.addr = 0;
		    os_sprintf(response, "IP from DHCP\r\n");
		} else {
		    config.my_addr.addr = ipaddr_addr(tokens[2]);
		    os_sprintf(response, "IP address set to %d.%d.%d.%d\r\n", 
			IP2STR(&config.my_addr));
		}
                goto command_handled;
            }

            if (strcmp(tokens[1],"netmask") == 0)
            {
                config.my_netmask.addr = ipaddr_addr(tokens[2]);
                os_sprintf(response, "IP netmask set to %d.%d.%d.%d\r\n", 
			IP2STR(&config.my_netmask));
                goto command_handled;
            }

            if (strcmp(tokens[1],"gw") == 0)
            {
                config.my_gw.addr = ipaddr_addr(tokens[2]);
                os_sprintf(response, "Gateway set to %d.%d.%d.%d\r\n", 
			IP2STR(&config.my_gw));
                goto command_handled;
            }

            if (strcmp(tokens[1],"ap_mac") == 0)
            {
                if (!parse_mac(config.AP_MAC_address, tokens[2]))
		  os_sprintf(response, INVALID_ARG);
		else
                  os_sprintf(response, "AP MAC set\r\n");
                goto command_handled;
            }

            if (strcmp(tokens[1],"sta_mac") == 0)
            {
                if (!parse_mac(config.STA_MAC_address, tokens[2]))
		  os_sprintf(response, INVALID_ARG);
		else
                  os_sprintf(response, "STA MAC set\r\n");
                goto command_handled;
            }

            if (strcmp(tokens[1],"bssid") == 0)
            {
                if (!parse_mac(config.bssid, tokens[2]))
		  os_sprintf(response, INVALID_ARG);
		else
                  os_sprintf(response, "bssid set\r\n");
                goto command_handled;
            }

#ifdef MQTT_CLIENT
	    if (strcmp(tokens[1], "mqtt_host") == 0)
	    {
		os_strncpy(config.mqtt_host, tokens[2], 32);
		config.mqtt_host[31] = 0;
		os_sprintf(response, "MQTT host set\r\n");
        	goto command_handled;
	    }

	    if (strcmp(tokens[1], "mqtt_port") == 0)
	    {
		config.mqtt_port = atoi(tokens[2]);
		os_sprintf(response, "MQTT port set\r\n");
        	goto command_handled;
	    }

	    if (strcmp(tokens[1], "mqtt_user") == 0)
	    {
		os_strncpy(config.mqtt_user, tokens[2], 32);
		config.mqtt_user[31] = 0;
		os_sprintf(response, "MQTT user set\r\n");
        	goto command_handled;
	    }

	    if (strcmp(tokens[1], "mqtt_password") == 0)
	    {
		os_strncpy(config.mqtt_password, tokens[2], 32);
		config.mqtt_password[31] = 0;
		os_sprintf(response, "MQTT password set\r\n");
        	goto command_handled;
	    }
	    if (strcmp(tokens[1], "mqtt_id") == 0)
	    {
		os_strncpy(config.mqtt_id, tokens[2], 32);
		config.mqtt_id[31] = 0;
		os_sprintf(response, "MQTT id set\r\n");
        	goto command_handled;
	    }
	    if (strcmp(tokens[1], "mqtt_prefix") == 0)
	    {
		os_strncpy(config.mqtt_prefix, tokens[2], 64);
		config.mqtt_prefix[63] = 0;
		os_sprintf(response, "MQTT prefix set\r\n");
        	goto command_handled;
	    }
	    if (strcmp(tokens[1], "mqtt_command_topic") == 0)
	    {
		os_strncpy(config.mqtt_command_topic, tokens[2], 64);
		config.mqtt_command_topic[63] = 0;
		os_sprintf(response, "MQTT command topic set\r\n");
        	goto command_handled;
	    }
	    if (strcmp(tokens[1], "mqtt_interval") == 0)
	    {
		config.mqtt_interval = atoi(tokens[2]);
		os_sprintf(response, "MQTT interval set\r\n");
        	goto command_handled;
	    }
	    if (strcmp(tokens[1], "mqtt_mask") == 0)
	    {
		uint16_t val = 0;
		uint8_t i;
		int8_t len = os_strlen(tokens[2]);

		for (i = 0; i<len; i++) {
		  uint8_t c = toupper(tokens[2][i]);
		  if (c < '0' || (c > '9' && c <'A') || c > 'F') break;
		  if (c > '9') c -= 'A' - 10; else c -= '0';
		  val |= c << (((len-i)-1)*4);
		}
		config.mqtt_topic_mask = val;
		os_sprintf(response, "MQTT topic mask set to %4x\r\n", val);
        	goto command_handled;
	    }
#ifdef USER_GPIO_OUT
	    if (strcmp(tokens[1], "mqtt_gpio_out_topic") == 0)
	    {
		os_strncpy(config.mqtt_gpio_out_topic, tokens[2], 64);
		config.mqtt_gpio_out_topic[63] = 0;
		os_sprintf(response, "MQTT gpio_out topic set\r\n");
        	goto command_handled;
	    }
#endif
#endif /* MQTT_CLIENT */

#ifdef USER_GPIO_OUT
	    if (strcmp(tokens[1], "gpio_out") == 0)
	    {
		config.gpio_out_status = atoi(tokens[2]);
		easygpio_outputSet(USER_GPIO_OUT, config.gpio_out_status);
		mqtt_publish_int(MQTT_TOPIC_GPIOOUT, "GpioOut", "%d", (uint32_t)config.gpio_out_status);
		os_sprintf(response, "GPIO out set to %d\r\n", config.gpio_out_status);
        	goto command_handled;
	    }
#endif
        }
    }

    /* Control comes here only if the tokens[0] command is not handled */
    os_sprintf(response, "\r\nInvalid Command\r\n");

command_handled:
    ringbuf_memcpy_into(console_tx_buffer, response, os_strlen(response));
command_handled_2:
    system_os_post(0, SIG_CONSOLE_TX, (ETSParam) pespconn);
    return;
}

bool ICACHE_FLASH_ATTR check_connection_access(struct espconn *pesp_conn, uint8_t access_flags) {
    remot_info *premot = NULL;
    ip_addr_t *remote_addr;
    bool is_local;

    remote_addr = (ip_addr_t *)&(pesp_conn->proto.tcp->remote_ip);
    //os_printf("Remote addr is %d.%d.%d.%d\r\n", IP2STR(remote_addr));
    is_local = (remote_addr->addr & 0x00ffffff) == (config.network_addr.addr & 0x00ffffff);

    if (is_local && (access_flags & LOCAL_ACCESS))
	return true;
    if (!is_local && (access_flags & REMOTE_ACCESS))
	return true;

    return false;
}

#ifdef REMOTE_CONFIG
static void ICACHE_FLASH_ATTR tcp_client_recv_cb(void *arg,
                                                 char *data,
                                                 unsigned short length)
{
    struct espconn *pespconn = (struct espconn *)arg;
    int            index;
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
#ifdef ACLS
    acl_debug = 0;
    deny_cb_conn = 0;
#endif
    struct espconn *pespconn = (struct espconn *)arg;
}


/* Called when a client connects to the console server */
static void ICACHE_FLASH_ATTR tcp_client_connected_cb(void *arg)
{
    char payload[128];
    struct espconn *pespconn = (struct espconn *)arg;

    os_printf("tcp_client_connected_cb(): Client connected\r\n");

    if (!check_connection_access(pespconn, config.config_access)) {
	os_printf("Client disconnected - no config access on this network\r\n");
	espconn_disconnect(pespconn);
	return;
    }

    //espconn_regist_sentcb(pespconn,     tcp_client_sent_cb);
    espconn_regist_disconcb(pespconn,   tcp_client_discon_cb);
    espconn_regist_recvcb(pespconn,     tcp_client_recv_cb);
    espconn_regist_time(pespconn,  300, 1);  // Specific to console only

    ringbuf_reset(console_rx_buffer);
    ringbuf_reset(console_tx_buffer);
    
    os_sprintf(payload, "CMD>");
    espconn_sent(pespconn, payload, os_strlen(payload));
#ifdef ACLS
    deny_cb_conn = pespconn;
#endif
}
#endif /* REMOTE_CONFIG */


#ifdef WEB_CONFIG
static char *page_buf = NULL;

static void ICACHE_FLASH_ATTR handle_set_cmd(void *arg, char *cmd, char* val)
{
    struct espconn *pespconn = (struct espconn *)arg;
    int max_current_cmd_size = MAX_CON_CMD_SIZE - (os_strlen(cmd)+1);
    char cmd_line[MAX_CON_CMD_SIZE+1];

    if (os_strlen(val) >= max_current_cmd_size)
    {
        val[max_current_cmd_size]='\0';
    }
    os_sprintf(cmd_line, "%s %s", cmd, val);
    os_printf("web_config_client_recv_cb(): cmd line:%s\n",cmd_line);

    ringbuf_memcpy_into(console_rx_buffer, cmd_line, os_strlen(cmd_line));
    console_handle_command(pespconn);
//    ringbuf_memcpy_into(console_rx_buffer, "save", os_strlen("save"));
//    console_handle_command(pespconn);
}

char *strstr(char *string, char *needle);
char *strtok ( char * str, const char * delimiters );
char *strtok_r(char *s, const char *delim, char **last);

static void ICACHE_FLASH_ATTR web_config_client_recv_cb(void *arg,
                                                 char *data,
                                                 unsigned short length)
{
    struct espconn *pespconn = (struct espconn *)arg;
    char *kv, *sv;
    bool do_reset = false;

    char *str = strstr(data, " /?");
    if (str != NULL)
    {
        str = strtok(str+3," ");

        char* keyval = strtok_r(str,"&",&kv);
        while (keyval != NULL)
        {
            char *key = strtok_r(keyval,"=", &sv);
            char *val = strtok_r(NULL, "=", &sv);

            keyval = strtok_r (NULL, "&", &kv);
            os_printf("web_config_client_recv_cb(): key:%s:val:%s:\n",key,val);
            if (val != NULL)
            {

                if (strcmp(key, "ssid") == 0)
                {
                    handle_set_cmd(pespconn, "set ssid", val);
                    do_reset = true;
                }
                else if (strcmp(key, "password") == 0)
                {
                    handle_set_cmd(pespconn, "set password", val);
                    do_reset = true;
                }
                else if (strcmp(key, "lock") == 0)
                {
                    config.locked = 1;
                }
                else if (strcmp(key, "ap_ssid") == 0)
                {
                    handle_set_cmd(pespconn, "set ap_ssid", val);
                    do_reset = true;
                }
                else if (strcmp(key, "ap_password") == 0)
                {
                    handle_set_cmd(pespconn, "set ap_password", val);
                    do_reset = true;
                }
                else if (strcmp(key, "network") == 0)
                {
                    handle_set_cmd(pespconn, "set network", val);
                    do_reset = true;
                }
                else if (strcmp(key, "unlock_password") == 0)
                {
                    handle_set_cmd(pespconn, "unlock", val);
                }
                else if (strcmp(key, "ap_open") == 0)
                {
                    if (strcmp(val, "wpa2") == 0)
                    {
                        config.ap_open = 0;
                        do_reset = true;
                    }
                    if (strcmp(val, "open") == 0)
                    {
                        config.ap_open = 1;
                        do_reset = true;
                    }                }
                else if (strcmp(key, "reset") == 0)
                {
                    do_reset = true;
                }
            }
        }

	config_save(&config);

        if (do_reset == true)
        {
            do_reset = false;
            ringbuf_memcpy_into(console_rx_buffer, "reset", os_strlen("reset"));
            console_handle_command(pespconn);
        }
    }
}

static void ICACHE_FLASH_ATTR web_config_client_discon_cb(void *arg)
{
    os_printf("web_config_client_discon_cb(): client disconnected\n");
    struct espconn *pespconn = (struct espconn *)arg;

    if (page_buf != NULL)
	os_free(page_buf);
    page_buf = NULL;
}

static void ICACHE_FLASH_ATTR web_config_client_sent_cb(void *arg)
{
    os_printf("web_config_client_discon_cb(): data sent to client\n");
    struct espconn *pespconn = (struct espconn *)arg;

    espconn_disconnect(pespconn);
}

/* Called when a client connects to the web config */
static void ICACHE_FLASH_ATTR web_config_client_connected_cb(void *arg)
{

    struct espconn *pespconn = (struct espconn *)arg;

    os_printf("web_config_client_connected_cb(): Client connected\r\n");

    if (!check_connection_access(pespconn, config.config_access)) {
	os_printf("Client disconnected - no config access on this network\r\n");
	espconn_disconnect(pespconn);
	return;
    }

    espconn_regist_disconcb(pespconn,   web_config_client_discon_cb);
    espconn_regist_recvcb(pespconn,     web_config_client_recv_cb);
    espconn_regist_sentcb(pespconn,     web_config_client_sent_cb);

    ringbuf_reset(console_rx_buffer);
    ringbuf_reset(console_tx_buffer);

    if (!config.locked) {
    //os_printf("config_str: %d\r\n", os_strlen(CONFIG_PAGE));
	if (page_buf == NULL)
	    page_buf = (char *)os_malloc(os_strlen(CONFIG_PAGE)+200);
	os_sprintf(page_buf, CONFIG_PAGE, config.ssid, config.password,
		   config.ap_ssid, config.ap_password,
		   config.ap_open?" selected":"", config.ap_open?"":" selected",
		   IP2STR(&config.network_addr));
	espconn_sent(pespconn, page_buf, os_strlen(CONFIG_PAGE));
    }
    else {
	espconn_sent(pespconn, LOCK_PAGE, os_strlen(LOCK_PAGE));
	page_buf = NULL;
    }
}
#endif /* WEB_CONFIG */


bool toggle;
// Timer cb function
void ICACHE_FLASH_ATTR timer_func(void *arg){
uint32_t Vcurr;
uint64_t t_new;
uint32_t t_diff;
#ifdef TOKENBUCKET
uint32_t Bps;
#endif

    toggle = !toggle;

    if (config.status_led <= 16)
	GPIO_OUTPUT_SET (config.status_led, toggle && connected);

    // Power measurement
    // Measure Vdd every second, sliding mean over the last 16 secs
    if (toggle) {
	Vcurr = readvdd33();
	Vdd = (Vdd * 15 + Vcurr)/16;
#ifdef ALLOW_SLEEP
	if (config.Vmin != 0 && Vdd<config.Vmin) {
            os_printf("Vdd (%d mV) < Vmin (%d mV) -> going to deep sleep\r\n", Vdd, config.Vmin);
            system_deep_sleep(config.Vmin_sleep * 1000000);
	}
#endif
    }

    // Do we still have to configure the AP netif? 
    if (do_ip_config) {
	user_set_softap_ip_config();
	do_ip_config = false;
    }

    t_new = get_long_systime();

#ifdef TOKENBUCKET
    t_diff = (uint32_t)((t_new-t_old_tb)/1000);
    if (config.kbps_ds != 0) {
	Bps = config.kbps_ds*1024/8;
	token_bucket_ds += (t_diff * Bps)/1000;
	if (token_bucket_ds > MAX_TOKEN_RATIO*Bps) token_bucket_ds = MAX_TOKEN_RATIO*Bps;
    }
    if (config.kbps_us != 0) {
	Bps = config.kbps_us*1024/8;
	token_bucket_us += (t_diff * Bps)/1000;
	if (token_bucket_us > MAX_TOKEN_RATIO*Bps) token_bucket_us = MAX_TOKEN_RATIO*Bps;
    }
    t_old_tb = t_new;
#endif

#ifdef MQTT_CLIENT
    t_diff = (uint32_t)((t_new-t_old)/1000000);
    if (mqtt_enabled && config.mqtt_interval != 0 && (t_diff > config.mqtt_interval)) {
	mqtt_publish_int(MQTT_TOPIC_UPTIME, "Uptime", "%d", (uint32_t)(t_new/1000000));
	mqtt_publish_int(MQTT_TOPIC_VDD, "Vdd", "%d", Vdd);
	mqtt_publish_int(MQTT_TOPIC_BYTES, "Bin", "%d", (uint32_t)(Bytes_in/1024));
	mqtt_publish_int(MQTT_TOPIC_BYTES, "Bout", "%d", (uint32_t)(Bytes_out/1024));
	mqtt_publish_int(MQTT_TOPIC_PIN, "Ppsin", "%d", (Packets_in-Packets_in_last)/t_diff);
	mqtt_publish_int(MQTT_TOPIC_POUT, "Ppsout", "%d", (Packets_out-Packets_out_last)/t_diff);
	mqtt_publish_int(MQTT_TOPIC_NOSTATIONS, "NoStations", "%d", config.ap_on?wifi_softap_get_station_num():0);
	mqtt_publish_int(MQTT_TOPIC_BPSIN, "Bpsin", "%d", (uint32_t)(Bytes_in-Bytes_in_last)/t_diff);
	mqtt_publish_int(MQTT_TOPIC_BPSOUT, "Bpsout", "%d", (uint32_t)(Bytes_out-Bytes_out_last)/t_diff);
#ifdef USER_GPIO_OUT
	mqtt_publish_int(MQTT_TOPIC_GPIOOUT, "GpioOut", "%d", (uint32_t)config.gpio_out_status);
#endif

        t_old = t_new;
        Bytes_in_last = Bytes_in;
        Bytes_out_last = Bytes_out;
        Packets_in_last = Packets_in;
        Packets_out_last = Packets_out;
    }
#endif

    os_timer_arm(&ptimer, toggle?1000:100, 0); 
}

//Priority 0 Task
static void ICACHE_FLASH_ATTR user_procTask(os_event_t *events)
{
    //os_printf("Sig: %d\r\n", events->sig);

    switch(events->sig)
    {
    case SIG_START_SERVER:
	// Anything else to do here, when the repeater has received its IP?
	break;

    case SIG_CONSOLE_TX:
        {
            struct espconn *pespconn = (struct espconn *) events->par;
            console_send_response(pespconn);

	    if (pespconn != 0 && remote_console_disconnect) espconn_disconnect(pespconn);
	    remote_console_disconnect = 0;
        }
        break;

    case SIG_CONSOLE_RX:
        {
            struct espconn *pespconn = (struct espconn *) events->par;
            console_handle_command(pespconn);
        }
        break;

#ifdef MQTT_CLIENT
#ifdef USER_GPIO_IN
    case SIG_GPIO_INT:
        {
	    mqtt_publish_int(MQTT_TOPIC_GPIOIN, "GpioIn", "%d", (uint32_t)events->par);
            //os_printf("GPIO %d %d\r\n", (uint32_t)events->par, easygpio_inputGet(USER_GPIO_IN));
        }
        break;
#endif
#endif
    case SIG_DO_NOTHING:
    default:
        // Intentionally ignoring other signals
        os_printf("Spurious Signal received\r\n");
        break;
    }
}

/* Callback called when the connection state of the module with an Access Point changes */
void wifi_handle_event_cb(System_Event_t *evt)
{
    uint16_t i;
    uint8_t mac_str[20];

    //os_printf("wifi_handle_event_cb: ");
    switch (evt->event)
    {
    case EVENT_STAMODE_CONNECTED:
        os_printf("connect to ssid %s, channel %d\n", evt->event_info.connected.ssid, evt->event_info.connected.channel);
	my_channel = evt->event_info.connected.channel;
        break;

    case EVENT_STAMODE_DISCONNECTED:
        os_printf("disconnect from ssid %s, reason %d\n", evt->event_info.disconnected.ssid, evt->event_info.disconnected.reason);
	connected = false;

#ifdef MQTT_CLIENT
	if (mqtt_enabled) MQTT_Disconnect(&mqttClient);
#endif /* MQTT_CLIENT */

        break;

    case EVENT_STAMODE_AUTHMODE_CHANGE:
        os_printf("mode: %d -> %d\n", evt->event_info.auth_change.old_mode, evt->event_info.auth_change.new_mode);
        break;

    case EVENT_STAMODE_GOT_IP:
	if (config.dns_addr.addr == 0) {
	    dns_ip = dns_getserver(0);
	}
	dhcps_set_DNS(&dns_ip);

        os_printf("ip:" IPSTR ",mask:" IPSTR ",gw:" IPSTR ",dns:" IPSTR "\n", IP2STR(&evt->event_info.got_ip.ip), IP2STR(&evt->event_info.got_ip.mask), IP2STR(&evt->event_info.got_ip.gw), IP2STR(&dns_ip));

	my_ip = evt->event_info.got_ip.ip;
	connected = true;

	// Update any predefined portmaps to the new IP addr
        for (i = 0; i<IP_PORTMAP_MAX; i++) {
	  if(ip_portmap_table[i].valid) {
	     ip_portmap_table[i].maddr = my_ip.addr;
	  }
	}

#ifdef MQTT_CLIENT
	if (mqtt_enabled) MQTT_Connect(&mqttClient);
#endif /* MQTT_CLIENT */

        // Post a Server Start message as the IP has been acquired to Task with priority 0
	system_os_post(user_procTaskPrio, SIG_START_SERVER, 0 );
        break;

    case EVENT_SOFTAPMODE_STACONNECTED:
	os_sprintf(mac_str, MACSTR, MAC2STR(evt->event_info.sta_connected.mac));
        os_printf("station: %s join, AID = %d\n", mac_str, evt->event_info.sta_connected.aid);
#ifdef MQTT_CLIENT
	mqtt_publish_str(MQTT_TOPIC_JOIN, "join", mac_str);
#endif
	patch_netif_ap(my_input_ap, my_output_ap, true);
        break;

    case EVENT_SOFTAPMODE_STADISCONNECTED:
	os_sprintf(mac_str, MACSTR, MAC2STR(evt->event_info.sta_disconnected.mac));
        os_printf("station: %s leave, AID = %d\n", mac_str, evt->event_info.sta_disconnected.aid);
#ifdef MQTT_CLIENT
	mqtt_publish_str(MQTT_TOPIC_LEAVE, "leave", mac_str);
#endif
        break;

    default:
        break;
    }
}


void ICACHE_FLASH_ATTR user_set_softap_wifi_config(void)
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
   apConfig.ssid_hidden = config.ssid_hidden;

   // Set ESP8266 softap config
   wifi_softap_set_config(&apConfig);
}


void ICACHE_FLASH_ATTR user_set_softap_ip_config(void)
{
struct ip_info info;
struct dhcps_lease dhcp_lease;
struct netif *nif;
int i;

   // Configure the internal network

   // Find the netif of the AP (that with num != 0)
   for (nif = netif_list; nif != NULL && nif->num == 0; nif = nif->next);
   if (nif == NULL) return;
   // If is not 1, set it to 1. 
   // Kind of a hack, but the Espressif-internals expect it like this (hardcoded 1).
   nif->num = 1;

   wifi_softap_dhcps_stop();

   info.ip = config.network_addr;
   ip4_addr4(&info.ip) = 1;
   info.gw = info.ip;
   IP4_ADDR(&info.netmask, 255, 255, 255, 0);

   wifi_set_ip_info(nif->num, &info);

   dhcp_lease.start_ip = config.network_addr;
   ip4_addr4(&dhcp_lease.start_ip) = 2;
   dhcp_lease.end_ip = config.network_addr;
   ip4_addr4(&dhcp_lease.end_ip) = 128;
   wifi_softap_set_dhcps_lease(&dhcp_lease);

   wifi_softap_dhcps_start();

   // Change the DNS server again
   dhcps_set_DNS(&dns_ip);

   // Enter any saved dhcp enties if they are in this network
   for (i = 0; i<config.dhcps_entries; i++) {
     if ((config.network_addr.addr & info.netmask.addr) == (config.dhcps_p[i].ip.addr & info.netmask.addr))
       dhcps_set_mapping(&config.dhcps_p[i].ip, &config.dhcps_p[i].mac[0], 100000 /* several month */);
   }
}


void ICACHE_FLASH_ATTR user_set_station_config(void)
{
    struct station_config stationConf;
    char hostname[40];

    /* Setup AP credentials */
    os_sprintf(stationConf.ssid, "%s", config.ssid);
    os_sprintf(stationConf.password, "%s", config.password);
    if (*(int*)config.bssid != 0) {
	stationConf.bssid_set = 1;
	os_memcpy(stationConf.bssid, config.bssid, 6);
    } else {
	stationConf.bssid_set = 0;
    }
    wifi_station_set_config(&stationConf);

    os_sprintf(hostname, "NET_%s", config.ap_ssid);
    hostname[32] = '\0';
    wifi_station_set_hostname(hostname);

    wifi_set_event_handler_cb(wifi_handle_event_cb);

    wifi_station_set_auto_connect(config.auto_connect != 0);
}

#ifdef MQTT_CLIENT
#ifdef USER_GPIO_IN
static os_timer_t inttimer;

void ICACHE_FLASH_ATTR int_timer_func(void *arg){
	mqtt_publish_int(MQTT_TOPIC_GPIOIN, "GpioIn", "%d", easygpio_inputGet(USER_GPIO_IN));
        //os_printf("GPIO %d %d\r\n", (uint32_t)arg, easygpio_inputGet(USER_GPIO_IN));

        // Reactivate interrupts for GPIO
        gpio_pin_intr_state_set(GPIO_ID_PIN(USER_GPIO_IN), GPIO_PIN_INTR_ANYEDGE);
}

// Interrupt handler - this function will be executed on any edge of USER_GPIO_IN
LOCAL void  gpio_intr_handler(void *dummy)
{
    uint32 gpio_status = GPIO_REG_READ(GPIO_STATUS_ADDRESS);

    if (gpio_status & BIT(USER_GPIO_IN)) {

        // Disable interrupt for GPIO
        gpio_pin_intr_state_set(GPIO_ID_PIN(USER_GPIO_IN), GPIO_PIN_INTR_DISABLE);

        // Post it to the main task
	//system_os_post(0, SIG_GPIO_INT, (ETSParam) easygpio_inputGet(USER_GPIO_IN));

        // Clear interrupt status for GPIO
        GPIO_REG_WRITE(GPIO_STATUS_W1TC_ADDRESS, gpio_status & BIT(USER_GPIO_IN));

	// Start the timer
    	os_timer_setfn(&inttimer, int_timer_func, easygpio_inputGet(USER_GPIO_IN));
    	os_timer_arm(&inttimer, 50, 0); 

        // Reactivate interrupts foR GPIO
        //gpio_pin_intr_state_set(GPIO_ID_PIN(USER_GPIO_IN), GPIO_PIN_INTR_ANYEDGE);
    }
}
#endif /* USER_GPIO_IN */
#endif /* MQTT_CLIENT */

void ICACHE_FLASH_ATTR user_init()
{
struct ip_info info;

    connected = false;
    do_ip_config = false;
    my_ip.addr = 0;
    Bytes_in = Bytes_out = Bytes_in_last = Bytes_out_last = 0,
    Packets_in = Packets_out = Packets_in_last = Packets_out_last = 0;
    t_old = 0;

#ifdef TOKENBUCKET
    t_old_tb = 0;
    token_bucket_ds = token_bucket_us = 0;
#endif

    console_rx_buffer = ringbuf_new(MAX_CON_CMD_SIZE);
    console_tx_buffer = ringbuf_new(MAX_CON_SEND_SIZE);

    gpio_init();
    init_long_systime();

    UART_init_console(BIT_RATE_115200, 0, console_rx_buffer, console_tx_buffer);

    os_printf("\r\n\r\nWiFi Repeater V1.4 starting\r\n");

    // Load config
    if (config_load(&config)== 0) {
	// valid config in FLASH, can read portmap table
	blob_load(0, (uint32_t *)ip_portmap_table, sizeof(struct portmap_table) * IP_PORTMAP_MAX);
    } else {
	
	// clear portmap table
	blob_zero(0, sizeof(struct portmap_table) * IP_PORTMAP_MAX);
    }
#ifdef ACLS
    acl_debug = 0;
    acl_clear_stats(0);
    acl_clear_stats(1);
    acl_set_deny_cb(acl_deny_cb);
#endif
    // Config GPIO pin as output
    if (config.status_led == 1) {
	// Disable output if serial pin is used as status LED
	system_set_os_print(0);
    }

    if (config.status_led <= 16) {
	easygpio_pinMode(config.status_led, EASYGPIO_NOPULL, EASYGPIO_OUTPUT);
	GPIO_OUTPUT_SET (config.status_led, 0);
    }

#ifdef MQTT_CLIENT
#ifdef USER_GPIO_IN
    easygpio_pinMode(USER_GPIO_IN, EASYGPIO_PULLUP, EASYGPIO_INPUT);
    easygpio_attachInterrupt(USER_GPIO_IN, EASYGPIO_PULLUP, gpio_intr_handler, NULL);
    gpio_pin_intr_state_set(GPIO_ID_PIN(USER_GPIO_IN), GPIO_PIN_INTR_ANYEDGE);
#endif
#endif
#ifdef USER_GPIO_OUT
    easygpio_pinMode(USER_GPIO_OUT, EASYGPIO_NOPULL, EASYGPIO_OUTPUT);
    easygpio_outputSet(USER_GPIO_OUT, config.gpio_out_status);
#endif

    wifi_set_macaddr(SOFTAP_IF, config.AP_MAC_address);	
    wifi_set_macaddr(STATION_IF, config.STA_MAC_address);

    // Configure the AP and start it, if required
    if (config.dns_addr.addr == 0)
	// Google's DNS as default, as long as we havn't got one from DHCP
	IP4_ADDR(&dns_ip, 8, 8, 8, 8);
    else
	// We have a static DNS server
	dns_ip.addr = config.dns_addr.addr;

    if (config.ap_on) {
	wifi_set_opmode(STATIONAP_MODE);
    	user_set_softap_wifi_config();
	do_ip_config = true;
    } else {
	wifi_set_opmode(STATION_MODE);
    }

#ifdef PHY_MODE
    wifi_set_phy_mode(config.phy_mode);
#endif

    if (config.my_addr.addr != 0) {
	wifi_station_dhcpc_stop();
	info.ip.addr = config.my_addr.addr;
	info.gw.addr = config.my_gw.addr;
	info.netmask.addr = config.my_netmask.addr;
	wifi_set_ip_info(STATION_IF, &info);
	espconn_dns_setserver(0, &dns_ip);
    }

#ifdef REMOTE_CONFIG
    if (config.config_port != 0) {
	os_printf("Starting Console TCP Server on port %d\r\n", config.config_port);
	struct espconn *pCon = (struct espconn *)os_zalloc(sizeof(struct espconn));

	/* Equivalent to bind */
	pCon->type  = ESPCONN_TCP;
	pCon->state = ESPCONN_NONE;
	pCon->proto.tcp = (esp_tcp *)os_zalloc(sizeof(esp_tcp));
	pCon->proto.tcp->local_port = config.config_port;

	/* Register callback when clients connect to the server */
	espconn_regist_connectcb(pCon, tcp_client_connected_cb);

	/* Put the connection in accept mode */
	espconn_accept(pCon);
    }
#endif

#ifdef WEB_CONFIG
    if (config.web_port != 0) {
        os_printf("Starting Web Config Server on port %d\r\n", config.web_port);
        struct espconn *pCon = (struct espconn *)os_zalloc(sizeof(struct espconn));

        /* Equivalent to bind */
        pCon->type  = ESPCONN_TCP;
        pCon->state = ESPCONN_NONE;
        pCon->proto.tcp = (esp_tcp *)os_zalloc(sizeof(esp_tcp));
        pCon->proto.tcp->local_port = config.web_port;

        /* Register callback when clients connect to the server */
        espconn_regist_connectcb(pCon, web_config_client_connected_cb);

        /* Put the connection in accept mode */
        espconn_accept(pCon);
    }
#endif

#ifdef REMOTE_MONITORING
    monitoring_on = 0;
    monitor_port = 0;
    acl_monitoring = 0;
#endif

#ifdef MQTT_CLIENT
    mqtt_enabled = (os_strcmp(config.mqtt_host, "none") != 0);
    if (mqtt_enabled) {
	MQTT_InitConnection(&mqttClient, config.mqtt_host, config.mqtt_port, 0);

//	MQTT_InitClient(&mqttClient, MQTT_CLIENT_ID, MQTT_USER, MQTT_PASS, MQTT_KEEPALIVE, MQTT_CLEAN_SESSION);
	if (os_strcmp(config.mqtt_user, "none") == 0) {
	  MQTT_InitClient(&mqttClient, config.mqtt_id, 0, 0, 120, 1);
	} else {
	  MQTT_InitClient(&mqttClient, config.mqtt_id, config.mqtt_user, config.mqtt_password, 120, 1);
	}
//	MQTT_InitLWT(&mqttClient, "/lwt", "offline", 0, 0);
	MQTT_OnConnected(&mqttClient, mqttConnectedCb);
	MQTT_OnDisconnected(&mqttClient, mqttDisconnectedCb);
	MQTT_OnPublished(&mqttClient, mqttPublishedCb);
	MQTT_OnData(&mqttClient, mqttDataCb);
    }
#endif /* MQTT_CLIENT */

    remote_console_disconnect = 0;

    // Now start the STA-Mode
    user_set_station_config();

    // Init power - set it to 3300mV
    Vdd = 3300;

    system_update_cpu_freq(config.clock_speed);

    // Start the timer
    os_timer_setfn(&ptimer, timer_func, 0);
    os_timer_arm(&ptimer, 500, 0); 

    //Start task
    system_os_task(user_procTask, user_procTaskPrio, user_procTaskQueue, user_procTaskQueueLen);
}

