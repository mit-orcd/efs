#!/usr/bin/env python3
"""Production REPORT rejects impossible wire counts before allocation/array walks."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[1];s=(root/'src/client/inode_rpc.c').read_text();a=s.index('int efs_client_rpc_report_dirty_raft(');b=s.index('\n}',a)+2
code=r'''
#include "efs/protocol.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
static unsigned allocations,rpcs;static int expired;
int efs_client_rpc_past_deadline(void){return expired;}
static void *test_malloc(size_t n){allocations++;assert(n<10000);return malloc(n);}
#define malloc test_malloc
static int rpc_status_to_efs(uint8_t s){return s==EFS_INODE_RPC_OK?EFS_OK:EFS_ERR_IO;}
static int rpc_send_recv_dual(uint8_t type,const void*p,uint32_t n,uint8_t expect,void*out,uint32_t olen,int ms){assert(type==EFS_MSG_REPORT_CHUNKS&&expect==EFS_MSG_REPORT_CHUNKS_REPLY&&olen==sizeof(struct efs_msg_inode_reply)&&ms>0);const struct efs_msg_report_chunks*h=p;rpcs++;assert(n==sizeof(*h)+(size_t)h->count*offsetof(struct efs_chunk_rec,deltas)+(size_t)h->ino_count*sizeof(struct efs_ino_size_rec));((struct efs_msg_inode_reply*)out)->status=EFS_INODE_RPC_OK;return EFS_OK;}
'''+s[a:b]+r'''
int main(void){struct efs_chunk_rec rec={0};struct efs_ino_size_rec ino={0};
 assert(efs_client_rpc_report_dirty_raft(1,NULL,1,&ino,1,1)==EFS_ERR_INVAL&&!allocations);
 assert(efs_client_rpc_report_dirty_raft(1,&rec,1,NULL,1,1)==EFS_ERR_INVAL&&!allocations);
 assert(efs_client_rpc_report_dirty_raft(1,&rec,UINT32_MAX,&ino,0,1)==EFS_ERR_INVAL&&!allocations);
 assert(efs_client_rpc_report_dirty_raft(1,&rec,0,&ino,UINT32_MAX,1)==EFS_ERR_INVAL&&!allocations);
 rec.delta_base_n=EFS_CHUNK_DELTA_MAX+1;assert(efs_client_rpc_report_dirty_raft(1,&rec,1,&ino,0,1)==EFS_ERR_INVAL&&!allocations);rec.delta_base_n=0;
 expired=1;assert(efs_client_rpc_report_dirty_raft(1,&rec,1,&ino,1,1)==EFS_ERR_BUSY&&!allocations);
 assert(!efs_client_rpc_report_dirty_raft(1,NULL,0,NULL,0,0)&&!allocations);expired=0;
 assert(!efs_client_rpc_report_dirty_raft(1,&rec,1,&ino,1,1)&&allocations==1&&rpcs==1);return 0;}
'''
with tempfile.TemporaryDirectory(prefix='efs-report-frame-') as d:
 p=Path(d)/'t.c';p.write_text(code)
 subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror','-I'+str(root/'include'),str(p),'-o',str(p.with_suffix(''))],check=True)
 subprocess.run([str(p.with_suffix(''))],check=True,timeout=5)
print('REPORT frame: pointer/count/delta/wire bounds, expired admission and normal framing PASS')
