#!/bin/sh
# -------- linkwatch.sh (BusyBox-friendly) -------------
# Poll /proc/net/wireless (RSSI) and the interface-level Tx-error counter.
# Prints every sample and calls $THROTTLE_CMD when either metric
# crosses the WARN_* thresholds.

IFACE=wlan0               # change if your radio has another name
WARN_RSSI=-65             # dBm threshold
WARN_ERRPCT=20            # %   threshold
INTERVAL=0.2              # seconds between polls
THROTTLE_CMD="/usr/bin/throttle-encoder"   # ← your control hook

prev_tx_ok=0
prev_tx_err=0

while :; do
    # ---- 1. RSSI from /proc/net/wireless ----
    rssi=$(awk -v ifname="$IFACE" '$1 == ifname":" {print -$4}' /proc/net/wireless)
    [ -z "$rssi" ] && rssi=-100            # default if awk finds nothing

    # ---- 2. Tx counters from sysfs (may be 0 if driver doesn’t fill them) ----
    tx_ok=$(cat /sys/class/net/$IFACE/statistics/tx_packets 2>/dev/null)
    tx_err=$(cat /sys/class/net/$IFACE/statistics/tx_errors  2>/dev/null)
    tx_ok=${tx_ok:-0};  tx_err=${tx_err:-0}

    delta_ok=$((tx_ok  - prev_tx_ok))
    delta_err=$((tx_err - prev_tx_err))
    prev_tx_ok=$tx_ok
    prev_tx_err=$tx_err

    # avoid divide-by-zero
    if [ "$delta_ok" -eq 0 ]; then
        errpct=0
    else
        errpct=$((100 * delta_err / delta_ok))
    fi

    # ---- 3. print the live sample ----
    # Format: epoch-seconds  RSSI  +pkt  +err  err%
    printf '%s  RSSI=%4s dBm  +pkt=%4s  +err=%4s  err%%=%3s\n' \
           "$(date +%s)" "$rssi" "$delta_ok" "$delta_err" "$errpct"

    # ---- 4. decide if we need to throttle ----
    if [ "$rssi" -lt "$WARN_RSSI" ] || [ "$errpct" -ge "$WARN_ERRPCT" ]; then
        logger -t linkwatch "RSSI ${rssi} dBm / err ${errpct}% – throttling encoder"
        $THROTTLE_CMD  low    # replace with your actual action
    fi

    sleep "$INTERVAL"
done
