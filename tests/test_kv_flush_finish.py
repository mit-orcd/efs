#!/usr/bin/env python3
"""Real LSM flush: bounded independent fsyncs, checked publication and cold replay.

Compiles the actual compactor with finish/thread-creation seams only. Every
successful finish uses the real segment writer. A failed finish consumes its
writer and must publish nothing; abrupt exit then exercises WAL recovery.
KV_FLUSH_SOURCE may select a serial control; it must fail the overlap check.
"""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = Path(os.environ.get('KV_FLUSH_SOURCE', root / 'src/kv/kv_compact.c'))
cc = shlex.split(os.environ.get('CC', 'cc'))
extra = ['-fsanitize=address,undefined', '-fno-omit-frame-pointer'] if os.environ.get('EFS_TEST_SANITIZE') == '1' else []
code = r'''
#include "kv_lsm_internal.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>
#include <time.h>
#define N 35
static pthread_mutex_t mu=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv=PTHREAD_COND_INITIALIZER;
static unsigned active,peak,calls,done,overlap,armed,fail_finish,fail_create,create_failed;
static char manifest[8192],manifest_path[4096];static ssize_t manifest_len;
static ssize_t manifest_read(char *buf) {
 int fd=open(manifest_path,O_RDONLY);if(fd<0){assert(errno==ENOENT);return -1;}ssize_t n=read(fd,buf,8192);assert(n>=0);assert(!close(fd));return n;
}
static void unchanged(void){char bytes[8192];ssize_t n=manifest_read(bytes);assert(n==manifest_len);if(n>=0)assert(!memcmp(bytes,manifest,(size_t)n));}
int controlled_create(pthread_t *thread,const pthread_attr_t *attr,void *(*fn)(void *),void *arg) {
 pthread_mutex_lock(&mu);int fail=armed&&fail_create&&!create_failed;
 if(fail){create_failed++;}
 pthread_mutex_unlock(&mu);
 return fail?EAGAIN:pthread_create(thread,attr,fn,arg);
}
int controlled_finish(struct kv_seg_w *writer) {
 pthread_mutex_lock(&mu);unsigned number=++calls;active++;if(active>peak)peak=active;
 if(active>1){overlap=1;}
 pthread_cond_broadcast(&cv);
 if(!overlap) {
  struct timespec end;clock_gettime(CLOCK_REALTIME,&end);end.tv_nsec+=50000000;
  if(end.tv_nsec>=1000000000){end.tv_sec++;end.tv_nsec-=1000000000;}
  while(!overlap){int rc=pthread_cond_timedwait(&cv,&mu,&end);if(rc==ETIMEDOUT)break;assert(!rc);}
 }
 pthread_mutex_unlock(&mu);
 unchanged();
 int rc;
 if(fail_finish&&number==5){kv_seg_w_abort(writer);rc=EFS_ERR_IO;}
 else rc=kv_seg_w_finish(writer);
 unchanged(); /* No live-set publication while even this job remains owned. */
 pthread_mutex_lock(&mu);active--;done++;pthread_cond_broadcast(&cv);pthread_mutex_unlock(&mu);
 return rc;
}
static void key(unsigned i,uint8_t out[4]) {
 if(i<17){out[0]=0;out[1]=(uint8_t)i;}else{out[0]=(uint8_t)(i-16);out[1]=0;}
 out[2]=33;out[3]=(uint8_t)i;
}
static void verify(struct efs_kv *kv,unsigned count) {
 for(unsigned i=0;i<count;i++){uint8_t k[4];key(i,k);uint64_t got=0;uint32_t n=8;
  assert(!efs_kv_get(kv,k,4,(uint8_t*)&got,&n)&&n==8&&got==101+17*i);}
}
int main(int argc,char **argv) {
 assert(argc==3);struct rlimit limit={0,0};assert(!setrlimit(RLIMIT_CORE,&limit));
 unsigned count=strstr(argv[1],"single")?1:N;
 struct efs_kv_lsm_cfg cfg={.sync_mode=EFS_KV_LSM_SYNC,.memtable_max=32u<<20,.l0_max=512};
 struct efs_kv *kv=efs_kv_lsm_open(argv[2],&cfg);assert(kv);
 if(!strncmp(argv[1],"read",4)){verify(kv,count);efs_kv_lsm_close(kv);return 0;}
 uint8_t keys[N][4];uint64_t values[N];struct efs_kv_item items[N];
 for(unsigned i=0;i<count;i++){key(i,keys[i]);values[i]=101+17*i;items[i]=(struct efs_kv_item){EFS_KV_PUT,keys[i],4,(uint8_t*)&values[i],8};}
 assert(!efs_kv_batch(kv,items,count));
 snprintf(manifest_path,sizeof(manifest_path),"%s/MANIFEST",argv[2]);manifest_len=manifest_read(manifest);
 fail_finish=!strcmp(argv[1],"write-fail");fail_create=!strcmp(argv[1],"write-create-fail");armed=1;
 int rc=efs_kv_lsm_flush(kv);
 assert(!active&&done==calls&&peak<=16);
 if(fail_finish){assert(rc==EFS_ERR_IO&&calls>=5&&calls<=16);unchanged();}
 else {assert(!rc&&calls==count);assert(count==1?peak==1:peak>=2);}
 if(fail_create)assert(create_failed==1);
 verify(kv,count);
 printf("mode=%s calls=%u done=%u peak=%u create_failed=%u rc=%d\n",argv[1],calls,done,peak,create_failed,rc);fflush(stdout);
 _exit(0); /* cold-open must recover a failed flush's untruncated WAL too */
}
'''
with tempfile.TemporaryDirectory(prefix='efs-kv-finish-') as td:
    d = Path(td)
    c = d / 'test.c'; c.write_text(code)
    obj, binary = d / 'compact.o', d / 'test'
    flags = ['-std=c99', '-D_GNU_SOURCE', '-Wall', '-Wextra', '-Werror', '-I'+str(root/'include'), '-I'+str(root/'src/kv'), '-pthread', *extra]
    subprocess.run(cc + flags + ['-Dkv_seg_w_finish=controlled_finish', '-Dpthread_create=controlled_create', '-c', str(source), '-o', str(obj)], check=True)
    subprocess.run(cc + flags + [str(c), str(obj), str(root/'libefs.a'), '-lm', '-ldl', '-o', str(binary)], check=True)
    for mode in ('good', 'fail', 'create-fail', 'single'):
        store = d / mode
        subprocess.run([str(binary), 'write-'+mode, str(store)], check=True, timeout=30)
        subprocess.run([str(binary), 'read-single' if mode=='single' else 'read', str(store)], check=True, timeout=30)
print('KV flush: bounded overlapping checked finishes, all jobs drained, no failed publication, resource fallback and exact cold replay PASS')
