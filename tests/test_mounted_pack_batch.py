#!/usr/bin/env python3
"""Production scheduler batching keeps exact ACK ownership across partial/lost replies."""
from pathlib import Path
import os,shlex,subprocess,tempfile
root=Path(__file__).resolve().parents[1];s=(root/'src/client/pack_write.c').read_text()
def fn(name):
 a=s.index('static '+name);return s[a:s.index('\n}',a)+2]
code=s[:s.index('static uint64_t pack_now(')]+r'''
#include <assert.h>
#include <stdio.h>
#include "efs/publication.h"
static struct efs_publication_ack_queue queues[PACK_WRITERS];
static struct efs_chunk_rec bound[PACK_WRITERS];
static int owned[PACK_WRITERS],lost,partial,retire_partial,unsupported;
static unsigned calls,consumes,binds,freed;static uint32_t last_count;
static struct efs_msg_publication initial[PACK_WRITERS];
static unsigned slot(efs_ino_t ino){for(unsigned i=0;i<PACK_WRITERS;i++)if(bound[i].ino==ino)return i;assert(0);return 0;}
int efs_dcache_finish_publication(efs_ino_t ino,uint64_t g,uint32_t ci,uint64_t object,uint64_t seq,int verdict,const struct efs_chunk_rec *put)
{assert(g==33&&!ci&&object&&seq&&!verdict);unsigned i=slot(ino);assert(!bound[i].chunk_generation);bound[i]=*put;bound[i].file_generation=g;bound[i].publish_flags|=EFS_CHUNK_REC_F_CAPTURED_FILEID|EFS_CHUNK_REC_F_CAPTURED_EPOCH;binds++;return 0;}
int efs_dcache_publication_request(efs_ino_t ino,uint64_t g,uint32_t ci,uint64_t o,uint64_t seq,uint64_t size,struct efs_msg_publication *r)
{assert(g==33&&!ci&&size==512&&bound[slot(ino)].chunk_generation==o);*r=(struct efs_msg_publication){.rec=bound[slot(ino)],.size=size,.id={.client_uuid={1},.session_epoch=3,.seq=seq}};return 0;}
int efs_dcache_consume_publication(efs_ino_t ino,uint64_t g,uint32_t ci,uint64_t o,uint64_t seq,const struct efs_msg_publication_reply *r,int *used)
{
 assert(g==33&&!ci&&o&&seq);unsigned i=slot(ino);struct efs_msg_publication req;assert(!efs_publication_ack_request(&queues[i],seq,&req));
 struct efs_meta_pub pub;uint8_t digest[32];assert(!efs_publication_from_request(&req,&pub)&&!efs_publication_digest(&pub,digest));assert(!memcmp(digest,r->digest,32)&&owned[i]==1);
 consumes++;*used=1;if(r->state==EFS_PUBLICATION_COMMITTED)owned[i]=0; /* Reject retains dirty bytes. */
 return r->verdict;
}
int efs_client_publication_ack_free(struct efs_publication_ack_queue *q){assert(q&&!q->count);freed++;return 0;}
int efs_client_rpc_publication_batch(const struct efs_msg_publication_batch *b,struct efs_msg_publication_batch_reply *r)
{
 calls++;last_count=b->count;assert(b->count&&b->count<=16&&b->group==0);
 if(unsupported)return EFS_ERR_PROTO;
 *r=(struct efs_msg_publication_batch_reply){.version=b->version,.mode=b->mode,.group=b->group,.count=b->count};
 for(unsigned j=0;j<b->count;j++) {
  unsigned i=slot(b->records[j].rec.ino);
  if(!initial[i].rec.ino)initial[i]=b->records[j];else assert(!memcmp(&initial[i],&b->records[j],sizeof(initial[i])));
  struct efs_meta_pub pub;assert(!efs_publication_from_request(&b->records[j],&pub));
  struct efs_msg_publication_reply *out=&r->records[j];assert(!efs_publication_digest(&pub,out->digest));
  out->state=b->mode==2?EFS_PUBLICATION_RETIRED:EFS_PUBLICATION_COMMITTED;out->verdict=b->mode==2?EFS_META_PUBLICATION_RETIRED:0;
  if(b->mode==0&&partial&&i==1){out->rpc.status=EFS_INODE_RPC_BUSY;out->state=0;}
  if(b->mode==0&&partial&&i==2){out->state=EFS_PUBLICATION_REJECTED;out->verdict=EFS_ERR_STALE;}
  if(b->mode==2&&retire_partial&&i==1){out->rpc.status=EFS_INODE_RPC_BUSY;out->state=0;}
 }
 return lost?EFS_ERR_NET:0;
}
'''+fn('int pack_batch_status(')+'\n'+fn('int pack_retire_batch(')+'\n'+fn('void pack_publish_batch(')+r'''
int main(void)
{
 static struct pack_context p;static struct pack_retirement r;
 p.batch=1;p.cohort.count=16;p.cohort.group=0;r.count=16;r.group=0;r.batch=1;
 efs_ino_t ino=1;
 for(unsigned i=0;i<16;i++) {
  while(efs_raft_shard_group(efs_kv_lane_shard(ino,0))!=0)ino++;
  struct pack_writer *w=&p.writers[i];bound[i].ino=ino;owned[i]=1;
  w->plan=(struct efs_writer_plan){.ino=ino++,.generation=33,.cache_sequence=i+1};w->object=900+i;w->len=512;w->acks=&queues[i];r.acks[i]=w->acks;
  p.cohort.refs[i]=(struct efs_pack_ref){.ino=w->plan.ino,.inode_generation=33,.record_generation=w->object,.segment_id=77,.segment_generation=88,.geometry=65536,.offset=2048+i*600,.len=512};
  for(unsigned j=0;j<3;j++)p.object.nodes[j]=j+1;
 }
 pack_ack_credits=16;lost=1;pack_publish_batch(&p);assert(calls==1&&last_count==16&&binds==16&&!consumes);
 for(unsigned i=0;i<16;i++)assert(p.batch_results[i]==EFS_ERR_NET&&queues[i].count==1&&!queues[i].entries[0].consumed_state&&owned[i]);
 lost=0;partial=1;pack_publish_batch(&p);assert(calls==2&&last_count==16&&binds==16&&consumes==15);
 for(unsigned i=0;i<16;i++)assert(p.batch_results[i]==(i==1?EFS_ERR_BUSY:i==2?EFS_ERR_STALE:0)&&owned[i]==(i==1||i==2));
 partial=0;pack_publish_batch(&p);assert(calls==3&&last_count==1&&consumes==16&&!owned[1]&&owned[2]);
 /* Committed and rejected outcomes are consumed exactly once; accepted reject
  * bytes remain owned for ordinary rebase while its immutable ACK survives. */
 pack_publish_batch(&p);assert(calls==3&&consumes==16&&owned[2]&&pack_ack_credits==16);
 lost=1;assert(pack_retire_batch(&r)==EFS_ERR_NET&&!freed&&pack_ack_credits==16);
 lost=0;retire_partial=1;assert(pack_retire_batch(&r)==EFS_ERR_BUSY&&freed==15&&pack_ack_credits==1&&r.acks[1]);
 for(unsigned i=0;i<16;i++)assert(!!r.acks[i]==(i==1));
 retire_partial=0;assert(!pack_retire_batch(&r)&&last_count==1&&freed==16&&!pack_ack_credits&&owned[2]);
 assert(!pack_retire_batch(&r)&&freed==16);
 /* Definitive unsupported transport enables exact scalar replay, without
  * pretending a terminal outcome or changing byte/ACK ownership. */
 memset(initial,0,sizeof(initial));memset(queues,0,sizeof(queues));
 p.writers[0].put_bound=1;p.cohort.count=1;p.writers[0].plan.cache_sequence=99;owned[0]=1;p.batch=1;unsupported=1;
 pack_publish_batch(&p);assert(!p.batch&&owned[0]&&queues[0].count==1&&!queues[0].entries[0].consumed_state);
 puts("mounted batch: 16 exact intents, no rebind after lost reply, independent rejection/BUSY retention, once-only consumption, oldest ACK retry/partial credits, scalar fallback and retained rejected bytes PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='efs-mounted-batch-') as d:
 p=Path(d)/'test.c';p.write_text(code);exe=p.with_suffix('')
 subprocess.run(shlex.split(os.environ.get('CC','cc'))+['-std=gnu11','-Wall','-Wextra','-Werror','-Wno-unused-variable','-pthread','-I'+str(root/'include'),'-I'+str(root/'src/client'),str(p),str(root/'libefs.a'),'-lm','-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)
