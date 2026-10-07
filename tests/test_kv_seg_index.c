/* Cached sorted-record index: ordering, tombstones, eviction and corruption. */
#include "../src/kv/kv_lsm_internal.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
static uint32_t u32(const uint8_t *p)
{ return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24; }
static void key_for(unsigned i,uint8_t key[4])
{ key[0]=1;key[1]=(uint8_t)(i>>16);key[2]=(uint8_t)(i>>8);key[3]=(uint8_t)i; }
int main(void)
{
    char path[]="/tmp/efs-seg-index-XXXXXX";int fd=mkstemp(path);assert(fd>=0);close(fd);unlink(path);
    struct kv_seg_w *w;assert(kv_seg_w_open(path,&w)==EFS_OK);
    uint8_t value[4096],key[4];
    for(unsigned i=0;i<700;++i) {
        key_for(i*2,key);memset(value,(int)(i%251),sizeof(value));
        assert(kv_seg_w_add(w,i%13?KV_OP_PUT:KV_OP_DEL,key,sizeof(key),value,i%17?sizeof(value):0)==EFS_OK);
    }
    assert(kv_seg_w_finish(w)==EFS_OK);
    struct kv_seg *s;assert(kv_seg_open(path,&s)==EFS_OK);struct kv_buf out={0};uint8_t op;
    /* Reverse/random order spans >256 cache slots. Revisit tombstones and holes. */
    for(unsigned pass=0;pass<3;++pass)for(unsigned j=0;j<700;++j) {
        unsigned i=pass==0?699-j:(j*311)%700;key_for(i*2,key);
        assert(kv_seg_get(s,key,sizeof(key),&out,&op)==EFS_OK);
        assert(op==(i%13?KV_OP_PUT:KV_OP_DEL));
        assert(out.len==(i%13 && i%17?sizeof(value):0));
        for(unsigned b=0;b<out.len;++b)assert(out.p[b]==i%251);
        key_for(i*2+1,key);assert(kv_seg_get(s,key,sizeof(key),&out,&op)==EFS_ERR_NOT_FOUND);
    }
    /* Dense blocks exercise many variable-length records in the same index. */
    kv_seg_close(s);unlink(path);assert(kv_seg_w_open(path,&w)==EFS_OK);
    for(unsigned i=0;i<700;++i) {
        key_for(i*2,key);memset(value,(int)(i%251),16);
        assert(kv_seg_w_add(w,i%13?KV_OP_PUT:KV_OP_DEL,key,sizeof(key),value,i%17?16:0)==EFS_OK);
    }
    assert(kv_seg_w_finish(w)==EFS_OK);assert(kv_seg_open(path,&s)==EFS_OK);
    struct kv_seg_io boundary_io;int boundary_need;key_for(0,key);
    assert(kv_seg_probe(s,key,sizeof(key),&out,&op,&boundary_need,&boundary_io)==EFS_OK && boundary_need);
    uint8_t *bad;assert(kv_seg_read(&boundary_io,&bad)==EFS_OK);
    uint32_t at=0,last=0;
    while(at<boundary_io.len){last=at;at+=9+u32(bad+at+1)+u32(bad+at+5);}
    bad[last+9]=255; /* sorted locally, but crosses the next block's first key */
    assert(kv_seg_install(s,&boundary_io,bad,key,sizeof(key),&out,&op)==EFS_ERR_PROTO);
    for(unsigned j=0;j<700;++j) {
        unsigned i=(j*311)%700;key_for(i*2,key);
        assert(kv_seg_get(s,key,sizeof(key),&out,&op)==EFS_OK);
        assert(op==(i%13?KV_OP_PUT:KV_OP_DEL) && out.len==(i%13 && i%17?16:0));
        for(unsigned b=0;b<out.len;++b)assert(out.p[b]==i%251);
        key_for(i*2+1,key);assert(kv_seg_get(s,key,sizeof(key),&out,&op)==EFS_ERR_NOT_FOUND);
    }
    /* Variable-length keys including prefix relationships within one block. */
    kv_seg_close(s);unlink(path);assert(kv_seg_w_open(path,&w)==EFS_OK);
    const uint8_t *keys[]={(const uint8_t *)"a",(const uint8_t *)"aa",(const uint8_t *)"ab",(const uint8_t *)"b"};
    for(unsigned i=0;i<4;++i)assert(kv_seg_w_add(w,KV_OP_PUT,keys[i],i==1||i==2?2:1,(const uint8_t *)"value",5)==EFS_OK);
    assert(kv_seg_w_finish(w)==EFS_OK);assert(kv_seg_open(path,&s)==EFS_OK);
    struct kv_seg_io io;int need;
    assert(kv_seg_probe(s,keys[0],1,&out,&op,&need,&io)==EFS_OK && need);
    uint8_t *blk;assert(kv_seg_read(&io,&blk)==EFS_OK);
    uint32_t second=9+u32(blk+1)+u32(blk+5);
    /* Even a hit in the first record must not hide malformed trailing data. */
    memset(blk+second+5,255,4);
    assert(kv_seg_install(s,&io,blk,keys[0],1,&out,&op)==EFS_ERR_PROTO);
    assert(kv_seg_read(&io,&blk)==EFS_OK);blk[second]=99;
    assert(kv_seg_install(s,&io,blk,keys[0],1,&out,&op)==EFS_ERR_PROTO);
    assert(kv_seg_read(&io,&blk)==EFS_OK);blk[second+9]='A';
    assert(kv_seg_install(s,&io,blk,keys[0],1,&out,&op)==EFS_ERR_PROTO);
    for(unsigned i=0;i<4;++i)assert(kv_seg_get(s,keys[i],i==1||i==2?2:1,&out,&op)==EFS_OK && out.len==5);
    assert(kv_seg_get(s,(const uint8_t *)"aaa",3,&out,&op)==EFS_ERR_NOT_FOUND);
    kv_buf_free(&out);kv_seg_close(s);unlink(path);
    puts("segment cached binary lookup, eviction, tombstones and malformed blocks PASS");
}
