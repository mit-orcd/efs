#include "efs/publication_ack.h"
#include <string.h>

static int same_stream(const struct efs_msg_publication *a,
                       const struct efs_msg_publication *b)
{
    return a->rec.ino==b->rec.ino &&
        a->rec.file_generation==b->rec.file_generation &&
        a->rec.chunk_index==b->rec.chunk_index &&
        a->id.session_epoch==b->id.session_epoch &&
        !memcmp(a->id.client_uuid,b->id.client_uuid,EFS_OPID_UUID_LEN);
}
static int intent_digest(const struct efs_msg_publication *request,
                         uint8_t digest[EFS_HASH_SIZE])
{
    struct efs_meta_pub p;
    int rc=efs_publication_from_rec(&request->rec,request->size,&p);
    if (rc!=EFS_OK) return rc;
    p.publication_id=request->id;
    return efs_publication_digest(&p,digest);
}
int efs_publication_ack_admit(struct efs_publication_ack_queue *q,
                             const struct efs_msg_publication *request)
{
    if (!q || !request) return EFS_ERR_INVAL;
    uint8_t digest[EFS_HASH_SIZE];
    int rc=intent_digest(request,digest);
    if (rc!=EFS_OK) return rc;
    if (q->bound && !same_stream(&q->stream,request)) return EFS_ERR_STALE;
    for (uint32_t i=0;i<q->count;i++) {
        struct efs_publication_ack_entry *e=&q->entries[(q->head+i)%EFS_META_PUBLICATION_MAX_RECEIPTS];
        if (e->request.id.seq==request->id.seq)
            return memcmp(e->digest,digest,sizeof(digest)) ? EFS_ERR_PROTO : EFS_OK;
    }
    if (request->id.seq<=q->last_sequence) return EFS_ERR_STALE;
    if (q->count==EFS_META_PUBLICATION_MAX_RECEIPTS) return EFS_ERR_BUSY;
    struct efs_publication_ack_entry *e=&q->entries[(q->head+q->count)%EFS_META_PUBLICATION_MAX_RECEIPTS];
    memset(e,0,sizeof(*e));e->request=*request;memcpy(e->digest,digest,sizeof(digest));
    if (!q->bound) {q->stream=*request;q->bound=1;}
    q->last_sequence=request->id.seq;q->count++;
    return EFS_OK;
}
int efs_publication_ack_consumed(struct efs_publication_ack_queue *q,
    uint64_t sequence, const struct efs_msg_publication_reply *reply)
{
    if (!q || !reply || !sequence) return EFS_ERR_INVAL;
    if (reply->rpc.status!=EFS_INODE_RPC_OK ||
        !((reply->state==EFS_PUBLICATION_COMMITTED && reply->verdict==EFS_OK) ||
          (reply->state==EFS_PUBLICATION_REJECTED &&
           (reply->verdict==EFS_ERR_STALE || reply->verdict==EFS_ERR_INVAL))))
        return EFS_ERR_BUSY;
    for (uint32_t i=0;i<q->count;i++) {
        struct efs_publication_ack_entry *e=&q->entries[(q->head+i)%EFS_META_PUBLICATION_MAX_RECEIPTS];
        if (e->request.id.seq!=sequence) continue;
        if (memcmp(e->digest,reply->digest,EFS_HASH_SIZE)) return EFS_ERR_PROTO;
        if (e->consumed_state && (e->consumed_state!=reply->state ||
            e->consumed_verdict!=reply->verdict)) return EFS_ERR_PROTO;
        e->consumed_state=reply->state;e->consumed_verdict=reply->verdict;
        return EFS_OK;
    }
    return EFS_ERR_NOT_FOUND;
}
int efs_publication_ack_next(const struct efs_publication_ack_queue *q,
                             struct efs_msg_publication *out)
{
    if (!q || !out) return EFS_ERR_INVAL;
    if (!q->count) return EFS_ERR_NOT_FOUND;
    const struct efs_publication_ack_entry *e=&q->entries[q->head];
    if (!e->consumed_state) return EFS_ERR_BUSY;
    *out=e->request;
    return EFS_OK;
}
int efs_publication_ack_complete(struct efs_publication_ack_queue *q,
    uint64_t sequence, const struct efs_msg_publication_reply *reply)
{
    if (!q || !reply || !sequence) return EFS_ERR_INVAL;
    if (!q->count || q->entries[q->head].request.id.seq!=sequence) return EFS_ERR_STALE;
    struct efs_publication_ack_entry *e=&q->entries[q->head];
    if (!e->consumed_state || reply->rpc.status!=EFS_INODE_RPC_OK ||
        reply->state!=EFS_PUBLICATION_RETIRED || reply->verdict!=EFS_META_PUBLICATION_RETIRED)
        return EFS_ERR_BUSY;
    if (memcmp(e->digest,reply->digest,EFS_HASH_SIZE)) return EFS_ERR_PROTO;
    memset(e,0,sizeof(*e));q->head=(q->head+1)%EFS_META_PUBLICATION_MAX_RECEIPTS;q->count--;
    return EFS_OK;
}
