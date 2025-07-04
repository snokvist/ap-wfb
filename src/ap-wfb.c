/* wifi_sniff2udp.c – open-WiFi monitor sniffer → UDP forwarder with 1 s stats
 *
 * Build:
 *   gcc -O3 -march=native -Wall -std=gnu11 -lpcap -o wifi_sniff2udp wifi_sniff2udp.c
 */

#define _GNU_SOURCE
#include <pcap.h>
#include <arpa/inet.h>
#include <netinet/if_ether.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sched.h>
#include <unistd.h>
#include <errno.h>
#include <inttypes.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#ifndef sendmmsg
# define sendmmsg(sockfd,msgvec,vlen,flags) syscall(SYS_sendmmsg,sockfd,msgvec,vlen,flags)
#endif

/* ---------- constants */
#define MAX_BATCH 64
#define MAX_PKT   1600

/* ---------- helpers */
static void pin_cpu(int cpu){
    if(cpu<0) return;
    cpu_set_t s; CPU_ZERO(&s); CPU_SET(cpu,&s);
    if(!sched_setaffinity(0,sizeof(s),&s))
        fprintf(stderr,"◎ pinned to CPU %d\n",cpu);
    else  perror("sched_setaffinity");
}
static int mac_aton(const char *s,uint8_t mac[6]){
    return sscanf(s,"%hhx:%hhx:%hhx:%hhx:%hhx:%hhx",
                  &mac[0],&mac[1],&mac[2],&mac[3],&mac[4],&mac[5])==6;
}

/* ---------- global filters */
static uint8_t mac_bssid[6];
static uint8_t mac_dest[6];  int dest_on=0;
static uint8_t mac_group[6]; int group_on=0;
static int udp_filter=-1;
static int batch_sz =16;

/* ---------- stats (per-second) */
static uint64_t stat_recv=0, stat_fwd=0, stat_badfcs=0;
static struct timespec t_prev;

/* ---------- TX batching */
static int out_sock=-1, tx_cnt=0;
static uint8_t  tx_buf[MAX_BATCH][MAX_PKT];
static struct iovec  tx_iov[MAX_BATCH];
static struct mmsghdr tx_msg[MAX_BATCH];
static void tx_flush(){
    if(tx_cnt==0) return;
    int sent=sendmmsg(out_sock,tx_msg,tx_cnt,0);
    if(sent<0) perror("sendmmsg"); else stat_fwd+=sent;
    tx_cnt=0;
}

/* ---------- radiotap */
#define RTAP_F_BADFCS 0x40
struct radiotap_header{ uint8_t v,p; uint16_t len; uint32_t present[]; } __attribute__((packed));

/* ---------- per-packet handler */
static void handle_pkt(const struct pcap_pkthdr *h,const uint8_t *p){
    if(h->caplen<sizeof(struct radiotap_header)) return;
    const struct radiotap_header *rh=(const void*)p;
    uint16_t rtlen=le16toh(rh->len); if(rtlen>h->caplen) return;
    uint32_t pres=rh->present[0];

    /* bad FCS? */
    if(pres&(1<<1)){
        size_t o=sizeof(struct radiotap_header);
        while(pres&0x80000000){ if(o+4>h->caplen) return;
            pres=((uint32_t*)p)[o/4]; o+=4; }
        if(p[o]&RTAP_F_BADFCS){ stat_badfcs++; return; }
    }
    size_t off=rtlen;

    if(off+24>h->caplen) return;
    const uint8_t *fc=p+off;
    uint16_t fc16=fc[0]|(fc[1]<<8);
    if(!(((fc16>>8)&1)==1 && ((fc16>>9)&1)==0)) return; /* to-DS only */

    const uint8_t *addr2=p+off+10; if(memcmp(addr2,mac_bssid,6)!=0) return;
    const uint8_t *addr1=p+off+4;
    if(dest_on && memcmp(addr1,mac_dest,6)!=0) return;
    if(group_on&& memcmp(addr1,mac_group,6)!=0) return;

    int qos=((fc16>>7)&1)&&((fc16&0x0c)==0x08);
    off+=24+(qos?2:0); if(off+8>h->caplen) return; off+=8;

    const uint8_t *ip=p+off; uint8_t ver=ip[0]>>4;
    uint16_t udp_dst, udp_len; size_t udp_off;
    if(ver==4){
        uint8_t ihl=(ip[0]&0x0f)*4; if(ihl<20||off+ihl+8>h->caplen) return;
        const uint8_t *udp=ip+ihl;
        udp_dst=(udp[2]<<8)|udp[3]; udp_len=(udp[4]<<8)|udp[5];
        udp_off=off+ihl;
    }else if(ver==6){
        if(off+40+8>h->caplen) return;
        const uint8_t *udp=ip+40;
        udp_dst=(udp[2]<<8)|udp[3]; udp_len=(udp[4]<<8)|udp[5];
        udp_off=off+40;
    }else return;
    if(udp_filter!=-1 && udp_dst!=udp_filter) return;
    if(udp_len+8>MAX_PKT) return;

    stat_recv++;
    memcpy(tx_buf[tx_cnt], p+udp_off, udp_len+8);
    tx_iov[tx_cnt].iov_base=tx_buf[tx_cnt];
    tx_iov[tx_cnt].iov_len =udp_len+8;
    tx_msg[tx_cnt].msg_hdr.msg_iov=&tx_iov[tx_cnt];
    tx_msg[tx_cnt].msg_hdr.msg_iovlen=1;
    tx_cnt++; if(tx_cnt==batch_sz) tx_flush();
}

/* ---------- main */
int main(int argc,char **argv){
    if(argc<5){
        fprintf(stderr,
"usage: %s IFACE BSSID DEST_IP DEST_PORT "
"[--udp-port N] [--dest-mac XX:..] [--group-ip A.B.C.D] [--batch N] [--cpu N]\n", argv[0]);
        return 1;
    }
    const char *iface=argv[1];
    if(!mac_aton(argv[2],mac_bssid)){fprintf(stderr,"bad BSSID\n");return 1;}
    const char *dst_ip=argv[3]; int dst_port=atoi(argv[4]);

    for(int i=5;i<argc;i++){
        if(!strcmp(argv[i],"--udp-port")&&i+1<argc){ udp_filter=atoi(argv[++i]); continue; }
        if(!strcmp(argv[i],"--dest-mac")&&i+1<argc){
            if(!mac_aton(argv[++i],mac_dest)){fprintf(stderr,"bad dest mac\n");return 1;}
            dest_on=1; continue;
        }
        if(!strcmp(argv[i],"--group-ip")&&i+1<argc){
            struct in_addr g; inet_pton(AF_INET,argv[++i],&g);
            mac_group[0]=0x01; mac_group[1]=0x00; mac_group[2]=0x5e;
            mac_group[3]=(g.s_addr>>8)&0x7f;
            mac_group[4]=(g.s_addr>>16)&0xff;
            mac_group[5]=(g.s_addr>>24)&0xff;
            group_on=1; continue;
        }
        if(!strcmp(argv[i],"--batch")&&i+1<argc){ batch_sz=atoi(argv[++i]); continue; }
        if(!strcmp(argv[i],"--cpu")&&i+1<argc){ pin_cpu(atoi(argv[++i])); continue; }
        fprintf(stderr,"unknown option %s\n",argv[i]); return 1;
    }
    if(batch_sz<1) batch_sz=1; if(batch_sz>MAX_BATCH) batch_sz=MAX_BATCH;

    /* pcap */
    char err[PCAP_ERRBUF_SIZE];
    pcap_t *pc=pcap_create(iface,err);
    if(!pc){fprintf(stderr,"%s\n",err);return 1;}
    pcap_set_snaplen(pc,2048);
    pcap_set_promisc(pc,1);
    pcap_set_immediate_mode(pc,1);
    pcap_set_timeout(pc,100);          /* 100 ms poll */
    if(pcap_activate(pc)!=0){fprintf(stderr,"pcap activate failed\n");return 1;}

    /* UDP out */
    out_sock=socket(AF_INET,SOCK_DGRAM,0);
    struct sockaddr_in dst={.sin_family=AF_INET,.sin_port=htons(dst_port)};
    inet_pton(AF_INET,dst_ip,&dst.sin_addr);
    if(connect(out_sock,(struct sockaddr*)&dst,sizeof(dst))<0){perror("connect");return 1;}
    for(int i=0;i<MAX_BATCH;i++){ tx_msg[i].msg_hdr.msg_name=NULL; tx_msg[i].msg_hdr.msg_namelen=0; }

    clock_gettime(CLOCK_MONOTONIC,&t_prev);

    /* loop */
    while(1){
        const uint8_t *pkt; struct pcap_pkthdr hdr;
        int rc=pcap_next_ex(pc,&hdr,&pkt);
        if(rc==1) handle_pkt(&hdr,pkt);      /* got packet */
        else if(rc==-1){ fprintf(stderr,"pcap err: %s\n",pcap_geterr(pc)); break; }

        /* stats every second */
        struct timespec now; clock_gettime(CLOCK_MONOTONIC,&now);
        double dt=(now.tv_sec-t_prev.tv_sec)+(now.tv_nsec-t_prev.tv_nsec)/1e9;
        if(dt>=1.0){
            tx_flush();                      /* flush leftovers */
            double ts=now.tv_sec+now.tv_nsec/1e9;
            printf("%.3f:recv=%"PRIu64":fwd=%"PRIu64":badfcs=%"PRIu64"\n",
                   ts,stat_recv,stat_fwd,stat_badfcs);
            fflush(stdout);
            stat_recv=stat_fwd=stat_badfcs=0;
            t_prev=now;
        }
    }
    return 0;
}
