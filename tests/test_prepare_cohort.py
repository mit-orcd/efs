#!/usr/bin/env python3
"""Bound async PREPs, preserve order/verdict IDs and close before remote work."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[1];s=(root/'src/server/raft_host.c').read_text()
a=s.index('#define HOST_PREP_COHORT_MAX');b=s.index('static int host_prep_async(',a)
code=r'''
#include "efs/raft.h"
#include "efs/txn.h"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
struct efs_raft {int role;unsigned voters;};
struct efs_raft_disk {unsigned depth;};
struct efs_raft_host {pthread_mutex_t mu;struct efs_raft_disk *disk;struct efs_raft r[3];};
struct host_idx_ref {uint8_t group;uint64_t idx,term;};
static unsigned sequence,holds,releases,fail_call,sync_fail;
static unsigned ids[64],groups[64],modes[64];
static struct efs_raft *group_raft(struct efs_raft_host *h,uint8_t g){return &h->r[g];}
int efs_raft_role(const struct efs_raft *r){return r->role;}
uint32_t efs_raft_voters(const struct efs_raft *r){return r->voters;}
static int efs_raft_disk_sync_depth(struct efs_raft_disk *d){return d->depth;}
static int efs_raft_disk_sync_hold(struct efs_raft_disk *d){d->depth++;holds++;return 0;}
static int host_propose_mode(struct efs_raft_host *h,uint8_t group,const uint8_t *cmd,
 uint32_t n,uint64_t *idx,uint64_t *term,int *hint,int local){
 (void)hint;assert(n==1);assert(sequence<64);
 ids[sequence]=cmd[0];groups[sequence]=group;modes[sequence]=local;sequence++;
 if(h->r[group].role!=EFS_RAFT_LEADER)assert(!h->disk->depth); /* Never forward inside hold. */
 if(fail_call==sequence)return EFS_ERR_NOT_PRIMARY;
 *idx=sequence;*term=7;return 0;
}
static int host_propose(struct efs_raft_host *h,uint8_t group,const uint8_t *cmd,uint32_t n,
 uint64_t *idx,uint64_t *term,int *hint){return host_propose_mode(h,group,cmd,n,idx,term,hint,0);}
'''+s[a:b]+r'''
static int host_report_release(struct efs_raft_host *h,int held){
 assert(held && h->disk->depth==1);h->disk->depth--;releases++;return sync_fail?EFS_ERR_IO:0;
}
static void reset(void){sequence=holds=releases=fail_call=sync_fail=0;}
int main(void){
 struct efs_raft_disk disk={0};struct efs_raft_host h={.disk=&disk};
 pthread_mutex_init(&h.mu,NULL);h.r[0].role=EFS_RAFT_LEADER;h.r[0].voters=7;
 h.r[2].role=EFS_RAFT_FOLLOWER;h.r[2].voters=7;
 struct host_prepare_cohort c;struct host_idx_ref refs[17]={0};int hint=-1;
 reset();host_prepare_cohort_begin(&c,&h);
 for(unsigned i=0;i<5;i++){uint8_t cmd=i;refs[i].group=(i==2 || i==3)?2:0;
   assert(!host_prepare_cohort_add(&h,&refs[i],&cmd,1,&hint));}
 assert(!sequence && c.n==5);assert(!host_prepare_cohort_end(&c,0,&hint));
 assert(sequence==5 && holds==1 && releases==1 && !disk.depth && !host_prepare_capture);
 for(unsigned i=0;i<5;i++)assert(ids[i]==i && refs[i].idx==i+1 && refs[i].term==7);
 assert(modes[0] && modes[1] && !modes[2] && !modes[3] && !modes[4]);
 reset();memset(refs,0,sizeof(refs));host_prepare_cohort_begin(&c,&h);
 for(unsigned i=0;i<17;i++){uint8_t cmd=i;assert(!host_prepare_cohort_add(&h,&refs[i],&cmd,1,&hint));assert(c.n<=8);}
 assert(sequence==16 && c.n==1);assert(!host_prepare_cohort_end(&c,0,&hint));
 assert(sequence==17 && holds==2 && releases==2 && !disk.depth);
 for(unsigned i=0;i<17;i++)assert(ids[i]==i && refs[i].idx==i+1);
 for(unsigned failure=0;failure<2;failure++){
  reset();fail_call=2;sync_fail=failure;memset(refs,0,sizeof(refs));host_prepare_cohort_begin(&c,&h);
  for(unsigned i=0;i<3;i++){uint8_t cmd=i;assert(!host_prepare_cohort_add(&h,&refs[i],&cmd,1,&hint));}
  assert(host_prepare_cohort_end(&c,0,&hint)==(failure?EFS_ERR_IO:EFS_ERR_NOT_PRIMARY));
  assert(sequence==2 && holds==1 && releases==1 && !disk.depth && refs[0].idx==1 && !refs[1].idx && !refs[2].idx);
 }
 reset();host_prepare_cohort_begin(&c,&h);uint8_t cmd=1;
 assert(!host_prepare_cohort_add(&h,&refs[0],&cmd,1,&hint));
 assert(host_prepare_cohort_end(&c,EFS_ERR_BUSY,&hint)==EFS_ERR_BUSY && !sequence && !disk.depth);
 reset();disk.depth=1;host_prepare_cohort_begin(&c,&h);
 for(unsigned i=0;i<2;i++)assert(!host_prepare_cohort_add(&h,&refs[i],&cmd,1,&hint));
 assert(!host_prepare_cohort_end(&c,0,&hint) && !holds && !releases && disk.depth==1);disk.depth=0;
 assert(!host_prepare_cohort_add(&h,&refs[0],&cmd,1,&hint)); /* Ordinary callers unchanged. */
 pthread_mutex_destroy(&h.mu);
 puts("prepare cohort: eight-entry bound, ordering, identities, role/fsync failure, remote escape and shared holds PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='efs-prepare-cohort-') as d:
    c=Path(d)/'test.c';c.write_text(code);binary=c.with_suffix('')
    subprocess.run(['cc','-std=c99','-D_GNU_SOURCE','-Wall','-Wextra','-Werror','-I'+str(root/'include'),str(c),'-pthread','-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
