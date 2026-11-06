/* meta_apply/chunk.c: chunk rows and views, inode authority, lane bootstrap
 * and chunk sweep. */

#include "apply_internal.h"
#include "rows.h"
#include "lookup.h"
#include "gc.h"
#include "sweep.h"

int efs_meta_apply_chunk_holds(struct efs_kv *kv, efs_ino_t ino,
                               uint64_t inode_gen, uint32_t chunk_index,
                               uint64_t gen, uint32_t span_off,
                               uint32_t span_len, uint64_t *committed)
{
    struct efs_meta_chunk ch;
    uint8_t lane;
    uint8_t key[EFS_KV_KEY_MAX], val[CHUNK_VAL_MAX];
    uint32_t klen = 0, vlen, nd, i;
    int rc;

    if (!kv || ino == 0)
        return EFS_ERR_INVAL;
    if (committed)
        *committed = 0;
    lane = (uint8_t)(chunk_index % EFS_META_LANES);
    rc = efs_kv_key_chunk(efs_kv_lane_shard(ino, lane), ino, inode_gen, lane,
                          chunk_index, key, &klen);
    if (rc != EFS_OK)
        return rc;
    vlen = sizeof(val);
    rc = kv_get_copy(kv, key, klen, val, sizeof(val), &vlen);
    if (rc != EFS_OK)
        return rc;
    rc = efs_kv_decode_result(kv, key, klen, unpack_chunk(val, vlen, &ch));
    if (rc != EFS_OK)
        return rc;
    rc = efs_kv_decode_result(kv, key, klen, efs_pack_chunk_identity(&ch, ino, inode_gen, chunk_index));
    if (rc != EFS_OK) return rc;
    if (committed)
        *committed = ch.generation;
    if (gen == 0)
        return 0;
    if (span_len == 0)
        return ch.generation == gen ? 1 : 0;
    nd = trailer_n(val, vlen);
    for (i = 0; i < nd; i++) {
        struct efs_meta_delta d;

        rc = efs_kv_decode_result(kv, key, klen, trailer_at(val, vlen, i, &d));
        if (rc != EFS_OK)
            return rc;
        if (d.generation != gen)
            continue;
        if (d.len == 0 || (d.off == span_off && d.len == span_len))
            return 1;
    }
    return 0;
}

int efs_meta_apply_get_chunk_deltas(struct efs_kv *kv, efs_ino_t ino,
                                    uint32_t chunk_index,
                                    struct efs_meta_delta *out, uint32_t cap,
                                    uint32_t *n, uint64_t *newest)
{
    struct efs_meta_row row;
    uint8_t lane;
    uint8_t key[EFS_KV_KEY_MAX], val[CHUNK_VAL_MAX];
    uint32_t klen = 0, vlen, nd, i, wr;
    int rc;

    if (!kv || !n || ino == 0)
        return EFS_ERR_INVAL;
    *n = 0;
    if (newest)
        *newest = 0;
    rc = efs_meta_apply_get_inode(kv, ino, &row);
    if (rc != EFS_OK)
        return rc;
    lane = (uint8_t)(chunk_index % EFS_META_LANES);
    rc = efs_kv_key_chunk(efs_kv_lane_shard(ino, lane), ino, row.generation, lane,
                          chunk_index, key, &klen);
    if (rc != EFS_OK)
        return rc;
    vlen = sizeof(val);
    rc = kv_get_copy(kv, key, klen, val, sizeof(val), &vlen);
    if (rc != EFS_OK)
        return rc;
    struct efs_meta_chunk base;
    rc = efs_kv_decode_result(kv, key, klen, unpack_chunk(val, vlen, &base));
    if (rc == EFS_OK) rc = efs_kv_decode_result(kv, key, klen, efs_pack_chunk_identity(&base, ino, row.generation, chunk_index));
    if (rc != EFS_OK) return rc;
    nd = trailer_n(val, vlen);
    wr = nd < cap ? nd : cap;
    for (i = 0; i < wr; i++) {
        if (!out)
            break;
        rc = efs_kv_decode_result(kv, key, klen, trailer_at(val, vlen, i, &out[i]));
        if (rc != EFS_OK)
            return rc;
    }
    *n = nd;
    if (newest && nd) {
        struct efs_meta_delta last;

        rc = efs_kv_decode_result(kv, key, klen, trailer_at(val, vlen, nd - 1, &last));
        if (rc != EFS_OK)
            return rc;
        *newest = last.seq;
    }
    return EFS_OK;
}

int efs_meta_apply_get_chunk(struct efs_kv *kv, efs_ino_t ino, uint32_t chunk_index,
                             struct efs_meta_chunk *out)
{
    struct efs_meta_row row;
    uint8_t lane;
    uint8_t key[EFS_KV_KEY_MAX], val[CHUNK_VAL_MAX];
    uint32_t klen = 0, vlen;
    int rc;

    if (!kv || !out || ino == 0)
        return EFS_ERR_INVAL;
    rc = efs_meta_apply_get_inode(kv, ino, &row);
    if (rc != EFS_OK)
        return rc;
    lane = (uint8_t)(chunk_index % EFS_META_LANES);
    rc = efs_kv_key_chunk(efs_kv_lane_shard(ino, lane), ino, row.generation, lane,
                          chunk_index, key, &klen);
    if (rc != EFS_OK)
        return rc;
    vlen = sizeof(val);
    rc = kv_get_copy(kv, key, klen, val, sizeof(val), &vlen);
    if (rc != EFS_OK)
        return rc;
    rc = efs_kv_decode_result(kv, key, klen, unpack_chunk(val, vlen, out));
    return rc == EFS_OK ? efs_kv_decode_result(kv, key, klen, efs_pack_chunk_identity(out, ino, row.generation, chunk_index)) : rc;
}

/* One KV get for the image and the span trailer, keyed by a row the
 * caller already holds. GETCHUNKS did get_chunk + get_chunk_deltas per
 * chunk = four point gets (two of them the inode row) for one value;
 * a 64-chunk window under a hard-write was 68 ms (Sep 30). */
int efs_meta_apply_get_chunk_row(struct efs_kv *kv, efs_ino_t ino,
                                 uint64_t inode_gen, uint32_t chunk_index,
                                 struct efs_meta_chunk *out,
                                 struct efs_meta_delta *ds, uint32_t cap,
                                 uint32_t *n, uint64_t *newest)
{
    uint8_t lane;
    uint8_t key[EFS_KV_KEY_MAX], val[CHUNK_VAL_MAX];
    uint32_t klen = 0, vlen, nd, i, wr;
    int rc;

    if (!kv || !out || !n || ino == 0)
        return EFS_ERR_INVAL;
    *n = 0;
    if (newest)
        *newest = 0;
    lane = (uint8_t)(chunk_index % EFS_META_LANES);
    rc = efs_kv_key_chunk(efs_kv_lane_shard(ino, lane), ino, inode_gen, lane,
                          chunk_index, key, &klen);
    if (rc != EFS_OK)
        return rc;
    vlen = sizeof(val);
    rc = kv_get_copy(kv, key, klen, val, sizeof(val), &vlen);
    if (rc != EFS_OK)
        return rc;
    rc = efs_kv_decode_result(kv, key, klen, unpack_chunk(val, vlen, out));
    if (rc == EFS_OK) rc = efs_kv_decode_result(kv, key, klen, efs_pack_chunk_identity(out, ino, inode_gen, chunk_index));
    if (rc != EFS_OK)
        return rc;
    nd = trailer_n(val, vlen);
    wr = nd < cap ? nd : cap;
    for (i = 0; i < wr && ds; i++) {
        rc = efs_kv_decode_result(kv, key, klen, trailer_at(val, vlen, i, &ds[i]));
        if (rc != EFS_OK)
            return rc;
    }
    *n = nd;
    if (newest && nd) {
        struct efs_meta_delta last;

        rc = efs_kv_decode_result(kv, key, klen, trailer_at(val, vlen, nd - 1, &last));
        if (rc != EFS_OK)
            return rc;
        *newest = last.seq;
    }
    return EFS_OK;
}

int efs_meta_get_chunk_view_tx(struct efs_kv *kv, efs_ino_t ino, uint64_t gen,
                            uint32_t ci, uint32_t cs,
                            efs_txn_coord_fn coord, void *ctx,
                            struct efs_meta_chunk_view *out)
{
    uint8_t key[EFS_KV_KEY_MAX], before[LANE_VAL], after[LANE_VAL];
    uint32_t kl = 0;
    uint8_t lane = (uint8_t)(ci % EFS_META_LANES);
    int rc;
    if (!kv || !out || !ino || !gen || cs < EFS_MIN_CHUNK_SIZE ||
        cs > EFS_MAX_CHUNK_SIZE || (cs & (cs - 1)))
        return EFS_ERR_INVAL;
    rc = efs_kv_key_lane(efs_kv_lane_shard(ino, lane), ino, gen, lane, key, &kl);
    if (rc != EFS_OK)
        return rc;
    for (unsigned attempt = 0; attempt < 4; ++attempt) {
        struct efs_meta_chunk_view view = {0};
        struct efs_txn_pending pend = {0};
        int moved = 0;
        struct efs_fence_history h1, h2;
        struct efs_fence_part parts[EFS_FENCE_PART_MAX] = {{0}};
        struct lane_rec stamp;
        uint32_t n1 = sizeof(before), n2 = sizeof(after);
        uint64_t epoch;
        /* Stamp first, history second: reversing these reads admits an
         * append/materialize/retire ABA between two empty histories. */
        rc = coord ? efs_txn_read_ex(kv, key, kl, coord, ctx, before, &n1, &pend) :
                     efs_kv_get(kv, key, kl, before, &n1);
        if (rc == EFS_ERR_NOT_FOUND) {
            n1 = 0;
            memset(&stamp, 0, sizeof(stamp));
        } else if (rc == EFS_OK) {
            rc = efs_kv_decode_result(kv, key, kl, unpack_lane(before, n1, &stamp));
        }
        if (rc != EFS_OK && rc != EFS_ERR_NOT_FOUND)
            return rc;
        rc = fence_history_read(kv, ino, gen, lane, coord, ctx, &pend, &h1);
        if (rc != EFS_OK)
            return rc;
        rc = efs_meta_apply_get_chunk_row(kv, ino, gen, ci, &view.base,
                                          view.deltas, EFS_CHUNK_DELTA_MAX,
                                          &view.ndelta, &view.delta_seq);
        if (rc != EFS_OK)
            return rc;
        if (view.ndelta > EFS_CHUNK_DELTA_MAX)
            return EFS_ERR_PROTO;
        rc = fence_history_read(kv, ino, gen, lane, coord, ctx, &pend, &h2);
        if (rc != EFS_OK)
            return rc;
        rc = coord ? efs_txn_read_ex(kv, key, kl, coord, ctx, after, &n2, &pend) :
                     efs_kv_get(kv, key, kl, after, &n2);
        if (rc == EFS_ERR_NOT_FOUND)
            n2 = 0;
        else if (rc != EFS_OK)
            return rc;
        if (coord) {
            rc = efs_txn_pending_recheck(&pend, coord, ctx, &moved);
            if (rc != EFS_OK)
                return rc;
        }
        if (moved || n1 != n2 || memcmp(before, after, n1) ||
            memcmp(&h1, &h2, sizeof(h1)))
            continue;
        if (h1.count && h1.entries[h1.count - 1].epoch > stamp.fenced_epoch)
            return EFS_ERR_PROTO;
        view.fence_epoch = stamp.fenced_epoch;
        epoch = max_u64(stamp.fenced_epoch, view.base.content_epoch);
        parts[0] = (struct efs_fence_part){view.base.content_epoch, 0,
                                          view.base.generation ?
                                          (view.base.base_clip && view.base.base_clip < cs ?
                                           view.base.base_clip : cs) : 0};
        for (uint32_t i = 0; i < view.ndelta; ++i) {
            const struct efs_meta_delta *d = &view.deltas[i];
            epoch = max_u64(epoch, d->content_epoch);
            parts[i + 1] = (struct efs_fence_part){d->content_epoch, d->off, d->len};
        }
        rc = efs_fence_view_build(&view.bytes, epoch, (uint64_t)ci * cs,
                                   cs, &h1, parts, view.ndelta + 1);
        if (rc != EFS_OK)
            return EFS_ERR_PROTO;
        view.bytes.fence_epoch = stamp.fenced_epoch;
        *out = view;
        return EFS_OK;
    }
    return EFS_ERR_BUSY;
}

int efs_meta_get_chunk_view(struct efs_kv *kv, efs_ino_t ino, uint64_t gen,
                            uint32_t ci, uint32_t cs,
                            struct efs_meta_chunk_view *out)
{
    return efs_meta_get_chunk_view_tx(kv, ino, gen, ci, cs, NULL, NULL, out);
}

/* D25 read authority is the inode: its content epoch and fence history,
 * captured together. Two reads of each must agree, and no transaction the
 * reads depended on may have been decided in between. */
int efs_meta_get_inode_authority_tx(struct efs_kv *kv, efs_ino_t ino,
                                    efs_txn_coord_fn coord, void *ctx,
                                    struct efs_meta_row *row,
                                    struct efs_fence_history *history)
{
    if (!kv || !ino || !row || !history)
        return EFS_ERR_INVAL;
    for (unsigned attempt = 0; attempt < 4; ++attempt) {
        struct efs_meta_row r1, r2;
        struct efs_fence_history h1, h2;
        struct efs_txn_pending pend = {0};
        int moved = 0;
        int rc = inode_read_tx(kv, ino, coord, ctx, &pend, &r1);
        if (rc == EFS_OK)
            rc = fence_history_read(kv, ino, r1.generation, EFS_META_FENCE_INODE,
                                    coord, ctx, &pend, &h1);
        if (rc == EFS_OK)
            rc = inode_read_tx(kv, ino, coord, ctx, &pend, &r2);
        if (rc == EFS_OK)
            rc = fence_history_read(kv, ino, r2.generation, EFS_META_FENCE_INODE,
                                    coord, ctx, &pend, &h2);
        if (rc == EFS_OK && coord)
            rc = efs_txn_pending_recheck(&pend, coord, ctx, &moved);
        if (rc != EFS_OK)
            return rc;
        if (moved || r1.generation != r2.generation ||
            r1.content_epoch != r2.content_epoch ||
            memcmp(&h1, &h2, sizeof(h1)))
            continue;
        if (h1.count && h1.entries[h1.count - 1].epoch > r1.content_epoch)
            return EFS_ERR_PROTO;
        *row = r2;
        *history = h1;
        return EFS_OK;
    }
    return EFS_ERR_BUSY;
}

/* A chunk view under the inode authority captured by
 * efs_meta_get_inode_authority_tx: masks come from the inode history, and
 * the lane stamp is not consulted. A part newer than the captured epoch
 * means a truncate committed after the capture: BUSY, recapture. */
int efs_meta_get_chunk_view_auth(struct efs_kv *kv, efs_ino_t ino, uint64_t gen,
                                 uint32_t ci, uint32_t cs, uint64_t epoch,
                                 const struct efs_fence_history *history,
                                 struct efs_meta_chunk_view *out)
{
    struct efs_meta_chunk_view view = {0};
    struct efs_fence_part parts[EFS_FENCE_PART_MAX] = {{0}};
    int rc;
    if (!kv || !out || !ino || !gen || !history || cs < EFS_MIN_CHUNK_SIZE ||
        cs > EFS_MAX_CHUNK_SIZE || (cs & (cs - 1)))
        return EFS_ERR_INVAL;
    rc = efs_meta_apply_get_chunk_row(kv, ino, gen, ci, &view.base, view.deltas,
                                      EFS_CHUNK_DELTA_MAX, &view.ndelta,
                                      &view.delta_seq);
    if (rc != EFS_OK)
        return rc;
    if (view.ndelta > EFS_CHUNK_DELTA_MAX)
        return EFS_ERR_PROTO;
    if (view.base.content_epoch > epoch)
        return EFS_ERR_BUSY;
    parts[0] = (struct efs_fence_part){view.base.content_epoch, 0,
                                      view.base.generation ?
                                      (view.base.base_clip && view.base.base_clip < cs ?
                                       view.base.base_clip : cs) : 0};
    for (uint32_t i = 0; i < view.ndelta; ++i) {
        const struct efs_meta_delta *d = &view.deltas[i];
        if (d->content_epoch > epoch)
            return EFS_ERR_BUSY;
        parts[i + 1] = (struct efs_fence_part){d->content_epoch, d->off, d->len};
    }
    if (efs_fence_view_build(&view.bytes, epoch, (uint64_t)ci * cs, cs, history,
                             parts, view.ndelta + 1) != EFS_OK)
        return EFS_ERR_PROTO;
    view.fence_epoch = epoch;
    view.bytes.fence_epoch = epoch;
    *out = view;
    return EFS_OK;
}

/* The lane stamp's adopted epoch; NOT_FOUND when the lane has no stamp. */
int efs_meta_get_lane_epoch(struct efs_kv *kv, efs_ino_t ino, uint64_t gen,
                            uint8_t lane, uint64_t *epoch)
{
    uint8_t key[EFS_KV_KEY_MAX], val[LANE_VAL];
    uint32_t kl = 0, vn = sizeof(val);
    struct lane_rec ln;
    int rc;
    if (!kv || !ino || !gen || lane >= EFS_META_LANES || !epoch)
        return EFS_ERR_INVAL;
    rc = efs_kv_key_lane(efs_kv_lane_shard(ino, lane), ino, gen, lane, key, &kl);
    if (rc == EFS_OK)
        rc = efs_kv_get(kv, key, kl, val, &vn);
    if (rc == EFS_OK)
        rc = efs_kv_decode_result(kv, key, kl, unpack_lane(val, vn, &ln));
    if (rc == EFS_OK)
        *epoch = ln.fenced_epoch;
    return rc;
}

/* D25 BOOTSTRAP on the lane's group: create a cross-group lane's stamp at
 * the inode epoch the proposer read, or adopt it if the stamp is older.
 * An equal or older epoch changes nothing. */
int efs_meta_apply_bootstrap_lane(struct efs_kv *kv, efs_ino_t ino, uint64_t gen,
                                  uint8_t lane, uint64_t epoch)
{
    uint8_t key[EFS_KV_KEY_MAX], val[LANE_VAL];
    uint32_t kl = 0, vn = sizeof(val);
    struct lane_rec ln;
    int rc;
    if (!kv || !ino || !gen || lane >= EFS_META_LANES)
        return EFS_ERR_INVAL;
    rc = efs_kv_key_lane(efs_kv_lane_shard(ino, lane), ino, gen, lane, key, &kl);
    if (rc != EFS_OK)
        return rc;
    rc = efs_kv_get(kv, key, kl, val, &vn);
    if (rc == EFS_ERR_NOT_FOUND) {
        memset(&ln, 0, sizeof(ln));
        ln.fenced_epoch = epoch;
    } else if (rc == EFS_OK) {
        rc = unpack_lane(val, vn, &ln);
        if (rc != EFS_OK)
            return rc;
        if (!lane_adopt(&ln, epoch))
            return EFS_OK;
    } else {
        return rc;
    }
    pack_lane(val, &ln);
    return meta_write_put(kv, key, kl, val, LANE_VAL);
}

int efs_meta_apply_sweep_chunk(struct efs_kv *kv, efs_ino_t ino, uint64_t gen,
                               uint32_t ci,
                               const struct efs_meta_chunk_view *source,
                               const struct efs_meta_chunk *replacement)
{
    struct efs_meta_chunk_view live;
    struct packed_retirement retirement = {0};
    struct lane_rec lane;
    enum { CAP = 3 + EFS_CHUNK_DELTA_MAX };
    struct efs_kv_item items[CAP] = {0};
    uint8_t gk[CAP][EFS_KV_KEY_MAX], gv[CAP][EFS_META_GC_VAL];
    uint8_t ck[EFS_KV_KEY_MAX], lk[EFS_KV_KEY_MAX], lv[LANE_VAL];
    uint8_t expected[CHUNK_VAL_MAX], actual[CHUNK_VAL_MAX], value[CHUNK_VAL + EFS_PACK_REF_BYTES];
    uint32_t ckl = 0, lkl = 0, lvl = sizeof(lv), n = 0, en, an;
    uint8_t authority = (uint8_t)(ci % EFS_META_LANES);
    int rc;
    if (!source || source->ndelta > EFS_CHUNK_DELTA_MAX)
        return EFS_ERR_INVAL;
    rc = efs_meta_get_chunk_view(kv, ino, gen, ci, source->bytes.chunk_size, &live);
    if (rc != EFS_OK)
        return rc;
    en = pack_chunk_value(expected, &source->base, source->deltas, source->ndelta);
    an = pack_chunk_value(actual, &live.base, live.deltas, live.ndelta);
    if (en != an || memcmp(expected, actual, en) ||
        live.fence_epoch != source->fence_epoch ||
        live.delta_seq != source->delta_seq ||
        memcmp(&live.bytes, &source->bytes, sizeof(live.bytes)))
        return EFS_ERR_STALE;
    if (!replacement) {
        for (uint32_t i = 0; i < live.bytes.count; ++i)
            if (live.bytes.parts[i].len)
                return EFS_ERR_INVAL;
    } else {
        struct efs_meta_pub evidence = {0};
        if (replacement->content_epoch != live.bytes.revision)
            return EFS_ERR_STALE;
        evidence.candidate_gen = replacement->generation;
        evidence.coding_profile_id = replacement->coding_profile_id;
        evidence.ch = *replacement;
        rc = evidence_ok(&evidence);
        if (rc != EFS_OK)
            return rc;
    }
    rc = efs_kv_key_chunk(efs_kv_lane_shard(ino, authority), ino, gen,
                           authority, ci, ck, &ckl);
    if (rc == EFS_OK)
        rc = efs_kv_key_lane(efs_kv_lane_shard(ino, authority), ino, gen,
                              authority, lk, &lkl);
    if (rc == EFS_OK)
        rc = efs_kv_get(kv, lk, lkl, lv, &lvl);
    if (rc == EFS_OK)
        rc = unpack_lane(lv, lvl, &lane);
    if (rc != EFS_OK)
        return rc;
    lane.seq++;
    if (!replacement && lane.present)
        --lane.present;
    pack_lane(lv, &lane);
    if (replacement) {
        uint32_t length = pack_chunk_value(value, replacement, NULL, 0);
        items[n++] = (struct efs_kv_item){EFS_KV_PUT, ck, ckl, value, length};
    } else {
        items[n++] = (struct efs_kv_item){EFS_KV_DEL, ck, ckl, NULL, 0};
    }
    items[n++] = (struct efs_kv_item){EFS_KV_PUT, lk, lkl, lv, LANE_VAL};
    if (live.base.coding_profile_id == EFS_META_PROFILE_PACK) {
        if (ci) return EFS_ERR_PROTO;
        retirement.before = live.base; retirement.present = 1;
        if (replacement) { retirement.after = *replacement; retirement.has_after = 1; }
    } else if (live.base.generation && (!replacement ||
        (live.base.generation != replacement->generation &&
         !chunk_aliases(&live.base, replacement)))) {
        rc = gc_queue(items, &n, CAP, gk, gv, ino, authority, ci, &live.base);
        if (rc != EFS_OK)
            return rc;
    }
    for (uint32_t i = 0; i < live.ndelta; ++i) {
        const struct efs_meta_delta *d = &live.deltas[i];
        struct efs_meta_chunk dead = {0};
        if (!d->generation)
            continue;
        dead.generation = d->generation;
        memcpy(dead.nodes, d->nodes, sizeof(dead.nodes));
        memcpy(dead.checksums, d->checksums, sizeof(dead.checksums));
        if (replacement && (dead.generation == replacement->generation ||
                            chunk_aliases(&dead, replacement)))
            continue;
        rc = gc_queue(items, &n, CAP, gk, gv, ino, authority, ci, &dead);
        if (rc != EFS_OK)
            return rc;
    }
    return packed_retirement_commit(kv, ino, gen, &retirement, items, n, CAP);
}
