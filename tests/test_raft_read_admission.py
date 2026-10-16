#!/usr/bin/env python3
"""Actual pre-mutex tickets select only rounds whose probes follow admission."""
import os,shlex,subprocess,tempfile
from pathlib import Path
root=Path(__file__).resolve().parents[1]
s=Path(os.environ.get('EFS_RAFT_HOST_SOURCE',root/'src/server/raft_host.c')).read_text()
a=s.index('static uint64_t host_read_register(');b=s.index('static int host_read_index_admitted(',a)
code=r'''
#define _GNU_SOURCE
#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
struct host_group {uint64_t read_admit_count,read_begin_admits,read_begin_round,read_begin_term;};
struct server {int unused;};
struct efs_raft {uint64_t round,term;int fail;};
struct efs_raft_host {pthread_mutex_t mu;struct server *s;struct host_group g;struct efs_raft r;};
static struct efs_raft_host *current;
static struct host_group *group_slot(struct efs_raft_host *h,uint8_t group){return group?NULL:&h->g;}
static uint64_t efs_raft_read_round(struct efs_raft *r){return r->round;}
static uint64_t efs_raft_term(struct efs_raft *r){return r->term;}
static uint64_t host_read_register(struct efs_raft_host *h,uint8_t group);
static int send_late;static uint64_t late;
static int efs_raft_read_begin(struct efs_raft *r){
 if(r->fail)return r->fail;
 r->round++;if(send_late)late=host_read_register(current,0);return 0;
}
'''+s[a:b]+r'''
static uint64_t early,selected,tickets[4][256];static int registered;
static void *reader(void *arg){
 struct efs_raft_host *h=arg;early=host_read_register(h,0);
 __atomic_store_n(&registered,1,__ATOMIC_RELEASE);
 pthread_mutex_lock(&h->mu);selected=host_read_eligible_round(h,0,&h->r,early);pthread_mutex_unlock(&h->mu);return NULL;
}
static void *register_many(void *arg){
 unsigned i=(uintptr_t)arg;for(unsigned j=0;j<256;j++)tickets[i][j]=host_read_register(current,0);return NULL;
}
int main(void){
 struct server server={1};struct efs_raft_host h={.mu=PTHREAD_MUTEX_INITIALIZER,.s=&server,.r={3,7,0}};current=&h;
 /* Reader arrives while another caller owns h->mu. The later round is
  * fresh for it, but not for a reader arriving inside the send callback. */
 pthread_mutex_lock(&h.mu);pthread_t worker;assert(!pthread_create(&worker,NULL,reader,&h));
 unsigned spins=0;while(!__atomic_load_n(&registered,__ATOMIC_ACQUIRE)){assert(spins++<3000);usleep(1000);}
 assert(early==1);send_late=1;assert(!host_read_begin_tracked(&h,0,&h.r));
 assert(h.g.read_begin_admits==1 && late==2);pthread_mutex_unlock(&h.mu);assert(!pthread_join(worker,NULL));
 assert(selected==4 && host_read_eligible_round(&h,0,&h.r,late)==5);
 /* Old pending/completed rounds cannot cover an admission made later. */
 uint64_t next=host_read_register(&h,0);assert(next==3 && host_read_eligible_round(&h,0,&h.r,next)==5);
 h.r.term++;assert(host_read_eligible_round(&h,0,&h.r,early)==5);h.r.term--;
 h.r.round++;assert(host_read_eligible_round(&h,0,&h.r,early)==6);h.r.round--;
 struct host_group before=h.g;h.r.fail=-2;assert(host_read_begin_tracked(&h,0,&h.r)==-2);
 assert(!memcmp(&before,&h.g,sizeof(before)));h.r.fail=0;
 h.s=NULL;assert(!host_read_register(&h,0));
 assert(host_read_eligible_round(&h,0,&h.r,early)==5);
 assert(!host_read_begin_tracked(&h,0,&h.r));assert(!memcmp(&before,&h.g,sizeof(before)));
 h.s=&server;server.unused=0;
 uint64_t file_ticket=host_read_register(&h,0);assert(file_ticket);
 assert(!host_read_begin_tracked(&h,0,&h.r));
 assert(host_read_eligible_round(&h,0,&h.r,file_ticket)==h.r.round);
 assert(host_read_eligible_round(&h,0,&h.r,late)==h.r.round+1);
 assert(!host_read_register(&h,2));
 h.g.read_admit_count=UINT64_MAX-2;assert(host_read_register(&h,0)==UINT64_MAX-1);
 assert(!host_read_register(&h,0) && !host_read_register(&h,0));
 assert(host_read_eligible_round(&h,0,&h.r,0)==h.r.round+1);
 /* Concurrent registration has a bounded monotonic identity, no queue. */
 memset(&h.g,0,sizeof(h.g));pthread_t threads[4];unsigned seen[1025]={0};
 for(uintptr_t i=0;i<4;i++)assert(!pthread_create(&threads[i],NULL,register_many,(void*)i));
 for(unsigned i=0;i<4;i++)assert(!pthread_join(threads[i],NULL));
 for(unsigned i=0;i<4;i++)for(unsigned j=0;j<256;j++){uint64_t t=tickets[i][j];assert(t && t<=1024 && !seen[t]);seen[t]=1;}
 assert(h.g.read_admit_count==1024);pthread_mutex_destroy(&h.mu);
 puts("raft read admission: before mutex/probes, late send exclusion, old/term/untracked refusal, failure/files/saturation, concurrent uniqueness PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='efs-read-admit-') as d:
 p=Path(d)/'test.c';p.write_text(code)
 subprocess.run(['cc','-std=c99','-Wall','-Wextra','-Werror',*shlex.split(os.environ.get('EFS_TEST_CFLAGS','')),'-pthread',str(p),'-o',str(p.with_suffix(''))],check=True)
 subprocess.run([str(p.with_suffix(''))],check=True,timeout=15)
# Integrate before each real mutex capture; final authority predicates unchanged.
a=s.index('static int host_read_index_admitted(');body=s[a:s.index('\n}',a)]
assert body.index('host_read_register(')<body.index('pthread_mutex_lock(&h->mu)')
assert 'host_view_covers(&v, want, round, admitted_term)' in body
assert 'if (v.term != admitted_term)' in body
