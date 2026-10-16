#!/usr/bin/env python3
"""Actual reaper stops within a64-lane inode and leaves its durable marker."""
from pathlib import Path
import os,shlex,subprocess,tempfile
root=Path(__file__).resolve().parents[1]
s=(root/'src/server/raft_host.c').read_text()
a=s.index('static void host_gc_reap_pass(');b=s.index('\n}',a)+2
ctx=s[s.index('struct reap_scan_ctx {'):s.index('\n};',s.index('struct reap_scan_ctx {'))+3]
finish_start=s.index('static void host_gc_reap_finish(')
finish=s[finish_start:s.index('\n}',finish_start)+2]
code=r'''
#include "efs/meta_apply.h"
#include "efs/meta_cmd.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#define REAP_SCAN_MAX 32
#define REAP_KEY_LEN 11
#define GC_FRAG_BUDGET_US 100000
#define efs_kv_scan_from fake_scan_from
#define efs_kv_inode_shard fake_inode_shard
struct server {int repair_experiment;uint64_t gc_scan_errors,gc_reap_seen[2],gc_reap_capped[2],gc_first_reap[2],gc_first_seen_us[2];};
struct efs_raft_host {struct efs_kv *kv;struct server *s;int gc_running,running;uint8_t reap_cursor[2][REAP_KEY_LEN];uint32_t reap_cursor_len[2];};
struct reap_times {uint64_t scan,sweep,fence,done;};
'''+ctx+r'''
static int host_gc_metadata_waiting(struct efs_raft_host *h){(void)h;return 0;}
static unsigned sweeps,fences,finishes,selects,stop_after;
static int hosted,busy;
static uint64_t now_us_(void) {return 1;}
static uint64_t efs_iostats_now_us(void) {return 1;}
static int env_on(const char *s) {(void)s;return 0;}
static void wr64be(uint8_t *p,uint64_t x) {for(int i=7;i>=0;i--){p[i]=(uint8_t)x;x>>=8;}}
static void wr32be(uint8_t *p,uint32_t x) {for(int i=3;i>=0;i--){p[i]=(uint8_t)x;x>>=8;}}
int efs_kv_key_reap_prefix(uint32_t sh,uint8_t *p,uint32_t *n) {(void)sh;memset(p,0,3);*n=3;return 0;}
static int reap_scan_cb(void *u,const uint8_t *k,uint32_t n,const uint8_t *v,uint32_t z) {(void)u;(void)k;(void)n;(void)v;(void)z;return 0;}
int efs_kv_scan_from(struct efs_kv *kv,const uint8_t *p,uint32_t n,const uint8_t *start,uint32_t slen,int (*cb)(void *,const uint8_t *,uint32_t,const uint8_t *,uint32_t),void *u) {
 (void)kv;(void)p;(void)n;(void)cb;(void)start;(void)slen;
 struct reap_scan_ctx *c=u;c->n=1;c->ino[0]=42;c->gen[0]=7;c->lanes[0]=UINT64_MAX;return 0;
}
uint32_t efs_kv_lane_shard(efs_ino_t ino,uint8_t lane) {(void)lane;assert(ino==42);return 1;}
uint32_t efs_kv_inode_shard(efs_ino_t ino) {assert(ino==42);return 1;}
uint8_t efs_raft_shard_group(uint32_t sh) {(void)sh;return 0;}
static int host_hosts(struct efs_raft_host *h,uint8_t group) {(void)h;(void)group;return hosted;}
int efs_meta_lane_sweep_select(struct efs_kv *kv,efs_ino_t ino,uint64_t gen,uint8_t lane,uint32_t *indices,uint32_t *n) {
 (void)kv;assert(ino==42&&gen==7&&lane<64);indices[0]=1;*n=1;return 0;
}
static int host_gc_propose(struct efs_raft_host *h,uint8_t group,const uint8_t *cmd,uint32_t len) {
 (void)group;(void)len;
 if(!__atomic_load_n(&h->gc_running,__ATOMIC_ACQUIRE))return EFS_ERR_BUSY;
 if(cmd[0]==EFS_MD_CMD_LANE_SWEEP_BATCH || cmd[0]==EFS_MD_REQ_SWEEP_NEXT) {
  sweeps++;
  if(sweeps==stop_after)__atomic_store_n(&h->gc_running,0,__ATOMIC_RELEASE);
  return busy?EFS_ERR_BUSY:EFS_OK;
 }
 assert(cmd[0]==EFS_MD_CMD_REAP_BATCH);finishes++;return EFS_OK;
}
static int host_gc_inode_nodes(struct efs_raft_host *h,efs_ino_t ino,uint64_t gen) {
 assert(__atomic_load_n(&h->gc_running,__ATOMIC_ACQUIRE)&&ino==42&&gen==7);fences++;return 0;
}
int efs_meta_reap_select(struct efs_kv *kv,efs_ino_t ino,uint64_t gen,struct efs_meta_reap_key *keys,uint32_t *n) {
 (void)kv;assert(ino==42&&gen==7);selects++;memset(keys,0,sizeof(*keys));*n=1;return 0;
}
'''+finish+'\n'+s[a:b]+r'''
int main(void) {
 struct server server={0};struct efs_raft_host h={.s=&server,.running=1};
 for(hosted=0;hosted<=1;hosted++)for(busy=0;busy<=1;busy++)for(unsigned stopped=0;stopped<=64;stopped++) {
  struct reap_times times={0};sweeps=fences=finishes=selects=0;stop_after=stopped;
  __atomic_store_n(&h.gc_running,stopped!=0,__ATOMIC_RELEASE);
  host_gc_reap_pass(&h,0,1,&times);
  assert(sweeps==stopped && !fences && !selects && !finishes);
  /* Restart/resume must still visit the same durable marker and every lane. */
  __atomic_store_n(&h.gc_running,1,__ATOMIC_RELEASE);stop_after=UINT32_MAX;
  sweeps=fences=finishes=selects=0;host_gc_reap_pass(&h,0,1,&times);
  assert(sweeps==64);
  assert(fences==!busy && selects==!busy && finishes==!busy);
 }
 puts("GC reaper shutdown: stop before/after each64 local/remote lane, OK/BUSY paths retain marker; restart resumes all lanes PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='efs-gc-reap-stop-') as d:
 p=Path(d)/'test.c';p.write_text(code)
 subprocess.run(shlex.split(os.environ.get('CC','cc'))+['-std=gnu11','-Wall','-Wextra','-Werror','-I'+str(root/'include'),str(p),'-o',str(p.with_suffix(''))],check=True)
 subprocess.run([str(p.with_suffix(''))],check=True)
