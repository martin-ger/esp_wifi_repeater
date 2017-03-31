#include "ip_addr.h"
#include "config_flash.h"


/*     From the document 99A-SDK-Espressif IOT Flash RW Operation_v0.2      *
 * -------------------------------------------------------------------------*
 * Flash is erased sector by sector, which means it has to erase 4Kbytes one
 * time at least. When you want to change some data in flash, you have to
 * erase the whole sector, and then write it back with the new data.
 *--------------------------------------------------------------------------*/
void config_load_default(sysconfig_p config)
{
uint8_t mac[6];

    os_memset(config, 0, sizeof(sysconfig_t));
    os_printf("Loading default configuration\r\n");
    config->magic_number                = MAGIC_NUMBER;
    config->length                      = sizeof(sysconfig_t);
    os_sprintf(config->ssid,"%s",       WIFI_SSID);
    os_sprintf(config->password,"%s",   WIFI_PASSWORD);
    config->auto_connect                = 1;
    os_sprintf(config->ap_ssid,"%s",    WIFI_AP_SSID);
    os_sprintf(config->ap_password,"%s",WIFI_AP_PASSWORD);
    config->ap_open			= 1;
    config->ap_on			= 1;
    config->locked			= 0;

    IP4_ADDR(&config->network_addr, 192, 168, 4, 1);
    config->dns_addr.addr		= 0;  // use DHCP
    config->my_addr.addr		= 0;  // use DHCP   
    config->my_netmask.addr		= 0;  // use DHCP   
    config->my_gw.addr			= 0;  // use DHCP   

    config->clock_speed			= 80;
    config->Vmin			= 0;
    config->Vmin_sleep			= 60;

    config->config_port			= CONSOLE_SERVER_PORT;

    os_sprintf(config->mqtt_host,"%s", "none");
    config->mqtt_port			= 1883;
    os_sprintf(config->mqtt_user,"%s", "none");
    config->mqtt_password[0]		= 0;
    wifi_get_macaddr(0, mac);
    os_sprintf(config->mqtt_id,"%s_%2x%2x%2x", MQTT_ID, mac[3], mac[4], mac[5]);
    os_sprintf(config->mqtt_prefix,"%s/%s/system", MQTT_PREFIX, config->mqtt_id);
    os_sprintf(config->mqtt_command_topic,"%s/%s/%s", MQTT_PREFIX, config->mqtt_id, "command");
    os_sprintf(config->mqtt_gpio_out_topic,"%s/%s/%s", MQTT_PREFIX, config->mqtt_id, "switch");
    config->gpio_out_status		= 0;
    config->mqtt_interval		= MQTT_REPORT_INTERVAL;
    config->mqtt_topic_mask		= 0xffff;

    config->dhcps_entries		= 0;
}

int config_load(sysconfig_p config)
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
    return 0;
}

void config_save(sysconfig_p config)
{
    uint16_t base_address = FLASH_BLOCK_NO;
    os_printf("Saving configuration\r\n");
    spi_flash_erase_sector(base_address);
    spi_flash_write(base_address * SPI_FLASH_SEC_SIZE, (uint32 *)config, sizeof(sysconfig_t));
}

void blob_save(uint8_t blob_no, uint32_t *data, uint16_t len)
{
    uint16_t base_address = FLASH_BLOCK_NO + 1 + blob_no;
    spi_flash_erase_sector(base_address);
    spi_flash_write(base_address * SPI_FLASH_SEC_SIZE, data, len);
}

void blob_load(uint8_t blob_no, uint32_t *data, uint16_t len)
{
    uint16_t base_address = FLASH_BLOCK_NO + 1 + blob_no;
    spi_flash_read(base_address * SPI_FLASH_SEC_SIZE, data, len);
}

void blob_zero(uint8_t blob_no, uint16_t len)
{
int i;
    uint8_t z[len];
    os_memset(z, 0,len);
    uint16_t base_address = FLASH_BLOCK_NO + 1 + blob_no;
    spi_flash_erase_sector(base_address);
    spi_flash_write(base_address * SPI_FLASH_SEC_SIZE, (uint32_t *)z, len);
}
