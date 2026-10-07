#!/usr/bin/env python3
"""Production GET-pool startup cleans partial initialization and supports restart."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[1];s=(root/'src/client/read.c').read_text()
def fn(p):
 a=s.index(p);return s[a:s.index('\n}',a)+2]
def block(n):
 a=s.index('struct '+n+' {');return s[a:s.index('\n};',a)+3]
a=s.index('#define GET_POOL_QDEPTH');b=s.index('static void *get_pool_thread(',a)
code=r'''
#include "client_internal.h"
#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
'''+block('get_batch')+block('chunk_get_job')+'\n'+s[a:b]+r'''
static int fail_kind,mi,ci,ti,md,cd,joined;
static int mock_mi(pthread_mutex_t*m,const pthread_mutexattr_t*a){mi++;return fail_kind==1&&mi==2?EAGAIN:pthread_mutex_init(m,a);}
static int mock_ci(pthread_cond_t*c,const pthread_condattr_t*a){ci++;return fail_kind==2&&ci==2?EAGAIN:pthread_cond_init(c,a);}
static int mock_create(pthread_t*t,const pthread_attr_t*a,void*(*f)(void*),void*p){ti++;return fail_kind==3&&ti==2?EAGAIN:pthread_create(t,a,f,p);}
static int mock_md(pthread_mutex_t*m){md++;return pthread_mutex_destroy(m);}
static int mock_cd(pthread_cond_t*c){cd++;return pthread_cond_destroy(c);}
static int mock_join(pthread_t t,void**p){joined++;return pthread_join(t,p);}
static uint32_t data_chunk_size(void){return 131072;}
void efs_buf_free(void*p,uint32_t n){(void)n;free(p);}
static void *get_job_run(void*p){(void)p;assert(0);return NULL;}
#define pthread_mutex_init mock_mi
#define pthread_cond_init mock_ci
#define pthread_create mock_create
#define pthread_mutex_destroy mock_md
#define pthread_cond_destroy mock_cd
#define pthread_join mock_join
'''+fn('static void *get_pool_thread(')+fn('static int get_pool_ensure(void)')+fn('void efs_client_read_pools_stop(void)')+r'''
static void reset(int kind){fail_kind=kind;mi=ci=ti=md=cd=joined=0;}
int main(void){
 reset(1);assert(get_pool_ensure()==-1&&mi==2&&md==1&&ci==1&&cd==1&&!ti);
 reset(2);assert(get_pool_ensure()==-1&&mi==2&&md==2&&ci==2&&cd==1&&!ti);
 reset(3);assert(get_pool_ensure()==-1&&ti==2&&joined==1&&md==GET_POOL_N&&cd==GET_POOL_N&&!g_get_pool.ready&&!g_get_pool.shutdown);
 for(int round=0;round<2;round++) {reset(0);assert(!get_pool_ensure()&&g_get_pool.ready&&g_get_pool.nworkers==GET_POOL_N&&ti==GET_POOL_N);efs_client_read_pools_stop();assert(joined==GET_POOL_N&&md==GET_POOL_N&&cd==GET_POOL_N&&!g_get_pool.ready&&!g_get_pool.shutdown&&!g_get_pool.nworkers);}
 efs_client_read_pools_stop();return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='efs-get-startup-') as d:
 p=Path(d)/'t.c';p.write_text(code)
 subprocess.run(['cc','-std=gnu11','-pthread','-Wall','-Wextra','-Werror','-I'+str(root/'include'),'-I'+str(root/'src/client'),str(p),'-o',str(p.with_suffix(''))],check=True)
 subprocess.run([str(p.with_suffix(''))],check=True,timeout=10)
print('GET pool: mutex/condition/thread failures, complete cleanup, atomic readiness and repeated stop/restart PASS')
