/* Inspect only a COPY of an owned, stopped fixture KV. LSM open may repair
 * its WAL tail; never open a running or installed metadata store here. */
#include "efs/kv_lsm.h"
#include "efs/kv_key.h"
#include "efs/meta_apply.h"
#include <stdio.h>
#include <stdlib.h>
int main(int argc,char **argv)
{
    if(argc!=3)return 2;
    efs_ino_t ino=strtoull(argv[2],NULL,10);
    struct efs_kv *kv=efs_kv_lsm_open(argv[1],NULL);
    if(!kv)return 2;
    uint8_t key[EFS_KV_KEY_MAX],value[64];uint32_t kl=0,vl=sizeof(value);
    int rc=efs_kv_key_reap(efs_kv_inode_shard(ino),ino,key,&kl);
    if(!rc)rc=efs_kv_get(kv,key,kl,value,&vl);
    uint64_t gen=0,lanes=0;
    if(!rc)rc=efs_meta_unpack_reap(value,vl,&gen,&lanes);
    printf("ino=%llu rc=%d retained=%u generation=%llu lanes=%llx\n",
           (unsigned long long)ino,rc,rc==EFS_OK,
           (unsigned long long)gen,(unsigned long long)lanes);
    efs_kv_lsm_close(kv);
    return rc==EFS_OK?0:rc==EFS_ERR_NOT_FOUND?1:2;
}
