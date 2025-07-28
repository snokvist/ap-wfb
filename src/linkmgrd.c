/* linkmgrd.c — single‑file UDP‑link fail‑over daemon
 * ---------------------------------------------------
 *   roles : master | sta      (configured in INI)
 *   metric: RSSI + dBm / time hysteresis
 *   API   : HTTP/1.0  (GET /status , POST /update)
 *   build : gcc -O2 -Wall -o linkmgrd linkmgrd.c
 *
 * summary of operation
 *   • master polls its AP radio (iw station dump), compares RSSI, pushes /update to STAs
 *   • each STA polls its own interface (iw link) and obeys /update to rewrite a /32 route
 *   • no STA‑>master “report” traffic is required
 *   • verbose mode prints every poll, MAC addresses, and route switches
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <limits.h>

#define MAX_STA   16
#define BUF_SZ    4096
#define LN_SZ     256
#define CFG_DEF   "/etc/linkmgrd.conf"

static volatile int g_run = 1;
static int  g_verbose    = 0;
static long ms_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}
static void sig_hdl(int s) { (void)s; g_run = 0; }

/* strip inline “;” or “#” comments and trim whitespace */
static void trim(char *s)
{
    char *c = strpbrk(s, ";#"); if (c) *c = 0;
    char *e = s + strlen(s);
    while (e > s && strchr("\n\r \t", *(e - 1))) *--e = 0;
    while (*s && strchr(" \t", *s)) memmove(s, s + 1, strlen(s));
}

/* ------------------------------------------------------------------ config */
struct gcfg {
    char role[8];
    int  poll_ms, hyst_ms, hyst_db;
    int  http_port, http_to;
    char html[PATH_MAX];
    char master_if[32];          /* interface polled on master */
};
struct sta {
    char ifc[32], ip[64], mac[32];
    int  rssi; long inact;
    unsigned long rx, tx;
};
struct cfg {
    struct gcfg g;
    char master_ip[64];
    char csv[256];               /* master: list from stas= */
    int  nsta;                   /* number of [staX] blocks */
    struct sta s[MAX_STA];
    char via[64];                /* master’s current active link */
};

static int ini_load(const char *fn, struct cfg *C)
{
    FILE *f = fopen(fn, "r");
    if (!f) { perror(fn); return -1; }

    char sec[32] = "", ln[LN_SZ];
    while (fgets(ln, sizeof ln, f)) {
        trim(ln); if (!*ln) continue;
        if (*ln == '[') { sscanf(ln, "[%31[^]]", sec); continue; }

        char *eq = strchr(ln, '=');
        if (!eq) continue;
        *eq = 0; char *k = ln, *v = eq + 1; trim(k); trim(v);

        if (!strcmp(sec, "general")) {
            if      (!strcmp(k, "role"))               strncpy(C->g.role, v, 7);
            else if (!strcmp(k, "poll_interval_ms"))   C->g.poll_ms  = atoi(v);
            else if (!strcmp(k, "hysteresis_ms"))      C->g.hyst_ms  = atoi(v);
            else if (!strcmp(k, "hysteresis_db"))      C->g.hyst_db  = atoi(v);
            else if (!strcmp(k, "http_port"))          C->g.http_port = atoi(v);
            else if (!strcmp(k, "http_timeout_s"))     C->g.http_to   = atoi(v);
            else if (!strcmp(k, "html_path"))          strncpy(C->g.html, v, PATH_MAX - 1);

        } else if (!strcmp(sec, "master")) {
            if      (!strcmp(k, "stas"))               strncpy(C->csv, v, 255);
            else if (!strcmp(k, "master_iface"))       strncpy(C->g.master_if, v, 31);

        } else if (!strcmp(sec, "sta")) {
            if (!strcmp(k, "master_ip"))               strncpy(C->master_ip, v, 63);

        } else if (!strncmp(sec, "sta", 3)) {
            int i = C->nsta; if (i >= MAX_STA) continue;
            if      (!strcmp(k, "iface")) strncpy(C->s[i].ifc, v, 31);
            else if (!strcmp(k, "ip"))    strncpy(C->s[i].ip,  v, 63);
            else if (!strcmp(k, "mac"))   strncpy(C->s[i].mac, v, 31);
            if (*C->s[i].ifc && *C->s[i].ip && *C->s[i].mac) C->nsta = i + 1;
        }
    }
    fclose(f);
    if (!*C->g.master_if) strcpy(C->g.master_if, "wlan0");
    return 0;
}

/* ------------------------------------------------------------------ helpers */
static int exec_line(const char *cmd, const char *key, char *out, size_t len)
{
    FILE *p = popen(cmd, "r"); if (!p) return -1;
    char b[LN_SZ]; int ok = -1;
    while (fgets(b, sizeof b, p))
        if (strstr(b, key)) { strncpy(out, b, len - 1); ok = 0; break; }
    pclose(p); return ok;
}
static void dev_stats(const char *ifc, unsigned long *rx, unsigned long *tx)
{
    FILE *f = fopen("/proc/net/dev", "r"); if (!f) return;
    char l[LN_SZ]; fgets(l, LN_SZ, f); fgets(l, LN_SZ, f);
    while (fgets(l, LN_SZ, f))
        if (strstr(l, ifc) == l) {
            unsigned long rxb, rxp, txb, txp;
            sscanf(l + strlen(ifc) + 1,
                   "%lu %lu %*u %*u %*u %*u %*u %*u %lu %lu",
                   &rxb, &rxp, &txb, &txp);
            *rx = rxp; *tx = txp; break;
        }
    fclose(f);
}

/* ------------------------------------------------------------------ polling – STA */
static void sta_poll(struct cfg *C)
{
    for (int i = 0; i < C->nsta; i++) {
        struct sta *s = &C->s[i];
        char cmd[128], ln[LN_SZ];

        snprintf(cmd, sizeof cmd, "iw dev %s link", s->ifc);
        s->rssi  = exec_line(cmd, "signal", ln, sizeof ln) ? -10000
                  : (sscanf(ln, "%*s %d", &s->rssi), s->rssi);

        s->inact = exec_line(cmd, "inactive time", ln, sizeof ln) ? -1
                  : (sscanf(ln, "%*s %*s %ld", &s->inact), s->inact);

        dev_stats(s->ifc, &s->rx, &s->tx);

        if (g_verbose)
            printf("[sta] %-10s mac=%s rssi=%d ina=%ld rx=%lu tx=%lu\n",
                   s->ifc, s->mac, s->rssi, s->inact, s->rx, s->tx);
    }
}

/* linkmgrd.c — July 28 2025, blank‑line‑free station‑dump parser */
//  (same header & includes as before)  …
// ---------- unchanged code until mst_poll() ------------------

static void mst_poll(struct cfg *C)
{
    char cmd[128]; snprintf(cmd,sizeof cmd,"iw dev %s station dump",C->g.master_if);
    FILE *p = popen(cmd,"r"); if(!p){ if(g_verbose) puts("[mst] dump fail"); return; }

    /* mark all unseen */
    for(int i=0;i<C->nsta;i++){ C->s[i].rssi=-10000; C->s[i].inact=-1; }

    char l[LN_SZ], mac[32]=""; int rssi=-10000; long ina=-1;

    /* helper commits one finished block */
    void store(const char *m){
        if(!*m) return;
        for(int i=0;i<C->nsta;i++)
            if(!strcasecmp(m,C->s[i].mac)){
                C->s[i].rssi=rssi; C->s[i].inact=ina;
                dev_stats(C->s[i].ifc,&C->s[i].rx,&C->s[i].tx);
                break;
            }
    };

    while(fgets(l,sizeof l,p)){
        if(sscanf(l,"Station %31s",mac)==1){ store(mac); /* commit prev */ rssi=-10000; ina=-1; }
        else if(strstr(l,"signal"))        sscanf(l,"%*s %d",&rssi);
        else if(strstr(l,"inactive time")) sscanf(l,"%*s %*s %ld",&ina);
    }
    store(mac);                /* commit last block */
    pclose(p);

    if(g_verbose)
        for(int i=0;i<C->nsta;i++)
            printf("[mst] %-15s mac=%s rssi=%d ina=%ld\n",
                   C->s[i].ip,C->s[i].mac,C->s[i].rssi,C->s[i].inact);
}
// --------------------- rest of file unchanged -----------------


/* ------------------------------------------------------------------ decision (master) */
static void push_update(struct cfg *C, const char *via)
{
    for (int i = 0; i < C->nsta; i++) {
        int s = socket(AF_INET, SOCK_STREAM, 0);
        if (s < 0) continue;
        struct timeval tv = { C->g.http_to, 0 };
        setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);

        struct sockaddr_in sa = {0};
        sa.sin_family = AF_INET;
        sa.sin_port   = htons(C->g.http_port);
        inet_aton(C->s[i].ip, &sa.sin_addr);

        if (connect(s, (void *)&sa, sizeof sa) == 0) {
            char body[128];
            int bl = snprintf(body, sizeof body,
                "{\"master_ip\":\"%s\",\"via\":\"%s\"}\n",
                *C->master_ip ? C->master_ip : "192.168.0.1",
                via);

            char req[256];
            int rl = snprintf(req, sizeof req,
                "POST /update HTTP/1.0\r\n"
                "Host: %s\r\n"
                "Content-Length: %d\r\n\r\n%s",
                C->s[i].ip, bl, body);

            send(s, req, rl, 0);
            recv(s, req, sizeof req, 0);
        }
        close(s);
    }
}
static void decide(struct cfg *C)
{
    if (C->nsta == 0) { *C->via = 0; return; }

    /* single STA shortcut */
    if (C->nsta == 1) {
        if (strcmp(C->via, C->s[0].ip)) {
            strcpy(C->via, C->s[0].ip);
            if (g_verbose) printf("[switch] via %s (single)\n", C->via);
            push_update(C, C->via);
        }
        return;
    }

    int best = -10000;
    for (int i = 0; i < C->nsta; i++)
        if (C->s[i].rssi > best) best = C->s[i].rssi;

    if (best <= -1000) {                      /* all links down */
        if (*C->via) {
            *C->via = 0;
            if (g_verbose) puts("[switch] link lost");
            push_update(C, "0.0.0.0");
        }
        return;
    }

    char cand[64] = "";
    for (int i = 0; i < C->nsta; i++)
        if (best - C->s[i].rssi < C->g.hyst_db)
            strncpy(cand, C->s[i].ip, 63);

    static char last[64] = "";
    static long t0 = 0;
    long now = ms_now();

    if (strcmp(cand, last)) {
        if (!t0) t0 = now;
        if (now - t0 >= C->g.hyst_ms) {
            strcpy(last, cand);
            strcpy(C->via, cand);
            if (g_verbose) printf("[switch] via %s\n", cand);
            push_update(C, cand);
            t0 = 0;
        }
    } else t0 = 0;
}

/* ------------------------------------------------------------------ HTTP helpers */
static int srv_init(int p)
{
    int s = socket(AF_INET, SOCK_STREAM, 0);
    int y = 1; setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &y, sizeof y);
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET; a.sin_port = htons(p);
    bind(s, (void *)&a, sizeof a); listen(s, 8);
    fcntl(s, F_SETFL, O_NONBLOCK);
    return s;
}
static void http_send(int fd, const char *typ, const char *b)
{
    char h[128];
    int n = snprintf(h, sizeof h,
        "HTTP/1.0 200 OK\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %zu\r\n\r\n",
        typ, strlen(b));
    send(fd, h, n, 0);
    send(fd, b, strlen(b), 0);
}
static void json_status(struct cfg *C, char *buf)
{
    int n = 0;
    n += sprintf(buf + n, "{\"role\":\"%s\",\"active\":\"%s\"",
                 C->g.role, *C->via ? C->via : "none");

    if (!strcmp(C->g.role, "sta") && C->nsta)
        n += sprintf(buf + n,
                     ",\"rssi\":%d,\"inactive_ms\":%ld",
                     C->s[0].rssi, C->s[0].inact);

    if (!strcmp(C->g.role, "master")) {
        n += sprintf(buf + n, ",\"nodes\":[");
        for (int i = 0; i < C->nsta; i++)
            n += sprintf(buf + n,
                "%s{\"ip\":\"%s\",\"rssi\":%d}",
                i ? "," : "", C->s[i].ip, C->s[i].rssi);
        n += sprintf(buf + n, "]");
    }
    sprintf(buf + n, "}\n");
}

/* BusyBox‑compatible neighbour flush */
static void sta_route(struct cfg *C, const char *via)
{
    int ok = 0;
    for (int i = 0; i < C->nsta; i++)
        if (!strcmp(via, C->s[i].ip)) { ok = 1; break; }
    if (!ok) { if (g_verbose) puts("[sta] invalid via"); return; }

    char cmd[128];
    snprintf(cmd, sizeof cmd,
             "ip route replace %s/32 via %s", C->master_ip, via);
    system(cmd);

    snprintf(cmd, sizeof cmd,
             "ip neigh del %s dev %s"
             " || ip -4 neigh flush to %s dev %s",
             C->master_ip, C->s[0].ifc,
             C->master_ip, C->s[0].ifc);
    system(cmd);

    if (g_verbose) printf("[sta] route -> %s\n", via);
}

static void handle(int fd, struct cfg *C)
{
    char req[BUF_SZ]; int r = recv(fd, req, sizeof req - 1, 0);
    if (r <= 0) { close(fd); return; } req[r] = 0;
    if (g_verbose) printf("[http] %.40s\n", req);

    if (!strncmp(req, "GET /status", 11)) {
        char body[BUF_SZ]; json_status(C, body);
        http_send(fd, "application/json", body);

    } else if (!strncmp(req, "GET / ", 6)) {
        int f = open(C->g.html, O_RDONLY);
        if (f >= 0) {
            struct stat st; fstat(f, &st);
            char hdr[128]; int n = sprintf(hdr,
                "HTTP/1.0 200 OK\r\n"
                "Content-Length: %zu\r\n"
                "Content-Type: text/html\r\n\r\n", st.st_size);
            send(fd, hdr, n, 0);
            while ((r = read(f, req, sizeof req)) > 0) send(fd, req, r, 0);
            close(f);
        } else http_send(fd, "text/plain", "404\n");

    } else if (!strncmp(req, "POST /update", 12) &&
               !strcmp(C->g.role, "sta")) {
        char *p = strstr(req, "\"via\":\"");
        if (p) {
            p += 7; char *e = strchr(p, '"');
            if (e && e - p < 64) {
                char ip[64]; memcpy(ip, p, e - p); ip[e - p] = 0;
                sta_route(C, ip);
                http_send(fd, "application/json", "{\"status\":\"ok\"}\n");
                close(fd); return;
            }
        }
        http_send(fd, "application/json", "{\"status\":\"error\"}\n");

    } else {
        http_send(fd, "text/plain", "404\n");
    }
    close(fd);
}

/* ------------------------------------------------------------------ main */
int main(int argc, char **argv)
{
    const char *cfgf = CFG_DEF;
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "--verbose")) g_verbose = 1;
        else cfgf = argv[i];

    struct cfg C = {0};
    /* defaults */
    strcpy(C.g.role,"sta");
    C.g.poll_ms   = 500;
    C.g.hyst_ms   = 2000;
    C.g.hyst_db   = 20;
    C.g.http_port = 8080;
    C.g.http_to   = 1;
    strcpy(C.g.html, "/etc/linkmgrd.html");

    if (ini_load(cfgf, &C) < 0) return 1;

    if (g_verbose) {
        printf("[init] role=%s nsta=%d poll=%dms hyst=%ddB/%dms\n",
               C.g.role, C.nsta, C.g.poll_ms,
               C.g.hyst_db, C.g.hyst_ms);
        if (!strcmp(C.g.role, "master"))
            printf("[init] master STAs: %s iface=%s\n",
                   C.csv, C.g.master_if);
    }

    signal(SIGINT, sig_hdl);
    signal(SIGTERM, sig_hdl);

    int srv = srv_init(C.g.http_port);
    long next_poll = ms_now() + C.g.poll_ms;
    long next_dec  = ms_now() + C.g.hyst_ms;

    while (g_run) {
        long now = ms_now();
        long to  = 500;
        if (now < next_poll && next_poll - now < to) to = next_poll - now;
        if (!strcmp(C.g.role,"master") &&
            now < next_dec && next_dec - now < to)  to = next_dec - now;

        struct timeval tv = { to / 1000, (to % 1000) * 1000 };
        fd_set rset; FD_ZERO(&rset); FD_SET(srv, &rset);
        if (select(srv + 1, &rset, NULL, NULL, &tv) > 0 &&
            FD_ISSET(srv, &rset)) {
            int c = accept(srv, NULL, NULL);
            if (c >= 0) { fcntl(c, F_SETFL, O_NONBLOCK); handle(c, &C); }
        }

        now = ms_now();
        if (now >= next_poll) {
            if (!strcmp(C.g.role, "master")) mst_poll(&C);
            else                             sta_poll(&C);
            next_poll = now + C.g.poll_ms;
        }
        if (!strcmp(C.g.role, "master") && now >= next_dec) {
            decide(&C); next_dec = now + C.g.hyst_ms;
        }
    }
    close(srv);
    return 0;
}
