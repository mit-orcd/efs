#!/usr/bin/env python3
"""Actual read/PUT window wait honors caller deadline and releases its mutex."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[1];s=(root/'src/client/write.c').read_text();a=s.index('static int put_win_wait(');f=s[a:s.index('\n}',a)+2]
code=r'''
#include "efs/common.h"
#include <assert.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>
#include <errno.h>
static pthread_mutex_t put_win_mu=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t put_win_cv=PTHREAD_COND_INITIALIZER;
static uint64_t deadline;static int busy;
static uint64_t report_clock_ms(void) {struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000+t.tv_nsec/1000000;}
static uint64_t efs_client_rpc_deadline_ms(void) {return deadline;}
static int put_win_busy_locked(efs_ino_t ino) {(void)ino;return busy;}
'''+f+r'''
static void *complete(void *p) {(void)p;usleep(10000);pthread_mutex_lock(&put_win_mu);busy=0;pthread_cond_broadcast(&put_win_cv);pthread_mutex_unlock(&put_win_mu);return NULL;}
int main(void) {
 busy=1;uint64_t start=report_clock_ms();deadline=start+10;
 assert(put_win_wait(1,8000)==EFS_ERR_BUSY&&report_clock_ms()-start<150);
 assert(!pthread_mutex_trylock(&put_win_mu));pthread_mutex_unlock(&put_win_mu);
 deadline=report_clock_ms();assert(put_win_wait(1,8000)==EFS_ERR_BUSY);
 deadline=0;start=report_clock_ms();assert(put_win_wait(1,10)==EFS_ERR_BUSY&&report_clock_ms()-start<150);
 pthread_t t;assert(!pthread_create(&t,NULL,complete,NULL));
 assert(put_win_wait(1,1000)==1);assert(!pthread_join(t,NULL));
 assert(put_win_wait(1,1000)==0);return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='efs-put-window-') as d:
 p=Path(d)/'t.c';p.write_text(code)
 subprocess.run(['cc','-std=gnu11','-pthread','-Wall','-Wextra','-Werror','-I'+str(root/'include'),str(p),'-o',str(p.with_suffix(''))],check=True)
 subprocess.run([str(p.with_suffix(''))],check=True,timeout=5)
print('PUT window: inherited/expired/local budgets, completion and unlocked exit PASS')
