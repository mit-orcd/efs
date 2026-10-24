#!/usr/bin/env python3
"""Actual immutable deferred lease transport with known/unknown reply proof."""
from pathlib import Path
import os, shlex, subprocess, tempfile
root=Path(__file__).resolve().parents[1]
s=(root/'src/client/inode_rpc.c').read_text()
a=s.index('static int session_command_reply(')
b=s.index('static int publication_session_command(',a)
c=s.index('static void lease_command_be(',b)
d=s.index('/* Call before byte admission.',c)
code=r'''
#include "efs/protocol.h"
#include "efs/meta_cmd.h"
#include "efs/kv_key.h"
#include "efs/raft.h"
#include "efs/session.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
struct efs_conn {int unused;};static struct efs_conn connection;
static struct efs_opid_req identity;
static uint8_t first[67];static unsigned calls,drops,redirect,busy,expired,expire_receive,malformed,fail_receive,query;
static uint64_t salt;static int result,hint;static uint32_t route;
static int efs_client_rpc_past_deadline(void){return expired;}
static unsigned fences;static void efs_client_session_fence(int rc){if(rc==EFS_ERR_FENCED)fences++;}
static struct efs_conn *efs_client_conn_get(efs_node_id_t n){assert(n==3);return &connection;}
/* K34: a failed exchange re-picks a voter (skip the one that failed). */
static unsigned peer_retries;
static struct efs_conn *raft_voter_conn_skip(uint32_t shard,efs_node_id_t *nid,efs_node_id_t skip)
{route=shard;(void)skip;*nid=1;return &connection;}
static void rpc_retry_peer(uint32_t shard,efs_node_id_t nid,efs_node_id_t *target,efs_node_id_t *skip,int failed)
{(void)shard;(void)failed;*target=0;*skip=nid;peer_retries++;}
static void efs_client_conn_drop(efs_node_id_t n,struct efs_conn *c){(void)n;(void)c;drops++;}
static void efs_client_conn_release(efs_node_id_t n,struct efs_conn *c){(void)n;(void)c;}
static int rpc_writer_retry_pause(unsigned n){(void)n;return EFS_OK;}
static unsigned leader_notes;static efs_node_id_t noted_leader;
static void rpc_note_leader(uint8_t g,efs_node_id_t n){assert(g==0 || g==2);leader_notes++;noted_leader=n;}
static uint64_t number(const uint8_t *b,unsigned n){uint64_t v=0;while(n--)v=v<<8|*b++;return v;}
int efs_conn_send_msg(struct efs_conn *c,uint8_t type,const void *data,uint32_t n)
{
 (void)c;assert(type==EFS_MSG_RAFT_MKFS && n==67 && route==1);const uint8_t *b=data;
 assert(b[0]==efs_raft_shard_group(1) && b[1]==EFS_MD_CMD_SESSION);
 assert(b[2]==(query?EFS_MD_SESS_LEASE_PROBE_DEFERRED:EFS_MD_SESS_LEASE_EDGE_DEFERRED));
 assert(!memcmp(b+3,identity.id.client_uuid,16));
 assert(number(b+19,4)==1 && number(b+23,8)==5 && number(b+31,8)==4);
 assert(number(b+39,8)==4097 && number(b+47,8)==8 && number(b+55,4)==EFS_LEASE_EDGE_OPEN && !number(b+59,8));
 if(calls)assert(!memcmp(first,b,n));else memcpy(first,b,n);
 calls++;return 0;
}
int efs_conn_recv_msg(struct efs_conn *c,uint8_t *type,void **out,uint32_t *n)
{
 (void)c;struct efs_msg_raft_mkfs_reply reply={.rc=result,.salt=salt,.leader_hint=hint};
 if(redirect){redirect--;reply.rc=EFS_ERR_NOT_PRIMARY;reply.salt=0;reply.leader_hint=2;}
 else if(busy){busy--;reply.rc=EFS_ERR_BUSY;reply.salt=0;}
 *type=EFS_MSG_RAFT_MKFS_REPLY;*n=sizeof(reply)-malformed;*out=malloc(sizeof(reply));memcpy(*out,&reply,sizeof(reply));
 if(expire_receive)expired=1;
 if(fail_receive){fail_receive--;return 1;}
 return 0;
}
''' + s[a:b] + s[c:d] + r'''
static void reset(void)
{calls=drops=redirect=busy=expired=expire_receive=malformed=fail_receive=query=0;salt=1;result=EFS_OK;hint=-1;}
static int invoke(uint64_t *known){return efs_client_rpc_lease_edge(4097,8,&identity,EFS_LEASE_EDGE_OPEN,0,query,known);}
int main(void)
{
 identity.id.client_uuid[0]=93;identity.id.session_epoch=1;identity.id.seq=5;identity.ack=4;
 const struct efs_opid_req original=identity;uint64_t known=99;
 reset();assert(invoke(&known)==EFS_OK && known==1 && calls==1 && !leader_notes);
 reset();hint=2;assert(invoke(&known)==EFS_OK && known==1 && leader_notes==1 && noted_leader==3);
 reset();hint=EFS_MAX_NODES;assert(invoke(&known)==EFS_OK && leader_notes==1);
 reset();hint=2;expire_receive=1;assert(invoke(&known)==EFS_ERR_BUSY && leader_notes==1);
 reset();hint=2;malformed=1;assert(invoke(&known)==EFS_ERR_PROTO && leader_notes==1);
 reset();hint=2;result=EFS_ERR_STALE;assert(invoke(&known)==EFS_ERR_STALE && leader_notes==1);
 reset();salt=0;assert(invoke(&known)==EFS_ERR_PROTO && !known && calls==1);
 reset();result=EFS_ERR_BUSY;assert(invoke(&known)==EFS_ERR_BUSY && known==1 && calls==1);
 reset();query=1;salt=0;result=EFS_ERR_BUSY;assert(invoke(&known)==EFS_ERR_BUSY && !known && calls==1);
 reset();busy=99;assert(invoke(&known)==EFS_ERR_BUSY && !known && calls==8);
 reset();redirect=1;assert(invoke(&known)==EFS_OK && known==1 && calls==2);
 reset();result=EFS_ERR_NOT_PRIMARY;salt=0;hint=EFS_MAX_NODES;assert(invoke(&known)==EFS_ERR_PROTO && !known);
 reset();result=EFS_ERR_NO_QUORUM;assert(invoke(&known)==EFS_ERR_PROTO && !known);
 reset();query=1;result=EFS_ERR_NOT_FOUND;assert(invoke(&known)==EFS_ERR_NOT_FOUND && known==1 && calls==1);
 /* K34: a lost exchange is retried on another voter with the same body;
  * NET (unknown, request kept) only once all attempts are used up. */
 reset();peer_retries=0;fail_receive=99;assert(invoke(&known)==EFS_ERR_NET && !known && drops==8 && calls==8 && peer_retries==8);
 fail_receive=0;assert(invoke(&known)==EFS_OK && known==1 && calls==9); /* same identity/body */
 reset();fail_receive=1;assert(invoke(&known)==EFS_OK && known==1 && calls==2 && drops==1);
 reset();expired=1;assert(invoke(&known)==EFS_ERR_BUSY && !known && !calls);
 reset();expire_receive=1;assert(invoke(&known)==EFS_ERR_BUSY && !known && calls==1);
 reset();malformed=1;assert(invoke(&known)==EFS_ERR_PROTO && !known && calls==1);
 reset();salt=2;assert(invoke(&known)==EFS_ERR_PROTO && !known);
 reset();assert(efs_client_rpc_lease_edge(4097,0,&identity,1,0,0,&known)==EFS_ERR_INVAL && !known && !calls);
 assert(efs_client_rpc_lease_edge(4097,8,&identity,3,0,0,&known)==EFS_ERR_INVAL);
 assert(efs_client_rpc_lease_edge(4097,8,&identity,1,42,0,&known)==EFS_ERR_INVAL);
 assert(efs_client_rpc_lease_edge(4097,8,&identity,1,0,2,&known)==EFS_ERR_INVAL);
 assert(!memcmp(&identity,&original,sizeof(identity)));
 puts("lease RPC: immutable body, stored verdict proof, unknown probe, ambiguity, a lost exchange retried on another voter (K34), redirects and deadline PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='efs-lease-rpc-') as t:
    p=Path(t);(p/'test.c').write_text(code)
    subprocess.run(shlex.split(os.environ.get('CC','cc'))+['-std=c11','-Wall','-Wextra','-Werror','-I'+str(root/'include'),str(p/'test.c'),str(root/'src/sim/opid.c'),str(root/'src/kv/kv_key.c'),'-o',str(p/'test')],check=True)
    subprocess.run([str(p/'test')],check=True)
