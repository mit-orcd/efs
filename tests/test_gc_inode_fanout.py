#!/usr/bin/env python3
"""Compile the actual W124 all-member reclaim handoff: every remote request is
sent before the local acceptance and before any reply is read; an item counts
only when every member accepted it; failures and malformed replies refuse the
whole batch and drain every connection."""
from pathlib import Path
import os,shlex,subprocess,tempfile
root=Path(__file__).resolve().parents[1]
s=(root/'src/server/raft_host.c').read_text()
a=s.index('static void host_gc_inode_nodes_batch(');b=s.index('/* Propose one GC command',a)
code=r'''
#include "efs/common.h"
#include <assert.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#define HOST_SEND_IO_MS 250
#define HOST_GC_BATCH_IO_MS 10000
#define EFS_CONN_TCP 1
#define EFS_MSG_GC_INODE_BATCH 1
#define EFS_MSG_GC_INODE_BATCH_REPLY 2
struct efs_conn { int kind,fd; };
struct server { pthread_mutex_t lock; unsigned node_count; struct efs_node nodes[EFS_MAX_NODES]; efs_node_id_t id; };
struct efs_raft_host { struct server *s; int n; };
struct efs_msg_gc_inode_batch { uint32_t export_id,count; struct { uint64_t ino,generation; } items[]; };
struct efs_msg_gc_inode_batch_reply { uint32_t count,reserved; int32_t rc[]; };
struct efs_reclaim_item { uint32_t export_id; efs_ino_t ino; uint64_t generation; };
#define N 5
static struct efs_conn conns[4];
static int sent,received,local_done,released,dropped,failed_send,failed_recv,busy_node,busy_item,
           wrong_type,wrong_size,wrong_count,missing,local_busy_item,expected_sends=3;
static struct efs_conn *server_peer_conn_get(const char *a,unsigned p) { (void)a;return (int)p==missing?NULL:&conns[p]; }
static void efs_set_recv_timeout(int fd,int ms) { (void)fd;(void)ms; }
static void efs_set_send_timeout(int fd,int ms) { (void)fd;(void)ms; }
static int efs_conn_send_msg(struct efs_conn *c,int type,const void *v,unsigned n) {
 const struct efs_msg_gc_inode_batch *r=v;
 assert(!local_done && !received && type==EFS_MSG_GC_INODE_BATCH && n==sizeof(*r)+N*16);
 assert(r->export_id==1 && r->count==N);
 for(unsigned i=0;i<N;i++) assert(r->items[i].ino==70+i && r->items[i].generation==90+i);
 sent++;
 return c->fd==failed_send?-1:0;
}
static int server_reclaim_accept(struct server *s,const struct efs_reclaim_item *it,uint32_t n,int32_t *rc) {
 (void)s;assert(sent==expected_sends-(missing>=1&&missing<=3));assert(!received&&n==N);
 for(unsigned i=0;i<n;i++){assert(it[i].export_id==1&&it[i].ino==70+i&&it[i].generation==90+i);
  rc[i]=(int)i==local_busy_item?EFS_ERR_BUSY:EFS_OK;}
 local_done=1;return EFS_OK;
}
static int efs_conn_recv_msg(struct efs_conn *c,uint8_t *type,void **v,uint32_t *n) {
 assert(local_done);received++;if(c->fd==failed_recv)return -1;
 struct efs_msg_gc_inode_batch_reply *r=malloc(sizeof(*r)+N*4);assert(r);
 r->count=c->fd==wrong_count?N-1:N;r->reserved=0;
 for(unsigned i=0;i<N;i++) r->rc[i]=(c->fd==busy_node&&(int)i==busy_item)?EFS_ERR_BUSY:EFS_OK;
 *v=r;*n=c->fd==wrong_size?sizeof(*r):sizeof(*r)+N*4;*type=c->fd==wrong_type?0:EFS_MSG_GC_INODE_BATCH_REPLY;return 0;
}
static void server_peer_conn_drop(const char *a,unsigned p,struct efs_conn *c) { (void)a;(void)p;(void)c;dropped++; }
static void server_peer_conn_release(const char *a,unsigned p,struct efs_conn *c) { (void)a;(void)p;(void)c;released++; }
'''+s[a:b]+r'''
static efs_ino_t inos[N]={70,71,72,73,74};
static uint64_t gens[N]={90,91,92,93,94};
static uint8_t ok[N];
static int accepted(void){int k=0;for(unsigned i=0;i<N;i++)k+=ok[i];return k;}
static int exercise(void) {
 struct server server={.lock=PTHREAD_MUTEX_INITIALIZER,.node_count=4,.id=1};
 struct efs_raft_host h={&server,4};
 sent=received=local_done=released=dropped=0;
 for(unsigned i=0;i<4;i++){server.nodes[i].id=i+1;server.nodes[i].port=i;conns[i].fd=i;conns[i].kind=EFS_CONN_TCP;}
 memset(ok,0xee,sizeof(ok));
 host_gc_inode_nodes_batch(&h,inos,gens,N,ok);
 assert(released+dropped==3-(missing>=1&&missing<=3));
 pthread_mutex_destroy(&server.lock);return accepted();
}
int main(void) {
 failed_send=failed_recv=busy_node=busy_item=wrong_type=wrong_size=wrong_count=missing=local_busy_item=-1;
 assert(exercise()==N && sent==3 && received==3 && released==3);
 busy_node=2;busy_item=3;assert(exercise()==N-1 && !ok[3] && ok[4] && released==3);busy_node=busy_item=-1;
 local_busy_item=0;assert(exercise()==N-1 && !ok[0]);local_busy_item=-1;
 failed_send=1;assert(exercise()==0 && received==2 && dropped==1);failed_send=-1;
 failed_recv=2;assert(exercise()==0 && received==3 && dropped==1);failed_recv=-1;
 wrong_type=1;assert(exercise()==0 && dropped==1);wrong_type=-1;
 wrong_size=3;assert(exercise()==0 && dropped==1);wrong_size=-1;
 wrong_count=3;assert(exercise()==0 && dropped==1);wrong_count=-1;
 missing=3;assert(exercise()==0 && sent==2 && received==2);missing=-1;
 /* Incomplete membership proves nothing: no RPC, no local acceptance. */
 struct server server={.lock=PTHREAD_MUTEX_INITIALIZER,.node_count=3,.id=1};struct efs_raft_host h={&server,4};
 for(unsigned i=0;i<3;i++){server.nodes[i].id=i+1;server.nodes[i].port=i;}
 sent=received=local_done=0;memset(ok,1,sizeof(ok));
 host_gc_inode_nodes_batch(&h,inos,gens,N,ok);assert(!accepted() && !sent && !local_done);
 server.node_count=0;memset(ok,1,sizeof(ok));
 host_gc_inode_nodes_batch(&h,inos,gens,N,ok);assert(!accepted() && !sent);
 pthread_mutex_destroy(&server.lock);
 puts("GC reclaim handoff: all sends before local accept and waits, per-item all-member verdict, failure/malformed replies refuse the batch PASS");
}
'''
# Only inodes every member accepted may complete their REAP marker.
reap=s[s.index('static void host_gc_reap_pass('):s.index('static int host_gc_ack_flush(')]
assert 'if (!host_reap_ok[k]) continue;' in reap
assert 'host_gc_inode_nodes_batch(h, host_reap_inos, host_reap_gens, m, host_reap_ok);' in reap
with tempfile.TemporaryDirectory(prefix='efs-gc-fanout-') as temp:
 p=Path(temp)/'test.c';p.write_text(code)
 command=shlex.split(os.environ.get('CC','cc'))+['-std=gnu11','-Wall','-Wextra','-Werror','-pthread','-I'+str(root/'include'),str(p),'-o',str(p.with_suffix(''))]
 subprocess.run(command,check=True);subprocess.run([str(p.with_suffix(''))],check=True)
