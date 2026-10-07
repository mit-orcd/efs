#!/usr/bin/env python3
"""Production WB worker must inherit budgets and finish ownership on expiry."""
from pathlib import Path
import re,subprocess,tempfile
root=Path(__file__).resolve().parents[1]
s=(root/'src/client/efs_fuse.c').read_text()
def fn(name):
 m=re.search(r'^static [^\n;]*\b'+name+r'\([^;]+?\)\n\{',s,re.M)
 return s[m.start():s.index('\n}',m.end())+2]
a=s.index('struct efs_wb_job {');job=s[a:s.index('\n};',a)+3]
code=r'''
#include "client_internal.h"
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>
#define EFS_WB_DEPTH 2
'''+job+r'''
static struct {
 pthread_mutex_t mu;pthread_cond_t not_empty,not_full,idle;
 struct efs_wb_job q[2];int count,head,shutdown,inflight,nworkers,err;
 efs_ino_t busy_ino[2],err_ino;uint64_t busy_off[2],queued_bytes,inflight_bytes;
 size_t busy_len[2];
} g_wb={.mu=PTHREAD_MUTEX_INITIALIZER,.not_empty=PTHREAD_COND_INITIALIZER,
.not_full=PTHREAD_COND_INITIALIZER,.idle=PTHREAD_COND_INITIALIZER};
static pthread_mutex_t stripe=PTHREAD_MUTEX_INITIALIZER;
static __thread uint64_t deadline;
static unsigned freed,writes,begins,ends;
static uint64_t stats_now_ms(void) {struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000+t.tv_nsec/1000000;}
uint64_t efs_client_rpc_deadline_ms(void) {return deadline;}
void efs_client_rpc_set_deadline_ms(uint64_t d) {deadline=d;}
int efs_client_rpc_past_deadline(void) {return deadline&&stats_now_ms()>=deadline;}
static uint32_t fuse_chunk_size(void) {return 131072;}
static pthread_mutex_t *efs_wb_ino_lock(efs_ino_t i) {(void)i;return &stripe;}
static void lookup_memo_mutation_begin(void) {begins++;}
static void lookup_memo_mutation_end(void) {ends++;}
static void wb_job_release_buf(char *p,char *base,size_t n) {(void)base;(void)n;free(p);freed++;}
int efs_client_write_no_replicate(efs_ino_t i,uint64_t o,size_t n,const char *p) {(void)i;(void)o;(void)n;assert(*p=='X'&&deadline&&!efs_client_rpc_past_deadline());writes++;return EFS_OK;}
static void efs_fuse_log_err(const char *op,int rc,efs_ino_t i,uint64_t off,size_t n,const char *p) {(void)op;(void)rc;(void)i;(void)off;(void)n;(void)p;}
const char *efs_strerror(int rc) {(void)rc;return "error";}
'''
for name in ['wb_ranges_overlap','wb_claim_span','wb_overlap_inflight','wb_wait_idle_budget','wb_lock_budget','efs_wb_thread']: code+=fn(name)
code+=r'''
static void run(int overlap,int locked,int expired) {
 int done=0,rc=EFS_OK;pthread_cond_t cv=PTHREAD_COND_INITIALIZER;
 g_wb.head=0;g_wb.count=1;g_wb.shutdown=1;g_wb.nworkers=2;g_wb.err=0;
 g_wb.busy_ino[1]=overlap?42:0;g_wb.busy_len[1]=overlap?131072:0;
 char *p=malloc(1);*p='X';
 g_wb.q[0]=(struct efs_wb_job){.ino=42,.size=1,.buf=p,.done=&done,.done_rc=&rc,.done_cv=&cv,.deadline=stats_now_ms()+(expired?0:20)};
 g_wb.queued_bytes=1;
 if(locked)pthread_mutex_lock(&stripe);
 deadline=777;uint64_t start=stats_now_ms();efs_wb_thread(NULL);
 if(locked)pthread_mutex_unlock(&stripe);
 assert(done&&deadline==777&&!g_wb.count&&!g_wb.inflight&&!g_wb.inflight_bytes&&!g_wb.queued_bytes&&!g_wb.busy_ino[0]);
 assert(stats_now_ms()-start<200);
 assert(rc==((overlap||locked||expired)?EFS_ERR_BUSY:EFS_OK));
 pthread_cond_destroy(&cv);
}
int main(void) {run(1,0,0);run(0,1,0);run(0,0,1);run(0,0,0);assert(freed==4&&writes==1&&begins==1&&ends==1);return 0;}
'''
with tempfile.TemporaryDirectory(prefix='efs-wb-budget-') as d:
 p=Path(d)/'t.c';p.write_text(code)
 subprocess.run(['cc','-std=gnu11','-pthread','-Wall','-Wextra','-Werror','-I'+str(root/'include'),'-I'+str(root/'src/client'),str(p),'-o',str(p.with_suffix(''))],check=True)
 subprocess.run([str(p.with_suffix(''))],check=True,timeout=5)
print('WB worker: overlap/stripe/expired budgets, inherited TLS and exact completion cleanup PASS')
