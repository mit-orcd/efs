#!/usr/bin/env python3
"""Actual Linux event worker: fresh votes off pump tail, no receive/credit changes."""
from pathlib import Path
import os, subprocess, sys, tempfile
if not sys.platform.startswith('linux'):
    print('container read worker: Linux eventfd native control NOTRUN on this host')
    raise SystemExit(0)
root=Path(__file__).resolve().parents[1]
s=(root/'src/server/raft_host.c').read_text()
a=s.index('static void host_container_pending_reads(');b=s.index('\n}',a)+2
pending=s[a:b]
a=s.index('static void host_container_read_kick(');b=s.index('static void *host_pump(',a)
worker=s[a:b]
code=r'''
#include "efs/raft.h"
#include "efs/wire.h"
#include "efs/batch_obs.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sched.h>
#include <pthread.h>
#include <stdint.h>
#include <string.h>
#include <sys/eventfd.h>
#include <time.h>
#include <unistd.h>
#define HOST_INBOX_MAX 8
#define HOST_NGROUPS 2
int efs_batch_obs_on;
void efs_batch_obs_count(enum efs_batch_counter c){(void)c;}
struct server{int storage_engine;};
struct host_inbox_item{uint8_t*buf;uint32_t len;int read_noted;};
struct efs_raft_host{struct server*s;pthread_mutex_t mu,inbox_mu;int inbox_n,read_efd,read_running,read_started;pthread_t read_tid;uint64_t inbox_work_n,inbox_work_bytes;struct host_inbox_item inbox[HOST_INBOX_MAX];struct{uint8_t group;}g[2];};
static struct efs_raft_host *current;
static pthread_mutex_t seen_mu=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t seen_cv=PTHREAD_COND_INITIALIZER;
static int seen,allow,fail_event,fail_thread,last_fd,created,published,woken;
static uint64_t rd64be(const uint8_t*p){uint64_t v=0;for(unsigned i=0;i<8;i++)v=(v<<8)|p[i];return v;}
int efs_wire_raft_decode(const uint8_t*p,uint32_t n,struct efs_raft_msg*m,uint8_t*c,uint32_t z){(void)n;(void)c;(void)z;memset(m,0,sizeof(*m));m->type=p[0];m->group=p[1];return EFS_OK;}
static struct efs_raft*group_raft(struct efs_raft_host*h,uint8_t g){(void)h;return (struct efs_raft*)(uintptr_t)(g+1);}
int efs_raft_read_control(struct efs_raft*r,const struct efs_raft_msg*m){
 (void)r;(void)m;assert(pthread_mutex_trylock(&current->mu)==EBUSY);
 pthread_mutex_lock(&seen_mu);seen++;pthread_cond_broadcast(&seen_cv);pthread_mutex_unlock(&seen_mu);return allow;
}
static void host_publish_group(struct efs_raft_host*h,uint8_t g){assert(h==current && g==2);published++;}
static void host_waiters_wake(struct efs_raft_host*h){assert(h==current);woken++;}
static int fixture_eventfd(unsigned n,int flags){if(fail_event){errno=EMFILE;return -1;}last_fd=eventfd(n,flags);return last_fd;}
#define eventfd fixture_eventfd
static int efsd_pthread_create(pthread_t*t,void*(*fn)(void*),void*arg){if(fail_thread)return EAGAIN;created++;return pthread_create(t,NULL,fn,arg);}
'''+pending+worker+r'''
static void wait_seen(int n){
 struct timespec end;clock_gettime(CLOCK_REALTIME,&end);end.tv_sec+=2;
 pthread_mutex_lock(&seen_mu);
 while(seen<n)assert(pthread_cond_timedwait(&seen_cv,&seen_mu,&end)==0);
 pthread_mutex_unlock(&seen_mu);
 /* The vote callback notifies before the helper marks its identity. */
 pthread_mutex_lock(&current->mu);pthread_mutex_unlock(&current->mu);
}
static void *stopper(void*arg){host_container_read_stop(arg);return NULL;}
int main(void){
 struct server server={0};struct efs_raft_host h={.s=&server,.mu=PTHREAD_MUTEX_INITIALIZER,.inbox_mu=PTHREAD_MUTEX_INITIALIZER,.read_efd=-1};current=&h;h.g[1].group=2;
 host_container_read_start(&h);assert(!created && !h.read_started && h.read_efd==-1);
 server.storage_engine=1;fail_event=1;host_container_read_start(&h);assert(!created && h.read_efd==-1);fail_event=0;
 fail_thread=1;host_container_read_start(&h);assert(!h.read_started && !h.read_running && h.read_efd==-1 && fcntl(last_fd,F_GETFD)==-1 && errno==EBADF);fail_thread=0;
 uint8_t frame[80]={0};frame[0]=EFS_RAFT_MSG_AE_REP;frame[1]=2;frame[35]=7;
 pthread_mutex_lock(&h.mu);h.inbox_n=1;h.inbox[0]=(struct host_inbox_item){frame,80,0};h.inbox_work_n=1;h.inbox_work_bytes=80;
 host_container_read_start(&h);assert(created==1 && h.read_started);host_container_read_kick(&h);
 assert(!seen);pthread_mutex_unlock(&h.mu);wait_seen(1);assert(!h.inbox[0].read_noted && !published && !woken);
 /* Retrying the exact admitted header after core eligibility changes
  * works even though the normal pump is deliberately not running. */
 pthread_mutex_lock(&h.mu);allow=2;pthread_mutex_unlock(&h.mu);host_container_read_kick(&h);wait_seen(2);
 assert(h.inbox[0].read_noted && published==1 && woken==1 && h.inbox_n==1 && h.inbox_work_n==1 && h.inbox_work_bytes==80 && h.inbox[0].buf==frame);
 pthread_mutex_lock(&h.mu);for(int i=0;i<10000;i++)host_container_read_kick(&h);pthread_mutex_unlock(&h.mu);
 assert(created==1);host_container_read_stop(&h);assert(!h.read_started && !h.read_running && h.read_efd==-1);
 /* Stop a helper waiting behind the serialization mutex: after stop
  * is requested it must join without touching the core or queued frame. */
 pthread_mutex_lock(&h.mu);h.inbox[0].read_noted=0;host_container_read_start(&h);host_container_read_kick(&h);
 pthread_t tid;assert(!pthread_create(&tid,NULL,stopper,&h));
 while(__atomic_load_n(&h.read_running,__ATOMIC_ACQUIRE))sched_yield();
 pthread_mutex_unlock(&h.mu);pthread_join(tid,NULL);assert(seen==2 && !h.read_started && h.read_efd==-1);
 pthread_mutex_destroy(&h.mu);pthread_mutex_destroy(&h.inbox_mu);
 puts("container read worker: pump-independent votes, refusal retry, FIFO/credits retained, bounded wakeups, files/failure fallback, join-before-free PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='efs-container-read-worker-') as d:
    p=Path(d)/'test.c';p.write_text(code)
    args=[os.environ.get('CC','cc'),'-std=gnu11','-Wall','-Wextra','-Werror','-pthread','-I'+str(root/'include')]
    for flags,name in [([], 'native'),(['-fsanitize=address,undefined','-fno-omit-frame-pointer'],'sanitized')]:
        target=Path(d)/name
        subprocess.run(args+flags+[str(p),'-o',str(target)],check=True)
        subprocess.run([str(target)],check=True,timeout=15)
    p.write_text(code.replace('            host_container_pending_reads(h);','            if(0)host_container_pending_reads(h);'))
    target=Path(d)/'pump-only-negative'
    subprocess.run(args+[str(p),'-o',str(target)],check=True)
    result=subprocess.run([str(target)],capture_output=True,timeout=15)
    assert result.returncode != 0, 'pump-only negative unexpectedly passed'
    print('container read worker: ASan/UBSan and pump-only negative PASS')
