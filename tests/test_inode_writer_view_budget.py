#!/usr/bin/env python3
"""Actual inode writer-view RPC bounds retries and refuses expired authority."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[1];s=(root/'src/client/inode_rpc.c').read_text()
def fn(p):
 a=s.index(p);return s[a:s.index('\n}',a)+2]
code=r'''
#include "efs/protocol.h"
#include "efs/kv_key.h"
#include <assert.h>
#include <stdlib.h>
#include <time.h>
static uint64_t now,deadline;static unsigned calls,sleeps,freed;static int busy,expire;
struct efs_conn{int unused;};static struct efs_conn connection;
uint64_t efs_client_rpc_deadline_ms(void){return deadline;}
int efs_client_rpc_past_deadline(void){return deadline&&now>=deadline;}
static int fake_clock(int id,struct timespec*t){(void)id;t->tv_sec=now/1000;t->tv_nsec=(now%1000)*1000000;return 0;}
static int fake_sleep(unsigned us){now+=us/1000;sleeps++;return 0;}
static void test_free(void*p){if(p)freed++;free(p);}
#define clock_gettime fake_clock
#define usleep fake_sleep
#define free test_free
static struct efs_conn *efs_client_conn_get(efs_node_id_t n){(void)n;return &connection;}
static struct efs_conn *rpc_owner_conn_shard(uint32_t s,efs_node_id_t*n){(void)s;*n=1;return &connection;}
static void efs_client_conn_drop(efs_node_id_t n,struct efs_conn*c){(void)n;(void)c;}
static void efs_client_conn_release(efs_node_id_t n,struct efs_conn*c){(void)n;(void)c;}
static int rpc_status_to_efs(uint8_t s){return s==EFS_INODE_RPC_OK?EFS_OK:EFS_ERR_BUSY;}
int efs_conn_send_msg(struct efs_conn*c,uint8_t t,const void*p,uint32_t n){(void)c;(void)p;assert(t==EFS_MSG_INODE_WRITER_VIEW&&n==sizeof(struct efs_msg_inode_writer_view));return 0;}
int efs_conn_recv_msg(struct efs_conn*c,uint8_t*t,void**p,uint32_t*n){(void)c;calls++;struct efs_msg_inode_writer_view_reply*r=calloc(1,sizeof(*r));r->ino=123;r->generation=456;r->chunk_index=17;r->status=busy?EFS_INODE_RPC_BUSY:EFS_INODE_RPC_OK;*p=r;*t=EFS_MSG_INODE_WRITER_VIEW_REPLY;*n=sizeof(*r);if(expire)now=deadline;return 0;}
'''+fn('static int rpc_writer_retry_pause(unsigned attempt)\n{')+fn('int efs_client_rpc_writer_view(')+r'''
static void reset(void){now=1000;deadline=0;calls=sleeps=freed=0;busy=expire=0;}
int main(void){struct efs_msg_inode_writer_view_reply out;
 reset();assert(!efs_client_rpc_writer_view(123,456,17,&out)&&calls==1&&freed==1&&out.generation==456);
 reset();deadline=1000;assert(efs_client_rpc_writer_view(123,456,17,&out)==EFS_ERR_BUSY&&!calls);
 reset();busy=1;deadline=1010;assert(efs_client_rpc_writer_view(123,456,17,&out)==EFS_ERR_BUSY&&calls==1&&freed==1&&sleeps==1&&now==1010);
 reset();expire=1;deadline=1010;memset(&out,0x5a,sizeof(out));struct efs_msg_inode_writer_view_reply saved=out;assert(efs_client_rpc_writer_view(123,456,17,&out)==EFS_ERR_BUSY&&freed==1&&!memcmp(&out,&saved,sizeof(out)));
 reset();busy=1;assert(efs_client_rpc_writer_view(123,456,17,&out)==EFS_ERR_BUSY&&calls==8&&freed==8&&sleeps==7);return 0;}
'''
with tempfile.TemporaryDirectory(prefix='efs-inode-view-') as d:
 p=Path(d)/'t.c';p.write_text(code)
 subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror','-I'+str(root/'include'),str(p),'-o',str(p.with_suffix(''))],check=True)
 subprocess.run([str(p.with_suffix(''))],check=True,timeout=5)
print('inode writer view: expired admission, bounded BUSY, late authority rejection and reply ownership PASS')
