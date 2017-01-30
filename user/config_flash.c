#include "ip_addr.h"
#include "config_flash.h"

/*     From the document 99A-SDK-Espressif IOT Flash RW Operation_v0.2      *
   -------------------------------------------------------------------------
   Flash is erased sector by sector, which means it has to erase 4Kbytes one
   time at least. When you want to change some data in flash, you have to
   erase the whole sector, and then write it back with the new data.
  --------------------------------------------------------------------------*/
void config_load_default(sysconfig_p config)
{
  os_memset(config, 0, sizeof(sysconfig_t));
  os_printf("Loading Default configuration's...\r\n");
  config->magic_number    = MAGIC_NUMBER;
  config->length      = sizeof(sysconfig_t);
  os_sprintf(config->ssid, "%s", WIFI_SSID);
  os_sprintf(config->password, "%s", WIFI_PASSWORD);
  config->auto_connect    = 0;
  os_sprintf(config->ap_ssid, "%s", WIFI_AP_SSID);
  os_sprintf(config->ap_password, "%s", WIFI_AP_PASSWORD);
  config->ap_open     = 1;
  config->ap_on     = 1;
  config->locked      = 0;
  IP4_ADDR(&config->network_addr, 192, 168, 2, 1);
  config->clock_speed   = 160;
}

int config_load(int version, sysconfig_p config)
{
  if (config == NULL) return -1;
  uint16_t base_address = 0x0c + version;

  spi_flash_read(base_address * SPI_FLASH_SEC_SIZE, &config->magic_number, 4);

  if ((config->magic_number != MAGIC_NUMBER))
  {
    os_printf("\r\nNo Config Found!!! Saving Default's in Flash...\r\n");
    config_load_default(config);
    config_save(version, config);
    return -1;
  }

  os_printf("\r\nConfig found and loaded successfully...\r\n");
  spi_flash_read(base_address * SPI_FLASH_SEC_SIZE, (uint32 *) config, sizeof(sysconfig_t));
  if (config->length != sizeof(sysconfig_t))
  {
    os_printf("Length Mismatch!!! Probably old version of Config!!! Loading Defaults...\r\n");
    config_load_default(config);
    config_save(version, config);
    return -1;
  }
  return 0;
}

void config_save(int version, sysconfig_p config)
{
  uint16_t base_address = 0x0c + version;
  os_printf("Saving Configuration's...\r\n");
  spi_flash_erase_sector(base_address);
  spi_flash_write(base_address * SPI_FLASH_SEC_SIZE, (uint32 *)config, sizeof(sysconfig_t));
}

