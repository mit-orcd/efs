/* I23 admission, apply-order races, receipt retention and durable fences. */
#include "efs/publication.h"
#include "efs/session.h"
#include "efs/kv_key.h"
#include "efs/kv_lsm.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <dirent.h>
static struct efs_meta_pub intent(efs_ino_t ino,uint64_t gen,uint64_t seq)
{
    struct efs_meta_pub p={0};p.ino=ino;p.inode_gen=gen;p.candidate_gen=seq;
    p.new_size=64;p.coding_profile_id=EFS_META_PROFILE_K2F1;p.durable_result=1;
    p.publication_id.client_uuid[0]=77;p.publication_id.session_epoch=1;p.publication_id.seq=seq;
    for(unsigned i=0;i<EFS_NUM_FRAGMENTS;i++)p.ch.nodes[i]=i+1;
    return p;
}
static void fenced(struct efs_kv *kv,struct efs_meta_pub p)
{
    int v;struct efs_meta_pub fresh=p;fresh.publication_id.seq=20;fresh.candidate_gen=20;
    fresh.expected_gen=10;
    assert(efs_meta_apply_publish(kv,&p)==EFS_ERR_STALE); /* even exact successful retry */
    assert(efs_meta_apply_publish(kv,&fresh)==EFS_ERR_STALE);
    assert(efs_meta_apply_publication_retire(kv,&p)==EFS_ERR_STALE);
    assert(efs_meta_publication_result(kv,&fresh,&v)==EFS_ERR_NOT_FOUND); /* no terminal receipt */
    assert(efs_meta_publication_result(kv,&p,&v)==EFS_OK && v==EFS_OK);
    struct efs_meta_chunk ch;assert(efs_meta_apply_get_chunk(kv,p.ino,0,&ch)==EFS_OK && ch.generation==10);
    uint32_t shard=efs_kv_lane_shard(p.ino,0);
    assert(efs_session_establish(kv,shard,p.publication_id.client_uuid,1)==EFS_ERR_STALE);
    assert(efs_session_reclaimable(kv,shard,p.publication_id.client_uuid,0)==EFS_ERR_INVAL);
}
static struct efs_meta_pub setup(struct efs_kv *kv,int finish)
{
    struct efs_meta_attrs attrs={0,0,1};efs_ino_t ino;struct efs_meta_row row;
    assert(efs_meta_apply_init(kv,1)==EFS_OK);
    assert(efs_meta_apply_create_file(kv,&attrs,EFS_ROOT_INO,S_IFREG|0644,"session",&ino)==EFS_OK);
    assert(efs_meta_apply_get_inode(kv,ino,&row)==EFS_OK);
    struct efs_meta_pub p=intent(ino,row.generation,10);int v;
    assert(efs_meta_apply_publish(kv,&p)==EFS_ERR_BUSY);
    assert(efs_meta_publication_result(kv,&p,&v)==EFS_ERR_NOT_FOUND);
    uint32_t shard=efs_kv_lane_shard(ino,0);const uint8_t *uuid=p.publication_id.client_uuid;
    assert(efs_session_create(kv,uuid,1)==EFS_OK);
    assert(efs_session_register(kv,uuid,1,shard)==EFS_OK);
    assert(efs_meta_apply_publish(kv,&p)==EFS_ERR_BUSY); /* REGISTER alone is not admission */
    assert(efs_session_establish(kv,shard,uuid,1)==EFS_OK);
    assert(efs_meta_apply_publish(kv,&p)==EFS_OK);
    assert(efs_session_begin_fence(kv,uuid)==EFS_OK);
    assert(efs_session_barrier_done(kv,uuid,1)==EFS_ERR_BUSY);
    struct efs_meta_pub new_epoch=p;new_epoch.publication_id.session_epoch=2;
    assert(efs_meta_apply_publish(kv,&new_epoch)==EFS_ERR_BUSY);
    assert(efs_session_accept(kv,shard,uuid,1)==EFS_OK); /* admitted before ordered local fence */
    assert(efs_session_fence_local(kv,shard,uuid,2)==EFS_OK);
    fenced(kv,p);
    assert(efs_session_barrier_done(kv,uuid,1)==EFS_ERR_BUSY); /* no cleanup before global ACK */
    assert(efs_session_reclaimable(kv,shard,uuid,1)==EFS_ERR_BUSY);
    if (!finish) return p; /* abrupt exit before the session-shard ACK */
    assert(efs_session_ack_fence(kv,uuid,shard)==EFS_OK);
    assert(efs_session_finish_fence(kv,uuid)==EFS_OK);
    assert(efs_session_barrier_done(kv,uuid,1)==EFS_OK);
    assert(efs_session_reclaimable(kv,shard,uuid,1)==EFS_OK);
    assert(efs_session_reclaimable(kv,(shard+1)%EFS_SESSION_BITS,uuid,1)==EFS_ERR_BUSY);
    assert(efs_session_reclaimable(kv,shard,uuid,2)==EFS_ERR_BUSY);
    return p;
}
static void next_epoch(struct efs_kv *kv,struct efs_meta_pub old)
{
    uint32_t shard=efs_kv_lane_shard(old.ino,0);const uint8_t *uuid=old.publication_id.client_uuid;
    assert(efs_session_register(kv,uuid,2,shard)==EFS_OK);
    assert(efs_session_establish(kv,shard,uuid,2)==EFS_OK);
    struct efs_meta_pub p=old;p.publication_id.session_epoch=2;
    p.expected_gen=10;p.candidate_gen=20;p.publication_id.seq=1;
    assert(efs_meta_apply_publish(kv,&p)==EFS_OK);
    assert(efs_meta_apply_publication_retire(kv,&p)==EFS_OK);
    assert(efs_meta_apply_publication_retire(kv,&old)==EFS_ERR_STALE);
    assert(efs_meta_apply_check(kv)==EFS_OK);
}
int main(void)
{
    struct efs_kv *kv=efs_kv_mem_create();assert(kv);
    struct efs_meta_pub p=setup(kv,1);fenced(kv,p);next_epoch(kv,p);efs_kv_mem_free(kv);
    char dir[]="/tmp/efs-publication-session-XXXXXX";assert(mkdtemp(dir));
    pid_t child=fork();assert(child>=0);
    if(!child) {kv=efs_kv_lsm_open(dir,NULL);assert(kv);setup(kv,0);_exit(0);}
    int status;assert(waitpid(child,&status,0)==child && WIFEXITED(status) && !WEXITSTATUS(status));
    kv=efs_kv_lsm_open(dir,NULL);assert(kv);
    struct efs_meta_dentry dent;struct efs_meta_row row;
    assert(efs_meta_apply_lookup(kv,EFS_ROOT_INO,"session",&dent)==EFS_OK);
    assert(efs_meta_apply_get_inode(kv,dent.ino,&row)==EFS_OK);
    p=intent(dent.ino,row.generation,10);fenced(kv,p);
    uint32_t shard=efs_kv_lane_shard(p.ino,0);
    assert(efs_session_reclaimable(kv,shard,p.publication_id.client_uuid,1)==EFS_ERR_BUSY);
    assert(efs_session_ack_fence(kv,p.publication_id.client_uuid,shard)==EFS_OK);
    assert(efs_session_finish_fence(kv,p.publication_id.client_uuid)==EFS_OK);
    assert(efs_session_reclaimable(kv,shard,p.publication_id.client_uuid,1)==EFS_OK);
    next_epoch(kv,p);efs_kv_lsm_close(kv);
    DIR *d=opendir(dir);assert(d);struct dirent *e;
    while((e=readdir(d))) {if(e->d_name[0]=='.')continue;char path[512];snprintf(path,sizeof(path),"%s/%s",dir,e->d_name);assert(!unlink(path));}
    closedir(d);assert(!rmdir(dir));
    puts("publication sessions: missing admission, ordered fence, retained receipts, barrier and crash recovery PASS");
    return 0;
}
