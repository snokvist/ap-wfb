/*
 * tiny_srv.c — micro HTTP/1.0 server with epoll + idle‑timeout
 *
 * Build (static, size‑optimised, arm example):
 *   arm-linux-gnueabihf-gcc -Wall -Wextra -O2 -static tiny_srv.c -o web
 *
 * Example run:
 *   ./web --html index.html --commands commands.conf --port 81
 *
 * Endpoints (unchanged):
 *   GET /                         → index.html
 *   GET /cmd/<name>[?args=...]    → executes mapped command, returns "OK"
 *   GET /value/<name>             → executes mapped read‑only command, returns its stdout
 *   GET /log                      → last 60 lines of /tmp/webui.log
 */

#define _GNU_SOURCE
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define PORT_DEF          80
#define BACKLOG           128
#define BUF_SZ            1024
#define MAX_CMDS          64
#define MAX_VALS          32
#define CMD_MAXLEN        256
#define VALUE_BUF         1024

#define MAX_EVENTS        64
#define MAX_FDS           65536
#define CLIENT_TIMEOUT_MS 5000           /* idle ms before close */

#define LOG_FILE     "/tmp/webui.log"
#define LOG_TAIL_BUF 8192
#define LOG_LINES     60

/* ----- helpers --------------------------------------------------------- */

static void die(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap);
    exit(EXIT_FAILURE);
}

static int set_nonblock(int fd)
{
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl < 0) return -1;
    return fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

static int send_all(int fd, const void *buf, size_t len)
{
    const char *p = buf;
    while (len) {
        ssize_t n = send(fd, p, len, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR)                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
            return -1;
        }
        p   += n;
        len -= n;
    }
    return 0;
}

static uint64_t msec_now(void)
{
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* ----- graceful shutdown flag ----------------------------------------- */

static volatile sig_atomic_t quit_flag = 0;
static void sig_handler(int sig) { (void)sig; quit_flag = 1; }

/* ----- global configuration ------------------------------------------- */

struct cmd { char *name; char *base; };

static struct cmd cmds[MAX_CMDS];   size_t n_cmds = 0;
static struct cmd vals[MAX_VALS];   size_t n_vals = 0;

static char *html_buf = NULL;       size_t html_len = 0;
static const char *HTML_HDR =
    "HTTP/1.0 200 OK\r\nContent-Type: text/html\r\n\r\n";

/* ----- file helpers ---------------------------------------------------- */

static char *slurp(const char *path, size_t *len_out)
{
    FILE *f = fopen(path, "rb");
    if (!f) die("Cannot open %s: %s\n", path, strerror(errno));
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    if (sz < 0) die("ftell failed on %s\n", path);
    rewind(f);
    char *buf = malloc(sz + 1);
    if (!buf) die("Out of memory\n");
    fread(buf, 1, sz, f); buf[sz] = 0; fclose(f);
    if (len_out) *len_out = (size_t)sz;
    return buf;
}

static char *rtrim(char *s)
{
    size_t n = strlen(s);
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t')) s[--n] = 0;
    return s;
}

/* ----- load commands.conf (same syntax as before) --------------------- */

static void load_commands(const char *path)
{
    char *file = slurp(path, NULL);
    char *saveptr = NULL, *line = strtok_r(file, "\r\n", &saveptr);

    while (line) {
        while (*line == ' ' || *line == '\t') ++line;
        if (*line && *line != '#') {

            char *c1 = strchr(line, ':');
            if (!c1) die("Bad line in %s: %s\n", path, line);

            *c1 = 0;
            char *left  = rtrim(line);
            char *right = c1 + 1;

            /* VALUE entry */
            if (!strncmp(left, "value", 5)) {
                char *vname, *vcmd;
                if (*right == ':') ++right;
                char *c2 = strchr(right, ':');

                if (c2) {
                    *c2 = 0; vname = rtrim(right); vcmd  = c2 + 1;
                } else {
                    vname = right;
                    vcmd  = strpbrk(right, " \t");
                    if (!vcmd) die("Bad value line in %s\n", path);
                    *vcmd++ = 0;
                }
                while (*vcmd == ' ' || *vcmd == '\t') ++vcmd;
                rtrim(vcmd);

                if (n_vals >= MAX_VALS) die("Too many values\n");
                vals[n_vals++] = (struct cmd){strdup(vname), strdup(vcmd)};
            }
            /* COMMAND entry */
            else {
                char *cmd = right;
                while (*cmd == ' ' || *cmd == '\t') ++cmd;
                rtrim(cmd);

                if (n_cmds >= MAX_CMDS) die("Too many commands\n");
                cmds[n_cmds++] = (struct cmd){strdup(left), strdup(cmd)};
            }
        }
        line = strtok_r(NULL, "\r\n", &saveptr);
    }
    if (!n_cmds && !n_vals) die("No entries loaded from %s\n", path);
}

/* ----- URL helpers ---------------------------------------------------- */

static void url_decode(char *s)
{
    char *o = s, *p = s;
    while (*p) {
        if (*p == '%' && isxdigit((unsigned char)p[1]) &&
                         isxdigit((unsigned char)p[2])) {
            char hex[3] = {p[1], p[2], 0};
            *o++ = (char)strtol(hex, NULL, 16); p += 3;
        } else if (*p == '+') { *o++ = ' '; ++p; }
        else { *o++ = *p++; }
    }
    *o = 0;
}

static bool safe_arg(const char *s)
{
    for (; *s; ++s)
        if (!(isalnum((unsigned char)*s) || *s == '_' || *s == '-' ||
              *s == '.' || *s == '/'  || *s == ' '))
            return false;
    return true;
}

/* ----- log tail ------------------------------------------------------- */

static void send_log(int cfd)
{
    int fd = open(LOG_FILE, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        send_all(cfd,
            "HTTP/1.0 200 OK\r\nContent-Type: text/plain\r\n\r\n(no log)\n",
            54);
        return;
    }

    off_t sz = lseek(fd, 0, SEEK_END);
    off_t start = (sz > LOG_TAIL_BUF) ? sz - LOG_TAIL_BUF : 0;
    lseek(fd, start, SEEK_SET);

    char buf[LOG_TAIL_BUF + 1];
    ssize_t n = read(fd, buf, LOG_TAIL_BUF); close(fd);
    if (n < 0) n = 0; buf[n] = 0;

    int lines = 0; char *p = buf + n;
    while (p > buf) if (*--p == '\n' && ++lines == LOG_LINES) { ++p; break; }
    if (p < buf) p = buf;

    send_all(cfd,
        "HTTP/1.0 200 OK\r\nContent-Type: text/plain\r\n\r\n", 45);
    send_all(cfd, p, strlen(p));
}

/* ----- single request ------------------------------------------------- */

static void handle_request(int cfd)
{
    char buf[BUF_SZ + 1];
    ssize_t n = recv(cfd, buf, BUF_SZ, 0);
    if (n <= 0) return;
    buf[n] = 0;

    char *path_start = strchr(buf, ' ');
    if (!path_start) return;
    ++path_start;
    char *path_end = strchr(path_start, ' ');
    if (!path_end) return;
    *path_end = 0;

    /* root */
    if (!strcmp(path_start, "/")) {
        send_all(cfd, HTML_HDR, strlen(HTML_HDR));
        send_all(cfd, html_buf, html_len);
        return;
    }

    /* /log */
    if (!strcmp(path_start, "/log")) { send_log(cfd); return; }

    /* /value/<name> */
    if (!strncmp(path_start, "/value/", 7)) {
        const char *name = path_start + 7, *cmd = NULL;
        for (size_t k = 0; k < n_vals; ++k)
            if (!strcmp(vals[k].name, name)) { cmd = vals[k].base; break; }

        if (!cmd) {
            dprintf(cfd,
                "HTTP/1.0 404 Not Found\r\n\r\nUnknown value %s\n", name);
            return;
        }

        FILE *p = popen(cmd, "r");
        if (!p) {
            send_all(cfd,
                "HTTP/1.0 500 Internal\r\n\r\nExec failed\n", 41);
            return;
        }
       /* allow up to 4 KiB of output */
       char out[VALUE_BUF] = {0};
       fgets(out, VALUE_BUF - 1, p);
        size_t L = strlen(out);
        while (L && (out[L - 1] == '\n' || out[L - 1] == '\r')) out[--L] = 0;

        dprintf(cfd,
            "HTTP/1.0 200 OK\r\nContent-Type: text/plain\r\n\r\n%s\n", out);
        return;
    }

    /* /cmd/<name>[?args=...] */
    if (!strncmp(path_start, "/cmd/", 5)) {
        char *name = path_start + 5;
        char *qs   = strchr(name, '?');
        if (qs) *qs = 0;

        const char *base = NULL;
        for (size_t k = 0; k < n_cmds; ++k)
            if (!strcmp(cmds[k].name, name)) { base = cmds[k].base; break; }
        if (!base) {
            dprintf(cfd,
                "HTTP/1.0 404 Not Found\r\n\r\nUnknown command %s\n", name);
            return;
        }

        char final[CMD_MAXLEN];
        strncpy(final, base, CMD_MAXLEN - 1);
        final[CMD_MAXLEN - 1] = 0;

        if (qs) {
            char *arg = strstr(qs + 1, "args=");
            if (arg) {
                arg += 5; url_decode(arg);
                if (!safe_arg(arg)) {
                    send_all(cfd,
                        "HTTP/1.0 400 Bad Request\r\n\r\nBad args\n", 45);
                    return;
                }
                strncat(final, " ", CMD_MAXLEN - 1 - strlen(final));
                strncat(final, arg, CMD_MAXLEN - 1 - strlen(final));
            }
        }

        if (fork() == 0) {
            execl("/bin/sh", "sh", "-c", final, (char*)NULL);
            _exit(EXIT_FAILURE);
        }

        send_all(cfd,
            "HTTP/1.0 200 OK\r\nContent-Type: text/plain\r\n\r\nOK\n", 48);
        return;
    }

    /* fallback */
    send_all(cfd, "HTTP/1.0 400 Bad Request\r\n\r\n", 28);
}

/* --------------------------------------------------------------------- */

int main(int argc, char **argv)
{
    signal(SIGPIPE, SIG_IGN);
    signal(SIGTERM, sig_handler);
    signal(SIGINT , sig_handler);

    struct sigaction sa = { .sa_handler = SIG_DFL, .sa_flags = SA_NOCLDWAIT };
    sigaction(SIGCHLD, &sa, NULL);

    const char *html_path = NULL, *cmd_path = NULL;
    int port = PORT_DEF;

    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--html") && i + 1 < argc)  html_path = argv[++i];
        else if (!strcmp(argv[i], "--commands") && i + 1 < argc)
            cmd_path = argv[++i];
        else if (!strcmp(argv[i], "--port") && i + 1 < argc)
            port = atoi(argv[++i]);
        else
            die("Usage: %s --html file --commands file [--port n]\n", argv[0]);
    }
    if (!html_path || !cmd_path) die("--html and --commands must be specified\n");

    html_buf = slurp(html_path, &html_len);
    load_commands(cmd_path);

    /* listening socket */
    int lfd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (lfd < 0) die("socket: %s\n", strerror(errno));

    int one = 1;                      /* <‑‑ SINGLE definition now */
#ifdef SO_REUSEPORT
    setsockopt(lfd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one));
#endif
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in addr = {
        .sin_family      = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_ANY),
        .sin_port        = htons(port)
    };
    if (bind(lfd, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
        listen(lfd, BACKLOG) < 0)
        die("bind/listen: %s\n", strerror(errno));

    set_nonblock(lfd);

    /* epoll */
    int efd = epoll_create1(EPOLL_CLOEXEC);
    if (efd < 0) die("epoll_create1: %s\n", strerror(errno));

    struct epoll_event ev = { .events = EPOLLIN, .data.fd = lfd };
    if (epoll_ctl(efd, EPOLL_CTL_ADD, lfd, &ev) < 0)
        die("epoll_ctl ADD lfd: %s\n", strerror(errno));

    static uint64_t last_active[MAX_FDS] = {0};

    printf("tiny_srv: port %d  (html %s, commands %s, log %s)\n",
           port, html_path, cmd_path, LOG_FILE);

    /* main loop */
    struct epoll_event events[MAX_EVENTS];
    size_t scan_idx = 0;

    while (!quit_flag) {
        int n = epoll_wait(efd, events, MAX_EVENTS, 1000);
        if (n < 0) {
            if (errno == EINTR) continue;
            die("epoll_wait: %s\n", strerror(errno));
        }

        uint64_t now = msec_now();

        for (int i = 0; i < n; ++i) {
            int fd = events[i].data.fd;

            /* new client */
            if (fd == lfd) {
                for (;;) {
                    struct sockaddr_in cli;
                    socklen_t clilen = sizeof(cli);
                    int cfd = accept4(lfd, (struct sockaddr *)&cli, &clilen,
                                      SOCK_NONBLOCK | SOCK_CLOEXEC);
                    if (cfd < 0) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                        die("accept: %s\n", strerror(errno));
                    }
                    setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY,
                               &one, sizeof(one));

                    struct epoll_event cev = {
                        .events = EPOLLIN | EPOLLRDHUP,
                        .data.fd = cfd
                    };
                    if (epoll_ctl(efd, EPOLL_CTL_ADD, cfd, &cev) < 0)
                        die("epoll_ctl ADD cfd: %s\n", strerror(errno));

                    if (cfd < MAX_FDS) last_active[cfd] = now;
                }
            }
            /* client data */
            else {
                if (events[i].events & (EPOLLHUP | EPOLLERR | EPOLLRDHUP))
                    goto close_client;

                handle_request(fd);

            close_client:
                epoll_ctl(efd, EPOLL_CTL_DEL, fd, NULL);
                close(fd);
                if (fd < MAX_FDS) last_active[fd] = 0;
            }
        }

        /* idle sweep */
        for (size_t sweep = 0; sweep < 256; ++sweep) {
            if (scan_idx >= MAX_FDS) scan_idx = 0;
            int fd = (int)scan_idx++;
            if (last_active[fd] && now - last_active[fd] > CLIENT_TIMEOUT_MS) {
                epoll_ctl(efd, EPOLL_CTL_DEL, fd, NULL);
                close(fd);
                last_active[fd] = 0;
            }
        }
    }

    close(lfd); close(efd);
    return 0;
}
