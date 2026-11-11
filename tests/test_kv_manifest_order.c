/* A concurrent L0 fold can allocate its output id after a newer flush.
 * File ids are allocation identities; MANIFEST order is logical recency.
 * Reopen must preserve replacement and tombstone precedence for every key,
 * No cluster, timing assumption or old format. Ported from W123 4417a57f7. */
#include "efs/kv_lsm.h"
#include "../src/kv/kv_lsm_internal.h"
#include "test_tmpdir.h"
#include <assert.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const uint8_t keys[3][4]={{0,2,EFS_KV_KIND_INODE,0},
    {0,2,EFS_KV_KIND_INODE,1},{0,2,EFS_KV_KIND_INODE,2}};
static const uint32_t lens[3]={3,4,4};
static void segment(const char *dir,unsigned id,int recent)
{
    char path[EFS_TEST_PATH+256];snprintf(path,sizeof(path),"%s/seg-0-%010u.sst",dir,id);
    struct kv_seg_w *w=NULL;assert(!kv_seg_w_open(path,&w));
    for(unsigned i=0;i<3;i++){
        uint8_t value=i?42:recent?111:107;
        uint8_t op=i==1&&recent?KV_OP_DEL:i==2&&!recent?KV_OP_DEL:KV_OP_PUT;
        assert(!kv_seg_w_add(w,op,keys[i],lens[i],&value,op==KV_OP_DEL?0:1));
    }
    assert(!kv_seg_w_finish(w));
}
static int scan(void *arg,const uint8_t *k,uint32_t kn,const uint8_t *v,uint32_t vn)
{
    unsigned *count=arg;assert(*count<2);unsigned i=*count?2:0;
    assert(kn==lens[i]&&!memcmp(k,keys[i],kn)&&vn==1&&v[0]==(i?42:111));
    (*count)++;return 0;
}
int main(void)
{
    char dir[EFS_TEST_PATH];efs_test_path(dir,sizeof(dir),"efs-manifest-order-XXXXXX");
    assert(mkdtemp(dir));segment(dir,30,0);segment(dir,20,1);
    char path[EFS_TEST_PATH+256];snprintf(path,sizeof(path),"%s/MANIFEST",dir);
    FILE *f=fopen(path,"w");assert(f);
    assert(fprintf(f,"KVLSM1 31\n0 20\n0 30\n")>0);assert(!fclose(f));
    struct efs_kv_lsm_cfg cfg={.memtable_max=1<<20,.l0_max=64};
    for(unsigned pass=0;pass<3;pass++) {
        struct efs_kv *kv=efs_kv_lsm_open(dir,&cfg);assert(kv);
        for(unsigned i=0;i<3;i++) {
            uint8_t value=0;uint32_t n=1;int rc=efs_kv_get(kv,keys[i],lens[i],&value,&n);
            if(i==1)assert(rc==EFS_ERR_NOT_FOUND);
            else assert(!rc&&n==1&&value==(i?42:111));
        }
        unsigned count=0;assert(!efs_kv_scan(kv,scan,&count)&&count==2);
        assert(!efs_kv_lsm_compact(kv));
        efs_kv_lsm_close(kv);
    }
    DIR *d=opendir(dir);assert(d);struct dirent *e;
    while((e=readdir(d)))if(strcmp(e->d_name,".")&&strcmp(e->d_name,"..")){
        snprintf(path,sizeof(path),"%s/%s",dir,e->d_name);assert(!unlink(path));
    }
    closedir(d);assert(!rmdir(dir));
    puts("KV MANIFEST order PASS: cold read, replacement, tombstone and compact/reopen");
}
