#!/usr/bin/env python3
"""Execute immutable ticket transport; errors never become admission proof.

K39: a lost exchange (connect, send or receive) is not the verdict: the
identical body goes to another voter of the lane group after a pause, NET
only once the eight attempts or the budget are used up. Before, the first
lost reply was NET and a write placed around a stopped group leader failed
EIO. The degraded mark (fsync with a member down) follows the same rule."""
from pathlib import Path
import os,shlex,subprocess,tempfile
root=Path(__file__).resolve().parents[1];s=(root/'src/client/inode_rpc.c').read_text()
a=s.index('static int rpc_put_ticket(');b=s.index('int efs_client_rpc_publication(',a)
code=r'''
#include "efs/protocol.h"
#include "efs/put_ticket.h"
#include "efs/kv_key.h"
#include "efs/raft.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
struct efs_conn {unsigned node;};static struct efs_conn connection;
static unsigned calls,drops,redirect,busy,expired,expire_receive,malformed,fail_receive,query,no_conn,repicks,mark,nodes[16];
static uint64_t salt;static int result,hint,invalid;static uint8_t original[4+EFS_PUT_TICKET_BATCH_MAX*216];static uint32_t sent;
static int efs_client_rpc_past_deadline(void){return expired;}
static int session_ready;static unsigned fences,session_checks;
static int efs_client_session_check(void){session_checks++;return session_ready;}
static void efs_client_session_fence(int rc){if(rc==EFS_ERR_FENCED)fences++;}
static struct efs_conn *efs_client_conn_get(efs_node_id_t n){assert(n==3);connection.node=n;return &connection;}
/* Voters 1 and 2 of the lane group; the skipped one is never picked first. */
static struct efs_conn *raft_voter_conn_skip(uint32_t shard,efs_node_id_t *nid,efs_node_id_t skip){
 assert(shard==1);*nid=skip==1?2:1;connection.node=*nid;if(no_conn){no_conn--;return NULL;}return &connection;}
static void rpc_retry_peer(uint32_t shard,efs_node_id_t nid,efs_node_id_t *target,efs_node_id_t *skip,int failed){
 (void)shard;(void)failed;*target=0;*skip=nid;repicks++;}
static void efs_client_conn_drop(efs_node_id_t n,struct efs_conn *c){(void)n;(void)c;drops++;}
static void efs_client_conn_release(efs_node_id_t n,struct efs_conn *c){(void)n;(void)c;}
static int rpc_writer_retry_pause(unsigned n){(void)n;return EFS_OK;}
int efs_put_ticket_encode(const struct efs_put_ticket *t,uint8_t out[EFS_PUT_TICKET_WIRE_LEN]) {
 assert(t->ino==4097 && t->ci==0);memset(out,0x39,EFS_PUT_TICKET_WIRE_LEN);return invalid?EFS_ERR_INVAL:EFS_OK;
}
int efs_conn_send_msg(struct efs_conn *c,uint8_t type,const void *data,uint32_t bytes) {
 if(calls<16)nodes[calls]=c->node;
 if(mark){assert(type==EFS_MSG_RAFT_MKFS && bytes==1u+EFS_MD_DEGRADED_MARK_LEN);
  if(calls)assert(!memcmp(original,data,bytes));else memcpy(original,data,bytes);calls++;return 0;}
 assert(type==EFS_MSG_RAFT_MKFS && bytes==(query>=2?4u+sent*216u:218u));const uint8_t *p=data;
 static const uint8_t tag[5]={EFS_MD_CMD_PUT_TICKET_ADMIT,EFS_MD_REQ_PUT_TICKET_STATUS,EFS_MD_CMD_PUT_TICKET_SETTLE,
  EFS_MD_CMD_PUT_TICKET_ADMIT_BATCH,EFS_MD_CMD_PUT_TICKET_SETTLE_BATCH};
 assert(p[0]==efs_raft_shard_group(1) && p[1]==tag[query]);
 if(query>=2)assert(p[2]==0 && p[3]==sent);
 if(calls)assert(!memcmp(original,p,bytes));else memcpy(original,p,bytes);calls++;return 0;
}
int efs_conn_recv_msg(struct efs_conn *c,uint8_t *type,void **out,uint32_t *bytes) {
 (void)c;struct efs_msg_raft_mkfs_reply r={.rc=result,.salt=salt,.leader_hint=hint};
 if(redirect){redirect--;r.rc=EFS_ERR_NOT_PRIMARY;r.salt=0;r.leader_hint=2;}
 else if(busy){busy--;r.rc=EFS_ERR_BUSY;r.salt=0;}
 *type=EFS_MSG_RAFT_MKFS_REPLY;*bytes=sizeof(r)-malformed;*out=malloc(sizeof(r));memcpy(*out,&r,sizeof(r));
 if(expire_receive)expired=1;
 if(fail_receive){fail_receive--;return 1;}
 return 0;
}
'''+s[a:b]+r'''
static void reset(void) {calls=drops=redirect=busy=expired=expire_receive=malformed=fail_receive=query=invalid=no_conn=repicks=mark=0;memset(nodes,0,sizeof(nodes));salt=1;result=EFS_OK;hint=-1;
 session_ready=EFS_OK;fences=session_checks=0;}
int main(void) {
 struct efs_put_ticket t={.ino=4097};uint32_t state=99;
 #define RUN efs_client_rpc_put_ticket(&t,query,&state)
 reset();assert(!RUN && state==1 && calls==1);
 reset();redirect=1;assert(!RUN && state==1 && calls==2);
 reset();busy=1;assert(!RUN && calls==2);
 reset();salt=0;assert(RUN==EFS_ERR_PROTO && !state);
 reset();salt=4;assert(RUN==EFS_ERR_PROTO && !state);
 reset();salt=2;assert(RUN==EFS_ERR_STALE && !state);
 reset();query=1;salt=2;assert(!RUN && state==2);
 reset();query=1;salt=3;assert(RUN==EFS_ERR_PROTO && !state); /* 3 (was RECLAIMING) is refused */
 reset();query=1;salt=5;assert(!RUN && state==5); /* REJECTED */
 reset();query=1;salt=4;assert(RUN==EFS_ERR_PROTO && !state); /* RETIRED is never a stored state */
 reset();query=1;salt=0;result=EFS_ERR_NOT_FOUND;assert(RUN==EFS_ERR_NOT_FOUND && !state && calls==1);
 reset();salt=0;result=EFS_ERR_STALE;assert(RUN==EFS_ERR_STALE && !state);
 reset();result=EFS_ERR_STALE;assert(RUN==EFS_ERR_PROTO && !state);
 reset();result=EFS_ERR_NOT_PRIMARY;salt=0;hint=EFS_MAX_NODES;assert(RUN==EFS_ERR_PROTO && !state);
 /* K39: a lost reply re-picks the other voter, same body; NET only after
  * all eight attempts. An unreachable voter (no connection) likewise. */
 reset();fail_receive=1;assert(!RUN && state==1 && calls==2 && drops==1 && repicks==1 && nodes[0]==1 && nodes[1]==2);
 reset();no_conn=1;assert(!RUN && state==1 && calls==1 && !drops && repicks==1 && nodes[0]==2);
 reset();fail_receive=99;assert(RUN==EFS_ERR_NET && !state && calls==8 && drops==8);
 reset();fail_receive=1;expire_receive=1;assert(RUN==EFS_ERR_NET && !state && calls==1); /* lost, then out of budget */
 reset();expired=1;assert(RUN==EFS_ERR_BUSY && !calls && !state);
 reset();expire_receive=1;assert(RUN==EFS_ERR_BUSY && !state && calls==1);
 reset();malformed=1;assert(RUN==EFS_ERR_PROTO && !state);
 reset();busy=99;assert(RUN==EFS_ERR_BUSY && !state && calls==8);
 reset();invalid=1;assert(RUN==EFS_ERR_INVAL && !calls && !state);
 assert(efs_client_rpc_put_ticket(&t,2,&state)==EFS_ERR_INVAL && !state);
 /* SETTLE: the reply counts leading retired tickets of the batch. */
 struct efs_put_ticket two[2]={{.ino=4097},{.ino=4097}};uint32_t done=99;
 reset();query=2;sent=1;salt=1;assert(!efs_client_rpc_put_ticket_settle(two,1,&done) && done==1 && calls==1);
 reset();query=2;sent=2;salt=1;assert(!efs_client_rpc_put_ticket_settle(two,2,&done) && done==1);
 reset();query=2;sent=1;salt=0;assert(!efs_client_rpc_put_ticket_settle(two,1,&done) && !done);
 reset();query=2;sent=1;salt=2;assert(efs_client_rpc_put_ticket_settle(two,1,&done)==EFS_ERR_PROTO && !done);
 reset();query=2;sent=1;salt=1;redirect=1;assert(!efs_client_rpc_put_ticket_settle(two,1,&done) && calls==2);
 reset();query=2;sent=1;salt=1;fail_receive=1;assert(!efs_client_rpc_put_ticket_settle(two,1,&done) && done==1 && calls==2 && drops==1);
 reset();query=2;sent=1;salt=1;fail_receive=99;assert(efs_client_rpc_put_ticket_settle(two,1,&done)==EFS_ERR_NET && drops==8);
 reset();assert(efs_client_rpc_put_ticket_settle(two,0,&done)==EFS_ERR_INVAL && !calls);
 reset();assert(efs_client_rpc_put_ticket_settle(two,EFS_PUT_TICKET_SETTLE_MAX+1,&done)==EFS_ERR_INVAL && !calls);
 reset();session_ready=EFS_ERR_FENCED;query=2;sent=1;assert(efs_client_rpc_put_ticket_settle(two,1,&done)==EFS_ERR_FENCED && !calls);
 /* K32: ADMIT_BATCH / SETTLE_BATCH: 2..EFS_PUT_TICKET_BATCH_MAX tickets, the
  * reply is a mask of those tickets only; errors and redirects as above. */
 struct efs_put_ticket many[EFS_PUT_TICKET_BATCH_MAX+1];uint32_t mask=99;
 for(unsigned i=0;i<=EFS_PUT_TICKET_BATCH_MAX;i++)many[i]=(struct efs_put_ticket){.ino=4097};
 for(int settle=0;settle<2;settle++){
  reset();query=3+settle;sent=3;salt=5;assert(!efs_client_rpc_put_ticket_batch(many,3,settle,&mask) && mask==5 && calls==1);
  reset();query=3+settle;sent=3;salt=0;assert(!efs_client_rpc_put_ticket_batch(many,3,settle,&mask) && !mask);
  reset();query=3+settle;sent=3;salt=8;assert(efs_client_rpc_put_ticket_batch(many,3,settle,&mask)==EFS_ERR_PROTO && !mask);
  reset();query=3+settle;sent=EFS_PUT_TICKET_BATCH_MAX;salt=0xffffffffu;
  assert(!efs_client_rpc_put_ticket_batch(many,EFS_PUT_TICKET_BATCH_MAX,settle,&mask) && mask==0xffffffffu);
  reset();query=3+settle;sent=2;salt=3;redirect=1;assert(!efs_client_rpc_put_ticket_batch(many,2,settle,&mask) && mask==3 && calls==2);
  reset();query=3+settle;sent=2;salt=0;result=EFS_ERR_STALE;assert(efs_client_rpc_put_ticket_batch(many,2,settle,&mask)==EFS_ERR_STALE && !mask);
  reset();query=3+settle;sent=2;salt=3;fail_receive=1;assert(!efs_client_rpc_put_ticket_batch(many,2,settle,&mask) && mask==3 && calls==2);
  reset();query=3+settle;sent=2;fail_receive=99;assert(efs_client_rpc_put_ticket_batch(many,2,settle,&mask)==EFS_ERR_NET && drops==8);
  reset();assert(efs_client_rpc_put_ticket_batch(many,1,settle,&mask)==EFS_ERR_INVAL && !calls);
  reset();assert(efs_client_rpc_put_ticket_batch(many,EFS_PUT_TICKET_BATCH_MAX+1,settle,&mask)==EFS_ERR_INVAL && !calls);
  reset();session_ready=EFS_ERR_FENCED;query=3+settle;sent=2;assert(efs_client_rpc_put_ticket_batch(many,2,settle,&mask)==EFS_ERR_FENCED && !calls);
 }
 /* K5: a fenced mount admits no ticket (probes still run); a FENCED
  * admission refusal latches the mount fenced. */
 reset();session_ready=EFS_ERR_FENCED;assert(RUN==EFS_ERR_FENCED && !calls && session_checks==1);
 reset();session_ready=EFS_ERR_FENCED;query=1;assert(!RUN && calls==1 && !session_checks);
 reset();salt=0;result=EFS_ERR_FENCED;assert(RUN==EFS_ERR_FENCED && fences==1 && !state);
 reset();salt=0;result=EFS_ERR_UNREGISTERED;assert(RUN==EFS_ERR_UNREGISTERED && !fences && !state);
 /* K39: the degraded mark of fsync with a member down: same re-pick. */
 reset();mark=1;salt=0;fail_receive=1;assert(!efs_client_rpc_degraded_mark(4097,1,2,0,1,3) && calls==2 && nodes[1]==2);
 reset();mark=1;salt=0;fail_receive=99;assert(efs_client_rpc_degraded_mark(4097,1,2,0,1,3)==EFS_ERR_NET && calls==8);
 reset();mark=1;salt=0;result=EFS_ERR_NOT_FOUND;assert(efs_client_rpc_degraded_mark(4097,1,2,0,1,3)==EFS_ERR_NOT_FOUND && calls==1);
 puts("PUT ticket RPC: immutable retries, pure probe, exact state, revoked/body-error refusal, redirects, deadlines and fenced mount; K32 ADMIT_BATCH/SETTLE_BATCH framing and mask replies; K39 lost exchanges re-pick a voter (ticket and degraded mark) PASS");
}
'''
with tempfile.TemporaryDirectory() as td:
 p=Path(td);(p/'test.c').write_text(code)
 subprocess.run(shlex.split(os.environ.get('CC','cc'))+['-std=gnu11','-Wall','-Wextra','-Werror','-I'+str(root/'include'),'-I'+str(root/'src/client'),str(p/'test.c'),str(root/'src/kv/kv_key.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
