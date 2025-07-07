#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <inttypes.h>
#include <signal.h>
#include <fcntl.h>
#include <errno.h>
#include <sched.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <time.h>

#define IN_PORT        5600
#define UNICAST_IP     "192.168.0.10"
#define UNICAST_PORT   5600
#define BUF_SIZE       2048
#define MAX_BATCH      64            /* max duplicates */

/* ───────── runtime-configurable broadcast address ───────── */
static char bcast_ip[INET_ADDRSTRLEN] = "";

/* ───────── global state ───────── */
volatile sig_atomic_t mode = 0;      /* default: unicast          */
int batch_size = 0;                  /* 0 = no duplication        */

uint64_t packet_count = 0, bytes_count = 0;

/* ───────── signal handlers ───────── */
void handle_sigusr1(int s){ (void)s; mode = 0; }  /* unicast           */
void handle_sigusr2(int s){ (void)s; mode = 1; }  /* broadcast         */
void handle_sigterm(int s){ (void)s; mode = 2; }  /* both              */
void handle_sigquit(int s){ (void)s; mode = 3; }  /* broadcast@5600    */

/* ───────── misc helpers ───────── */
static void print_help(const char *p)
{
    printf("Usage: %s --bcast-addr A.B.C.D  [--batch N] [--start-mode MODE]\n", p);
    printf("  --bcast-addr ADDR   (mandatory) LAN broadcast address, e.g. 192.168.0.255\n");
    printf("  --batch N           duplicate each packet N times (1-%d)\n", MAX_BATCH);
    printf("  --start-mode MODE   unicast | broadcast | both | broadcast5600 (default: unicast)\n");
    printf("  --help              show this help\n\n");
    printf("Signals at runtime:\n");
    printf("  SIGUSR1 → unicast only  (192.168.0.10:5600)\n");
    printf("  SIGUSR2 → broadcast only (%s:5601)\n", *bcast_ip ? bcast_ip : "<set at runtime>");
    printf("  SIGTERM → both (unicast + broadcast)\n");
    printf("  SIGQUIT → broadcast only on %s:5600\n", *bcast_ip ? bcast_ip : "<set at runtime>");
    exit(0);
}

static const char *mode_str(void){
    switch(mode){
        case 0: return "unicast";
        case 1: return "broadcast";
        case 2: return "both";
        case 3: return "broadcast5600";
        default:return "?";
    }
}

/* ───────── realtime tweaks ───────── */
static void set_realtime(void)
{
    struct sched_param sp = { .sched_priority = 20 };
    sched_setscheduler(0, SCHED_FIFO, &sp);
    mlockall(MCL_CURRENT | MCL_FUTURE);
    cpu_set_t m; CPU_ZERO(&m); CPU_SET(0, &m);
    sched_setaffinity(0, sizeof(m), &m);
}

/* ───────── main ───────── */
int main(int argc, char **argv)
{
    /* ─── parse CLI ─── */
    for(int i=1; i<argc; i++){
        if(!strcmp(argv[i], "--help")) print_help(argv[0]);

        else if(!strcmp(argv[i], "--bcast-addr") && i+1<argc){
            strncpy(bcast_ip, argv[++i], sizeof(bcast_ip)-1);
            bcast_ip[sizeof(bcast_ip)-1] = '\0';
        }
        else if(!strcmp(argv[i], "--batch") && i+1<argc){
            batch_size = atoi(argv[++i]);
            if(batch_size<1 || batch_size>MAX_BATCH){
                fprintf(stderr, "Invalid batch size (1-%d)\n", MAX_BATCH);
                return 1;
            }
        }
        else if(!strcmp(argv[i], "--start-mode") && i+1<argc){
            i++;
            if(!strcmp(argv[i], "unicast"))          mode = 0;
            else if(!strcmp(argv[i], "broadcast"))   mode = 1;
            else if(!strcmp(argv[i], "both"))        mode = 2;
            else if(!strcmp(argv[i], "broadcast5600")) mode = 3;
            else { fprintf(stderr, "Unknown mode: %s\n", argv[i]); return 1; }
        }
        else{
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            print_help(argv[0]);
        }
    }

    if(*bcast_ip == '\0'){
        fprintf(stderr, "--bcast-addr is mandatory\n");
        print_help(argv[0]);
    }

    set_realtime();

    /* signals */
    signal(SIGUSR1, handle_sigusr1);
    signal(SIGUSR2, handle_sigusr2);
    signal(SIGTERM, handle_sigterm);
    signal(SIGQUIT, handle_sigquit);

    /* sockets */
    int in_sock  = socket(AF_INET, SOCK_DGRAM, 0);
    int out_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if(in_sock<0 || out_sock<0){ perror("socket"); return 1; }

    int yes=1; setsockopt(out_sock, SOL_SOCKET, SO_BROADCAST, &yes, sizeof(yes));

    struct sockaddr_in in_addr={0}, uni_addr={0}, bcast_addr={0}, bcast5600_addr={0};
    in_addr.sin_family = AF_INET;
    in_addr.sin_port   = htons(IN_PORT);
    inet_pton(AF_INET, "127.0.0.1", &in_addr.sin_addr);
    if(bind(in_sock, (struct sockaddr*)&in_addr, sizeof(in_addr))<0){ perror("bind"); return 1; }

    uni_addr.sin_family = AF_INET;
    uni_addr.sin_port   = htons(UNICAST_PORT);
    inet_pton(AF_INET, UNICAST_IP, &uni_addr.sin_addr);

    bcast_addr.sin_family = AF_INET;
    bcast_addr.sin_port   = htons(5601);
    if(inet_pton(AF_INET, bcast_ip, &bcast_addr.sin_addr)!=1){
        fprintf(stderr, "Invalid --bcast-addr: %s\n", bcast_ip); return 1;
    }

    bcast5600_addr      = bcast_addr;
    bcast5600_addr.sin_port = htons(UNICAST_PORT);

    /* main loop */
    char buf[BUF_SIZE];
    struct sockaddr_in src; socklen_t srclen = sizeof(src);

    struct timespec last; clock_gettime(CLOCK_MONOTONIC, &last);
    int loops = 0;

    for(;;){
        ssize_t len = recvfrom(in_sock, buf, sizeof(buf), 0,
                               (struct sockaddr*)&src, &srclen);
        if(len<=0) continue;

        packet_count++; bytes_count += len;

        struct sockaddr_in *d1=NULL,*d2=NULL; int dup1=0,dup2=0;
        switch(mode){
            case 0: d1=&uni_addr;           dup1=batch_size?batch_size:1; break;
            case 1: d1=&bcast_addr;         dup1=batch_size?batch_size:1; break;
            case 2: d1=&uni_addr; d2=&bcast_addr;
                    dup1=dup2=batch_size?batch_size:1;                    break;
            case 3: d1=&bcast5600_addr;     dup1=batch_size?batch_size:1; break;
        }

        for(int i=0;i<dup1;i++)
            sendto(out_sock, buf, len, 0, (struct sockaddr*)d1, sizeof(*d1));
        if(d2) for(int i=0;i<dup2;i++)
            sendto(out_sock, buf, len, 0, (struct sockaddr*)d2, sizeof(*d2));

        /* stats each ~sec */
        if(++loops>=100){
            loops=0;
            struct timespec now; clock_gettime(CLOCK_MONOTONIC,&now);
            if(now.tv_sec - last.tv_sec >= 1){
                double mbps = (bytes_count*8)/1e6;
                printf("%" PRIu64 " packets (%.2f Mbps) last sec, mode=%s\n",
                       packet_count, mbps, mode_str());
                packet_count = bytes_count = 0;
                last = now;
            }
        }
    }
}
