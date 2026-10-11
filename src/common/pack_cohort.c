#include "efs/pack_cohort.h"
#include "efs/kv_key.h"
#include "efs/raft.h"
#include <string.h>
int efs_pack_cohort_init(struct efs_pack_cohort *c, uint32_t group,
    uint32_t limit, uint64_t delay)
{
    if (!c || (group != EFS_RAFT_GROUP_SHARD && group != EFS_RAFT_GROUP_SHARD2) ||
        !limit || limit > EFS_PACK_COHORT_RECORDS || !delay) return EFS_ERR_INVAL;
    memset(c, 0, sizeof(*c)); c->group = group; c->limit = limit; c->delay_ns = delay;
    return EFS_OK;
}
int efs_pack_cohort_offer(struct efs_pack_cohort *c, const struct efs_pack_record *r,
    uint64_t snapshot, uint64_t now)
{
    if (!c || !r || !c->limit || !r->ino || efs_ino_is_meta_table(r->ino) ||
        !r->inode_generation || !r->record_generation || !snapshot || !r->data ||
        !r->len || r->len > EFS_PACK_MAX_PAYLOAD ||
        efs_raft_shard_group(efs_kv_lane_shard(r->ino, 0)) != c->group) return EFS_ERR_INVAL;
    if (c->sealed || c->count == c->limit) return EFS_ERR_BUSY;
    for (uint32_t i = 0; i < c->count; i++) if (c->records[i].ino == r->ino) return EFS_ERR_BUSY;
    size_t used = EFS_PACK_HEADER_BYTES + EFS_HASH_SIZE +
        (size_t)(c->count + 1) * EFS_PACK_RECORD_BYTES + c->payload_bytes + r->len;
    if (used > EFS_SEGMENT_MAX_SIZE) return EFS_ERR_BUSY;
    uint8_t *copy = c->payload + c->payload_bytes;
    /* The source may be a caller's mutable cache. Copy before returning success. */
    memcpy(copy, r->data, r->len);
    c->records[c->count] = *r; c->records[c->count].data = copy;
    c->snapshots[c->count] = snapshot;
    if (!c->count) c->first_ns = now;
    c->count++; c->payload_bytes += r->len;
    return EFS_OK;
}
int efs_pack_cohort_due(const struct efs_pack_cohort *c, uint64_t now, int force)
{
    if (!c || !c->count) return 0;
    if (c->sealed || force || c->count == c->limit) return 1;
    /* Flush if the monotonic clock unexpectedly moves backwards; never turn
     * unsigned wrap into an unbounded wait. Saturation cannot postpone sync. */
    if (now < c->first_ns || now - c->first_ns >= c->delay_ns) return 1;
    size_t next = EFS_PACK_HEADER_BYTES + EFS_HASH_SIZE +
        (size_t)(c->count + 1) * EFS_PACK_RECORD_BYTES + c->payload_bytes + EFS_PACK_MAX_PAYLOAD;
    return next > EFS_SEGMENT_MAX_SIZE;
}
int efs_pack_cohort_seal(struct efs_pack_cohort *c, const struct efs_opid *owner)
{
    if (!c || !owner || !c->count) return EFS_ERR_INVAL;
    if (c->sealed) return owner->session_epoch == c->owner.session_epoch &&
        owner->seq == c->owner.seq && !memcmp(owner->client_uuid, c->owner.client_uuid, EFS_OPID_UUID_LEN) ? EFS_OK : EFS_ERR_PROTO;
    uint64_t id, generation;
    int rc = efs_pack_identity(owner, c->group, &id, &generation);
    if (!rc) rc = efs_pack_build(id, generation, c->group, c->records,
        c->count, c->image, sizeof(c->image), &c->geometry, c->refs);
    if (!rc) { c->owner = *owner; c->sealed = 1; }
    return rc;
}
