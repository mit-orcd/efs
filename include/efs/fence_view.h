#ifndef EFS_FENCE_VIEW_H
#define EFS_FENCE_VIEW_H

#include "efs/common.h"
#include <string.h>

/* D25's byte semantics, shared by reads, publish merges and materialisation.
 * This helper does not grant read authority or protect fragment lifetime.
 * A caller must obtain the row and history from one validated snapshot.
 * Epoch zero is a valid pre-fence epoch. Sizes are absolute file offsets. */
#define EFS_FENCE_HISTORY_MAX 32u
#define EFS_FENCE_PART_MAX 9u /* one base and eight publication-ordered spans */

struct efs_content_fence {
    uint64_t epoch;
    uint64_t size;
};

struct efs_fence_history {
    uint32_t count;
    struct efs_content_fence entries[EFS_FENCE_HISTORY_MAX];
};

/* Object identity is kept by the caller. These are byte ranges in the chunk,
 * not in the encoded fragment; base is range [0, chunk_size). */
struct efs_fence_part {
    uint64_t epoch;
    uint32_t off;
    uint32_t len;
};

/* Self-contained masks: retiring history cannot change an existing view.
 * Zero-length parts must not trigger a fragment GET. The identities supplied
 * to the loader must be the ones captured alongside this view. */
struct efs_fence_view {
    uint64_t fence_epoch; /* captured authority stamp, independent of part age */
    uint64_t revision;
    uint32_t chunk_size;
    uint32_t count;
    struct efs_fence_part parts[EFS_FENCE_PART_MAX];
};

static inline int efs_fence_history_valid(const struct efs_fence_history *h)
{
    if (!h || h->count > EFS_FENCE_HISTORY_MAX)
        return EFS_ERR_INVAL;
    for (uint32_t i = 0; i < h->count; ++i)
        if (!h->entries[i].epoch ||
            (i && h->entries[i - 1].epoch >= h->entries[i].epoch))
            return EFS_ERR_INVAL;
    return EFS_OK;
}

/* Append, never replace. Failure leaves the history unchanged. Retrying the
 * exact last fence is idempotent even when the history is full. */
static inline int efs_fence_history_append(struct efs_fence_history *h,
                                          uint64_t epoch, uint64_t size)
{
    if (efs_fence_history_valid(h) != EFS_OK || !epoch)
        return EFS_ERR_INVAL;
    if (h->count && epoch <= h->entries[h->count - 1].epoch) {
        const struct efs_content_fence *last = &h->entries[h->count - 1];
        return epoch == last->epoch && size == last->size ?
               EFS_OK : EFS_ERR_INVAL;
    }
    if (h->count == EFS_FENCE_HISTORY_MAX)
        return EFS_ERR_BUSY;
    h->entries[h->count++] = (struct efs_content_fence){epoch, size};
    return EFS_OK;
}

static inline uint64_t efs_fence_valid_end(const struct efs_fence_history *h,
                                          uint64_t part_epoch)
{
    uint64_t end = UINT64_MAX;
    for (uint32_t i = 0; i < h->count; ++i)
        if (h->entries[i].epoch > part_epoch && h->entries[i].size < end)
            end = h->entries[i].size;
    return end;
}

/* Call only after resolving authority. Revision is the fence revision to
 * compare, together with the exact source row version, at a sweep CAS. */
static inline int efs_fence_view_build(struct efs_fence_view *out,
                                      uint64_t revision, uint64_t chunk_start,
                                      uint32_t chunk_size,
                                      const struct efs_fence_history *history,
                                      const struct efs_fence_part *parts,
                                      uint32_t count)
{
    struct efs_fence_view v = {0};
    if (!out || !chunk_size || count > EFS_FENCE_PART_MAX ||
        (count && !parts) ||
        chunk_start > UINT64_MAX - chunk_size ||
        efs_fence_history_valid(history) != EFS_OK)
        return EFS_ERR_INVAL;
    v.fence_epoch = revision;
    v.revision = revision;
    v.chunk_size = chunk_size;
    v.count = count;
    for (uint32_t i = 0; i < count; ++i) {
        uint64_t end;
        if (parts[i].epoch > revision || parts[i].off > chunk_size ||
            parts[i].len > chunk_size - parts[i].off)
            return EFS_ERR_INVAL;
        v.parts[i] = parts[i];
        end = efs_fence_valid_end(history, parts[i].epoch);
        if (end <= chunk_start + parts[i].off)
            v.parts[i].len = 0;
        else if (end - chunk_start - parts[i].off < parts[i].len)
            v.parts[i].len = (uint32_t)(end - chunk_start - parts[i].off);
    }
    if (history->count && history->entries[history->count - 1].epoch > revision)
        return EFS_ERR_INVAL;
    *out = v;
    return EFS_OK;
}

/* Load exactly the requested decoded byte slice of captured object `part`.
 * A loader may decode a whole object internally. Never paint an unmasked
 * span and zero its tail afterwards: that would erase surviving older data.
 * On error the caller must discard dst (it may contain a partial image). */
typedef int (*efs_fence_load_fn)(void *ctx, uint32_t part, uint32_t off,
                                 uint32_t len, uint8_t *dst);

static inline int efs_fence_materialize(const struct efs_fence_view *view,
                                       uint8_t *dst, uint32_t capacity,
                                       efs_fence_load_fn load, void *ctx)
{
    if (!view || !dst || !load || !view->chunk_size ||
        capacity < view->chunk_size || view->count > EFS_FENCE_PART_MAX)
        return EFS_ERR_INVAL;
    for (uint32_t i = 0; i < view->count; ++i)
        if (view->parts[i].off > view->chunk_size ||
            view->parts[i].len > view->chunk_size - view->parts[i].off)
            return EFS_ERR_INVAL;
    memset(dst, 0, view->chunk_size);
    for (uint32_t i = 0; i < view->count; ++i) {
        const struct efs_fence_part *p = &view->parts[i];
        if (p->len) {
            int rc = load(ctx, i, p->off, p->len, dst + p->off);
            if (rc != EFS_OK)
                return rc;
        }
    }
    return EFS_OK;
}

#endif
