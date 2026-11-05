#include "meta_apply/apply_internal.h"
#include "meta_apply/rows.h"
#include "meta_apply/lookup.h"
#include "meta_apply/init.h"
#include "meta_apply/xattr.h"
#include "meta_apply/namespace.h"
#include "meta_apply/gc.h"
#include "meta_apply/publish.h"
#include "meta_apply/pub_stage.h"
#include "meta_apply/lanes.h"
#include "meta_apply/sweep.h"
#include "meta_apply/attr.h"
#include "meta_apply/append.h"

/* ---- W125 background packer: admit, switch, cancel ------------------- */

static int pack_chunk_key(efs_ino_t ino, uint64_t gen, uint8_t *key, uint32_t *kl)
{
    return efs_kv_key_chunk(efs_kv_lane_shard(ino, 0), ino, gen, 0, 0, key, kl);
}

/* Newest content epoch a row carries: its base or any span. */
static uint64_t pack_row_epoch(const uint8_t *val, uint32_t vlen,
                               const struct efs_meta_chunk *base)
{
    uint64_t epoch = base->content_epoch;
    uint32_t nd = trailer_n(val, vlen);
    for (uint32_t i = 0; i < nd; i++) {
        struct efs_meta_delta d;
        if (trailer_at(val, vlen, i, &d) == EFS_OK)
            epoch = max_u64(epoch, d.content_epoch);
    }
    return epoch;
}

int efs_meta_pack_source(struct efs_kv *kv, efs_ino_t ino, uint64_t inode_gen,
                         uint8_t digest[EFS_HASH_SIZE], struct efs_meta_chunk *base,
                         uint64_t *content_epoch)
{
    uint8_t key[EFS_KV_KEY_MAX], val[CHUNK_VAL_MAX];
    uint32_t kl = 0, vl = sizeof(val);
    struct efs_meta_chunk ch;
    if (!kv || !ino || !inode_gen || !digest) return EFS_ERR_INVAL;
    int rc = pack_chunk_key(ino, inode_gen, key, &kl);
    if (rc == EFS_OK) rc = kv_get_copy(kv, key, kl, val, sizeof(val), &vl);
    if (rc == EFS_OK) rc = unpack_chunk(val, vl, &ch);
    if (rc == EFS_OK) rc = efs_pack_chunk_identity(&ch, ino, inode_gen, 0);
    if (rc != EFS_OK) return rc;
    efs_hash(val, vl, digest);
    if (base) *base = ch;
    if (content_epoch) *content_epoch = pack_row_epoch(val, vl, &ch);
    return EFS_OK;
}

int efs_meta_apply_pack_admit(struct efs_kv *kv, uint32_t group,
                              const struct efs_pack_object *o)
{
    uint8_t key[EFS_PACK_OBJECT_KEY_BYTES];
    size_t len;
    if (!kv || !o || o->ledger.publication_group != group) return EFS_ERR_INVAL;
    struct efs_pack_ledger_usage u;
    efs_pack_ledger_usage(&o->ledger, &u);
    if (o->ledger.phase != EFS_PACK_ACTIVE || u.pending != o->ledger.count ||
        o->ledger.fence_acks || o->ledger.delete_acks) return EFS_ERR_INVAL;
    int rc = efs_pack_object_key(group, o->ledger.segment_id, o->ledger.segment_generation, key);
    if (rc != EFS_OK) return rc;
    uint8_t *value = malloc(2 * EFS_PACK_OBJECT_MAX_BYTES);
    if (!value) return EFS_ERR_NOMEM;
    uint8_t *old = value + EFS_PACK_OBJECT_MAX_BYTES;
    rc = efs_pack_object_encode(o, value, EFS_PACK_OBJECT_MAX_BYTES, &len);
    if (rc == EFS_OK) rc = efs_txn_key_exclusive(kv, key, sizeof(key));
    if (rc == EFS_OK) {
        uint32_t ol = EFS_PACK_OBJECT_MAX_BYTES;
        int g = efs_kv_get(kv, key, sizeof(key), old, &ol);
        if (g == EFS_OK) {
            /* A replay of the same admission is a no-op; a different object
             * under the same identity is refused. Once switched, the stored
             * ledger differs: a late replay never resets it. */
            struct efs_pack_object *cur = malloc(sizeof(*cur));
            if (!cur) rc = EFS_ERR_NOMEM;
            else {
                rc = efs_pack_object_decode(old, ol, cur);
                if (rc == EFS_OK && (memcmp(cur->ledger.body_digest, o->ledger.body_digest, EFS_HASH_SIZE) ||
                    memcmp(cur->ledger.reference_digest, o->ledger.reference_digest,
                           sizeof(o->ledger.reference_digest[0]) * o->ledger.count) ||
                    cur->ledger.count != o->ledger.count))
                    rc = EFS_ERR_EXIST;
                free(cur);
            }
            free(value);
            return rc;
        }
        if (g != EFS_ERR_NOT_FOUND) rc = g;
    }
    /* Packed rows become visible only on a store that older builds refuse. */
    if (rc == EFS_OK) rc = efs_kv_lsm_require_reader(kv, EFS_KV_READER_PACK_V2);
    if (rc == EFS_OK) rc = meta_write_put(kv, key, sizeof(key), value, (uint32_t)len);
    free(value);
    return rc;
}

int efs_meta_apply_pack_cancel(struct efs_kv *kv, uint32_t group, uint64_t id, uint64_t gen)
{
    uint8_t key[EFS_PACK_OBJECT_KEY_BYTES];
    size_t len;
    if (!kv) return EFS_ERR_INVAL;
    int rc = efs_pack_object_key(group, id, gen, key);
    if (rc != EFS_OK) return rc;
    struct efs_pack_object *o = malloc(sizeof(*o) + EFS_PACK_OBJECT_MAX_BYTES);
    if (!o) return EFS_ERR_NOMEM;
    uint8_t *value = (uint8_t *)(o + 1);
    rc = efs_txn_key_exclusive(kv, key, sizeof(key));
    if (rc == EFS_OK) rc = efs_pack_object_get(kv, group, id, gen, o);
    int changed = 0;
    if (rc == EFS_OK && o->ledger.phase == EFS_PACK_ACTIVE) {
        for (uint32_t i = 0; i < o->ledger.count; i++)
            if (o->ledger.state[i] == EFS_PACK_PENDING) { o->ledger.state[i] = EFS_PACK_CANCELLED; changed = 1; }
        if (changed) {
            int r = efs_pack_ledger_reclaim_begin(&o->ledger);
            if (r != EFS_OK && r != EFS_ERR_BUSY) rc = r;
        }
    }
    if (rc == EFS_OK && changed) rc = efs_pack_object_encode(o, value, EFS_PACK_OBJECT_MAX_BYTES, &len);
    if (rc == EFS_OK && changed) rc = meta_write_put(kv, key, sizeof(key), value, (uint32_t)len);
    free(o);
    return rc;
}

/* Per-member staging for one switch batch. */
struct pack_switch_slot {
    uint8_t ck[EFS_KV_KEY_MAX], lk[EFS_KV_KEY_MAX];
    uint32_t ckl, lkl;
    uint8_t cv[CHUNK_VAL + EFS_PACK_REF_BYTES], lv[LANE_VAL];
    uint32_t cvl;
    struct efs_meta_chunk old;
};
#define PACK_SWITCH_ITEMS_PER (3u + EFS_CHUNK_DELTA_MAX)

/* One member's checks. EFS_OK: switch it; EFS_ERR_STALE (or any logical
 * refusal): cancel it; EFS_ERR_IO/NOMEM: abort the whole entry. */
static int pack_switch_member(struct efs_kv *kv, const struct efs_pack_object *o,
                              const struct efs_pack_switch_member *m, unsigned at,
                              struct pack_switch_slot *s, uint8_t *val, uint32_t *vl,
                              uint64_t *epoch)
{
    struct efs_meta_row row;
    uint8_t digest[EFS_HASH_SIZE];
    if (o->ledger.state[at] != EFS_PACK_PENDING) return EFS_ERR_STALE;
    int rc = efs_meta_apply_get_inode(kv, m->ino, &row);
    if (rc == EFS_ERR_NOT_FOUND) return EFS_ERR_STALE;
    if (rc != EFS_OK) return rc;
    if (row.generation != m->inode_generation || !row.nlink || !S_ISREG(row.mode))
        return EFS_ERR_STALE;
    rc = efs_lease_any(kv, m->ino, row.generation);
    if (rc == 1) return EFS_ERR_STALE;     /* open: retry in a later pass */
    if (rc != EFS_OK) return rc;
    rc = pack_chunk_key(m->ino, row.generation, s->ck, &s->ckl);
    if (rc == EFS_OK) rc = efs_kv_key_lane(efs_kv_lane_shard(m->ino, 0), m->ino,
                                           row.generation, 0, s->lk, &s->lkl);
    if (rc != EFS_OK) return rc;
    *vl = CHUNK_VAL_MAX;
    rc = kv_get_copy(kv, s->ck, s->ckl, val, CHUNK_VAL_MAX, vl);
    if (rc == EFS_ERR_NOT_FOUND) return EFS_ERR_STALE;
    if (rc != EFS_OK) return rc;
    efs_hash(val, *vl, digest);
    if (memcmp(digest, m->source_digest, EFS_HASH_SIZE)) return EFS_ERR_STALE;
    rc = unpack_chunk(val, *vl, &s->old);
    if (rc == EFS_OK) rc = efs_pack_chunk_identity(&s->old, m->ino, row.generation, 0);
    if (rc != EFS_OK) return EFS_ERR_STALE;
    *epoch = pack_row_epoch(val, *vl, &s->old);
    if (m->ref.content_epoch != *epoch || m->ref.record_generation == s->old.generation)
        return EFS_ERR_STALE;
    for (uint32_t i = 0, nd = trailer_n(val, *vl); i < nd; i++) {
        struct efs_meta_delta d;
        if (trailer_at(val, *vl, i, &d) != EFS_OK) return EFS_ERR_STALE;
        if (d.generation == m->ref.record_generation) return EFS_ERR_STALE;
    }
    if (s->old.coding_profile_id == EFS_META_PROFILE_PACK &&
        s->old.packed.segment_id == o->ledger.segment_id &&
        s->old.packed.segment_generation == o->ledger.segment_generation)
        return EFS_ERR_STALE;
    struct lane_rec lane;
    uint32_t ll = sizeof(s->lv);
    rc = efs_kv_get(kv, s->lk, s->lkl, s->lv, &ll);
    if (rc == EFS_OK) rc = unpack_lane(s->lv, ll, &lane);
    if (rc == EFS_ERR_NOT_FOUND || rc == EFS_ERR_PROTO) return EFS_ERR_STALE;
    if (rc != EFS_OK) return rc;
    lane.seq++;
    pack_lane(s->lv, &lane);
    struct efs_meta_chunk nw;
    memset(&nw, 0, sizeof(nw));
    nw.generation = m->ref.record_generation;
    nw.coding_profile_id = EFS_META_PROFILE_PACK;
    nw.content_epoch = *epoch;
    memcpy(nw.nodes, o->nodes, sizeof(nw.nodes));
    memcpy(nw.checksums, o->checksums, sizeof(nw.checksums));
    nw.packed = m->ref;
    s->cvl = pack_chunk_value(s->cv, &nw, NULL, 0);
    return EFS_OK;
}

int efs_meta_apply_pack_switch(struct efs_kv *kv, const struct efs_pack_switch *sw,
                               uint32_t *switched)
{
    uint8_t okey[EFS_PACK_OBJECT_KEY_BYTES];
    size_t olen;
    uint32_t n = 0, live = 0;
    int rc;
    if (switched) *switched = 0;
    if (!kv || !sw || !sw->count || sw->count > EFS_PACK_SWITCH_MAX) return EFS_ERR_INVAL;
    rc = efs_pack_object_key(sw->group, sw->segment_id, sw->segment_generation, okey);
    if (rc != EFS_OK) return rc;
    const uint32_t cap = sw->count * PACK_SWITCH_ITEMS_PER + 1u + EFS_PACK_MAPPING_OBJECT_MAX;
    struct efs_pack_object *o = malloc(sizeof(*o));
    struct pack_switch_slot *slots = calloc(sw->count, sizeof(*slots));
    struct efs_kv_item *it = calloc(cap, sizeof(*it));
    uint8_t (*gk)[EFS_KV_KEY_MAX] = calloc(cap, EFS_KV_KEY_MAX);
    uint8_t (*gv)[EFS_META_GC_VAL] = calloc(cap, EFS_META_GC_VAL);
    struct efs_pack_mapping_change *rel = calloc(sw->count, sizeof(*rel));
    uint8_t *val = malloc(CHUNK_VAL_MAX), *ov = malloc(EFS_PACK_OBJECT_MAX_BYTES);
    struct efs_pack_mapping_plan plan = {0};
    uint32_t nrel = 0;
    if (!o || !slots || !it || !gk || !gv || !rel || !val || !ov) { rc = EFS_ERR_NOMEM; goto out; }
    rc = efs_txn_key_exclusive(kv, okey, sizeof(okey));
    if (rc == EFS_OK) rc = efs_pack_object_get(kv, sw->group, sw->segment_id, sw->segment_generation, o);
    if (rc != EFS_OK) goto out;
    if (o->ledger.phase != EFS_PACK_ACTIVE) { rc = EFS_ERR_STALE; goto out; }
    for (uint32_t i = 0; i < sw->count; i++) {
        const struct efs_pack_switch_member *m = &sw->members[i];
        struct pack_switch_slot *s = &slots[i];
        unsigned at;
        uint32_t vl = 0;
        uint64_t epoch = 0;
        int mrc = EFS_ERR_STALE;
        if (m->ref.publication_group != sw->group || m->ref.segment_id != sw->segment_id ||
            m->ref.segment_generation != sw->segment_generation || m->ref.ino != m->ino ||
            m->ref.inode_generation != m->inode_generation ||
            m->ref.publication_group != efs_raft_shard_group(efs_kv_lane_shard(m->ino, 0)))
            continue; /* not a record of this segment: nothing to cancel */
        if (efs_pack_ledger_slot(&o->ledger, &m->ref, &at) != EFS_OK) continue;
        /* A member already in the batch must not switch twice. */
        int dup = 0;
        for (uint32_t j = 0; j < i; j++) if (sw->members[j].ino == m->ino) dup = 1;
        if (!dup) mrc = pack_switch_member(kv, o, m, at, s, val, &vl, &epoch);
        if (mrc == EFS_ERR_IO || mrc == EFS_ERR_NOMEM) { rc = mrc; goto out; }
        if (mrc != EFS_OK) {
            (void)efs_pack_ledger_cancel(&o->ledger, &m->ref);
            continue;
        }
        it[n++] = (struct efs_kv_item){EFS_KV_PUT, s->ck, s->ckl, s->cv, s->cvl};
        it[n++] = (struct efs_kv_item){EFS_KV_PUT, s->lk, s->lkl, s->lv, LANE_VAL};
        /* The superseded per-file fragments (base and spans) are dead from
         * this commit on (L7); an old packed record is released instead. */
        if (s->old.coding_profile_id == EFS_META_PROFILE_PACK)
            rel[nrel++] = (struct efs_pack_mapping_change){m->ino, m->inode_generation, 0, &s->old, NULL};
        else if (s->old.generation) {
            rc = gc_queue(it, &n, cap, gk, gv, m->ino, 0, 0, &s->old);
            if (rc != EFS_OK) goto out;
        }
        for (uint32_t d = 0, nd = trailer_n(val, vl); d < nd; d++) {
            struct efs_meta_delta dd;
            struct efs_meta_chunk span;
            rc = trailer_at(val, vl, d, &dd);
            if (rc != EFS_OK) goto out;
            if (!dd.generation) continue;
            memset(&span, 0, sizeof(span));
            span.generation = dd.generation;
            memcpy(span.nodes, dd.nodes, sizeof(span.nodes));
            memcpy(span.checksums, dd.checksums, sizeof(span.checksums));
            rc = gc_queue(it, &n, cap, gk, gv, m->ino, 0, 0, &span);
            if (rc != EFS_OK) goto out;
        }
        rc = efs_pack_ledger_publish(&o->ledger, &m->ref);
        if (rc != EFS_OK) goto out;
        live++;
    }
    if (nrel) {
        rc = efs_pack_mapping_plan(kv, rel, nrel, &plan);
        if (rc != EFS_OK) goto out;
        if (n + plan.count + 1 > cap) { rc = EFS_ERR_NOMEM; goto out; }
        memcpy(it + n, plan.items, plan.count * sizeof(it[0]));
        n += plan.count;
    }
    rc = efs_pack_ledger_reclaim_begin(&o->ledger);
    if (rc != EFS_OK && rc != EFS_ERR_BUSY) goto out;
    rc = efs_pack_object_encode(o, ov, EFS_PACK_OBJECT_MAX_BYTES, &olen);
    if (rc != EFS_OK) goto out;
    it[n++] = (struct efs_kv_item){EFS_KV_PUT, okey, sizeof(okey), ov, (uint32_t)olen};
    rc = meta_write_batch(kv, it, n);
    if (rc == EFS_OK && switched) *switched = live;
out:
    efs_pack_mapping_plan_free(&plan);
    free(o); free(slots); free(it); free(gk); free(gv); free(rel); free(val); free(ov);
    return rc;
}

/* Every member durably accepted these reclaiming segments into its reclaim
 * journal (fenced, physical deletion owned by the node): forget them. A
 * segment still ACTIVE is refused; an absent one is already retired. */
int efs_meta_apply_pack_retire(struct efs_kv *kv, uint32_t group, const uint64_t *ids,
                               const uint64_t *gens, uint32_t n, uint32_t *retired)
{
    uint8_t key[EFS_PACK_OBJECT_KEY_BYTES];
    uint32_t done = 0;
    int rc = EFS_OK;
    if (retired) *retired = 0;
    if (!kv || (n && (!ids || !gens)) || n > EFS_PACK_RETIRE_MAX) return EFS_ERR_INVAL;
    struct efs_pack_object *o = malloc(sizeof(*o));
    if (!o) return EFS_ERR_NOMEM;
    for (uint32_t i = 0; i < n && rc == EFS_OK; i++) {
        rc = efs_pack_object_key(group, ids[i], gens[i], key);
        if (rc == EFS_OK) rc = efs_txn_key_exclusive(kv, key, sizeof(key));
        if (rc == EFS_OK) rc = efs_pack_object_get(kv, group, ids[i], gens[i], o);
        if (rc == EFS_ERR_NOT_FOUND) { rc = EFS_OK; continue; }
        if (rc != EFS_OK) break;
        if (o->ledger.phase == EFS_PACK_ACTIVE) continue;
        /* K37: the segment's degraded mark goes first, so a replay after a
         * crash between the two deletes still finds the object and retires
         * both; a mark never outlives its segment. */
        uint8_t mk[EFS_PACK_OBJECT_KEY_BYTES];
        rc = efs_pack_degraded_key(group, ids[i], gens[i], mk);
        if (rc == EFS_OK) rc = meta_write_del(kv, mk, sizeof(mk));
        if (rc == EFS_ERR_NOT_FOUND) rc = EFS_OK;
        if (rc == EFS_OK) rc = meta_write_del(kv, key, sizeof(key));
        if (rc == EFS_ERR_NOT_FOUND) rc = EFS_OK;
        if (rc == EFS_OK) done++;
    }
    free(o);
    if (retired) *retired = done;
    return rc;
}

/* K37 segment degraded marks (pack_mapping.h). Serialized apply on the
 * segment's group; the object row is read under the same exclusion as the
 * packer's own commands. */
int efs_meta_apply_pack_degraded(struct efs_kv *kv, uint32_t group, uint32_t op, uint64_t id,
                                 uint64_t gen, uint32_t mask, uint32_t reason, int *outcome)
{
    uint8_t okey[EFS_PACK_OBJECT_KEY_BYTES], mkey[EFS_PACK_OBJECT_KEY_BYTES];
    uint8_t have = 0, why = 0, v[EFS_PACK_DEGRADED_VAL] = {0};
    if (outcome) *outcome = 0;
    if (!kv || !mask || mask > 7u || reason > 0xffu ||
        (op != EFS_MD_PACK_DEGRADED_MARK && op != EFS_MD_PACK_DEGRADED_CLEAR &&
         op != EFS_MD_PACK_DEGRADED_RETIRE) ||
        (op == EFS_MD_PACK_DEGRADED_MARK && !reason) ||
        (op == EFS_MD_PACK_DEGRADED_CLEAR && (mask & (mask - 1u))))
        return EFS_ERR_INVAL;
    int rc = efs_pack_object_key(group, id, gen, okey);
    if (rc == EFS_OK) rc = efs_pack_degraded_key(group, id, gen, mkey);
    if (rc != EFS_OK) return rc;
    rc = efs_txn_key_exclusive(kv, okey, sizeof(okey));
    if (rc != EFS_OK) return rc;
    rc = efs_pack_degraded_get(kv, group, id, gen, &have, &why);
    if (rc == EFS_ERR_NOT_FOUND) { have = 0; rc = EFS_OK; }
    if (rc != EFS_OK) return rc; /* a malformed mark is never overwritten */
    struct efs_pack_object *o = malloc(sizeof(*o));
    if (!o) return EFS_ERR_NOMEM;
    int orc = efs_pack_object_get(kv, group, id, gen, o);
    if (orc != EFS_OK && orc != EFS_ERR_NOT_FOUND) { free(o); return orc; }
    struct efs_pack_ledger_usage u = {0};
    if (orc == EFS_OK) efs_pack_ledger_usage(&o->ledger, &u);
    int active = orc == EFS_OK && o->ledger.phase == EFS_PACK_ACTIVE;
    free(o);
    if (op == EFS_MD_PACK_DEGRADED_MARK) {
        /* Only an ACTIVE segment holds data that a lost fragment endangers. */
        if (!active) return EFS_ERR_STALE;
        if ((have | mask) == have && (why | reason) == why) return EFS_OK;
        v[0] = (uint8_t)(have | mask);
        v[1] = (uint8_t)(why | reason);
        rc = meta_write_put(kv, mkey, sizeof(mkey), v, sizeof(v));
    } else {
        if (!have) return EFS_OK; /* already cleared: idempotent */
        if (have != mask) return EFS_ERR_STALE; /* decided for another mark */
        if (op == EFS_MD_PACK_DEGRADED_RETIRE && active && (u.live || u.pending))
            return EFS_ERR_STALE;
        rc = meta_write_del(kv, mkey, sizeof(mkey));
    }
    if (rc == EFS_OK && outcome) *outcome = 1;
    return rc;
}

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
static uint32_t sweep_visible(const struct efs_fence_history *s, uint64_t epoch,
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

int efs_meta_pack_candidate(struct efs_kv *kv, efs_ino_t ino, uint64_t inode_gen,
                            struct efs_pack_candidate *out)
{
    struct efs_meta_row row;
    struct lane_rec lane;
    uint8_t lk[EFS_KV_KEY_MAX], lv[LANE_VAL];
    uint32_t lkl = 0, lvl = sizeof(lv);
    if (!kv || !ino || !out) return EFS_ERR_INVAL;
    memset(out, 0, sizeof(*out));
    int rc = efs_meta_apply_get_inode(kv, ino, &row);
    if (rc == EFS_ERR_NOT_FOUND) return EFS_ERR_STALE;
    if (rc != EFS_OK) return rc;
    if (row.generation != inode_gen) return EFS_ERR_STALE;
    out->ino = ino; out->parent = row.parent; out->inode_generation = row.generation;
    out->mode = row.mode; out->nlink = row.nlink; out->active_lanes = row.active_lanes;
    memset(&lane, 0, sizeof(lane));
    rc = efs_kv_key_lane(efs_kv_lane_shard(ino, 0), ino, row.generation, 0, lk, &lkl);
    if (rc == EFS_OK) rc = efs_kv_get(kv, lk, lkl, lv, &lvl);
    if (rc == EFS_OK) rc = efs_kv_decode_result(kv, lk, lkl, unpack_lane(lv, lvl, &lane));
    if (rc != EFS_OK && rc != EFS_ERR_NOT_FOUND) return rc;
    /* D25: the packed image is masked by the inode history (and the base
     * clip); a lane EOF counts only at the current epoch. */
    struct efs_fence_history hist;
    rc = fence_history_read(kv, ino, row.generation, EFS_META_FENCE_INODE,
                            NULL, NULL, NULL, &hist);
    if (rc != EFS_OK) return rc;
    out->size = lane.fenced_epoch == row.content_epoch ?
                max_u64(row.base_size, lane.max_end) : row.base_size;
    out->mtime = max_u64(row.base_mtime, lane.max_mtime);
    uint8_t key[EFS_KV_KEY_MAX], val[CHUNK_VAL_MAX];
    uint32_t kl = 0, vl = sizeof(val);
    rc = pack_chunk_key(ino, row.generation, key, &kl);
    if (rc == EFS_OK) rc = kv_get_copy(kv, key, kl, val, sizeof(val), &vl);
    if (rc == EFS_ERR_NOT_FOUND) return EFS_ERR_STALE;
    if (rc == EFS_OK) rc = efs_kv_decode_result(kv, key, kl, unpack_chunk(val, vl, &out->base));
    if (rc == EFS_OK) rc = efs_kv_decode_result(kv, key, kl, efs_pack_chunk_identity(&out->base, ino, row.generation, 0));
    if (rc != EFS_OK) return rc;
    efs_hash(val, vl, out->source_digest);
    out->content_epoch = pack_row_epoch(val, vl, &out->base);
    out->ndelta = trailer_n(val, vl);
    if (out->ndelta > EFS_CHUNK_DELTA_MAX) return EFS_ERR_PROTO;
    for (uint32_t i = 0; i < out->ndelta; i++) {
        rc = efs_kv_decode_result(kv, key, kl, trailer_at(val, vl, i, &out->deltas[i]));
        if (rc != EFS_OK) return rc;
        out->deltas[i].len = sweep_visible(&hist, out->deltas[i].content_epoch, 0,
                                           out->deltas[i].off, out->deltas[i].len);
    }
    out->base_visible = out->base.generation ?
        sweep_visible(&hist, out->base.content_epoch, 0, 0,
                      out->base.base_clip ? out->base.base_clip : UINT32_MAX) : 0;
    return EFS_OK;
}

int efs_meta_apply_pack_unindex(struct efs_kv *kv, uint32_t group,
                                const struct efs_pack_unindex_item *items, uint32_t n)
{
    uint32_t anchor = efs_pack_group_anchor(group);
    if (!kv || !anchor || (n && !items) || n > EFS_PACK_UNINDEX_MAX) return EFS_ERR_INVAL;
    for (uint32_t i = 0; i < n; i++)
        if (items[i].epoch == EFS_PACK_CURSOR_EPOCH) return EFS_ERR_INVAL;
    for (uint32_t i = 0; i < n; i++) {
        uint8_t key[EFS_PACK_CANDIDATE_KEY_BYTES], v[EFS_PACK_CANDIDATE_VAL_BYTES];
        uint32_t vl = sizeof(v);
        int rc = efs_pack_candidate_key(anchor, items[i].epoch, items[i].ino, key);
        if (rc == EFS_OK) rc = efs_kv_get(kv, key, sizeof(key), v, &vl);
        if (rc == EFS_ERR_NOT_FOUND) continue;
        /* A longer value is INVAL with its size in vl (kv.h). */
        if ((rc == EFS_OK || rc == EFS_ERR_INVAL) && vl != EFS_PACK_CANDIDATE_VAL_BYTES) rc = EFS_ERR_PROTO;
        if (rc != EFS_OK) return rc;
        /* A publication re-added the key after the packer read it. */
        if (rd64(v) != items[i].inode_generation || rd64(v + 8) != items[i].stamp) continue;
        rc = meta_write_del(kv, key, sizeof(key));
        if (rc != EFS_OK && rc != EFS_ERR_NOT_FOUND) return rc;
    }
    return EFS_OK;
}

int efs_meta_apply_pack_index(struct efs_kv *kv, uint32_t group, uint32_t next_shard, int done,
                              const struct efs_pack_index_item *items, uint32_t n)
{
    uint32_t anchor = efs_pack_group_anchor(group);
    if (!kv || !anchor || (n && !items) || n > EFS_PACK_INDEX_MAX ||
        next_shard > EFS_KV_SHARD_MASK + 1u) return EFS_ERR_INVAL;
    struct efs_kv_item *it = calloc(n + 1, sizeof(*it));
    uint8_t (*keys)[EFS_PACK_CANDIDATE_KEY_BYTES] = calloc(n + 1, EFS_PACK_CANDIDATE_KEY_BYTES);
    uint8_t (*vals)[EFS_PACK_CANDIDATE_VAL_BYTES] = calloc(n + 1, EFS_PACK_CANDIDATE_VAL_BYTES);
    int rc = it && keys && vals ? EFS_OK : EFS_ERR_NOMEM;
    for (uint32_t i = 0; rc == EFS_OK && i < n; i++) {
        if (items[i].epoch == EFS_PACK_CURSOR_EPOCH || items[i].ino <= EFS_ROOT_INO) { rc = EFS_ERR_INVAL; break; }
        rc = efs_pack_candidate_key(anchor, items[i].epoch, items[i].ino, keys[i]);
        be64(vals[i], items[i].inode_generation); /* stamp 0: calloc */
        it[i] = (struct efs_kv_item){EFS_KV_PUT, keys[i], EFS_PACK_CANDIDATE_KEY_BYTES, vals[i],
                                     EFS_PACK_CANDIDATE_VAL_BYTES};
    }
    if (rc == EFS_OK) rc = efs_pack_candidate_key(anchor, EFS_PACK_CURSOR_EPOCH, 1, keys[n]);
    if (rc == EFS_OK) {
        efs_store_be32(vals[n], next_shard);
        vals[n][4] = done ? 1 : 0;
        it[n] = (struct efs_kv_item){EFS_KV_PUT, keys[n], EFS_PACK_CANDIDATE_KEY_BYTES, vals[n], 5};
        rc = meta_write_batch(kv, it, n + 1);
    }
    free(it); free(keys); free(vals);
    return rc;
}

int efs_meta_pack_backfill_cursor(struct efs_kv *kv, uint32_t group, uint32_t *next_shard, int *done)
{
    uint8_t key[EFS_PACK_CANDIDATE_KEY_BYTES], v[8];
    uint32_t vl = sizeof(v);
    uint32_t anchor = efs_pack_group_anchor(group);
    if (!kv || !anchor || !next_shard || !done) return EFS_ERR_INVAL;
    *next_shard = 0; *done = 0;
    int rc = efs_pack_candidate_key(anchor, EFS_PACK_CURSOR_EPOCH, 1, key);
    if (rc == EFS_OK) rc = efs_kv_get(kv, key, sizeof(key), v, &vl);
    if (rc == EFS_ERR_NOT_FOUND) return EFS_OK;
    if (rc != EFS_OK) return rc;
    if (vl != 5) return EFS_ERR_PROTO;
    *next_shard = efs_load_be32(v);
    *done = v[4] != 0;
    return EFS_OK;
}
