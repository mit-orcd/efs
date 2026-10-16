#!/usr/bin/env python3
"""Execute production inbox drain/handoff with a real waiting mutex owner.

Raft receive/decoding are adapters. A completed-batch callback models both
success and a fenced failure; this does not replace durable-store fault gates.
"""
from pathlib import Path
import os, shlex, subprocess, tempfile
root=Path(__file__).resolve().parents[1]
ref=os.environ.get('EFS_HANDOFF_SOURCE_REF')
s=subprocess.check_output(['git','show',ref+':src/server/raft_host.c'],cwd=root,text=True) if ref else (root/'src/server/raft_host.c').read_text()
def fn(signature):
    a=s.index(signature);return s[a:s.index('\n}',a)+2]
helper=fn('static void host_inbox_batch_handoff(') if 'static void host_inbox_batch_handoff(' in s else ''
code=r'''
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#define HOST_INBOX_MAX 8
#define HOST_NGROUPS 2
#define EFS_RAFT_RECV_MAX 2
#define EFS_RAFT_AE_BYTES 256
#define EFS_WIRE_RAFT_HDR_LEN 4
#define EFS_RAFT_FOLLOWER 0
#define EFS_OK 0
#define EFS_ERR_INVAL -1
#define BO_ON() 0
#define BO_COUNT(x) ((void)0)
#define BO_HIST(x,y) ((void)(y))
struct efs_raft {int group;};
struct efs_raft_msg {int group;};
struct host_inbox_item {uint8_t *buf;uint32_t len;uint64_t queued_us;};
struct host_drain_view {uint64_t applied,term;int leader,ready;};
struct efs_raft_host {pthread_mutex_t mu;int group_drain;struct {struct efs_raft*r;int hosted;}g[2];};
static unsigned batches,handoffs,published,wakes,active,fail_batch,probe_batches;
static atomic_int observer_seen;
static int kv_hold=1;
static uint64_t efs_batch_obs_now_us(void){return 0;}
static struct efs_raft *group_raft(struct efs_raft_host*h,int group){return h->g[group/2].r;}
static uint64_t efs_raft_applied(struct efs_raft*r){(void)r;return 0;}
static uint64_t efs_raft_commit(struct efs_raft*r){(void)r;return 0;}
static uint64_t efs_raft_term(struct efs_raft*r){(void)r;return 1;}
static int efs_raft_leader(struct efs_raft*r){(void)r;return 0;}
static int efs_raft_role(struct efs_raft*r){(void)r;return 0;}
static void __attribute__((unused)) host_pending_reads(struct efs_raft_host*h){
 (void)h;assert(!active && kv_hold && batches==probe_batches+1);probe_batches++;
}
static void __attribute__((unused)) host_publish_view(struct efs_raft_host*h,int gi){(void)h;assert(!active && kv_hold && gi>=0 && gi<2);published|=1u<<gi;}
static void __attribute__((unused)) host_waiters_wake(struct efs_raft_host*h){(void)h;assert(published==3 && !active);wakes=batches;}
static void host_inbox_order(const struct host_inbox_item*i,unsigned n,const struct host_drain_view*v,unsigned*o){(void)i;(void)v;for(unsigned j=0;j<n;j++)o[j]=j;}
static int host_inbox_take(struct efs_raft_host*h,struct host_inbox_item*i,unsigned*n,uint64_t*b){
 (void)h;*n=5;*b=25;for(unsigned j=0;j<5;j++){i[j]=(struct host_inbox_item){.buf=calloc(1,5),.len=5};assert(i[j].buf);i[j].buf[0]=(j%2)*2;}return 5;
}
static void host_note_queued_reads(struct efs_raft_host*h,struct host_inbox_item*i,int n){(void)h;(void)i;(void)n;}
static int efs_wire_raft_decode(uint8_t*p,uint32_t n,struct efs_raft_msg*m,uint8_t*c,uint32_t cap){assert(n==5 && cap==1 && c);m->group=p[0];return 0;}
static int efs_raft_recv_batch(struct efs_raft*r,struct efs_raft_msg*m,uint32_t n){
 assert(n==1 && r->group==m->group && !active && kv_hold);active=1;batches++;published=0;
 /* Disk ACK/apply callback finishes or fences before returning. */
 active=0;return batches==fail_batch?-7:0;
}
static int __attribute__((unused)) unlock_seen(pthread_mutex_t*m){
 assert(!active && published==3 && wakes==batches && kv_hold);handoffs++;
 int rc=pthread_mutex_unlock(m);assert(!rc);
 if(handoffs==1)for(unsigned i=0;!atomic_load(&observer_seen) && i<10000;i++)usleep(100);
 assert(atomic_load(&observer_seen));return rc;
}
static int __attribute__((unused)) lock_seen(pthread_mutex_t*m){return pthread_mutex_lock(m);}
#define pthread_mutex_unlock unlock_seen
#define pthread_mutex_lock lock_seen
'''+helper+fn('static void drain_inbox(')+r'''
#undef pthread_mutex_unlock
#undef pthread_mutex_lock
static void *observe(void*p){struct efs_raft_host*h=p;assert(!pthread_mutex_lock(&h->mu));assert(!active && published==3 && wakes>=1 && kv_hold);atomic_store(&observer_seen,1);assert(!pthread_mutex_unlock(&h->mu));return NULL;}
int main(void){
 for(unsigned fault=0;fault<2;fault++){
  struct efs_raft r[2]={{0},{2}};struct efs_raft_host h={.mu=PTHREAD_MUTEX_INITIALIZER,.g={{&r[0],1},{&r[1],1}}};
  batches=handoffs=published=wakes=active=probe_batches=0;fail_batch=fault?2:0;atomic_store(&observer_seen,0);
  assert(!pthread_mutex_lock(&h.mu));pthread_t t;assert(!pthread_create(&t,NULL,observe,&h));
  unsigned n=0;uint64_t bytes=0;drain_inbox(&h,&n,&bytes);
  assert(batches==5 && handoffs==4 && probe_batches==4 && n==5 && bytes==25 && kv_hold);
  assert(!pthread_mutex_unlock(&h.mu));assert(!pthread_join(t,NULL));pthread_mutex_destroy(&h.mu);
 }
 puts("inbox handoff: complete success/fenced failure before admission, published progress, real waiting owner, unchanged KV hold and credits PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='efs-host-handoff-') as d:
    p=Path(d)/'test.c';p.write_text(code)
    subprocess.run(shlex.split(os.environ.get('CC','cc'))+['-std=gnu11','-Wall','-Wextra','-Werror','-pthread',str(p),'-o',str(p.with_suffix(''))],check=True)
    subprocess.run([str(p.with_suffix(''))],check=True,timeout=10)
