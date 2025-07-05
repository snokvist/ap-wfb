#!/bin/sh
# ------------------------------------------------------------
# linkwatch-realtek-tight4.sh
#   – Floor 4 096 k, Ceiling 20 480 k
#   – Retry >3 % → floor
#   – Otherwise based on MCS capacity (no Tx-throughput cap)
#   – SUGGEST every sample
# ------------------------------------------------------------

IFACE=wlan0

# ----- guard-rails --------------------------------------------
WARN_RSSI=-65
WARN_MCS=4
WARN_THRUPUT=5
WARN_RETRY=30
FAST_RETRY=60
FAST_TX=1

INTERVAL=0.2
BAD_TRIP=3
GOOD_TRIP=10           # faster recover

FLOOR_KBPS=4096
RECOVER_MAX_KBPS=20480

# ----- EWMA for display ---------------------------------------
s_retry=0; s_tx=0
bad_ctr=0; good_ctr=0; throttled=0
prev_tx_ok=0

mcs_phy_kbps() {
    case "$1" in
        0) echo 6500 ;; 1) echo 13000 ;; 2) echo 19500 ;; 3) echo 26000 ;;
        4) echo 39000 ;; 5) echo 52000 ;; 6) echo 58500 ;; 7) echo 65000 ;;
        *) echo 0 ;;
    esac
}

read_metrics() {
    local base="/proc/net/rtl8733bu/$IFACE"

    tx_tp_mbps=$(sed -n 's/.*\[TP\] Tx[[:space:]]*:[[:space:]]*\([0-9]\+\).*/\1/p' \
                 "$base/sta_tp_info" 2>/dev/null)
    tx_tp_mbps=${tx_tp_mbps:-0}

    if [ -r "$base/trx_info_debug" ]; then
        mcs=$(awk '/curr_tx_rate/ {
                     for(i=1;i<=NF;i++)
                       if($i ~ /^MCS[0-9]+/){ sub(/^MCS/, "", $i); print $i; exit }
                  }' "$base/trx_info_debug")
        retry_pct=$(awk '/curr_retry_ratio/ {print $(NF)}' "$base/trx_info_debug")
        mcs=${mcs:--1}; retry_pct=${retry_pct:-0}
    else
        mcs=-1; retry_pct=0
    fi
}

while :; do
    rssi=$(awk -v ifname="$IFACE" '$1==ifname":"{print -$4}' /proc/net/wireless)
    [ -z "$rssi" ] && rssi=-100

    tx_ok=$(cat /sys/class/net/$IFACE/statistics/tx_packets 2>/dev/null)
    tx_ok=${tx_ok:-0}
    delta_ok=$((tx_ok - prev_tx_ok))
    prev_tx_ok=$tx_ok

    read_metrics   # → mcs retry_pct tx_tp_mbps

    # EWMA display
    s_retry=$(( (s_retry*3 + retry_pct*100) / 4 ))
    s_tx=$(( (s_tx*3 + tx_tp_mbps*100) / 4 ))
    disp_retry=$(( s_retry / 100 ))
    disp_tx=$(( s_tx / 100 ))

    # ------- SUGGEST -------------------------------------------
    if [ "$retry_pct" -gt 3 ]; then
        safe_kbps=$FLOOR_KBPS
    else
        phy=$(mcs_phy_kbps "$mcs")
        safe_kbps=$(( phy * 65 * (100 - retry_pct) / 10000 ))
        [ "$safe_kbps" -lt "$FLOOR_KBPS" ] && safe_kbps=$FLOOR_KBPS
        [ "$safe_kbps" -gt "$RECOVER_MAX_KBPS" ] && safe_kbps=$RECOVER_MAX_KBPS
    fi

    # ------- print sample --------------------------------------
    printf '%s  RSSI=%4s +pkt=%5s MCS=%2s Retry=%3s%% Tx=%3sMb Suggest=%5sk' \
           "$(date +%s)" "$rssi" "$delta_ok" "$mcs" "$disp_retry" "$disp_tx" "$safe_kbps"

    # ------- decision logic ------------------------------------
    reasons=""
    if [ "$retry_pct" -ge "$FAST_RETRY" ]; then
        reasons="RetryFast"
    elif [ "$tx_tp_mbps" -le "$FAST_TX" ] && [ "$tx_tp_mbps" -gt 0 ]; then
        reasons="TxFast"
    else
        [ "$rssi" -lt "$WARN_RSSI" ] && reasons="$reasons RSSI"
        ( [ "$mcs" -ge 0 ] && [ "$mcs" -le "$WARN_MCS" ] ) && reasons="$reasons MCS"
        [ "$tx_tp_mbps" -le "$WARN_THRUPUT" ] && reasons="$reasons Tx"
        [ "$retry_pct"  -ge "$WARN_RETRY"   ] && reasons="$reasons Retry"
        reasons=$(echo "$reasons" | sed 's/^ //')
    fi

    [ -n "$reasons" ] && { bad_ctr=$((bad_ctr+1)); good_ctr=0; } \
                       || { good_ctr=$((good_ctr+1)); bad_ctr=0; }

    fire=0
    [ "$throttled" -eq 0 ] && [ "$bad_ctr" -ge "$BAD_TRIP" ] && fire=1
    [ "$reasons" = "RetryFast" -o "$reasons" = "TxFast" ] && fire=1

    recover=0
    [ "$throttled" -eq 1 ] && [ "$good_ctr" -ge "$GOOD_TRIP" ] && recover=1

    if [ "$fire" -eq 1 ]; then
        throttled=1; bad_ctr=0
        echo "  THROTTLE:$reasons"
    elif [ "$recover" -eq 1 ]; then
        throttled=0; good_ctr=0
        echo "  RECOVER"
    else
        echo
    fi

    sleep "$INTERVAL"
done
