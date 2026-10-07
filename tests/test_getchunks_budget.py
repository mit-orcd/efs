#!/usr/bin/env python3
"""Production GETCHUNKS retries retain outputs and release all reply ownership."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[1];s=(root/'src/client/inode_rpc.c').read_text()
def fn(prefix):
 a=s.index(prefix);return s[a:s.index('\n}',a)+2]
code=r'''
#include "efs/protocol.h"
#include "efs/kv_key.h"
#include <assert.h>
#include <stdlib.h>
#include <time.h>
uint32_t efs_export_shard_of(efs_ino_t ino,uint32_t bits) {(void)ino;(void)bits;return 0;}
int efs_chunk_size_valid(uint32_t cs) {return cs==EFS_MIN_CHUNK_SIZE;}
static uint64_t now=1000,deadline;static unsigned calls,sleeps,freed,drops,releases;
static int busy,fail_recv,bad_type,expire_recv;
struct efs_conn {int unused;};static struct efs_conn connection;
uint64_t efs_client_rpc_deadline_ms(void) {return deadline;}
int efs_client_rpc_past_deadline(void) {return deadline&&now>=deadline;}
static int fake_clock(int id,struct timespec *t) {(void)id;t->tv_sec=now/1000;t->tv_nsec=(now%1000)*1000000;return 0;}
static int fake_sleep(unsigned us) {now+=us/1000;sleeps++;return 0;}
static void test_free(void *p) {if(p)freed++;free(p);}
#define free test_free
#define clock_gettime fake_clock
#define usleep fake_sleep
static struct efs_conn *efs_client_conn_get(efs_node_id_t n) {(void)n;return &connection;}
static struct efs_conn *rpc_owner_conn_shard(uint32_t shard,efs_node_id_t *n) {(void)shard;*n=1;return &connection;}
static void efs_client_conn_drop(efs_node_id_t n,struct efs_conn *c) {(void)n;(void)c;drops++;}
static void efs_client_conn_release(efs_node_id_t n,struct efs_conn *c) {(void)n;(void)c;releases++;}
static int rpc_prof_enabled(void) {return 0;}
static unsigned long long rpc_prof_now_us(void) {return now*1000;}
static void rpc_prof_add(int type,unsigned long long a,unsigned long long b,unsigned long long c,unsigned long long d) {(void)type;(void)a;(void)b;(void)c;(void)d;}
static int rpc_status_to_efs(uint8_t status) {return status==EFS_INODE_RPC_OK?EFS_OK:EFS_ERR_BUSY;}
int efs_conn_send_msg(struct efs_conn *c,uint8_t type,const void *p,uint32_t n) {(void)c;assert(type==EFS_MSG_INODE_GETCHUNKS&&n==sizeof(struct efs_msg_inode_getchunks));const struct efs_msg_inode_getchunks *r=p;assert(r->ino==123&&r->generation==456&&r->start==0&&r->max==2);return 0;}
int efs_conn_recv_msg(struct efs_conn *c,uint8_t *type,void **p,uint32_t *n) {(void)c;calls++;struct efs_msg_inode_getchunks_reply *r=calloc(1,sizeof(*r));assert(r);r->ino=123;r->generation=456;r->status=busy?EFS_INODE_RPC_BUSY:EFS_INODE_RPC_OK;*p=r;*n=sizeof(*r);*type=bad_type?EFS_MSG_VERSION_REPLY:EFS_MSG_INODE_GETCHUNKS_REPLY;if(expire_recv)now=deadline;return fail_recv;}
'''+fn('static int rpc_writer_retry_pause(')+fn('int efs_client_rpc_getchunks_fileid(')+r'''
static void reset(void) {calls=sleeps=freed=drops=releases=0;busy=fail_recv=bad_type=expire_recv=0;now=1000;deadline=0;}
static int run(uint32_t *count,uint64_t *gen) {struct efs_chunk_rec r[2];return efs_client_rpc_getchunks_fileid(1,123,456,0,r,count,gen);}
int main(void) {
 uint32_t n=2;uint64_t gen=999;reset();assert(!run(&n,&gen)&&n==0&&gen==456&&calls==1&&freed==1);
 reset();n=2;gen=999;deadline=1000;assert(run(&n,&gen)==EFS_ERR_BUSY&&n==2&&gen==999&&!calls);
 reset();busy=1;deadline=1010;assert(run(&n,&gen)==EFS_ERR_BUSY&&calls==1&&freed==1&&sleeps==1&&now==1010&&n==2&&gen==999);
 reset();fail_recv=1;assert(run(&n,&gen)==EFS_ERR_NET&&calls==1&&freed==1&&drops==1&&!releases);
 reset();bad_type=1;assert(run(&n,&gen)==EFS_ERR_PROTO&&freed==1);
 reset();deadline=1010;expire_recv=1;assert(run(&n,&gen)==EFS_ERR_BUSY&&freed==1&&n==2&&gen==999);
 reset();busy=1;assert(run(&n,&gen)==EFS_ERR_BUSY&&calls==16&&freed==16&&sleeps==15);
 return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='efs-getchunks-budget-') as d:
 p=Path(d)/'t.c';p.write_text(code)
 subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror','-I'+str(root/'include'),str(p),str(root/'src/kv/kv_key.c'),'-o',str(p.with_suffix(''))],check=True)
 subprocess.run([str(p.with_suffix(''))],check=True)
print('GETCHUNKS RPC: whole retry budget, output retention and receive-payload cleanup PASS')
