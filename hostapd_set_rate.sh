#!/bin/sh
# ---------------------------------------------------------
# set_rates.sh – BusyBox-compatible rate masks + channel helper
#
# Usage:
#   set_rates.sh <mode> [--make-permanent]
#
# Rate-mask modes  (supported: RTL8733BU • RTL8812EU)
#   reset mcs0 mcs1 mcs2 mcs3 mcs0-3 mcs0-3-sgi mcs7
#
# Channel modes (work on any driver)
#   ch36  ch36-20  ch44  ch44-20  ch52  ch52-20
#   ch100 ch100-20 ch149 ch149-20 ch157 ch157-20
#
#   Add --make-permanent after a channel mode to rewrite /etc/hostapd.conf
# ---------------------------------------------------------

### USER CONSTANTS #######################################################
IFACE=wlan0
HOSTAPD_CLI="hostapd_cli -i $IFACE"
CONFIG=/etc/hostapd.conf
CS=5                                   # beacon delay for CSA
BW40="bandwidth=40 ht vht"             # CSA flags for HT40
##########################################################################

############################################################################
# 1. Detect USB Wi-Fi adapter → $driver
############################################################################
driver=unknown
for id in $(lsusb | awk '{print $6}' | uniq); do
    case "$id" in
        0bda:8812|0bda:881a|0b05:17d2|2357:0101|2604:0012) driver=88XXau  ;;
        0bda:a81a)                                          driver=8812eu ;;
        0bda:f72b|0bda:b733)                                driver=8733bu ;;
    esac
done
[ "$driver" = unknown ] && { echo "Wireless adapter not detected."; exit 1; }

############################################################################
# 2. Set RATE_FILE path for this driver
############################################################################
case "$driver" in
    8733bu)  RATE_FILE="/proc/net/rtl8733bu/${IFACE}/rate_ctl" ;;
    8812eu)  RATE_FILE="/proc/net/rtl88x2eu/${IFACE}/rate_ctl" ;;
    *)       RATE_FILE="" ;;
esac

############################################################################
# 3. Helper: return hex mask for <mode> on current $driver
############################################################################
mask_lookup() {
    mode="$1"
    case "$driver" in
        8733bu)
            case "$mode" in
                reset)        echo 0xFFFFFFFF ;;
                mcs0)         echo 0x000000CC ;;
                mcs1)         echo 0x000000CD ;;
                mcs2)         echo 0x000000CE ;;
                mcs3)         echo 0x000000CF ;;
                mcs0-3)       echo 0x0000000F ;;
                mcs0-3-sgi)   echo 0x0000008F ;;
                mcs7)         echo 0x000000FF ;;
            esac
            ;;
        8812eu)
            case "$mode" in
                reset)        echo 0xFFFFFFFF ;;
                mcs0)         echo 0x68C ;;
                mcs1)         echo 0x68D ;;
                mcs2)         echo 0x68E ;;
                mcs3)         echo 0x68F ;;
                mcs0-3)       echo 0x0000000F ;;
                mcs0-3-sgi)   echo 0x0000008F ;;
                mcs7)         echo 0x693 ;;
            esac
            ;;
    esac
}

############################################################################
# 4. Helpers to update /etc/hostapd.conf
############################################################################
permanent_40() {  # $1=primary  $2=centreidx  $3=+|-
    sed -i \
        -e "s/^channel=.*/channel=$1/" \
        -e "s/^ht_capab=.*/ht_capab=[HT40$3][SHORT-GI-20][SHORT-GI-40]/" \
        -e "s/^vht_oper_centr_freq_seg0_idx=.*/vht_oper_centr_freq_seg0_idx=$2/" \
        "$CONFIG"
    echo "(hostapd.conf → $1/HT40$3)"
}
permanent_20() {  # $1=primary
    sed -i \
        -e "s/^channel=.*/channel=$1/" \
        -e "s/^ht_capab=.*/ht_capab=[SHORT-GI-20]/" \
        -e "s/^vht_oper_centr_freq_seg0_idx=.*/vht_oper_centr_freq_seg0_idx=0/" \
        "$CONFIG"
    echo "(hostapd.conf → $1/20 MHz)"
}

############################################################################
# 5. Main dispatch
############################################################################
MODE="$1"
PERM="$2"   # optional --make-permanent

case "$MODE" in
#################### RATE-MASK MODES ######################################
    reset|mcs0|mcs1|mcs2|mcs3|mcs0-3|mcs0-3-sgi|mcs7)
        [ -z "$RATE_FILE" ] && { echo "Rate masks not supported on driver '$driver'."; exit 1; }
        HEX=$(mask_lookup "$MODE")
        [ -z "$HEX" ] && { echo "Mode '$MODE' unknown for driver '$driver'."; exit 1; }
        printf "%s\n" "$HEX" > "$RATE_FILE"
        ;;

#################### CHANNEL MODES ########################################
    ch36)
        $HOSTAPD_CLI chan_switch $CS 5180 sec_channel_offset=1 center_freq1=5190 $BW40
        [ "$PERM" = "--make-permanent" ] && permanent_40 36 38 "+"
        ;;
    ch36-20)
        $HOSTAPD_CLI chan_switch $CS 5180
        [ "$PERM" = "--make-permanent" ] && permanent_20 36
        ;;
    ch44)
        $HOSTAPD_CLI chan_switch $CS 5220 sec_channel_offset=1 center_freq1=5230 $BW40
        [ "$PERM" = "--make-permanent" ] && permanent_40 44 46 "+"
        ;;
    ch44-20)
        $HOSTAPD_CLI chan_switch $CS 5220
        [ "$PERM" = "--make-permanent" ] && permanent_20 44
        ;;
    ch52)
        echo "DFS: CAC ~60 s…"
        $HOSTAPD_CLI chan_switch $CS 5260 sec_channel_offset=1 center_freq1=5270 $BW40
        [ "$PERM" = "--make-permanent" ] && permanent_40 52 54 "+"
        ;;
    ch52-20)
        echo "DFS: CAC ~60 s…"
        $HOSTAPD_CLI chan_switch $CS 5260
        [ "$PERM" = "--make-permanent" ] && permanent_20 52
        ;;
    ch100)
        echo "DFS: CAC ~60 s…"
        $HOSTAPD_CLI chan_switch $CS 5500 sec_channel_offset=1 center_freq1=5510 $BW40
        [ "$PERM" = "--make-permanent" ] && permanent_40 100 102 "+"
        ;;
    ch100-20)
        echo "DFS: CAC ~60 s…"
        $HOSTAPD_CLI chan_switch $CS 5500
        [ "$PERM" = "--make-permanent" ] && permanent_20 100
        ;;
    ch149)
        $HOSTAPD_CLI chan_switch $CS 5745 sec_channel_offset=1 center_freq1=5755 $BW40
        [ "$PERM" = "--make-permanent" ] && permanent_40 149 151 "+"
        ;;
    ch149-20)
        $HOSTAPD_CLI chan_switch $CS 5745
        [ "$PERM" = "--make-permanent" ] && permanent_20 149
        ;;
    ch157)
        $HOSTAPD_CLI chan_switch $CS 5785 sec_channel_offset=1 center_freq1=5795 $BW40
        [ "$PERM" = "--make-permanent" ] && permanent_40 157 159 "+"
        ;;
    ch157-20)
        $HOSTAPD_CLI chan_switch $CS 5785
        [ "$PERM" = "--make-permanent" ] && permanent_20 157
        ;;

#################### HELP #################################################
    *)
        cat <<EOF
Usage: $0 <mode> [--make-permanent]

Rate-mask modes (8733BU & 8812EU):
  reset mcs0 mcs1 mcs2 mcs3 mcs0-3 mcs0-3-sgi mcs7

Channel modes:
  ch36  ch36-20  ch44  ch44-20  ch52  ch52-20
  ch100 ch100-20 ch149 ch149-20 ch157 ch157-20

Add --make-permanent after a channel mode to rewrite /etc/hostapd.conf.

Detected driver: $driver
EOF
        exit 1
        ;;
esac
