#include "efs/query_policy.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include "test_tmpdir.h"
struct failing_kv {struct efs_kv *base;int read_error,write_error;unsigned writes;};
static int fault_get(void *ctx,const uint8_t *key,uint32_t kl,uint8_t *value,uint32_t *vl)
{
    struct failing_kv *fault=ctx;
    return fault->read_error?fault->read_error:efs_kv_get(fault->base,key,kl,value,vl);
}
static int fault_put(void *ctx,const uint8_t *key,uint32_t kl,const uint8_t *value,uint32_t vl)
{
    struct failing_kv *fault=ctx;fault->writes++;
    return fault->write_error?fault->write_error:efs_kv_put(fault->base,key,kl,value,vl);
}
int main(void)
{
    char dir[EFS_TEST_PATH];efs_test_path(dir,sizeof(dir),"efs-policy-XXXXXX");assert(efs_test_mkdtemp(dir));
    struct efs_kv *kv=efs_kv_lsm_open(dir,NULL);assert(kv);
    uint64_t version;uint32_t mask;
    assert(!efs_query_policy_get(kv,&version,&mask) && !version && mask==3);
    struct failing_kv fault={.base=kv};
    const struct efs_kv_ops ops={.put=fault_put,.get=fault_get};
    struct efs_kv wrapped={.ops=&ops,.ctx=&fault};
    const int failures[]={EFS_ERR_IO,EFS_ERR_NOMEM,EFS_ERR_BUSY};
    for(unsigned i=0;i<sizeof(failures)/sizeof(failures[0]);i++) {
        fault.read_error=failures[i];fault.write_error=0;fault.writes=0;
        assert(efs_query_policy_get(&wrapped,&version,&mask)==failures[i] && !mask);
        assert(efs_query_policy_apply(&wrapped,0,0)==failures[i] && !fault.writes);
        fault.read_error=0;fault.write_error=failures[i];
        assert(efs_query_policy_apply(&wrapped,0,0)==failures[i] && fault.writes==1);
        assert(!efs_query_policy_get(kv,&version,&mask) && !version && mask==3);
    }
    assert(efs_query_policy_apply(kv,0,4)==EFS_ERR_INVAL);
    assert(!efs_query_policy_apply(kv,0,0));
    assert(efs_query_policy_apply(kv,0,3)==EFS_ERR_STALE);
    assert(!efs_query_policy_get(kv,&version,&mask) && version==1 && !mask);
    efs_kv_lsm_close(kv);kv=efs_kv_lsm_open(dir,NULL);assert(kv);
    assert(!efs_query_policy_get(kv,&version,&mask) && version==1 && !mask);
    for(unsigned i=0;i<4;i++)assert(!efs_query_policy_apply(kv,i+1,i));
    assert(!efs_query_policy_get(kv,&version,&mask) && version==5 && mask==3);
    uint8_t key[3]={0,1,EFS_KV_KIND_QUERY_POLICY},value[12]={0};
    memset(value,255,8);value[11]=3;
    assert(!efs_kv_put(kv,key,3,value,12));
    assert(efs_query_policy_apply(kv,UINT64_MAX,0)==EFS_ERR_INVAL);
    assert(!efs_query_policy_get(kv,&version,&mask) && version==UINT64_MAX && mask==3);
    value[11]=4;assert(!efs_kv_put(kv,key,3,value,12));
    assert(efs_query_policy_get(kv,&version,&mask)==EFS_ERR_PROTO && !mask);
    assert(efs_query_policy_apply(kv,0,3)==EFS_ERR_PROTO);
    assert(!efs_kv_put(kv,key,3,value,11));
    assert(efs_query_policy_get(kv,&version,&mask)==EFS_ERR_PROTO && !mask);
    memset(value,0,12);value[11]=3;assert(!efs_kv_put(kv,key,3,value,12));
    assert(efs_query_policy_get(kv,&version,&mask)==EFS_ERR_PROTO && !mask);
    efs_kv_lsm_close(kv);
    DIR *d=opendir(dir);assert(d);struct dirent *entry;
    while((entry=readdir(d)))if(strcmp(entry->d_name,".") && strcmp(entry->d_name,"..")) {
        char path[512];snprintf(path,sizeof(path),"%s/%s",dir,entry->d_name);assert(!unlink(path));
    }
    closedir(d);assert(!rmdir(dir));
    puts("query policy: defaults, masks, stale CAS, cold persistence, read/write IO/NOMEM/BUSY refusal without effects, overflow and malformed refusal PASS");
    return 0;
}
