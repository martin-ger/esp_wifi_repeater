# esp_wifi_repeater
A full functional WiFi Repeater (correctly: a WiFi NAT Router)
                                         CODED by martin-ger & further Moded and Optimized by yoAeroA00

This is a proof of concept implementation of a WiFi NAT router on the esp8266. It can be used as range extender for an existing WiFi network. The esp acts as STA and as soft-AP and transparently forwards any IP traffic through it. As it uses NAT no routing entries are required neither on the network side nor on the connected stations. Stations are configured via DHCP by default in the 192.168.2.0/24 net and receive their DNS responder address from the existing WiFi network.

The router also allows for remote monitoring (or packet sniffing), e.g. with Wireshark. 

Some details are explained in this video: https://www.youtube.com/watch?v=OM2FqnMFCLw

# Building and Flashing
To build this binary you download and install the esp-open-sdk (https://github.com/pfalcon/esp-open-sdk) and my version of the esp-open-lwip library (https://github.com/yoAeroA00/esp-open-lwip). Replace that in the esp-open-sdk tree. "make clean" in the esp_open_lwip dir and once again a "make" in the upper esp_open_sdk will do the job. This installs a new version of the liblwip_open.a that contains the NAT-features.

Then adjust the BUILD_AREA variable in the Makefile and any desired options in user/user_config.h.

Build the esp_wifi_repeater firmware with "make". "make flash" flashes it onto an esp8266.

If you want to use the precompiled binaries you can flash them with "esptool.py --port /dev/ttyUSB0 write_flash -fs 32m 0x00000 firmware/0x00000.bin 0x10000 firmware/0x10000.bin" (use -fs 8m for an ESP-01)

On Windows you can flash it using the "ESP8266 Download Tool" available at https://espressif.com/en/support/download/other-tools. Download the two files 0x00000.bin and 0x10000.bin from the firmware directory. For a generic ESP12, a NodeMCU or a Wemos D1 use the following settings (for an ESP-01 change FLASH SIZE to "8Mbit"):

<img src="https://raw.githubusercontent.com/martin-ger/esp_wifi_repeater/master/FlashRepeaterWindows.jpg">

For some reasons that I still do not understand, the firmware compiled with the V2.0.0 SDK fails to start on some ESP-01 modules. If you experience these problem, use the files from the directory firmware_sdk_1.5.4 instead (addresses 0x00000 and 0x40000).

# Usage
The Firmware starts with the following default configuration:
- ssid: ssid, pasword: password
- ap_ssid: ESP, ap_password: repeator, ap_on: 1, ap_open: 1
- network: 192.168.2.0/24

This means it connects to the internet via AP ssid,password and offers an open AP with ap_ssid MyAP. This default can be changed in the file user_config.h. The default can be overwritten and persistenly saved to flash by using a console interface. This console is available either via the serial port at 115200 baud or via tcp port 7777 (e.g. "telnet 192.168.2.1 7777" from a connected STA). 

The console understands the following command:
- help: prints a short help message
- show [config|stats]: Prints the current Config or some Statistics.
- set ssid|pasword|ap_ssid|ap_password [value]: Changes the named Config Parameter.
- set ap_open [0|1]: selects, wheter the soft-AP uses WPA2 security (ap_open=0) or is set to OPEN with no password (ap_open=1).
- set ap_on [0|1]: selects, wheter the soft-AP is Disabled (ap_on=0) or Enabled (ap_on=1).
- set network [ip-addr]: sets the IP address of the Internal Network, Network is always /24, Router is Always x.x.x.1 .
- set speed [80|160]: sets the CPU clock Frequency.
- save [auto_connect]: Saves the Current Parameters to Flash and Restarts the System to apply the Settings, optionally set auto_connect to 1 .
- quit: Terminates a Remote Session.
- reset [factory]: Resets the ESP, optionally resets WiFi params to Default Values.
- lock: Locks the current Config, changes are not Allowed.
- unlock [password]: Unlocks the Config, Requires Password of the STA.
- scan: Does a Scan for APs.
- monitor [on|off] [port]: Starts and Stops Monitor Server on a given Port and run "netcat [ip-addr] [portno] | sudo wireshark -k -S -i -" on a remote computer to observe the traffic in RealTime.

# Status LED
In default config GPIO2 is configured to drive a status LED (connected to GND) with the following indications:
- permanently on: started, but not successfully connected to the AP (no valid external IP)
- flashing (1 per second): working, connected to the AP
- unperiodically flashing: working, traffic in the internal network

In user_config.h an alternative GPIO port can be configured. When configured to GPIO1 it works with the buildin blue LED on the ESP-01 and ESP-12 boards. However, as GPIO1 ist also the UART-TX-pin this means, that the serial console is not working. Configuration is then limited to network access.

# Monitoring
From the console a monitor service can be started ("monitor on [portno]"). This service mirrors the traffic of the internal network in pcap format to a TCP stream. E.g. with a "netcat [external_ip_of_the_repeater] [portno] | sudo wireshark -k -S -i -" from an computer in the external network you can now observe the traffic in the internal network in real time. Use this e.g. to observe with which internet sites your internals clients are communicating. Be aware that this at least doubles the load on the esp and the WiFi network. Under heavy load this might result in some packets beeing cut short or even dropped in the monitor session. CAUTION: leaving this port open is a potential security issue. Anybody from the local networks can connect and observe your traffic.

# Known Issues
- Configuration via TCP (write_flash) requires a good power supply. A large capacitor between Vcc and Gnd can help if you experience problems here.
