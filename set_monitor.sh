#!/bin/bash
set -emb

export LC_ALL=C

_cleanup()
{
  plist=$(jobs -p)
  if [ -n "$plist" ]
  then
      kill -TERM $plist || true
  fi
  exit 1
}

trap _cleanup EXIT


iw reg set BO

# init wlp3s0
if which nmcli > /dev/null && ! nmcli device show wlx40a5ef2f2308 | grep -q '(unmanaged)'
then
  nmcli device set wlx40a5ef2f2308 managed no
  sleep 1
fi

ip link set wlx40a5ef2f2308 down
iw dev wlx40a5ef2f2308 set monitor otherbss
ip link set wlx40a5ef2f2308 up
iw dev wlx40a5ef2f2308 set channel 161 HT20


echo "WFB-ng init done"
