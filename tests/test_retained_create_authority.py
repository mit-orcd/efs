#!/usr/bin/env python3
"""Retained CREATE proof uses fresh quorum or its own settled exact entry."""
from pathlib import Path
import os,shlex,subprocess,tempfile
root=Path(__file__).resolve().parents[1]
s=(root/'src/server/raft_host.c').read_text()
a=s.index('static int host_retained_create_proof(');b=s.index('\n}',a)+2
code=r'''
#include "efs/meta_apply.h"
#include "efs/session.h"
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
# Coverage remains stack-local. Only a newly settled exact-entry verdict
# survives submission; plain CREATE and early forwarding preserve fresh proof.
a=s.index('void server_raft_host_create_retained(');b=s.index('\n}',a);body=s[a:b]
assert body.index('covered=submitted.status==EFS_INODE_RPC_OK?settled:0;')>body.index('host_create(')
assert body.count('host_retained_create_proof(h,request,out,&hint,&covered)')==2
assert 'request->shard,1,covered,&out->rpc,&settled)' in body
assert 'owner,q,0,0,0,out,NULL)' in s
a=s.index('static void host_create_ex(');b=s.index('\nstatic void host_create(',a);mutation=s[a:b]
assert mutation.index('if(settled)*settled=0;')<mutation.index('host_fwd_create(')
assert mutation.index('if(rc==EFS_OK && settled)*settled=(uint8_t)(1u<<dg);')>mutation.index('rc = host_wait_verdict(h, dg, idx, term, &hint);')
assert mutation.count('*settled=')==2
# Pending CREATE must pass namespace admission, whose first-touch protocol
# establishes a durable floor. Established participants are fenced locally,
# including before replay in apply; append's fresh-owner path is unnecessary.
assert 'host_append_session_admit_covered(' not in mutation
assert mutation.index('host_namespace_session_admit_covered(')<mutation.index('host_opid_replay(')
apply=(root/'src/meta/meta_apply.c').read_text()
a=apply.index('static int create_file_batch(');b=apply.index('int efs_meta_apply_create_file_log_pending(',a)
create=apply[a:b]
assert create.index('efs_session_accept(kv, shard, lease_uuid, lease_epoch)')<create.index('efs_meta_apply_opid_probe(')

with tempfile.TemporaryDirectory(prefix='efs-create-authority-') as work:
 p=Path(work)/'t.c';p.write_text(code)
 subprocess.run(shlex.split(os.environ.get('CC','cc'))+['-std=gnu11','-Wall','-Wextra','-Werror','-I'+str(root/'include'),str(p),'-o',str(p.with_suffix(''))],check=True)
 subprocess.run([str(p.with_suffix(''))],check=True)

# Exercise the complete retained handler across the mutation boundary.
handler=body+'\n}'
pre=code[:code.index('int main(void)')]
pre=pre.replace('struct efs_raft_host{struct efs_kv *kv;};','struct efs_raft_host{struct efs_kv *kv;int running;};\nstatic struct efs_raft_host *g_host;')
pre=pre.replace('assert(ino==8193);fetches++;row->generation=7;return fetch_rc;','if(ino==4097){memset(row,0,sizeof(*row));row->mode=S_IFDIR|0755;row->layout=EFS_META_LAYOUT_LOCAL;return 0;}assert(ino==8193);fetches++;row->generation=7;return fetch_rc;')
pre+=r'''
static int submits,settled_mode,hosted=1,forwards;
int efs_opid_req_valid(const struct efs_opid_req*q){return q->id.seq&&q->id.session_epoch&&q->ack<q->id.seq;}
static int host_hosts(struct efs_raft_host*h,uint8_t group){(void)h;(void)group;return hosted;}
static int host_leader_cover_pick(struct efs_raft_host*h,uint8_t g,const uint8_t*groups,int n){(void)h;(void)g;(void)groups;(void)n;return -1;}
static void set_inode_rc(struct efs_msg_inode_reply*out,int rc,int hint){(void)hint;out->status=rc==EFS_ERR_STALE?EFS_INODE_RPC_STALE:EFS_INODE_RPC_BUSY;}
static void host_retained_create_forward(struct efs_raft_host*h,const struct efs_msg_create_retained*r,struct efs_msg_create_retained_reply*out,const uint8_t*g,int n){(void)h;(void)r;(void)g;(void)n;forwards++;out->rpc.status=EFS_INODE_RPC_NOT_PRIMARY;}
static void host_create(efs_ino_t parent,const char*name,uint32_t mode,uint32_t uid,uint32_t gid,uint32_t flags,uint64_t owner,const struct efs_opid_req*q,uint32_t shard,int pinned,uint8_t covered,struct efs_msg_inode_reply*out,uint8_t*settled){
 assert(parent==4097&&!strcmp(name,"f")&&S_ISREG(mode)&&!uid&&!gid&&flags==EFS_CREATE_F_PENDING&&!owner&&q->id.seq==9&&shard==1&&pinned&&covered==(1u<<efs_raft_shard_group(1)));
 submits++;*settled=settled_mode?covered:0;
 if(settled_mode==2){out->status=EFS_INODE_RPC_BUSY;return;}
 hit=1;receipt.rc=0;receipt.ino=8193;out->status=EFS_INODE_RPC_OK;
}
''' + handler + r'''
int main(void){struct efs_raft_host h={.running=1};g_host=&h;
 struct efs_msg_create_retained r={.create={.parent=4097,.name="f",.mode=S_IFREG|0600,.flags=EFS_CREATE_F_PENDING},.q={.id={.seq=9,.session_epoch=1},.ack=8},.routed=1,.shard=1};
 struct efs_msg_create_retained_reply out;
 for(int mode=0;mode<3;mode++){
  hit=reads=submits=0;settled_mode=mode;
  server_raft_host_create_retained(&r,&out);
  assert(submits==1&&reads==(mode==1?1:2));
  if(mode==2)assert(!out.known&&out.rpc.status==EFS_INODE_RPC_BUSY);
  else assert(out.known&&out.rpc.inode.ino==8193&&out.generation==7);
 }
 hosted=0;reads=submits=hit=0;
 server_raft_host_create_retained(&r,&out);
 assert(forwards==1&&!reads&&!submits&&!out.known);
 puts("retained CREATE handler: own settled entry saves one round; unknown/forwarded outcomes require fresh proof PASS");}
'''
with tempfile.TemporaryDirectory(prefix='efs-create-handler-') as work:
 p=Path(work)/'t.c';p.write_text(pre)
 subprocess.run(shlex.split(os.environ.get('CC','cc'))+['-std=gnu11','-Wall','-Wextra','-Werror','-I'+str(root/'include'),str(p),str(root/'src/kv/kv_key.c'),'-o',str(p.with_suffix(''))],check=True)
 subprocess.run([str(p.with_suffix(''))],check=True)
