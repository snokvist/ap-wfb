/*
 * rssi_bar.c – RSSI bar OSD + background ICMP traffic generator
 * -------------------------------------------------------------------------
 * Build :   gcc -O2 -std=c11 -Wall -o rssi_bar rssi_bar.c
 * Run  :    sudo ./rssi_bar [--config <file>]
 * The program needs root for raw ICMP sockets.
 * -------------------------------------------------------------------------
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
#include <time.h>
#include <unistd.h>

/* ----------------------------- defaults --------------------------------- */
#define DEF_CFG_FILE     "/etc/antennaosd.conf"
#define DEF_INFO_FILE    "/proc/net/rtl88x2eu/wlan0/trx_info_debug"
#define DEF_OUT_FILE     "/tmp/MSPOSD.msg"
#define DEF_INTERVAL      0.1
#define DEF_BAR_WIDTH     37
#define DEF_TOP           80
#define DEF_BOTTOM        20
#define DEF_OSD_HDR       " &F34&L20"   /* header for first line            */
#define DEF_OSD_HDR2      ""            /* header for stats line            */
#define DEF_SYS_MSG_HDR   ""            /* header for third line            */
#define DEF_SYSTEM_MSG    ""            /* third-line text                  */
#define DEF_PING_IP       "192.168.0.10"
#define DEF_START         "["
#define DEF_END           "]"
#define DEF_EMPTY         "."
#define DEF_SHOW_STATS    1             /* 0 = hide stats line              */

/* ------------------------------ glyphs ---------------------------------- */
static const char *GL_ANT  = "\uF012";                 /*   */
static const char *FULL    = "\u2588";                 /* █  */
static const char *PART[7] = { "\u2581","\u2582","\u2583","\u2584",
                               "\u2585","\u2586","\u2587" };          /* ▁▂▃▄▅▆▇ */

/* ------------------------------ config ---------------------------------- */
typedef struct {
    const char *info_file;
    const char *out_file;
    double      interval;
    int         bar_width;
    int         top;
    int         bottom;
    const char *osd_hdr;       /* first line */
    const char *osd_hdr2;      /* stats line */
    const char *sys_msg_hdr;   /* third line header */
    const char *system_msg;    /* third line text   */
    const char *ping_ip;       /* empty ⇒ disable pings */
    const char *start_sym;
    const char *end_sym;
    const char *empty_sym;
    bool        show_stats_line;
} cfg_t;

/* global instance with defaults */
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
    .system_msg       = DEF_SYSTEM_MSG,
    .ping_ip          = DEF_PING_IP,
    .start_sym        = DEF_START,
    .end_sym          = DEF_END,
    .empty_sym        = DEF_EMPTY,
    .show_stats_line  = DEF_SHOW_STATS
};

/* ------------ live reload (SIGHUP) -------------------------------------- */
static volatile sig_atomic_t reload_cfg = 0;
static const char            *cfg_path = DEF_CFG_FILE;
static void hup_handler(int sig) { (void)sig; reload_cfg = 1; }

/* ------------------------- config parsing -------------------------------- */
static void set_cfg_field(const char *k, const char *v)
{
#define EQ(a,b) (strcmp((a),(b))==0)
    if      (EQ(k,"info_file"))        cfg.info_file       = strdup(v);
    else if (EQ(k,"out_file"))         cfg.out_file        = strdup(v);
    else if (EQ(k,"interval"))         cfg.interval        = atof(v);
    else if (EQ(k,"bar_width"))        cfg.bar_width       = atoi(v);
    else if (EQ(k,"top"))              cfg.top             = atoi(v);
    else if (EQ(k,"bottom"))           cfg.bottom          = atoi(v);
    else if (EQ(k,"osd_hdr"))          cfg.osd_hdr         = strdup(v);
    else if (EQ(k,"osd_hdr2"))         cfg.osd_hdr2        = strdup(v);
    else if (EQ(k,"sys_msg_hdr"))      cfg.sys_msg_hdr     = strdup(v);
    else if (EQ(k,"system_msg"))       cfg.system_msg      = strdup(v);
    else if (EQ(k,"ping_ip"))          cfg.ping_ip         = strdup(v);
    else if (EQ(k,"start_sym"))        cfg.start_sym       = strdup(v);
    else if (EQ(k,"end_sym"))          cfg.end_sym         = strdup(v);
    else if (EQ(k,"empty_sym"))        cfg.empty_sym       = strdup(v);
    else if (EQ(k,"show_stats_line"))  cfg.show_stats_line = atoi(v) != 0;
#undef EQ
}

static void load_config(const char *path)
{
    FILE *fp = fopen(path, "r");
    if (!fp) {
        fprintf(stderr, "[antenna_osd] config \"%s\" not found – defaults in use\n",
                path);
        return;
    }

    char *line = NULL;
    size_t len = 0;

    while (getline(&line, &len, fp) != -1) {
        char *s = line;
        while (*s == ' ' || *s == '\t') ++s;          /* leading ws   */
        if (*s == '#' || *s == '\n' || *s == '\0')
            continue;                                /* comment/blank */

        char *eq = strchr(s, '=');
        if (!eq) continue;
        *eq = '\0';

        char *key = s;
        char *val = eq + 1;

        /* trim rhs of key */
        char *end = key + strlen(key);
        while (end > key && (end[-1] == ' ' || end[-1] == '\t')) *--end = '\0';

        /* trim lhs/trailing ws of value */
        while (*val == ' ' || *val == '\t') ++val;
        char *ve = val + strlen(val);
        while (ve > val && (ve[-1] == ' ' || ve[-1] == '\t' || ve[-1] == '\n'))
            *--ve = '\0';

        set_cfg_field(key, val);
    }
    free(line);
    fclose(fp);
}

/* ----------------------------- helpers ---------------------------------- */
static uint16_t icmp_cksum(const void *d, size_t l)
{
    const uint8_t *p = d;
    uint32_t sum = 0;
    while (l > 1) {
        uint16_t w;
        memcpy(&w, p, 2);            /* safe for unaligned addr */
        sum += w;
        p += 2;
        l -= 2;
    }
    if (l) sum += *p;
    sum = (sum >> 16) + (sum & 0xFFFF);
    sum += (sum >> 16);
    return (uint16_t)~sum;
}

static int init_icmp_socket(const char *ip, struct sockaddr_in *dst)
{
    if (!ip || !*ip) return -1;                  /* pings disabled */

    int s = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
    if (s < 0) {
        perror("socket");
        return -1;
    }

    memset(dst, 0, sizeof(*dst));
    dst->sin_family = AF_INET;
    if (inet_pton(AF_INET, ip, &dst->sin_addr) != 1) {
        fprintf(stderr, "Invalid ping_ip \"%s\"\n", ip);
        close(s);
        return -1;
    }
    return s;
}

static int send_icmp_echo(int s, struct sockaddr_in *dst, uint16_t seq)
{
    struct { struct icmphdr h; char payload[56]; } pkt = {0};

    pkt.h.type              = ICMP_ECHO;
    pkt.h.un.echo.id        = htons(getpid() & 0xFFFF);
    pkt.h.un.echo.sequence  = htons(seq);
    for (size_t i = 0; i < sizeof(pkt.payload); ++i)
        pkt.payload[i] = (char)i;

    pkt.h.checksum = icmp_cksum(&pkt, sizeof(pkt));

    return sendto(s, &pkt, sizeof(pkt), 0,
                  (struct sockaddr *)dst, sizeof(*dst));
}

static int read_rssi(const char *path)
{
    FILE *fp = fopen(path, "r");
    if (!fp) return -1;

    char *l = NULL;
    size_t n = 0;
    int val = -1;

    while (getline(&l, &n, fp) != -1) {
        char *p = strstr(l, "rssi");
        if (!p) continue;
        if (sscanf(p, "rssi : %d", &val) == 1 ||
            sscanf(p, "rssi: %d",  &val) == 1)
            break;
    }
    free(l);
    fclose(fp);
    return val;
}

static void build_bar(char *out, size_t outsz, int pct)
{
    int eig   = pct * cfg.bar_width * 8 / 100;   /* “pixel eighths” */
    int fulls = eig / 8;
    int rem   = eig % 8;
    size_t pos = 0;

    for (int i = 0; i < cfg.bar_width; ++i) {
        const char *sym;
        if (i < fulls)
            sym = FULL;
        else if (i == fulls && rem) {
            sym = PART[rem - 1];
            rem = 0;
        } else
            sym = cfg.empty_sym;

        size_t l = strlen(sym);
        if (pos + l < outsz) {
            memcpy(out + pos, sym, l);
            pos += l;
        }
    }
    out[pos] = '\0';
}

static void write_osd(int rssi)
{
    FILE *fp = fopen(cfg.out_file, "w");
    if (!fp) { perror("fopen"); return; }

    int pct;
    if (rssi <= cfg.bottom)      pct = 0;
    else if (rssi >= cfg.top)    pct = 100;
    else                         pct = (rssi - cfg.bottom) * 100 / (cfg.top - cfg.bottom);

    char bar[cfg.bar_width * 3 + 1];
    build_bar(bar, sizeof(bar), pct);

    /* ---- line 1 : antenna + bar */
    fprintf(fp, "%s%s %s%s%s %d%%\n",
            cfg.osd_hdr, GL_ANT, cfg.start_sym, bar, cfg.end_sym, rssi);

    /* ---- line 2 : diagnostics (optional) */
    if (cfg.show_stats_line)
        fprintf(fp, "%sTEMP: &TC/&WC | STATS: &B | CPU: &C\n", cfg.osd_hdr2);

    /* ---- line 3 : system message (optional) */
    if (cfg.system_msg && *cfg.system_msg)
        fprintf(fp, "%s%s\n", cfg.sys_msg_hdr, cfg.system_msg);

    fclose(fp);
}

/* ----------------------------- main ------------------------------------- */
int main(int argc, char **argv)
{
    if (getuid() != 0) {
        fprintf(stderr, "rssi_bar: must run as root (raw sockets)\n");
        return 1;
    }

    /* single CLI flag: --config */
    static const struct option opts[] = {
        { "config", required_argument, NULL, 'c' },
        { "help",   no_argument,       NULL, 'h' },
        { 0, 0, 0, 0 } };

    int opt;
    while ((opt = getopt_long(argc, argv, "c:h", opts, NULL)) != -1) {
        if (opt == 'c')
            cfg_path = optarg;
        else {
            printf("Usage: %s [--config <file>]\n", argv[0]);
            return 0;
        }
    }

    load_config(cfg_path);

    /* SIGHUP handler for live reloads */
    struct sigaction sa = { .sa_handler = hup_handler };
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sigaction(SIGHUP, &sa, NULL);

    /* ICMP socket (optional) */
    struct sockaddr_in dst;
    int  sock     = init_icmp_socket(cfg.ping_ip, &dst);
    bool ping_en  = (sock >= 0);
    uint16_t seq  = 0;

    /* timing: pings 3× faster than OSD refresh */
    double ping_int = cfg.interval / 3.0;
    struct timespec ts = {
        .tv_sec  = (time_t)ping_int,
        .tv_nsec = (long)((ping_int - (time_t)ping_int) * 1e9)
    };

    int ping_count = 0;

    /* ---------------- main loop ---------------- */
    while (true) {
        if (ping_en)
            send_icmp_echo(sock, &dst, seq++);

        if (++ping_count == 3) {
            ping_count = 0;
            int r = read_rssi(cfg.info_file);
            if (r < 0) r = 0;
            write_osd(r);
        }

        nanosleep(&ts, NULL);

        /* apply updates after a SIGHUP */
        if (reload_cfg) {
            reload_cfg = 0;
            fprintf(stderr, "[antenna_osd] reloading %s\n", cfg_path);
            load_config(cfg_path);
        }
    }
}
