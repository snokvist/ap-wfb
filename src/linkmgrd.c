/* linkmgrd.c — master-only fail-over daemon
 * -----------------------------------------
 * Build:  gcc -O2 -Wall -o linkmgrd linkmgrd.c
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
#include <sys/wait.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/ip_icmp.h>
#include <arpa/inet.h>
#include <limits.h>

#define MAX_STA   16
#define BUF_SZ    4096
#define LN_SZ     256
#define CFG_DEF   "/etc/linkmgrd.conf"
#define EFFECTIVE_RSSI(sta, cfg) \
        (((sta).ping_fail >= (cfg).g.ping_fail_max) ? -10000 : (sta).rssi)

static volatile int g_run = 1;
static int  g_verbose    = 0;

/* ───────────────────────── data structures ───────────────────────────── */
struct gcfg {
    int  poll_ms, hyst_ms, hyst_db;
    int  floor_db;
    int  http_port;
    int  ping_to_ms, ping_fail_max;
    char html[PATH_MAX];
    char master_if[32];
};
struct sta {
    char ip[64], mac[32];
    int  rssi;
    int  ping_fail;                  /* consecutive ping time-outs        */
};
struct cfg {
    struct gcfg g;
    int  nsta;
    struct sta s[MAX_STA];
    char via[64];                    /* current default gateway IP        */
};

/* ───────────────────────── helpers ───────────────────────────────────── */
static long ms_now(void)
{
    struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}
static void sig_hdl(int s) { (void)s; g_run = 0; }
static void trim(char *s)
{
    char *c = strpbrk(s, ";#"); if (c) *c = 0;
    char *e = s + strlen(s);
    while (e > s && strchr("\n\r \t", *(e - 1))) *--e = 0;
    while (*s && strchr(" \t", *s)) memmove(s, s + 1, strlen(s));
}

/* ───────────────────────── INI parser ─────────────────────────────────── */
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
            if      (!strcmp(k, "poll_interval_ms"))   C->g.poll_ms  = atoi(v);
            else if (!strcmp(k, "hysteresis_ms"))      C->g.hyst_ms  = atoi(v);
            else if (!strcmp(k, "hysteresis_db"))      C->g.hyst_db  = atoi(v);
            else if (!strcmp(k, "switch_floor_db"))    C->g.floor_db = atoi(v);
            else if (!strcmp(k, "http_port"))          C->g.http_port = atoi(v);
            else if (!strcmp(k, "ping_timeout_ms"))    C->g.ping_to_ms = atoi(v);
            else if (!strcmp(k, "ping_fail_max"))      C->g.ping_fail_max = atoi(v);
            else if (!strcmp(k, "html_path"))          strncpy(C->g.html, v, PATH_MAX - 1);
            else if (!strcmp(k, "master_iface"))       strncpy(C->g.master_if, v, 31);

        } else if (!strncmp(sec, "sta", 3)) {
            int i = C->nsta; if (i >= MAX_STA) continue;
            if      (!strcmp(k, "ip"))  strncpy(C->s[i].ip,  v, 63);
            else if (!strcmp(k, "mac")) strncpy(C->s[i].mac, v, 31);
            if (*C->s[i].ip && *C->s[i].mac) C->nsta = i + 1;
        }
    }
    fclose(f);
    if (!*C->g.master_if) strcpy(C->g.master_if, "wlan0");
    return 0;
}

/* ───────────────────────── RSSI polling via iw ───────────────────────── */
static void rssi_poll(struct cfg *C)
{
    char cmd[128];
    snprintf(cmd, sizeof cmd, "iw dev %s station dump", C->g.master_if);

    int fds[2]; pid_t pid;
    if (pipe(fds) || (pid = fork()) < 0) { perror("pipe/fork"); return; }

    if (pid == 0) {                       /* child */
        close(fds[0]); dup2(fds[1], STDOUT_FILENO); close(fds[1]);
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL); _exit(127);
    }
    close(fds[1]);

    FILE *p = fdopen(fds[0], "r");
    if (!p) { close(fds[0]); return; }

    for (int i = 0; i < C->nsta; i++) C->s[i].rssi = -10000;

    char l[LN_SZ], mac[32] = ""; int rssi = -10000;

    auto void commit(const char *m)
    {
        if (!*m) return;
        for (int i = 0; i < C->nsta; i++)
            if (!strcasecmp(m, C->s[i].mac)) { C->s[i].rssi = rssi; break; }
    }

    struct timespec ts0; clock_gettime(CLOCK_MONOTONIC, &ts0);
    int lc = 0;
    while (fgets(l, sizeof l, p)) {
        if ((++lc & 15) == 0) {
            struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
            long ms = (ts.tv_sec - ts0.tv_sec) * 1000L +
                      (ts.tv_nsec - ts0.tv_nsec) / 1000000L;
            if (ms > 300) break;
        }
        char new_mac[32];
        if (sscanf(l, "Station %31s", new_mac) == 1) {
            commit(mac); strcpy(mac, new_mac); rssi = -10000; continue;
        }
        if (strstr(l, "signal")) sscanf(l, "%*s %d", &rssi);
    }
    commit(mac);

    fclose(p); kill(pid, SIGKILL); waitpid(pid, NULL, 0);
}

/* ───────────────────────── tiny raw-socket ICMP ping ──────────────────── */
static uint16_t csum16(const void *v, size_t len)
{
    const uint16_t *p = v; uint32_t sum = 0;
    while (len > 1) { sum += *p++; len -= 2; }
    if (len) sum += *(uint8_t *)p;
    sum = (sum >> 16) + (sum & 0xffff);
    sum += (sum >> 16);
    return (uint16_t)~sum;
}
static int ping_alive(const char *ip, int timeout_ms)
{
    int sock = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
    if (sock < 0) return 0;

    struct timeval tv = { timeout_ms / 1000,
                          (timeout_ms % 1000) * 1000 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);

    struct sockaddr_in dst = {0};
    dst.sin_family = AF_INET; inet_aton(ip, &dst.sin_addr);

    uint8_t pkt[64] = {0};
    struct icmphdr *h = (struct icmphdr *)pkt;
    h->type = ICMP_ECHO; h->code = 0;
    h->un.echo.id = htons(getpid() & 0xffff);
    h->un.echo.sequence = htons(1);
    h->checksum = csum16(pkt, sizeof pkt);

    int ok = sendto(sock, pkt, sizeof pkt, 0,
                    (struct sockaddr *)&dst, sizeof dst) >= 0;

    if (ok) {
        uint8_t buf[128];
        struct sockaddr_in sfrom; socklen_t sl = sizeof sfrom;
        ok = 0;
        if (recvfrom(sock, buf, sizeof buf, 0,
                     (struct sockaddr *)&sfrom, &sl) >= (ssize_t)sizeof(struct ip))
        {
            struct icmphdr *rh = (struct icmphdr *)(buf + sizeof(struct ip));

            /* accept *only* a genuine Echo‑Reply from the target IP */
            if (rh->type == ICMP_ECHOREPLY &&
                rh->un.echo.id == h->un.echo.id &&
                sfrom.sin_addr.s_addr == dst.sin_addr.s_addr)
                ok = 1;
        }
    }
    close(sock);
    return ok;
}
static void ping_poll(struct cfg *C)
{
    /* remembers consecutive successes per STA */
    static uint8_t ok_streak[MAX_STA] = {0};

    for (int i = 0; i < C->nsta; i++) {
        int alive = ping_alive(C->s[i].ip, C->g.ping_to_ms);

        if (alive) {
            if (ok_streak[i] < 255) ok_streak[i]++;

            /* clear fault after ping_fail_max consecutive OKs */
            if (ok_streak[i] >= C->g.ping_fail_max) {
                C->s[i].ping_fail = 0;
            } else if (C->s[i].ping_fail > 0) {
                C->s[i].ping_fail--;             /* gentle decay */
            }
        } else {                                 /* timeout */
            ok_streak[i] = 0;                    /* break streak */
            if (C->s[i].ping_fail < 255)
                C->s[i].ping_fail++;
        }

        /* verbose console line uses masked RSSI */
        if (g_verbose) {
            int er = EFFECTIVE_RSSI(C->s[i], *C);
            printf("[ping] %s %s  rssi=%d  fail=%d\n",
                   C->s[i].ip,
                   alive ? "OK" : "timeout",
                   er,
                   C->s[i].ping_fail);
        }
    }
}



/* ───────────────────────── route helpers ─────────────────────────────── */
static void master_route(struct cfg *C, const char *gw)
{
    char cmd[256];
    snprintf(cmd, sizeof cmd, "ip route del default dev %s 2>/dev/null",
             C->g.master_if);
    system(cmd);
    if (!*gw) return;
    snprintf(cmd, sizeof cmd, "ip route add default via %s dev %s",
             gw, C->g.master_if);
    system(cmd);
}
static int route_is_ok(struct cfg *C, const char *gw)
{
    FILE *p = popen("ip route show default", "r");
    if (!p) return 0;
    char l[256]; int ok = 0;
    while (fgets(l, sizeof l, p))
        if (strstr(l, gw) && strstr(l, C->g.master_if)) { ok = 1; break; }
    pclose(p); return ok;
}
static void route_watchdog(struct cfg *C)
{
    if (!*C->via) return;
    if (route_is_ok(C, C->via)) return;
    if (g_verbose) fprintf(stderr, "[route] watchdog: repairing table\n");
    master_route(C, C->via);
}

/* ───────────────────────── decision engine ───────────────────────────── */
static void decide(struct cfg *C)
{
    if (*C->via) {
        for (int i = 0; i < C->nsta; i++)
            if (!strcmp(C->via, C->s[i].ip) &&
                EFFECTIVE_RSSI(C->s[i], *C) >= C->g.floor_db)
                return;                        /* don’t switch */
    }

    /* find best usable link */
    int best = -10000; char best_ip[64] = "";
    for (int i = 0; i < C->nsta; i++) {
        int er = EFFECTIVE_RSSI(C->s[i], *C);
        if (er > best) { best = er; strcpy(best_ip, C->s[i].ip); }
    }

    if (best <= -1000) {                       /* nothing usable */
        if (*C->via) { *C->via = 0; master_route(C, ""); }
        return;
    }

    /* hysteresis window */
    char cand[64] = "";
    for (int i = 0; i < C->nsta; i++)
        if (best - EFFECTIVE_RSSI(C->s[i], *C) < C->g.hyst_db)
            strncpy(cand, C->s[i].ip, 63);

    static char last[64] = "";
    static long t0 = 0;
    long now = ms_now();

    if (strcmp(cand, last)) {
        if (!t0) t0 = now;
        if (now - t0 >= C->g.hyst_ms) {
            strcpy(last, cand); strcpy(C->via, cand);
            master_route(C, cand);
            if (g_verbose) printf("[switch] via %s (rssi %d)\n", cand, best);
            t0 = 0;
        }
    } else t0 = 0;
}

/* ───────────────────────── minimal HTTP API ──────────────────────────── */
static int srv_init(int p)
{
    int s = socket(AF_INET, SOCK_STREAM, 0);
    int y = 1; setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &y, sizeof y);
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET; a.sin_port = htons(p);
    bind(s, (void *)&a, sizeof a); listen(s, 8);
    fcntl(s, F_SETFL, O_NONBLOCK); return s;
}
static void http_send(int fd, const char *typ, const char *b)
{
    char h[128];
    int n = snprintf(h, sizeof h,
        "HTTP/1.0 200 OK\r\nContent-Type: %s\r\nContent-Length: %zu\r\n\r\n",
        typ, strlen(b));
    send(fd, h, n, 0); send(fd, b, strlen(b), 0);
}
static void json_status(struct cfg *C, char *out)
{
    int n = 0;
    n += sprintf(out + n,
                 "{\"role\":\"master\",\"active\":\"%s\",\"nodes\":[",
                 *C->via ? C->via : "none");

    for (int i = 0; i < C->nsta; i++)
        n += sprintf(out + n,
            "%s{\"ip\":\"%s\",\"rssi\":%d,\"fail\":%d}",
            i ? "," : "",
            C->s[i].ip,
            EFFECTIVE_RSSI(C->s[i], *C),
            C->s[i].ping_fail);

    sprintf(out + n, "]}\n");
}


static void handle(int fd, struct cfg *C)
{
    char req[BUF_SZ];
    int r = 0, n;
    while ((n = recv(fd, req + r, sizeof req - 1 - r, 0)) > 0) {
        r += n;
        if (memchr(req, '\n', r)) break;          /* got first line */
    }
    if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) { close(fd); return; }
    if (r == 0) { close(fd); return; }            /* nothing read */
    req[r] = 0;



    if (!strncmp(req, "GET /status", 11)) {
        char body[BUF_SZ]; json_status(C, body);
        http_send(fd, "application/json", body);

    } else if (!strncmp(req, "GET / ", 6)) {
        int f = open(C->g.html, O_RDONLY);
        if (f >= 0) {
            struct stat st; fstat(f, &st);
            char hdr[128]; int n = snprintf(hdr, sizeof hdr,
                "HTTP/1.0 200 OK\r\nContent-Length: %zu\r\n"
                "Content-Type: text/html\r\n\r\n", st.st_size);
            send(fd, hdr, n, 0);
            while ((r = read(f, req, sizeof req)) > 0) send(fd, req, r, 0);
            close(f);
        } else http_send(fd, "text/plain", "404\n");

    } else http_send(fd, "text/plain", "404\n");

    close(fd);
}

/* ───────────────────────── main loop ─────────────────────────────────── */
int main(int argc, char **argv)
{
    const char *cfgf = CFG_DEF;
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "--verbose")) g_verbose = 1;
        else cfgf = argv[i];

    struct cfg C = {0};
    /* sensible defaults */
    C.g.poll_ms       = 500;
    C.g.hyst_ms       = 2000;
    C.g.hyst_db       = 20;
    C.g.floor_db      = -40;
    C.g.http_port     = 8080;
    C.g.ping_to_ms    = 700;
    C.g.ping_fail_max = 3;
    strcpy(C.g.master_if, "wlan0");
    strcpy(C.g.html, "/etc/linkmgrd.html");

    if (ini_load(cfgf, &C) < 0) return 1;

    if (g_verbose)
        printf("[init] nsta=%d poll=%dms ping_to=%dms fail_max=%d iface=%s\n",
               C.nsta, C.g.poll_ms, C.g.ping_to_ms, C.g.ping_fail_max,
               C.g.master_if);

    signal(SIGINT, sig_hdl); signal(SIGTERM, sig_hdl);

    int srv = srv_init(C.g.http_port);
    long next_poll = ms_now() + C.g.poll_ms;
    long next_dec  = ms_now() + C.g.hyst_ms;

    while (g_run) {
        long now = ms_now();
        long to = 500;
        if (now < next_poll && next_poll - now < to) to = next_poll - now;
        if (now < next_dec  && next_dec  - now < to) to = next_dec  - now;

        struct timeval tv = { to / 1000, (to % 1000) * 1000 };
        fd_set rset; FD_ZERO(&rset); FD_SET(srv, &rset);
        if (select(srv + 1, &rset, NULL, NULL, &tv) > 0 &&
            FD_ISSET(srv, &rset)) {
            int c = accept(srv, NULL, NULL);
            if (c >= 0) {
                int flags = fcntl(c, F_GETFL, 0);          /* ← NEW */
                fcntl(c, F_SETFL, flags & ~O_NONBLOCK);    /* ← NEW */

    handle(c, &C);
}
        }

        now = ms_now();
        if (now >= next_poll) {
            rssi_poll(&C);
            ping_poll(&C);
            route_watchdog(&C);          /* keep table sane */
            next_poll = now + C.g.poll_ms;
        }
        if (now >= next_dec) {
            decide(&C);
            next_dec  = now + C.g.hyst_ms;
        }
    }
    close(srv);
    return 0;
}
