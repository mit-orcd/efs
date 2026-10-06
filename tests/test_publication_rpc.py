#!/usr/bin/env python3
"""Execute the production publication RPC recovery boundary with faulted replies."""
from pathlib import Path
import os, shlex, subprocess, tempfile
root=Path(__file__).resolve().parents[1]
text=(root/'src/client/inode_rpc.c').read_text()
start=text.index('int efs_client_rpc_publication(')
function=text[start:text.index('\n}',start)+2]
a=text.index('int efs_client_publication_id(')
identity=text[a:text.index('\n}',a)+2]
source=r'''
#include "efs/publication.h"
#include "efs/kv_key.h"
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
static pthread_mutex_t opid_mu=PTHREAD_MUTEX_INITIALIZER;
static struct {uint8_t opid_uuid[EFS_OPID_UUID_LEN];uint32_t opid_epoch;} g_client;
static void opid_seed_locked(void){g_client.opid_uuid[0]=1;g_client.opid_epoch=1;}
''' + identity + r'''
struct efs_conn { int unused; };
static struct efs_conn conn;
static struct efs_msg_publication_reply replies[8];
static struct efs_chunk_rec rec;
static struct efs_msg_publication request;
static unsigned calls,sleeps,drops,releases,targets;
static int fail_send,fail_recv,bad_type,bad_length,expired,query;
static efs_node_id_t last_target;
int efs_chunk_size_valid(uint32_t s){return s==EFS_MIN_CHUNK_SIZE;}
int efs_client_rpc_past_deadline(void){return expired;}
static int rpc_writer_retry_pause(unsigned n){(void)n;sleeps++;return EFS_OK;}
static struct efs_conn *efs_client_conn_get(efs_node_id_t n){last_target=n;targets++;return &conn;}
static struct efs_conn *rpc_owner_conn_shard(uint32_t s,efs_node_id_t *n)
{assert(s==efs_kv_lane_shard(123,17));*n=1;return &conn;}
static void efs_client_conn_drop(efs_node_id_t n,struct efs_conn *c){(void)n;(void)c;drops++;}
static void efs_client_conn_release(efs_node_id_t n,struct efs_conn *c){(void)n;(void)c;releases++;}
int efs_conn_send_msg(struct efs_conn *c,uint8_t t,const void *p,uint32_t n)
{
 (void)c;assert(t==(query?EFS_MSG_PUBLICATION_STATUS:EFS_MSG_PUBLICATION));
 assert(n==sizeof(struct efs_msg_publication));const struct efs_msg_publication *r=p;
 assert(!memcmp(&r->rec,&rec,sizeof(rec))&&r->size==100);return fail_send;
}
int efs_conn_recv_msg(struct efs_conn *c,uint8_t *t,void **p,uint32_t *n)
{
 (void)c;*t=bad_type?EFS_MSG_VERSION_REPLY:(query?EFS_MSG_PUBLICATION_STATUS_REPLY:EFS_MSG_PUBLICATION_REPLY);
 *n=sizeof(replies[0])-bad_length;*p=malloc(sizeof(replies[0]));assert(*p);
 memcpy(*p,&replies[calls++],sizeof(replies[0]));return fail_recv;
}
static int rpc_status_to_efs(uint8_t s)
{switch(s){case EFS_INODE_RPC_OK:return EFS_OK;case EFS_INODE_RPC_BUSY:return EFS_ERR_BUSY;
case EFS_INODE_RPC_NOT_FOUND:return EFS_ERR_NOT_FOUND;default:return EFS_ERR_PROTO;}}
''' + function + r'''
static void reset(void)
{
 calls=sleeps=drops=releases=targets=0;fail_send=fail_recv=bad_type=bad_length=expired=query=0;last_target=0;
 struct efs_meta_pub p;assert(efs_publication_from_rec(&rec,100,&p)==EFS_OK);p.publication_id=request.id;
 for(unsigned i=0;i<8;i++) {memset(&replies[i],0,sizeof(replies[i]));replies[i].state=EFS_PUBLICATION_COMMITTED;
 assert(efs_publication_digest(&p,replies[i].digest)==EFS_OK);}
}
int main(void)
{
 struct efs_opid id,expected={0};memset(&id,0xa5,sizeof(id));
 expected.client_uuid[0]=1;expected.session_epoch=1;expected.seq=77;
 assert(efs_client_publication_id(77,&id)==EFS_OK && !memcmp(&id,&expected,sizeof(id)));
 assert(efs_client_publication_id(0,&id)==EFS_ERR_INVAL);
 rec.ino=123;rec.file_generation=456;rec.chunk_index=17;rec.chunk_generation=789;
 rec.publish_flags=EFS_CHUNK_REC_F_CAPTURED_FILEID|EFS_CHUNK_REC_F_CAPTURED_EPOCH;
 for(unsigned i=0;i<EFS_NUM_FRAGMENTS;i++)rec.nodes[i]=i+1;
 request.rec=rec;request.size=100;request.id.client_uuid[0]=1;request.id.session_epoch=1;request.id.seq=77;
 struct efs_msg_publication_reply out;
 reset();replies[0].rpc.status=EFS_INODE_RPC_NOT_PRIMARY;replies[0].rpc.primary_id=2;
 replies[1].rpc.status=EFS_INODE_RPC_BUSY;
 assert(efs_client_rpc_publication(&request,0,&out)==EFS_OK && out.state==EFS_PUBLICATION_COMMITTED);
 assert(calls==3 && sleeps==1 && last_target==2 && releases==3 && !drops);
 reset();query=1;replies[0].state=EFS_PUBLICATION_REJECTED;replies[0].verdict=EFS_ERR_STALE;
 assert(efs_client_rpc_publication(&request,1,&out)==EFS_OK && out.state==EFS_PUBLICATION_REJECTED);
 reset();query=1;replies[0].rpc.status=EFS_INODE_RPC_NOT_FOUND;
 assert(efs_client_rpc_publication(&request,1,&out)==EFS_ERR_NOT_FOUND && out.state==EFS_PUBLICATION_UNKNOWN);
 for(unsigned i=0;i<5;i++) {
  reset();
  if(i==0)replies[0].digest[0]^=1;
  if(i==1)replies[0].state=EFS_PUBLICATION_UNKNOWN;
  if(i==2)replies[0].verdict=EFS_ERR_STALE;
  if(i==3)bad_type=1;
  if(i==4)bad_length=1;
  assert(efs_client_rpc_publication(&request,0,&out)==EFS_ERR_PROTO && out.state==EFS_PUBLICATION_UNKNOWN);
 }
 reset();for(unsigned i=0;i<8;i++)replies[i].rpc.status=EFS_INODE_RPC_BUSY;
 assert(efs_client_rpc_publication(&request,0,&out)==EFS_ERR_BUSY && calls==8 && out.state==0);
 reset();fail_send=1;assert(efs_client_rpc_publication(&request,0,&out)==EFS_ERR_NET && drops==1);
 reset();fail_recv=1;assert(efs_client_rpc_publication(&request,0,&out)==EFS_ERR_NET && drops==1);
 reset();expired=1;assert(efs_client_rpc_publication(&request,0,&out)==EFS_ERR_BUSY && !calls);
 reset();request.id.seq=0;out.state=EFS_PUBLICATION_COMMITTED;
 assert(efs_client_rpc_publication(&request,0,&out)==EFS_ERR_INVAL && out.state==EFS_PUBLICATION_UNKNOWN && !calls);
 puts("publication RPC: exact identity, lane routing, redirects, bounded retries, unknown and lost reply PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='efs-publication-rpc-') as d:
    p=Path(d)/'test.c';p.write_text(source)
    cmd=shlex.split(os.environ.get('CC','cc'))+['-std=gnu11','-Wall','-Wextra','-Werror','-I'+str(root/'include')]
    cmd+=shlex.split(os.environ.get('WRITER_RPC_TEST_CFLAGS',''))
    subprocess.run(cmd+[str(p),str(root/'src/kv/kv_key.c'),str(root/'src/common/publication.c'),
        str(root/'src/common/checksum.c'),str(root/'deps/blake3/blake3.c'),
        str(root/'deps/blake3/blake3_dispatch.c'),str(root/'deps/blake3/blake3_portable.c'),
        '-I'+str(root/'deps/blake3'),'-DBLAKE3_NO_SSE2','-DBLAKE3_NO_SSE41','-DBLAKE3_NO_AVX2','-DBLAKE3_NO_AVX512','-DBLAKE3_USE_NEON=0',
        '-pthread','-o',str(p.with_suffix(''))],check=True)
    subprocess.run([str(p.with_suffix(''))],check=True)
