#!/usr/bin/env python3
"""Actual HOLD/lock RPCs share a full UUID, including release/read paths."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[1];s=(root/'src/client/inode_rpc.c').read_text()
def fn(name):
 a=s.index('int '+name+'(');return s[a:s.index('\n}',a)+2]
code=r'''
#include "client_internal.h"
#include "efs/kv_key.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>
struct efs_client g_client;
static int admissions,requests,admit_rc;static uint32_t lock_op;
int efs_client_publication_id(uint64_t seq,struct efs_opid *id) {
 assert(seq==1);memset(id,0,sizeof(*id));id->client_uuid[0]=19;id->client_uuid[15]=231;id->session_epoch=3;return EFS_OK;
}
int efs_client_rpc_publication_session(uint32_t shard,const struct efs_opid *id) {
 assert(shard==1&&id->client_uuid[0]==19&&id->client_uuid[15]==231&&id->session_epoch==3);admissions++;return admit_rc;
}
static int rpc_status_to_efs(unsigned status){return status==EFS_INODE_RPC_OK?EFS_OK:EFS_ERR_STALE;}
static int rpc_send_recv_owner(efs_ino_t ino,uint8_t type,const void *data,uint32_t length,uint8_t reply_type,void *out,uint32_t outlen) {
 assert(ino==4097&&outlen==sizeof(struct efs_msg_inode_reply));const uint8_t *p=data;unsigned base;
 if(type==EFS_MSG_INODE_HOLD) {base=sizeof(struct efs_msg_inode_hold);assert(reply_type==EFS_MSG_INODE_HOLD_REPLY);}
 else {assert(type==EFS_MSG_INODE_FLOCK&&reply_type==EFS_MSG_INODE_FLOCK_REPLY);base=sizeof(struct efs_msg_inode_flock)+EFS_FLOCK_RANGE_LEN;
  const struct efs_msg_inode_flock *q=data;assert(q->owner==456&&q->op==lock_op);uint64_t start,end;memcpy(&start,p+sizeof(*q),8);memcpy(&end,p+sizeof(*q)+8,8);assert(start==7&&end==99);}
 assert(length==base+EFS_SESS_WIRE_LEN&&p[base]==19&&p[base+15]==231);uint32_t epoch;memcpy(&epoch,p+base+16,4);assert(epoch==3);
 requests++;memset(out,0,outlen);((struct efs_msg_inode_reply *)out)->status=EFS_INODE_RPC_OK;return EFS_OK;
}
'''+fn('efs_client_rpc_hold')+fn('efs_client_rpc_flock_range')+r'''
int main(void) {
 assert(!efs_client_rpc_hold(1,4097,1,123)&&admissions==1&&requests==1);
 assert(!efs_client_rpc_hold(1,4097,0,123)&&admissions==1&&requests==2);
 lock_op=EFS_FLOCK_EX|EFS_FLOCK_FCNTL;assert(!efs_client_rpc_flock_range(1,4097,lock_op,456,7,99,NULL)&&admissions==2);
 lock_op=EFS_FLOCK_UN|EFS_FLOCK_FCNTL;assert(!efs_client_rpc_flock_range(1,4097,lock_op,456,7,99,NULL)&&admissions==2);
 lock_op=EFS_FLOCK_GETLK|EFS_FLOCK_FCNTL;assert(!efs_client_rpc_flock_range(1,4097,lock_op,456,7,99,NULL)&&admissions==2);
 admit_rc=EFS_ERR_STALE;int before=requests;assert(efs_client_rpc_hold(1,4097,1,123)==EFS_ERR_STALE&&requests==before);
 lock_op=EFS_FLOCK_EX;assert(efs_client_rpc_flock_range(1,4097,lock_op,456,7,99,NULL)==EFS_ERR_STALE&&requests==before);
 puts("mount identity RPC: full UUID/epoch, admission before grants, release/query without registration and revoked refusal PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='efs-mount-identity-') as d:
 p=Path(d)/'t.c';p.write_text(code)
 subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror','-pthread','-I'+str(root/'include'),'-I'+str(root/'src/client'),str(p),'-o',str(p.with_suffix(''))],check=True)
 subprocess.run([str(p.with_suffix(''))],check=True)
