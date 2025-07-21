/*
 * rssi_bar.c – RSSI bar OSD + background ICMP traffic generator
 * -------------------------------------------------------------------------
 * Configuration‑file driven.  Only CLI flag:   --config <file>
 * If omitted, defaults to /etc/antennaosd.conf.  Missing keys fall back to
 * the compiled‑in defaults below.
 *
 * NEW (2025‑07‑21):
 *   •  show_stats_line=0|1   – omit or include the second diagnostics line.
 *   •  SIGHUP reloads the config at runtime.
 *
 * Compile :  gcc -O2 -std=c11 -Wall -o rssi_bar rssi_bar.c
 * Run     :  sudo ./rssi_bar [--config /path/to/file]
 * -------------------------------------------------------------------------
 */

#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
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
#define DEF_CFG_FILE  "/etc/antennaosd.conf"
#define DEF_INFO_FILE "/proc/net/rtl88x2eu/wlan0/trx_info_debug"
#define DEF_OUT_FILE  "/tmp/MSPOSD.msg"
#define DEF_INTERVAL   0.1
#define DEF_BAR_WIDTH  37
#define DEF_TOP        80
#define DEF_BOTTOM     20
#define DEF_OSD_HDR    " &F34&L20"
#define DEF_PING_IP    "192.168.0.10"
#define DEF_START      "["
#define DEF_END        "]"
#define DEF_EMPTY      "."
#define DEF_SHOW_STATS 1         /* 1 = print stats line, 0 = omit */

/* ------------------------------ glyphs ---------------------------------- */
static const char *GL_ANT  = "\uF012";                 /*  antenna */
static const char *FULL    = "\u2588";                 /* █ */
static const char *PART[7] = { "\u2581","\u2582","\u2583","\u2584",
                               "\u2585","\u2586","\u2587"};            /* ▁▂▃▄▅▆▇ */

/* ------------------------------ config ---------------------------------- */
typedef struct {
    const char *info_file;
    const char *out_file;
    double      interval;
    int         bar_width;
    int         top;
    int         bottom;
    const char *osd_hdr;
    const char *ping_ip;        /* empty ⇒ disable pings */
    const char *start_sym;
    const char *end_sym;
    const char *empty_sym;
    bool        show_stats_line;
} cfg_t;

static cfg_t cfg = {
    .info_file        = DEF_INFO_FILE,
    .out_file         = DEF_OUT_FILE,
    .interval         = DEF_INTERVAL,
    .bar_width        = DEF_BAR_WIDTH,
    .top              = DEF_TOP,
    .bottom           = DEF_BOTTOM,
    .osd_hdr          = DEF_OSD_HDR,
    .ping_ip          = DEF_PING_IP,
    .start_sym        = DEF_START,
    .end_sym          = DEF_END,
    .empty_sym        = DEF_EMPTY,
    .show_stats_line  = DEF_SHOW_STATS
};

/* ---------- live‑reload machinery --------------------------------------- */
static volatile sig_atomic_t cfg_reload_requested = 0;
static const char           *cfg_path_used       = DEF_CFG_FILE;

static void hup_handler(int signo)
{
    (void)signo;
    cfg_reload_requested = 1;
}

/* ------------------------- config parsing -------------------------------- */
static void set_cfg_field(const char *key, const char *val)
{
#define EQ(a,b) (strcmp((a),(b)) == 0)
    if      (EQ(key,"info_file"))        cfg.info_file        = strdup(val);
    else if (EQ(key,"out_file"))         cfg.out_file         = strdup(val);
    else if (EQ(key,"interval"))         cfg.interval         = atof(val);
    else if (EQ(key,"bar_width"))        cfg.bar_width        = atoi(val);
    else if (EQ(key,"top"))              cfg.top              = atoi(val);
    else if (EQ(key,"bottom"))           cfg.bottom           = atoi(val);
    else if (EQ(key,"osd_hdr"))          cfg.osd_hdr          = strdup(val);
    else if (EQ(key,"ping_ip"))          cfg.ping_ip          = strdup(val);
    else if (EQ(key,"start_sym"))        cfg.start_sym        = strdup(val);
    else if (EQ(key,"end_sym"))          cfg.end_sym          = strdup(val);
    else if (EQ(key,"empty_sym"))        cfg.empty_sym        = strdup(val);
    else if (EQ(key,"show_stats_line"))  cfg.show_stats_line  = atoi(val) != 0;
#undef EQ
}

static void load_config(const char *path)
{
    FILE *fp = fopen(path, "r");
    if (!fp) {
        fprintf(stderr,
                "[antenna_osd] config \"%s\" not found – using defaults.\n",
                path);
        return;
    }

    char *line = NULL;
    size_t len = 0;
    while (getline(&line,&len,fp) != -1) {
        /* strip leading spaces/tabs */
        char *s = line;
        while (*s == ' ' || *s == '\t') ++s;
        if (*s == '#' || *s == '\n' || *s == '\0') continue;   /* comment/blank */

        char *eq = strchr(s,'=');
        if (!eq) continue;
        *eq = '\0';
        char *key = s;
        char *val = eq + 1;

        /* trim trailing spaces on key */
        char *k_end = key + strlen(key);
        while (k_end > key && (k_end[-1] == ' ' || k_end[-1] == '\t'))
            *--k_end = '\0';

        /* trim leading spaces on val */
        while (*val == ' ' || *val == '\t') ++val;
        /* trim trailing whitespace/newlines on val */
        char *v_end = val + strlen(val);
        while (v_end > val &&
              (v_end[-1] == ' ' || v_end[-1] == '\t' || v_end[-1] == '\n'))
            *--v_end = '\0';

        set_cfg_field(key, val);
    }
    free(line);
    fclose(fp);
}

/* ----------------------------- helpers ---------------------------------- */
static uint16_t icmp_cksum(const void *data, size_t len)
{
    const uint8_t *ptr = data;
    uint32_t sum = 0;
    while (len > 1) {
        uint16_t word;
        memcpy(&word, ptr, 2);       /* safe for unaligned addr */
        sum += word;
        ptr += 2;
        len -= 2;
    }
    if (len) sum += *ptr;
    sum = (sum >> 16) + (sum & 0xFFFF);
    sum += (sum >> 16);
    return (uint16_t)~sum;
}

static int init_icmp_socket(const char *dst_ip, struct sockaddr_in *dst)
{
    if (!dst_ip || !*dst_ip) return -1;    /* pings disabled */

    int sock = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
    if (sock < 0) { perror("socket"); return -1; }

    memset(dst, 0, sizeof(*dst));
    dst->sin_family = AF_INET;
    if (inet_pton(AF_INET, dst_ip, &dst->sin_addr) != 1) {
        fprintf(stderr, "Invalid ping_ip \"%s\"\n", dst_ip);
        close(sock);
        return -1;
    }
    return sock;
}

static int send_icmp_echo(int sock, struct sockaddr_in *dst, uint16_t seq)
{
    struct { struct icmphdr hdr; char payload[56]; } pkt = {0};

    pkt.hdr.type        = ICMP_ECHO;
    pkt.hdr.un.echo.id  = htons(getpid() & 0xFFFF);
    pkt.hdr.un.echo.sequence = htons(seq);

    for (size_t i = 0; i < sizeof(pkt.payload); ++i)
        pkt.payload[i] = (char)i;

    pkt.hdr.checksum = icmp_cksum(&pkt, sizeof(pkt));

    return sendto(sock, &pkt, sizeof(pkt), 0,
                  (struct sockaddr *)dst, sizeof(*dst));
}

static int read_rssi(const char *path)
{
    FILE *fp = fopen(path, "r");
    if (!fp) return -1;

    char *l = NULL;
    size_t n = 0;
    int val = -1;

    while (getline(&l,&n,fp) != -1) {
        char *p = strstr(l, "rssi");
        if (!p) continue;
        if (sscanf(p, "rssi : %d", &val) == 1 ||
            sscanf(p, "rssi: %d",  &val) == 1) break;
    }
    free(l);
    fclose(fp);
    return val;
}

static void build_bar(char *out, size_t outsz, int pct)
{
    int eig   = pct * cfg.bar_width * 8 / 100;   /* eighth‑pixels */
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
    if (rssi <= cfg.bottom)           pct = 0;
    else if (rssi >= cfg.top)         pct = 100;
    else pct = (rssi - cfg.bottom) * 100 / (cfg.top - cfg.bottom);

    char bar[cfg.bar_width * 3 + 1];
    build_bar(bar, sizeof(bar), pct);

    /* line 1 */
    fprintf(fp, "%s%s %s%s%s %d%%\n",
            cfg.osd_hdr, GL_ANT, cfg.start_sym, bar, cfg.end_sym, rssi);

    /* optional line 2 */
    if (cfg.show_stats_line)
        fprintf(fp, "TEMP: &TC/&WC | STATS: &B | CPU: &C\n");

    fclose(fp);
}

/* ----------------------------- main ------------------------------------- */
int main(int argc, char **argv)
{
    if (getuid() != 0) {
        fprintf(stderr, "rssi_bar needs root (raw sockets).\n");
        return 1;
    }

    /* ---- option parsing (only --config) ---- */
    static const struct option long_opts[] = {
        { "config", required_argument, NULL, 'c' },
        { "help",   no_argument,       NULL, 'h' },
        { 0, 0, 0, 0 }
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "c:h", long_opts, NULL)) != -1) {
        if (opt == 'c')
            cfg_path_used = optarg;
        else {
            printf("Usage: %s [--config <file>]\n", argv[0]);
            return 0;
        }
    }

    /* initial load */
    load_config(cfg_path_used);

    /* install SIGHUP handler */
    struct sigaction sa = { .sa_handler = hup_handler };
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sigaction(SIGHUP, &sa, NULL);

    /* ICMP socket setup */
    struct sockaddr_in dst;
    int icmp_sock = init_icmp_socket(cfg.ping_ip, &dst);
    bool ping_enabled = (icmp_sock >= 0);
    uint16_t seq = 0;

    /* timing: pings 3× faster than OSD interval */
    double ping_int = cfg.interval / 3.0;
    struct timespec ts = {
        .tv_sec  = (time_t)ping_int,
        .tv_nsec = (long)((ping_int - (time_t)ping_int) * 1e9)
    };

    int ping_count = 0;

    while (1) {
        /* ---------------- main loop ---------------- */
        if (ping_enabled)
            send_icmp_echo(icmp_sock, &dst, seq++);

        if (++ping_count == 3) {
            ping_count = 0;
            int rssi = read_rssi(cfg.info_file);
            if (rssi < 0) rssi = 0;
            write_osd(rssi);
        }

        nanosleep(&ts, NULL);

        /* check if SIGHUP triggered */
        if (cfg_reload_requested) {
            cfg_reload_requested = 0;
            fprintf(stderr,
                    "[antenna_osd] SIGHUP received – reloading \"%s\"\n",
                    cfg_path_used);
            load_config(cfg_path_used);
        }
    }

    return 0;
}
