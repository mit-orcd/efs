#!/usr/bin/env python3
"""Actual append flush refuses expired work even when its stripe is free."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[1];s=(root/'src/client/efs_fuse.c').read_text()
def fn(p):
 a=s.index(p);return s[a:s.index('\n}',a)+2]
code=r'''
#include "client_internal.h"
#include <errno.h>
#include <time.h>
#include <unistd.h>
#include <assert.h>
struct fuse_file_info{int unused;};
static uint64_t deadline;static int dirty,sync_rc,flush_rc,expire_sync;static unsigned flushes,reports,syncs;
static pthread_mutex_t stripe=PTHREAD_MUTEX_INITIALIZER;
static uint64_t now_ms(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000+t.tv_nsec/1000000;}
uint64_t efs_client_rpc_deadline_ms(void){return deadline;}
int efs_client_rpc_past_deadline(void){return deadline&&now_ms()>=deadline;}
static int virt_kind(efs_ino_t n){(void)n;return 0;}
static int efs_ino_has_unpublished(efs_ino_t n){(void)n;return dirty;}
static int efs_wb_sync_ino(efs_ino_t n){(void)n;syncs++;if(expire_sync)deadline=now_ms();return sync_rc;}
static pthread_mutex_t *append_mu(efs_ino_t n){(void)n;return &stripe;}
int efs_dcache_flush_ino(efs_ino_t n){(void)n;flushes++;return flush_rc;}
int efs_client_report_dirty_ino(efs_ino_t n,int sync){(void)n;(void)sync;reports++;return EFS_OK;}
'''+fn('static int wb_lock_budget(')+fn('static int efs_append_flush_report_run(')+r'''
static void reset(void){dirty=1;sync_rc=flush_rc=expire_sync=0;flushes=reports=syncs=0;deadline=now_ms()+100;}
int main(void){reset();deadline=now_ms();assert(efs_append_flush_report_run(NULL,42)==EFS_ERR_BUSY&&!syncs&&!flushes&&!reports);
 reset();expire_sync=1;assert(efs_append_flush_report_run(NULL,42)==EFS_ERR_BUSY&&syncs==1&&!flushes&&!reports);
 reset();pthread_mutex_lock(&stripe);deadline=now_ms()+10;assert(efs_append_flush_report_run(NULL,42)==EFS_ERR_BUSY&&!flushes&&!reports);pthread_mutex_unlock(&stripe);
 reset();sync_rc=EFS_ERR_IO;assert(efs_append_flush_report_run(NULL,42)==EFS_ERR_IO&&!flushes);
 reset();flush_rc=EFS_ERR_IO;assert(efs_append_flush_report_run(NULL,42)==EFS_ERR_IO&&flushes==1&&!reports);assert(!pthread_mutex_trylock(&stripe));pthread_mutex_unlock(&stripe);
 reset();assert(!efs_append_flush_report_run(NULL,42)&&flushes==1&&reports==1);
 reset();dirty=0;deadline=now_ms();assert(!efs_append_flush_report_run(NULL,42)&&!syncs&&!flushes);return 0;}
'''
with tempfile.TemporaryDirectory(prefix='efs-append-flush-') as d:
 p=Path(d)/'t.c';p.write_text(code)
 subprocess.run(['cc','-std=gnu11','-pthread','-Wall','-Wextra','-Werror','-I'+str(root/'include'),'-I'+str(root/'src/client'),str(p),'-o',str(p.with_suffix(''))],check=True)
 subprocess.run([str(p.with_suffix(''))],check=True,timeout=5)
print('append flush: expired/free/contended stripe, post-drain expiry and failed-flush retention PASS')
