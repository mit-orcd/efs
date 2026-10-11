#!/usr/bin/env python3
"""Captured append PREPs must settle before a decision; ambiguous COMMIT stays."""
from pathlib import Path
import os,subprocess,tempfile
root=Path(__file__).resolve().parents[1];s=(root/'src/server/raft_host.c').read_text()
a=s.index('#define HOST_CMD_COHORT_MAX');b=s.index('/* DECIDE COMMIT at the coordinator',a)
c=s.index('struct host_append_batch {');d=s.index('static int host_append_transaction(',c)
code=r'''
#include "efs/raft.h"
#include "efs/meta_apply.h"
#include <assert.h>
#include <pthread.h>
#include <string.h>
#include <stdio.h>
struct efs_raft {int role;};struct efs_raft_disk {int depth;};
struct efs_raft_host {struct efs_kv *kv;pthread_mutex_t mu;struct efs_raft_disk *disk;struct efs_raft r;};
struct host_idx_ref {uint8_t group;uint64_t idx,term;};
struct host_resize_ctx {struct efs_raft_host *host;int *hint;};
static unsigned submitted,holds,waited,commits,aborts,resolves,prepared;
static int sync_rc,wait_rc,commit_rc,abort_rc,pack_rc,execute_rc;
static struct efs_raft *group_raft(struct efs_raft_host *h,uint8_t g){(void)g;return &h->r;}
int efs_raft_role(const struct efs_raft *r){return r->role;}
uint32_t efs_raft_voters(const struct efs_raft *r){(void)r;return 7;}
static int efs_raft_disk_sync_depth(struct efs_raft_disk *d){return d->depth;}
static int efs_raft_disk_sync_hold(struct efs_raft_disk *d){d->depth++;holds++;return 0;}
static int host_propose_mode(struct efs_raft_host*h,uint8_t g,const uint8_t*p,uint32_t n,uint64_t*i,uint64_t*t,int*hint,int local){
 (void)h;(void)g;(void)hint;(void)local;assert(n==1 && p[0]==submitted);*i=++submitted;*t=3;return 0;}
static int host_propose(struct efs_raft_host*h,uint8_t g,const uint8_t*p,uint32_t n,uint64_t*i,uint64_t*t,int*hint){return host_propose_mode(h,g,p,n,i,t,hint,0);}
'''+s[a:b]+r'''
static int host_report_release(struct efs_raft_host*h,int held){assert(held && h->disk->depth);h->disk->depth--;return sync_rc;}
int efs_txn_encode_excl_value(uint8_t *p,uint32_t cap,uint32_t *pn,const uint8_t *before,uint32_t bn,int op,const uint8_t *after,uint32_t an){
 (void)cap;(void)before;(void)bn;(void)op;(void)after;(void)an;p[0]=(uint8_t)prepared++;*pn=1;return pack_rc;}
static int host_prep_raw(struct efs_raft_host*h,uint32_t sh,int kind,const struct efs_txid*t,const struct efs_txn_parts*parts,const uint8_t*k,uint32_t kl,const uint8_t*p,uint32_t pn,struct host_idx_ref*r,int*hint){
 (void)t;(void)parts;(void)k;(void)kl;assert(kind==EFS_TXN_EXCL_VALUE && r);r->group=efs_raft_shard_group(sh);return host_command_cohort_add(h,r,p,pn,hint);}
static int host_wait_refs(struct efs_raft_host*h,struct host_idx_ref*r,int n,int*hint){
 (void)h;(void)hint;assert(!host_command_capture && !commits);waited++;for(int i=0;i<n;i++)assert(r[i].idx==(uint64_t)i+1 && r[i].term==3);return wait_rc;}
static int host_resize_decide(void *u,const struct efs_txid*t,const struct efs_txn_parts*p,int decision){
 (void)u;(void)t;(void)p;assert(!host_command_capture);
 if(decision==EFS_TXN_COMMIT){assert(waited && !wait_rc && !sync_rc);commits++;return commit_rc;}
 assert(decision==EFS_TXN_ABORT);aborts++;return abort_rc;}
static int host_resize_resolve(void *u,const struct efs_txid*t,const struct efs_txn_parts*p,int decision){
 (void)u;(void)t;(void)p;assert(!host_command_capture);assert(decision==EFS_TXN_COMMIT?commits:aborts);resolves++;return 0;}
int efs_meta_execute_append(const struct efs_meta_append_plan *p,const struct efs_txid*t,const struct efs_meta_append_ops*ops,void*ctx){
 if(execute_rc)return execute_rc;struct efs_meta_append_image im={0};
 for(unsigned i=0;i<p->count;i++){im.key[1]=(uint8_t)i;int rc=ops->prepare(ctx,t,&p->parts,&im);if(rc){int aborted=ops->decide(ctx,t,&p->parts,EFS_TXN_ABORT);if(aborted)return aborted;(void)ops->resolve(ctx,t,&p->parts,EFS_TXN_ABORT);return rc;}}
 if(!p->count)return 0;int rc=ops->decide(ctx,t,&p->parts,EFS_TXN_COMMIT);return rc?rc:ops->resolve(ctx,t,&p->parts,EFS_TXN_COMMIT);}
int efs_meta_execute_append_owned(struct efs_kv*k,const struct efs_meta_append_plan*p,const struct efs_opid_req*q,const struct efs_txid*t,const struct efs_meta_append_ops*ops,void*ctx){(void)k;(void)q;return efs_meta_execute_append(p,t,ops,ctx);}
'''+s[c:d]+r'''
static void reset(void){submitted=holds=waited=commits=aborts=resolves=prepared=0;sync_rc=wait_rc=commit_rc=abort_rc=pack_rc=execute_rc=0;}
int main(void){struct efs_raft_disk disk={0};struct efs_raft_host h={.mu=PTHREAD_MUTEX_INITIALIZER,.disk=&disk,.r={.role=EFS_RAFT_LEADER}};
 struct efs_meta_append_plan p={.count=17};struct efs_txid t={0};struct efs_opid_req q={0};int hint=-1;
 assert(!host_append_execute_batched(&h,&p,&q,&t,1,&hint));
 assert(submitted==17 && holds==2 && waited==1 && commits==1 && !aborts && resolves==1 && !disk.depth && !host_command_capture);
 reset();wait_rc=EFS_ERR_STALE;assert(host_append_execute_batched(&h,&p,&q,&t,0,&hint)==EFS_ERR_STALE);
 assert(submitted==17 && waited && !commits && aborts==1 && resolves==1);
 reset();sync_rc=EFS_ERR_IO;assert(host_append_execute_batched(&h,&p,&q,&t,0,&hint)==EFS_ERR_IO);
 assert(!commits && aborts==1 && resolves==1 && !disk.depth && !host_command_capture);
 reset();wait_rc=EFS_ERR_BUSY;abort_rc=EFS_ERR_IO;assert(host_append_execute_batched(&h,&p,&q,&t,0,&hint)==EFS_ERR_IO && !commits && !resolves);
 reset();commit_rc=EFS_ERR_BUSY;assert(host_append_execute_batched(&h,&p,&q,&t,0,&hint)==EFS_ERR_BUSY && commits==1 && !aborts && !resolves);
 reset();pack_rc=EFS_ERR_INVAL;assert(host_append_execute_batched(&h,&p,&q,&t,0,&hint)==EFS_ERR_INVAL && !submitted && aborts==1 && resolves==1);
 reset();p.count=EFS_META_LANES+5;
 assert(!host_append_execute_batched(&h,&p,&q,&t,1,&hint) && submitted==EFS_META_LANES+5 && commits==1);
 reset();p.count=EFS_META_LANES+6;
 assert(host_append_execute_batched(&h,&p,&q,&t,1,&hint)==EFS_ERR_OVERFLOW && !commits && aborts==1 && !host_command_capture);
 reset();p.count=0;assert(!host_append_execute_batched(&h,&p,&q,&t,0,&hint) && !host_command_capture && !submitted && !commits);
 execute_rc=EFS_ERR_INVAL;assert(host_append_execute_batched(&h,&p,&q,&t,0,&hint)==EFS_ERR_INVAL && !host_command_capture);
 struct host_command_cohort other;host_command_cohort_begin(&other,&h);
 assert(host_append_execute_batched(&h,&p,&q,&t,0,&hint)==EFS_ERR_BUSY && host_command_capture==&other);host_command_cohort_end(&other,EFS_ERR_BUSY,&hint);
 puts("append cohort: captured identities, bounded batches, verdict-before-COMMIT, pre-decision ABORT, ambiguous COMMIT and TLS cleanup PASS");}
'''
with tempfile.TemporaryDirectory(prefix='efs-append-cohort-') as d:
 p=Path(d)/'t.c';p.write_text(code)
 flags=['-fsanitize=address,undefined'] if os.getenv('EFS_TEST_SANITIZE')=='1' else []
 subprocess.run(['cc',*flags,'-std=gnu11','-Wall','-Wextra','-Werror','-Wno-misleading-indentation','-pthread','-I'+str(root/'include'),str(p),'-o',str(p.with_suffix(''))],check=True)
 subprocess.run([str(p.with_suffix(''))],check=True)
