#!/bin/sh
# ---------------------------------------------------------
# set_channel.sh – BusyBox-compatible channel helper
#
# Usage:
#   set_channel.sh <channel-mode> [--make-permanent]
#
# Channel modes:
#   ch36  ch36-20  ch44  ch44-20  ch52  ch52-20
#   ch100 ch100-20 ch149 ch149-20 ch157 ch157-20
#
# Add --make-permanent after a channel mode to rewrite /etc/hostapd.conf
# ---------------------------------------------------------

### USER CONSTANTS #######################################################
IFACE=wlan0
HOSTAPD_CLI="hostapd_cli -i $IFACE"
CONFIG=/etc/hostapd.conf
CS=5                                   # beacon delay for CSA
BW40="bandwidth=40 ht vht"             # CSA flags for HT40
##########################################################################

############################################################################
# Helpers to update /etc/hostapd.conf
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
# Main dispatch
############################################################################
MODE="$1"
PERM="$2"   # optional --make-permanent

case "$MODE" in
################################ CHANNEL MODES ################################
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

########################## HELP ###########################################
    *)
        cat <<EOF
Usage: $0 <channel-mode> [--make-permanent]

Channel modes:
  ch36  ch36-20  ch44  ch44-20  ch52  ch52-20
  ch100 ch100-20 ch149 ch149-20 ch157 ch157-20

Add --make-permanent after a channel mode to rewrite /etc/hostapd.conf.
EOF
        exit 1
        ;;
esac
