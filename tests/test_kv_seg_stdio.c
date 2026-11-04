/* Real SST FILE buffer capacity, exact readback and owned failure cleanup.
 * --old-buffer-control is an expected-failure resource control, not a format
 * compatibility test. It still writes/reads the same current SST records. */
#include "../src/kv/kv_lsm_internal.h"
#include "test_tmpdir.h"
#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#ifdef __GLIBC__
#include <stdio_ext.h>
#endif

static unsigned calls,bad_buffers,syncs;
static size_t capacity;
static int fail_buffer,fail_setvbuf;
void *__real_malloc(size_t);
int __real_setvbuf(FILE *,char *,int,size_t);
int __real_fsync(int);
void *__wrap_malloc(size_t n)
{if(fail_buffer&&n==(1u<<20)){fail_buffer=0;errno=ENOMEM;return NULL;}return __real_malloc(n);}
int __wrap_fsync(int fd){syncs++;return __real_fsync(fd);}
int __wrap_setvbuf(FILE *f,char *p,int mode,size_t n)
{
    calls++;
    if(fail_setvbuf){fail_setvbuf=0;return 1;}
    int rc=__real_setvbuf(f,p,mode,n);
    capacity=n;
#ifdef __GLIBC__
    capacity=__fbufsize(f);
#endif
    if(rc||!p||mode!=_IOFBF||n!=(1u<<20)||capacity!=(1u<<20))bad_buffers++;
    return rc;
}
static void key(unsigned i,uint8_t p[4])
{p[0]=1;p[1]=(uint8_t)(i>>16);p[2]=(uint8_t)(i>>8);p[3]=(uint8_t)i;}

int main(int argc,char **argv)
{
    int control=argc==2&&!strcmp(argv[1],"--old-buffer-control");
    char path[EFS_TEST_PATH],tmp[EFS_TEST_PATH+4];
    efs_test_path(path,sizeof(path),"efs-seg-stdio-XXXXXX");
    int fd=efs_test_mkstemp(path);assert(fd>=0);close(fd);unlink(path);
    snprintf(tmp,sizeof(tmp),"%s.tmp",path);
    struct kv_seg_w *w=NULL;assert(!kv_seg_w_open(path,&w));
    uint8_t k[4],value[4096];
    for(unsigned i=0;i<1024;i++){
        key(i,k);memset(value,(int)(i%251),sizeof(value));
        assert(!kv_seg_w_add(w,KV_OP_PUT,k,sizeof(k),value,sizeof(value)));
    }
    assert(!kv_seg_w_finish(w));assert(syncs==1);
    struct kv_seg *seg=NULL;assert(!kv_seg_open(path,&seg));struct kv_buf got={0};uint8_t op;
    for(unsigned i=0;i<1024;i++){
        key(i,k);assert(!kv_seg_get(seg,k,sizeof(k),&got,&op));
        assert(op==KV_OP_PUT&&got.len==sizeof(value));
        for(unsigned j=0;j<got.len;j++)assert(got.p[j]==i%251);
    }
    free(got.p);kv_seg_close(seg);unlink(path);
    assert(!kv_seg_w_open(path,&w));
    for(unsigned i=0;i<16;i++){key(i,k);assert(!kv_seg_w_add(w,KV_OP_PUT,k,4,value,sizeof(value)));}
    kv_seg_w_abort(w);assert(access(path,F_OK)&&access(tmp,F_OK)&&syncs==1);
    printf("SST stdio supplied-buffer checks=%u failures=%u actual_capacity=%zu\n",calls,bad_buffers,capacity);
    if(control)return bad_buffers?1:0;
    assert(!bad_buffers&&calls==2);
    fail_buffer=1;w=NULL;assert(kv_seg_w_open(path,&w)==EFS_ERR_NOMEM);
    assert(access(path,F_OK)&&access(tmp,F_OK)&&calls==2);
    fail_setvbuf=1;assert(kv_seg_w_open(path,&w)==EFS_ERR_IO);
    assert(access(path,F_OK)&&access(tmp,F_OK)&&calls==3&&syncs==1);
    puts("SST owned 1 MiB stdio buffer PASS: exact multi-buffer readback, unchanged fsync, abort/allocation/setup-failure cleanup");
    return 0;
}
