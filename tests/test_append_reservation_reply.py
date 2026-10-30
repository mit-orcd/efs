#!/usr/bin/env python3
"""A proved reservation reply must not depend on a later attribute collect."""
from pathlib import Path
import os, subprocess, tempfile
root=Path(__file__).resolve().parents[1]
s=Path(os.environ.get('EFS_APPEND_HOST_SOURCE',root/'src/server/raft_host.c')).read_text()
a=s.index('static void host_append_owner(');b=s.index('\n}',a)+2
code=r'''
#include "efs/meta_apply.h"
#include "efs/protocol.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>
struct efs_raft_host {int running;struct efs_kv *kv;};
static int reserve_rc,abandon_rc,collects,transactions;
static uint64_t durable_offset=123;
static int host_append_transaction(struct efs_raft_host *h,efs_ino_t ino,uint64_t len,const struct efs_opid_req *q,uint64_t *off,int *hint){(void)h;assert(ino==7&&len==6&&q->id.seq==1&&q->id.session_epoch==2&&q->ack==0);transactions++;*hint=2;if(!reserve_rc)*off=durable_offset;return reserve_rc;}
static int host_append_abandon(struct efs_raft_host *h,efs_ino_t ino,uint64_t len,const struct efs_opid_req *q,unsigned *retired,int *hint){(void)h;(void)q;(void)hint;assert(ino==7&&len==6);*retired=1;return abandon_rc;}
static int __attribute__((unused)) host_txn_coord(void *ctx,const struct efs_txid *id,uint32_t sh,int *d){(void)ctx;(void)id;(void)sh;(void)d;return EFS_OK;}
int efs_meta_apply_getattr(struct efs_kv *kv,efs_ino_t ino,efs_txn_coord_fn co,void *ctx,struct efs_meta_stat *st){(void)kv;(void)ino;(void)co;(void)ctx;(void)st;collects++;return EFS_ERR_IO;}
static void __attribute__((unused)) stat_to_inode(const struct efs_meta_stat *st,struct efs_inode *ino){(void)st;(void)ino;assert(0);}
static void set_inode_rc(struct efs_msg_inode_reply *out,int rc,int hint){out->primary_id=hint<0?0:hint+1;out->status=rc==EFS_OK?EFS_INODE_RPC_OK:rc==EFS_ERR_UNREGISTERED?EFS_INODE_RPC_UNREGISTERED:EFS_INODE_RPC_BUSY;}
''' + s[a:b] + r'''
int main(void){struct efs_raft_host h={.running=1};uint8_t uuid[16]={1};uint64_t ack=0;struct efs_msg_inode_reply out={0};
 host_append_owner(&h,7,6,uuid,2,1,&ack,&out);
 assert(out.status==EFS_INODE_RPC_OK&&out.inode.ino==7&&out.inode.atime==123&&out.inode.size==129&&transactions==1&&!collects);
 /* A repeated identity returns its proved range, regardless of a later
  * file size or failed unrelated storage reads. */
 memset(&out,0,sizeof(out));host_append_owner(&h,7,6,uuid,2,1,&ack,&out);
 assert(out.status==EFS_INODE_RPC_OK&&out.inode.atime==123&&out.inode.size==129&&!collects);
 reserve_rc=EFS_ERR_BUSY;memset(&out,0,sizeof(out));host_append_owner(&h,7,6,uuid,2,1,&ack,&out);assert(out.status==EFS_INODE_RPC_BUSY&&!out.inode.size);
 int before=transactions;host_append_owner(&h,7,6,uuid,2,1,NULL,&out);assert(out.status==EFS_INODE_RPC_UNREGISTERED&&transactions==before);
 host_append_owner(&h,7,EFS_APPEND_ABANDON|6,uuid,2,1,&ack,&out);assert(out.status==EFS_INODE_RPC_OK&&out.inode.nlink==1&&transactions==before);
 h.running=0;host_append_owner(&h,7,6,uuid,2,1,&ack,&out);assert(out.status==EFS_INODE_RPC_BUSY&&transactions==before);
 puts("append reply: exact durable range, replay, refused admission, abandon and no post-acceptance stat dependency PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='efs-append-reply-') as d:
    p=Path(d)/'t.c';p.write_text(code)
    subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror','-I'+str(root/'include'),str(p),'-o',str(p.with_suffix(''))],check=True)
    subprocess.run([str(p.with_suffix(''))],check=True)
