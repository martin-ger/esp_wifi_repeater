#include "user_interface.h"
#include "lwip/ip.h"
#include "config_flash.h"


/*     From the document 99A-SDK-Espressif IOT Flash RW Operation_v0.2      *
 * -------------------------------------------------------------------------*
 * Flash is erased sector by sector, which means it has to erase 4Kbytes one
 * time at least. When you want to change some data in flash, you have to
 * erase the whole sector, and then write it back with the new data.
 *--------------------------------------------------------------------------*/
void ICACHE_FLASH_ATTR config_load_default(sysconfig_p config)
{
uint8_t mac[6];
uint32_t reg0, reg1, reg3;

    os_memset(config, 0, sizeof(sysconfig_t));
    os_printf("Loading default configuration\r\n");
    config->magic_number                = MAGIC_NUMBER;
    config->length                      = sizeof(sysconfig_t);

    reg0 = *(uint32*)(0x3ff00050);
    reg1 = *(uint32*)(0x3ff00054);
    reg3 = *(uint32*)(0x3ff0005c);

    if (reg3 != 0) {
	mac[0] = (reg3 >> 16) & 0xff;
	mac[1] = (reg3 >> 8) & 0xff;
	mac[2] = reg3 & 0xff;
    } else
    if (((reg1 >> 16) & 0xff) == 0) {
	mac[0] = 0x1a;
	mac[1] = 0xfe;
	mac[2] = 0x34;
    } else 
    if (((reg1 >> 16) & 0xff) == 1) {
	mac[0] = 0xac;
	mac[1] = 0xd0;
	mac[2] = 0x74;
    } else {
	os_printf("MAC read error\r\n");
    }
    mac[3] = (reg1 >> 8) & 0xff;
    mac[4] = reg1 & 0xff;
    mac[5] = (reg0 >> 24) & 0xff;

    os_printf("%02x:%02x:%02x\r\n", reg0, reg1, reg3);
    os_printf("%02x:%02x:%02x:%02x:%02x:%02x\r\n", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    os_memcpy(config->STA_MAC_address, mac, 6);
    mac[0] |= 0x02;
    os_memcpy(config->AP_MAC_address, mac, 6);

    os_sprintf(config->ssid,"%s",       WIFI_SSID);
    os_sprintf(config->password,"%s",   WIFI_PASSWORD);
    config->auto_connect                = 0;
    os_memset(config->bssid, 0, 6);
    os_sprintf(config->sta_hostname,"ESP_%02x%02x%02x", mac[3], mac[4], mac[5]);
    os_sprintf(config->ap_ssid,"%s",    WIFI_AP_SSID);
    os_sprintf(config->ap_password,"%s",WIFI_AP_PASSWORD);
    config->ap_open			= 1;
    config->ap_on			= 1;
    config->ssid_hidden			= 0;
    config->max_clients			= MAX_CLIENTS;
#ifdef WPA2_PEAP
    config->use_PEAP			= 0;
    config->PEAP_identity[0]		= '\0';
    config->PEAP_username[0]		= '\0';
    config->PEAP_password[0]		= '\0';
#endif
    config->lock_password[0]		= '\0';
    config->locked			= 0;

    config->ap_watchdog			= -1;
    config->client_watchdog		= -1;

    config->automesh_mode		= 0;
    config->automesh_checked		= 0;
    config->automesh_tries		= 0;
    config->automesh_threshold		= 85;
    config->am_scan_time		= 0;
    config->am_sleep_time		= 0;

    config->nat_enable			= 1;
    IP4_ADDR(&config->network_addr, 192, 168, 4, 1);
    config->dns_addr.addr		= 0;  // use DHCP
    config->my_addr.addr		= 0;  // use DHCP   
    config->my_netmask.addr		= 0;  // use DHCP   
    config->my_gw.addr			= 0;  // use DHCP   
#ifdef PHY_MODE
    config->phy_mode			= 3;  // mode n
#endif
    config->clock_speed			= 80;
    config->status_led			= STATUS_LED_GIPO;
    config->hw_reset			= FACTORY_RESET_PIN;
#ifdef DAILY_LIMIT
    config->daily_limit			= 0;
    config->ntp_timezone		= 0;
#endif
#ifdef ALLOW_SLEEP
    config->Vmin			= 0;
    config->Vmin_sleep			= 60;
#endif
#ifdef REMOTE_CONFIG
    config->config_port			= CONSOLE_SERVER_PORT;
#endif
#ifdef WEB_CONFIG
    config->web_port			= WEB_CONFIG_PORT;
#endif
    config->config_access		= LOCAL_ACCESS | REMOTE_ACCESS;
#ifdef TOKENBUCKET
    config->kbps_ds			= 0;
    config->kbps_us			= 0;
#endif

#ifdef MQTT_CLIENT
    os_sprintf(config->mqtt_host,"%s", "none");
    config->mqtt_port			= 1883;
    os_sprintf(config->mqtt_user,"%s", "none");
    config->mqtt_password[0]		= 0;
    os_sprintf(config->mqtt_id,"%s_%02x%02x%02x", MQTT_ID, mac[3], mac[4], mac[5]);
    os_sprintf(config->mqtt_prefix,"%s/%s/system", MQTT_PREFIX, config->mqtt_id);
    os_sprintf(config->mqtt_command_topic,"%s/%s/%s", MQTT_PREFIX, config->mqtt_id, "command");
    os_sprintf(config->mqtt_gpio_out_topic,"%s/%s/%s", MQTT_PREFIX, config->mqtt_id, "switch");
    config->gpio_out_status		= 0;
    config->mqtt_interval		= MQTT_REPORT_INTERVAL;
    config->mqtt_topic_mask		= 0xffff;
#endif

#ifdef HAVE_ENC28J60
    mac[0] ^= 0x04;
    os_memcpy(config->ETH_MAC_address, mac, 6);
    config->eth_addr.addr		= 0;  // use DHCP   
    config->eth_netmask.addr		= 0;  // use DHCP   
    config->eth_gw.addr			= 0;  // use DHCP
    config->eth_enable			= 0;  // off
#endif

    config->no_routes			= 0;

    config->dhcps_entries		= 0;
#ifdef ACLS
    acl_init();	// initializes the ACLs, written in config during save
#endif
}

int ICACHE_FLASH_ATTR config_load(sysconfig_p config)
{
    if (config == NULL) return -1;
    uint16_t base_address = FLASH_BLOCK_NO;

    spi_flash_read(base_address* SPI_FLASH_SEC_SIZE, &config->magic_number, 4);

    if((config->magic_number != MAGIC_NUMBER))
    {
        os_printf("\r\nNo config found, saving default in flash\r\n");
        config_load_default(config);
        config_save(config);
        return -1;
    }

    os_printf("\r\nConfig found and loaded\r\n");
    spi_flash_read(base_address * SPI_FLASH_SEC_SIZE, (uint32 *) config, sizeof(sysconfig_t));
    if (config->length != sizeof(sysconfig_t))
    {
        os_printf("Length Mismatch, probably old version of config, loading defaults\r\n");
        config_load_default(config);
        config_save(config);
	return -1;
    }

    ip_route_max = config->no_routes;
    os_memcpy(ip_rt_table, config->rt_table, sizeof(ip_rt_table));

#ifdef ACLS
    os_memcpy(&acl, &(config->acl), sizeof(acl));
    os_memcpy(&acl_freep, &(config->acl_freep), sizeof(acl_freep));
#endif
    return 0;
}

void ICACHE_FLASH_ATTR config_save(sysconfig_p config)
{
    uint16_t base_address = FLASH_BLOCK_NO;
    config->no_routes = ip_route_max;
    os_memcpy(config->rt_table, ip_rt_table, sizeof(ip_rt_table));

#ifdef ACLS
    os_memcpy(&(config->acl), &acl, sizeof(acl));
    os_memcpy(&(config->acl_freep), &acl_freep, sizeof(acl_freep));
#endif
    os_printf("Saving configuration\r\n");
    spi_flash_erase_sector(base_address);
    spi_flash_write(base_address * SPI_FLASH_SEC_SIZE, (uint32 *)config, sizeof(sysconfig_t));
}

void ICACHE_FLASH_ATTR blob_save(uint8_t blob_no, uint32_t *data, uint16_t len)
{
    uint16_t base_address = FLASH_BLOCK_NO + 1 + blob_no;
    spi_flash_erase_sector(base_address);
    spi_flash_write(base_address * SPI_FLASH_SEC_SIZE, data, len);
}

void ICACHE_FLASH_ATTR blob_load(uint8_t blob_no, uint32_t *data, uint16_t len)
{
    uint16_t base_address = FLASH_BLOCK_NO + 1 + blob_no;
    spi_flash_read(base_address * SPI_FLASH_SEC_SIZE, data, len);
}

void ICACHE_FLASH_ATTR blob_zero(uint8_t blob_no, uint16_t len)
{
int i;
    uint8_t z[len];
    os_memset(z, 0,len);
    uint16_t base_address = FLASH_BLOCK_NO + 1 + blob_no;
    spi_flash_erase_sector(base_address);
    spi_flash_write(base_address * SPI_FLASH_SEC_SIZE, (uint32_t *)z, len);
}

