/* meta_apply/sweep.c: lane sweep, reap selection and batches, GC ACK. */

#include "apply_internal.h"
#include "sweep.h"
#include "rows.h"
#include "xattr.h"
#include "gc.h"

int packed_retirement_commit(struct efs_kv *kv, efs_ino_t ino,
    uint64_t gen, struct packed_retirement *retirement,
    struct efs_kv_item *it, uint32_t n, uint32_t cap)
{
    if (!retirement->present) return meta_write_batch(kv, it, n);
    struct efs_pack_mapping_change change = {ino, gen, 0,
        &retirement->before, retirement->has_after ? &retirement->after : NULL};
    struct efs_pack_mapping_plan plan = {0};
    int rc = efs_pack_mapping_plan(kv, &change, 1, &plan);
    if (rc == EFS_OK && plan.count > cap - n) rc = EFS_ERR_NOMEM;
    if (rc == EFS_OK) {
        memcpy(it + n, plan.items, plan.count * sizeof(it[0]));
        rc = meta_write_batch(kv, it, n + plan.count);
    }
    efs_pack_mapping_plan_free(&plan);
    return rc;
}

/* Lane sweep batch: 64 chunk keys per KV batch (each a DEL + a GC record),
 * then the lane key. 64 keeps one entry's apply cost bounded while still
 * draining a 1 TB file's lane (~8192 chunks) in ~128 batches inside the
 * single LANE_SWEEP entry — tens of ms, far under the election deadline. */
#define SWEEP_CHUNKS 64

struct sweep_scan {
    efs_ino_t ino;
    uint64_t gen;
    struct efs_kv_item *it;
    uint8_t (*keys)[EFS_KV_KEY_MAX];
    uint8_t (*gc_vals)[EFS_META_GC_VAL];
    uint32_t n;
    uint32_t cap;
    uint32_t chunks;
    struct efs_meta_chunk packed_dead;
    uint32_t packed_ci;
    int has_packed;
    int full;
    int rc;
    int nogc; /* W124 reaped inode: node fences delete the whole inode dir */
};

/* A packed file has exactly one base, at chunk zero. Its reference is
 * released in the same atomic batch as the authoritative delete; that holds
 * for a W124 reaped inode too, whose directory never holds the segment. */
static int sweep_commit(struct efs_kv *kv, struct sweep_scan *ss)
{
    if (!ss->has_packed) return meta_write_batch(kv, ss->it, ss->n);
    struct efs_pack_mapping_change change = {ss->ino, ss->gen,
        ss->packed_ci, &ss->packed_dead, NULL};
    struct efs_pack_mapping_plan plan = {0};
    int rc = efs_pack_mapping_plan(kv, &change, 1, &plan);
    if (rc == EFS_OK && plan.count > ss->cap - ss->n) rc = EFS_ERR_NOMEM;
    if (rc == EFS_OK) {
        memcpy(ss->it + ss->n, plan.items, plan.count * sizeof(ss->it[0]));
        rc = meta_write_batch(kv, ss->it, ss->n + plan.count);
    }
    efs_pack_mapping_plan_free(&plan);
    return rc;
}

/* Reserve every dead identity before queuing its only reference's deletion. */
static int chunk_gc_slots(const uint8_t *val, uint32_t vlen,
                           const struct efs_meta_chunk *dead, uint32_t *slots)
{
    uint32_t nd = 0, hdr = chunk_header_size(val, vlen);
    if (vlen != hdr) {
        if (vlen < hdr + DELTA_HDR)
            return EFS_ERR_PROTO;
        nd = ((uint32_t)val[hdr] << 8) | val[hdr + 1];
        if (nd > EFS_CHUNK_DELTA_MAX ||
            vlen < hdr + DELTA_HDR + nd * DELTA_REC)
            return EFS_ERR_PROTO;
    }
    *slots = 1u + (dead->generation != 0);
    for (uint32_t i = 0; i < nd; i++) {
        struct efs_meta_delta dd;
        int rc = trailer_at(val, vlen, i, &dd);
        if (rc != EFS_OK)
            return rc;
        *slots += dd.generation != 0;
    }
    return EFS_OK;
}

static int sweep_cb(void *user, const uint8_t *key, uint32_t klen,
                    const uint8_t *val, uint32_t vlen)
{
    struct sweep_scan *ss = user;
    struct efs_meta_chunk dead;
    efs_ino_t ino;
    uint64_t gen;
    uint32_t ci, need;
    uint8_t lane;

    if (klen < 24 || key[2] != EFS_KV_KIND_CHUNK)
        return 0;
    ino = rd64(key + 3);
    gen = rd64(key + 11);
    lane = key[19];
    ci = efs_load_be32(key + 20);
    if (ino != ss->ino || gen != ss->gen)
        return 0;
    ss->rc = unpack_chunk(val, vlen, &dead);
    if (ss->rc == EFS_OK)
        ss->rc = chunk_gc_slots(val, vlen, &dead, &need);
    if (ss->rc != EFS_OK)
        return 1;
    if (need > ss->cap - ss->n - (ss->has_packed ? 1u : 0u)) {
        ss->full = 1;
        return 1;
    }
    memcpy(ss->keys[ss->n], key, klen);
    ss->it[ss->n].op = EFS_KV_DEL;
    ss->it[ss->n].key = ss->keys[ss->n];
    ss->it[ss->n].klen = klen;
    ss->it[ss->n].val = NULL; /* `it` is an uninitialised stack array */
    ss->it[ss->n].vlen = 0;
    ss->n++;
    if (dead.coding_profile_id == EFS_META_PROFILE_PACK) {
        if (ci || lane || ss->has_packed) { ss->rc = EFS_ERR_PROTO; return 1; }
        ss->packed_dead = dead; ss->packed_ci = ci; ss->has_packed = 1;
    } else if (dead.generation != 0 && !ss->nogc) {
        ss->rc = gc_queue(ss->it, &ss->n, ss->cap, ss->keys, ss->gc_vals,
                          ino, lane, ci, &dead);
        if (ss->rc != EFS_OK)
            return 1;
    }
    if (!ss->nogc) {
        uint32_t di, nd = trailer_n(val, vlen);

        for (di = 0; di < nd; di++) {
            struct efs_meta_delta dd;
            struct efs_meta_chunk span;

            ss->rc = trailer_at(val, vlen, di, &dd);
            if (ss->rc != EFS_OK)
                return 1;
            if (dd.generation == 0)
                continue;
            memset(&span, 0, sizeof(span));
            span.generation = dd.generation;
            memcpy(span.nodes, dd.nodes, sizeof(span.nodes));
            memcpy(span.checksums, dd.checksums, sizeof(span.checksums));
            ss->rc = gc_queue(ss->it, &ss->n, ss->cap, ss->keys, ss->gc_vals,
                              ino, lane, ci, &span);
            if (ss->rc != EFS_OK)
                return 1;
        }
    }
    ss->chunks++;
    return 0;
}

int efs_meta_apply_lane_sweep(struct efs_kv *kv, efs_ino_t ino, uint64_t gen,
                              uint8_t lane)
{
    struct efs_kv_item it[SWEEP_CHUNKS * 2];
    uint8_t keys[SWEEP_CHUNKS * 2][EFS_KV_KEY_MAX];
    uint8_t gc_vals[SWEEP_CHUNKS * 2][EFS_META_GC_VAL];
    uint8_t pref[EFS_KV_KEY_MAX], k_ln[EFS_KV_KEY_MAX];
    uint32_t plen = 0, kl = 0;
    struct sweep_scan ss;
    uint32_t lsh;
    int rc;

    if (!kv || ino == 0 || lane >= EFS_META_LANES)
        return EFS_ERR_INVAL;
    lsh = efs_kv_lane_shard(ino, lane);
    rc = efs_kv_key_chunk(lsh, ino, gen, lane, 0, pref, &plen);
    if (rc == EFS_OK)
        rc = efs_kv_key_lane(lsh, ino, gen, lane, k_ln, &kl);
    if (rc != EFS_OK)
        return rc;
    plen = 20;
    for (;;) {
        memset(&ss, 0, sizeof(ss));
        ss.ino = ino;
        ss.gen = gen;
        ss.it = it;
        ss.keys = keys;
        ss.gc_vals = gc_vals;
        ss.cap = (uint32_t)(sizeof(it) / sizeof(it[0]));
        rc = efs_kv_scan_prefix(kv, pref, plen, sweep_cb, &ss);
        if (ss.rc != EFS_OK)
            return ss.rc;
        /* rc > 0 is sweep_cb's "batch full, stop" (merge_scan propagates
         * the callback's return), not an error. Treating it as one made
         * every lane with more than SWEEP_CHUNKS chunks unsweepable: the
         * reaper re-proposed LANE_SWEEP once a second forever and no
         * fragment of any file over ~8 MiB per lane was ever reclaimed
         * (IO-500 ior-easy 1.2 GiB files: `apply lane-sweep rc=1` every
         * 33 entries). */
        if (rc < 0)
            return rc;
        if (ss.chunks == 0)
            break;
        rc = sweep_commit(kv, &ss);
        if (rc != EFS_OK)
            return rc;
        if (!ss.full)
            break;
    }
    /* The lane key itself is the last thing to go. A replay finds no
     * chunks and re-deletes an absent lane key, which is a no-op. Only the
     * reaper sweeps lanes, after the last name of the inode is gone. */
    rc = meta_cleanup_del(kv, k_ln, kl);
    if (rc == EFS_ERR_NOT_FOUND)
        rc = EFS_OK;
    return rc;
}

struct sweep_selection {
    uint32_t *indices, n;
};

static int sweep_select_cb(void *user, const uint8_t *key, uint32_t klen,
                            const uint8_t *val, uint32_t vlen)
{
    struct sweep_selection *s = user;
    (void)val; (void)vlen;
    if (klen != 24) return EFS_ERR_PROTO;
    s->indices[s->n++] = efs_load_be32(key + 20);
    return s->n == EFS_META_SWEEP_BATCH_MAX ? 1 : 0;
}

int efs_meta_lane_sweep_select(struct efs_kv *kv, efs_ino_t ino, uint64_t gen,
                               uint8_t lane, uint32_t *indices, uint32_t *n)
{
    uint8_t pref[EFS_KV_KEY_MAX]; uint32_t plen;
    struct sweep_selection s = {.indices = indices};
    if (!kv || !ino || lane >= EFS_META_LANES || !indices || !n)
        return EFS_ERR_INVAL;
    *n = 0;
    int rc = efs_kv_key_chunk(efs_kv_lane_shard(ino, lane), ino, gen, lane,
                              0, pref, &plen);
    if (rc != EFS_OK) return rc;
    rc = efs_kv_scan_prefix(kv, pref, 20, sweep_select_cb, &s);
    if (rc < 0) return rc;
    *n = s.n;
    return EFS_OK;
}

/* Unlike a scan-and-delete step, the immutable command names exactly the
 * keys it may remove. A storage/apply-marker retry therefore cannot advance
 * into another batch. All source deletes and GC references commit together. */
static int lane_sweep_batch(struct efs_kv *kv, efs_ino_t ino, uint64_t gen,
                            uint8_t lane, const uint32_t *indices,
                            uint32_t n, int nogc)
{
    struct efs_kv_item it[SWEEP_CHUNKS * 2];
    uint8_t keys[SWEEP_CHUNKS * 2][EFS_KV_KEY_MAX];
    uint8_t gc_vals[SWEEP_CHUNKS * 2][EFS_META_GC_VAL];
    struct sweep_scan ss = {.ino=ino, .gen=gen, .it=it, .keys=keys,
        .gc_vals=gc_vals, .cap=SWEEP_CHUNKS * 2, .nogc=nogc};
    uint8_t key[EFS_KV_KEY_MAX]; uint32_t kl;
    if (!kv || !ino || lane >= EFS_META_LANES || n > EFS_META_SWEEP_BATCH_MAX ||
        (n && !indices)) return EFS_ERR_INVAL;
    for (uint32_t i=0; i<n; i++) {
        if (indices[i] % EFS_META_LANES != lane || (i && indices[i] <= indices[i-1]))
            return EFS_ERR_INVAL;
    }
    for (uint32_t i=0; i<n; i++) {
        /* Same capacity as the canonical chunk/history decoder. */
        uint8_t value[CHUNK_VAL_MAX]; uint32_t vl=sizeof(value);
        int rc = efs_kv_key_chunk(efs_kv_lane_shard(ino,lane), ino, gen, lane,
                                  indices[i], key, &kl);
        if (rc != EFS_OK) return rc;
        rc = efs_kv_get(kv,key,kl,value,&vl);
        if (rc == EFS_ERR_NOT_FOUND) continue;
        if (rc != EFS_OK) return rc;
        (void)sweep_cb(&ss,key,kl,value,vl);
        if (ss.rc != EFS_OK) return ss.rc;
        if (ss.full) return EFS_ERR_NOMEM; /* no partial writes */
    }
    if (ss.n) {
        int rc=sweep_commit(kv, &ss);
        if (rc != EFS_OK) return rc;
    }
    uint32_t remaining[EFS_META_SWEEP_BATCH_MAX], count;
    int rc=efs_meta_lane_sweep_select(kv,ino,gen,lane,remaining,&count);
    if (rc != EFS_OK) return rc;
    if (count) return EFS_ERR_BUSY;
    rc=efs_kv_key_lane(efs_kv_lane_shard(ino,lane),ino,gen,lane,key,&kl);
    if (rc != EFS_OK) return rc;
    rc=meta_cleanup_del(kv,key,kl);
    return rc == EFS_ERR_NOT_FOUND ? EFS_OK : rc;
}

int efs_meta_apply_lane_sweep_batch(struct efs_kv *kv, efs_ino_t ino, uint64_t gen,
                                    uint8_t lane, const uint32_t *indices,
                                    uint32_t n)
{
    return lane_sweep_batch(kv, ino, gen, lane, indices, n, 0);
}
/* W124: sweep a reaped inode's lane without queuing per-chunk GC records.
 * Every member's reclaim fence deletes the whole inode directory, including
 * fragments of losing or never-published PUTs, so the records would only add
 * per-fragment delete RPCs and acks for files already being removed. */
int efs_meta_apply_lane_sweep_reaped(struct efs_kv *kv, efs_ino_t ino, uint64_t gen,
                                     uint8_t lane, const uint32_t *indices,
                                     uint32_t n)
{
    return lane_sweep_batch(kv, ino, gen, lane, indices, n, 1);
}

struct rsv_purge {
    struct efs_kv_item *it;
    uint8_t (*keys)[EFS_KV_KEY_MAX];
    uint32_t n;
    uint32_t cap;
    uint32_t found;
    int full;
};

static int rsv_purge_cb(void *user, const uint8_t *key, uint32_t klen,
                        const uint8_t *val, uint32_t vlen)
{
    struct rsv_purge *rp = user;

    (void)val;
    (void)vlen;
    if (rp->n >= rp->cap) {
        rp->full = 1;
        return 1;
    }
    memcpy(rp->keys[rp->n], key, klen);
    rp->it[rp->n].op = EFS_KV_DEL;
    rp->it[rp->n].key = rp->keys[rp->n];
    rp->it[rp->n].klen = klen;
    rp->it[rp->n].val = NULL;
    rp->it[rp->n].vlen = 0;
    rp->n++;
    rp->found++;
    return 0;
}

struct reap_selection {
    struct efs_meta_reap_key *keys;
    uint32_t n;
};

static int reap_select_cb(void *user, const uint8_t *key, uint32_t kl,
                           const uint8_t *value, uint32_t vl)
{
    struct reap_selection *s = user;
    (void)value; (void)vl;
    if (kl > EFS_META_REAP_KEY_MAX) return EFS_ERR_PROTO;
    memset(&s->keys[s->n], 0, sizeof(s->keys[s->n]));
    s->keys[s->n].len = (uint8_t)kl;
    memcpy(s->keys[s->n++].key, key, kl);
    return s->n == EFS_META_REAP_BATCH_MAX ? 1 : 0;
}

int efs_meta_reap_select(struct efs_kv *kv, efs_ino_t ino, uint64_t gen,
                          struct efs_meta_reap_key *keys, uint32_t *n)
{
    uint8_t prefix[EFS_KV_KEY_MAX]; uint32_t plen;
    struct reap_selection s = {.keys = keys};
    if (!kv || !ino || !keys || !n) return EFS_ERR_INVAL;
    *n = 0;
    int rc = efs_kv_key_append_rsv_prefix(efs_kv_inode_shard(ino), ino, gen,
                                         prefix, &plen);
    if (rc == EFS_OK) rc = efs_kv_scan_prefix(kv,prefix,plen,reap_select_cb,&s);
    if (rc < 0) return rc;
    if (s.n < EFS_META_REAP_BATCH_MAX) {
        rc = efs_kv_key_inode(efs_kv_inode_shard(ino),ino,prefix,&plen);
        if (rc != EFS_OK) return rc;
        prefix[2] = EFS_KV_KIND_APPEND_RECEIPT;
        rc = efs_kv_scan_prefix(kv,prefix,plen,reap_select_cb,&s);
        if (rc < 0) return rc;
    }
    *n = s.n;
    return EFS_OK;
}

int efs_meta_apply_reap_batch(struct efs_kv *kv, efs_ino_t ino, uint64_t gen,
                               const struct efs_meta_reap_key *keys, uint32_t n)
{
    struct efs_kv_item items[EFS_META_REAP_BATCH_MAX];
    uint8_t prefix[EFS_KV_KEY_MAX]; uint32_t plen;
    struct efs_meta_row row;
    if (!kv || !ino || n > EFS_META_REAP_BATCH_MAX || (n && !keys))
        return EFS_ERR_INVAL;
    int rc = efs_kv_key_inode(efs_kv_inode_shard(ino),ino,prefix,&plen);
    if (rc != EFS_OK) return rc;
    /* Reject the complete malformed set before touching any key. */
    for (uint32_t i=0; i<n; i++) {
        const uint8_t *k = keys[i].key;
        if ((keys[i].len != 27 || k[2] != EFS_KV_KIND_APPEND_RSV || rd64(k+11) != gen) &&
            (keys[i].len != 31 || k[2] != EFS_KV_KIND_APPEND_RECEIPT))
            return EFS_ERR_INVAL;
        if (memcmp(k,prefix,2) || memcmp(k+3,prefix+3,8)) return EFS_ERR_INVAL;
        for (uint32_t j=0; j<i; j++)
            if (keys[j].len == keys[i].len && !memcmp(keys[j].key,k,keys[i].len))
                return EFS_ERR_INVAL;
        memset(&items[i],0,sizeof(items[i]));
        items[i].op=EFS_KV_DEL;items[i].key=k;items[i].klen=keys[i].len;
    }
    rc = efs_meta_apply_get_inode(kv,ino,&row);
    if (rc == EFS_OK) {
        if (row.generation != gen || row.nlink)
            return efs_meta_apply_reap_done(kv,ino,gen); /* stale marker only */
        int held = efs_lease_any(kv,ino,gen);
        if (held < 0) return held;
        if (held) return EFS_ERR_BUSY;
    } else if (rc != EFS_ERR_NOT_FOUND) return rc;
    if (n && (rc=meta_write_batch(kv,items,n)) != EFS_OK) return rc;
    struct efs_meta_reap_key remaining[EFS_META_REAP_BATCH_MAX]; uint32_t count;
    rc=efs_meta_reap_select(kv,ino,gen,remaining,&count);
    if (rc != EFS_OK) return rc;
    if (count) return EFS_ERR_BUSY;
    /* No append rows remain: historical finalization performs only empty
     * prefix scans and fixed cursor/marker/inode cleanup. */
    return efs_meta_apply_reap_done(kv,ino,gen);
}

int efs_meta_apply_reap_done(struct efs_kv *kv, efs_ino_t ino, uint64_t gen)
{
    struct efs_meta_row row;
    struct efs_kv_item it[6];
    struct efs_expiry_change expiry_change;
    uint8_t k_reap[EFS_KV_KEY_MAX], k_cur[EFS_KV_KEY_MAX];
    uint8_t k_ino[EFS_KV_KEY_MAX], k_xa[EFS_KV_KEY_MAX];
    uint32_t kr = 0, kc = 0, ki = 0, kxa = 0, n = 0;
    uint32_t ish;
    int rc, held, drop_row = 0;

    if (!kv || ino == 0)
        return EFS_ERR_INVAL;
    ish = efs_kv_inode_shard(ino);
    /* Defensive: the marker and the row delete commit in one batch, so a
     * live row here should be impossible. If one shows up anyway (a row
     * re-created under the same ino is not a thing that exists), the
     * marker is the stale part — drop it and keep the row. */
    rc = efs_meta_apply_get_inode(kv, ino, &row);
    if (rc == EFS_OK) {
        if (row.generation != gen || row.nlink != 0) {
            rc = efs_kv_key_reap(efs_kv_anchor_shard(ish), ino, k_reap, &kr);
            if (rc != EFS_OK)
                return rc;
            rc = meta_write_del(kv, k_reap, kr);
            if (rc == EFS_ERR_NOT_FOUND)
                rc = EFS_OK;
            return rc;
        }
        held = efs_lease_any(kv, ino, row.generation);
        if (held < 0)
            return held;
        if (!held)
            drop_row = 1;
    } else if (rc != EFS_ERR_NOT_FOUND) {
        return rc;
    }
    /* Leftover append reservations of a killed appender. Bounded batches;
     * each batch's deletes make the next scan progress. */
    for (;;) {
        struct efs_kv_item rit[32];
        uint8_t rkeys[32][EFS_KV_KEY_MAX];
        uint8_t pref[EFS_KV_KEY_MAX];
        uint32_t plen = 0;
        struct rsv_purge rp;

        rc = efs_kv_key_append_rsv_prefix(ish, ino, gen, pref, &plen);
        if (rc != EFS_OK)
            return rc;
        memset(&rp, 0, sizeof(rp));
        rp.it = rit;
        rp.keys = rkeys;
        rp.cap = 32;
        rc = efs_kv_scan_prefix(kv, pref, plen, rsv_purge_cb, &rp);
        if (rc < 0) /* rc > 0 = callback's batch-full stop, not an error */
            return rc;
        if (rp.found == 0)
            break;
        rc = meta_write_batch(kv, rit, rp.n);
        if (rc != EFS_OK)
            return rc;
        if (!rp.full)
            break;
    }
    if (drop_row || efs_meta_apply_get_inode(kv, ino, &row) == EFS_ERR_NOT_FOUND) {
        for (;;) {
            struct efs_kv_item rit[32];
            uint8_t keys[32][EFS_KV_KEY_MAX], prefix[EFS_KV_KEY_MAX];
            uint32_t plen = 0;
            struct rsv_purge rp = {.it = rit, .keys = keys, .cap = 32};
            rc = efs_kv_key_inode(ish, ino, prefix, &plen);
            if (rc != EFS_OK) return rc;
            prefix[2] = EFS_KV_KIND_APPEND_RECEIPT;
            rc = efs_kv_scan_prefix(kv, prefix, plen, rsv_purge_cb, &rp);
            if (rc < 0) return rc;
            if (!rp.found) break;
            rc = meta_write_batch(kv, rit, rp.n);
            if (rc != EFS_OK) return rc;
            if (!rp.full) break;
        }
    }
    rc = efs_kv_key_append_cur(ish, ino, gen, k_cur, &kc);
    if (rc == EFS_OK)
        rc = efs_kv_key_reap(efs_kv_anchor_shard(ish), ino, k_reap, &kr);
    if (rc != EFS_OK)
        return rc;
    memset(it, 0, sizeof(it));
    it[n].op = EFS_KV_DEL;
    it[n].key = k_cur;
    it[n].klen = kc;
    n++;
    it[n].op = EFS_KV_DEL;
    it[n].key = k_reap;
    it[n].klen = kr;
    n++;
    if (drop_row) {
        rc = efs_kv_key_inode(ish, ino, k_ino, &ki);
        if (rc != EFS_OK)
            return rc;
        int xa = 0;

        it[n].op = EFS_KV_DEL;
        it[n].key = k_ino;
        it[n].klen = ki;
        n++;
        rc = xattr_del_item(kv, ino, k_xa, &kxa, &it[n], &xa);
        if (rc != EFS_OK)
            return rc;
        n += (uint32_t)xa;
    }
    if(drop_row || efs_meta_apply_get_inode(kv,ino,&row)==EFS_ERR_NOT_FOUND) {
        struct efs_expiry_record expiry;
        rc=efs_expiry_get(kv,ino,gen,&expiry);
        if(rc!=EFS_OK)return rc;
        if(expiry.revision) {
            expiry.deadline_ns=0;
            rc=efs_expiry_plan(kv,ino,gen,expiry.revision,&expiry,&expiry_change);
            if(rc!=EFS_OK)return rc;
            expiry_change.items[0].op=EFS_KV_DEL;
            expiry_change.items[0].val=NULL;expiry_change.items[0].vlen=0;
            if(n+expiry_change.count>sizeof(it)/sizeof(it[0]))return EFS_ERR_OVERFLOW;
            for(uint32_t i=0;i<expiry_change.count;i++)it[n++]=expiry_change.items[i];
        }
    }
    /* The row is absent or has nlink 0 and no lease (checked above). */
    return meta_cleanup_batch(kv, it, n);
}

struct gc_ack_scratch {
    struct efs_kv_item batch[EFS_META_GC_ACK_MAX];
    uint8_t keys[EFS_META_GC_ACK_MAX][EFS_KV_KEY_MAX];
    uint8_t vals[EFS_META_GC_ACK_MAX][EFS_META_GC_VAL];
    uint8_t retired[EFS_META_GC_ACK_MAX];
};

static int gc_ack_apply(struct efs_kv *kv, const struct efs_gc_ack_item *it,
                        uint32_t n, struct gc_ack_scratch *sc);

int efs_meta_apply_gc_ack(struct efs_kv *kv, const struct efs_gc_ack_item *it,
                          uint32_t n)
{
    struct gc_ack_scratch *sc;
    int rc;

    if (!kv || (!it && n))
        return EFS_ERR_INVAL;
    if (n > EFS_META_GC_ACK_MAX)
        return EFS_ERR_INVAL;
    if (n == 0)
        return EFS_OK;
    /* ~60 KB: off the apply thread's stack. */
    sc = malloc(sizeof(*sc));
    if (!sc)
        return EFS_ERR_NOMEM;
    rc = gc_ack_apply(kv, it, n, sc);
    free(sc);
    return rc;
}

static int gc_ack_apply(struct efs_kv *kv, const struct efs_gc_ack_item *it,
                        uint32_t n, struct gc_ack_scratch *sc)
{
    struct efs_kv_item *batch = sc->batch;
    uint8_t (*keys)[EFS_KV_KEY_MAX] = sc->keys;
    uint8_t (*vals)[EFS_META_GC_VAL] = sc->vals;
    uint8_t *retired = sc->retired;
    uint32_t i, j, bn = 0;
    int rc;

    memset(retired, 0, EFS_META_GC_ACK_MAX);
    for (i = 0; i < n; i++) {
        efs_node_id_t nodes[EFS_NUM_FRAGMENTS];
        uint8_t sums[EFS_NUM_FRAGMENTS][EFS_HASH_SIZE];
        uint8_t ack_bits;
        uint32_t kg = 0, vn;
        uint8_t key[EFS_KV_KEY_MAX];
        uint8_t old[EFS_META_GC_VAL];

        if (it[i].lane >= EFS_META_LANES || it[i].frag >= EFS_NUM_FRAGMENTS)
            return EFS_ERR_INVAL;
        rc = efs_kv_key_gc(efs_kv_anchor_shard(
                               efs_kv_lane_shard(it[i].ino, it[i].lane)),
                           it[i].ino, it[i].gen, it[i].lane, it[i].ci,
                           key, &kg);
        if (rc != EFS_OK)
            return rc;
        /* One batch usually carries every fragment of the same record;
         * fold into the pending entry — a fresh KV read cannot see the
         * bits the earlier items of this batch have not applied yet. */
        for (j = 0; j < bn; j++) {
            if (batch[j].klen == kg && memcmp(batch[j].key, key, kg) == 0)
                break;
        }
        if (j < bn) {
            if (retired[j])
                continue;
            rc = efs_meta_unpack_gc(vals[j], EFS_META_GC_VAL, nodes,
                                    &ack_bits, sums);
            if (rc != EFS_OK)
                return rc;
            ack_bits |= (uint8_t)(1u << it[i].frag);
            if (ack_bits == (uint8_t)((1u << EFS_NUM_FRAGMENTS) - 1u)) {
                batch[j].op = EFS_KV_DEL;
                batch[j].val = NULL;
                batch[j].vlen = 0;
                retired[j] = 1;
            } else {
                efs_meta_pack_gc(vals[j], nodes, ack_bits,
                                 (const uint8_t (*)[EFS_HASH_SIZE])sums);
            }
            continue;
        }
        vn = sizeof(old);
        rc = efs_kv_get(kv, key, kg, old, &vn);
        /* Already retired (a replay of this entry): skip. */
        if (rc == EFS_ERR_NOT_FOUND)
            continue;
        if (rc != EFS_OK)
            return rc;
        rc = efs_meta_unpack_gc(old, vn, nodes, &ack_bits, sums);
        if (rc != EFS_OK)
            return rc;
        ack_bits |= (uint8_t)(1u << it[i].frag);
        memcpy(keys[bn], key, kg);
        batch[bn].key = keys[bn];
        batch[bn].klen = kg;
        if (ack_bits == (uint8_t)((1u << EFS_NUM_FRAGMENTS) - 1u)) {
            batch[bn].op = EFS_KV_DEL;
            batch[bn].val = NULL;
            batch[bn].vlen = 0;
            retired[bn] = 1;
        } else {
            efs_meta_pack_gc(vals[bn], nodes, ack_bits,
                             (const uint8_t (*)[EFS_HASH_SIZE])sums);
            batch[bn].op = EFS_KV_PUT;
            batch[bn].val = vals[bn];
            batch[bn].vlen = EFS_META_GC_VAL;
        }
        bn++;
    }
    if (bn == 0)
        return EFS_OK;
    rc = meta_write_batch(kv, batch, bn);
    if (rc != EFS_OK)
        return rc;
    /* A retired record lowers its anchor's pending-GC watermark (D26).
     * The anchor is the key's shard field; replays skipped above never
     * reach here, so each retirement lowers exactly once. */
    for (i = 0; i < bn; i++) {
        if (retired[i])
            efs_meta_gc_pending_note(
                ((uint32_t)batch[i].key[0] << 8) | batch[i].key[1], -1);
    }
    return EFS_OK;
}
