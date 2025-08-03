/*  linkparser.c  (v2.0-2025-08-03)
 *  -------------------------------------------------------------
 *  + Adds active-STA detection & outputs:
 *        active_sta            <-- index (0-based) of strongest STA
 *        active_sta_rssi       <-- its RSSI
 *        staX_active           <-- 1 for active, 0 otherwise
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>
#include <stdint.h>
#include <errno.h>
#include <stdbool.h>
#include <sys/time.h>
#include <getopt.h>

#define MAX_STA   16
#define MAC_LEN   18
#define LINE_SZ   512

static bool verbose=false;

/* ---------------- data ---------------- */
typedef struct {
    char mac[MAC_LEN];
    int  rssi, bw, retry;
    char mcs[32];
    int  active;              /* 1 if this STA is the ‘active’ one */
} StaStats;

typedef struct {
    int  rssi_min, cnt_cck_fail, cnt_ofdm_fail, false_alarm;
    char rx_rate[32];
    int  rssi_a, rssi_b;
} RxInfo;

/* --------------- helpers -------------- */
static char *trim(char *s){ while(isspace((unsigned char)*s))++s;
    if(!*s)return s; char*e=s+strlen(s)-1; while(e>s&&isspace((unsigned char)*e))*e--='\0'; return s;}
static int first_int(const char *p){ while(*p && !isdigit((unsigned char)*p)) ++p; return *p?atoi(p):-1; }
static void reset_rx(RxInfo *r){ r->rssi_min=r->cnt_cck_fail=r->cnt_ofdm_fail=r->false_alarm=-1;
    r->rssi_a=r->rssi_b=-1; strcpy(r->rx_rate,"NA"); }

/* --------------- INI ------------------- */
static int load_cfg(const char *file,char macs[][MAC_LEN],int *intv,char *out,size_t os,char *proc,size_t ps)
{
    FILE *fp=fopen(file,"r"); if(!fp){perror(file);return-1;}
    char l[LINE_SZ]; int n=0;
    while(fgets(l,sizeof l,fp)){
        char *c=strpbrk(l,"#;"); if(c) *c='\0';
        char *eq=strchr(l,'='); if(!eq)continue; *eq='\0';
        char *k=trim(l),*v=trim(eq+1); if(!*k||!*v)continue;
        if(!strncmp(k,"sta",3)&&strstr(k,"_mac")){ if(n<MAX_STA) strncpy(macs[n++],v,MAC_LEN); }
        else if(!strcmp(k,"output_file")||!strcmp(k,"output_path")) strncpy(out,v,os);
        else if(!strcmp(k,"interval_ms")||!strcmp(k,"interval")) *intv=atoi(v);
        else if(!strcmp(k,"proc_path")||!strcmp(k,"trx_debug_path")) strncpy(proc,v,ps);
    }
    fclose(fp); return n;
}

/* --------------- parsing --------------- */
static int find_mac(const char *m,char macs[][MAC_LEN],int n){ for(int i=0;i<n;++i) if(!strcasecmp(m,macs[i])) return i; return -1; }

static void reset_sta(StaStats *s,const char *mac){ strncpy(s->mac,mac,MAC_LEN); s->rssi=s->bw=s->retry=-1; s->active=0; strcpy(s->mcs,"NA"); }

static void parse_proc(const char *path,char macs[][MAC_LEN],int n,StaStats st[],RxInfo *rx)
{
    FILE *fp=fopen(path,"r"); if(!fp){ if(verbose)perror(path); return; }
    reset_rx(rx);

    char line[LINE_SZ]; int cur=-1;
    while(fgets(line,sizeof line,fp)){
        if(strstr(line,"STA [")){                      /* STA header */
            char mac[MAC_LEN];
            if(sscanf(line,"%*[^[][%17[^]]",mac)==1) cur=find_mac(mac,macs,n);
            else cur=-1;
            continue;
        }
        if(strstr(line,"Rx Info dump")){ cur=-1; continue; }

        /* ------------ RX info ------------ */
        if(cur==-1){
            if(strstr(line,"rssi_min"))            rx->rssi_min     = first_int(line);
            else if(strstr(line,"cnt_cck_fail")){  rx->cnt_cck_fail = first_int(line);
                char *p=strstr(line,"cnt_ofdm_fail"); if(p)rx->cnt_ofdm_fail=first_int(p);
                p=strstr(line,"Total False Alarm"); if(p)rx->false_alarm   =first_int(p);
            }else if(strstr(line,"rx_rate")){
                if(sscanf(line,"%*[^=]= %31s",rx->rx_rate)==1){
                    char *p=strstr(line,"rssi_a"); if(p)rx->rssi_a=first_int(p);
                    p=strstr(line,"rssi_b"); if(p)rx->rssi_b=first_int(p);
                }
            }
            continue;
        }

        /* ------------ STA fields ---------- */
        if(strstr(line,"rssi :"))               st[cur].rssi  = first_int(line);
        else if(strstr(line,"curr_tx_rate")){   char *p=strchr(line,':'); if(p){ while(*++p&&isspace(*p)); strncpy(st[cur].mcs,p,sizeof st[cur].mcs-1); st[cur].mcs[strcspn(st[cur].mcs,"\r\n")]=0; } }
        else if(strstr(line,"curr_tx_bw"))      st[cur].bw    = first_int(line);
        else if(strstr(line,"curr_retry_ratio"))st[cur].retry = first_int(line);
    }
    fclose(fp);
}

/* --------------- tx-power --------------- */
static int txpower(const char *ifc){
    char cmd[64]; snprintf(cmd,sizeof cmd,"iw dev %s info 2>/dev/null",ifc);
    FILE *pp=popen(cmd,"r"); if(!pp)return-1; char l[LINE_SZ]; int tx=-1;
    while(fgets(l,sizeof l,pp)){ char *p=strstr(l,"txpower"); if(!p)continue;
        float d; if(sscanf(p,"txpower %f",&d)==1||sscanf(p,"txpower: %f",&d)==1){ tx=(int)(d+0.5f); break; } }
    pclose(pp); return tx;
}

/* --------------- output ----------------- */
static void write_out(const char *file,StaStats st[],int n,int tx,const RxInfo *rx,int active_idx)
{
    char tmp[256]; snprintf(tmp,sizeof tmp,"%s.tmp",file);
    FILE *f=fopen(tmp,"w"); if(!f){ if(verbose)perror(tmp); return; }

    /* per-STA */
    for(int i=0;i<n;++i){
        fprintf(f,"sta%d_rssi=%d\nsta%d_mcs=%s\nsta%d_bw=%d\nsta%d_retry=%d\nsta%d_active=%d\n",
                i,st[i].rssi,i,st[i].mcs,i,st[i].bw,i,st[i].retry,i,st[i].active);
    }
    /* global */
    fprintf(f,"active_sta=%d\nactive_sta_rssi=%d\n",active_idx,
            active_idx>=0?st[active_idx].rssi:-1);
    fprintf(f,"txpwr=%d\n"
              "rxinfo_rssi_min=%d\nrxinfo_cnt_cck_fail=%d\nrxinfo_cnt_ofdm_fail=%d\n"
              "rxinfo_false_alarm=%d\nrxinfo_rx_rate=%s\nrxinfo_rssi_a=%d\nrxinfo_rssi_b=%d\n",
            tx,rx->rssi_min,rx->cnt_cck_fail,rx->cnt_ofdm_fail,rx->false_alarm,
            rx->rx_rate,rx->rssi_a,rx->rssi_b);
    fclose(f); rename(tmp,file);
}

/* --------------- summary ---------------- */
static void summary(const StaStats s[],int n,int tx,const RxInfo *rx,int active)
{
    for(int i=0;i<n;++i)
        fprintf(stderr,"%sSTA%d %s R=%d M=%s BW=%d Re=%d\n",
                i==active?"*":" ",i,s[i].mac,s[i].rssi,s[i].mcs,s[i].bw,s[i].retry);
    fprintf(stderr,"RX rssi_min=%d FA=%d CCK=%d OFDM=%d Rate=%s rssi_a=%d rssi_b=%d\nTX=%d dBm\n",
            rx->rssi_min,rx->false_alarm,rx->cnt_cck_fail,rx->cnt_ofdm_fail,
            rx->rx_rate,rx->rssi_a,rx->rssi_b,tx);
}

/* ---------------- main ----------------- */
static void usage(const char *p){ fprintf(stderr,"Usage: %s [-c conf] [-o out] [-i ms] [-p proc] [-d iface] [-v]\n",p); }

int main(int argc,char *argv[])
{
    const char *cfg="/etc/sta_monitor.conf";
    char out[256]="/tmp/sta_data.info";
    char proc[256]="/proc/net/rtl8733bu/wlan0/trx_info_debug";
    const char *iface="wlan0";
    int intv=200; char macs[MAX_STA][MAC_LEN]={{0}};

    static const struct option lo[]={{"verbose",0,0,'v'},{0}};
    int o,i; while((o=getopt_long(argc,argv,"c:o:i:p:d:vh",lo,&i))!=-1){
        if(o=='c')cfg=optarg; else if(o=='o')strncpy(out,optarg,sizeof out);
        else if(o=='i')intv=atoi(optarg); else if(o=='p')strncpy(proc,optarg,sizeof proc);
        else if(o=='d')iface=optarg; else if(o=='v')verbose=true; else{ usage(argv[0]); return 1; }
    }

    int n=load_cfg(cfg,macs,&intv,out,sizeof out,proc,sizeof proc);
    if(n<=0){ fprintf(stderr,"No STA MACs\n"); return 1; }

    StaStats st[MAX_STA]; RxInfo rx; struct timeval last={0,0};

    for(;;){
        for(int j=0;j<n;++j) reset_sta(&st[j],macs[j]);
        parse_proc(proc,macs,n,st,&rx);

        /* determine active STA (strongest RSSI) */
        int active=-1,best=-2;
        for(int j=0;j<n;++j) if(st[j].rssi>best){ best=st[j].rssi; active=j; }
        if(active>=0) st[active].active=1;

        int tx=txpower(iface);
        write_out(out,st,n,tx,&rx,active);

        if(verbose){
            struct timeval now; gettimeofday(&now,NULL);
            if(now.tv_sec!=last.tv_sec){ summary(st,n,tx,&rx,active); last=now; }
        }
        usleep(intv*1000);
    }
}
