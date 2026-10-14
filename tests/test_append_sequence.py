#!/usr/bin/env python3
"""Actual append identity lifecycle and shared contiguous acknowledgement."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[1]
s=(root/'src/client/inode_rpc.c').read_text()
def fn(name):
 a=s.index(name); return s[a:s.index('\n}',a)+2]
code=r'''
#include "efs/opid.h"
#include <stdint.h>
#include <stdlib.h>
#include <assert.h>
#include <pthread.h>
#include <string.h>
#include <unistd.h>
#include <stdio.h>
#define EFS_OPID_INFLIGHT 512
#define N 8
#define M 100000
static struct {uint64_t opid_next,opid_inflight[EFS_OPID_INFLIGHT],flock_token;
 uint8_t opid_uuid[16];uint32_t opid_epoch;} g_client={.opid_next=1,.opid_epoch=1,.opid_uuid={1,[15]=77},.flock_token=42};
static pthread_mutex_t opid_mu=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t opid_cv=PTHREAD_COND_INITIALIZER;
int efs_client_rpc_past_deadline(void) {return 0;}
uint64_t efs_client_rpc_deadline_ms(void) {return 0;}
static void opid_seed_locked(void) {}
static uint64_t values[N][M];
'''+fn('static int opid_take_locked(')+fn('static int opid_begin(')+fn('static void opid_end(')+fn('int efs_client_append_begin(')+fn('void efs_client_append_end(')+r'''
static void *worker(void *arg) {
 unsigned i=(unsigned)(uintptr_t)arg;
 for(unsigned j=0;j<M;j++) {
  struct efs_opid_req q;int slot=efs_client_append_begin(&q);assert(slot>=0);
  assert(q.ack<q.id.seq && q.id.session_epoch==1 && q.id.client_uuid[0]==1 && q.id.client_uuid[15]==77);
  values[i][j]=q.id.seq;efs_client_append_end(slot);
 }return NULL;
}
static int cmp(const void *a,const void *b) {uint64_t x=*(const uint64_t *)a,y=*(const uint64_t *)b;return (x>y)-(x<y);}
int main(void) {
 pthread_t t[N];
 for(uintptr_t i=0;i<N;i++)assert(!pthread_create(&t[i],NULL,worker,(void *)i));
 for(unsigned i=0;i<N;i++)assert(!pthread_join(t[i],NULL));
 qsort(values,N*M,sizeof(uint64_t),cmp);
 for(unsigned i=0;i<N*M;i++)assert(((uint64_t *)values)[i]==i+1);
 struct efs_opid_req first,last;int a=efs_client_append_begin(&first),b=efs_client_append_begin(&last);
 assert(a>=0 && b>=0 && last.ack==first.id.seq-1);
 efs_client_append_end(b);struct efs_opid_req next; b=efs_client_append_begin(&next);
 assert(next.ack==first.id.seq-1);efs_client_append_end(a);efs_client_append_end(b);
 int slots[EFS_OPID_REPLY_CACHE];
 for(unsigned i=0;i<EFS_OPID_REPLY_CACHE;i++)assert((slots[i]=efs_client_append_begin(&next))>=0);
 assert(efs_client_append_begin(&next)<0 && !next.id.seq);
 for(unsigned i=0;i<EFS_OPID_REPLY_CACHE;i++)efs_client_append_end(slots[i]);
 g_client.opid_next=UINT64_MAX;assert(efs_client_append_begin(&next)<0 && !next.id.seq);
 return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='efs-append-seq-') as d:
 p=Path(d)/'t.c';p.write_text(code)
 subprocess.run(['cc','-O2','-std=gnu11','-pthread','-Wall','-Wextra','-Werror','-I'+str(root/'include'),str(p),'-o',str(p.with_suffix(''))],check=True)
 subprocess.run([str(p.with_suffix(''))],check=True)
print('append sequence: 800000 concurrent unique IDs, contiguous ACK, bounded in-flight admission and exhaustion PASS')
