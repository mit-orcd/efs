#!/usr/bin/env python3
"""Both production metadata retry loops bound backoff and reclaim failed replies."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[1];s=(root/'src/client/inode_rpc.c').read_text()
def fn(prefix,second=False):
 a=s.index(prefix)
 if second:a=s.index(prefix,a+len(prefix))
 return s[a:s.index('\n}',a)+2]
code=r'''
#include "efs/protocol.h"
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
static uint64_t now=1000,deadline;static unsigned calls,sleeps,freed,drops,releases;
static int reply_status,fail_recv,fail_send,bad_type,expire_recv;
struct efs_conn {int unused;};static struct efs_conn connection;
static unsigned g_group_leader[2];
uint64_t efs_client_rpc_deadline_ms(void){return deadline;}
int efs_client_rpc_past_deadline(void){return deadline&&now>=deadline;}
static int rpc_past_deadline(void){return efs_client_rpc_past_deadline();}
static int fake_clock(int id,struct timespec*t){(void)id;t->tv_sec=now/1000;t->tv_nsec=(now%1000)*1000000;return 0;}
static int fake_sleep(unsigned us){now+=us/1000;sleeps++;return 0;}
static void test_free(void*p){if(p)freed++;free(p);}
#define free test_free
#define clock_gettime fake_clock
#define usleep fake_sleep
static struct efs_conn *efs_client_conn_get(efs_node_id_t n){(void)n;return &connection;}
static struct efs_conn *raft_voter_conn_skip(uint32_t s,efs_node_id_t*n,efs_node_id_t skip){(void)s;(void)skip;*n=1;return &connection;}
static struct efs_conn *raft_dual_voter_conn(efs_node_id_t*n,efs_node_id_t skip){return raft_voter_conn_skip(0,n,skip);}
static void efs_client_conn_drop(efs_node_id_t n,struct efs_conn*c){(void)n;(void)c;drops++;}
static void efs_client_conn_release(efs_node_id_t n,struct efs_conn*c){(void)n;(void)c;releases++;}
static int rpc_prof_enabled(void){return 0;}
static unsigned long long rpc_prof_now_us(void){return now*1000;}
static void rpc_prof_add(int type,unsigned long long a,unsigned long long b,unsigned long long c,unsigned long long d){(void)type;(void)a;(void)b;(void)c;(void)d;}
static void rpc_note_slow_ok(uint8_t t,int a,int b,uint8_t st,unsigned long long n){(void)t;(void)a;(void)b;(void)st;(void)n;}
static int stale_retryable(uint8_t type){(void)type;return 1;}
static unsigned efs_raft_shard_group(uint32_t s){(void)s;return 0;}
static unsigned rpc_leader_of(unsigned g){(void)g;return 1;}
static void efs_conn_set_recv_timeout(struct efs_conn*c,int n){(void)c;(void)n;}
int efs_conn_send_msg(struct efs_conn*c,uint8_t t,const void*p,uint32_t n){(void)c;(void)t;(void)p;(void)n;return fail_send;}
int efs_conn_recv_msg(struct efs_conn*c,uint8_t*t,void**p,uint32_t*n){(void)c;calls++;struct efs_msg_inode_reply*r=calloc(1,sizeof(*r));assert(r);r->status=reply_status;*p=r;*n=sizeof(*r);*t=bad_type?0:EFS_MSG_INODE_GETATTR_REPLY;if(expire_recv)now=deadline;return fail_recv;}
'''+fn('static int rpc_writer_retry_pause(' ,True)+fn('static int rpc_send_recv_shard(')+fn('static int rpc_send_recv_dual(',True)+r'''
static void reset(void){calls=sleeps=freed=drops=releases=0;reply_status=EFS_INODE_RPC_OK;fail_recv=fail_send=bad_type=expire_recv=0;now=1000;deadline=0;}
static int run(int dual,struct efs_msg_inode_reply*r){return dual?rpc_send_recv_dual(EFS_MSG_INODE_GETATTR,NULL,0,EFS_MSG_INODE_GETATTR_REPLY,r,sizeof(*r),100):rpc_send_recv_shard(0,EFS_MSG_INODE_GETATTR,NULL,0,EFS_MSG_INODE_GETATTR_REPLY,r,sizeof(*r),0,NULL);}
int main(void){for(int d=0;d<2;d++){
 struct efs_msg_inode_reply r;
 reset();assert(!run(d,&r)&&calls==1&&freed==1&&releases==1);
 reset();deadline=1000;assert(run(d,&r)==EFS_ERR_BUSY&&!calls&&!sleeps);
 reset();reply_status=EFS_INODE_RPC_BUSY;deadline=1010;assert(run(d,&r)==EFS_ERR_BUSY&&calls==1&&freed==1&&sleeps==1&&now==1010);
 reset();reply_status=EFS_INODE_RPC_STALE;deadline=1010;assert(run(d,&r)==EFS_ERR_BUSY&&calls==1&&freed==1&&now==1010);
 reset();reply_status=EFS_INODE_RPC_NOT_PRIMARY;deadline=1010;assert(run(d,&r)==EFS_ERR_BUSY&&calls==1&&freed==1&&now==1010);
 reset();fail_recv=1;deadline=1010;assert(run(d,&r)==EFS_ERR_BUSY&&calls==1&&freed==1&&drops==1&&!releases);
 reset();fail_send=1;deadline=1010;assert(run(d,&r)==EFS_ERR_BUSY&&!calls&&drops==1&&sleeps==1);
 reset();deadline=1010;expire_recv=1;memset(&r,0x5a,sizeof(r));struct efs_msg_inode_reply before=r;assert(run(d,&r)==EFS_ERR_BUSY&&freed==1&&!memcmp(&r,&before,sizeof(r)));
 reset();reply_status=EFS_INODE_RPC_BUSY;assert(run(d,&r)==EFS_ERR_BUSY&&calls==16&&freed==16&&sleeps==15);
 }return 0;}
'''
with tempfile.TemporaryDirectory(prefix='efs-metadata-budget-') as d:
 p=Path(d)/'t.c';p.write_text(code)
 subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror','-I'+str(root/'include'),str(p),'-o',str(p.with_suffix(''))],check=True)
 subprocess.run([str(p.with_suffix(''))],check=True,timeout=5)
print('metadata retries: shard/dual budgets, STALE/BUSY/hints, failed payload cleanup and late-output retention PASS')
