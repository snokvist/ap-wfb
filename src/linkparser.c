/*  linkparser.c  (v1.9-2025-08-03)
 *  -------------------------------------------------------------
 *  – Robust Rx-Info parser (rssi_min, fail counters, rx_rate, rssi_a/b).
 *  – Rx info shown in verbose summary.
 *  – Still prints one compact block/second, no blank line.
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

/* ---------------- data holders ---------------- */
typedef struct {
    char mac[MAC_LEN];
    int  rssi;
    char mcs[32];
    int  bw;
    int  retry;
} StaStats;

typedef struct {
    int  rssi_min;
    int  cnt_cck_fail;
    int  cnt_ofdm_fail;
    int  false_alarm;
    char rx_rate[32];
    int  rssi_a;
    int  rssi_b;
} RxInfo;

/* ---------------- helpers --------------------- */
static char *trim(char *s){ while(isspace((unsigned char)*s))++s;
    if(!*s)return s; char*e=s+strlen(s)-1; while(e>s&&isspace((unsigned char)*e))*e--='\0'; return s;}
static int first_int_after(const char *p){ while(*p && !isdigit((unsigned char)*p)) ++p;
    return *p ? atoi(p) : -1; }
static int find_mac(const char *m,char macs[][MAC_LEN],int n){ for(int i=0;i<n;++i) if(!strcasecmp(m,macs[i])) return i; return -1; }
static void reset_sta(StaStats *s,const char *mac){ strncpy(s->mac,mac,MAC_LEN); s->rssi=s->bw=s->retry=-1; strcpy(s->mcs,"NA"); }
static void reset_rx(RxInfo *r){ r->rssi_min=r->cnt_cck_fail=r->cnt_ofdm_fail=r->false_alarm=-1;
    r->rssi_a=r->rssi_b=-1; strcpy(r->rx_rate,"NA"); }

/* -------------- INI loader -------------------- */
static int load_cfg(const char *path,char macs[][MAC_LEN],int *intv,char *out_p,size_t osz,char *proc_p,size_t psz)
{
    FILE *fp=fopen(path,"r"); if(!fp){perror(path);return -1;}
    char line[LINE_SZ]; int n=0;
    while(fgets(line,sizeof line,fp)){
        char *hash=strpbrk(line,"#;"); if(hash)*hash='\0';
        char *eq=strchr(line,'='); if(!eq)continue; *eq='\0';
        char *k=trim(line),*v=trim(eq+1); if(!*k||!*v)continue;
        if(!strncmp(k,"sta",3)&&strstr(k,"_mac")){ if(n<MAX_STA) strncpy(macs[n++],v,MAC_LEN); }
        else if(!strcmp(k,"output_file")||!strcmp(k,"output_path")) strncpy(out_p,v,osz);
        else if(!strcmp(k,"interval_ms")||!strcmp(k,"interval")) *intv=atoi(v);
        else if(!strcmp(k,"proc_path")||!strcmp(k,"trx_debug_path")) strncpy(proc_p,v,psz);
    }
    fclose(fp); return n;
}

/* -------------- /proc parser ------------------ */
static void parse_proc(const char *path,char macs[][MAC_LEN],int n_sta,
                       StaStats st[],RxInfo *rx)
{
    FILE *fp=fopen(path,"r"); if(!fp){ if(verbose)perror(path); return; }
    reset_rx(rx);

    char line[LINE_SZ]; int cur=-1;
    while(fgets(line,sizeof line,fp)){
        /* --- STA header */
        if(strstr(line,"STA [")){
            char mac[MAC_LEN];
            if(sscanf(line,"%*[^[][%17[^]]",mac)==1) cur=find_mac(mac,macs,n_sta);
            else cur=-1;
            continue;
        }
        /* --- Rx Info start */
        if(strstr(line,"Rx Info dump")){ cur=-1; continue; }

        /* Rx Info lines (when cur==-1 but we're after header) */
        if(cur==-1){
            if(strstr(line,"rssi_min")){
                char *p=strstr(line,"rssi_min"); rx->rssi_min=first_int_after(p);
            }else if(strstr(line,"cnt_cck_fail")){
                char *p=strstr(line,"cnt_cck_fail"); rx->cnt_cck_fail=first_int_after(p);
                p=strstr(line,"cnt_ofdm_fail"); if(p)rx->cnt_ofdm_fail=first_int_after(p);
                p=strstr(line,"Total False Alarm"); if(p)rx->false_alarm=first_int_after(p);
            }else if(strstr(line,"rx_rate")){
                if(sscanf(line,"%*[^=]= %31s",rx->rx_rate)==1){
                    char *p=strstr(line,"rssi_a"); if(p)rx->rssi_a=first_int_after(p);
                    p=strstr(line,"rssi_b"); if(p)rx->rssi_b=first_int_after(p);
                }
            }
            continue;
        }

        /* --- Per-STA fields */
        if(strstr(line,"rssi :"))             st[cur].rssi = first_int_after(strchr(line,':'));
        else if(strstr(line,"curr_tx_rate")){ char *p=strchr(line,':'); if(p){ while(*++p&&isspace(*p)); strncpy(st[cur].mcs,p,sizeof st[cur].mcs-1); st[cur].mcs[strcspn(st[cur].mcs,"\r\n")]=0; } }
        else if(strstr(line,"curr_tx_bw"))    st[cur].bw   = first_int_after(strchr(line,':'));
        else if(strstr(line,"curr_retry_ratio")) st[cur].retry = first_int_after(strchr(line,':'));
    }
    fclose(fp);
}

/* -------------- tx-power ---------------------- */
static int get_txpower(const char *ifc){
    char cmd[64]; snprintf(cmd,sizeof cmd,"iw dev %s info 2>/dev/null",ifc);
    FILE *pp=popen(cmd,"r"); if(!pp) return -1; char l[LINE_SZ]; int tx=-1;
    while(fgets(l,sizeof l,pp)){
        char *p=strstr(l,"txpower"); if(!p)continue;
        float d; if(sscanf(p,"txpower %f",&d)==1||sscanf(p,"txpower: %f",&d)==1){ tx=(int)(d+0.5f); break; }
    } pclose(pp); return tx;
}

/* -------------- output ------------------------ */
static void write_out(const char *p,StaStats st[],int n,int tx,const RxInfo *rx){
    char tmp[256]; snprintf(tmp,sizeof tmp,"%s.tmp",p); FILE *fp=fopen(tmp,"w"); if(!fp){ if(verbose)perror(tmp); return; }
    for(int i=0;i<n;++i){
        fprintf(fp,"sta%d_rssi=%d\nsta%d_mcs=%s\nsta%d_bw=%d\nsta%d_retry=%d\n",
                i,st[i].rssi,i,st[i].mcs,i,st[i].bw,i,st[i].retry);
    }
    fprintf(fp,"txpwr=%d\n"
               "rxinfo_rssi_min=%d\n"
               "rxinfo_cnt_cck_fail=%d\n"
               "rxinfo_cnt_ofdm_fail=%d\n"
               "rxinfo_false_alarm=%d\n"
               "rxinfo_rx_rate=%s\n"
               "rxinfo_rssi_a=%d\n"
               "rxinfo_rssi_b=%d\n",
            tx,rx->rssi_min,rx->cnt_cck_fail,rx->cnt_ofdm_fail,
            rx->false_alarm,rx->rx_rate,rx->rssi_a,rx->rssi_b);
    fclose(fp); rename(tmp,p);
}

/* -------------- verbose summary -------------- */
static void summary(const StaStats s[],int n,int tx,const RxInfo *rx){
    for(int i=0;i<n;++i)
        fprintf(stderr,"STA%d %s R=%d M=%s BW=%d Re=%d\n",
                i,s[i].mac,s[i].rssi,s[i].mcs,s[i].bw,s[i].retry);
    fprintf(stderr,"RX rssi_min=%d FA=%d CCK=%d OFDM=%d Rate=%s rssi_a=%d rssi_b=%d\n",
            rx->rssi_min,rx->false_alarm,rx->cnt_cck_fail,rx->cnt_ofdm_fail,
            rx->rx_rate,rx->rssi_a,rx->rssi_b);
    fprintf(stderr,"TX=%d dBm\n",tx);
}

/* ------------------ main ---------------------- */
static void usage(const char *p){ fprintf(stderr,"Usage: %s [-c conf] [-o out] [-i ms] [-p proc] [-d iface] [-v]\n",p); }

int main(int argc,char *argv[])
{
    const char *cfg="/etc/sta_monitor.conf"; char out[256]="/tmp/sta_data.info";
    char proc[256]="/proc/net/rtl8733bu/wlan0/trx_info_debug"; const char *iface="wlan0";
    int intv=200; char macs[MAX_STA][MAC_LEN]={{0}};

    static const struct option lo[]={{"verbose",0,0,'v'},{0}};
    int o,i; while((o=getopt_long(argc,argv,"c:o:i:p:d:vh",lo,&i))!=-1){
        if(o=='c')cfg=optarg; else if(o=='o')strncpy(out,optarg,sizeof out);
        else if(o=='i')intv=atoi(optarg); else if(o=='p')strncpy(proc,optarg,sizeof proc);
        else if(o=='d')iface=optarg; else if(o=='v')verbose=true; else { usage(argv[0]); return 1; }
    }

    int n_sta=load_cfg(cfg,macs,&intv,out,sizeof out,proc,sizeof proc);
    if(n_sta<=0){ fprintf(stderr,"No STA MACs in %s\n",cfg); return 1; }

    StaStats st[MAX_STA]; RxInfo rx; struct timeval last={0,0};

    for(;;){
        for(int j=0;j<n_sta;++j) reset_sta(&st[j],macs[j]);
        parse_proc(proc,macs,n_sta,st,&rx);
        int tx=get_txpower(iface);
        write_out(out,st,n_sta,tx,&rx);

        if(verbose){
            struct timeval now; gettimeofday(&now,NULL);
            if(now.tv_sec!=last.tv_sec){ summary(st,n_sta,tx,&rx); last=now; }
        }
        usleep(intv*1000);
    }
}
