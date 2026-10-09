/* Actual sync-WAL recovery of exact session cleanup and close obligations. */
#include "efs/session.h"
#include "efs/meta_apply.h"
#include "efs/kv_lsm.h"
#include "efs/kv_key.h"
#include <assert.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
static const uint8_t uuid[16]={61},other[16]={62};
static efs_ino_t ino;
static uint64_t gen;
static uint32_t shard;
static void key_at(unsigned i,const uint8_t *owner,unsigned epoch,uint8_t *key,uint32_t *len)
{assert(!efs_kv_key_lease(shard,ino+(uint64_t)i*4096,gen,owner,epoch,key,len));}
static void load_identity(struct efs_kv *kv)
{
    struct efs_meta_dentry dent;struct efs_meta_row row;
    assert(!efs_meta_apply_lookup(kv,1,"cold-cleanup",&dent));ino=dent.ino;
    assert(!efs_meta_apply_get_inode(kv,ino,&row));gen=row.generation;shard=efs_kv_inode_shard(ino);
}
int main(int argc,char **argv)
{
    assert(argc==2 && !mkdir(argv[1],0700));
    struct efs_kv_lsm_cfg cfg={.sync_mode=EFS_KV_LSM_SYNC};
    pid_t child=fork();assert(child>=0);
    if(!child) {
        struct efs_kv *kv=efs_kv_lsm_open(argv[1],&cfg);assert(kv);
        struct efs_meta_attrs attrs={.uid=1000,.gid=1000,.now=1000000000000000000ull};
        assert(!efs_meta_apply_init(kv,attrs.now));
        assert(!efs_meta_apply_create_file(kv,&attrs,1,S_IFREG|0600,"cold-cleanup",&ino));
        load_identity(kv);
        uint8_t key[EFS_KV_KEY_MAX],one=1;uint32_t len;
        for(unsigned i=0;i<13;i++){key_at(i,uuid,1,key,&len);assert(!efs_kv_put(kv,key,len,&one,1));}
        key_at(0,other,1,key,&len);assert(!efs_kv_put(kv,key,len,&one,1));
        key_at(0,uuid,2,key,&len);assert(!efs_kv_put(kv,key,len,&one,1));
        assert(!efs_session_fence_local(kv,shard,uuid,2));
        struct efs_session_cleanup_key keys[8];uint32_t n;
        assert(!efs_session_cleanup_select(kv,shard,uuid,1,keys,&n) && n==8);
        assert(!efs_session_cleanup_apply(kv,shard,uuid,1,keys,n));
        _exit(0); /* no clean close/checkpoint */
    }
    int status;assert(waitpid(child,&status,0)==child && WIFEXITED(status) && !WEXITSTATUS(status));
    struct efs_kv *kv=efs_kv_lsm_open(argv[1],&cfg);assert(kv);load_identity(kv);
    struct efs_session_cleanup_key old[8],next[8];uint32_t n;
    for(unsigned i=0;i<8;i++) {
        uint8_t key[EFS_KV_KEY_MAX];uint32_t len;key_at(i,uuid,1,key,&len);
        old[i].len=(uint8_t)len;memcpy(old[i].key,key,len);
    }
    assert(!efs_session_cleanup_apply(kv,shard,uuid,1,old,8));
    assert(!efs_session_cleanup_select(kv,shard,uuid,1,next,&n) && n==5);
    uint8_t marker[EFS_KV_KEY_MAX],value[8];uint32_t ml,vl=8;
    assert(!efs_kv_key_reap(efs_kv_anchor_shard(shard),ino,marker,&ml));marker[2]=EFS_KV_KIND_CLOSE_CLEANUP;
    assert(!efs_kv_get(kv,marker,ml,value,&vl) && vl==8);
    assert(!efs_session_cleanup_apply(kv,shard,uuid,1,next,n));efs_kv_lsm_close(kv);
    kv=efs_kv_lsm_open(argv[1],&cfg);assert(kv);load_identity(kv);
    assert(!efs_session_cleanup_select(kv,shard,uuid,1,next,&n) && !n);
    assert(!efs_session_cleanup_apply(kv,shard,uuid,1,old,8));
    uint8_t key[EFS_KV_KEY_MAX];uint32_t len;
    key_at(0,other,1,key,&len);vl=8;assert(!efs_kv_get(kv,key,len,value,&vl));
    key_at(0,uuid,2,key,&len);vl=8;assert(!efs_kv_get(kv,key,len,value,&vl));
    assert(efs_session_accept(kv,shard,uuid,1)==EFS_ERR_STALE);
    efs_kv_lsm_close(kv);return 0;
}
