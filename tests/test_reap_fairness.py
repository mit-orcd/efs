#!/usr/bin/env python3
"""Execute the actual batched REAP pass: capped scans advance a round-robin
cursor, a refused or busy inode never blocks the rest of its batch, the
metadata-waiting budget yields between sweep rounds (not under capacity
pressure), and shutdown or scan errors attempt nothing."""
from pathlib import Path
import os, subprocess, tempfile
root=Path(__file__).resolve().parents[1]
s=Path(os.environ.get('EFS_REAP_SOURCE',root/'src/server/raft_host.c')).read_text()
def function(start):
    a=s.index(start);return s[a:s.index('\n}',a)+2]
ctx=s[s.index('struct reap_scan_ctx {'):s.index('\n};',s.index('struct reap_scan_ctx {'))+3]
helper=function('static int host_gc_metadata_waiting(')
a=s.index('/* W124 multi-entry builder');b=s.index('\n}',s.index('static void host_gc_reap_pass(',a))+2
code=r'''
#include "efs/meta_apply.h"
#include "efs/raft.h"
#include "efs/meta_cmd.h"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#define REAP_SCAN_MAX 2 /* four markers: two capped passes */
#define REAP_ENTRY_BYTES (48u * 1024u)
#define REAP_KEY_LEN 11u
#define GC_FRAG_BUDGET_US 200000
#define HOST_WAITERS 512
#define HOST_NGROUPS 2
#define efs_kv_get fake_kv_get
#define efs_kv_key_lane fake_key_lane
#define efs_kv_lane_shard fake_lane_shard
#define efs_kv_inode_shard fake_inode_shard
struct fake_server {pthread_mutex_t lock;uint64_t quota,gc_scan_errors,gc_reap_seen[2],gc_reap_capped[2],gc_first_reap[2],gc_first_seen_us[2];};
struct efs_raft_host {struct fake_server *s;struct efs_kv *kv;pthread_mutex_t cv_mu,wait_mu;void *wait_head;struct {uint8_t active;} waiters[HOST_WAITERS];int gc_running,running;uint8_t reap_cursor[2][11];uint32_t reap_cursor_len[2];};
struct reap_times {uint64_t scan,sweep,fence,done;};
static struct efs_node node;static int missing_node,scan_error,refuse,busy_lane;
static uint64_t clock_us;static unsigned handed[4],reaped[4],dead[4],chunks[4],rounds_of[4];
static struct efs_node * __attribute__((unused)) server_local_node(struct fake_server *s){(void)s;return missing_node?NULL:&node;}
static uint64_t now_us_(void){return clock_us;}
uint64_t efs_iostats_now_us(void){return clock_us;}
static uint64_t rd64be(const uint8_t *p){uint64_t v=0;for(unsigned i=0;i<8;i++)v=v*256+p[i];return v;}
static void wr64be(uint8_t *p,uint64_t v){for(unsigned i=0;i<8;i++)p[i]=(uint8_t)(v>>(56-i*8));}
static void wr32be(uint8_t *p,uint32_t v){for(unsigned i=0;i<4;i++)p[i]=(uint8_t)(v>>(24-i*8));}
int efs_meta_unpack_reap(const uint8_t *value,uint32_t len,uint64_t *gen,uint64_t *bits){(void)value;if(len!=16)return EFS_ERR_PROTO;*gen=3;*bits=1;return EFS_OK;}
int efs_kv_key_reap_prefix(uint32_t anchor,uint8_t *key,uint32_t *len){key[0]=anchor>>8;key[1]=anchor;key[2]=22;*len=3;return EFS_OK;}
uint32_t efs_kv_lane_shard(efs_ino_t ino,uint8_t lane){(void)ino;(void)lane;return 1;}
uint32_t efs_kv_inode_shard(efs_ino_t ino){(void)ino;return 1;}
static int host_hosts(struct efs_raft_host *h,uint8_t group){(void)h;(void)group;return 1;}
static unsigned ix(efs_ino_t ino){assert(ino>=11&&ino<=14);return (unsigned)(ino-11);}
/* Lane 0 of inode i holds chunks[i] rows; UINT32_MAX = lane key gone. */
int efs_meta_lane_sweep_select(struct efs_kv *kv,efs_ino_t ino,uint64_t gen,uint8_t lane,uint32_t *indices,uint32_t *n){
 (void)kv;(void)indices;assert(gen==3&&!lane);unsigned c=chunks[ix(ino)];if(c==UINT32_MAX)c=0;*n=c<8?c:8;return EFS_OK;}
int efs_kv_key_lane(uint32_t sh,efs_ino_t ino,uint64_t gen,uint8_t lane,uint8_t *key,uint32_t *len){(void)sh;assert(gen==3&&!lane);key[0]=(uint8_t)ix(ino);*len=1;return EFS_OK;}
static int efs_kv_get(struct efs_kv *kv,const uint8_t *key,uint32_t kl,uint8_t *v,uint32_t *vl){(void)kv;(void)v;(void)vl;assert(kl==1);return chunks[key[0]]==UINT32_MAX?EFS_ERR_NOT_FOUND:EFS_OK;}
static void host_gc_inode_nodes_batch(struct efs_raft_host *h,const efs_ino_t *inos,const uint64_t *gens,uint32_t n,uint8_t *ok){
 (void)h;for(uint32_t i=0;i<n;i++){assert(gens[i]==3&&chunks[ix(inos[i])]==UINT32_MAX);handed[ix(inos[i])]++;ok[i]=(int)inos[i]!=refuse;}}
int efs_meta_reap_select(struct efs_kv *kv,efs_ino_t ino,uint64_t gen,struct efs_meta_reap_key *keys,uint32_t *n){(void)kv;(void)keys;(void)ino;assert(gen==3);*n=0;return EFS_OK;}
static int host_gc_propose(struct efs_raft_host *h,uint8_t group,const uint8_t *cmd,uint32_t len){(void)h;(void)group;clock_us+=150000;
 uint32_t count=(uint32_t)cmd[1]<<8|cmd[2],off=3;
 if(cmd[0]==EFS_MD_CMD_LANE_SWEEP_MULTI){
  for(uint32_t k=0;k<count;k++){unsigned i=ix(rd64be(cmd+off));uint8_t n=cmd[off+17];rounds_of[i]++;
   if((int)i+11!=busy_lane&&chunks[i]!=UINT32_MAX){chunks[i]-=n;if(!chunks[i])chunks[i]=UINT32_MAX;}
   off+=18u+4u*n;}
  assert(off==len);return EFS_OK;}
 assert(cmd[0]==EFS_MD_CMD_REAP_MULTI);
 for(uint32_t k=0;k<count;k++){unsigned i=ix(rd64be(cmd+off));assert((int)i+11!=refuse);reaped[i]++;dead[i]=1;off+=17u+32u*cmd[off+16];}
 assert(off==len);return EFS_OK;
}
'''+ctx+'\n'+function('static int reap_scan_cb(')+r'''
typedef int (*visit_fn)(void *,const uint8_t *,uint32_t,const uint8_t *,uint32_t);
int mock_scan_from(struct efs_kv *kv,const uint8_t *prefix,uint32_t pn,const uint8_t *start,uint32_t sn,visit_fn cb,void *user){
 (void)kv;assert(pn==3);if(scan_error)return scan_error;
 for(unsigned i=0;i<4;i++) {uint8_t key[11],value[16]={0};memcpy(key,prefix,3);wr64be(key+3,i+11);
  if(dead[i]||(sn&&memcmp(key,start,sn)<0))continue;
  int rc=cb(user,key,11,value,16);if(rc)return rc;
 }
 return EFS_OK;
}
'''+helper+'\n'+s[a:b]+r'''
static void reset(struct efs_raft_host *h,unsigned c){
 memset(handed,0,sizeof(handed));memset(reaped,0,sizeof(reaped));memset(dead,0,sizeof(dead));memset(rounds_of,0,sizeof(rounds_of));
 for(unsigned i=0;i<4;i++)chunks[i]=c;
 memset(h->reap_cursor_len,0,sizeof(h->reap_cursor_len));clock_us=0;scan_error=refuse=busy_lane=missing_node=0;node.used=100;
 h->wait_head=NULL;memset(h->waiters,0,sizeof(h->waiters));h->waiters[HOST_WAITERS-1].active=1;h->gc_running=h->running=1;h->s->quota=1000;}
static unsigned total(unsigned *v){return v[0]+v[1]+v[2]+v[3];}
int main(void){struct fake_server server={0};struct efs_raft_host h={.s=&server};pthread_mutex_init(&server.lock,NULL);pthread_mutex_init(&h.cv_mu,NULL);pthread_mutex_init(&h.wait_mu,NULL);
 for(unsigned group=0;group<=2;group+=2){unsigned gi=group/2;struct reap_times times={0};
  /* Capped scans: two markers per pass, the cursor moves past the batch. */
  reset(&h,1);host_gc_reap_pass(&h,group,1,&times);
  assert(reaped[0]&&reaped[1]&&!reaped[2]&&!reaped[3]&&server.gc_reap_capped[gi]);
  assert(h.reap_cursor_len[gi]==11&&rd64be(h.reap_cursor[gi]+3)==12);
  host_gc_reap_pass(&h,group,1,&times);assert(total(reaped)==4&&!h.reap_cursor_len[gi]);
  /* A member refusing one inode keeps only its marker; it is retried. */
  reset(&h,1);refuse=11;host_gc_reap_pass(&h,group,1,&times);assert(handed[0]&&handed[1]&&!reaped[0]&&reaped[1]);
  host_gc_reap_pass(&h,group,1,&times);assert(reaped[2]&&reaped[3]&&!h.reap_cursor_len[gi]);
  host_gc_reap_pass(&h,group,1,&times);assert(handed[0]==2&&!reaped[0]);
  refuse=0;host_gc_reap_pass(&h,group,1,&times);assert(reaped[0]==1&&total(reaped)==4);
  /* A lane that stays busy blocks only its own inode. */
  reset(&h,1);busy_lane=11;host_gc_reap_pass(&h,group,1,&times);assert(!handed[0]&&reaped[1]);busy_lane=0;
  /* Metadata waiting: yield between sweep rounds after the budget. */
  reset(&h,24);h.waiters[HOST_WAITERS-1].active=1;host_gc_reap_pass(&h,group,1,&times);
  assert(rounds_of[0]==2&&!handed[0]&&server.gc_reap_capped[gi]); /* 2 x 150 ms >= 200 ms budget */
  /* No waiters, or capacity pressure: the rounds run to completion. */
  reset(&h,24);h.waiters[HOST_WAITERS-1].active=0;host_gc_reap_pass(&h,group,1,&times);assert(rounds_of[0]==3&&reaped[0]&&reaped[1]);
  reset(&h,24);node.used=950;host_gc_reap_pass(&h,group,1,&times);assert(rounds_of[0]==3&&reaped[0]); /* capacity pressure must reclaim */
  reset(&h,24);missing_node=1;host_gc_reap_pass(&h,group,1,&times);assert(rounds_of[0]==3&&reaped[0]); /* unknown usage is no proof of free space */
  /* Shutdown and scan errors attempt nothing. */
  reset(&h,1);h.gc_running=0;host_gc_reap_pass(&h,group,1,&times);assert(!total(handed)&&!total(rounds_of)&&!h.reap_cursor_len[gi]);
  reset(&h,1);scan_error=EFS_ERR_IO;uint64_t before=server.gc_scan_errors;host_gc_reap_pass(&h,group,1,&times);
  assert(!total(rounds_of)&&!h.reap_cursor_len[gi]&&server.gc_scan_errors==before+1);
 }
 pthread_mutex_destroy(&h.cv_mu);pthread_mutex_destroy(&h.wait_mu);pthread_mutex_destroy(&server.lock);
 puts("actual batched REAP fairness: capped round-robin cursor, per-inode refusal/busy isolation, metadata budget between rounds, quota speed, shutdown and scan errors PASS");
}
'''
code=code.replace('efs_kv_scan_from','mock_scan_from')
with tempfile.TemporaryDirectory(prefix='efs-reap-fairness-') as d:
    p=Path(d)/'test.c';p.write_text(code);exe=p.with_suffix('')
    subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror','-Wno-unused-function','-pthread','-I'+str(root/'include'),str(p),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
