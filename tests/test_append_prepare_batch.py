#!/usr/bin/env python3
"""Actual append transport batches after ordered guards, before a checked decision."""
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
if os.environ.get('EFS_APPEND_BATCH_SYNC')=='1':
 a=helpers.index('static int host_append_reserve_prepare(');b=helpers.index('\n}',a)+2
 helpers=helpers[:a]+'''static int host_append_reserve_prepare(void *user,const struct efs_txid *t,
 const struct efs_txn_parts *parts,const struct efs_meta_append_image *im) {
 struct host_append_reserve_ctx *ctx=user;
 int rc=host_append_prepare(&ctx->base,t,parts,im);
 if(rc==EFS_OK && ctx->leading)--ctx->leading;
 return rc;
}'''+helpers[b:]
code=r'''
#include <stdint.h>
#define EFS_META_APPEND_COMPLETE_MAX 8u
#include <stddef.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>
#include <pthread.h>
#define EFS_OK 0
#define EFS_ERR_INVAL -1
#define EFS_ERR_IO -6
#define EFS_ERR_BUSY -13
#define EFS_ERR_STALE -14
#define EFS_META_LANES 64
#define EFS_OPID_VAL_MAX 1024
#define EFS_TXN_PREPARE_MAX 4096
#define EFS_TXN_VALUE_MAX 1024
#define EFS_KV_KEY_MAX 64
#define EFS_TXN_MAX_PART 64
#define EFS_TXN_EXCL_VALUE 7
#define EFS_TXN_COMMIT 1
#define EFS_TXN_ABORT 2
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
static unsigned syncs,proposals,checks,waits,decisions,resolutions;
static int guard_error,encode_error,proposal_error,wait_error,decide_error,resolve_error;
static int applied_error[128],last_decision,last_resolve;
static uint64_t waited[2];
static int efs_txn_encode_excl_value(uint8_t *pay,uint32_t cap,uint32_t *pn,
 const uint8_t *expected,uint32_t en,int op,const uint8_t *value,uint32_t vn) {
 assert(cap>=4&&expected&&value&&en==1&&vn==1&&op==3);*pn=4;memset(pay,0,4);return encode_error;
}
static uint32_t pack_prep_raw(uint8_t *cmd,int kind,const struct efs_txid *t,
 const struct efs_txn_parts *p,const uint8_t *key,uint32_t kl,const uint8_t *pay,uint32_t pn) {
 assert(kind==7&&t->id==19&&p->n==2&&kl==4&&pay&&pn==4);cmd[0]=key[2];return 1;
}
static uint8_t efs_raft_shard_group(uint32_t shard) {return shard%2?2:0;}
static int host_propose_wait(struct efs_raft_host *h,uint8_t g,const uint8_t *cmd,uint32_t n,int *hint) {
 (void)h;assert(g==0||g==2);assert(cmd&&n==1);*hint=1;syncs++;return guard_error;
}
static int host_propose(struct efs_raft_host *h,uint8_t g,const uint8_t *cmd,uint32_t n,uint64_t *idx,uint64_t *term,int *hint) {
 (void)h;assert(g==0||g==2);assert(cmd&&n==1);*hint=1;proposals++;
 if(proposal_error)return proposal_error;
 *idx=proposals;*term=42;return 0;
}
static int host_wait_applied(struct efs_raft_host *h,uint8_t g,uint64_t idx,int *hint) {
 (void)h;assert(g==0||g==2);assert(idx&&idx<=proposals);*hint=1;waits++;waited[g/2]=idx;return wait_error;
}
static int host_apply_rc_locked(struct efs_raft_host *h,uint8_t g,uint64_t idx,uint64_t term) {
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
static unsigned efs_raft_disk_sync_depth(struct efs_raft_disk *d){return d->depth;}
static int efs_raft_disk_sync_hold(struct efs_raft_disk *d){d->depth++;return 0;}
static int host_report_release(struct efs_raft_host *h,int held){assert(held&&h->disk->depth==1);h->disk->depth--;return 0;}
static int host_propose_mode(struct efs_raft_host *h,uint8_t group,const uint8_t *cmd,uint32_t n,uint64_t *idx,uint64_t *term,int *hint,int local){assert(local);return host_propose(h,group,cmd,n,idx,term,hint);}
'''+cohort+'\n'+function('host_prep_raw')+'\n'+function('host_wait_refs')+'\n'+helpers+r'''

typedef uint64_t efs_ino_t;
struct efs_opid {int owner;};
#define EFS_ERR_NOT_FOUND -2
#define EFS_META_APPEND_COMPLETED 0
static unsigned report_done,report_authority,report_reads;
static int report_error;
static unsigned report_batch_max=8,report_refusals;
static int efs_meta_apply_append_open(void *kv,efs_ino_t ino,uint64_t *offs,uint64_t *lens,uint32_t *n) {
 (void)kv;assert(ino==11&&*n==64);report_reads++;
 *n=report_done==2?0:2-report_done;
 for(unsigned i=0;i<*n;i++){offs[i]=(report_done+i)*8;lens[i]=8;}
 return 0;
}
static int host_append_authority(struct efs_raft_host *h,int *hint) {
 (void)h;*hint=1;report_authority++;return 0;
}
static void fill_txid(struct efs_raft_host *h,struct efs_txid *t) {(void)h;t->id=19;}
static int report_finish(void *kv,efs_ino_t ino,uint64_t offset,int outcome,
 const struct efs_opid *owner,const struct efs_txid *t,const struct efs_meta_append_ops *ops,void *ctx) {
 (void)kv;assert(ino==11&&offset==report_done*8&&outcome==0);
 struct host_append_reserve_ctx *c=ctx;
 assert(ops==&host_append_reserve_ops&&!c->nrefs&&c->leading==(owner?2:1));
 struct efs_txn_parts p={2};
 struct efs_meta_append_image im={.klen=4,.expected_len=1,.new_len=1,.op=3};
 im.key[1]=1;im.key[2]=1;
 for(unsigned i=0;i<(owner?6:5);i++) {
  int rc=ops->prepare(ctx,t,&p,&im);
  if(rc){assert(!ops->decide(ctx,t,&p,EFS_TXN_ABORT));assert(!ops->resolve(ctx,t,&p,EFS_TXN_ABORT));return rc;}
 }
 if(report_error)applied_error[c->refs[0].idx]=report_error;
 int rc=ops->decide(ctx,t,&p,EFS_TXN_COMMIT);
 if(!rc)rc=ops->resolve(ctx,t,&p,EFS_TXN_COMMIT);
 if(!rc)report_done++;
 return rc;
}
static int efs_meta_finish_append_batch(void *kv,efs_ino_t ino,const uint64_t *offsets,uint32_t count,int outcome,
 const struct efs_txid *t,const struct efs_meta_append_ops *ops,void *ctx) {
 if(count>report_batch_max){report_refusals++;return EFS_ERR_INVAL;}
 int rc=report_finish(kv,ino,offsets[0],outcome,NULL,t,ops,ctx);
 if(!rc)report_done+=count-1;
 return rc;
}
static int efs_meta_finish_append_batch_owned(void *kv,efs_ino_t ino,const uint64_t *offsets,uint32_t count,int outcome,
 const struct efs_opid *id,const struct efs_txid *t,const struct efs_meta_append_ops *ops,void *ctx) {
 if(count>report_batch_max){report_refusals++;return EFS_ERR_INVAL;}
 int rc=report_finish(kv,ino,offsets[0],outcome,id,t,ops,ctx);
 if(!rc)report_done+=count-1;
 return rc;
}
''' + function('host_resolve_caught_up') + r'''
static struct efs_raft_disk disk;
static struct efs_raft_host host={.mu=PTHREAD_MUTEX_INITIALIZER,.disk=&disk,.r={{.role=EFS_RAFT_LEADER,.voters=7},{0},{.role=EFS_RAFT_LEADER,.voters=7}}};
static struct efs_txid tx={19};static struct efs_txn_parts parts={2};static int hint;
static struct efs_meta_append_image image={.klen=4,.expected_len=1,.new_len=1,.op=3};
static void reset(struct host_append_reserve_ctx *c,int leading) {
 memset(c,0,sizeof(*c));c->base=(struct host_resize_ctx){&host,&hint};c->leading=leading;
 syncs=proposals=checks=waits=decisions=resolutions=0;last_decision=last_resolve=0;
 guard_error=encode_error=proposal_error=wait_error=decide_error=resolve_error=0;
 memset(applied_error,0,sizeof(applied_error));memset(waited,0,sizeof(waited));
}
static void ready(struct host_append_reserve_ctx *c,int leading) {
 reset(c,leading);
 for(int i=0;i<leading;i++)assert(!host_append_reserve_ops.prepare(c,&tx,&parts,&image));
 assert(syncs==(unsigned)leading&&!proposals&&!waits&&!checks&&!c->leading);
 for(unsigned i=0;i<4;i++){image.key[1]=i%2;assert(!host_append_reserve_ops.prepare(c,&tx,&parts,&image));}
 assert(c->nrefs==4&&proposals==4&&syncs==(unsigned)leading&&!waits&&!checks&&!decisions);
}
int main(void) {
 struct host_append_reserve_ctx c;
 reset(&c,0);struct host_command_cohort captured;
 host_command_cohort_begin(&captured,&host);
 for(unsigned i=0;i<4;i++){image.key[1]=i%2;assert(!host_append_reserve_ops.prepare(&c,&tx,&parts,&image));}
 assert(c.nrefs==4&&!proposals&&!checks&&!decisions);
 assert(!host_command_cohort_end(&captured,0,&hint)&&proposals==4&&!disk.depth);
 assert(!host_append_reserve_ops.decide(&c,&tx,&parts,EFS_TXN_COMMIT)&&checks==4&&waits==2&&last_decision==EFS_TXN_COMMIT);
 assert(host_append_ops.prepare==host_append_prepare && host_append_ops.decide==host_resize_decide);
 for(int leading=1;leading<=2;leading++) {
  ready(&c,leading);assert(!host_append_reserve_ops.decide(&c,&tx,&parts,EFS_TXN_COMMIT));
  assert(waits==2&&waited[0]==3&&waited[1]==4&&checks==4&&decisions==1&&last_decision==EFS_TXN_COMMIT&&!resolutions&&!c.nrefs);
  assert(!host_append_reserve_ops.resolve(&c,&tx,&parts,EFS_TXN_COMMIT)&&last_resolve==EFS_TXN_COMMIT);
 }
 for(unsigned index=1;index<=4;index++)for(int error=EFS_ERR_BUSY;error>=EFS_ERR_STALE;error--) {
  ready(&c,2);applied_error[index]=error;
  assert(host_append_reserve_ops.decide(&c,&tx,&parts,EFS_TXN_COMMIT)==error);
  assert(checks==4&&decisions==1&&last_decision==EFS_TXN_ABORT&&resolutions==1&&last_resolve==EFS_TXN_ABORT&&!c.nrefs);
 }
 ready(&c,2);applied_error[2]=EFS_ERR_IO;assert(host_append_reserve_ops.decide(&c,&tx,&parts,EFS_TXN_COMMIT)==EFS_ERR_IO&&last_decision==EFS_TXN_ABORT);
 ready(&c,2);wait_error=EFS_ERR_BUSY;assert(host_append_reserve_ops.decide(&c,&tx,&parts,EFS_TXN_COMMIT)==EFS_ERR_BUSY&&last_decision==EFS_TXN_ABORT&&!checks);
 ready(&c,2);decide_error=EFS_ERR_BUSY;assert(host_append_reserve_ops.decide(&c,&tx,&parts,EFS_TXN_COMMIT)==EFS_ERR_BUSY&&decisions==1&&last_decision==EFS_TXN_COMMIT&&!resolutions);
 ready(&c,2);applied_error[1]=EFS_ERR_STALE;decide_error=EFS_ERR_IO;
 assert(host_append_reserve_ops.decide(&c,&tx,&parts,EFS_TXN_COMMIT)==EFS_ERR_IO&&last_decision==EFS_TXN_ABORT&&!resolutions);
 ready(&c,2);applied_error[1]=EFS_ERR_STALE;resolve_error=EFS_ERR_IO;
 assert(host_append_reserve_ops.decide(&c,&tx,&parts,EFS_TXN_COMMIT)==EFS_ERR_IO&&last_resolve==EFS_TXN_ABORT);
 ready(&c,2);proposal_error=EFS_ERR_IO;assert(host_append_reserve_ops.prepare(&c,&tx,&parts,&image)==EFS_ERR_IO&&c.nrefs==4);
 assert(!host_append_reserve_ops.decide(&c,&tx,&parts,EFS_TXN_ABORT)&&checks==4&&last_decision==EFS_TXN_ABORT);
 reset(&c,2);guard_error=EFS_ERR_STALE;assert(host_append_reserve_ops.prepare(&c,&tx,&parts,&image)==EFS_ERR_STALE&&c.leading==2&&!proposals&&!c.nrefs);
 assert(!host_append_reserve_ops.decide(&c,&tx,&parts,EFS_TXN_ABORT)&&!waits&&last_decision==EFS_TXN_ABORT);
 reset(&c,2);assert(host_append_reserve_ops.decide(&c,&tx,&parts,EFS_TXN_COMMIT)==EFS_ERR_INVAL&&last_decision==EFS_TXN_ABORT&&last_resolve==EFS_TXN_ABORT);
 reset(&c,0);encode_error=EFS_ERR_INVAL;assert(host_append_reserve_ops.prepare(&c,&tx,&parts,&image)==EFS_ERR_INVAL&&!proposals&&!c.nrefs);
 reset(&c,0);c.nrefs=sizeof(c.refs)/sizeof(c.refs[0]);assert(host_append_reserve_ops.prepare(&c,&tx,&parts,&image)==EFS_ERR_INVAL&&!proposals);

 for(int owned=0;owned<2;owned++) {
  struct efs_opid id={1};int hint=0;
  reset(&c,0);report_done=report_authority=report_reads=report_error=0;
  assert(!host_resolve_caught_up(&host,11,16,owned?&id:NULL,&hint));
  assert(report_done==2&&report_authority==1&&report_reads==3);
  assert(syncs==(unsigned)(owned?2:1)&&proposals==4&&checks==4&&decisions==1&&resolutions==1);
  reset(&c,0);report_done=report_authority=report_reads=report_refusals=report_error=0;report_batch_max=1;
  assert(!host_resolve_caught_up(&host,11,16,owned?&id:NULL,&hint));
  assert(report_done==2&&report_refusals==1&&decisions==2&&resolutions==2);
  report_batch_max=8;
  reset(&c,0);report_done=report_authority=report_reads=report_error=0;
  assert(!host_resolve_caught_up(&host,11,8,owned?&id:NULL,&hint));
  assert(report_done==1&&decisions==1&&resolutions==1);
  reset(&c,0);report_done=report_authority=report_reads=0;report_error=EFS_ERR_STALE;
  assert(host_resolve_caught_up(&host,11,16,owned?&id:NULL,&hint)==EFS_ERR_BUSY);
  assert(!report_done&&report_authority==1&&last_decision==EFS_TXN_ABORT&&last_resolve==EFS_TXN_ABORT);
 }
 puts("append PREPARE: ordered floor/inode guards, bounded cohorts, every applied verdict before decision, refusal/abort cleanup and ambiguous COMMIT retention PASS");
 return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='efs-append-prepares-') as td:
 p=Path(td)/'test.c';p.write_text(code);binary=p.with_suffix('')
 subprocess.run(shlex.split(os.environ.get('CC','cc'))+['-std=c99','-Wall','-Wextra','-Werror','-pthread',str(p),'-o',str(binary)],check=True)
 subprocess.run([str(binary)],check=True,timeout=30)
a=s.index('static int host_append_transaction(');b=s.index('\n}',a);body=s[a:b]
assert body.count('&host_append_reserve_ops,&batch')==2
assert '.leading=registered?2:1' in body
assert '&host_append_ops,&ctx,&removed' in body
