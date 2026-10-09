
#include <stdio.h>
#include "efs/kv_lsm.h"
#include "efs/meta_apply.h"
int main(int argc,char **argv) {
    if(argc!=2)return 2;
    struct efs_kv_lsm_cfg cfg={0};
    struct efs_kv *kv=efs_kv_lsm_open(argv[1],&cfg);
    if(!kv)return 3;
    struct efs_meta_dentry d;
    int rc=efs_meta_apply_lookup(kv,1,"after-kv-fault",&d);
    printf("rc=%d ino=%llu\n",rc,(unsigned long long)(rc?0:d.ino));
    efs_kv_lsm_close(kv);return rc?4:0;
}
