#!/usr/bin/env python3
"""Compile the mounted scheduler against real pack codecs; fault its RPC edges."""
from pathlib import Path
import os, shlex, subprocess, tempfile
root=Path(__file__).resolve().parents[1]
s=(root/'src/client/pack_write.c').read_text()
code=s[:s.index('static uint64_t pack_now(')]+r'''
#include <assert.h>
#include <stdio.h>
#include "efs/publication_ack.h"
#include "efs/erasure.h"
struct efs_client g_client;
static unsigned sends[3],publications[2],loads[2],retirements[2],released;
static unsigned commands,upload_retired;
static int admit_fail,put_fail,publication_fail,load_fail,retire_fail,upload_fail;
static uint32_t current_fi;static uint8_t digest[EFS_HASH_SIZE];static int have_digest;
static uint64_t pack_now(void){return 1000000000;}
int efs_client_rpc_past_deadline(void){return 0;}
static int pack_command(struct pack_context *p,uint8_t tag){
 commands++;assert(p->cohort.sealed && p->cohort.owner.seq==17);
 if(tag==EFS_MD_CMD_PACK_ADMIT){if(admit_fail){admit_fail=0;return EFS_ERR_NET;}return 0;}
 assert(tag==EFS_MD_CMD_PACK_UPLOAD_RETIRE && p->writers[0].done && p->writers[1].done);
 if(upload_fail){upload_fail=0;return EFS_ERR_NET;}upload_retired++;return 0;
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
 (void)q;assert(g==3&&!ci&&obj&&seq&&size==512);unsigned i=ino==101?0:1;publications[i]++;
 if(i==1 && publication_fail){publication_fail=0;return EFS_ERR_NET;}return 0;
}
int efs_client_pull_chunks_range(efs_ino_t ino,uint32_t a,uint32_t b){
 assert(!a&&b==1);unsigned i=ino==101?0:1;loads[i]++;
 if(i==1&&load_fail){load_fail=0;return EFS_ERR_NET;}return 0;
}
void efs_dcache_drop(efs_ino_t ino,uint32_t ci){assert((ino==101||ino==103)&&!ci);}
int efs_client_publication_retire(struct efs_publication_ack_queue *q){
 unsigned i=(unsigned)(uintptr_t)q-1;assert(i<2);retirements[i]++;
 if(i==1&&retire_fail){retire_fail=0;return EFS_ERR_NET;}return 0;
}
int efs_client_publication_ack_free(struct efs_publication_ack_queue *q){assert(q);released++;return 0;}
'''+s[s.index('static int pack_progress('):s.index('static void *pack_worker(')]+r'''
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
  p.writers[i].acks=(struct efs_publication_ack_queue *)(uintptr_t)(i+1);
 }
 pack_groups[0]=&p;
 assert(!pack_flush_group(0,0)&&!commands); /* partial cohort waits only its bound */
 p.busy=1;assert(pack_flush_group(0,1)==EFS_ERR_BUSY&&!commands);p.busy=0;
 admit_fail=put_fail=publication_fail=load_fail=retire_fail=upload_fail=1;
 assert(pack_flush_group(0,1)==EFS_ERR_NET && p.built && p.cohort.count==2 && !released);
 assert(pack_flush_group(0,1)==EFS_ERR_NET && p.put_mask==1 && !released);
 assert(pack_flush_group(0,1)==EFS_ERR_NET && p.put_mask==7 && p.writers[0].done && !p.writers[1].done && released==1);
 assert(pack_flush_group(0,1)==EFS_ERR_NET && p.writers[1].published && !p.writers[1].loaded);
 assert(pack_flush_group(0,1)==EFS_ERR_NET && p.writers[1].loaded && !p.writers[1].done);
 assert(pack_flush_group(0,1)==EFS_ERR_NET && p.writers[1].done && p.cohort.count==2 && released==2);
 assert(!pack_flush_group(0,1) && !p.cohort.count && upload_retired==1);
 assert(sends[0]==1&&sends[1]==2&&sends[2]==1); /* no re-PUT after publication */
 assert(publications[0]==1&&publications[1]==2 && loads[0]==1&&loads[1]==2);
 assert(retirements[0]==1&&retirements[1]==2);
 puts("mounted pack retry: bounded partial cohort, immutable lost-ACK retry, per-record receipt/load/retirement retention and no re-PUT after publication PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='efs-mounted-pack-') as d:
 p=Path(d)/'t.c';p.write_text(code);exe=p.with_suffix('')
 subprocess.run(shlex.split(os.environ.get('CC','cc'))+['-std=gnu11','-Wall','-Wextra','-Werror','-Wno-unused-variable','-pthread','-I'+str(root/'include'),'-I'+str(root/'src/client'),str(p),str(root/'libefs.a'),'-lm','-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)
