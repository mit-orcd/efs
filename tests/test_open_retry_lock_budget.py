#!/usr/bin/env python3
"""A busy inode stripe cannot steal the budget reserved for another queue."""
from pathlib import Path
import os,shlex,subprocess,tempfile
root=Path(__file__).resolve().parents[1];s=(root/'src/client/efs_fuse.c').read_text()
def function(name):
 a=s.index('static int '+name+'(');return s[a:s.index('\n}',a)+2]
code=r'''
#include <assert.h>
#include <stdint.h>
#include <pthread.h>
#include <errno.h>
#include <stdio.h>
#define EFS_OK 0
#define EFS_ERR_BUSY -13
#define EFS_ERR_IO -5
typedef uint64_t efs_ino_t;
struct efs_open_ref {efs_ino_t ino;int close_pending;uint64_t retry_order;struct efs_open_ref *next;};
static struct efs_open_ref row={.ino=42,.close_pending=1,.retry_order=3},*g_open_refs=&row;
static pthread_mutex_t g_open_mu=PTHREAD_MUTEX_INITIALIZER, stripe=PTHREAD_MUTEX_INITIALIZER;
static uint64_t now,deadline=10;static int releases;
static uint64_t efs_client_rpc_deadline_ms(void){return deadline;}
static int efs_client_rpc_past_deadline(void){return deadline && now>=deadline;}
static void usleep(unsigned n){now+=(n+999)/1000;}
static pthread_mutex_t *open_edge_mu(efs_ino_t ino){assert(ino==42);return &stripe;}
static int efs_open_release_pending(efs_ino_t ino){assert(ino==42);releases++;return EFS_OK;}
'''+function('wb_lock_budget')+'\n'+function('efs_open_drain_pending')+r'''
int main(void){
 assert(!pthread_mutex_lock(&stripe));
 assert(efs_open_drain_pending()==EFS_ERR_BUSY && now==10 && !releases && g_open_refs==&row);
 assert(!pthread_mutex_unlock(&stripe));now=0;
 assert(efs_open_drain_pending()==EFS_ERR_BUSY && releases==1 && now==0);
 g_open_refs=NULL;assert(efs_open_drain_pending()==EFS_OK);
 puts("retry stripe: deadline refuses contended edge without losing ownership; later release progresses PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='efs-open-retry-lock-') as directory:
 p=Path(directory)/'t.c';p.write_text(code)
 compile=shlex.split(os.environ.get('CC','cc'))+['-std=gnu11','-Wall','-Wextra','-Werror','-pthread',str(p),'-o',str(p.with_suffix(''))]
 subprocess.run(compile,check=True);subprocess.run([str(p.with_suffix(''))],check=True)
 legacy=code.replace('int rc = wb_lock_budget(edge);','(void)wb_lock_budget; int rc = pthread_mutex_lock(edge);')
 p.write_text(legacy);subprocess.run([x for x in compile if x!='-Werror'],check=True)
 try:subprocess.run([str(p.with_suffix(''))],capture_output=True,timeout=.5)
 except subprocess.TimeoutExpired:print('legacy unbounded stripe wait negative control rejected PASS')
 else:raise AssertionError('legacy stripe wait must block')
