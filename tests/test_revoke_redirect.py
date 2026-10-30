#!/usr/bin/env python3
"""Exercise production client revocation routing; stale hints cannot pin it."""
from pathlib import Path
import os, subprocess, tempfile
root=Path(__file__).resolve().parents[1]
s=Path(os.environ.get('REVOKE_REDIRECT_SOURCE',root/'src/client/inode_rpc.c')).read_text()
a=s.index('static int session_revoke_submit(');b=s.index('\n}',a)+2
code=r'''
#include "efs/protocol.h"
#include "efs/raft.h"
#include "efs/session_revoke.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
struct efs_conn {efs_node_id_t node;};
static struct efs_conn connections[4]={{0},{1},{2},{3}};
static unsigned sends,pauses,drops;static int expired,fail_recv,fail_node1,busy,all_refuse,hint=-1,down[4];
static efs_node_id_t cached=1,leader=2,visited[8];
static int efs_client_rpc_past_deadline(void){return expired;}
static struct efs_conn *efs_client_conn_get(efs_node_id_t n){assert(n>0&&n<4);return down[n]?NULL:&connections[n];}
static struct efs_conn *raft_voter_conn_skip(uint32_t shard,efs_node_id_t *n,efs_node_id_t skip){
 assert(shard==1);*n=cached&&cached!=skip&&!down[cached]?cached:0;
 for(efs_node_id_t i=1;!*n&&i<4;i++)if(i!=skip&&!down[i])*n=i;
 return *n?efs_client_conn_get(*n):NULL;
}
static __attribute__((unused)) struct efs_conn *rpc_owner_conn_shard(uint32_t s,efs_node_id_t *n){return raft_voter_conn_skip(s,n,0);}
static __attribute__((unused)) void rpc_retry_peer(uint32_t s,efs_node_id_t n,efs_node_id_t *target,efs_node_id_t *skip,int failed){
 assert(s==1);if(failed)down[n]=1;if(cached==n)cached=0;*target=0;*skip=n;
}
static void efs_client_conn_drop(efs_node_id_t n,struct efs_conn *c){assert(n==c->node);drops++;}
static void efs_client_conn_release(efs_node_id_t n,struct efs_conn *c){assert(n==c->node);}
int efs_conn_send_msg(struct efs_conn *c,uint8_t t,const void *p,uint32_t n){(void)p;assert(t==EFS_MSG_RAFT_MKFS&&(n==23||n==1+EFS_MD_SESS_ACK_LEN||n==1+EFS_MD_SESS_ACK_LEN-4)&&sends<8);visited[sends++]=c->node;return 0;}
int efs_conn_recv_msg(struct efs_conn *c,uint8_t *t,void **p,uint32_t *n){
 if(fail_recv||(fail_node1&&c->node==1))return -1;
 struct efs_msg_raft_mkfs_reply *r=calloc(1,sizeof(*r));assert(r);
 r->rc=busy?EFS_ERR_BUSY:!all_refuse&&c->node==leader?EFS_OK:EFS_ERR_NOT_PRIMARY;r->leader_hint=hint;
 *p=r;*n=sizeof(*r);*t=EFS_MSG_RAFT_MKFS_REPLY;return 0;
}
static int rpc_writer_retry_pause(unsigned n){assert(n<8);pauses++;return EFS_OK;}
'''+s[a:b]+r'''
static void reset(void){sends=pauses=drops=0;expired=fail_recv=fail_node1=busy=all_refuse=0;down[1]=down[2]=down[3]=0;hint=-1;cached=1;leader=2;}
int main(void){
 uint8_t p[23]={EFS_RAFT_GROUP_SHARD,EFS_MD_CMD_SESSION,EFS_MD_SESS_FINISH};struct efs_msg_raft_mkfs_reply r;
 reset();assert(session_revoke_submit(NULL,p,sizeof(p),&r)==EFS_OK);assert(sends==2&&visited[0]==1&&visited[1]==2&&!drops);
 reset();hint=0;assert(session_revoke_submit(NULL,p,sizeof(p),&r)==EFS_OK&&sends==2&&visited[1]==2);
 reset();hint=2;leader=3;assert(session_revoke_submit(NULL,p,sizeof(p),&r)==EFS_OK&&sends==2&&visited[1]==3);
 reset();all_refuse=1;assert(session_revoke_submit(NULL,p,sizeof(p),&r)==EFS_ERR_NOT_PRIMARY&&sends==8);
 /* K54: a transport failure moves on to another voter; every voter failing is NET. */
 reset();fail_recv=1;assert(session_revoke_submit(NULL,p,sizeof(p),&r)==EFS_ERR_NET&&sends==3&&drops==3);
 reset();fail_node1=1;assert(session_revoke_submit(NULL,p,sizeof(p),&r)==EFS_OK&&sends==2&&visited[1]==2&&drops==1);
 /* K54: a hint naming a member that is down is skipped; the voter that named it is not asked again. */
 reset();hint=2;leader=2;down[3]=1;assert(session_revoke_submit(NULL,p,sizeof(p),&r)==EFS_OK&&sends==2&&visited[0]==1&&visited[1]==2);
 reset();hint=EFS_MAX_NODES;assert(session_revoke_submit(NULL,p,sizeof(p),&r)==EFS_ERR_PROTO&&sends==1);
 reset();busy=1;assert(session_revoke_submit(NULL,p,sizeof(p),&r)==EFS_ERR_BUSY&&sends==1);
 reset();expired=1;assert(session_revoke_submit(NULL,p,sizeof(p),&r)==EFS_ERR_BUSY&&!sends);
 reset();busy=1;p[2]=EFS_MD_SESS_STORAGE_FENCE;assert(session_revoke_submit(NULL,p,sizeof(p),&r)==EFS_ERR_BUSY&&sends==8);
 uint8_t ack[1+EFS_MD_SESS_ACK_LEN]={EFS_RAFT_GROUP_SHARD,EFS_MD_CMD_SESSION,EFS_MD_SESS_ACK};
 reset();busy=1;assert(session_revoke_submit(NULL,ack,sizeof(ack),&r)==EFS_ERR_BUSY&&sends==8);
 reset();busy=1;assert(session_revoke_submit(NULL,ack,sizeof(ack)-4,&r)==EFS_ERR_BUSY&&sends==1);
 puts("revocation redirect: stale/no/self hint, bounded refusal, transport failure and dead hint move to another voter, BUSY policy and deadline PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='efs-revoke-route-') as work:
 p=Path(work)/'test.c';p.write_text(code);binary=p.with_suffix('')
 subprocess.run([os.environ.get('CC','cc'),'-std=gnu11','-D_GNU_SOURCE','-Wall','-Wextra','-Werror','-I'+str(root/'include'),str(p),'-o',str(binary)],check=True)
 subprocess.run([str(binary)],check=True)
