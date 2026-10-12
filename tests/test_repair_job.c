/* Production journal/apply + real synced LSM. No target repair/protection
 * claim: physical writes, target receipt verification and GC integration are
 * deliberately absent in this inactive first-stage prerequisite. */
#include "efs/repair_job.h"
#include "efs/meta_cmd.h"
#include "efs/kv_lsm.h"
#include "efs/raft.h"
#include "kv_lsm_internal.h"
#include <assert.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
static struct efs_meta_pub pub(struct efs_kv *kv,efs_ino_t ino,uint64_t object,uint64_t expected)
{
    struct efs_meta_row row;assert(!efs_meta_apply_get_inode(kv,ino,&row));
    struct efs_meta_pub p={0};p.ino=ino;p.expected_gen=expected;p.candidate_gen=object;
    p.new_size=131072;p.now=object;p.content_epoch=row.content_epoch;
    p.coding_profile_id=EFS_META_PROFILE_K2F1;
    for(unsigned i=0;i<3;i++){p.ch.nodes[i]=i+1;memset(p.ch.checksums[i],11+i,32);}
    return p;
}
static efs_ino_t create(struct efs_kv *kv)
{
    struct efs_meta_attrs at={0,0,1};efs_ino_t ino;
    assert(!efs_meta_apply_init(kv,1));
    assert(!efs_meta_apply_create_file(kv,&at,EFS_ROOT_INO,S_IFREG|0644,"repair",&ino));
    struct efs_meta_pub p=pub(kv,ino,101,0);assert(!efs_meta_apply_publish(kv,&p));return ino;
}
static struct efs_repair_job capture(struct efs_kv *kv,efs_ino_t ino,uint64_t object,unsigned uuid)
{
    struct efs_meta_row row;assert(!efs_meta_apply_get_inode(kv,ino,&row));
    struct efs_repair_job q={0},j;q.uuid[0]=uuid;q.ino=ino;q.inode_gen=row.generation;
    q.target_store[0]=7;q.object=object;q.fragment_len=65536;q.epoch=1;q.owner=1;
    assert(!efs_repair_capture(kv,&q,&j));return j;
}
static void fail(struct efs_kv *kv,int lsm)
{
    if(lsm){struct kv_lsm *l=kv->ctx;pthread_mutex_lock(&l->mu);l->fail_next_batch=1;pthread_mutex_unlock(&l->mu);}
    else assert(!efs_kv_mem_fail_next_batch(kv));
}
static int command(struct efs_kv *kv,const struct efs_repair_job *j,unsigned op,unsigned arg,unsigned group)
{
    uint8_t cmd[EFS_REPAIR_COMMAND_BYTES]={EFS_MD_CMD_REPAIR_JOB,op};
    assert(!efs_repair_job_encode(j,cmd+2));
    for(unsigned i=0;i<4;i++)cmd[2+EFS_REPAIR_JOB_BYTES+i]=(uint8_t)(arg>>(24-8*i));
    return efs_repair_apply_command(kv,group,cmd,sizeof(cmd));
}
static int term_command(struct efs_kv *kv,const struct efs_repair_job *j,unsigned op,unsigned group,uint64_t term)
{
    uint8_t cmd[EFS_REPAIR_COMMAND_BYTES]={EFS_MD_CMD_REPAIR_JOB,op};
    assert(!efs_repair_job_encode(j,cmd+2));
    return efs_repair_apply_command_term(kv,group,term,cmd,sizeof(cmd));
}
static void journal(struct efs_kv *kv,int lsm)
{
    efs_ino_t ino=create(kv);struct efs_repair_job j=capture(kv,ino,101,17),got;
    unsigned group=efs_raft_shard_group(efs_kv_lane_shard(ino,0));
    uint8_t wire[EFS_REPAIR_JOB_BYTES];assert(!efs_repair_job_encode(&j,wire));
    assert(!efs_repair_job_decode(wire,sizeof(wire),&got));
    assert(efs_repair_job_decode(wire,244,&got)==EFS_ERR_PROTO); /* legacy v1 never reinterpreted */
    wire[3]='1';assert(efs_repair_job_decode(wire,sizeof(wire),&got)==EFS_ERR_PROTO);wire[3]='2';
    wire[44]^=1;
    assert(efs_repair_job_decode(wire,sizeof(wire),&got)==EFS_ERR_PROTO);
    assert(command(kv,&j,EFS_REPAIR_OP_ADMIT,0,group^2)==EFS_ERR_INVAL);
    /* GC-scheduled object still visible in source row: ADMIT must refuse.
     * This is the scan->ADMIT->delete race boundary, serialized at Raft apply. */
    uint8_t gk[EFS_KV_KEY_MAX],gv[EFS_META_GC_VAL]={0};uint32_t gkl=0;
    assert(!efs_kv_key_gc(efs_kv_anchor_shard(efs_kv_lane_shard(ino,0)),ino,101,0,0,gk,&gkl));
    assert(!efs_kv_put(kv,gk,gkl,gv,sizeof(gv)));
    assert(efs_repair_admit(kv,&j)==EFS_ERR_STALE);
    assert(efs_repair_get(kv,&j,&got)==EFS_ERR_NOT_FOUND);
    assert(!efs_kv_del(kv,gk,gkl));
    assert(term_command(kv,&j,EFS_REPAIR_OP_ADMIT,group,0)==EFS_ERR_STALE);
    assert(term_command(kv,&j,EFS_REPAIR_OP_ADMIT,group,2)==EFS_ERR_STALE);
    assert(term_command(kv,&j,EFS_REPAIR_OP_ADMIT,group,UINT64_MAX)==EFS_ERR_STALE);
    fail(kv,lsm);assert(command(kv,&j,EFS_REPAIR_OP_ADMIT,0,group)==EFS_ERR_IO);
    assert(efs_repair_get(kv,&j,&got)==EFS_ERR_NOT_FOUND);
    assert(!command(kv,&j,EFS_REPAIR_OP_ADMIT,0,group));assert(!efs_repair_admit(kv,&j));
    assert(!efs_repair_write_authority(kv,&j));
    assert(!efs_kv_put(kv,gk,gkl,gv,sizeof(gv)));
    assert(efs_repair_write_authority(kv,&j)==EFS_ERR_STALE);
    assert(!efs_kv_del(kv,gk,gkl));
    struct efs_repair_job conflict=j;conflict.sums[0][0]^=1;
    assert(efs_repair_admit(kv,&conflict)==EFS_ERR_STALE);
    conflict=j;conflict.owner=2;assert(efs_repair_admit(kv,&conflict)==EFS_ERR_STALE);
    conflict=j;conflict.target_store[0]^=1;assert(efs_repair_admit(kv,&conflict)==EFS_ERR_STALE);
    assert(efs_repair_write_authority(kv,&conflict)==EFS_ERR_STALE);
    conflict=j;conflict.uuid[0]=18;assert(efs_repair_admit(kv,&conflict)==EFS_ERR_BUSY);
    assert(efs_repair_gc_ready(kv,ino,j.inode_gen,0,101)==EFS_ERR_BUSY);
    assert(efs_repair_gc_object_ready(kv,ino,0,101)==EFS_ERR_BUSY);
    assert(efs_repair_retire(kv,&j,0)==EFS_ERR_BUSY);
    assert(efs_repair_record_seal(kv,&j,1)==EFS_ERR_STALE);
    assert(term_command(kv,&j,EFS_REPAIR_OP_DATA,group,2)==EFS_ERR_STALE);
    assert(!efs_repair_get(kv,&j,&got) && !got.flags);
    fail(kv,lsm);assert(efs_repair_record_data(kv,&j)==EFS_ERR_IO);
    assert(!efs_repair_get(kv,&j,&got) && !got.flags);
    assert(!efs_repair_record_data(kv,&j));
    fail(kv,lsm);assert(efs_repair_record_seal(kv,&j,2)==EFS_ERR_IO);
    assert(efs_repair_gc_ready(kv,ino,j.inode_gen,0,101)==EFS_ERR_BUSY);
    assert(!efs_repair_record_seal(kv,&j,2));assert(efs_repair_write_authority(kv,&j)==EFS_ERR_STALE);assert(!efs_repair_gc_object_ready(kv,ino,0,101));assert(!efs_repair_gc_ready(kv,ino,j.inode_gen,0,101));
    fail(kv,lsm);assert(efs_repair_retire(kv,&j,0)==EFS_ERR_IO);
    assert(!efs_repair_get(kv,&j,&got) && got.state==EFS_REPAIR_SEALED);
    assert(!efs_repair_retire(kv,&j,0));assert(!efs_repair_retire(kv,&j,0));
    assert(efs_repair_retire(kv,&j,1)==EFS_ERR_STALE);
    assert(efs_repair_admit(kv,&j)==EFS_ERR_STALE);
    assert(efs_repair_admit(kv,&conflict)==EFS_OK); /* fresh UUID, same live object */
    assert(!term_command(kv,&conflict,EFS_REPAIR_OP_CANCEL,group,2));assert(efs_repair_retire(kv,&conflict,1)==EFS_ERR_BUSY);
    assert(efs_repair_gc_ready(kv,ino,j.inode_gen,0,101)==EFS_ERR_BUSY);
    assert(!efs_repair_record_seal(kv,&conflict,2));assert(!efs_repair_retire(kv,&conflict,1));
    assert(!efs_repair_retire(kv,&conflict,1));assert(efs_repair_admit(kv,&conflict)==EFS_ERR_STALE);
    /* Exact interleaving: capture, concurrent publication applied, admission. */
    struct efs_repair_job stale=capture(kv,ino,101,19);
    struct efs_meta_pub p=pub(kv,ino,102,101);assert(!efs_meta_apply_publish(kv,&p));
    assert(efs_repair_admit(kv,&stale)==EFS_ERR_STALE);
    /* Pending debt survives source disappearance; completion cannot pretend
     * the newer row is the repaired row. Cancellation still needs target seal. */
    j=capture(kv,ino,102,20);assert(!efs_repair_admit(kv,&j));
    p=pub(kv,ino,103,102);assert(!efs_meta_apply_publish(kv,&p));
    assert(efs_repair_record_data(kv,&j)==EFS_ERR_STALE);
    assert(!efs_repair_cancel(kv,&j));assert(!efs_repair_record_seal(kv,&j,2));
    assert(!efs_repair_retire(kv,&j,1));
    j=capture(kv,ino,103,21);assert(!efs_repair_admit(kv,&j));
    assert(!efs_meta_apply_lane_fence(kv,ino,j.inode_gen,0,2,0,0,0));
    int fenced_rc=efs_repair_record_data(kv,&j);fprintf(stderr,"fenced source rc=%d\n",fenced_rc);assert(fenced_rc==EFS_ERR_STALE);
    assert(!efs_repair_cancel(kv,&j));assert(!efs_repair_record_seal(kv,&j,2));assert(!efs_repair_retire(kv,&j,1));
    printf("repair journal %s: exact replay/conflict, failed batches, floor and GC dependency PASS\n",lsm?"synced-LSM":"mem");
}
static void bounded(void)
{
    struct efs_kv *kv=efs_kv_mem_create();assert(kv);efs_ino_t ino=create(kv);
    struct efs_meta_row row;assert(!efs_meta_apply_get_inode(kv,ino,&row));
    struct efs_repair_job first={0},last={0};
    for(unsigned i=0;i<=EFS_REPAIR_PENDING_MAX;i++){
        if(i){struct efs_meta_pub p=pub(kv,ino,101+i,0);p.chunk_index=i*64;p.new_size=((uint64_t)p.chunk_index+1)*131072;assert(!efs_meta_apply_publish(kv,&p));}
        struct efs_repair_job q={0},j;q.uuid[0]=100;q.uuid[1]=i;q.uuid[2]=i>>8;
        q.target_store[0]=7;q.ino=ino;q.inode_gen=row.generation;q.ci=i*64;q.object=101+i;q.fragment_len=65536;q.epoch=1;q.owner=1;
        assert(!efs_repair_capture(kv,&q,&j));int rc=efs_repair_admit(kv,&j);
        assert(rc==(i==EFS_REPAIR_PENDING_MAX?EFS_ERR_BUSY:EFS_OK));
        if(!i)first=j;
        last=j;
    }
    assert(!efs_repair_cancel(kv,&first));assert(!efs_repair_record_seal(kv,&first,2));assert(!efs_repair_retire(kv,&first,1));
    assert(!efs_repair_admit(kv,&last));
    efs_kv_mem_free(kv);puts("repair journal bounded: exact128 pending jobs;129th BUSY; durable retirement releases admission slot PASS");
}
static void cold(void)
{
    char path[]="/tmp/efs-repair-journal-XXXXXX";assert(mkdtemp(path));
    struct efs_kv *kv=efs_kv_lsm_open(path,NULL);assert(kv);efs_ino_t ino=create(kv);
    struct efs_repair_job j=capture(kv,ino,101,80);efs_kv_lsm_close(kv);
    pid_t pid=fork();assert(pid>=0);
    if(!pid){kv=efs_kv_lsm_open(path,NULL);assert(kv);assert(!efs_repair_admit(kv,&j));_exit(0);}
    int status;assert(waitpid(pid,&status,0)==pid && WIFEXITED(status) && !WEXITSTATUS(status));
    kv=efs_kv_lsm_open(path,NULL);assert(kv);struct efs_repair_job got;
    assert(!efs_repair_get(kv,&j,&got));assert(efs_repair_gc_ready(kv,ino,j.inode_gen,0,101)==EFS_ERR_BUSY);
    assert(!efs_repair_cancel(kv,&j));assert(!efs_repair_record_seal(kv,&j,2));
    assert(!efs_repair_retire(kv,&j,1));assert(!efs_kv_lsm_flush(kv));assert(!efs_kv_lsm_compact(kv));efs_kv_lsm_close(kv);
    kv=efs_kv_lsm_open(path,NULL);assert(kv);assert(efs_repair_admit(kv,&j)==EFS_ERR_STALE);
    assert(!efs_repair_retire(kv,&j,1));efs_kv_lsm_close(kv);
    printf("repair journal cold: admitted debt after abrupt process exit; terminal floor after flush/compaction/reopen PASS\n");
}
static void fenced_liveness(void)
{
    struct efs_kv *kv=efs_kv_mem_create();assert(kv);efs_ino_t ino=create(kv);
    struct efs_meta_pub p=pub(kv,ino,102,101);p.delta_off=4096;p.delta_len=1024;
    assert(!efs_meta_apply_publish(kv,&p));
    struct efs_meta_row row;assert(!efs_meta_apply_get_inode(kv,ino,&row));
    struct efs_repair_job q={.ino=ino,.inode_gen=row.generation,.object=102,.fragment_len=65536,.span_off=4096,.span_len=1024,.epoch=1,.owner=1},j;
    q.uuid[0]=92;q.target_store[0]=7;assert(!efs_repair_capture(kv,&q,&j));q=j;assert(!efs_repair_admit(kv,&j));
    assert(!efs_meta_apply_content_fence(kv,ino,row.generation,0,0,1,1000,103));
    struct efs_meta_chunk_view view;assert(!efs_meta_get_chunk_view(kv,ino,row.generation,0,131072,&view));
    assert(view.ndelta==1 && view.deltas[0].generation==102 && view.bytes.parts[1].len==0 && view.bytes.parts[0].len==1000);
    uint8_t gk[EFS_KV_KEY_MAX],gv[EFS_META_GC_VAL];uint32_t kl=0,n=sizeof(gv);
    assert(!efs_kv_key_gc(efs_kv_anchor_shard(efs_kv_lane_shard(ino,0)),ino,102,0,0,gk,&kl));assert(efs_kv_get(kv,gk,kl,gv,&n)==EFS_ERR_NOT_FOUND);
    assert(efs_repair_capture(kv,&q,&j)==EFS_ERR_STALE);
    q.uuid[0]=93;assert(efs_repair_admit(kv,&q)==EFS_ERR_STALE);
    /* Partial live base is eligible; wholly fenced retained raw base is not. */
    q.object=101;q.span_off=q.span_len=0;assert(!efs_repair_capture(kv,&q,&j));
    assert(!efs_meta_apply_content_fence(kv,ino,row.generation,0,1,2,0,104));
    assert(!efs_meta_get_chunk_view(kv,ino,row.generation,0,131072,&view));assert(view.base.generation==101 && !view.bytes.parts[0].len);
    assert(efs_repair_capture(kv,&q,&j)==EFS_ERR_STALE);efs_kv_mem_free(kv);
    puts("repair selector clipped liveness: retained fully fenced span/base refused without GC key; partial live base allowed PASS");
}
static void sync_faults(void)
{
    const char *marker=getenv("EFS_REPAIR_SYNC_MARKER");if(!marker)return;
    for(unsigned retirement=0;retirement<2;retirement++){
        char path[]="/tmp/efs-repair-sync-XXXXXX";assert(mkdtemp(path));
        struct efs_kv *kv=efs_kv_lsm_open(path,NULL);assert(kv);efs_ino_t ino=create(kv);
        struct efs_repair_job j=capture(kv,ino,101,91);
        if(retirement){assert(!efs_repair_admit(kv,&j));assert(!efs_repair_cancel(kv,&j));assert(!efs_repair_record_seal(kv,&j,2));}
        efs_kv_lsm_close(kv);
        pid_t pid=fork();assert(pid>=0);
        if(!pid){
            kv=efs_kv_lsm_open(path,NULL);assert(kv);int fd=open(marker,O_WRONLY|O_CREAT|O_EXCL,0600);assert(fd>=0);assert(!close(fd));
            int rc=retirement?efs_repair_retire(kv,&j,1):efs_repair_admit(kv,&j);
            assert(rc==EFS_ERR_IO);_exit(0); /* poison/crash, no graceful WAL repair */
        }
        int status;assert(waitpid(pid,&status,0)==pid && WIFEXITED(status) && !WEXITSTATUS(status));assert(!unlink(marker));
        kv=efs_kv_lsm_open(path,NULL);assert(kv);struct efs_repair_job got;
        /* EIO is an ambiguous durability outcome: replay may retain the atomic
         * old state OR the complete new state, never a lost-debt/no-floor gap. */
        int recovered=efs_repair_get(kv,&j,&got);
        if(retirement){
            if(recovered==EFS_OK)assert(got.state==EFS_REPAIR_SEALED);
            else {assert(recovered==EFS_ERR_NOT_FOUND);assert(efs_repair_admit(kv,&j)==EFS_ERR_STALE);}
            assert(!efs_repair_retire(kv,&j,1));assert(efs_repair_admit(kv,&j)==EFS_ERR_STALE);
        } else {
            assert(recovered==EFS_OK || recovered==EFS_ERR_NOT_FOUND);
            if(recovered==EFS_OK)assert(got.state==EFS_REPAIR_ADMITTED);
            assert(!efs_repair_admit(kv,&j));
            assert(efs_repair_gc_ready(kv,ino,j.inode_gen,0,101)==EFS_ERR_BUSY);
        }
        efs_kv_lsm_close(kv);printf("repair WAL sync EIO %s: failed mutation not acknowledged, atomic debt/floor outcome recovered PASS\n",retirement?"retirement":"admission");
    }
}
int main(void)
{
    struct efs_kv *kv=efs_kv_mem_create();assert(kv);journal(kv,0);efs_kv_mem_free(kv);
    char path[]="/tmp/efs-repair-apply-XXXXXX";assert(mkdtemp(path));
    kv=efs_kv_lsm_open(path,NULL);assert(kv);journal(kv,1);efs_kv_lsm_close(kv);cold();bounded();fenced_liveness();sync_faults();
    puts("inactive repair-journal prerequisite PASS; physical repair and distributed authority NOTRUN");return 0;
}
