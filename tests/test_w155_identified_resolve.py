#!/usr/bin/env python3
"""W155: an identified directory op whose reply was lost resolves by identity.

Production code under test (src/client/inode_rpc.c): the shard and dual
retry loops, their W155 wrappers and rpc_resolve_identified. The server is
a fake whose receipt is the real I16 op-id window (src/sim/opid.c): a
completed identity is answered from the window's reply cache, a new one is
applied once and recorded, exactly as the hosted directory RPCs do.

Faults, each injected deterministically (fake clock):
  M1 applied, reply lost, budget spent   -> same identity replayed, original reply
  M2 request lost before apply           -> same identity applied now, once
  M3 applied, reply lost, cluster down   -> error after the bound, outcome unknown
  M4 GETATTR (no identity), reply lost   -> unchanged: no resolution
for CREATE on the shard and the dual-host paths and RENAME_AT on the shard
path. M1/M2 end with the correct reply and no error.

Negative control: W155_RESOLVE_MS=0 disables the resolution; the test then
fails (the reply-loss cases end in an error).
"""
from pathlib import Path
import os, re, shlex, subprocess, tempfile

root = Path(__file__).resolve().parents[1]
s = (root / 'src/client/inode_rpc.c').read_text()


def fn(prefix, nth=1):
    a = -1
    for _ in range(nth):
        a = s.index(prefix, a + 1)
    return s[a:s.index('\n}', a) + 2] + '\n'


client = ''.join([
    fn('static int rpc_writer_retry_pause(', 2), fn('static void rpc_retry_peer('),
    fn('static int rpc_type_mutates('), fn('static int rpc_identified('),
    fn('static const char *rpc_req_ident('), fn('static uint64_t rpc_resolve_ms('),
    fn('static int rpc_send_recv_shard_once('), fn('static int rpc_resolve_identified('),
    fn('static int rpc_send_recv_shard('), fn('static int rpc_send_recv_dual_once(', 2),
    fn('static int rpc_send_recv_dual(', 2)])

code = r'''
#include "efs/protocol.h"
#include "efs/opid.h"
#include "efs/meta_cmd.h"
#include "metadata_read_route.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
static uint64_t now=1000,deadline;
static int fake_clock(int id,struct timespec*t){(void)id;t->tv_sec=now/1000;t->tv_nsec=(now%1000)*1000000;return 0;}
static int fake_sleep(unsigned us){now+=us/1000;return 0;}
#define clock_gettime fake_clock
#define usleep fake_sleep
uint64_t efs_client_rpc_deadline_ms(void){return deadline;}
int efs_client_rpc_past_deadline(void){return deadline&&now>=deadline;}
static int rpc_past_deadline(void){return efs_client_rpc_past_deadline();}
static void efs_client_rpc_set_deadline_ms(uint64_t d){deadline=d;}
static __thread uint64_t rpc_tl_sent;static __thread unsigned rpc_tl_lost;
static int rpc_send_recv_dual_once(uint8_t,const void*,uint32_t,uint8_t,void*,uint32_t,int);
struct efs_conn {unsigned node;int have;struct efs_msg_inode_reply r;};static struct efs_conn conns[4];
static void rpc_learn_leader(const struct efs_msg_inode_reply*r){(void)r;}
static unsigned g_group_leader[2];
static struct {unsigned node_count;} g_client={3};
static struct efs_meta_read_routes g_read_routes={.mu=PTHREAD_MUTEX_INITIALIZER};
static int up[4]={0,1,1,1};
static int efs_client_node_is_down(efs_node_id_t n){return !up[n];}
static struct efs_conn *efs_client_conn_get(efs_node_id_t n){return n<=3&&up[n]?&conns[n]:NULL;}
static struct efs_conn *raft_voter_conn_skip(uint32_t sh,efs_node_id_t*n,efs_node_id_t skip){(void)sh;
 for(efs_node_id_t i=1;i<=3;i++){if(up[i]&&i!=skip){*n=i;conns[i].node=i;return &conns[i];}}
 *n=0;return NULL;}
static struct efs_conn *raft_dual_voter_conn(efs_node_id_t*n,efs_node_id_t skip){return raft_voter_conn_skip(0,n,skip);}
static void efs_client_conn_drop(efs_node_id_t n,struct efs_conn*c){(void)n;c->have=0;}
static void efs_client_conn_release(efs_node_id_t n,struct efs_conn*c){(void)n;(void)c;}
static int rpc_prof_enabled(void){return 0;}
static unsigned long long rpc_prof_now_us(void){return (unsigned long long)now*1000ull;}
static void rpc_prof_add(int type,unsigned long long a,unsigned long long b,unsigned long long c,unsigned long long d){(void)type;(void)a;(void)b;(void)c;(void)d;}
static void rpc_note_slow_ok(uint8_t t,int a,int b,uint8_t st,unsigned long long n){(void)t;(void)a;(void)b;(void)st;(void)n;}
static int stale_retryable(uint8_t type){(void)type;return 1;}
static void efs_client_node_note_fail(efs_node_id_t n){(void)n;}
static void efs_client_node_note_ok(efs_node_id_t n){(void)n;}
static unsigned efs_raft_shard_group(uint32_t s){(void)s;return 0;}
static unsigned rpc_leader_of(unsigned g){(void)g;return 0;}
static void efs_conn_set_recv_timeout(struct efs_conn*c,int n){(void)c;(void)n;}
static void opid_retry_ack(void *packet,uint32_t length){(void)packet;(void)length;}
static int efs_client_session_check(void){return EFS_OK;}

/* The server: its receipt is the durable op-id window (replicated, so one
 * for the cluster). */
static struct efs_opid_window win;
static unsigned applies,replays,sends,served;
static int drop_request,drop_reply,down_after_first;
int efs_conn_send_msg(struct efs_conn*c,uint8_t t,const void*p,uint32_t n){
 sends++;
 struct efs_msg_inode_reply *r=&c->r;memset(r,0,sizeof(*r));c->have=0;
 if(drop_request){drop_request--;return 0;}
 served++;
 if(t==EFS_MSG_INODE_GETATTR){r->status=EFS_INODE_RPC_OK;r->inode.ino=7;}
 else {
  struct efs_opid_req q;struct efs_opid_reply rep;
  assert(n>EFS_OPID_WIRE_LEN);efs_opid_req_unpack(&q,(const uint8_t*)p+n-EFS_OPID_WIRE_LEN);
  if(efs_opid_lookup(&win,&q.id,&rep)==1){replays++;}
  else {applies++;rep=(struct efs_opid_reply){.seq=q.id.seq,.rc=EFS_OK,.ino=500+applies};
        assert(efs_opid_complete(&win,&q.id,&rep)==0);}
  r->status=rep.rc==EFS_OK?EFS_INODE_RPC_OK:EFS_INODE_RPC_ERROR;r->inode.ino=rep.ino;
 }
 c->have=1;
 if(drop_reply){drop_reply--;c->have=0;}
 if(down_after_first){up[1]=up[2]=up[3]=0;}
 return 0;}
int efs_conn_recv_msg(struct efs_conn*c,uint8_t*t,void**p,uint32_t*n){
 if(!c->have){now+=30000;return -1;} /* receive timeout */
 c->have=0;struct efs_msg_inode_reply*r=malloc(sizeof(*r));assert(r);*r=c->r;*p=r;*n=sizeof(*r);
 *t=EFS_MSG_INODE_CREATE_REPLY;return 0;}
'''+client+r'''
static uint8_t uuid[EFS_OPID_UUID_LEN]={0x13,0x55};
static uint64_t seq;
static void reset(void){now=1000;deadline=0;up[1]=up[2]=up[3]=1;memset(conns,0,sizeof(conns));
 applies=replays=sends=served=0;drop_request=drop_reply=down_after_first=0;efs_opid_window_init(&win,uuid,3);}
static uint32_t packet(uint8_t type,uint8_t *buf){
 struct efs_opid_req q;memset(&q,0,sizeof(q));memcpy(q.id.client_uuid,uuid,EFS_OPID_UUID_LEN);
 q.id.session_epoch=3;q.id.seq=++seq;q.ack=seq-1;
 uint32_t len=type==EFS_MSG_INODE_CREATE?sizeof(struct efs_msg_inode_create):sizeof(struct efs_msg_inode_rename_at);
 memset(buf,0,len);
 if(type==EFS_MSG_INODE_CREATE){((struct efs_msg_inode_create*)buf)->mode=S_IFREG|0644;strcpy(((struct efs_msg_inode_create*)buf)->name,"w155");}
 efs_opid_req_pack(&q,buf+len);return len+EFS_OPID_WIRE_LEN;}
/* path: 0 shard, 1 dual. A 2 s caller budget, as a FUSE callback has. */
static int call(int path,uint8_t type,struct efs_msg_inode_reply*r){
 uint8_t buf[1024];uint32_t n=packet(type,buf);deadline=now+2000;
 uint8_t reply_type=type==EFS_MSG_INODE_CREATE?EFS_MSG_INODE_CREATE_REPLY:EFS_MSG_INODE_RENAME_AT_REPLY;
 int rc=path?rpc_send_recv_dual(type,buf,n,EFS_MSG_INODE_CREATE_REPLY,r,sizeof(*r),100)
            :rpc_send_recv_shard(1,type,buf,n,EFS_MSG_INODE_CREATE_REPLY,r,sizeof(*r),0,NULL);
 (void)reply_type;deadline=0;return rc;}
int main(void){
 struct efs_msg_inode_reply r;int rc;
 for(int path=0;path<2;path++){
  /* M1: applied, reply lost, budget spent: the window answers the original. */
  reset();drop_reply=1;memset(&r,0,sizeof(r));rc=call(path,EFS_MSG_INODE_CREATE,&r);
  assert(rc==EFS_OK && r.status==EFS_INODE_RPC_OK && r.inode.ino==501 && applies==1 && replays==1 && sends==2);
  /* M2: the request never arrived: the same identity applies now, once. */
  reset();drop_request=1;memset(&r,0,sizeof(r));rc=call(path,EFS_MSG_INODE_CREATE,&r);
  assert(rc==EFS_OK && r.status==EFS_INODE_RPC_OK && r.inode.ino==501 && applies==1 && !replays && served==1);
  /* M3: applied, reply lost, then no node answers for the whole bound. */
  reset();drop_reply=1;down_after_first=1;uint64_t t0=now;rc=call(path,EFS_MSG_INODE_CREATE,&r);
  assert((rc==EFS_ERR_BUSY||rc==EFS_ERR_NET) && applies==1 && now-t0>=30000+8000);
  /* M4: an unidentified read keeps its contract: no resolution. */
  reset();drop_reply=1;rc=path?rpc_send_recv_dual(EFS_MSG_INODE_GETATTR,NULL,0,EFS_MSG_INODE_CREATE_REPLY,&r,sizeof(r),100)
                                :rpc_send_recv_shard(0,EFS_MSG_INODE_GETATTR,NULL,0,EFS_MSG_INODE_CREATE_REPLY,&r,sizeof(r),0,NULL);
  (void)0;
 }
 /* RENAME_AT on the shard path: applied, reply lost. */
 reset();drop_reply=1;memset(&r,0,sizeof(r));rc=call(0,EFS_MSG_INODE_RENAME_AT,&r);
 assert(rc==EFS_OK && r.status==EFS_INODE_RPC_OK && applies==1 && replays==1);
 puts("W155 identified ops: applied+lost reply replayed, lost request applied once, unknown after the bound, unidentified unchanged, CREATE shard/dual and RENAME_AT PASS");
 return 0;}
'''

with tempfile.TemporaryDirectory(prefix='efs-w155-ident-') as d:
    p = Path(d) / 't.c'
    p.write_text(code)
    cc = shlex.split(os.environ.get('CC', 'cc'))
    extra = ['-fsanitize=address,undefined', '-fno-omit-frame-pointer'] if os.environ.get('EFS_SANITIZE') == '1' else []
    subprocess.run(cc + ['-std=gnu11', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function',
                         '-I' + str(root / 'include'), '-I' + str(root / 'src/client'), *extra,
                         str(p), str(root / 'src/sim/opid.c'), '-pthread',
                         '-o', str(p.with_suffix(''))], check=True)
    out = subprocess.run([str(p.with_suffix(''))], check=True, timeout=30, capture_output=True,
                         text=True, env=dict(os.environ, EFS_RESOLVE_MS=os.environ.get('W155_RESOLVE_MS', '8000')))
    log = out.stderr
    assert re.search(r'inode-rpc: retry type=45 attempt=0 why=recv rc=-?\d+ nid=1 id=13550000/3/\d+', log), log
    assert re.search(r'inode-rpc: resolve type=45 id=13550000/3/\d+ rc=-13 lost=1: reply lost', log), log
    assert len(re.findall(r'inode-rpc: resolve type=45 id=\S+ outcome=answered status=0', log)) == 4, log
    assert len(re.findall(r'inode-rpc: resolve type=45 id=\S+ outcome=unknown', log)) == 2, log
    assert re.search(r'inode-rpc: resolve type=87 id=\S+ outcome=answered status=0', log), log
    assert 'resolve type=47' not in log, log  # GETATTR is never resolved
    print(out.stdout.strip())
