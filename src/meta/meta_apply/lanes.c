/* meta_apply/lanes.c: lane activation, content fences, resize, lane
 * bootstrap, writer views. */

#include "apply_internal.h"
#include "lanes.h"
#include "rows.h"
#include "lookup.h"

static int activate_lanes(struct efs_kv *kv, efs_ino_t ino, uint64_t mask,
                          int check_epoch, uint64_t expected_epoch)
{
    struct efs_meta_row row;
    uint8_t key[EFS_KV_KEY_MAX], val[INO_VAL];
    uint32_t klen = 0;
    int rc;

    if (!kv || ino == 0)
        return EFS_ERR_INVAL;
    if (mask == 0)
        return EFS_OK;
    rc = efs_meta_apply_get_inode(kv, ino, &row);
    /* P3: the inode was unlinked between the host's read and this apply —
     * the activation is stale work, harmless to drop. */
    if (rc == EFS_ERR_NOT_FOUND)
        return EFS_OK;
    if (rc != EFS_OK)
        return rc;
    if ((row.active_lanes & mask) == mask)
        return EFS_OK; /* idempotent replay */
    if (check_epoch && row.content_epoch != expected_epoch)
        return EFS_ERR_STALE;
    {
        /* D25 §3.4: a lane registered now cannot hold rows older than the
         * current epoch, so it is done for every existing fence entry. */
        struct efs_fence_history h;
        uint8_t hk[EFS_KV_KEY_MAX], hv[EFS_META_FENCE_BYTES];
        uint32_t hkl = 0, hvl = 0;
        struct efs_kv_item it[2];
        uint64_t added = mask & ~row.active_lanes;
        rc = fence_history_read(kv, ino, row.generation, EFS_META_FENCE_INODE,
                                NULL, NULL, NULL, &h);
        if (rc != EFS_OK)
            return rc;
        row.active_lanes |= mask;
        pack_inode(val, &row);
        rc = efs_kv_key_inode(efs_kv_inode_shard(ino), ino, key, &klen);
        if (rc != EFS_OK)
            return rc;
        it[0] = (struct efs_kv_item){EFS_KV_PUT, key, klen, val, INO_VAL};
        if (!h.count)
            return meta_write_batch(kv, it, 1);
        for (uint32_t i = 0; i < h.count; i++)
            h.entries[i].done |= added;
        rc = content_fence_key(ino, row.generation, EFS_META_FENCE_INODE, hk, &hkl);
        if (rc == EFS_OK)
            rc = efs_meta_pack_fence_history(&h, hv, sizeof(hv), &hvl);
        if (rc != EFS_OK)
            return rc;
        it[1] = (struct efs_kv_item){EFS_KV_PUT, hk, hkl, hv, hvl};
        return meta_write_batch(kv, it, 2);
    }
}

int efs_meta_apply_activate_lanes(struct efs_kv *kv, efs_ino_t ino,
                                  uint64_t mask)
{
    return activate_lanes(kv, ino, mask, 0, 0);
}

int efs_meta_apply_activate_lanes_at(struct efs_kv *kv, efs_ino_t ino,
                                     uint64_t mask, uint64_t expected_epoch)
{
    return activate_lanes(kv, ino, mask, 1, expected_epoch);
}

int efs_meta_apply_activate_lane(struct efs_kv *kv, efs_ino_t ino,
                                 uint8_t lane)
{
    if (lane >= EFS_META_LANES)
        return EFS_ERR_INVAL;
    return efs_meta_apply_activate_lanes(kv, ino, 1ULL << lane);
}

int efs_meta_apply_content_fence(struct efs_kv *kv, efs_ino_t ino,
                                 uint64_t gen, uint8_t authority,
                                 uint64_t expected_epoch, uint64_t epoch,
                                 uint64_t size, uint64_t now)
{
    struct efs_fence_history history;
    struct efs_meta_row row;
    struct lane_rec lane;
    struct efs_kv_item items[3] = {0};
    uint8_t hk[EFS_KV_KEY_MAX], hv[EFS_META_FENCE_BYTES];
    uint8_t key[EFS_KV_KEY_MAX], value[INO_VAL];
    uint32_t hkl = 0, hvl = 0, kl = 0, vl = 0;
    uint8_t ak[EFS_KV_KEY_MAX], av[EFS_META_LANE_AUTHORITY_BYTES];
    uint32_t akl = 0, avl = sizeof(av), acs = 0, count = 2;
    uint64_t ae = 0, af = 0;
    uint64_t current;
    int rc;
    if (!kv || !ino || !gen || authority > EFS_META_FENCE_INODE ||
        expected_epoch == UINT64_MAX || epoch != expected_epoch + 1)
        return EFS_ERR_INVAL;
    if (authority == EFS_META_FENCE_INODE) {
        rc = efs_meta_apply_get_inode(kv, ino, &row);
        if (rc != EFS_OK)
            return rc;
        if (row.generation != gen)
            return EFS_ERR_STALE;
        if (!S_ISREG(row.mode))
            return EFS_ERR_INVAL;
        rc = efs_meta_pub_pinned(kv, ino, gen);
        if (rc != EFS_OK)
            return rc;
        current = row.content_epoch;
        rc = efs_kv_key_inode(efs_kv_inode_shard(ino), ino, key, &kl);
    } else {
        rc = efs_kv_key_lane(efs_kv_lane_shard(ino, authority), ino, gen,
                              authority, key, &kl);
        if (rc != EFS_OK)
            return rc;
        vl = sizeof(value);
        rc = efs_kv_get(kv, key, kl, value, &vl);
        memset(&lane, 0, sizeof(lane));
        if (rc == EFS_OK)
            rc = unpack_lane(value, vl, &lane);
        else if (rc == EFS_ERR_NOT_FOUND)
            rc = EFS_OK;
        current = lane.fenced_epoch;
    }
    if (rc != EFS_OK)
        return rc;
    rc = efs_meta_get_fence_history(kv, ino, gen, authority, &history);
    if (rc != EFS_OK)
        return rc;
    for (uint32_t i = 0; i < history.count; ++i) {
        if (history.entries[i].epoch > current)
            return EFS_ERR_PROTO;
        if (history.entries[i].epoch == epoch)
            return history.entries[i].size == size ? EFS_OK : EFS_ERR_STALE;
    }
    if (current != expected_epoch)
        return EFS_ERR_STALE;
    rc = efs_fence_history_append(&history, epoch, size);
    if (rc != EFS_OK)
        return rc; /* in particular BUSY: no stamp or size was changed */
    rc = content_fence_key(ino, gen, authority, hk, &hkl);
    if (rc == EFS_OK)
        rc = efs_meta_pack_fence_history(&history, hv, sizeof(hv), &hvl);
    if (rc != EFS_OK)
        return rc;
    if (authority == EFS_META_FENCE_INODE) {
        row.content_epoch = epoch;
        row.base_size = size;
        row.base_mtime = max_u64(row.base_mtime, now);
        row.base_ctime = max_u64(row.base_ctime, now);
        pack_inode(value, &row);
        vl = INO_VAL;
    } else {
        lane.fenced_epoch = epoch;
        lane.max_end = 0;
        lane.max_mtime = max_u64(lane.max_mtime, now);
        lane.max_ctime = max_u64(lane.max_ctime, now);
        lane.seq++;
        pack_lane(value, &lane);
        vl = LANE_VAL;
    }
    items[0] = (struct efs_kv_item){EFS_KV_PUT, hk, hkl, hv, hvl};
    items[1] = (struct efs_kv_item){EFS_KV_PUT, key, kl, value, vl};
    if (authority != EFS_META_FENCE_INODE) {
        rc = lane_authority_key(ino, gen, authority, ak, &akl);
        if (rc == EFS_OK)
            rc = efs_kv_get(kv, ak, akl, av, &avl);
        if (rc == EFS_OK) {
            rc = unpack_lane_authority(av, avl, &acs, &ae, &af);
            if (rc != EFS_OK)
                return rc;
            if (ae != expected_epoch)
                return EFS_ERR_BUSY;
            pack_lane_authority(av, acs, epoch, max_u64(af, writer_history_floor(epoch, &history)));
            items[count++] = (struct efs_kv_item){EFS_KV_PUT, ak, akl, av, sizeof(av)};
        } else if (rc != EFS_ERR_NOT_FOUND) {
            return rc;
        }
    }
    return meta_write_batch(kv, items, count);
}

/* A lane may skip past earlier global fences only while it has no chunks.
 * Stop on the first row: this is an existence probe, never a range drain. */
static int resize_nonempty_lane(void *ctx, const uint8_t *key, uint32_t kl,
                                const uint8_t *value, uint32_t vl)
{
    (void)ctx; (void)key; (void)kl; (void)value; (void)vl;
    return EFS_ERR_PROTO;
}

int efs_meta_prepare_content_fence(struct efs_kv *kv, const struct efs_txid *t,
                                   const struct efs_txn_parts *parts,
                                   efs_ino_t ino, uint64_t gen, uint8_t authority,
                                   uint64_t expected_epoch, uint64_t expected_seq,
                                   uint64_t expected_active_lanes,
                                   uint64_t expected_base_size,
                                   uint64_t epoch, uint64_t size, uint64_t now)
{
    struct efs_fence_history history;
    struct efs_meta_row row;
    struct lane_rec lane = {0};
    uint8_t key[EFS_KV_KEY_MAX], hk[EFS_KV_KEY_MAX];
    uint8_t old[INO_VAL], value[INO_VAL];
    uint8_t old_history[EFS_META_FENCE_BYTES], new_history[EFS_META_FENCE_BYTES];
    uint32_t kl = 0, hkl = 0, old_len = sizeof(old), vl;
    uint32_t old_hlen = sizeof(old_history), hlen = 0;
    struct efs_txn_value_cas requests[3];
    uint8_t auth_key[EFS_KV_KEY_MAX], auth_old[EFS_META_LANE_AUTHORITY_BYTES],
            auth_new[EFS_META_LANE_AUTHORITY_BYTES];
    uint32_t auth_kl = 0, auth_len = sizeof(auth_old), auth_cs = 0;
    uint64_t auth_epoch = 0, auth_floor = 0;
    int has_authority = 0;
    uint32_t count = 1;
    int rc;
    if (!kv || !t || !parts || !parts->n || parts->n > EFS_TXN_MAX_PART ||
        !ino || !gen || authority > EFS_META_FENCE_INODE ||
        expected_epoch == UINT64_MAX || epoch != expected_epoch + 1)
        return EFS_ERR_INVAL;
    if (authority == EFS_META_FENCE_INODE &&
        (rc = efs_meta_pub_pinned(kv, ino, gen)) != EFS_OK)
        return rc;
    if (authority == EFS_META_FENCE_INODE)
        rc = efs_kv_key_inode(efs_kv_inode_shard(ino), ino, key, &kl);
    else
        rc = efs_kv_key_lane(efs_kv_lane_shard(ino, authority), ino, gen,
                              authority, key, &kl);
    if (rc != EFS_OK)
        return rc;
    rc = efs_kv_get(kv, key, kl, old, &old_len);
    if (rc == EFS_ERR_NOT_FOUND && authority != EFS_META_FENCE_INODE) {
        old_len = EFS_TXN_ABSENT;
        rc = EFS_OK;
    } else if (rc == EFS_OK) {
        rc = authority == EFS_META_FENCE_INODE ?
             unpack_inode(old, old_len, &row) : unpack_lane(old, old_len, &lane);
    }
    if (rc != EFS_OK)
        return rc;
    if (authority == EFS_META_FENCE_INODE) {
        if (row.generation != gen || row.content_epoch != expected_epoch ||
            row.active_lanes != expected_active_lanes ||
            row.base_size != expected_base_size)
            return EFS_ERR_STALE;
        if (!S_ISREG(row.mode))
            return EFS_ERR_INVAL;
        for (uint32_t i = 0; i < EFS_META_LANES; ++i) {
            int member = 0;
            uint32_t shard;
            if (!(row.active_lanes & (1ull << i)))
                continue;
            shard = efs_kv_lane_shard(ino, (uint8_t)i);
            for (uint32_t j = 0; j < parts->n; ++j)
                member |= parts->shard[j] == shard;
            if (!member)
                return EFS_ERR_INVAL;
        }
    } else if (lane.fenced_epoch != expected_epoch || lane.seq != expected_seq) {
        return EFS_ERR_STALE;
    }
    if (authority != EFS_META_FENCE_INODE) {
        rc = lane_authority_key(ino, gen, authority, auth_key, &auth_kl);
        if (rc == EFS_OK)
            rc = efs_kv_get(kv, auth_key, auth_kl, auth_old, &auth_len);
        if (rc == EFS_OK) {
            rc = unpack_lane_authority(auth_old, auth_len, &auth_cs, &auth_epoch, &auth_floor);
            if (rc != EFS_OK)
                return rc;
            if (auth_epoch != expected_epoch)
                return EFS_ERR_BUSY;
            has_authority = 1;
        } else if (rc != EFS_ERR_NOT_FOUND) {
            return rc;
        }
    }
    {
        rc = content_fence_key(ino, gen, authority, hk, &hkl);
        if (rc != EFS_OK)
            return rc;
        rc = efs_kv_get(kv, hk, hkl, old_history, &old_hlen);
        if (rc == EFS_ERR_NOT_FOUND) {
            memset(&history, 0, sizeof(history));
            old_hlen = EFS_TXN_ABSENT;
        } else if (rc == EFS_OK) {
            rc = efs_meta_unpack_fence_history(old_history, old_hlen, &history);
        }
        if (rc != EFS_OK && rc != EFS_ERR_NOT_FOUND)
            return rc;
        if (history.count && history.entries[history.count - 1].epoch > expected_epoch)
            return EFS_ERR_PROTO;
        rc = efs_fence_history_append(&history, epoch, size);
        if (rc != EFS_OK)
            return rc;
        rc = efs_meta_pack_fence_history(&history, new_history, sizeof(new_history), &hlen);
        if (rc != EFS_OK)
            return rc;
    }
    if (authority == EFS_META_FENCE_INODE) {
        row.content_epoch = epoch;
        row.base_size = size;
        row.base_mtime = max_u64(row.base_mtime, now);
        row.base_ctime = max_u64(row.base_ctime, now);
        pack_inode(value, &row);
        vl = INO_VAL;
    } else {
        lane.fenced_epoch = epoch;
        lane.max_end = 0;
        lane.max_mtime = max_u64(lane.max_mtime, now);
        lane.max_ctime = max_u64(lane.max_ctime, now);
        ++lane.seq;
        pack_lane(value, &lane);
        vl = LANE_VAL;
    }
    requests[0] = (struct efs_txn_value_cas){key, kl, old, old_len,
                                            EFS_TXN_PUT, value, vl};
    requests[count++] = (struct efs_txn_value_cas){hk, hkl, old_history, old_hlen,
                                                   EFS_TXN_PUT, new_history, hlen};
    if (has_authority) {
        uint64_t floor = max_u64(writer_history_floor(epoch, &history), auth_floor);
        pack_lane_authority(auth_new, auth_cs, epoch, floor);
        requests[count++] = (struct efs_txn_value_cas){auth_key, auth_kl,
            auth_old, auth_len, EFS_TXN_PUT, auth_new, sizeof(auth_new)};
    }
    return efs_txn_prepare_excl_batch(kv, t, parts, requests, count);
}

int efs_meta_encode_fence_prepare(const struct efs_meta_fence_prepare *q,
                                  uint8_t *key, uint32_t *kl,
                                  uint8_t pay[EFS_META_FENCE_PREPARE_BYTES])
{
    int rc;
    if (!q || !key || !kl || !pay || !q->ino || !q->generation ||
        q->authority > EFS_META_FENCE_INODE || q->expected_epoch == UINT64_MAX)
        return EFS_ERR_INVAL;
    if (q->authority == EFS_META_FENCE_INODE)
        rc = efs_kv_key_inode(efs_kv_inode_shard(q->ino), q->ino, key, kl);
    else
        rc = efs_kv_key_lane(efs_kv_lane_shard(q->ino, q->authority), q->ino,
                              q->generation, q->authority, key, kl);
    if (rc != EFS_OK)
        return rc;
    be64(pay, q->ino);
    be64(pay + 8, q->generation);
    pay[16] = q->authority;
    be64(pay + 17, q->expected_epoch);
    be64(pay + 25, q->expected_seq);
    be64(pay + 33, q->expected_active_lanes);
    be64(pay + 41, q->expected_base_size);
    be64(pay + 49, q->size);
    be64(pay + 57, q->now);
    return EFS_OK;
}

int efs_meta_apply_fence_prepare(struct efs_kv *kv, const struct efs_txid *t,
                                 const struct efs_txn_parts *parts,
                                 const uint8_t *key, uint32_t kl,
                                 const uint8_t *pay, uint32_t plen)
{
    struct efs_meta_fence_prepare q;
    uint8_t expected_key[EFS_KV_KEY_MAX], encoded[EFS_META_FENCE_PREPARE_BYTES];
    uint32_t expected_kl = 0;
    int rc;
    if (!key || !pay || plen != EFS_META_FENCE_PREPARE_BYTES)
        return EFS_ERR_PROTO;
    q = (struct efs_meta_fence_prepare){rd64(pay), rd64(pay + 8), pay[16],
          rd64(pay + 17), rd64(pay + 25), rd64(pay + 33), rd64(pay + 41),
          rd64(pay + 49), rd64(pay + 57)};
    rc = efs_meta_encode_fence_prepare(&q, expected_key, &expected_kl, encoded);
    if (rc != EFS_OK || kl != expected_kl || memcmp(key, expected_key, kl))
        return EFS_ERR_PROTO; /* never mutate a different shard than the command key */
    return efs_meta_prepare_content_fence(kv, t, parts, q.ino, q.generation,
                                           q.authority, q.expected_epoch,
                                           q.expected_seq, q.expected_active_lanes,
                                           q.expected_base_size,
                                           q.expected_epoch + 1, q.size, q.now);
}

int resize_parts_add(struct efs_txn_parts *parts, uint32_t shard)
{
    for (uint32_t i = 0; i < parts->n; ++i)
        if (parts->shard[i] == shard)
            return EFS_OK;
    if (parts->n == EFS_TXN_MAX_PART)
        return EFS_ERR_INVAL;
    parts->shard[parts->n++] = shard;
    return EFS_OK;
}

int resize_lane_read(struct efs_kv *kv, efs_ino_t ino, uint64_t gen,
                             uint8_t lane, efs_txn_coord_fn coord, void *ctx,
                             struct efs_txn_pending *pend,
                             uint8_t val[LANE_VAL], uint32_t *len,
                             struct lane_rec *stamp)
{
    uint8_t key[EFS_KV_KEY_MAX];
    uint32_t kl = 0;
    int rc = efs_kv_key_lane(efs_kv_lane_shard(ino, lane), ino, gen, lane, key, &kl);
    *len = LANE_VAL;
    if (rc == EFS_OK)
        rc = efs_txn_read_ex(kv, key, kl, coord, ctx, val, len, pend);
    if (rc == EFS_ERR_NOT_FOUND) {
        *len = 0;
        memset(stamp, 0, sizeof(*stamp));
        return EFS_OK;
    }
    return rc == EFS_OK ? efs_kv_decode_result(kv, key, kl, unpack_lane(val, *len, stamp)) : rc;
}

int efs_meta_get_lane_writer_view(struct efs_kv *kv, efs_ino_t ino,
                                  uint64_t gen, uint32_t ci, uint32_t cs,
                                  struct efs_meta_writer_view *out)
{
    uint8_t lane = ci % EFS_META_LANES;
    uint8_t keys[3][EFS_KV_KEY_MAX], before[3][EFS_META_FENCE_BYTES],
            after[3][EFS_META_FENCE_BYTES];
    uint32_t kl[3], bl[3], al[3];
    if (!kv || !out || !efs_chunk_size_valid(cs))
        return EFS_ERR_INVAL;
    int rc = lane_authority_key(ino, gen, lane, keys[0], &kl[0]);
    if (rc == EFS_OK)
        rc = efs_kv_key_lane(efs_kv_lane_shard(ino, lane), ino, gen, lane, keys[1], &kl[1]);
    if (rc == EFS_OK)
        rc = content_fence_key(ino, gen, lane, keys[2], &kl[2]);
    if (rc != EFS_OK)
        return rc;
    for (unsigned attempt = 0; attempt < 4; ++attempt) {
        struct efs_meta_writer_view view = {.ino = ino, .generation = gen};
        struct lane_rec stamp;
        uint32_t got_cs;
        int stable = 1;
        for (unsigned pass = 0; pass < 2; ++pass) {
            for (unsigned i = 0; i < 3; ++i) {
                uint32_t *n = pass ? &al[i] : &bl[i];
                uint8_t *v = pass ? after[i] : before[i];
                rc = efs_txn_key_exclusive(kv, keys[i], kl[i]);
                if (rc != EFS_OK)
                    return rc;
                *n = EFS_META_FENCE_BYTES;
                rc = efs_kv_get(kv, keys[i], kl[i], v, n);
                /* Absence of the authority itself is a cold bootstrap request;
                 * a partially installed stamp/history is never a valid view. */
                if (rc != EFS_OK)
                    return rc == EFS_ERR_NOT_FOUND && i ? EFS_ERR_BUSY : rc;
            }
        }
        for (unsigned i = 0; i < 3; ++i) {
            rc = efs_txn_key_exclusive(kv, keys[i], kl[i]);
            if (rc != EFS_OK)
                return rc;
            stable &= bl[i] == al[i] && !memcmp(before[i], after[i], bl[i]);
        }
        if (!stable)
            continue;
        rc = efs_kv_decode_result(kv, keys[0], kl[0],
                unpack_lane_authority(before[0], bl[0], &got_cs,
                                      &view.authority_epoch, &view.oldest_complete_epoch));
        if (rc == EFS_OK)
            rc = efs_kv_decode_result(kv, keys[1], kl[1], unpack_lane(before[1], bl[1], &stamp));
        if (rc == EFS_OK)
            rc = efs_kv_decode_result(kv, keys[2], kl[2],
                    efs_meta_unpack_fence_history(before[2], bl[2], &view.history));
        if (rc != EFS_OK)
            return rc;
        if (got_cs != cs)
            return EFS_ERR_STALE;
        if (stamp.fenced_epoch != view.authority_epoch)
            return EFS_ERR_BUSY;
        if ((view.history.count && view.history.entries[view.history.count - 1].epoch >
             view.authority_epoch) || view.oldest_complete_epoch <
             writer_history_floor(view.authority_epoch, &view.history))
            return EFS_ERR_PROTO;
        *out = view;
        return EFS_OK;
    }
    return EFS_ERR_BUSY;
}

int efs_meta_capture_lane_bootstrap(struct efs_kv *kv, efs_ino_t ino,
                                    uint64_t gen, uint32_t ci, uint32_t cs,
                                    efs_txn_coord_fn coord, void *ctx,
                                    struct efs_meta_lane_bootstrap *out)
{
    if (!kv || !out || !coord || !efs_chunk_size_valid(cs))
        return EFS_ERR_INVAL;
    struct efs_meta_lane_bootstrap q = {.chunk_size = cs, .lane = ci % EFS_META_LANES};
    struct efs_meta_writer_view again;
    struct efs_meta_row row;
    struct lane_rec stamp;
    struct efs_txn_pending pending = {0};
    uint8_t value[LANE_VAL];
    uint32_t len;
    int moved = 0;
    int rc = efs_meta_get_writer_view_tx(kv, ino, gen, coord, ctx, &q.view);
    if (rc == EFS_OK)
        rc = inode_read_tx(kv, ino, coord, ctx, &pending, &row);
    if (rc == EFS_OK)
        rc = resize_lane_read(kv, ino, gen, q.lane, coord, ctx, &pending, value, &len, &stamp);
    if (rc == EFS_OK)
        rc = efs_meta_get_writer_view_tx(kv, ino, gen, coord, ctx, &again);
    if (rc == EFS_OK)
        rc = efs_txn_pending_recheck(&pending, coord, ctx, &moved);
    if (rc != EFS_OK)
        return rc;
    if (moved || memcmp(&q.view, &again, sizeof(again)) || row.generation != gen ||
        row.content_epoch != q.view.authority_epoch)
        return EFS_ERR_BUSY;
    if (len && stamp.fenced_epoch != q.view.authority_epoch)
        return EFS_ERR_BUSY; /* lagging populated lanes must finish coordinated fencing */
    q.active_lanes = row.active_lanes; q.base_size = row.base_size; q.mtime_gen = row.mtime_gen;
    q.lane_epoch = stamp.fenced_epoch; q.lane_seq = stamp.seq; q.lane_present = len != 0;
    *out = q;
    return EFS_OK;
}

static int lane_bootstrap_valid(const struct efs_meta_lane_bootstrap *q, uint8_t authority)
{
    if (!q || !q->view.ino || !q->view.generation || q->lane >= EFS_META_LANES ||
        (authority != q->lane && authority != EFS_META_FENCE_INODE) ||
        q->lane_present > 1 || !efs_chunk_size_valid(q->chunk_size) ||
        efs_fence_history_valid(&q->view.history) != EFS_OK ||
        q->view.oldest_complete_epoch > q->view.authority_epoch ||
        q->view.oldest_complete_epoch < writer_history_floor(q->view.authority_epoch, &q->view.history) ||
        (q->view.history.count && q->view.history.entries[q->view.history.count - 1].epoch > q->view.authority_epoch) ||
        (q->lane_present && q->lane_epoch != q->view.authority_epoch) ||
        (!q->lane_present && (q->lane_epoch || q->lane_seq)))
        return EFS_ERR_INVAL;
    return EFS_OK;
}

int efs_meta_encode_lane_bootstrap(const struct efs_meta_lane_bootstrap *q,
                                   uint8_t authority, uint8_t *key, uint32_t *kl,
                                   uint8_t pay[EFS_META_LANE_BOOTSTRAP_BYTES])
{
    if (!key || !kl || !pay || lane_bootstrap_valid(q, authority) != EFS_OK)
        return EFS_ERR_INVAL;
    int rc = authority == EFS_META_FENCE_INODE ?
        efs_kv_key_inode(efs_kv_inode_shard(q->view.ino), q->view.ino, key, kl) :
        efs_kv_key_lane(efs_kv_lane_shard(q->view.ino, q->lane), q->view.ino,
                         q->view.generation, q->lane, key, kl);
    if (rc != EFS_OK)
        return rc;
    memset(pay, 0, EFS_META_LANE_BOOTSTRAP_BYTES);
    be64(pay, q->view.ino); be64(pay + 8, q->view.generation);
    be64(pay + 16, q->view.authority_epoch); be64(pay + 24, q->view.oldest_complete_epoch);
    be64(pay + 32, q->active_lanes); be64(pay + 40, q->base_size);
    be64(pay + 48, q->lane_epoch); be64(pay + 56, q->lane_seq);
    efs_store_be32(pay + 64, q->chunk_size); pay[68] = q->lane; pay[69] = q->lane_present;
    pay[70] = authority; be64(pay + 72, q->mtime_gen);
    uint32_t len;
    return efs_meta_pack_fence_history(&q->view.history, pay + 80, EFS_META_FENCE_BYTES, &len);
}

int efs_meta_apply_lane_bootstrap(struct efs_kv *kv, const struct efs_txid *t,
                                  const struct efs_txn_parts *parts,
                                  const uint8_t *key, uint32_t kl,
                                  const uint8_t *pay, uint32_t plen)
{
    struct efs_meta_lane_bootstrap q = {0};
    uint8_t expected[EFS_KV_KEY_MAX], encoded[EFS_META_LANE_BOOTSTRAP_BYTES];
    uint8_t hk[EFS_KV_KEY_MAX], ak[EFS_KV_KEY_MAX];
    uint8_t old[INO_VAL], value[INO_VAL], hv[EFS_META_FENCE_BYTES], av[EFS_META_LANE_AUTHORITY_BYTES];
    uint32_t len, hkl, akl, old_len = sizeof(old), hlen = sizeof(hv), vl;
    struct efs_txn_value_cas requests[3];
    if (!kv || !t || !parts || !key || !pay || plen != EFS_META_LANE_BOOTSTRAP_BYTES ||
        efs_load_be32(pay + 80 + 4) > EFS_FENCE_HISTORY_MAX)
        return EFS_ERR_PROTO;
    q.view.ino = rd64(pay); q.view.generation = rd64(pay + 8);
    q.view.authority_epoch = rd64(pay + 16); q.view.oldest_complete_epoch = rd64(pay + 24);
    q.active_lanes = rd64(pay + 32); q.base_size = rd64(pay + 40);
    q.lane_epoch = rd64(pay + 48); q.lane_seq = rd64(pay + 56);
    q.chunk_size = efs_load_be32(pay + 64); q.lane = pay[68]; q.lane_present = pay[69];
    uint8_t authority = pay[70]; q.mtime_gen = rd64(pay + 72);
    int rc = efs_meta_unpack_fence_history(pay + 80, 16u + 24u * efs_load_be32(pay + 84), &q.view.history);
    if (rc != EFS_OK || efs_meta_encode_lane_bootstrap(&q, authority, expected, &len, encoded) != EFS_OK ||
        kl != len || memcmp(key, expected, kl) || memcmp(pay, encoded, plen))
        return EFS_ERR_PROTO;
    /* Freeze the bitmap before adopting the lane. Both shards must participate,
     * even when inode and lane happen to share a shard or a Raft group. */
    struct efs_txn_parts needed = {0};
    resize_parts_add(&needed, efs_kv_inode_shard(q.view.ino));
    resize_parts_add(&needed, efs_kv_lane_shard(q.view.ino, q.lane));
    if (parts->n != needed.n || memcmp(parts->shard, needed.shard, needed.n * sizeof(uint32_t)))
        return EFS_ERR_INVAL;
    rc = efs_kv_get(kv, key, kl, old, &old_len);
    if (rc == EFS_ERR_NOT_FOUND && authority != EFS_META_FENCE_INODE) {
        old_len = EFS_TXN_ABSENT; rc = EFS_OK;
    }
    if (rc != EFS_OK)
        return rc;
    rc = content_fence_key(q.view.ino, q.view.generation, authority, hk, &hkl);
    if (rc == EFS_OK)
        rc = efs_kv_get(kv, hk, hkl, hv, &hlen);
    if (rc == EFS_ERR_NOT_FOUND) { hlen = EFS_TXN_ABSENT; rc = EFS_OK; }
    if (rc != EFS_OK)
        return rc;
    if (authority == EFS_META_FENCE_INODE) {
        struct efs_meta_row row;
        struct efs_fence_history history = {0};
        rc = unpack_inode(old, old_len, &row);
        if (rc == EFS_OK && hlen != EFS_TXN_ABSENT)
            rc = efs_meta_unpack_fence_history(hv, hlen, &history);
        if (rc != EFS_OK)
            return rc;
        if (row.generation != q.view.generation || row.content_epoch != q.view.authority_epoch ||
            row.active_lanes != q.active_lanes || row.base_size != q.base_size || row.mtime_gen != q.mtime_gen ||
            memcmp(&history, &q.view.history, sizeof(history)) ||
            q.view.oldest_complete_epoch != writer_history_floor(row.content_epoch, &history))
            return EFS_ERR_STALE;
        if (!S_ISREG(row.mode))
            return EFS_ERR_INVAL;
        row.active_lanes |= 1ull << q.lane;
        pack_inode(value, &row); vl = INO_VAL;
    } else {
        struct lane_rec stamp = {0};
        if (old_len != EFS_TXN_ABSENT && unpack_lane(old, old_len, &stamp) != EFS_OK)
            return EFS_ERR_PROTO;
        if ((old_len != EFS_TXN_ABSENT) != q.lane_present || stamp.seq != q.lane_seq ||
            stamp.fenced_epoch != q.lane_epoch)
            return EFS_ERR_STALE;
        if (stamp.seq == UINT64_MAX || stamp.append_bar)
            return EFS_ERR_BUSY;
        /* A missing stamp with existing chunks cannot be re-aged as a new lane. */
        if (!q.lane_present) {
            uint8_t prefix[EFS_KV_KEY_MAX]; uint32_t pl;
            rc = efs_kv_key_chunk(efs_kv_lane_shard(q.view.ino, q.lane), q.view.ino,
                                  q.view.generation, q.lane, 0, prefix, &pl);
            if (rc == EFS_OK)
                rc = efs_kv_scan_prefix(kv, prefix, pl - 4, resize_nonempty_lane, NULL);
            if (rc != EFS_OK)
                return rc;
        }
        if (stamp.mtime_gen > q.mtime_gen)
            return EFS_ERR_BUSY;
        if (stamp.mtime_gen < q.mtime_gen) {
            stamp.mtime_gen = q.mtime_gen;
            stamp.max_mtime = 0;
        }
        stamp.fenced_epoch = q.view.authority_epoch; ++stamp.seq;
        pack_lane(value, &stamp); vl = LANE_VAL;
        lane_authority_key(q.view.ino, q.view.generation, q.lane, ak, &akl);
        pack_lane_authority(av, q.chunk_size, q.view.authority_epoch, q.view.oldest_complete_epoch);
    }
    requests[0] = (struct efs_txn_value_cas){key, kl, old, old_len, EFS_TXN_PUT, value, vl};
    uint32_t nh = 16u + 24u * q.view.history.count;
    requests[1] = (struct efs_txn_value_cas){hk, hkl, hv, hlen, EFS_TXN_PUT, pay + 80, nh};
    if (authority != EFS_META_FENCE_INODE)
        requests[2] = (struct efs_txn_value_cas){ak, akl, NULL, EFS_TXN_ABSENT, EFS_TXN_PUT, av, sizeof(av)};
    return efs_txn_prepare_excl_batch(kv, t, parts, requests,
                                      authority == EFS_META_FENCE_INODE ? 2 : 3);
}

int efs_meta_get_writer_chunk_view_tx(struct efs_kv *kv, efs_ino_t ino,
                                      uint64_t gen, uint32_t ci,
                                      efs_txn_coord_fn coord, void *ctx,
                                      struct efs_meta_writer_view *out)
{
    if (!kv || !ino || !gen || !coord || !out)
        return EFS_ERR_INVAL;
    uint8_t lane = (uint8_t)(ci % EFS_META_LANES);
    for (unsigned attempt = 0; attempt < 4; ++attempt) {
        struct efs_meta_writer_view view, again;
        struct efs_meta_row row;
        struct lane_rec before, after;
        struct efs_txn_pending pend = {0};
        uint8_t b[LANE_VAL], a[LANE_VAL];
        uint32_t bl, al;
        int moved = 0;
        int rc = efs_meta_get_writer_view_tx(kv, ino, gen, coord, ctx, &view);
        if (rc == EFS_OK)
            rc = resize_lane_read(kv, ino, gen, lane, coord, ctx, &pend, b, &bl, &before);
        if (rc == EFS_OK)
            rc = inode_read_tx(kv, ino, coord, ctx, &pend, &row);
        if (rc == EFS_OK)
            rc = resize_lane_read(kv, ino, gen, lane, coord, ctx, &pend, a, &al, &after);
        if (rc == EFS_OK)
            rc = efs_meta_get_writer_view_tx(kv, ino, gen, coord, ctx, &again);
        if (rc == EFS_OK)
            rc = efs_txn_pending_recheck(&pend, coord, ctx, &moved);
        if (rc != EFS_OK)
            return rc;
        if (moved || bl != al || memcmp(b, a, bl) ||
            memcmp(&view, &again, sizeof(view)) || row.generation != gen ||
            row.content_epoch != view.authority_epoch)
            continue;
        if (!bl) {
            if (row.active_lanes & (1ull << lane))
                return EFS_ERR_BUSY;
        } else if (before.fenced_epoch != view.authority_epoch) {
            return EFS_ERR_BUSY;
        }
        *out = view;
        return EFS_OK;
    }
    return EFS_ERR_BUSY;
}
