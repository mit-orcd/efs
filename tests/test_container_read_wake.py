#!/usr/bin/env python3
"""Actual wake selection avoids unrelated publications, preserves useful progress."""
from pathlib import Path
import subprocess,tempfile,re
root=Path(__file__).resolve().parents[1];s=(root/'src/server/raft_host.c').read_text()
def function(name):
    m=re.search(r'static (?:int|void) '+name+r'\([\s\S]*?^}',s,re.M)
    assert m,name
    return m.group(0)
def structure(name):
    a=s.index('struct '+name+' {');return s[a:s.index('\n};',a)+3]
code=r'''
#define _POSIX_C_SOURCE 200809L
#include <stdint.h>
#include <pthread.h>
#include <assert.h>
#include <stdio.h>
#include <time.h>
#define HOST_WAITERS 8
#define EFS_RAFT_LEADER 2
#define V_LOAD(x) (x)
struct server{int storage_engine;};
'''+structure('host_view')+structure('host_waiter')+r'''
struct host_group{uint64_t v_has,v_stamp,v_applied;};
struct efs_raft_host{struct server*s;pthread_mutex_t cv_mu;pthread_cond_t applied_cv;unsigned waiter_hint,cv_overflow;struct host_waiter waiters[HOST_WAITERS];struct host_group g[2];struct host_view views[2];};
static struct host_group*group_slot(struct efs_raft_host*h,uint8_t group){return group==0||group==2?&h->g[group/2]:NULL;}
static void host_view_get(struct efs_raft_host*h,uint8_t group,struct host_view*v){*v=h->views[group/2];}
static int signals,registered,returned,expected_proof;
static pthread_cond_t registry=PTHREAD_COND_INITIALIZER;
static struct efs_raft_host*current;
static int noted_signal(pthread_cond_t*c){signals++;return pthread_cond_signal(c);}
static int noted_wait(pthread_cond_t*c,pthread_mutex_t*m,const struct timespec*end){
 if(c!=&current->applied_cv){
  int found=0;for(unsigned i=0;i<HOST_WAITERS;i++)if(c==&current->waiters[i].cv){
   const struct host_waiter*w=&current->waiters[i];assert(w->active && w->read_proof==expected_proof);
   if(expected_proof)assert(w->read_want==40 && w->read_round==4 && w->read_term==11);
   found=1;
  }assert(found);
 }
 registered=1;pthread_cond_broadcast(&registry);return pthread_cond_timedwait(c,m,end);
}
#define pthread_cond_signal noted_signal
#define pthread_cond_timedwait noted_wait
'''+''.join(function(n)+'\n' for n in ('host_view_covers','host_view_wait_apply','host_waiter_sleep_impl','host_waiter_sleep','host_read_waiter_sleep','host_read_waiter_ready','host_waiters_wake'))+r'''
#undef pthread_cond_timedwait
static struct host_view baseline(void){return (struct host_view){.has=1,.role=2,.term=11,.read_pending=1,.stamp=1,.applied=39,.commit=40,.read_round=4,.read_index=40};}
static void *sleeper(void*arg){
 struct efs_raft_host*h=arg;struct timespec end;clock_gettime(CLOCK_REALTIME,&end);end.tv_sec+=3;
 pthread_mutex_lock(&h->cv_mu);
 uint64_t stamp=h->views[0].stamp;
 while(h->s->storage_engine ? !host_view_covers(&h->views[0],40,4,11) : h->g[0].v_stamp==stamp)
  host_read_waiter_sleep(h,0,40,4,11,stamp,&end);
 __atomic_store_n(&returned,1,__ATOMIC_RELEASE);pthread_mutex_unlock(&h->cv_mu);return NULL;
}
static void wait_registered(struct efs_raft_host*h){
 struct timespec end;clock_gettime(CLOCK_REALTIME,&end);end.tv_sec+=3;
 pthread_mutex_lock(&h->cv_mu);while(!registered)assert(!pthread_cond_timedwait(&registry,&h->cv_mu,&end));pthread_mutex_unlock(&h->cv_mu);
}
int main(void){
 struct server server={1};struct efs_raft_host h={.s=&server,.cv_mu=PTHREAD_MUTEX_INITIALIZER,.applied_cv=PTHREAD_COND_INITIALIZER};current=&h;
 for(unsigned i=0;i<HOST_WAITERS;i++)assert(!pthread_cond_init(&h.waiters[i].cv,NULL));
 h.g[0].v_has=h.g[1].v_has=1;h.views[0]=baseline();
 struct host_waiter*w=&h.waiters[0];w->active=1;w->group=0;w->any=1;w->read_proof=1;w->read_want=40;w->read_round=4;w->read_term=11;
 for(unsigned i=0;i<1000;i++){h.g[0].v_stamp++;h.views[0].stamp++;h.views[0].commit++;host_waiters_wake(&h);}
 assert(!signals); /* unrelated writes cannot complete this pending proof */
 h.views[0].read_pending=0;host_waiters_wake(&h);assert(signals==1); /* next round must begin */
 signals=0;h.views[0]=baseline();h.views[0].proven_done=1;h.views[0].proven_round=4;h.views[0].proven_index=40;
 h.views[0].read_pending=0;host_waiters_wake(&h);assert(!signals); /* own apply still owed */
 h.views[0].applied=40;host_waiters_wake(&h);assert(signals==1);
 signals=0;h.views[0]=baseline();h.views[0].term++;host_waiters_wake(&h);assert(signals==1);
 signals=0;h.views[0]=baseline();h.views[0].role=1;host_waiters_wake(&h);assert(signals==1);
 signals=0;h.views[0]=baseline();h.views[0].has=0;host_waiters_wake(&h);assert(signals==1);
 signals=0;h.views[0]=baseline();h.views[0].proven_done=1;h.views[0].proven_round=3;h.views[0].proven_index=50;h.views[0].applied=50;
 host_waiters_wake(&h);assert(!signals); /* completed pre-admission context is not authority */
 h.views[0].proven_round=4;h.views[0].proven_index=39;host_waiters_wake(&h);assert(!signals); /* wrong floor */
 /* Legacy stamp and applied-index waiters remain unchanged. */
 w->read_proof=0;w->seen=h.g[0].v_stamp;host_waiters_wake(&h);assert(!signals);
 h.g[0].v_stamp++;host_waiters_wake(&h);assert(signals==1);
 signals=0;w->any=0;w->idx=10;h.g[0].v_applied=9;host_waiters_wake(&h);assert(!signals);
 h.g[0].v_applied=10;host_waiters_wake(&h);assert(signals==1);w->active=0;
 /* Actual registration and condition-variable wakeup share cv_mu: a
  * relevant publication immediately after registration cannot be lost. */
 signals=registered=returned=0;expected_proof=1;h.views[0]=baseline();pthread_t tid;
 assert(!pthread_create(&tid,NULL,sleeper,&h));wait_registered(&h);
 for(unsigned i=0;i<1000;i++){pthread_mutex_lock(&h.cv_mu);h.views[0].stamp++;h.g[0].v_stamp++;pthread_mutex_unlock(&h.cv_mu);host_waiters_wake(&h);}assert(!signals && !__atomic_load_n(&returned,__ATOMIC_ACQUIRE));
 pthread_mutex_lock(&h.cv_mu);h.views[0].proven_done=1;h.views[0].proven_round=4;h.views[0].proven_index=40;h.views[0].applied=40;pthread_mutex_unlock(&h.cv_mu);
 host_waiters_wake(&h);pthread_join(tid,NULL);assert(signals==1 && returned);
 /* Files call the same registration wrapper with original stamp policy. */
 server.storage_engine=0;signals=registered=returned=0;expected_proof=0;h.views[0]=baseline();h.g[0].v_stamp=h.views[0].stamp;
 assert(!pthread_create(&tid,NULL,sleeper,&h));wait_registered(&h);pthread_mutex_lock(&h.cv_mu);h.g[0].v_stamp++;pthread_mutex_unlock(&h.cv_mu);
 host_waiters_wake(&h);pthread_join(tid,NULL);assert(signals==1 && returned);
 struct timespec end;clock_gettime(CLOCK_REALTIME,&end); /* expired ordinary wait exercises unchanged wrapper */
 pthread_mutex_lock(&h.cv_mu);host_waiter_sleep(&h,0,0,100,0,&end);pthread_mutex_unlock(&h.cv_mu);
 for(unsigned i=0;i<HOST_WAITERS;i++){assert(!h.waiters[i].active);pthread_cond_destroy(&h.waiters[i].cv);}
 pthread_cond_destroy(&h.applied_cv);pthread_mutex_destroy(&h.cv_mu);
 puts("container read wake: unrelated publications suppressed, fresh proof/apply and next-round progress, role/term/refusal, actual registration/no lost wake, files and index waits PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='efs-container-read-wake-') as d:
    p=Path(d)/'test.c';p.write_text(code)
    args=['cc','-std=c11','-Wall','-Wextra','-Werror','-pthread']
    for flags,name in [([], 'native'),(['-fsanitize=address,undefined','-fno-omit-frame-pointer'],'sanitized')]:
        target=Path(d)/name
        subprocess.run(args+flags+[str(p),'-o',str(target)],check=True)
        subprocess.run([str(target)],check=True,timeout=15)
    p.write_text(code.replace('wake=host_read_waiter_ready(&v,w);','wake=host_read_waiter_ready(&v,w) || V_LOAD(g->v_stamp)!=w->seen;'))
    target=Path(d)/'stamp-negative'
    subprocess.run(args+[str(p),'-o',str(target)],check=True)
    result=subprocess.run([str(target)],capture_output=True,timeout=15)
    assert result.returncode != 0, 'unrelated-stamp negative unexpectedly passed'
    print('container read wake: ASan/UBSan and stamp-wake negative PASS')
