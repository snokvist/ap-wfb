#!/bin/sh
SRC=/proc/net/rtl8733bu/wlan0/trx_info_debug
MSG=/tmp/MSPOSD.msg

while true; do
    # Grab the three lines we want (any # of STAs → 0-N lines, plus one RX line)
    metrics="$(awk '
        # ---------- STA heading ----------
        /^============[[:space:]]*STA[[:space:]]*\[/ {
            mac = $3; sub(/^\[/,"",mac); sub(/\]$/,"",mac); next
        }

        # ---------- tracked STA fields ----------
        $1=="rssi"                 { rssi=$3 }
        $1=="is_noisy"             { noisy=$3 }
        $1=="curr_retry_ratio"     {
            retry=$3
            printf "STA %-17s RSSI=%s%% NOISY=%s RETRY=%s\n", mac,rssi,noisy,retry
            mac=rssi=noisy=retry=""
        }

        # ---------- Rx info ----------
        /^============[[:space:]]*Rx Info dump/ {
            getline                              # line with rssi_min
            for(i=1;i<=NF;i++) if($i=="rssi_min"){ rm=$(i+2); sub(/\(%.*$/,"",rm) }

            getline                              # Total False Alarm line
            for(i=1;i<=NF;i++) if($i=="Alarm"){ fa=$(i+2); gsub(/[^0-9]/,"",fa) }

            getline                              # rssi_a / rssi_b line
            for(i=1;i<=NF;i++){
                if($i=="rssi_a"){ ra=$(i+2); sub(/\(%.*$/,"",ra) }
                if($i=="rssi_b"){ rb=$(i+2); sub(/\(%.*$/,"",rb) }
            }
            printf "RX  RSSI_MIN=%s%%  RSSI_A=%s%%  RSSI_B=%s%%  FA=%s\n", \
                   rm,ra,rb,fa
            exit
        }' "$SRC")"

    # Compose the 4-line overlay file (banner + up to 3 metric lines)
    {
        echo '&L30&G8&F32CPU:&C &B temp:&T'   # ← line 1
        printf '%s\n' "$metrics"                               # ← lines 2-4
    } > "$MSG"

    sleep 0.5
done
