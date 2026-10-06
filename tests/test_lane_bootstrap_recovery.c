/* Crash after durable PREPARE/DECIDE: authority must remain fail closed. */
#include "efs/meta_apply.h"
#include "efs/kv_lsm.h"
#include "efs/kv_key.h"
#include "efs/txn.h"
#include <assert.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
static int decision(void *ctx, const struct efs_txid *t, uint32_t sh, int *d)
{ return efs_txn_decision_get(ctx, sh, t, d); }
static void prepare(struct efs_kv *kv, const struct efs_txid *t,
                    const struct efs_txn_parts *parts,
                    const struct efs_meta_lane_bootstrap *q, uint8_t lane)
{
    uint8_t key[EFS_KV_KEY_MAX], pay[EFS_META_LANE_BOOTSTRAP_BYTES]; uint32_t kl;
    assert(efs_meta_encode_lane_bootstrap(q,lane,key,&kl,pay)==EFS_OK);
    assert(efs_txn_apply_prepare(kv,EFS_TXN_LANE_BOOTSTRAP,t,parts,key,kl,pay,sizeof(pay))==EFS_OK);
}
static void run(int verdict)
{
    char dir[]="/tmp/efs-bootstrap-XXXXXX"; assert(mkdtemp(dir));
    struct efs_kv *kv=efs_kv_lsm_open(dir,NULL); assert(kv);
    struct efs_meta_attrs at={1000,1000,1}; efs_ino_t ino=0;
    struct efs_meta_row row; struct efs_txid t={{0x75}};
    assert(efs_meta_apply_init(kv,1)==EFS_OK);
    assert(efs_meta_apply_create_file(kv,&at,EFS_ROOT_INO,S_IFREG|0644,"file",&ino)==EFS_OK);
    assert(efs_meta_apply_get_inode(kv,ino,&row)==EFS_OK);
    assert(efs_meta_apply_content_fence(kv,ino,row.generation,EFS_META_FENCE_INODE,0,1,100,2)==EFS_OK);
    struct efs_txn_parts parts={0}; parts.shard[parts.n++]=efs_kv_inode_shard(ino);
    uint32_t lane=efs_kv_lane_shard(ino,17);
    if(lane!=parts.shard[0]) parts.shard[parts.n++]=lane;
    efs_kv_lsm_close(kv);
    pid_t child=fork(); assert(child>=0);
    if(!child) {
        kv=efs_kv_lsm_open(dir,NULL); assert(kv);
        struct efs_meta_lane_bootstrap q;
        assert(efs_meta_capture_lane_bootstrap(kv,ino,row.generation,17,EFS_MIN_CHUNK_SIZE,decision,kv,&q)==EFS_OK);
        prepare(kv,&t,&parts,&q,EFS_META_FENCE_INODE); prepare(kv,&t,&parts,&q,17);
        if(verdict) assert(efs_txn_decide(kv,efs_txn_coordinator(&t,&parts),&t,verdict)==EFS_OK);
        _exit(0); /* no flush/close: only acknowledged WAL batches survive */
    }
    int status; assert(waitpid(child,&status,0)==child && WIFEXITED(status) && !WEXITSTATUS(status));
    kv=efs_kv_lsm_open(dir,NULL); assert(kv);
    struct efs_meta_writer_view view;
    assert(efs_meta_get_lane_writer_view(kv,ino,row.generation,17,EFS_MIN_CHUNK_SIZE,&view)==EFS_ERR_BUSY);
    if(!verdict) { verdict=EFS_TXN_ABORT; assert(efs_txn_decide(kv,efs_txn_coordinator(&t,&parts),&t,verdict)==EFS_OK); }
    assert(efs_txn_resolve(kv,&t,lane,verdict)==EFS_OK);
    int expected=verdict==EFS_TXN_COMMIT?EFS_OK:EFS_ERR_NOT_FOUND;
    assert(efs_meta_get_lane_writer_view(kv,ino,row.generation,17,EFS_MIN_CHUNK_SIZE,&view)==expected);
    if(expected==EFS_OK) assert(view.authority_epoch==1 && view.history.count==1);
    assert(efs_txn_resolve(kv,&t,parts.shard[0],verdict)==EFS_OK);
    efs_kv_lsm_close(kv); kv=efs_kv_lsm_open(dir,NULL); assert(kv);
    assert(efs_meta_get_lane_writer_view(kv,ino,row.generation,17,EFS_MIN_CHUNK_SIZE,&view)==expected);
    assert(efs_meta_apply_get_inode(kv,ino,&row)==EFS_OK);
    assert(!!(row.active_lanes & (1ULL<<17))==(expected==EFS_OK));
    efs_kv_lsm_close(kv);
    DIR *d=opendir(dir); assert(d); struct dirent *e;
    while((e=readdir(d))) { if(e->d_name[0]=='.') continue; char path[512]; snprintf(path,sizeof(path),"%s/%s",dir,e->d_name); assert(!unlink(path)); }
    closedir(d); assert(!rmdir(dir));
}
int main(void) { run(EFS_TXN_COMMIT);run(EFS_TXN_ABORT);run(0);puts("lane bootstrap durable crash recovery PASS"); }
