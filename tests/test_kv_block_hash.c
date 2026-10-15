/* Differential cached-block lookup, bounded memory and allocation fallback. */
#include <stdlib.h>
#include <assert.h>
#include <stdio.h>
#include <time.h>
static int refuse_hash;
static void *hash_calloc(size_t n,size_t size)
{ return refuse_hash ? NULL : calloc(n,size); }
#define calloc hash_calloc
#include "../src/kv/kv_seg.c"
#undef calloc

static void key_for(unsigned n,uint8_t key[12])
{
    memset(key,0,12);key[0]=1;key[2]=7;
    for(int i=11;i>=4;--i){key[i]=(uint8_t)n;n>>=8;}
}
static void check(const struct seg_cslot *c,const uint8_t *key,uint32_t kl)
{
    struct kv_buf a={0},b={0};uint8_t ao=99,bo=99;
    int ar=search_block(c,key,kl,&a,&ao),br=search_block_binary(c,key,kl,&b,&bo);
    assert(ar==br && ao==bo && a.len==b.len);
    assert(!a.len || !memcmp(a.p,b.p,a.len));kv_buf_free(&a);kv_buf_free(&b);
}
static uint64_t ns(void)
{struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000000000+t.tv_nsec;}
static void block_checks(int measure)
{
    uint8_t block[65536],key[12];uint32_t off=0;
    /* Deliberate bucket collisions among full keys, with shared prefixes. */
    unsigned ids[64],n=0;
    for(unsigned i=0;n<64;++i){key_for(i*2,key);if((block_key_hash(key,12)&127)==0)ids[n++]=i*2;}
    for(unsigned i=0;i<n;++i){
        key_for(ids[i],key);block[off]=i%7?KV_OP_PUT:KV_OP_DEL;
        put_u32(block+off+1,12);put_u32(block+off+5,i%7?4:0);
        memcpy(block+off+9,key,12);if(i%7)put_u32(block+off+21,ids[i]);
        off+=21+(i%7?4:0);
    }
    struct seg_cslot c={.blk=block};assert(!index_block(block,off,&c.entries,&c.nentries));
    refuse_hash=1;assert(!block_hash_build(&c,KV_BLOCK_HASH_BYTES_MAX) && !c.hash);refuse_hash=0;
    for(unsigned i=0;i<n;++i){key_for(ids[i],key);check(&c,key,12);}
    assert(!block_hash_build(&c,511) && !c.hash);
    assert(block_hash_build(&c,512)==512 && c.hash_slots==128);
    for(unsigned i=0;i<n;++i){key_for(ids[i],key);check(&c,key,12);key[11]^=1;check(&c,key,12);check(&c,key,11);}
    free(c.hash);free(c.entries);
    /* Mixed key lengths and embedded zero, including strict prefix keys. */
    const uint8_t keys[8][3]={{0},{0,0},{0,0,0},{0,0,1},{0,1},{1},{1,0},{255}};
    const unsigned lens[8]={1,2,3,3,2,1,2,1};off=0;
    for(unsigned i=0;i<8;++i){block[off]=i%2?KV_OP_PUT:KV_OP_DEL;put_u32(block+off+1,lens[i]);put_u32(block+off+5,0);memcpy(block+off+9,keys[i],lens[i]);off+=9+lens[i];}
    memset(&c,0,sizeof(c));c.blk=block;assert(!index_block(block,off,&c.entries,&c.nentries));assert(block_hash_build(&c,1024));
    for(unsigned i=0;i<8;++i)check(&c,keys[i],lens[i]);
    check(&c,(const uint8_t *)"missing",7);free(c.hash);free(c.entries);
    /* Word boundaries and long unaligned keys must hash every byte. */
    off=0;uint8_t mixed[256];
    for(unsigned i=0;i<32;++i){unsigned kl=i*7+1;memset(mixed,0,kl);mixed[0]=(uint8_t)(i+1);if(kl>1)mixed[kl-1]=255;
        block[off]=KV_OP_PUT;put_u32(block+off+1,kl);put_u32(block+off+5,0);memcpy(block+off+9,mixed,kl);off+=9+kl;}
    memset(&c,0,sizeof(c));c.blk=block;assert(!index_block(block,off,&c.entries,&c.nentries));assert(block_hash_build(&c,1024));
    for(unsigned i=0;i<32;++i){unsigned kl=i*7+1;memset(mixed,0,kl);mixed[0]=(uint8_t)(i+1);if(kl>1)mixed[kl-1]=255;check(&c,mixed,kl);mixed[kl-1]^=1;check(&c,mixed,kl);}
    free(c.hash);free(c.entries);
    /* Representative dense metadata block; identical warmed bytes in A/B. */
    off=0;
    for(unsigned i=0;i<256;++i){key_for(2*i,key);block[off]=KV_OP_PUT;put_u32(block+off+1,12);put_u32(block+off+5,8);memcpy(block+off+9,key,12);put_u64(block+off+21,i);off+=29;}
    memset(&c,0,sizeof(c));c.blk=block;assert(!index_block(block,off,&c.entries,&c.nentries));assert(block_hash_build(&c,KV_BLOCK_HASH_BYTES_MAX));
    for(unsigned i=0;i<512;++i){key_for(i,key);check(&c,key,12);}
    if(measure){
        for(unsigned round=0;round<6;++round)for(unsigned m=0;m<2;++m){
            unsigned mode=m^(round&1);struct kv_buf v={0};uint8_t op=0;uint64_t start=ns(),sum=0;
            for(unsigned i=0;i<1000000;++i){key_for((i*313u)&511,key);int rc=mode?search_block(&c,key,12,&v,&op):search_block_binary(&c,key,12,&v,&op);assert(rc==EFS_OK || rc==EFS_ERR_NOT_FOUND);sum+=(unsigned)(rc==EFS_OK);}
            printf("round=%u mode=%s ns/op=%.2f hits=%llu hash_bytes=%u\n",round,mode?"hash":"binary",(double)(ns()-start)/1000000,(unsigned long long)sum,c.hash_slots*4);kv_buf_free(&v);
        }
    }
    free(c.hash);c.hash=NULL;c.hash_slots=0;
    if(measure){uint64_t start=ns();for(unsigned i=0;i<10000;++i){assert(block_hash_build(&c,KV_BLOCK_HASH_BYTES_MAX));free(c.hash);c.hash=NULL;c.hash_slots=0;}
        printf("hash_build ns/block=%.2f entries=%u bytes=2048\n",(ns()-start)/10000.,c.nentries);}
    free(c.entries);
}
static void eviction_checks(void)
{
    char path[]="/tmp/efs-block-hash-XXXXXX";int fd=mkstemp(path);assert(fd>=0);close(fd);unlink(path);
    struct kv_seg_w *w;assert(!kv_seg_w_open(path,&w));uint8_t key[12],value[64]={0};
    for(unsigned i=0;i<40000;++i){key_for(i*2,key);assert(!kv_seg_w_add(w,KV_OP_PUT,key,12,value,64));}
    assert(!kv_seg_w_finish(w));struct kv_seg *s;assert(!kv_seg_open(path,&s));assert(s->nblocks>256);
    struct kv_buf v={0};uint8_t op;unsigned fallback=0,indexed=0;
    for(unsigned pass=0;pass<3;++pass)for(unsigned j=0;j<s->nblocks;++j){
        unsigned i=pass&1?s->nblocks-1-j:j;
        assert(!kv_seg_get(s,s->idx[i].key,s->idx[i].klen,&v,&op) && v.len==64 && op==KV_OP_PUT);
        struct seg_cslot *c=&s->cslot[i%KV_SEG_CACHE_SLOTS];check(c,s->idx[i].key,s->idx[i].klen);
        if(c->hash)indexed++;else fallback++;
        unsigned sum=0;for(unsigned k=0;k<KV_SEG_CACHE_SLOTS;++k)sum+=s->cslot[k].hash_slots*4;
        assert(sum==s->hash_bytes && sum<=KV_SEG_HASH_BYTES_MAX);
    }
    assert(indexed && fallback);
    /* Pinned immutable segment stays readable after owner retirement. */
    kv_seg_pin(s);kv_seg_close(s);key_for(0,key);assert(!kv_seg_get(s,key,12,&v,&op));kv_seg_unpin(s);
    kv_buf_free(&v);unlink(path);
}
int main(int argc,char **argv)
{
    block_checks(argc==2 && !strcmp(argv[1],"--measure"));eviction_checks();
    puts("block hash: collisions, variable keys, misses/tombstones, refusal, budget, eviction and pinned lifetime PASS");
}
