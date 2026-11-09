/* meta_apply/gc.c: GC and reap records, the pending-GC and degraded-mark
 * counts, degraded and recovery marks. */

#include "apply_internal.h"
#include "gc.h"
#include "rows.h"

void efs_meta_pack_gc(uint8_t *p, const efs_node_id_t nodes[EFS_NUM_FRAGMENTS],
                      uint8_t ack_bits,
                      const uint8_t sums[EFS_NUM_FRAGMENTS][EFS_HASH_SIZE])
{
    int i;

    for (i = 0; i < EFS_NUM_FRAGMENTS; i++)
        efs_store_be32(p + (uint32_t)i * 4u, nodes[i]);
    p[4 * EFS_NUM_FRAGMENTS] = ack_bits;
    memset(p + 4 * EFS_NUM_FRAGMENTS + 1, 0, 3);
    memcpy(p + 4 * EFS_NUM_FRAGMENTS + 4, sums,
           EFS_HASH_SIZE * EFS_NUM_FRAGMENTS);
}

int efs_meta_unpack_gc(const uint8_t *p, uint32_t n,
                       efs_node_id_t nodes[EFS_NUM_FRAGMENTS],
                       uint8_t *ack_bits,
                       uint8_t sums[EFS_NUM_FRAGMENTS][EFS_HASH_SIZE])
{
    int i;

    if (!p || n < EFS_META_GC_VAL)
        return EFS_ERR_PROTO;
    for (i = 0; i < EFS_NUM_FRAGMENTS; i++)
        nodes[i] = efs_load_be32(p + (uint32_t)i * 4u);
    *ack_bits = p[4 * EFS_NUM_FRAGMENTS];
    memcpy(sums, p + 4 * EFS_NUM_FRAGMENTS + 4,
           EFS_HASH_SIZE * EFS_NUM_FRAGMENTS);
    return EFS_OK;
}

void efs_meta_pack_reap(uint8_t *p, uint64_t generation,
                        uint64_t active_lanes)
{
    be64(p, generation);
    be64(p + 8, active_lanes);
}

int efs_meta_unpack_reap(const uint8_t *p, uint32_t n, uint64_t *generation,
                         uint64_t *active_lanes)
{
    if (!p || n < EFS_META_REAP_VAL)
        return EFS_ERR_PROTO;
    *generation = rd64(p);
    *active_lanes = rd64(p + 8);
    return EFS_OK;
}

/* D26: per-anchor pending-GC watermark (anchors are 1 = odd group,
 * 2 = even group; efs_kv_anchor_shard). Bumped in gc_queue, lowered in
 * gc_ack_apply, both inside the record's own apply. Unknown until the
 * GC thread derives it from one prefix scan (process start, snapshot
 * import); while unknown, inserts accumulate in gc_wm_pos and lowers are
 * dropped — the derive recounts live keys, so a dropped lower lands in
 * the count or not at all, never twice, and a folded insert may double
 * (upward only: an extra bounded scan, never a missed record).
 * gc_wm_cur holds the inserts noted since the latest apply began: the
 * bump precedes its batch's commit, so begin keeps those, not zero. */
#define GC_WM_ANCHOR_MAX 2

static pthread_mutex_t gc_wm_mu = PTHREAD_MUTEX_INITIALIZER;
static int64_t gc_wm_pending[GC_WM_ANCHOR_MAX + 1];
static int64_t gc_wm_pos[GC_WM_ANCHOR_MAX + 1];
static int64_t gc_wm_cur[GC_WM_ANCHOR_MAX + 1];
static int gc_wm_known[GC_WM_ANCHOR_MAX + 1];

void efs_meta_gc_pending_note(uint32_t anchor, int delta)
{
    if (anchor == 0 || anchor > GC_WM_ANCHOR_MAX)
        return;
    pthread_mutex_lock(&gc_wm_mu);
    if (gc_wm_known[anchor]) {
        gc_wm_pending[anchor] += delta;
        if (gc_wm_pending[anchor] < 0)
            gc_wm_pending[anchor] = 0;
    } else if (delta > 0) {
        gc_wm_pos[anchor] += delta;
        gc_wm_cur[anchor] += delta;
    }
    pthread_mutex_unlock(&gc_wm_mu);
}

/* host_apply calls this once the apply scope holds the KV writers lock:
 * applies are serialized there, so every insert noted earlier belongs to
 * an apply that already committed. */
void efs_meta_gc_pending_apply_start(void)
{
    pthread_mutex_lock(&gc_wm_mu);
    memset(gc_wm_cur, 0, sizeof(gc_wm_cur));
    pthread_mutex_unlock(&gc_wm_mu);
}

/* Call just before the derive scan. An insert noted before the latest
 * apply began is committed, so the scan counts it if it is still live;
 * only the latest apply's inserts (maybe not committed yet) and those
 * noted during the scan are folded on top. A follower never derives, so
 * without this a leader elected later added every record it ever applied. */
void efs_meta_gc_pending_begin(uint32_t anchor)
{
    if (anchor == 0 || anchor > GC_WM_ANCHOR_MAX)
        return;
    pthread_mutex_lock(&gc_wm_mu);
    if (!gc_wm_known[anchor])
        gc_wm_pos[anchor] = gc_wm_cur[anchor];
    pthread_mutex_unlock(&gc_wm_mu);
}

int64_t efs_meta_gc_pending_peek(uint32_t anchor)
{
    int64_t r;

    if (anchor == 0 || anchor > GC_WM_ANCHOR_MAX)
        return 0;
    pthread_mutex_lock(&gc_wm_mu);
    r = gc_wm_known[anchor] ? gc_wm_pending[anchor] : -1;
    pthread_mutex_unlock(&gc_wm_mu);
    return r;
}

void efs_meta_gc_pending_derived(uint32_t anchor, uint64_t n)
{
    if (anchor == 0 || anchor > GC_WM_ANCHOR_MAX)
        return;
    pthread_mutex_lock(&gc_wm_mu);
    gc_wm_pending[anchor] = (int64_t)n + gc_wm_pos[anchor];
    gc_wm_pos[anchor] = 0;
    gc_wm_known[anchor] = 1;
    pthread_mutex_unlock(&gc_wm_mu);
}

void efs_meta_gc_pending_invalidate(uint32_t anchor)
{
    if (anchor == 0 || anchor > GC_WM_ANCHOR_MAX)
        return;
    pthread_mutex_lock(&gc_wm_mu);
    gc_wm_known[anchor] = 0;
    gc_wm_pending[anchor] = 0;
    gc_wm_pos[anchor] = 0;
    pthread_mutex_unlock(&gc_wm_mu);
}

int efs_meta_gc_pending_zero_if(uint32_t anchor, int64_t expect)
{
    int ok = 0;

    if (anchor == 0 || anchor > GC_WM_ANCHOR_MAX)
        return 0;
    pthread_mutex_lock(&gc_wm_mu);
    if (gc_wm_known[anchor] && gc_wm_pending[anchor] == expect) {
        gc_wm_pending[anchor] = 0;
        ok = 1;
    }
    pthread_mutex_unlock(&gc_wm_mu);
    return ok;
}

/* Queue one GC record for a dead chunk generation. The record lands on the
 * anchor shard of the chunk's lane shard, so the reaper scans one prefix
 * per group. gc_keys/gc_vals are caller storage indexed by the global item
 * number. */
int gc_queue(struct efs_kv_item *it, uint32_t *n, uint32_t cap,
                    uint8_t (*gc_keys)[EFS_KV_KEY_MAX],
                    uint8_t (*gc_vals)[EFS_META_GC_VAL],
                    efs_ino_t ino, uint8_t lane, uint32_t ci,
                    const struct efs_meta_chunk *dead)
{
    uint32_t kg = 0, anch;
    int rc;

    /* A packed segment is shared: its space is released through the
     * ledger (efs_pack_mapping_plan), never by per-inode GC. */
    if (dead->coding_profile_id == EFS_META_PROFILE_PACK)
        return EFS_ERR_BUSY;
    if (*n >= cap)
        return EFS_ERR_NOMEM;
    anch = efs_kv_anchor_shard(efs_kv_lane_shard(ino, lane));
    rc = efs_kv_key_gc(anch, ino, dead->generation, lane, ci,
                       gc_keys[*n], &kg);
    if (rc != EFS_OK)
        return rc;
    efs_meta_pack_gc(gc_vals[*n], dead->nodes, 0, dead->checksums);
    it[*n].op = EFS_KV_PUT;
    it[*n].key = gc_keys[*n];
    it[*n].klen = kg;
    it[*n].val = gc_vals[*n];
    it[*n].vlen = EFS_META_GC_VAL;
    (*n)++;
    /* The bump is at queue time, not batch commit: a failed apply drifts
     * the watermark upward (an extra bounded scan), never downward. */
    efs_meta_gc_pending_note(anch, 1);
    return EFS_OK;
}


/* ---- W154 degraded marks ---- */

/* Per-anchor mark count, the D26 GC watermark's rules: unknown until the
 * rebuild pass derives it from one prefix count (process start, snapshot
 * import); while unknown, inserts accumulate and removals are dropped, so
 * the count can only overshoot, and a pass that finds the prefix empty
 * clamps it back to zero. Adjusted after a batch commits. Zero lets a
 * rewrite skip the per-superseded-object lookup. */
static pthread_mutex_t dg_wm_mu = PTHREAD_MUTEX_INITIALIZER;
static int64_t dg_wm_count[GC_WM_ANCHOR_MAX + 1];
static int64_t dg_wm_pos[GC_WM_ANCHOR_MAX + 1];
static int dg_wm_known[GC_WM_ANCHOR_MAX + 1];

void degraded_note(uint32_t anchor, int delta)
{
    if (anchor == 0 || anchor > GC_WM_ANCHOR_MAX || !delta)
        return;
    pthread_mutex_lock(&dg_wm_mu);
    if (dg_wm_known[anchor]) {
        dg_wm_count[anchor] += delta;
        if (dg_wm_count[anchor] < 0)
            dg_wm_count[anchor] = 0;
    } else if (delta > 0) {
        dg_wm_pos[anchor] += delta;
    }
    pthread_mutex_unlock(&dg_wm_mu);
}

int64_t efs_meta_degraded_count_peek(uint32_t anchor)
{
    int64_t r;

    if (anchor == 0 || anchor > GC_WM_ANCHOR_MAX)
        return 0;
    pthread_mutex_lock(&dg_wm_mu);
    r = dg_wm_known[anchor] ? dg_wm_count[anchor] : -1;
    pthread_mutex_unlock(&dg_wm_mu);
    return r;
}

void efs_meta_degraded_count_derived(uint32_t anchor, uint64_t n)
{
    if (anchor == 0 || anchor > GC_WM_ANCHOR_MAX)
        return;
    pthread_mutex_lock(&dg_wm_mu);
    dg_wm_count[anchor] = (int64_t)n + dg_wm_pos[anchor];
    dg_wm_pos[anchor] = 0;
    dg_wm_known[anchor] = 1;
    pthread_mutex_unlock(&dg_wm_mu);
}

/* Call just before the derive scan. Inserts noted while unknown are rows the
 * scan counts itself; only those applied during the scan are kept, so the
 * derived count overshoots by at most the scan's window. A follower never
 * derives, so a leader elected later added every mark it ever applied
 * (k37 step 3: `Degraded chunks: 8` with all 8 marks retired). */
void efs_meta_degraded_count_begin(uint32_t anchor)
{
    if (anchor == 0 || anchor > GC_WM_ANCHOR_MAX)
        return;
    pthread_mutex_lock(&dg_wm_mu);
    if (!dg_wm_known[anchor])
        dg_wm_pos[anchor] = 0;
    pthread_mutex_unlock(&dg_wm_mu);
}

void efs_meta_degraded_count_invalidate(uint32_t anchor)
{
    if (anchor == 0 || anchor > GC_WM_ANCHOR_MAX)
        return;
    pthread_mutex_lock(&dg_wm_mu);
    dg_wm_known[anchor] = 0;
    dg_wm_count[anchor] = 0;
    dg_wm_pos[anchor] = 0;
    pthread_mutex_unlock(&dg_wm_mu);
}

int efs_meta_degraded_count_zero_if(uint32_t anchor, int64_t expect)
{
    int ok = 0;

    if (anchor == 0 || anchor > GC_WM_ANCHOR_MAX)
        return 0;
    pthread_mutex_lock(&dg_wm_mu);
    if (dg_wm_known[anchor] && dg_wm_count[anchor] == expect) {
        dg_wm_count[anchor] = 0;
        ok = 1;
    }
    pthread_mutex_unlock(&dg_wm_mu);
    return ok;
}

void efs_meta_pack_degraded(uint8_t *p, const struct efs_meta_degraded *d)
{
    int i;

    be64(p, d->inode_gen);
    p[8] = d->missing_mask;
    p[9] = p[10] = p[11] = 0;
    for (i = 0; i < EFS_NUM_FRAGMENTS; i++)
        efs_store_be32(p + 12 + (uint32_t)i * 4u, d->nodes[i]);
    memcpy(p + 12 + 4 * EFS_NUM_FRAGMENTS, d->checksums,
           EFS_HASH_SIZE * EFS_NUM_FRAGMENTS);
}

int efs_meta_unpack_degraded(const uint8_t *key, uint32_t klen,
                             const uint8_t *val, uint32_t vlen,
                             struct efs_meta_degraded *out)
{
    uint32_t shard;
    int i;

    if (!key || !val || !out || klen != 24 || key[2] != EFS_KV_KIND_DEGRADED ||
        vlen != EFS_META_DEGRADED_VAL || val[9] || val[10] || val[11])
        return EFS_ERR_PROTO;
    memset(out, 0, sizeof(*out));
    out->ino = rd64(key + 3);
    out->object_gen = rd64(key + 11);
    out->lane = key[19];
    out->chunk_index = efs_load_be32(key + 20);
    out->inode_gen = rd64(val);
    out->missing_mask = val[8];
    for (i = 0; i < EFS_NUM_FRAGMENTS; i++)
        out->nodes[i] = efs_load_be32(val + 12 + (uint32_t)i * 4u);
    memcpy(out->checksums, val + 12 + 4 * EFS_NUM_FRAGMENTS,
           EFS_HASH_SIZE * EFS_NUM_FRAGMENTS);
    shard = ((uint32_t)key[0] << 8) | key[1];
    if (!out->ino || !out->inode_gen || !out->object_gen ||
        out->lane >= EFS_META_LANES ||
        out->lane != out->chunk_index % EFS_META_LANES ||
        shard != efs_kv_anchor_shard(efs_kv_lane_shard(out->ino, out->lane)) ||
        !out->missing_mask || !efs_chunk_missing_valid(out->missing_mask))
        return EFS_ERR_PROTO;
    for (i = 0; i < EFS_NUM_FRAGMENTS; i++)
        if (!out->nodes[i] || out->nodes[i] > EFS_MAX_NODES)
            return EFS_ERR_PROTO;
    return EFS_OK;
}

int degraded_key(efs_ino_t ino, uint64_t object_gen, uint8_t lane,
                        uint32_t ci, uint8_t *key, uint32_t *kl)
{
    return efs_kv_key_degraded(efs_kv_anchor_shard(efs_kv_lane_shard(ino, lane)),
                               ino, object_gen, lane, ci, key, kl);
}

int efs_meta_degraded_live(struct efs_kv *kv, const struct efs_meta_degraded *d)
{
    struct efs_meta_chunk ch;
    struct efs_meta_delta ds[EFS_CHUNK_DELTA_MAX];
    uint32_t n = 0, i;
    int rc;

    if (!kv || !d)
        return EFS_ERR_INVAL;
    rc = efs_meta_apply_get_chunk_row(kv, d->ino, d->inode_gen, d->chunk_index,
                                      &ch, ds, EFS_CHUNK_DELTA_MAX, &n, NULL);
    if (rc == EFS_ERR_NOT_FOUND)
        return 0;
    if (rc != EFS_OK)
        return rc;
    /* Generations are unique object names: a match is this object, and a
     * placement that disagrees with the mark is a damaged row, not a
     * reason to rebuild anything. */
    if (ch.coding_profile_id != EFS_META_PROFILE_PACK &&
        ch.generation == d->object_gen)
        return memcmp(ch.nodes, d->nodes, sizeof(ch.nodes)) ||
               memcmp(ch.checksums, d->checksums, sizeof(ch.checksums))
                   ? EFS_ERR_PROTO : 1;
    if (n > EFS_CHUNK_DELTA_MAX)
        n = EFS_CHUNK_DELTA_MAX;
    for (i = 0; i < n; i++) {
        if (!ds[i].len || ds[i].generation != d->object_gen)
            continue;
        return memcmp(ds[i].nodes, d->nodes, sizeof(ds[i].nodes)) ||
               memcmp(ds[i].checksums, d->checksums, sizeof(ds[i].checksums))
                   ? EFS_ERR_PROTO : 1;
    }
    return 0;
}

int efs_meta_apply_degraded_clear(struct efs_kv *kv, const struct efs_meta_degraded *d,
                                  int rebuilt, int *outcome)
{
    uint8_t key[EFS_KV_KEY_MAX], val[EFS_META_DEGRADED_VAL];
    uint8_t gkey[EFS_KV_KEY_MAX], gval[EFS_META_GC_VAL];
    struct efs_kv_item it[2];
    struct efs_meta_degraded cur;
    uint32_t kl = 0, gl = 0, vn, n = 0, anchor;
    int rc, found = 0, new_gc = 0, fi;

    if (outcome)
        *outcome = EFS_META_DEGRADED_ABSENT;
    if (!kv || !d || !d->ino || !d->object_gen || d->lane >= EFS_META_LANES ||
        d->lane != d->chunk_index % EFS_META_LANES ||
        !d->missing_mask || !efs_chunk_missing_valid(d->missing_mask))
        return EFS_ERR_INVAL;
    rc = degraded_key(d->ino, d->object_gen, d->lane, d->chunk_index, key, &kl);
    if (rc != EFS_OK)
        return rc;
    anchor = ((uint32_t)key[0] << 8) | key[1];
    vn = sizeof(val);
    rc = efs_kv_get(kv, key, kl, val, &vn);
    if (rc == EFS_OK) {
        rc = efs_meta_unpack_degraded(key, kl, val, vn, &cur);
        if (rc != EFS_OK)
            return rc;
        /* Delete only the mark this clear was decided for. */
        if (cur.inode_gen == d->inode_gen && cur.missing_mask == d->missing_mask &&
            !memcmp(cur.nodes, d->nodes, sizeof(cur.nodes)) &&
            !memcmp(cur.checksums, d->checksums, sizeof(cur.checksums))) {
            it[n++] = (struct efs_kv_item){EFS_KV_DEL, key, kl, NULL, 0};
            found = 1;
            if (outcome)
                *outcome = EFS_META_DEGRADED_CLEARED;
        }
    } else if (rc != EFS_ERR_NOT_FOUND) {
        return rc;
    }
    if (rebuilt) {
        int live = efs_meta_degraded_live(kv, d);
        if (live < 0)
            return live;
        if (!live) {
            uint8_t ack_bits;
            efs_node_id_t gnodes[EFS_NUM_FRAGMENTS];
            uint8_t sums[EFS_NUM_FRAGMENTS][EFS_HASH_SIZE];

            /* A rewrite (or unlink, truncate, pack) won the race: the GC
             * record of the dead object may already have visited the
             * rebuilt node, so the fragment just written there is
             * queued again, checksum-guarded like any superseded one. */
            fi = __builtin_ctz(d->missing_mask);
            rc = efs_kv_key_gc(anchor, d->ino, d->object_gen, d->lane,
                               d->chunk_index, gkey, &gl);
            if (rc != EFS_OK)
                return rc;
            vn = sizeof(gval);
            rc = efs_kv_get(kv, gkey, gl, gval, &vn);
            if (rc == EFS_OK) {
                rc = efs_meta_unpack_gc(gval, vn, gnodes, &ack_bits, sums);
                if (rc != EFS_OK)
                    return rc;
                ack_bits &= (uint8_t)~(1u << fi);
            } else if (rc == EFS_ERR_NOT_FOUND) {
                memcpy(gnodes, d->nodes, sizeof(gnodes));
                memcpy(sums, d->checksums, sizeof(sums));
                ack_bits = (uint8_t)(((1u << EFS_NUM_FRAGMENTS) - 1u) & ~(1u << fi));
                new_gc = 1;
            } else {
                return rc;
            }
            efs_meta_pack_gc(gval, gnodes, ack_bits,
                             (const uint8_t (*)[EFS_HASH_SIZE])sums);
            it[n++] = (struct efs_kv_item){EFS_KV_PUT, gkey, gl, gval, EFS_META_GC_VAL};
            if (outcome)
                *outcome = EFS_META_DEGRADED_ORPHANED;
        }
    }
    if (!n)
        return EFS_OK;
    if (new_gc)
        efs_meta_gc_pending_note(anchor, 1);
    rc = meta_write_batch(kv, it, n);
    if (rc == EFS_OK && found)
        degraded_note(anchor, -1);
    return rc;
}

int efs_meta_apply_degraded_mark(struct efs_kv *kv, efs_ino_t ino, uint64_t inode_gen,
                                 uint64_t object_gen, uint32_t ci, uint32_t fi,
                                 efs_node_id_t node, int *added)
{
    struct efs_meta_chunk ch;
    struct efs_meta_delta ds[EFS_CHUNK_DELTA_MAX];
    struct efs_meta_degraded d, cur;
    uint8_t key[EFS_KV_KEY_MAX], val[EFS_META_DEGRADED_VAL];
    struct efs_kv_item it;
    uint32_t n = 0, i, kl = 0, vn = sizeof(val);
    int rc, found = 0;

    if (added)
        *added = 0;
    if (!kv || !ino || !inode_gen || !object_gen || fi >= EFS_NUM_FRAGMENTS || !node ||
        node > EFS_MAX_NODES)
        return EFS_ERR_INVAL;
    memset(&d, 0, sizeof(d));
    rc = efs_meta_apply_get_chunk_row(kv, ino, inode_gen, ci, &ch, ds, EFS_CHUNK_DELTA_MAX,
                                      &n, NULL);
    if (rc != EFS_OK)
        return rc;
    /* The same object test as efs_meta_degraded_live: the base (never a
     * packed reference) or a live span with this generation. */
    if (ch.coding_profile_id != EFS_META_PROFILE_PACK && ch.generation == object_gen) {
        memcpy(d.nodes, ch.nodes, sizeof(d.nodes));
        memcpy(d.checksums, ch.checksums, sizeof(d.checksums));
        found = 1;
    }
    if (n > EFS_CHUNK_DELTA_MAX)
        n = EFS_CHUNK_DELTA_MAX;
    for (i = 0; !found && i < n; i++) {
        if (!ds[i].len || ds[i].generation != object_gen)
            continue;
        memcpy(d.nodes, ds[i].nodes, sizeof(d.nodes));
        memcpy(d.checksums, ds[i].checksums, sizeof(d.checksums));
        found = 1;
    }
    if (!found)
        return EFS_ERR_NOT_FOUND;
    if (d.nodes[fi] != node)
        return EFS_ERR_STALE;
    for (i = 0; i < EFS_NUM_FRAGMENTS; i++)
        if (!d.nodes[i] || d.nodes[i] > EFS_MAX_NODES)
            return EFS_ERR_PROTO;
    d.ino = ino;
    d.inode_gen = inode_gen;
    d.object_gen = object_gen;
    d.chunk_index = ci;
    d.lane = (uint8_t)(ci % EFS_META_LANES);
    d.missing_mask = (uint8_t)(1u << fi);
    rc = degraded_key(ino, object_gen, d.lane, ci, key, &kl);
    if (rc != EFS_OK)
        return rc;
    rc = efs_kv_get(kv, key, kl, val, &vn);
    if (rc == EFS_OK) {
        rc = efs_meta_unpack_degraded(key, kl, val, vn, &cur);
        if (rc != EFS_OK)
            return rc;
        /* Published degraded already (or marked by an earlier fsync). */
        return cur.missing_mask == d.missing_mask ? EFS_OK : EFS_ERR_STALE;
    }
    if (rc != EFS_ERR_NOT_FOUND)
        return rc;
    efs_meta_pack_degraded(val, &d);
    it = (struct efs_kv_item){EFS_KV_PUT, key, kl, val, EFS_META_DEGRADED_VAL};
    rc = meta_write_batch(kv, &it, 1);
    if (rc != EFS_OK)
        return rc;
    degraded_note(((uint32_t)key[0] << 8) | key[1], 1);
    if (added)
        *added = 1;
    return EFS_OK;
}

int efs_meta_apply_recovery_mark(struct efs_kv *kv, const struct efs_meta_degraded *d)
{
    uint8_t key[EFS_KV_KEY_MAX], val[EFS_META_DEGRADED_VAL];
    struct efs_meta_degraded cur;
    uint32_t kl = 0, vn = sizeof(val), anchor;
    int rc, live;

    if (!kv || !d || !d->ino || !d->inode_gen || !d->object_gen ||
        d->lane >= EFS_META_LANES || d->lane != d->chunk_index % EFS_META_LANES ||
        !d->missing_mask || !efs_chunk_missing_valid(d->missing_mask))
        return EFS_ERR_INVAL;
    live = efs_meta_degraded_live(kv, d);
    if (live < 0)
        return live;
    if (!live)
        return EFS_ERR_NOT_FOUND;
    rc = degraded_key(d->ino, d->object_gen, d->lane, d->chunk_index, key, &kl);
    if (rc != EFS_OK)
        return rc;
    rc = efs_kv_get(kv, key, kl, val, &vn);
    if (rc == EFS_OK) {
        rc = efs_meta_unpack_degraded(key, kl, val, vn, &cur);
        if (rc != EFS_OK)
            return rc;
        /* One mark per object: a second missing fragment of a 2+1 stripe
         * cannot be rebuilt, and is reported, never overwritten. */
        return cur.inode_gen == d->inode_gen && cur.missing_mask == d->missing_mask
                   ? EFS_OK : EFS_ERR_STALE;
    }
    if (rc != EFS_ERR_NOT_FOUND)
        return rc;
    anchor = ((uint32_t)key[0] << 8) | key[1];
    efs_meta_pack_degraded(val, d);
    rc = meta_write_batch(kv, &(struct efs_kv_item){EFS_KV_PUT, key, kl, val,
                                                     EFS_META_DEGRADED_VAL}, 1);
    if (rc == EFS_OK)
        degraded_note(anchor, 1);
    return rc;
}

/* One object of a chunk row placed on `node`: the mark it would carry. */
static int chunk_ref_emit(const uint8_t *key, uint64_t object_gen,
                          const efs_node_id_t nodes[EFS_NUM_FRAGMENTS],
                          const uint8_t sums[EFS_NUM_FRAGMENTS][EFS_HASH_SIZE],
                          efs_node_id_t node, efs_meta_chunk_ref_fn cb, void *user)
{
    for (int i = 0; i < EFS_NUM_FRAGMENTS; i++) {
        if (nodes[i] != node)
            continue;
        struct efs_meta_degraded r = {
            .ino = rd64(key + 3), .inode_gen = rd64(key + 11), .object_gen = object_gen,
            .chunk_index = efs_load_be32(key + 20), .lane = key[19], .missing_mask = (uint8_t)(1u << i)};
        memcpy(r.nodes, nodes, sizeof(r.nodes));
        memcpy(r.checksums, sums, sizeof(r.checksums));
        int stop = cb(user, &r, key, EFS_KV_CHUNK_KEY_LEN);
        if (stop)
            return stop;
    }
    return 0;
}

struct chunk_refs_scan {
    const uint8_t *after;
    uint32_t alen;
    efs_node_id_t node;
    efs_meta_chunk_ref_fn cb;
    void *user;
    int rc;
};

static int chunk_refs_cb(void *user, const uint8_t *key, uint32_t klen,
                         const uint8_t *val, uint32_t vlen)
{
    struct chunk_refs_scan *c = user;
    struct efs_meta_chunk ch;
    uint32_t i, nd;
    int stop;

    if (c->alen && klen == c->alen && !memcmp(key, c->after, klen))
        return 0;
    if (klen != EFS_KV_CHUNK_KEY_LEN || key[2] != EFS_KV_KIND_CHUNK ||
        unpack_chunk(val, vlen, &ch) != EFS_OK) {
        c->rc = EFS_ERR_PROTO; /* a row recovery cannot read is not skipped */
        return 1;
    }
    if (ch.coding_profile_id != EFS_META_PROFILE_PACK && ch.generation &&
        (stop = chunk_ref_emit(key, ch.generation, ch.nodes,
                               (const uint8_t (*)[EFS_HASH_SIZE])ch.checksums,
                               c->node, c->cb, c->user)))
        return stop;
    nd = trailer_n(val, vlen);
    for (i = 0; i < nd; i++) {
        struct efs_meta_delta d;
        if (trailer_at(val, vlen, i, &d) != EFS_OK) {
            c->rc = EFS_ERR_PROTO;
            return 1;
        }
        if (!d.len || !d.generation)
            continue;
        stop = chunk_ref_emit(key, d.generation, d.nodes,
                              (const uint8_t (*)[EFS_HASH_SIZE])d.checksums,
                              c->node, c->cb, c->user);
        if (stop)
            return stop;
    }
    return 0;
}

int efs_meta_chunk_refs_scan(struct efs_kv *kv, uint32_t shard, const uint8_t *after,
                             uint32_t alen, efs_node_id_t node, efs_meta_chunk_ref_fn cb,
                             void *user)
{
    uint8_t prefix[3] = {(uint8_t)(shard >> 8), (uint8_t)shard, EFS_KV_KIND_CHUNK};
    struct chunk_refs_scan c = {after, alen, node, cb, user, EFS_OK};
    int rc;

    if (!kv || !cb || !node || shard > EFS_KV_SHARD_MASK || (alen && !after))
        return EFS_ERR_INVAL;
    rc = alen ? efs_kv_scan_from(kv, prefix, 3, after, alen, chunk_refs_cb, &c)
              : efs_kv_scan_prefix(kv, prefix, 3, chunk_refs_cb, &c);
    if (c.rc != EFS_OK)
        return c.rc;
    return rc;
}

/* Queue the deletion of `gen`'s degraded mark when one exists. A known-zero
 * count for the anchor skips the lookup. *removed counts queued deletes. */
int degraded_retire(struct efs_kv *kv, struct efs_kv_item *it, uint32_t *n,
                           uint32_t cap, uint8_t (*keys)[EFS_KV_KEY_MAX], uint32_t *nk,
                           efs_ino_t ino, uint8_t lane, uint32_t ci, uint64_t gen,
                           uint32_t *removed)
{
    uint8_t val[EFS_META_DEGRADED_VAL];
    uint32_t kl = 0, vn = sizeof(val);
    int rc;

    if (!gen)
        return EFS_OK;
    if (efs_meta_degraded_count_peek(efs_kv_anchor_shard(efs_kv_lane_shard(ino, lane))) == 0)
        return EFS_OK;
    if (*n >= cap)
        return EFS_ERR_NOMEM;
    rc = degraded_key(ino, gen, lane, ci, keys[*nk], &kl);
    if (rc != EFS_OK)
        return rc;
    rc = efs_kv_get(kv, keys[*nk], kl, val, &vn);
    if (rc == EFS_ERR_NOT_FOUND)
        return EFS_OK;
    if (rc != EFS_OK)
        return rc;
    it[*n] = (struct efs_kv_item){EFS_KV_DEL, keys[*nk], kl, NULL, 0};
    (*n)++;
    (*nk)++;
    (*removed)++;
    return EFS_OK;
}


int evidence_ok(const struct efs_meta_pub *p)
{
    int i, j;

    if (!p || p->candidate_gen == 0)
        return EFS_ERR_INVAL;
    if (p->coding_profile_id != EFS_META_PROFILE_K2F1) return EFS_ERR_STALE;
    for (i = 0; i < EFS_NUM_FRAGMENTS; i++) {
        if (p->ch.nodes[i] == 0)
            return EFS_ERR_INVAL;
        for (j = 0; j < i; j++) {
            if (p->ch.nodes[i] == p->ch.nodes[j])
                return EFS_ERR_INVAL;
        }
    }
    return EFS_OK;
}
