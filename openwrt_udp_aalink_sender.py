#!/usr/bin/env python3
"""
Realtime link‑quality (0‑100 %) → UDP forwarder
===============================================

Changes in this build
---------------------
* **Source switched to `iwinfo`** – reads the “Link Quality x/70” field the
  driver exports via libiwinfo instead of the Realtek‑only *trx_info_debug*.
* **Percentage remap with head‑room / foot‑room** – the raw quality number
  is linearly mapped to 0‑100 % using user‑tunable `--min-quality` and
  `--max-quality` command‑line parameters (defaults: 0 → 0 %, 70 → 100 %).
  Adjust them later if you want to line‑up with Realtek’s 0‑100 % scale.
* **Graceful pause on missing data** – if quality can’t be read we stop
  sending UDP packets, back‑off for a moment and retry until data reappears.
* **Automatic retry on any failure** – wrap the whole detection/send loop
  so that any exception will be caught, logged, and retried after 3 seconds.
"""

from __future__ import annotations

import argparse
import os
import re
import signal
import socket
import subprocess
import sys
import time
from datetime import datetime

# ───────── constants ──────────────────────────────────────────────────────────
DETECT_SLEEP = 3          # seconds between attempts to find the interface/IP,
                          # and also used as back‑off on any unexpected error
BACKOFF_SLEEP = 1         # seconds to wait after quality/link failure
QUALITY_RE = re.compile(r"Link Quality:\s*(\d+)/(\d+)")

# ───────── helpers ────────────────────────────────────────────────────────────
def log(msg: str, verbose: bool) -> None:
    """Print a timestamped message when --verbose is active."""
    if verbose:
        print(f"{datetime.now():%F %T} {msg}", file=sys.stderr, flush=True)

def run(cmd: list[str]) -> str:
    """Return stdout of *cmd* or empty string on failure."""
    try:
        return subprocess.check_output(
            cmd, text=True, stderr=subprocess.DEVNULL, timeout=0.5
        )
    except subprocess.SubprocessError:
        return ""
    except OSError:
        # command not found or other OS error => treat as "no output"
        return ""

def find_iface_for_ip(ip: str) -> str | None:
    """Return the interface that carries *ip* (CIDR ignored) or None."""
    for line in run(["ip", "-o", "-4", "addr", "show"]).splitlines():
        parts = line.split()
        if len(parts) >= 4 and parts[3].startswith(ip + "/"):
            return parts[1]
    return None

def iface_is_connected(iface: str) -> bool:
    """True when the interface is associated to an AP (STA mode)."""
    return run(["iw", "dev", iface, "link"]).startswith("Connected to")

def quality_pct(
    iface: str, qmin: int = 0, qmax: int = 70
) -> int | None:
    """
    Return link quality as an integer 0‑100 %.
    *qmin* and *qmax* define the linear mapping of the raw quality score.
    """
    out = run(["iwinfo", iface, "info"])
    m = QUALITY_RE.search(out)
    if not m:
        return None

    raw, denom = map(int, m.groups())

    # Fall back to driver‑reported denominator when caller left *qmax* at default
    if qmax == 70 and denom != 0:
        qmax = denom

    # Linear scaling with clamping
    pct = round((raw - qmin) * 100 / max(1, qmax - qmin))
    return max(0, min(100, pct))

def _exit(*_):
    sys.exit(0)

# ───────── main ───────────────────────────────────────────────────────────────
def main() -> None:
    p = argparse.ArgumentParser(description="Forward link quality over UDP")
    p.add_argument("--target-ip", default="192.168.0.10",
                   help="IPv4 address expected on the chosen interface")
    p.add_argument("--dest-ip", default="192.168.0.1",
                   help="UDP receiver address")
    p.add_argument("--dest-port", type=int, default=12345,
                   help="UDP receiver port")
    p.add_argument("--interval", type=float, default=0.1,
                   help="seconds between sends")
    p.add_argument("--iface",
                   help="force WLAN interface (skip auto-detection)")
    p.add_argument("--min-quality", type=int, default=0,
                   help="quality value that maps to 0 %% (default 0)")
    p.add_argument("--max-quality", type=int, default=70,
                   help="quality value that maps to 100 %% (default 70)")
    p.add_argument("-v", "--verbose", action="store_true",
                   help="print per-second stats + diagnostics")
    args = p.parse_args()

    for sig in (signal.SIGINT, signal.SIGTERM):
        signal.signal(sig, _exit)

    # create our UDP socket once; on error we’ll recreate below
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setblocking(False)

    # ── infinite retry loop ────────────────────────────────────────────────────
    while True:
        try:
            # ─── choose interface ────────────────────────────────────────────────
            if args.iface:
                iface = args.iface
                log(f"Using interface {iface} (forced)", args.verbose)
            else:
                log(f"Waiting for {args.target_ip}…", args.verbose)
                while True:
                    iface = find_iface_for_ip(args.target_ip)
                    if iface and iface_is_connected(iface):
                        break
                    time.sleep(DETECT_SLEEP)
                log(f"{args.target_ip} is on {iface}; begin sending", args.verbose)

            # ─── per-second aggregation ─────────────────────────────────────────
            t0 = time.monotonic()
            cnt = total = 0
            last: int | None = None

            while True:
                if not iface_is_connected(iface):
                    log("Link lost; back to wait", args.verbose)
                    time.sleep(BACKOFF_SLEEP)
                    break

                q = quality_pct(iface, args.min_quality, args.max_quality)
                if q is None:
                    log("No quality data; pausing", args.verbose)
                    time.sleep(BACKOFF_SLEEP)
                    break

                try:
                    sock.sendto(str(q).encode(), (args.dest_ip, args.dest_port))
                except OSError:
                    pass  # silently drop if buffer/network issue

                cnt += 1
                total += q
                last = q

                now = time.monotonic()
                if now - t0 >= 1.0 and args.verbose and cnt:
                    avg = total / cnt
                    print(f"pkts={cnt}:avg={avg:.1f}:latest={last}", flush=True)
                    t0 = now
                    cnt = total = 0
                    last = None

                time.sleep(args.interval)

        except Exception as e:
            # catch _any_ unexpected failure, log & back off
            print(f"{datetime.now():%F %T} Error: {e!r}; retrying in {DETECT_SLEEP}s",
                  file=sys.stderr, flush=True)
            time.sleep(DETECT_SLEEP)

            # recreate socket (in case it was left in a bad state)
            sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            sock.setblocking(False)
            # and loop back to try again

if __name__ == "__main__":
    main()
