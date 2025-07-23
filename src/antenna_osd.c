/*
 * rssi_bar.c – RSSI bar OSD + background ICMP traffic generator
 * -------------------------------------------------------------------------
 * Build :  gcc -O2 -std=c11 -Wall -o rssi_bar rssi_bar.c
 * Run   :  sudo ./rssi_bar [--config <file>]
 * Needs root for raw ICMP sockets.
 */

#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <getopt.h>
#include <netinet/ip_icmp.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* ----------------------------- defaults --------------------------------- */
#define DEF_CFG_FILE       "/etc/antennaosd.conf"
#define DEF_INFO_FILE      "/proc/net/rtl88x2eu/wlan0/trx_info_debug"
#define DEF_OUT_FILE       "/tmp/MSPOSD.msg"
#define DEF_INTERVAL        0.1
#define DEF_BAR_WIDTH       37
#define DEF_TOP             80
#define DEF_BOTTOM          20

#define DEF_OSD_HDR         " &F34&L20"
#define DEF_OSD_HDR2        ""
#define DEF_SYS_MSG_HDR     ""
#define DEF_SYS_MSG_TIMEOUT 10         /* system message timeout (seconds) */

#define DEF_RSSI_CONTROL    0
#define DEF_RSSI_RANGE0     "&F34&L10"
#define DEF_RSSI_RANGE1     "&F34&L10"
#define DEF_RSSI_RANGE2     "&F34&L40"
#define DEF_RSSI_RANGE3     "&F34&L40"
#define DEF_RSSI_RANGE4     "&F34&L20"
#define DEF_RSSI_RANGE5     "&F34&L20"

#define DEF_PING_IP         "192.168.0.10"
#define DEF_START           "["
#define DEF_END             "]"
#define DEF_EMPTY           "."
#define DEF_SHOW_STATS      1

#define SYS_MSG_FILE        "/tmp/osd_system.msg"

/* ------------------------------ glyphs ---------------------------------- */
static const char *GL_ANT  = "\uF012";                 /*   */
static const char *FULL    = "\u2588";                 /* █  */
static const char *PART[7] = { "\u2581","\u2582","\u2583","\u2584",
                               "\u2585","\u2586","\u2587" };

/* ------------------------------ config ---------------------------------- */
typedef struct {
    const char *info_file;
    const char *out_file;
    double      interval;
    int         bar_width;
    int         top;
    int         bottom;

    /* headers & optional lines */
    const char *osd_hdr;
    const char *osd_hdr2;
    const char *sys_msg_hdr;
    char        system_msg[256];  /* system message (dynamic) */
    bool        show_stats_line;
    int         sys_msg_timeout;  /* timeout in seconds */

    /* RSSI‑controlled header */
    bool        rssi_control;
    const char *rssi_hdr[6];

    /* misc */
    const char *ping_ip;
    const char *start_sym;
    const char *end_sym;
    const char *empty_sym;
} cfg_t;

/* -------------------- global instance with defaults --------------------- */
static cfg_t cfg = {
    .info_file        = DEF_INFO_FILE,
    .out_file         = DEF_OUT_FILE,
    .interval         = DEF_INTERVAL,
    .bar_width        = DEF_BAR_WIDTH,
    .top              = DEF_TOP,
    .bottom           = DEF_BOTTOM,

    .osd_hdr          = DEF_OSD_HDR,
    .osd_hdr2         = DEF_OSD_HDR2,
    .sys_msg_hdr      = DEF_SYS_MSG_HDR,
    .system_msg       = "",
    .show_stats_line  = DEF_SHOW_STATS,
    .sys_msg_timeout  = DEF_SYS_MSG_TIMEOUT,

    .rssi_control     = DEF_RSSI_CONTROL,
    .rssi_hdr         = { DEF_RSSI_RANGE0, DEF_RSSI_RANGE1, DEF_RSSI_RANGE2,
                          DEF_RSSI_RANGE3, DEF_RSSI_RANGE4, DEF_RSSI_RANGE5 },

    .ping_ip          = DEF_PING_IP,
    .start_sym        = DEF_START,
    .end_sym          = DEF_END,
    .empty_sym        = DEF_EMPTY
};

/* ------------ live reload (SIGHUP) -------------------------------------- */
static volatile sig_atomic_t reload_cfg = 0;
static const char            *cfg_path = DEF_CFG_FILE;
static void hup_handler(int s){ (void)s; reload_cfg = 1; }

/* ------------------------- config parsing -------------------------------- */
static void set_cfg_field(const char *k,const char *v)
{
#define EQ(a,b) (strcmp((a),(b))==0)
    if      (EQ(k,"info_file"))         cfg.info_file  = strdup(v);
    else if (EQ(k,"out_file"))          cfg.out_file   = strdup(v);
    else if (EQ(k,"interval"))          cfg.interval   = atof(v);
    else if (EQ(k,"bar_width"))         cfg.bar_width  = atoi(v);
    else if (EQ(k,"top"))               cfg.top        = atoi(v);
    else if (EQ(k,"bottom"))            cfg.bottom     = atoi(v);

    else if (EQ(k,"osd_hdr"))           cfg.osd_hdr    = strdup(v);
    else if (EQ(k,"osd_hdr2"))          cfg.osd_hdr2   = strdup(v);
    else if (EQ(k,"sys_msg_hdr"))       cfg.sys_msg_hdr= strdup(v);
    else if (EQ(k,"show_stats_line"))   cfg.show_stats_line = atoi(v)!=0;
    else if (EQ(k,"sys_msg_timeout"))   cfg.sys_msg_timeout = atoi(v);

    else if (EQ(k,"rssi_control"))      cfg.rssi_control = atoi(v)!=0;
    else if (EQ(k,"rssi_range0_hdr"))   cfg.rssi_hdr[0] = strdup(v);
    else if (EQ(k,"rssi_range1_hdr"))   cfg.rssi_hdr[1] = strdup(v);
    else if (EQ(k,"rssi_range2_hdr"))   cfg.rssi_hdr[2] = strdup(v);
    else if (EQ(k,"rssi_range3_hdr"))   cfg.rssi_hdr[3] = strdup(v);
    else if (EQ(k,"rssi_range4_hdr"))   cfg.rssi_hdr[4] = strdup(v);
    else if (EQ(k,"rssi_range5_hdr"))   cfg.rssi_hdr[5] = strdup(v);

    else if (EQ(k,"ping_ip"))           cfg.ping_ip    = strdup(v);
    else if (EQ(k,"start_sym"))         cfg.start_sym  = strdup(v);
    else if (EQ(k,"end_sym"))           cfg.end_sym    = strdup(v);
    else if (EQ(k,"empty_sym"))         cfg.empty_sym  = strdup(v);
#undef EQ
}

static void load_config(const char *path)
{
    FILE *fp = fopen(path,"r");
    if(!fp){ fprintf(stderr,"[antenna_osd] config \"%s\" not found – defaults in use\n",path); return; }

    char *line=NULL; size_t len=0;
    while(getline(&line,&len,fp)!=-1){
        char *s=line; while(*s==' '||*s=='\t') ++s;
        if(*s=='#'||*s=='\n'||*s=='\0') continue;
        char *eq=strchr(s,'='); if(!eq) continue; *eq='\0';
        char *k=s,*v=eq+1;
        char *ke=k+strlen(k); while(ke>k&&(ke[-1]==' '||ke[-1]=='\t')) *--ke='\0';
        while(*v==' '||*v=='\t') ++v;
        char *ve=v+strlen(v); while(ve>v&&(ve[-1]==' '||ve[-1]=='\t'||ve[-1]=='\n')) *--ve='\0';
        set_cfg_field(k,v);
    }
    free(line); fclose(fp);
}

/* ---------------------- System Message Handling ------------------------- */
static time_t sys_msg_last_update = 0;

static void read_system_msg(void) {
    struct stat st;
    if (stat(SYS_MSG_FILE, &st) == 0) {
        if (st.st_mtime != sys_msg_last_update) {
            FILE *fp = fopen(SYS_MSG_FILE, "r");
            if (fp) {
                if (fgets(cfg.system_msg, sizeof(cfg.system_msg), fp)) {
                    char *p = strchr(cfg.system_msg, '\n');
                    if (p) *p = '\0';
                }
                fclose(fp);
                sys_msg_last_update = st.st_mtime;
            }
        }
    } else {
        cfg.system_msg[0] = '\0'; // File missing
    }

    // Timeout logic
    time_t now = time(NULL);
    if (cfg.system_msg[0] && (now - sys_msg_last_update > cfg.sys_msg_timeout)) {
        cfg.system_msg[0] = '\0';  // Clear message
    }
}

/* ----------------------------- helpers ---------------------------------- */
static uint16_t icmp_cksum(const void *d,size_t l){
    const uint8_t *p=d; uint32_t s=0; while(l>1){uint16_t w; memcpy(&w,p,2); s+=w; p+=2; l-=2;} if(l) s+=*p;
    s=(s>>16)+(s&0xFFFF); s+=(s>>16); return (uint16_t)~s;
}

static int init_icmp_socket(const char *ip, struct sockaddr_in *dst){
    if(!ip||!*ip) return -1;
    int s=socket(AF_INET,SOCK_RAW,IPPROTO_ICMP); if(s<0){perror("socket"); return -1;}
    memset(dst,0,sizeof(*dst)); dst->sin_family=AF_INET;
    if(inet_pton(AF_INET,ip,&dst->sin_addr)!=1){fprintf(stderr,"Invalid ping_ip \"%s\"\n",ip); close(s); return -1;}
    return s;
}

static int send_icmp_echo(int s,struct sockaddr_in *dst,uint16_t seq){
    struct {struct icmphdr h; char p[56];} pkt={0};
    pkt.h.type=ICMP_ECHO; pkt.h.un.echo.id=htons(getpid()&0xFFFF); pkt.h.un.echo.sequence=htons(seq);
    for(size_t i=0;i<sizeof(pkt.p);++i) pkt.p[i]=(char)i;
    pkt.h.checksum=icmp_cksum(&pkt,sizeof(pkt));
    return sendto(s,&pkt,sizeof(pkt),0,(struct sockaddr*)dst,sizeof(*dst));
}

static int read_rssi(const char *path){
    FILE *fp=fopen(path,"r"); if(!fp) return -1;
    char *l=NULL; size_t n=0; int v=-1;
    while(getline(&l,&n,fp)!=-1){ char *p=strstr(l,"rssi"); if(!p) continue;
        if(sscanf(p,"rssi : %d",&v)==1 || sscanf(p,"rssi: %d",&v)==1) break; }
    free(l); fclose(fp); return v;
}

static void build_bar(char *o,size_t sz,int pct){
    int eig=pct*cfg.bar_width*8/100, full=eig/8, rem=eig%8; size_t pos=0;
    for(int i=0;i<cfg.bar_width;++i){ const char *sym;
        if(i<full) sym=FULL;
        else if(i==full&&rem){ sym=PART[rem-1]; rem=0; }
        else sym=cfg.empty_sym;
        size_t L=strlen(sym); if(pos+L<sz){ memcpy(o+pos,sym,L); pos+=L; }
    }
    o[pos]='\0';
}

static inline const char *choose_rssi_hdr(int pct)
{
    if(!cfg.rssi_control) return cfg.osd_hdr;
    int idx = (pct * 6) / 100;
    if(idx > 5) idx = 5;
    return cfg.rssi_hdr[idx];
}

static void write_osd(int rssi)
{
    /* 1) compute percentage */
    int pct;
    if      (rssi <= cfg.bottom) pct = 0;
    else if (rssi >= cfg.top)    pct = 100;
    else                          pct = (rssi - cfg.bottom) * 100 / (cfg.top - cfg.bottom);

    /* 2) build bar */
    char bar[cfg.bar_width*3 + 1];
    build_bar(bar, sizeof(bar), pct);

    /* 3) pick header */
    const char *hdr = choose_rssi_hdr(pct);

    /* 4) assemble into one buffer, using "\\n" for literal backslash‑n */
    char msg[1024];
    int len = snprintf(msg, sizeof(msg),
        "%s%3d%% %s%s%s\\n"        /* first line + “\n” */
        "%sTEMP: &TC/&WC | &B | CPU: &C",  /* second line */
        hdr, pct, cfg.start_sym, bar, cfg.end_sym,
        cfg.osd_hdr2
    );

    /* 5) append system message if present */
    if (cfg.system_msg[0]) {
        len += snprintf(msg + len, sizeof(msg) - len,
                        "\\n%s%s",         /* literal “\n” + sys_msg_hdr + system_msg */
                        cfg.sys_msg_hdr, cfg.system_msg);
    }

    /* 6) print the echo command (with embedded \n) To be added as a deub argument later.
    printf("echo \"%s\" > %s\n", msg, cfg.out_file); */

    /* 7) write the same raw buffer (with backslash‑n) to the OSD file */
    FILE *fp = fopen(cfg.out_file, "w");
    if (!fp) {
        perror("fopen");
        return;
    }
    fwrite(msg, 1, len, fp);
    fclose(fp);
}



/* ----------------------------- main ------------------------------------- */
int main(int argc,char **argv)
{
    if(getuid()!=0){ fprintf(stderr,"rssi_bar: run as root (raw sockets)\n"); return 1; }

    static const struct option optv[]={
        {"config", required_argument, NULL,'c'},
        {"help",   no_argument,       NULL,'h'},
        {0,0,0,0}};
    int opt;
    while((opt=getopt_long(argc,argv,"c:h",optv,NULL))!=-1){
        if(opt=='c') cfg_path = optarg;
        else { printf("Usage: %s [--config <file>]\n",argv[0]); return 0; }
    }

    load_config(cfg_path);

    struct sigaction sa={.sa_handler=hup_handler};
    sigemptyset(&sa.sa_mask); sa.sa_flags=SA_RESTART;
    sigaction(SIGHUP,&sa,NULL);

    struct sockaddr_in dst;
    int sock = init_icmp_socket(cfg.ping_ip,&dst);
    bool ping_en = (sock >= 0);
    uint16_t seq = 0;

    double ping_int = cfg.interval / 3.0;
    struct timespec ts={.tv_sec=(time_t)ping_int,
                        .tv_nsec=(long)((ping_int-(time_t)ping_int)*1e9)};
    int cnt=0;

    while(true){
        if(ping_en) send_icmp_echo(sock,&dst,seq++);

        read_system_msg();

        if(++cnt==3){
            cnt=0;
            int r=read_rssi(cfg.info_file);
            if(r<0) r=0;
            write_osd(r);
        }

        nanosleep(&ts,NULL);

        if(reload_cfg){
            reload_cfg=0;
            fprintf(stderr,"[antenna_osd] reload %s\n",cfg_path);
            load_config(cfg_path);
        }
    }
}
