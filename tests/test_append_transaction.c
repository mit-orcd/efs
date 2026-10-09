/* APPEND chooses one offset, then freezes all EOF inputs and durable output
 * through exact-image transactions. No hosted-view recomputation at apply. */
#include "efs/meta_apply.h"
#include "efs/kv_lsm.h"
#include "efs/session.h"
#include <assert.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
static int decision(void *ctx,const struct efs_txid *t,uint32_t sh,int *d)
{ return efs_txn_decision_get(ctx,sh,t,d); }
static int prepare(void *ctx,const struct efs_txid *t,const struct efs_txn_parts *p,
                   const struct efs_meta_append_image *q)
{ return efs_txn_prepare_excl_value(ctx,t,p,q->key,q->klen,q->expected,q->expected_len,
                                    q->op,q->value,q->new_len); }
static int decide(void *ctx,const struct efs_txid *t,const struct efs_txn_parts *p,int d)
{ return efs_txn_decide(ctx,efs_txn_coordinator(t,p),t,d); }
static int resolve(void *ctx,const struct efs_txid *t,const struct efs_txn_parts *p,int d)
{
 for(unsigned i=0;i<p->n;++i){int rc=efs_txn_resolve(ctx,t,p->shard[i],d);if(rc){fprintf(stderr,"resolve shard=%u rc=%d\n",p->shard[i],rc);return rc;}}
 return EFS_OK;
}
static const struct efs_meta_append_ops ops={prepare,decide,resolve};
static struct efs_opid_req identity(unsigned seq,unsigned ack)
{struct efs_opid_req q={0};q.id.client_uuid[0]=42;q.id.session_epoch=1;q.id.seq=seq;q.ack=ack;return q;}
static struct efs_meta_pub publication(efs_ino_t ino,uint64_t gen,unsigned ci,uint64_t end,unsigned candidate)
{
 struct efs_meta_pub p={0};p.ino=ino;p.inode_gen=gen;p.chunk_index=ci;
 p.new_size=end;p.now=100;p.candidate_gen=candidate;
 p.coding_profile_id=EFS_META_PROFILE_K2F1;
 for(unsigned i=0;i<EFS_NUM_FRAGMENTS;++i)p.ch.nodes[i]=i+1;
 return p;
}
static void fixture(struct efs_kv *kv,efs_ino_t *ino,uint64_t *gen)
{
 struct efs_meta_attrs a={1,1,1};struct efs_meta_row row;
 assert(efs_meta_apply_init(kv,1)==EFS_OK);
 assert(efs_meta_apply_create_file(kv,&a,EFS_ROOT_INO,S_IFREG|0644,"file",ino)==EFS_OK);
 assert(efs_meta_apply_get_inode(kv,*ino,&row)==EFS_OK);*gen=row.generation;
 assert(efs_meta_apply_activate_lanes(kv,*ino,3)==EFS_OK);
 struct efs_meta_pub p=publication(*ino,*gen,0,100,10);
 assert(efs_meta_apply_publish(kv,&p)==EFS_OK);
 p=publication(*ino,*gen,1,200,20);assert(efs_meta_apply_publish(kv,&p)==EFS_OK);
}
static struct efs_txn_parts last_parts;
static int lost_decision(void *ctx,const struct efs_txid *t,const struct efs_txn_parts *p,int d)
{last_parts=*p;assert(decide(ctx,t,p,d)==EFS_OK);return EFS_ERR_IO;}
static void unit(void)
{
 struct efs_kv *kv=efs_kv_mem_create();assert(kv);efs_ino_t ino;uint64_t gen,off=999;
 fixture(kv,&ino,&gen);struct efs_opid_req q=identity(1,0);
 struct efs_meta_append_plan *a=malloc(sizeof(*a)),*b=malloc(sizeof(*b));assert(a&&b);
 assert(efs_meta_capture_append(kv,ino,8,&q,decision,kv,a)==EFS_OK && a->offset==200 && a->count==6);
 assert(efs_meta_append_result(kv,ino,8,&q,&off)==EFS_ERR_NOT_FOUND); /* capture never mutates */
 /* Full-image CAS rejects a non-append extension racing the snapshot. */
 struct efs_meta_pub p=publication(ino,gen,65,300,30);
 assert(efs_meta_apply_publish(kv,&p)==EFS_OK);
 struct efs_txid t={{1}};
 assert(efs_meta_execute_append(a,&t,&ops,kv)==EFS_ERR_STALE);
 assert(efs_meta_append_result(kv,ino,8,&q,&off)==EFS_ERR_NOT_FOUND);
 uint64_t wm,frontier;uint32_t open;
 assert(efs_meta_apply_append_state(kv,ino,&wm,&frontier,&open)==EFS_OK && !open);
 assert(efs_meta_capture_append(kv,ino,8,&q,decision,kv,a)==EFS_OK && a->offset==300);
 /* Same operation and competing stream choose the same snapshot. Only one
  * can own the inode; a loser must recapture, never reserve the same extent. */
 struct efs_opid_req competing=q;competing.id.client_uuid[0]=43;
 assert(efs_meta_capture_append(kv,ino,8,&competing,decision,kv,b)==EFS_OK);
 t.bytes[0]=2;assert(prepare(kv,&t,&a->parts,&a->images[0])==EFS_OK);
 struct efs_txid other={{3}};
 assert(efs_meta_execute_append(b,&other,&ops,kv)==EFS_ERR_BUSY);
 /* Inode freeze blocks new-lane activation and legacy cursor mutation. */
 assert(efs_meta_apply_activate_lane(kv,ino,2)==EFS_ERR_BUSY);
 assert(efs_meta_apply_append_reserve_durable(kv,ino,8,&q,decision,kv,&off)==EFS_ERR_BUSY);
 for(unsigned i=1;i<a->count;++i)assert(prepare(kv,&t,&a->parts,&a->images[i])==EFS_OK);
 p=publication(ino,gen,129,301,40);assert(efs_meta_apply_publish(kv,&p)==EFS_ERR_BUSY);
 assert(decide(kv,&t,&a->parts,EFS_TXN_COMMIT)==EFS_OK);
 assert(efs_meta_append_result(kv,ino,8,&q,&off)==EFS_ERR_NOT_FOUND);
 assert(resolve(kv,&t,&a->parts,EFS_TXN_COMMIT)==EFS_OK);
 assert(efs_meta_append_result(kv,ino,8,&q,&off)==EFS_OK && off==300);
 assert(efs_meta_capture_append(kv,ino,8,&q,decision,kv,b)==EFS_OK && !b->count && b->offset==300);
 assert(efs_meta_apply_append_state(kv,ino,&wm,&frontier,&open)==EFS_OK && wm==308 && open==1);
 /* A lost COMMIT reply preserves its decision; recovery installs one next
  * reservation and the same durable result, never an abort/rebase. */
 q=identity(2,1);assert(efs_meta_capture_append(kv,ino,9,&q,decision,kv,a)==EFS_OK && a->offset==308);
 struct efs_meta_append_ops lost={prepare,lost_decision,resolve};t.bytes[0]=4;
 assert(efs_meta_execute_append(a,&t,&lost,kv)==EFS_ERR_IO);
 int d=0;assert(decision(kv,&t,efs_txn_coordinator(&t,&a->parts),&d)==EFS_OK && d==EFS_TXN_COMMIT);
 assert(resolve(kv,&t,&a->parts,EFS_TXN_COMMIT)==EFS_OK);
 assert(efs_meta_capture_append(kv,ino,9,&q,decision,kv,b)==EFS_OK && !b->count && b->offset==308);
 q=identity(3,2);assert(efs_meta_capture_append(kv,ino,10,&q,decision,kv,a)==EFS_OK);
 unsigned count=a->count;a->count=4;t.bytes[0]=5;
 assert(efs_meta_execute_append(a,&t,&ops,kv)==EFS_ERR_INVAL);a->count=count;
 a->parts.n--;assert(efs_meta_execute_append(a,&t,&ops,kv)==EFS_ERR_INVAL);a->parts.n++;
 a->offset++;assert(efs_meta_execute_append(a,&t,&ops,kv)==EFS_ERR_INVAL);a->offset--;
 a->images[4].value[48]^=1;assert(efs_meta_execute_append(a,&t,&ops,kv)==EFS_ERR_INVAL);a->images[4].value[48]^=1;
 assert(efs_meta_execute_append(a,&t,&ops,kv)==EFS_OK);
 q=identity(1,0);assert(efs_meta_capture_append(kv,ino,8,&q,decision,kv,b)==EFS_ERR_STALE);
 assert(efs_meta_apply_check(kv)==EFS_OK);
 free(a);free(b);efs_kv_mem_free(kv);
}
static void bounds_and_pending(void)
{
 struct efs_kv *kv=efs_kv_mem_create();assert(kv);efs_ino_t ino;uint64_t gen,off=999;
 fixture(kv,&ino,&gen);
 struct efs_meta_append_plan *a=malloc(sizeof(*a)),*b=malloc(sizeof(*b));assert(a&&b);
 struct efs_opid_req q=identity(1,0),foreign=q;foreign.id.client_uuid[0]=43;
 assert(efs_meta_capture_append(kv,ino,8,&foreign,decision,kv,a)==EFS_OK);
 struct efs_txid t={{60}};
 for(unsigned i=0;i<a->count;++i)assert(prepare(kv,&t,&a->parts,&a->images[i])==EFS_OK);
 assert(decide(kv,&t,&a->parts,EFS_TXN_COMMIT)==EFS_OK);
 /* A committed overlay cursor must not be combined with an old raw scan
  * and then accepted after its foreign reservation materializes. */
 assert(efs_meta_capture_append(kv,ino,8,&q,decision,kv,b)==EFS_OK);
 assert(resolve(kv,&t,&a->parts,EFS_TXN_COMMIT)==EFS_OK);
 struct efs_txid later={{61}};
 assert(efs_meta_execute_append(b,&later,&ops,kv)==EFS_ERR_STALE);
 assert(efs_meta_capture_append(kv,ino,8,&q,decision,kv,b)==EFS_ERR_BUSY);
 assert(efs_meta_apply_append_resolve(kv,ino,200,EFS_META_APPEND_COMPLETED)==EFS_OK);
 /* All 64 active lanes participate, including absent stamps. The inode
  * guard and absent lane CAS close first-publication races. */
 assert(efs_meta_apply_activate_lanes(kv,ino,UINT64_MAX)==EFS_OK);
 assert(efs_meta_capture_append(kv,ino,8,&q,decision,kv,a)==EFS_OK && a->count==EFS_META_LANES+4);
 t.bytes[0]=62;assert(efs_meta_execute_append(a,&t,&ops,kv)==EFS_OK);
 assert(efs_meta_apply_append_resolve(kv,ino,a->offset,EFS_META_APPEND_COMPLETED)==EFS_OK);
 for(unsigned seq=2;seq<=EFS_OPID_REPLY_CACHE;++seq){
  q=identity(seq,0);assert(efs_meta_capture_append(kv,ino,8,&q,decision,kv,a)==EFS_OK);
  t.bytes[0]=62+seq;assert(efs_meta_execute_append(a,&t,&ops,kv)==EFS_OK);
  assert(efs_meta_apply_append_resolve(kv,ino,a->offset,EFS_META_APPEND_COMPLETED)==EFS_OK);
 }
 q=identity(17,0);assert(efs_meta_capture_append(kv,ino,8,&q,decision,kv,a)==EFS_ERR_BUSY);
 assert(efs_meta_append_result(kv,ino,8,&q,&off)==EFS_ERR_NOT_FOUND);
 q=identity(17,16);assert(efs_meta_capture_append(kv,ino,8,&q,decision,kv,a)==EFS_OK);
 q=identity(18,18);assert(efs_meta_capture_append(kv,ino,8,&q,decision,kv,b)==EFS_ERR_INVAL);
 q=identity(18,17);assert(efs_meta_capture_append(kv,ino,UINT64_MAX,&q,decision,kv,b)==EFS_ERR_INVAL);
 assert(efs_meta_apply_check(kv)==EFS_OK);free(a);free(b);efs_kv_mem_free(kv);
}
struct visit_source {struct efs_kv *base;unsigned rows;};
struct visit_callback {struct visit_source *source;int (*cb)(void *,const uint8_t *,uint32_t,const uint8_t *,uint32_t);void *user;};
static int visit_row(void *ctx,const uint8_t *key,uint32_t kl,const uint8_t *value,uint32_t vl)
{struct visit_callback *v=ctx;++v->source->rows;return v->cb(v->user,key,kl,value,vl);}
static int visit_get(void *ctx,const uint8_t *key,uint32_t kl,uint8_t *value,uint32_t *vl)
{struct visit_source *s=ctx;return efs_kv_get(s->base,key,kl,value,vl);}
static int visit_from(void *ctx,const uint8_t *prefix,uint32_t pl,const uint8_t *start,uint32_t sl,
                         int (*cb)(void *,const uint8_t *,uint32_t,const uint8_t *,uint32_t),void *user)
{struct visit_source *s=ctx;struct visit_callback v={s,cb,user};return efs_kv_scan_from(s->base,prefix,pl,start,sl,visit_row,&v);}
static void completion(void)
{
 struct efs_kv *kv=efs_kv_mem_create();assert(kv);efs_ino_t ino;uint64_t gen;
 fixture(kv,&ino,&gen);struct efs_meta_append_plan *a=malloc(sizeof(*a));assert(a);
 struct efs_txid t={{100}};struct efs_opid_req q;
 for(unsigned i=1;i<=200;++i){
  q=identity(i,i-1);assert(efs_meta_capture_append(kv,ino,8,&q,decision,kv,a)==EFS_OK);
  t.bytes[0]=i;t.bytes[1]=1;assert(efs_meta_execute_append(a,&t,&ops,kv)==EFS_OK);
 }
 struct visit_source source={kv,0};
 const struct efs_kv_ops view_ops={.get=visit_get,.scan_from=visit_from};
 struct efs_kv view={.ops=&view_ops,.ctx=&source};
 uint64_t offsets[8],lengths[8];uint32_t n=8;
 assert(efs_meta_apply_append_open(&view,ino,offsets,lengths,&n)==EFS_OK && n==8 && source.rows==8);
 for(unsigned i=0;i<8;++i)assert(offsets[i]==200+i*8 && lengths[i]==8);
 source.rows=0;n=0;
 assert(efs_meta_apply_append_open(&view,ino,NULL,NULL,&n)==EFS_OK && n==200 && !source.rows);
 /* Reverse completion retains frontier dependencies, then the final
  * transaction retains DONE rows and clears BOTH groups. */
 for(unsigned i=200;i>1;--i){
  t.bytes[0]=i;t.bytes[1]=2;
  assert(efs_meta_finish_append(&view,ino,200+(i-1)*8,EFS_META_APPEND_COMPLETED,&t,&ops,kv)==EFS_OK);
 }
 unsigned removed=999;t.bytes[0]=222;t.bytes[1]=6;
 assert(efs_meta_append_prune_pending(kv,ino)==0);
 assert(efs_meta_prune_append(kv,ino,&t,&ops,kv,&removed)==EFS_OK && removed==0);
 uint64_t wm,frontier;uint32_t open;
 assert(efs_meta_apply_append_state(kv,ino,&wm,&frontier,&open)==EFS_OK && open==1 && frontier==200);
 struct efs_meta_append_ops lost={prepare,lost_decision,resolve};t.bytes[0]=1;
 assert(efs_meta_finish_append(&view,ino,200,EFS_META_APPEND_ABORTED_HOLE,&t,&lost,kv)==EFS_ERR_IO);
 assert(source.rows==399); /* 199 single-row stops + one 200-row frontier pass */
 assert(efs_meta_finish_append(&view,ino,200,EFS_META_APPEND_COMPLETED,&t,&ops,kv)==EFS_ERR_BUSY);
 assert(source.rows==399); /* prepared inode refuses before rescanning */
 assert(resolve(kv,&t,&last_parts,EFS_TXN_COMMIT)==EFS_OK);
 assert(efs_meta_apply_append_state(kv,ino,&wm,&frontier,&open)==EFS_OK && !open && !wm);
 struct efs_meta_stat st;assert(efs_meta_apply_getattr(kv,ino,decision,kv,&st)==EFS_OK && st.size==1800);
 /* No stale foreign-lane append_bar can block an ordinary extension. */
 struct efs_meta_pub p=publication(ino,gen,65,1900,77);p.lane_local=1;
 assert(efs_meta_apply_publish(kv,&p)==EFS_OK);
 q=identity(201,200);assert(efs_meta_capture_append(kv,ino,8,&q,decision,kv,a)==EFS_ERR_BUSY);
 unsigned total=0,pages=0;
 while(efs_meta_append_prune_pending(kv,ino)==1) {
  unsigned removed=0;t.bytes[0]=++pages;t.bytes[1]=5;
  assert(efs_meta_prune_append(kv,ino,&t,&ops,kv,&removed)==EFS_OK && removed>0 && removed<=8);
  total+=removed;
 }
 assert(total==200 && pages==25);
 q=identity(201,200);assert(efs_meta_capture_append(kv,ino,8,&q,decision,kv,a)==EFS_OK && a->offset==1900);
 t.bytes[0]=201;t.bytes[1]=3;assert(efs_meta_execute_append(a,&t,&ops,kv)==EFS_OK);
 t.bytes[1]=4;assert(efs_meta_finish_append(kv,ino,1900,EFS_META_APPEND_COMPLETED,&t,&ops,kv)==EFS_OK);
 /* Lost reply replay of a completed reservation is an exact no-op. */
 assert(efs_meta_finish_append(kv,ino,1900,EFS_META_APPEND_COMPLETED,&t,&ops,kv)==EFS_OK);
 assert(efs_meta_apply_check(kv)==EFS_OK);free(a);efs_kv_mem_free(kv);
}
static void recovery(int committed)
{
 char dir[]="/tmp/efs-append-txn-XXXXXX";assert(mkdtemp(dir));
 struct efs_kv *kv=efs_kv_lsm_open(dir,NULL);assert(kv);efs_ino_t ino;uint64_t gen,off=999;
 fixture(kv,&ino,&gen);efs_kv_lsm_close(kv);
 struct efs_txid t={{90}};struct efs_opid_req q=identity(1,0);
 pid_t child=fork();assert(child>=0);
 if(!child){
  kv=efs_kv_lsm_open(dir,NULL);assert(kv);struct efs_meta_append_plan *plan=malloc(sizeof(*plan));assert(plan);
  assert(efs_meta_capture_append(kv,ino,8,&q,decision,kv,plan)==EFS_OK && plan->offset==200);
  for(unsigned i=0;i<plan->count;++i)assert(prepare(kv,&t,&plan->parts,&plan->images[i])==EFS_OK);
  if(committed)assert(decide(kv,&t,&plan->parts,EFS_TXN_COMMIT)==EFS_OK);
  _exit(0);
 }
 int status;assert(waitpid(child,&status,0)==child && WIFEXITED(status) && !WEXITSTATUS(status));
 kv=efs_kv_lsm_open(dir,NULL);assert(kv);
 struct efs_txn_parts parts={0};parts.shard[parts.n++]=efs_kv_inode_shard(ino);
 for(unsigned lane=0;lane<2;++lane){unsigned sh=efs_kv_lane_shard(ino,lane);int exists=0;for(unsigned i=0;i<parts.n;++i)exists|=parts.shard[i]==sh;if(!exists)parts.shard[parts.n++]=sh;}
 if(!committed)assert(decide(kv,&t,&parts,EFS_TXN_ABORT)==EFS_OK);
 int verdict=committed?EFS_TXN_COMMIT:EFS_TXN_ABORT;
 /* Resolve the foreign lane, then crash/reopen before the inode participant. */
 assert(efs_txn_resolve(kv,&t,efs_kv_lane_shard(ino,1),verdict)==EFS_OK);
 efs_kv_lsm_close(kv);kv=efs_kv_lsm_open(dir,NULL);assert(kv);
 assert(resolve(kv,&t,&parts,verdict)==EFS_OK);
 assert(efs_meta_append_result(kv,ino,8,&q,&off)==(committed?EFS_OK:EFS_ERR_NOT_FOUND));
 if(committed)assert(off==200);
 uint64_t wm,frontier;uint32_t open;
 assert(efs_meta_apply_append_state(kv,ino,&wm,&frontier,&open)==EFS_OK && open==(unsigned)committed);
 if(committed) {
  struct efs_meta_append_ops lost={prepare,lost_decision,resolve};
  t.bytes[0]=91;
  assert(efs_meta_finish_append(kv,ino,200,EFS_META_APPEND_ABORTED_HOLE,&t,&lost,kv)==EFS_ERR_IO);
  efs_kv_lsm_close(kv);kv=efs_kv_lsm_open(dir,NULL);assert(kv);
  assert(efs_txn_resolve(kv,&t,efs_kv_lane_shard(ino,1),EFS_TXN_COMMIT)==EFS_OK);
  efs_kv_lsm_close(kv);kv=efs_kv_lsm_open(dir,NULL);assert(kv);
  assert(resolve(kv,&t,&last_parts,EFS_TXN_COMMIT)==EFS_OK);
  assert(efs_meta_append_prune_pending(kv,ino)==1);
  unsigned removed=99;t.bytes[0]=92;
  assert(efs_meta_prune_append(kv,ino,&t,&lost,kv,&removed)==EFS_ERR_IO && !removed);
  efs_kv_lsm_close(kv);kv=efs_kv_lsm_open(dir,NULL);assert(kv);
  assert(resolve(kv,&t,&last_parts,EFS_TXN_COMMIT)==EFS_OK);
  assert(efs_meta_append_prune_pending(kv,ino)==0);
  assert(efs_meta_append_result(kv,ino,8,&q,&off)==EFS_OK && off==200);
  struct efs_meta_stat st;assert(efs_meta_apply_getattr(kv,ino,decision,kv,&st)==EFS_OK && st.size==208);
 }
 efs_kv_lsm_close(kv);DIR *d=opendir(dir);assert(d);struct dirent *e;
 while((e=readdir(d))){if(e->d_name[0]=='.')continue;char path[512];snprintf(path,sizeof(path),"%s/%s",dir,e->d_name);assert(!unlink(path));}
 closedir(d);assert(!rmdir(dir));
}
struct close_context {struct efs_kv *kv;efs_ino_t ino;uint64_t gen;struct efs_opid_req reopen;unsigned fenced;};
static int close_prepare(void *ctx,const struct efs_txid *t,const struct efs_txn_parts *parts,
                           const struct efs_meta_append_image *image)
{
 struct close_context *c=ctx;int rc=prepare(c->kv,t,parts,image);
 if(!rc && image->key[2]==EFS_KV_KIND_CLOSE_STEP) {
  assert(efs_lease_guarded_edge_deferred(c->kv,c->ino,c->gen,&c->reopen,EFS_LEASE_EDGE_OPEN,0)==EFS_ERR_BUSY);
  uint8_t legacy[16]={119};
  assert(efs_lease_open(c->kv,c->ino,c->gen,legacy,1)==EFS_ERR_BUSY);
  assert(efs_lease_guarded_edge(c->kv,c->ino,c->gen,&c->reopen,EFS_LEASE_EDGE_OPEN,0)==EFS_ERR_BUSY);
  c->fenced++;
 }
 return rc;
}
static int close_decide(void *ctx,const struct efs_txid *t,const struct efs_txn_parts *p,int d)
{return decide(((struct close_context *)ctx)->kv,t,p,d);}
static int close_resolve(void *ctx,const struct efs_txid *t,const struct efs_txn_parts *p,int d)
{return resolve(((struct close_context *)ctx)->kv,t,p,d);}
static void close_completion(void)
{
 struct efs_kv *kv=efs_kv_mem_create();efs_ino_t ino;uint64_t gen;fixture(kv,&ino,&gen);
 struct efs_meta_append_plan *plan=malloc(sizeof(*plan));assert(plan);
 struct efs_opid_req reservation=identity(1,0);struct efs_txid tx={{91}};
 for(unsigned i=0;i<2;i++) {
  reservation.id.seq=i+1;reservation.ack=i;
  assert(efs_meta_capture_append(kv,ino,8,&reservation,decision,kv,plan)==EFS_OK);
  tx.bytes[1]=i+1;assert(efs_meta_execute_append(plan,&tx,&ops,kv)==EFS_OK);
 }
 struct efs_opid_req close=identity(1,0);close.id.client_uuid[0]=99;
 uint32_t shard=efs_kv_inode_shard(ino);
 assert(efs_session_create(kv,close.id.client_uuid,1)==EFS_OK);
 assert(efs_session_register(kv,close.id.client_uuid,1,shard)==EFS_OK);
 assert(efs_session_establish(kv,shard,close.id.client_uuid,1)==EFS_OK);
 assert(efs_lease_guarded_edge_deferred(kv,ino,gen,&close,EFS_LEASE_EDGE_CLOSE,0)==EFS_OK);
 uint8_t key[EFS_KV_KEY_MAX],first[EFS_CLOSE_STEP_VALUE],latest[EFS_CLOSE_STEP_VALUE];uint32_t kl,vl=sizeof(first);
 assert(efs_kv_key_reap(efs_kv_anchor_shard(shard),ino,key,&kl)==EFS_OK);key[2]=EFS_KV_KIND_CLOSE_STEP;
 assert(efs_kv_get(kv,key,kl,first,&vl)==EFS_OK && vl==sizeof(first));
 close.id.seq=2;
 assert(efs_lease_guarded_edge_deferred(kv,ino,gen,&close,EFS_LEASE_EDGE_OPEN,0)==EFS_OK);
 tx.bytes[1]=10;
 assert(efs_meta_finish_append_close(kv,ino,200,first,&tx,&ops,kv)==EFS_ERR_BUSY);
 uint32_t open;assert(efs_meta_apply_append_state(kv,ino,NULL,NULL,&open)==EFS_OK && open==2);
 close.id.seq=3;close.ack=2;
 assert(efs_lease_guarded_edge_deferred(kv,ino,gen,&close,EFS_LEASE_EDGE_CLOSE,0)==EFS_OK);
 tx.bytes[1]=11;
 assert(efs_meta_finish_append_close(kv,ino,200,first,&tx,&ops,kv)==EFS_ERR_STALE);
 vl=sizeof(latest);assert(efs_kv_get(kv,key,kl,latest,&vl)==EFS_OK);
 assert(efs_close_step_apply(kv,ino,latest,EFS_CLOSE_STEP_RETIRE)==EFS_ERR_BUSY);
 struct close_context context={.kv=kv,.ino=ino,.gen=gen,.reopen=close};context.reopen.id.seq=4;
 const struct efs_meta_append_ops closed_ops={close_prepare,close_decide,close_resolve};
 for(unsigned i=0;i<2;i++) {
  tx.bytes[1]=20+i;
  assert(efs_meta_finish_append_close(kv,ino,200+8*i,latest,&tx,&closed_ops,&context)==EFS_OK);
 }
 assert(context.fenced==2);
 assert(efs_meta_apply_append_state(kv,ino,NULL,NULL,&open)==EFS_OK && !open);
 unsigned removed=0;tx.bytes[1]=30;
 assert(efs_meta_prune_append_close(kv,ino,latest,&tx,&closed_ops,&context,&removed)==EFS_OK && removed==2);
 assert(context.fenced==3 && !efs_meta_append_prune_pending(kv,ino));
 assert(efs_close_step_apply(kv,ino,latest,EFS_CLOSE_STEP_LOCKS)==EFS_OK);
 assert(efs_close_step_apply(kv,ino,latest,EFS_CLOSE_STEP_RETIRE)==EFS_OK);
 struct efs_meta_stat st;assert(efs_meta_apply_getattr(kv,ino,decision,kv,&st)==EFS_OK && st.size==216);
 assert(efs_kv_get(kv,key,kl,first,&vl)==EFS_ERR_NOT_FOUND);
 free(plan);efs_kv_mem_free(kv);
}
static void fenced_completion(void)
{
 struct efs_kv *kv=efs_kv_mem_create();efs_ino_t ino;uint64_t gen;fixture(kv,&ino,&gen);
 struct efs_meta_append_plan *plan=malloc(sizeof(*plan));assert(plan);
 struct efs_opid_req q=identity(1,0);struct efs_txid t={{121}};
 for(unsigned i=0;i<3;i++) {
  q.id.seq=i+1;q.ack=i;
  assert(efs_meta_capture_append(kv,ino,8,&q,decision,kv,plan)==EFS_OK);
  t.bytes[1]=i+1;assert(efs_meta_execute_append(plan,&t,&ops,kv)==EFS_OK);
 }
 uint32_t shard=efs_kv_inode_shard(ino);efs_ino_t selected=0;uint64_t generation=0,off=0;unsigned found=99;
 assert(efs_meta_append_session_next(kv,shard,q.id.client_uuid,1,&selected,&generation,&off,&found)==EFS_ERR_BUSY && !found);
 assert(efs_session_fence_local(kv,shard,q.id.client_uuid,2)==EFS_OK);
 assert(efs_meta_append_session_next(kv,shard,q.id.client_uuid,1,&selected,&generation,&off,&found)==EFS_OK && found && selected==ino && generation==gen && off==200);
 t.bytes[1]=10;
 assert(efs_meta_finish_append_fenced(kv,ino,gen+1,off,q.id.client_uuid,1,&t,&ops,kv)==EFS_ERR_STALE);
 uint8_t inode_key[EFS_KV_KEY_MAX],inode_value[EFS_META_INO_BYTES];uint32_t il,iv=sizeof(inode_value);
 assert(efs_kv_key_inode(shard,ino,inode_key,&il)==EFS_OK && efs_kv_get(kv,inode_key,il,inode_value,&iv)==EFS_OK);
 struct efs_txid held={{122}};struct efs_txn_parts held_parts={.n=1,.shard={shard}};
 assert(efs_txn_prepare_excl_value(kv,&held,&held_parts,inode_key,il,inode_value,iv,EFS_TXN_PUT,inode_value,iv)==EFS_OK);
 assert(efs_meta_finish_append_fenced(kv,ino,gen,off,q.id.client_uuid,1,&t,&ops,kv)==EFS_ERR_BUSY);
 assert(efs_txn_resolve(kv,&held,shard,EFS_TXN_ABORT)==EFS_OK);
 t.bytes[1]=12; /* each attempted transaction keeps a distinct identity */
 uint8_t key[EFS_KV_KEY_MAX],value[48];uint32_t kl,vl=sizeof(value);
 assert(efs_kv_key_append_rsv(shard,ino,gen,208,key,&kl)==EFS_OK);
 assert(efs_kv_get(kv,key,kl,value,&vl)==EFS_OK);value[16]=43;
 assert(efs_kv_put(kv,key,kl,value,vl)==EFS_OK); /* independent surviving owner */
 uint8_t other[16]={43};assert(efs_session_fence_local(kv,shard,other,2)==EFS_OK);
 assert(efs_meta_finish_append_fenced(kv,ino,gen,200,other,1,&t,&ops,kv)==EFS_ERR_STALE);
 const struct efs_meta_append_ops lost={prepare,lost_decision,resolve};
 assert(efs_meta_finish_append_fenced(kv,ino,gen,200,q.id.client_uuid,1,&t,&lost,kv)==EFS_ERR_IO);
 assert(resolve(kv,&t,&last_parts,EFS_TXN_COMMIT)==EFS_OK);
 assert(efs_meta_finish_append_fenced(kv,ino,gen,200,q.id.client_uuid,1,&t,&ops,kv)==EFS_OK);
 assert(efs_meta_append_session_next(kv,shard,q.id.client_uuid,1,&selected,&generation,&off,&found)==EFS_OK && found && off==216);
 t.bytes[1]=11;assert(efs_meta_finish_append_fenced(kv,ino,gen,off,q.id.client_uuid,1,&t,&ops,kv)==EFS_OK);
 assert(efs_meta_append_session_next(kv,shard,q.id.client_uuid,1,&selected,&generation,&off,&found)==EFS_OK && !found);
 uint32_t open;uint64_t watermark,frontier;
 assert(efs_meta_apply_append_state(kv,ino,&watermark,&frontier,&open)==EFS_OK && open==1 && watermark==224 && frontier==208);
 /* Corrupt evidence cannot become false empty-phase proof. */
 assert(efs_kv_put(kv,key,kl,value,2)==EFS_OK);
 assert(efs_meta_append_session_next(kv,shard,q.id.client_uuid,1,&selected,&generation,&off,&found)==EFS_ERR_PROTO && !found);
 assert(efs_kv_put(kv,key,kl,value,vl)==EFS_OK);
 free(plan);efs_kv_mem_free(kv);
}
static int owner_prepare(void *kv,const struct efs_txid *t,const struct efs_txn_parts *parts,
    const struct efs_meta_append_image *image)
{
 int rc=prepare(kv,t,parts,image);
 if(rc==EFS_OK && image->key[2]==EFS_KV_KIND_SESS_LOCAL) {
  uint32_t shard=(uint32_t)image->key[0]<<8|image->key[1];
  assert(efs_session_fence_local(kv,shard,image->key+3,2)==EFS_ERR_BUSY);
 }
 return rc;
}
static void session_owned_append(int cold)
{
 char dir[]="/tmp/efs-owned-append-XXXXXX";
 if(cold)assert(mkdtemp(dir));
 struct efs_kv *kv=cold?efs_kv_lsm_open(dir,NULL):efs_kv_mem_create();assert(kv);
 efs_ino_t ino;uint64_t gen;fixture(kv,&ino,&gen);
 struct efs_opid_req q=identity(1,0);uint32_t shard=efs_kv_inode_shard(ino);
 assert(efs_session_create(kv,q.id.client_uuid,1)==EFS_OK &&
        efs_session_register(kv,q.id.client_uuid,1,shard)==EFS_OK &&
        efs_session_establish(kv,shard,q.id.client_uuid,1)==EFS_OK);
 struct efs_meta_append_plan *p=malloc(sizeof(*p));assert(p);
 assert(efs_meta_capture_append(kv,ino,8,&q,decision,kv,p)==EFS_OK);
 struct efs_txid tx={{199}};const struct efs_meta_append_ops lost={owner_prepare,lost_decision,resolve};
 assert(efs_meta_execute_append_owned(kv,p,&q,&tx,&lost,kv)==EFS_ERR_IO);
 if(cold){efs_kv_lsm_close(kv);kv=efs_kv_lsm_open(dir,NULL);assert(kv);}
 assert(efs_session_fence_local(kv,shard,q.id.client_uuid,2)==EFS_ERR_BUSY);
 if(cold){
  assert(efs_txn_resolve(kv,&tx,efs_kv_lane_shard(ino,1),EFS_TXN_COMMIT)==EFS_OK);
  efs_kv_lsm_close(kv);kv=efs_kv_lsm_open(dir,NULL);assert(kv);
  assert(efs_session_fence_local(kv,shard,q.id.client_uuid,2)==EFS_ERR_BUSY);
 }
 assert(resolve(kv,&tx,&last_parts,EFS_TXN_COMMIT)==EFS_OK);
 assert(efs_session_fence_local(kv,shard,q.id.client_uuid,2)==EFS_OK);
 q.id.seq=2;q.ack=1;tx.bytes[1]=2;
 assert(efs_meta_execute_append_owned(kv,p,&q,&tx,&ops,kv)==EFS_ERR_STALE);
 assert(efs_meta_capture_append(kv,ino,8,&q,decision,kv,p)==EFS_ERR_STALE);
 uint32_t open;assert(efs_meta_apply_append_state(kv,ino,NULL,NULL,&open)==EFS_OK && open==1);
 free(p);
 if(cold){
  efs_kv_lsm_close(kv);DIR *d=opendir(dir);assert(d);struct dirent *e;
  while((e=readdir(d))){if(e->d_name[0]=='.')continue;char path[512];snprintf(path,sizeof(path),"%s/%s",dir,e->d_name);assert(!unlink(path));}
  closedir(d);assert(!rmdir(dir));
 }else efs_kv_mem_free(kv);
}
static void retired_append_receipts(int cold)
{
 char dir[]="/tmp/efs-retired-append-XXXXXX";if(cold)assert(mkdtemp(dir));
 struct efs_kv *kv=cold?efs_kv_lsm_open(dir,NULL):efs_kv_mem_create();assert(kv);
 efs_ino_t ino;uint64_t gen;fixture(kv,&ino,&gen);
 struct efs_meta_append_plan *plan=malloc(sizeof(*plan));assert(plan);
 struct efs_opid_req q=identity(1,0);struct efs_txid tx={{211}};
 for(unsigned i=1;i<=20;i++) {
  q.id.seq=i;q.ack=i-1;
  assert(efs_meta_capture_append(kv,ino,8,&q,decision,kv,plan)==EFS_OK);
  tx.bytes[1]=i;assert(efs_meta_execute_append(plan,&tx,&ops,kv)==EFS_OK);
 }
 unsigned progress=99;uint32_t shard=efs_kv_inode_shard(ino);
 assert(efs_meta_retire_append_session(kv,shard,q.id.client_uuid,1,&tx,&ops,kv,&progress)==EFS_ERR_BUSY && !progress);
 assert(efs_session_fence_local(kv,shard,q.id.client_uuid,2)==EFS_OK);
 /* Any OPEN reservation defers retirement; neither result nor frontier is lost. */
 tx.bytes[1]=30;
 assert(efs_meta_retire_append_session(kv,shard,q.id.client_uuid,1,&tx,&ops,kv,&progress)==EFS_ERR_BUSY && !progress);
 for(unsigned i=20;i;i--) {
  tx.bytes[1]=40+i;
  assert(efs_meta_finish_append(kv,ino,200+(i-1)*8,EFS_META_APPEND_COMPLETED,&tx,&ops,kv)==EFS_OK);
 }
 uint8_t key[EFS_KV_KEY_MAX],value[EFS_OPID_VAL_MAX];uint32_t kl,vl=sizeof(value);
 assert(efs_kv_key_inode(shard,ino,key,&kl)==EFS_OK);key[2]=EFS_KV_KIND_APPEND_RECEIPT;
 memcpy(key+kl,q.id.client_uuid,16);kl+=16;key[kl++]=0;key[kl++]=0;key[kl++]=0;key[kl++]=1;
 assert(efs_kv_get(kv,key,kl,value,&vl)==EFS_OK);
 uint8_t foreign[EFS_KV_KEY_MAX];memcpy(foreign,key,kl);foreign[11]=43;
 struct efs_opid_window window;assert(efs_opid_window_unpack(&window,value,vl)==EFS_OK);
 window.client_uuid[0]=43;uint8_t foreign_value[EFS_OPID_VAL_MAX];uint32_t foreign_len=sizeof(foreign_value);
 assert(efs_opid_window_pack(&window,foreign_value,&foreign_len)==EFS_OK);
 assert(efs_kv_put(kv,foreign,kl,foreign_value,foreign_len)==EFS_OK);
 uint8_t newer[EFS_KV_KEY_MAX],newer_value[EFS_OPID_VAL_MAX];uint32_t newer_len=sizeof(newer_value);
 memcpy(newer,key,kl);newer[kl-1]=2;window.client_uuid[0]=42;window.session_epoch=2;
 assert(efs_opid_window_pack(&window,newer_value,&newer_len)==EFS_OK && efs_kv_put(kv,newer,kl,newer_value,newer_len)==EFS_OK);
 /* Corrupt matching evidence is not empty proof. */
 assert(efs_kv_put(kv,key,kl,value,2)==EFS_OK);tx.bytes[1]=70;
 assert(efs_meta_retire_append_session(kv,shard,q.id.client_uuid,1,&tx,&ops,kv,&progress)==EFS_ERR_PROTO && !progress);
 assert(efs_kv_put(kv,key,kl,value,vl)==EFS_OK);
 uint8_t reservation[EFS_KV_KEY_MAX],body[48];uint32_t rl,bl=sizeof(body);
 assert(efs_kv_key_append_rsv(shard,ino,gen,200,reservation,&rl)==EFS_OK && efs_kv_get(kv,reservation,rl,body,&bl)==EFS_OK);
 body[0]^=1;assert(efs_kv_put(kv,reservation,rl,body,bl)==EFS_OK);
 assert(efs_meta_retire_append_session(kv,shard,q.id.client_uuid,1,&tx,&ops,kv,&progress)==EFS_ERR_PROTO && !progress);
 body[0]^=1;assert(efs_kv_put(kv,reservation,rl,body,bl)==EFS_OK);
 const struct efs_meta_append_ops lost={prepare,lost_decision,resolve};tx.bytes[1]=71;
 assert(efs_meta_retire_append_session(kv,shard,q.id.client_uuid,1,&tx,&lost,kv,&progress)==EFS_ERR_IO && !progress);
 if(cold){efs_kv_lsm_close(kv);kv=efs_kv_lsm_open(dir,NULL);assert(kv);}
 assert(resolve(kv,&tx,&last_parts,EFS_TXN_COMMIT)==EFS_OK);
 unsigned total=8,pages=1;
 do {
  tx.bytes[1]=71+pages++;
  if(total==20) {
   struct efs_txid held={{214}};struct efs_txn_parts part={.n=1,.shard={shard}};
   assert(efs_txn_prepare_excl_value(kv,&held,&part,key,kl,value,vl,EFS_TXN_PUT,value,vl)==EFS_OK);
   assert(efs_meta_retire_append_session(kv,shard,q.id.client_uuid,1,&tx,&ops,kv,&progress)==EFS_ERR_BUSY && !progress);
   assert(efs_txn_resolve(kv,&held,shard,EFS_TXN_ABORT)==EFS_OK);
   tx.bytes[2]=1;
   assert(efs_meta_retire_append_session(kv,shard,q.id.client_uuid,1,&tx,&lost,kv,&progress)==EFS_ERR_IO && !progress);
   if(cold){efs_kv_lsm_close(kv);kv=efs_kv_lsm_open(dir,NULL);assert(kv);}
   assert(resolve(kv,&tx,&last_parts,EFS_TXN_COMMIT)==EFS_OK);progress=1;
  }else assert(efs_meta_retire_append_session(kv,shard,q.id.client_uuid,1,&tx,&ops,kv,&progress)==EFS_OK && progress<=8);
  total+=progress;
 }while(progress);
 assert(total==21 && pages==5); /* 20 DONE rows plus one receipt, then empty */
 vl=sizeof(value);assert(efs_kv_get(kv,key,kl,value,&vl)==EFS_ERR_NOT_FOUND);
 vl=sizeof(value);assert(efs_kv_get(kv,foreign,kl,value,&vl)==EFS_OK && vl==foreign_len && !memcmp(value,foreign_value,vl));
 vl=sizeof(value);assert(efs_kv_get(kv,newer,kl,value,&vl)==EFS_OK && vl==newer_len && !memcmp(value,newer_value,vl));
 assert(efs_session_accept(kv,shard,q.id.client_uuid,1)==EFS_ERR_STALE);
 uint64_t rejected=999;
 assert(efs_meta_apply_append_reserve(kv,ino,8,&q.id,decision,kv,&rejected)==EFS_ERR_STALE && rejected==999);
 assert(efs_lease_open(kv,ino,gen,q.id.client_uuid,1)==EFS_ERR_STALE);
 /* Historical seq0 reservations have no append receipt but must not be
  * mistaken for an empty cleanup phase. */
 struct efs_opid legacy=q.id;legacy.client_uuid[0]=45;legacy.seq=0;uint64_t off=0;
 assert(efs_meta_apply_append_reserve(kv,ino,8,&legacy,decision,kv,&off)==EFS_OK);
 tx.bytes[1]=90;
 assert(efs_meta_finish_append(kv,ino,off,EFS_META_APPEND_COMPLETED,&tx,&ops,kv)==EFS_OK);
 assert(efs_session_fence_local(kv,shard,legacy.client_uuid,2)==EFS_OK);
 tx.bytes[1]=91;
 assert(efs_meta_retire_append_session(kv,shard,legacy.client_uuid,1,&tx,&ops,kv,&progress)==EFS_OK && progress==1);
 assert(efs_meta_apply_append_reserve(kv,ino,8,&legacy,decision,kv,&off)==EFS_ERR_STALE);
 tx.bytes[1]=92;
 assert(efs_meta_retire_append_session(kv,shard,legacy.client_uuid,1,&tx,&ops,kv,&progress)==EFS_OK && !progress);
 free(plan);
 if(cold){
  efs_kv_lsm_close(kv);DIR *d=opendir(dir);assert(d);struct dirent *e;
  while((e=readdir(d))){if(e->d_name[0]=='.')continue;char path[512];snprintf(path,sizeof(path),"%s/%s",dir,e->d_name);assert(!unlink(path));}
  closedir(d);assert(!rmdir(dir));
 }else efs_kv_mem_free(kv);
}
int main(void)
{retired_append_receipts(0);retired_append_receipts(1);session_owned_append(0);session_owned_append(1);fenced_completion();unit();bounds_and_pending();completion();close_completion();recovery(0);recovery(1);puts("append transaction: fixed EOF, exact lane/inode guards, conflict abort, lost COMMIT and partial resolution recovery PASS");return 0;}
