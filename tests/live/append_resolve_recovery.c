/* Private sync-WAL: failed legacy completion survives crash, retry is durable. */
#include "efs/meta_apply.h"
#include "efs/kv_lsm.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
static int coord(void *ctx,const struct efs_txid *id,uint32_t shard,int *decision)
{return efs_txn_decision_get(ctx,shard,id,decision);}
static int fail_batch(void *ctx,const struct efs_kv_item *items,uint32_t n)
{(void)ctx;(void)items;(void)n;return EFS_ERR_IO;}
static efs_ino_t identity(struct efs_kv *kv)
{
    struct efs_meta_dentry d;
    assert(!efs_meta_apply_lookup(kv,1,"append-recovery",&d));return d.ino;
}
static void wait_ok(pid_t child)
{
    int status;assert(waitpid(child,&status,0)==child &&
        WIFEXITED(status) && !WEXITSTATUS(status));
}
int main(int argc,char **argv)
{
    assert(argc==2 && !mkdir(argv[1],0700));
    struct efs_kv_lsm_cfg cfg={.sync_mode=EFS_KV_LSM_SYNC};
    struct efs_opid op={.session_epoch=1,.seq=1};op.client_uuid[15]=71;
    pid_t child=fork();assert(child>=0);
    if(!child) {
        struct efs_kv *kv=efs_kv_lsm_open(argv[1],&cfg);assert(kv);
        struct efs_meta_attrs a={.uid=1000,.gid=1000,.now=1000000000000000000ull};
        efs_ino_t ino;uint64_t off;
        assert(!efs_meta_apply_init(kv,a.now));
        assert(!efs_meta_apply_create_file(kv,&a,1,S_IFREG|0600,"append-recovery",&ino));
        assert(!efs_meta_apply_append_reserve(kv,ino,64,&op,coord,kv,&off) && !off);
        const struct efs_kv_ops *original=kv->ops;struct efs_kv_ops fault=*original;
        fault.batch=fail_batch;kv->ops=&fault;
        assert(efs_meta_apply_append_resolve(kv,ino,off,EFS_META_APPEND_ABORTED_HOLE)==EFS_ERR_IO);
        kv->ops=original;
        struct efs_meta_stat st;
        assert(!efs_meta_apply_getattr(kv,ino,coord,kv,&st) && !st.size);
        _exit(0); /* no close/checkpoint after failed apply */
    }
    wait_ok(child);
    child=fork();assert(child>=0);
    if(!child) {
        struct efs_kv *kv=efs_kv_lsm_open(argv[1],&cfg);assert(kv);
        efs_ino_t ino=identity(kv);uint64_t off=99;
        assert(!efs_meta_apply_append_reserve(kv,ino,64,&op,coord,kv,&off) && !off);
        assert(!efs_meta_apply_append_resolve(kv,ino,off,EFS_META_APPEND_ABORTED_HOLE));
        _exit(0); /* lose the successful reply and clean close */
    }
    wait_ok(child);
    struct efs_kv *kv=efs_kv_lsm_open(argv[1],&cfg);assert(kv);
    efs_ino_t ino=identity(kv);struct efs_meta_stat st;
    assert(!efs_meta_apply_getattr(kv,ino,coord,kv,&st) && st.size==64);
    assert(!efs_meta_apply_append_resolve(kv,ino,0,EFS_META_APPEND_ABORTED_HOLE));
    assert(!efs_meta_apply_getattr(kv,ino,coord,kv,&st) && st.size==64);
    efs_kv_lsm_close(kv);
    puts("append resolution: failed apply, cold retry and lost-reply replay PASS");
    return 0;
}
