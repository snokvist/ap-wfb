
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
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* ----------------------------- defaults --------------------------------- */
static int last_valid_rssi      = 0;
static int neg1_count_rssi      = 0;
static int last_valid_udp       = 0;
static int neg1_count_udp       = 0;
static char *info_buf   = NULL;
static size_t info_size = 0;
static time_t last_info_attempt = 0;
static bool   info_buf_valid   = false;
static int rssi_hist[3] = { -1, -1, -1 };
static int udp_hist[3]  = { -1, -1, -1 };

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

#define DEF_RSSI_KEY       "rssi"
#define DEF_CURR_TX_RATE_KEY  "curr_tx_rate"
#define DEF_CURR_TX_BW_KEY    "curr_tx_bw"
#define DEF_RSSI_UDP_ENABLE  0
#define DEF_RSSI_UDP_KEY     "rssi_udp"
#define DEF_TX_POWER_KEY   "tx_power"

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
    const char *rssi_key;
    const char *curr_tx_rate_key;
    const char *curr_tx_bw_key;
    bool        rssi_udp_enable;   /* NEW: show second bar? */
    const char *rssi_udp_key;      /* NEW: what prefix to look for */
    const char *tx_power_key;

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
    .empty_sym        = DEF_EMPTY,
    .curr_tx_rate_key = DEF_CURR_TX_RATE_KEY,
    .curr_tx_bw_key   = DEF_CURR_TX_BW_KEY,
    .rssi_udp_enable = DEF_RSSI_UDP_ENABLE,
    .rssi_udp_key    = DEF_RSSI_UDP_KEY,
    .tx_power_key    = DEF_TX_POWER_KEY,
    .rssi_key        = DEF_RSSI_KEY

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
    else if (EQ(k,"rssi_key"))        cfg.rssi_key      = strdup(v);
    else if (EQ(k,"curr_tx_rate_key"))  cfg.curr_tx_rate_key = strdup(v);
    else if (EQ(k,"curr_tx_bw_key"))    cfg.curr_tx_bw_key   = strdup(v);
    else if (EQ(k,"rssi_udp_enable")) cfg.rssi_udp_enable = atoi(v)!=0;
    else if (EQ(k,"rssi_udp_key"))    cfg.rssi_udp_key    = strdup(v);
    else if (EQ(k,"tx_power_key"))     cfg.tx_power_key = strdup(v);

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


static int smooth_rssi_sample(int *hist, int newval)
{
    if (newval < 0) return newval; // do not smooth invalid values

    // shift history
    hist[2] = hist[1];
    hist[1] = hist[0];
    hist[0] = newval;

    if (hist[1] < 0 || hist[2] < 0)
        return newval; // not enough history yet

    return (int)(0.5 * hist[0] + 0.25 * hist[1] + 0.25 * hist[2]);
}


static int get_display_rssi(int raw)
{
    if (raw >= 0) {
        last_valid_rssi = raw;
        neg1_count_rssi = 0;
        return raw;
    }
    /* raw == –1 */
    if (++neg1_count_rssi >= 3) {
        /* now give up and show –1 */
        return -1;
    }
    /* still in grace period: show last valid */
    return last_valid_rssi;
}

/**
 * Read cfg.info_file into info_buf (null-terminated).
 * Returns true on success, false on error (buffer is left empty).
 */
static bool load_info_buffer(void)
{
    FILE *fp = fopen(cfg.info_file, "r");
    if (!fp) return false;

    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    rewind(fp);

    free(info_buf);
    info_buf = malloc(sz + 1);
    if (!info_buf) { fclose(fp); return false; }

    size_t got = fread(info_buf, 1, sz, fp);
    fclose(fp);

    info_buf[got] = '\0';
    info_size    = got;
    return true;
}



 /**
  * Try to load cfg.info_file, but only once every 3 seconds UNTIL we succeed.
  * Returns true if we have ever successfully loaded the file.
  */
 static bool try_initial_load_info(void) {
     time_t now = time(NULL);
     if (info_buf_valid) {
         return true;
     }
     if (now - last_info_attempt < 3) {
         return false;
     }
     last_info_attempt = now;
     if (load_info_buffer()) {
         info_buf_valid = true;
         return true;
     }
     return false;
 }


static int get_display_udp(int raw)
{
    if (raw >= 0) {
        last_valid_udp = raw;
        neg1_count_udp = 0;
        return raw;
    }
    if (++neg1_count_udp >= 3) {
        return -1;
    }
    return last_valid_udp;
}






/**
 * Find “key(:|=)NUM” in info_buf, return NUM or -1.
 */
static int parse_int_from_buf(const char *buf, const char *key)
{
    const char *p = buf;
    while ((p = strcasestr(p, key)) != NULL) {
        const char *sep = strchr(p, ':');
        if (!sep) sep = strchr(p, '=');
        if (sep) {
            sep++;
            while (*sep==' '||*sep=='\t') sep++;
            return (int)strtol(sep, NULL, 10);
        }
        p += strlen(key);
    }
    return -1;
}

/**
 * Find “key(:|=)VALUE…\n” and copy VALUE into out, else “NA”.
 */
static void parse_value_from_buf(const char *buf,
                                 const char *key,
                                 char       *out,
                                 size_t      outlen)
{
    const char *p = buf;
    while ((p = strcasestr(p, key)) != NULL) {
        const char *sep = strchr(p, ':');
        if (!sep) sep = strchr(p, '=');
        if (!sep) { p += strlen(key); continue; }
        sep++;
        while (*sep==' '||*sep=='\t') sep++;
        const char *end = sep;
        while (*end && *end!='\n' && *end!='\r') end++;
        size_t len = end - sep;
        if (len >= outlen) len = outlen - 1;
        memcpy(out, sep, len);
        out[len] = '\0';
        return;
    }
    /* not found */
    snprintf(out, outlen, "NA");
}



static int init_icmp_socket(const char *ip, struct sockaddr_in *dst) {
    if (!ip || !*ip) return -1;

    int s = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
    if (s < 0) {
        perror("socket");
        return -1;
    }

    memset(dst, 0, sizeof(*dst));
    dst->sin_family = AF_INET;
    if (inet_pton(AF_INET, ip, &dst->sin_addr) != 1) {
        fprintf(stderr, "[warning] invalid ping_ip \"%s\", ICMP disabled\n", ip);
        close(s);
        return -1;
    }
    return s;
}

static int send_icmp_echo(int s,struct sockaddr_in *dst,uint16_t seq){
    struct {struct icmphdr h; char p[56];} pkt={0};
    pkt.h.type=ICMP_ECHO; pkt.h.un.echo.id=htons(getpid()&0xFFFF); pkt.h.un.echo.sequence=htons(seq);
    for(size_t i=0;i<sizeof(pkt.p);++i) pkt.p[i]=(char)i;
    pkt.h.checksum=icmp_cksum(&pkt,sizeof(pkt));
    return sendto(s,&pkt,sizeof(pkt),0,(struct sockaddr*)dst,sizeof(*dst));
}

static int read_rssi(const char *path)
{
    FILE *fp = fopen(path, "r");
    if (!fp) return -1;

    char *line = NULL;
    size_t len = 0;
    int  value = -1;

    while (getline(&line, &len, fp) != -1) {
        char *p;

        /* 1) try the configured key (case‑insensitive) */
        p = strcasestr(line, cfg.rssi_key);
        if (p) {
            char *eq = strchr(p, '=');
            if (eq) {
                eq++;
                /* skip whitespace */
                while (*eq == ' ' || *eq == '\t') eq++;
                /* atoi()/strtol will stop at the first non‑digit */
                value = (int)strtol(eq, NULL, 10);
                break;
            }
        }

        /* 2) fallback old-style parsing (lower‑case “rssi : <n>” or “rssi:<n>”) */
        if (sscanf(line, "rssi : %d", &value) == 1 ||
            sscanf(line, "rssi: %d",  &value) == 1) {
            break;
        }
    }

    free(line);
    fclose(fp);
    return value;
}

static void build_bar(char *o, size_t sz, int pct)
{
    /* 1) clamp pct to [0..100] */
    if      (pct <   0) pct =   0;
    else if (pct > 100) pct = 100;

    /* 2) how many “eighths” of a block we need */
    int total_eighths = pct * cfg.bar_width * 8 / 100;
    int full_blocks   = total_eighths / 8;
    int rem_eighths   = total_eighths % 8;

    /* 3) safety: never exceed bar_width */
    if (full_blocks > cfg.bar_width) {
        full_blocks = cfg.bar_width;
        rem_eighths = 0;
    }

    /* 4) build exactly bar_width symbols */
    size_t pos = 0;
    for (int i = 0; i < cfg.bar_width; ++i) {
        const char *sym = cfg.empty_sym;

        if (i < full_blocks) {
            sym = FULL;
        } else if (i == full_blocks && rem_eighths > 0) {
            sym = PART[rem_eighths - 1];
        }

        size_t L = strlen(sym);
        if (pos + L < sz) {
            memcpy(o + pos, sym, L);
            pos += L;
        }
    }
    o[pos] = '\0';
}

static inline const char *choose_rssi_hdr(int pct)
{
    if(!cfg.rssi_control) return cfg.osd_hdr;
    int idx = (pct * 6) / 100;
    if(idx > 5) idx = 5;
    return cfg.rssi_hdr[idx];
}

static void write_osd(int rssi,
                      int udp_rssi,
                      const char *mcs_str,
                      const char *bw_str,
                      const char *tx_str)
{
    /* 1) compute main RSSI percentage */
    int pct;
    if      (rssi < 0               ) pct = 0;
    else if (rssi <= cfg.bottom    ) pct = 0;
    else if (rssi >= cfg.top       ) pct = 100;
    else                               pct = (rssi - cfg.bottom) * 100 / (cfg.top - cfg.bottom);

    /* 2) build main bar */
    char bar[cfg.bar_width * 3 + 1];
    build_bar(bar, sizeof(bar), pct);
    const char *hdr = choose_rssi_hdr(pct);

    /* 3) optional UDP‑RSSI bar */
    int pct_udp = 0;
    char bar_udp[cfg.bar_width * 3 + 1];
    const char *hdr_udp = NULL;
    if (cfg.rssi_udp_enable) {
        int disp_udp = udp_rssi;
        if      (disp_udp < 0            ) pct_udp = 0;
        else if (disp_udp <= cfg.bottom ) pct_udp = 0;
        else if (disp_udp >= cfg.top    ) pct_udp = 100;
        else                               pct_udp = (disp_udp - cfg.bottom) * 100 / (cfg.top - cfg.bottom);

        build_bar(bar_udp, sizeof(bar_udp), pct_udp);
        hdr_udp = choose_rssi_hdr(pct_udp);
    }

    /* 4) assemble the file buffer */
    char filebuf[2048];
    int  flen = 0;

    /* — main bar line — */
    flen += snprintf(filebuf + flen, sizeof(filebuf) - flen,
                     "%s %3d%% %s%s%s\n",
                     hdr, pct, cfg.start_sym, bar, cfg.end_sym);

    /* — UDP bar line if enabled — */
    if (cfg.rssi_udp_enable) {
        flen += snprintf(filebuf + flen, sizeof(filebuf) - flen,
                         "%s %3d%% %s%s%s\n",
                         hdr_udp, pct_udp, cfg.start_sym, bar_udp, cfg.end_sym);
    }
    /* — optional stats line with MCS / BW / TX_POWER — */
    if (cfg.show_stats_line) {
        flen += snprintf(filebuf + flen, sizeof(filebuf) - flen,
                         "%sTEMP: &TC/&WC | CPU: &C | %s / %s / %s | &B\n",
                         cfg.osd_hdr2,
                         mcs_str, bw_str, tx_str);
    }

    /* — optional system message — */
    if (cfg.system_msg[0]) {
        flen += snprintf(filebuf + flen, sizeof(filebuf) - flen,
                         "%s%s\n", cfg.sys_msg_hdr, cfg.system_msg);
    }


    /* 5) build a debug buffer escaping newlines as “\\n”
    char dbgbuf[4096];
    char *p = filebuf;
    char *q = dbgbuf;
    while (p < filebuf + flen && (q - dbgbuf) < (int)sizeof(dbgbuf) - 2) {
        if (*p == '\n') {
            *q++ = '\\';
            *q++ = 'n';
        } else {
            *q++ = *p;
        }
        p++;
    }
    *q = '\0';

    /* (optional) debug print:*/
     /*  printf("echo -e \"%s\" > %s\n", dbgbuf, cfg.out_file);*/


    /* 6) write out */
    FILE *fp = fopen(cfg.out_file, "w");
    if (!fp) { perror("fopen"); return; }
    fwrite(filebuf, 1, flen, fp);
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

    while (true) {
        if (ping_en) send_icmp_echo(sock, &dst, seq++);

        read_system_msg();

        if (++cnt == 3) {
            cnt = 0;

    /* 1) ensure we’ve ever loaded the info file at least once */
    if (!try_initial_load_info()) {
        continue;  // still in back‑off / no data yet
    }

    /* 2) from now on, read it every cycle (≈0.1s) */
    if (!load_info_buffer()) {
        // if a sudden read error happens, mark invalid and restart back‑off
        info_buf_valid = false;
        last_info_attempt = time(NULL);
        continue;
    }
        /* 2) now `info_buf` is guaranteed non-NULL: */
        int raw_rssi  = parse_int_from_buf(info_buf, cfg.rssi_key);



            int raw_udp   = cfg.rssi_udp_enable
                            ? parse_int_from_buf(info_buf, cfg.rssi_udp_key)
                            : -1;

            char mcs_str[32], bw_str[32];
            parse_value_from_buf(info_buf, cfg.curr_tx_rate_key,
                                 mcs_str, sizeof(mcs_str));
            parse_value_from_buf(info_buf, cfg.curr_tx_bw_key,
                                 bw_str,   sizeof(bw_str));

            /* 3) apply your “–1 smoothing” on raw_rssi/raw_udp */
            int disp_rssi = get_display_rssi(raw_rssi);
            disp_rssi = smooth_rssi_sample(rssi_hist, disp_rssi);

            int disp_udp  = get_display_udp(raw_udp);
            disp_udp = smooth_rssi_sample(udp_hist, disp_udp);




            char tx_str[32];
            parse_value_from_buf(info_buf, cfg.tx_power_key,
                                tx_str, sizeof(tx_str));
            /* 4) hand disp_rssi into write_osd (which also
             *    builds the optional UDP bar from disp_udp) */
            write_osd(disp_rssi, disp_udp, mcs_str, bw_str, tx_str);
        }

        nanosleep(&ts, NULL);

        if (reload_cfg) {
            reload_cfg = 0;
            load_config(cfg_path);

            if (cfg.bar_width < 1) {
                cfg.bar_width = DEF_BAR_WIDTH;
            } else if (cfg.bar_width > 200) {
                cfg.bar_width = 200;
            }




        }
    }
}
