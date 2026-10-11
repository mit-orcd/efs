#!/usr/bin/env python3
"""Actual cohort fanout: bounded workers, charged stacks and joined failures."""
from pathlib import Path
import os,shlex,subprocess,tempfile
root=Path(__file__).resolve().parents[1];s=(root/'src/client/pack_write.c').read_text()
code=s[:s.index('static uint64_t pack_now(')].replace('static int pack_batch_peer_unsupported(void);\n','')+r'''
#include <assert.h>
#include <errno.h>
#include <stdio.h>
static __thread uint64_t deadline;
uint64_t efs_client_rpc_deadline_ms(void){return deadline;}
void efs_client_rpc_set_deadline_ms(uint64_t v){deadline=v;}
static pthread_mutex_t mu=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv=PTHREAD_COND_INITIALIZER;
static unsigned calls[PACK_WRITERS],active,peak,arrived;
static int fail_index=5,fail_create,barrier,expired;
static struct pack_context context;
static int checked_create(pthread_t *t,const pthread_attr_t *attr,void *(*fn)(void *),void *arg){
 void *stack;size_t bytes;assert(!pthread_attr_getstack(attr,&stack,&bytes));
 assert(bytes==PACK_PUBLISH_STACK_BYTES&&(uintptr_t)stack%PACK_PUBLISH_STACK_ALIGN==0);
 assert((uintptr_t)stack>=(uintptr_t)context.publication_stacks&&
        (uintptr_t)stack+bytes<=(uintptr_t)(context.publication_stacks+PACK_PUBLISH_WORKERS-1));
 if(fail_create)return EAGAIN;
 return pthread_create(t,attr,fn,arg);
}
#define pthread_create checked_create
static int pack_publish_record(struct pack_context *p,uint32_t i){
 assert(p==&context&&i<p->cohort.count&&deadline==12345);
 if(__atomic_load_n(&p->writers[i].done,__ATOMIC_ACQUIRE))return 0;
 if(expired)return EFS_ERR_BUSY;
 pthread_mutex_lock(&mu);calls[i]++;active++;if(active>peak)peak=active;
 assert(active<=PACK_PUBLISH_WORKERS);
 if(barrier&&i<PACK_PUBLISH_WORKERS){arrived++;pthread_cond_broadcast(&cv);while(arrived<PACK_PUBLISH_WORKERS)pthread_cond_wait(&cv,&mu);}
 pthread_mutex_unlock(&mu);usleep(1000);
 pthread_mutex_lock(&mu);active--;pthread_mutex_unlock(&mu);
 if((int)i==fail_index)return EFS_ERR_NET;
 __atomic_store_n(&p->writers[i].done,1,__ATOMIC_RELEASE);return 0;
}
static void pack_publish_batch(struct pack_context *p){(void)p;assert(0);}
'''+s[s.index('struct pack_parallel_run {'):s.index('static int pack_retire_one(')]+s[s.index('static int pack_publish_one('):s.index('static void pack_publish_batch(')]+s[s.index('static int pack_publish_cohort('):s.index('static int pack_progress(')]+r'''
int main(void){context.cohort.count=PACK_WRITERS;deadline=12345;barrier=1;
 assert(pack_publish_cohort(&context)==EFS_ERR_NET&&deadline==12345&&!active&&peak==PACK_PUBLISH_WORKERS);
 for(unsigned i=0;i<PACK_WRITERS;i++)assert(calls[i]==1&&context.writers[i].done==(i!=5));
 /* All helpers are joined before failure escapes; retry visits only ownership
  * still retained by the failed record, without redoing completed neighbors. */
 fail_index=-1;barrier=0;assert(!pack_publish_cohort(&context)&&!active&&deadline==12345);
 for(unsigned i=0;i<PACK_WRITERS;i++)assert(calls[i]==(i==5?2u:1u)&&context.writers[i].done);
 memset(&context,0,sizeof(context));memset(calls,0,sizeof(calls));context.cohort.count=7;fail_create=1;peak=0;
 assert(!pack_publish_cohort(&context)&&peak==1&&!active&&deadline==12345);
 for(unsigned i=0;i<7;i++)assert(calls[i]==1&&context.writers[i].done);
 memset(&context,0,sizeof(context));memset(calls,0,sizeof(calls));context.cohort.count=7;fail_create=0;expired=1;
 assert(pack_publish_cohort(&context)==EFS_ERR_BUSY&&!active&&deadline==12345);
 for(unsigned i=0;i<7;i++)assert(!calls[i]&&!context.writers[i].done);
 puts("mounted pack fanout: four-worker bound, precharged stacks, inherited/restored deadline, joined partial errors, exact retry and thread-creation fallback PASS");}
'''
with tempfile.TemporaryDirectory(prefix='efs-pack-parallel-') as td:
 p=Path(td)/'t.c';p.write_text(code);exe=p.with_suffix('')
 subprocess.run(shlex.split(os.environ.get('CC','cc'))+['-std=gnu11','-Wall','-Wextra','-Werror','-Wno-unused-variable','-pthread','-I'+str(root/'include'),'-I'+str(root/'src/client'),str(p),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True,timeout=5)
