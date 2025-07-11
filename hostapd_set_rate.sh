#!/bin/sh
# ---------------------------------------------------------
# set_rate_mask.sh  –  RTL8733BU rate mask *and* channel helper
#
# NEW:  ./set_rate_mask.sh <mode> [--make-permanent]
#       • <mode> is one of the rate-mask keywords OR a channel keyword
#       • If --make-permanent is supplied and <mode> is a channel,
#         the script edits /etc/hostapd.conf accordingly.
#
# Rate-mask modes (unchanged):
#   reset mcs0 mcs1 mcs2 mcs3 mcs0-3 mcs0-3-sgi mcs7
#
# Channel modes (40 MHz / 20 MHz):
#   ch36   ch36-20   ch44   ch44-20
#   ch52   ch52-20   ch100  ch100-20
#   ch149  ch149-20  ch157  ch157-20
#
# Example:
#   ./set_rate_mask.sh ch149 --make-permanent
#
# Notes:
#   • Requires hostapd running on wlan0 and accepting CSA.
#   • DFS channels (52–64, 100–144) incur 60-s CAC each time.
# ---------------------------------------------------------

MASK_FILE="/proc/net/rtl8733bu/wlan0/rate_ctl"
HOSTAPD_CLI="hostapd_cli -i wlan0"
CONFIG="/etc/hostapd.conf"

CS=5               # Channel-Switch Announcement delay (beacons)
BW40="bandwidth=40 ht vht"

# ---------- helper functions ----------
make_permanent_40 () {
    CH="$1"            # primary channel number
    CENTRE="$2"        # centre-freq index (chan ±2)
    OFFSET="$3"        # + or -
    sed -i -e "s/^channel=.*/channel=${CH}/" \
           -e "s/^ht_capab=.*/ht_capab=[HT40${OFFSET}][SHORT-GI-20][SHORT-GI-40]/" \
           -e "s/^vht_oper_centr_freq_seg0_idx=.*/vht_oper_centr_freq_seg0_idx=${CENTRE}/" \
           "$CONFIG"
    echo "hostapd.conf updated → ${CH}/HT40${OFFSET} (centre ${CENTRE})"
}

make_permanent_20 () {
    CH="$1"
    sed -i -e "s/^channel=.*/channel=${CH}/" \
           -e "s/^ht_capab=.*/ht_capab=[SHORT-GI-20]/" \
           -e "s/^vht_oper_centr_freq_seg0_idx=.*/vht_oper_centr_freq_seg0_idx=0/" \
           "$CONFIG"
    echo "hostapd.conf updated → ${CH} (20 MHz)"
}

# ----------- MAIN -----------
MODE="$1"
PERM="$2"   # may be empty or '--make-permanent'

case "$MODE" in
################################
# ---- RATE MASKS -------------
################################
    reset)             echo 0xFFFFFFFF > "$MASK_FILE" ;;
    mcs0)              echo 0x000000CC > "$MASK_FILE" ;;
    mcs1)              echo 0x000000CD > "$MASK_FILE" ;;
    mcs2)              echo 0x000000CE > "$MASK_FILE" ;;
    mcs3)              echo 0x000000CF > "$MASK_FILE" ;;
    mcs0-3)            echo 0x0000000F > "$MASK_FILE" ;;
    mcs0-3-sgi)        echo 0x0000008F > "$MASK_FILE" ;;
    mcs7)              echo 0x000000FF > "$MASK_FILE" ;;

################################
# ---- CHANNEL SWITCHES -------
################################
    ch36)
        $HOSTAPD_CLI chan_switch $CS 5180 sec_channel_offset=1 center_freq1=5190 $BW40
        [ "$PERM" = "--make-permanent" ] && make_permanent_40 36 38 "+"
        ;;
    ch36-20)
        $HOSTAPD_CLI chan_switch $CS 5180
        [ "$PERM" = "--make-permanent" ] && make_permanent_20 36
        ;;

    ch44)
        $HOSTAPD_CLI chan_switch $CS 5220 sec_channel_offset=1 center_freq1=5230 $BW40
        [ "$PERM" = "--make-permanent" ] && make_permanent_40 44 46 "+"
        ;;
    ch44-20)
        $HOSTAPD_CLI chan_switch $CS 5220
        [ "$PERM" = "--make-permanent" ] && make_permanent_20 44
        ;;

    ch52)
        echo "DFS notice: CAC ~60 s…"
        $HOSTAPD_CLI chan_switch $CS 5260 sec_channel_offset=1 center_freq1=5270 $BW40
        [ "$PERM" = "--make-permanent" ] && make_permanent_40 52 54 "+"
        ;;
    ch52-20)
        echo "DFS notice: CAC ~60 s…"
        $HOSTAPD_CLI chan_switch $CS 5260
        [ "$PERM" = "--make-permanent" ] && make_permanent_20 52
        ;;

    ch100)
        echo "DFS notice: CAC ~60 s…"
        $HOSTAPD_CLI chan_switch $CS 5500 sec_channel_offset=1 center_freq1=5510 $BW40
        [ "$PERM" = "--make-permanent" ] && make_permanent_40 100 102 "+"
        ;;
    ch100-20)
        echo "DFS notice: CAC ~60 s…"
        $HOSTAPD_CLI chan_switch $CS 5500
        [ "$PERM" = "--make-permanent" ] && make_permanent_20 100
        ;;

    ch149)
        $HOSTAPD_CLI chan_switch $CS 5745 sec_channel_offset=1 center_freq1=5755 $BW40
        [ "$PERM" = "--make-permanent" ] && make_permanent_40 149 151 "+"
        ;;
    ch149-20)
        $HOSTAPD_CLI chan_switch $CS 5745
        [ "$PERM" = "--make-permanent" ] && make_permanent_20 149
        ;;

    ch157)
        $HOSTAPD_CLI chan_switch $CS 5785 sec_channel_offset=1 center_freq1=5795 $BW40
        [ "$PERM" = "--make-permanent" ] && make_permanent_40 157 159 "+"
        ;;
    ch157-20)
        $HOSTAPD_CLI chan_switch $CS 5785
        [ "$PERM" = "--make-permanent" ] && make_permanent_20 157
        ;;

################################
# ---- HELP / FALLBACK ---------
################################
    *)
        echo "Usage: $0 <mode> [--make-permanent]"
        echo
        echo "Rate-mask modes:"
        echo "  reset mcs0 mcs1 mcs2 mcs3 mcs0-3 mcs0-3-sgi mcs7"
        echo
        echo "Channel modes (40 MHz / 20 MHz):"
        echo "  ch36   ch36-20   ch44   ch44-20"
        echo "  ch52   ch52-20   ch100  ch100-20"
        echo "  ch149  ch149-20  ch157  ch157-20"
        exit 1
        ;;
esac
