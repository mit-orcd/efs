#!/usr/bin/env python3
"""Production root inventories overlap with bounded helpers and fail closed."""
from pathlib import Path
import os,re,shlex,subprocess,tempfile
root=Path(__file__).resolve().parents[1]
s=(root/'src/server/store.c').read_text();a=s.index('struct usage_scan_batch {');b=s.index('\nvoid server_format_storage_paths',a)
code=r'''
#include "efs/common.h"
#include <pthread.h>
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
struct efsd_server {unsigned storage_path_count;int storage_engine;const char *storage_paths[EFS_MAX_STORAGE_PATHS];};
static pthread_mutex_t gate=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv=PTHREAD_COND_INITIALIZER;
static unsigned active,peak,entered,target,attempts,fail_on,visits[EFS_MAX_STORAGE_PATHS];
static int released,error_root,overflow;
static uint64_t server_compute_usage(const char *path){
 unsigned i=(unsigned)atoi(path);assert(i<EFS_MAX_STORAGE_PATHS);
 pthread_mutex_lock(&gate);visits[i]++;active++;if(active>peak)peak=active;
 if(++entered>=target){released=1;pthread_cond_broadcast(&cv);}
 while(!released)pthread_cond_wait(&cv,&gate);
 pthread_mutex_unlock(&gate);usleep(1000);
 pthread_mutex_lock(&gate);active--;pthread_mutex_unlock(&gate);
 return (int)i==error_root?UINT64_MAX:overflow?UINT64_MAX-1:100+i;
}
static int fixture_create(pthread_t *t,const pthread_attr_t *a,void *(*fn)(void *),void *arg){
 if(++attempts==fail_on)return EAGAIN;
 return pthread_create(t,a,fn,arg);
}
#define pthread_create fixture_create
'''+s[a:b]+r'''
#undef pthread_create
static void reset(unsigned expected){active=peak=entered=attempts=released=0;target=expected;error_root=-1;overflow=0;memset(visits,0,sizeof(visits));}
static void check(unsigned count,int containers,unsigned failure){
 struct efsd_server server={.storage_path_count=count,.storage_engine=containers};
 char names[EFS_MAX_STORAGE_PATHS][16];uint64_t values[EFS_MAX_STORAGE_PATHS];
 unsigned actual=count?count:1,helpers=containers?0:actual<4?actual-1:3;
 if(failure && failure<=helpers)helpers=failure-1;
 for(unsigned i=0;i<actual;i++){snprintf(names[i],sizeof(names[i]),"%u",i);server.storage_paths[i]=names[i];}
 reset(helpers+1);fail_on=failure;
 assert(!server_compute_roots_usage(&server,values));assert(!active && peak==helpers+1 && peak<=4);
 for(unsigned i=0;i<actual;i++)assert(visits[i]==1 && values[i]==100+i);
 reset(helpers+1);fail_on=failure;error_root=(int)(actual-1);
 assert(server_compute_roots_usage(&server,values)==EFS_ERR_IO && !active);
 for(unsigned i=0;i<actual;i++)assert(visits[i]==1);
 reset(helpers+1);fail_on=failure;overflow=1;
 assert(server_compute_local_usage(&server)==(actual>1?UINT64_MAX:UINT64_MAX-1) && !active);
}
int main(void){
 check(0,0,0);check(1,0,0);check(2,0,0);check(6,0,0);check(EFS_MAX_STORAGE_PATHS,0,0);
 check(6,1,0);check(6,0,1);check(6,0,2);check(6,0,3);
 uint64_t values[EFS_MAX_STORAGE_PATHS];struct efsd_server invalid={.storage_path_count=EFS_MAX_STORAGE_PATHS+1};
 assert(server_compute_roots_usage(NULL,values)==EFS_ERR_INVAL);
 assert(server_compute_roots_usage(&invalid,values)==EFS_ERR_INVAL);
 assert(server_compute_local_usage(&invalid)==UINT64_MAX);
 invalid.storage_path_count=1;assert(server_compute_roots_usage(&invalid,NULL)==EFS_ERR_INVAL);
 puts("root inventory: exact once, max4 walks, full join, fallback, unknown-root refusal, overflow and serial containers PASS");
}
'''
flags=shlex.split(os.environ.get('CC','cc'))+['-std=gnu11','-D_GNU_SOURCE','-Wall','-Wextra','-Werror','-pthread','-I'+str(root/'include')]
with tempfile.TemporaryDirectory(prefix='efs-root-inventory-') as td:
 p=Path(td)/'test.c';p.write_text(code);binary=p.with_suffix('');subprocess.run(flags+[str(p),'-o',str(binary)],check=True);subprocess.run([str(binary)],check=True,timeout=15)
 p.write_text(code.replace('while (!s->storage_engine && created < 3 && created + 1 < count)', 'while (0)'))
 subprocess.run(flags+[str(p),'-o',str(binary)],check=True)
 try:
  result=subprocess.run([str(binary)],capture_output=True,timeout=2)
  assert result.returncode,'serial negative passed intended overlap oracle'
 except subprocess.TimeoutExpired:pass
 print('serial root-walk negative rejected PASS')
