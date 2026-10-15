#!/usr/bin/env python3
"""Execute the production worker: coalescing, failure retention, retry and join."""
from pathlib import Path
import os, shlex, subprocess, tempfile
s=(Path(__file__).resolve().parents[1]/'src/client/efs_fuse.c').read_text()
a=s.index('/* One coalesced pressure wakeup,');b=s.index('/* utimens(mtime)',a)
code=r'''
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <assert.h>
#include <unistd.h>
#define EFS_OK 0
typedef uint64_t efs_ino_t;
static pthread_mutex_t mu=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv=PTHREAD_COND_INITIALIZER;
static int pressure=1,calls,completed,release_call,result,owned=1,entered,active,peak;
static int efs_buf_should_drain(void) {pthread_mutex_lock(&mu);int p=pressure;pthread_mutex_unlock(&mu);return p;}
static efs_ino_t efs_dcache_pressure_ino(efs_ino_t after) {return after<42?42:0;}
static void efs_buf_drain_enter(void) {entered++;}
static void efs_buf_drain_leave(void) {entered--;}
static void efs_rdcache_trim(void) {}
static void efs_dcache_trim_metadata(void) {}
static int fuse_pressure_flush(efs_ino_t ino) {
assert(ino==42 && entered==1);pthread_mutex_lock(&mu);
calls++;active++;if(active>peak)peak=active;pthread_cond_broadcast(&cv);
while(release_call<calls)pthread_cond_wait(&cv,&mu);
int rc=result;if(!rc)owned=0;active--;completed++;pthread_cond_broadcast(&cv);
pthread_mutex_unlock(&mu);return rc;
}
'''+s[a:b]+r'''
static void wait_calls(int n) {pthread_mutex_lock(&mu);while(calls<n)pthread_cond_wait(&cv,&mu);pthread_mutex_unlock(&mu);}
static void complete(int n,int rc) {pthread_mutex_lock(&mu);result=rc;pressure=0;release_call=n;pthread_cond_broadcast(&cv);while(completed<n)pthread_cond_wait(&cv,&mu);pthread_mutex_unlock(&mu);}
int main(void) {
assert(fuse_pressure_kick());wait_calls(1);
for(int i=0;i<10000;i++)assert(fuse_pressure_kick());
pthread_mutex_lock(&g_pressure.mu);assert(g_pressure.pending==1);pthread_mutex_unlock(&g_pressure.mu);
complete(1,-13);pthread_mutex_lock(&mu);assert(owned && peak==1);pressure=1;pthread_mutex_unlock(&mu);
assert(fuse_pressure_kick());wait_calls(2);complete(2,0);
/* Stop joins even if a coalesced wake is still pending. No detached thread or
 * queued identity can access freed client state. New kicks must be refused. */
fuse_pressure_stop();assert(!fuse_pressure_kick());
assert(!entered && !owned && completed==2 && peak==1);
puts("background pressure: bounded wakeup, no concurrent drains, failure retention/retry, shutdown join PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='efs-pressure-background-') as d:
 p=Path(d)/'t.c';p.write_text(code)
 subprocess.run(shlex.split(os.environ.get('CC','cc'))+shlex.split(os.environ.get('EXTRA_CFLAGS',''))+['-std=gnu11','-Wall','-Wextra','-Werror','-pthread',str(p),'-o',str(p.with_suffix(''))],check=True)
 subprocess.run([str(p.with_suffix(''))],check=True,timeout=30)
