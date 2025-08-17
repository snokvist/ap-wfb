/* trafficctrl.c — single-stream MCS0–7 traffic shaper with tiny HTTP API
 * Build:  gcc -O2 -Wall -Wextra -o trafficctrl trafficctrl.c
 * 2025-08-17  v1.0  — Single loop, file-telemetry, HTB updater, /api/v1/*
 */

#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdnoreturn.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define MAX_LINE         1024
#define RES_BUFSZ        131072
#define REQ_BUFSZ        131072
#define MAX_CLIENTS      16
#define MAX_KEYS         4096
#define MAX_NAME         128
#define MAX_PATH         512

/* ---- time ---- */
static inline uint64_t now_ms(void){
  struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec*1000ull + ts.tv_nsec/1000000ull;
}

/* ---- log ---- */
static void logln(const char *fmt, ...) {
  va_list ap; va_start(ap, fmt);
  vfprintf(stderr, fmt, ap); va_end(ap);
  fputc('\n', stderr);
}

/* ---- tiny INI ---- */
typedef struct { char section[MAX_NAME]; char key[MAX_NAME]; char val[1024]; } kv_t;

static char *trim(char *s){
  if(!s) return s;
  while(*s==' '||*s=='\t'||*s=='\r'||*s=='\n') s++;
  size_t n=strlen(s);
  while(n>0 && (s[n-1]==' '||s[n-1]=='\t'||s[n-1]=='\r'||s[n-1]=='\n')) s[--n]=0;
  return s;
}
static int ini_load(const char *path, kv_t *arr, int maxn, int *out_n){
  *out_n=0;
  FILE *f=fopen(path,"r"); if(!f) return -1;
  char sec[MAX_NAME]="", line[2048];
  while(fgets(line,sizeof(line),f)){
    char *s=trim(line);
    if(!*s || *s=='#' || *s==';') continue;
    if(*s=='['){
      char *e=strchr(s,']'); if(!e) continue; *e=0;
      snprintf(sec,sizeof(sec),"%s", trim(s+1));
      continue;
    }
    char *eq=strchr(s,'='); if(!eq) continue; *eq=0;
    if(*out_n>=maxn) break;
    snprintf(arr[*out_n].section,sizeof(arr[*out_n].section),"%s",sec);
    snprintf(arr[*out_n].key,sizeof(arr[*out_n].key),"%s",trim(s));
    snprintf(arr[*out_n].val,sizeof(arr[*out_n].val),"%s",trim(eq+1));
    (*out_n)++;
  }
  fclose(f); return 0;
}
static int ini_save(const char *path, const char *tmp, kv_t *arr, int n){
  FILE *f=fopen(tmp,"w"); if(!f) return -1;
  const char *cur="";
  for(int i=0;i<n;i++){
    if(strcmp(cur, arr[i].section)!=0){
      if(arr[i].section[0]) fprintf(f,"[%s]\n",arr[i].section);
      cur=arr[i].section;
    }
    fprintf(f,"%s=%s\n", arr[i].key, arr[i].val);
  }
  fclose(f);
  if(rename(tmp,path)<0) return -1;
  return 0;
}
static int ini_get(kv_t *arr,int n,const char *sect,const char *key,char *out,size_t outsz){
  for(int i=0;i<n;i++) if((!sect[0]||strcmp(arr[i].section,sect)==0)&&strcmp(arr[i].key,key)==0){
    snprintf(out,outsz,"%s",arr[i].val); return 0;
  }
  errno=ENOENT; return -1;
}
static int ini_set(const char *path,const char *sect,const char *key,const char *val){
  kv_t arr[MAX_KEYS]; int n=0; if(ini_load(path,arr,MAX_KEYS,&n)<0) return -1;
  for(int i=0;i<n;i++){
    if(strcmp(arr[i].section,sect)==0 && strcmp(arr[i].key,key)==0){
      snprintf(arr[i].val,sizeof(arr[i].val),"%s",val);
      char tmp[MAX_PATH]; snprintf(tmp,sizeof(tmp), "%s.tmp", path);
      return ini_save(path,tmp,arr,n);
    }
  }
  if(n>=MAX_KEYS){ errno=ENOSPC; return -1; }
  /* ensure section ordering minimal: append */
  snprintf(arr[n].section,sizeof(arr[n].section),"%s",sect);
  snprintf(arr[n].key,sizeof(arr[n].key),"%s",key);
  snprintf(arr[n].val,sizeof(arr[n].val),"%s",val);
  char tmp[MAX_PATH]; snprintf(tmp,sizeof(tmp), "%s.tmp", path);
  return ini_save(path,tmp,arr,n+1);
}

/* ---- URL/query ---- */
static int hexv(char c){ if(c>='0'&&c<='9') return c-'0'; if(c>='a'&&c<='f') return c-'a'+10; if(c>='A'&&c<='F') return c-'A'+10; return -1; }
static void url_decode(char *s){
  char *o=s; for(;*s;s++){
    if(*s=='%' && hexv(s[1])>=0 && hexv(s[2])>=0){ *o++=(char)(hexv(s[1])*16+hexv(s[2])); s+=2; }
    else if(*s=='+'){ *o++=' '; }
    else *o++=*s;
  } *o=0;
}
static int query_get(const char *q,const char *name,char *out,size_t outsz){
  if(!q||!*q) return 0;
  size_t nl=strlen(name);
  const char *p=q;
  while(*p){
    const char *amp=strchr(p,'&'); size_t seg=amp? (size_t)(amp-p):strlen(p);
    const char *eq=memchr(p,'=',seg);
    if(eq && (size_t)(eq-p)==nl && strncmp(p,name,nl)==0){
      size_t vlen=seg-nl-1; if(vlen>=outsz) vlen=outsz-1;
      memcpy(out, eq+1, vlen); out[vlen]=0; url_decode(out); return 1;
    }
    if(!amp) break; p=amp+1;
  }
  return 0;
}
static int query_get_int(const char *q,const char *name,int dflt){ char b[64]; return query_get(q,name,b,sizeof(b))? atoi(b):dflt; }

/* ---- cfg ---- */
typedef struct {
  char cfg_path[MAX_PATH];
  char http_addr[128];
  char wlan[32];
  char telem_file[MAX_PATH];
  char key_mcs[32];
  char key_width[32];
  int  sample_hz;
  double alpha;
  int hysteresis_pct;
  int hysteresis_hold_ms;
  int min_dwell_ms;
  int headroom_pct;
  int stale_ms;
  double eff_10, eff_20, eff_40;
  /* marks */
  int mark_video, mark_mavlink, mark_tunnel;
  /* floors/ceils */
  int video_floor_kbps, video_ceil_max_kbps;
  int mav_floor_kbps,   mav_min_floor_kbps, mav_ceil_max_kbps;
  int tun_floor_kbps,   tun_ceil_max_kbps;
  int def_floor_kbps,   def_ceil_max_kbps;
  int ceil_margin_pct;
  /* http */
  int http_max_clients;
} config_t;

static void cfg_defaults(config_t *c){
  snprintf(c->cfg_path,sizeof(c->cfg_path), "/etc/trafficctrl.conf");
  snprintf(c->http_addr,sizeof(c->http_addr), "0.0.0.0:8084");
  snprintf(c->wlan, sizeof(c->wlan), "wlan0");
  snprintf(c->telem_file,sizeof(c->telem_file), "/tmp/aalink_ext.msg");
  snprintf(c->key_mcs,sizeof(c->key_mcs), "mcs");
  snprintf(c->key_width,sizeof(c->key_width), "width");
  c->sample_hz=10; c->alpha=0.5; c->hysteresis_pct=15; c->hysteresis_hold_ms=800; c->min_dwell_ms=800;
  c->headroom_pct=20; c->stale_ms=2500; c->ceil_margin_pct=15;
  c->eff_10=0.55; c->eff_20=0.60; c->eff_40=0.58;
  c->mark_video=1; c->mark_mavlink=10; c->mark_tunnel=20;
  c->video_floor_kbps=2000; c->video_ceil_max_kbps=120000;
  c->mav_floor_kbps=300; c->mav_min_floor_kbps=150; c->mav_ceil_max_kbps=2000;
  c->tun_floor_kbps=200; c->tun_ceil_max_kbps=3000;
  c->def_floor_kbps=5;   c->def_ceil_max_kbps=500;
  c->http_max_clients=16;
}
static int cfg_load(config_t *c, const char *path){
  kv_t arr[MAX_KEYS]; int n=0; if(ini_load(path,arr,MAX_KEYS,&n)<0) return -1;
  snprintf(c->cfg_path,sizeof(c->cfg_path), "%s", path);
  char v[256];
  if(!ini_get(arr,n,"general","http_addr",v,sizeof(v))) snprintf(c->http_addr,sizeof(c->http_addr),"%s",v);
  if(!ini_get(arr,n,"general","wlan",v,sizeof(v))) snprintf(c->wlan,sizeof(c->wlan),"%s",v);
  if(!ini_get(arr,n,"general","telem_file",v,sizeof(v))) snprintf(c->telem_file,sizeof(c->telem_file),"%s",v);
  if(!ini_get(arr,n,"general","telem_key_mcs",v,sizeof(v))) snprintf(c->key_mcs,sizeof(c->key_mcs),"%s",v);
  if(!ini_get(arr,n,"general","telem_key_width",v,sizeof(v))) snprintf(c->key_width,sizeof(c->key_width),"%s",v);
  if(!ini_get(arr,n,"general","sample_hz",v,sizeof(v))) c->sample_hz=atoi(v);
  if(!ini_get(arr,n,"general","smoothing_alpha",v,sizeof(v))) c->alpha=strtod(v,NULL);
  if(!ini_get(arr,n,"general","hysteresis_pct",v,sizeof(v))) c->hysteresis_pct=atoi(v);
  if(!ini_get(arr,n,"general","hysteresis_hold_ms",v,sizeof(v))) c->hysteresis_hold_ms=atoi(v);
  if(!ini_get(arr,n,"general","min_dwell_ms",v,sizeof(v))) c->min_dwell_ms=atoi(v);
  if(!ini_get(arr,n,"general","headroom_pct",v,sizeof(v))) c->headroom_pct=atoi(v);
  if(!ini_get(arr,n,"general","stale_ms",v,sizeof(v))) c->stale_ms=atoi(v);
  if(!ini_get(arr,n,"general","ceil_margin_pct",v,sizeof(v))) c->ceil_margin_pct=atoi(v);
  if(!ini_get(arr,n,"general","eff_10mhz",v,sizeof(v))) c->eff_10=strtod(v,NULL);
  if(!ini_get(arr,n,"general","eff_20mhz",v,sizeof(v))) c->eff_20=strtod(v,NULL);
  if(!ini_get(arr,n,"general","eff_40mhz",v,sizeof(v))) c->eff_40=strtod(v,NULL);
  if(!ini_get(arr,n,"class.video","mark",v,sizeof(v))) c->mark_video=atoi(v);
  if(!ini_get(arr,n,"class.video","floor_kbps",v,sizeof(v))) c->video_floor_kbps=atoi(v);
  if(!ini_get(arr,n,"class.video","ceil_kbps_max",v,sizeof(v))) c->video_ceil_max_kbps=atoi(v);
  if(!ini_get(arr,n,"class.mavlink","mark",v,sizeof(v))) c->mark_mavlink=atoi(v);
  if(!ini_get(arr,n,"class.mavlink","floor_kbps",v,sizeof(v))) c->mav_floor_kbps=atoi(v);
  if(!ini_get(arr,n,"class.mavlink","min_floor_kbps",v,sizeof(v))) c->mav_min_floor_kbps=atoi(v);
  if(!ini_get(arr,n,"class.mavlink","ceil_kbps_max",v,sizeof(v))) c->mav_ceil_max_kbps=atoi(v);
  if(!ini_get(arr,n,"class.tunnel","mark",v,sizeof(v))) c->mark_tunnel=atoi(v);
  if(!ini_get(arr,n,"class.tunnel","floor_kbps",v,sizeof(v))) c->tun_floor_kbps=atoi(v);
  if(!ini_get(arr,n,"class.tunnel","ceil_kbps_max",v,sizeof(v))) c->tun_ceil_max_kbps=atoi(v);
  if(!ini_get(arr,n,"class.default","floor_kbps",v,sizeof(v))) c->def_floor_kbps=atoi(v);
  if(!ini_get(arr,n,"class.default","ceil_kbps_max",v,sizeof(v))) c->def_ceil_max_kbps=atoi(v);
  if(!ini_get(arr,n,"general","http_max_clients",v,sizeof(v))) c->http_max_clients=atoi(v);
  return 0;
}

/* ---- telemetry ---- */
typedef struct { int mcs; int width; uint64_t ts_ms; bool valid; } telem_t;

/* simple key=value reader */
static int read_telem_file(const char *path, const char *kmcs, const char *kw, int *out_mcs, int *out_w){
  FILE *f=fopen(path,"r"); if(!f) return -1;
  char line[256]; int m=-1, w=-1;
  while(fgets(line,sizeof(line),f)){
    char *s=trim(line); if(!*s || *s=='#' || *s==';') continue;
    char *eq=strchr(s,'='); if(!eq) continue; *eq=0;
    char *k=trim(s), *v=trim(eq+1);
    if(strcmp(k,kmcs)==0) m=atoi(v);
    else if(strcmp(k,kw)==0) w=atoi(v);
  }
  fclose(f);
  if(m<0 || w<=0) { errno=EINVAL; return -1; }
  *out_mcs=m; *out_w=w; return 0;
}

/* ---- capacity & allocation ---- */
static double phy_20[8] = {6.5,13,19.5,26,39,52,58.5,65};
static double phy_40[8] = {13.5,27,40.5,54,81,108,121.5,135};
/* 10 MHz = half of 20 MHz */
static double phy_for(int width,int mcs){
  if(mcs<0) mcs=0; if(mcs>7) mcs=7;
  if(width==40) return phy_40[mcs];
  if(width==10) return phy_20[mcs]*0.5;
  return phy_20[mcs];
}
static double eff_for(config_t *c,int width){
  if(width==40) return c->eff_40;
  if(width==10) return c->eff_10;
  return c->eff_20;
}
typedef struct {
  int rate_video, ceil_video;
  int rate_mav,   ceil_mav;
  int rate_tun,   ceil_tun;
  int rate_def,   ceil_def;
  int alloc_total;
} rates_t;

static void allocate(config_t *cfg, int alloc_kbps, rates_t *r){
  if(alloc_kbps<100) alloc_kbps=100;
  int vfloor=cfg->video_floor_kbps;
  int mfloor=cfg->mav_floor_kbps;
  int tfloor=cfg->tun_floor_kbps;
  int dfloor=cfg->def_floor_kbps;
  int sumflo = vfloor+mfloor+tfloor+dfloor;
  int vmarg_pct=cfg->ceil_margin_pct;
  r->alloc_total=alloc_kbps;

  if(alloc_kbps < sumflo){
    double scale = (double)alloc_kbps / (double)sumflo;
    int nm = (int)(mfloor*scale);
    if(nm < cfg->mav_min_floor_kbps) nm = cfg->mav_min_floor_kbps;
    int nt = (int)(tfloor*scale);
    int nd = (int)(dfloor*scale);
    int nv = alloc_kbps - (nm+nt+nd);
    if(nv<0) nv=0;
    r->rate_mav=nm; r->rate_tun=nt; r->rate_def=nd; r->rate_video=nv;
  } else {
    int rem = alloc_kbps - (mfloor+tfloor+dfloor);
    int vr = (rem>vfloor)? rem : vfloor;
    r->rate_mav=mfloor; r->rate_tun=tfloor; r->rate_def=dfloor; r->rate_video=vr;
  }
  /* ceils */
  r->ceil_mav = (cfg->mav_ceil_max_kbps<r->rate_mav)? r->rate_mav : cfg->mav_ceil_max_kbps;
  r->ceil_tun = (cfg->tun_ceil_max_kbps<r->rate_tun)? r->rate_tun : cfg->tun_ceil_max_kbps;
  r->ceil_def = (cfg->def_ceil_max_kbps<r->rate_def)? r->rate_def : cfg->def_ceil_max_kbps;

  int vceil1 = r->rate_video + (r->rate_video * vmarg_pct)/100;
  int vceil2 = (cfg->video_ceil_max_kbps<alloc_kbps? cfg->video_ceil_max_kbps: alloc_kbps);
  r->ceil_video = vceil1<r->rate_video? r->rate_video : vceil1;
  if(r->ceil_video > vceil2) r->ceil_video = vceil2;
  if(r->ceil_video < r->rate_video) r->ceil_video = r->rate_video;
}

/* ---- tc helper ---- */
static int sh(const char *fmt, ...){
  char cmd[1024];
  va_list ap; va_start(ap, fmt); vsnprintf(cmd,sizeof(cmd),fmt,ap); va_end(ap);
  int rc=system(cmd);
  if(rc!=0) logln("tc-cmd rc=%d: %s", rc, cmd);
  return rc;
}
static void tc_setup(config_t *c){
  const char *ifn=c->wlan;
  sh("tc qdisc del dev %s root 2>/dev/null", ifn);
  sh("tc qdisc add dev %s handle 1: root htb default 100", ifn);
  sh("tc class add dev %s parent 1: classid 1:99 htb rate 100mbit ceil 100mbit", ifn);

  sh("tc class add dev %s parent 1:99 classid 1:1   htb rate %dkbit ceil %dkbit prio 2", ifn, 1000, 2000);
  sh("tc class add dev %s parent 1:99 classid 1:10  htb rate %dkbit ceil %dkbit prio 1", ifn, 300, 2000);
  sh("tc class add dev %s parent 1:99 classid 1:20  htb rate %dkbit ceil %dkbit prio 3", ifn, 200, 3000);
  sh("tc class add dev %s parent 1:99 classid 1:100 htb rate %dkbit ceil %dkbit prio 4", ifn, 5,   500);

  if(sh("tc qdisc add dev %s parent 1:1   fq_codel 2>/dev/null", ifn)!=0) sh("tc qdisc add dev %s parent 1:1   pfifo", ifn);
  if(sh("tc qdisc add dev %s parent 1:10  fq_codel 2>/dev/null", ifn)!=0) sh("tc qdisc add dev %s parent 1:10  pfifo", ifn);
  if(sh("tc qdisc add dev %s parent 1:20  fq_codel 2>/dev/null", ifn)!=0) sh("tc qdisc add dev %s parent 1:20  pfifo", ifn);
  if(sh("tc qdisc add dev %s parent 1:100 pfifo", ifn)!=0) {}

  sh("tc filter add dev %s parent 1: protocol ip prio 1 handle %d fw flowid 1:1",   ifn, c->mark_video);
  sh("tc filter add dev %s parent 1: protocol ip prio 1 handle %d fw flowid 1:10",  ifn, c->mark_mavlink);
  sh("tc filter add dev %s parent 1: protocol ip prio 1 handle %d fw flowid 1:20",  ifn, c->mark_tunnel);
}
static void tc_apply_rates(config_t *c, const rates_t *r){
  const char *ifn=c->wlan;
  sh("tc class change dev %s classid 1:1   htb rate %dkbit ceil %dkbit prio 2", ifn, r->rate_video, r->ceil_video);
  sh("tc class change dev %s classid 1:10  htb rate %dkbit ceil %dkbit prio 1", ifn, r->rate_mav,   r->ceil_mav);
  sh("tc class change dev %s classid 1:20  htb rate %dkbit ceil %dkbit prio 3", ifn, r->rate_tun,   r->ceil_tun);
  sh("tc class change dev %s classid 1:100 htb rate %dkbit ceil %dkbit prio 4", ifn, r->rate_def,   r->ceil_def);
}

/* ---- HTTP ---- */
typedef struct {
  int fd;
  char req[REQ_BUFSZ]; size_t rlen;
  size_t content_len;
  char method[8], path[1024], query[1024];
  char body[REQ_BUFSZ];
} conn_t;

static void http_send(int fd, const char *ct, const char *fmt, ...){
  char body[RES_BUFSZ];
  va_list ap; va_start(ap,fmt); int blen=vsnprintf(body,sizeof(body),fmt,ap); va_end(ap);
  if(blen<0) blen=0; if(blen>(int)sizeof(body)) blen=sizeof(body);
  char hdr[512];
  int hlen=snprintf(hdr,sizeof(hdr),
    "HTTP/1.1 200 OK\r\nContent-Type: %s\r\nContent-Length: %d\r\nConnection: close\r\nCache-Control: no-store\r\nPragma: no-cache\r\n\r\n",
    ct, blen);
  (void)write(fd, hdr, hlen);
  (void)write(fd, body, blen);
}
static void http_err(int fd, int code, const char *msg){
  char body[256]; int blen=snprintf(body,sizeof(body), "{\"error\":%d,\"message\":\"%s\"}", code, msg?msg:"");
  char hdr[256]; int hlen=snprintf(hdr,sizeof(hdr),
    "HTTP/1.1 %d ERR\r\nContent-Type: application/json\r\nContent-Length: %d\r\nConnection: close\r\n\r\n",
    code, blen);
  (void)write(fd, hdr, hlen); (void)write(fd, body, blen);
}
static int tcp_listen(const char *bind_addr, int backlog){
  char ip[128]={0}; int port=0;
  const char *colon=strrchr(bind_addr, ':'); if(!colon) return -1;
  size_t iplen=(size_t)(colon-bind_addr); if(iplen>=sizeof(ip)) return -1;
  memcpy(ip,bind_addr,iplen); ip[iplen]=0; port=atoi(colon+1);
  int fd=socket(AF_INET,SOCK_STREAM,0); if(fd<0) return -1;
  int one=1; setsockopt(fd,SOL_SOCKET,SO_REUSEADDR,&one,sizeof(one));
  struct sockaddr_in sa; memset(&sa,0,sizeof(sa)); sa.sin_family=AF_INET; sa.sin_port=htons(port);
  if(strlen(ip)==0 || strcmp(ip,"0.0.0.0")==0) sa.sin_addr.s_addr=INADDR_ANY;
  else inet_pton(AF_INET,ip,&sa.sin_addr);
  if(bind(fd,(struct sockaddr*)&sa,sizeof(sa))<0){ close(fd); return -1; }
  if(listen(fd, backlog>0 ? backlog : 16)<0){ close(fd); return -1; }
  fcntl(fd,F_SETFL, fcntl(fd,F_GETFL,0)|O_NONBLOCK);
  return fd;
}
static int parse_request(conn_t *c){
  c->req[c->rlen]=0;
  char *hdrs=strstr(c->req,"\r\n\r\n"); if(!hdrs) hdrs=strstr(c->req,"\n\n"); if(!hdrs) return 0;
  size_t head_len = (size_t)(hdrs - c->req) + ((hdrs[0]=='\r'&&hdrs[1]=='\n')?4:2);
  char proto[16]={0}, url[1024]={0};
  if(sscanf(c->req,"%7s %1023s %15s",c->method,url,proto)!=3) return -1;
  char *q=strchr(url,'?'); if(q){ *q=0; snprintf(c->path,sizeof(c->path),"%s",url); snprintf(c->query,sizeof(c->query),"%s",q+1); }
  else { snprintf(c->path,sizeof(c->path),"%s",url); c->query[0]=0; }
  char *cl=strcasestr(c->req,"Content-Length:"); c->content_len=0;
  if(cl){ c->content_len=(size_t)atoi(cl+15); }
  size_t body_have = c->rlen - head_len;
  if(body_have < c->content_len) return 0;
  if(c->content_len>0){
    size_t tocopy = c->content_len<sizeof(c->body)-1? c->content_len: sizeof(c->body)-1;
    memcpy(c->body, c->req+head_len, tocopy); c->body[tocopy]=0;
  } else c->body[0]=0;
  return 1;
}

/* ---- world ---- */
static volatile sig_atomic_t want_reload_sig=0;
static void on_hup(int s){ (void)s; want_reload_sig=1; }

static config_t ccfg;
static int lfd=-1;

static uint64_t start_ms=0;
static uint64_t last_tc_ms=0;
static uint64_t last_hold_start_ms=0;

static int last_mcs=-1, last_width=-1;
static uint64_t last_telem_ms=0;
static double sm_alloc_kbps=0.0;
static int last_applied_alloc=-1;

static void json_ok(int fd){ http_send(fd,"application/json","{\"ok\":1}"); }

/* keys endpoint (flat/tree with values) */
static int cmp_kv(const void *a,const void *b){
  const kv_t *ka=(const kv_t*)a, *kb=(const kv_t*)b;
  int s=strcmp(ka->section,kb->section); if(s) return s; return strcmp(ka->key,kb->key);
}
static void handle_keys(int fd, const char *path, const char *q){
  kv_t arr[MAX_KEYS]; int n=0; if(ini_load(path,arr,MAX_KEYS,&n)<0){ http_err(fd,404,"no config"); return; }
  char fmt[16]="flat", want_values_buf[8]="0", section[128]="", prefix[256]="", sortbuf[8]="1";
  (void)query_get(q,"format",fmt,sizeof(fmt));
  (void)query_get(q,"values",want_values_buf,sizeof(want_values_buf));
  (void)query_get(q,"section",section,sizeof(section));
  (void)query_get(q,"prefix",prefix,sizeof(prefix));
  (void)query_get(q,"sort",sortbuf,sizeof(sortbuf));
  bool want_values = (strcmp(want_values_buf,"1")==0 || strcasecmp(want_values_buf,"true")==0);
  bool do_sort = (strcmp(sortbuf,"1")==0 || strcasecmp(sortbuf,"true")==0);
  if(do_sort) qsort(arr,n,sizeof(kv_t),cmp_kv);

  if(strcmp(fmt,"tree")==0){
    char out[RES_BUFSZ]; int off=0; off+=snprintf(out+off,sizeof(out)-off,"{\"sections\":{");
    const char *cur=""; int first_sec=1;
    for(int i=0;i<n;i++){
      if(section[0] && strcmp(arr[i].section,section)!=0) continue;
      if(prefix[0] && strncmp(arr[i].key,prefix,strlen(prefix))!=0) continue;
      if(strcmp(cur,arr[i].section)!=0){
        if(!first_sec){ if(off>0 && out[off-1]==',') off--; off+=snprintf(out+off,sizeof(out)-off,"},"); }
        cur=arr[i].section; first_sec=0;
        off+=snprintf(out+off,sizeof(out)-off, "\"%s\":{", cur);
      }
      if(want_values){
        /* escape minimal */
        char ev[1024]; int eo=0; const char *p=arr[i].val;
        for(;*p && eo<(int)sizeof(ev)-2;p++){ if(*p=='\\'||*p=='"'){ ev[eo++]='\\'; ev[eo++]=*p; }
          else if(*p=='\n'){ ev[eo++]='\\'; ev[eo++]='n'; } else ev[eo++]=*p; } ev[eo]=0;
        off+=snprintf(out+off,sizeof(out)-off,"\"%s\":\"%s\",", arr[i].key, ev);
      }else{
        off+=snprintf(out+off,sizeof(out)-off,"\"%s\",", arr[i].key);
      }
      if(off>(int)sizeof(out)-128) break;
    }
    if(!first_sec){ if(off>0 && out[off-1]==',') off--; off+=snprintf(out+off,sizeof(out)-off,"}"); }
    off+=snprintf(out+off,sizeof(out)-off,"},\"count\":%d}", n);
    http_send(fd,"application/json","%.*s", off, out);
  }else{
    char out[RES_BUFSZ]; int off=0; off+=snprintf(out+off,sizeof(out)-off,"{\"keys\":[");
    for(int i=0;i<n;i++){
      if(section[0] && strcmp(arr[i].section,section)!=0) continue;
      if(prefix[0] && strncmp(arr[i].key,prefix,strlen(prefix))!=0) continue;
      if(want_values){
        char ev[1024]; int eo=0; const char *p=arr[i].val;
        for(;*p && eo<(int)sizeof(ev)-2;p++){ if(*p=='\\'||*p=='"'){ ev[eo++]='\\'; ev[eo++]=*p; }
          else if(*p=='\n'){ ev[eo++]='\\'; ev[eo++]='n'; } else ev[eo++]=*p; } ev[eo]=0;
        off+=snprintf(out+off,sizeof(out)-off,"{\"k\":\"%s.%s\",\"v\":\"%s\"},", arr[i].section,arr[i].key,ev);
      }else{
        off+=snprintf(out+off,sizeof(out)-off,"\"%s.%s\",", arr[i].section, arr[i].key);
      }
      if(off>(int)sizeof(out)-128) break;
    }
    if(off>0 && out[off-1]==',') off--;
    off+=snprintf(out+off,sizeof(out)-off,"],\"count\":%d}", n);
    http_send(fd,"application/json","%.*s", off, out);
  }
}

/* ---- API handlers ---- */
static void handle_get_config(int fd, const char *path){
  FILE *f=fopen(path,"r"); if(!f){ http_err(fd,404,"no config"); return; }
  struct stat st; fstat(fileno(f), &st);
  size_t sz=(size_t)st.st_size; if(sz>RES_BUFSZ-1) sz=RES_BUFSZ-1;
  char *content=(char*)malloc(sz+1); if(!content){ fclose(f); http_err(fd,500,"oom"); return; }
  size_t rd=fread(content,1,sz,f); content[rd]=0; fclose(f);
  http_send(fd,"text/plain","%s",content); free(content);
}
static void handle_post_config(int fd, const char *path, const char *body, size_t blen){
  char tmp[MAX_PATH]; snprintf(tmp,sizeof(tmp), "%s.tmp", path);
  FILE *f=fopen(tmp,"w"); if(!f){ http_err(fd,500,"write tmp"); return; }
  fwrite(body,1,blen,f); fclose(f);
  if(rename(tmp,path)<0){ http_err(fd,500,"rename"); return; }
  json_ok(fd); want_reload_sig=1;
}
static void handle_get_kv(int fd, const char *path, const char *q){
  char sk[256]; if(!query_get(q,"key",sk,sizeof(sk))){ http_err(fd,400,"missing key"); return; }
  char sect[MAX_NAME]="", key[MAX_NAME]=""; char *dot=strchr(sk,'.');
  if(dot){ snprintf(sect,sizeof(sect),"%.*s",(int)(dot-sk)); snprintf(key,sizeof(key),"%s",dot+1); }
  else { sect[0]=0; snprintf(key,sizeof(key),"%s",sk); }
  kv_t arr[MAX_KEYS]; int n=0; if(ini_load(path,arr,MAX_KEYS,&n)<0){ http_err(fd,404,"no config"); return; }
  for(int i=0;i<n;i++) if((!sect[0]||strcmp(arr[i].section,sect)==0)&&strcmp(arr[i].key,key)==0){
    http_send(fd,"application/json","{\"value\":\"%s\"}", arr[i].val); return;
  }
  http_err(fd,404,"not found");
}
static void handle_set_kv(int fd, const char *path, const char *q){
  char sk[256]; if(!query_get(q,"key",sk,sizeof(sk))){ http_err(fd,400,"missing key"); return; }
  char val[1024]; if(!query_get(q,"value",val,sizeof(val))){ http_err(fd,400,"missing value"); return; }
  char sect[MAX_NAME]="", key[MAX_NAME]=""; char *dot=strchr(sk,'.');
  if(dot){ snprintf(sect,sizeof(sect),"%.*s",(int)(dot-sk)); snprintf(key,sizeof(key),"%s",dot+1); }
  else { sect[0]=0; snprintf(key,sizeof(key),"%s",sk); }
  if(ini_set(ccfg.cfg_path,sect,key,val)<0){ http_err(fd,500,"set failed"); return; }
  json_ok(fd); want_reload_sig=1;
}
static void handle_status(int fd, int mcs, int width, int alloc_kbps, const rates_t *r, double eff, double phy, int usable_kbps){
  http_send(fd,"application/json",
    "{"
    "\"wlan\":\"%s\","
    "\"link\":{\"mcs\":%d,\"width\":%d,\"phy_mbps\":%.1f,\"eff\":%.2f,"
      "\"usable_kbps\":%d,\"headroom_pct\":%d,\"alloc_kbps\":%d,"
      "\"provider_file\":\"%s\",\"last_telem_ms\":%llu},"
    "\"classes\":["
      "{\"name\":\"video\",\"cid\":\"1:1\",\"mark\":%d,\"rate_kbps\":%d,\"ceil_kbps\":%d},"
      "{\"name\":\"mavlink\",\"cid\":\"1:10\",\"mark\":%d,\"rate_kbps\":%d,\"ceil_kbps\":%d},"
      "{\"name\":\"tunnel\",\"cid\":\"1:20\",\"mark\":%d,\"rate_kbps\":%d,\"ceil_kbps\":%d},"
      "{\"name\":\"default\",\"cid\":\"1:100\",\"rate_kbps\":%d,\"ceil_kbps\":%d}"
    "],"
    "\"tc_last_update_ms\":%llu"
    "}",
    ccfg.wlan, mcs, width, phy, eff, usable_kbps, ccfg.headroom_pct, alloc_kbps,
    ccfg.telem_file, (unsigned long long)(now_ms()-last_telem_ms),
    ccfg.mark_video, r->rate_video, r->ceil_video,
    ccfg.mark_mavlink, r->rate_mav, r->ceil_mav,
    ccfg.mark_tunnel, r->rate_tun, r->ceil_tun,
    r->rate_def, r->ceil_def,
    (unsigned long long)(now_ms()-last_tc_ms)
  );
}

/* ---- server loop helpers ---- */
static int accept_client(int lfd){ struct sockaddr_in sa; socklen_t sl=sizeof(sa); int fd=accept(lfd,(struct sockaddr*)&sa,&sl); if(fd<0) return -1; fcntl(fd,F_SETFL, fcntl(fd,F_GETFL,0)|O_NONBLOCK); return fd; }
static int is_get(const char *m){ return strcmp(m,"GET")==0; }
static int is_post(const char *m){ return strcmp(m,"POST")==0; }
static int is_path(const char *p,const char *want){ return strcmp(p,want)==0 || strcmp(p,want+9)==0; } /* allow legacy w/o /api/v1 */

static void ensure_default_conf(const char *path){
  struct stat st; if(stat(path,&st)==0) return;
  FILE *f=fopen(path,"w");
  if(!f){ logln("cannot create default conf %s", path); return; }
  fprintf(f,
"[general]\n"
"wlan=wlan0\n"
"http_addr=0.0.0.0:8084\n"
"telem_file=/tmp/aalink_ext.msg\n"
"telem_key_mcs=mcs\n"
"telem_key_width=width\n"
"sample_hz=10\n"
"smoothing_alpha=0.5\n"
"hysteresis_pct=15\n"
"hysteresis_hold_ms=800\n"
"min_dwell_ms=800\n"
"headroom_pct=20\n"
"stale_ms=2500\n"
"ceil_margin_pct=15\n"
"eff_10mhz=0.55\n"
"eff_20mhz=0.60\n"
"eff_40mhz=0.58\n"
"http_max_clients=16\n"
"\n[class.video]\nmark=1\nfloor_kbps=2000\nceil_kbps_max=120000\n"
"\n[class.mavlink]\nmark=10\nfloor_kbps=300\nmin_floor_kbps=150\nceil_kbps_max=2000\n"
"\n[class.tunnel]\nmark=20\nfloor_kbps=200\nceil_kbps_max=3000\n"
"\n[class.default]\nfloor_kbps=5\nceil_kbps_max=500\n");
  fclose(f);
}

/* ---- main serve loop ---- */
int main(int argc, char **argv){
  cfg_defaults(&ccfg);
  if(argc>1) snprintf(ccfg.cfg_path,sizeof(ccfg.cfg_path), "%s", argv[1]);
  ensure_default_conf(ccfg.cfg_path);
  cfg_load(&ccfg, ccfg.cfg_path);

  signal(SIGPIPE, SIG_IGN);
  signal(SIGHUP, on_hup);

  lfd = tcp_listen(ccfg.http_addr, ccfg.http_max_clients);
  if(lfd<0){ fprintf(stderr,"bind %s failed\n", ccfg.http_addr); return 1; }

  tc_setup(&ccfg);

  start_ms = now_ms();
  uint64_t tick_ms = (ccfg.sample_hz>0? (1000/ccfg.sample_hz):100);
  if(tick_ms<10) tick_ms=10;

  conn_t clients[MAX_CLIENTS]; memset(clients,0,sizeof(clients));
  int cuse[MAX_CLIENTS]; for(int i=0;i<MAX_CLIENTS;i++) cuse[i]=0;

  while(1){
    if(want_reload_sig){
      want_reload_sig=0;
      cfg_load(&ccfg, ccfg.cfg_path);
      tc_setup(&ccfg);
      last_applied_alloc=-1; /* force re-apply */
    }

    /* telemetry tick + shaping */
    static uint64_t last_tick=0;
    uint64_t now=now_ms();
    if(now - last_tick >= tick_ms){
      last_tick = now;
      int m=-1, w=-1;
      if(read_telem_file(ccfg.telem_file, ccfg.key_mcs, ccfg.key_width, &m, &w)==0){
        last_mcs=m; last_width=w; last_telem_ms=now;
      }
      int use_m = last_mcs, use_w = last_width;
      if(use_m<0 || use_w<=0 || (now - last_telem_ms) > (uint64_t)ccfg.stale_ms){
        use_m=0; use_w=20; /* fallback */
      }
      double phy = phy_for(use_w, use_m);
      double eff = eff_for(&ccfg, use_w);
      int usable_kbps = (int)(phy * 1000.0 * eff + 0.5);
      int alloc_kbps = (int)(usable_kbps * (100 - ccfg.headroom_pct) / 100);
      if(alloc_kbps<100) alloc_kbps=100;

      if(sm_alloc_kbps<=0.1) sm_alloc_kbps = alloc_kbps;
      else sm_alloc_kbps = ccfg.alpha*alloc_kbps + (1.0-ccfg.alpha)*sm_alloc_kbps;

      static int hold_active=0;
      static int last_target=-1;
      int target = (int)(sm_alloc_kbps + 0.5);

      int diff = (last_applied_alloc<0)? 100 : abs(target - last_applied_alloc);
      int pct  = (last_applied_alloc<=0)? 100 : (diff*100)/(last_applied_alloc? last_applied_alloc:1);

      if(pct >= ccfg.hysteresis_pct){
        if(!hold_active){ hold_active=1; last_hold_start_ms=now; }
        if(now - last_hold_start_ms >= (uint64_t)ccfg.hysteresis_hold_ms && now - last_tc_ms >= (uint64_t)ccfg.min_dwell_ms){
          rates_t rr; allocate(&ccfg, target, &rr);
          tc_apply_rates(&ccfg, &rr);
          last_tc_ms = now;
          last_applied_alloc = target;
          hold_active=0;
        }
      } else { hold_active=0; last_target=target; }

      /* keep some status ready: we can recompute when /status hits to avoid drift */
    }

    /* accept + serve */
    fd_set rfds; FD_ZERO(&rfds);
    int maxfd=lfd; FD_SET(lfd,&rfds);
    for(int i=0;i<MAX_CLIENTS;i++){ if(cuse[i]){ FD_SET(clients[i].fd,&rfds); if(clients[i].fd>maxfd) maxfd=clients[i].fd; } }
    struct timeval tv; tv.tv_sec=0; tv.tv_usec=10000; /* 10ms */
    int rv=select(maxfd+1,&rfds,NULL,NULL,&tv);
    if(rv<0){ if(errno==EINTR) continue; break; }

    if(FD_ISSET(lfd,&rfds)){
      int cfd=accept_client(lfd);
      if(cfd>=0){
        int slot=-1;
        for(int i=0;i<MAX_CLIENTS;i++) if(!cuse[i]){ slot=i; break; }
        if(slot<0){ close(cfd); } else { memset(&clients[slot],0,sizeof(conn_t)); clients[slot].fd=cfd; cuse[slot]=1; }
      }
    }
    for(int i=0;i<MAX_CLIENTS;i++) if(cuse[i] && FD_ISSET(clients[i].fd,&rfds)){
      conn_t *c=&clients[i];
      ssize_t rd=read(c->fd, c->req+c->rlen, sizeof(c->req)-1-c->rlen);
      if(rd<=0){ close(c->fd); cuse[i]=0; continue; }
      c->rlen+= (size_t)rd; if(c->rlen>=sizeof(c->req)-1){ close(c->fd); cuse[i]=0; continue; }
      int pr=parse_request(c); if(pr==0) continue; /* need more */
      /* route */
      bool handled=false;
      if(is_get(c->method) && (is_path(c->path,"/api/v1/status") || is_path(c->path,"/status"))){
        /* recompute status quickly from current smoothed/baseline */
        int use_m = (last_mcs>=0)? last_mcs:0;
        int use_w = (last_width>0)? last_width:20;
        if((now_ms()-last_telem_ms) > (uint64_t)ccfg.stale_ms){ use_m=0; use_w=20; }
        double phy=phy_for(use_w,use_m), eff=eff_for(&ccfg,use_w);
        int usable_kbps=(int)(phy*1000.0*eff + 0.5);
        int alloc_kbps=(int)(usable_kbps * (100 - ccfg.headroom_pct) / 100);
        if(alloc_kbps<100) alloc_kbps=100;
        rates_t rr; allocate(&ccfg, (int)(sm_alloc_kbps>0? sm_alloc_kbps:alloc_kbps), &rr);
        handle_status(c->fd, use_m, use_w, rr.alloc_total, &rr, eff, phy, usable_kbps);
        handled=true;
      } else if(is_get(c->method) && (is_path(c->path,"/api/v1/config") || is_path(c->path,"/config"))){
        handle_get_config(c->fd, ccfg.cfg_path); handled=true;
      } else if(is_post(c->method) && (is_path(c->path,"/api/v1/config") || is_path(c->path,"/config"))){
        handle_post_config(c->fd, ccfg.cfg_path, c->body, c->content_len); handled=true;
      } else if(is_get(c->method) && (is_path(c->path,"/api/v1/get") || is_path(c->path,"/get"))){
        handle_get_kv(c->fd, ccfg.cfg_path, c->query); handled=true;
      } else if(is_post(c->method) && (is_path(c->path,"/api/v1/set") || is_path(c->path,"/set"))){
        handle_set_kv(c->fd, ccfg.cfg_path, c->query); handled=true;
      } else if(is_post(c->method) && (is_path(c->path,"/api/v1/action/reload") || is_path(c->path,"/action/reload") || is_path(c->path,"/reload"))){
        json_ok(c->fd); want_reload_sig=1; handled=true;
      } else if(is_get(c->method) && (is_path(c->path,"/api/v1/keys") || is_path(c->path,"/keys"))){
        handle_keys(c->fd, ccfg.cfg_path, c->query); handled=true;
      }
      if(!handled) http_err(c->fd,404,"no route");
      close(c->fd); cuse[i]=0;
    }
  }
  return 0;
}
