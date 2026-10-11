#!/usr/bin/env python3
"""Production batch RPC: validate every exact result before exposing any proof."""
from pathlib import Path
import os, shlex, subprocess, tempfile
root=Path(__file__).resolve().parents[1]
s=(root/'src/client/inode_rpc.c').read_text();a=s.index('int efs_client_rpc_publication_batch(')
f=s[a:s.index('\n}',a)+2]
source=r'''
#include "efs/publication.h"
#include "efs/kv_key.h"
#include "efs/raft.h"
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
int efs_segment_geometry(size_t n,size_t *out){(void)n;(void)out;return EFS_ERR_INVAL;}
int efs_chunk_size_valid(uint32_t n){return n==EFS_MIN_CHUNK_SIZE;}
struct efs_conn {int unused;};static struct efs_conn conn;
static struct efs_msg_publication_batch request,sent;
static unsigned calls,pauses,drops;static int corrupt,lost,expired,redirect,busy,partial;
int efs_client_rpc_past_deadline(void){return expired;}
static int rpc_writer_retry_pause(unsigned n){(void)n;pauses++;return 0;}
static struct efs_conn *efs_client_conn_get(efs_node_id_t n){assert(n==2);return &conn;}
static struct efs_conn *rpc_owner_conn_shard(uint32_t shard,efs_node_id_t *n)
{assert(shard==efs_kv_lane_shard(request.records[0].rec.ino,0));*n=1;return &conn;}
static void efs_client_conn_drop(efs_node_id_t n,struct efs_conn *c){(void)n;assert(c==&conn);drops++;}
static void efs_client_conn_release(efs_node_id_t n,struct efs_conn *c){(void)n;assert(c==&conn);}
static int rpc_status_to_efs(uint8_t s){switch(s){case EFS_INODE_RPC_OK:return 0;case EFS_INODE_RPC_BUSY:return EFS_ERR_BUSY;case EFS_INODE_RPC_INVAL:return EFS_ERR_INVAL;default:return EFS_ERR_IO;}}
int efs_conn_send_msg(struct efs_conn *c,uint8_t t,const void *p,uint32_t bytes)
{assert(c==&conn&&t==EFS_MSG_PUBLICATION_BATCH&&bytes==efs_publication_batch_bytes(request.count));memset(&sent,0,sizeof(sent));memcpy(&sent,p,bytes);assert(!memcmp(&sent,&request,bytes));return 0;}
int efs_conn_recv_msg(struct efs_conn *c,uint8_t *t,void **p,uint32_t *n)
{
 assert(c==&conn);calls++;*t=EFS_MSG_PUBLICATION_BATCH_REPLY;
 struct efs_msg_publication_batch_reply r={.version=sent.version,.mode=sent.mode,.group=sent.group,.count=sent.count};
 for(uint32_t i=0;i<sent.count;i++) {
  struct efs_meta_pub pub;assert(!efs_publication_from_request(&sent.records[i],&pub));
  assert(!efs_publication_digest(&pub,r.records[i].digest));
  r.records[i].state=sent.mode==2?EFS_PUBLICATION_RETIRED:EFS_PUBLICATION_COMMITTED;
  r.records[i].verdict=sent.mode==2?EFS_META_PUBLICATION_RETIRED:0;
  r.records[i].receipt_group=sent.group;r.records[i].receipt_index=100+i;r.records[i].receipt_term=7;
 }
 if(partial){r.records[0].state=EFS_PUBLICATION_REJECTED;r.records[0].verdict=EFS_ERR_STALE;
  r.records[1].rpc.status=EFS_INODE_RPC_BUSY;r.records[1].state=0;
  r.records[1].receipt_group=r.records[1].receipt_index=r.records[1].receipt_term=0;}
 if(redirect&&calls==1){r.rpc.status=EFS_INODE_RPC_NOT_PRIMARY;r.rpc.primary_id=2;}
 if(busy){r.rpc.status=EFS_INODE_RPC_BUSY;}
 *n=(uint32_t)efs_publication_batch_reply_bytes(sent.count);
 switch(corrupt) {
 case 1:r.count--;break;case 2:(*n)--;break;case 3:r.version++;break;
 case 4:r.records[sent.count-1].digest[0]^=1;break;
 case 5:r.records[sent.count-1].receipt_term=0;break;
 case 6:r.records[sent.count-1].receipt_group^=2;break;
 case 7:r.records[sent.count-1].state=EFS_PUBLICATION_UNKNOWN;break;
 case 8:r.records[sent.count-1].verdict=EFS_ERR_STALE;break;
 case 9:r.records[sent.count-1].rpc.status=EFS_INODE_RPC_BUSY;break;
 case 10:r.mode^=2;break;case 11:r.group^=2;break;
 case 12:*t=EFS_MSG_PUBLICATION_REPLY;break;
 }
 *p=malloc(sizeof(r));assert(*p);memcpy(*p,&r,sizeof(r));return lost;
}
'''+f+r'''
static void reset(void){calls=pauses=drops=0;corrupt=lost=expired=redirect=busy=partial=0;}
int main(void)
{
 request.version=EFS_PUBLICATION_BATCH_VERSION;request.count=EFS_PUBLICATION_BATCH_MAX;
 efs_ino_t ino=1;
 for(uint32_t i=0;i<request.count;i++) {
  while(efs_raft_shard_group(efs_kv_lane_shard(ino,0))!=request.group)ino++;
  struct efs_msg_publication *r=&request.records[i];
  r->rec.ino=ino++;r->rec.file_generation=33;r->rec.chunk_generation=900+i;
  r->rec.publish_flags=EFS_CHUNK_REC_F_CAPTURED_FILEID|EFS_CHUNK_REC_F_CAPTURED_EPOCH|EFS_CHUNK_REC_F_FRESH_OBJECT|EFS_CHUNK_REC_F_PACKED_BASE;
  r->size=512;r->id=(struct efs_opid){.client_uuid={1},.session_epoch=3,.seq=i+1};
  r->rec.packed=(struct efs_pack_ref){.ino=r->rec.ino,.inode_generation=33,.record_generation=r->rec.chunk_generation,
    .segment_id=77,.segment_generation=88,.geometry=65536,.offset=2048+i*600,.len=512,.publication_group=request.group};
  for(unsigned j=0;j<3;j++)r->rec.nodes[j]=j+1;
 }
 struct efs_msg_publication_batch_reply out;
 reset();assert(!efs_client_rpc_publication_batch(&request,&out)&&calls==1&&out.count==16);
 reset();partial=1;assert(!efs_client_rpc_publication_batch(&request,&out)&&out.records[0].state==EFS_PUBLICATION_REJECTED&&out.records[1].rpc.status==EFS_INODE_RPC_BUSY&&out.records[15].state==EFS_PUBLICATION_COMMITTED);
 for(corrupt=1;corrupt<=12;corrupt++){calls=0;partial=0;assert(efs_client_rpc_publication_batch(&request,&out)==EFS_ERR_PROTO&&!out.count);}
 reset();redirect=1;assert(!efs_client_rpc_publication_batch(&request,&out)&&calls==2);
 reset();busy=1;assert(efs_client_rpc_publication_batch(&request,&out)==EFS_ERR_BUSY&&calls==8&&pauses==8&&!out.count);
 reset();lost=1;assert(efs_client_rpc_publication_batch(&request,&out)==EFS_ERR_NET&&drops==1&&!out.count);
 reset();expired=1;assert(efs_client_rpc_publication_batch(&request,&out)==EFS_ERR_BUSY&&!calls&&!out.count);
 reset();request.mode=2;assert(!efs_client_rpc_publication_batch(&request,&out)&&out.records[15].state==EFS_PUBLICATION_RETIRED);
 request.mode=0;request.count=17;assert(efs_client_rpc_publication_batch(&request,&out)==EFS_ERR_INVAL);
 request.count=16;request.records[15]=request.records[0];assert(efs_client_rpc_publication_batch(&request,&out)==EFS_ERR_INVAL);
 request.records[15].rec.ino++;assert(efs_client_rpc_publication_batch(&request,&out)==EFS_ERR_INVAL);
 puts("publication batch RPC: 16-record bound, exact frame/digests/locators, partial verdicts, no omitted success, redirect/deadline/lost reply, retirement and invalid stream rejection PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='efs-publication-batch-rpc-') as d:
 p=Path(d)/'test.c';p.write_text(source);exe=p.with_suffix('')
 cmd=shlex.split(os.environ.get('CC','cc'))+['-std=gnu11','-Wall','-Wextra','-Werror','-I'+str(root/'include')]
 cmd+=shlex.split(os.environ.get('WRITER_RPC_TEST_CFLAGS',''))
 files=['src/kv/kv_key.c','src/common/publication.c','src/common/packed_segment.c','src/common/checksum.c','deps/blake3/blake3.c','deps/blake3/blake3_dispatch.c','deps/blake3/blake3_portable.c']
 subprocess.run(cmd+[str(p)]+[str(root/f) for f in files]+['-I'+str(root/'deps/blake3'),'-DBLAKE3_NO_SSE2','-DBLAKE3_NO_SSE41','-DBLAKE3_NO_AVX2','-DBLAKE3_NO_AVX512','-DBLAKE3_USE_NEON=0','-pthread','-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)
