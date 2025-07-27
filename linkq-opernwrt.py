#!/usr/bin/env python3
import time, subprocess, sys, os, re, argparse, socket
from collections import deque

# ───── static configuration ─────────────────────────────────────────────────
DBG = "/sys/kernel/debug/ieee80211/phy1/ath10k"
IFACE_STA = "phy1-sta0"
MAC = "98:03:cf:cf:a4:28"

POLL_INTERVAL = 0.1
WINDOW_SIZE   = 3                    # 3 samples ≈ 0 .3 s
RETRY_INTERVAL = 3.0

RT_WARN, RT_CRIT = 0.20, 0.50
FL_WARN, FL_CRIT = 0.02, 0.05
ACK_WARN_DBM, ACK_CRIT_DBM = -60, -80
SIG_WARN_DBM, SIG_CRIT_DBM = -60, -80
PRSSI_WARN, PRSSI_CRIT     = 60, 40

# ───── colour helpers (unchanged) ───────────────────────────────────────────
def tri_color(val, warn, crit, prec=1):
    GRN,YEL,RED,RES = "\x1b[32m","\x1b[33m","\x1b[31m","\x1b[0m"
    pct=val*100
    col=RED if val>=crit else (YEL if val>=warn else GRN)
    return f"{col}{pct:.{prec}f}%{RES}"

def tri_color_dbm(val, warn, crit):
    GRN,YEL,RED,RES = "\x1b[32m","\x1b[33m","\x1b[31m","\x1b[0m"
    col=RED if val<=crit else (YEL if val<=warn else GRN)
    return f"{col}{val:4.0f}dBm{RES}"

# ───── parsers (unchanged) ─────────────────────────────────────────────────
def read_sta():
    try:
        out=subprocess.check_output(
            ["iw","dev",IFACE_STA,"station","get",MAC],
            stderr=subprocess.DEVNULL,timeout=1).decode()
    except subprocess.SubprocessError:
        return None
    r=f=ack=sig=None
    if m:=re.search(r"last ack signal:\s*(-?\d+)",out): ack=float(m.group(1))
    if m:=re.search(r"signal avg:\s*(-?\d+)",out):      sig=float(m.group(1))
    for l in out.splitlines():
        l=l.strip()
        if l.startswith("tx retries:"): r=int(l.split()[2])
        elif l.startswith("tx failed:"): f=int(l.split()[2])
    return None if None in (r,f,ack,sig) else (r,f,ack,sig)

def read_vdev():
    try: lines=open(os.path.join(DBG,"fw_stats")).readlines()
    except: return None
    mq=swr=sr1=srN=fr=na=0
    for L in lines:
        low=L.lower()
        if   "mpdu queued"            in low: mq=int(L.split()[-1])
        elif "mpdu sw requeued"       in low: swr=int(L.split()[-1])
        elif "mpdu success retry"     in low: sr1=int(L.split()[-1])
        elif "mpdu success multitry"  in low: srN=int(L.split()[-1])
        elif "mpdu fail retry"        in low: fr =int(L.split()[-1])
        elif "ppdu noack"             in low: na =int(L.split()[-1])
    return (mq,swr,sr1,srN,fr,na)

def read_pdev():
    try: lines=open(os.path.join(DBG,"fw_stats")).readlines()
    except: return None
    p_rq=p_ex=p_na=0
    for L in lines:
        low=L.lower()
        if   "mpdus requeued"        in low: p_rq=int(L.split()[-1])
        elif "excessive retries"     in low: p_ex=int(L.split()[-1])
        elif "mpdus receive no ack"  in low: p_na=int(L.split()[-1])
    return (p_rq,p_ex,p_na)

def read_peer_rssi():
    try: data=open(os.path.join(DBG,"fw_stats")).read()
    except: return None
    m=re.search(rf"Peer MAC address\s+{re.escape(MAC)}.*?Peer RSSI\s+(\d+)",
                data,re.DOTALL)
    return float(m.group(1)) if m else None

def wait_for_stats():
    while True:
        s,v,p = read_sta(), read_vdev(), read_pdev()
        if s and v and p: return s,v,p
        time.sleep(RETRY_INTERVAL)

# ───── main ────────────────────────────────────────────────────────────────
def main():
    cli=argparse.ArgumentParser(description="Link health monitor + UDP")
    cli.add_argument('-v','--verbose',action='store_true')
    cli.add_argument('--udp-ip',default='192.168.0.1')
    cli.add_argument('--udp-port',type=int,default=12345)
    args=cli.parse_args()

    sock=socket.socket(socket.AF_INET,socket.SOCK_DGRAM)
    dest=(args.udp_ip,args.udp_port)

    # deques for counters (full set so verbose works)
    sta_r=deque(maxlen=WINDOW_SIZE); sta_f=deque(maxlen=WINDOW_SIZE)
    mq_q=deque(maxlen=WINDOW_SIZE);  mq_s=deque(maxlen=WINDOW_SIZE)
    mq_sr1=deque(maxlen=WINDOW_SIZE);mq_srN=deque(maxlen=WINDOW_SIZE)
    mq_fr=deque(maxlen=WINDOW_SIZE); mq_na=deque(maxlen=WINDOW_SIZE)
    p_rq=deque(maxlen=WINDOW_SIZE);  p_ex=deque(maxlen=WINDOW_SIZE)
    p_na=deque(maxlen=WINDOW_SIZE)

    # history of instantaneous RT%
    rt_hist=deque(maxlen=WINDOW_SIZE)

    # prime
    sta,vdev,pdev = wait_for_stats()
    r,f,ack,sigavg = sta
    mq,swr,sr1,srN,fr,dna = vdev
    prq,pex,pna          = pdev
    prev_r, prev_q = r, mq

    # headers
    if args.verbose:
        print(f"{'TIME':<8} {'STA-R':>6} {'STA-F':>6}   "
              f"{'Q':>6} {'SW-RQ':>6} {'1-RT':>6} {'>1-RT':>6} "
              f"{'FAIL':>6} {'NA':>6}   {'FW-RQ':>6} {'FW-EX':>6} {'FW-NA':>6}   "
              f"RT%    FL%   ACK   SIGAVG   PRSSI")
    else:
        print("TIME     RT%    FL%   ACK   SIGAVG   PRSSI")

    counter=0; fast=False

    try:
        while True:
            time.sleep(POLL_INTERVAL)

            sta,vdev,pdev = read_sta(), read_vdev(), read_pdev()
            if not (sta and vdev and pdev):
                fast=False
                sta,vdev,pdev = wait_for_stats()
                r,f,ack,sigavg = sta
                mq,swr,sr1,srN,fr,dna = vdev
                prq,pex,pna = pdev
                prev_r, prev_q = r, mq
                sta_r.clear(); sta_f.clear(); mq_q.clear(); mq_s.clear()
                mq_sr1.clear(); mq_srN.clear(); mq_fr.clear(); mq_na.clear()
                p_rq.clear(); p_ex.clear(); p_na.clear(); rt_hist.clear()
                counter=0
                continue

            r,f,ack,sigavg = sta
            mq,swr,sr1,srN,fr,dna = vdev
            prq,pex,pna          = pdev
            peer = read_peer_rssi() or 0.0

            # instantaneous differences (0.1 s)
            dr_i = r  - prev_r
            dq_i = mq - prev_q
            prev_r, prev_q = r, mq
            rt_i = dr_i / dq_i if dq_i else 0.0
            rt_hist.append(rt_i)

            # weighted RT% over last 3 polls
            if len(rt_hist)==3:
                rt_w = 0.5*rt_hist[-1] + 0.25*rt_hist[-2] + 0.25*rt_hist[-3]
            elif len(rt_hist)==2:
                rt_w = 0.6*rt_hist[-1] + 0.4*rt_hist[-2]
            else:
                rt_w = rt_hist[-1]

            # health score 0‑80
            rt_pct   = min(rt_w*100.0, 50.0)
            score    = (50.0 - rt_pct) * 1.6

            # send UDP
            payload=f"{peer:.0f},{score:.0f}"
            try: sock.sendto(payload.encode(),dest)
            except Exception: pass
            if args.verbose: print(f"[UDP] {payload}")

            # append counters for verbose deltas / FL%
            sta_r.append(r); sta_f.append(f)
            mq_q.append(mq); mq_s.append(swr)
            mq_sr1.append(sr1); mq_srN.append(srN)
            mq_fr.append(fr);  mq_na.append(dna)
            p_rq.append(prq);  p_ex.append(pex); p_na.append(pna)

            counter += 1
            window_ready = counter >= WINDOW_SIZE
            if window_ready:
                # window deltas
                dr = sta_r[-1]-sta_r[0]; df = sta_f[-1]-sta_f[0]
                dq = mq_q[-1]-mq_q[0];   dswr = mq_s[-1]-mq_s[0]
                dsr1=mq_sr1[-1]-mq_sr1[0]; dsrN=mq_srN[-1]-mq_srN[0]
                dfr = mq_fr[-1]-mq_fr[0]; dna = mq_na[-1]-mq_na[0]
                dp_rq=p_rq[-1]-p_rq[0]; dp_ex=p_ex[-1]-p_ex[0]; dp_na=p_na[-1]-p_na[0]
                fl_w = dfr / dq if dq else 0.0
                counter=0
            else:
                # placeholder values until window fills
                dr=df=dq=dswr=dsr1=dsrN=dfr=dna=dp_rq=dp_ex=dp_na=0
                fl_w = 0.0

            # colours & fast mode
            rt_col = tri_color(rt_w, RT_WARN, RT_CRIT, 1)
            fl_col = tri_color(fl_w, FL_WARN, FL_CRIT, 3)
            ack_col  = tri_color_dbm(ack,    ACK_WARN_DBM, ACK_CRIT_DBM)
            sig_col  = tri_color_dbm(sigavg, SIG_WARN_DBM, SIG_CRIT_DBM)
            peer_col = tri_color_dbm(peer,   PRSSI_WARN,   PRSSI_CRIT)
            fast = rt_w >= RT_WARN or fl_w >= FL_WARN

            if fast or window_ready:
                t=time.strftime("%H:%M:%S")
                if args.verbose and window_ready:
                    print(f"{t:<8} {dr:6d} {df:6d}   "
                          f"{dq:6d} {dswr:6d} {dsr1:6d} {dsrN:6d} {dfr:6d} {dna:6d}   "
                          f"{dp_rq:6d} {dp_ex:6d} {dp_na:6d}   "
                          f"{rt_col}  {fl_col}  {ack_col} {sig_col} {peer_col}")
                elif window_ready:
                    print(f"{t:<8} {rt_col}  {fl_col}  {ack_col} {sig_col} {peer_col}")

    except KeyboardInterrupt:
        print("\nExiting gracefully.")
        sys.exit(0)

if __name__ == "__main__":
    main()
