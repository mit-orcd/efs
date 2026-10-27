#!/usr/bin/env python3
"""Execute pressure -> real REPORT completion, including held-unit handling.

Pressure must retain held units without waiting/flushing future writes. A
whole-inode flusher that refuses is supplied, so the old synchronous call
fails this test; K38_FUSE_SOURCE selects that call site for a negative control.
Foreground fsync must still wait/flush before success. Snapshot/PUTs are
adapters, and both production completion entry points execute in this test.
"""
from pathlib import Path
import os, re, shlex, subprocess, tempfile
root=Path(__file__).resolve().parents[1]
w=(root/'src/client/write.c').read_text();f=Path(os.environ.get('K38_FUSE_SOURCE',root/'src/client/efs_fuse.c')).read_text()
def fn(s,name):
 m=re.search(r'^(?:static )?(?:int|void) '+name+r'\([^;]+?\)\n\{',s,re.M);assert m,name
 return s[m.start():s.index('\n}',m.end())+2]
code=r'''
#include <stdint.h>
#include <stdlib.h>
#include <pthread.h>
#include <assert.h>
#include <stdio.h>
#define EFS_OK 0
#define EFS_ERR_BUSY -13
#define EFS_ERR_IO -1
#define EFS_ERR_AGAIN -18
#define EFS_WRITE_PIPELINE 32
typedef uint64_t efs_ino_t;typedef unsigned efs_node_id_t;
struct efs_chunk_entry {int unused;};struct flush_pipe {int rc;};
static int stolen,drained,reports,fail_at,lock_rc,waits,whole_flushes,held,foreground,whole_fail;static uint64_t deadline;
static pthread_mutex_t mu=PTHREAD_MUTEX_INITIALIZER;
static uint64_t efs_client_rpc_deadline_ms(void){return deadline;}
static int efs_client_rpc_past_deadline(void){return deadline==1;}
static void efs_client_rpc_set_deadline_ms(uint64_t d){deadline=d;}
static uint64_t stats_now_ms(void){return 10;}
static uint64_t report_clock_ms(void){return stats_now_ms();}
static uint64_t efs_client_rpc_resolve_budget_ms(void){return 8000;}
static void efs_client_put_settle_kick(void){}
static int unit_wait_idle(efs_ino_t ino){assert(ino==42);waits++;return EFS_OK;}
static int efs_dcache_flush_ino(efs_ino_t ino){assert(ino==42);whole_flushes++;return whole_fail?EFS_ERR_BUSY:EFS_OK;}
static int efs_dcache_flush_all(void){whole_flushes++;return EFS_ERR_BUSY;}
static int report_dirty_ino_run(efs_ino_t ino,int sync,uint64_t end,int *resolved){
assert(ino==42&&sync);(void)end;*resolved=0;reports++;
if(foreground && reports>1){held=0;return EFS_OK;}
/* Carried records committed, but other units remain held by writers. */
held=1;return -18;
}
static void dcache_ensure(void){}
static int dcache_dirty_cis_of(efs_ino_t ino,uint32_t **out){assert(ino==42);*out=malloc(100*sizeof(**out));assert(*out);for(int i=0;i<100;i++)(*out)[i]=i;return 100;}
static void flush_pipe_init(struct flush_pipe*p){p->rc=0;}
static int dcache_steal_dirty(efs_ino_t ino,uint32_t ci,uint8_t **b,uint32_t*l,uint64_t*bg,uint64_t*seq,uint32_t*off,uint32_t*len,struct efs_chunk_entry*obs,int*have){
(void)ino;(void)ci;(void)l;(void)bg;(void)seq;(void)off;(void)len;(void)obs;(void)have;
stolen++;if(fail_at==stolen)return EFS_ERR_IO;*b=malloc(1);assert(*b);return 1;}
static int flush_pipe_add(struct flush_pipe*p,efs_ino_t ino,uint32_t ci,uint8_t*b,uint32_t l,uint64_t bg,uint64_t seq,uint32_t off,uint32_t len,const struct efs_chunk_entry*obs){
(void)p;(void)ino;(void)ci;(void)l;(void)bg;(void)seq;(void)off;(void)len;(void)obs;free(b);return 0;}
static int flush_pipe_drain(struct flush_pipe*p){drained++;return p->rc;}
static pthread_mutex_t *append_mu(efs_ino_t ino){assert(ino==42);return &mu;}
static int wb_lock_budget(pthread_mutex_t*m){if(lock_rc)return lock_rc;pthread_mutex_lock(m);return 0;}
'''+fn(w,'report_dirty_ino_complete')+fn(w,'efs_client_report_dirty_ino')+fn(w,'efs_client_report_pressure_ino')+fn(w,'dcache_flush_ino_pass')+fn(w,'efs_dcache_flush_pressure_ino')+fn(f,'fuse_pressure_flush')+r'''
static void reset(void){stolen=drained=reports=fail_at=lock_rc=waits=whole_flushes=foreground=0;held=1;whole_fail=1;deadline=200;}
int main(void){
reset();assert(!fuse_pressure_flush(42));assert(stolen==32&&drained==1&&reports==1&&deadline==200&&held&&!waits&&!whole_flushes);
reset();assert(!dcache_flush_ino_pass(42,0)&&stolen==100&&drained==1);
reset();fail_at=7;assert(fuse_pressure_flush(42)==EFS_ERR_IO&&stolen==7&&drained==1&&!reports&&deadline==200);
assert(!pthread_mutex_trylock(&mu));pthread_mutex_unlock(&mu);
reset();lock_rc=EFS_ERR_BUSY;assert(fuse_pressure_flush(42)==EFS_ERR_BUSY&&!stolen&&!reports&&deadline==200);
reset();deadline=1;assert(fuse_pressure_flush(42)==EFS_ERR_BUSY&&!stolen&&!reports&&deadline==1);
/* A real fsync still waits and flushes held units before success. */
reset();foreground=1;whole_fail=0;assert(efs_client_report_dirty_ino(42,1)==EFS_OK);
assert(reports==2&&waits==1&&whole_flushes==1&&!held&&deadline==200);
puts("pressure cohort: fixed snapshot, held units retained without recursive full flush, foreground fsync strict, failures/deadline preserve ownership PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='efs-pressure-cohort-') as d:
 p=Path(d)/'t.c';p.write_text(code)
 subprocess.run(shlex.split(os.environ.get('CC','cc'))+shlex.split(os.environ.get('EXTRA_CFLAGS',''))+['-std=gnu11','-Wall','-Wextra','-Werror','-pthread',str(p),'-o',str(p.with_suffix(''))],check=True)
 subprocess.run([str(p.with_suffix(''))],check=True,timeout=15)
