/* Read-only key lookup; only open stopped private fixture stores. */
#include "efs/kv_lsm.h"
#include "efs/kv_key.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
int main(int argc,char **argv) {
 assert(argc==3);efs_ino_t ino=strtoull(argv[2],NULL,10);
 struct efs_kv *kv=efs_kv_lsm_open(argv[1],NULL);assert(kv);
 uint8_t key[EFS_KV_KEY_MAX],value[4096];uint32_t kl=0,vl=sizeof(value);
 uint32_t shard=efs_kv_anchor_shard(efs_kv_inode_shard(ino));
 assert(!efs_kv_key_reap(shard,ino,key,&kl));key[2]=EFS_KV_KIND_CLOSE_STEP;
 int rc=efs_kv_get(kv,key,kl,value,&vl);
 printf("ino=%llu shard=%u rc=%d bytes=%u value=",(unsigned long long)ino,shard,rc,vl);
 if(!rc) {for(unsigned i=0;i<vl;i++)printf("%02x",value[i]);}
 puts("");
 efs_kv_lsm_close(kv);return 0;
}
