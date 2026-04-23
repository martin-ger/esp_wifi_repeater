//////////////////////////////////////////////////
// rBoot OTA sample code for ESP8266 C API.
// Copyright 2015 Richard A Burton
// richardaburton@gmail.com
// See license.txt for license terms.
// OTA code based on SDK sample from Espressif.
//////////////////////////////////////////////////

#include "user_config.h"
#if OTAUPDATE == 1

#include <c_types.h>
#include <user_interface.h>
#include <espconn.h>
#include <mem.h>
#include <osapi.h>

#include "rboot-ota.h"

#include "config_flash.h"

extern sysconfig_t config;
extern struct espconn *currentconn;

#ifdef __cplusplus
extern "C" {
#endif

#define UPGRADE_FLAG_IDLE		0x00
#define UPGRADE_FLAG_START		0x01
#define UPGRADE_FLAG_FINISH		0x02

typedef struct {
	uint8 rom_slot;   // rom slot to update, or FLASH_BY_ADDR
	ota_callback callback;  // user callback when completed
	uint32 total_len;
	uint32 content_len;
	bool ok;
	struct espconn *conn;
	ip_addr_t ip;
	rboot_write_status write_status;
} upgrade_status;

static upgrade_status *upgrade;
static os_timer_t ota_timer;

// clean up at the end of the update
// will call the user call back to indicate completion
void ICACHE_FLASH_ATTR rboot_ota_deinit() {

	bool result;
	uint8 rom_slot;
	ota_callback callback;
	struct espconn *conn;

	os_timer_disarm(&ota_timer);

	// save only remaining bits of interest from upgrade struct
	// then we can clean it up early, so disconnect callback
	// can distinguish between us calling it after update finished
	// or being called earlier in the update process
	conn = upgrade->conn;
	rom_slot = upgrade->rom_slot;
	callback = upgrade->callback;

	// clean up
	os_free(upgrade);
	upgrade = 0;

	// if connected, disconnect and clean up connection
	if (conn) espconn_disconnect(conn);

	// check for completion
	if (system_upgrade_flag_check() == UPGRADE_FLAG_FINISH) {
		result = true;
	} else {
		system_upgrade_flag_set(UPGRADE_FLAG_IDLE);
		result = false;
	}

	// call user call back
	if (callback) {
		callback(result, rom_slot);
	}

}

// called when connection receives data (hopefully the rom)
static void ICACHE_FLASH_ATTR upgrade_recvcb(void *arg, char *pusrdata, unsigned short length) {

	char *ptrData, *ptrLen, *ptr;
	char response[128];

	// disarm the timer
	os_timer_disarm(&ota_timer);

	if (!upgrade->ok && length < 12) {
		to_console("HTTP error - response too short\r\n");
		system_os_post(0, SIG_CONSOLE_TX_RAW, (ETSParam) currentconn);

		rboot_ota_deinit();
		return;
	}

	// Still in header?
	if (upgrade->content_len == 0) {
		// http response 200 ok?
		if (!upgrade->ok) {
			os_sprintf(response, "HTTP %c%c%c\r\n", pusrdata[9], pusrdata[10], pusrdata[11]);
			to_console(response);
  			system_os_post(0, SIG_CONSOLE_TX_RAW, (ETSParam) currentconn);

			if (os_strncmp(pusrdata + 9, "200", 3) == 0) {
				upgrade->ok = true;
			} else {
				// fail, not a 200 response
				rboot_ota_deinit();
				return;
			}
		}

		if ((ptrLen = (char*)os_strstr(pusrdata, "Content-Length: "))
			&& (ptrData = (char*)os_strstr(ptrLen, "\r\n\r\n"))) {

			// end of header/start of data
			ptrData += 4;
			// length of data after header in this chunk
			length -= (ptrData - pusrdata);
			// running total of download length
			upgrade->total_len += length;
			// process current chunk
			if (!rboot_write_flash(&upgrade->write_status, (uint8*)ptrData, length)) {
				// write error
				rboot_ota_deinit();
				return;
			}
			// work out total download size
			ptrLen += 16;
			ptr = (char *)os_strstr(ptrLen, "\r\n");
			*ptr = '\0'; // destructive
			upgrade->content_len = atoi(ptrLen);

			os_sprintf(response, "Binary length: %d\r\n", upgrade->content_len);
			to_console(response);
  			system_os_post(0, SIG_CONSOLE_TX_RAW, (ETSParam) currentconn);
		} else {
			return;
		}
	} else {
		// not the first chunk, process it 
		os_sprintf(response, " %d KB \r", upgrade->total_len/1024);
		to_console(response);
		system_os_post(0, SIG_CONSOLE_TX_RAW, (ETSParam) currentconn);
		
		upgrade->total_len += length;
		if (!rboot_write_flash(&upgrade->write_status, (uint8*)pusrdata, length)) {
			// write error
			rboot_ota_deinit();
			return;
		}
	}

	// check if we are finished
	if (upgrade->total_len == upgrade->content_len) {
		system_upgrade_flag_set(UPGRADE_FLAG_FINISH);
		// clean up and call user callback
		rboot_ota_deinit();
	} else if (upgrade->conn->state != ESPCONN_READ) {
		// fail, but how do we get here? premature end of stream?
		rboot_ota_deinit();
	} else {
		// timer for next recv
		os_timer_setfn(&ota_timer, (os_timer_func_t *)rboot_ota_deinit, 0);
		os_timer_arm(&ota_timer, OTA_NETWORK_TIMEOUT, 0);
	}
}

// disconnect callback, clean up the connection
// we also call this ourselves
static void ICACHE_FLASH_ATTR upgrade_disconcb(void *arg) {
	// use passed ptr, as upgrade struct may have gone by now
	struct espconn *conn = (struct espconn*)arg;

	os_timer_disarm(&ota_timer);
	if (conn) {
		if (conn->proto.tcp) {
			os_free(conn->proto.tcp);
		}
		os_free(conn);
	}

	// is upgrade struct still around?
	// if so disconnect was from remote end, or we called
	// ourselves to cleanup a failed connection attempt
	// must ensure disconnect was for this upgrade attempt,
	// not a previous one! this call back is async so another
	// upgrade struct may have been created already
	if (upgrade && (upgrade->conn == conn)) {
		// mark connection as gone
		upgrade->conn = 0;
		// end the update process
		rboot_ota_deinit();
	}
}

// successfully connected to update server, send the request
static void ICACHE_FLASH_ATTR upgrade_connect_cb(void *arg) {

	uint8 *request;

	// disable the timeout
	os_timer_disarm(&ota_timer);

	// register connection callbacks
	espconn_regist_disconcb(upgrade->conn, upgrade_disconcb);
	espconn_regist_recvcb(upgrade->conn, upgrade_recvcb);

	// http request string
	request = (uint8 *)os_malloc(512);
	if (!request) {
		os_printf("No ram!\r\n");
		rboot_ota_deinit();
		return;
	}
	os_sprintf((char*)request,
		"GET /%s HTTP/1.0\r\nHost: %s\r\n%s",
		(upgrade->rom_slot == FLASH_BY_ADDR ? OTA_FILE : (upgrade->rom_slot == 0 ? OTA_ROM0 : OTA_ROM1)),
		config.ota_host, HTTP_HEADER);

	// send the http request, with timeout for reply
	os_timer_setfn(&ota_timer, (os_timer_func_t *)rboot_ota_deinit, 0);
	os_timer_arm(&ota_timer, OTA_NETWORK_TIMEOUT, 0);
	espconn_sent(upgrade->conn, request, os_strlen((char*)request));
	os_free(request);
}

// connection attempt timed out
static void ICACHE_FLASH_ATTR connect_timeout_cb() {
	os_printf("Connect timeout.\r\n");
	// not connected so don't call disconnect on the connection
	// but call our own disconnect callback to do the cleanup
	upgrade_disconcb(upgrade->conn);
}

static const char* ICACHE_FLASH_ATTR esp_errstr(sint8 err) {
	switch(err) {
		case ESPCONN_OK:
			return "No error, everything OK.";
		case ESPCONN_MEM:
			return "Out of memory error.";
		case ESPCONN_TIMEOUT:
			return "Timeout.";
		case ESPCONN_RTE:
			return "Routing problem.";
		case ESPCONN_INPROGRESS:
			return "Operation in progress.";
		case ESPCONN_ABRT:
			return "Connection aborted.";
		case ESPCONN_RST:
			return "Connection reset.";
		case ESPCONN_CLSD:
			return "Connection closed.";
		case ESPCONN_CONN:
			return "Not connected.";
		case ESPCONN_ARG:
			return "Illegal argument.";
		case ESPCONN_ISCONN:
			return "Already connected.";
	}
}

// call back for lost connection
static void ICACHE_FLASH_ATTR upgrade_recon_cb(void *arg, sint8 errType) {
	os_printf("Connection error: %s\r\n", esp_errstr(errType));
	//os_printf(esp_errstr(errType));
	//os_printf("\r\n");
	// not connected so don't call disconnect on the connection
	// but call our own disconnect callback to do the cleanup
	upgrade_disconcb(upgrade->conn);
}

// call back for dns lookup
static void ICACHE_FLASH_ATTR upgrade_resolved(const char *name, ip_addr_t *ip, void *arg) {

	if (ip == 0) {
		os_printf("DNS lookup failed for: %s\r\n", config.ota_host);
		//os_printf(config.ota_host);
		//os_printf("\r\n");
		// not connected so don't call disconnect on the connection
		// but call our own disconnect callback to do the cleanup
		upgrade_disconcb(upgrade->conn);
		return;
	}

	// set up connection
	upgrade->conn->type = ESPCONN_TCP;
	upgrade->conn->state = ESPCONN_NONE;
	upgrade->conn->proto.tcp->local_port = espconn_port();
	upgrade->conn->proto.tcp->remote_port = config.ota_port;
	*(ip_addr_t*)upgrade->conn->proto.tcp->remote_ip = *ip;
	// set connection call backs
	espconn_regist_connectcb(upgrade->conn, upgrade_connect_cb);
	espconn_regist_reconcb(upgrade->conn, upgrade_recon_cb);

	// try to connect
	espconn_connect(upgrade->conn);

	// set connection timeout timer
	os_timer_disarm(&ota_timer);
	os_timer_setfn(&ota_timer, (os_timer_func_t *)connect_timeout_cb, 0);
	os_timer_arm(&ota_timer, OTA_NETWORK_TIMEOUT, 0);
}

// start the ota process, with user supplied options
bool ICACHE_FLASH_ATTR rboot_ota_start(ota_callback callback) {

	uint8 slot;
	rboot_config bootconf;
	err_t result;

	// check not already updating
	if (system_upgrade_flag_check() == UPGRADE_FLAG_START) {
		return false;
	}

	// create upgrade status structure
	upgrade = (upgrade_status*)os_zalloc(sizeof(upgrade_status));
	if (!upgrade) {
		os_printf("No ram!\r\n");
		return false;
	}

	// store the callback
	upgrade->callback = callback;

	// get details of rom slot to update
	bootconf = rboot_get_config();
	slot = bootconf.current_rom;
	if (slot == 0) slot = 1; else slot = 0;
	upgrade->rom_slot = slot;

	// flash to rom slot
	upgrade->write_status = rboot_write_init(bootconf.roms[upgrade->rom_slot]);
	// to flash a file (e.g. containing a filesystem) to an arbitrary location
	// (e.g. 0x40000 bytes after the start of the rom) use code this like instead:
	// Note: address must be start of a sector (multiple of 4k)!
	//upgrade->write_status = rboot_write_init(bootconf.roms[upgrade->rom_slot] + 0x40000);
	//upgrade->rom_slot = FLASH_BY_ADDR;

	// create connection
	upgrade->conn = (struct espconn *)os_zalloc(sizeof(struct espconn));
	if (!upgrade->conn) {
		os_printf("No ram!\r\n");
		os_free(upgrade);
		return false;
	}
	upgrade->conn->proto.tcp = (esp_tcp *)os_zalloc(sizeof(esp_tcp));
	if (!upgrade->conn->proto.tcp) {
		os_printf("No ram!\r\n");
		os_free(upgrade->conn);
		os_free(upgrade);
		return false;
	}

	// set update flag
	system_upgrade_flag_set(UPGRADE_FLAG_START);

	// dns lookup
	result = espconn_gethostbyname(upgrade->conn, config.ota_host, &upgrade->ip, upgrade_resolved);
	if (result == ESPCONN_OK) {
		// hostname is already cached or is actually a dotted decimal ip address
		upgrade_resolved(0, &upgrade->ip, upgrade->conn);
	} else if (result == ESPCONN_INPROGRESS) {
		// lookup taking place, will call upgrade_resolved on completion
	} else {
		os_printf("DNS error!\r\n");
		os_free(upgrade->conn->proto.tcp);
		os_free(upgrade->conn);
		os_free(upgrade);
		return false;
	}

	return true;
}

#ifdef __cplusplus
}
#endif

#endif