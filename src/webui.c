/*
 * tiny_srv.c  — minimal single-file HTTP/1.0 server + command dispatcher
 * Build (static, size-optimised):
 *     arm-linux-gnueabihf-gcc -Os -static tiny_srv.c -o tiny_srv
 *
 * Usage:
 *     ./tiny_srv --html index.html --commands commands.conf [--port 8080]
 *
 *     • GET  /               → serves index.html
 *     • GET  /cmd/<name>     → runs mapped shell command, returns its exit code
 *
 * Notes:
 *   – exits if --html or --commands are missing/unreadable
 *   – lines that start with # or are blank in commands.conf are ignored
 *   – maximum 64 commands; raise MAX_CMDS if you need more
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#define PORT_DEF   80
#define BACKLOG    8
#define BUF_SZ     1024
#define MAX_CMDS   64

struct cmd {
    char *name;
    char *cmd;
} cmds[MAX_CMDS];
size_t n_cmds = 0;

char *html_buf      = NULL;
size_t html_len     = 0;
char  *html_header  = NULL;
size_t header_len   = 0;

/* -------------------------------------------------------------------------- */

static void die(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    exit(EXIT_FAILURE);
}

static char *slurp(const char *path, size_t *out_len)
{
    FILE *f = fopen(path, "rb");
    if (!f) die("Cannot open %s: %s\n", path, strerror(errno));
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    if (sz < 0) die("ftell failed on %s\n", path);
    rewind(f);
    char *buf = malloc(sz + 1);
    if (!buf) die("OOM\n");
    if (fread(buf, 1, sz, f) != (size_t)sz) die("Read failed on %s\n", path);
    buf[sz] = 0;
    fclose(f);
    if (out_len) *out_len = sz;
    return buf;
}

static void load_commands(const char *path)
{
    char *file = slurp(path, NULL);
    char *line = strtok(file, "\r\n");
    while (line && n_cmds < MAX_CMDS) {
        while (*line == ' ' || *line == '\t') ++line;
        if (*line && *line != '#') {
            char *colon = strchr(line, ':');
            if (!colon) die("Invalid line in %s: %s\n", path, line);
            *colon = 0;
            char *name = strdup(line);
            char *cmd  = strdup(colon + 1);
            /* trim leading spaces on cmd */
            while (*cmd == ' ' || *cmd == '\t') ++cmd;
            cmds[n_cmds++] = (struct cmd){name, strdup(cmd)};
        }
        line = strtok(NULL, "\r\n");
    }
    if (!n_cmds) die("No commands loaded from %s\n", path);
}

static int set_nonblock(int fd)
{
    int fl = fcntl(fd, F_GETFL, 0);
    return fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

/* -------------------------------------------------------------------------- */

static void handle_request(int cfd)
{
    char buf[BUF_SZ + 1];
    int n = read(cfd, buf, BUF_SZ);
    if (n <= 0) return;
    buf[n] = 0;

    /* ---- GET / ----------------------------------------------------------- */
    if (!strncmp(buf, "GET / ", 6)) {
        send(cfd, html_header, header_len, 0);
        send(cfd, html_buf, html_len, 0);
        return;
    }

    /* ---- GET /cmd/<name> -------------------------------------------------- */
    if (!strncmp(buf, "GET /cmd/", 9)) {
        char name[64] = {0};
        char *p = buf + 9;
        int i = 0;
        while (i < 63 && *p && *p != ' ' && *p != '/') name[i++] = *p++;
        name[i] = 0;

        const char *cmd = NULL;
        for (size_t k = 0; k < n_cmds; ++k)
            if (!strcmp(cmds[k].name, name)) { cmd = cmds[k].cmd; break; }

        if (cmd) {
            int rc = system(cmd);
            dprintf(cfd,
                    "HTTP/1.0 200 OK\r\nContent-Type: text/plain\r\n\r\nOK "
                    "%s (%d)\n",
                    name, rc);
        } else {
            dprintf(cfd,
                    "HTTP/1.0 404 Not Found\r\nContent-Type: text/plain\r\n\r\n"
                    "Unknown command %s\n",
                    name);
        }
        return;
    }

    /* ---- fallback -------------------------------------------------------- */
    dprintf(cfd, "HTTP/1.0 400 Bad Request\r\n\r\n");
}

/* -------------------------------------------------------------------------- */

int main(int argc, char **argv)
{
    signal(SIGPIPE, SIG_IGN);

    const char *html_path = NULL, *cmds_path = NULL;
    int port = PORT_DEF;

    /* --- parse CLI -------------------------------------------------------- */
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--html") && i + 1 < argc) html_path = argv[++i];
        else if (!strcmp(argv[i], "--commands") && i + 1 < argc)
            cmds_path = argv[++i];
        else if (!strcmp(argv[i], "--port") && i + 1 < argc)
            port = atoi(argv[++i]);
        else {
            fprintf(stderr,
                    "Usage: %s --html file --commands file [--port 8080]\n",
                    argv[0]);
            return 1;
        }
    }
    if (!html_path || !cmds_path)
        die("Both --html and --commands must be specified.\n");

    /* --- load assets ------------------------------------------------------ */
    html_buf = slurp(html_path, &html_len);
    load_commands(cmds_path);

    const char *hdr =
        "HTTP/1.0 200 OK\r\nContent-Type: text/html\r\n\r\n";
    header_len = strlen(hdr);
    html_header = strdup(hdr);

    /* --- socket setup ----------------------------------------------------- */
    int sfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sfd < 0) die("socket: %s\n", strerror(errno));
    int on = 1;
    setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));

    struct sockaddr_in addr = {.sin_family      = AF_INET,
                               .sin_addr.s_addr = htonl(INADDR_ANY),
                               .sin_port        = htons(port)};
    if (bind(sfd, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
        listen(sfd, BACKLOG) < 0)
        die("bind/listen: %s\n", strerror(errno));

    set_nonblock(sfd);
    printf("tiny_srv: serving %s (commands %s) on port %d\n", html_path,
           cmds_path, port);

    /* --- main loop -------------------------------------------------------- */
    for (;;) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(sfd, &rfds);
        int maxfd = sfd;

        struct sockaddr_in cli;
        socklen_t clilen = sizeof(cli);
        int cfd;
        while ((cfd = accept(sfd, (struct sockaddr *)&cli, &clilen)) >= 0) {
            set_nonblock(cfd);
            FD_SET(cfd, &rfds);
            if (cfd > maxfd) maxfd = cfd;
        }

        select(maxfd + 1, &rfds, NULL, NULL, NULL);

        for (int fd = 0; fd <= maxfd; ++fd)
            if (FD_ISSET(fd, &rfds) && fd != sfd) {
                handle_request(fd);
                close(fd);
            }
    }
    return 0;
}
