#!/bin/bash

# Configure and optimize NetworkManager connection
nmcli connection modify Drone connection.autoconnect yes
nmcli connection modify Drone connection.autoconnect-retries -1
nmcli connection modify Drone ipv4.method manual ipv4.addresses 192.168.0.10/24 ipv4.gateway 192.168.0.1
nmcli connection modify Drone ipv4.dns 192.168.0.1
nmcli connection modify Drone ipv6.method ignore

# Bounce connection to apply immediately
nmcli connection down Drone
nmcli connection up Drone

# Watchdog loop for <0.5s reconnects
while true; do
    state=$(nmcli -t -f GENERAL.STATE device show wlx40a5ef2f229b | cut -d: -f2)
    if [[ "$state" != "100 (connected)" && "$state" != "100" ]]; then
        echo "$(date) Not connected, forcing reconnect"
        nmcli connection up Drone
    fi
    sleep 0.5
done
