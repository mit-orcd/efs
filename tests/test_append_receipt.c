/* Actual metadata append reservations: atomic durable receipts, bounded ACK
 * retirement, payload binding and recovery without a graceful KV close. */
#include "efs/meta_apply.h"
#include "efs/kv_lsm.h"
#include "efs/txn.h"
#include "efs/kv_key.h"
#include <assert.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
static int coord(void *ctx,const struct efs_txid *t,uint32_t shard,int *decision)
{ return efs_txn_decision_get(ctx,shard,t,decision); }
static struct efs_opid_req identity(uint64_t seq,uint64_t ack)
{
    struct efs_opid_req q={0};q.id.client_uuid[0]=17;
    q.id.session_epoch=1;q.id.seq=seq;q.ack=ack;return q;
}
static void exercise(struct efs_kv *kv,efs_ino_t ino)
{
    uint64_t off=999,wm,frontier;uint32_t open;
    struct efs_opid_req q=identity(1,0);
    assert(efs_meta_apply_append_reserve_durable(kv,ino,8,&q,coord,kv,&off)==EFS_OK && off==0);
    assert(efs_meta_apply_append_resolve(kv,ino,off,EFS_META_APPEND_COMPLETED)==EFS_OK);
    off=999;assert(efs_meta_apply_append_reserve_durable(kv,ino,8,&q,coord,kv,&off)==EFS_OK && off==0);
    assert(efs_meta_apply_append_reserve_durable(kv,ino,9,&q,coord,kv,&off)==EFS_ERR_INVAL);
    for(unsigned i=2;i<=EFS_OPID_REPLY_CACHE;i++) {
        q=identity(i,0);
        assert(efs_meta_apply_append_reserve_durable(kv,ino,8,&q,coord,kv,&off)==EFS_OK && off==(i-1)*8);
        assert(efs_meta_apply_append_resolve(kv,ino,off,EFS_META_APPEND_COMPLETED)==EFS_OK);
    }
    assert(efs_meta_apply_append_state(kv,ino,&wm,&frontier,&open)==EFS_OK && !wm && !open);
    q=identity(17,0);off=999;
    assert(efs_meta_apply_append_reserve_durable(kv,ino,8,&q,coord,kv,&off)==EFS_ERR_BUSY && off==999);
    assert(efs_meta_apply_append_state(kv,ino,&wm,&frontier,&open)==EFS_OK && !wm && !open);
    q=identity(17,16);
    assert(efs_meta_apply_append_reserve_durable(kv,ino,8,&q,coord,kv,&off)==EFS_OK && off==128);
    q=identity(1,0);off=999;
    assert(efs_meta_append_result(kv,ino,8,&q,&off)==EFS_ERR_STALE && off==999);
    q=identity(18,18);
    assert(efs_meta_apply_append_reserve_durable(kv,ino,8,&q,coord,kv,&off)==EFS_ERR_INVAL);
}
static efs_ino_t create(struct efs_kv *kv)
{
    struct efs_meta_attrs at={0,0,1};efs_ino_t ino;
    assert(efs_meta_apply_init(kv,1)==EFS_OK);
    assert(efs_meta_apply_create_file(kv,&at,EFS_ROOT_INO,S_IFREG|0644,"append",&ino)==EFS_OK);
    return ino;
}
int main(void)
{
    struct efs_kv *kv=efs_kv_mem_create();assert(kv);efs_ino_t ino=create(kv);
    struct efs_opid_req q=identity(1,0);uint64_t off=999;
    assert(efs_kv_mem_fail_next_batch(kv)==EFS_OK);
    assert(efs_meta_apply_append_reserve_durable(kv,ino,8,&q,coord,kv,&off)==EFS_ERR_IO);
    assert(efs_meta_append_result(kv,ino,8,&q,&off)==EFS_ERR_NOT_FOUND);
    uint64_t wm,frontier;uint32_t open;
    assert(efs_meta_apply_append_state(kv,ino,&wm,&frontier,&open)==EFS_OK && !wm && !open);
    exercise(kv,ino);
    assert(efs_meta_apply_check(kv)==EFS_OK);
    struct efs_meta_row row;assert(efs_meta_apply_get_inode(kv,ino,&row)==EFS_OK);
    assert(efs_meta_apply_unlink(kv,EFS_ROOT_INO,"append",2)==EFS_OK);
    assert(efs_meta_apply_reap_done(kv,ino,row.generation)==EFS_OK);
    q=identity(17,16);assert(efs_meta_append_result(kv,ino,8,&q,&off)==EFS_ERR_NOT_FOUND);
    assert(efs_meta_apply_get_inode(kv,ino,&row)==EFS_ERR_NOT_FOUND);
    efs_kv_mem_free(kv);
    char dir[]="/tmp/efs-append-receipt-XXXXXX";assert(mkdtemp(dir));
    kv=efs_kv_lsm_open(dir,NULL);assert(kv);ino=create(kv);efs_kv_lsm_close(kv);
    pid_t child=fork();assert(child>=0);
    if(!child){kv=efs_kv_lsm_open(dir,NULL);assert(kv);exercise(kv,ino);_exit(0);}
    int status;assert(waitpid(child,&status,0)==child && WIFEXITED(status) && !WEXITSTATUS(status));
    kv=efs_kv_lsm_open(dir,NULL);assert(kv);q=identity(17,16);
    assert(efs_meta_apply_append_reserve_durable(kv,ino,8,&q,coord,kv,&off)==EFS_OK && off==128);
    q=identity(1,0);assert(efs_meta_apply_append_reserve_durable(kv,ino,8,&q,coord,kv,&off)==EFS_ERR_STALE);
    assert(efs_meta_apply_append_state(kv,ino,&wm,&frontier,&open)==EFS_OK && wm==136 && open==1);
    efs_kv_lsm_close(kv);
    DIR *d=opendir(dir);assert(d);struct dirent *e;
    while((e=readdir(d))) {if(e->d_name[0]=='.')continue;char p[512];snprintf(p,sizeof(p),"%s/%s",dir,e->d_name);assert(!unlink(p));}
    closedir(d);assert(!rmdir(dir));puts("append durable receipt: atomic reservation, bounded ACK retirement, payload binding and crash recovery PASS");
    return 0;
}
