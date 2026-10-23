#!/usr/bin/env python3
"""Production REPORT rejects impossible wire counts before allocation/array walks."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[1];s=(root/'src/client/inode_rpc.c').read_text();a=s.index('int efs_client_rpc_report_dirty_raft(');b=s.index('\n}',s.index('int efs_client_rpc_report_dirty_pub(',a))+2
code=r'''
#include "efs/protocol.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
static unsigned allocations,rpcs;static int expired;
static int efs_client_publication_id(uint64_t seq,struct efs_opid *id){memset(id,0,sizeof(*id));id->client_uuid[0]=9;id->session_epoch=4;id->seq=seq;return EFS_OK;}
/* W154: no degraded objects in this harness. */
static uint32_t efs_client_degraded_flags(efs_ino_t i,uint32_t c,uint64_t g){(void)i;(void)c;(void)g;return 0;}
static uint64_t efs_client_degraded_pending(void){return 0;}
static void efs_client_degraded_drop(efs_ino_t i,uint32_t c,uint64_t g){(void)i;(void)c;(void)g;}
int efs_client_rpc_past_deadline(void){return expired;}
static void *test_malloc(size_t n){allocations++;assert(n<10000);return malloc(n);}
#define malloc test_malloc
static int rpc_status_to_efs(uint8_t s){return s==EFS_INODE_RPC_OK?EFS_OK:EFS_ERR_IO;}
static int with_pubid;
int efs_client_rpc_report_dirty_pub(efs_export_id_t,const struct efs_chunk_rec*,uint32_t,const struct efs_ino_size_rec*,uint32_t,int,const struct efs_report_pubid*);
static int rpc_send_recv_dual(uint8_t type,const void*p,uint32_t n,uint8_t expect,void*out,uint32_t olen,int ms){assert(type==EFS_MSG_REPORT_CHUNKS&&expect==EFS_MSG_REPORT_CHUNKS_REPLY&&olen==sizeof(struct efs_msg_inode_reply)&&ms>0);const struct efs_msg_report_chunks*h=p;rpcs++;assert(n==sizeof(*h)+(size_t)h->count*offsetof(struct efs_chunk_rec,deltas)+(size_t)h->ino_count*sizeof(struct efs_ino_size_rec)+EFS_REPORT_SESSION_LEN+(with_pubid?EFS_REPORT_PUBID_LEN:0));assert(h->sync==(EFS_REPORT_F_SYNC|EFS_REPORT_F_SESSION|(with_pubid?EFS_REPORT_F_PUBID:0)));
 if(with_pubid){struct efs_report_pubid id;assert(efs_report_pubid_unpack(&id,(const uint8_t*)p+n-EFS_REPORT_SESSION_LEN-EFS_REPORT_PUBID_LEN)&&id.mint_ns==77&&id.seq==5);}struct efs_opid id;assert(efs_report_session_unpack(&id,(const uint8_t*)p+n-EFS_REPORT_SESSION_LEN)&&id.client_uuid[0]==9&&id.session_epoch==4);((struct efs_msg_inode_reply*)out)->status=EFS_INODE_RPC_OK;return EFS_OK;}
'''+s[a:b]+r'''
int main(void){struct efs_chunk_rec rec={0};struct efs_ino_size_rec ino={0};
 assert(efs_client_rpc_report_dirty_raft(1,NULL,1,&ino,1,1)==EFS_ERR_INVAL&&!allocations);
 assert(efs_client_rpc_report_dirty_raft(1,&rec,1,NULL,1,1)==EFS_ERR_INVAL&&!allocations);
 assert(efs_client_rpc_report_dirty_raft(1,&rec,UINT32_MAX,&ino,0,1)==EFS_ERR_INVAL&&!allocations);
 assert(efs_client_rpc_report_dirty_raft(1,&rec,0,&ino,UINT32_MAX,1)==EFS_ERR_INVAL&&!allocations);
 rec.delta_base_n=EFS_CHUNK_DELTA_MAX+1;assert(efs_client_rpc_report_dirty_raft(1,&rec,1,&ino,0,1)==EFS_ERR_INVAL&&!allocations);rec.delta_base_n=0;
 expired=1;assert(efs_client_rpc_report_dirty_raft(1,&rec,1,&ino,1,1)==EFS_ERR_BUSY&&!allocations);
 assert(!efs_client_rpc_report_dirty_raft(1,NULL,0,NULL,0,0)&&!allocations);expired=0;
 assert(!efs_client_rpc_report_dirty_raft(1,&rec,1,&ino,1,1)&&allocations==1&&rpcs==1);
 /* W77: the publication id rides directly before the session suffix. */
 struct efs_report_pubid pid={77,5};with_pubid=1;
 assert(!efs_client_rpc_report_dirty_pub(1,&rec,1,&ino,1,1,&pid)&&allocations==2&&rpcs==2);return 0;}
'''
with tempfile.TemporaryDirectory(prefix='efs-report-frame-') as d:
 p=Path(d)/'t.c';p.write_text(code)
 subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror','-I'+str(root/'include'),str(p),'-o',str(p.with_suffix(''))],check=True)
 subprocess.run([str(p.with_suffix(''))],check=True,timeout=5)
print('REPORT frame: pointer/count/delta/wire bounds, expired admission, normal and publication-id framing PASS')
