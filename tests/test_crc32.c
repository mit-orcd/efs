#include "efs/checksum.h"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Independent bit-at-a-time IEEE oracle, not the production table builder. */
static uint32_t reference(const uint8_t *p, size_t n)
{
    uint32_t c = UINT32_MAX;
    while (n--) {
        c ^= *p++;
        for (unsigned j=0;j<8;j++)
            c = (c >> 1) ^ ((c & 1) ? 0xedb88320u : 0);
    }
    return ~c;
}
struct job { const uint8_t *p; size_t n; uint32_t expected; };
static void *first_call(void *arg)
{
    const struct job *j=arg;
    assert(efs_crc32(j->p,j->n)==j->expected);
    return NULL;
}
static double now(void)
{
    struct timespec t; assert(!clock_gettime(CLOCK_MONOTONIC,&t));
    return t.tv_sec+t.tv_nsec/1e9;
}
/* Prior byte-at-a-time algorithm, used only for a component cost comparison. */
static uint32_t byte_table[256];
static uint32_t byte_reference(const uint8_t *p,size_t n)
{
    uint32_t c=UINT32_MAX;
    while(n--)c=byte_table[(c^*p++)&255]^(c>>8);
    return ~c;
}
int main(int argc,char **argv)
{
    const size_t cap=(4u<<20)+16;
    uint8_t *p=malloc(cap);assert(p);
    uint32_t state=0x413d9e21;
    for(size_t i=0;i<cap;i++) {
        state^=state<<13;state^=state>>17;state^=state<<5;p[i]=(uint8_t)state;
    }
    /* No production CRC call precedes the racing initialization. */
    struct job j={p+3,1u<<20,reference(p+3,1u<<20)};
    pthread_t threads[8];
    for(unsigned i=0;i<8;i++)assert(!pthread_create(threads+i,NULL,first_call,&j));
    for(unsigned i=0;i<8;i++)assert(!pthread_join(threads[i],NULL));
    assert(efs_crc32(NULL,0)==0);
    assert(efs_crc32("123456789",9)==0xcbf43926u);
    for(size_t off=0;off<16;off++) {
        for(size_t n=0;n<258;n++)assert(efs_crc32(p+off,n)==reference(p+off,n));
        const size_t sizes[]={511,512,4095,4096,65535,65536,1048576,cap-off};
        for(size_t i=0;i<sizeof(sizes)/sizeof(*sizes);i++)
            assert(efs_crc32(p+off,sizes[i])==reference(p+off,sizes[i]));
    }
    memset(p,0,cap);
    assert(efs_crc32(p,cap)==reference(p,cap));
    memset(p,255,cap);
    assert(efs_crc32(p+1,cap-1)==reference(p+1,cap-1));
    puts("CRC32 IEEE oracle, tails, alignment, large records and concurrent init PASS");
    if(argc==2&&!strcmp(argv[1],"--bench")) {
        for(unsigned i=0;i<256;i++) {
            uint32_t c=i;
            for(unsigned k=0;k<8;k++)c=(c>>1)^((c&1)?0xedb88320u:0);
            byte_table[i]=c;
        }
        volatile uint32_t sink=0;
        double t=now();
        for(unsigned i=0;i<128;i++)sink^=byte_reference(p+(i&15),1u<<20);
        double old=now()-t;t=now();
        for(unsigned i=0;i<128;i++)sink^=efs_crc32(p+(i&15),1u<<20);
        double current=now()-t;
        printf("component only: prior %.3f MiB/s, sliced %.3f MiB/s; sink=%u\n",
               128/old,128/current,(unsigned)sink);
    }
    free(p);return 0;
}
