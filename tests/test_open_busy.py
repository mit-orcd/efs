#!/usr/bin/env python3
"""Actual FUSE OPEN ownership across transient BUSY and a single call deadline."""
from pathlib import Path
import os,re,shlex,subprocess,tempfile
root=Path(__file__).resolve().parents[1]
s=Path(os.environ.get('EFS_FUSE_SOURCE',root/'src/client/efs_fuse.c')).read_text()
def function(name):
 m=re.search(r'^static (?:int|void|uint64_t) '+name+r'\([^;]+?\)\n\{',s,re.M)
 return s[m.start():s.index('\n}',m.end())+2] if m else ''
a=s.index('static pthread_mutex_t g_open_mu');b=s.index('/* Returns 1 when this is the first open',a)
code=r'''
#include "client_internal.h"
static __thread uint64_t t_open_read_size;
uint64_t efs_client_map_clock(void){return 1;}
void efs_client_map_install(efs_ino_t i,const struct efs_chunk_rec *r,uint32_t n,uint64_t u){(void)i;(void)r;(void)n;(void)u;}
void efs_client_prefetch_open(efs_ino_t i,uint64_t s){(void)i;(void)s;}
#include "efs/session.h"
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
struct efs_client g_client;
static _Thread_local uint64_t deadline;
static uint64_t clock_ms;static int begins,resumes,busy,expire,known,io,begin_busy,admission_sleep;
static pthread_mutex_t *open_edge_mu(efs_ino_t ino);
uint64_t efs_client_rpc_deadline_ms(void){return deadline;}
void efs_client_rpc_set_deadline_ms(uint64_t v){deadline=v;}
static __attribute__((unused)) uint64_t stats_now_ms(void){return clock_ms;}
int efs_client_rpc_past_deadline(void){return deadline && clock_ms>=deadline;}
static __attribute__((unused)) int fixture_sleep(unsigned us){if(admission_sleep){pthread_mutex_t *mu=open_edge_mu(42);assert(!pthread_mutex_trylock(mu));pthread_mutex_unlock(mu);admission_sleep=0;}clock_ms+=(us+999)/1000;return 0;}
#define usleep fixture_sleep
void efs_client_opid_wait(unsigned max_ms){usleep(max_ms*1000u);}
int efs_client_rpc_getchunks_fileid(efs_export_id_t ex,efs_ino_t ino,uint64_t gen,uint32_t start,struct efs_chunk_rec *r,uint32_t *n,uint64_t *out){
 (void)ex;(void)ino;(void)gen;(void)start;(void)r;(void)n;*out=99;return EFS_OK;
}
int efs_client_lease_begin(struct efs_client_lease_request *r,efs_ino_t ino,uint64_t gen,unsigned flags,uint64_t token){
 assert(!r->active && ino==42 && gen==99 && !token);begins++;
 if(begin_busy){begin_busy--;admission_sleep=1;return EFS_ERR_BUSY;}
 *r=(struct efs_client_lease_request){.ino=ino,.generation=gen,.flags=flags,.active=1};r->q.id.seq=55;return EFS_OK;
}
int efs_client_lease_resume(struct efs_client_lease_request *r,int probe){
 assert(!probe && r->active && r->q.id.seq==55);resumes++;
 if(r->complete)return r->result;
 if(efs_client_rpc_past_deadline())return EFS_ERR_BUSY;
 r->attempted=1;
 if(io)return EFS_ERR_IO;
 if(busy){if(busy>0)busy--;if(expire)clock_ms=deadline;
  if(known){r->complete=1;r->result=EFS_ERR_BUSY;}return EFS_ERR_BUSY;}
 r->complete=1;r->result=EFS_OK;return EFS_OK;
}
int efs_client_lease_reset(struct efs_client_lease_request *r){assert(!r->active||r->complete);memset(r,0,sizeof(*r));return EFS_OK;}
''' + s[a:b]
for name in ('efs_open_note','efs_open_set_leased','efs_open_retry_order','efs_close_note','efs_open_release_pending_run','efs_open_release_pending','wb_lock_budget','efs_open_lease_resume_wait','efs_open_acquire_fileid_run','efs_open_acquire_fileid_mode','efs_open_acquire_fileid','efs_open_acquire'):
 if name=='wb_lock_budget' and not function('efs_open_acquire_fileid_run'):continue
 code+='\n'+function(name)
code+=r'''
static void close_owned(void){int leased;assert(efs_close_note(42,&leased) && leased);assert(!efs_open_release_pending(42) && !g_open_refs);}
int main(void){
 begin_busy=3;assert(!efs_open_acquire(42));assert(begins==4 && !begin_busy && !admission_sleep && g_open_refs->edge.complete);close_owned();begins=resumes=0;
 /* Pending CLOSE admission must release the stripe before retry too;
  * otherwise a new OPEN exposes the transient local capacity refusal. */
 assert(!efs_open_acquire(42));int leased=0;
 assert(efs_close_note(42,&leased) && leased && g_open_refs->close_pending);
 begin_busy=3;assert(!efs_open_acquire(42));
 assert(!begin_busy && !admission_sleep && g_open_refs->n==1);
 close_owned();begins=resumes=0;
 busy=2;assert(!efs_open_acquire(42));assert(begins==1 && resumes==3 && g_open_refs->n==1 && g_open_refs->edge.complete && !deadline);close_owned();
 /* A known rejection is final, not an excuse to allocate a different edge. */
 busy=1;known=1;int before=begins;assert(efs_open_acquire(42)==EFS_ERR_BUSY);assert(begins==before+1 && !g_open_refs && !deadline);known=0;
 /* Expiration retains the exact unknown OPEN for later compensating close. */
 busy=-1;expire=1;deadline=7;clock_ms=1;before=begins;
 assert(efs_open_acquire(42)==EFS_ERR_BUSY && deadline==7);
 assert(begins==before+1 && g_open_refs && !g_open_refs->n && g_open_refs->close_pending && g_open_refs->edge.active && !g_open_refs->edge.complete && g_open_refs->edge.q.id.seq==55);
 busy=expire=0;deadline=0;clock_ms=1;assert(!efs_open_release_pending(42) && !g_open_refs);
 /* An expired contender cannot wait indefinitely or invent a new request. */
 pthread_mutex_t *mu=open_edge_mu(42);pthread_mutex_lock(mu);deadline=5;clock_ms=1;before=begins;
 assert(efs_open_acquire(42)==EFS_ERR_BUSY && deadline==5 && begins==before && !g_open_refs);pthread_mutex_unlock(mu);deadline=0;
 io=1;before=begins;assert(efs_open_acquire(42)==EFS_ERR_IO && begins==before+1 && g_open_refs && !g_open_refs->n);io=0;assert(!efs_open_release_pending(42) && !g_open_refs);
 puts("OPEN BUSY: same retained identity, terminal verdict, deadline, mutex budget and unresolved ownership PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='efs-open-busy-') as t:
 p=Path(t);(p/'test.c').write_text(code)
 subprocess.run(shlex.split(os.environ.get('CC','cc'))+['-std=gnu11','-D_GNU_SOURCE','-Wall','-Wextra','-Werror','-pthread','-I'+str(root/'include'),'-I'+str(root/'src/client'),str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
