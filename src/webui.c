/*  tiny_srv.c — HTTP/1.0 micro‑server
 *  now supports fire‑and‑forget commands tagged with ‘!’
 *  Build:  arm-linux-gnueabihf-gcc -Os -static tiny_srv.c -o tiny_srv
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define PORT_DEF    80
#define BACKLOG     8
#define BUF_SZ      1024
#define MAX_CMDS    64
#define CMD_MAXLEN  256
#define OUT_MAX     4096
#define CMD_TIMEOUT 6000                   /* ms */

struct cmd { char *name; char *base; bool capture; } cmds[MAX_CMDS];
size_t n_cmds = 0;

char *html_buf = NULL;  size_t html_len = 0;
const char *HTML_HDR = "HTTP/1.0 200 OK\r\nContent-Type: text/html\r\n\r\n";

/* ---------- helpers ------------------------------------------------------ */
static void die(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap);
    exit(EXIT_FAILURE);
}
static char *slurp(const char *path, size_t *out_len)
{
    FILE *f = fopen(path, "rb");
    if (!f) die("Cannot open %s: %s\n", path, strerror(errno));
    fseek(f, 0, SEEK_END); long sz = ftell(f); rewind(f);
    char *buf = malloc(sz + 1); fread(buf, 1, sz, f); fclose(f);
    buf[sz] = 0; if (out_len) *out_len = sz; return buf;
}
static char *rtrim(char *s){ size_t n=strlen(s);
    while(n&&(s[n-1]==' '||s[n-1]=='\t')) s[--n]=0; return s; }
static void url_decode(char *s){
    char *o=s,*p=s; while(*p){ if(*p=='%'&&isxdigit(p[1])&&isxdigit(p[2])){
        char h[3]={p[1],p[2],0}; *o++=(char)strtol(h,NULL,16); p+=3;}
        else if(*p=='+'){*o++=' ';p++;} else *o++=*p++; } *o=0;}
static bool safe_arg(const char *s){
    for(;*s;++s) if(!(isalnum(*s)||*s=='_'||*s=='-'||*s=='.'||*s=='/'||*s==' '))
        return false; return true;}
static int  set_nonblock(int fd){int fl=fcntl(fd,F_GETFL,0);
    return fcntl(fd,F_SETFL,fl|O_NONBLOCK);}
static long ms_since(const struct timespec *st){
    struct timespec n; clock_gettime(CLOCK_MONOTONIC,&n);
    return (n.tv_sec-st->tv_sec)*1000L+(n.tv_nsec-st->tv_nsec)/1000000L;}

/* ---------- load commands.conf ------------------------------------------ */
static void load_commands(const char *path)
{
    char *file = slurp(path, NULL);
    char *saveptr=NULL, *line=strtok_r(file,"\r\n",&saveptr);
    while(line && n_cmds<MAX_CMDS){
        while(*line==' '||*line=='\t') ++line;
        if(*line&&*line!='#'){
            char *colon=strchr(line,':');
            if(!colon) die("Bad line in %s: %s\n",path,line);
            *colon=0;
            bool cap=true;
            if(colon>line && colon[-1]=='!'){ colon[-1]=0; cap=false; }
            char *name=rtrim(line);
            char *cmd =colon+1; while(*cmd==' '||*cmd=='\t') ++cmd; rtrim(cmd);
            cmds[n_cmds++] = (struct cmd){strdup(name),strdup(cmd),cap};
        }
        line=strtok_r(NULL,"\r\n",&saveptr);
    }
    if(!n_cmds) die("No commands in %s\n",path);
}

/* ---------- HTTP handler ------------------------------------------------- */
static void handle_request(int cfd)
{
    char buf[BUF_SZ+1]; int n=read(cfd,buf,BUF_SZ); if(n<=0) return; buf[n]=0;
    char *p0=strchr(buf,' '); if(!p0) return; ++p0; char *p1=strchr(p0,' ');
    if(!p1) return; *p1=0;

    if(!strcmp(p0,"/")){ send(cfd,HTML_HDR,strlen(HTML_HDR),0);
        send(cfd,html_buf,html_len,0); return; }

    if(strncmp(p0,"/cmd/",5)){ dprintf(cfd,"HTTP/1.0 400 Bad Request\r\n\r\n");
        return; }

    /* lookup command ------------------------------------------------------ */
    char *name=p0+5; char *qs=strchr(name,'?'); if(qs)*qs=0;
    struct cmd *entry=NULL;
    for(size_t i=0;i<n_cmds;++i) if(!strcmp(cmds[i].name,name)){entry=&cmds[i];break;}
    if(!entry){ dprintf(cfd,"HTTP/1.0 404 Not Found\r\n\r\nUnknown cmd\n");return;}

    /* build final command ------------------------------------------------- */
    char final[CMD_MAXLEN]; strncpy(final, entry->base, CMD_MAXLEN-1);
    final[CMD_MAXLEN-1]=0;
    if(qs){
        char *arg=strstr(qs+1,"args="); if(arg){
            arg+=5; url_decode(arg); if(!safe_arg(arg)){
                dprintf(cfd,"HTTP/1.0 400 Bad Request\r\n\r\nBad args\n");return;}
            strncat(final," ",CMD_MAXLEN-1-strlen(final));
            strncat(final,arg ,CMD_MAXLEN-1-strlen(final));
        }
    }

    /* -------- A) fire‑and‑forget ---------------------------------------- */
    if(!entry->capture){
        char bg[CMD_MAXLEN+32];
        snprintf(bg,sizeof(bg),"%s >/dev/null 2>&1 &",final);
        system(bg);
        dprintf(cfd,
          "HTTP/1.0 200 OK\r\nContent-Type: text/plain\r\n\r\n"
          "Executing command %s …\n", entry->name);
        return;
    }

    /* -------- B) capture with timeout ----------------------------------- */
    char with_err[CMD_MAXLEN+8]; snprintf(with_err,sizeof(with_err),"%s 2>&1",final);
    FILE *pp=popen(with_err,"r"); if(!pp){dprintf(cfd,"HTTP/1.0 500\r\n\r\n");return;}
    int fd=fileno(pp); set_nonblock(fd);

    char out[OUT_MAX+128]; size_t used=0; char tmp[256]; bool timed=false;
    struct timespec st; clock_gettime(CLOCK_MONOTONIC,&st);

    while(1){
        long left=CMD_TIMEOUT - ms_since(&st);
        if(left<=0){timed=true;break;}
        struct timeval tv={.tv_sec=left/1000,.tv_usec=(left%1000)*1000};
        fd_set rf; FD_ZERO(&rf); FD_SET(fd,&rf);
        int sel=select(fd+1,&rf,NULL,NULL,&tv);
        if(sel<0){ if(errno==EINTR) continue; break; }
        if(sel==0){ timed=true; break; }
        ssize_t r=read(fd,tmp,sizeof(tmp));
        if(r<=0) break;
        if(used<OUT_MAX){
            size_t keep = (r<=OUT_MAX-used)? r : OUT_MAX-used;
            memcpy(out+used,tmp,keep); used+=keep;
        }
    }
    fclose(pp);

    if(timed){
        const char *msg="\n\nExecution exceeded max time limit...";
        size_t len=strlen(msg); if(used+len>sizeof(out)-1) len=sizeof(out)-1-used;
        memcpy(out+used,msg,len); used+=len;
    }
    out[used]=0;
    dprintf(cfd,"HTTP/1.0 200 OK\r\nContent-Type: text/plain\r\n\r\n%s\n",
            used?out:"(no output)");
}

/* ---------- main --------------------------------------------------------- */
int main(int argc,char **argv)
{
    signal(SIGPIPE,SIG_IGN); signal(SIGCHLD,SIG_IGN);

    const char *html_path=NULL,*cmd_path=NULL; int port=PORT_DEF;
    for(int i=1;i<argc;++i){
        if(!strcmp(argv[i],"--html")&&i+1<argc) html_path=argv[++i];
        else if(!strcmp(argv[i],"--commands")&&i+1<argc) cmd_path=argv[++i];
        else if(!strcmp(argv[i],"--port")&&i+1<argc) port=atoi(argv[++i]);
        else die("Usage: %s --html f --commands f [--port n]\n",argv[0]);
    }
    if(!html_path||!cmd_path) die("--html and --commands required\n");

    html_buf=slurp(html_path,&html_len); load_commands(cmd_path);

    int sfd=socket(AF_INET,SOCK_STREAM,0); if(sfd<0) die("socket: %s\n",strerror(errno));
    int on=1; setsockopt(sfd,SOL_SOCKET,SO_REUSEADDR,&on,sizeof(on));
#ifdef SO_REUSEPORT
    setsockopt(sfd,SOL_SOCKET,SO_REUSEPORT,&on,sizeof(on));
#endif
    struct sockaddr_in a={.sin_family=AF_INET,.sin_addr.s_addr=htonl(INADDR_ANY),
                          .sin_port=htons(port)};
    if(bind(sfd,(struct sockaddr*)&a,sizeof(a))<0||listen(sfd,BACKLOG)<0)
        die("bind/listen: %s\n",strerror(errno));
    set_nonblock(sfd);
    printf("tiny_srv: port %d  (html %s, commands %s)\n",port,html_path,cmd_path);

    for(;;){
        fd_set rfds; FD_ZERO(&rfds); FD_SET(sfd,&rfds); int max=sfd;
        struct sockaddr_in cli; socklen_t len=sizeof(cli);
        int c; while((c=accept(sfd,(struct sockaddr*)&cli,&len))>=0){
            set_nonblock(c); FD_SET(c,&rfds); if(c>max) max=c; }
        select(max+1,&rfds,NULL,NULL,NULL);
        for(int fd=0;fd<=max;++fd) if(FD_ISSET(fd,&rfds)&&fd!=sfd){
            handle_request(fd); close(fd);
        }
    }
    return 0;
}
