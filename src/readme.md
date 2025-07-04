gcc -O3 -march=native -Wall -std=gnu11 -lpcap -o wifi_sniff2udp wifi_sniff2udp.c

sudo setcap cap_net_raw,cap_net_admin,cap_sys_nice=eip \
     ./wifi_sniff2udp mon0 8c:aa:b5:12:34:56 127.0.0.1 5600 \
     --udp-port 5600 --dest-mac 3c:71:bf:09:8e:22 --batch 32 --cpu 2



gcc -O3 -march=native -Wall -std=gnu11 -o rtp_merge rtp_merge.c
