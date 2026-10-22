#!/usr/bin/env python3
"""Both production metadata retry loops bound backoff and reclaim failed replies."""
from pathlib import Path
import os,shlex,subprocess,tempfile
root=Path(__file__).resolve().parents[1];s=Path(os.environ.get('EFS_INODE_RPC_SOURCE',root/'src/client/inode_rpc.c')).read_text()
def fn(prefix,second=False):
 a=s.index(prefix)
 if second:a=s.index(prefix,a+len(prefix))
 return s[a:s.index('\n}',a)+2]
code=r'''
#include "efs/protocol.h"
#include "efs/meta_cmd.h"
#include "metadata_read_route.h"
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
static uint64_t now=1000,deadline;static unsigned calls,sleeps,freed,drops,releases;
static int reply_status,fail_recv,fail_send,bad_type,expire_recv;
static unsigned read_hint;
static unsigned mkdir_ready, mkdir_packet_len;
static unsigned char mkdir_packet[1024];
static unsigned failover,routing_sends,stale_hint;
struct efs_conn {unsigned node;};static struct efs_conn connection;
static void rpc_learn_leader(const struct efs_msg_inode_reply*r){(void)r;}
static unsigned g_group_leader[2];
static struct {unsigned node_count;} g_client={4};
static struct efs_meta_read_routes g_read_routes={.mu=PTHREAD_MUTEX_INITIALIZER};
static int efs_client_node_is_down(efs_node_id_t n){(void)n;return 0;}
uint64_t efs_client_rpc_deadline_ms(void){return deadline;}
int efs_client_rpc_past_deadline(void){return deadline&&now>=deadline;}
static int rpc_past_deadline(void){return efs_client_rpc_past_deadline();}
static int fake_clock(int id,struct timespec*t){(void)id;t->tv_sec=now/1000;t->tv_nsec=(now%1000)*1000000;return 0;}
static int fake_sleep(unsigned us){now+=us/1000;sleeps++;return 0;}
static void test_free(void*p){if(p)freed++;free(p);}
#define free test_free
#define clock_gettime fake_clock
#define usleep fake_sleep
static struct efs_conn *efs_client_conn_get(efs_node_id_t n){connection.node=n;return &connection;}
static struct efs_conn *raft_voter_conn_skip(uint32_t s,efs_node_id_t*n,efs_node_id_t skip){(void)s;*n=(failover&&skip==2)?3:1;connection.node=*n;return &connection;}
static struct efs_conn *raft_dual_voter_conn(efs_node_id_t*n,efs_node_id_t skip){return raft_voter_conn_skip(0,n,skip);}
static void efs_client_conn_drop(efs_node_id_t n,struct efs_conn*c){(void)n;(void)c;drops++;}
static void efs_client_conn_release(efs_node_id_t n,struct efs_conn*c){(void)n;(void)c;releases++;}
static int rpc_prof_enabled(void){return 0;}
static unsigned long long rpc_prof_now_us(void){return now*1000;}
static void rpc_prof_add(int type,unsigned long long a,unsigned long long b,unsigned long long c,unsigned long long d){(void)type;(void)a;(void)b;(void)c;(void)d;}
static void rpc_note_slow_ok(uint8_t t,int a,int b,uint8_t st,unsigned long long n){(void)t;(void)a;(void)b;(void)st;(void)n;}
static int stale_retryable(uint8_t type){(void)type;return 1;}
static void efs_client_node_note_fail(efs_node_id_t n){(void)n;}
static void efs_client_node_note_ok(efs_node_id_t n){(void)n;}
static unsigned efs_raft_shard_group(uint32_t s){(void)s;return 0;}
static unsigned rpc_leader_of(unsigned g){(void)g;return 1;}
static void efs_conn_set_recv_timeout(struct efs_conn*c,int n){(void)c;(void)n;}
int efs_conn_send_msg(struct efs_conn*c,uint8_t t,const void*p,uint32_t n){(void)t;if(stale_hint && calls)assert(c->node==3);if(mkdir_ready){assert(n<=sizeof(mkdir_packet));if(!mkdir_packet_len){memcpy(mkdir_packet,p,n);mkdir_packet_len=n;}else assert(n==mkdir_packet_len&&!memcmp(mkdir_packet,p,n));}if(failover){routing_sends++;if(routing_sends==2){assert(c->node==2);if(failover==2)return 1;}if(routing_sends==3)assert(c->node==3);}return fail_send;}
int efs_conn_recv_msg(struct efs_conn*c,uint8_t*t,void**p,uint32_t*n){(void)c;calls++;struct efs_msg_inode_reply*r=calloc(1,sizeof(*r));assert(r);r->read_node_id=read_hint;r->status=mkdir_ready && calls>=mkdir_ready ? EFS_INODE_RPC_OK : reply_status;*p=r;*n=sizeof(*r);*t=bad_type?0:EFS_MSG_INODE_GETATTR_REPLY;if(expire_recv)now=deadline;if(stale_hint && calls==1){r->status=EFS_INODE_RPC_STALE;r->primary_id=3;}if(failover){if(calls==1){r->status=EFS_INODE_RPC_NOT_PRIMARY;r->primary_id=2;}else if(failover==1&&calls==2)return 1;else assert(c->node==3);}return fail_recv;}
static void opid_retry_ack(void *packet,uint32_t length){(void)packet;(void)length;}
static int efs_client_session_check(void){return EFS_OK;} /* K5 mount session, ready */
'''+fn('static int rpc_writer_retry_pause(' ,True)+fn('static void rpc_retry_peer(')+fn('static int rpc_type_mutates(')+fn('static int rpc_send_recv_shard(')+fn('static int rpc_send_recv_dual(',True)+r'''
static void reset(void){calls=sleeps=freed=drops=releases=0;reply_status=EFS_INODE_RPC_OK;fail_recv=fail_send=bad_type=expire_recv=0;now=1000;deadline=0;failover=routing_sends=mkdir_ready=mkdir_packet_len=stale_hint=0;g_group_leader[0]=0;read_hint=0;memset(g_read_routes.slot,0,sizeof(g_read_routes.slot));}
static int run(int dual,struct efs_msg_inode_reply*r){return dual?rpc_send_recv_dual(EFS_MSG_INODE_GETATTR,NULL,0,EFS_MSG_INODE_GETATTR_REPLY,r,sizeof(*r),100):rpc_send_recv_shard(0,EFS_MSG_INODE_GETATTR,NULL,0,EFS_MSG_INODE_GETATTR_REPLY,r,sizeof(*r),0,NULL);}
int main(void){
 struct efs_msg_inode_reply route_reply;
 reset();read_hint=2;assert(!run(0,&route_reply));
 uint64_t key=efs_meta_read_route_key(EFS_MSG_INODE_GETATTR,NULL,0);
 assert(efs_meta_read_route_get(&g_read_routes,key)==2);
 assert(!run(0,&route_reply)&&connection.node==2);
 reset();read_hint=9;assert(!run(0,&route_reply));assert(!efs_meta_read_route_get(&g_read_routes,key));
 reset();read_hint=3;reply_status=EFS_INODE_RPC_BUSY;deadline=1010;
 assert(run(0,&route_reply)==EFS_ERR_BUSY&&!efs_meta_read_route_get(&g_read_routes,key));
 reset();read_hint=3;deadline=1010;expire_recv=1;
 assert(run(0,&route_reply)==EFS_ERR_BUSY&&!efs_meta_read_route_get(&g_read_routes,key));
 for(int d=0;d<2;d++){
 struct efs_msg_inode_reply r;
 reset();assert(!run(d,&r)&&calls==1&&freed==1&&releases==1);
 reset();deadline=1000;assert(run(d,&r)==EFS_ERR_BUSY&&!calls&&!sleeps);
 reset();reply_status=EFS_INODE_RPC_BUSY;deadline=1010;assert(run(d,&r)==EFS_ERR_BUSY&&calls==1&&freed==1&&sleeps==1&&now==1010);
 reset();reply_status=EFS_INODE_RPC_STALE;deadline=1010;assert(run(d,&r)==EFS_ERR_BUSY&&calls==1&&freed==1&&now==1010);
 reset();reply_status=EFS_INODE_RPC_NOT_PRIMARY;deadline=1010;assert(run(d,&r)==EFS_ERR_BUSY&&calls==1&&freed==1&&now==1010);
 reset();fail_recv=1;deadline=1010;assert(run(d,&r)==EFS_ERR_BUSY&&calls==1&&freed==1&&drops==1&&!releases);
 reset();fail_send=1;deadline=1010;assert(run(d,&r)==EFS_ERR_BUSY&&!calls&&drops==1&&sleeps==1);
 reset();deadline=1010;expire_recv=1;memset(&r,0x5a,sizeof(r));struct efs_msg_inode_reply before=r;assert(run(d,&r)==EFS_ERR_BUSY&&freed==1&&!memcmp(&r,&before,sizeof(r)));
 reset();stale_hint=1;assert(!run(d,&r)&&calls==2&&freed==2);
 reset();reply_status=EFS_INODE_RPC_BUSY;assert(run(d,&r)==EFS_ERR_BUSY&&calls==16&&freed==16&&sleeps==15);
 }
 /* Unlock BUSY is transient admission, not a grant conflict. Preserve the
  * complete owner/range/session packet, the 16 attempts and the deadline. */
 unsigned char unlock[sizeof(struct efs_msg_inode_flock)+EFS_FLOCK_RANGE_LEN+EFS_SESS_WIRE_LEN];
 memset(unlock,0x5a,sizeof(unlock));
 struct efs_msg_inode_flock uq={.ino=4097,.op=EFS_FLOCK_UN|EFS_FLOCK_FCNTL,.owner=123};
 memcpy(unlock,&uq,sizeof(uq));struct efs_msg_inode_reply ur;
 reset();reply_status=EFS_INODE_RPC_BUSY;mkdir_ready=3;
 assert(rpc_send_recv_shard(0,EFS_MSG_INODE_FLOCK,unlock,sizeof(unlock),EFS_MSG_INODE_GETATTR_REPLY,&ur,sizeof(ur),0,NULL)==EFS_OK);
 assert(calls==3 && sleeps==2 && freed==3 && ur.status==EFS_INODE_RPC_OK);
 reset();reply_status=EFS_INODE_RPC_BUSY;deadline=1010;
 assert(rpc_send_recv_shard(0,EFS_MSG_INODE_FLOCK,unlock,sizeof(unlock),EFS_MSG_INODE_GETATTR_REPLY,&ur,sizeof(ur),0,NULL)==EFS_ERR_BUSY);
 assert(calls==1 && sleeps==1 && now==deadline && freed==1);
 reset();reply_status=EFS_INODE_RPC_BUSY;
 assert(rpc_send_recv_shard(0,EFS_MSG_INODE_FLOCK,unlock,sizeof(unlock),EFS_MSG_INODE_GETATTR_REPLY,&ur,sizeof(ur),0,NULL)==EFS_ERR_BUSY);
 assert(calls==16 && sleeps==15 && freed==16);
 for(unsigned op=EFS_FLOCK_EX;op<=EFS_FLOCK_EX+EFS_FLOCK_NB;op+=EFS_FLOCK_NB){
  uq.op=op;memcpy(unlock,&uq,sizeof(uq));reset();reply_status=EFS_INODE_RPC_BUSY;
  assert(rpc_send_recv_shard(0,EFS_MSG_INODE_FLOCK,unlock,sizeof(unlock),EFS_MSG_INODE_GETATTR_REPLY,&ur,sizeof(ur),0,NULL)==EFS_OK);
  assert(calls==1 && !sleeps && ur.status==EFS_INODE_RPC_BUSY);
  }
 /* A directory prepare can outlive the legacy 16-attempt window. */
 unsigned char packet[sizeof(struct efs_msg_inode_create)+EFS_OPID_WIRE_LEN]={0};
 ((struct efs_msg_inode_create *)packet)->mode=S_IFDIR|0755;
 struct efs_msg_inode_reply mkdir_reply;
 reset();deadline=31000;reply_status=EFS_INODE_RPC_BUSY;mkdir_ready=21;
 assert(rpc_send_recv_dual(EFS_MSG_INODE_CREATE,packet,sizeof(packet),EFS_MSG_INODE_GETATTR_REPLY,&mkdir_reply,sizeof(mkdir_reply),100)==EFS_OK);
 assert(calls==21 && now<deadline && freed==21);
 reset();deadline=1010;reply_status=EFS_INODE_RPC_BUSY;mkdir_ready=21;
 assert(rpc_send_recv_dual(EFS_MSG_INODE_CREATE,packet,sizeof(packet),EFS_MSG_INODE_GETATTR_REPLY,&mkdir_reply,sizeof(mkdir_reply),100)==EFS_ERR_BUSY);
 assert(calls==1 && now==deadline && freed==1);
 for(unsigned mode=1;mode<=2;mode++){
  struct efs_msg_inode_reply r;reset();failover=mode;g_group_leader[0]=2;
  assert(!run(0,&r)&&routing_sends==3&&calls==(mode==1?3u:2u)&&g_group_leader[0]==0);
 }
 efs_node_id_t target=2,skip=0;g_group_leader[0]=3;
 rpc_retry_peer(0,2,&target,&skip,1);
 assert(!target&&skip==2&&g_group_leader[0]==3);
 return 0;}
'''
with tempfile.TemporaryDirectory(prefix='efs-metadata-budget-') as d:
 p=Path(d)/'t.c';p.write_text(code)
 subprocess.run(shlex.split(os.environ.get('CC','cc'))+['-std=gnu11','-Wall','-Wextra','-Werror','-I'+str(root/'include'),'-I'+str(root/'src/client')]+shlex.split(os.environ.get('EFS_TEST_CFLAGS',''))+[str(p),'-o',str(p.with_suffix(''))],check=True)
 subprocess.run([str(p.with_suffix(''))],check=True,timeout=5)
print('metadata retries: shard/dual budgets, STALE/BUSY/hints, failed payload cleanup and late-output retention, lost hinted peer rerouting, concurrent leader retention same-identity MKDIR beyond 16 attempts with shorter deadline enforcement, and exact-packet bounded unlock BUSY retries with immediate grant conflicts PASS')
