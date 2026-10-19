/* Real durable KV batches, abrupt process exit and cold segment/WAL recovery.
 * Private test keys only: no production key kind or Raft/RPC is activated. */
#include "efs/pack_ledger.h"
#include "efs/kv_lsm.h"
#include <assert.h>
#include <ftw.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static struct efs_pack_ref refs[16];
static uint8_t ledger_key[]={0,1,250,1}, receipt_key[]={0,1,250,2};
static uint8_t map_keys[16][4];
static uint8_t wire[EFS_PACK_LEDGER_MAX_BYTES];
static void save(struct efs_kv *kv,const struct efs_pack_ledger *ledger,
    const struct efs_kv_item *extra,unsigned count)
{
    assert(count<=17);struct efs_kv_item batch[18];size_t bytes;
    assert(!efs_pack_ledger_encode(ledger,wire,sizeof(wire),&bytes));
    batch[0]=(struct efs_kv_item){EFS_KV_PUT,ledger_key,sizeof(ledger_key),wire,(uint32_t)bytes};
    if(count)memcpy(batch+1,extra,count*sizeof(*extra));
    assert(!efs_kv_batch(kv,batch,count+1));
}
static struct efs_pack_ledger load(struct efs_kv *kv)
{
    struct efs_pack_ledger ledger;uint32_t bytes=sizeof(wire);
    assert(!efs_kv_get(kv,ledger_key,sizeof(ledger_key),wire,&bytes));
    assert(!efs_pack_ledger_decode(wire,bytes,&ledger));return ledger;
}
static void crash_wait(pid_t pid)
{
    int status;assert(pid>0 && waitpid(pid,&status,0)==pid);
    assert(WIFEXITED(status) && !WEXITSTATUS(status));
}
static int cleanup(const char *path,const struct stat *st,int kind,struct FTW *f)
{(void)st;(void)f;return kind==FTW_DP?rmdir(path):unlink(path);}
int main(void)
{
    char dir[]="/tmp/efs-pack-ledger-recovery-XXXXXX";assert(mkdtemp(dir));
    uint8_t image[EFS_SEGMENT_MAX_SIZE],payload[512];memset(payload,0x41,sizeof(payload));
    struct efs_pack_record records[16];
    for(unsigned i=0;i<16;i++) {
        records[i]=(struct efs_pack_record){.ino=100+i,.inode_generation=7,
            .record_generation=9+i,.content_epoch=11,.len=512,.data=payload};
        map_keys[i][1]=1;map_keys[i][2]=251;map_keys[i][3]=(uint8_t)i;
    }
    size_t geometry;assert(!efs_pack_build(23,29,2,records,16,image,sizeof(image),&geometry,refs));
    struct efs_opid owner={.client_uuid={13},.session_epoch=17,.seq=19};
    struct efs_pack_ledger ledger;
    assert(!efs_pack_ledger_init(image,geometry,refs,16,&owner,15,0,&ledger));
    pid_t pid=fork();assert(pid>=0);
    if(!pid) {
        struct efs_kv *kv=efs_kv_lsm_open(dir,NULL);assert(kv);
        save(kv,&ledger,NULL,0);
        struct efs_kv_item items[17];uint8_t values[16][EFS_PACK_REF_BYTES],receipt[]={1};
        for(unsigned i=0;i<16;i++) {
            assert(!efs_pack_ledger_publish(&ledger,&refs[i]));
            assert(!efs_pack_ref_encode(&refs[i],values[i]));
            items[i]=(struct efs_kv_item){EFS_KV_PUT,map_keys[i],4,values[i],sizeof(values[i])};
        }
        items[16]=(struct efs_kv_item){EFS_KV_PUT,receipt_key,4,receipt,sizeof(receipt)};
        save(kv,&ledger,items,17);_exit(0); /* ACKed WAL, no close/flush */
    }
    crash_wait(pid);struct efs_kv *kv=efs_kv_lsm_open(dir,NULL);assert(kv);ledger=load(kv);
    uint8_t value[EFS_PACK_REF_BYTES];uint32_t bytes;
    for(unsigned i=0;i<16;i++) {
        assert(ledger.state[i]==EFS_PACK_LIVE);bytes=sizeof(value);
        assert(!efs_kv_get(kv,map_keys[i],4,value,&bytes));struct efs_pack_ref actual;
        assert(!efs_pack_ref_decode(value,bytes,&actual));uint8_t expected[EFS_PACK_REF_BYTES];
        assert(!efs_pack_ref_encode(&refs[i],expected));assert(!memcmp(expected,value,bytes));
    }
    bytes=sizeof(value);assert(!efs_kv_get(kv,receipt_key,4,value,&bytes) && bytes==1 && value[0]==1);
    struct efs_kv_item deleted[15];
    for(unsigned i=0;i<15;i++) {
        assert(!efs_pack_ledger_release(&ledger,&refs[i]));
        deleted[i]=(struct efs_kv_item){EFS_KV_DEL,map_keys[i],4,NULL,0};
    }
    save(kv,&ledger,deleted,15);assert(!efs_kv_lsm_flush(kv));assert(!efs_kv_lsm_compact(kv));
    efs_kv_lsm_close(kv);kv=efs_kv_lsm_open(dir,NULL);assert(kv);ledger=load(kv);
    assert(efs_pack_ledger_reclaim_begin(&ledger)==EFS_ERR_BUSY);
    for(unsigned i=0;i<15;i++) {
        bytes=sizeof(value);assert(efs_kv_get(kv,map_keys[i],4,value,&bytes)==EFS_ERR_NOT_FOUND);
        assert(efs_pack_ledger_publish(&ledger,&refs[i])==EFS_ERR_STALE);
    }
    bytes=sizeof(value);assert(!efs_kv_get(kv,map_keys[15],4,value,&bytes));
    assert(!efs_pack_ledger_release(&ledger,&refs[15]));assert(!efs_pack_ledger_reclaim_begin(&ledger));
    struct efs_kv_item last={EFS_KV_DEL,map_keys[15],4,NULL,0};save(kv,&ledger,&last,1);
    assert(!efs_pack_ledger_fence_ack(&ledger,1));assert(!efs_pack_ledger_fence_ack(&ledger,2));
    save(kv,&ledger,NULL,0);efs_kv_lsm_close(kv);
    pid=fork();assert(pid>=0);
    if(!pid) {
        kv=efs_kv_lsm_open(dir,NULL);assert(kv);ledger=load(kv);
        assert(efs_pack_ledger_delete_ack(&ledger,1)==EFS_ERR_BUSY);
        assert(!efs_pack_ledger_fence_ack(&ledger,4));assert(!efs_pack_ledger_fence_ack(&ledger,8));
        assert(!efs_pack_ledger_delete_ack(&ledger,1));save(kv,&ledger,NULL,0);_exit(0);
    }
    crash_wait(pid);kv=efs_kv_lsm_open(dir,NULL);assert(kv);ledger=load(kv);
    assert(ledger.phase==EFS_PACK_RECLAIMING && ledger.fence_acks==15 && ledger.delete_acks==1);
    assert(!efs_pack_ledger_delete_ack(&ledger,1)); /* lost ACK retry */
    assert(!efs_pack_ledger_delete_ack(&ledger,2));assert(!efs_pack_ledger_delete_ack(&ledger,4));
    save(kv,&ledger,NULL,0);efs_kv_lsm_close(kv);
    pid=fork();assert(pid>=0);
    if(!pid) {
        kv=efs_kv_lsm_open(dir,NULL);assert(kv);ledger=load(kv);
        assert(!efs_pack_ledger_delete_ack(&ledger,8));save(kv,&ledger,NULL,0);_exit(0);
    }
    crash_wait(pid);kv=efs_kv_lsm_open(dir,NULL);assert(kv);ledger=load(kv);
    assert(ledger.phase==EFS_PACK_RETIRED && ledger.delete_acks==15);
    assert(!efs_pack_ledger_delete_ack(&ledger,8));
    for(unsigned i=0;i<16;i++)assert(efs_pack_ledger_publish(&ledger,&refs[i])==EFS_ERR_STALE);
    efs_kv_lsm_close(kv);assert(!nftw(dir,cleanup,32,FTW_DEPTH|FTW_PHYS));
    puts("pack ledger: durable mapping cohort, abrupt WAL recovery, cold compaction, neighbor retention and GC ACK replay PASS");
}
