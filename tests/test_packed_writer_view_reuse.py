#!/usr/bin/env python3
"""Actual host writer view: request-local barriers, current epochs and forwarding."""
from pathlib import Path
import os,shlex,subprocess,tempfile
root=Path(__file__).resolve().parents[1]
s=(root/'src/server/raft_host.c').read_text()
a=s.index('void server_raft_host_writer_view_reuse(');b=s.index('void server_raft_host_getchunks(',a)
code=r'''
#include "efs/protocol.h"
#include "efs/meta_apply.h"
#include "efs/kv_key.h"
#include "efs/raft.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
struct efs_raft_host {int running,n;struct efs_kv *kv;};
static struct efs_raft_host host={.running=1,.n=2},*g_host=&host;
static unsigned barriers[3],failed,unhosted,forwards,reads;
static uint64_t epoch;
static int host_hosts(struct efs_raft_host *h,uint8_t g){assert(h==&host);return !(unhosted&(1u<<g));}
static int host_read_index(struct efs_raft_host *h,uint8_t g,int *hint){assert(h==&host);*hint=0;barriers[g]++;return failed&(1u<<g)?EFS_ERR_BUSY:0;}
static int host_pick_peer(struct efs_raft_host *h,const uint8_t *g,int n,int skip){(void)h;(void)g;(void)n;return skip<0?1:-1;}
static int host_inode_rpc_peer(struct efs_raft_host *h,int rid,uint8_t type,const void *q,uint32_t len,uint8_t rtype,void *out,uint32_t bytes){(void)h;(void)q;assert(rid==1&&type==EFS_MSG_INODE_WRITER_VIEW&&len==sizeof(struct efs_msg_inode_writer_view)&&rtype==EFS_MSG_INODE_WRITER_VIEW_REPLY&&bytes==sizeof(struct efs_msg_inode_writer_view_reply));forwards++;memset(out,0,bytes);return 0;}
#define host_txn_coord NULL
int efs_meta_apply_get_inode_tx(struct efs_kv *kv,efs_ino_t ino,efs_txn_coord_fn fn,void *u,struct efs_meta_row *r){(void)kv;(void)ino;(void)fn;(void)u;r->generation=5;return 0;}
int efs_meta_get_writer_chunk_view_tx(struct efs_kv *kv,efs_ino_t ino,uint64_t gen,uint32_t ci,efs_txn_coord_fn fn,void *u,struct efs_meta_writer_view *v){(void)kv;(void)fn;(void)u;reads++;memset(v,0,sizeof(*v));v->ino=ino;v->generation=gen;(void)ci;v->authority_epoch=epoch;return gen==5?0:EFS_ERR_STALE;}
static int rc_to_inode_status(int rc){return rc==0?EFS_INODE_RPC_OK:rc==EFS_ERR_BUSY?EFS_INODE_RPC_BUSY:EFS_INODE_RPC_STALE;}
'''+s[a:b]+r'''
int main(void){
 uint8_t covered=0;struct efs_msg_inode_writer_view q={.ino=101,.generation=5};struct efs_msg_inode_writer_view_reply out;
 server_raft_host_writer_view_reuse(&q,&out,&covered);assert(out.status==EFS_INODE_RPC_OK&&covered==1&&barriers[0]==1&&reads==1);
 epoch=3;q.ino=103;server_raft_host_writer_view_reuse(&q,&out,&covered);assert(out.status==EFS_INODE_RPC_OK&&out.authority_epoch==3&&barriers[0]==1&&reads==2);
 q.generation=4;server_raft_host_writer_view_reuse(&q,&out,&covered);assert(out.status==EFS_INODE_RPC_STALE&&barriers[0]==1&&reads==3);q.generation=5;
 server_raft_host_writer_view(&q,&out);assert(out.status==EFS_INODE_RPC_OK&&barriers[0]==2); /* new RPC, new barrier */
 failed=1;covered=0;server_raft_host_writer_view_reuse(&q,&out,&covered);assert(out.status==EFS_INODE_RPC_BUSY&&!covered&&barriers[0]==3);failed=0;
 unhosted=1;server_raft_host_writer_view_reuse(&q,&out,&covered);assert(out.status==EFS_INODE_RPC_OK&&!covered&&forwards==1);server_raft_host_writer_view_reuse(&q,&out,&covered);assert(!covered&&forwards==2);unhosted=0;
 q.ino=102;server_raft_host_writer_view_reuse(&q,&out,&covered);assert(out.status==EFS_INODE_RPC_OK&&covered==4&&barriers[2]==1);
 q.ino=101;server_raft_host_writer_view_reuse(&q,&out,&covered);assert(out.status==EFS_INODE_RPC_OK&&covered==5&&barriers[0]==4);
 puts("packed writer views: one barrier per local group/request, current epoch/FileID refusal, fresh next RPC and no forwarded coverage PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='efs-pack-view-') as td:
 p=Path(td)/'t.c';p.write_text(code);exe=p.with_suffix('')
 subprocess.run(shlex.split(os.environ.get('CC','cc'))+['-std=gnu11','-Wall','-Wextra','-Werror','-I'+str(root/'include'),str(p),str(root/'src/kv/kv_key.c'),'-pthread','-lm','-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)
