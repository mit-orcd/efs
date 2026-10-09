#include "efs/query_catalog.h"
#include <stdio.h>
#include <string.h>
static unsigned long long live,dead;
static int entry(void *ctx,const uint8_t *key,uint32_t kn,const uint8_t *value,uint32_t vn) {
    (void)ctx;
    struct efs_meta_dentry dent;
    int rc=efs_meta_unpack_dentry(value,vn,&dent);if(rc)return rc;
    if(dent.ino==4113) {
        efs_ino_t parent=0;if(kn<11)return EFS_ERR_PROTO;
        for(unsigned i=3;i<11;i++)parent=(parent<<8)|key[i];
        fprintf(stderr,"incoming shard=%u parent=%llu type=%u gen=%u name=%.*s\n",((unsigned)key[0]<<8)|key[1],(unsigned long long)parent,dent.type,dent.generation,(int)(kn-11),key+11);
        if(dent.type==EFS_META_DENT_TOMBSTONE)dead++;else live++;
    }
    return 0;
}
int main(int argc,char **argv) {
    if(argc!=2)return 2;
    struct efs_kv_lsm_cfg cfg={EFS_KV_LSM_NOSYNC,0,4};struct efs_kv *kv=efs_kv_lsm_open(argv[1],&cfg);if(!kv)return 3;
    struct efs_meta_row row;
    int rc=efs_meta_apply_get_inode(kv,4113,&row);
    fprintf(stderr,"inode=4113 rc=%d parent=%llu nlink=%u mode=%o generation=%u\n",rc,(unsigned long long)(rc?0:row.parent),rc?0:row.nlink,rc?0:row.mode,rc?0:row.generation);
    rc=efs_meta_apply_get_inode(kv,9876,&row);fprintf(stderr,"parent=9876 rc=%d mode=%o layout=%u inode_shard=%u hashed_shard=%u\n",rc,row.mode,row.layout,efs_kv_inode_shard(9876),efs_kv_dentry_shard(9876,"level_2",EFS_META_LAYOUT_HASHED));
    struct efs_meta_dentry lookup;
    rc=efs_meta_apply_lookup(kv,9876,"level_2",&lookup);
    fprintf(stderr,"lookup rc=%d ino=%llu\n",rc,(unsigned long long)(rc?0:lookup.ino));
    for(unsigned shard=0;shard<4096;shard++) {
        uint8_t prefix[3]={shard>>8,shard,EFS_KV_KIND_DENTRY};
        rc=efs_kv_scan_prefix(kv,prefix,3,entry,NULL);if(rc)break;
    }
    fprintf(stderr,"incoming_live=%llu incoming_tombstones=%llu scan_rc=%d\n",live,dead,rc);
    efs_kv_lsm_close(kv);return rc?1:0;
}
