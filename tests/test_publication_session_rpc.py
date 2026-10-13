#!/usr/bin/env python3
"""Run production pre-write session sequencing with ambiguity and fencing."""
from pathlib import Path
import os,subprocess,tempfile
root=Path(__file__).resolve().parents[1]
s=(root/'src/client/inode_rpc.c').read_text()
a=s.index('static int session_command_reply(')
b=s.index('/* Production ownership primitive:',a)
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
static struct efs_opid identity;
static unsigned calls,stage,redirect,busy,drops,expired,malformed,fail_receive;
static int failure,existing;static uint64_t query_salt;
static uint32_t routing,participant;
static uint8_t sent;
uint32_t efs_kv_session_shard(const uint8_t uuid[EFS_OPID_UUID_LEN]){assert(uuid[0]==9);return 31;}
static int efs_client_rpc_past_deadline(void){return expired;}
static struct efs_conn *efs_client_conn_get(efs_node_id_t n){assert(n==3);return &connection;}
static struct efs_conn *rpc_owner_conn_shard(uint32_t shard,efs_node_id_t *nid)
{routing=shard;*nid=1;return &connection;}
static void efs_client_conn_drop(efs_node_id_t n,struct efs_conn *c){(void)n;(void)c;drops++;}
static void efs_client_conn_release(efs_node_id_t n,struct efs_conn *c){(void)n;(void)c;}
static int rpc_writer_retry_pause(unsigned n){(void)n;return EFS_OK;}
static unsigned leader_notes;static efs_node_id_t noted_leader;
static void rpc_note_leader(uint8_t g,efs_node_id_t n){assert(g==0 || g==2);leader_notes++;noted_leader=n;}
int efs_conn_send_msg(struct efs_conn *c,uint8_t type,const void *data,uint32_t n)
{
 (void)c;assert(type==EFS_MSG_RAFT_MKFS);const uint8_t *b=data;
 sent=b[2];assert(b[1]==EFS_MD_CMD_SESSION && !memcmp(b+3,identity.client_uuid,16));
 assert(!sent || (sent==EFS_MD_SESS_REGISTER_ESTABLISH && stage==1) || sent==stage+1 || (stage==2 && sent==EFS_MD_SESS_ESTABLISH));
 assert(n==(!sent || sent==EFS_MD_SESS_CREATE?23:27));
 uint32_t shard=(sent==EFS_MD_SESS_ESTABLISH || sent==EFS_MD_SESS_REGISTER_ESTABLISH)?participant:efs_kv_session_shard(identity.client_uuid);
 assert(b[0]==efs_raft_shard_group(shard) && routing==shard);
 assert(b[19]==0 && b[20]==0 && b[21]==0 && b[22]==(!sent?participant:1));
 if(n==27)assert(b[23]==0 && b[24]==0 && b[25]==0 && b[26]==participant);
 calls++;return 0;
}
int efs_conn_recv_msg(struct efs_conn *c,uint8_t *type,void **out,uint32_t *n)
{
 (void)c;struct efs_msg_raft_mkfs_reply r={.leader_hint=-1};
 if(redirect){redirect--;r.rc=EFS_ERR_NOT_PRIMARY;r.leader_hint=2;}
 else if(busy){busy--;r.rc=EFS_ERR_BUSY;}
 else if(failure)r.rc=failure;
 else if(!fail_receive) {
  if(!sent) {
   if(!existing)r.rc=EFS_ERR_NOT_FOUND;
   else {r.salt=query_salt;if((uint32_t)r.salt==1 && ((r.salt>>32)&255u)==EFS_SESSION_ACTIVE)stage=r.salt&(1ull<<40)?2:1;}
  } else if(sent==EFS_MD_SESS_REGISTER_ESTABLISH)stage=3;
  else stage++;
 }
 *type=EFS_MSG_RAFT_MKFS_REPLY;*n=sizeof(r)-malformed;*out=malloc(sizeof(r));memcpy(*out,&r,sizeof(r));
 return fail_receive;
}
''' + s[a:b] + r'''
static void reset(void){calls=stage=redirect=busy=drops=expired=malformed=fail_receive=0;failure=existing=0;query_salt=0;participant=18;}
int main(void)
{
 identity.client_uuid[0]=9;identity.session_epoch=1;
 reset();assert(efs_client_rpc_publication_session(participant,&identity)==EFS_OK && calls==4 && stage==3);
 reset();existing=1;query_salt=1|((uint64_t)EFS_SESSION_ACTIVE<<32)|(1ull<<40);
 assert(efs_client_rpc_publication_session(participant,&identity)==EFS_OK && calls==2 && stage==3);
 reset();existing=1;query_salt=1|((uint64_t)EFS_SESSION_ACTIVE<<32);
 assert(efs_client_rpc_publication_session(participant,&identity)==EFS_OK && calls==3 && stage==3);
 reset();participant=17;
 assert(efs_client_rpc_publication_session(participant,&identity)==EFS_OK && calls==3 && stage==3);
 reset();participant=17;existing=1;query_salt=1|((uint64_t)EFS_SESSION_ACTIVE<<32);
 assert(efs_client_rpc_publication_session(participant,&identity)==EFS_OK && calls==2 && stage==3);
 reset();participant=17;existing=1;query_salt=1|((uint64_t)EFS_SESSION_ACTIVE<<32)|(1ull<<40);
 assert(efs_client_rpc_publication_session(participant,&identity)==EFS_OK && calls==2 && stage==3);
 reset();existing=1;query_salt=2|((uint64_t)EFS_SESSION_ACTIVE<<32)|(1ull<<40);
 assert(efs_client_rpc_publication_session(participant,&identity)==EFS_ERR_STALE && calls==1&&!stage);
 reset();existing=1;query_salt=1|((uint64_t)EFS_SESSION_FENCING<<32)|(1ull<<40);
 assert(efs_client_rpc_publication_session(participant,&identity)==EFS_ERR_BUSY && calls==1&&!stage);
 reset();existing=1;query_salt=1|((uint64_t)EFS_SESSION_ACTIVE<<32)|(1ull<<41);
 assert(efs_client_rpc_publication_session(participant,&identity)==EFS_ERR_PROTO && calls==1);
 reset();existing=1;query_salt=1|(3ull<<32);
 assert(efs_client_rpc_publication_session(participant,&identity)==EFS_ERR_PROTO && calls==1);
 reset();redirect=1;busy=1;
 assert(efs_client_rpc_publication_session(participant,&identity)==EFS_OK && calls==6 && stage==3);
 reset();failure=EFS_ERR_STALE;
 assert(efs_client_rpc_publication_session(participant,&identity)==EFS_ERR_STALE && calls==1 && !stage);
 reset();fail_receive=1;
 assert(efs_client_rpc_publication_session(participant,&identity)==EFS_ERR_NET && calls==1 && drops==1 && !stage);
 fail_receive=0;assert(efs_client_rpc_publication_session(participant,&identity)==EFS_OK && stage==3);
 reset();busy=99;assert(efs_client_rpc_publication_session(participant,&identity)==EFS_ERR_BUSY && calls==8 && !stage);
 reset();malformed=1;assert(efs_client_rpc_publication_session(participant,&identity)==EFS_ERR_PROTO && calls==1);
 reset();expired=1;assert(efs_client_rpc_publication_session(participant,&identity)==EFS_ERR_BUSY && !calls);
 reset();assert(efs_client_rpc_publication_session(4096,&identity)==EFS_ERR_INVAL && !calls);
 identity.session_epoch=0;assert(efs_client_rpc_publication_session(participant,&identity)==EFS_ERR_INVAL && !calls);
 puts("publication session RPC: ordered admission, redirects, bounded retries, ambiguity, fences and deadline PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='efs-session-rpc-') as d:
    path=Path(d);(path/'test.c').write_text(code)
    subprocess.run([os.environ.get('CC','cc'),'-std=c11','-Wall','-Wextra','-Werror','-I'+str(root/'include'),str(path/'test.c'),str(root/'src/sim/opid.c'),'-o',str(path/'test')],check=True)
    subprocess.run([str(path/'test')],check=True)
