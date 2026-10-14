/* Read-only TCP LOOKUP route probe. Same requests, persistent socket, chosen
 * entry node; normal server freshness/forwarding remains fully enabled.
 * Usage: metadata_route_smoke HOST PORT RELATIVE_PATH [REPEATS<=1000]
 * Single-threaded bounded diagnostic; no mutation, mounts or cluster control. */
#include "efs/protocol.h"
#include "efs/network.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static uint64_t ns(void)
{struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000000000+t.tv_nsec;}
static unsigned last_status, last_hint;
static struct efs_inode lookup(int fd,efs_ino_t parent,const char *name)
{
    struct efs_msg_inode_lookup q={0};q.export_id=1;q.parent=parent;
    assert(strlen(name)<sizeof(q.name));strcpy(q.name,name);
    assert(!efs_send_msg(fd,EFS_MSG_INODE_LOOKUP,&q,sizeof(q)));
    uint8_t type;void *buf=NULL;uint32_t len=0;
    assert(!efs_recv_msg(fd,&type,&buf,&len));
    assert(type==EFS_MSG_INODE_LOOKUP_REPLY && len==sizeof(struct efs_msg_inode_reply));
    struct efs_msg_inode_reply *r=buf;
    last_status=r->status;last_hint=r->primary_id;
    if(r->status!=EFS_INODE_RPC_OK){free(buf);return (struct efs_inode){0};}
    struct efs_inode row=r->inode;free(buf);return row;
}
static int cmp(const void *a,const void *b)
{uint64_t x=*(const uint64_t *)a,y=*(const uint64_t *)b;return (x>y)-(x<y);}
int main(int argc,char **argv)
{
    assert(argc==4 || argc==5);unsigned n=argc==5?(unsigned)atoi(argv[4]):120;
    int port=atoi(argv[2]);assert(n>=1 && n<=1000 && port>0 && port<=65535);
    assert(strlen(argv[3])<1024 && argv[3][0]!='/' && !strchr(argv[3],'"'));
    char path[1024];strcpy(path,argv[3]);char *parts[16],*save=NULL;unsigned np=0;
    for(char *p=strtok_r(path,"/",&save);p;p=strtok_r(NULL,"/",&save)){
        assert(np<16 && strcmp(p,".") && strcmp(p,".."));parts[np++]=p;
    }
    assert(np);int fd=efs_connect_tcp(argv[1],(uint16_t)port);assert(fd>=0);
    assert(!efs_set_recv_timeout(fd,3000) && !efs_set_send_timeout(fd,3000));
    efs_ino_t parents[16],inos[16],parent=EFS_ROOT_INO;
    for(unsigned j=0;j<np;j++){
        parents[j]=parent;struct efs_inode row=lookup(fd,parent,parts[j]);
        if(!row.ino){printf("{\"host\":\"%s\",\"port\":%d,\"component\":%u,\"refused_status\":%u,\"hint\":%u}\n",argv[1],port,j,last_status,last_hint);close(fd);return 2;}
        parent=inos[j]=row.ino;
    }
    for(unsigned j=0;j<np;j++){
        uint64_t samples[1000],sum=0;
        for(unsigned i=0;i<10;i++){struct efs_inode row=lookup(fd,parents[j],parts[j]);if(row.ino!=inos[j]){fprintf(stderr,"warm route refused status=%u hint=%u\n",last_status,last_hint);close(fd);return 2;}}
        for(unsigned i=0;i<n;i++){
            uint64_t a=ns();struct efs_inode row=lookup(fd,parents[j],parts[j]);samples[i]=ns()-a;
            if(row.ino!=inos[j]){fprintf(stderr,"timed route refused status=%u hint=%u\n",last_status,last_hint);close(fd);return 2;}sum+=samples[i];
        }
        qsort(samples,n,sizeof(*samples),cmp);
        printf("{\"host\":\"%s\",\"port\":%d,\"component\":%u,\"parent\":%llu,\"ino\":%llu,\"n\":%u,\"mean_us\":%.3f,\"p50_us\":%.3f,\"p95_us\":%.3f}\n",
               argv[1],port,j,(unsigned long long)parents[j],(unsigned long long)inos[j],n,
               sum/(1000.*n),samples[(n-1)/2]/1000.,samples[(n-1)*95/100]/1000.);
    }
    close(fd);return 0;
}
