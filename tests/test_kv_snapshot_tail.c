/* Whole-source query tail filtering: fixed scratch, no shared-cache promotion. */
#include <stdlib.h>
#include <assert.h>
#include <stdio.h>
static unsigned allocations;
static void *count_malloc(size_t size){allocations++;return malloc(size);}
#define malloc count_malloc
#include "../src/kv/kv_seg.c"
#include "test_tmpdir.h"
#undef malloc
static void forget_tail(struct kv_seg *s){free(s->last_key);s->last_key=NULL;s->last_klen=0;}
int main(void)
{
    char path[EFS_TEST_PATH];efs_test_path(path,sizeof(path),"efs-query-tail-XXXXXX");int fd=efs_test_mkstemp(path);assert(fd>=0);close(fd);unlink(path);
    struct kv_seg_w *w;struct kv_seg *s;int excluded;
    assert(!kv_seg_w_open(path,&w));
    for(unsigned i=0;i<3;i++){uint8_t key=(uint8_t)('a'+i);assert(!kv_seg_w_add(w,KV_OP_PUT,&key,1,(const uint8_t *)"v",1));}
    assert(!kv_seg_w_finish(w) && !kv_seg_open(path,&s));forget_tail(s);
    allocations=0;
    assert(!kv_seg_snapshot_excludes(s,(const uint8_t *)"b",1,NULL,0,&excluded) && !excluded);
    assert(!kv_seg_snapshot_excludes(s,(const uint8_t *)"z",1,NULL,0,&excluded) && excluded);
    assert(!allocations && !s->last_key);
    /* Corrupt only this private fixture: checked subtraction must reject a
     * length that would wrap the old tail-parser addition. */
    fd=open(path,O_WRONLY);assert(fd>=0);uint8_t bad[4]={255,255,255,255};
    assert(pwrite(fd,bad,4,(off_t)s->idx[s->nblocks-1].off+1)==4);close(fd);
    assert(kv_seg_snapshot_excludes(s,(const uint8_t *)"b",1,NULL,0,&excluded)==EFS_ERR_PROTO);
    assert(!allocations && !s->last_key);kv_seg_close(s);unlink(path);
    uint32_t size=KV_LSM_SNAPSHOT_BLOCK_MAX+1;uint8_t *value=malloc(size);assert(value);memset(value,0x5a,size);
    assert(!kv_seg_w_open(path,&w));assert(!kv_seg_w_add(w,KV_OP_PUT,(const uint8_t *)"a",1,value,size));
    assert(!kv_seg_w_finish(w) && !kv_seg_open(path,&s));forget_tail(s);allocations=0;
    assert(kv_seg_snapshot_excludes(s,(const uint8_t *)"z",1,NULL,0,&excluded)==EFS_ERR_BUSY);
    assert(!allocations && !s->last_key);
    struct kv_seg_iter *it;assert(!kv_seg_iter_open(s,&it));kv_seg_iter_set_private(it,1);kv_seg_iter_set_seq(it,1);
    assert(!kv_seg_iter_seek(it,(const uint8_t *)"a",1) && !it->blk && !it->ra);
    const uint8_t *key,*val;uint32_t kl,vl;uint8_t op;
    assert(kv_seg_iter_next(it,&key,&kl,&val,&vl,&op)==EFS_ERR_BUSY && !it->blk && !it->ra);
    kv_seg_iter_close(it);kv_seg_close(s);unlink(path);free(value);
    puts("query source tails: exact/missing, fixed scratch, no promotion, overflow and oversized refusal PASS");return 0;
}
