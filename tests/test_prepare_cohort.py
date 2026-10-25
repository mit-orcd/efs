#!/usr/bin/env python3
"""Bound async PREPs, preserve order/verdict IDs and close before remote work.

K29: a transaction's RESOLVEs share one TXN_BATCH entry per Raft group (split
at the cohort item size); every shard is resolved, in order, and a group with
one RESOLVE proposes it unchanged."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[1];s=(root/'src/server/raft_host.c').read_text()
a=s.index('#define HOST_CMD_COHORT_MAX');b=s.index('/* DECIDE COMMIT at the coordinator',a)
ca=s.index('static int host_txn_commit(');cb=s.index('\n}',ca)+2
ra=s.index('static int host_resize_resolve(');rb=s.index('\n}',ra)+2
ba=s.index('#define HOST_TXN_BATCH_BYTES');bb=s.index('static int host_txn_commit(',ba)
code=r'''
#include "efs/raft.h"
#include "efs/txn.h"
#include "efs/meta_cmd.h"
#include <assert.h>
#include <stdlib.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
struct efs_raft {int role;unsigned voters;};
struct efs_raft_disk {unsigned depth;};
struct efs_raft_host {pthread_mutex_t mu;struct efs_raft_disk *disk;struct efs_raft r[3];};
struct host_idx_ref {uint8_t group;uint64_t idx,term;};
static unsigned sequence,holds,releases,fail_call,sync_fail,external_drop;
static unsigned ids[256],groups[256],modes[256],commands,shards[512],shard_groups[512],entry_commands[256];
#define HOST_NGROUPS 2
static uint32_t rd32(const uint8_t *p){return (uint32_t)p[0]<<24|(uint32_t)p[1]<<16|(uint32_t)p[2]<<8|p[3];}
/* One RESOLVE (22 bytes) or a TXN_BATCH of them: record every shard. */
static unsigned record(const uint8_t *cmd,uint32_t n,uint8_t group){
 if(n==22){assert(cmd[0]==EFS_MD_CMD_RESOLVE);shards[commands]=rd32(cmd+17);shard_groups[commands++]=group;return 1;}
 assert(n>=4 && cmd[0]==EFS_MD_CMD_TXN_BATCH && cmd[1]==EFS_MD_CMD_RESOLVE);
 unsigned count=(unsigned)cmd[2]<<8|cmd[3],at=4;assert(count>=2 && count<=EFS_MD_TXN_BATCH_MAX);
 for(unsigned i=0;i<count;i++){unsigned len=(unsigned)cmd[at]<<8|cmd[at+1];at+=2;assert(len==22);
  assert(efs_raft_shard_group(rd32(cmd+at+17))==group);record(cmd+at,len,group);at+=len;}
 assert(at==n && n<=EFS_TXN_PREPARE_MAX);return count;
}
#define HOST_FIN_ABORT 2
static int decide_rc,wait_rc;static unsigned decided,finished,waited,retired,aborted;
static struct efs_raft *group_raft(struct efs_raft_host *h,uint8_t g){return &h->r[g];}
int efs_raft_role(const struct efs_raft *r){return r->role;}
uint32_t efs_raft_voters(const struct efs_raft *r){return r->voters;}
static int efs_raft_disk_sync_hold(struct efs_raft_disk *d){d->depth++;holds++;return 0;}
static int host_propose_mode(struct efs_raft_host *h,uint8_t group,const uint8_t *cmd,
 uint32_t n,uint64_t *idx,uint64_t *term,int *hint,int local){
 (void)hint;assert(sequence<256);
 if(external_drop){assert(local&&h->disk->depth==2);h->disk->depth--;external_drop=0;}
 entry_commands[sequence]=n==1?1:record(cmd,n,group);
 ids[sequence]=cmd[0];groups[sequence]=group;modes[sequence]=local;sequence++;
 if(h->r[group].role!=EFS_RAFT_LEADER)assert(!h->disk->depth); /* Never forward inside hold. */
 if(fail_call==sequence)return EFS_ERR_NOT_PRIMARY;
 *idx=sequence;*term=7;return 0;
}
static int host_propose(struct efs_raft_host *h,uint8_t group,const uint8_t *cmd,uint32_t n,
 uint64_t *idx,uint64_t *term,int *hint){return host_propose_mode(h,group,cmd,n,idx,term,hint,0);}
'''+s[a:b]+r'''
#define HOST_DECIDE_MAX (23u+4u*EFS_TXN_MAX_PART)
static uint32_t pack_decide(uint8_t *cmd,const struct efs_txid *t,uint32_t sh,int state,const struct efs_txn_parts *p){(void)t;(void)sh;assert(state==EFS_TXN_COMMIT && p && p->n && p->n<=EFS_TXN_MAX_PART);memset(cmd,0,22);return 22;}
static void pack_resolve(uint8_t *cmd,const struct efs_txid *t,uint32_t sh,int state){(void)t;assert(state==EFS_TXN_COMMIT);memset(cmd,0,22);
 cmd[0]=EFS_MD_CMD_RESOLVE;cmd[17]=(uint8_t)(sh>>24);cmd[18]=(uint8_t)(sh>>16);cmd[19]=(uint8_t)(sh>>8);cmd[20]=(uint8_t)sh;cmd[21]=(uint8_t)state;assert(decided);}
static int host_propose_wait(struct efs_raft_host *h,uint8_t group,const uint8_t *cmd,uint32_t n,int *hint){(void)h;(void)group;(void)cmd;(void)hint;assert(n==22 && !sequence);if(!decide_rc)decided++;return decide_rc;}
static void host_fin_add(struct efs_raft_host *h,const struct efs_txid *t,const struct efs_txn_parts *p,uint32_t coord,int retire_only){(void)h;(void)t;(void)p;(void)coord;if(retire_only==HOST_FIN_ABORT)aborted++;else if(retire_only)retired++;else finished++;}
static int host_wait_refs(struct efs_raft_host *h,struct host_idx_ref *refs,int n,int *hint){(void)h;(void)hint;assert(decided);waited++;for(int i=0;i<n;i++)assert(refs[i].idx==(uint64_t)i+1 && refs[i].term==7);return wait_rc;}
'''+s[ba:bb]+s[ca:cb]+r'''
struct host_resize_ctx {struct efs_raft_host *host;int *hint;};
uint32_t efs_txn_coordinator(const struct efs_txid*t,const struct efs_txn_parts*p){(void)t;(void)p;return 1;}
'''+s[ra:rb]+r'''
static int host_report_release(struct efs_raft_host *h,int held){
 assert(held && h->disk->depth==1);h->disk->depth--;releases++;return sync_fail?EFS_ERR_IO:0;
}
static void reset(void){sequence=holds=releases=fail_call=sync_fail=decided=finished=waited=retired=aborted=commands=0;decide_rc=wait_rc=0;}
/* Every shard resolved exactly once, each group's shards in part order. */
static void resolved_in_order(const struct efs_txn_parts *parts){
 assert(commands==parts->n);
 for(unsigned g=0;g<=2;g+=2){unsigned k=0;
  for(unsigned i=0;i<parts->n;i++){if(efs_raft_shard_group(parts->shard[i])!=g)continue;
   while(k<commands && shard_groups[k]!=g){k++;}
   assert(k<commands && shards[k]==parts->shard[i]);k++;}}
}
int main(void){
 struct efs_raft_disk disk={0};struct efs_raft_host h={.disk=&disk};
 pthread_mutex_init(&h.mu,NULL);h.r[0].role=EFS_RAFT_LEADER;h.r[0].voters=7;
 h.r[2].role=EFS_RAFT_FOLLOWER;h.r[2].voters=7;
 struct host_command_cohort c;struct host_idx_ref refs[17]={0};int hint=-1;
 reset();host_command_cohort_begin(&c,&h);
 for(unsigned i=0;i<5;i++){uint8_t cmd=i;refs[i].group=(i==2 || i==3)?2:0;
   assert(!host_command_cohort_add(&h,&refs[i],&cmd,1,&hint));}
 assert(!sequence && c.n==5);assert(!host_command_cohort_end(&c,0,&hint));
 assert(sequence==5 && holds==1 && releases==1 && !disk.depth && !host_command_capture);
 for(unsigned i=0;i<5;i++)assert(ids[i]==i && refs[i].idx==i+1 && refs[i].term==7);
 assert(modes[0] && modes[1] && !modes[2] && !modes[3] && !modes[4]);
 reset();h.r[2].role=EFS_RAFT_LEADER;host_command_cohort_begin(&c,&h);
 for(unsigned i=0;i<8;i++){uint8_t cmd=i;refs[i].group=(i&1)?2:0;assert(!host_command_cohort_add(&h,&refs[i],&cmd,1,&hint));}
 assert(!host_command_cohort_end(&c,0,&hint) && holds==1 && releases==1 && !disk.depth);
 for(unsigned i=0;i<8;i++)assert(ids[i]==i && groups[i]==((i&1)?2:0) && modes[i] && refs[i].idx==i+1);
 h.r[2].role=EFS_RAFT_FOLLOWER;
 reset();memset(refs,0,sizeof(refs));host_command_cohort_begin(&c,&h);
 for(unsigned i=0;i<17;i++){uint8_t cmd=i;assert(!host_command_cohort_add(&h,&refs[i],&cmd,1,&hint));assert(c.n<=8);}
 assert(sequence==16 && c.n==1);assert(!host_command_cohort_end(&c,0,&hint));
 assert(sequence==17 && holds==2 && releases==2 && !disk.depth);
 for(unsigned i=0;i<17;i++)assert(ids[i]==i && refs[i].idx==i+1);
 for(unsigned failure=0;failure<2;failure++){
  reset();fail_call=2;sync_fail=failure;memset(refs,0,sizeof(refs));host_command_cohort_begin(&c,&h);
  for(unsigned i=0;i<3;i++){uint8_t cmd=i;assert(!host_command_cohort_add(&h,&refs[i],&cmd,1,&hint));}
  assert(host_command_cohort_end(&c,0,&hint)==(failure?EFS_ERR_IO:EFS_ERR_NOT_PRIMARY));
  assert(sequence==2 && holds==1 && releases==1 && !disk.depth && refs[0].idx==1 && !refs[1].idx && !refs[2].idx);
 }
 reset();host_command_cohort_begin(&c,&h);uint8_t cmd=1;
 assert(!host_command_cohort_add(&h,&refs[0],&cmd,1,&hint));
 assert(host_command_cohort_end(&c,EFS_ERR_BUSY,&hint)==EFS_ERR_BUSY && !sequence && !disk.depth);
 reset();disk.depth=1;external_drop=1;host_command_cohort_begin(&c,&h);
 for(unsigned i=0;i<2;i++)assert(!host_command_cohort_add(&h,&refs[i],&cmd,1,&hint));
 assert(!host_command_cohort_end(&c,0,&hint) && holds==1 && releases==1 && !disk.depth && !external_drop);
 assert(!host_command_cohort_add(&h,&refs[0],&cmd,1,&hint)); /* Ordinary callers unchanged. */
 struct efs_txid tx={0};struct efs_txn_parts parts={0};parts.n=EFS_TXN_MAX_PART;
 for(unsigned i=0;i<parts.n;i++)parts.shard[i]=i+1;
 h.r[2].role=EFS_RAFT_LEADER;reset();
 /* 144 parts, 72 per group: one RESOLVE entry per group, one shared hold. */
 assert(!host_txn_commit(&h,&tx,&parts,1,&hint) && decided==1 && waited==1 && !finished && retired==1);
 assert(sequence==2 && holds==1 && releases==holds && !disk.depth);
 assert(groups[0]==0 && groups[1]==2 && entry_commands[0]==72 && entry_commands[1]==72);
 resolved_in_order(&parts);
 /* One group only: split at the cohort item size, order kept. */
 struct efs_txn_parts odd={0};odd.n=EFS_TXN_MAX_PART;
 for(unsigned i=0;i<odd.n;i++)odd.shard[i]=2*i+1;
 reset();assert(!host_txn_commit(&h,&tx,&odd,1,&hint) && waited==1 && retired==1);
 unsigned per=(EFS_TXN_PREPARE_MAX-4)/24;if(per>EFS_MD_TXN_BATCH_MAX)per=EFS_MD_TXN_BATCH_MAX;
 assert(sequence==2 && entry_commands[0]==per && entry_commands[1]==EFS_TXN_MAX_PART-per && groups[0]==0 && groups[1]==0);
 resolved_in_order(&odd);
 /* A group with a single RESOLVE proposes it unchanged. */
 struct efs_txn_parts two={0};two.n=2;two.shard[0]=7;two.shard[1]=8;
 reset();assert(!host_txn_commit(&h,&tx,&two,7,&hint) && sequence==2 && entry_commands[0]==1 && entry_commands[1]==1);
 resolved_in_order(&two);
 reset();decide_rc=EFS_ERR_BUSY;assert(host_txn_commit(&h,&tx,&parts,1,&hint)==EFS_ERR_BUSY && !sequence && finished==1);
 reset();decide_rc=EFS_ERR_STALE;assert(host_txn_commit(&h,&tx,&parts,1,&hint)==EFS_ERR_STALE && !sequence && !finished && !aborted);
 /* A11 finding 2: a DECIDE lost to a step-down goes to the finisher as ABORT
  * (decide-once keeps a surviving COMMIT); decided elsewhere: nothing. */
 reset();decide_rc=EFS_ERR_NOT_PRIMARY;assert(host_txn_commit(&h,&tx,&parts,1,&hint)==EFS_ERR_NOT_PRIMARY && !sequence && !finished && aborted==1);
 reset();decide_rc=EFS_ERR_PROTO;assert(host_txn_commit(&h,&tx,&parts,1,&hint)==EFS_ERR_PROTO && !sequence && !finished && !aborted);
 reset();fail_call=2;assert(host_txn_commit(&h,&tx,&parts,1,&hint)==EFS_ERR_NOT_PRIMARY && finished==1 && !waited && !disk.depth);
 reset();sync_fail=1;assert(host_txn_commit(&h,&tx,&parts,1,&hint)==EFS_ERR_IO && finished==1 && !waited && !disk.depth);
 reset();wait_rc=EFS_ERR_STALE;assert(host_txn_commit(&h,&tx,&parts,1,&hint)==EFS_ERR_STALE && finished==1 && waited==1 && !disk.depth);
 struct host_resize_ctx ctx={&h,&hint};reset();decided=1;
 assert(!host_resize_resolve(&ctx,&tx,&parts,EFS_TXN_COMMIT) && waited==1 && !finished && retired==1);
 assert(sequence==2 && holds==1 && releases==holds && !disk.depth);
 resolved_in_order(&parts);
 reset();decided=1;sync_fail=1;
 assert(host_resize_resolve(&ctx,&tx,&parts,EFS_TXN_COMMIT)==EFS_ERR_IO && finished==1 && !waited && !disk.depth);
 reset();decided=1;wait_rc=EFS_ERR_STALE;
 assert(host_resize_resolve(&ctx,&tx,&parts,EFS_TXN_COMMIT)==EFS_ERR_STALE && finished==1 && waited==1);
 pthread_mutex_destroy(&h.mu);
 puts("command cohort: eight-entry bound, cross-group ordering, identities, role/fsync failure, remote boundaries, 144-part RESOLVE in one entry per group (split, order, single command unchanged) and durable-decision/finisher gates PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='efs-prepare-cohort-') as d:
    c=Path(d)/'test.c';c.write_text(code);binary=c.with_suffix('')
    subprocess.run(['cc','-std=c99','-D_GNU_SOURCE','-Wall','-Wextra','-Werror','-I'+str(root/'include'),str(c),'-pthread','-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
