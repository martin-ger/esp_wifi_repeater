# esp_wifi_repeater
A full functional WiFi Repeater (correctly: a WiFI NAT Router)

This is a proof of concept implementation of a WiFi NAT router on the esp8266. It can be used as range extender for an existing WiFi network. The esp acts as STA and as soft-AP and transparently forwards any IP traffic through it. As it uses NAT no routing entries are required neither on the network side nor on the connected stations. Stations are configured via DHCP by default in the 192.168.4.0/24 net and receive their DNS responder address from the existing WiFi network.

The router also allows for remote monitoring, e.g. with Wireshark.

# Building
To build this binary you need the esp-open-sdk (https://github.com/pfalcon/esp-open-sdk) and my "nat" branch of the esp-open-lwip library (https://github.com/martin-ger/esp-open-lwip/tree/nat). Drop the esp-open-lwip into the esp-open-sdk, i.e. copy it into the build tree, and build the esp-open-sdk. 

Then adjust the BUILD_AREA variable in the Makefile and build the esp_wifi_repeater firmware with "make". "make flash" flashes it onto an esp8266.

If you want to use the precompiled binaries you can flash them with "esptool.py --port /dev/ttyUSB0 write_flash 0x00000 firmware/0x00000.bin 0x10000 firmware/0x10000.bin"

# Usage
The Firmware starts with the following default configuration:
- ssid: ssid, pasword: password
- ap_ssid: MyAP, ap_password: none, ap_open: 1
- network: 192.168.4.0/24

This means it connects to the internet via AP ssid,password and offers an open AP with ap_ssid MyAP. This default can be changed in the file user_config.h. The default can be overwritten and persistenly saved to flash by using a console interface. This console is available either via the serial port at 115200 baud or via tcp port 7777 (e.g. "telnet 192.168.4.1 7777" from a connected STA). 

The console understands the following command:
- help: prints a short help message
- show [config|stats]: prints the current config or some statistics
- set ssid|pasword|ap_ssid|ap_password [value]: changes the named config parameter
- set ap_open [0|1]: selects, wheter the soft-AP uses WPA2 security (ap_open=0) or no password (ap_open=1)
- set network [ip-addr]: sets the IP address of the internal network, network is always /24, router is always x.x.x.1
- save: saves the current parameters to flash
- quit: terminates a remote session
- reset [factory]: resets the esp, optionally resets WiFi params to default values
- lock: locks the current config, changes are not allowed
- unlock [password]: unlocks the config, requires password of the network AP
- monitor [on|off] [port]: starts and stops monitor server on a given port

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
- For me some versions of the ESP-01 seem to have problems with the ESP SDK V2.0. In this case SDK V1.5 works.
