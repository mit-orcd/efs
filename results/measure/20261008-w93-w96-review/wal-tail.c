#include "kv_lsm_internal.h"
#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
static unsigned mask;
static int record(void *u,uint8_t op,const uint8_t *k,uint32_t kl,const uint8_t *v,uint32_t vl){
 (void)u;(void)op;(void)v;(void)vl;assert(kl==1);mask|=1u<<(*k-'A');return EFS_OK;
}
int main(void){
 char path[]="/tmp/efs-w96-tail-XXXXXX";int fd=mkstemp(path);assert(fd>=0);close(fd);
 struct kv_wal *w;uint64_t seq;struct efs_kv_item item={EFS_KV_PUT,(const uint8_t*)"A",1,(const uint8_t*)"a",1};
 assert(kv_wal_open(path,EFS_KV_LSM_SYNC,&w)==EFS_OK);assert(kv_wal_append(w,&item,1,&seq)==EFS_OK);kv_wal_close(w);
 fd=open(path,O_WRONLY|O_APPEND);assert(fd>=0);unsigned char tail[8]={0};assert(write(fd,tail,8)==8);assert(fsync(fd)==0);close(fd);
 assert(kv_wal_replay(path,record,NULL)==EFS_OK);printf("first recovery mask=%u (A=1)\n",mask);
 assert(kv_wal_open(path,EFS_KV_LSM_SYNC,&w)==EFS_OK);item.key=(const uint8_t*)"B";item.val=(const uint8_t*)"b";int rc=kv_wal_append(w,&item,1,&seq);assert(rc==EFS_OK);kv_wal_close(w);
 mask=0;assert(kv_wal_replay(path,record,NULL)==EFS_OK);printf("B append rc=%d; second recovery mask=%u (A+B expected=3)\n",rc,mask);unlink(path);
 return mask==3?0:2;
}
