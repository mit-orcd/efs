#!/usr/bin/env python3
"""Actual REPORT guards: unrelated files progress, owned rows remain excluded."""
from pathlib import Path
import os, subprocess, tempfile
root = Path(__file__).resolve().parents[1]
s = Path(os.getenv('EFS_REPORT_SNAPSHOT_SOURCE', root/'src/client/write.c')).read_text()
def function(name):
    a = s.index('static void '+name+'(')
    return s[a:s.index('\n}', a)+2]
if 'static void report_snapshot_lock(' in s:
    guards = function('report_snapshot_lock')+'\n'+function('report_snapshot_unlock')
    run = s[s.index('static int report_dirty_ino_run('):s.index('\nint efs_client_report_dirty_ino(')]
    assert 'efs_client_table_lock();' not in run and 'efs_client_table_unlock();' not in run
    assert run.count('report_snapshot_lock(only_ino);') == 3
    assert run.count('report_snapshot_unlock(only_ino);') == 6
else:
    # The old production caller used these whole-table guards for every inode.
    guards = 'static void report_snapshot_lock(efs_ino_t ino){(void)ino;efs_client_table_lock();}\nstatic void report_snapshot_unlock(efs_ino_t ino){(void)ino;efs_client_table_unlock();}'
code = r'''
#include "efs/common.h"
#include <assert.h>
#include <pthread.h>
#include <errno.h>
#include <time.h>
#include <stdio.h>
static struct {pthread_mutex_t lock,idx_mu,dir[64];} g_client;
void efs_client_lock_dir(efs_ino_t ino){pthread_mutex_lock(&g_client.dir[efs_ino_lock_slot(ino,64)]);}
void efs_client_unlock_dir(efs_ino_t ino){pthread_mutex_unlock(&g_client.dir[efs_ino_lock_slot(ino,64)]);}
static void efs_client_table_lock(void){pthread_mutex_lock(&g_client.lock);for(unsigned i=0;i<64;i++)pthread_mutex_lock(&g_client.dir[i]);}
static void efs_client_table_unlock(void){for(unsigned i=64;i-->0;)pthread_mutex_unlock(&g_client.dir[i]);pthread_mutex_unlock(&g_client.lock);}
''' + guards + r'''
static pthread_mutex_t progress=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv=PTHREAD_COND_INITIALIZER;
static int ready,entered;
static void *snapshot(void *arg){
 pthread_mutex_lock(&progress);ready=1;pthread_cond_broadcast(&cv);pthread_mutex_unlock(&progress);
 report_snapshot_lock(*(efs_ino_t*)arg);
 pthread_mutex_lock(&progress);entered=1;pthread_cond_broadcast(&cv);pthread_mutex_unlock(&progress);
 report_snapshot_unlock(*(efs_ino_t*)arg);return NULL;
}
static int wait_entered(unsigned ms){struct timespec until;clock_gettime(CLOCK_REALTIME,&until);until.tv_nsec+=(long)ms*1000000L;until.tv_sec+=until.tv_nsec/1000000000L;until.tv_nsec%=1000000000L;
 pthread_mutex_lock(&progress);while(!ready)pthread_cond_wait(&cv,&progress);
 while(!entered){int rc=pthread_cond_timedwait(&cv,&progress,&until);if(rc==ETIMEDOUT)break;assert(!rc);}
 int done=entered;pthread_mutex_unlock(&progress);return done;}
static void exercise(efs_ino_t ino,pthread_mutex_t *held,int allowed){pthread_t thread;ready=entered=0;pthread_mutex_lock(held);
 assert(!pthread_create(&thread,NULL,snapshot,&ino));assert(wait_entered(allowed?2000:30)==allowed);
 pthread_mutex_unlock(held);assert(!pthread_join(thread,NULL));assert(entered);
 assert(!pthread_mutex_trylock(&g_client.lock));pthread_mutex_unlock(&g_client.lock);
 assert(!pthread_mutex_trylock(&g_client.idx_mu));pthread_mutex_unlock(&g_client.idx_mu);
 for(unsigned i=0;i<64;i++){assert(!pthread_mutex_trylock(&g_client.dir[i]));pthread_mutex_unlock(&g_client.dir[i]);}
}
int main(void){pthread_mutex_init(&g_client.lock,NULL);pthread_mutex_init(&g_client.idx_mu,NULL);for(unsigned i=0;i<64;i++)pthread_mutex_init(&g_client.dir[i],NULL);
 efs_ino_t ino=4097,other=8193;while(efs_ino_lock_slot(ino,64)==efs_ino_lock_slot(other,64))other+=4096;
 exercise(ino,&g_client.dir[efs_ino_lock_slot(other,64)],1); /* unrelated long write */
 exercise(ino,&g_client.dir[efs_ino_lock_slot(ino,64)],0); /* same-file writer */
 exercise(ino,&g_client.idx_mu,0); /* table/slab mutation */
 exercise(0,&g_client.dir[efs_ino_lock_slot(other,64)],0); /* whole-set snapshot */
 puts("REPORT snapshots: unrelated-file progress, same-file/index exclusion, whole-set coverage and complete unlock PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='efs-report-locks-') as d:
    p = Path(d)/'test.c'; p.write_text(code)
    flags = ['-fsanitize=address,undefined'] if os.getenv('EFS_TEST_SANITIZE') == '1' else []
    subprocess.run(['cc',*flags,'-std=gnu11','-Wall','-Wextra','-Werror','-pthread','-I'+str(root/'include'),str(p),'-o',str(p.with_suffix(''))],check=True)
    subprocess.run([str(p.with_suffix(''))],check=True,timeout=10)
