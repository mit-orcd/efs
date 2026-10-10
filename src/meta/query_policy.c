#include "efs/query_policy.h"
static uint64_t read64(const uint8_t *p)
{
    uint64_t n=0;for(unsigned i=0;i<8;i++)n=(n<<8)|p[i];return n;
}
static void write64(uint8_t *p,uint64_t n)
{
    for(unsigned i=0;i<8;i++)p[7-i]=(uint8_t)(n>>(8*i));
}
static const uint8_t policy_key[3]={0,1,EFS_KV_KIND_QUERY_POLICY};
int efs_query_policy_get(struct efs_kv *kv,uint64_t *version,uint32_t *mask)
{
    if(!kv || !version || !mask)return EFS_ERR_INVAL;
    *version=0;*mask=0;
    uint8_t value[12];uint32_t length=sizeof(value);
    int rc=efs_kv_get(kv,policy_key,sizeof(policy_key),value,&length);
    if(rc==EFS_ERR_NOT_FOUND){*mask=EFS_FEATURES_DEFAULT;return EFS_OK;}
    if(rc)return rc;
    if(length!=sizeof(value) || value[8] || value[9] || value[10] ||
       (value[11]&~EFS_FEATURES_DEFAULT) || !read64(value))return EFS_ERR_PROTO;
    *version=read64(value);*mask=value[11];return EFS_OK;
}
int efs_query_policy_apply(struct efs_kv *kv,uint64_t expected,uint32_t mask)
{
    if(mask&~EFS_FEATURES_DEFAULT)return EFS_ERR_INVAL;
    uint64_t version;uint32_t old;
    int rc=efs_query_policy_get(kv,&version,&old);
    if(rc)return rc;
    if(version!=expected)return EFS_ERR_STALE;
    if(version==UINT64_MAX)return EFS_ERR_INVAL;
    uint8_t value[12]={0};write64(value,version+1);value[11]=(uint8_t)mask;
    return efs_kv_put(kv,policy_key,sizeof(policy_key),value,sizeof(value));
}
