# esp_wifi_repeater
A full functional WiFi Repeater (correctly: a WiFI NAT Router)

This is a proof of concept implementation of a WiFi NAT router on the esp8266. It can be used as range extender for an existing WiFi network. The esp acts as STA and as soft-AP and transparently forwards any IP traffic through it. As it uses NAT no routing entries are required neither on the network side nor on the connected stations. Stations are configured via DHCP in the 192.168.4.0/24 net and receive their DNS responder address from the existing WiFi network.

# Building
To build this binary you need the esp-open-sdk (https://github.com/pfalcon/esp-open-sdk) and my "nat" branch of the esp-open-lwip library (https://github.com/martin-ger/esp-open-lwip/tree/nat). Drop the esp-open-lwip into the esp-open-sdk, i.e. copy it into the build tree, and build the esp-open-sdk. 

Then adjust the BUILD_AREA variable in the Makefile and build the esp_wifi_repeater firmware with "make". "make flash" flashes it onto an esp8266.

If you want to use the precompiled binaries you can flash them with "esptool.py --port /dev/ttyUSB1 write_flash 0x00000 firmware/0x00000.bin 0x10000 firmware/0x10000.bin"

# Usage
The Firmware starts with the following default configuration:
ssid: ssid
pasword: password
ap_ssid: MyAP
ap_password: none
ap_open: 1

This means it connects to the internet via AP ssid,password and offers an open AP with ap_ssid MyAP. This default can be changed in the file user_config.h. The default can be overwritten and persistenly saved to flash by using a console interface. This console is available either via the serial port at 115200 baud or via tcp port 7777 (e.g. "telnet 192.168.4.1 7777" from a connected STA). 
For me TCP configuration (write_flash) currently fails on an ESP-01 modul, but works fine on any ESP-12.

The console understands the following command:
- help: prints a short help message
- show: prints the current config
- set ssid|pasword|ap_ssid|ap_password [value]: changes the named config parameter
- set ap_open [0|1]: selects, wheter the soft-AP uses WPA2 security (ap_open=0) or no password (ap_open=1)
- save: saves the current parameters to flash
- reset: resets the esp
- lock: locks the current config, changes are not allowed
- unlock [password]: unlocks the config, requires password of the network AP
