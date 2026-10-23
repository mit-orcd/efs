/* A KVLSM2 MANIFEST carries reader requirements. This build implements
 * only PACK_V2 (W125 packed rows): it refuses capability 1 (the archived
 * packing format), every other requirement and malformed headers rather
 * than misread them, keeps writing plain KVLSM1 until a requirement is
 * set, and keeps a set requirement across writes and reopen. */
#include "efs/kv.h"
#include "efs/kv_key.h"
#include "efs/kv_lsm.h"
#include <assert.h>
#include <ftw.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "test_tmpdir.h"
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
    char dir[EFS_TEST_PATH],line[128];efs_test_path(dir,sizeof(dir),"efs-kv-reader-gate-XXXXXX");assert(efs_test_mkdtemp(dir));
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
    char padded[128];snprintf(padded,sizeof(padded),"KVLSM2 %llu 02\n",seq);
    char three[128];snprintf(three,sizeof(three),"KVLSM2 %llu 3\n",seq);
    const char *bad[]={required,padded,three,"KVLSM3 1 1\n","KVLSM2 1 0\n",
        "KVLSM2 1 4294967298\n","KVLSM2 1 2 extra\n","KVLSM2 1 -1\n"};
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
    /* Requiring PACK_V2 is durable, idempotent and survives writes and
     * reopen; only PACK_V2 may be required. */
    kv=efs_kv_lsm_open(dir,NULL);assert(kv);
    assert(efs_kv_lsm_require_reader(kv,1)==EFS_ERR_INVAL);
    assert(efs_kv_lsm_require_reader(kv,3)==EFS_ERR_INVAL);
    assert(!efs_kv_lsm_require_reader(kv,EFS_KV_READER_PACK_V2));
    header(dir,line);unsigned req=0;
    assert(sscanf(line,"KVLSM2 %llu %u",&seq,&req)==2 && req==EFS_KV_READER_PACK_V2);
    assert(!efs_kv_lsm_require_reader(kv,EFS_KV_READER_PACK_V2));
    assert(!efs_kv_put(kv,key,sizeof(key),value,sizeof(value)));assert(!efs_kv_lsm_flush(kv));
    assert(!efs_kv_lsm_compact(kv));efs_kv_lsm_close(kv);
    header(dir,line);req=0;
    assert(sscanf(line,"KVLSM2 %llu %u",&seq,&req)==2 && req==EFS_KV_READER_PACK_V2);
    kv=efs_kv_lsm_open(dir,NULL);assert(kv);bytes=sizeof(got);
    assert(!efs_kv_get(kv,key,sizeof(key),got,&bytes) && got[0]==41);
    efs_kv_lsm_close(kv);
    header(dir,line);assert(sscanf(line,"KVLSM2 %llu %u",&seq,&req)==2);
    assert(!nftw(dir,cleanup,32,FTW_DEPTH|FTW_PHYS));
    puts("KV reader gate: unsupported KVLSM2 and malformed MANIFESTs refused, plain store stays KVLSM1, PACK_V2 requirement durable PASS");
    return 0;
}
