Other commands:
cat /proc/net/rtl8733bu/wlan0/trx_info_debug
iw dev phy1-sta0 link
sudo tcpdump -i wlx40a5ef2f2308



Resets rate to standard
echo 0xFFFFFFFF > /proc/net/rtl8733bu/wlan0/rate_ctl

Everything in between 0F-FF defaults to legacy 6mbit
MCS3: 
26 Mbit/s, 20 MHz, MCS 3
echo 0x0000000F > /proc/net/rtl8733bu/wlan0/rate_ctl

26 Mbit/s, 20 MHz, MCS 3
echo 0x0000004F > /proc/net/rtl8733bu/wlan0/rate_ctl

28.9 Mbit/s, 20 MHz, MCS 3, Short GI
echo 0x0000008F > /proc/net/rtl8733bu/wlan0/rate_ctl

28.9 Mbit/s, 20 MHz, MCS 3, Short GI
echo 0x000000CF > /proc/net/rtl8733bu/wlan0/rate_ctl

72.2 Mbit/s, 20 MHz, MCS 7, Short GI 
echo 0x000000FF > /proc/net/rtl8733bu/wlan0/rate_ctl



Legacy:
48 Mbit/s, 20 MHz
echo 0x000000CA > /proc/net/rtl8733bu/wlan0/rate_ctl

54 Mbit/s, 20 MHz
echo 0x000000CB > /proc/net/rtl8733bu/wlan0/rate_ctl

36 Mbit/s, 20 MHz
echo 0x000000C9 > /proc/net/rtl8733bu/wlan0/rate_ctl

MCS2
21.7 Mbit/s, 20 MHz, MCS 2, Short GI
echo 0x000000CE > /proc/net/rtl8733bu/wlan0/rate_ctl


MCS1:
14.4 Mbit/s, 20 MHz, MCS 1, Short GI
echo 0x000000CD > /proc/net/rtl8733bu/wlan0/rate_ctl

13 Mbit/s, 20 MHz, MCS 1
echo 0x0000004D > /proc/net/rtl8733bu/wlan0/rate_ctl

MCS0:
7.2 Mbit/s, 20 MHz, MCS 0, Short GI
echo 0x000000CC > /proc/net/rtl8733bu/wlan0/rate_ctl

C0-C4 seems to affect RX rates.
