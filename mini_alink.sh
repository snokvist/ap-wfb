#!/bin/sh
# linkwatch-ultra-param.sh
#  • 4 096 / 10 240 kbit/s two-rate controller
#  • RSSI_PANIC  – drop to MIN when signal ≤ value
#  • RETRY_CLAMP – drop to MIN when avg-retry ≥ value
# ------------------------------------------------------------

IFACE=wlan0

MIN=4096            # kbit/s – encoder floor
MAX=10240           # kbit/s – encoder ceiling
RSSI_PANIC=-85      # dBm     – bad signal threshold
RETRY_CLAMP=10       #  %      – avg retry clamp
UP_COOL=5           # s       – min gap between UP shifts
AVG_WIN=5           # samples – moving-average window
INTERVAL=0.2        # s       – polling period

curr=$MIN
last_up=0
retry_hist="0 0 0 0 0"

# --- helpers -------------------------------------------------
diff10() {                                # $1=new $2=old
    [ "$2" -eq 0 ] && return 0
    d=$(( $1>$2 ? $1-$2 : $2-$1 ))
    [ $(( d*10 )) -ge "$2" ]              # true if |Δ| ≥10 %
}

# --- main loop ----------------------------------------------
while :; do
    now=$(date +%s)

    # 1. RSSI from `iw` (display only)
    rssi=$(iw dev "$IFACE" station dump 2>/dev/null |
           awk '/signal:/ {print $2; exit}')
    [ -z "$rssi" ] && rssi=-100

    # 2. MCS + instantaneous retry% from Realtek
    base="/proc/net/rtl8733bu/$IFACE"
    if [ -r "$base/trx_info_debug" ]; then
        mcs=$(awk '/curr_tx_rate/{for(i=1;i<=NF;i++)if($i~/^MCS/){sub(/^MCS/,"",$i);print $i;exit}}' \
               "$base/trx_info_debug")
        raw_retry=$(awk '/curr_retry_ratio/ {print $(NF)}' "$base/trx_info_debug")
        mcs=${mcs:--1}; raw_retry=${raw_retry:-0}
    else
        mcs=-1; raw_retry=0
    fi

    # 3. moving-average retry% (AVG_WIN samples)
    retry_hist="${retry_hist#* } $raw_retry"
    sum=0; for v in $retry_hist; do sum=$((sum+v)); done
    retry=$(( sum / AVG_WIN ))

    # 4. propose MIN / MAX
    if [ "$rssi" -le "$RSSI_PANIC" ] || [ "$retry" -gt "$RETRY_CLAMP" ]; then
        prop=$MIN                                   # bad link → floor
    else
        prop=$([ "$mcs" -ge 3 ] && echo $MAX || echo $MIN)
    fi

    # 5. maybe change encoder
    if diff10 "$prop" "$curr"; then
        if [ "$prop" -lt "$curr" ]; then            # DOWN: always take
            curr=$prop
            echo "CMD:set video0.bitrate=$curr"
            curl -s "http://localhost/api/v1/set?video0.bitrate=$curr" >/dev/null &
        else                                        # UP: retry OK & cool-down
            if [ "$retry" -lt "$RETRY_CLAMP" ] && [ $((now-last_up)) -ge $UP_COOL ]; then
                curr=$prop; last_up=$now
                echo "CMD:set video0.bitrate=$curr"
                curl -s "http://localhost/api/v1/set?video0.bitrate=$curr" >/dev/null &
            fi
        fi
    fi

    printf '%s RSSI=%4s MCS=%2s Retry=%3s%% Rate=%5sk\n' \
           "$now" "$rssi" "$mcs" "$retry" "$curr"

    sleep "$INTERVAL"
done
