/* Read the stopped private follower's actual cold KV, bypassing leaders. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "efs/kv_lsm.h"
#include "efs/meta_apply.h"
static int decision(void *ctx,const struct efs_txid *t,uint32_t shard,int *out)
{ return efs_txn_decision_get(ctx,shard,t,out); }
int main(int argc,char **argv)
{
    assert(argc==3);
    struct efs_kv_lsm_cfg cfg={0};
    struct efs_kv *kv=efs_kv_lsm_open(argv[1],&cfg);assert(kv);
    if(!strcmp(argv[2],"--cursor")) {
        for(unsigned group=0;group<=2;group+=2) {
            uint8_t key[7]={255,255,'r','a','f','t',(uint8_t)group},value[16];
            uint32_t bytes=sizeof(value);uint64_t index=0,check=0;
            assert(efs_kv_get(kv,key,sizeof(key),value,&bytes)==EFS_OK && bytes==16);
            for(unsigned j=0;j<8;j++){index=index<<8|value[j];check=check<<8|value[j+8];}
            assert(check==~index && index);
            printf("%u %llu\n",group,(unsigned long long)index);
        }
        efs_kv_lsm_close(kv);return 0;
    }
    efs_ino_t parent=(efs_ino_t)strtoull(argv[2],NULL,10);assert(parent);
    for(unsigned i=0;i<8;i++) {
        char name[64];struct efs_meta_dentry a,b,absent;
        snprintf(name,sizeof(name),"original-%u",i);
        assert(efs_meta_apply_lookup(kv,parent,name,&absent)==EFS_ERR_NOT_FOUND);
        snprintf(name,sizeof(name),"renamed-%u",i);
        assert(efs_meta_apply_lookup(kv,parent,name,&a)==EFS_OK);
        snprintf(name,sizeof(name),"linked-%u",i);
        assert(efs_meta_apply_lookup(kv,parent,name,&b)==EFS_OK);
        struct efs_meta_stat st;
        assert(efs_meta_apply_getattr(kv,a.ino,decision,kv,&st)==EFS_OK);
        assert(a.ino==b.ino && st.nlink==2 && (st.mode&0777)==0640 && st.size==1024+i);
        printf("%u %llu %llu %u %u %llu\n",i,(unsigned long long)a.ino,
               (unsigned long long)b.ino,st.nlink,st.mode&0777,(unsigned long long)st.size);
    }
    struct efs_meta_dentry gone;
    assert(efs_meta_apply_lookup(kv,parent,"removed",&gone)==EFS_ERR_NOT_FOUND);
    efs_kv_lsm_close(kv);return 0;
}
