/* Private LSM: crash after one exact-key batch, then replay it unchanged. */
#include "efs/meta_apply.h"
#include "efs/kv_lsm.h"
#include "efs/kv_key.h"
#include <assert.h>
#include <stdio.h>
#include <sys/wait.h>
#include <unistd.h>
#include <sys/stat.h>
static const efs_ino_t ino=70001;
static const uint64_t gen=9;
int main(int argc,char **argv)
{
    assert(argc==2 && mkdir(argv[1],0700)==0);
    struct efs_kv_lsm_cfg cfg={.sync_mode=EFS_KV_LSM_SYNC};
    pid_t pid=fork();assert(pid>=0);
    if (!pid) {
        struct efs_kv *kv=efs_kv_lsm_open(argv[1],&cfg);assert(kv);
        uint8_t key[EFS_KV_KEY_MAX],value[EFS_META_REAP_VAL]={0};uint32_t kl;
        for (unsigned i=0;i<13;i++) {
            assert(efs_kv_key_append_rsv(efs_kv_inode_shard(ino),ino,gen,i,key,&kl)==EFS_OK);
            assert(efs_kv_put(kv,key,kl,value,1)==EFS_OK);
        }
        assert(efs_kv_key_reap(efs_kv_anchor_shard(efs_kv_inode_shard(ino)),ino,key,&kl)==EFS_OK);
        efs_meta_pack_reap(value,gen,0);
        assert(efs_kv_put(kv,key,kl,value,sizeof(value))==EFS_OK);
        struct efs_meta_reap_key keys[EFS_META_REAP_BATCH_MAX];uint32_t n;
        assert(efs_meta_reap_select(kv,ino,gen,keys,&n)==EFS_OK && n==8);
        assert(efs_meta_apply_reap_batch(kv,ino,gen,keys,n)==EFS_ERR_BUSY);
        _exit(0); /* no close/flush; recover the durable WAL */
    }
    int status;assert(waitpid(pid,&status,0)==pid && WIFEXITED(status) && WEXITSTATUS(status)==0);
    struct efs_kv *kv=efs_kv_lsm_open(argv[1],&cfg);assert(kv);
    struct efs_meta_reap_key old[EFS_META_REAP_BATCH_MAX],next[EFS_META_REAP_BATCH_MAX];uint32_t n;
    for (unsigned i=0;i<EFS_META_REAP_BATCH_MAX;i++) {
        uint8_t key[EFS_KV_KEY_MAX];uint32_t kl;
        assert(efs_kv_key_append_rsv(efs_kv_inode_shard(ino),ino,gen,i,key,&kl)==EFS_OK);
        old[i].len=(uint8_t)kl;
        for (unsigned j=0;j<kl;j++) old[i].key[j]=key[j];
    }
    assert(efs_meta_apply_reap_batch(kv,ino,gen,old,EFS_META_REAP_BATCH_MAX)==EFS_ERR_BUSY);
    assert(efs_meta_reap_select(kv,ino,gen,next,&n)==EFS_OK && n==5);
    assert(efs_meta_apply_reap_batch(kv,ino,gen,next,n)==EFS_OK);
    efs_kv_lsm_close(kv);
    kv=efs_kv_lsm_open(argv[1],&cfg);assert(kv);
    assert(efs_meta_reap_select(kv,ino,gen,next,&n)==EFS_OK && n==0);
    assert(efs_meta_apply_reap_batch(kv,ino,gen,old,EFS_META_REAP_BATCH_MAX)==EFS_OK);
    efs_kv_lsm_close(kv);
    puts("bounded reap: durable WAL replay preserves next five keys and cold finalization PASS");
    return 0;
}
