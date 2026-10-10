/* Read the stopped private follower's actual cold KV, bypassing leaders. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "efs/kv_lsm.h"
#include "efs/meta_apply.h"
int main(int argc,char **argv)
{
    assert(argc==3);
    struct efs_kv_lsm_cfg cfg={0};
    struct efs_kv *kv=efs_kv_lsm_open(argv[1],&cfg);assert(kv);
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
        assert(efs_meta_apply_getattr(kv,a.ino,NULL,NULL,&st)==EFS_OK);
        assert(a.ino==b.ino && st.nlink==2 && (st.mode&0777)==0640 && st.size==1024+i);
        printf("%u %llu %llu %u %u %llu\n",i,(unsigned long long)a.ino,
               (unsigned long long)b.ino,st.nlink,st.mode&0777,(unsigned long long)st.size);
    }
    struct efs_meta_dentry gone;
    assert(efs_meta_apply_lookup(kv,parent,"removed",&gone)==EFS_ERR_NOT_FOUND);
    efs_kv_lsm_close(kv);return 0;
}
