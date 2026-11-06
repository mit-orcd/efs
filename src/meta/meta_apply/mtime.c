/* meta_apply/mtime.c: the exact-image mtime capture and execute. */

#include "apply_internal.h"
#include "rows.h"
#include "lookup.h"
#include "lanes.h"

int efs_meta_capture_mtime(struct efs_kv *kv, efs_ino_t ino, uint64_t now,
                           uint32_t mask, uint64_t mtime, uint64_t atime,
                           efs_txn_coord_fn coord, void *ctx,
                           struct efs_meta_mtime_plan *out)
{
    struct efs_meta_row row;
    struct efs_txn_pending pending = {0};
    int moved = 0;
    if (!kv || !ino || !coord || !out || !(mask & EFS_META_SET_MTIME) ||
        (mask & ~(EFS_META_SET_MTIME | EFS_META_SET_ATIME)))
        return EFS_ERR_INVAL;
    memset(out, 0, sizeof(*out));
    int rc = inode_read_tx(kv, ino, coord, ctx, &pending, &row);
    if (rc != EFS_OK) return rc;
    if (!S_ISREG(row.mode) || row.mtime_gen == UINT64_MAX)
        return EFS_ERR_INVAL;
    uint64_t generation = row.generation, bits = row.active_lanes;
    uint64_t epoch = row.content_epoch, next = row.mtime_gen + 1;
    struct efs_meta_mtime_image *image = &out->images[out->count++];
    rc = efs_kv_key_inode(efs_kv_inode_shard(ino), ino, image->key, &image->klen);
    if (rc != EFS_OK) return rc;
    image->expected_len = image->new_len = INO_VAL;
    pack_inode(image->expected, &row);
    row.mtime_gen = next; row.base_mtime = mtime;
    if (mask & EFS_META_SET_ATIME) row.base_atime = atime;
    row.base_ctime = max_u64(row.base_ctime, now);
    pack_inode(image->value, &row);
    rc = resize_parts_add(&out->parts, efs_kv_inode_shard(ino));
    for (uint8_t lane = 0; lane < EFS_META_LANES && rc == EFS_OK; ++lane) {
        struct lane_rec stamp;
        uint32_t len;
        if (!(bits & (1ull << lane))) continue;
        image = &out->images[out->count++];
        rc = resize_lane_read(kv, ino, generation, lane, coord, ctx,
                              &pending, image->expected, &len, &stamp);
        if (rc != EFS_OK) return rc;
        /* A lane ahead of the row signals an unfinished legacy fanout. */
        if (stamp.mtime_gen >= next || stamp.seq == UINT64_MAX)
            return EFS_ERR_BUSY;
        /* D25: a lane behind the inode epoch adopts it here; ahead is a race. */
        if (len && stamp.fenced_epoch > epoch) return EFS_ERR_BUSY;
        if (len) (void)lane_adopt(&stamp, epoch);
        image->expected_len = len ? len : EFS_TXN_ABSENT;
        image->new_len = LANE_VAL;
        stamp.fenced_epoch = epoch; stamp.mtime_gen = next;
        stamp.max_mtime = 0; ++stamp.seq;
        pack_lane(image->value, &stamp);
        rc = efs_kv_key_lane(efs_kv_lane_shard(ino, lane), ino, generation,
                             lane, image->key, &image->klen);
        if (rc == EFS_OK) rc = resize_parts_add(&out->parts, efs_kv_lane_shard(ino, lane));
    }
    if (rc == EFS_OK) rc = efs_txn_pending_recheck(&pending, coord, ctx, &moved);
    return rc != EFS_OK ? rc : moved ? EFS_ERR_BUSY : EFS_OK;
}

int efs_meta_execute_mtime(const struct efs_meta_mtime_plan *plan,
                           const struct efs_txid *t,
                           const struct efs_meta_mtime_ops *ops, void *ctx)
{
    struct efs_txn_parts expected = {0};
    struct efs_meta_row before, after;
    uint64_t bits = 0;
    if (!plan || !t || !ops || !ops->prepare || !ops->decide || !ops->resolve ||
        !plan->count || plan->count > EFS_META_LANES + 1)
        return EFS_ERR_INVAL;
    const struct efs_meta_mtime_image *inode = &plan->images[0];
    if (inode->expected_len != INO_VAL || inode->new_len != INO_VAL ||
        unpack_inode(inode->expected, INO_VAL, &before) != EFS_OK ||
        unpack_inode(inode->value, INO_VAL, &after) != EFS_OK ||
        !S_ISREG(before.mode) || before.mtime_gen == UINT64_MAX ||
        after.mtime_gen != before.mtime_gen + 1)
        return EFS_ERR_INVAL;
    struct efs_meta_row unchanged = after;
    unchanged.base_mtime = before.base_mtime; unchanged.base_atime = before.base_atime;
    unchanged.base_ctime = before.base_ctime; unchanged.mtime_gen = before.mtime_gen;
    uint8_t original[INO_VAL], normalized[INO_VAL];
    pack_inode(original, &before); pack_inode(normalized, &unchanged);
    if (memcmp(original, normalized, INO_VAL) || after.base_ctime < before.base_ctime)
        return EFS_ERR_INVAL;
    for (uint32_t i = 0; i < plan->count; ++i) {
        const struct efs_meta_mtime_image *image = &plan->images[i];
        uint8_t key[EFS_KV_KEY_MAX]; uint32_t kl;
        int rc;
        if (!i) rc = efs_kv_key_inode(efs_kv_inode_shard(before.ino), before.ino, key, &kl);
        else {
            struct lane_rec stamp;
            if (image->klen != 20 || image->new_len != LANE_VAL ||
                (image->expected_len != LANE_VAL && image->expected_len != EFS_TXN_ABSENT) ||
                image->key[19] >= EFS_META_LANES ||
                unpack_lane(image->value, LANE_VAL, &stamp) != EFS_OK ||
                stamp.mtime_gen != after.mtime_gen || stamp.max_mtime ||
                stamp.fenced_epoch != before.content_epoch)
                return EFS_ERR_INVAL;
            struct lane_rec old = {0};
            if (image->expected_len != EFS_TXN_ABSENT &&
                unpack_lane(image->expected, LANE_VAL, &old) != EFS_OK)
                return EFS_ERR_INVAL;
            if (old.seq == UINT64_MAX || old.mtime_gen >= after.mtime_gen ||
                (image->expected_len != EFS_TXN_ABSENT && old.fenced_epoch > before.content_epoch))
                return EFS_ERR_INVAL;
            if (image->expected_len != EFS_TXN_ABSENT)
                (void)lane_adopt(&old, before.content_epoch);
            old.fenced_epoch = before.content_epoch; old.mtime_gen = after.mtime_gen;
            old.max_mtime = 0; ++old.seq;
            uint8_t encoded[LANE_VAL]; pack_lane(encoded, &old);
            if (memcmp(encoded, image->value, LANE_VAL)) return EFS_ERR_INVAL;
            uint8_t lane = image->key[19];
            if (bits & (1ull << lane)) return EFS_ERR_INVAL;
            bits |= 1ull << lane;
            rc = efs_kv_key_lane(efs_kv_lane_shard(before.ino, lane), before.ino,
                                  before.generation, lane, key, &kl);
        }
        if (rc != EFS_OK || image->klen != kl || memcmp(key, image->key, kl))
            return EFS_ERR_INVAL;
        rc = resize_parts_add(&expected, ((uint32_t)key[0] << 8) | key[1]);
        if (rc != EFS_OK) return rc;
    }
    if (bits != before.active_lanes || expected.n != plan->parts.n ||
        memcmp(expected.shard, plan->parts.shard, expected.n * sizeof(uint32_t)))
        return EFS_ERR_INVAL;
    for (uint32_t i = 0; i < plan->count; ++i) {
        int rc = ops->prepare(ctx, t, &plan->parts, &plan->images[i]);
        if (rc != EFS_OK) {
            int abort_rc = ops->decide(ctx, t, &plan->parts, EFS_TXN_ABORT);
            if (abort_rc != EFS_OK) return abort_rc;
            (void)ops->resolve(ctx, t, &plan->parts, EFS_TXN_ABORT);
            return rc;
        }
    }
    int rc = ops->decide(ctx, t, &plan->parts, EFS_TXN_COMMIT);
    if (rc != EFS_OK) return rc; /* ambiguous COMMIT never becomes ABORT */
    return ops->resolve(ctx, t, &plan->parts, EFS_TXN_COMMIT);
}
