#!/usr/bin/env python3
"""Retained CREATE authority is reused only before the mutation boundary."""
from pathlib import Path
import os,shlex,subprocess,tempfile
root=Path(__file__).resolve().parents[1]
s=(root/'src/server/raft_host.c').read_text()
a=s.index('static int host_retained_create_proof(');b=s.index('\n}',a)+2
code=r'''
#include "efs/meta_apply.h"
#include "efs/protocol.h"
#include "efs/kv_key.h"
#include "efs/raft.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
struct efs_raft_host{struct efs_kv *kv;};
static int reads,read_rc,replays,hit,fetches,fetch_rc;
static struct efs_opid_reply receipt;
static int host_read_index(struct efs_raft_host*h,uint8_t g,int*hint){(void)h;(void)hint;assert(g==0 || g==2);reads++;return read_rc;}
static int host_opid_replay(struct efs_raft_host*h,uint32_t sh,const struct efs_opid_req*q,struct efs_opid_reply*r){(void)h;assert(sh==1 || sh==2);assert(q->id.seq==9);replays++;*r=receipt;return hit;}
static void host_opid_reply_ino(struct efs_raft_host*h,efs_ino_t parent,const char*name,const struct efs_opid_reply*r,struct efs_msg_inode_reply*out){(void)h;assert(parent==4097 && !strcmp(name,"f"));out->inode.ino=r->ino;}
int efs_meta_apply_get_inode(struct efs_kv*kv,efs_ino_t ino,struct efs_meta_row*row){(void)kv;assert(ino==8193);fetches++;row->generation=7;return fetch_rc;}
''' + s[a:b] + r'''
int main(void){struct efs_raft_host h={0};int hint=-1;uint8_t covered=0;
 struct efs_msg_create_retained request={.create={.parent=4097,.name="f"},.q={.id={.seq=9}}};
 struct efs_msg_create_retained_reply out={.shard=1};
 read_rc=EFS_ERR_IO;
 assert(host_retained_create_proof(&h,&request,&out,&hint,&covered)==EFS_ERR_IO && !covered && !replays && !out.known);
 read_rc=0;hit=0;
 assert(host_retained_create_proof(&h,&request,&out,&hint,&covered)==EFS_ERR_BUSY && covered==(1u<<efs_raft_shard_group(1)) && reads==2 && replays==1);
 /* An eligible boundary never turns an absent receipt into terminal proof. */
 assert(host_retained_create_proof(&h,&request,&out,&hint,&covered)==EFS_ERR_BUSY && reads==2 && replays==2 && !out.known);
 hit=1;receipt.rc=EFS_ERR_EXIST;
 assert(host_retained_create_proof(&h,&request,&out,&hint,&covered)==EFS_ERR_EXIST && out.known && !fetches);
 receipt.rc=0;receipt.ino=8193;fetch_rc=EFS_ERR_NOT_FOUND;
 assert(!host_retained_create_proof(&h,&request,&out,&hint,&covered) && out.generation==1 && out.rpc.inode.ino==8193);
 fetch_rc=0;
 assert(!host_retained_create_proof(&h,&request,&out,&hint,&covered) && out.generation==7);
 covered=0;read_rc=EFS_ERR_BUSY;int before=replays;
 assert(host_retained_create_proof(&h,&request,&out,&hint,&covered)==EFS_ERR_BUSY && replays==before && !covered);
 puts("retained CREATE authority: covered missing/negative/exact receipts, durable FileID, storage failure and fresh post-mutation refusal PASS");
}
'''
# Coverage remains stack-local and is explicitly reset after mutation. Plain
# CREATE and remote forwarding retain their independent fresh admissions.
a=s.index('void server_raft_host_create_retained(');b=s.index('\n}',a);body=s[a:b]
assert body.index('covered=0; /* Submission')>body.index('host_create(')
assert body.count('host_retained_create_proof(h,request,out,&hint,&covered)')==2
assert 'request->shard,1,covered,&out->rpc)' in body
assert 'owner,q,0,0,0,out)' in s
with tempfile.TemporaryDirectory(prefix='efs-create-authority-') as work:
 p=Path(work)/'t.c';p.write_text(code)
 subprocess.run(shlex.split(os.environ.get('CC','cc'))+['-std=gnu11','-Wall','-Wextra','-Werror','-I'+str(root/'include'),str(p),'-o',str(p.with_suffix(''))],check=True)
 subprocess.run([str(p.with_suffix(''))],check=True)
