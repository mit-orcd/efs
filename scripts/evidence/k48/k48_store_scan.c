/* Offline K48 reader: build against the captured store's exact source.
 * Only open a disposable COPY: LSM open can replay/repair its WAL. */
#include "efs/kv_lsm.h"
#include "efs/meta_apply.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned long long be(const unsigned char *p,unsigned n){unsigned long long v=0;while(n--)v=v*256+*p++;return v;}
static efs_ino_t target;
static int scan(void *u,const uint8_t *k,uint32_t kl,const uint8_t *v,uint32_t vl){
 (void)u;if(kl<11||be(k+3,8)!=target)return 0;
 if(k[2]==EFS_KV_KIND_DENTRY){struct efs_meta_dentry d={0};int rc=efs_meta_unpack_dentry(v,vl,&d);
  printf("DENTRY shard=%llu name=%.*s rc=%d ino=%llu type=%u bytes=%u\n",be(k,2),(int)(kl-11),k+11,rc,(unsigned long long)d.ino,d.type,vl);}
 if(k[2]==EFS_KV_KIND_INODE){struct efs_meta_row r={0};int rc=efs_meta_unpack_inode(v,vl,&r);
  printf("INODE shard=%llu rc=%d layout=%u nents=%u bitmap=%016llx bytes=%u\n",be(k,2),rc,rc?0:r.layout,rc?0:r.nents,rc?0:(unsigned long long)r.used_shards,vl);}
 return 0;
}
int main(int ac,char **av){if(ac!=3){fprintf(stderr,"usage: k48_store_scan COPY_KV_DIR INODE\n");return 2;}target=strtoull(av[2],0,10);
 struct efs_kv_lsm_cfg cfg={0};struct efs_kv *kv=efs_kv_lsm_open(av[1],&cfg);if(!kv)return 1;
 int rc=efs_kv_scan(kv,scan,0);printf("SCAN rc=%d\n",rc);
 for(unsigned size=1;size<=128;size*=2){struct efs_meta_dir_cursor c={0};unsigned total=0,n=0,pages=0;
  do{struct efs_meta_dir_ent out[128];n=0;rc=efs_meta_apply_readdir(kv,target,&c,out,size,&n);total+=n;pages++;
   if(rc)break;
   if(size==128)for(unsigned j=0;j<n;j++)printf("READDIR name=%s ino=%llu\n",out[j].name,(unsigned long long)out[j].d.ino);
  }while(!c.done&&pages<10000);
  printf("PAGES size=%u total=%u pages=%u rc=%d done=%u src=%u last=%s\n",size,total,pages,rc,c.done,c.src,c.name);}
 efs_kv_lsm_close(kv);return rc?1:0;}
