#!/usr/bin/env python3
"""Production WAL: failed writes/syncs cannot ACK later bytes behind damage."""
from pathlib import Path
import os, shlex, subprocess, tempfile
root=Path(__file__).resolve().parents[1]
source=r'''
#include "kv_lsm_internal.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
static int write_fail, sync_fail, calls;
static pthread_mutex_t sync_gate=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t sync_cv=PTHREAD_COND_INITIALIZER;
static int sync_pause,sync_entered;
static ssize_t fault_pwrite(int fd,const void *p,size_t n,off_t off) {
    if (write_fail && ++calls == 1) return pwrite(fd,p,n>3?3:n,off);
    if (write_fail) {errno=ENOSPC;return -1;}
    return pwrite(fd,p,n,off);
}
static int fault_sync(int fd) {
 pthread_mutex_lock(&sync_gate);sync_entered++;pthread_cond_broadcast(&sync_cv);
 while(sync_pause)pthread_cond_wait(&sync_cv,&sync_gate);
 pthread_mutex_unlock(&sync_gate);
 if(sync_fail){errno=EIO;return -1;}return fdatasync(fd);
}
#ifdef __linux__
static int allocation_fail, allocation_calls;
static int fault_fallocate(int fd,int mode,off_t start,off_t len) {
    allocation_calls++;
    if(allocation_fail){errno=allocation_fail;return -1;}
    return fallocate(fd,mode,start,len);
}
#define fallocate fault_fallocate
#endif
#define pwrite fault_pwrite
#define fdatasync fault_sync
#include "kv_wal.c"
#undef pwrite
#ifdef __linux__
#undef fallocate
#endif
#undef fdatasync
static unsigned mask;
static int count_record(void *u,uint8_t op,const uint8_t *k,uint32_t kl,const uint8_t *v,uint32_t vl) {
    (void)op;(void)k;(void)kl;(void)v;(void)vl;(*(unsigned*)u)++;return EFS_OK;
}
static void reservation(void) {
    char path[]="/tmp/efs-wal-reserve-XXXXXX";int fd=mkstemp(path);assert(fd>=0);close(fd);
    struct kv_wal *w;uint64_t seq;char val[1024];memset(val,0x5a,sizeof(val));
    struct efs_kv_item item={EFS_KV_PUT,(const uint8_t*)"X",1,(const uint8_t*)val,sizeof(val)};
    assert(kv_wal_open(path,EFS_KV_LSM_SYNC,&w)==EFS_OK);assert(kv_wal_hold(w,1)==EFS_OK);
    for(unsigned i=0;i<1500;i++) {
        assert(kv_wal_append(w,&item,1,&seq)==EFS_OK);
        assert(w->allocated_bytes >= w->bytes && w->allocated_bytes-w->bytes < 1048576);
    }
    assert(kv_wal_hold(w,0)==EFS_OK);kv_wal_close(w);
    unsigned count=0;assert(kv_wal_replay(path,count_record,&count)==EFS_OK && count==1500);
    struct stat st;assert(!stat(path,&st));uint64_t valid=st.st_size;
    assert(kv_wal_open(path,EFS_KV_LSM_SYNC,&w)==EFS_OK && w->bytes==valid);
    assert(kv_wal_append(w,&item,1,&seq)==EFS_OK);assert(kv_wal_reset(w,seq-1)==EFS_OK && w->bytes>valid);
    assert(kv_wal_reset(w,seq)==EFS_OK && w->bytes==0 && w->allocated_bytes==0);
    assert(kv_wal_append(w,&item,1,&seq)==EFS_OK);kv_wal_close(w);count=0;
    assert(kv_wal_replay(path,count_record,&count)==EFS_OK && count==1);unlink(path);
}

static int record(void *u,uint8_t op,const uint8_t *k,uint32_t kl,const uint8_t *v,uint32_t vl) {
    (void)u;(void)op;(void)v;(void)vl;assert(kl==1);mask|=1u<<(*k-'A');return EFS_OK;
}
static void run(int sync_error) {
    char path[]="/tmp/efs-wal-fault-XXXXXX";int fd=mkstemp(path);assert(fd>=0);close(fd);
    struct kv_wal *w;uint64_t seq;
    struct efs_kv_item item={EFS_KV_PUT,(const uint8_t*)"A",1,(const uint8_t*)"a",1};
    assert(kv_wal_open(path,EFS_KV_LSM_SYNC,&w)==EFS_OK);
    assert(kv_wal_append(w,&item,1,&seq)==EFS_OK);
    write_fail=!sync_error;sync_fail=sync_error;calls=0;item.key=(const uint8_t*)"B";
    assert(kv_wal_append(w,&item,1,&seq)==EFS_ERR_IO);
    write_fail=sync_fail=0;item.key=(const uint8_t*)"C";
    assert(kv_wal_append(w,&item,1,&seq)==EFS_ERR_IO && seq==0);
    assert(kv_wal_hold(w,1)==EFS_ERR_IO);
    assert(kv_wal_reset(w,UINT64_MAX)==EFS_ERR_IO);
    kv_wal_close(w);mask=0;
    assert(kv_wal_replay(path,record,NULL)==EFS_OK);
    assert(mask&1);assert(!(mask&4)); /* B's failed sync may or may not persist. */
    assert(kv_wal_open(path,EFS_KV_LSM_SYNC,&w)==EFS_OK);
    assert(kv_wal_append(w,&item,1,&seq)==EFS_OK);kv_wal_close(w);mask=0;
    assert(kv_wal_replay(path,record,NULL)==EFS_OK && (mask&5)==5);
    unlink(path);
}
static void allocation_fallback(void) {
#ifdef __linux__
    int errors[]={EOPNOTSUPP,ENOSPC};
    for(unsigned i=0;i<sizeof(errors)/sizeof(errors[0]);i++) {
        char path[]="/tmp/efs-wal-fallback-XXXXXX";int fd=mkstemp(path);assert(fd>=0);close(fd);
        struct kv_wal *w;uint64_t seq;
        struct efs_kv_item item={EFS_KV_PUT,(const uint8_t*)"A",1,(const uint8_t*)"a",1};
        assert(kv_wal_open(path,EFS_KV_LSM_SYNC,&w)==EFS_OK);
        allocation_fail=errors[i];allocation_calls=0;
        assert(kv_wal_append(w,&item,1,&seq)==EFS_OK);
        assert(w->prealloc_off && w->bytes==w->allocated_bytes && allocation_calls==1);
        assert(kv_wal_append(w,&item,1,&seq)==EFS_OK && allocation_calls==1);
        allocation_fail=0;kv_wal_close(w);unsigned count=0;
        assert(kv_wal_replay(path,count_record,&count)==EFS_OK && count==2);unlink(path);
    }
#endif
}
static void malformed_batch(void) {
    char path[]="/tmp/efs-wal-malformed-XXXXXX";int fd=mkstemp(path);assert(fd>=0);
    uint8_t bytes[24]={0};
    put_u32(bytes+4,16);put_u32(bytes+8,2);
    bytes[12]=KV_OP_PUT;put_u32(bytes+13,1);put_u32(bytes+17,1);
    bytes[21]='A';bytes[22]='a';bytes[23]=KV_OP_PUT;
    put_u32(bytes,efs_crc32(bytes+8,16));
    assert(write(fd,bytes,sizeof(bytes))==sizeof(bytes));close(fd);
    mask=0;assert(kv_wal_replay(path,record,NULL)==EFS_ERR_PROTO && mask==0);
    fd=open(path,O_RDONLY);assert(fd>=0);assert(lseek(fd,0,SEEK_END)==24);close(fd);unlink(path);
}
static void interior_damage(void) {
    for(unsigned gap=0;gap<2;gap++) {
        char path[]="/tmp/efs-wal-interior-XXXXXX";int fd=mkstemp(path);assert(fd>=0);close(fd);
        struct kv_wal *w;uint64_t seq;
        struct efs_kv_item item={EFS_KV_PUT,(const uint8_t*)"A",1,(const uint8_t*)"a",1};
        assert(kv_wal_open(path,EFS_KV_LSM_SYNC,&w)==EFS_OK);
        assert(kv_wal_append(w,&item,1,&seq)==EFS_OK);uint64_t first=w->bytes;
        item.key=(const uint8_t*)"B";assert(kv_wal_append(w,&item,1,&seq)==EFS_OK);
        uint64_t end=w->bytes;kv_wal_close(w);
        fd=open(path,O_RDWR);assert(fd>=0);
        if(gap) {
            uint8_t b[64];assert(end-first<sizeof(b));assert(pread(fd,b,end-first,first)==(ssize_t)(end-first));
            uint8_t zero[8]={0};assert(pwrite(fd,zero,8,first)==8);
            assert(pwrite(fd,b,end-first,first+8)==(ssize_t)(end-first));
        } else {
            uint8_t bad;assert(pread(fd,&bad,1,0)==1);bad^=1;assert(pwrite(fd,&bad,1,0)==1);
        }
        assert(!fdatasync(fd));struct stat before,after;assert(!fstat(fd,&before));close(fd);
        mask=0;assert(kv_wal_replay(path,record,NULL)==EFS_ERR_PROTO);
        assert(!stat(path,&after) && before.st_size==after.st_size);
        fd=open(path,O_RDONLY);assert(fd>=0);uint8_t key;
        /* B's key is the penultimate byte of its complete frame. */
        assert(pread(fd,&key,1,(off_t)end+(gap?8:0)-2)==1 && key=='B');close(fd);unlink(path);
    }
}
struct prefix_job {struct kv_wal *w;uint64_t prefix;int rc;};
static void *flush_prefix(void *p){struct prefix_job *j=p;j->rc=kv_wal_sync_prefix(j->w,j->prefix);return NULL;}
static void detached_prefix(void) {
 char path[]="/tmp/efs-wal-prefix-XXXXXX";int fd=mkstemp(path);assert(fd>=0);close(fd);
 struct kv_wal *w;uint64_t seq,prefix;assert(!kv_wal_open(path,EFS_KV_LSM_SYNC,&w));
 struct efs_kv_item item={EFS_KV_PUT,(const uint8_t*)"P",1,(const uint8_t*)"V",1};
 assert(!kv_wal_hold(w,1));assert(!kv_wal_hold(w,1));
 assert(kv_wal_detach(w,&prefix)==EFS_ERR_BUSY && w->hold==2);
 assert(!kv_wal_hold(w,0));assert(!kv_wal_append(w,&item,1,&seq));
 assert(!kv_wal_detach(w,&prefix)&&prefix==1&&!w->hold&&!w->synced);
 int covered=7;assert(!kv_wal_sync_covered(w,prefix,&covered)&&!covered);
 pthread_mutex_lock(&sync_gate);sync_pause=1;sync_entered=0;pthread_mutex_unlock(&sync_gate);
 struct prefix_job job={w,prefix,0};pthread_t thread;assert(!pthread_create(&thread,NULL,flush_prefix,&job));
 pthread_mutex_lock(&sync_gate);while(!sync_entered)pthread_cond_wait(&sync_cv,&sync_gate);pthread_mutex_unlock(&sync_gate);
 /* New held records arrive during the older fsync. They are not its proof. */
 assert(!kv_wal_hold(w,1));assert(!kv_wal_append(w,&item,1,&seq)&&seq==2);
 pthread_mutex_lock(&sync_gate);sync_pause=0;pthread_cond_broadcast(&sync_cv);pthread_mutex_unlock(&sync_gate);
 pthread_join(thread,NULL);assert(!job.rc&&w->synced==1&&w->appended==2);
 assert(!kv_wal_detach(w,&prefix)&&prefix==2);assert(!kv_wal_sync_prefix(w,prefix)&&w->synced==2);
 assert(kv_wal_sync_prefix(w,3)==EFS_ERR_INVAL);
 /* Two detached cycles arriving before a barrier share its physical sync,
  * while the worker still publishes only each captured cursor separately. */
 assert(!kv_wal_hold(w,1));assert(!kv_wal_append(w,&item,1,&seq));uint64_t first_prefix;
 assert(!kv_wal_detach(w,&first_prefix));
 assert(!kv_wal_hold(w,1));assert(!kv_wal_append(w,&item,1,&seq));assert(!kv_wal_detach(w,&prefix));
 unsigned sync_before=(unsigned)sync_entered;
 assert(!kv_wal_sync_prefix(w,first_prefix)&&w->synced==prefix);
 assert(!kv_wal_sync_covered(w,prefix,&covered)&&covered);
 assert(kv_wal_sync_covered(w,prefix+1,&covered)==EFS_ERR_INVAL&&!covered);
 assert(!kv_wal_sync_prefix(w,prefix)&&(unsigned)sync_entered==sync_before+1);
 assert(!kv_wal_hold(w,1));assert(!kv_wal_append(w,&item,1,&seq));assert(!kv_wal_detach(w,&prefix));
 sync_fail=1;assert(kv_wal_sync_prefix(w,prefix)==EFS_ERR_IO && w->synced==4);sync_fail=0;
 assert(kv_wal_sync_prefix(w,prefix)==EFS_ERR_IO);
 assert(kv_wal_sync_covered(w,first_prefix,&covered)==EFS_ERR_IO&&!covered);
 kv_wal_close(w);unlink(path);
}
int main(void){run(0);run(1);malformed_batch();interior_damage();reservation();allocation_fallback();detached_prefix();puts("WAL sticky failures and reopen recovery PASS");}
'''
with tempfile.TemporaryDirectory(prefix='efs-wal-faults-') as directory:
    p=Path(directory)/'test.c';p.write_text(source)
    command=shlex.split(os.environ.get('CC','cc'))+['-std=gnu11','-D_GNU_SOURCE','-Wall','-Wextra','-Werror','-pthread','-DBLAKE3_USE_NEON=0','-DBLAKE3_NO_SSE2','-DBLAKE3_NO_SSE41','-DBLAKE3_NO_AVX2','-DBLAKE3_NO_AVX512','-I'+str(root/'include'),'-I'+str(root/'src/kv'),'-I'+str(root/'deps/blake3')]
    command+=shlex.split(os.environ.get('WAL_TEST_CFLAGS',''))
    command += [str(p),str(root/'src/common/checksum.c'),str(root/'deps/blake3/blake3.c'),str(root/'deps/blake3/blake3_dispatch.c'),str(root/'deps/blake3/blake3_portable.c'),'-o',str(p.with_suffix(''))]
    subprocess.run(command,check=True);subprocess.run([str(p.with_suffix(''))],check=True)
