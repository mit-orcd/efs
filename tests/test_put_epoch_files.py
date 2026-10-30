#!/usr/bin/env python3
"""Exercise the production cohort's descriptor ownership and bounded workers."""
from pathlib import Path
import os,subprocess,tempfile
root=Path(__file__).resolve().parents[1]
source=r'''
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
static int fail_threads;
#include <pthread.h>
static int create_thread(pthread_t *t,const pthread_attr_t *a,void *(*f)(void*),void *p) {
    return fail_threads ? 11 : pthread_create(t,a,f,p);
}
#define pthread_create create_thread
#include "src/server/put_epoch_files.h"
#undef pthread_create
static unsigned seen[128],active,peak;
static int fail_sync;
static int sync_file(const struct put_epoch_file *f) {
    unsigned i=(unsigned)(uintptr_t)f->owner;
    assert(__atomic_fetch_add(&seen[i],1,__ATOMIC_RELAXED)==0);
    unsigned n=__atomic_add_fetch(&active,1,__ATOMIC_RELAXED),old;
    do {old=__atomic_load_n(&peak,__ATOMIC_RELAXED);if(old>=n)break;}
    while(!__atomic_compare_exchange_n(&peak,&old,n,0,__ATOMIC_RELAXED,__ATOMIC_RELAXED));
    struct timespec delay={0,1000000};nanosleep(&delay,NULL);
    assert(!fsync(f->fd));__atomic_sub_fetch(&active,1,__ATOMIC_RELAXED);
    return fail_sync && i==17 ? EFS_ERR_IO : EFS_OK;
}
static int finished;
static void *wait_pin(void *p) {
    int fd=put_epoch_pin(*(int*)p);assert(fd>=0);
    __atomic_store_n(&finished,1,__ATOMIC_RELEASE);
    assert(!put_epoch_close(fd));return NULL;
}
int main(void) {
    char path[]="/tmp/efs-epoch-files-XXXXXX";int fd=mkstemp(path);assert(fd>=0);assert(!unlink(path));
    assert(write(fd,"payload",7)==7);
    for(int mode=0;mode<3;mode++) {
        struct put_epoch_file files[128];memset(seen,0,sizeof(seen));active=peak=0;
        fail_sync=mode==1;fail_threads=mode==2;
        for(unsigned i=0;i<128;i++){files[i].fd=put_epoch_pin(fd);assert(files[i].fd>=0);files[i].owner=(void*)(uintptr_t)i;}
        assert(put_epoch_owned==128);
        assert(put_epoch_sync(files,128,sync_file)==(fail_sync?EFS_ERR_IO:EFS_OK));
        assert(!put_epoch_owned && !active && peak<=8 && (fail_threads ? peak==1 : peak>1));
        for(unsigned i=0;i<128;i++){assert(seen[i]==1);errno=0;assert(fcntl(files[i].fd,F_GETFD)==-1 && errno==EBADF);}
    }
    assert(put_epoch_pin(-1)<0 && !put_epoch_owned);
    put_epoch_limit=1;int pin=put_epoch_pin(fd);assert(pin>=0);
    pthread_t t;assert(!pthread_create(&t,NULL,wait_pin,&fd));
    struct timespec delay={0,20000000};nanosleep(&delay,NULL);assert(!__atomic_load_n(&finished,__ATOMIC_ACQUIRE));
    assert(!put_epoch_close(pin));assert(!pthread_join(t,NULL));assert(finished && !put_epoch_owned);
    /* Even a close error releases ownership; never retry an ambiguous close. */
    pin=put_epoch_pin(fd);assert(pin>=0);assert(!close(pin));assert(put_epoch_close(pin)==EFS_ERR_IO && !put_epoch_owned);
    assert(!close(fd));puts("PUT cohorts: every file once, bounded workers and credits, blocking/release, sync failure, close failure and thread fallback PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='efs-epoch-unit-') as d:
    p=Path(d);(p/'test.c').write_text(source)
    subprocess.run([os.environ.get('CC','cc'),'-std=c11','-D_GNU_SOURCE','-I'+str(root),'-I'+str(root/'include'),'-pthread',str(p/'test.c'),'-o',str(p/'test')],check=True)
    subprocess.run([str(p/'test')],check=True,timeout=20)
