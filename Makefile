# Makefile for ESP8266 projects
#
# Thanks to:
# - zarya
# - Jeroen Domburg (Sprite_tm)
# - Christian Klippel (mamalala)
# - Tommie Gannert (tommie)
#
# Changelog:
# - 2014-10-06: Changed the variables to include the header file directory
# - 2014-10-06: Added global var for the Xtensa tool root
# - 2014-11-23: Updated for SDK 0.9.3
# - 2014-12-25: Replaced esptool by esptool.py

BUILD_AREA = $(CURDIR)/..

# Output directors to store intermediate compiled files
# relative to the project directory
BUILD_BASE	= build
FW_BASE		= firmware

# base directory for the compiler
XTENSA_TOOLS_ROOT ?= $(BUILD_AREA)/esp-open-sdk/xtensa-lx106-elf/bin
PATH := $(XTENSA_TOOLS_ROOT):$(PATH)

# # base directory of the ESP8266 SDK package, absolute
SDK_BASE	?= $(BUILD_AREA)/esp-open-sdk/sdk

# # esptool.py path and port
ESPTOOL		?= $(XTENSA_TOOLS_ROOT)/esptool.py
ESPPORT		?= /dev/ttyUSB0
ESPTOOLBAUD	?= 115200
ESPTOOLOPTS	= -ff 40m -fm dio -fs 32m

# name for the target project
TARGET		= app

# which modules (subdirectories) of the project to include in compiling
MODULES		= driver user mqtt easygpio
#EXTRA_INCDIR    = include $(BUILD_AREA)/esp-open-sdk/esp-open-lwip/include
EXTRA_INCDIR    = include

#LIB_MODULES	= mqtt

# libraries used in this project, mainly provided by the SDK
LIBS		= c gcc hal pp phy net80211 lwip_open_napt wpa wpa2 main

# compiler flags using during compilation of source files
CFLAGS		= -Os -g -O2 -Wpointer-arith -Wundef -Werror -Wl,-EL -fno-inline-functions -nostdlib -mlongcalls -mtext-section-literals  -D__ets__ -DICACHE_FLASH -DLWIP_OPEN_SRC -DUSE_OPTIMIZE_PRINTF

# linker flags used to generate the main object file
LDFLAGS		= -nostdlib -Wl,--no-check-sections -u call_user_start -Wl,-static -L.

# linker script used for the above linkier step
#LD_SCRIPT	= eagle.app.v6.ld
LD_SCRIPT1	= -Trom0.ld
LD_SCRIPT2	= -Trom1.ld

# various paths from the SDK used in this project
SDK_LIBDIR	= lib
SDK_LDDIR	= ld
SDK_INCDIR	= include include/json

# we create two different files for uploading into the flash
# these are the names and options to generate them
FW_FILE_1_ADDR	= 0x02000
FW_FILE_2_ADDR	= 0x82000

# select which tools to use as compiler, librarian and linker
CC		:= $(XTENSA_TOOLS_ROOT)/xtensa-lx106-elf-gcc
AR		:= $(XTENSA_TOOLS_ROOT)/xtensa-lx106-elf-ar
LD		:= $(XTENSA_TOOLS_ROOT)/xtensa-lx106-elf-gcc



####
#### no user configurable options below here
####
SRC_DIR		:= $(MODULES)
BUILD_DIR	:= $(addprefix $(BUILD_BASE)/,$(MODULES))

SDK_LIBDIR	:= $(addprefix $(SDK_BASE)/,$(SDK_LIBDIR))
SDK_INCDIR	:= $(addprefix -I$(SDK_BASE)/,$(SDK_INCDIR))

SRC		:= $(foreach sdir,$(SRC_DIR),$(wildcard $(sdir)/*.c))
OBJ		:= $(patsubst %.c,$(BUILD_BASE)/%.o,$(SRC))
LIBS		:= $(addprefix -l,$(LIBS))
APP_AR		:= $(addprefix $(BUILD_BASE)/,$(TARGET)_app.a)
TARGET_OUT	:= $(addprefix $(BUILD_BASE)/,$(TARGET).out)

#LD_SCRIPT	:= $(addprefix -T$(SDK_BASE)/$(SDK_LDDIR)/,$(LD_SCRIPT))

INCDIR	:= $(addprefix -I,$(SRC_DIR))
EXTRA_INCDIR	:= $(addprefix -I,$(EXTRA_INCDIR))
MODULE_INCDIR	:= $(addsuffix /include,$(INCDIR))

FW_FILE_1	:= $(addprefix $(FW_BASE)/,$(FW_FILE_1_ADDR).bin)
FW_FILE_2	:= $(addprefix $(FW_BASE)/,$(FW_FILE_2_ADDR).bin)
RBOOT_FILE	:= $(addprefix $(FW_BASE)/,0x00000.bin)

V ?= $(VERBOSE)
ifeq ("$(V)","1")
Q :=
vecho := @true
else
Q := @
vecho := @echo
endif

vpath %.c $(SRC_DIR)

define compile-objects
$1/%.o: %.c
	$(vecho) "CC $$<"
	$(Q) $(CC) $(INCDIR) $(MODULE_INCDIR) $(EXTRA_INCDIR) $(SDK_INCDIR) $(CFLAGS) -c $$< -o $$@
endef

.PHONY: all checkdirs clean

#all: checkdirs $(TARGET_OUT) $(FW_FILE_1) $(FW_FILE_2)
all: checkdirs $(FW_FILE_1) $(FW_FILE_2) $(RBOOT_FILE) $(FW_BASE)/sha1sums

#$(FW_BASE)/%.bin: $(TARGET_OUT) | $(FW_BASE)
#	$(vecho) "FW" $@
#	$(Q) $(ESPTOOL) elf2image --version=2 $(TARGET_OUT) -o $@

#../esp-open-lwip/liblwip_open.a:
#	cd ../esp-open-lwip ; make -f Makefile.ajk all


$(FW_FILE_1): $(APP_AR)
	$(Q) $(LD) -L$(BUILD_AREA)/esp-open-lwip -L$(SDK_LIBDIR) $(LD_SCRIPT1) $(LDFLAGS) -Wl,--start-group $(LIBS) $(APP_AR) -Wl,--end-group -o $(TARGET_OUT)
	$(ESPTOOL) elf2image --version=2 $(TARGET_OUT) -o $(FW_FILE_1)


$(FW_FILE_2): $(APP_AR)
	$(Q) $(LD) -L$(BUILD_AREA)/esp-open-lwip -L$(SDK_LIBDIR) $(LD_SCRIPT2) $(LDFLAGS) -Wl,--start-group $(LIBS) $(APP_AR) -Wl,--end-group -o $(TARGET_OUT)
	$(ESPTOOL) elf2image --version=2 $(TARGET_OUT) -o $(FW_FILE_2)

$(RBOOT_FILE): rboot.bin
	$(Q) cp rboot.bin $(RBOOT_FILE)


$(FW_BASE)/sha1sums: $(APP_AR) $(FW_FILE_1) $(FW_FILE_2) $(RBOOT_FILE)
	$(Q) sha1sum $(FW_FILE_1) $(FW_FILE_2) $(RBOOT_FILE) > $(FW_BASE)/sha1sums

$(APP_AR): $(OBJ)
	$(vecho) "AR $@"
	$(Q) $(AR) cru $@ $^

checkdirs: $(BUILD_DIR) $(FW_BASE)

$(BUILD_DIR):
	$(Q) mkdir -p $@

$(FW_BASE):
	$(Q) mkdir -p $@

flash: $(FW_BASE)/sha1sums
	$(ESPTOOL) --port $(ESPPORT) --baud $(ESPTOOLBAUD) write_flash $(ESPTOOLOPTS) 0x00000 $(RBOOT_FILE) $(FW_FILE_1_ADDR) $(FW_FILE_1)

flash1: $(FW_BASE)/sha1sums
	$(ESPTOOL) --port $(ESPPORT) --baud $(ESPTOOLBAUD) write_flash $(ESPTOOLOPTS) 0x00000 $(RBOOT_FILE) $(FW_FILE_2_ADDR) $(FW_FILE_2)

flashboth: $(FW_BASE)/sha1sums
	$(ESPTOOL) --port $(ESPPORT) --baud $(ESPTOOLBAUD) write_flash $(ESPTOOLOPTS) 0x00000 $(RBOOT_FILE) $(FW_FILE_1_ADDR) $(FW_FILE_1) $(FW_FILE_2_ADDR) $(FW_FILE_2)

clean:
	$(Q) rm -rf $(FW_BASE) $(BUILD_BASE)
	$(Q) find . -name "*~" -print0 | xargs -0 rm -rf

$(foreach bdir,$(BUILD_DIR),$(eval $(call compile-objects,$(bdir))))
