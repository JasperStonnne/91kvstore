#define _POSIX_C_SOURCE 200809L
#include <arpa/inet.h>
#include <errno.h>
#include <getopt.h>
#include <math.h>
#include <poll.h>
#include <netinet/tcp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
static void die(const char *s){perror(s);exit(1);}
static uint64_t ns(clockid_t id){struct timespec t;if(clock_gettime(id,&t))die("clock");return (uint64_t)t.tv_sec*1000000000+t.tv_nsec;}
static void send_all(int fd,const char *p,size_t n){while(n){ssize_t r=send(fd,p,n,MSG_NOSIGNAL);if(r<0&&errno==EINTR)continue;if(r<=0)die("send");p+=r;n-=(size_t)r;}}
static void exact(int fd,char *p,size_t n){while(n){ssize_t r=recv(fd,p,n,0);if(r<0&&errno==EINTR)continue;if(r<=0)die("recv");p+=r;n-=(size_t)r;}}
static int connect_to(const char *host,int port){int fd=socket(AF_INET,SOCK_STREAM,0);if(fd<0)die("socket");int one=1;struct timeval tv={5,0};setsockopt(fd,IPPROTO_TCP,TCP_NODELAY,&one,sizeof(one));setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&tv,sizeof(tv));setsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,&tv,sizeof(tv));struct sockaddr_in a={.sin_family=AF_INET,.sin_port=htons((uint16_t)port)};if(inet_pton(AF_INET,host,&a.sin_addr)!=1||connect(fd,(struct sockaddr*)&a,sizeof(a)))die("connect");return fd;}
static void key(char out[33],unsigned int i){uint64_t z=(uint64_t)i+0x9e3779b97f4a7c15ULL;z=(z^(z>>30))*0xbf58476d1ce4e5b9ULL;z=(z^(z>>27))*0x94d049bb133111ebULL;z^=z>>31;if(snprintf(out,33,"%016llx%016llx",(unsigned long long)i,(unsigned long long)z)!=32)abort();}
static void value(char out[129],unsigned int i,int changed){memset(out,'x',128);out[128]=0;if(changed){char tag[21];snprintf(tag,sizeof(tag),"v%019u",i);memcpy(out,tag,20);}}
static size_t command(char *out,const char *op,unsigned int i,const char *val){char k[33];key(k,i);if(val)return (size_t)sprintf(out,"*3\r\n$%zu\r\n%s\r\n$32\r\n%s\r\n$128\r\n%s\r\n",strlen(op),op,k,val);return (size_t)sprintf(out,"*2\r\n$%zu\r\n%s\r\n$32\r\n%s\r\n",strlen(op),op,k);}
static void ok(int fd){char b[5];exact(fd,b,5);if(memcmp(b,"+OK\r\n",5)){fprintf(stderr,"not +OK: %.5s\n",b);exit(1);}}
static int bulk(int fd,char out[128]){char line[64];size_t n=0;do{if(n==sizeof(line)-1)abort();exact(fd,line+n,1);n++;}while(line[n-1]!='\n');line[n]=0;if(!strcmp(line,"$-1\r\n"))return 0;if(strcmp(line,"$128\r\n")){fprintf(stderr,"bad bulk: %s",line);exit(1);}exact(fd,out,128);char tail[2];exact(fd,tail,2);if(memcmp(tail,"\r\n",2))abort();return 1;}
static int compare(const void *a,const void *b){uint64_t x=*(const uint64_t*)a,y=*(const uint64_t*)b;return x>y?1:x<y?-1:0;}
static double percentile(uint64_t *v,size_t n,size_t p){return (double)v[(p*n+99)/100-1]/1000000.;}
static long long ticks(int pid){if(!pid)return -1;char path[64],buf[4096];snprintf(path,sizeof(path),"/proc/%d/stat",pid);FILE *f=fopen(path,"r");if(!f)die("proc stat");if(!fgets(buf,sizeof(buf),f))die("read stat");fclose(f);char *p=strrchr(buf,')');if(!p)abort();char *save,*token=strtok_r(p+2," ",&save);long long total=0;for(int field=3;token;field++,token=strtok_r(NULL," ",&save)){if(field==14||field==15)total+=strtoll(token,NULL,10);if(field==15)break;}return total;}
int main(int argc,char **argv){const char *host=NULL,*replica=NULL,*phase=NULL,*output=NULL;int port=29191,rport=29192,c=1,P=1,n=100000,k=32,d=128,changed=0,probes=1000,primary_pid=0,capture_pid=0,duration=10,gate=0;static const struct option options[]={{"phase",1,0,1000},{"mutated",1,0,1001},{"probes",1,0,1002},{"primary-pid",1,0,1003},{"capture-pid",1,0,1004},{"seconds",1,0,1005},{"gate",0,0,1006},{0,0,0,0}};int opt;while((opt=getopt_long(argc,argv,"h:p:R:q:c:P:n:k:d:o:",options,NULL))!=-1){switch(opt){case'h':host=optarg;break;case'p':port=atoi(optarg);break;case'R':replica=optarg;break;case'q':rport=atoi(optarg);break;case'c':c=atoi(optarg);break;case'P':P=atoi(optarg);break;case'n':n=atoi(optarg);break;case'k':k=atoi(optarg);break;case'd':d=atoi(optarg);break;case'o':output=optarg;break;case 1000:phase=optarg;break;case 1001:changed=atoi(optarg);break;case 1002:probes=atoi(optarg);break;case 1003:primary_pid=atoi(optarg);break;case 1004:capture_pid=atoi(optarg);break;case 1005:duration=atoi(optarg);break;case 1006:gate=1;break;default:return 2;}}
if(duration<1||duration>60||!host||!phase||(c!=1&&c!=8)||P!=1||n<1||n>1000000||k!=32||d!=128){fprintf(stderr,"require host/phase, -c 1 or 8 -P 1 -k 32 -d 128\n");return 2;}
int fd=connect_to(host,port);uint64_t *lat=calloc((size_t)n,sizeof(*lat)),*e2e=calloc((size_t)n,sizeof(*e2e));if(!lat||!e2e)die("calloc");char request[512],expected[129],actual[128];uint64_t start=0,end=0;unsigned long long polls=0;int matched=0;
if(!strcmp(phase,"write")||!strcmp(phase,"pressure")){char *frames=malloc((size_t)n*256);if(!frames)die("frames");size_t length=0;for(int i=0;i<n;i++){value(expected,(unsigned int)i,0);size_t got=command(frames+(size_t)i*256,"HSET",(unsigned int)i,expected);if(i&&got!=length)abort();length=got;}/* Each connection has exactly one outstanding command. */
struct pollfd sockets[8]; int indices[8]; uint64_t sent[8];
int next=0,completed=0; int pressure=!strcmp(phase,"pressure"); uint64_t deadline=0;
for(int j=0;j<c;j++) { sockets[j].fd=j?connect_to(host,port):fd; sockets[j].events=POLLIN; indices[j]=-1; }
if(gate){puts("READY");fflush(stdout);if(getchar()!='G'){fprintf(stderr,"missing GO\n");return 1;}}
long long before=ticks(primary_pid),cap_before=ticks(capture_pid);uint64_t cpu=ns(CLOCK_PROCESS_CPUTIME_ID);start=ns(CLOCK_MONOTONIC_RAW);deadline=start+(uint64_t)duration*1000000000;
for(int j=0;j<c&&next<n;j++) { indices[j]=next++; sent[j]=ns(CLOCK_MONOTONIC_RAW); send_all(sockets[j].fd,frames+(size_t)indices[j]*256,length); }
int active=c<n?c:n;
while(active>0) {
    int ready=poll(sockets,(nfds_t)c,5000);
    if(ready<0&&errno==EINTR)continue;
    if(ready<=0){fprintf(stderr,"poll failed or timed out\n");return 1;}
    for(int j=0;j<c;j++) {
        if(!(sockets[j].revents&POLLIN)) { if(sockets[j].revents&(POLLERR|POLLHUP|POLLNVAL))die("poll connection"); continue; }
        ok(sockets[j].fd); lat[indices[j]]=ns(CLOCK_MONOTONIC_RAW)-sent[j]; completed++;
        if((pressure&&ns(CLOCK_MONOTONIC_RAW)<deadline)||(!pressure&&next<n)) { indices[j]=(next++)%n; sent[j]=ns(CLOCK_MONOTONIC_RAW); send_all(sockets[j].fd,frames+(size_t)indices[j]*256,length); }
        else { if(j)close(sockets[j].fd); sockets[j].fd=-1; active--; }
    }
}
end=ns(CLOCK_MONOTONIC_RAW);cpu=ns(CLOCK_PROCESS_CPUTIME_ID)-cpu;long long after=ticks(primary_pid),cap_after=ticks(capture_pid);double seconds=(double)(end-start)/1e9;qsort(lat,(size_t)n,sizeof(*lat),compare);if(pressure){printf("{\"phase\":\"pressure\",\"requests\":%d,\"connections\":%d,\"pipeline\":1,\"seconds\":%.9f,\"qps\":%.6f}\n",completed,c,seconds,completed/seconds);}else printf("{\"phase\":\"%s\",\"uid\":%u,\"requests\":%d,\"connections\":%d,\"pipeline\":1,\"seconds\":%.9f,\"qps\":%.6f,\"p50_ms\":%.9f,\"p99_ms\":%.9f,\"max_ms\":%.9f,\"primary_cpu_pct\":%.6f,\"capture_cpu_pct\":%.6f,\"client_cpu_pct\":%.6f}\n",phase,(unsigned)getuid(),completed,c,seconds,completed/seconds,percentile(lat,n,50),percentile(lat,n,99),(double)lat[n-1]/1e6,primary_pid?(after-before)/(double)sysconf(_SC_CLK_TCK)/seconds*100:-1.,capture_pid?(cap_after-cap_before)/(double)sysconf(_SC_CLK_TCK)/seconds*100:-1.,cpu/1e9/seconds*100);free(frames);
}else if(!strcmp(phase,"verify")){start=ns(CLOCK_MONOTONIC_RAW);for(int i=0;i<n;i++){size_t length=command(request,"HGET",(unsigned int)i,NULL);send_all(fd,request,length);int got=bulk(fd,actual);value(expected,(unsigned int)i,changed&&i<probes);if(!got||memcmp(actual,expected,128)){fprintf(stderr,"verification failed index=%d\n",i);return 1;}matched++;}end=ns(CLOCK_MONOTONIC_RAW);printf("{\"phase\":\"verify\",\"matched\":%d,\"total\":%d,\"seconds\":%.9f}\n",matched,n,(double)(end-start)/1e9);
}else if(!strcmp(phase,"visibility")){if(!replica)abort();
struct pollfd sockets[16]; int index[8],state[8]; uint64_t begun[8],ack[8];
int next=0,done=0;
for(int j=0;j<c;j++) {
    sockets[2*j].fd=j?connect_to(host,port):fd;
    sockets[2*j+1].fd=connect_to(replica,rport);
    sockets[2*j].events=sockets[2*j+1].events=0; state[j]=0; index[j]=-1;
}
start=ns(CLOCK_MONOTONIC_RAW);
for(int j=0;j<c&&next<n;j++) {
    index[j]=next++; value(expected,(unsigned)index[j],1);
    size_t length=command(request,"HMOD",(unsigned)index[j],expected);
    begun[j]=ns(CLOCK_MONOTONIC_RAW);send_all(sockets[2*j].fd,request,length);
    sockets[2*j].events=POLLIN;
}
while(done<n) {
    int ready=poll(sockets,(nfds_t)(2*c),5000);
    if(ready<0&&errno==EINTR)continue;
    if(ready<=0){fprintf(stderr,"visibility poll timeout or error\n");return 1;}
    for(int j=0;j<c;j++) {
        if(index[j]<0)continue;
        int pidx=2*j,ridx=pidx+1;
        if(state[j]==0&&(sockets[pidx].revents&POLLIN)) {
            ok(sockets[pidx].fd);ack[j]=ns(CLOCK_MONOTONIC_RAW);
            sockets[pidx].events=0; sockets[ridx].events=POLLIN;state[j]=1;
            size_t length=command(request,"HGET",(unsigned)index[j],NULL);
            send_all(sockets[ridx].fd,request,length);
        } else if(state[j]==1&&(sockets[ridx].revents&POLLIN)) {
            int got=bulk(sockets[ridx].fd,actual);polls++;
            uint64_t observed=ns(CLOCK_MONOTONIC_RAW);
            value(expected,(unsigned)index[j],1);
            if(got&&!memcmp(actual,expected,128)) {
                lat[index[j]]=observed-ack[j];e2e[index[j]]=observed-begun[j];done++;
                sockets[ridx].events=0;
                if(next<n) {
                    index[j]=next++;value(expected,(unsigned)index[j],1);
                    size_t length=command(request,"HMOD",(unsigned)index[j],expected);
                    begun[j]=ns(CLOCK_MONOTONIC_RAW);send_all(sockets[pidx].fd,request,length);
                    sockets[pidx].events=POLLIN;state[j]=0;
                } else index[j]=-1;
            } else {
                if(observed-ack[j]>5000000000ULL){fprintf(stderr,"visibility timeout\n");return 1;}
                size_t length=command(request,"HGET",(unsigned)index[j],NULL);
                send_all(sockets[ridx].fd,request,length);
            }
        }
        if((sockets[pidx].revents|sockets[ridx].revents)&(POLLERR|POLLHUP|POLLNVAL))die("visibility connection");
    }
}
end=ns(CLOCK_MONOTONIC_RAW);
qsort(lat,(size_t)n,sizeof(*lat),compare);qsort(e2e,(size_t)n,sizeof(*e2e),compare);
printf("{\"phase\":\"visibility\",\"probe_connections\":%d,\"probe_pipeline\":1,\"samples\":%d,\"polls\":%llu,\"ack_to_observed_p50_ms\":%.9f,\"ack_to_observed_p99_ms\":%.9f,\"request_to_observed_p50_ms\":%.9f,\"request_to_observed_p99_ms\":%.9f,\"seconds\":%.9f}\n",c,n,polls,percentile(lat,n,50),percentile(lat,n,99),percentile(e2e,n,50),percentile(e2e,n,99),(double)(end-start)/1e9);
for(int j=0;j<c;j++){if(j)close(sockets[2*j].fd);close(sockets[2*j+1].fd);}

}else return 2;
fflush(stdout);
if(output&&(!strcmp(phase,"write")||!strcmp(phase,"visibility"))){FILE *f=fopen(output,"w");if(!f)die("latency output");fprintf(f,"sorted_latency_ns\n");for(int i=0;i<n;i++)fprintf(f,"%llu\n",(unsigned long long)lat[i]);fclose(f);}free(lat);free(e2e);close(fd);return 0;}
