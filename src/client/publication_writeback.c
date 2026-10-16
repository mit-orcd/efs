#include "client_internal.h"
#include "efs/publication_ack.h"
#include "efs/kv_key.h"
#include <string.h>

/* Queue ownership outlives the body: reserve it before accepting the first
 * write, and never accept another pending intent beyond the receipt bound. */
int efs_client_publication_admit_write(struct efs_publication_ack_queue **slot,
    efs_ino_t ino, uint64_t generation, uint32_t ci,
    const struct efs_msg_lane_writer_view_reply *view, uint32_t off,
    const uint8_t *src, uint32_t len)
{
    if (!slot || !view || !ino || !generation || !src || !len ||
        off>=view->chunk_size || len>view->chunk_size-off) return EFS_ERR_INVAL;
    struct efs_opid identity;
    int rc=efs_client_publication_id(1,&identity);
    if (rc!=EFS_OK) return rc;
    struct efs_publication_ack_queue *queue=*slot;
    if (queue && queue->bound && (queue->stream.rec.ino!=ino ||
        queue->stream.rec.file_generation!=generation || queue->stream.rec.chunk_index!=ci ||
        queue->stream.id.session_epoch!=identity.session_epoch ||
        memcmp(queue->stream.id.client_uuid,identity.client_uuid,EFS_OPID_UUID_LEN)))
        return EFS_ERR_STALE;
    if (queue && queue->count>=EFS_META_PUBLICATION_MAX_RECEIPTS) return EFS_ERR_BUSY;
    int fresh=!queue;
    if (fresh) {
        queue=efs_client_publication_ack_alloc();
        if (!queue) return EFS_ERR_NOMEM;
    }
    rc=efs_dcache_admit_lane_write(ino,generation,ci,view,off,src,len);
    if (rc!=EFS_OK) {
        if (fresh) (void)efs_client_publication_ack_free(queue);
        return rc;
    }
    if (!queue->bound) {
        queue->stream.rec.ino=ino;queue->stream.rec.file_generation=generation;
        queue->stream.rec.chunk_index=ci;queue->stream.id=identity;queue->bound=1;
    }
    *slot=queue;return EFS_OK;
}

/* Always recover using the admitted immutable request. Packed admission
 * already established the producer session; submit itself reads the exact
 * durable outcome before proposing. Unknown state or command commitment
 * never proves that the mapping CAS succeeded. */
int efs_client_publication_progress(struct efs_publication_ack_queue *queue,
    uint64_t sequence, struct efs_msg_publication_reply *out)
{
    if (!out) return EFS_ERR_INVAL;
    struct efs_msg_publication request;
    int rc=efs_publication_ack_request(queue,sequence,&request);
    if (rc!=EFS_OK) return rc;
    struct efs_msg_publication_reply reply={0};
    rc=efs_client_rpc_publication(&request,1,&reply);
    if (rc==EFS_ERR_NOT_FOUND || rc==EFS_ERR_BUSY) {
        uint32_t shard=efs_kv_lane_shard(request.rec.ino,
                                       request.rec.chunk_index%EFS_META_LANES);
        rc=efs_client_rpc_publication_session(shard,&request.id);
        if (rc!=EFS_OK) return rc;
        rc=efs_client_rpc_publication(&request,0,&reply);
    } else if (rc==EFS_OK && reply.state==EFS_PUBLICATION_PENDING) {
        rc=efs_client_rpc_publication(&request,0,&reply);
    }
    if (rc!=EFS_OK) return rc;
    *out=reply;
    if (reply.state==EFS_PUBLICATION_COMMITTED || reply.state==EFS_PUBLICATION_REJECTED)
        return EFS_OK;
    return reply.state==EFS_PUBLICATION_RETIRED ? EFS_ERR_STALE : EFS_ERR_BUSY;
}

int efs_client_publication_writeback(struct efs_publication_ack_queue *queue,
    efs_ino_t ino, uint64_t generation, uint32_t ci, uint64_t object,
    uint64_t sequence, uint64_t size)
{
    struct efs_msg_publication request;
    int rc=efs_publication_ack_request(queue,sequence,&request);
    if (rc==EFS_ERR_NOT_FOUND) {
        rc=efs_dcache_publication_request(ino,generation,ci,object,sequence,size,&request);
        if (rc==EFS_OK) rc=efs_publication_ack_admit(queue,&request);
    }
    if (rc!=EFS_OK) return rc;
    if (request.rec.ino!=ino || request.rec.file_generation!=generation ||
        request.rec.chunk_index!=ci || request.rec.chunk_generation!=object)
        return EFS_ERR_STALE;
    for (uint32_t i=0;i<queue->count;i++) {
        const struct efs_publication_ack_entry *entry=&queue->entries[
            (queue->head+i)%EFS_META_PUBLICATION_MAX_RECEIPTS];
        if (entry->request.id.seq==sequence && entry->consumed_state)
            return entry->consumed_verdict;
    }
    struct efs_msg_publication_reply reply;
    rc=efs_client_publication_progress(queue,sequence,&reply);
    if (rc!=EFS_OK) return rc;
    int consumed=0;
    rc=efs_dcache_consume_publication(ino,generation,ci,object,sequence,&reply,&consumed);
    if (!consumed) return rc;
    rc=efs_publication_ack_consumed(queue,sequence,&reply);
    return rc==EFS_OK ? reply.verdict : rc;
}

/* No cache dependency. A lost ACK retains this oldest intent and its budget;
 * a newer consumed result must never retire across an older unresolved one. */
int efs_client_publication_retire(struct efs_publication_ack_queue *queue)
{
    struct efs_msg_publication request;
    int rc=efs_publication_ack_next(queue,&request);
    if (rc!=EFS_OK) return rc;
    struct efs_msg_publication_reply reply;
    rc=efs_client_rpc_publication(&request,2,&reply);
    return rc==EFS_OK ? efs_publication_ack_complete(queue,request.id.seq,&reply) : rc;
}
