#!/usr/bin/env python3
"""Execute production truncate maintenance under slow proposals and a stuck prefix."""
import os, subprocess, tempfile
from pathlib import Path
root=Path(__file__).resolve().parents[1]
s=Path(os.environ.get('EFS_TRUNC_GC_SOURCE',root/'src/server/raft_host.c')).read_text()
a=s.index('static int trunc_pass_room(') if 'static int trunc_pass_room(' in s else s.index('struct trunc_scan_ctx {')
b=s.index('/* ---- W154 rebuild on return',a)
code=r'''
#include "efs/meta_apply.h"
#include "efs/raft.h"
#include "efs/meta_cmd.h"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#define efs_kv_scan_prefix fake_kv_scan_prefix
#define TRUNC_SCAN_INODES 256u
#define TRUNC_PASS_PROPOSALS 128u
#define TRUNC_PASS_INODES 32u
#define GC_FRAG_BUDGET_US 200000
#define N 100
struct fake_server {pthread_mutex_t lock;uint64_t gc_trunc_seen[2];};
struct efs_raft_host {struct fake_server *s;struct efs_kv *kv;uint8_t trunc_cursor[2][11];uint32_t trunc_cursor_len[2];};
static uint64_t clock_us;static unsigned calls,blocked,scan_error,page_limit=4096;
static unsigned pending[N+1],lane_swept[N+1],done[N+1],retired[N+1];
static uint64_t now_us_(void){return clock_us;}
static uint64_t now_ns(void){return clock_us*1000;}
static uint64_t rd64be(const uint8_t *p){uint64_t v=0;for(unsigned i=0;i<8;i++)v=v*256+p[i];return v;}
static void wr64be(uint8_t *p,uint64_t v){for(unsigned i=0;i<8;i++)p[i]=(uint8_t)(v>>(56-i*8));}
static void wr32be(uint8_t *p,uint32_t v){for(unsigned i=0;i<4;i++)p[i]=(uint8_t)(v>>(24-i*8));}
static int host_hosts(struct efs_raft_host *h,uint8_t group){(void)h;(void)group;return 1;}
static struct efs_export *server_export_acquire_locked(struct fake_server *s,unsigned id){(void)s;(void)id;return NULL;}
static unsigned server_data_chunk_size(struct efs_export *ex){(void)ex;return 65536;}
static void server_export_put(struct fake_server *s,struct efs_export *ex){(void)s;(void)ex;}
static int trunc_sweep_due(efs_ino_t ino,uint64_t target,uint64_t now){(void)ino;(void)target;(void)now;return 1;}
int efs_meta_apply_get_inode(struct efs_kv *kv,efs_ino_t ino,struct efs_meta_row *row){
 (void)kv;assert(ino>0&&ino<=N);memset(row,0,sizeof(*row));row->generation=1;row->active_lanes=1;return EFS_OK;}
int efs_meta_get_fence_history(struct efs_kv *kv,efs_ino_t ino,uint64_t gen,uint8_t auth,struct efs_fence_history *hist){
 (void)kv;assert(gen==1&&auth==EFS_META_FENCE_INODE);hist->count=1;hist->entries[0].epoch=1;hist->entries[0].done=done[ino];return EFS_OK;}
int efs_meta_get_lane_swept(struct efs_kv *kv,efs_ino_t ino,uint64_t gen,uint8_t lane,uint64_t *out){(void)kv;assert(gen==1&&!lane);*out=lane_swept[ino];return EFS_OK;}
uint32_t efs_kv_lane_shard(efs_ino_t ino,uint8_t lane){(void)ino;(void)lane;return 1;}
static int host_gc_propose(struct efs_raft_host *h,uint8_t group,const uint8_t *cmd,uint32_t len){
 (void)h;(void)group;(void)len;uint64_t ino=rd64be(cmd+1);assert(ino&&ino<=N);calls++;clock_us+=60000;
 if(ino<=blocked)return EFS_ERR_BUSY;
 if(cmd[0]==EFS_MD_CMD_TRUNC_SWEEP){assert(rd64be(cmd+18)==1);lane_swept[ino]=1;}
 else if(cmd[0]==EFS_MD_CMD_SWEEP_DONE){assert(lane_swept[ino]);done[ino]=1;}
 else {assert(cmd[0]==EFS_MD_CMD_RETIRE&&done[ino]);pending[ino]=0;retired[ino]++;}
 return EFS_OK;}
typedef int (*visit_fn)(void *,const uint8_t *,uint32_t,const uint8_t *,uint32_t);
static int scan(const uint8_t *prefix,const uint8_t *start,uint32_t sn,visit_fn cb,void *user,uint8_t *last,uint32_t *last_len){
 if(scan_error)return EFS_ERR_IO;
 unsigned visited=0;
 for(unsigned i=1;i<=N;i++){
  uint8_t key[11],value[8];memcpy(key,prefix,3);wr64be(key+3,i);wr64be(value,1);
  if(sn&&memcmp(key,start,sn)<0)continue;
  if(visited==page_limit){if(last&&i>1){wr64be(last+3,i-1);memcpy(last,prefix,3);*last_len=11;}return 1;}
  if(pending[i]){int rc=cb(user,key,11,value,8);if(rc){if(last){memcpy(last,key,11);*last_len=11;}return rc;}}
  visited++;
 }
 return EFS_OK;}
static int host_gc_scan_work(struct efs_raft_host *h,const uint8_t *prefix,uint32_t pn,const uint8_t *start,uint32_t sn,
 visit_fn cb,void *user,uint8_t *last,uint32_t *last_len,uint32_t cap){(void)h;assert(pn==3&&cap==11);return scan(prefix,start,sn,cb,user,last,last_len);}
int efs_kv_scan_prefix(struct efs_kv *kv,const uint8_t *prefix,uint32_t pn,visit_fn cb,void *user){(void)kv;assert(pn==3);return scan(prefix,NULL,0,cb,user,NULL,NULL);}
'''+s[a:b]+r'''
static void reset(struct efs_raft_host *h){memset(h->trunc_cursor_len,0,sizeof(h->trunc_cursor_len));memset(lane_swept,0,sizeof(lane_swept));memset(done,0,sizeof(done));memset(retired,0,sizeof(retired));for(unsigned i=1;i<=N;i++)pending[i]=1;clock_us=0;calls=blocked=scan_error=0;page_limit=4096;}
int main(void){struct fake_server server={0};pthread_mutex_init(&server.lock,NULL);struct efs_raft_host h={.s=&server};
 for(unsigned group=0;group<=2;group+=2){
  reset(&h);host_gc_trunc_pass(&h,group,group?2:1);
  /* An individual slow proposal may cross the budget; never start the next. */
  if(calls>4){fprintf(stderr,"truncate monopolized GC: %u slow proposals\n",calls);return 10;}
  assert(calls==4&&clock_us==240000);
  for(unsigned pass=0;pass<200;pass++){
   unsigned before=calls;host_gc_trunc_pass(&h,group,group?2:1);assert(calls-before<=4);
  }
  for(unsigned i=1;i<=N;i++)assert(retired[i]==1&&!pending[i]);
  /* Block the prefix permanently: later inodes still retire and a wrap
   * retries those markers after their authority becomes available. */
  reset(&h);blocked=32;
  for(unsigned pass=0;pass<100;pass++)host_gc_trunc_pass(&h,group,group?2:1);
  for(unsigned i=1;i<=N;i++)assert(i<=blocked?pending[i]&&!retired[i]:retired[i]==1);
  blocked=0;for(unsigned pass=0;pass<150;pass++)host_gc_trunc_pass(&h,group,group?2:1);
  for(unsigned i=1;i<=N;i++)assert(retired[i]==1);
  /* Bounded tombstone pages also progress even without emitted candidates. */
  reset(&h);page_limit=7;for(unsigned i=1;i<=80;i++)pending[i]=0;
  for(unsigned pass=0;pass<100;pass++)host_gc_trunc_pass(&h,group,group?2:1);
  for(unsigned i=81;i<=N;i++)assert(retired[i]==1);
  reset(&h);scan_error=1;host_gc_trunc_pass(&h,group,group?2:1);assert(!calls&&!h.trunc_cursor_len[group/2]);
 }
 puts("truncate GC: time-bounded proposals, partial proofs resume, stuck prefix isolated, tombstone pages advance, both groups PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='efs-trunc-gc-') as tmp:
 p=Path(tmp)/'test.c';p.write_text(code)
 subprocess.run([os.environ.get('CC','cc'),'-std=gnu11','-Wall','-Wextra','-Werror','-Wno-unused-function','-I'+str(root/'include'),str(p),'-pthread','-o',str(p.with_suffix(''))],check=True)
 subprocess.run([str(p.with_suffix(''))],check=True,timeout=5)
