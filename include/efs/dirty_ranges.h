#ifndef EFS_DIRTY_RANGES_H
#define EFS_DIRTY_RANGES_H

#include "efs/fence_view.h"

/* Caller-locked D25 writer ownership. Each byte belongs to the most recent
 * admitted write covering it, at that write's captured authority epoch.
 * This helper supplies no read authority and does not choose the write's
 * linearization point. Epoch zero is valid; never derive it from a cache hit.
 * Snapshot by value before dropping the lock for GET/PUT. */
#define EFS_DIRTY_RANGE_MAX 32u
struct efs_dirty_ranges {
    uint64_t mutation;
    uint32_t chunk_size;
    uint32_t count;
    struct efs_fence_part ranges[EFS_DIRTY_RANGE_MAX];
};

static inline int efs_dirty_ranges_valid(const struct efs_dirty_ranges *r)
{
    if (!r || !r->chunk_size || r->count > EFS_DIRTY_RANGE_MAX)
        return EFS_ERR_INVAL;
    for (uint32_t i = 0; i < r->count; ++i) {
        const struct efs_fence_part *p = &r->ranges[i];
        if (!p->len || p->off >= r->chunk_size ||
            p->len > r->chunk_size - p->off)
            return EFS_ERR_INVAL;
        if (i) {
            const struct efs_fence_part *before = &r->ranges[i - 1];
            uint32_t end = before->off + before->len;
            if (end > p->off || (end == p->off && before->epoch == p->epoch))
                return EFS_ERR_INVAL;
        }
    }
    return EFS_OK;
}

/* At most two pieces per old range and one incoming write. Merge adjacent
 * equal-age bytes only; bounding a mixed-age set by its union would re-stamp
 * old bytes or declare holes owned. BUSY leaves ownership unchanged, so the
 * caller can drain before accepting/copying the new application bytes. */
static inline int efs_dirty_ranges_write(struct efs_dirty_ranges *r,
                                          uint32_t off, uint32_t len,
                                          uint64_t epoch)
{
    struct efs_fence_part parts[2 * EFS_DIRTY_RANGE_MAX + 1];
    struct efs_dirty_ranges next;
    uint32_t end, n = 0;
    int inserted = 0;
    if (efs_dirty_ranges_valid(r) != EFS_OK || !len || off >= r->chunk_size ||
        len > r->chunk_size - off)
        return EFS_ERR_INVAL;
    if (r->mutation == UINT64_MAX)
        return EFS_ERR_BUSY;
    end = off + len;
    for (uint32_t i = 0; i < r->count; ++i) {
        struct efs_fence_part p = r->ranges[i];
        uint32_t pe = p.off + p.len;
        if (pe <= off) {
            parts[n++] = p;
        } else if (p.off >= end) {
            if (!inserted) {
                parts[n++] = (struct efs_fence_part){epoch, off, len};
                inserted = 1;
            }
            parts[n++] = p;
        } else {
            if (p.off < off)
                parts[n++] = (struct efs_fence_part){p.epoch, p.off, off - p.off};
            if (!inserted) {
                parts[n++] = (struct efs_fence_part){epoch, off, len};
                inserted = 1;
            }
            if (pe > end)
                parts[n++] = (struct efs_fence_part){p.epoch, end, pe - end};
        }
    }
    if (!inserted)
        parts[n++] = (struct efs_fence_part){epoch, off, len};
    next = (struct efs_dirty_ranges){.mutation = r->mutation + 1,
                                     .chunk_size = r->chunk_size};
    for (uint32_t i = 0; i < n; ++i) {
        struct efs_fence_part *last = next.count ? &next.ranges[next.count - 1] : NULL;
        if (last && last->epoch == parts[i].epoch &&
            last->off + last->len == parts[i].off) {
            last->len += parts[i].len;
        } else {
            if (next.count == EFS_DIRTY_RANGE_MAX)
                return EFS_ERR_BUSY;
            next.ranges[next.count++] = parts[i];
        }
    }
    *r = next;
    return EFS_OK;
}

/* Resolve masks for this immutable snapshot under fresh lane authority.
 * Keep the original range ages. An incomplete history is never sufficient:
 * oldest_complete_epoch is the oldest writer epoch the supplied history can
 * still answer. A scheduler must retain needed history or reject/reconcile
 * an older snapshot; it must never treat a missing fence as no fence. */
static inline int efs_dirty_ranges_clip(const struct efs_dirty_ranges *snapshot,
                                         const struct efs_fence_history *history,
                                         uint64_t authority_epoch,
                                         uint64_t oldest_complete_epoch,
                                         uint64_t chunk_start,
                                         struct efs_dirty_ranges *out)
{
    struct efs_dirty_ranges next;
    if (!out || efs_dirty_ranges_valid(snapshot) != EFS_OK ||
        efs_fence_history_valid(history) != EFS_OK ||
        oldest_complete_epoch > authority_epoch ||
        chunk_start > UINT64_MAX - snapshot->chunk_size ||
        (history->count && history->entries[history->count - 1].epoch > authority_epoch))
        return EFS_ERR_INVAL;
    next = (struct efs_dirty_ranges){.mutation = snapshot->mutation,
                                     .chunk_size = snapshot->chunk_size};
    for (uint32_t i = 0; i < snapshot->count; ++i) {
        struct efs_fence_part p = snapshot->ranges[i];
        uint64_t end;
        if (p.epoch > authority_epoch || p.epoch < oldest_complete_epoch)
            return EFS_ERR_STALE;
        end = efs_fence_valid_end(history, p.epoch);
        if (end <= chunk_start + p.off)
            continue;
        if (end - chunk_start - p.off < p.len)
            p.len = (uint32_t)(end - chunk_start - p.off);
        next.ranges[next.count++] = p;
    }
    *out = next;
    return EFS_OK;
}

/* Overlay only the surviving locally owned bytes on an already-masked
 * published image. Discarded dirty suffixes do not zero surviving peer data.
 * Validation happens before copying. Exact in-place use is safe; distinct
 * buffers must not partially overlap. */
static inline int efs_dirty_ranges_overlay(const struct efs_dirty_ranges *masked,
                                            uint8_t *base, uint32_t capacity,
                                            const uint8_t *local)
{
    if (!base || !local || efs_dirty_ranges_valid(masked) != EFS_OK ||
        capacity < masked->chunk_size)
        return EFS_ERR_INVAL;
    for (uint32_t i = 0; i < masked->count; ++i) {
        const struct efs_fence_part *p = &masked->ranges[i];
        if (base != local)
            memcpy(base + p->off, local + p->off, p->len);
    }
    return EFS_OK;
}

/* Clear only the original ownership snapshot whose publication committed.
 * A later write, even of identical bytes/epoch, has a distinct mutation.
 * Concurrent changes stay owned for another flush; a clipped byte view is
 * not an acknowledgement token. Caller also verifies the PUT/REPORT identity. */
static inline int efs_dirty_ranges_ack(struct efs_dirty_ranges *current,
                                       const struct efs_dirty_ranges *snapshot)
{
    if (efs_dirty_ranges_valid(current) != EFS_OK ||
        efs_dirty_ranges_valid(snapshot) != EFS_OK)
        return EFS_ERR_INVAL;
    if (current->mutation != snapshot->mutation ||
        current->chunk_size != snapshot->chunk_size ||
        current->count != snapshot->count)
        return EFS_ERR_STALE;
    for (uint32_t i = 0; i < current->count; ++i)
        if (current->ranges[i].epoch != snapshot->ranges[i].epoch ||
            current->ranges[i].off != snapshot->ranges[i].off ||
            current->ranges[i].len != snapshot->ranges[i].len)
            return EFS_ERR_STALE;
    memset(current->ranges, 0, sizeof(current->ranges));
    current->count = 0;
    return EFS_OK;
}

#endif
