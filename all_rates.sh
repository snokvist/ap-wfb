#!/bin/sh
# set_rate.sh – universal writer + legacy-mask compatibility

RATE_FILE=$(ls /proc/net/*/wlan0/rate_ctl 2>/dev/null | head -n1) ||
  { echo "rate_ctl not found"; exit 1; }

##################### driver detect #####################
driver=unknown
for id in $(lsusb | awk '{print $6}' | uniq); do
  case "$id" in
    0bda:a81a)           driver=8812eu ;;
    0bda:f72b|0bda:b733) driver=8733bu ;;
    0bda:8812|0bda:881a|0b05:17d2|2357:0101|2604:0012) driver=88XXau ;;
  esac
done

##################### compatibility map #################
compat_mask() {  # $1=name  -> echoes hex OR empty
  case "$driver" in
    8733bu)
      case "$1" in
        mcs0) echo 0x000000CC ;;
        mcs1) echo 0x000000CD ;;
        mcs2) echo 0x000000CE ;;
        mcs3) echo 0x000000CF ;;
        mcs0-3) echo 0x0000000F ;;
        mcs0-3-sgi) echo 0x0000008F ;;
        mcs7) echo 0x000000FF ;;
      esac
      ;;
    8812eu)
      case "$1" in
        mcs0-3) echo 0x0000000F ;;
        mcs0-3-sgi) echo 0x0000008F ;;
        mcs7) echo 0x693 ;;
      esac
      ;;
  esac
}

##################### blacklist (8812eu) ################
is_black_8812eu() {
  idx=$1
  case "$idx" in
    0|1|2|3|9|10|11|1[6-9]|2[7-9]|3[0-9]|4[0-3]|4[8-9]|5[0-3]|6[4-9]|7[0-3]|7[4-9]|8[0-3]) return 0;;
  esac; return 1
}

##################### helpers ###########################
use_sgi=0; [ "$2" = "sgi" ] && use_sgi=128

name_to_idx() {  # (identical mapper as before – omitted here for brevity)
  n=$(echo "$1" | tr A-Z a-z)
  case "$n" in
    cck_1m) echo 0 ;; cck_2m) echo 1 ;; cck_5_5m) echo 2 ;; cck_11m) echo 3 ;;
    ofdm_6m) echo 4 ;; ofdm_9m) echo 5 ;; ofdm_12m) echo 6 ;; ofdm_18m) echo 7 ;;
    ofdm_24m) echo 8 ;; ofdm_36m) echo 9 ;; ofdm_48m) echo 10 ;; ofdm_54m) echo 11 ;;
    mcs*) v=${n#mcs}; [ "$v" -ge 0 ] 2>/dev/null && [ "$v" -le 31 ] && echo $((12+v)) ;;
    vhtss1mcs*) v=${n#vhtss1mcs}; [ "$v" -ge 0 ] 2>/dev/null && [ "$v" -le 9 ] && echo $((44+v)) ;;
    vhtss2mcs*) v=${n#vhtss2mcs}; [ "$v" -ge 0 ] 2>/dev/null && [ "$v" -le 9 ] && echo $((54+v)) ;;
    vhtss3mcs*) v=${n#vhtss3mcs}; [ "$v" -ge 0 ] 2>/dev/null && [ "$v" -le 9 ] && echo $((64+v)) ;;
    vhtss4mcs*) v=${n#vhtss4mcs}; [ "$v" -ge 0 ] 2>/dev/null && [ "$v" -le 9 ] && echo $((74+v)) ;;
  esac
}

##################### resolve hex #######################
raw="$1"
if printf '%s\n' "$raw" | grep -qi '^0x[0-9a-f]\+'; then
  HEX="$raw"                                # raw hex
else
  # 1) try legacy map
  HEX=$(compat_mask "$raw")
  if [ -z "$HEX" ]; then
    idx=$(name_to_idx "$raw") || { echo "Unknown rate '$raw'"; exit 1; }

    if [ "$driver" = 8812eu ] && is_black_8812eu "$idx"; then
      echo "Rate '$raw' is black-listed for 8812EU – not set."; exit 1
    fi
    HEX=$(printf '0x%X' $((idx + use_sgi)))
  fi
fi

echo "$HEX" > "$RATE_FILE" && echo "Wrote $HEX to $RATE_FILE"
