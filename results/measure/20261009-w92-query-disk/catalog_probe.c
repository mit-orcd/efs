#include "efs/query_catalog.h"
#include <stdio.h>
int main(int argc,char **argv) {
    if(argc!=3)return 2;
    struct efs_kv_lsm_cfg cfg={EFS_KV_LSM_NOSYNC,0,4};
    struct efs_kv *kv=efs_kv_lsm_open(argv[1],&cfg);
    if(!kv)return 3;
    struct efs_kv_lsm_snapshot *source=NULL;
    int rc=efs_kv_lsm_snapshot_pin(kv,8<<20,&source);
    struct efs_query_catalog *catalog=NULL;
    if(!rc)rc=efs_query_catalog_build_disk(argv[2],source,NULL,16<<20,NULL,NULL,&catalog);
    fprintf(stderr,"probe rc=%d accounted_bytes=%llu\n",rc,(unsigned long long)efs_query_catalog_bytes(catalog));
    efs_query_catalog_free(catalog);efs_kv_lsm_snapshot_release(source);efs_kv_lsm_close(kv);
    return rc?1:0;
}
