#!/usr/bin/env python3
"""Actual experimental GC hook/helper; authority/storage are adapters."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[1];s=(root/'src/server/raft_host.c').read_text();a=s.index('static void host_gc_record(');fn=s[a:s.index('\n}',a)+2]
a=s.index('int server_raft_host_repair_gc_ready(');proof=s[a:s.index('\n}',a)+2]
a=s.index('int server_raft_host_repair_gc_inode_local_ready(');local=s[a:s.index('\n}',a)+2]
code=r'''
#include "efs/repair_job.h"
#include <assert.h>
#include <string.h>
#define GC_ACK_MAX 128
struct server {uint32_t id;int repair_experiment;};
struct efs_raft_host {struct server *s;void *kv;int gc_running,running;pthread_mutex_t mu;};
static struct efs_raft_host *g_host;
struct efs_export {uint32_t id;};
static int barrier,removed,scans,reads,read_rc,owns=5;
static int starts,finishes,fail_start,fail_finish,stop_after_start;
static uint8_t preset_ack;
static int host_hosts(struct efs_raft_host *h,uint8_t group){(void)h;return !!(owns&(1<<group));}
static int host_read_index(struct efs_raft_host *h,uint8_t group,int *hint){(void)h;(void)group;(void)hint;reads++;return read_rc;}
uint32_t efs_kv_lane_shard(efs_ino_t ino,uint8_t lane){(void)ino;(void)lane;return 2;}
int efs_repair_gc_inode_ready(struct efs_kv *kv,efs_ino_t ino){(void)kv;assert(ino==9);scans++;return barrier;}
static uint64_t rd64be(const uint8_t *p){uint64_t n=0;for(unsigned i=0;i<8;i++)n=n*256+p[i];return n;}
static uint32_t rd32be(const uint8_t *p){return (uint32_t)rd64be((const uint8_t[8]){0,0,0,0,p[0],p[1],p[2],p[3]});}
static int env_on(const char *p){(void)p;return 0;}
int efs_meta_unpack_gc(const uint8_t *v,uint32_t n,uint32_t *nodes,uint8_t *ack,uint8_t sums[3][32]){(void)v;(void)n;*ack=preset_ack;nodes[0]=1;nodes[1]=2;nodes[2]=3;memset(sums,0,96);return 0;}
int efs_repair_gc_object_ready(struct efs_kv *kv,efs_ino_t ino,uint32_t ci,uint64_t obj){(void)kv;assert(ino==9&&ci==7&&obj==101);scans++;return barrier;}
static int host_gc_local_del(struct efs_raft_host *h,struct efs_export *ex,efs_ino_t ino,uint32_t ci,uint32_t fi,uint64_t gen,const uint8_t *sum){(void)h;(void)ex;(void)ino;(void)ci;(void)fi;(void)gen;(void)sum;assert(starts==2);removed++;return 0;}
struct host_gc_delete {int pending;};
static int host_gc_remote_start(struct efs_raft_host *h,struct host_gc_delete *job,uint32_t node,uint32_t ex,efs_ino_t ino,uint32_t ci,uint32_t fi,uint64_t gen,const uint8_t *sum){(void)h;(void)node;(void)ex;(void)ino;(void)ci;(void)fi;(void)gen;(void)sum;starts++;if(stop_after_start)h->gc_running=0;if(fail_start==(int)fi+1)return EFS_ERR_IO;job->pending=(int)fi+1;return 0;}
static int host_gc_remote_finish(struct host_gc_delete *job){finishes++;if(!job->pending)return EFS_ERR_IO;int fail=fail_finish==job->pending;job->pending=0;if(fail)return EFS_ERR_IO;removed++;return 0;}
'''+proof+local+fn+r'''
int main(void){struct server server={1,1};struct efs_raft_host h={.s=&server,.gc_running=1,.running=1};pthread_mutex_init(&h.mu,NULL);g_host=&h;struct efs_export ex={1};uint8_t key[24]={0},val[128]={0};key[10]=9;key[18]=101;key[23]=7;struct efs_gc_ack_item ack[128];int n=0;
 server.repair_experiment=0;barrier=EFS_ERR_BUSY;host_gc_record(&h,&ex,key,val,ack,&n);assert(removed==3&&n==3&&!scans);assert(server_raft_host_repair_gc_ready(9,7,101)==EFS_ERR_ACCES&&!reads);
 removed=n=starts=finishes=0;server.repair_experiment=1;
 read_rc=EFS_ERR_IO;assert(server_raft_host_repair_gc_ready(9,7,101)==EFS_ERR_IO&&!scans);read_rc=0;
 owns=1;assert(server_raft_host_repair_gc_ready(9,7,101)==EFS_ERR_BUSY&&!scans);owns=5;
 barrier=EFS_ERR_BUSY;assert(server_raft_host_repair_gc_ready(9,0,0)==EFS_ERR_BUSY&&scans==1);scans=0;
 int failures[]={EFS_ERR_BUSY,EFS_ERR_PROTO,EFS_ERR_IO,EFS_ERR_NOMEM};
 for(unsigned i=0;i<4;i++){barrier=failures[i];host_gc_record(&h,&ex,key,val,ack,&n);assert(!removed&&!n);}
 int old_reads=reads,old_scans=scans;
 pthread_mutex_lock(&h.mu);assert(server_raft_host_repair_gc_inode_local_ready(9)==EFS_ERR_BUSY);pthread_mutex_unlock(&h.mu);
 assert(reads==old_reads&&scans==old_scans);
 barrier=EFS_ERR_BUSY;assert(server_raft_host_repair_gc_inode_local_ready(9)==EFS_ERR_BUSY);
 assert(reads==old_reads&&scans==old_scans+1);
 owns=1;assert(server_raft_host_repair_gc_inode_local_ready(9)==EFS_ERR_BUSY);owns=5;
 barrier=0;host_gc_record(&h,&ex,key,val,ack,&n);assert(removed==3&&n==3&&starts==2&&finishes==2);
 server.repair_experiment=0;removed=n=starts=finishes=0;fail_start=2;
 host_gc_record(&h,&ex,key,val,ack,&n);assert(n==2&&removed==2&&starts==2&&finishes==2);assert(ack[0].frag==0&&ack[1].frag==2);
 removed=n=starts=finishes=0;fail_start=0;fail_finish=3;
 host_gc_record(&h,&ex,key,val,ack,&n);assert(n==2&&removed==2&&starts==2&&finishes==2);assert(ack[1].frag==1);
 /* Near-full ACK arrays admit at most their reserved number of requests. */
 removed=starts=finishes=0;fail_finish=0;preset_ack=1;n=GC_ACK_MAX-1;
 host_gc_record(&h,&ex,key,val,ack,&n);assert(n==GC_ACK_MAX&&starts==1&&finishes==1&&removed==1&&ack[n-1].frag==1);
 n=0;removed=starts=finishes=0;preset_ack=1;stop_after_start=1;
 host_gc_record(&h,&ex,key,val,ack,&n);assert(!h.gc_running&&starts==1&&finishes==1&&removed==1&&n==1);
 n=GC_ACK_MAX;starts=finishes=0;h.gc_running=1;stop_after_start=0;
 host_gc_record(&h,&ex,key,val,ack,&n);assert(!starts&&!finishes&&n==GC_ACK_MAX);
 return 0;}
'''
with tempfile.TemporaryDirectory() as d:
 p=Path(d)/'gc.c';p.write_text('#include <stdio.h>\n#include <pthread.h>\n#include \"efs/raft.h\"\n'+code);b=Path(d)/'gc'
 subprocess.run(['cc','-std=gnu11','-I'+str(root/'include'),str(p),'-o',str(b),'-lpthread'],check=True);subprocess.run([b],check=True)
print('actual GC coordinator: peer pipeline, partial failures, ACK capacity, shutdown drain; repair authority/storage errors block deletion PASS')
