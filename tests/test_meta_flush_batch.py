#!/usr/bin/env python3
"""Execute production background queue/worker with controlled dispatch timing."""
from pathlib import Path
import os, shlex, subprocess, tempfile
root=Path(__file__).resolve().parents[1]
s=(root/'src/client/write.c').read_text()
a=s.index('static void meta_flush_enqueue(efs_ino_t ino)\n{')
b=s.index('void efs_client_enable_meta_batch',a)
c=s.index('int efs_client_note_meta_change(int force)')
e=s.index('/* Consume one PUT_CHUNK',c)
code=r'''
#include <pthread.h>
#include <stdint.h>
#include <errno.h>
#include <assert.h>
#include <stdio.h>
#include <unistd.h>
#include <time.h>
#include <stdlib.h>
#include <string.h>
#define EFS_OK 0
typedef uint64_t efs_ino_t;
#define EFS_META_FLUSH_QLEN 8
struct { int meta_flush_started,meta_dirty,meta_flush_stop,meta_flush_force,meta_batch;
uint32_t meta_dirty_ops,meta_batch_ops;
pthread_mutex_t dirty_mu,meta_flush_mu;pthread_cond_t meta_flush_cv;
efs_ino_t meta_flush_q[EFS_META_FLUSH_QLEN];unsigned meta_flush_qn,meta_flush_qh;
struct timespec meta_flush_ready; } g_client;
static pthread_mutex_t check=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t done=PTHREAD_COND_INITIALIZER;
static int reports;static efs_ino_t reported;
static void efs_client_ensure_dir_locks(void) {}
static int forced_reports, threshold_reports;
static int efs_client_report_dirty(int sync) {if(sync)forced_reports++;else threshold_reports++;return 0;}
static int efs_client_report_dirty_ino(efs_ino_t ino,int sync) {
assert(!sync);pthread_mutex_lock(&check);reports++;reported=ino;
pthread_cond_broadcast(&done);pthread_mutex_unlock(&check);return 0;
}
'''+s[a:b]+s[c:e]+r'''
int main(void) {
pthread_mutex_init(&g_client.dirty_mu,NULL);
pthread_mutex_init(&g_client.meta_flush_mu,NULL);
pthread_condattr_t attr;pthread_condattr_init(&attr);
assert(!pthread_condattr_setclock(&attr,CLOCK_MONOTONIC));
pthread_cond_init(&g_client.meta_flush_cv,&attr);pthread_condattr_destroy(&attr);
g_client.meta_flush_started=1;
meta_flush_enqueue(42);
/* Hold the first batch long enough to test dedupe/signals deterministically. */
g_client.meta_flush_ready.tv_sec+=30;
struct timespec deadline=g_client.meta_flush_ready;
for(int i=0;i<1000;i++)meta_flush_enqueue(42);
assert(g_client.meta_flush_qn==1);
assert(g_client.meta_flush_ready.tv_sec==deadline.tv_sec && g_client.meta_flush_ready.tv_nsec==deadline.tv_nsec);
pthread_t workers[4];for(int i=0;i<4;i++)assert(!pthread_create(&workers[i],NULL,meta_flush_main,NULL));
usleep(20000);pthread_mutex_lock(&check);assert(!reports);pthread_mutex_unlock(&check);
pthread_mutex_lock(&g_client.meta_flush_mu);
g_client.meta_flush_ready=(struct timespec){0};pthread_cond_broadcast(&g_client.meta_flush_cv);
pthread_mutex_unlock(&g_client.meta_flush_mu);
pthread_mutex_lock(&check);while(!reports)pthread_cond_wait(&done,&check);
assert(reports==1 && reported==42);pthread_mutex_unlock(&check);
/* New kicks get a new deadline; full queue collapses safely to all-inodes. */
meta_flush_enqueue(43);pthread_mutex_lock(&g_client.meta_flush_mu);
g_client.meta_flush_ready.tv_sec+=30;pthread_mutex_unlock(&g_client.meta_flush_mu);
for(int i=44;i<52;i++)meta_flush_enqueue(i);
pthread_mutex_lock(&g_client.meta_flush_mu);assert(g_client.meta_flush_qn==1);
assert(g_client.meta_flush_q[g_client.meta_flush_qh]==0);
g_client.meta_flush_stop=1;pthread_cond_broadcast(&g_client.meta_flush_cv);pthread_mutex_unlock(&g_client.meta_flush_mu);
for(int i=0;i<4;i++)assert(!pthread_join(workers[i],NULL));
/* Shutdown returns promptly, without waiting the artificial deadline. */
assert(reports==1);
/* Foreground forced publication bypasses the stopped/delayed background queue.
 * Non-forced publication uses only the snapshot-owned operation threshold. */
g_client.meta_batch=1;g_client.meta_batch_ops=4;g_client.meta_flush_started=0;
assert(!efs_client_note_meta_change(1) && forced_reports==1);
for(int i=0;i<3;i++)assert(!efs_client_note_meta_change(0));
assert(!threshold_reports);assert(!efs_client_note_meta_change(0));assert(threshold_reports==1);
g_client.meta_dirty_ops=0; /* successful whole-set snapshot resets ownership */
assert(!efs_client_note_meta_change(0));assert(threshold_reports==1);
puts("metadata batch: dedupe, fixed deadline, four workers, overflow, shutdown PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='efs-meta-batch-') as d:
 p=Path(d)/'test.c';p.write_text(code)
 subprocess.run(shlex.split(os.environ.get('CC','cc'))+shlex.split(os.environ.get('EXTRA_CFLAGS',''))+['-D_GNU_SOURCE','-std=c99','-Wall','-Wextra','-Werror','-pthread',str(p),'-o',str(p.with_suffix(''))],check=True)
 subprocess.run([str(p.with_suffix(''))],check=True,timeout=10)
