#!/usr/bin/env python3
"""Bounded independent fences join before metadata retirement; failures persist."""
import os,shlex,subprocess,tempfile
from pathlib import Path
root=Path(__file__).resolve().parents[1]
s=Path(os.environ.get('EFS_RAFT_HOST_SOURCE',root/'src/server/raft_host.c')).read_text()
a=s.index('struct host_gc_fence_batch {');b=s.index('static void host_gc_reap_pass(',a)
code=r'''
#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#define EFS_OK 0
#define EFS_ERR_BUSY -13
#define REAP_SCAN_MAX 32
struct efs_raft_host {int gc_running,running;};
struct reap_scan_ctx {int n;uint64_t ino[32],gen[32];};
static pthread_mutex_t mu=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv=PTHREAD_COND_INITIALIZER;
static int permits,created,active,peak,arrived,participants,released,calls[32];
static int create_worker(pthread_t *t,const pthread_attr_t *a,void *(*fn)(void *),void *v){
 if(created>=permits)return EAGAIN;created++;return pthread_create(t,a,fn,v);
}
static int host_gc_inode_nodes(struct efs_raft_host *h,uint64_t ino,uint64_t gen){
 assert(h->running && h->gc_running && ino<32 && gen==100+ino);
 pthread_mutex_lock(&mu);calls[ino]++;active++;if(active>peak)peak=active;
 if(!released){arrived++;if(arrived==participants){released=1;pthread_cond_broadcast(&cv);}
 while(!released)pthread_cond_wait(&cv,&mu);}
 active--;pthread_mutex_unlock(&mu);
 return ino==5?EFS_ERR_BUSY:ino==9?-2:EFS_OK;
}
#define pthread_create create_worker
'''+s[a:b]+r'''
#undef pthread_create
int main(void){
 struct efs_raft_host h={1,1};struct reap_scan_ctx scan={.n=32};
 unsigned char ready[32];int result[32];
 for(int i=0;i<32;i++){scan.ino[i]=i;scan.gen[i]=100+i;ready[i]=1;}
 ready[3]=0;
 for(int limit=0;limit<=3;limit++){
  permits=limit;created=active=peak=arrived=released=0;participants=limit+1;memset(calls,0,sizeof(calls));
  host_gc_container_fences(&h,&scan,ready,result);
  assert(!active && created==limit && peak==limit+1);
  for(int i=0;i<32;i++){
   assert(calls[i]==(i!=3));assert(result[i]==(i==3||i==5?EFS_ERR_BUSY:i==9?-2:EFS_OK));
  }
 }
 memset(ready,0,sizeof(ready));created=0;host_gc_container_fences(&h,&scan,ready,result);assert(!created);
 memset(ready,1,sizeof(ready));h.gc_running=0;host_gc_container_fences(&h,&scan,ready,result);
 for(int i=0;i<32;i++)assert(result[i]==EFS_ERR_BUSY);assert(!created);
 h.gc_running=1;h.running=0;host_gc_container_fences(&h,&scan,ready,result);assert(!created);
 puts("container fences: four bounded participants, join, per-inode generation/results, partial/zero creation, stop/empty PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='efs-gc-fence-') as d:
 p=Path(d)/'test.c';p.write_text(code)
 subprocess.run(['cc','-std=c99','-Wall','-Wextra','-Werror',*shlex.split(os.environ.get('EFS_TEST_CFLAGS','')),'-pthread',str(p),'-o',str(p.with_suffix(''))],check=True)
 subprocess.run([str(p.with_suffix(''))],check=True,timeout=15)
# Actual integration retains serial files/repair and all-node success before REAP.
a=s.index('static void host_gc_reap_pass(');body=s[a:s.index('\n}',a)]
assert 'h->s->storage_engine && !h->s->repair_experiment' in body
assert body.index('host_gc_container_fences(')<body.rindex('host_gc_reap_finish(')
assert 'if(ready[i] && results[i]==EFS_OK)' in body
assert 'if (frc != EFS_OK)\n            continue;' in body
