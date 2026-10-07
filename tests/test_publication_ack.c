#include "efs/publication_ack.h"
#include "efs/session.h"
#include "efs/kv_key.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
static struct efs_msg_publication request(uint64_t seq)
{
    struct efs_msg_publication r={0};
    r.id.client_uuid[0]=1;r.id.session_epoch=1;r.id.seq=seq;
    r.rec.ino=123;r.rec.file_generation=456;r.rec.chunk_generation=seq;
    r.rec.publish_flags=EFS_CHUNK_REC_F_CAPTURED_FILEID|EFS_CHUNK_REC_F_CAPTURED_EPOCH;
    r.size=64;
    for(unsigned i=0;i<EFS_NUM_FRAGMENTS;i++) r.rec.nodes[i]=i+1;
    return r;
}
static struct efs_meta_pub intent(const struct efs_msg_publication *r)
{
    struct efs_meta_pub p;
    assert(efs_publication_from_rec(&r->rec,r->size,&p)==EFS_OK);
    p.publication_id=r->id;p.durable_result=1;return p;
}
static struct efs_msg_publication_reply reply(const struct efs_msg_publication *r,
                                              unsigned state,int verdict)
{
    struct efs_msg_publication_reply out={0};out.state=state;out.verdict=verdict;
    struct efs_meta_pub p=intent(r);
    assert(efs_publication_digest(&p,out.digest)==EFS_OK);return out;
}
static void ownership(void)
{
    struct efs_publication_ack_queue q={0},saved;
    struct efs_msg_publication a=request(10),b=request(11),out;
    struct efs_msg_publication invalid=a;invalid.id.seq=0;
    saved=q;assert(efs_publication_ack_admit(&q,&invalid)==EFS_ERR_INVAL);
    assert(!memcmp(&q,&saved,sizeof(q)));
    assert(efs_publication_ack_admit(&q,&a)==EFS_OK);
    assert(efs_publication_ack_admit(&q,&b)==EFS_OK);
    saved=q;
    assert(efs_publication_ack_admit(&q,&a)==EFS_OK && !memcmp(&q,&saved,sizeof(q)));
    struct efs_msg_publication changed=a;changed.size++;
    assert(efs_publication_ack_admit(&q,&changed)==EFS_ERR_PROTO);
    changed=request(9);assert(efs_publication_ack_admit(&q,&changed)==EFS_ERR_STALE);
    changed=b;changed.id.session_epoch++;assert(efs_publication_ack_admit(&q,&changed)==EFS_ERR_STALE);
    changed=b;changed.rec.chunk_index++;assert(efs_publication_ack_admit(&q,&changed)==EFS_ERR_STALE);
    changed=b;changed.id.client_uuid[1]=1;
    assert(efs_publication_ack_admit(&q,&changed)==EFS_ERR_STALE);
    changed=b;changed.rec.file_generation++;
    assert(efs_publication_ack_admit(&q,&changed)==EFS_ERR_STALE);
    assert(!memcmp(&q,&saved,sizeof(q)));
    struct efs_msg_publication_reply rb=reply(&b,EFS_PUBLICATION_COMMITTED,EFS_OK);
    assert(efs_publication_ack_consumed(&q,11,&rb)==EFS_OK);
    memset(&out,0xa5,sizeof(out));struct efs_msg_publication untouched=out;
    assert(efs_publication_ack_next(&q,&out)==EFS_ERR_BUSY && !memcmp(&out,&untouched,sizeof(out)));
    rb.state=EFS_PUBLICATION_RETIRED;rb.verdict=EFS_META_PUBLICATION_RETIRED;
    assert(efs_publication_ack_complete(&q,11,&rb)==EFS_ERR_STALE);
    struct efs_msg_publication_reply ra=reply(&a,EFS_PUBLICATION_UNKNOWN,EFS_OK);
    assert(efs_publication_ack_consumed(&q,10,&ra)==EFS_ERR_BUSY);
    ra=reply(&a,EFS_PUBLICATION_RETIRED,EFS_META_PUBLICATION_RETIRED);
    assert(efs_publication_ack_consumed(&q,10,&ra)==EFS_ERR_BUSY);
    ra=reply(&a,EFS_PUBLICATION_REJECTED,EFS_ERR_STALE);ra.digest[0]^=1;
    assert(efs_publication_ack_consumed(&q,10,&ra)==EFS_ERR_PROTO);ra.digest[0]^=1;
    assert(efs_publication_ack_consumed(&q,10,&ra)==EFS_OK);
    assert(efs_publication_ack_consumed(&q,10,&ra)==EFS_OK);
    struct efs_msg_publication_reply conflict=reply(&a,EFS_PUBLICATION_COMMITTED,EFS_OK);
    assert(efs_publication_ack_consumed(&q,10,&conflict)==EFS_ERR_PROTO);
    assert(efs_publication_ack_next(&q,&out)==EFS_OK && !memcmp(&out,&a,sizeof(a)));
    saved=q;ra.state=EFS_PUBLICATION_RETIRED;ra.verdict=EFS_META_PUBLICATION_RETIRED;
    ra.rpc.status=EFS_INODE_RPC_BUSY;
    assert(efs_publication_ack_complete(&q,10,&ra)==EFS_ERR_BUSY && !memcmp(&q,&saved,sizeof(q)));
    ra.rpc.status=EFS_INODE_RPC_OK;ra.digest[0]^=1;
    assert(efs_publication_ack_complete(&q,10,&ra)==EFS_ERR_PROTO && !memcmp(&q,&saved,sizeof(q)));
    ra.digest[0]^=1;assert(efs_publication_ack_complete(&q,10,&ra)==EFS_OK);
    assert(efs_publication_ack_complete(&q,10,&ra)==EFS_ERR_STALE);
    assert(efs_publication_ack_next(&q,&out)==EFS_OK && out.id.seq==11);
    assert(efs_publication_ack_complete(&q,11,&rb)==EFS_OK && !q.count);
    assert(efs_publication_ack_admit(&q,&a)==EFS_ERR_STALE);
    out=untouched;assert(efs_publication_ack_next(&q,&out)==EFS_ERR_NOT_FOUND);
    assert(!memcmp(&out,&untouched,sizeof(out)));
}
static void bounded_wrap(void)
{
    struct efs_publication_ack_queue q={0};
    for(unsigned pass=0;pass<20;pass++) {
        for(unsigned i=0;i<EFS_META_PUBLICATION_MAX_RECEIPTS;i++) {
            struct efs_msg_publication r=request(1+pass*64+i);
            assert(efs_publication_ack_admit(&q,&r)==EFS_OK);
            struct efs_msg_publication_reply terminal=reply(&r,EFS_PUBLICATION_COMMITTED,EFS_OK);
            assert(efs_publication_ack_consumed(&q,r.id.seq,&terminal)==EFS_OK);
        }
        struct efs_publication_ack_queue saved=q;
        struct efs_msg_publication excess=request(1+(pass+1)*64);
        assert(efs_publication_ack_admit(&q,&excess)==EFS_ERR_BUSY && !memcmp(&q,&saved,sizeof(q)));
        for(unsigned i=0;i<64;i++) {
            struct efs_msg_publication r;
            assert(efs_publication_ack_next(&q,&r)==EFS_OK && r.id.seq==1+pass*64+i);
            struct efs_msg_publication_reply retired=reply(&r,EFS_PUBLICATION_RETIRED,EFS_META_PUBLICATION_RETIRED);
            assert(efs_publication_ack_complete(&q,r.id.seq,&retired)==EFS_OK);
        }
        assert(!q.count);
    }
}
static void durable_order(void)
{
    struct efs_kv *kv=efs_kv_mem_create();assert(kv);
    assert(efs_meta_apply_init(kv,1)==EFS_OK);
    struct efs_meta_attrs attrs={0,0,1};efs_ino_t ino;struct efs_meta_row row;
    assert(efs_meta_apply_create_file(kv,&attrs,EFS_ROOT_INO,S_IFREG|0644,"ack",&ino)==EFS_OK);
    assert(efs_meta_apply_get_inode(kv,ino,&row)==EFS_OK);
    struct efs_msg_publication a=request(10),b=request(11),out;
    a.rec.ino=b.rec.ino=ino;a.rec.file_generation=b.rec.file_generation=row.generation;
    struct efs_publication_ack_queue q={0};
    assert(efs_publication_ack_admit(&q,&a)==EFS_OK);
    assert(efs_publication_ack_admit(&q,&b)==EFS_OK);
    assert(efs_session_create(kv,a.id.client_uuid,1)==EFS_OK);
    uint32_t shard=efs_kv_lane_shard(ino,0);
    assert(efs_session_register(kv,a.id.client_uuid,1,shard)==EFS_OK);
    assert(efs_session_establish(kv,shard,a.id.client_uuid,1)==EFS_OK);
    struct efs_meta_pub pb=intent(&b);assert(efs_meta_apply_publish(kv,&pb)==EFS_OK);
    struct efs_msg_publication_reply rb=reply(&b,EFS_PUBLICATION_COMMITTED,EFS_OK);
    assert(efs_publication_ack_consumed(&q,11,&rb)==EFS_OK);
    /* Server has no older receipt yet: local ownership still prevents retiring
     * 11, which would reject the as-yet-unsubmitted 10 below its replay floor. */
    assert(efs_publication_ack_next(&q,&out)==EFS_ERR_BUSY);
    struct efs_meta_pub pa=intent(&a);assert(efs_meta_apply_publish(kv,&pa)==EFS_ERR_STALE);
    struct efs_msg_publication_reply ra=reply(&a,EFS_PUBLICATION_REJECTED,EFS_ERR_STALE);
    assert(efs_publication_ack_consumed(&q,10,&ra)==EFS_OK);
    assert(efs_publication_ack_next(&q,&out)==EFS_OK && out.id.seq==10);
    assert(efs_meta_apply_publication_retire(kv,&pa)==EFS_OK);
    /* Lost retirement reply: immutable request survives for idempotent retry. */
    assert(efs_publication_ack_next(&q,&out)==EFS_OK && out.id.seq==10);
    assert(efs_meta_apply_publication_retire(kv,&pa)==EFS_OK);
    ra=reply(&a,EFS_PUBLICATION_RETIRED,EFS_META_PUBLICATION_RETIRED);
    assert(efs_publication_ack_complete(&q,10,&ra)==EFS_OK);
    assert(efs_publication_ack_next(&q,&out)==EFS_OK && out.id.seq==11);
    assert(efs_meta_apply_publication_retire(kv,&pb)==EFS_OK);
    rb=reply(&b,EFS_PUBLICATION_RETIRED,EFS_META_PUBLICATION_RETIRED);
    assert(efs_publication_ack_complete(&q,11,&rb)==EFS_OK);
    assert(efs_meta_apply_publish(kv,&pa)==EFS_ERR_STALE);
    assert(efs_meta_apply_publish(kv,&pb)==EFS_ERR_STALE);
    efs_kv_mem_free(kv);
}
int main(void)
{
    struct efs_msg_publication r=request(99);struct efs_meta_pub p=intent(&r);
    uint8_t before[EFS_HASH_SIZE],after[EFS_HASH_SIZE];
    assert(!efs_publication_digest(&p,before));
    r.rec.publish_flags|=EFS_CHUNK_REC_F_FRESH_OBJECT;p=intent(&r);assert(p.fresh_object);
    assert(!efs_publication_digest(&p,after)&&memcmp(before,after,sizeof(before)));
    ownership();bounded_wrap();durable_order();
    puts("publication ACK: immutable ownership, ordered retirement, bounded wrap and lost replies PASS");
    return 0;
}
