/* A KVLSM2 MANIFEST carries reader requirements this build does not
 * implement: it must refuse it rather than misread it, keep writing plain
 * KVLSM1, and refuse unknown or malformed headers too. */
#include "efs/kv.h"
#include "efs/kv_key.h"
#include "efs/kv_lsm.h"
#include <assert.h>
#include <ftw.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static void header(const char *dir,char out[128])
{
    char path[8192];snprintf(path,sizeof(path),"%s/MANIFEST",dir);
    FILE *f=fopen(path,"r");assert(f && fgets(out,128,f));assert(!fclose(f));
}
static void replace_header(const char *dir,const char *first)
{
    /* Keep the segment list, swap only the version line. */
    char path[8192],body[65536];snprintf(path,sizeof(path),"%s/MANIFEST",dir);
    FILE *f=fopen(path,"r");assert(f);
    char line[128];assert(fgets(line,sizeof(line),f));
    size_t n=fread(body,1,sizeof(body),f);assert(!fclose(f));
    f=fopen(path,"w");assert(f && fputs(first,f)>=0 && fwrite(body,1,n,f)==n);assert(!fclose(f));
}
static int cleanup(const char *path,const struct stat *st,int kind,struct FTW *f)
{(void)st;(void)f;return kind==FTW_DP?rmdir(path):unlink(path);}
int main(void)
{
    char dir[]="/tmp/efs-kv-reader-gate-XXXXXX",line[128];assert(mkdtemp(dir));
    struct efs_kv *kv=efs_kv_lsm_open(dir,NULL);assert(kv);
    const uint8_t key[]={0,1,250},value[]={41};
    assert(!efs_kv_put(kv,key,sizeof(key),value,sizeof(value)));assert(!efs_kv_lsm_flush(kv));
    efs_kv_lsm_close(kv);
    header(dir,line);unsigned long long seq;
    assert(sscanf(line,"KVLSM1 %llu",&seq)==1);
    char saved[128];snprintf(saved,sizeof(saved),"KVLSM1 %llu\n",seq);
    /* KVLSM2 with a requirement and every other version/requirement are
     * refused, never opened as an empty or partial store. */
    char required[128];snprintf(required,sizeof(required),"KVLSM2 %llu 1\n",seq);
    const char *bad[]={required,"KVLSM3 1 1\n","KVLSM2 1 2\n","KVLSM2 1 0\n",
        "KVLSM2 1 4294967297\n","KVLSM2 1 1 extra\n","KVLSM2 1 -1\n"};
    for(unsigned i=0;i<sizeof(bad)/sizeof(bad[0]);i++) {
        replace_header(dir,bad[i]);assert(!efs_kv_lsm_open(dir,NULL));
    }
    /* Restoring the plain header recovers the same data: refusal is read-only. */
    replace_header(dir,saved);
    kv=efs_kv_lsm_open(dir,NULL);assert(kv);
    uint8_t got[1];uint32_t bytes=sizeof(got);
    assert(!efs_kv_get(kv,key,sizeof(key),got,&bytes) && got[0]==41);
    assert(!efs_kv_put(kv,key,sizeof(key),value,sizeof(value)));assert(!efs_kv_lsm_flush(kv));
    efs_kv_lsm_close(kv);
    header(dir,line);assert(sscanf(line,"KVLSM1 %llu",&seq)==1);
    assert(!nftw(dir,cleanup,32,FTW_DEPTH|FTW_PHYS));
    puts("KV reader gate: KVLSM2 and malformed MANIFESTs refused, plain store reopens and stays KVLSM1 PASS");
    return 0;
}
