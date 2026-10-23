#!/usr/bin/env python3
"""Actual append transport batches after ordered guards, before a checked decision.

K29: after the two synchronous guards (session owner, inode), a reservation's
exact images are proposed as one TXN_BATCH entry per Raft group when the
decision is reached, not one entry per image: a file with 64 active lanes
costs 2 PREPARE entries instead of 67. Every image is still prepared, every
entry's applied verdict is checked before COMMIT, and an ABORT proposes no
buffered image. Negative control: EFS_APPEND_BATCH_PER_IMAGE=1 restores the
per-image proposal and must fail the K29 count."""
from pathlib import Path
import os,shlex,subprocess,tempfile
root=Path(__file__).resolve().parents[1]
s=(root/'src/server/raft_host.c').read_text()
def function(name):
 a=s.index('static int '+name+'(');return s[a:s.index('\n}',a)+2]
start=s.index('static int host_append_prepare_ref(')
end=s.index('static int host_append_authority(',start)
helpers=s[start:end]
ca=s.index('#define HOST_CMD_COHORT_MAX');cb=s.index('/* DECIDE COMMIT at the coordinator',ca)
cohort=s[ca:cb]
ba=s.index('#define HOST_TXN_BATCH_BYTES');bb=s.index('static int host_txn_commit(',ba)
builder=s[ba:bb]
if os.environ.get('EFS_APPEND_BATCH_PER_IMAGE')=='1':
 a=helpers.index('static int host_append_reserve_prepare(');b=helpers.index('\n}',a)+2
 helpers=helpers[:a]+'''static int host_append_reserve_prepare(void *user,const struct efs_txid *t,
 const struct efs_txn_parts *parts,const struct efs_meta_append_image *im) {
 struct host_append_reserve_ctx *ctx=user;
 if(ctx->leading){int rc=host_append_prepare(&ctx->base,t,parts,im);if(rc==EFS_OK)--ctx->leading;return rc;}
 if(ctx->nrefs>=(int)(sizeof(ctx->refs)/sizeof(ctx->refs[0])))return EFS_ERR_INVAL;
 int rc=host_append_prepare_ref(&ctx->base,t,parts,im,&ctx->refs[ctx->nrefs]);
 if(rc==EFS_OK)++ctx->nrefs;
 return rc;
}'''+helpers[b:]
code=r'''
#include <stdint.h>
#include "efs/meta_cmd.h"
#define EFS_META_APPEND_COMPLETE_MAX 8u
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>
#include <pthread.h>
#define EFS_OK 0
#define EFS_ERR_INVAL -1
#define EFS_ERR_NOMEM -2
#define EFS_ERR_IO -6
#define EFS_ERR_BUSY -13
#define EFS_ERR_STALE -14
#define EFS_ERR_OVERFLOW -22
#define EFS_META_LANES 64
#define EFS_OPID_VAL_MAX 1024
#define EFS_TXN_PREPARE_MAX 4096
#define EFS_TXN_VALUE_MAX 1024
#define EFS_KV_KEY_MAX 64
#define EFS_TXN_MAX_PART 64
#define EFS_TXN_EXCL_VALUE 7
#define EFS_TXN_COMMIT 1
#define EFS_TXN_ABORT 2
#define HOST_NGROUPS 2
struct efs_txid {uint64_t id;};
struct efs_txn_parts {unsigned n;};
struct host_idx_ref {uint8_t group;uint64_t idx,term;};
struct efs_raft {int role;unsigned voters;};
struct efs_raft_disk {unsigned depth;};
#define EFS_RAFT_LEADER 1
#define EFS_ERR_NOT_PRIMARY -10
struct efs_raft_host {pthread_mutex_t mu;void *kv;struct efs_raft_disk *disk;struct efs_raft r[3];};
struct host_resize_ctx {struct efs_raft_host *host;int *hint;};
struct efs_meta_append_image {uint8_t key[64];uint32_t klen,expected_len,new_len;int op;uint8_t expected[1024],value[1024];};
struct efs_meta_append_ops {
 int (*prepare)(void *,const struct efs_txid *,const struct efs_txn_parts *,const struct efs_meta_append_image *);
 int (*decide)(void *,const struct efs_txid *,const struct efs_txn_parts *,int);
 int (*resolve)(void *,const struct efs_txid *,const struct efs_txn_parts *,int);
};
/* syncs: guard PREPAREs (wait); proposals: log entries; images: PREPAREs in
 * those entries; per_group: images per group in the last entry of each. */
static unsigned syncs,proposals,images,checks,waits,decisions,resolutions,per_group[3],seen[256];
static int guard_error,encode_error,proposal_error,wait_error,decide_error,resolve_error;
static int applied_error[128],last_decision,last_resolve;
static uint64_t waited[2];
static int efs_txn_encode_excl_value(uint8_t *pay,uint32_t cap,uint32_t *pn,
 const uint8_t *expected,uint32_t en,int op,const uint8_t *value,uint32_t vn) {
 assert(cap>=4&&expected&&value&&en==1&&vn==1&&op==3);*pn=4;memset(pay,0,4);return encode_error;
}
/* [PREPARE][image id][shard] */
static uint32_t pack_prep_raw(uint8_t *cmd,int kind,const struct efs_txid *t,
 const struct efs_txn_parts *p,const uint8_t *key,uint32_t kl,const uint8_t *pay,uint32_t pn) {
 assert(kind==7&&t->id==19&&p->n==2&&kl==4&&pay&&pn==4);cmd[0]=EFS_MD_CMD_PREPARE;cmd[1]=key[2];cmd[2]=key[1];return 3;
}
static uint8_t efs_raft_shard_group(uint32_t shard) {return shard%2?2:0;}
static void image_seen(const uint8_t *cmd,uint32_t n,uint8_t g){
 assert(n==3&&cmd[0]==EFS_MD_CMD_PREPARE&&efs_raft_shard_group(cmd[2])==g);seen[cmd[1]]++;images++;per_group[g]++;
}
static int host_propose_wait(struct efs_raft_host *h,uint8_t g,const uint8_t *cmd,uint32_t n,int *hint) {
 (void)h;assert(g==0||g==2);assert(cmd&&n==3&&cmd[0]==EFS_MD_CMD_PREPARE);*hint=1;syncs++;return guard_error;
}
static int host_propose(struct efs_raft_host *h,uint8_t g,const uint8_t *cmd,uint32_t n,uint64_t *idx,uint64_t *term,int *hint) {
 (void)h;assert(g==0||g==2);assert(cmd&&n>=3);*hint=1;proposals++;
 if(proposal_error)return proposal_error;
 per_group[g]=0;
 if(cmd[0]==EFS_MD_CMD_TXN_BATCH){
  unsigned count=(unsigned)cmd[2]<<8|cmd[3],at=4;assert(cmd[1]==EFS_MD_CMD_PREPARE&&count>=2&&count<=EFS_MD_TXN_BATCH_MAX);
  for(unsigned i=0;i<count;i++){unsigned len=(unsigned)cmd[at]<<8|cmd[at+1];image_seen(cmd+at+2,len,g);at+=2+len;}
  assert(at==n);
 } else image_seen(cmd,n,g);
 *idx=proposals;*term=42;return 0;
}
static int host_wait_applied(struct efs_raft_host *h,uint8_t g,uint64_t idx,int *hint) {
 (void)h;assert(g==0||g==2);assert(idx&&idx<=proposals);*hint=1;waits++;waited[g/2]=idx;return wait_error;
}
static int host_apply_rc(struct efs_raft_host *h,uint8_t g,uint64_t idx,uint64_t term) {
 (void)h;assert((g==0||g==2)&&idx<=proposals&&term==42);checks++;return applied_error[idx];
}
static int host_resize_decide(void *user,const struct efs_txid *t,const struct efs_txn_parts *p,int d) {
 struct host_resize_ctx *ctx=user;assert(ctx->host&&ctx->hint&&t->id==19&&p->n==2);decisions++;last_decision=d;return decide_error;
}
static int host_resize_resolve(void *user,const struct efs_txid *t,const struct efs_txn_parts *p,int d) {
 struct host_resize_ctx *ctx=user;assert(ctx->host&&ctx->hint&&t->id==19&&p->n==2);resolutions++;last_resolve=d;return resolve_error;
}

static struct efs_raft *group_raft(struct efs_raft_host *h,uint8_t group){return &h->r[group];}
static int efs_raft_role(const struct efs_raft *r){return r->role;}
static unsigned efs_raft_voters(const struct efs_raft *r){return r->voters;}
static int efs_raft_disk_sync_hold(struct efs_raft_disk *d){d->depth++;return 0;}
static int host_report_release(struct efs_raft_host *h,int held){assert(held&&h->disk->depth==1);h->disk->depth--;return 0;}
static int host_propose_mode(struct efs_raft_host *h,uint8_t group,const uint8_t *cmd,uint32_t n,uint64_t *idx,uint64_t *term,int *hint,int local){assert(local);return host_propose(h,group,cmd,n,idx,term,hint);}
'''+cohort+'\n'+builder+'\n'+function('host_prep_raw_pack')+'\n'+function('host_prep_raw')+'\n'+function('host_wait_refs')+'\n'+helpers+r'''

static struct efs_raft_disk disk;
static struct efs_raft_host host={.mu=PTHREAD_MUTEX_INITIALIZER,.disk=&disk,.r={{.role=EFS_RAFT_LEADER,.voters=7},{0},{.role=EFS_RAFT_LEADER,.voters=7}}};
static struct efs_txid tx={19};static struct efs_txn_parts parts={2};static int hint;
static struct efs_meta_append_image image={.klen=4,.expected_len=1,.new_len=1,.op=3};
static void reset(struct host_append_reserve_ctx *c,int leading) {
 memset(c,0,sizeof(*c));c->base=(struct host_resize_ctx){&host,&hint};c->leading=leading;
 syncs=proposals=images=checks=waits=decisions=resolutions=0;last_decision=last_resolve=0;
 guard_error=encode_error=proposal_error=wait_error=decide_error=resolve_error=0;
 memset(applied_error,0,sizeof(applied_error));memset(waited,0,sizeof(waited));
 memset(per_group,0,sizeof(per_group));memset(seen,0,sizeof(seen));
}
/* The guards, then `count` images alternating between the two groups. */
static void ready_n(struct host_append_reserve_ctx *c,int leading,unsigned count) {
 reset(c,leading);
 for(int i=0;i<leading;i++)assert(!host_append_reserve_ops.prepare(c,&tx,&parts,&image));
 assert(syncs==(unsigned)leading&&!proposals&&!waits&&!checks&&!c->leading);
 for(unsigned i=0;i<count;i++){image.key[1]=i%2;image.key[2]=(uint8_t)i;assert(!host_append_reserve_ops.prepare(c,&tx,&parts,&image));}
 assert(!waits&&!checks&&!decisions&&syncs==(unsigned)leading);
}
static void ready(struct host_append_reserve_ctx *c,int leading) {
 ready_n(c,leading,4);assert(!proposals); /* buffered until the decision */
}
int main(void) {
 struct host_append_reserve_ctx c;
 /* K29 count: owner and inode guards, cursor, receipt, reservation and 64
  * lanes (67 images): one entry per group, every image exactly once. */
 ready_n(&c,2,67);
 assert(!host_append_reserve_ops.decide(&c,&tx,&parts,EFS_TXN_COMMIT));
 assert(proposals==2&&images==67&&per_group[0]==34&&per_group[2]==33);
 for(unsigned i=0;i<67;i++)assert(seen[i]==1);
 assert(waits==2&&checks==2&&decisions==1&&last_decision==EFS_TXN_COMMIT);
 /* A caller's open cohort is neither used nor flushed by the decision. */
 reset(&c,0);struct host_command_cohort captured;
 host_command_cohort_begin(&captured,&host);
 for(unsigned i=0;i<4;i++){image.key[1]=i%2;assert(!host_append_reserve_ops.prepare(&c,&tx,&parts,&image));}
 assert(c.images==4&&!proposals&&!checks&&!decisions&&!captured.n);
 assert(!host_append_reserve_ops.decide(&c,&tx,&parts,EFS_TXN_COMMIT)&&proposals==2&&!captured.n);
 assert(checks==2&&waits==2&&last_decision==EFS_TXN_COMMIT);
 assert(!host_command_cohort_end(&captured,0,&hint)&&proposals==2&&!disk.depth);
 assert(host_append_ops.prepare==host_append_prepare && host_append_ops.decide==host_resize_decide);
 for(int leading=1;leading<=2;leading++) {
  ready(&c,leading);assert(!host_append_reserve_ops.decide(&c,&tx,&parts,EFS_TXN_COMMIT));
  assert(proposals==2&&images==4&&waits==2&&waited[0]==1&&waited[1]==2&&checks==2&&decisions==1&&last_decision==EFS_TXN_COMMIT&&!resolutions&&!c.nrefs);
  assert(!host_append_reserve_ops.resolve(&c,&tx,&parts,EFS_TXN_COMMIT)&&last_resolve==EFS_TXN_COMMIT);
 }
 /* A refused PREPARE in either entry: durable ABORT, never COMMIT. */
 for(unsigned index=1;index<=2;index++)for(int error=EFS_ERR_BUSY;error>=EFS_ERR_STALE;error--) {
  ready(&c,2);applied_error[index]=error;
  assert(host_append_reserve_ops.decide(&c,&tx,&parts,EFS_TXN_COMMIT)==error);
  assert(checks==2&&decisions==1&&last_decision==EFS_TXN_ABORT&&resolutions==1&&last_resolve==EFS_TXN_ABORT&&!c.nrefs);
 }
 ready(&c,2);applied_error[2]=EFS_ERR_IO;assert(host_append_reserve_ops.decide(&c,&tx,&parts,EFS_TXN_COMMIT)==EFS_ERR_IO&&last_decision==EFS_TXN_ABORT);
 ready(&c,2);wait_error=EFS_ERR_BUSY;assert(host_append_reserve_ops.decide(&c,&tx,&parts,EFS_TXN_COMMIT)==EFS_ERR_BUSY&&last_decision==EFS_TXN_ABORT&&!checks);
 ready(&c,2);decide_error=EFS_ERR_BUSY;assert(host_append_reserve_ops.decide(&c,&tx,&parts,EFS_TXN_COMMIT)==EFS_ERR_BUSY&&decisions==1&&last_decision==EFS_TXN_COMMIT&&!resolutions);
 ready(&c,2);applied_error[1]=EFS_ERR_STALE;decide_error=EFS_ERR_IO;
 assert(host_append_reserve_ops.decide(&c,&tx,&parts,EFS_TXN_COMMIT)==EFS_ERR_IO&&last_decision==EFS_TXN_ABORT&&!resolutions);
 ready(&c,2);applied_error[1]=EFS_ERR_STALE;resolve_error=EFS_ERR_IO;
 assert(host_append_reserve_ops.decide(&c,&tx,&parts,EFS_TXN_COMMIT)==EFS_ERR_IO&&last_resolve==EFS_TXN_ABORT);
 /* A failed entry proposal: no COMMIT; the accepted entry is still checked. */
 ready(&c,2);proposal_error=EFS_ERR_IO;
 assert(host_append_reserve_ops.decide(&c,&tx,&parts,EFS_TXN_COMMIT)==EFS_ERR_IO&&proposals==1&&!checks&&last_decision==EFS_TXN_ABORT&&last_resolve==EFS_TXN_ABORT);
 /* ABORT proposes no buffered image. */
 ready(&c,2);assert(!host_append_reserve_ops.decide(&c,&tx,&parts,EFS_TXN_ABORT)&&!proposals&&!waits&&last_decision==EFS_TXN_ABORT);
 reset(&c,2);guard_error=EFS_ERR_STALE;assert(host_append_reserve_ops.prepare(&c,&tx,&parts,&image)==EFS_ERR_STALE&&c.leading==2&&!proposals&&!c.images);
 assert(!host_append_reserve_ops.decide(&c,&tx,&parts,EFS_TXN_ABORT)&&!waits&&last_decision==EFS_TXN_ABORT);
 reset(&c,2);assert(host_append_reserve_ops.decide(&c,&tx,&parts,EFS_TXN_COMMIT)==EFS_ERR_INVAL&&last_decision==EFS_TXN_ABORT&&last_resolve==EFS_TXN_ABORT);
 reset(&c,0);encode_error=EFS_ERR_INVAL;assert(host_append_reserve_ops.prepare(&c,&tx,&parts,&image)==EFS_ERR_INVAL&&!proposals&&!c.images);
 reset(&c,0);c.images=sizeof(c.refs)/sizeof(c.refs[0]);assert(host_append_reserve_ops.prepare(&c,&tx,&parts,&image)==EFS_ERR_INVAL&&!proposals);

 puts("append PREPARE: ordered floor/inode guards, one entry per group (64 lanes: 2 entries, not 67), every applied verdict before decision, refusal/abort cleanup and ambiguous COMMIT retention PASS");
 return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='efs-append-prepares-') as td:
 p=Path(td)/'test.c';p.write_text(code);binary=p.with_suffix('')
 subprocess.run(shlex.split(os.environ.get('CC','cc'))+['-std=c99','-Wall','-Wextra','-Werror','-pthread']+(['-Wno-unused-function'] if os.environ.get('EFS_APPEND_BATCH_PER_IMAGE')=='1' else [])+['-I'+str(root/'include'),str(p),'-o',str(binary)],check=True)
 subprocess.run([str(binary)],check=True,timeout=30)
a=s.index('static int host_append_transaction(');b=s.index('\n}',a);body=s[a:b]
assert 'host_append_atomic_eligible(h,plan)' in body
assert 'host_append_execute_batched(h,plan,q,&t,hint)' in body
a=s.index('static int host_append_execute_batched(');b=s.index('\n}',a);adapter=s[a:b]
# K5: every append is session-owned; only the owned execution remains.
assert adapter.count('&host_append_reserve_ops,&batch')==1
assert 'efs_meta_execute_append_owned(' in adapter and '.leading=2' in adapter
assert 'host_append_prune_batched(h,ino,&t,hint,&removed)' in body
