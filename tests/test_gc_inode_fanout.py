#!/usr/bin/env python3
"""Compile actual all-member GC fence: pipeline requests, retain failed authority."""
from pathlib import Path
import os,shlex,subprocess,tempfile
root=Path(__file__).resolve().parents[1]
s=(root/'src/server/raft_host.c').read_text();a=s.index('static int host_gc_inode_nodes(');b=s.index('/* Propose one GC command',a)
code=r'''
#include "efs/common.h"
#include <assert.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#define HOST_SEND_IO_MS 250
#define EFS_CONN_TCP 1
#define EFS_MSG_GC_INODE 1
#define EFS_MSG_GC_INODE_REPLY 2
struct efs_conn { int kind,fd; };
struct server { pthread_mutex_t lock; unsigned node_count; struct efs_node nodes[EFS_MAX_NODES]; efs_node_id_t id; };
struct efs_raft_host { struct server *s; int n; };
struct efs_msg_gc_inode { unsigned export_id; efs_ino_t ino; uint64_t generation; };
struct efs_msg_gc_inode_reply { int rc; };
static struct efs_conn conns[4];
static int sent,received,local_done,released,dropped,failed_send,failed_recv,busy,wrong_type,wrong_size,missing,local_rc,expected_sends=3;
static struct efs_conn *server_peer_conn_get(const char *a,unsigned p) { (void)a;return (int)p==missing?NULL:&conns[p]; }
static void efs_set_recv_timeout(int fd,int ms) { (void)fd;(void)ms; }
static void efs_set_send_timeout(int fd,int ms) { (void)fd;(void)ms; }
static int efs_conn_send_msg(struct efs_conn *c,int type,const void *v,unsigned n) {
 const struct efs_msg_gc_inode *r=v;
 assert(!local_done && !received && type==EFS_MSG_GC_INODE && n==sizeof(*r));
 assert(r->export_id==1 && r->ino==77 && r->generation==99);sent++;
 return c->fd==failed_send?-1:0;
}
static int server_gc_inode(struct server *s,unsigned eid,efs_ino_t ino,uint64_t gen) {
 (void)s;assert(sent==expected_sends-(missing>=1&&missing<=3));assert(eid==1&&ino==77&&gen==99);assert(!received);local_done=1;return local_rc;
}
static int efs_conn_recv_msg(struct efs_conn *c,uint8_t *type,void **v,uint32_t *n) {
 assert(local_done);received++;if(c->fd==failed_recv)return -1;
 struct efs_msg_gc_inode_reply *r=malloc(sizeof(*r));assert(r);r->rc=c->fd==busy?EFS_ERR_BUSY:EFS_OK;
 *v=r;*n=c->fd==wrong_size?0:sizeof(*r);*type=c->fd==wrong_type?0:EFS_MSG_GC_INODE_REPLY;return 0;
}
static void server_peer_conn_drop(const char *a,unsigned p,struct efs_conn *c) { (void)a;(void)p;(void)c;dropped++; }
static void server_peer_conn_release(const char *a,unsigned p,struct efs_conn *c) { (void)a;(void)p;(void)c;released++; }
'''+s[a:b]+r'''
static int exercise(void) {
 struct server server={.lock=PTHREAD_MUTEX_INITIALIZER,.node_count=4,.id=1};
 struct efs_raft_host h={&server,4};
 sent=received=local_done=released=dropped=0;
 for(unsigned i=0;i<4;i++){server.nodes[i].id=i+1;server.nodes[i].port=i;conns[i].fd=i;conns[i].kind=EFS_CONN_TCP;}
 int rc=host_gc_inode_nodes(&h,77,99);assert(released+dropped==3-(missing>=1&&missing<=3));
 pthread_mutex_destroy(&server.lock);return rc;
}
int main(void) {
 failed_send=failed_recv=busy=wrong_type=wrong_size=missing=-1;
 assert(exercise()==EFS_OK && sent==3 && received==3 && released==3);
 busy=2;assert(exercise()==EFS_ERR_BUSY && received==3 && released==3);busy=-1;
 failed_send=1;assert(exercise()==EFS_ERR_IO && received==2 && dropped==1);failed_send=-1;
 failed_recv=2;assert(exercise()==EFS_ERR_IO && received==3 && dropped==1);failed_recv=-1;
 wrong_type=1;assert(exercise()==EFS_ERR_IO && dropped==1);wrong_type=-1;
 wrong_size=3;assert(exercise()==EFS_ERR_IO && dropped==1);wrong_size=-1;
 missing=3;assert(exercise()==EFS_ERR_IO && sent==2 && received==2);missing=-1;
 local_rc=EFS_ERR_BUSY;assert(exercise()==EFS_ERR_BUSY && received==3);
 local_rc=EFS_ERR_IO;assert(exercise()==EFS_ERR_IO && received==3);local_rc=0;
 struct server server={.lock=PTHREAD_MUTEX_INITIALIZER,.node_count=3,.id=1};struct efs_raft_host h={&server,4};
 for(unsigned i=0;i<3;i++){server.nodes[i].id=i+1;server.nodes[i].port=i;}
 expected_sends=2;sent=received=local_done=released=dropped=0;
 assert(host_gc_inode_nodes(&h,77,99)==EFS_ERR_BUSY && received==2);
 server.node_count=0;
 assert(host_gc_inode_nodes(&h,77,99)==EFS_ERR_BUSY);pthread_mutex_destroy(&server.lock);
 puts("GC inode fanout: all sends before waits, every member required, failure/BUSY/malformed replies drained PASS");
}
'''
# A failed all-member fence must retain its durable retry record.
reap=s[s.index('static void host_gc_reap_pass('):s.index('static int host_gc_ack_flush(')]
assert 'if (frc != EFS_OK)\n            continue;' in reap
with tempfile.TemporaryDirectory(prefix='efs-gc-fanout-') as temp:
 p=Path(temp)/'test.c';p.write_text(code)
 command=shlex.split(os.environ.get('CC','cc'))+['-std=gnu11','-Wall','-Wextra','-Werror','-pthread','-I'+str(root/'include'),str(p),'-o',str(p.with_suffix(''))]
 subprocess.run(command,check=True);subprocess.run([str(p.with_suffix(''))],check=True)
