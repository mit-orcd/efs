#!/usr/bin/env python3
"""Actual follower coalescer: delayed delivery retains first fresh proof."""
from pathlib import Path
import os,subprocess,tempfile
root=Path(__file__).resolve().parents[1]
s=Path(os.environ.get('EFS_RAFT_HOST_SOURCE',root/'src/server/raft_host.c')).read_text()
legacy=os.environ.get('EFS_TEST_LEGACY_COALESCER')=='1'
a=s.index('struct host_ri_coal {' if legacy else 'struct host_ri_wait {');b=s.index('/* ReadIndex for one group',a)
code=r'''
#define _GNU_SOURCE
#include "efs/protocol.h"
#include <pthread.h>
#include <stdint.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
#include <time.h>
#include <stdio.h>
#define HOST_NGROUPS 2
struct efs_raft_host {int unused;};
static pthread_mutex_t test_mu=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t test_cv=PTHREAD_COND_INITIALIZER;
static unsigned calls,entered[4],release1,release2,d_done,delayed;
static __thread int who;
static int host_remote_cmd(struct efs_raft_host*h,uint8_t group,const uint8_t*cmd,uint32_t len,struct efs_msg_raft_mkfs_reply*r,int leader){
 (void)h;(void)group;(void)cmd;(void)len;(void)leader;
 pthread_mutex_lock(&test_mu);unsigned n=++calls;pthread_cond_broadcast(&test_cv);
 while((n==1&&!release1)||(n==2&&!release2))pthread_cond_wait(&test_cv,&test_mu);
 pthread_mutex_unlock(&test_mu);assert(n<=3);*r=(struct efs_msg_raft_mkfs_reply){.index=n==1?100:n==2?110:900,.leader_hint=1};return EFS_OK;
}
/* Delay one second-round waiter AFTER wake so the third round completes
 * before it reads its result. This reproduces scheduler delay deterministically. */
static int delayed_wait(pthread_cond_t*cv,pthread_mutex_t*mu,const struct timespec*end){
 pthread_mutex_lock(&test_mu);entered[who]++;pthread_cond_broadcast(&test_cv);pthread_mutex_unlock(&test_mu);
 int rc=pthread_cond_timedwait(cv,mu,end);
 pthread_mutex_lock(&test_mu);int pause=(who==1||who==2)&&calls>=2;
 if(pause){delayed++;pthread_cond_broadcast(&test_cv);pthread_mutex_unlock(mu);while(!d_done)pthread_cond_wait(&test_cv,&test_mu);}
 pthread_mutex_unlock(&test_mu);if(pause)pthread_mutex_lock(mu);return rc;
}
#define pthread_cond_timedwait delayed_wait
'''+s[a:b]+r'''
#undef pthread_cond_timedwait
struct result {uint64_t index;int rc,hint,id;};
static void *reader(void*arg){
 struct result*r=arg;who=r->id;struct efs_raft_host h={0};struct timespec end;clock_gettime(CLOCK_REALTIME,&end);end.tv_sec+=5;
 r->rc=host_remote_read_index(&h,0,1,&end,&r->index,&r->hint);
 if(who==3){pthread_mutex_lock(&test_mu);d_done=1;pthread_cond_broadcast(&test_cv);pthread_mutex_unlock(&test_mu);}return NULL;
}
int main(void){
 struct result r[4]={0};pthread_t t[4];for(int i=0;i<4;i++)r[i].id=i;
 assert(!pthread_create(&t[0],NULL,reader,&r[0]));pthread_mutex_lock(&test_mu);
 while(calls<1){pthread_cond_wait(&test_cv,&test_mu);}
 pthread_mutex_unlock(&test_mu);
 assert(!pthread_create(&t[1],NULL,reader,&r[1]));assert(!pthread_create(&t[2],NULL,reader,&r[2]));
 pthread_mutex_lock(&test_mu);while(!entered[1]||!entered[2])pthread_cond_wait(&test_cv,&test_mu);release1=1;pthread_cond_broadcast(&test_cv);
 while(calls<2){pthread_cond_wait(&test_cv,&test_mu);}
 pthread_mutex_unlock(&test_mu);
 assert(!pthread_create(&t[3],NULL,reader,&r[3]));pthread_mutex_lock(&test_mu);while(!entered[3])pthread_cond_wait(&test_cv,&test_mu);
 release2=1;pthread_cond_broadcast(&test_cv);pthread_mutex_unlock(&test_mu);
 for(int i=0;i<4;i++)assert(!pthread_join(t[i],NULL));
 assert(calls==3&&delayed&&r[0].index==100&&r[1].index==110&&r[2].index==110&&r[3].index==900);
 for(int i=0;i<4;i++)assert(r[i].rc==EFS_OK&&r[i].hint==1);
 assert(!g_ri_coal[0].waiters&&!g_ri_coal[0].inflight);
 /* Freeze errors too, and never let a later success overwrite them. */
 struct host_ri_wait future={.need=12},old={.need=11,.next=&future};struct host_ri_coal c={.waiters=&old};
 host_ri_complete(&c,11,EFS_ERR_IO,123,2);assert(old.done&&old.rc==EFS_ERR_IO&&!old.index&&old.hint==-1&&!future.done);
 host_ri_complete(&c,12,EFS_OK,999,3);assert(old.rc==EFS_ERR_IO&&!old.index&&future.done&&future.index==999);
 host_ri_remove(&c,&old);host_ri_remove(&c,&future);assert(!c.waiters);
 /* Timeout and exhausted context detach/refuse without leaked stack nodes. */
 g_ri_coal[0].inflight=1;struct efs_raft_host h={0};struct timespec end={0};uint64_t idx=0;int hint=0;who=0;
 assert(host_remote_read_index(&h,0,1,&end,&idx,&hint)==EFS_ERR_BUSY&&!g_ri_coal[0].waiters);
 g_ri_coal[0].inflight=0;g_ri_coal[0].started=UINT64_MAX;
 assert(host_remote_read_index(&h,0,1,&end,&idx,&hint)==EFS_ERR_BUSY&&!g_ri_coal[0].waiters);
 return 0;
}
'''
if legacy:
    code=code.replace('assert(!g_ri_coal[0].waiters&&!g_ri_coal[0].inflight);','assert(!g_ri_coal[0].inflight);')
    start=code.index(' /* Freeze errors too');end=code.index(' return 0;',start)
    code=code[:start]+code[end:]
with tempfile.TemporaryDirectory(prefix='efs-read-coal-') as d:
 p=Path(d)/'t.c';p.write_text(code)
 subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror','-pthread','-I'+str(root/'include'),str(p),'-o',str(p.with_suffix(''))],check=True)
 subprocess.run([str(p.with_suffix(''))],check=True,timeout=10)
print('remote coalescer: delayed first-proof delivery, post-send arrivals, frozen errors, timeout unlink and context exhaustion PASS')
