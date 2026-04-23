#ifndef __RBOOT_H__
#define __RBOOT_H__

//////////////////////////////////////////////////
// rBoot open source boot loader for ESP8266.
// Copyright 2015 Richard A Burton
// richardaburton@gmail.com
// See license.txt for license terms.
//////////////////////////////////////////////////

#ifdef __cplusplus
extern "C" {
#endif

#include <c_types.h> // uint* types

#ifdef RBOOT_INTEGRATION
#include <rboot-integration.h>
#endif

// uncomment to use only c code
// if you aren't using gcc you may need to do this
//#define BOOT_NO_ASM

// uncomment to have a checksum on the boot config
//#define BOOT_CONFIG_CHKSUM

// uncomment to enable big flash support (>1MB)
//#define BOOT_BIG_FLASH

// uncomment to enable 2 way communication between
// rBoot and the user app via the esp rtc data area
//#define BOOT_RTC_ENABLED

// uncomment to enable GPIO booting of specific rom
// (specified in rBoot config block)
// cannot be used at same time as BOOT_GPIO_SKIP_ENABLED
//#define BOOT_GPIO_ENABLED

// uncomment to enable GPIO rom skip mode, trigger
// GPIO at boot time to skip to next rom
// cannot be used at same time as BOOT_GPIO_ENABLED
//#define BOOT_GPIO_SKIP_ENABLED

// set the GPIO pin used by GPIO modes above (will default
// to 16 if not manually set), only applicable when
// BOOT_GPIO_ENABLED or BOOT_GPIO_SKIP_ENABLED is enabled
//#define BOOT_GPIO_NUM 16

// uncomment to include .irom0.text section in the checksum
// roms must be built with esptool2 using -iromchksum option
//#define BOOT_IROM_CHKSUM

// uncomment to add a boot delay, allows you time to connect
// a terminal before rBoot starts to run and output messages
// value is in microseconds
//#define BOOT_DELAY_MICROS 2000000

// define your own default custom rBoot config, used on
// first boot and in case of corruption, standard fields
// (magic, version and chksum (if applicable) are included
// for you automatically), see example at end of this file
// and customise as required
//#define BOOT_CUSTOM_DEFAULT_CONFIG

// max number of roms in the config (defaults to 4), higher
// values will use more ram at run time
//#define MAX_ROMS 4


// you should not need to modify anything below this line,
// except default_config() right at the bottom of the file


#define CHKSUM_INIT 0xef

#define SECTOR_SIZE 0x1000
#define BOOT_CONFIG_SECTOR 1

#define BOOT_CONFIG_MAGIC 0xe1
#define BOOT_CONFIG_VERSION 0x01

#define MODE_STANDARD    0x00
#define MODE_GPIO_ROM    0x01
#define MODE_TEMP_ROM    0x02
#define MODE_GPIO_ERASES_SDKCONFIG 0x04
#define MODE_GPIO_SKIP   0x08

#define RBOOT_RTC_MAGIC 0x2334ae68
#define RBOOT_RTC_READ 1
#define RBOOT_RTC_WRITE 0
#define RBOOT_RTC_ADDR 64

// defaults for unset user options
#ifndef BOOT_GPIO_NUM
#define BOOT_GPIO_NUM 16
#endif

#ifndef MAX_ROMS
#define MAX_ROMS 4
#endif

/** @brief  Structure containing rBoot configuration
 *  @note   ROM addresses must be multiples of 0x1000 (flash sector aligned).
 *          Without BOOT_BIG_FLASH only the first 8Mbit (1MB) of the chip will
 *          be memory mapped so ROM slots containing .irom0.text sections must
 *          remain below 0x100000. Slots beyond this will only be accessible via
 *          spi read calls, so use these for stored resources, not code. With
 *          BOOT_BIG_FLASH the flash will be mapped in chunks of 8MBit (1MB), so
 *          ROMs can be anywhere, but must not straddle two 8MBit (1MB) blocks.
 *  @ingroup rboot
*/
typedef struct {
	uint8 magic;           ///< Our magic, identifies rBoot configuration - should be BOOT_CONFIG_MAGIC
	uint8 version;         ///< Version of configuration structure - should be BOOT_CONFIG_VERSION
	uint8 mode;            ///< Boot loader mode (MODE_STANDARD | MODE_GPIO_ROM | MODE_GPIO_SKIP)
	uint8 current_rom;     ///< Currently selected ROM (will be used for next standard boot)
	uint8 gpio_rom;        ///< ROM to use for GPIO boot (hardware switch) with mode set to MODE_GPIO_ROM
	uint8 count;           ///< Quantity of ROMs available to boot
	uint8 unused[2];       ///< Padding (not used)
	uint32 roms[MAX_ROMS]; ///< Flash addresses of each ROM
#ifdef BOOT_CONFIG_CHKSUM
	uint8 chksum;          ///< Checksum of this configuration structure (if BOOT_CONFIG_CHKSUM defined)
#endif
} rboot_config;

#ifdef BOOT_RTC_ENABLED
/** @brief  Structure containing rBoot status/control data
 *  @note   This structure is used to, optionally, communicate between rBoot and
 *          the user app. It is stored in the ESP RTC data area.
 *  @ingroup rboot
*/
typedef struct {
	uint32 magic;           ///< Magic, identifies rBoot RTC data - should be RBOOT_RTC_MAGIC
	uint8 next_mode;        ///< The next boot mode, defaults to MODE_STANDARD - can be set to MODE_TEMP_ROM
	uint8 last_mode;        ///< The last (this) boot mode - can be MODE_STANDARD, MODE_GPIO_ROM or MODE_TEMP_ROM
	uint8 last_rom;         ///< The last (this) boot rom number
	uint8 temp_rom;         ///< The next boot rom number when next_mode set to MODE_TEMP_ROM
	uint8 chksum;           ///< Checksum of this structure this will be updated for you passed to the API
} rboot_rtc_data;
#endif

// override function to create default config, must be placed after type
// and constant defines as it uses some of them, flashsize is the used size
// (may be smaller than actual flash size if big flash mode is not enabled,
// or just plain wrong if the device has not been programmed correctly!)
#ifdef BOOT_CUSTOM_DEFAULT_CONFIG
static uint8 default_config(rboot_config *romconf, uint32 flashsize) {
	romconf->count = 2;
	romconf->roms[0] = SECTOR_SIZE * (BOOT_CONFIG_SECTOR + 1);
	romconf->roms[1] = (flashsize / 2) + (SECTOR_SIZE * (BOOT_CONFIG_SECTOR + 1));
}
#endif

#ifdef __cplusplus
}
#endif

#endif
