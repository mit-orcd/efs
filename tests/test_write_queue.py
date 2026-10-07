#!/usr/bin/env python3
"""Exercise production WB enqueue: bounded admission and owned-buffer lifetime."""
from pathlib import Path
import os,re,shlex,subprocess,tempfile
root=Path(__file__).resolve().parents[1]
f=(root/'src/client/efs_fuse.c').read_text()
def function(name):
    m=re.search(r'^static [^\n;]*\b'+name+r'\([^;]+?\)\n\{',f,re.M)
    return f[m.start():f.index('\n}',m.end())+2]
a=f.index('struct efs_wb_job {');job=f[a:f.index('\n};',a)+3]
source=r'''
#include "client_internal.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#define EFS_WB_DEPTH 2
'''+job+r'''
static struct {
    pthread_mutex_t mu;
    pthread_cond_t not_full,not_empty;
    struct efs_wb_job q[EFS_WB_DEPTH];
    int count,tail,head,shutdown;
    uint64_t queued_bytes,inflight_bytes,admission_waits,admission_timeouts;
} g_wb={.mu=PTHREAD_MUTEX_INITIALIZER,.not_full=PTHREAD_COND_INITIALIZER,
        .not_empty=PTHREAD_COND_INITIALIZER};
static uint64_t deadline;
static unsigned freed;
static uint64_t stats_now_ms(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC,&ts);
    return (uint64_t)ts.tv_sec*1000+ts.tv_nsec/1000000;
}
uint64_t efs_client_rpc_deadline_ms(void) { return deadline; }
static int efs_wb_ensure(void) { return 0; }
static void wb_job_release_buf(char *buf,char *base,size_t cap) {
    (void)base; (void)cap; ++freed; free(buf);
}
'''+function('efs_wb_enqueue_owned')+r'''
static void *worker(void *arg) {
    (void)arg;
    pthread_mutex_lock(&g_wb.mu);
    while (!g_wb.count) pthread_cond_wait(&g_wb.not_empty,&g_wb.mu);
    struct efs_wb_job job=g_wb.q[g_wb.head];
    --g_wb.count; g_wb.queued_bytes-=job.size;
    pthread_cond_broadcast(&g_wb.not_full);
    pthread_mutex_unlock(&g_wb.mu);
    usleep(30000); /* accepted job outlives admission deadline */
    assert(job.buf[0]=='X' && job.deadline==deadline);
    wb_job_release_buf(job.buf,job.free_base,job.buf_cap);
    pthread_mutex_lock(&g_wb.mu);
    *job.done_rc=EFS_OK; *job.done=1;
    pthread_cond_signal(job.done_cv);
    pthread_mutex_unlock(&g_wb.mu);
    return NULL;
}
int main(void) {
    g_wb.count=EFS_WB_DEPTH; g_wb.queued_bytes=20;
    g_wb.q[0].queued_ms=stats_now_ms()-100;
    deadline=stats_now_ms()+10;
    uint64_t start=stats_now_ms();
    assert(efs_wb_enqueue_owned(1,0,1,malloc(1),NULL,0)==EFS_ERR_BUSY);
    assert(stats_now_ms()-start<200 && g_wb.count==2 &&
           g_wb.queued_bytes==20 && freed==1 && g_wb.admission_timeouts==1);
    g_wb.shutdown=1;
    assert(efs_wb_enqueue_owned(1,0,1,malloc(1),NULL,0)==EFS_ERR_IO);
    assert(freed==2 && g_wb.count==2);
    g_wb.shutdown=0; g_wb.count=0; g_wb.queued_bytes=0;
    deadline=stats_now_ms();
    assert(efs_wb_enqueue_owned(1,0,1,malloc(1),NULL,0)==EFS_ERR_BUSY);
    assert(freed==3&&!g_wb.count);
    char *copy=malloc(1); copy[0]='X';
    pthread_t t; assert(!pthread_create(&t,NULL,worker,NULL));
    deadline=stats_now_ms()+5;
    assert(efs_wb_enqueue_owned(1,0,1,copy,NULL,0)==EFS_OK);
    assert(!pthread_join(t,NULL));
    assert(!g_wb.count && !g_wb.queued_bytes && freed==4);
    puts("write queue: timeout/shutdown ownership and accepted-job completion PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='efs-write-queue-') as directory:
    p=Path(directory)/'test.c';p.write_text(source)
    cmd=shlex.split(os.environ.get('CC','cc'))+['-std=gnu11','-D_GNU_SOURCE','-Wall','-Wextra','-Werror','-pthread','-I'+str(root/'include'),'-I'+str(root/'src/client')]
    cmd+=shlex.split(os.environ.get('WB_TEST_CFLAGS',''))
    subprocess.run(cmd+[str(p),'-o',str(p.with_suffix(''))],check=True)
    subprocess.run([str(p.with_suffix(''))],check=True,timeout=10)
