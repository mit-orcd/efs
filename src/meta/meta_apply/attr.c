/* meta_apply/attr.c: GETATTR, the epoch fence, UTIMENS. */

#include "apply_internal.h"
#include "attr.h"
#include "rows.h"
#include "lookup.h"
#include "append.h"

/* Reads a lane's sequence number. Absent is seq 0, which is a real value:
 * a lane with no record has nothing to contribute and nothing to change. */
int lane_seq_get(struct efs_kv *kv, efs_ino_t ino, uint64_t gen,
                        uint8_t lane, uint64_t *seq)
{
    uint8_t key[EFS_KV_KEY_MAX], val[LANE_VAL];
    uint32_t kl = 0, vn = sizeof(val);
    struct lane_rec ln;
    int rc;

    *seq = 0;
    rc = efs_kv_key_lane(efs_kv_lane_shard(ino, lane), ino, gen, lane, key, &kl);
    if (rc != EFS_OK)
        return rc;
    vn = sizeof(val);
    rc = efs_kv_get(kv, key, kl, val, &vn);
    if (rc == EFS_ERR_NOT_FOUND)
        return EFS_OK;
    if (rc != EFS_OK)
        return rc;
    rc = efs_kv_decode_result(kv, key, kl, unpack_lane(val, vn, &ln));
    if (rc != EFS_OK)
        return rc;
    *seq = ln.seq;
    return EFS_OK;
}

int efs_meta_apply_getattr(struct efs_kv *kv, efs_ino_t ino,
                           efs_txn_coord_fn coord, void *ctx,
                           struct efs_meta_stat *out)
{
    uint64_t seq1[EFS_META_LANES], seq2[EFS_META_LANES];
    uint8_t lanes[EFS_META_LANES];
    uint32_t tries;

    if (!kv || !coord || !out || ino == 0)
        return EFS_ERR_INVAL;
    for (tries = 1; tries <= EFS_META_STAT_TRIES; tries++) {
        struct efs_meta_row row, again;
        struct efs_txn_pending pend;
        uint64_t size, mtime, ctime, bits;
        uint64_t alloc = 0;
        uint32_t i, nl = 0;
        int moved = 0, stable = 1, rc, is_dir;

        memset(&pend, 0, sizeof(pend));
        rc = inode_read_tx(kv, ino, coord, ctx, &pend, &row);
        if (rc != EFS_OK)
            return rc;
        is_dir = S_ISDIR(row.mode) ? 1 : 0;
        size = row.base_size;
        mtime = row.base_mtime;
        ctime = row.base_ctime;
        /* Files reduce over write lanes; a spread directory reduces over
         * dir lanes (used_shards). A LOCAL directory's times already live
         * on the row, so it is a one-read collect. */
        bits = is_dir
                   ? (row.layout != EFS_META_LAYOUT_LOCAL ? row.used_shards : 0)
                   : row.active_lanes;
        for (i = 0; i < EFS_META_LANES; i++)
            if (bits & (1ULL << i))
                lanes[nl++] = (uint8_t)i;

        for (i = 0; i < nl; i++) {
            uint8_t key[EFS_KV_KEY_MAX], val[LANE_VAL];
            uint32_t kl = 0, vn;
            struct efs_txn_reduce red;
            struct lane_rec ln;
            int value_rc;

            rc = efs_kv_key_lane(efs_kv_lane_shard(ino, lanes[i]), ino,
                                 row.generation, lanes[i], key, &kl);
            if (rc != EFS_OK)
                return rc;
            /* Committed-but-not-yet-materialized reductions have to count:
             * a transaction is visible at its decision, not when a reducer
             * gets around to folding it in, so a stat that ignored them
             * could report a size older than a write that already
             * returned. */
            /* Reuse only the reduction's first visible lane image. The
             * second collect below must independently read its sequence. */
            vn = sizeof(val);
            rc = efs_txn_reduce_read_value_ex(kv, key, kl, coord, ctx, &red,
                                               &pend, val, &vn, &value_rc);
            if (rc != EFS_OK)
                return rc;
            if (value_rc == EFS_OK) {
                rc = efs_kv_decode_result(kv, key, kl, unpack_lane(val, vn, &ln));
                if (rc != EFS_OK)
                    return rc;
            } else if (value_rc == EFS_ERR_NOT_FOUND) {
                memset(&ln, 0, sizeof(ln));
            } else {
                return value_rc;
            }
            seq1[i] = ln.seq;
            if (!is_dir) {
                /* D25: a lane's EOF counts only under the current epoch;
                 * a lane that has not adopted it holds a superseded claim. */
                if (ln.fenced_epoch == row.content_epoch)
                    size = max_u64(size, red.max_end);
                /* D17: present-chunk counts live only on materialized
                 * lanes (a pending reduction never makes a chunk
                 * present), so the reduce is a plain sum. */
                alloc += ln.present;
            }
            ctime = max_u64(ctime, red.max_ctime);
            /* A lane mtime stamped before the current utimens generation was
             * invalidated by it, and taking a MAX against it would let the
             * old value win — which is the whole reason utimens has a
             * generation and the other time sources do not.
             *
             * The fence zeros materialized max_mtime and bumps the lane's
             * generation. A committed-but-unmaterialized reduction from
             * before the fence still sits under the lane prefix with the
             * old mtime; folding it in would undo the utimens. After the
             * fence the lane is (gen>0, max_mtime==0); a later write puts
             * a real stamp back and pending reductions count again. */
            if (ln.mtime_gen >= row.mtime_gen) {
                if (!(ln.max_mtime == 0 && ln.mtime_gen > 0))
                    mtime = max_u64(mtime, red.max_mtime);
            }
        }

        /* Second collect: the sequence numbers only. */
        for (i = 0; i < nl && stable; i++) {
            uint8_t key[EFS_KV_KEY_MAX], val[LANE_VAL];
            uint32_t kl = 0, vn = sizeof(val);
            struct lane_rec ln;
            rc = efs_kv_key_lane(efs_kv_lane_shard(ino, lanes[i]), ino,
                                   row.generation, lanes[i], key, &kl);
            if (rc == EFS_OK)
                rc = efs_txn_read_ex(kv, key, kl, coord, ctx, val, &vn, &pend);
            if (rc == EFS_ERR_NOT_FOUND) {
                seq2[i] = 0;
                rc = EFS_OK;
            } else if (rc == EFS_OK) {
                rc = efs_kv_decode_result(kv, key, kl, unpack_lane(val, vn, &ln));
                if (rc == EFS_OK)
                    seq2[i] = ln.seq;
            }
            if (rc != EFS_OK)
                return rc;
            if (seq2[i] != seq1[i])
                stable = 0;
        }
        rc = inode_read_tx(kv, ino, coord, ctx, &pend, &again);
        if (rc != EFS_OK)
            return rc;
        rc = efs_txn_pending_recheck(&pend, coord, ctx, &moved);
        if (rc != EFS_OK)
            return rc;
        if (moved)
            stable = 0;
        if (again.content_epoch != row.content_epoch ||
            again.base_size != row.base_size ||
            again.mtime_gen != row.mtime_gen ||
            again.active_lanes != row.active_lanes ||
            again.used_shards != row.used_shards ||
            again.generation != row.generation)
            stable = 0;
        if (!stable)
            continue;

        if (!is_dir) {
            struct append_cur cur;

            rc = load_append_cur(kv, ino, row.generation, &cur);
            if (rc != EFS_OK)
                return rc;
            if (cur.nopen)
                size = cur.frontier;
        }

        memset(out, 0, sizeof(*out));
        out->ino = row.ino;
        out->generation = row.generation;
        out->mode = row.mode;
        out->nlink = row.nlink;
        out->uid = row.uid;
        out->gid = row.gid;
        out->size = size;
        out->mtime = mtime;
        out->ctime = ctime;
        out->alloc = alloc;
        out->atime = row.base_atime; /* noatime: reads do not move it */
        out->lanes = nl;
        out->attempts = tries;
        return EFS_OK;
    }
    return EFS_ERR_BUSY;
}

/* Push a per-lane update across a bitmap: the epoch fence and utimens'
 * mtime_gen. A bit with no lane record yet is
 * skipped — the first write to that lane will observe the row and stamp
 * itself. `upd` returns 0 to leave the record untouched (replay). */
int fence_lane_bits(struct efs_kv *kv, efs_ino_t ino, uint64_t generation,
                           uint64_t bits,
                           int (*upd)(struct lane_rec *ln, void *arg),
                           void *arg, struct efs_kv_item *it, uint32_t *n,
                           uint8_t k_ln[][EFS_KV_KEY_MAX],
                           uint8_t v_ln[][LANE_VAL])
{
    uint32_t i;
    int rc;

    for (i = 0; i < EFS_META_LANES; i++) {
        uint8_t old_ln[LANE_VAL];
        uint32_t kl = 0, vn;
        struct lane_rec ln;

        if ((bits & (1ULL << i)) == 0)
            continue;
        rc = efs_kv_key_lane(efs_kv_lane_shard(ino, (uint8_t)i), ino, generation,
                             (uint8_t)i, k_ln[i], &kl);
        if (rc != EFS_OK)
            return rc;
        vn = sizeof(old_ln);
        rc = efs_kv_get(kv, k_ln[i], kl, old_ln, &vn);
        if (rc == EFS_ERR_NOT_FOUND)
            continue;
        if (rc != EFS_OK)
            return rc;
        rc = unpack_lane(old_ln, vn, &ln);
        if (rc != EFS_OK)
            return rc;
        if (!upd(&ln, arg))
            continue;
        pack_lane(v_ln[i], &ln);
        it[*n].op = EFS_KV_PUT;
        it[*n].key = k_ln[i];
        it[*n].klen = kl;
        it[*n].val = v_ln[i];
        it[*n].vlen = LANE_VAL;
        (*n)++;
    }
    return EFS_OK;
}

static int epoch_lane_upd(struct lane_rec *ln, void *arg)
{
    ln->fenced_epoch = *(const uint64_t *)arg;
    /* Invalidate the size claim: after a truncate the authoritative size is
     * base_size on the inode row, and a lane still reporting the pre-truncate
     * high-water mark would win the MAX and undo the truncate. Times are not
     * invalidated — a truncate is a modification, so they only move forward. */
    ln->max_end = 0;
    ln->seq++; /* a stat collect in flight must not validate across this */
    return 1;
}

static int utimens_lane_upd(struct lane_rec *ln, void *arg)
{
    uint64_t gen = *(const uint64_t *)arg;

    if (ln->mtime_gen >= gen)
        return 0;
    ln->mtime_gen = gen;
    /* Zero, not the utimens value: getattr takes base_mtime, and a later
     * write MAX(0, now) must not resurrect the pre-utimens stamp. */
    ln->max_mtime = 0;
    ln->seq++;
    return 1;
}

int efs_meta_apply_epoch_fence(struct efs_kv *kv, efs_ino_t ino)
{
    struct efs_meta_row row;
    uint8_t k_ino[EFS_KV_KEY_MAX];
    uint8_t v_ino[INO_VAL];
    uint8_t k_ln[EFS_META_LANES][EFS_KV_KEY_MAX];
    uint8_t v_ln[EFS_META_LANES][LANE_VAL];
    struct efs_kv_item it[EFS_META_LANES + 1];
    uint32_t ki = 0, n = 0;
    int rc;

    if (!kv || ino == 0)
        return EFS_ERR_INVAL;
    rc = efs_meta_apply_get_inode(kv, ino, &row);
    if (rc != EFS_OK)
        return rc;
    rc = efs_meta_pub_pinned(kv, ino, row.generation);
    if (rc != EFS_OK)
        return rc;
    row.content_epoch++;
    pack_inode(v_ino, &row);
    rc = efs_kv_key_inode(efs_kv_inode_shard(ino), ino, k_ino, &ki);
    if (rc != EFS_OK)
        return rc;
    memset(it, 0, sizeof(it));
    it[n].op = EFS_KV_PUT;
    it[n].key = k_ino;
    it[n].klen = ki;
    it[n].val = v_ino;
    it[n].vlen = INO_VAL;
    n++;
    /* Every active lane, not just lane 0: a lane the fence skipped keeps
     * believing its own epoch and its own size. */
    rc = fence_lane_bits(kv, ino, row.generation, row.active_lanes,
                         epoch_lane_upd, &row.content_epoch, it, &n, k_ln, v_ln);
    if (rc != EFS_OK)
        return rc;
    return meta_write_batch(kv, it, n);
}

int efs_meta_apply_utimens(struct efs_kv *kv, efs_ino_t ino, uint64_t now,
                           const struct efs_meta_utimens *u)
{
    struct efs_meta_row row;
    uint8_t k_ino[EFS_KV_KEY_MAX];
    uint8_t v_ino[INO_VAL];
    uint8_t k_ln[EFS_META_LANES][EFS_KV_KEY_MAX];
    uint8_t v_ln[EFS_META_LANES][LANE_VAL];
    struct efs_kv_item it[EFS_META_LANES + 1];
    uint32_t ki = 0, n = 0;
    uint64_t bits;
    int rc;

    if (!kv || !u || ino == 0)
        return EFS_ERR_INVAL;
    if (u->mask == 0 ||
        (u->mask & ~(EFS_META_SET_MTIME | EFS_META_SET_ATIME)) != 0)
        return EFS_ERR_INVAL;
    if ((u->mask & EFS_META_SET_MTIME) && u->mtime_gen == 0)
        return EFS_ERR_INVAL;
    rc = efs_meta_apply_get_inode(kv, ino, &row);
    if (rc == EFS_ERR_NOT_FOUND && u->lane_bits && u->expect_gen &&
        (u->mask & EFS_META_SET_MTIME)) {
        memset(it, 0, sizeof(it));
        rc = fence_lane_bits(kv, ino, u->expect_gen, u->lane_bits,
                             utimens_lane_upd, (void *)&u->mtime_gen, it, &n,
                             k_ln, v_ln);
        if (rc != EFS_OK)
            return rc;
        if (n == 0)
            return EFS_OK;
        return meta_write_batch(kv, it, n);
    }
    if (rc != EFS_OK)
        return rc;
    if (u->expect_gen != 0 && u->expect_gen != row.generation)
        return EFS_ERR_STALE;
    if ((u->mask & EFS_META_SET_MTIME) && u->mtime_gen < row.mtime_gen)
        return EFS_ERR_STALE;
    if (u->mask & EFS_META_SET_ATIME)
        row.base_atime = u->atime;
    if (u->mask & EFS_META_SET_MTIME)
        row.base_mtime = u->mtime;
    /* ctime always moves (POSIX), MAX-clamped like every implicit stamp. */
    row.base_ctime = max_u64(row.base_ctime, now);
    if (u->mask & EFS_META_SET_MTIME)
        row.mtime_gen = u->mtime_gen;
    pack_inode(v_ino, &row);
    rc = efs_kv_key_inode(efs_kv_inode_shard(ino), ino, k_ino, &ki);
    if (rc != EFS_OK)
        return rc;
    memset(it, 0, sizeof(it));
    it[n].op = EFS_KV_PUT;
    it[n].key = k_ino;
    it[n].klen = ki;
    it[n].val = v_ino;
    it[n].vlen = INO_VAL;
    n++;
    if (u->mask & EFS_META_SET_MTIME) {
        bits = S_ISDIR(row.mode) && row.layout != EFS_META_LAYOUT_LOCAL
                   ? row.used_shards
                   : row.active_lanes;
        rc = fence_lane_bits(kv, ino, row.generation, bits, utimens_lane_upd,
                             &row.mtime_gen, it, &n, k_ln, v_ln);
        if (rc != EFS_OK)
            return rc;
    }
    return meta_write_batch(kv, it, n);
}
