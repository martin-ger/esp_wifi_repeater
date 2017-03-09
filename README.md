# esp_wifi_repeater
A full functional WiFi Repeater (correctly: a WiFi NAT Router)

This is a proof of concept implementation of a WiFi NAT router on the esp8266. It can be used as range extender for an existing WiFi network. The esp acts as STA and as soft-AP and transparently forwards any IP traffic through it. As it uses NAT no routing entries are required neither on the network side nor on the connected stations. Stations are configured via DHCP by default in the 192.168.4.0/24 net and receive their DNS responder address from the existing WiFi network.

Measurements show, that it can achieve about 5 Mbps in both directions, so even streaming is possible.

The router also allows for remote monitoring (or packet sniffing), e.g. with Wireshark. 

Some details are explained in this video: https://www.youtube.com/watch?v=OM2FqnMFCLw

# Building and Flashing
To build this binary you download and install the esp-open-sdk (https://github.com/pfalcon/esp-open-sdk). Make sure, you can compile and download the included "blinky" example. Then download or clone my version of the esp-open-lwip library and its source tree from (https://github.com/martin-ger/esp-open-lwip). Use it to replace the directory "esp-open-lwip" in the esp-open-sdk tree. "make clean" in the esp_open_lwip dir and once again a "make" in the upper esp_open_sdk directory will do the job. This installs a new version of the liblwip_open.a that contains the NAT-features.

Then download this source tree in a separate directory and adjust the BUILD_AREA variable in the Makefile and any desired options in user/user_config.h.

Build the esp_wifi_repeater firmware with "make". "make flash" flashes it onto an esp8266.

If you want to use the precompiled binaries you can flash them with "esptool.py --port /dev/ttyUSB0 write_flash -fs 32m 0x00000 firmware/0x00000.bin 0x10000 firmware/0x10000.bin" (use -fs 8m for an ESP-01)

On Windows you can flash it using the "ESP8266 Download Tool" available at https://espressif.com/en/support/download/other-tools. Download the two files 0x00000.bin and 0x10000.bin from the firmware directory. For a generic ESP12, a NodeMCU or a Wemos D1 use the following settings (for an ESP-01 change FLASH SIZE to "8Mbit"):

<img src="https://raw.githubusercontent.com/martin-ger/esp_wifi_repeater/master/FlashRepeaterWindows.jpg">

For some reasons that I still do not understand, the firmware compiled with the V2.0.0 SDK fails to start on some ESP-01 modules. If you experience these problem, use the files from the directory firmware_sdk_1.5.4 instead (addresses 0x00000 and 0x40000).

# Usage
The Firmware starts with the following default configuration:
- ssid: ssid, pasword: password
- ap_ssid: MyAP, ap_password: none, ap_on: 1, ap_open: 1
- network: 192.168.4.0/24

This means it connects to the internet via AP ssid,password and offers an open AP with ap_ssid MyAP. This default can be changed in the file user_config.h. The default can be overwritten and persistenly saved to flash by using a console interface. This console is available either via the serial port at 115200 baud or via tcp port 7777 (e.g. "telnet 192.168.4.1 7777" from a connected STA). 

The console understands the following command:
- help: prints a short help message
- show [config|stats]: prints the current config or some statistics
- set [ssid|password|ap_ssid|ap_password] _value_: changes the named config parameter
- set ap_open [0|1]: selects, wheter the soft-AP uses WPA2 security (ap_open=0) or no password (ap_open=1)
- set ap_on [0|1]: selects, wheter the soft-AP is disabled (ap_on=0) or enabled (ap_on=1)
- set network _ip-addr_: sets the IP address of the internal network, network is always /24, router is always x.x.x.1
- set speed [80|160]: sets the CPU clock frequency
- set vmin _voltage_: sets the minimum battery voltage in mV. If Vdd drops below, the ESP goes into deep sleep. If 0, nothing happens
- set vmin_sleep _time_: sets the time interval in seconds the ESP sleeps on low voltage
- set config_port _portno_: sets the port number of the console login (default is 7777, 0 disables remote console config)
- portmap add [TCP|UDP] _external_port_ _internal_ip_ _internal_port_: adds a port forwarding
- portmap remove [TCP|UDP] _external_port_: deletes a port forwarding
- save [dhcp]: saves the current config parameters [+ the current DHCP leases] to flash
- quit: terminates a remote session
- reset [factory]: resets the esp, optionally resets WiFi params to default values
- lock: locks the current config, changes are not allowed
- unlock _password_: unlocks the config, requires password of the network AP
- scan: does a scan for APs
- monitor [on|off] _port_: starts and stops monitor server on a given port

# Status LED
In default config GPIO2 is configured to drive a status LED (connected to GND) with the following indications:
- permanently on: started, but not successfully connected to the AP (no valid external IP)
- flashing (1 per second): working, connected to the AP
- unperiodically flashing: working, traffic in the internal network

In user_config.h an alternative GPIO port can be configured. When configured to GPIO1, it works with the buildin blue LED on the ESP-01 boards. However, as GPIO1 ist also the UART-TX-pin this means, that the serial console is not working. Configuration is then limited to network access.

# Monitoring
From the console a monitor service can be started ("monitor on [portno]"). This service mirrors the traffic of the internal network in pcap format to a TCP stream. E.g. with a "netcat [external_ip_of_the_repeater] [portno] | sudo wireshark -k -S -i -" from an computer in the external network you can now observe the traffic in the internal network in real time. Use this e.g. to observe with which internet sites your internals clients are communicating. Be aware that this at least doubles the load on the esp and the WiFi network. Under heavy load this might result in some packets beeing cut short or even dropped in the monitor session. CAUTION: leaving this port open is a potential security issue. Anybody from the local networks can connect and observe your traffic.

# Port Mapping
In order to allow clients from the external network to connect to server port on the internal network, ports have to be mapped. An external port is mapped to an internal port of a specific internal IP address. Use the "portmap add" command for that. Port mappings can be listed with the "show" command and are saved with the current config. 

However, to make sure that the expected device is listening at a certain IP address, it has to be ensured the this devices has the same IP address once it or the ESP is rebooted. To achive this, either fixed IP adresses can be configured in the devices or the ESP has to remember its DHCP leases. This can be achived with the "save dhcp" command. It saves the current state and all DHCP leases, so that they will be restored after reboot. DHCP leases can be listed with the "show stats" command.

# MQTT-Anbindung
Since version 1.3 the router has a build-in MQTT client (thanks to Tuan PM for his library https://github.com/tuanpmt/esp_mqtt). This can help to integrate the router/repeater into the IoT. A home automation system can e.g. make decisions based on infos about the currently associated stations, it can switch on and of the repeaters (e.g. based on a time schedule), or it can simply be used to monitor the load. The router can be connected either to a local MQTT broker or to a publically available broker in the cloud. However, currently it does not support TLS encryption.

By default the MQTT client is disabled. It can be enabled by setting the config parameter "mqtt_host" to a hostname different from "none". To configure MQTT you can set the following parameters:
- set mqtt_host _IP_or_hostname_: IP or hostname of the MQTT broker ("none" disables the MQTT client)
- set mqtt_user _username_: Username for authentication ("none" if no authentication is required at the broker)
- set mqtt_user _password_: Password for authentication
- set mqtt_id _clientId_: Id of the client at the broker (default: "ESPRouter_xxxxxx" derived from the MAC address)
- set mqtt_prefix _prefix_path_: Prefix for all published topics (default: "/WiFi/ESPRouter_xxxxxx/system", again derived from the MAC address)
- set mqtt_command_topic _command_topic_: Topic subscribed to receive commands, same as from the console. (default: "/WiFi/ESPRouter_xxxxxx/system/command", "none" disables commands via MQTT)
- set mqtt_interval _secs_: Set the interval in which the router publishs status topics (default: 15s, 0 disables status publication)

The router currently publishs the following status topics periodically:
- _prefix_path_/Vdd: Voltage of the power supply in mV
- _prefix_path_/Bin: Bytes/s from stations into the AP
- _prefix_path_/Bout: Bytes/s from the AP to stations
- _prefix_path_/Pin: Packets/s from stations into the AP
- _prefix_path_/Pout: Packets/s from the AP to stations
- _prefix_path_/NoStations: Stations currently connected to the AP

In addition it publishs on an event basis:
- _prefix_path_/join: MAC address of a station joining the AP
- _prefix_path_/leave: MAC address of a station leaving the AP
- _prefix_path_/IP: IP address of the router when received via DHCP

The router can be configured using the following topics:
- _command_topic_: The router subscribes on this topic and interprets all messages as command lines
- _prefix_path_/response: The router publishes on this topic the command line output

# Power Management
The repeater monitors its current supply voltage (shown in the "show stats" command). If _vmin_ (in mV, default 0) is set to a value > 0 and the supply voltage drops below this value, it will go into deep sleep mode for _vmin_sleep_ seconds. If you have connected GPIO16 to RST (which is hard to solder on an ESP-01) it will reboot after this interval, try to reconnect, and will continue its measurements. If _vmin_ is saved with the config, it will sleep over and over again, until the supply voltage raises above the threshold. These settings are especially (only?) useful if you have powered the ESP with a (lithium) battery whithout undercharge protection. Then a value of 2900mV-3000mV is probably helpful, as it reduces power consumption of the ESP to a minimum and you have much more time to recharge or replace the battery before damage. This only makes sense, if you have the ESP connected directly to the battery. If you have additional logic, this will still drain the battery.

You can send the ESP to sleep manually once by using the "sleep" command.

Caution: If you save a _vmin_ value higher than the max supply voltage to flash, the repeater will immediatly shutdown every time after reboot. Then you have to wipe out the whole config by flashing blank.bin (or any other file) to 0x0c000. 

# Known Issues
- Configuration via TCP (write_flash) requires a good power supply. A large capacitor between Vdd and Gnd can help if you experience problems here.
