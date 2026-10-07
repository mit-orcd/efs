#!/usr/bin/env python3
"""Production global/per-inode drains retain errors and pending ownership on expiry."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[1];s=(root/'src/client/efs_fuse.c').read_text()
def fn(prefix):
 a=s.index(prefix)
 while ';' in s[a:s.index('\n',a)]:a=s.index(prefix,a+len(prefix))
 return s[a:s.index('\n}',a)+2]
code=r'''
#include "client_internal.h"
#include <assert.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>
#define EFS_WB_DEPTH 2
static struct {int ready,count,inflight,head,nworkers,err;efs_ino_t err_ino,busy_ino[2];struct {efs_ino_t ino;}q[2];pthread_mutex_t mu;pthread_cond_t idle;}g_wb={.mu=PTHREAD_MUTEX_INITIALIZER,.idle=PTHREAD_COND_INITIALIZER};
static uint64_t deadline;
static int fail_wait;
static uint64_t stats_now_ms(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000+t.tv_nsec/1000000;}
uint64_t efs_client_rpc_deadline_ms(void){return deadline;}
int efs_client_rpc_past_deadline(void){return deadline&&stats_now_ms()>=deadline;}
static int timed_wait(pthread_cond_t*c,pthread_mutex_t*m,const struct timespec*t){return fail_wait?EINVAL:pthread_cond_timedwait(c,m,t);}
#define pthread_cond_timedwait timed_wait
'''
for p in ['static int wb_wait_idle_budget(', 'static int wb_lock_budget(', 'static int efs_wb_sync(void)', 'static int efs_wb_ino_pending_locked(', 'static int efs_wb_sync_ino(']:code+=fn(p)
code+=r'''
static int drain(int per){return per?efs_wb_sync_ino(42):efs_wb_sync();}
int main(void){g_wb.ready=1;g_wb.count=1;g_wb.q[0].ino=42;g_wb.err=EFS_ERR_IO;g_wb.err_ino=42;
 for(int per=0;per<2;per++){
 uint64_t start=stats_now_ms();deadline=start+10;assert(drain(per)==EFS_ERR_BUSY&&stats_now_ms()-start<75&&g_wb.count==1&&g_wb.err==EFS_ERR_IO);
 assert(!pthread_mutex_trylock(&g_wb.mu));pthread_mutex_unlock(&g_wb.mu);
 fail_wait=1;deadline=stats_now_ms()+100;assert(drain(per)==EFS_ERR_IO&&g_wb.count==1&&g_wb.err==EFS_ERR_IO);fail_wait=0;
 }
 g_wb.count=0;deadline=0;assert(efs_wb_sync_ino(7)==EFS_OK&&g_wb.err==EFS_ERR_IO);assert(efs_wb_sync_ino(42)==EFS_ERR_IO&&!g_wb.err);
 g_wb.err=EFS_ERR_IO;assert(efs_wb_sync()==EFS_ERR_IO&&!g_wb.err);return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='efs-wb-drain-') as d:
 p=Path(d)/'t.c';p.write_text(code)
 subprocess.run(['cc','-std=gnu11','-pthread','-Wall','-Wextra','-Werror','-I'+str(root/'include'),'-I'+str(root/'src/client'),str(p),'-o',str(p.with_suffix(''))],check=True)
 subprocess.run([str(p.with_suffix(''))],check=True,timeout=5)
print('WB drains: short deadline, wait errors, mutex release and pending/sticky-error retention PASS')
