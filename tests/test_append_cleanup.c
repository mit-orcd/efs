/* K15/W64 cleanup-append: a reservation granted with a lost reply is retired
 * through the real transaction apply path, decide-once. Fenced owner:
 * FENCED_HOLE through session cleanup. Live owner: explicit abandon of the
 * exact identity. The frontier advances, later appends proceed, a late
 * completion or replay is refused, and a completion already in the log is
 * never overtaken. */
#include "efs/meta_apply.h"
#include "efs/session.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
static unsigned append_wait; /* K42 wait reason of the last cleanup step */

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
 for(unsigned i=0;i<p->n;++i){int rc=efs_txn_resolve(ctx,t,p->shard[i],d);if(rc)return rc;}
 return EFS_OK;
}
static const struct efs_meta_append_ops ops={prepare,decide,resolve};
/* COMMIT decided, participants not yet resolved: a completion in the log. */
static struct efs_txn_parts pending_parts;
static int decide_only(void *ctx,const struct efs_txid *t,const struct efs_txn_parts *p,int d)
{pending_parts=*p;assert(decide(ctx,t,p,d)==EFS_OK);return EFS_ERR_IO;}
static const struct efs_meta_append_ops lost_resolve={prepare,decide_only,resolve};

static struct efs_opid_req identity(unsigned char owner,unsigned seq,unsigned ack)
{struct efs_opid_req q={0};q.id.client_uuid[0]=owner;q.id.session_epoch=1;q.id.seq=seq;q.ack=ack;return q;}
static struct efs_meta_pub publication(efs_ino_t ino,uint64_t gen,unsigned ci,uint64_t end,unsigned candidate)
{
 struct efs_meta_pub p={0};p.ino=ino;p.inode_gen=gen;p.chunk_index=ci;
 p.new_size=end;p.now=100;p.candidate_gen=candidate;
 p.coding_profile_id=EFS_META_PROFILE_K2F1;
 for(unsigned i=0;i<EFS_NUM_FRAGMENTS;++i)p.ch.nodes[i]=i+1;
 return p;
}
/* EOF 200 on three active lanes; owners 42 and 43 established (K5). */
static struct efs_kv *fixture(efs_ino_t *ino,uint64_t *gen)
{
 struct efs_kv *kv=efs_kv_mem_create();assert(kv);
 struct efs_meta_attrs a={1,1,1};struct efs_meta_row row;
 assert(efs_meta_apply_init(kv,1)==EFS_OK);
 assert(efs_meta_apply_create_file(kv,&a,EFS_ROOT_INO,S_IFREG|0644,"file",ino)==EFS_OK);
 assert(efs_meta_apply_get_inode(kv,*ino,&row)==EFS_OK);*gen=row.generation;
 assert(efs_meta_apply_activate_lanes(kv,*ino,3)==EFS_OK);
 struct efs_meta_pub p=publication(*ino,*gen,0,100,10);
 assert(efs_meta_apply_publish(kv,&p)==EFS_OK);
 p=publication(*ino,*gen,1,200,20);assert(efs_meta_apply_publish(kv,&p)==EFS_OK);
 uint8_t uuid[16]={42};
 assert(efs_session_establish(kv,efs_kv_inode_shard(*ino),uuid,1)==EFS_OK);
 uuid[0]=43;assert(efs_session_establish(kv,efs_kv_inode_shard(*ino),uuid,1)==EFS_OK);
 return kv;
}
static uint8_t txn_seq;
static struct efs_txid next_tx(void){struct efs_txid t={{0}};t.bytes[0]=77;t.bytes[1]=++txn_seq;return t;}
/* Grant: the reservation commits; the caller drops the reply (lost). */
static uint64_t grant(struct efs_kv *kv,efs_ino_t ino,const struct efs_opid_req *q)
{
 struct efs_meta_append_plan *plan=malloc(sizeof(*plan));assert(plan);
 assert(efs_meta_capture_append(kv,ino,8,q,decision,kv,plan)==EFS_OK);
 struct efs_txid t=next_tx();
 assert(efs_meta_execute_append_owned(kv,plan,q,&t,&ops,kv)==EFS_OK);
 uint64_t off=plan->offset;free(plan);return off;
}
static int try_reserve(struct efs_kv *kv,efs_ino_t ino,const struct efs_opid_req *q,uint64_t *off)
{
 struct efs_meta_append_plan *plan=malloc(sizeof(*plan));assert(plan);
 int rc=efs_meta_capture_append(kv,ino,8,q,decision,kv,plan);
 if(rc==EFS_OK && plan->count) {
  struct efs_txid t=next_tx();rc=efs_meta_execute_append_owned(kv,plan,q,&t,&ops,kv);
 }
 if(off)*off=plan->offset;
 free(plan);return rc;
}
static void state(struct efs_kv *kv,efs_ino_t ino,uint32_t *open,uint64_t *size)
{
 uint64_t watermark=0,frontier=0;*open=0;
 int rc=efs_meta_apply_append_state(kv,ino,&watermark,&frontier,open);
 assert(rc==EFS_OK || rc==EFS_ERR_NOT_FOUND);
 struct efs_meta_stat st;assert(efs_meta_apply_getattr(kv,ino,decision,kv,&st)==EFS_OK);
 *size=st.size;
}
static int complete(struct efs_kv *kv,efs_ino_t ino,uint64_t off,const struct efs_opid_req *q)
{struct efs_txid t=next_tx();return efs_meta_finish_append_owned(kv,ino,off,EFS_META_APPEND_COMPLETED,&q->id,&t,&ops,kv);}
static int abandon(struct efs_kv *kv,efs_ino_t ino,const struct efs_opid_req *q,unsigned *retired)
{struct efs_txid t=next_tx();return efs_meta_abandon_append(kv,ino,8,q,&t,&ops,kv,retired);}
static uint8_t row_outcome(struct efs_kv *kv,efs_ino_t ino,uint64_t gen,uint64_t off)
{
 uint8_t key[EFS_KV_KEY_MAX],value[48];uint32_t kl,vl=sizeof(value);
 assert(efs_kv_key_append_rsv(efs_kv_inode_shard(ino),ino,gen,off,key,&kl)==EFS_OK);
 assert(efs_kv_get(kv,key,kl,value,&vl)==EFS_OK && vl==48 && value[44]==1);
 return value[45];
}

/* Grant, lose the reply, fence the session, cleanup retires, frontier
 * advances, late completion refused. */
static void fenced_lost_reply(void)
{
 efs_ino_t ino;uint64_t gen;struct efs_kv *kv=fixture(&ino,&gen);
 uint32_t shard=efs_kv_inode_shard(ino),open;uint64_t size,off2;
 struct efs_opid_req a=identity(42,1,0),b=identity(43,1,0);
 uint64_t off=grant(kv,ino,&a);assert(off==200);
 state(kv,ino,&open,&size);assert(open==1 && size==200);
 /* The stranded reservation blocks every other appender. */
 assert(try_reserve(kv,ino,&b,NULL)==EFS_ERR_BUSY);
 /* Session still established: no cleanup authority yet. */
 struct efs_txid t=next_tx();
 assert(efs_meta_finish_append_fenced(kv,ino,gen,off,a.id.client_uuid,1,&t,&ops,kv,&append_wait)==EFS_ERR_BUSY);
 assert(efs_session_fence_local(kv,shard,a.id.client_uuid,2)==EFS_OK);
 efs_ino_t sel;uint64_t sgen,soff;unsigned found=0;
 assert(efs_meta_append_session_next(kv,shard,a.id.client_uuid,1,&sel,&sgen,&soff,&found)==EFS_OK &&
        found && sel==ino && sgen==gen && soff==off);
 t=next_tx();
 assert(efs_meta_finish_append_fenced(kv,ino,gen,off,a.id.client_uuid,1,&t,&ops,kv,&append_wait)==EFS_OK);
 assert(row_outcome(kv,ino,gen,off)==EFS_META_APPEND_FENCED_HOLE);
 state(kv,ino,&open,&size);assert(!open && size==208);
 assert(efs_meta_append_session_next(kv,shard,a.id.client_uuid,1,&sel,&sgen,&soff,&found)==EFS_OK && !found);
 /* Later appends proceed past the retired range. */
 assert(try_reserve(kv,ino,&b,&off2)==EFS_OK && off2==208);
 /* Late completions from the fenced owner are refused, never applied. */
 assert(complete(kv,ino,off,&a)==EFS_ERR_FENCED);
 uint8_t key[EFS_KV_KEY_MAX],body[48];uint32_t kl,vl=sizeof(body);
 assert(efs_kv_key_append_rsv(shard,ino,gen,off,key,&kl)==EFS_OK && efs_kv_get(kv,key,kl,body,&vl)==EFS_OK);
 body[45]=0; /* the OPEN image the owner captured before retirement */
 assert(efs_meta_apply_append_complete_atomic(kv,ino,gen,off,&a.id,0,body)==EFS_ERR_FENCED);
 /* The decide-once row itself refuses a completion of the retired range. */
 t=next_tx();
 assert(efs_meta_finish_append(kv,ino,off,EFS_META_APPEND_COMPLETED,&t,&ops,kv)==EFS_ERR_STALE);
 assert(efs_meta_apply_append_resolve(kv,ino,off,EFS_META_APPEND_COMPLETED)==EFS_ERR_STALE);
 /* A repeated retirement is a no-op. */
 t=next_tx();
 assert(efs_meta_finish_append_fenced(kv,ino,gen,off,a.id.client_uuid,1,&t,&ops,kv,&append_wait)==EFS_OK);
 assert(row_outcome(kv,ino,gen,off)==EFS_META_APPEND_FENCED_HOLE);
 assert(complete(kv,ino,off2,&b)==EFS_OK);
 state(kv,ino,&open,&size);assert(!open && size==216);
 efs_kv_mem_free(kv);
}

/* Grant, lose the reply, the live owner abandons the exact identity. */
static void abandoned_lost_reply(void)
{
 efs_ino_t ino;uint64_t gen;struct efs_kv *kv=fixture(&ino,&gen);
 uint32_t open;uint64_t size,off2=0;unsigned retired=9;
 struct efs_opid_req a=identity(42,1,0),b=identity(43,1,0);
 uint64_t off=grant(kv,ino,&a);assert(off==200);
 assert(try_reserve(kv,ino,&b,NULL)==EFS_ERR_BUSY);
 /* Wrong length names a different operation. */
 struct efs_txid t=next_tx();
 assert(efs_meta_abandon_append(kv,ino,9,&a,&t,&ops,kv,&retired)==EFS_ERR_INVAL && !retired);
 assert(abandon(kv,ino,&a,&retired)==EFS_OK && retired==1);
 assert(row_outcome(kv,ino,gen,off)==EFS_META_APPEND_ABORTED_HOLE);
 state(kv,ino,&open,&size);assert(!open && size==208);
 assert(try_reserve(kv,ino,&b,&off2)==EFS_OK && off2==208);
 /* The receipt says retired: a replayed reserve never learns the offset
  * and never allocates again; a late completion is refused. */
 uint64_t got=999;
 assert(efs_meta_append_result(kv,ino,8,&a,&got)==EFS_ERR_STALE && got==999);
 assert(try_reserve(kv,ino,&a,NULL)==EFS_ERR_STALE);
 assert(efs_meta_apply_append_reserve_durable(kv,ino,8,&a,decision,kv,&got)==EFS_ERR_STALE && got==999);
 assert(complete(kv,ino,off,&a)==EFS_ERR_STALE);
 assert(abandon(kv,ino,&a,&retired)==EFS_OK && !retired); /* exact replay */
 /* Delayed reserve: the abandon lands first, the reserve can never apply. */
 struct efs_opid_req late=identity(42,2,1);
 assert(abandon(kv,ino,&late,&retired)==EFS_OK && !retired);
 assert(try_reserve(kv,ino,&late,NULL)==EFS_ERR_STALE);
 assert(efs_meta_apply_append_reserve_durable(kv,ino,8,&late,decision,kv,&got)==EFS_ERR_STALE);
 state(kv,ino,&open,&size);assert(open==1 && size==208); /* only b's reservation */
 /* The owner's next identity still appends normally. */
 struct efs_opid_req next=identity(42,3,2);
 assert(complete(kv,ino,off2,&b)==EFS_OK);
 uint64_t off3=0;assert(try_reserve(kv,ino,&next,&off3)==EFS_OK && off3==216);
 assert(complete(kv,ino,off3,&next)==EFS_OK);
 state(kv,ino,&open,&size);assert(!open && size==224);
 /* A fenced owner is cleaned by session cleanup, not by abandon. */
 assert(efs_session_fence_local(kv,efs_kv_inode_shard(ino),a.id.client_uuid,2)==EFS_OK);
 struct efs_opid_req fenced=identity(42,4,3);
 assert(abandon(kv,ino,&fenced,&retired)==EFS_ERR_FENCED && !retired);
 efs_kv_mem_free(kv);
}

/* Grant, complete, cleanup is a no-op; a completion already in the log is
 * never overtaken by a retirement. */
static void completed_then_cleanup(void)
{
 efs_ino_t ino;uint64_t gen;struct efs_kv *kv=fixture(&ino,&gen);
 uint32_t shard=efs_kv_inode_shard(ino),open;uint64_t size;unsigned retired=9;
 struct efs_opid_req a=identity(42,1,0);
 uint64_t off=grant(kv,ino,&a);
 assert(complete(kv,ino,off,&a)==EFS_OK);
 assert(row_outcome(kv,ino,gen,off)==EFS_META_APPEND_COMPLETED);
 state(kv,ino,&open,&size);assert(!open && size==208);
 assert(abandon(kv,ino,&a,&retired)==EFS_OK && !retired);
 assert(row_outcome(kv,ino,gen,off)==EFS_META_APPEND_COMPLETED);
 assert(complete(kv,ino,off,&a)==EFS_OK); /* completion replay stays OK */
 assert(efs_session_fence_local(kv,shard,a.id.client_uuid,2)==EFS_OK);
 struct efs_txid t=next_tx();
 assert(efs_meta_finish_append_fenced(kv,ino,gen,off,a.id.client_uuid,1,&t,&ops,kv,&append_wait)==EFS_OK);
 assert(row_outcome(kv,ino,gen,off)==EFS_META_APPEND_COMPLETED);
 state(kv,ino,&open,&size);assert(!open && size==208);
 efs_kv_mem_free(kv);

 /* Completion decided (in the log) but not yet resolved: the abandon and
  * the fenced cleanup both stop at its intents; after resolution the
  * completion stands and the retirement does nothing. */
 kv=fixture(&ino,&gen);shard=efs_kv_inode_shard(ino);
 struct efs_opid_req b=identity(42,1,0);
 off=grant(kv,ino,&b);
 struct efs_txid inflight=next_tx();
 assert(efs_meta_finish_append_owned(kv,ino,off,EFS_META_APPEND_COMPLETED,&b.id,&inflight,&lost_resolve,kv)==EFS_ERR_IO);
 assert(abandon(kv,ino,&b,&retired)==EFS_ERR_BUSY && !retired);
 assert(resolve(kv,&inflight,&pending_parts,EFS_TXN_COMMIT)==EFS_OK);
 assert(abandon(kv,ino,&b,&retired)==EFS_OK && !retired);
 assert(row_outcome(kv,ino,gen,off)==EFS_META_APPEND_COMPLETED);
 state(kv,ino,&open,&size);assert(!open && size==208);
 efs_kv_mem_free(kv);
}

/* Grant with the owner alive and within its deadline: no cleanup acts. */
static void live_owner_untouched(void)
{
 efs_ino_t ino;uint64_t gen;struct efs_kv *kv=fixture(&ino,&gen);
 uint32_t shard=efs_kv_inode_shard(ino),open;uint64_t size;unsigned retired=9;
 struct efs_opid_req a=identity(42,1,0);
 uint64_t off=grant(kv,ino,&a);
 struct efs_txid t=next_tx();
 assert(efs_meta_finish_append_fenced(kv,ino,gen,off,a.id.client_uuid,1,&t,&ops,kv,&append_wait)==EFS_ERR_BUSY);
 efs_ino_t sel;uint64_t sgen,soff;unsigned found=7,progress=7;
 assert(efs_meta_append_session_next(kv,shard,a.id.client_uuid,1,&sel,&sgen,&soff,&found)==EFS_ERR_BUSY && !found);
 t=next_tx();
 assert(efs_meta_retire_append_session(kv,shard,a.id.client_uuid,1,&t,&ops,kv,&progress,&append_wait)==EFS_ERR_BUSY && !progress);
 /* Abandoning another identity of the same owner leaves this one alone. */
 struct efs_opid_req other=identity(42,2,0);
 assert(abandon(kv,ino,&other,&retired)==EFS_OK && !retired);
 /* A different owner cannot abandon it (its receipt window is separate). */
 struct efs_opid_req foreign=identity(43,1,0);
 assert(abandon(kv,ino,&foreign,&retired)==EFS_OK && !retired);
 state(kv,ino,&open,&size);assert(open==1 && size==200);
 /* The owner, alive, replays and completes normally. */
 uint64_t got=0;assert(efs_meta_append_result(kv,ino,8,&a,&got)==EFS_OK && got==off);
 assert(complete(kv,ino,off,&a)==EFS_OK);
 state(kv,ino,&open,&size);assert(!open && size==208);
 efs_kv_mem_free(kv);
}

/* K42 (Task A candidate D): a revoked session's append receipt sits on an
 * inode another live session is appending to. Its retirement waits on that
 * OPEN reservation and names the wait (OPEN), a PREPARE that loses its lock
 * aborts and names ABORTED, and once the foreign close REPORT lands the same
 * step retires everything and the shard is idle. Nothing is committed while
 * a wait is named; the foreign append is never discarded. */
static int prepare_locked(void *ctx,const struct efs_txid *t,const struct efs_txn_parts *p,
                          const struct efs_meta_append_image *q)
{(void)ctx;(void)t;(void)p;(void)q;return EFS_ERR_BUSY;}
static const struct efs_meta_append_ops locked={prepare_locked,decide,resolve};
/* The coordinator's own DECIDE ABORT fails too (it lost its group). */
static int decide_lost(void *ctx,const struct efs_txid *t,const struct efs_txn_parts *p,int d)
{(void)ctx;(void)t;(void)p;assert(d==EFS_TXN_ABORT);return EFS_ERR_NOT_PRIMARY;}
static const struct efs_meta_append_ops locked_lost={prepare_locked,decide_lost,resolve};
static void foreign_open_wait(void)
{
 efs_ino_t ino;uint64_t gen;struct efs_kv *kv=fixture(&ino,&gen);
 uint32_t shard=efs_kv_inode_shard(ino),open;uint64_t size;unsigned progress=9;int idle=7;
 struct efs_opid_req a=identity(42,1,0),b=identity(43,1,0);
 uint64_t off=grant(kv,ino,&a);
 assert(complete(kv,ino,off,&a)==EFS_OK);
 uint64_t off2=grant(kv,ino,&b);assert(off2==208);
 assert(efs_session_fence_local(kv,shard,a.id.client_uuid,2)==EFS_OK);
 assert(efs_meta_append_session_idle(kv,shard,a.id.client_uuid,1,&idle)==EFS_OK && !idle);
 for(unsigned i=0;i<3;i++) {
  struct efs_txid t=next_tx();append_wait=99;
  assert(efs_meta_retire_append_session(kv,shard,a.id.client_uuid,1,&t,&ops,kv,&progress,&append_wait)==EFS_ERR_BUSY);
  assert(!progress && append_wait==EFS_META_APPEND_WAIT_OPEN);
 }
 state(kv,ino,&open,&size);assert(open==1 && size==208);
 assert(complete(kv,ino,off2,&b)==EFS_OK);
 state(kv,ino,&open,&size);assert(!open && size==216);
 /* A PREPARE refused by a held lock aborts: BUSY, ABORTED, nothing applied. */
 struct efs_txid t=next_tx();append_wait=99;
 assert(efs_meta_retire_append_session(kv,shard,a.id.client_uuid,1,&t,&locked,kv,&progress,&append_wait)==EFS_ERR_BUSY);
 assert(!progress && append_wait==EFS_META_APPEND_WAIT_ABORTED);
 /* COMMIT was never proposed: still a certified no-op when ABORT's own
  * decision fails (the server's finisher decides it). */
 t=next_tx();append_wait=99;
 assert(efs_meta_retire_append_session(kv,shard,a.id.client_uuid,1,&t,&locked_lost,kv,&progress,&append_wait)==EFS_ERR_BUSY);
 assert(!progress && append_wait==EFS_META_APPEND_WAIT_ABORTED);
 assert(efs_meta_append_session_idle(kv,shard,a.id.client_uuid,1,&idle)==EFS_OK && !idle);
 unsigned steps=0;
 do {
  t=next_tx();append_wait=99;
  assert(efs_meta_retire_append_session(kv,shard,a.id.client_uuid,1,&t,&ops,kv,&progress,&append_wait)==EFS_OK);
  assert(append_wait==EFS_META_APPEND_WAIT_NONE && ++steps<16);
 } while(progress);
 assert(steps>=2);
 assert(efs_meta_append_session_idle(kv,shard,a.id.client_uuid,1,&idle)==EFS_OK && idle);
 state(kv,ino,&open,&size);assert(!open && size==216);
 efs_kv_mem_free(kv);
}

int main(void)
{
 fenced_lost_reply();
 abandoned_lost_reply();
 completed_then_cleanup();
 live_owner_untouched();
 foreign_open_wait();
 puts("append cleanup (K15/W64): lost reply retired by fence or abandon, frontier advances, late completion/replay refused, completion never overtaken, live owner untouched PASS");
 puts("append cleanup (K42): revoked receipt waits on a foreign OPEN (named), aborted PREPARE named (also with a failed ABORT decide), retired after the foreign REPORT PASS");
 return 0;
}
