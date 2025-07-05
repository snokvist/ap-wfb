#!/bin/sh
# ------------------------------------------------------------
# linkwatch-realtek-tight7.sh
#   • floor 4096 k, ceiling 20480 k
#   • retry >3 %  → floor
#   • |Δ| ≥ 10 % to change
#   • UP shift: raw retry <3 %, EWMA retry <3 %, 5-s cool-down
#   • DOWN shift: immediate
#   • prints CMD:set ... when curl fires
# ------------------------------------------------------------

IFACE=wlan0
FLOOR_KBPS=4096
CEIL_KBPS=12480
UP_COOLDOWN=5          # seconds between upward changes

# link guard-rails
WARN_RSSI=-65; WARN_MCS=4; WARN_THRUPUT=5; WARN_RETRY=30
FAST_RETRY=60; FAST_TX=1
INTERVAL=0.2
BAD_TRIP=3
GOOD_TRIP=5          # ≈2 s recovery

# state
s_retry=0; s_tx=0
bad_ctr=0; good_ctr=0; throttled=0
prev_tx_ok=0
curr_rate_kbps=$FLOOR_KBPS
last_up_epoch=0       # last successful up-shift (epoch seconds)

mcs_phy_kbps() { case "$1" in
 0) echo 6500;;1) echo 13000;;2) echo 19500;;3) echo 26000;;
 4) echo 39000;;5) echo 52000;;6) echo 58500;;7) echo 65000;;*) echo 0;; esac; }

percent_diff_ge10() { [ "$2" -eq 0 ] && return 0
    diff=$(( $1>$2 ? $1-$2 : $2-$1 ))
    [ $(( diff*10 )) -ge "$2" ]; }

while :; do
  epoch=$(date +%s)

  # ----- RSSI & packet delta ----------------------------------
  rssi=$(awk -v ifname="$IFACE" '$1==ifname":"{print -$4}' /proc/net/wireless)
  [ -z "$rssi" ] && rssi=-100
  tx_ok=$(cat /sys/class/net/$IFACE/statistics/tx_packets 2>/dev/null); tx_ok=${tx_ok:-0}
  delta_ok=$((tx_ok-prev_tx_ok)); prev_tx_ok=$tx_ok

  # ----- Realtek metrics --------------------------------------
  base="/proc/net/rtl8733bu/$IFACE"

  # instantaneous 1-s TP, ignore “Smooth TP”
  tx_tp_mbps=$(sed -n '/\[TP\] Tx[[:space:]]*:/s/.*Tx[[:space:]]*:[[:space:]]*\([0-9]\+\).*/\1/p' \
               "$base/sta_tp_info" 2>/dev/null)
  tx_tp_mbps=${tx_tp_mbps:-0}; [ "$tx_tp_mbps" -gt 100 ] && tx_tp_mbps=0

  if [ -r "$base/trx_info_debug" ]; then
      mcs=$(awk '/curr_tx_rate/{for(i=1;i<=NF;i++)if($i~/^MCS[0-9]+/){sub(/^MCS/,"",$i);print $i;exit}}' "$base/trx_info_debug")
      retry_pct=$(awk '/curr_retry_ratio/{print $(NF)}' "$base/trx_info_debug")
      mcs=${mcs:--1}; retry_pct=${retry_pct:-0}
  else
      mcs=-1; retry_pct=0
  fi

  # ----- EWMA (display only) ----------------------------------
  s_retry=$(( (s_retry*3 + retry_pct*100)/4 ))
  s_tx=$(( (s_tx*3 + tx_tp_mbps*100)/4 ))
  disp_retry=$(( s_retry/100 ))
  disp_tx=$(( s_tx/100 ))

  # ----- propose safe bitrate ---------------------------------
  phy_kbps=$(mcs_phy_kbps "$mcs")
  if [ "$retry_pct" -gt 3 ]; then
      prop_kbps=$FLOOR_KBPS
  else
      prop_kbps=$(( phy_kbps*65*(100-retry_pct)/10000 ))
      [ "$prop_kbps" -lt "$FLOOR_KBPS" ] && prop_kbps=$FLOOR_KBPS
      [ "$prop_kbps" -gt "$CEIL_KBPS" ] && prop_kbps=$CEIL_KBPS
  fi

  # ----- decide if we can switch --------------------------------
  new_flag=""
  if percent_diff_ge10 "$prop_kbps" "$curr_rate_kbps"; then
      if [ "$prop_kbps" -lt "$curr_rate_kbps" ]; then             # DOWN — always
          curr_rate_kbps=$prop_kbps
          new_flag=" NEW=${curr_rate_kbps}k"
          echo "CMD:set video0.bitrate=${curr_rate_kbps}"
          curl -s "http://localhost/api/v1/set?video0.bitrate=$curr_rate_kbps" >/dev/null &
      else                                                         # UP — obey rules
          if [ $((epoch-last_up_epoch)) -ge $UP_COOLDOWN ] && \
             [ "$retry_pct" -lt 3 ] && [ "$disp_retry" -lt 3 ]; then
              curr_rate_kbps=$prop_kbps
              new_flag=" NEW=${curr_rate_kbps}k"
              echo "CMD:set video0.bitrate=${curr_rate_kbps}"
              curl -s "http://localhost/api/v1/set?video0.bitrate=$curr_rate_kbps" >/dev/null &
              last_up_epoch=$epoch
          fi
      fi
  fi

  # ----- print sample ------------------------------------------
  printf '%s RSSI=%4s +pkt=%5s MCS=%2s Retry=%3s%% Tx=%3sMb Rate=%5sk%s' \
         "$epoch" "$rssi" "$delta_ok" "$mcs" "$disp_retry" "$disp_tx" \
         "$curr_rate_kbps" "$new_flag"

  # ----- throttle / recover tag (logic unchanged) --------------
  reasons=""
  if [ "$retry_pct" -ge "$FAST_RETRY" ]; then reasons="RetryFast"
  elif [ "$tx_tp_mbps" -le "$FAST_TX" ] && [ "$tx_tp_mbps" -gt 0 ]; then reasons="TxFast"
  else
      [ "$rssi" -lt "$WARN_RSSI" ] && reasons="$reasons RSSI"
      ( [ "$mcs" -ge 0 ] && [ "$mcs" -le "$WARN_MCS" ] ) && reasons="$reasons MCS"
      [ "$tx_tp_mbps" -le "$WARN_THRUPUT" ] && reasons="$reasons Tx"
      [ "$retry_pct"  -ge "$WARN_RETRY"   ] && reasons="$reasons Retry"
      reasons=$(echo "$reasons"|sed 's/^ //')
  fi

  [ -n "$reasons" ] && { bad_ctr=$((bad_ctr+1)); good_ctr=0; } \
                     || { good_ctr=$((good_ctr+1)); bad_ctr=0; }
  fire=0
  [ "$throttled" -eq 0 ] && [ "$bad_ctr" -ge "$BAD_TRIP" ] && fire=1
  [ "$reasons" = "RetryFast" -o "$reasons" = "TxFast" ] && fire=1
  recover=0
  [ "$throttled" -eq 1 ] && [ "$good_ctr" -ge "$GOOD_TRIP" ] && recover=1

  if [ "$fire" -eq 1 ]; then throttled=1; bad_ctr=0; echo "  THROTTLE:$reasons"
  elif [ "$recover" -eq 1 ]; then throttled=0; good_ctr=0; echo "  RECOVER"
  else echo; fi

  sleep "$INTERVAL"
done
