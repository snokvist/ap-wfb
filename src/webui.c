/*
 * tiny_srv.c — micro HTTP/1.0 server with command + value endpoints
 *
 * Build:
 *   arm-linux-gnueabihf-gcc -Os -static tiny_srv.c -o web
 *
 * Typical run:
 *   ./web --html index.html --commands commands.conf --port 81
 *
 * Endpoints
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
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define PORT_DEF      80
#define BACKLOG       8
#define BUF_SZ        1024
#define MAX_CMDS      64
#define MAX_VALS      32
#define CMD_MAXLEN    256
#define LOG_FILE     "/tmp/webui.log"
#define LOG_TAIL_BUF 8192          /* bytes read from end of log */
#define LOG_LINES     60           /* how many rows to return    */

struct cmd { char *name; char *base; };

struct cmd cmds[MAX_CMDS];   size_t n_cmds = 0;
struct cmd vals[MAX_VALS];   size_t n_vals = 0;

char *html_buf = NULL;  size_t html_len = 0;
const char *HTML_HDR = "HTTP/1.0 200 OK\r\nContent-Type: text/html\r\n\r\n";

/* -------------------------------------------------------------------------- */
/* graceful shutdown flag & handler */
volatile sig_atomic_t quit_flag = 0;
static void sig_handler(int sig) { (void)sig; quit_flag = 1; }

/* -------------------------------------------------------------------------- */
static void die(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap);
    exit(EXIT_FAILURE);
}

static char *slurp(const char *path, size_t *len_out)
{
    FILE *f = fopen(path, "rb");
    if (!f) die("Cannot open %s: %s\n", path, strerror(errno));
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    if (sz < 0) die("ftell failed on %s\n", path);
    rewind(f);
    char *buf = malloc(sz + 1);
    if (!buf) die("OOM\n");
    fread(buf, 1, sz, f);
    buf[sz] = 0;
    fclose(f);
    if (len_out) *len_out = sz;
    return buf;
}

static char *rtrim(char *s)
{
    size_t n = strlen(s);
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t')) s[--n] = 0;
    return s;
}

/* -------------------------------------------------------------------------- */
/* load commands.conf (both /cmd and /value lines) */
static void load_commands(const char *path)
{
    char *file = slurp(path, NULL);
    char *saveptr = NULL, *line = strtok_r(file, "\r\n", &saveptr);

    while (line) {
        while (*line == ' ' || *line == '\t') ++line;
        if (*line && *line != '#') {
            char *colon = strchr(line, ':');
            if (!colon) die("Bad line in %s: %s\n", path, line);

            *colon = 0;
            char *name = rtrim(line);
            char *cmd  = colon + 1;
            while (*cmd == ' ' || *cmd == '\t') ++cmd;
            rtrim(cmd);

            if (!strncmp(name, "value:", 6)) {
                name += 6;
                if (n_vals >= MAX_VALS) die("Too many values\n");
                vals[n_vals++] = (struct cmd){strdup(name), strdup(cmd)};
            } else {
                if (n_cmds >= MAX_CMDS) die("Too many commands\n");
                cmds[n_cmds++] = (struct cmd){strdup(name), strdup(cmd)};
            }
        }
        line = strtok_r(NULL, "\r\n", &saveptr);
    }

    if (!n_cmds && !n_vals) die("No entries loaded from %s\n", path);
}

/* -------------------------------------------------------------------------- */
/* URL‑decode (in place) */
static void url_decode(char *s)
{
    char *o = s, *p = s;
    while (*p) {
        if (*p == '%' && isxdigit(p[1]) && isxdigit(p[2])) {
            char hex[3] = {p[1], p[2], 0};
            *o++ = (char)strtol(hex, NULL, 16);
            p += 3;
        } else if (*p == '+') { *o++ = ' '; ++p; }
        else { *o++ = *p++; }
    }
    *o = 0;
}

/* allow‑list for args */
static bool safe_arg(const char *s)
{
    for (; *s; ++s)
        if (!(isalnum(*s) || *s == '_' || *s == '-' || *s == '.' ||
              *s == '/' || *s == ' '))
            return false;
    return true;
}

/* -------------------------------------------------------------------------- */
static int set_nonblock(int fd)
{
    int fl = fcntl(fd, F_GETFL, 0);
    return fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

/* -------------------------------------------------------------------------- */
/* send last LOG_LINES lines of LOG_FILE */
static void send_log(int cfd)
{
    int fd = open(LOG_FILE, O_RDONLY);
    if (fd < 0) {
        dprintf(cfd,
                "HTTP/1.0 200 OK\r\nContent-Type:text/plain\r\n\r\n(no log)\n");
        return;
    }

    off_t sz = lseek(fd, 0, SEEK_END);
    off_t start = 0;
    if (sz > LOG_TAIL_BUF) start = sz - LOG_TAIL_BUF;
    lseek(fd, start, SEEK_SET);

    char buf[LOG_TAIL_BUF + 1];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n < 0) n = 0;
    buf[n] = 0;

    /* find last LOG_LINES newlines */
    int lines = 0;
    char *p = buf + n;
    while (p > buf) {
        if (*--p == '\n' && ++lines == LOG_LINES) { ++p; break; }
    }
    if (p < buf) p = buf;

    dprintf(cfd,
            "HTTP/1.0 200 OK\r\nContent-Type: text/plain\r\n\r\n%s", p);
}

/* -------------------------------------------------------------------------- */
static void handle_request(int cfd)
{
    char buf[BUF_SZ + 1];
    int n = read(cfd, buf, BUF_SZ);
    if (n <= 0) return;
    buf[n] = 0;

    /* isolate path */
    char *path_start = strchr(buf, ' ');
    if (!path_start) return;
    ++path_start;
    char *path_end = strchr(path_start, ' ');
    if (!path_end) return;
    *path_end = 0;

    /* root */
    if (!strcmp(path_start, "/")) {
        send(cfd, HTML_HDR, strlen(HTML_HDR), 0);
        send(cfd, html_buf, html_len, 0);
        return;
    }

    /* /log */
    if (!strcmp(path_start, "/log")) {
        send_log(cfd);
        return;
    }

    /* /value/<name> */
    if (!strncmp(path_start, "/value/", 7)) {
        char *name = path_start + 7;

        const char *cmd = NULL;
        for (size_t k = 0; k < n_vals; ++k)
            if (!strcmp(vals[k].name, name)) { cmd = vals[k].base; break; }

        if (!cmd) {
            dprintf(cfd,
                    "HTTP/1.0 404 Not Found\r\n\r\nUnknown value %s\n", name);
            return;
        }

        FILE *p = popen(cmd, "r");
        if (!p) {
            dprintf(cfd,
                    "HTTP/1.0 500 Internal\r\n\r\nExec failed\n");
            return;
        }

        char out[128] = {0};
        fgets(out, sizeof(out) - 1, p);
        pclose(p);
        /* strip trailing CR/LF */
        size_t L = strlen(out);
        while (L && (out[L - 1] == '\n' || out[L - 1] == '\r'))
            out[--L] = 0;

        dprintf(cfd,
                "HTTP/1.0 200 OK\r\nContent-Type: text/plain\r\n\r\n%s\n",
                out);
        return;
    }

    /* /cmd/<name>[?args=...] */
    if (!strncmp(path_start, "/cmd/", 5)) {
        char *name = path_start + 5;
        char *qs   = strchr(name, '?');
        if (qs) *qs = 0;

        /* find base */
        const char *base = NULL;
        for (size_t k = 0; k < n_cmds; ++k)
            if (!strcmp(cmds[k].name, name)) { base = cmds[k].base; break; }
        if (!base) {
            dprintf(cfd,
                    "HTTP/1.0 404 Not Found\r\n\r\nUnknown command %s\n",
                    name);
            return;
        }

        /* build final command */
        char final[CMD_MAXLEN];
        strncpy(final, base, CMD_MAXLEN - 1);
        final[CMD_MAXLEN - 1] = 0;

        if (qs) {
            char *arg = strstr(qs + 1, "args=");
            if (arg) {
                arg += 5;
                url_decode(arg);
                if (!safe_arg(arg)) {
                    dprintf(cfd,
                            "HTTP/1.0 400 Bad Request\r\n\r\nBad args\n");
                    return;
                }
                strncat(final, " ", CMD_MAXLEN - 1 - strlen(final));
                strncat(final, arg, CMD_MAXLEN - 1 - strlen(final));
            }
        }

        system(final);

        dprintf(cfd,
                "HTTP/1.0 200 OK\r\nContent-Type: text/plain\r\n\r\nOK\n");
        return;
    }

    /* fallback */
    dprintf(cfd, "HTTP/1.0 400 Bad Request\r\n\r\n");
}

/* -------------------------------------------------------------------------- */
int main(int argc, char **argv)
{
    /* make "web" visible in ps/top/killall */
    prctl(PR_SET_NAME, "web", 0, 0, 0);

    /* graceful signals */
    signal(SIGPIPE, SIG_IGN);
    signal(SIGTERM, sig_handler);
    signal(SIGINT , sig_handler);

    const char *html_path = NULL, *cmd_path = NULL;
    int port = PORT_DEF;

    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--html") && i + 1 < argc) html_path = argv[++i];
        else if (!strcmp(argv[i], "--commands") && i + 1 < argc)
            cmd_path = argv[++i];
        else if (!strcmp(argv[i], "--port") && i + 1 < argc)
            port = atoi(argv[++i]);
        else
            die("Usage: %s --html file --commands file [--port n]\n",
                argv[0]);
    }
    if (!html_path || !cmd_path)
        die("--html and --commands must be specified\n");

    html_buf = slurp(html_path, &html_len);
    load_commands(cmd_path);

    /* socket */
    int sfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sfd < 0) die("socket: %s\n", strerror(errno));
    int on = 1; setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));

    struct sockaddr_in addr = { .sin_family      = AF_INET,
                                .sin_addr.s_addr = htonl(INADDR_ANY),
                                .sin_port        = htons(port) };
    if (bind(sfd, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
        listen(sfd, BACKLOG) < 0)
        die("bind/listen: %s\n", strerror(errno));

    set_nonblock(sfd);
    printf("tiny_srv: port %d  (html %s, commands %s, log %s)\n",
           port, html_path, cmd_path, LOG_FILE);

    /* main loop */
    while (!quit_flag) {
        fd_set rfds; FD_ZERO(&rfds); FD_SET(sfd, &rfds); int maxfd = sfd;

        struct sockaddr_in cli; socklen_t clilen = sizeof(cli);
        int cfd;
        while ((cfd = accept(sfd, (struct sockaddr *)&cli, &clilen)) >= 0) {
            set_nonblock(cfd); FD_SET(cfd, &rfds); if (cfd > maxfd) maxfd = cfd;
        }

        int rc = select(maxfd + 1, &rfds, NULL, NULL, NULL);
        if (rc < 0) {
            if (errno == EINTR) continue; /* interrupted by signal */
            die("select: %s\n", strerror(errno));
        }

        for (int fd = 0; fd <= maxfd; ++fd)
            if (FD_ISSET(fd, &rfds) && fd != sfd) {
                handle_request(fd);
                close(fd);
            }
    }

    close(sfd);
    return 0;
}
