#include "efs/raft.h"
#include "efs/raft_disk.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
static unsigned acks;static struct efs_raft_disk *disk;static double *lat,started[16];static unsigned base;
static double now(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec/1e9;}
static int send_reply(void *p,const struct efs_raft_msg*m){(void)p;if(m->success&&m->match_index){assert(m->match_index<=efs_raft_disk_covered_index(disk,0,efs_raft_disk_synced_bytes(disk)));lat[acks++]=(now()-started[m->match_index-base-1])*1e6;}return EFS_OK;}
static int cmp(const void*a,const void*b){double x=*(double*)a,y=*(double*)b;return(x>y)-(x<y);}
int main(int argc,char**argv){assert(argc==2);int batch=atoi(argv[1]);char path[]="/data1/efs/w93-cost-XXXXXX";assert(mkdtemp(path));disk=efs_raft_disk_open(path,EFS_RAFT_DISK_SYNC);assert(disk);struct efs_raft_store*store=efs_raft_disk_group(disk,0);struct efs_raft_cfg cfg={0};cfg.id=1;cfg.n=3;cfg.election_ticks=10;cfg.heartbeat_ticks=1;cfg.store=store;cfg.store_ctx=store;cfg.send=send_reply;struct efs_raft*r=efs_raft_new(&cfg);assert(r);struct efs_raft_msg msgs[16]={0};msgs[0].type=EFS_RAFT_MSG_AE_REQ;msgs[0].from=0;msgs[0].to=1;msgs[0].term=2;assert(efs_raft_recv(r,&msgs[0])==EFS_OK);char value[1024]={0};lat=calloc(2560,sizeof(double));assert(lat);double t=now();for(base=0;base<2560;base+=16){double ready=now();for(unsigned i=0;i<16;i++){msgs[i]=msgs[0];msgs[i].prev_index=base+i;msgs[i].prev_term=base+i?2:0;msgs[i].nentries=1;msgs[i].entries[0].term=2;msgs[i].entries[0].cmd=(uint8_t*)value;msgs[i].entries[0].clen=sizeof(value);started[i]=ready;}if(batch)assert(efs_raft_recv_batch(r,msgs,16)==EFS_OK);else for(unsigned i=0;i<16;i++)assert(efs_raft_recv(r,&msgs[i])==EFS_OK);}double elapsed=now()-t;assert(acks==2560);efs_raft_free(r);efs_raft_disk_close(disk);disk=efs_raft_disk_open(path,EFS_RAFT_DISK_SYNC);assert(disk);store=efs_raft_disk_group(disk,0);for(unsigned i=1;i<=2560;i++){char bytes[1024];uint32_t n=sizeof(bytes);uint64_t term;assert(store->get(store,i,&term,(uint8_t*)bytes,&n)==EFS_OK&&term==2&&n==sizeof(value)&&!memcmp(bytes,value,n));}efs_raft_disk_close(disk);char file[1024];snprintf(file,sizeof(file),"%s/raft.log",path);assert(!unlink(file)&&!rmdir(path));qsort(lat,acks,sizeof(double),cmp);printf("{\"batch\":%d,\"records\":%u,\"ops_s\":%.1f,\"ready_to_ack_p50_us\":%.1f,\"p95_us\":%.1f,\"p99_us\":%.1f}\n",batch,acks,acks/elapsed,lat[acks/2],lat[acks*95/100],lat[acks*99/100]);free(lat);}
