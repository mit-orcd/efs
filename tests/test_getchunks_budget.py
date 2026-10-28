#!/usr/bin/env python3
"""Production GETCHUNKS retries retain outputs and release all reply ownership."""
from pathlib import Path
import os,shlex,subprocess,tempfile
from sanitize_env import cflags, wrap, timeout
root=Path(__file__).resolve().parents[1];s=(root/'src/client/inode_rpc.c').read_text()
def fn(prefix):
 a=s.index(prefix);return s[a:s.index('\n}',a)+2]
code=r'''
#include "efs/protocol.h"
#include "efs/kv_key.h"
#include <assert.h>
#include <stdlib.h>
#include <time.h>
static void efs_client_epoch_note(efs_ino_t i,uint64_t e){(void)i;(void)e;}
uint32_t efs_export_shard_of(efs_ino_t ino,uint32_t bits) {(void)ino;(void)bits;return 0;}
int efs_chunk_size_valid(uint32_t cs) {return cs==EFS_MIN_CHUNK_SIZE;}
static struct { efs_export_id_t export_id; } g_client = {1};
static unsigned request_max=2, object_view;
static uint8_t reply_status;
static uint64_t now=1000,deadline;static unsigned calls,sleeps,freed,drops,releases;
static int busy,fail_recv,bad_type,expire_recv,fail_once,fail_send,no_conn,not_primary;
static unsigned selections;static efs_node_id_t last_skip;
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
static struct efs_conn *raft_voter_conn_skip(uint32_t shard,efs_node_id_t *n,efs_node_id_t skip) {(void)shard;selections++;last_skip=skip;*n=skip==1?2:1;return no_conn?NULL:&connection;}
static void efs_client_conn_drop(efs_node_id_t n,struct efs_conn *c) {(void)n;(void)c;drops++;}
static void efs_client_conn_release(efs_node_id_t n,struct efs_conn *c) {(void)n;(void)c;releases++;}
static int rpc_prof_enabled(void) {return 0;}
static unsigned long long rpc_prof_now_us(void) {return now*1000;}
static void rpc_prof_add(int type,unsigned long long a,unsigned long long b,unsigned long long c,unsigned long long d) {(void)type;(void)a;(void)b;(void)c;(void)d;}
static int rpc_status_to_efs(uint8_t status) {return status==EFS_INODE_RPC_OK?EFS_OK:status==EFS_INODE_RPC_NOT_FOUND?EFS_ERR_NOT_FOUND:status==EFS_INODE_RPC_STALE?EFS_ERR_STALE:EFS_ERR_BUSY;}
int efs_conn_send_msg(struct efs_conn *c,uint8_t type,const void *p,uint32_t n) {(void)c;assert(type==EFS_MSG_INODE_GETCHUNKS&&n==sizeof(struct efs_msg_inode_getchunks));const struct efs_msg_inode_getchunks *r=p;assert(r->ino==123&&r->generation==456&&r->start==0&&r->max==request_max);return fail_send;}
static void fill_object_view(struct efs_msg_inode_getchunks_reply *r) {
 if(!object_view)return;
 r->count=1;
 struct efs_chunk_rec *c=&r->recs[0];
 c->ino=123;c->chunk_generation=object_view==3?888:777;
 c->read_view.chunk_size=EFS_MIN_CHUNK_SIZE;c->read_view.count=1;
 c->read_view.parts[0].len=EFS_MIN_CHUNK_SIZE;
 if(object_view==2 || object_view==6) {
  c->chunk_generation=888;c->delta_base_n=1;c->read_view.count=2;
  c->deltas[0].generation=777;c->deltas[0].off=1;c->deltas[0].len=3;
  c->read_view.parts[1].off=1;c->read_view.parts[1].len=object_view==6?0:3;
 }
 if(object_view==4) {
  c->publish_flags=EFS_CHUNK_REC_F_PACKED;
  c->packed_ref[7]=123;c->packed_ref[30]=3;c->packed_ref[31]=9;
 }
 if(object_view==5)c->read_view.parts[0].len=0;
}
int efs_conn_recv_msg(struct efs_conn *c,uint8_t *type,void **p,uint32_t *n) {
 (void)c;calls++;struct efs_msg_inode_getchunks_reply *r=calloc(1,sizeof(*r));assert(r);
 r->ino=123;r->generation=456;r->status=not_primary && calls==1?EFS_INODE_RPC_NOT_PRIMARY:busy?EFS_INODE_RPC_BUSY:reply_status;
 r->primary_id=not_primary==2?1:0;fill_object_view(r);
 *p=r;*n=efs_getchunks_reply_len(r->count);*type=bad_type?EFS_MSG_VERSION_REPLY:EFS_MSG_INODE_GETCHUNKS_REPLY;
 if(expire_recv)now=deadline;
 return fail_recv||(fail_once&&calls==1);
}
'''+fn('static int rpc_writer_retry_pause(unsigned attempt)\n{')+fn('int efs_client_rpc_getchunks_fileid(')+fn('int efs_client_rpc_object_live(')+r'''
static void reset(void) {calls=sleeps=freed=drops=releases=0;busy=fail_recv=bad_type=expire_recv=fail_once=fail_send=no_conn=not_primary=0;selections=last_skip=0;now=1000;deadline=0;request_max=2;object_view=reply_status=0;}
static int run(uint32_t *count,uint64_t *gen) {struct efs_chunk_rec r[2];return efs_client_rpc_getchunks_fileid(1,123,456,0,r,count,gen);}
int main(void) {
 uint32_t n=2;uint64_t gen=999;reset();assert(!run(&n,&gen)&&n==0&&gen==456&&calls==1&&freed==1);
 reset();n=2;gen=999;deadline=1000;assert(run(&n,&gen)==EFS_ERR_BUSY&&n==2&&gen==999&&!calls);
 reset();busy=1;deadline=1010;assert(run(&n,&gen)==EFS_ERR_BUSY&&calls==1&&freed==1&&sleeps==1&&now==1010&&n==2&&gen==999);
 reset();fail_recv=1;assert(run(&n,&gen)==EFS_ERR_NET&&calls==16&&freed==16&&drops==16&&!releases&&sleeps==15&&n==2&&gen==999);
 reset();fail_once=1;assert(!run(&n,&gen)&&calls==2&&freed==2&&drops==1&&releases==1&&last_skip==1&&n==0&&gen==456);
 reset();n=2;gen=999;fail_send=1;assert(run(&n,&gen)==EFS_ERR_NET&&!calls&&drops==16&&sleeps==15&&n==2&&gen==999);
 reset();no_conn=1;assert(run(&n,&gen)==EFS_ERR_NET&&!calls&&!drops&&selections==16&&sleeps==15);
 reset();fail_recv=1;deadline=1010;assert(run(&n,&gen)==EFS_ERR_BUSY&&calls==1&&freed==1&&drops==1&&sleeps==1&&now==1010&&n==2&&gen==999);
 reset();not_primary=1;assert(!run(&n,&gen)&&calls==2&&last_skip==1&&sleeps==1);
 reset();n=2;gen=999;not_primary=2;assert(!run(&n,&gen)&&calls==2&&last_skip==1&&sleeps==1);
 reset();n=2;gen=999;bad_type=1;assert(run(&n,&gen)==EFS_ERR_PROTO&&freed==1);
 reset();deadline=1010;expire_recv=1;assert(run(&n,&gen)==EFS_ERR_BUSY&&freed==1&&n==2&&gen==999);
 reset();busy=1;assert(run(&n,&gen)==EFS_ERR_BUSY&&calls==16&&freed==16&&sleeps==15);
 /* The exact production retirement read uses a one-record FileID-bound
  * request. A live base/span prevents retirement; packing and irreversible
  * zero-length fence masks do not name the old physical object. */
 for(unsigned view=0;view<=6;view++) {
  reset();request_max=1;object_view=view;int live=9;
  assert(!efs_client_rpc_object_live(123,456,0,777,&live));
  assert(live==(view==1 || view==2) && calls==1 && freed==1);
 }
 reset();request_max=1;reply_status=EFS_INODE_RPC_NOT_FOUND;int live=9;
 assert(!efs_client_rpc_object_live(123,456,0,777,&live) && live==0);
 reset();request_max=1;reply_status=EFS_INODE_RPC_STALE;live=9;
 assert(efs_client_rpc_object_live(123,456,0,777,&live)==EFS_ERR_STALE && live==9);
 reset();request_max=1;bad_type=1;live=9;
 assert(efs_client_rpc_object_live(123,456,0,777,&live)==EFS_ERR_PROTO && live==9);
 reset();request_max=1;deadline=1010;expire_recv=1;live=9;
 assert(efs_client_rpc_object_live(123,456,0,777,&live)==EFS_ERR_BUSY && live==9);
 assert(efs_client_rpc_object_live(0,456,0,777,&live)==EFS_ERR_INVAL);
 assert(efs_client_rpc_object_live(123,0,0,777,&live)==EFS_ERR_INVAL);
 assert(efs_client_rpc_object_live(123,456,0,0,&live)==EFS_ERR_INVAL);
 assert(efs_client_rpc_object_live(123,456,0,777,NULL)==EFS_ERR_INVAL);
 reset();live=9;
 assert(efs_client_rpc_object_live(123,456,UINT32_MAX,777,&live)==EFS_ERR_BUSY && live==9 && !calls);
 assert(efs_client_rpc_object_live(123,456,UINT32_MAX-EFS_CHUNK_GROUP_SIZE+1,777,&live)==EFS_ERR_BUSY && live==9 && !calls);
 return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='efs-getchunks-budget-') as d:
 p=Path(d)/'t.c';p.write_text(code)
 subprocess.run(shlex.split(os.environ.get('CC','cc'))+cflags()+['-std=gnu11','-Wall','-Wextra','-Werror','-I'+str(root/'include'),str(p),str(root/'src/kv/kv_key.c'),'-o',str(p.with_suffix(''))],check=True,timeout=timeout(60))
 subprocess.run(wrap()+[str(p.with_suffix(''))],check=True,timeout=timeout(30))
print('GETCHUNKS RPC: bounded endpoint-loss/no-hint retries, exact FileID, whole deadline, output retention, payload cleanup and read-only retired-object base/span/packed/fence checks PASS')
