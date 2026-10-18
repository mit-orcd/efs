#!/usr/bin/env python3
"""Actual W124 batched reaper: bounded multi-item sweeps per owning group,
completion read back from lane keys, all-member handoff, REAP_MULTI only for
accepted inodes, and shutdown between proposals leaves every durable marker."""
from pathlib import Path
import os,shlex,subprocess,tempfile
root=Path(__file__).resolve().parents[1]
s=(root/'src/server/raft_host.c').read_text()
a=s.index('/* W124 multi-entry builder');b=s.index('\n}',s.index('static void host_gc_reap_pass(',a))+2
ctx=s[s.index('struct reap_scan_ctx {'):s.index('\n};',s.index('struct reap_scan_ctx {'))+3]
code=r'''
#include "efs/meta_apply.h"
#include "efs/meta_cmd.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#define REAP_SCAN_MAX 512
#define REAP_ENTRY_BYTES (48u * 1024u)
#define REAP_KEY_LEN 11
#define GC_FRAG_BUDGET_US 100000
#define HOST_NGROUPS 2
#define efs_kv_scan_from fake_scan_from
#define efs_kv_get fake_kv_get
#define efs_kv_lane_shard fake_lane_shard
#define efs_kv_inode_shard fake_inode_shard
#define efs_kv_key_lane fake_key_lane
#define efs_raft_shard_group fake_shard_group
struct server {uint64_t gc_scan_errors,gc_reap_seen[2],gc_reap_capped[2],gc_first_reap[2],gc_first_seen_us[2];};
struct efs_raft_host {struct efs_kv *kv;struct server *s;int gc_running,running;uint8_t reap_cursor[2][REAP_KEY_LEN];uint32_t reap_cursor_len[2];};
struct reap_times {uint64_t scan,sweep,fence,done;};
'''+ctx+r'''
#define NI 3
static unsigned remaining[NI][64]; /* chunk rows left per (inode, lane) */
static unsigned per_lane,proposals,remote,multi_entries,fenced,reaped,stop_after,refuse;
static int hosted;
static uint64_t now_us_(void) {return 1;}
static int env_on(const char *s){(void)s;return 0;}
static uint64_t efs_iostats_now_us(void) {return 1;}
static int host_gc_metadata_waiting(struct efs_raft_host *h){(void)h;return 0;}
static uint64_t rd64(const uint8_t *p){uint64_t x=0;for(int i=0;i<8;i++)x=x<<8|p[i];return x;}
static uint32_t rd32(const uint8_t *p){uint32_t x=0;for(int i=0;i<4;i++)x=x<<8|p[i];return x;}
static void wr64be(uint8_t *p,uint64_t x) {for(int i=7;i>=0;i--){p[i]=(uint8_t)x;x>>=8;}}
static void wr32be(uint8_t *p,uint32_t x) {for(int i=3;i>=0;i--){p[i]=(uint8_t)x;x>>=8;}}
static int ix(efs_ino_t ino){assert(ino>=42&&ino<42+NI);return (int)(ino-42);}
int efs_kv_key_reap_prefix(uint32_t sh,uint8_t *p,uint32_t *n) {(void)sh;memset(p,0,3);*n=3;return 0;}
static int reap_scan_cb(void *u,const uint8_t *k,uint32_t n,const uint8_t *v,uint32_t z) {(void)u;(void)k;(void)n;(void)v;(void)z;return 0;}
static int efs_kv_scan_from(struct efs_kv *kv,const uint8_t *p,uint32_t n,const uint8_t *start,uint32_t slen,int (*cb)(void *,const uint8_t *,uint32_t,const uint8_t *,uint32_t),void *u) {
 (void)kv;(void)p;(void)n;(void)cb;(void)start;(void)slen;
 struct reap_scan_ctx *c=u;c->n=NI;
 for(int i=0;i<NI;i++){c->ino[i]=42+i;c->gen[i]=7;c->lanes[i]=i==2?0:UINT64_MAX;memset(c->keys[i],i,REAP_KEY_LEN);}
 return 0;
}
static uint32_t efs_kv_lane_shard(efs_ino_t ino,uint8_t lane) {(void)ino;return lane;}
static uint32_t efs_kv_inode_shard(efs_ino_t ino) {return (uint32_t)ino;}
static uint8_t efs_raft_shard_group(uint32_t sh) {return (sh&1)?0:2;}
static int host_hosts(struct efs_raft_host *h,uint8_t group) {(void)h;(void)group;return hosted;}
static int efs_kv_key_lane(uint32_t sh,efs_ino_t ino,uint64_t gen,uint8_t lane,uint8_t *out,uint32_t *len) {
 (void)sh;assert(gen==7);out[0]=(uint8_t)ix(ino);out[1]=lane;*len=2;return 0;
}
static int efs_kv_get(struct efs_kv *kv,const uint8_t *k,uint32_t kl,uint8_t *v,uint32_t *vl) {
 (void)kv;(void)v;(void)vl;assert(kl==2);return remaining[k[0]][k[1]]==UINT32_MAX?EFS_ERR_NOT_FOUND:EFS_OK;
}
int efs_meta_lane_sweep_select(struct efs_kv *kv,efs_ino_t ino,uint64_t gen,uint8_t lane,uint32_t *indices,uint32_t *n) {
 (void)kv;assert(gen==7&&lane<64);unsigned left=remaining[ix(ino)][lane];if(left==UINT32_MAX)left=0; /* swept lane: empty selection */
 *n=left<EFS_META_SWEEP_BATCH_MAX?left:EFS_META_SWEEP_BATCH_MAX;for(uint32_t i=0;i<*n;i++)indices[i]=i;return 0;
}
/* Apply one sweep: delete n chunk rows, then the lane key once none remain. */
static int sweep(efs_ino_t ino,uint8_t lane,unsigned n){
 unsigned *l=&remaining[ix(ino)][lane];if(*l==UINT32_MAX)return EFS_OK;
 assert(n<=*l);*l-=n;if(*l)return EFS_ERR_BUSY;*l=UINT32_MAX;return EFS_OK;
}
static int host_gc_propose(struct efs_raft_host *h,uint8_t group,const uint8_t *cmd,uint32_t len) {
 if(!__atomic_load_n(&h->gc_running,__ATOMIC_ACQUIRE))return EFS_ERR_BUSY;
 proposals++;
 if(proposals==stop_after)__atomic_store_n(&h->gc_running,0,__ATOMIC_RELEASE);
 if(cmd[0]==EFS_MD_REQ_SWEEP_NEXT){assert(len==18&&!hosted);remote++;
  return sweep(rd64(cmd+1),cmd[17],remaining[ix(rd64(cmd+1))][cmd[17]]==UINT32_MAX?0:
               (remaining[ix(rd64(cmd+1))][cmd[17]]<8?remaining[ix(rd64(cmd+1))][cmd[17]]:8));}
 uint32_t count=(uint32_t)cmd[1]<<8|cmd[2],off=3;assert(count);
 if(cmd[0]==EFS_MD_CMD_LANE_SWEEP_MULTI){
  assert(hosted);multi_entries++;
  for(uint32_t k=0;k<count;k++){efs_ino_t ino=rd64(cmd+off);assert(rd64(cmd+off+8)==7);
   uint8_t lane=cmd[off+16],n=cmd[off+17];assert(efs_raft_shard_group(lane)==group);
   for(unsigned j=0;j<n;j++)(void)rd32(cmd+off+18+4*j);
   (void)sweep(ino,lane,n);off+=18u+4u*n;}
  assert(off==len);return EFS_OK;
 }
 assert(cmd[0]==EFS_MD_CMD_REAP_MULTI);
 for(uint32_t k=0;k<count;k++){efs_ino_t ino=rd64(cmd+off);
  assert(efs_raft_shard_group(efs_kv_inode_shard(ino))==group);
  for(int lane=0;lane<64;lane++)assert(remaining[ix(ino)][lane]==UINT32_MAX);
  reaped++;off+=17u+32u*cmd[off+16];}
 assert(off==len);return EFS_OK;
}
static void host_gc_inode_nodes_batch(struct efs_raft_host *h,const efs_ino_t *inos,const uint64_t *gens,uint32_t n,uint8_t *ok) {
 assert(__atomic_load_n(&h->gc_running,__ATOMIC_ACQUIRE));
 for(uint32_t i=0;i<n;i++){assert(gens[i]==7);for(int lane=0;lane<64;lane++)assert(remaining[ix(inos[i])][lane]==UINT32_MAX);
  ok[i]=inos[i]!=refuse;fenced++;}
}
int efs_meta_reap_select(struct efs_kv *kv,efs_ino_t ino,uint64_t gen,struct efs_meta_reap_key *keys,uint32_t *n) {
 (void)kv;(void)ino;assert(gen==7);memset(keys,0,sizeof(*keys));keys[0].len=3;*n=1;return 0;
}
'''+s[a:b]+r'''
static void reset(unsigned chunks){
 per_lane=chunks;proposals=remote=multi_entries=fenced=reaped=0;
 for(int i=0;i<NI;i++)for(int l=0;l<64;l++)remaining[i][l]=i==2?UINT32_MAX:chunks;
}
static void pass(struct efs_raft_host *h){struct reap_times t={0};host_gc_reap_pass(h,0,1,&t);}
int main(void) {
 struct server server={0};struct efs_raft_host h={.s=&server,.running=1};
 for(hosted=0;hosted<=1;hosted++) {
  /* Every lane drains in one pass: all three inodes complete (inode 44 has
   * no lanes at all); hosted lanes need one entry per owning group. */
  reset(5);refuse=0;stop_after=UINT32_MAX;__atomic_store_n(&h.gc_running,1,__ATOMIC_RELEASE);
  pass(&h);
  assert(fenced==3 && reaped==3);
  if(hosted) assert(multi_entries==2 && !remote); else assert(remote==128 && !multi_entries);
  /* A member refusing one inode keeps only that marker. */
  reset(5);refuse=43;pass(&h);assert(fenced==3 && reaped==2);refuse=0;
  /* Large lanes: hosted sweeps run bounded rounds within the pass; remote
   * lanes advance one batch per pass. Nothing completes early. */
  reset(300);pass(&h);
  if(hosted) assert(fenced==1 && reaped==1); /* 300 > 32 rounds x 8: only the empty inode */
  else assert(fenced==1 && reaped==1 && remote==128);
  reset(100);pass(&h);
  if(hosted) assert(fenced==3 && reaped==3 && multi_entries>=2*13); else assert(fenced==1 && reaped==1);
  /* Shutdown at any proposal: no handoff and no completion for unswept
   * inodes; the durable markers drive the next pass. */
  /* Stop at each sweep proposal (hosted: one entry per group; remote: one
   * request per lane). A proposal made before the stop still applies. */
  for(unsigned stop=1;stop<=(hosted?2:128);stop++){
   reset(5);stop_after=stop;__atomic_store_n(&h.gc_running,1,__ATOMIC_RELEASE);
   pass(&h);
   assert(!__atomic_load_n(&h.gc_running,__ATOMIC_ACQUIRE));
   assert(!reaped);
   __atomic_store_n(&h.gc_running,1,__ATOMIC_RELEASE);stop_after=UINT32_MAX;
   pass(&h);assert(reaped==3);
  }
 }
 puts("GC batched reaper: per-group multi sweeps, read-back completion, per-item handoff, bounded rounds, shutdown keeps markers PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='efs-gc-reap-stop-') as d:
 p=Path(d)/'test.c';p.write_text(code)
 subprocess.run(shlex.split(os.environ.get('CC','cc'))+['-std=gnu11','-Wall','-Wextra','-Werror','-Wno-unused-function','-I'+str(root/'include'),str(p),'-o',str(p.with_suffix(''))],check=True)
 subprocess.run([str(p.with_suffix(''))],check=True)
