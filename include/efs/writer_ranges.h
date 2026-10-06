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

#endif
