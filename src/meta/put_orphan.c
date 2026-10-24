/* D31 abandoned-upload collection step; see include/efs/put_orphan.h. */
#include "efs/put_orphan.h"
#include "efs/kv_key.h"
#include "efs/session.h"

int efs_put_orphan_collect(struct efs_kv *kv, const struct efs_put_ticket *t, uint32_t acked,
                           const struct efs_put_orphan_ops *ops, uint64_t *skip,
                           struct efs_put_orphan_stats *st)
{
    if (!kv || !t || !ops || !ops->revoked || !ops->delete_fragment || !ops->ack || !st ||
        acked >= EFS_PUT_ORPHAN_ALL)
        return EFS_ERR_INVAL;
    uint32_t shard = efs_kv_lane_shard(t->ino, (uint8_t)(t->ci % EFS_META_LANES));
    int rc = ops->revoked(ops->ctx, t->id.client_uuid, t->id.session_epoch);
    if (rc == EFS_OK)
        rc = efs_session_local_revoked(kv, shard, t->id.client_uuid, t->id.session_epoch);
    if (rc != EFS_OK) {
        st->waiting++;
        return rc == EFS_ERR_BUSY || rc == EFS_ERR_NOT_FOUND ? EFS_ERR_BUSY : rc;
    }
    uint32_t done = 0;
    for (unsigned fi = 0; fi < EFS_NUM_FRAGMENTS; fi++) {
        uint32_t bit = 1u << fi;
        efs_node_id_t node = t->nodes[fi];
        if (acked & bit)
            continue;
        if (!node || node > EFS_MAX_NODES)
            return EFS_ERR_PROTO;
        if (skip && (*skip & (1ull << (node - 1))))
            continue;
        rc = ops->delete_fragment(ops->ctx, t, fi);
        if (rc == EFS_OK) {
            done |= bit;
            st->deleted++;
            continue;
        }
        st->failures++;
        /* Unreachable: do not wait on it again this pass. */
        if (skip && (rc == EFS_ERR_IO || rc == EFS_ERR_BUSY || rc == EFS_ERR_NOT_PRIMARY))
            *skip |= 1ull << (node - 1);
    }
    if (!done)
        return EFS_ERR_BUSY;
    rc = ops->ack(ops->ctx, t, done);
    if (rc != EFS_OK) {
        /* Lost or refused: the deletes stand; a later pass re-deletes
         * (a no-op on an absent fragment) and acknowledges again. */
        st->failures++;
        return rc == EFS_ERR_IO || rc == EFS_ERR_NOMEM ? rc : EFS_ERR_BUSY;
    }
    rc = efs_put_ticket_orphan_get(kv, t, NULL);
    if (rc == EFS_ERR_NOT_FOUND) {
        st->collected++;
        return EFS_OK;
    }
    return rc == EFS_OK ? EFS_ERR_BUSY : rc;
}

int efs_put_drain_stream(struct efs_kv *kv, const struct efs_put_stream *s,
                         const struct efs_put_drain_ops *ops, uint32_t budget,
                         struct efs_put_drain_stats *st)
{
    if (!kv || !s || !ops || !ops->revoked || !ops->drain || !st || !budget)
        return EFS_ERR_INVAL;
    int rc = efs_put_ticket_stream_live(kv, s);
    if (rc == EFS_ERR_NOT_FOUND)
        return EFS_OK;
    if (rc == EFS_OK)
        rc = ops->revoked(ops->ctx, s->uuid, s->epoch);
    if (rc == EFS_OK)
        rc = efs_session_local_revoked(kv, s->shard, s->uuid, s->epoch);
    if (rc != EFS_OK) {
        if (rc == EFS_ERR_BUSY || rc == EFS_ERR_NOT_FOUND) {
            st->waiting++;
            return EFS_ERR_BUSY;
        }
        return rc;
    }
    for (uint32_t i = 0; i < budget; i++) {
        rc = ops->drain(ops->ctx, s);
        if (rc != EFS_OK) {
            if (rc == EFS_ERR_BUSY)
                st->blocked++;
            return rc;
        }
        st->commands++;
        rc = efs_put_ticket_stream_live(kv, s);
        if (rc == EFS_ERR_NOT_FOUND) {
            st->drained++;
            return EFS_OK;
        }
        if (rc != EFS_OK)
            return rc;
    }
    return EFS_ERR_BUSY;
}
