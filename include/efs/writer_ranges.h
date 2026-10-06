#ifndef EFS_WRITER_RANGES_H
#define EFS_WRITER_RANGES_H

#include "efs/dirty_ranges.h"
#include "efs/protocol.h"

/* Caller-locked writer ownership. Initialize bytes.chunk_size before use.
 * Reserve any metadata allocation before accepting application bytes. These
 * functions own no buffers: copy bytes only after admit succeeds, and capture
 * the matching body while holding the same lock as snapshot planning. */
struct efs_writer_ranges {
    efs_ino_t ino;
    uint64_t generation;
    uint64_t observed_epoch;
    uint32_t chunk_index;
    struct efs_dirty_ranges bytes;
};
struct efs_writer_plan {
    efs_ino_t ino;
    uint64_t generation;
    uint64_t publish_epoch;
    uint32_t chunk_index;
    int base_absent; /* zero image + expected object generation zero */
    int base_bound; /* GETCHUNKS captured exact CAS identity */
    uint64_t base_generation, base_sequence;
    uint32_t base_delta_count;
    struct efs_dirty_ranges original;
    struct efs_dirty_ranges surviving;
};

static inline int efs_writer_ranges_authority(
    const struct efs_writer_ranges *writer,
    const struct efs_msg_inode_writer_view_reply *view)
{
    if (!writer || !view || view->status != EFS_INODE_RPC_OK ||
        efs_dirty_ranges_valid(&writer->bytes) != EFS_OK || !view->ino)
        return EFS_ERR_INVAL;
    struct efs_msg_inode_writer_view req = {
        view->ino, view->generation, view->chunk_index, 0};
    int rc = efs_writer_view_reply_valid(&req, view);
    if (rc != EFS_OK)
        return rc;
    if (writer->ino && (writer->ino != view->ino ||
        writer->generation != view->generation ||
        writer->chunk_index != view->chunk_index ||
        writer->observed_epoch > view->authority_epoch))
        return EFS_ERR_STALE;
    /* Unbound ownership must be empty: an epoch alone is not FileID. */
    if (!writer->ino && writer->bytes.count)
        return EFS_ERR_INVAL;
    return EFS_OK;
}

/* Admission is transactional. BUSY means drain before copying this write;
 * it never promotes sparse ownership to an unconditional chunk overwrite.
 * A changed FileID or regressed authority cannot replace accepted bytes. */
static inline int efs_writer_ranges_admit(
    struct efs_writer_ranges *writer,
    const struct efs_msg_inode_writer_view_reply *view,
    uint32_t off, uint32_t len)
{
    int rc = efs_writer_ranges_authority(writer, view);
    if (rc != EFS_OK)
        return rc;
    struct efs_writer_ranges next = *writer;
    rc = efs_dirty_ranges_write(&next.bytes, off, len, view->authority_epoch);
    if (rc != EFS_OK)
        return rc;
    next.ino = view->ino;
    next.generation = view->generation;
    next.chunk_index = view->chunk_index;
    next.observed_epoch = view->authority_epoch;
    *writer = next;
    return EFS_OK;
}

/* The published image must be materialized from GETCHUNKS at this same
 * authority epoch; an authoritative absent row uses the writer snapshot
 * epoch as well. Resolve local
 * ownership only under complete history. STALE retains the original state:
 * the caller must preserve accepted bytes, never recapture them at a newer
 * epoch to manufacture validity after retirement. */
static inline int efs_writer_ranges_plan(
    const struct efs_writer_ranges *writer,
    const struct efs_msg_inode_writer_view_reply *view,
    uint64_t published_authority_epoch, struct efs_writer_plan *out)
{
    int rc = efs_writer_ranges_authority(writer, view);
    if (rc != EFS_OK)
        return rc;
    if (!out || !writer->ino)
        return EFS_ERR_INVAL;
    if (published_authority_epoch != view->authority_epoch)
        return EFS_ERR_STALE;
    struct efs_writer_plan plan = {0};
    plan.ino = writer->ino;
    plan.generation = writer->generation;
    plan.chunk_index = writer->chunk_index;
    plan.publish_epoch = view->authority_epoch;
    plan.original = writer->bytes;
    rc = efs_dirty_ranges_clip(&plan.original, &view->history,
        view->authority_epoch, view->oldest_complete_epoch,
        (uint64_t)writer->chunk_index * writer->bytes.chunk_size,
        &plan.surviving);
    if (rc != EFS_OK)
        return rc;
    *out = plan;
    return EFS_OK;
}

/* Prefer this boundary when planning from an actual GETCHUNKS reply. Even
 * an empty result carries FileID and inode authority: absence is not epoch
 * zero. A row additionally must match chunk geometry and lane authority. */
static inline int efs_writer_ranges_plan_base(
    const struct efs_writer_ranges *writer,
    const struct efs_msg_inode_writer_view_reply *view,
    const struct efs_msg_inode_getchunks_reply *base,
    struct efs_writer_plan *out)
{
    int rc = efs_writer_ranges_authority(writer, view);
    if (rc != EFS_OK)
        return rc;
    if (!out || !base || base->status != EFS_INODE_RPC_OK)
        return EFS_ERR_INVAL;
    if (base->ino != writer->ino || base->generation != writer->generation ||
        base->authority_epoch != view->authority_epoch)
        return EFS_ERR_STALE;
    struct efs_msg_inode_getchunks req = {0};
    req.ino = writer->ino;
    req.generation = writer->generation;
    req.start = writer->chunk_index;
    req.max = 1;
    rc = efs_getchunks_reply_valid(&req, base);
    if (rc != EFS_OK)
        return rc;
    /* A later first row proves the requested chunk is a hole. Never use
     * that later row's bytes/identity as this chunk's CAS base. */
    int absent = !base->count || base->recs[0].chunk_index > writer->chunk_index;
    if (!absent && (base->recs[0].read_view.chunk_size != writer->bytes.chunk_size ||
        base->recs[0].read_view.fence_epoch != view->authority_epoch))
        return EFS_ERR_STALE;
    struct efs_writer_plan plan;
    rc = efs_writer_ranges_plan(writer, view, base->authority_epoch, &plan);
    if (rc == EFS_OK) {
        plan.base_absent = absent;
        plan.base_bound = 1;
        if (!absent) {
            plan.base_generation = base->recs[0].chunk_generation;
            plan.base_sequence = base->recs[0].delta_base_seq;
            plan.base_delta_count = base->recs[0].delta_base_n;
        }
        *out = plan;
    }
    return rc;
}

/* Verify the base again before touching a materialized output. A token
 * captured from one object list cannot be reused with a later peer image. */
static inline int efs_writer_plan_base_valid(const struct efs_writer_plan *plan,
    const struct efs_msg_inode_getchunks_reply *base)
{
    if (!plan || !base || !plan->base_bound || base->status != EFS_INODE_RPC_OK)
        return EFS_ERR_INVAL;
    struct efs_msg_inode_getchunks req = {0};
    req.ino=plan->ino;req.generation=plan->generation;
    req.start=plan->chunk_index;req.max=1;
    int rc=efs_getchunks_reply_valid(&req,base);
    if (rc != EFS_OK) return rc;
    if (base->authority_epoch != plan->publish_epoch) return EFS_ERR_STALE;
    int absent=!base->count || base->recs[0].chunk_index>plan->chunk_index;
    if (absent != plan->base_absent) return EFS_ERR_STALE;
    if (!absent) {
        const struct efs_chunk_rec *r=&base->recs[0];
        if (r->chunk_generation != plan->base_generation ||
            r->delta_base_seq != plan->base_sequence ||
            r->delta_base_n != plan->base_delta_count ||
            r->read_view.chunk_size != plan->original.chunk_size ||
            r->read_view.fence_epoch != plan->publish_epoch)
            return EFS_ERR_STALE;
    }
    return EFS_OK;
}

/* Caller first matches the committed PUT/REPORT object and snapshot sequence.
 * A clipped view is never the acknowledgement token. A later mutation, even
 * at the same authority epoch, prevents the old plan clearing its ownership. */
static inline int efs_writer_ranges_ack(struct efs_writer_ranges *writer,
                                         const struct efs_writer_plan *plan)
{
    if (!writer || !plan)
        return EFS_ERR_INVAL;
    if (writer->ino != plan->ino || writer->generation != plan->generation ||
        writer->chunk_index != plan->chunk_index)
        return EFS_ERR_STALE;
    return efs_dirty_ranges_ack(&writer->bytes, &plan->original);
}

/* A successful PUT alone cannot release bytes. Keep its object and snapshot
 * sequence attached to the immutable original ownership until REPORT commits. */
struct efs_writer_publication {
    struct efs_writer_plan plan;
    uint64_t object_generation;
    uint64_t snapshot_sequence;
};
/* Clipping can only remove owned bytes. A token must never manufacture
 * ranges in holes, change their admission ages, or name a future epoch. */
static inline int efs_writer_plan_valid(const struct efs_writer_plan *plan)
{
    if (!plan || !plan->ino || !plan->generation ||
        (plan->base_absent != 0 && plan->base_absent != 1) ||
        (plan->base_bound != 0 && plan->base_bound != 1) ||
        plan->base_delta_count > EFS_CHUNK_DELTA_MAX ||
        plan->base_generation == EFS_CHUNK_BASE_UNCOND ||
        (plan->base_absent && (plan->base_generation || plan->base_sequence || plan->base_delta_count)) ||
        (!plan->base_bound && (plan->base_generation || plan->base_sequence || plan->base_delta_count)) ||
        efs_dirty_ranges_valid(&plan->original) != EFS_OK ||
        efs_dirty_ranges_valid(&plan->surviving) != EFS_OK ||
        plan->original.chunk_size != plan->surviving.chunk_size ||
        plan->original.mutation != plan->surviving.mutation)
        return EFS_ERR_INVAL;
    for (uint32_t i = 0; i < plan->original.count; ++i)
        if (plan->original.ranges[i].epoch > plan->publish_epoch)
            return EFS_ERR_INVAL;
    for (uint32_t i = 0; i < plan->surviving.count; ++i) {
        const struct efs_fence_part *part = &plan->surviving.ranges[i];
        int owned = 0;
        for (uint32_t j = 0; j < plan->original.count; ++j) {
            const struct efs_fence_part *source = &plan->original.ranges[j];
            if (part->epoch == source->epoch && part->off >= source->off &&
                part->off + part->len <= source->off + source->len) {
                owned = 1;
                break;
            }
        }
        if (!owned)
            return EFS_ERR_INVAL;
    }
    return EFS_OK;
}

/* A whole-image CAS is always rebuilt from a fresh, masked peer base plus
 * the immutable owned snapshot. The caller fetched/merged published bytes
 * for exactly this GETCHUNKS identity. Output must be separate storage. */
static inline int efs_writer_plan_materialize(const struct efs_writer_plan *plan,
    const struct efs_msg_inode_getchunks_reply *base, const uint8_t *peer,
    const uint8_t *owned, uint8_t *out, uint32_t size)
{
    if (efs_writer_plan_valid(plan)!=EFS_OK || !owned || !out ||
        size!=plan->original.chunk_size || !efs_chunk_size_valid(size))
        return EFS_ERR_INVAL;
    int rc=efs_writer_plan_base_valid(plan,base);
    if (rc!=EFS_OK) return rc;
    if (!plan->base_absent && !peer) return EFS_ERR_INVAL;
    uintptr_t o=(uintptr_t)out, w=(uintptr_t)owned, p=(uintptr_t)peer;
    uintptr_t t=(uintptr_t)plan, b=(uintptr_t)base;
    if ((o<=t ? t-o<size : o-t<sizeof(*plan)) ||
        (o<=b ? b-o<size : o-b<sizeof(*base)) ||
        (o<=w ? w-o<size : o-w<size) ||
        (peer && (o<=p ? p-o<size : o-p<size))) return EFS_ERR_INVAL;
    memset(out,0,size);
    if (!plan->base_absent) {
        const struct efs_fence_view *v=&base->recs[0].read_view;
        for (uint32_t i=0;i<v->count;i++)
            memcpy(out+v->parts[i].off,peer+v->parts[i].off,v->parts[i].len);
    }
    for (uint32_t i=0;i<plan->surviving.count;i++) {
        const struct efs_fence_part *r=&plan->surviving.ranges[i];
        memcpy(out+r->off,owned+r->off,r->len);
    }
    return EFS_OK;
}

/* PUT identity supplies only immutable object placement/checksums. The plan
 * supplies every CAS/epoch/FileID field; cached publication defaults cannot
 * turn a sparse patch into an unconditional overwrite. */
static inline int efs_writer_plan_report(const struct efs_writer_plan *plan,
    const struct efs_chunk_rec *put, struct efs_chunk_rec *out)
{
    if (efs_writer_plan_valid(plan)!=EFS_OK || !plan->base_bound || !put || !out ||
        !plan->surviving.count || !put->chunk_generation || put->delta_len ||
        put->ino!=plan->ino || put->chunk_index!=plan->chunk_index)
        return EFS_ERR_INVAL;
    for (unsigned i=0;i<EFS_NUM_FRAGMENTS;i++)
        if (!put->nodes[i]) return EFS_ERR_INVAL;
    struct efs_chunk_rec r={0};
    r.ino=plan->ino;r.chunk_index=plan->chunk_index;
    r.chunk_generation=put->chunk_generation;
    memcpy(r.nodes,put->nodes,sizeof(r.nodes));
    memcpy(r.checksums,put->checksums,sizeof(r.checksums));
    r.base_gen=plan->base_generation;
    r.delta_base_n=plan->base_delta_count;r.delta_base_seq=plan->base_sequence;
    r.file_generation=plan->generation;r.publish_epoch=plan->publish_epoch;
    r.publish_flags=EFS_CHUNK_REC_F_CAPTURED_EPOCH|EFS_CHUNK_REC_F_CAPTURED_FILEID;
    *out=r;
    return EFS_OK;
}

static inline int efs_writer_publication_bind(
    const struct efs_writer_plan *plan, uint64_t object_generation,
    uint64_t snapshot_sequence, struct efs_writer_publication *out)
{
    if (!out || !object_generation || !snapshot_sequence ||
        efs_writer_plan_valid(plan) != EFS_OK)
        return EFS_ERR_INVAL;
    struct efs_writer_publication publication = {0};
    publication.plan = *plan;
    publication.object_generation = object_generation;
    publication.snapshot_sequence = snapshot_sequence;
    *out = publication;
    return EFS_OK;
}
static inline int efs_writer_publication_complete(
    struct efs_writer_ranges *writer, const struct efs_writer_publication *publication,
    uint64_t committed_object, uint64_t committed_sequence, int verdict)
{
    if (!writer || !publication || !publication->object_generation ||
        !publication->snapshot_sequence ||
        efs_writer_plan_valid(&publication->plan) != EFS_OK)
        return EFS_ERR_INVAL;
    if (verdict != EFS_OK)
        return verdict;
    if (committed_object != publication->object_generation ||
        committed_sequence != publication->snapshot_sequence)
        return EFS_ERR_STALE;
    int rc = efs_writer_ranges_ack(writer, &publication->plan);
    if ((rc == EFS_OK || rc == EFS_ERR_STALE) &&
        writer->ino == publication->plan.ino &&
        writer->generation == publication->plan.generation &&
        writer->chunk_index == publication->plan.chunk_index &&
        writer->observed_epoch < publication->plan.publish_epoch)
        writer->observed_epoch = publication->plan.publish_epoch;
    return rc;
}

#endif
