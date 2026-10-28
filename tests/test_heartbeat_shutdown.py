#!/usr/bin/env python3
"""Interrupt the real heartbeat wait, but join in-flight work before host free."""
from pathlib import Path
import os
import shlex
import subprocess
import tempfile
from sanitize_env import cflags, wrap

root = Path(__file__).resolve().parents[1]
text = Path(os.environ.get('EFS_HEARTBEAT_SOURCE', root / 'src/server/cluster.c')).read_text()
wait_start = text.index('static int heartbeat_wait(')
thread_start = text.index('static void *heartbeat_thread(', wait_start)
start = text.index('void server_start_heartbeat(')
assert 'if (!heartbeat_wait(s))' in text[thread_start:start]
assert 'usleep(EFS_HEARTBEAT_MS' not in text[thread_start:start]
end = text.index('/* Rejoin thread:', start)
efsd = (root / 'src/server/efsd.c').read_text()
stop = efsd[efsd.index('static int sd_raft('):efsd.index('static int sd_peers(')]
assert stop.index('server_stop_heartbeat(s)') < stop.index('server_raft_host_stop()')
code = r'''
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
static int interval_ms=10000;
#define EFS_HEARTBEAT_MS interval_ms
struct efsd_server {
 int running;pthread_t heartbeat_tid;int heartbeat_started;
 pthread_mutex_t heartbeat_lock;pthread_cond_t heartbeat_wake;
};
static pthread_mutex_t mu=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv=PTHREAD_COND_INITIALIZER;
static int entered,release,exited,joined,fail_spawn,mode;
static double now(void) {
 struct timespec t;assert(!clock_gettime(CLOCK_MONOTONIC,&t));return t.tv_sec+t.tv_nsec/1e9;
}
static int capture_deadline,seen_due;
static struct timespec first_due;
static int observed_timedwait(pthread_cond_t *c,pthread_mutex_t *m,const struct timespec *due) {
 if(capture_deadline) {
  if(seen_due)assert(due->tv_sec==first_due.tv_sec && due->tv_nsec==first_due.tv_nsec);
  else {first_due=*due;seen_due=1;}
 }
 return pthread_cond_timedwait(c,m,due);
}
#define pthread_cond_timedwait observed_timedwait
static void *heartbeat_thread(void *);
static int efsd_pthread_create(pthread_t *tid,void *(*fn)(void *),void *arg) {
 return fail_spawn ? EAGAIN : pthread_create(tid,NULL,fn,arg);
}
''' + text[wait_start:thread_start] + text[start:end] + r'''
static void *heartbeat_thread(void *arg) {
 struct efsd_server *s=arg;
 pthread_mutex_lock(&mu);entered=1;pthread_cond_broadcast(&cv);
 /* mode 1 models a GC/status callback already using the Raft host;
  * mode 2 holds the worker just before its wait predicate. */
 while(mode && !release)pthread_cond_wait(&cv,&mu);
 pthread_mutex_unlock(&mu);
 int ran=heartbeat_wait(s);
 assert(!ran); /* all worker cases stop before the 10 s period */
 pthread_mutex_lock(&mu);exited=1;pthread_cond_broadcast(&cv);pthread_mutex_unlock(&mu);
 return NULL;
}
static void *stop(void *arg) {
 server_stop_heartbeat(arg);
 pthread_mutex_lock(&mu);assert(exited);joined=1;pthread_mutex_unlock(&mu);return NULL;
}
static void reset(void) {entered=release=exited=joined=0;}
static void await_entry(void) {
 pthread_mutex_lock(&mu);while(!entered)pthread_cond_wait(&cv,&mu);pthread_mutex_unlock(&mu);
}
static void check_wait(struct efsd_server *s) {
 /* A spurious wake must not start a cycle or reset its due time. */
 interval_ms=80;__atomic_store_n(&s->running,1,__ATOMIC_RELEASE);
 capture_deadline=1;seen_due=0;
 double t=now();assert(heartbeat_wait(s));double elapsed=now()-t;
 assert(seen_due);capture_deadline=0;
 assert(elapsed>=0.065 && elapsed<0.8);
 interval_ms=10000;
}
static void *spurious(void *arg) {
 struct efsd_server *s=arg;
 for(int i=0;i<8;i++) {usleep(10000);pthread_mutex_lock(&s->heartbeat_lock);
 pthread_cond_broadcast(&s->heartbeat_wake);pthread_mutex_unlock(&s->heartbeat_lock);}
 return NULL;
}
int main(void) {
 struct efsd_server server={.running=1};
 server_stop_heartbeat(&server); /* startup never started a heartbeat */
 fail_spawn=1;server_start_heartbeat(&server);assert(!server.heartbeat_started);
 server_stop_heartbeat(&server);
 fail_spawn=0;
 for(int round=0;round<20;round++) {
  reset();mode=0;__atomic_store_n(&server.running,1,__ATOMIC_RELEASE);
  server_start_heartbeat(&server);assert(server.heartbeat_started);await_entry();
  /* Worker announces entry before locking; stop must work both when it is
   * already waiting and when the stop wins before it starts waiting. */
  if(round&1)usleep(1000);
  double t=now();server_stop_heartbeat(&server);
  assert(now()-t<0.8 && exited && !server.heartbeat_started);
  server_stop_heartbeat(&server);
 }
 for(mode=1;mode<=2;mode++) {
  reset();__atomic_store_n(&server.running,1,__ATOMIC_RELEASE);
  server_start_heartbeat(&server);await_entry();
  pthread_t stopper;assert(!pthread_create(&stopper,NULL,stop,&server));
  while(__atomic_load_n(&server.running,__ATOMIC_ACQUIRE))usleep(100);
  pthread_mutex_lock(&mu);assert(!joined && !exited);release=1;
  pthread_cond_broadcast(&cv);pthread_mutex_unlock(&mu);
  double t=now();pthread_join(stopper,NULL);assert(now()-t<0.8 && joined);
 }
 /* No thread owns these temporary synchronization objects except the
  * spurious notifier; exercise the actual timed-wait helper directly. */
 assert(!pthread_mutex_init(&server.heartbeat_lock,NULL));
 pthread_condattr_t attr;assert(!pthread_condattr_init(&attr));
 assert(!pthread_condattr_setclock(&attr,CLOCK_MONOTONIC));
 assert(!pthread_cond_init(&server.heartbeat_wake,&attr));pthread_condattr_destroy(&attr);
 pthread_t notifier;assert(!pthread_create(&notifier,NULL,spurious,&server));
 check_wait(&server);pthread_join(notifier,NULL);
 pthread_cond_destroy(&server.heartbeat_wake);pthread_mutex_destroy(&server.heartbeat_lock);
 return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='efs-heartbeat-stop-') as directory:
    source = Path(directory) / 'stop.c'
    binary = source.with_suffix('')
    source.write_text(code)
    subprocess.run(shlex.split(os.environ.get('CC', 'cc')) + cflags() + [
        '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-pthread', str(source), '-o', str(binary)], check=True)
    subprocess.run(wrap() + [str(binary)], check=True, timeout=15)
print('heartbeat shutdown: prompt wake, stop-before-wait, spurious wakes, monotonic cadence, in-flight drain, failed start and repeated stop PASS')
