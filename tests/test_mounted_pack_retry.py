#!/usr/bin/env python3
"""Compile the mounted scheduler against real pack codecs; fault its RPC edges."""
from pathlib import Path
import os, shlex, subprocess, tempfile
root=Path(__file__).resolve().parents[1]
s=(root/'src/client/pack_write.c').read_text()
code=s[:s.index('static uint64_t pack_now(')]+r'''
#include <assert.h>
#include <stdio.h>
#include <errno.h>
#include "efs/publication_ack.h"
#include "efs/erasure.h"
struct efs_client g_client;
static unsigned sends[3],publications[2],loads[2],retirements[2],released;
static unsigned commands,upload_retired,retire_inflight,retire_peak;
static int admit_fail,put_fail,publication_fail,load_fail,retire_fail,upload_fail;
static int terminal_reject,rejection_proven;static struct efs_publication_ack_queue queues[2];
static unsigned exercised[PACK_WRITERS],parallel_live,parallel_peak;static int thread_fail;
static int fault_create(pthread_t *thread,const pthread_attr_t *attr,void *(*worker)(void *),void *context)
{return thread_fail?EAGAIN:pthread_create(thread,attr,worker,context);}
static int exercise_one(void *context,uint32_t i)
{
 assert(context==exercised&&i<PACK_WRITERS&&efs_client_rpc_deadline_ms()==12345);
 assert(__atomic_fetch_add(&exercised[i],1,__ATOMIC_SEQ_CST)==0);
 unsigned live=__atomic_add_fetch(&parallel_live,1,__ATOMIC_SEQ_CST),peak=__atomic_load_n(&parallel_peak,__ATOMIC_SEQ_CST);
 while(live>peak&&!__atomic_compare_exchange_n(&parallel_peak,&peak,live,0,__ATOMIC_SEQ_CST,__ATOMIC_SEQ_CST)){}
 usleep(20000);__atomic_sub_fetch(&parallel_live,1,__ATOMIC_SEQ_CST);
 return i==11?EFS_ERR_NET:EFS_OK;
}
static uint32_t current_fi;static uint8_t digest[EFS_HASH_SIZE];static int have_digest;
static uint64_t pack_now(void){return 1000000000;}
static __thread uint64_t deadline;
uint64_t efs_client_rpc_deadline_ms(void){return deadline;}
void efs_client_rpc_set_deadline_ms(uint64_t v){deadline=v;}
int efs_client_rpc_past_deadline(void){return 0;}
static int pack_command_wire(uint32_t group,const struct efs_pack_object *object,const struct efs_pack_ref *refs,uint32_t count,uint8_t *wire,size_t capacity,uint8_t tag){
 (void)group;(void)refs;(void)wire;assert(object->ledger.owner.seq==17&&count==2&&capacity);
 assert(tag==EFS_MD_CMD_PACK_UPLOAD_RETIRE);
 if(upload_fail){upload_fail=0;return EFS_ERR_NET;}upload_retired++;return 0;
}
static int pack_command(struct pack_context *p,uint8_t tag){
 commands++;assert(p->cohort.sealed && p->cohort.owner.seq==17);
 if(tag==EFS_MD_CMD_PACK_ADMIT){if(admit_fail){admit_fail=0;return EFS_ERR_NET;}return 0;}
 assert(0);return EFS_ERR_PROTO;
}
struct efs_conn *efs_client_conn_get(efs_node_id_t n){assert(n>=1&&n<=4);return (struct efs_conn *)(uintptr_t)n;}
void efs_client_conn_release(efs_node_id_t n,struct efs_conn *c){assert((uintptr_t)c==n);}
void efs_client_conn_drop(efs_node_id_t n,struct efs_conn *c){assert((uintptr_t)c==n);}
int efs_conn_send_msg_parts(struct efs_conn *c,uint8_t t,const void *a,uint32_t an,const void *b,uint32_t bn){
 assert(t==EFS_MSG_PUT_PACKED);const uint8_t *h=a;current_fi=efs_put_upload_rd32(h+4);assert(current_fi<3);
 struct efs_pack_object o;struct efs_pack_ref refs[EFS_PACK_MAX_RECORDS];uint32_t n;
 assert(!efs_pack_upload_decode(h+EFS_PACKED_PUT_HEADER,an-EFS_PACKED_PUT_HEADER,&o,refs,EFS_PACK_MAX_RECORDS,&n));
 assert(n==2 && o.nodes[current_fi]==(uintptr_t)c && bn==o.ledger.geometry/2);
 uint8_t sum[EFS_HASH_SIZE];efs_hash(b,bn,sum);assert(!memcmp(sum,o.checksums[current_fi],sizeof(sum)));
 if(have_digest)assert(!memcmp(digest,o.ledger.body_digest,sizeof(digest)));
 else {memcpy(digest,o.ledger.body_digest,sizeof(digest));have_digest=1;}
 sends[current_fi]++;return 0;
}
int efs_conn_recv_msg(struct efs_conn *c,uint8_t *t,void **b,uint32_t *n){
 (void)c;*b=NULL;*n=0;if(current_fi==1 && put_fail){put_fail=0;return -1;}
 *t=EFS_MSG_PUT_PACKED_REPLY;*n=4;*b=calloc(1,4);assert(*b);return 0;
}
int efs_dcache_finish_publication(efs_ino_t ino,uint64_t g,uint32_t ci,uint64_t obj,uint64_t seq,int verdict,const struct efs_chunk_rec *r){
 assert(g==3 && !ci && obj && seq && !verdict && r->packed.ino==ino && (r->publish_flags&EFS_CHUNK_REC_F_PACKED_BASE));
 assert(sends[0] && sends[1]>=2 && sends[2]);return 0;
}
int efs_client_publication_writeback(struct efs_publication_ack_queue *q,efs_ino_t ino,uint64_t g,uint32_t ci,uint64_t obj,uint64_t seq,uint64_t size){
 assert(g==3&&!ci&&obj&&seq&&size==512);unsigned i=ino==101?0:1;publications[i]++;
 if(terminal_reject){assert(q==&queues[i]);q->count=1;q->entries[0].request.id.seq=seq;
  q->entries[0].consumed_state=rejection_proven?EFS_PUBLICATION_REJECTED:0;
  q->entries[0].consumed_verdict=EFS_ERR_STALE;return EFS_ERR_STALE;}
 if(i==1 && publication_fail){publication_fail=0;return EFS_ERR_NET;}return 0;
}
int efs_client_pull_chunks_range(efs_ino_t ino,uint32_t a,uint32_t b){
 assert(!a&&b==1);unsigned i=ino==101?0:1;loads[i]++;
 if(i==1&&load_fail){load_fail=0;return EFS_ERR_NET;}return 0;
}
void efs_dcache_drop(efs_ino_t ino,uint32_t ci){assert((ino==101||ino==103)&&!ci);}
int efs_client_publication_retire(struct efs_publication_ack_queue *q){
 unsigned i=q==&queues[0]?0:1;assert(q==&queues[i]);retirements[i]++;
 unsigned live=__atomic_add_fetch(&retire_inflight,1,__ATOMIC_SEQ_CST),peak=__atomic_load_n(&retire_peak,__ATOMIC_SEQ_CST);
 while(live>peak&&!__atomic_compare_exchange_n(&retire_peak,&peak,live,0,__ATOMIC_SEQ_CST,__ATOMIC_SEQ_CST)){}
 usleep(20000);__atomic_sub_fetch(&retire_inflight,1,__ATOMIC_SEQ_CST);
 if(i==1&&retire_fail){retire_fail=0;return EFS_ERR_NET;}return 0;
}
int efs_client_publication_ack_free(struct efs_publication_ack_queue *q){assert(q);__atomic_fetch_add(&released,1,__ATOMIC_RELAXED);return 0;}
'''+s[s.index('struct pack_parallel_run {'):s.index('static int pack_capabilities(')].replace('pthread_create(', 'fault_create(')+s[s.index('static int pack_publish_record('):s.index('static void *pack_worker(')]+r'''
int main(void){
 static struct pack_context p;g_client.node_count=4;
 for(unsigned i=0;i<4;i++)g_client.nodes[i].id=i+1;
 assert(!efs_pack_cohort_init(&p.cohort,0,16,1000000));p.members=15;
 p.cohort.owner=(struct efs_opid){.client_uuid={19},.session_epoch=7,.seq=17};
 uint8_t body[512];memset(body,59,sizeof(body));
 for(unsigned i=0;i<2;i++){
  struct efs_pack_record r={101+2*i,3,0,23+i,512,body};
  assert(!efs_pack_cohort_offer(&p.cohort,&r,1+i,pack_now()));
  p.writers[i].plan=(struct efs_writer_plan){.ino=r.ino,.generation=3,.cache_sequence=1+i};
  p.writers[i].object=r.record_generation;p.writers[i].len=512;
  p.writers[i].acks=&queues[i];
 }
 static struct pack_retirement retirement;pack_groups[0]=&p;pack_retirements[0]=&retirement;pack_ack_credits=2;
 assert(!pack_flush_group(0,0)&&!commands); /* partial cohort waits only its bound */
 p.busy=1;assert(pack_flush_group(0,1)==EFS_ERR_BUSY&&!commands);p.busy=0;
 admit_fail=put_fail=publication_fail=load_fail=retire_fail=upload_fail=1;
 assert(pack_flush_group(0,1)==EFS_ERR_NET && p.built && p.cohort.count==2 && !released);
 assert(pack_flush_group(0,1)==EFS_ERR_NET && p.put_mask==1 && !released);
 assert(pack_flush_group(0,1)==EFS_ERR_NET && p.put_mask==7 && p.writers[0].done && !p.writers[1].done && !released);
 assert(pack_flush_group(0,1)==EFS_ERR_NET && p.writers[1].published && !p.writers[1].loaded);
 assert(!pack_flush_group(0,1) && !p.cohort.count && retirement.active && !released && pack_ack_credits==2);
 p.cohort.count=2;p.cohort.sealed=1;p.built=1;p.put_mask=7;
 p.writers[0].done=p.writers[1].done=1;
 assert(pack_flush_group(0,1)==EFS_ERR_BUSY && p.cohort.count==2 && retirement.active && !released);
 p.cohort.count=0;
 /* Close is complete, but exact ACKs/manifest still survive all cleanup errors.
  * The data context is reusable; its cleanup slot cannot be overwritten. */
 p.busy=1;assert(pack_retire_group(0)==EFS_ERR_BUSY&&!released);p.busy=0;
 p.stack_busy=1;assert(pack_retire_group(0)==EFS_ERR_BUSY&&!released);
 p.cohort.count=2;assert(pack_flush_group(0,1)==EFS_ERR_BUSY&&!released);p.cohort.count=0;p.stack_busy=0;
 assert(pack_retire_group(0)==EFS_ERR_NET && retirement.active && released==1 && pack_ack_credits==1);
 assert(retire_peak==2&&!retire_inflight&&!p.stack_busy&&!retirement.busy);
 assert(pack_retire_group(0)==EFS_ERR_NET && retirement.active && released==2 && !pack_ack_credits && !upload_retired);
 assert(!pack_retire_group(0) && !retirement.active && upload_retired==1);
 assert(!pack_retire_group(0) && upload_retired==1);
 assert(sends[0]==1&&sends[1]==2&&sends[2]==1); /* no re-PUT after publication */
 assert(publications[0]==1&&publications[1]==2 && loads[0]==1&&loads[1]==2);
 assert(retirements[0]==1&&retirements[1]==2);
 efs_client_rpc_set_deadline_ms(12345);
 assert(pack_parallel(&p,PACK_WRITERS,exercise_one,exercised)==EFS_ERR_NET);
 for(unsigned i=0;i<PACK_WRITERS;i++)assert(exercised[i]==1);
 assert(parallel_peak==PACK_PUBLISH_WORKERS&&!parallel_live&&efs_client_rpc_deadline_ms()==12345);
 memset(exercised,0,sizeof(exercised));parallel_peak=0;thread_fail=1;
 assert(pack_parallel(&p,PACK_WRITERS,exercise_one,exercised)==EFS_ERR_NET);
 for(unsigned i=0;i<PACK_WRITERS;i++)assert(exercised[i]==1);
 assert(parallel_peak==1&&!parallel_live&&efs_client_rpc_deadline_ms()==12345);
 thread_fail=0;efs_client_rpc_set_deadline_ms(0);
 /* A transport/session STALE without exact consumed evidence retains packing.
  * A proven terminal rejection hands off only to the still-owned ordinary
  * dirty cache; it never loads/drops data or discards the rejected ACK. */
 memset(p.writers,0,sizeof(p.writers));p.writers[0].plan=(struct efs_writer_plan){.ino=101,.generation=3,.cache_sequence=91};
 p.writers[0].object=23;p.writers[0].len=512;p.writers[0].put_bound=1;p.writers[0].acks=&queues[0];
 terminal_reject=1;unsigned before_loads=loads[0];
 assert(pack_publish_record(&p,0)==EFS_ERR_STALE&&!p.writers[0].done);
 rejection_proven=1;assert(!pack_publish_record(&p,0)&&p.writers[0].done);
 assert(!p.writers[0].published&&!p.writers[0].loaded&&loads[0]==before_loads&&p.writers[0].acks==&queues[0]);
 puts("mounted pack retry: bounded partial cohort, immutable lost-ACK retry, per-record receipt/load retention, bounded deferred retirement and no re-PUT after publication PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='efs-mounted-pack-') as d:
 p=Path(d)/'t.c';p.write_text(code);exe=p.with_suffix('')
 subprocess.run(shlex.split(os.environ.get('CC','cc'))+['-std=gnu11','-Wall','-Wextra','-Werror','-Wno-unused-variable','-pthread','-I'+str(root/'include'),'-I'+str(root/'src/client'),str(p),str(root/'libefs.a'),'-lm','-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)
