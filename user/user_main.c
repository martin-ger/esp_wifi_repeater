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
#include "lwip/ip_route.h"
#include "lwip/app/dhcpserver.h"
#include "lwip/app/espconn.h"
#include "lwip/app/espconn_tcp.h"

#if OTAUPDATE
#include "rboot-api.h"
#include "rboot-ota.h"
#endif

#if ALLOW_PING
#include "lwip/app/ping.h"
#endif
#if HAVE_ENC28J60
#include "netif/espenc.h"
#if DCHPSERVER_ENC28J60
#include "lwip/app/encdhcpserver.h"
#endif
#endif
#include "user_interface.h"
#include "string.h"
#include "driver/uart.h"

#include "ringbuf.h"
#include "user_config.h"
#include "config_flash.h"
#include "sys_time.h"
#include "sntp.h"

#include "easygpio.h"

#if WEB_CONFIG
#include "web.h"
#endif

#if ACLS
#include "acl.h"
#endif

#if REMOTE_MONITORING
#include "pcap.h"
#endif

#if MQTT_CLIENT
#include "mqtt.h"
#endif

#define os_sprintf_flash(str, fmt, ...) do {	\
	static const char flash_str[] ICACHE_RODATA_ATTR STORE_ATTR = fmt;	\
	int flen = (sizeof(flash_str) + 4) & ~3;	\
	char *f = (char *)os_malloc(flen);	\
	os_memcpy(f, flash_str, flen);	\
	ets_vsprintf(str, f,  ##__VA_ARGS__);	\
	os_free(f);	\
	} while(0)

uint32_t Vdd;

/* System Task, for signals refer to user_config.h */
#define user_procTaskPrio        0
#define user_procTaskQueueLen    2
os_event_t    user_procTaskQueue[user_procTaskQueueLen];
static void user_procTask(os_event_t *events);

static os_timer_t ptimer;

int32_t ap_watchdog_cnt;
int32_t client_watchdog_cnt;

/* Some stats */
uint64_t Bytes_in, Bytes_out, Bytes_in_last, Bytes_out_last;
uint32_t Packets_in, Packets_out, Packets_in_last, Packets_out_last;
uint64_t t_old;

#if DAILY_LIMIT
uint64_t Bytes_per_day;
uint8_t last_date;
#endif

#if TOKENBUCKET
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

static ip_addr_t resolve_ip;

uint8_t mesh_level;
uint8_t uplink_bssid[6];

static netif_input_fn orig_input_ap, orig_input_sta;
static netif_linkoutput_fn orig_output_ap, orig_output_sta;

#if HAVE_ENC28J60
struct netif* eth_netif;
#endif

uint8_t remote_console_disconnect;
struct espconn *currentconn;

void ICACHE_FLASH_ATTR user_set_softap_wifi_config(void);
void ICACHE_FLASH_ATTR user_set_softap_ip_config(void);
void ICACHE_FLASH_ATTR user_set_station_config(void);

void ICACHE_FLASH_ATTR to_console(char *str) {
    ringbuf_memcpy_into(console_tx_buffer, str, os_strlen(str));
}

void ICACHE_FLASH_ATTR mac_2_buff(char *buf, uint8_t mac[6]) {
    os_sprintf(buf, "%02x:%02x:%02x:%02x:%02x:%02x",
		    mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

#if MQTT_CLIENT

#define MQTT_TOPIC_RESPONSE	0x0001
#define MQTT_TOPIC_IP		0x0002
#define MQTT_TOPIC_SCANRESULT	0x0004
#define MQTT_TOPIC_JOIN		0x0008
#define MQTT_TOPIC_LEAVE	0x0010
#define MQTT_TOPIC_UPTIME	0x0020
#define MQTT_TOPIC_VDD		0x0040
#define MQTT_TOPIC_ACLDENY	0x0080
#define MQTT_TOPIC_BYTES	0x0100
#define MQTT_TOPIC_PACKETS	0x0200
#define MQTT_TOPIC_BPD		0x0400
#define MQTT_TOPIC_BPS		0x0800
#define MQTT_TOPIC_TOPOLOGY	0x1000
#define MQTT_TOPIC_NOSTATIONS	0x2000
#define MQTT_TOPIC_GPIOIN	0x4000
#define MQTT_TOPIC_GPIOOUT	0x8000

MQTT_Client mqttClient;
bool mqtt_enabled, mqtt_connected;

void ICACHE_FLASH_ATTR mqtt_publish_str(uint16_t mask, uint8_t *sub_topic, uint8_t *str)
{
uint8_t buf[256];
  if (!mqtt_enabled || (config.mqtt_topic_mask & mask) == 0) return;

  os_sprintf(buf, "%s/%s", config.mqtt_prefix, sub_topic);
//os_printf("Publish: %s %s\r\n", buf, str);
  MQTT_Publish(&mqttClient, buf, str, os_strlen(str), config.mqtt_qos, 0);
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
uint8_t buf[256];

  MQTT_Client* client = (MQTT_Client*)args;
  os_printf("MQTT: Connected\r\n");
  mqtt_connected = true;

  os_sprintf(buf, "%s/status", config.mqtt_prefix);
  MQTT_Publish(client, buf, "online", os_strlen("online"), config.mqtt_qos, 1);

  os_sprintf(buf, IPSTR, IP2STR(&my_ip));
  mqtt_publish_str(MQTT_TOPIC_IP, "IP", buf);

  if (os_strcmp(config.mqtt_command_topic, "none") != 0) {
    MQTT_Subscribe(client, config.mqtt_command_topic, config.mqtt_qos);
  }
#ifdef USER_GPIO_OUT
  if (os_strcmp(config.mqtt_gpio_out_topic, "none") != 0) {
    MQTT_Subscribe(client, config.mqtt_gpio_out_topic, config.mqtt_qos);
  }
#endif
}

static void ICACHE_FLASH_ATTR mqttDisconnectedCb(uint32_t *args)
{
  MQTT_Client* client = (MQTT_Client*)args;
  os_printf("MQTT: Disconnected\r\n");
  mqtt_connected = false;
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

// call back for dns lookup
static void ICACHE_FLASH_ATTR dns_resolved(const char *name, ip_addr_t *ip, void *arg) {
char response[128];

    if (ip == 0) {
	os_sprintf(response, "DNS lookup failed for: %s\r\n", name);
    } else {
	os_sprintf(response, "DNS lookup for %s: " IPSTR "\r\n", name, IP2STR(ip));
    }

    to_console(response);
    system_os_post(0, SIG_CONSOLE_TX, (ETSParam) currentconn);
}

#if ALLOW_PING
struct ping_option ping_opt;
uint8_t ping_success_count;

void ICACHE_FLASH_ATTR user_ping_recv(void *arg, void *pdata)
{
    struct ping_resp *ping_resp = pdata;
    struct ping_option *ping_opt = arg;
    char response[128];

    if (ping_resp->ping_err == -1) {
        os_sprintf(response, "ping failed\r\n");
    } else {
        os_sprintf(response, "ping recv bytes: %d time: %d ms\r\n",ping_resp->bytes,ping_resp->resp_time);
	ping_success_count++;
    }

    to_console(response);
    system_os_post(0, SIG_CONSOLE_TX_RAW, (ETSParam) currentconn);
}

void ICACHE_FLASH_ATTR user_ping_sent(void *arg, void *pdata)
{
    char response[128];

    os_sprintf(response, "ping finished (%d/%d)\r\n", ping_success_count, ping_opt.count);
    to_console(response);
    system_os_post(0, SIG_CONSOLE_TX, (ETSParam) currentconn);
}

void ICACHE_FLASH_ATTR user_do_ping(const char *name, ip_addr_t *ipaddr, void *arg)
{
    ping_opt.count = 4;    //  try to ping how many times
    ping_opt.coarse_time = 2;  // ping interval
    ping_opt.ip = ipaddr->addr;
    ping_success_count = 0;

    ping_regist_recv(&ping_opt,user_ping_recv);
    ping_regist_sent(&ping_opt,user_ping_sent);

    ping_start(&ping_opt);
}
#endif

#if ALLOW_SLEEP
static os_timer_t sleep_delay_timer;
uint32_t sleeptime;

void ICACHE_FLASH_ATTR sleep_delay_timer_func(void *arg){
    system_deep_sleep(sleeptime*1000000);
}
#endif

#if REMOTE_MONITORING
static uint8_t monitoring_on;
static uint16_t monitor_port;
static ringbuf_t pcap_buffer;
struct espconn *cur_mon_conn;
struct espconn *cur_mon_listen;
static uint8_t monitoring_send_ongoing;
static uint8_t acl_monitoring;

static void ICACHE_FLASH_ATTR tcp_monitor_sent_cb(void *arg)
{
    uint16_t len;
    static uint8_t tbuf[1400];

    struct espconn *pespconn = (struct espconn *)arg;
    //os_printf("tcp_monitor_sent_cb(): Data sent to monitor\n");

    monitoring_send_ongoing = 0;
    if (!monitoring_on) return;

    len = ringbuf_bytes_used(pcap_buffer);
    if (len > 0) {
	 if (len > 1400)
		 len = 1400;

	 ringbuf_memcpy_from(tbuf, pcap_buffer, len);
	 //os_printf("tcp_monitor_sent_cb(): %d Bytes sent to monitor\n", len);
	 if (espconn_send(pespconn, tbuf, len) != 0) {
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

    espconn_send(pespconn, (uint8_t *)&pcf_hdr, sizeof(pcf_hdr));
    monitoring_send_ongoing = 1;

    monitoring_on = 1;
}


static void ICACHE_FLASH_ATTR start_monitor(uint16_t portno)
{
    if (monitoring_on) return;

    pcap_buffer = ringbuf_new(MONITOR_BUFFER_SIZE);
    monitoring_send_ongoing = 0;

    os_printf("Starting Monitor TCP Server on %d port\r\n", portno);
    cur_mon_listen = (struct espconn *)os_zalloc(sizeof(struct espconn));
    if (cur_mon_listen == NULL) {
        os_printf("Monitor conn open failed\r\n");
        return;
    }

    /* Equivalent to bind */
    cur_mon_listen->type  = ESPCONN_TCP;
    cur_mon_listen->state = ESPCONN_NONE;
    cur_mon_listen->proto.tcp = (esp_tcp *)os_zalloc(sizeof(esp_tcp));
    cur_mon_listen->proto.tcp->local_port = portno;

    /* Register callback when clients connect to the server */
    espconn_regist_connectcb(cur_mon_listen, tcp_monitor_connected_cb);

    /* Put the connection in accept mode */
    espconn_accept(cur_mon_listen);
}

static void ICACHE_FLASH_ATTR stop_monitor(void)
{
    if (monitoring_on == 1) {
	os_printf("Stopping Monitor TCP Server\r\n");
	espconn_disconnect(cur_mon_conn);
    }

    if (cur_mon_listen != NULL) {
	espconn_delete(cur_mon_listen);
	os_free(cur_mon_listen->proto.tcp);
	os_free(cur_mon_listen);
    }

    monitoring_on = 0;
    monitor_port = 0;
    cur_mon_listen = NULL;
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
	easygpio_outputSet (config.status_led, 1);

    client_watchdog_cnt = config.client_watchdog;

#if ACLS
    // Check ACLs - store result
    uint8_t acl_check = ACL_ALLOW;
    if (!acl_is_empty(0))
       acl_check = acl_check_packet(0, p);
#endif

#if REMOTE_MONITORING
    if (monitoring_on && !acl_monitoring) {
       if (put_packet_to_ringbuf(p) != 0) {
#if DROP_PACKET_IF_NOT_RECORDED
               pbuf_free(p);
	       return ERR_OK;
#endif
       }
       if (!monitoring_send_ongoing)
	       tcp_monitor_sent_cb(cur_mon_conn);
    }
#if ACLS
    // Check if packet should be monitored by ACL
    if (monitoring_on && acl_monitoring && (acl_check&ACL_MONITOR)) {
       put_packet_to_ringbuf(p);
       if (!monitoring_send_ongoing)
	       tcp_monitor_sent_cb(cur_mon_conn);
    }
#endif
#endif /* REMOTE_MONITORING */

#if ACLS
    // If not allowed, drop packet
    if (!(acl_check&ACL_ALLOW)) {
	pbuf_free(p);
	return ERR_OK;
    };
#endif

#if TOKENBUCKET
    if (config.kbps_us != 0) {
        if (p->tot_len <= token_bucket_us) {
	    token_bucket_us -= p->tot_len;
        } else {
	    pbuf_free(p);
	    return ERR_OK;
	}
    }
#endif

#if DAILY_LIMIT
    if (config.daily_limit != 0 && Bytes_per_day/1024 >= config.daily_limit) {
	pbuf_free(p);
	return ERR_OK;
    }

    Bytes_per_day += p->tot_len;
#endif
    Bytes_in += p->tot_len;
    Packets_in++;

    return orig_input_ap (p, inp);
}

err_t ICACHE_FLASH_ATTR my_output_ap (struct netif *outp, struct pbuf *p) {

//  os_printf("Send packet to STA\r\n");

    if (config.status_led <= 16)
	easygpio_outputSet (config.status_led, 0);

#if ACLS
    // Check ACLs - store result
    uint8_t acl_check = ACL_ALLOW;
    if (!acl_is_empty(1))
       acl_check = acl_check_packet(1, p);
#endif

#if REMOTE_MONITORING
    if (monitoring_on && !acl_monitoring) {
       if (put_packet_to_ringbuf(p) != 0) {
#if DROP_PACKET_IF_NOT_RECORDED
               pbuf_free(p);
	       return ERR_OK;
#endif
       }
       if (!monitoring_send_ongoing)
	       tcp_monitor_sent_cb(cur_mon_conn);
    }

#if ACLS
    // Check if packet should be monitored by ACL
    if (monitoring_on && acl_monitoring && (acl_check&ACL_MONITOR)) {
       put_packet_to_ringbuf(p);
       if (!monitoring_send_ongoing)
	       tcp_monitor_sent_cb(cur_mon_conn);
    }
#endif
#endif /* REMOTE_MONITORING */

#if ACLS
    // If not allowed, drop packet
    if (!(acl_check&ACL_ALLOW)) {
	pbuf_free(p);
	return ERR_OK;
    };
#endif

#if TOKENBUCKET
    if (config.kbps_ds != 0) {
        if (p->tot_len <= token_bucket_ds) {
	    token_bucket_ds -= p->tot_len;
        } else {
	    pbuf_free(p);
	    return ERR_OK;
	}
    }
#endif

#if DAILY_LIMIT
    if (config.daily_limit != 0 && Bytes_per_day/1024 >= config.daily_limit) {
	pbuf_free(p);
	return ERR_OK;
    }

    Bytes_per_day += p->tot_len;
#endif
    Bytes_out += p->tot_len;
    Packets_out++;

    return orig_output_ap (outp, p);
}

err_t ICACHE_FLASH_ATTR my_input_sta (struct pbuf *p, struct netif *inp) {

    ap_watchdog_cnt = config.ap_watchdog;
#if ACLS
    if (!acl_is_empty(2) && !(acl_check_packet(2, p) & ACL_ALLOW)) {
	pbuf_free(p);
	return ERR_OK;
    };
#endif
    return orig_input_sta (p, inp);
}

err_t ICACHE_FLASH_ATTR my_output_sta (struct netif *outp, struct pbuf *p) {
#if ACLS
    if (!acl_is_empty(3) && !(acl_check_packet(3, p) & ACL_ALLOW)) {
	pbuf_free(p);
	return ERR_OK;
    };
#endif
    return orig_output_sta (outp, p);
}

static void ICACHE_FLASH_ATTR patch_netif(ip_addr_t netif_ip, netif_input_fn ifn, netif_input_fn *orig_ifn, netif_linkoutput_fn ofn, netif_linkoutput_fn *orig_ofn, bool nat)
{
struct netif *nif;

	for (nif = netif_list; nif != NULL && nif->ip_addr.addr != netif_ip.addr; nif = nif->next);
	if (nif == NULL) return;

	nif->napt = nat?1:0;
	if (ifn != NULL && nif->input != ifn) {
	  *orig_ifn = nif->input;
	  nif->input = ifn;
	}
	if (ofn != NULL && nif->linkoutput != ofn) {
	  *orig_ofn = nif->linkoutput;
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

char *console_output = NULL;
void console_send_response(struct espconn *pespconn, uint8_t do_cmd)
{
    uint16_t len = ringbuf_bytes_used(console_tx_buffer);
    console_output = (char*) os_malloc(len+4);

    ringbuf_memcpy_from(console_output, console_tx_buffer, len);
#if MQTT_CLIENT
    console_output[len] = 0;
    if (os_strcmp(config.mqtt_command_topic, "none") != 0) {
	mqtt_publish_str(MQTT_TOPIC_RESPONSE, "response", console_output);
    }
#endif
    if (do_cmd) {
	os_memcpy(&console_output[len], "CMD>", 4);
	len += 4;
    }

    if (pespconn != NULL) {
	espconn_send(pespconn, console_output, len);
    } else {
	UART_Send(0, console_output, len);
	os_free(console_output);
	console_output = NULL;
    }
}


#if ALLOW_SCANNING
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
      to_console(response);
#if MQTT_CLIENT
      mqtt_publish_str(MQTT_TOPIC_SCANRESULT, "ScanResult", response);
#endif
      bss_link = bss_link->next.stqe_next;
    }
  }
  else
  {
     os_sprintf(response, "scan fail !!!\r\n");
     to_console(response);
  }
  system_os_post(0, SIG_CONSOLE_TX, (ETSParam) currentconn);
}
#endif

#if ACLS
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
#if MQTT_CLIENT
	&& !mqtt_enabled
#endif
	) return allow;

    os_sprintf(response, "\rdeny: %s Src: %d.%d.%d.%d:%d Dst: %d.%d.%d.%d:%d\r\n",
	proto==IP_PROTO_TCP?"TCP":proto==IP_PROTO_UDP?"UDP":"IP4",
	IP2STR((ip_addr_t *)&saddr), s_port, IP2STR((ip_addr_t *)&daddr), d_port);

#if MQTT_CLIENT
    mqtt_publish_str(MQTT_TOPIC_ACLDENY, "ACLDeny", response);
#endif
    if (acl_debug) {
	to_console(response);
	system_os_post(0, SIG_CONSOLE_TX, (ETSParam) deny_cb_conn);
    }
    return allow;
}
#endif /* ACLS */

#if OTAUPDATE
void ICACHE_FLASH_ATTR Switch() {
	char msg[50];
	uint8 before, after;
	before = rboot_get_current_rom();
	if (before == 0) after = 1; else after = 0;
	os_sprintf(msg, "Swapping from rom %d to rom %d.\r\n", before, after);
	to_console(msg);
	rboot_set_current_rom(after);
	to_console("Restarting...\r\n\r\n");
	system_restart();
	while (true);
}

static void ICACHE_FLASH_ATTR OtaUpdate_CallBack(bool result, uint8 rom_slot) {
	if(result == true) {
		// success
		if (rom_slot == FLASH_BY_ADDR) {
			to_console("Write successful.\r\n");
		} else {
			// set to boot new rom and then reboot
			char msg[40];
			os_sprintf(msg, "Firmware updated, rebooting to rom %d...\r\n", rom_slot);
			to_console(msg);
			rboot_set_current_rom(rom_slot);
			system_restart();
			while (true);
		}
	} else {
		// fail
		to_console("Firmware update failed!\r\n");
	}
}

static void ICACHE_FLASH_ATTR OtaUpdate() {
	// start the upgrade process
	if (rboot_ota_start((ota_callback)OtaUpdate_CallBack)) {
		to_console("Updating...\r\n");
	} else {
		to_console("Updating failed!\r\n\r\n");
	}
}
#endif

// Use this from ROM instead
int ets_str2macaddr(uint8 *mac, char *str_mac);
#define parse_mac ets_str2macaddr
/*bool parse_mac(uint8_t *mac, uint8_t *inp)
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
*/
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
        os_sprintf(response, "show [config|stats|route|dhcp%s]\r\n",
#if ACLS
		"|acl"
#else
		""
#endif
#if MQTT_CLIENT
		"|mqtt"
#else
		""
#endif
#if OTAUPDATE
		"|ota"
#else
		""
#endif
	);
        to_console(response);

        os_sprintf_flash(response, "set [ssid|password|auto_connect|ap_ssid|ap_password|ap_on|ap_open|nat] <val>\r\n");
        to_console(response);
#if WPA2_PEAP
        os_sprintf_flash(response, "set [use_peap|peap_identity|peap_username|peap_password] <val>\r\n");
        to_console(response);
#endif
        os_sprintf_flash(response, "set [ap_mac|sta_mac|ssid_hidden|sta_hostname|max_clients] <val>\r\nset [network|dns|ip|netmask|gw] <val>\r\n");
        to_console(response);
#if HAVE_ENC28J60
#if DCHPSERVER_ENC28J60
        os_sprintf_flash(response, "set [eth_dhcpd] <val>\r\n");
        to_console(response);
#endif
        os_sprintf_flash(response, "set [eth_enable|eth_ip|eth_netmask|eth_gw|eth_mac] <val>\r\n");
        to_console(response);
#endif
        os_sprintf_flash(response, "set [tcp_timeout|udp_timeout] <val>\r\nroute clear|route add <network> <gw>|route delete <network>\r\ninterface <int> [up|down]\r\nportmap [add|remove] [TCP|UDP] <ext_port> <int_addr> <int_port>\r\n");
        to_console(response);
#if ACLS
        os_sprintf_flash(response, "show acl|acl [from_sta|to_sta|from_ap|to_ap] [IP|TCP|UDP] <src_addr> [<src_port>] <dest_addr> [<dest_port>] [allow|deny|allow_monitor|deny_monitor]\r\nacl [from_sta|to_sta|from_ap|to_ap] clear\r\n");
        to_console(response);
#endif
#if DAILY_LIMIT
        os_sprintf_flash(response, "set [daily_limit|timezone] <val>\r\n");
        to_console(response);
#endif
        os_sprintf_flash(response, "nslookup <name>");
        to_console(response);
#if ALLOW_PING
        os_sprintf_flash(response, "|ping <ip_addr>");
        to_console(response);
#endif
        os_sprintf_flash(response, "\r\n");
        to_console(response);
#if REMOTE_MONITORING
        os_sprintf_flash(response, "monitor [on|off] <portnumber>\r\n");
        to_console(response);
#endif
#if TOKENBUCKET
        os_sprintf_flash(response, "set [upstream_kbps|downstream_kbps] <val>\r\n");
        to_console(response);
#endif
        os_sprintf_flash(response, "set [automesh|am_threshold");
        to_console(response);
#if ALLOW_SLEEP
        os_sprintf_flash(response, "|am_scan_time|am_sleep_time");
        to_console(response);
#endif
        os_sprintf_flash(response, "] <val>\r\n");
        to_console(response);
        os_sprintf_flash(response, "set [speed|status_led|hw_reset|config_port|config_access|web_port] <val>\r\nsave [config|dhcp]\r\nconnect|disconnect|reset [factory]|lock|unlock <password>|quit\r\n");
        to_console(response);
        os_sprintf_flash(response, "set [client_watchdog|ap_watchdog] <val>\r\n");
        to_console(response);
#if ALLOW_SCANNING
        os_sprintf_flash(response, "scan\r\n");
        to_console(response);
#endif
#if PHY_MODE
        os_sprintf_flash(response, "set phy_mode [1|2|3]\r\n");
        to_console(response);
#endif
#if ALLOW_SLEEP
        os_sprintf_flash(response, "sleep <secs>\r\nset [vmin|vmin_sleep] <val>\r\n");
        to_console(response);
#endif
#if MQTT_CLIENT
        os_sprintf_flash(response, "set [mqtt_host|mqtt_port|mqtt_user|mqtt_password|mqtt_id|mqtt_qos|mqtt_prefix|mqtt_command_topic|mqtt_interval] <val>\r\n");
        to_console(response);
#endif
#if OTAUPDATE
        os_sprintf_flash(response, "ota [switch|update]\r\n");
        to_console(response);
#endif
        goto command_handled_2;
    }

    if (strcmp(tokens[0], "show") == 0)
    {
      int16_t i;
      struct portmap_table *p;
      ip_addr_t i_ip;

      if (nTokens == 1 || (nTokens == 2 && strcmp(tokens[1], "config") == 0)) {
        os_sprintf(response, "Version %s (build: %s)\r\n", ESP_REPEATER_VERSION, __TIMESTAMP__);
        to_console(response);

        os_sprintf(response, "STA: SSID:%s PW:%s%s\r\n",
                   config.ssid,
                   config.locked?"***":(char*)config.password,
                   config.auto_connect?"":" [AutoConnect:0]");
        to_console(response);
	if (*(int*)config.bssid != 0) {
		os_sprintf(response, "BSSID: %02x:%02x:%02x:%02x:%02x:%02x\r\n",
		    config.bssid[0], config.bssid[1], config.bssid[2],
		    config.bssid[3], config.bssid[4], config.bssid[5]);
		to_console(response);
	}
#if WPA2_PEAP
	if (config.use_PEAP) {
	        os_sprintf(response, "PEAP: Identity:%s Username:%s Password: %s\r\n",
			   config.PEAP_identity, config.PEAP_username,
			   config.locked?"***":(char*)config.PEAP_password);
        	to_console(response);
	}
#endif
	// if static IP, add it
	os_sprintf(response, config.my_addr.addr?"STA: IP: %d.%d.%d.%d Netmask: %d.%d.%d.%d Gateway: %d.%d.%d.%d\r\n":"",
		IP2STR(&config.my_addr), IP2STR(&config.my_netmask), IP2STR(&config.my_gw));
        to_console(response);
	// if static DNS, add it
	os_sprintf(response, config.dns_addr.addr?" DNS: %d.%d.%d.%d\r\n":"", IP2STR(&config.dns_addr));
        to_console(response);

	if (config.automesh_mode != AUTOMESH_OFF) {
	        os_sprintf(response, "Automesh: on (%s) Level: %d Threshold: -%d\r\n",
		config.automesh_mode==AUTOMESH_LEARNING?"learning":"operational",
		config.automesh_mode==AUTOMESH_OPERATIONAL?config.AP_MAC_address[2]:-1,
		config.automesh_threshold);
        	to_console(response);
	}
#if ALLOW_SLEEP
	if (config.am_scan_time != 0 && config.automesh_mode != AUTOMESH_OFF) {
            os_sprintf(response, "Automesh: Scan time: %d Sleep time: %d s\r\n", config.am_scan_time, config.am_sleep_time);
            to_console(response);
	}
#endif
        os_sprintf(response, "AP:  SSID:%s %s PW:%s%s%s IP:%d.%d.%d.%d/24%s\r\n",
                   config.ap_ssid,
		   config.ssid_hidden?"[hidden]":"",
                   config.locked?"***":(char*)config.ap_password,
                   config.ap_open?" [open]":"",
                   config.ap_on?"":" [disabled]",
		   IP2STR(&config.network_addr),
		   config.nat_enable?" [NAT]":"");
        to_console(response);

#if HAVE_ENC28J60
	if (config.eth_enable) {
	    os_sprintf(response, config.eth_addr.addr?"ETH IP: %d.%d.%d.%d Netmask: %d.%d.%d.%d Gateway: %d.%d.%d.%d\r\n":
		"ETH: DHCP\r\n", IP2STR(&config.eth_addr), IP2STR(&config.eth_netmask), IP2STR(&config.eth_gw));
	} else {
	    os_sprintf_flash(response, "ETH: disabled\r\n");
	}
        to_console(response);
#endif

	uint8_t mac_buf[20];
	char *rand = "";
	if (strcmp(config.STA_MAC_address, "random") == 0) {
	    uint8_t mac[6];
	    wifi_get_macaddr(STATION_IF, mac);
	    mac_2_buff(mac_buf, mac);
	    rand = " (random)";
	} else {
	    mac_2_buff(mac_buf, config.STA_MAC_address);
	}
	os_sprintf(response, "STA MAC: %s%s\r\n", mac_buf, rand);
	to_console(response);
	mac_2_buff(mac_buf, config.AP_MAC_address);
	os_sprintf(response, "AP MAC:  %s\r\n", mac_buf);
	to_console(response);
#if HAVE_ENC28J60
	if (config.eth_enable) {
	    mac_2_buff(mac_buf, config.ETH_MAC_address);
	    os_sprintf(response, "ETH MAC: %s\r\n", mac_buf);
	    to_console(response);
	}
#endif
	os_sprintf(response, "STA hostname: %s\r\n", config.sta_hostname);
	to_console(response);
	if (config.max_clients != MAX_CLIENTS) {
	    os_sprintf(response, "Max WiFi clients: %d\r\n", config.max_clients);
	    to_console(response);
	}

	if (config.tcp_timeout || config.udp_timeout) {
	    os_sprintf(response, "NAPT timeouts: TCP %ds UDP %ds\r\n", 
		config.tcp_timeout?config.tcp_timeout:IP_NAPT_TIMEOUT_MS_TCP/1000,
		config.udp_timeout?config.udp_timeout:IP_NAPT_TIMEOUT_MS_UDP/1000);
	    to_console(response);	    
	}

#if REMOTE_CONFIG
	if (config.config_port == 0 || config.config_access == 0) {
		os_sprintf_flash(response, "No network console access\r\n");
	} else {
		os_sprintf(response, "Network console access on port %d (mode %d)\r\n", config.config_port, config.config_access);
	}
	to_console(response);
#endif

        os_sprintf(response, "Clock speed: %d\r\n", config.clock_speed);
        to_console(response);
#if TOKENBUCKET
	if (config.kbps_ds != 0) {
            os_sprintf(response, "Downstream limit: %d kbps\r\n", config.kbps_ds);
            to_console(response);
	}
	if (config.kbps_us != 0) {
            os_sprintf(response, "Upstream limit: %d kbps\r\n", config.kbps_us);
            to_console(response);
	}
#endif
#if MQTT_CLIENT
        os_sprintf(response, "MQTT: %s\r\n", mqtt_enabled?"enabled":"disabled");
        to_console(response);
#endif
#if ALLOW_SLEEP
	if (config.Vmin != 0) {
            os_sprintf(response, "Vmin: %d mV Sleep time: %d s\r\n", config.Vmin, config.Vmin_sleep);
            to_console(response);
	}
#endif
	for (i = 0; i<IP_PORTMAP_MAX; i++) {
	    p = &ip_portmap_table[i];
	    if(p->valid) {
		i_ip.addr = p->daddr;
		os_sprintf(response, "Portmap: %s: " IPSTR ":%d -> "  IPSTR ":%d\r\n",
		   p->proto==IP_PROTO_TCP?"TCP":p->proto==IP_PROTO_UDP?"UDP":"???",
		   IP2STR(&my_ip), ntohs(p->mport), IP2STR(&i_ip), ntohs(p->dport));
		to_console(response);
	    }
	}
#if REMOTE_MONITORING
	if (!config.locked&&monitor_port != 0) {
           	os_sprintf(response, "Monitor (mode %s) started on port %d\r\n", acl_monitoring?"acl":"all", monitor_port);
		to_console(response);
	}
#endif
	goto command_handled_2;
      }

      if (nTokens == 2 && strcmp(tokens[1], "stats") == 0) {
           uint32_t time = (uint32_t)(get_long_systime()/1000000);
	   int16_t i;
	   enum phy_mode phy;

           os_sprintf(response, "System uptime: %d:%02d:%02d\r\n", time/3600, (time%3600)/60, time%60);
	   to_console(response);
#if DAILY_LIMIT
	   uint32_t current_stamp = sntp_get_current_timestamp();
	   os_sprintf(response, "Local time: %s\r", current_stamp?sntp_get_real_time(current_stamp):"no NTP sync\n");
	   to_console(response);
#endif
	   os_sprintf(response, "%d KiB in (%d packets)\r\n%d KiB out (%d packets)\r\n",
			(uint32_t)(Bytes_in/1024), Packets_in,
			(uint32_t)(Bytes_out/1024), Packets_out);
           to_console(response);
#if DAILY_LIMIT
	   if (config.daily_limit != 0) {
	       os_sprintf(response, "%d KiB of %d per day used\r\n",
			(uint32_t)(Bytes_per_day/1024), config.daily_limit);
               to_console(response);
	   }
#endif
           os_sprintf(response, "Power supply: %d.%03d V\r\n", Vdd/1000, Vdd%1000);
	   to_console(response);
#ifdef USER_GPIO_OUT
	   os_sprintf(response, "GPIO output status: %d\r\n", config.gpio_out_status);
           to_console(response);
#endif
#if PHY_MODE
	   phy = wifi_get_phy_mode();
	   os_sprintf(response, "Phy mode: %c\r\n", phy == PHY_MODE_11B?'b':phy == PHY_MODE_11G?'g':'n');
           to_console(response);
#endif
	   os_sprintf(response, "Free mem: %d\r\n", system_get_free_heap_size());
	   to_console(response);

	   if (connected) {
		uint8_t buf[20];
		struct netif *sta_nf = (struct netif *)eagle_lwip_getif(0);
		addr2str(buf, sta_nf->ip_addr.addr, sta_nf->netmask.addr);
		os_sprintf(response, "STA IP: %s GW: %d.%d.%d.%d\r\n", buf, IP2STR(&sta_nf->gw));
		to_console(response);
		os_sprintf(response, "STA RSSI: %d\r\n", wifi_station_get_rssi());
		to_console(response);
	   } else {
		os_sprintf_flash(response, "STA not connected\r\n");
		to_console(response);
	   }
#if HAVE_ENC28J60
	   if (eth_netif) {
		uint8_t buf[20];
		addr2str(buf, eth_netif->ip_addr.addr, eth_netif->netmask.addr);
		os_sprintf(response, "ETH IP: %s GW: %d.%d.%d.%d\r\n", buf, IP2STR(&eth_netif->gw));
	   } else {
		os_sprintf_flash(response, "ETH not initialized\r\n");
	   }
	   to_console(response);
#endif
	   if (config.ap_on)
		os_sprintf(response, "%d Station%s connected to SoftAP\r\n", wifi_softap_get_station_num(),
		  wifi_softap_get_station_num()==1?"":"s");
	   else
		os_sprintf(response, "AP disabled\r\n");
           to_console(response);
           struct station_info *station = wifi_softap_get_station_info();
	   while(station) {
		uint8_t sta_mac[20];
		mac_2_buff(sta_mac, station->bssid);
		os_sprintf(response, "Station: %s - "  IPSTR "\r\n", sta_mac, IP2STR(&station->ip));
		to_console(response);
                station = STAILQ_NEXT(station, next);
           }
           wifi_softap_free_station_info();

	   if (config.ap_watchdog >= 0 || config.client_watchdog >= 0) {
		os_sprintf(response, "AP watchdog: %d Client watchdog: %d\r\n", ap_watchdog_cnt, client_watchdog_cnt);
		to_console(response);
	   }
	   goto command_handled_2;
      }

      if (nTokens == 2 && strcmp(tokens[1], "route") == 0) {
	   int i;
	   struct netif *nif;
	   ip_addr_t ip;
	   ip_addr_t mask;
	   ip_addr_t gw;
	   uint8_t buf[20];

	   os_sprintf_flash(response, "Routing table:\r\nNetwork              Dest\r\n");
	   to_console(response);

           for (i = 0; ip_get_route(i, &ip, &mask, &gw); i++) {
		addr2str(buf, ip.addr, mask.addr);
		os_sprintf(response, buf);
		to_console(response);
		int j = 21-os_strlen(buf);
		for (; j>0; j--)
		  to_console(" ");
		os_sprintf(response, IPSTR "\r\n", IP2STR(&gw));
		to_console(response);
	   }

	   for (nif = netif_list; nif != NULL; nif = nif->next) {
		if (!netif_is_up(nif))
		    continue;
		addr2str(buf, nif->ip_addr.addr&nif->netmask.addr, nif->netmask.addr);
		os_sprintf(response, buf);
		to_console(response);
		int j = 21-os_strlen(buf);
		for (; j>0; j--)
		  to_console(" ");
		os_sprintf(response, "%c%c%d\r\n", nif->name[0], nif->name[1], nif->num);
		to_console(response);
	   }

	   /* On the ESP the STA netif is the hardcoded default */
	   struct netif *default_nf = (struct netif *)eagle_lwip_getif(0);

	   /* Only if it is down, the "real" lwip default is used */
	   if ((default_nf == NULL) || (!netif_is_up(default_nf))) {
		default_nf = netif_default;
	   }

	   if ((default_nf != NULL) && (netif_is_up(default_nf))) {
		os_sprintf_flash(response, "default              ");
		to_console(response);
		os_sprintf(response, IPSTR "\r\n", IP2STR(&default_nf->gw));
		to_console(response);
	   }
	   goto command_handled_2;
      }

      if (nTokens == 2 && strcmp(tokens[1], "dhcp") == 0) {
	   int i;
	   struct dhcps_pool *p;
	   os_sprintf_flash(response, "DHCP table:\r\n");
	   to_console(response);
           for (i = 0; (p = dhcps_get_mapping(i)); i++) {
		os_sprintf(response, "%02x:%02x:%02x:%02x:%02x:%02x - "  IPSTR " - %d\r\n",
		   p->mac[0], p->mac[1], p->mac[2], p->mac[3], p->mac[4], p->mac[5],
		   IP2STR(&p->ip), p->lease_timer);
		to_console(response);
	   }
	   goto command_handled_2;
      }
#if ACLS
      if (nTokens == 2 && strcmp(tokens[1], "acl") == 0) {
	   char *txt[] = {"From STA:\r\n", "To STA:\r\n", "From AP:\r\n", "To AP:\r\n"};
	   for (i = 0; i<MAX_NO_ACLS; i++) {
	       if (!acl_is_empty(i)) {
		   ringbuf_memcpy_into(console_tx_buffer, txt[i], os_strlen(txt[i]));
		   acl_show(i, response);
		   to_console(response);
	       }
	   }
	   os_sprintf(response, "Packets denied: %d Packets allowed: %d\r\n",
			acl_deny_count, acl_allow_count);
	   to_console(response);
	   goto command_handled_2;
      }
#endif
#if MQTT_CLIENT
      if (nTokens == 2 && strcmp(tokens[1], "mqtt") == 0) {
	   if (os_strcmp(config.mqtt_host, "none") == 0) {
	     os_sprintf_flash(response, "MQTT not enabled (no mqtt_host)\r\n");
	     to_console(response);
	     goto command_handled_2;
	   }
	   os_sprintf(response, "MQTT client %s\r\n", mqtt_connected ? "connected" : "disconnected");
	   to_console(response);
           os_sprintf(response, "MQTT host: %s\r\nMQTT port: %d\r\nMQTT user: %s\r\nMQTT password: %s\r\n",
		config.mqtt_host, config.mqtt_port, config.mqtt_user, config.locked?"***":(char*)config.mqtt_password);
	   to_console(response);
           os_sprintf(response, "MQTT id: %s\r\nMQTT prefix: %s\r\nMQTT QoS: %d\r\nMQTT command topic: %s\r\nMQTT gpio_out topic: %s\r\nMQTT interval: %d s\r\nMQTT mask: %04x\r\n",
		config.mqtt_id, config.mqtt_prefix, config.mqtt_qos, config.mqtt_command_topic, config.mqtt_gpio_out_topic, config.mqtt_interval, config.mqtt_topic_mask);
	   to_console(response);
	   goto command_handled_2;
      }
#endif
#if OTAUPDATE
      if (nTokens == 2 && strcmp(tokens[1], "ota") == 0) {
	   os_sprintf_flash(response, "Currently running rom %d\r\n", rboot_get_current_rom());
	   to_console(response);
	   os_sprintf(response, "Firmware update: %s:%d/%s\r\n", config.ota_host, config.ota_port,
			rboot_get_current_rom()?OTA_ROM0:OTA_ROM1);
	   to_console(response);
	   goto command_handled_2;
      }
#endif
    }
#if ACLS
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
        else if (strcmp(tokens[1],"from_ap")==0)
	    acl_no = 2;
        else if (strcmp(tokens[1],"to_ap")==0)
	    acl_no = 3;
	else {
	    os_sprintf(response, INVALID_ARG);
	    goto command_handled;
	}

	if (strcmp(tokens[2],"clear")==0) {
	    acl_clear(acl_no);
	    os_sprintf_flash(response, "ACL cleared\r\n");
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
#if REMOTE_MONITORING
	else if (strcmp(tokens[last_arg],"allow_monitor") == 0) allow = ACL_ALLOW|ACL_MONITOR;
	else if (strcmp(tokens[last_arg],"deny_monitor") == 0) allow = ACL_DENY|ACL_MONITOR;
#endif
        else {
	    os_sprintf(response, INVALID_ARG);
	    goto command_handled;
	}

	if (acl_add(acl_no, saddr, smask, daddr, dmask, proto, sport, dport, allow)) {
	    os_sprintf_flash(response, "ACL added\r\n");
	} else {
	    os_sprintf_flash(response, "ACL add failed\r\n");
	}
        goto command_handled;
    }
#endif /* ACLS */

    if (strcmp(tokens[0], "route") == 0)
    {
    ip_addr_t daddr;
    ip_addr_t dmask;
    ip_addr_t gw;

        if (config.locked)
        {
            os_sprintf(response, INVALID_LOCKED);
            goto command_handled;
        }

        if (nTokens == 2 && strcmp(tokens[1],"clear")==0) {
	    ip_delete_routes();
	    os_sprintf_flash(response, "All routes cleared\r\n");
	    goto command_handled;
	}

        if (nTokens == 3 && strcmp(tokens[1],"delete")==0) {
	    parse_IP_addr(tokens[2], (uint32_t*)&daddr.addr, (uint32_t*)&dmask.addr);

	    if (ip_rm_route(daddr, dmask)) {
		os_sprintf_flash(response, "Route deleted\r\n");
	    } else {
		os_sprintf_flash(response, "Route not found\r\n");
	    }
	    goto command_handled;
        }

        if (nTokens == 4 && strcmp(tokens[1],"add")==0) {
	    uint32_t dummy;
	    parse_IP_addr(tokens[2], (uint32_t*)&daddr.addr, (uint32_t*)&dmask.addr);
	    parse_IP_addr(tokens[3], (uint32_t*)&gw.addr, &dummy);

	    if (ip_add_route(daddr, dmask, gw)) {
		os_sprintf_flash(response, "Route added\r\n");
	    } else {
		os_sprintf_flash(response, "Route add failed\r\n");
	    }
	    goto command_handled;
        }

	os_sprintf(response, INVALID_ARG);
        goto command_handled;
    }

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
	    os_sprintf_flash(response, "Portmap failed\r\n");
	}
        goto command_handled;
    }

    if (strcmp(tokens[0], "connect") == 0)
    {
      if (config.locked) {
        os_sprintf(response, INVALID_LOCKED);
        goto command_handled;
      }
      if (nTokens > 1) {
        os_sprintf(response, INVALID_NUMARGS);
	goto command_handled;
      }

      user_set_station_config();
      os_sprintf(response, "Trying to connect to ssid %s, password: %s\r\n", config.ssid, config.password);

      wifi_station_disconnect();
      wifi_station_connect();

      goto command_handled;
    }

    if (strcmp(tokens[0], "disconnect") == 0)
    {
      if (config.locked) {
        os_sprintf(response, INVALID_LOCKED);
        goto command_handled;
      }
      if (nTokens > 1) {
        os_sprintf(response, INVALID_NUMARGS);
	goto command_handled;
      }

      os_sprintf_flash(response, "Disconnect from ssid\r\n");

      wifi_station_disconnect();

      goto command_handled;
    }

    if (strcmp(tokens[0], "interface") == 0)
    {
      if (config.locked) {
        os_sprintf(response, INVALID_LOCKED);
        goto command_handled;
      }
      if (nTokens != 3) {
        os_sprintf(response, INVALID_NUMARGS);
	goto command_handled;
      }
      if (os_strlen(tokens[1]) != 3) {
	os_sprintf_flash(response, "Invalid interface\r\n");
	goto command_handled;
      }
      struct netif *nif;
      for (nif = netif_list; nif != NULL; nif = nif->next) {
	if (nif->name[0] == tokens[1][0] &&
	    nif->name[1] == tokens[1][1] &&
	    nif->num == tokens[1][2]-'0') {
	  break;
	}
      }
      if (nif == NULL) {
	os_sprintf_flash(response, "Invalid interface\r\n");
	goto command_handled;
      }

      if (strcmp(tokens[2], "up") == 0) {
	netif_set_up(nif);
      } else if (strcmp(tokens[2], "down") == 0) {
	netif_set_down(nif);
      } else {
	os_sprintf_flash(response, "Invalid command\r\n");
      }

      os_sprintf(response, "Interface %s %s\r\n", tokens[1], tokens[2]);
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
        os_sprintf_flash(response, "Config saved\r\n");
        goto command_handled;
      }

      if (nTokens == 2 && strcmp(tokens[1], "dhcp") == 0) {
	int16_t i = 0;

	// Copy all active STAs and their DHCP mappings to the config
        struct station_info *station = wifi_softap_get_station_info();
	while(station) {
	    config.dhcps_p[i].ip = station->ip;
	    os_memcpy(config.dhcps_p[i].mac, station->bssid, sizeof(station->bssid));
            station = STAILQ_NEXT(station, next);
	    if (++i >= MAX_DHCP)
		break;
        }

/*	for (i = 0; i<MAX_DHCP && (p = dhcps_get_mapping(i)); i++) {
	  os_memcpy(&config.dhcps_p[i], p, sizeof(struct dhcps_pool));
	}
*/
	config.dhcps_entries = i;
        config_save(&config);
	// also save the portmap table
	blob_save(0, (uint32_t *)ip_portmap_table, sizeof(struct portmap_table) * IP_PORTMAP_MAX);
        os_sprintf_flash(response, "Config and DHCP table saved\r\n");
        goto command_handled;
      }
    }
    if (strcmp(tokens[0], "nslookup") == 0)
    {
	if (nTokens != 2) {
	    os_sprintf(response, INVALID_NUMARGS);
	    goto command_handled;
	}
	uint32_t result = espconn_gethostbyname(NULL, tokens[1], &resolve_ip, dns_resolved);
	if (result == ESPCONN_OK) {
		os_sprintf(response, "DNS lookup for %s: " IPSTR "\r\n", tokens[1], IP2STR(&resolve_ip));
	} else if (result == ESPCONN_INPROGRESS) {
		// lookup taking place, will call dns_resolved on completion
		return;
	} else {
		os_sprintf(response, "DNS lookup failed for: %s\r\n", tokens[1]);
	}
	goto command_handled;
    }
#if ALLOW_SCANNING
    if (strcmp(tokens[0], "scan") == 0)
    {
        to_console("Scanning...\r\n");
        currentconn = pespconn;
        wifi_station_scan(NULL,scan_done);
        goto command_handled;
    }
#endif
#if ALLOW_PING
    if (strcmp(tokens[0], "ping") == 0)
    {
	if (nTokens != 2) {
	    os_sprintf(response, INVALID_NUMARGS);
	    goto command_handled;
	}
        currentconn = pespconn;
	uint32_t result = espconn_gethostbyname(NULL, tokens[1], &resolve_ip, user_do_ping);
	if (result == ESPCONN_OK) {
		ip_addr_t ip;
		ip.addr = ipaddr_addr(tokens[1]);
		user_do_ping(tokens[1], &ip, NULL);
	}
        //user_do_ping(ipaddr_addr(tokens[1]));
        return;
    }
#endif
#if OTAUPDATE
    if (strcmp(tokens[0], "ota") == 0)
    {
	if (config.locked) {
	    os_sprintf(response, INVALID_LOCKED);
	    goto command_handled;
	}
	if (nTokens != 2) {
	    os_sprintf(response, INVALID_NUMARGS);
	    goto command_handled;
	}
        if (strcmp(tokens[1],"switch") == 0)
        {
	    Switch();
            os_sprintf_flash(response, "ROM switched\r\n");
            goto command_handled;
        }
        if (strcmp(tokens[1],"update") == 0)
        {
	    currentconn = pespconn;
	    OtaUpdate();
            os_sprintf_flash(response, "Update initiated\r\n");
            goto command_handled;
        }
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
	system_restart();
	while (true);

        goto command_handled;
    }

    if (strcmp(tokens[0], "quit") == 0)
    {
	remote_console_disconnect = 1;
	os_sprintf_flash(response, "Quitting console\r\n");
        goto command_handled;
    }
#if ALLOW_SLEEP
    if (strcmp(tokens[0], "sleep") == 0)
    {
	sleeptime = 10; // seconds
	if (nTokens == 2) sleeptime = atoi(tokens[1]);

        os_sprintf(response, "Going to deep sleep for %ds\r\n", sleeptime);

	// Start the timer
    	os_timer_setfn(&sleep_delay_timer, sleep_delay_timer_func, 0);
    	os_timer_arm(&sleep_delay_timer, 2000, 0);

        goto command_handled;
    }
#endif
    if (strcmp(tokens[0], "lock") == 0) {
	if (config.locked) {
	    os_sprintf_flash(response, "Config already locked\r\n");
	    goto command_handled;
	}
	if (nTokens == 1) {
	    if (os_strlen(config.lock_password) == 0) {
		os_sprintf_flash(response, "No password defined\r\n");
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
	    os_sprintf_flash(response, "Config unlocked\r\n");
	} else {
	    os_sprintf_flash(response, "Unlock failed. Invalid password\r\n");
	}
	goto command_handled;
    }

#if REMOTE_MONITORING
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
#if ACLS
	    || strcmp(tokens[1],"acl") == 0
#endif
					) {
  	    if (nTokens != 3) {
        	os_sprintf_flash(response, "Port number missing\r\n");
		goto command_handled;
            }
	    if (monitor_port != 0) {
		os_sprintf_flash(response, "Monitor already started\r\n");
		goto command_handled;
	    }

            monitor_port = atoi(tokens[2]);
	    if (monitor_port != 0) {
#if ACLS
		acl_monitoring = (strcmp(tokens[1],"acl") == 0);
#endif
		start_monitor(monitor_port);
		os_sprintf(response, "Started monitor on port %d\r\n", monitor_port);
                goto command_handled;
            } else {
		os_sprintf_flash(response, "Invalid monitor port\r\n");
		goto command_handled;
	    }
	}

	if (strcmp(tokens[1],"off") == 0) {
	    if (monitor_port == 0) {
		os_sprintf_flash(response, "Monitor already stopped\r\n");
		goto command_handled;
	    }
	    monitor_port = 0;
	    stop_monitor();
	    os_sprintf_flash(response, "Stopped monitor\r\n");
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
		if (config.automesh_mode != AUTOMESH_OFF) {
		  config.automesh_checked = 0;
		  config.automesh_mode = AUTOMESH_LEARNING;
		}
		config.auto_connect = 1;
                os_sprintf_flash(response, "SSID set (auto_connect = 1)\r\n");
                goto command_handled;
            }

            if (strcmp(tokens[1],"password") == 0)
            {
                os_sprintf(config.password, "%s", tokens[2]);
		if (config.automesh_mode != AUTOMESH_OFF) {
		  config.automesh_checked = 0;
		  config.automesh_mode = AUTOMESH_LEARNING;
		}

		// WiFi pw of the uplink network is also the default lock pw (backward compatibility)
		os_sprintf(config.lock_password, "%s", tokens[2]);

                os_sprintf_flash(response, "Password set\r\n");
                goto command_handled;
            }

            if (strcmp(tokens[1],"auto_connect") == 0)
            {
                config.auto_connect = atoi(tokens[2]);
                os_sprintf_flash(response, "Auto Connect set\r\n");
                goto command_handled;
            }

            if (strcmp(tokens[1],"sta_hostname") == 0)
            {
                os_sprintf(config.sta_hostname, "%s", tokens[2]);
                os_sprintf_flash(response, "STA hostname set\r\n");
                goto command_handled;
            }

            if (strcmp(tokens[1],"ap_ssid") == 0)
            {
                os_sprintf(config.ap_ssid, "%s", tokens[2]);
                os_sprintf_flash(response, "AP SSID set\r\n");
                goto command_handled;
            }

            if (strcmp(tokens[1],"ap_password") == 0)
            {
		if (os_strlen(tokens[2])<8) {
		    os_sprintf_flash(response, "Password too short (min. 8)\r\n");
		} else {
                    os_sprintf(config.ap_password, "%s", tokens[2]);
		    config.ap_open = 0;
                    os_sprintf_flash(response, "AP Password set\r\n");
		}
                goto command_handled;
            }

            if (strcmp(tokens[1],"ap_open") == 0)
            {
                config.ap_open = atoi(tokens[2]);
                os_sprintf_flash(response, "Open Auth set\r\n");
                goto command_handled;
            }

            if (strcmp(tokens[1],"nat") == 0)
            {
                config.nat_enable = atoi(tokens[2]);
		if (config.nat_enable) {
		    ip_napt_enable_no(1, 1);
		    os_sprintf_flash(response, "NAT enabled\r\n");
		} else {
		    ip_napt_enable_no(1, 0);
		    os_sprintf_flash(response, "NAT disabled\r\n");
		}
                goto command_handled;
            }

            if (strcmp(tokens[1],"ssid_hidden") == 0)
            {
                config.ssid_hidden = atoi(tokens[2]);
                os_sprintf_flash(response, "Hidden SSID set\r\n");
                goto command_handled;
            }

            if (strcmp(tokens[1],"max_clients") == 0)
            {
		if (atoi(tokens[2]) <= MAX_CLIENTS) {
		    config.max_clients = atoi(tokens[2]);
		    os_sprintf_flash(response, "Max clients set\r\n");
		} else {
		    os_sprintf(response, INVALID_ARG);
		}
                goto command_handled;
            }

            if (strcmp(tokens[1],"automesh") == 0)
            {
		if (config.automesh_mode != AUTOMESH_OFF && atoi(tokens[2]) == 0) {
		    config.automesh_mode = AUTOMESH_OFF;
		    *(int*)config.bssid = 0;
		    wifi_get_macaddr(SOFTAP_IF, config.AP_MAC_address);
		} else {
		    config.automesh_mode = AUTOMESH_LEARNING;
		    config.automesh_checked = 0;
		}
                os_sprintf(response, "Set automesh %s\r\n", config.automesh_mode?"on":"off");
                goto command_handled;
            }

            if (strcmp(tokens[1],"ap_watchdog") == 0)
            {
		if (strcmp(tokens[2],"none") == 0) {
		    config.ap_watchdog = ap_watchdog_cnt = -1;
		    os_sprintf_flash(response, "AP watchdog off\r\n");
		    goto command_handled;
		}
		int32_t wd_val = atoi(tokens[2]);
		if (wd_val < 30) {
		    os_sprintf_flash(response, "AP watchdog value invalid\r\n");
		    goto command_handled;
		}
                config.ap_watchdog = ap_watchdog_cnt = wd_val;
                os_sprintf(response, "AP watchdog set to %d\r\n", config.ap_watchdog);
                goto command_handled;
            }

            if (strcmp(tokens[1],"client_watchdog") == 0)
            {
		if (strcmp(tokens[2],"none") == 0) {
		    config.client_watchdog = client_watchdog_cnt = -1;
		    os_sprintf_flash(response, "Client watchdog off\r\n");
		    goto command_handled;
		}
		int32_t wd_val = atoi(tokens[2]);
		if (wd_val < 30) {
		    os_sprintf_flash(response, "Client watchdog value invalid\r\n");
		    goto command_handled;
		}
                config.client_watchdog = client_watchdog_cnt = wd_val;
                os_sprintf(response, "Client watchdog set to %d\r\n", config.client_watchdog);
                goto command_handled;
            }

            if (strcmp(tokens[1],"am_threshold") == 0)
            {
                config.automesh_threshold = atoi(tokens[2]);
                os_sprintf(response, "Automesh threshold set to -%d\r\n", config.automesh_threshold);
                goto command_handled;
            }
#if WPA2_PEAP
            if (strcmp(tokens[1],"use_peap") == 0)
            {
                config.use_PEAP = atoi(tokens[2]);
                os_sprintf_flash(response, "PEAP authenticaton set\r\n");
                goto command_handled;
            }

            if (strcmp(tokens[1],"peap_identity") == 0)
            {
		if (os_strlen(tokens[2]) > sizeof(config.PEAP_password)-1) {
		    os_sprintf(response, "Identity too long (max. %d)\r\n", sizeof(config.PEAP_identity)-1);
		} else {
                    os_sprintf(config.PEAP_identity, "%s", tokens[2]);
                    os_sprintf_flash(response, "PEAP identity set\r\n");
		}
                goto command_handled;
            }

            if (strcmp(tokens[1],"peap_username") == 0)
            {
		if (os_strlen(tokens[2]) > sizeof(config.PEAP_password)-1) {
		    os_sprintf(response, "Username too long (max. %d)\r\n", sizeof(config.PEAP_username)-1);
		} else {
                    os_sprintf(config.PEAP_username, "%s", tokens[2]);
                    os_sprintf_flash(response, "PEAP username set\r\n");
		}
                goto command_handled;
            }

            if (strcmp(tokens[1],"peap_password") == 0)
            {
		if (os_strlen(tokens[2]) > sizeof(config.PEAP_password)-1) {
		    os_sprintf(response, "Password too long (max. %d)\r\n", sizeof(config.PEAP_password)-1);
		} else {
                    os_sprintf(config.PEAP_password, "%s", tokens[2]);
                    os_sprintf_flash(response, "PEAP password set\r\n");
		}
                goto command_handled;
            }
#endif
#if ACLS
            if (strcmp(tokens[1],"acl_debug") == 0)
            {
		acl_debug = atoi(tokens[2]);
                os_sprintf_flash(response, "ACL debug set\r\n");
                goto command_handled;
            }
#endif
#if OTAUPDATE
	    if (strcmp(tokens[1], "ota_host") == 0)
	    {
		os_strncpy(config.ota_host, tokens[2], 64);
#if MQTT_CLIENT
		config.mqtt_host[63] = 0;
#endif
		os_sprintf_flash(response, "OTA host set\r\n");
        	goto command_handled;
	    }
            if (strcmp(tokens[1],"ota_port") == 0)
            {
                config.ota_port = atoi(tokens[2]);
                os_sprintf_flash(response, "OTA port set\r\n");
                goto command_handled;
            }
#endif
#if REMOTE_CONFIG
            if (strcmp(tokens[1],"config_port") == 0)
            {
                config.config_port = atoi(tokens[2]);
		if (config.config_port == 0)
		  os_sprintf_flash(response, "WARNING: if you save this, remote console access will be disabled!\r\n");
		else
		  os_sprintf(response, "Config port set to %d\r\n", config.config_port);
                goto command_handled;
            }

	    if (strcmp(tokens[1], "config_access") == 0) {
		config.config_access = atoi(tokens[2]) & (LOCAL_ACCESS | REMOTE_ACCESS);
		if (config.config_access == 0)
		    os_sprintf_flash(response, "WARNING: if you save this, remote console and web access will be disabled!\r\n");
		else
		    os_sprintf(response, "Config access set\r\n", config.config_port);
		goto command_handled;
	    }
#endif
#if WEB_CONFIG
            if (strcmp(tokens[1],"web_port") == 0)
            {
                config.web_port = atoi(tokens[2]);
		if (config.web_port == 0)
		  os_sprintf_flash(response, "WARNING: if you save this, web config will be disabled!\r\n");
		else
		  os_sprintf(response, "Web port set to %d\r\n", config.web_port);
                goto command_handled;
            }
#endif
#if DAILY_LIMIT
            if (strcmp(tokens[1],"daily_limit") == 0)
            {
                config.daily_limit = atoi(tokens[2]);
                os_sprintf_flash(response, "Daily_limit set\r\n");
                goto command_handled;
            }
            if (strcmp(tokens[1],"timezone") == 0)
            {
                config.ntp_timezone = atoi(tokens[2]);
                os_sprintf_flash(response, "NTP timezone set\r\n");
                goto command_handled;
            }
#endif
#if TOKENBUCKET
            if (strcmp(tokens[1],"downstream_kbps") == 0)
            {
                config.kbps_ds = atoi(tokens[2]);
                os_sprintf_flash(response, "Bitrate set\r\n");
                goto command_handled;
            }
            if (strcmp(tokens[1],"upstream_kbps") == 0)
            {
                config.kbps_us = atoi(tokens[2]);
                os_sprintf_flash(response, "Bitrate set\r\n");
                goto command_handled;
            }
#endif
#if ALLOW_SLEEP
            if (strcmp(tokens[1],"vmin") == 0)
            {
                config.Vmin = atoi(tokens[2]);
                os_sprintf_flash(response, "Vmin set\r\n");
                goto command_handled;
            }

            if (strcmp(tokens[1],"vmin_sleep") == 0)
            {
                config.Vmin_sleep = atoi(tokens[2]);
                os_sprintf_flash(response, "Vmin sleep time set\r\n");
                goto command_handled;
            }

            if (strcmp(tokens[1],"am_scan_time") == 0)
            {
                config.am_scan_time = atoi(tokens[2]);
                os_sprintf_flash(response, "Automesh scan time set\r\n");
                goto command_handled;
            }

            if (strcmp(tokens[1],"am_sleep_time") == 0)
            {
                config.am_sleep_time = atoi(tokens[2]);
                os_sprintf_flash(response, "Automesh sleep time set\r\n");
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
	                	os_sprintf_flash(response, "AP on\r\n");
			} else {
				os_sprintf_flash(response, "AP already on\r\n");
			}

		} else {
			if (config.ap_on) {
				wifi_set_opmode(STATION_MODE);
				config.ap_on = false;
                		os_sprintf_flash(response, "AP off\r\n");
			} else {
				os_sprintf_flash(response, "AP already off\r\n");
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
		    easygpio_outputSet (config.status_led, 1);
		}
		if (config.status_led == 1) {
		    // Enable output if serial pin was used as status LED
		    system_set_os_print(1);
		}
		config.status_led = atoi(tokens[2]);
		if (config.status_led > 16) {
		    os_sprintf_flash(response, "Status led disabled\r\n");
		    goto command_handled;
		}
		if (config.status_led == 1) {
		    // Disable output if serial pin is used as status LED
		    system_set_os_print(0);
		}
		easygpio_pinMode(config.status_led, EASYGPIO_NOPULL, EASYGPIO_OUTPUT);
		easygpio_outputSet (config.status_led, 0);
		os_sprintf(response, "Status led set to GPIO %d\r\n", config.status_led);
        	goto command_handled;
	    }

	    if (strcmp(tokens[1], "hw_reset") == 0)
	    {
		config.hw_reset = atoi(tokens[2]);
		if (config.hw_reset > 16) {
		    os_sprintf_flash(response, "HW factory reset disabled\r\n");
		    goto command_handled;
		}
		easygpio_pinMode(config.hw_reset, EASYGPIO_PULLUP, EASYGPIO_INPUT);
		os_sprintf(response, "\r\nHW factory reset set to GPIO %d\r\n", config.hw_reset);
        	goto command_handled;
	    }
#if PHY_MODE
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
            if (strcmp(tokens[1],"tcp_timeout") == 0)
            {
                config.tcp_timeout = atoi(tokens[2]);
		ip_napt_set_tcp_timeout(config.tcp_timeout);
                os_sprintf(response, "TCP NAPT timeout set to %ds\r\n", config.tcp_timeout);
                goto command_handled;
            }

            if (strcmp(tokens[1],"udp_timeout") == 0)
            {
                config.udp_timeout = atoi(tokens[2]);
		ip_napt_set_udp_timeout(config.udp_timeout);
                os_sprintf(response, "UDP NAPT timeout set to %ds\r\n", config.udp_timeout);
                goto command_handled;
            }

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
		    os_sprintf_flash(response, "DNS from DHCP\r\n");
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
		    os_sprintf_flash(response, "IP from DHCP\r\n");
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
                  os_sprintf_flash(response, "AP MAC set\r\n");
                goto command_handled;
            }

            if (strcmp(tokens[1],"sta_mac") == 0)
            {
		if (strcmp(tokens[2], "random") == 0) {
		  os_memcpy(config.STA_MAC_address, tokens[2], 6);
                  os_sprintf_flash(response, "STA MAC randomized\r\n");
                  goto command_handled;
		}
                if (!parse_mac(config.STA_MAC_address, tokens[2]))
		  os_sprintf(response, INVALID_ARG);
		else
                  os_sprintf_flash(response, "STA MAC set\r\n");
                goto command_handled;
            }

            if (strcmp(tokens[1],"bssid") == 0)
            {
                if (!parse_mac(config.bssid, tokens[2]))
		  os_sprintf(response, INVALID_ARG);
		else
                  os_sprintf_flash(response, "bssid set\r\n");
                goto command_handled;
            }
#if HAVE_ENC28J60
#if DCHPSERVER_ENC28J60
            if(strcmp(tokens[1],"eth_dhcpd") == 0) {
                    config.enc_DHCPserver = atoi(tokens[2]);
                if (config.enc_DHCPserver) {
                    os_sprintf_flash(response, "eth_dhcpd enabled\r\n");
		} else {
                    os_sprintf_flash(response, "eth_dhcpd disabled\r\n");
		}
                    goto command_handled;
            }
#endif
            if (strcmp(tokens[1],"eth_enable") == 0)
            {
                config.eth_enable = atoi(tokens[2]);
                if (config.eth_enable){
                    os_sprintf_flash(response, "eth enabled\r\n");
		} else {
                    os_sprintf_flash(response, "eth disabled\r\n");
		}
                goto command_handled;
            }

            if (strcmp(tokens[1],"eth_ip") == 0)
            {
		if (os_strcmp(tokens[2], "dhcp") == 0) {
		    config.eth_addr.addr = 0;
		    os_sprintf_flash(response, "ETH IP from DHCP\r\n");
		} else {
		    config.eth_addr.addr = ipaddr_addr(tokens[2]);
		    os_sprintf(response, "ETH IP address set to %d.%d.%d.%d\r\n",
			IP2STR(&config.eth_addr));
		}
                goto command_handled;
            }

            if (strcmp(tokens[1],"eth_netmask") == 0)
            {
                config.eth_netmask.addr = ipaddr_addr(tokens[2]);
                os_sprintf(response, "ETH IP netmask set to %d.%d.%d.%d\r\n",
			IP2STR(&config.eth_netmask));
                goto command_handled;
            }

            if (strcmp(tokens[1],"eth_gw") == 0)
            {
                config.eth_gw.addr = ipaddr_addr(tokens[2]);
                os_sprintf(response, "ETH Gateway set to %d.%d.%d.%d\r\n",
			IP2STR(&config.eth_gw));
                goto command_handled;
            }

            if (strcmp(tokens[1],"eth_mac") == 0)
            {
                if (!parse_mac(config.ETH_MAC_address, tokens[2]))
		  os_sprintf(response, INVALID_ARG);
		else
                  os_sprintf_flash(response, "ETH MAC set\r\n");
                goto command_handled;
            }
#endif
#if MQTT_CLIENT
	    if (strcmp(tokens[1], "mqtt_host") == 0)
	    {
		os_strncpy(config.mqtt_host, tokens[2], 32);
		config.mqtt_host[31] = 0;
		os_sprintf_flash(response, "MQTT host set\r\n");
        	goto command_handled;
	    }

	    if (strcmp(tokens[1], "mqtt_port") == 0)
	    {
		config.mqtt_port = atoi(tokens[2]);
		os_sprintf_flash(response, "MQTT port set\r\n");
        	goto command_handled;
	    }

	    if (strcmp(tokens[1], "mqtt_user") == 0)
	    {
		os_strncpy(config.mqtt_user, tokens[2], 32);
		config.mqtt_user[31] = 0;
		os_sprintf_flash(response, "MQTT user set\r\n");
        	goto command_handled;
	    }

	    if (strcmp(tokens[1], "mqtt_password") == 0)
	    {
		os_strncpy(config.mqtt_password, tokens[2], 32);
		config.mqtt_password[31] = 0;
		os_sprintf_flash(response, "MQTT password set\r\n");
        	goto command_handled;
	    }
	    if (strcmp(tokens[1], "mqtt_id") == 0)
	    {
		os_strncpy(config.mqtt_id, tokens[2], 32);
		config.mqtt_id[31] = 0;
		os_sprintf_flash(response, "MQTT id set\r\n");
        	goto command_handled;
	    }

        if (strcmp(tokens[1], "mqtt_qos") == 0)
	    {
        if (atoi(tokens[2]) <0 || atoi(tokens[2]) >2) {
            os_sprintf_flash(response, "Invalid QoS value\r\n");
        	goto command_handled;
        }
		config.mqtt_qos = atoi(tokens[2]);
		os_sprintf_flash(response, "MQTT QoS set\r\n");
        	goto command_handled;
	    }

	    if (strcmp(tokens[1], "mqtt_prefix") == 0)
	    {
		os_strncpy(config.mqtt_prefix, tokens[2], 64);
		config.mqtt_prefix[63] = 0;
		os_sprintf_flash(response, "MQTT prefix set\r\n");
        	goto command_handled;
	    }
	    if (strcmp(tokens[1], "mqtt_command_topic") == 0)
	    {
		os_strncpy(config.mqtt_command_topic, tokens[2], 64);
		config.mqtt_command_topic[63] = 0;
		os_sprintf_flash(response, "MQTT command topic set\r\n");
        	goto command_handled;
	    }
	    if (strcmp(tokens[1], "mqtt_interval") == 0)
	    {
		config.mqtt_interval = atoi(tokens[2]);
		os_sprintf_flash(response, "MQTT interval set\r\n");
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
		os_sprintf_flash(response, "MQTT gpio_out topic set\r\n");
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
#if GPIO_CMDS
    if (strcmp(tokens[0], "gpio") == 0)
    {
        /*
         * For gpio commands at least 4 tokens "gpio" pin:"[0-16]" action:"mode|set|get" value:"low|high|out|in|in_pullup" is needed
         * hence the check
         * Examples:
         *      Set GPIO pin 04 mode to output:
         *          gpio 4 mode out
         *      Set GPIO pin 04 to high:
         *          gpio 4 set high
         *      Set GPIO pin 16 to low:
         *          gpio 16 set low
         *      Get GPIO pin 2 value:
         *          gpio 2 get
         */
        if (nTokens < 3)
        {
            os_sprintf(response, INVALID_NUMARGS);
            goto command_handled;
        }
        else
        {
            uint16_t pin = atoi(tokens[1]); // 0-16
            uint8_t *action = tokens[2];    // mode|set|get
            uint8_t *value = nTokens == 4 ? tokens[3] : "";

            if ((pin < 0) && (pin > 16))
            {
                os_sprintf_flash(response, "Invalid pin number (try 0-16)\r\n");
                goto command_handled;
            }

            os_sprintf(response, "Successfuly executed %d %s %s\r\n", pin, action, value);

            if (strcmp(action, "mode")==0)
            {
                if (strcmp(value, "in")==0)
                {
                    easygpio_pinMode(pin, EASYGPIO_NOPULL, EASYGPIO_INPUT);
                    config.gpiomode[pin] = IN;
                    goto command_handled;
                }
                if (strcmp(value, "out")==0)
                {
                    easygpio_pinMode(pin, EASYGPIO_NOPULL, EASYGPIO_OUTPUT);
                    config.gpiomode[pin] = OUT;
                    goto command_handled;
                }
                if (strcmp(value, "in_pullup")==0)
                {
                    easygpio_pinMode(pin, EASYGPIO_PULLUP, EASYGPIO_OUTPUT);
                    config.gpiomode[pin] = IN_PULLUP;
                    goto command_handled;
                }
                os_sprintf_flash(response, "Invalid mode (in, in_pullup, or out)\r\n");
            }

            if (strcmp(action, "set")==0)
            {
                if (strcmp(value, "high")==0)
                {
                    easygpio_outputSet(pin, 1);
                    goto command_handled;
                }
                if (strcmp(value, "low")==0)
                {
                    easygpio_outputSet(pin, 0);
                    goto command_handled;
                }
                os_sprintf_flash(response, "Invalid value (high or low)\r\n");
            }

            if (strcmp(action, "get")==0)
            {
                uint16_t pinVal = easygpio_inputGet(pin);
                os_sprintf(response, "%d\r\n", pinVal);
                goto command_handled;
            }
        }
    }
#endif

    /* Control comes here only if the tokens[0] command is not handled */
    os_sprintf_flash(response, "\r\nInvalid Command\r\n");

command_handled:
    to_console(response);
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

#if REMOTE_CONFIG
static void ICACHE_FLASH_ATTR tcp_client_sent_cb(void *arg)
{
    uint16_t len;
    static uint8_t tbuf[1400];

    struct espconn *pespconn = (struct espconn *)arg;
    //os_printf("tcp_client_sent_cb(): Data sent to console\n");
    if (console_output != NULL) {
	os_free(console_output);
	console_output = NULL;
    }
}

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
    //os_printf("tcp_client_discon_cb(): client disconnected\n");
#if ACLS
    acl_debug = 0;
    deny_cb_conn = 0;
#endif
    struct espconn *pespconn = (struct espconn *)arg;
}


/* Called when a client connects to the console server */
static void ICACHE_FLASH_ATTR tcp_client_connected_cb(void *arg)
{
    struct espconn *pespconn = (struct espconn *)arg;

    //os_printf("tcp_client_connected_cb(): Client connected\r\n");

    if (!check_connection_access(pespconn, config.config_access)) {
	os_printf("Client disconnected - no config access on this network\r\n");
	espconn_disconnect(pespconn);
	return;
    }

    console_output = NULL;
    espconn_regist_sentcb(pespconn,     tcp_client_sent_cb);
    espconn_regist_disconcb(pespconn,   tcp_client_discon_cb);
    espconn_regist_recvcb(pespconn,     tcp_client_recv_cb);
    espconn_regist_time(pespconn,  300, 1);  // Specific to console only

    ringbuf_reset(console_rx_buffer);
    ringbuf_reset(console_tx_buffer);

    espconn_send(pespconn, "CMD>", 4);
#if ACLS
    deny_cb_conn = pespconn;
#endif
}
#endif /* REMOTE_CONFIG */


#if WEB_CONFIG
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
    //os_printf("web_config_client_recv_cb(): cmd line:%s\n",cmd_line);

    ringbuf_memcpy_into(console_rx_buffer, cmd_line, os_strlen(cmd_line));
    console_handle_command(pespconn);
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
    char *token[1];
    char *str;

    str = strstr(data, " /?");
    if (str != NULL)
    {
        str = strtok(str+3," ");

        char* keyval = strtok_r(str,"&",&kv);
        while (keyval != NULL)
        {
            char *key = strtok_r(keyval,"=", &sv);
            char *val = strtok_r(NULL, "=", &sv);

            keyval = strtok_r (NULL, "&", &kv);
            //os_printf("web_config_client_recv_cb(): key:%s:val:%s:\n",key,val);
            if (val != NULL)
            {

                if (strcmp(key, "ssid") == 0)
                {
		    parse_str_into_tokens(val, token, 1);
                    handle_set_cmd(pespconn, "set ssid", token[0]);
		    config.automesh_mode = AUTOMESH_OFF;
                    do_reset = true;
                }
                else if (strcmp(key, "password") == 0)
                {
		    parse_str_into_tokens(val, token, 1);
                    handle_set_cmd(pespconn, "set password", token[0]);
                    do_reset = true;
                }
                else if (strcmp(key, "am") == 0)
                {
                    config.automesh_mode = AUTOMESH_LEARNING;
		    config.automesh_checked = 0;
		    do_reset = true;
                }
                else if (strcmp(key, "lock") == 0)
                {
		    os_memcpy(config.lock_password, config.password, sizeof(config.lock_password));
                    config.locked = 1;
                }
                else if (strcmp(key, "ap_ssid") == 0)
                {
		    parse_str_into_tokens(val, token, 1);
                    handle_set_cmd(pespconn, "set ap_ssid", token[0]);
                    do_reset = true;
                }
                else if (strcmp(key, "ap_password") == 0)
                {
		    parse_str_into_tokens(val, token, 1);
                    handle_set_cmd(pespconn, "set ap_password", token[0]);
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
#if GPIO_CMDS
                else if (strcmp(key, "gpio") == 0)
                {
                    handle_set_cmd(pespconn, "gpio", val);
                }
#endif
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
    //os_printf("web_config_client_discon_cb(): client disconnected\n");
    struct espconn *pespconn = (struct espconn *)arg;
}

static void ICACHE_FLASH_ATTR web_config_client_sent_cb(void *arg)
{
    //os_printf("web_config_client_sent_cb(): data sent to client\n");
    struct espconn *pespconn = (struct espconn *)arg;

    espconn_disconnect(pespconn);
}

/* Called when a client connects to the web config */
static void ICACHE_FLASH_ATTR web_config_client_connected_cb(void *arg)
{

    struct espconn *pespconn = (struct espconn *)arg;

    //os_printf("web_config_client_connected_cb(): Client connected\r\n");

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
    	static const uint8_t config_page_str[] ICACHE_RODATA_ATTR STORE_ATTR = CONFIG_PAGE;
	uint32_t slen = (sizeof(config_page_str) + 4) & ~3;
	uint8_t *config_page = (char *)os_malloc(slen);
	if (config_page == NULL)
	    return;
	os_memcpy(config_page, config_page_str, slen);

	uint8_t *page_buf = (char *)os_malloc(slen+200);
	if (page_buf == NULL)
	    return;
	os_sprintf(page_buf, config_page, config.ssid, config.password,
		   config.automesh_mode!=AUTOMESH_OFF?"checked":"",
		   config.ap_ssid, config.ap_password,
		   config.ap_open?" selected":"", config.ap_open?"":" selected",
		   IP2STR(&config.network_addr));
	os_free(config_page);

	espconn_send(pespconn, page_buf, os_strlen(page_buf));

	os_free(page_buf);
    }
    else {
    	static const uint8_t lock_page_str[] ICACHE_RODATA_ATTR STORE_ATTR = LOCK_PAGE;
	uint32_t slen = (sizeof(lock_page_str) + 4) & ~3;
	uint8_t *lock_page = (char *)os_malloc(slen);
	if (lock_page == NULL)
	    return;
	os_memcpy(lock_page, lock_page_str, slen);

	espconn_send(pespconn, lock_page, sizeof(lock_page_str));

	os_free(lock_page);
    }
}
#endif /* WEB_CONFIG */


bool toggle;
// Timer cb function
void ICACHE_FLASH_ATTR timer_func(void *arg){
uint32_t Vcurr;
uint64_t t_new;
uint32_t t_diff;
#if TOKENBUCKET
uint32_t Bps;
#endif

    toggle = !toggle;

    // Check if watchdogs
    if (toggle){
	if (ap_watchdog_cnt >= 0) {
	    if (ap_watchdog_cnt == 0) {
		os_printf("AP watchdog reset\r\n");
		system_restart();
		while (true);
	    }
	    ap_watchdog_cnt--;
	}

	if (client_watchdog_cnt >= 0) {
	    if (client_watchdog_cnt == 0) {
		os_printf("Client watchdog reset\r\n");
		system_restart();
		while (true);
	    }
	    client_watchdog_cnt--;
	}
    }

    // Check the HW factory reset pin
    static count_hw_reset;
    if (config.hw_reset <= 16) {
	bool pin_in = easygpio_inputGet(config.hw_reset);
	if (!pin_in) {
	    count_hw_reset++;
	    if (toggle)
		os_printf(".");
	    if (count_hw_reset > 6) {
        	if (config.status_led <= 16)
		    easygpio_outputSet (config.status_led, true);
		os_printf("\r\nFactory reset\r\n");
		uint16_t pin = config.hw_reset;
		config_load_default(&config);
		config.hw_reset = pin;
		config_save(&config);
		blob_zero(0, sizeof(struct portmap_table) * IP_PORTMAP_MAX);
		system_restart();
		while (true);
	    }
	} else {
	    count_hw_reset = 0;
	}
    }

    if (config.status_led <= 16)
	easygpio_outputSet (config.status_led, toggle && connected);

    // Power measurement
    // Measure Vdd every second, sliding mean over the last 16 secs
    if (toggle) {

	Vcurr = (system_get_vdd33()*1000)/1024;
	Vdd = (Vdd * 3 + Vcurr)/4;
#if ALLOW_SLEEP
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

#if DAILY_LIMIT
    if (connected && toggle) {
	uint32_t current_stamp = sntp_get_current_timestamp();
	if(current_stamp != 0) {
	    char *s = sntp_get_real_time(current_stamp);
	    if (last_date != atoi(&s[8])) {
		Bytes_per_day = 0;
		last_date = atoi(&s[8]);
	    }
	}
    }
#endif

    t_new = get_long_systime();

#if TOKENBUCKET
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

#if MQTT_CLIENT
    t_diff = (uint32_t)((t_new-t_old)/1000000);
    if (mqtt_enabled && config.mqtt_interval != 0 && (t_diff > config.mqtt_interval)) {
	mqtt_publish_int(MQTT_TOPIC_UPTIME, "Uptime", "%d", (uint32_t)(t_new/1000000));
	mqtt_publish_int(MQTT_TOPIC_VDD, "Vdd", "%d", Vdd);
	mqtt_publish_int(MQTT_TOPIC_BYTES, "Bin", "%d", (uint32_t)(Bytes_in/1024));
	mqtt_publish_int(MQTT_TOPIC_BYTES, "Bout", "%d", (uint32_t)(Bytes_out/1024));
	mqtt_publish_int(MQTT_TOPIC_PACKETS, "Ppsin", "%d", (Packets_in-Packets_in_last)/t_diff);
	mqtt_publish_int(MQTT_TOPIC_PACKETS, "Ppsout", "%d", (Packets_out-Packets_out_last)/t_diff);
	mqtt_publish_int(MQTT_TOPIC_NOSTATIONS, "NoStations", "%d", config.ap_on?wifi_softap_get_station_num():0);
	mqtt_publish_int(MQTT_TOPIC_BPS, "Bpsin", "%d", (uint32_t)(Bytes_in-Bytes_in_last)/t_diff);
	mqtt_publish_int(MQTT_TOPIC_BPS, "Bpsout", "%d", (uint32_t)(Bytes_out-Bytes_out_last)/t_diff);
#if DAILY_LIMIT
	mqtt_publish_int(MQTT_TOPIC_BPD, "Bpd", "%d", (uint32_t)(Bytes_per_day/1024));
#endif
#ifdef USER_GPIO_OUT
	mqtt_publish_int(MQTT_TOPIC_GPIOOUT, "GpioOut", "%d", (uint32_t)config.gpio_out_status);
#endif


	if (config.mqtt_topic_mask & MQTT_TOPIC_TOPOLOGY) {
	    uint8_t *buffer = (uint8_t *)os_malloc(1024);

	    if (buffer != NULL) {
		uint8_t ap_mac[20], sta_mac[20], bssid_mac[20];
		ip_addr_t my_ap_ip = config.network_addr;
		my_ap_ip.addr |= 0x01000000;

		mac_2_buff(ap_mac, config.AP_MAC_address);

		uint8_t mac_buf[6];
		wifi_get_macaddr(STATION_IF, mac_buf);
		mac_2_buff(sta_mac, mac_buf);

		mac_2_buff(bssid_mac, uplink_bssid);

		os_sprintf(buffer, "{\"nodeinfo\":{\"id\":\"%s\",\"ap_mac\":\"%s\",\"sta_mac\":\"%s\",\"uplink_bssid\":\"%s\",\"ap_ip\":\"" IPSTR "\",\"sta_ip\":\"" IPSTR "\",\"rssi\":\"%d\",\"mesh_level\":\"%u\",\"no_stas\":\"%d\"},\"stas\":[",
			config.sta_hostname, ap_mac, sta_mac, bssid_mac,
			IP2STR(&my_ap_ip), IP2STR(&my_ip),
			wifi_station_get_rssi(),
			config.automesh_mode==AUTOMESH_OPERATIONAL?config.AP_MAC_address[2]:0,
			wifi_softap_get_station_num());

		struct station_info *station = wifi_softap_get_station_info();
		bool do_colon = false;
		while(station) {
		    if (do_colon)
			os_sprintf(&buffer[os_strlen(buffer)], ",");
		    do_colon = true;
		    mac_2_buff(sta_mac, station->bssid);
		    os_sprintf(&buffer[os_strlen(buffer)], "{\"mac\":\"%s\",\"ip\":\""  IPSTR "\"}", sta_mac, IP2STR(&station->ip));
		    station = STAILQ_NEXT(station, next);
		}
		wifi_softap_free_station_info();
		os_sprintf(&buffer[os_strlen(buffer)], "]}");

		mqtt_publish_str(MQTT_TOPIC_TOPOLOGY, "Topology", buffer);
		os_free(buffer);
	    }
	}

        t_old = t_new;
        Bytes_in_last = Bytes_in;
        Bytes_out_last = Bytes_out;
        Packets_in_last = Packets_in;
        Packets_out_last = Packets_out;
    }
#endif

    os_timer_arm(&ptimer, toggle?900:100, 0);
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
    case SIG_CONSOLE_TX_RAW:
        {
            struct espconn *pespconn = (struct espconn *) events->par;
            console_send_response(pespconn, events->sig == SIG_CONSOLE_TX);

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
#if HAVE_LOOPBACK
    case SIG_LOOPBACK:
	{
	    struct netif *netif = (struct netif *) events->par;
	    netif_poll(netif);
	}
        break;
#endif
#if MQTT_CLIENT
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
	mac_2_buff(mac_str, evt->event_info.connected.bssid);
        os_printf("connect to ssid %s, bssid %s, channel %d\r\n", evt->event_info.connected.ssid, mac_str, evt->event_info.connected.channel);
	my_channel = evt->event_info.connected.channel;
	os_memcpy(uplink_bssid, evt->event_info.connected.bssid, sizeof(uplink_bssid));

	bool wrong_bssid = false;
	if (*(int*)config.bssid != 0) {
	    for (i=0; i<6; i++) {
		if (evt->event_info.connected.bssid[i] != config.bssid[i]) {
		    wrong_bssid = true;
		    os_printf("connect to non configured bssid!");
		    break;
	        }
	    }
	}

	if (config.automesh_mode == AUTOMESH_OPERATIONAL && wrong_bssid) {
	    config.automesh_mode = AUTOMESH_LEARNING;
	    config_save(&config);
	    system_restart();
	    while (true);
	    return;
	}

#if WPA2_PEAP
	//if (config.use_PEAP) {
	//    wifi_station_clear_enterprise_identity();
	//    wifi_station_clear_enterprise_username();
	//    wifi_station_clear_enterprise_password();
	//}
#endif
        break;

    case EVENT_STAMODE_DISCONNECTED:
        os_printf("disconnect from ssid %s, reason %d\r\n", evt->event_info.disconnected.ssid, evt->event_info.disconnected.reason);
	connected = false;

#if MQTT_CLIENT
	if (mqtt_enabled) MQTT_Disconnect(&mqttClient);
#endif /* MQTT_CLIENT */

	os_memset(uplink_bssid, 0, sizeof(uplink_bssid));
	if (config.automesh_mode == AUTOMESH_OPERATIONAL) {
	  if (evt->event_info.disconnected.reason == 201) {
	    wifi_set_opmode(STATION_MODE);
	  }

	  config.automesh_tries++;

	  if (config.automesh_checked) {
	    if (config.automesh_tries <= 3)
	      break;
            os_printf("Connect to known SSID %s failed, rouge AP?\r\n", config.ssid);
	    *(int*)config.bssid = 0;
	    config.automesh_mode = AUTOMESH_LEARNING;
	  } else {
	    if (config.automesh_tries > 3) {
	      os_printf("Initial connect to SSID %s failed, check password - factory reset\r\n", config.ssid);
	      config_load_default(&config);
	    } else {
	      os_printf("Cannot connect to SSID %s - %d. trial\r\n", config.ssid, config.automesh_tries);
	    }
	  }

	  config_save(&config);
	  system_restart();
	  while (true);
	  return;
	}

        break;

    case EVENT_STAMODE_AUTHMODE_CHANGE:
        //os_printf("mode: %d -> %d\r\n", evt->event_info.auth_change.old_mode, evt->event_info.auth_change.new_mode);
        break;

    case EVENT_STAMODE_GOT_IP:

	if (config.dns_addr.addr == 0) {
	    dns_ip = dns_getserver(0);
	}
	dhcps_set_DNS(&dns_ip);

        os_printf("ip:" IPSTR ",mask:" IPSTR ",gw:" IPSTR ",dns:" IPSTR "\n", IP2STR(&evt->event_info.got_ip.ip), IP2STR(&evt->event_info.got_ip.mask), IP2STR(&evt->event_info.got_ip.gw), IP2STR(&dns_ip));

	my_ip = evt->event_info.got_ip.ip;
	connected = true;

	patch_netif(my_ip, my_input_sta, &orig_input_sta, my_output_sta, &orig_output_sta, false);

	// Update any predefined portmaps to the new IP addr
        for (i = 0; i<IP_PORTMAP_MAX; i++) {
	  if(ip_portmap_table[i].valid) {
	     ip_portmap_table[i].maddr = my_ip.addr;
	  }
	}

	if (config.automesh_mode == AUTOMESH_OPERATIONAL) {
	    wifi_set_opmode(STATIONAP_MODE);
	    if (config.automesh_checked == 0) {
		config.automesh_checked = 1;
		config_save(&config);
	    }
	    os_printf("Automesh successfully configured and started\r\n");
	}

#if MQTT_CLIENT
	if (mqtt_enabled) MQTT_Connect(&mqttClient);
#endif /* MQTT_CLIENT */

        // Post a Server Start message as the IP has been acquired to Task with priority 0
	system_os_post(user_procTaskPrio, SIG_START_SERVER, 0 );
        break;

    case EVENT_SOFTAPMODE_STACONNECTED:
	os_sprintf(mac_str, MACSTR, MAC2STR(evt->event_info.sta_connected.mac));
        os_printf("station: %s join, AID = %d\r\n", mac_str, evt->event_info.sta_connected.aid);
#if MQTT_CLIENT
	mqtt_publish_str(MQTT_TOPIC_JOIN, "join", mac_str);
#endif
	ip_addr_t ap_ip = config.network_addr;
	ip4_addr4(&ap_ip) = 1;
	patch_netif(ap_ip, my_input_ap, &orig_input_ap, my_output_ap, &orig_output_ap, config.nat_enable);
        break;

    case EVENT_SOFTAPMODE_STADISCONNECTED:
	os_sprintf(mac_str, MACSTR, MAC2STR(evt->event_info.sta_disconnected.mac));
        os_printf("station: %s leave, AID = %d\r\n", mac_str, evt->event_info.sta_disconnected.aid);
#if MQTT_CLIENT
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

   apConfig.max_connection = config.max_clients; // how many stations can connect to ESP8266 softAP at most.
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


#if WPA2_PEAP
void ICACHE_FLASH_ATTR user_set_wpa2_config()
{
   wifi_station_set_wpa2_enterprise_auth(1);

   //This is an option. If not call this API, the outer identity will be "anonymous@espressif.com".
   wifi_station_set_enterprise_identity(config.PEAP_identity, os_strlen(config.PEAP_identity));

   wifi_station_set_enterprise_username(config.PEAP_username, os_strlen(config.PEAP_username));
   wifi_station_set_enterprise_password(config.PEAP_password, os_strlen(config.PEAP_password));

   //This is an option for EAP_PEAP and EAP_TTLS.
   //wifi_station_set_enterprise_ca_cert(ca, os_strlen(ca)+1);
}
#endif


void ICACHE_FLASH_ATTR user_set_station_config(void)
{
    struct station_config stationConf;
    //char hostname[40];

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

    wifi_station_set_hostname(config.sta_hostname);

    wifi_set_event_handler_cb(wifi_handle_event_cb);

    wifi_station_set_auto_connect(config.auto_connect != 0);
}

#if MQTT_CLIENT
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
    	os_timer_setfn(&inttimer, int_timer_func, (void *)(uint32_t)easygpio_inputGet(USER_GPIO_IN));
    	os_timer_arm(&inttimer, 50, 0);

        // Reactivate interrupts foR GPIO
        //gpio_pin_intr_state_set(GPIO_ID_PIN(USER_GPIO_IN), GPIO_PIN_INTR_ANYEDGE);
    }
}
#endif /* USER_GPIO_IN */
#endif /* MQTT_CLIENT */

void ICACHE_FLASH_ATTR automesh_scan_done(void *arg, STATUS status)
{
  if (status == OK)
  {
    mesh_level = 0xff;
    int rssi = -1000;

    struct bss_info *bss_link;

    for (bss_link = (struct bss_info *)arg; bss_link != NULL; bss_link = bss_link->next.stqe_next)
    {
      if (os_strcmp(bss_link->ssid, config.ssid) == 0) {
	uint8_t this_mesh_level;

        os_printf("Found: %d,\"%s\",%d,\""MACSTR"\",%d",
                 bss_link->authmode, bss_link->ssid, bss_link->rssi,
                 MAC2STR(bss_link->bssid),bss_link->channel);
	if (bss_link->bssid[0] != 0x24 || bss_link->bssid[1] != 0x24)  {
	  this_mesh_level = 0;
	} else {
	  this_mesh_level = bss_link->bssid[2];
	}

	// If it is bad quality, give is a handicap of one level
	if (bss_link->rssi < -config.automesh_threshold)
	  this_mesh_level++;

	os_printf(", mesh level: %d\r\n", this_mesh_level);

	// Lower mesh level or same but better RSSI
	if (this_mesh_level < mesh_level ||
	    (this_mesh_level == mesh_level && bss_link->rssi > rssi)) {
	  rssi = bss_link->rssi;
	  mesh_level = this_mesh_level;
	  os_memcpy(config.bssid, bss_link->bssid, 6);
	}
      }
    }

    if (mesh_level < 0xff) {
      os_printf("Using: "MACSTR"\r\n", MAC2STR(config.bssid));

      config.AP_MAC_address[0] = 0x24;
      config.AP_MAC_address[1] = 0x24;
      config.AP_MAC_address[2] = mesh_level+1;
      os_get_random(&config.AP_MAC_address[3], 3);

      wifi_set_macaddr(SOFTAP_IF, config.AP_MAC_address);
      user_set_softap_wifi_config();

      IP4_ADDR(&config.network_addr, 10, 24, mesh_level+1, 1);

      config.automesh_mode = AUTOMESH_OPERATIONAL;
      config.automesh_tries = 0;

      config_save(&config);
      //wifi_set_macaddr(SOFTAP_IF, config.AP_MAC_address);

      system_restart();
      while (true);
      return;
    }

  }
  else
  {
     os_printf("Scan fail !!!\r\n");
  }

  os_printf("No AP with ssid %s found\r\n", config.ssid);

#if ALLOW_SLEEP
  if (config.am_scan_time && config.am_sleep_time) {
    int32_t secs_left = config.am_scan_time - ((uint32_t)(get_long_systime()/1000000));
    os_printf("%d s scanning time left\r\n", secs_left);

    if (secs_left < 0) {
      os_printf("Scan time exceeded - going to sleep\r\n");
      system_deep_sleep(config.am_sleep_time * 1000000);
      return;
    }
  }
#endif

  wifi_station_scan(NULL, automesh_scan_done);
}

void ICACHE_FLASH_ATTR to_scan(void) {
    if (config.automesh_mode == AUTOMESH_LEARNING){
      wifi_station_scan(NULL, automesh_scan_done);
    }
}

#if HAVE_LOOPBACK
void ICACHE_FLASH_ATTR *schedule_netif_poll(struct netif *netif) {
    system_os_post(0, SIG_LOOPBACK, (ETSParam) netif);
    return NULL;
}
#endif

void ICACHE_FLASH_ATTR user_init()
{
struct ip_info info;
struct espconn *pCon;

    connected = false;
    do_ip_config = false;
    my_ip.addr = 0;
    Bytes_in = Bytes_out = Bytes_in_last = Bytes_out_last = 0,
    Packets_in = Packets_out = Packets_in_last = Packets_out_last = 0;
    t_old = 0;
    os_memset(uplink_bssid, 0, sizeof(uplink_bssid));

#if DAILY_LIMIT
    Bytes_per_day = 0;
    last_date = 0;
#endif

#if TOKENBUCKET
    t_old_tb = 0;
    token_bucket_ds = token_bucket_us = 0;
#endif

    console_rx_buffer = ringbuf_new(MAX_CON_CMD_SIZE);
    console_tx_buffer = ringbuf_new(MAX_CON_SEND_SIZE);

    gpio_init();
    init_long_systime();

    UART_init_console(BIT_RATE_115200, 0, console_rx_buffer, console_tx_buffer);

    os_printf("\r\n\r\nWiFi Repeater %s starting\r\n\nrunning rom %d\r", ESP_REPEATER_VERSION, rboot_get_current_rom());

    // Load config
    if (config_load(&config)== 0) {
	// valid config in FLASH, can read portmap table
	blob_load(0, (uint32_t *)ip_portmap_table, sizeof(struct portmap_table) * IP_PORTMAP_MAX);
    } else {

	// clear portmap table
	blob_zero(0, sizeof(struct portmap_table) * IP_PORTMAP_MAX);
    }

    if (config.tcp_timeout != 0)
	ip_napt_set_tcp_timeout(config.tcp_timeout);
    if (config.udp_timeout != 0)
	ip_napt_set_udp_timeout(config.udp_timeout);

#if ACLS
    acl_debug = 0;
    int i;
    for(i=0; i< MAX_NO_ACLS; i++) {
	acl_clear_stats(i);
    }
    acl_set_deny_cb(acl_deny_cb);
#endif
    // Config GPIO pin as output
    if (config.status_led == 1) {
	// Disable output if serial pin is used as status LED
	system_set_os_print(0);
    }

    ap_watchdog_cnt = config.ap_watchdog;
    client_watchdog_cnt = config.client_watchdog;

    if (config.status_led <= 16) {
	easygpio_pinMode(config.status_led, EASYGPIO_NOPULL, EASYGPIO_OUTPUT);
	easygpio_outputSet (config.status_led, 0);
    }
#ifdef FACTORY_RESET_PIN
    if (config.hw_reset <= 16) {
	easygpio_pinMode(config.hw_reset, EASYGPIO_PULLUP, EASYGPIO_INPUT);
    }
#endif
#if MQTT_CLIENT
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

#if GPIO_CMDS
    for (i=0; i<17; i++) {
        if (config.gpiomode[i] == OUT) {
            easygpio_pinMode(i, EASYGPIO_NOPULL, EASYGPIO_OUTPUT);
        }
        if (config.gpiomode[i] == IN) {
            easygpio_pinMode(i, EASYGPIO_NOPULL, EASYGPIO_INPUT);
        }
        if (config.gpiomode[i] == IN_PULLUP) {
            easygpio_pinMode(i, EASYGPIO_PULLUP, EASYGPIO_INPUT);
        }
    }
#endif

    // In Automesh STA and AP passwords and credentials are the same
    if (config.automesh_mode != AUTOMESH_OFF) {
	os_memcpy(config.ap_ssid, config.ssid, sizeof(config.ssid));
	os_memcpy(config.ap_password, config.password, sizeof(config.password));

	if (config.automesh_mode == AUTOMESH_LEARNING) {
	  config.ap_on = 0;
	  config.auto_connect = 0;
	} else {
	  config.ap_on = 1;
	  config.auto_connect = 1;
	  config.ap_open = os_strncmp(config.password, "none", 4) == 0;
	}
    }

    // Configure the AP and start it, if required
    if (config.dns_addr.addr == 0)
	// Google's DNS as default, as long as we havn't got one from DHCP
	IP4_ADDR(&dns_ip, 8, 8, 8, 8);
    else
	// We have a static DNS server
	dns_ip.addr = config.dns_addr.addr;

    // Now config the STA-Mode
    user_set_station_config();
#if WPA2_PEAP
    if (config.use_PEAP) {
	user_set_wpa2_config();
	wifi_station_connect();
    }
#endif

    if (config.ap_on) {
	wifi_set_opmode(STATIONAP_MODE);
	wifi_set_macaddr(SOFTAP_IF, config.AP_MAC_address);
    	user_set_softap_wifi_config();
	do_ip_config = true;
    } else {
	wifi_set_opmode(STATION_MODE);
    }
    if (strcmp(config.STA_MAC_address, "random") == 0) {
	uint8_t random_mac[6];
	os_get_random(random_mac, 6);
	random_mac[0] &= 0xfe;
	wifi_set_macaddr(STATION_IF, random_mac);
    } else {
	wifi_set_macaddr(STATION_IF, config.STA_MAC_address);
    }

#if PHY_MODE
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

#if HAVE_LOOPBACK
    loopback_netif_init((netif_status_callback_fn)schedule_netif_poll);
#endif

#if REMOTE_CONFIG
    pCon = (struct espconn *)os_zalloc(sizeof(struct espconn));
    if (config.config_port != 0) {
	os_printf("Starting Console TCP Server on port %d\r\n", config.config_port);

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

#if WEB_CONFIG
    pCon = (struct espconn *)os_zalloc(sizeof(struct espconn));
    if (config.web_port != 0) {
        os_printf("Starting Web Config Server on port %d\r\n", config.web_port);


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

#if REMOTE_MONITORING
    monitoring_on = 0;
    monitor_port = 0;
    acl_monitoring = 0;
#endif

#if MQTT_CLIENT
    mqtt_connected = false;
    mqtt_enabled = (os_strcmp(config.mqtt_host, "none") != 0);
    if (mqtt_enabled) {
	MQTT_InitConnection(&mqttClient, config.mqtt_host, config.mqtt_port, 0);

//	MQTT_InitClient(&mqttClient, MQTT_CLIENT_ID, MQTT_USER, MQTT_PASS, MQTT_KEEPALIVE, MQTT_CLEAN_SESSION);
	if (os_strcmp(config.mqtt_user, "none") == 0) {
	  MQTT_InitClient(&mqttClient, config.mqtt_id, 0, 0, 120, 1);
	} else {
	  MQTT_InitClient(&mqttClient, config.mqtt_id, config.mqtt_user, config.mqtt_password, 120, 1);
	}
	uint8_t buf[256];
	os_sprintf(buf, "%s/status", config.mqtt_prefix);
	MQTT_InitLWT(&mqttClient, buf, "offline", 0, 1);
	MQTT_OnConnected(&mqttClient, mqttConnectedCb);
	MQTT_OnDisconnected(&mqttClient, mqttDisconnectedCb);
	MQTT_OnPublished(&mqttClient, mqttPublishedCb);
	MQTT_OnData(&mqttClient, mqttDataCb);
    }
#endif /* MQTT_CLIENT */

    remote_console_disconnect = 0;

    system_init_done_cb(to_scan);

    // Init power - set it to 3300mV
    Vdd = 3300;
#if HAVE_ENC28J60
    eth_netif = NULL;
    if (config.eth_enable) {
        os_printf("Starting enc28j60\r\n");
#ifdef ENC28J60_HW_RESET
	easygpio_pinMode(ENC28J60_HW_RESET, EASYGPIO_PULLUP, EASYGPIO_OUTPUT);
	easygpio_outputSet(ENC28J60_HW_RESET, 0);
	os_delay_us(500);
	easygpio_outputSet(ENC28J60_HW_RESET, 1);
	os_delay_us(1000);
#endif
	eth_netif = espenc_init(config.ETH_MAC_address, &config.eth_addr, &config.eth_netmask,
			    &config.eth_gw, (config.eth_addr.addr == 0));
    }
#if DCHPSERVER_ENC28J60
    if(config.enc_DHCPserver) {
        os_printf("Starting enc28j60 DHCPd\r\n");
            enc_dhcps_start(eth_netif);
    }
#endif
#endif

    system_update_cpu_freq(config.clock_speed);

#if DAILY_LIMIT
    sntp_setservername(0, "1.pool.ntp.org");
    sntp_setservername(1, "2.pool.ntp.org");
    sntp_set_timezone(config.ntp_timezone);
    sntp_init();
#endif

    // Start the timer
    os_timer_setfn(&ptimer, timer_func, 0);
    os_timer_arm(&ptimer, 500, 0);

    //Start task
    system_os_task(user_procTask, user_procTaskPrio, user_procTaskQueue, user_procTaskQueueLen);
}
