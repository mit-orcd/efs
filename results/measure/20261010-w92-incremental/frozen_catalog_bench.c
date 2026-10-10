/* Same stopped authoritative namespace, fresh cache, then unchanged refreshes. */
#include "efs/query_catalog.h"
#include <stdio.h>
#include <time.h>
static double stamp(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec/1e9;}
int main(int argc,char **argv)
{
    if(argc!=3)return 2;
    struct efs_kv_lsm_cfg cfg={EFS_KV_LSM_NOSYNC,0,4};
    struct efs_kv *kv=efs_kv_lsm_open(argv[1],&cfg);if(!kv)return 1;
    struct efs_kv_lsm_snapshot *s=NULL;int rc=efs_kv_lsm_snapshot_pin(kv,64<<20,&s);
    struct efs_query_catalog *old=NULL;
    for(int i=0;i<4 && !rc;i++){
        struct efs_query_catalog *next=NULL;double start=stamp();
        rc=efs_query_catalog_build_disk(argv[2],s,old,4<<20,NULL,NULL,&next);
        double elapsed=stamp()-start;struct efs_query_rollup stats={0};
        if(!rc)rc=efs_query_catalog_stats(next,EFS_ROOT_INO,&stats);
        printf("{\"round\":%d,\"seconds\":%.6f,\"rc\":%d,\"files\":%llu,\"dirs\":%llu,\"logical_bytes\":%llu,\"accounted_bytes\":%llu,\"rebuilt\":%u,\"refolded\":%u}\n",i,elapsed,rc,
          (unsigned long long)stats.tree_files,(unsigned long long)stats.tree_dirs,(unsigned long long)stats.tree_bytes,
          (unsigned long long)efs_query_catalog_bytes(next),efs_query_catalog_rebuilt(next),efs_query_catalog_refolded(next));fflush(stdout);
        efs_query_catalog_free(old);old=next;
    }
    efs_query_catalog_free(old);efs_kv_lsm_snapshot_release(s);efs_kv_lsm_close(kv);return rc?1:0;
}
