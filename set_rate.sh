#!/bin/sh
# ---------------------------------------------------------
# set_rate_mask.sh - set specific rate mask on rtl8733bu
# Only uses masks confirmed by your tests.
#
# Usage:
#   ./set_rate_mask.sh <mode>
#
# Modes:
#   reset        -> restore all rates (0xFFFFFFFF)
#   mcs0         -> lock to HT MCS0 (7.2 Mb/s SGI)
#   mcs1         -> lock to HT MCS1 (13-14.4 Mb/s)
#   mcs2         -> lock to HT MCS2 (19-21 Mb/s)
#   mcs3         -> lock to HT MCS3 (26-28.9 Mb/s)
#   mcs0-3       -> autoselect up to MCS3 long GI
#   mcs0-3-sgi   -> autoselect up to MCS3 short GI
#   mcs7         -> full HT up to MCS7 (72 Mb/s)
#   leg36        -> lock to legacy 36 Mb/s
#   leg48        -> lock to legacy 48 Mb/s
#   leg54        -> lock to legacy 54 Mb/s
#
# ---------------------------------------------------------

MASK_FILE="/proc/net/rtl8733bu/wlan0/rate_ctl"

case "$1" in
    reset)
        echo "Restoring all rates..."
        echo 0xFFFFFFFF > $MASK_FILE
        ;;
    mcs0)
        echo "Locking to MCS0 (7.2 Mb/s SGI)..."
        echo 0x000000CC > $MASK_FILE
        ;;
    mcs1)
        echo "Locking to MCS1 (13-14.4 Mb/s)..."
        echo 0x000000CD > $MASK_FILE
        ;;
    mcs2)
        echo "Locking to MCS2 (19-21 Mb/s)..."
        echo 0x000000CE > $MASK_FILE
        ;;
    mcs3)
        echo "Locking to MCS3 (26-28.9 Mb/s)..."
        echo 0x000000CF > $MASK_FILE
        ;;
    mcs0-3)
        echo "Allowing MCS0-3 (long GI, ~26 Mb/s)..."
        echo 0x0000000F > $MASK_FILE
        ;;
    mcs0-3-sgi)
        echo "Allowing MCS0-3 short GI (~28.9 Mb/s)..."
        echo 0x0000008F > $MASK_FILE
        ;;
    mcs7)
        echo "Allowing up to MCS7 (72 Mb/s)..."
        echo 0x000000FF > $MASK_FILE
        ;;
    *)
        echo "Usage: $0 <mode>"
        echo "Modes: reset mcs0 mcs1 mcs2 mcs3 mcs0-3 mcs0-3-sgi mcs7"
        exit 1
        ;;
esac
