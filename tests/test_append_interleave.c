/* K9/W87: seeded interleavings of concurrent O_APPEND writers over the real
 * reservation state machine (capture/execute, receipts, REPORT completion,
 * abandon, session fence cleanup, prune), with an exact record-set check.
 *
 * Two owners (two mounts) issue numbered writes of distinct lengths. Each
 * step picks one enabled event: reserve (reply delivered, reply lost, or the
 * transaction left decided-but-unresolved or prepared-only, as a coordinator
 * that lost its group mid-transaction leaves it), replay of a lost reply,
 * abandon of a lost identity, recovery of an in-flight transaction (COMMIT
 * resolve, or ABORT of a prepared-only one), REPORT completion of granted
 * writes (one or a batch), a session fence with cleanup and re-registration,
 * and append-row pruning. Acknowledged = the client learned the offset
 * (write(2) returned). The checks:
 *   - a replay returns the offset the server granted, never another one;
 *   - a live owner's completion of an acknowledged write never fails, and
 *     the row then says COMPLETED (never a hole);
 *   - a hole only ever comes from an abandon of a write that failed, or from
 *     a fence (that owner's later fsync fails: accounted, not silent);
 *   - per owner, offsets grow in issue order;
 *   - once settled, every applied reservation tiles [200, size) exactly,
 *     with no overlap and no gap, and nopen is 0.
 * Negative control: a corrupted tiling (a dropped record) must be refused
 * by the same checker. */
#include "efs/meta_apply.h"
#include "efs/session.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

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
/* Coordinator lost after DECIDE (in the log) or after PREPARE (no decision). */
static struct efs_txn_parts held_parts;
/* Only the COMMIT decision is lost; a failed prepare still aborts normally. */
static int decide_only(void *ctx,const struct efs_txid *t,const struct efs_txn_parts *p,int d)
{if(d!=EFS_TXN_COMMIT)return decide(ctx,t,p,d);held_parts=*p;assert(decide(ctx,t,p,d)==EFS_OK);return EFS_ERR_IO;}
static int no_decide(void *ctx,const struct efs_txid *t,const struct efs_txn_parts *p,int d)
{if(d!=EFS_TXN_COMMIT)return decide(ctx,t,p,d);held_parts=*p;return EFS_ERR_IO;}
static const struct efs_meta_append_ops decided_ops={prepare,decide_only,resolve};
static const struct efs_meta_append_ops prepared_ops={prepare,no_decide,resolve};

static uint32_t txn_seq;
static struct efs_txid next_tx(void)
{struct efs_txid t={{0}};t.bytes[0]=91;t.bytes[1]=(uint8_t)(txn_seq>>16);
 t.bytes[2]=(uint8_t)(txn_seq>>8);t.bytes[3]=(uint8_t)++txn_seq;return t;}

enum { IDLE, LOST, DECIDED, PREPARED, GRANTED, COMPLETED, ABANDONED, FENCED };
#define OWNERS 2
#define WRITES 7
#define BASE 200
struct wr {
 int owner, state, applied;      /* applied: the server holds a reservation */
 uint32_t epoch; unsigned seq;   /* identity once issued */
 uint64_t len, off;              /* off: server's grant, once applied */
 struct efs_txid tx; struct efs_txn_parts parts;
};
struct owner { uint8_t uuid; uint32_t epoch; unsigned next_seq; };
static struct wr w[OWNERS*WRITES];
static struct owner own[OWNERS];
static efs_ino_t ino; static uint64_t gen; static struct efs_kv *kv;
static uint64_t rng;
static unsigned pick(unsigned n){rng=rng*6364136223846793005ULL+1442695040888963407ULL;return (unsigned)((rng>>33)%n);}
static unsigned long stats[16];
static int trace;
#define TR(...) do{if(trace)fprintf(stderr,__VA_ARGS__);}while(0)

static struct efs_opid_req ident(const struct wr *x)
{
 struct efs_opid_req q={0};q.id.client_uuid[0]=own[x->owner].uuid;
 q.id.session_epoch=x->epoch;q.id.seq=x->seq;
 /* ACK: everything below this owner's oldest unresolved identity. */
 unsigned ack=x->seq-1;
 for(unsigned i=0;i<OWNERS*WRITES;i++) {
  const struct wr *y=&w[i];
  if(y->owner==x->owner && y->epoch==x->epoch && y->seq && y->seq<=ack &&
     (y->state==LOST || y->state==DECIDED || y->state==PREPARED))ack=y->seq-1;
 }
 q.ack=ack;return q;
}
static uint8_t row_outcome(uint64_t off,int *present)
{
 uint8_t key[EFS_KV_KEY_MAX],value[48];uint32_t kl,vl=sizeof(value);
 assert(efs_kv_key_append_rsv(efs_kv_inode_shard(ino),ino,gen,off,key,&kl)==EFS_OK);
 int rc=efs_kv_get(kv,key,kl,value,&vl);
 *present=rc==EFS_OK;
 if(rc==EFS_ERR_NOT_FOUND)return 0;
 assert(rc==EFS_OK && vl==48);
 return value[44]?value[45]:0xff;
}
static void expect_outcome(uint64_t off,uint8_t want)
{int present;uint8_t got=row_outcome(off,&present);if(present)assert(got==want);}
static void note_applied(struct wr *x,uint64_t off)
{
 /* Offsets of one owner grow in issue order; no two grants overlap. */
 for(unsigned i=0;i<OWNERS*WRITES;i++) {
  struct wr *y=&w[i];
  if(y==x || !y->applied)continue;
  if(!(off>=y->off+y->len || off+x->len<=y->off)) {
   fprintf(stderr,"overlap: new owner=%d epoch=%u seq=%u state=%d off=%llu len=%llu "
           "vs owner=%d epoch=%u seq=%u state=%d off=%llu len=%llu\n",x->owner,x->epoch,x->seq,
           x->state,(unsigned long long)off,(unsigned long long)x->len,y->owner,y->epoch,y->seq,
           y->state,(unsigned long long)y->off,(unsigned long long)y->len);
   abort();
  }
  if(y->owner==x->owner && y->epoch==x->epoch && y->seq<x->seq)assert(y->off<off);
 }
 x->applied=1;x->off=off;
}
static int pending_any(void)
{for(unsigned i=0;i<OWNERS*WRITES;i++)if(w[i].state==DECIDED||w[i].state==PREPARED)return 1;return 0;}

/* Reserve x with its identity; mode 0 delivered, 1 reply lost, 2 decided-in-
 * flight, 3 prepared-only. */
static void reserve(struct wr *x,unsigned mode)
{
 struct efs_opid_req q=ident(x);
 struct efs_meta_append_plan *plan=malloc(sizeof(*plan));assert(plan);
 int rc=efs_meta_capture_append(kv,ino,x->len,&q,decision,kv,plan);
 TR("reserve o=%d e=%u s=%u ack=%u mode=%u capture=%d count=%u off=%llu\n",x->owner,x->epoch,x->seq,(unsigned)q.ack,mode,rc,plan->count,(unsigned long long)plan->offset);
 if(rc==EFS_OK && !plan->count) { /* receipt replay inside capture */
  free(plan);uint64_t off=0;rc=efs_meta_append_result(kv,ino,x->len,&q,&off);
  assert(rc==EFS_OK && x->applied && off==x->off);
  if(mode!=1)x->state=GRANTED;
  return;
 }
 if(rc!=EFS_OK) {
  free(plan);
  assert(rc==EFS_ERR_BUSY);
  /* The client's BUSY loop keeps this identity: retry or abandon later. */
  x->state=LOST;stats[0]++;return;
 }
 x->tx=next_tx();
 const struct efs_meta_append_ops *o=mode==2?&decided_ops:mode==3?&prepared_ops:&ops;
 rc=efs_meta_execute_append_owned(kv,plan,&q,&x->tx,o,kv);
 uint64_t off=plan->offset;free(plan);
 TR("  execute=%d\n",rc);
 if(mode>=2 && rc!=EFS_ERR_BUSY) {
  assert(rc==EFS_ERR_IO);x->parts=held_parts;
  if(mode==2)note_applied(x,off);
  x->state=mode==2?DECIDED:PREPARED;stats[mode==2?5:6]++;return;
 }
 if(rc==EFS_ERR_BUSY){x->state=LOST;stats[0]++;return;}
 assert(rc==EFS_OK);
 note_applied(x,off);
 x->state=mode==1?LOST:GRANTED;stats[mode==1?2:1]++;
}
static void issue(int o)
{
 for(unsigned i=0;i<OWNERS*WRITES;i++) {
  struct wr *x=&w[i];
  if(x->owner!=o || x->state!=IDLE || x->seq)continue;
  x->epoch=own[o].epoch;x->seq=++own[o].next_seq;
  reserve(x,pick(8)<5?0:pick(3)+1);return;
 }
}
/* Lost reply: replay the identity (same offset) or give up and abandon. */
static void resolve_lost(struct wr *x,int abandon_it)
{
 struct efs_opid_req q=ident(x);
 if(!abandon_it) {
  uint64_t off=0;int rc=efs_meta_append_result(kv,ino,x->len,&q,&off);
  TR("replay o=%d e=%u s=%u rc=%d off=%llu\n",x->owner,x->epoch,x->seq,rc,(unsigned long long)off);
  if(rc==EFS_OK){assert(x->applied && off==x->off);x->state=GRANTED;stats[3]++;return;}
  assert(rc==EFS_ERR_NOT_FOUND && !x->applied);
  reserve(x,0);return; /* the client's retry loop, same identity */
 }
 unsigned retired=9;struct efs_txid t=next_tx();
 int rc=efs_meta_abandon_append(kv,ino,x->len,&q,&t,&ops,kv,&retired);
 TR("abandon o=%d e=%u s=%u rc=%d retired=%u\n",x->owner,x->epoch,x->seq,rc,retired);
 if(rc==EFS_ERR_BUSY){stats[0]++;return;}
 assert(rc==EFS_OK);
 assert(retired==(unsigned)x->applied); /* a granted reservation is always OPEN here */
 if(x->applied)expect_outcome(x->off,EFS_META_APPEND_ABORTED_HOLE);
 x->state=ABANDONED;stats[4]++;
 /* Never learnable afterwards. */
 uint64_t off=7;assert(efs_meta_append_result(kv,ino,x->len,&q,&off)==EFS_ERR_STALE && off==7);
}
static void recover(struct wr *x)
{
 TR("recover o=%d e=%u s=%u state=%d\n",x->owner,x->epoch,x->seq,x->state);
 if(x->state==DECIDED) {
  assert(resolve(kv,&x->tx,&x->parts,EFS_TXN_COMMIT)==EFS_OK);
  x->state=LOST; /* the client saw IO; it replays or abandons */
 } else {
  assert(efs_txn_decide(kv,efs_txn_coordinator(&x->tx,&x->parts),&x->tx,EFS_TXN_ABORT)==EFS_OK);
  assert(resolve(kv,&x->tx,&x->parts,EFS_TXN_ABORT)==EFS_OK);
  x->state=LOST;
 }
 stats[7]++;
}
/* REPORT: publish one or all of an owner's acknowledged writes. */
static void report(int o,int all)
{
 uint64_t offs[OWNERS*WRITES];struct wr *xs[OWNERS*WRITES];uint32_t n=0;
 for(unsigned i=0;i<OWNERS*WRITES && (all || !n);i++) {
  struct wr *x=&w[i];
  if(x->owner==o && x->state==GRANTED && n<EFS_META_APPEND_COMPLETE_MAX){offs[n]=x->off;xs[n++]=x;}
 }
 if(!n)return;
 struct efs_opid id;memset(&id,0,sizeof(id));id.client_uuid[0]=own[o].uuid;id.session_epoch=own[o].epoch;
 struct efs_txid t=next_tx();
 int rc=efs_meta_finish_append_batch_owned(kv,ino,offs,n,EFS_META_APPEND_COMPLETED,&id,&t,&ops,kv);
 TR("report o=%d n=%u first=%llu rc=%d\n",o,n,(unsigned long long)offs[0],rc);
 if(rc==EFS_ERR_BUSY && pending_any()){stats[0]++;return;}
 /* An acknowledged write of a live owner always publishes. */
 if(rc!=EFS_OK){fprintf(stderr,"completion rc=%d n=%u owner=%d\n",rc,n,o);abort();}
 for(uint32_t i=0;i<n;i++){xs[i]->state=COMPLETED;expect_outcome(xs[i]->off,EFS_META_APPEND_COMPLETED);}
 stats[8]+=n;
}
/* Session fence (lease expired): cleanup retires the owner's OPEN rows as
 * FENCED_HOLE; acknowledged unpublished writes are reported to the app as a
 * failed fsync. The owner re-registers with the next epoch. */
static void fence(int o)
{
 uint32_t shard=efs_kv_inode_shard(ino),old=own[o].epoch;uint8_t uuid[16]={own[o].uuid};
 TR("fence o=%d epoch=%u\n",o,old);
 assert(efs_session_fence_local(kv,shard,uuid,old+1)==EFS_OK);
 for(unsigned guard=0;;guard++) {
  assert(guard<64);
  efs_ino_t sel;uint64_t sgen,soff;unsigned found=0;
  assert(efs_meta_append_session_next(kv,shard,uuid,old,&sel,&sgen,&soff,&found)==EFS_OK);
  if(!found)break;
  struct efs_txid t=next_tx();
  assert(sel==ino && sgen==gen);
  assert(efs_meta_finish_append_fenced(kv,ino,gen,soff,uuid,old,&t,&ops,kv)==EFS_OK);
  expect_outcome(soff,EFS_META_APPEND_FENCED_HOLE);
 }
 for(unsigned i=0;i<OWNERS*WRITES;i++) {
  struct wr *x=&w[i];
  if(x->owner!=o || x->epoch!=old)continue;
  if(x->state==GRANTED || x->state==LOST){x->state=FENCED;stats[9]++;}
  /* A late completion from the fenced epoch is refused. */
  if(x->state==FENCED && x->applied) {
   struct efs_opid id;memset(&id,0,sizeof(id));id.client_uuid[0]=own[o].uuid;id.session_epoch=old;
   struct efs_txid t=next_tx();
   assert(efs_meta_finish_append_owned(kv,ino,x->off,EFS_META_APPEND_COMPLETED,&id,&t,&ops,kv)==EFS_ERR_FENCED);
  }
 }
 own[o].epoch=old+1;own[o].next_seq=0;
 assert(efs_session_establish(kv,shard,uuid,own[o].epoch)==EFS_OK);
}
static void prune(void)
{
 int due=efs_meta_append_prune_due(kv,ino);assert(due>=0);
 if(!due)return;
 unsigned removed=0;struct efs_txid t=next_tx();
 int rc=efs_meta_prune_append(kv,ino,&t,&ops,kv,&removed);
 TR("prune rc=%d removed=%u\n",rc,removed);
 assert(rc==EFS_OK || (rc==EFS_ERR_BUSY && pending_any()));
 stats[10]+=removed;
}

/* Every applied reservation tiles [BASE, size) exactly. Returns 0 if not. */
static int tiles(const struct wr *v,unsigned n,uint64_t size)
{
 unsigned applied=0,walked=0;
 for(unsigned i=0;i<n;i++) {
  if(!v[i].applied)continue;
  applied++;
  for(unsigned j=0;j<i;j++)
   if(v[j].applied && v[i].off<v[j].off+v[j].len && v[j].off<v[i].off+v[i].len)return 0;
 }
 uint64_t at=BASE;
 for(int hit=1;hit;) {
  hit=0;
  for(unsigned i=0;i<n;i++)if(v[i].applied && v[i].off==at){at+=v[i].len;walked++;hit=1;break;}
 }
 return at==size && walked==applied;
}

static void settle(void)
{
 for(unsigned round=0;round<64;round++) {
  int busy=0;
  for(unsigned i=0;i<OWNERS*WRITES;i++)if(w[i].state==DECIDED||w[i].state==PREPARED){recover(&w[i]);busy=1;}
  for(unsigned i=0;i<OWNERS*WRITES;i++)if(w[i].state==LOST){resolve_lost(&w[i],pick(2));busy=1;}
  for(int o=0;o<OWNERS;o++)for(unsigned i=0;i<OWNERS*WRITES;i++)if(w[i].owner==o && w[i].state==GRANTED){report(o,1);busy=1;break;}
  if(!busy)return;
 }
 abort();
}

static void run(uint64_t seed)
{
 rng=seed*0x9E3779B97F4A7C15ULL+1;txn_seq=0;
 kv=efs_kv_mem_create();assert(kv);
 struct efs_meta_attrs a={1,1,1};struct efs_meta_row row;
 assert(efs_meta_apply_init(kv,1)==EFS_OK);
 assert(efs_meta_apply_create_file(kv,&a,EFS_ROOT_INO,S_IFREG|0644,"file",&ino)==EFS_OK);
 assert(efs_meta_apply_get_inode(kv,ino,&row)==EFS_OK);gen=row.generation;
 assert(efs_meta_apply_activate_lanes(kv,ino,3)==EFS_OK);
 struct efs_meta_pub p={0};p.ino=ino;p.inode_gen=gen;p.now=100;
 p.coding_profile_id=EFS_META_PROFILE_K2F1;
 for(unsigned i=0;i<EFS_NUM_FRAGMENTS;++i)p.ch.nodes[i]=i+1;
 p.chunk_index=0;p.new_size=100;p.candidate_gen=10;assert(efs_meta_apply_publish(kv,&p)==EFS_OK);
 p.chunk_index=1;p.new_size=BASE;p.candidate_gen=20;assert(efs_meta_apply_publish(kv,&p)==EFS_OK);
 memset(w,0,sizeof(w));
 for(int o=0;o<OWNERS;o++) {
  own[o]=(struct owner){(uint8_t)(42+o),1,0};
  uint8_t uuid[16]={own[o].uuid};
  assert(efs_session_establish(kv,efs_kv_inode_shard(ino),uuid,1)==EFS_OK);
 }
 for(unsigned i=0;i<OWNERS*WRITES;i++){w[i].owner=(int)(i%OWNERS);w[i].len=8+3*i;}
 for(unsigned step=0;step<120;step++) {
  unsigned e=pick(10);int o=(int)pick(OWNERS);
  if(e<3)issue(o);
  else if(e<5) {
   unsigned i=pick(OWNERS*WRITES);
   if(w[i].state==LOST)resolve_lost(&w[i],pick(3)==0);
   else if(w[i].state==DECIDED||w[i].state==PREPARED)recover(&w[i]);
  }
  else if(e<7)report(o,pick(2));
  else if(e==7 && !pending_any() && pick(6)==0)fence(o);
  else if(e==8)prune();
 }
 settle();
 for(unsigned i=0;i<OWNERS*WRITES;i++)
  assert(w[i].state==IDLE || w[i].state==COMPLETED || w[i].state==ABANDONED || w[i].state==FENCED);
 prune();
 uint64_t watermark=0,frontier=0;uint32_t open=9;
 int rc=efs_meta_apply_append_state(kv,ino,&watermark,&frontier,&open);
 assert((rc==EFS_OK || rc==EFS_ERR_NOT_FOUND) && (rc==EFS_ERR_NOT_FOUND || !open));
 struct efs_meta_stat st;assert(efs_meta_apply_getattr(kv,ino,decision,kv,&st)==EFS_OK);
 if(!tiles(w,OWNERS*WRITES,st.size)) {
  fprintf(stderr,"seed %llu: size %llu does not tile\n",(unsigned long long)seed,(unsigned long long)st.size);
  for(unsigned i=0;i<OWNERS*WRITES;i++)
   fprintf(stderr,"  w%u owner=%d state=%d applied=%d off=%llu len=%llu\n",i,w[i].owner,w[i].state,
           w[i].applied,(unsigned long long)w[i].off,(unsigned long long)w[i].len);
  abort();
 }
 /* Negative control: the checker refuses a dropped acknowledged record. */
 for(unsigned i=0;i<OWNERS*WRITES;i++)if(w[i].state==COMPLETED) {
  struct wr copy[OWNERS*WRITES];memcpy(copy,w,sizeof(copy));copy[i].applied=0;
  assert(!tiles(copy,OWNERS*WRITES,st.size));stats[11]++;break;
 }
 efs_kv_mem_free(kv);
}

int main(int argc,char **argv)
{
 unsigned seeds=argc>1?(unsigned)strtoul(argv[1],NULL,10):400;
 unsigned first=argc>2?(unsigned)strtoul(argv[2],NULL,10):1;
 trace=getenv("K9_TRACE")!=NULL;
 for(unsigned s=first;s<first+seeds;s++){TR("== seed %u\n",s);run(s);}
 printf("append interleave (K9/W87): %u seeds, busy=%lu granted=%lu lost=%lu replayed=%lu "
        "abandoned=%lu decided-inflight=%lu prepared-only=%lu recovered=%lu completed=%lu "
        "fenced=%lu pruned=%lu negative-controls=%lu PASS\n",seeds,stats[0],stats[1],stats[2],
        stats[3],stats[4],stats[5],stats[6],stats[7],stats[8],stats[9],stats[10],stats[11]);
 if(seeds>=100)assert(stats[1] && stats[2] && stats[3] && stats[4] && stats[5] && stats[6] && stats[8] &&
        stats[9] && stats[10] && stats[11]);
 return 0;
}
