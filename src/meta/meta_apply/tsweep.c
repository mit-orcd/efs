/* meta_apply/tsweep.c: truncate sweep, swept lanes, sweep done and retire. */

#include "apply_internal.h"
#include "tsweep.h"
#include "rows.h"
#include "gc.h"
#include "sweep.h"
#include "append.h"

/* ---- D25 truncate sweep (protocol §4.2-§4.4) ---- */

#define SWEEP_SCAN_MAX 256u
#define SWEEP_CHANGE_MAX 64u
#define SWEEP_ITEMS (SWEEP_SCAN_MAX + SWEEP_CHANGE_MAX * (2u + EFS_CHUNK_DELTA_MAX) + 8u)

struct tsweep_row {
    uint32_t ci;
    uint32_t klen, vlen;
    uint8_t key[EFS_KV_KEY_MAX];
    uint8_t val[CHUNK_VAL_MAX];
};
struct tsweep_scan {
    efs_ino_t ino;
    uint64_t gen;
    uint8_t lane;
    uint32_t n;
    int more;
    struct tsweep_row *rows;
};
static int tsweep_scan_cb(void *user, const uint8_t *key, uint32_t klen,
                         const uint8_t *val, uint32_t vlen)
{
    struct tsweep_scan *sc = user;
    if (klen < 24 || key[2] != EFS_KV_KIND_CHUNK || rd64(key + 3) != sc->ino ||
        rd64(key + 11) != sc->gen || key[19] != sc->lane)
        return 0;
    if (sc->n == SWEEP_SCAN_MAX) {
        sc->more = 1;
        return 1;
    }
    if (klen > EFS_KV_KEY_MAX || vlen > CHUNK_VAL_MAX)
        return 1;
    struct tsweep_row *r = &sc->rows[sc->n++];
    r->ci = efs_load_be32(key + 20);
    r->klen = klen;
    r->vlen = vlen;
    memcpy(r->key, key, klen);
    memcpy(r->val, val, vlen);
    return 0;
}

/* Visible length of a part at chunk offset `off` (file offset c + off) that
 * would otherwise show `len` bytes, under history S. */
uint32_t sweep_visible(const struct efs_fence_history *s, uint64_t epoch,
                              uint64_t c, uint32_t off, uint32_t len)
{
    uint64_t end = efs_fence_valid_end(s, epoch), start = c + off;
    if (end <= start)
        return 0;
    return end - start < len ? (uint32_t)(end - start) : len;
}

struct tsweep_page {
    struct tsweep_row rows[SWEEP_SCAN_MAX];
    struct efs_kv_item it[SWEEP_ITEMS];
    uint8_t vals[SWEEP_CHANGE_MAX][CHUNK_VAL_MAX];
    uint8_t gc_keys[SWEEP_ITEMS][EFS_KV_KEY_MAX];
    uint8_t gc_vals[SWEEP_ITEMS][EFS_META_GC_VAL];
};

int efs_meta_apply_trunc_sweep(struct efs_kv *kv, efs_ino_t ino, uint64_t gen,
                               uint8_t lane, uint64_t target, uint32_t cs,
                               const struct efs_fence_history *snap)
{
    uint8_t lk[EFS_KV_KEY_MAX], lv[LANE_VAL], pref[EFS_KV_KEY_MAX], start[EFS_KV_KEY_MAX];
    uint32_t lkl = 0, lvn = sizeof(lv), plen = 0, slen = 0, n = 0, changed = 0, dropped = 0;
    struct lane_rec ln;
    struct packed_retirement retirement = {0};
    struct tsweep_scan sc;
    struct tsweep_page *pg;
    uint32_t i;
    int rc, stop = 0;

    if (!kv || !ino || !gen || lane >= EFS_META_LANES || !target || !snap ||
        cs < EFS_MIN_CHUNK_SIZE || cs > EFS_MAX_CHUNK_SIZE || (cs & (cs - 1)) ||
        efs_fence_history_valid(snap) != EFS_OK || !snap->count ||
        snap->entries[snap->count - 1].epoch != target)
        return EFS_ERR_INVAL;
    rc = efs_kv_key_lane(efs_kv_lane_shard(ino, lane), ino, gen, lane, lk, &lkl);
    if (rc != EFS_OK)
        return rc;
    rc = efs_kv_get(kv, lk, lkl, lv, &lvn);
    if (rc == EFS_ERR_NOT_FOUND) {
        memset(&ln, 0, sizeof(ln));
        ln.fenced_epoch = target;
    } else if (rc == EFS_OK) {
        rc = unpack_lane(lv, lvn, &ln);
        if (rc != EFS_OK)
            return rc;
    } else {
        return rc;
    }
    if (target < ln.sweep_target)
        return EFS_OK; /* obsolete command */
    if (target > ln.sweep_target) {
        ln.sweep_target = target;
        ln.sweep_next_ci = 0;
    }
    (void)lane_adopt(&ln, target);
    if (ln.swept_epoch >= target) {
        pack_lane(lv, &ln);
        return meta_write_put(kv, lk, lkl, lv, LANE_VAL);
    }
    pg = malloc(sizeof(*pg));
    if (!pg)
        return EFS_ERR_NOMEM;
    memset(&sc, 0, sizeof(sc));
    sc.ino = ino; sc.gen = gen; sc.lane = lane; sc.rows = pg->rows;
    rc = efs_kv_key_chunk(efs_kv_lane_shard(ino, lane), ino, gen, lane, 0, pref, &plen);
    if (rc == EFS_OK)
        rc = efs_kv_key_chunk(efs_kv_lane_shard(ino, lane), ino, gen, lane,
                              ln.sweep_next_ci > UINT32_MAX ? UINT32_MAX :
                              (uint32_t)ln.sweep_next_ci, start, &slen);
    if (rc == EFS_OK && ln.sweep_next_ci <= UINT32_MAX)
        rc = efs_kv_scan_from(kv, pref, 20, start, slen, tsweep_scan_cb, &sc);
    if (rc < 0) {
        free(pg);
        return rc;
    }
    rc = EFS_OK;
    uint64_t next = ln.sweep_next_ci;
    for (i = 0; i < sc.n && rc == EFS_OK && !stop; i++) {
        struct tsweep_row *r = &pg->rows[i];
        struct efs_meta_chunk base;
        struct efs_meta_delta ds[EFS_CHUNK_DELTA_MAX];
        uint32_t nd = trailer_n(r->val, r->vlen), j, need;
        uint64_t c = (uint64_t)r->ci * cs;
        int dirty = 0, live = 0;

        /* A row under a pending transaction intent waits: stop here, the
         * cursor stays on it, a later entry retries. */
        need = 3u + 2u * (nd + 1u);
        if (changed == SWEEP_CHANGE_MAX || n + need > SWEEP_ITEMS ||
            meta_write_check(kv, r->key, r->klen) != EFS_OK) {
            stop = 1;
            break;
        }
        rc = unpack_chunk(r->val, r->vlen, &base);
        for (j = 0; rc == EFS_OK && j < nd; j++)
            rc = trailer_at(r->val, r->vlen, j, &ds[j]);
        if (rc != EFS_OK)
            break;
        if (base.generation) {
            uint32_t blen = base.base_clip && base.base_clip < cs ? base.base_clip : cs;
            uint32_t vis = sweep_visible(snap, base.content_epoch, c, 0, blen);
            if (!vis) {
                if (base.coding_profile_id == EFS_META_PROFILE_PACK) {
                    if (retirement.present) { stop = 1; break; } /* one per batch */
                    retirement.before = base;
                    retirement.present = 1;
                } else {
                    rc = gc_queue(pg->it, &n, SWEEP_ITEMS, pg->gc_keys, pg->gc_vals,
                                  ino, lane, r->ci, &base);
                    if (rc != EFS_OK)
                        break;
                }
                uint64_t epoch = base.content_epoch;
                memset(&base, 0, sizeof(base));
                base.coding_profile_id = EFS_META_PROFILE_K2F1;
                base.content_epoch = epoch;
                dirty = 1;
            } else if (vis < blen) {
                base.base_clip = vis;
                dirty = 1;
            }
        }
        for (j = 0; j < nd && rc == EFS_OK; j++) {
            struct efs_meta_delta *d = &ds[j];
            uint32_t vis;
            if (!d->len || !d->generation)
                continue;
            vis = sweep_visible(snap, d->content_epoch, c, d->off, d->len);
            if (!vis) {
                struct efs_meta_chunk span;
                memset(&span, 0, sizeof(span));
                span.generation = d->generation;
                memcpy(span.nodes, d->nodes, sizeof(span.nodes));
                memcpy(span.checksums, d->checksums, sizeof(span.checksums));
                rc = gc_queue(pg->it, &n, SWEEP_ITEMS, pg->gc_keys, pg->gc_vals,
                              ino, lane, r->ci, &span);
                d->off = 0;
                d->len = 0; /* tombstone: its object is now GC's */
                dirty = 1;
            } else if (vis < d->len) {
                d->len = vis;
                dirty = 1;
            }
        }
        if (rc != EFS_OK)
            break;
        live = base.generation != 0;
        for (j = 0; j < nd; j++)
            live |= ds[j].len != 0;
        if (!live) {
            pg->it[n++] = (struct efs_kv_item){EFS_KV_DEL, r->key, r->klen, NULL, 0};
            dropped++;
            changed++;
        } else if (dirty) {
            uint32_t vl = pack_chunk_value(pg->vals[changed], &base, ds, nd);
            pg->it[n++] = (struct efs_kv_item){EFS_KV_PUT, r->key, r->klen,
                                               pg->vals[changed], vl};
            if (retirement.present && !retirement.has_after && r->ci == 0) {
                retirement.after = base;
                retirement.has_after = 1;
            }
            changed++;
        }
        next = (uint64_t)r->ci + 1;
    }
    if (rc == EFS_OK) {
        ln.sweep_next_ci = next;
        if (!stop && !sc.more)
            ln.swept_epoch = max_u64(ln.swept_epoch, target);
        ln.present = ln.present >= dropped ? ln.present - dropped : 0;
        if (changed)
            ln.seq++;
        pack_lane(lv, &ln);
        pg->it[n++] = (struct efs_kv_item){EFS_KV_PUT, lk, lkl, lv, LANE_VAL};
        rc = packed_retirement_commit(kv, ino, gen, &retirement, pg->it, n, SWEEP_ITEMS);
    }
    free(pg);
    return rc;
}

int efs_meta_get_lane_swept(struct efs_kv *kv, efs_ino_t ino, uint64_t gen,
                            uint8_t lane, uint64_t *swept_epoch)
{
    uint8_t key[EFS_KV_KEY_MAX], val[LANE_VAL];
    uint32_t kl = 0, vn = sizeof(val);
    struct lane_rec ln;
    int rc;
    if (!kv || !ino || !gen || lane >= EFS_META_LANES || !swept_epoch)
        return EFS_ERR_INVAL;
    rc = efs_kv_key_lane(efs_kv_lane_shard(ino, lane), ino, gen, lane, key, &kl);
    if (rc == EFS_OK)
        rc = efs_kv_get(kv, key, kl, val, &vn);
    if (rc == EFS_OK)
        rc = efs_kv_decode_result(kv, key, kl, unpack_lane(val, vn, &ln));
    if (rc == EFS_OK)
        *swept_epoch = ln.swept_epoch;
    return rc;
}

/* Inode group: record that `lane` holds swept_epoch >= target. Obsolete or
 * foreign evidence is OK with no change. */
int efs_meta_apply_sweep_done(struct efs_kv *kv, efs_ino_t ino, uint64_t gen,
                              uint8_t lane, uint64_t target)
{
    struct efs_meta_row row;
    struct efs_fence_history h;
    uint8_t k[EFS_KV_KEY_MAX], v[EFS_META_FENCE_BYTES];
    uint32_t kl = 0, vl = 0;
    int rc;
    if (!kv || !ino || lane >= EFS_META_LANES || !target)
        return EFS_ERR_INVAL;
    rc = efs_meta_apply_get_inode(kv, ino, &row);
    if (rc == EFS_ERR_NOT_FOUND)
        return EFS_OK;
    if (rc != EFS_OK)
        return rc;
    if (row.generation != gen || !(row.active_lanes & (1ull << lane)))
        return EFS_OK;
    rc = fence_history_read(kv, ino, gen, EFS_META_FENCE_INODE, NULL, NULL, NULL, &h);
    if (rc != EFS_OK)
        return rc;
    for (uint32_t i = 0; i < h.count; i++) {
        if (h.entries[i].epoch != target)
            continue;
        if (h.entries[i].done & (1ull << lane))
            return EFS_OK;
        h.entries[i].done |= 1ull << lane;
        rc = content_fence_key(ino, gen, EFS_META_FENCE_INODE, k, &kl);
        if (rc == EFS_OK)
            rc = efs_meta_pack_fence_history(&h, v, sizeof(v), &vl);
        return rc == EFS_OK ? meta_write_put(kv, k, kl, v, vl) : rc;
    }
    return EFS_OK;
}

/* Inode group: drop every entry <= target once all active lanes are done
 * for target. The record stays (count 0, revision kept); TRUNC_PENDING goes
 * with the last entry. */
int efs_meta_apply_retire(struct efs_kv *kv, efs_ino_t ino, uint64_t gen,
                          uint64_t target)
{
    struct efs_meta_row row;
    struct efs_fence_history h, next;
    uint8_t k[EFS_KV_KEY_MAX], v[EFS_META_FENCE_BYTES], kp[EFS_KV_KEY_MAX];
    uint32_t kl = 0, vl = 0, kpl = 0, at = UINT32_MAX;
    struct efs_kv_item it[2];
    int rc;
    if (!kv || !ino || !target)
        return EFS_ERR_INVAL;
    rc = efs_meta_apply_get_inode(kv, ino, &row);
    if (rc == EFS_ERR_NOT_FOUND || (rc == EFS_OK && row.generation != gen)) {
        /* The file is gone: its pending sweep is the reaper's now. */
        uint8_t pv[8];
        uint32_t pvl = sizeof(pv);
        rc = trunc_pending_key(ino, kp, &kpl);
        if (rc == EFS_OK)
            rc = efs_kv_get(kv, kp, kpl, pv, &pvl);
        if (rc == EFS_ERR_NOT_FOUND)
            return EFS_OK;
        if (rc != EFS_OK)
            return rc;
        if (pvl != 8 || rd64(pv) != gen)
            return EFS_OK;
        rc = meta_write_del(kv, kp, kpl);
        return rc == EFS_ERR_NOT_FOUND ? EFS_OK : rc;
    }
    if (rc != EFS_OK)
        return rc;
    rc = fence_history_read(kv, ino, gen, EFS_META_FENCE_INODE, NULL, NULL, NULL, &h);
    if (rc != EFS_OK)
        return rc;
    for (uint32_t i = 0; i < h.count; i++)
        if (h.entries[i].epoch == target)
            at = i;
    if (at == UINT32_MAX)
        return EFS_OK;
    if ((h.entries[at].done & row.active_lanes) != row.active_lanes)
        return EFS_ERR_INVAL;
    if (h.revision == UINT64_MAX)
        return EFS_ERR_INVAL;
    next = h;
    next.count = 0;
    for (uint32_t i = at + 1; i < h.count; i++)
        next.entries[next.count++] = h.entries[i];
    next.revision++;
    rc = content_fence_key(ino, gen, EFS_META_FENCE_INODE, k, &kl);
    if (rc == EFS_OK)
        rc = efs_meta_pack_fence_history(&next, v, sizeof(v), &vl);
    if (rc != EFS_OK)
        return rc;
    it[0] = (struct efs_kv_item){EFS_KV_PUT, k, kl, v, vl};
    if (next.count)
        return meta_write_batch(kv, it, 1);
    rc = trunc_pending_key(ino, kp, &kpl);
    if (rc != EFS_OK)
        return rc;
    it[1] = (struct efs_kv_item){EFS_KV_DEL, kp, kpl, NULL, 0};
    return meta_write_batch(kv, it, 2);
}
