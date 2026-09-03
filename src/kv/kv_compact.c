/* Turning the memtable into segments, and segments into fewer segments.
 *
 * Flush writes the memtable out as one L0 segment; L0 segments have
 * overlapping key ranges, so a read consults all of them newest-first.
 * Compaction merges every L0 plus the L1 segments whose ranges they touch
 * into fresh disjoint L1 segments, so a read consults at most one of those.
 * Both publish by writing the MANIFEST, which is the only authority on
 * which segment files are live; the old files are unlinked after.
 *
 * All three entry points are called with l->mu held. */

#include "kv_lsm_internal.h"

#include <stdlib.h>
#include <unistd.h>

int kv_flush_locked(struct kv_lsm *l)
{
    char path[KV_LSM_PATH_MAX + 64];
    struct kv_seg_w *w = NULL;
    struct kv_seg *s = NULL;
    uint64_t seq;
    uint32_t i;
    int rc;

    if (l->mt.n == 0)
        return EFS_OK;
    if (l->n_l0 >= KV_LSM_MAX_SEGS)
        return EFS_ERR_BUSY;
    seq = l->next_seq++;
    kv_seg_path(l, 0, seq, path, sizeof(path));
    rc = kv_seg_w_open(path, &w);
    if (rc != EFS_OK)
        return rc;
    for (i = 0; i < l->mt.n; i++) {
        struct kv_ent *e = l->mt.e[i];
        rc = kv_seg_w_add(w, e->op, e->key, e->klen, e->val, e->vlen);
        if (rc != EFS_OK) {
            kv_seg_w_abort(w);
            return rc;
        }
    }
    rc = kv_seg_w_finish(w);
    if (rc != EFS_OK)
        return rc;
    rc = kv_seg_open(path, &s);
    if (rc != EFS_OK)
        return rc;
    memmove(&l->l0[1], &l->l0[0], l->n_l0 * sizeof(l->l0[0]));
    l->l0[0].seg = s;
    l->l0[0].seq = seq;
    l->l0[0].level = 0;
    l->n_l0++;
    rc = kv_manifest_write(l);
    if (rc != EFS_OK)
        return rc;
    kv_mtab_clear(&l->mt);
    return kv_wal_reset(l->wal);
}

/* True when seg's key range intersects [lo,hi]. */
static int overlaps(struct kv_seg *seg, const uint8_t *lo, uint32_t lol,
                    const uint8_t *hi, uint32_t hil)
{
    const uint8_t *fk = NULL, *lk = NULL;
    uint32_t fl = 0, ll = 0;

    if (kv_seg_first_key(seg, &fk, &fl) != EFS_OK)
        return 0;
    if (kv_seg_last_key(seg, &lk, &ll) != EFS_OK)
        return 0;
    return kv_key_cmp(fk, fl, hi, hil) <= 0 && kv_key_cmp(lk, ll, lo, lol) >= 0;
}

struct compact_ctx {
    struct kv_lsm *l;
    struct kv_seg_w *w;
    uint64_t seq;
    struct seg_slot out[KV_LSM_MAX_SEGS];
    uint32_t n_out;
    int rc;
};

static int compact_emit(struct compact_ctx *c, uint8_t op, const uint8_t *key,
                        uint32_t klen, const uint8_t *val, uint32_t vlen)
{
    char path[KV_LSM_PATH_MAX + 64];
    int rc;

    if (c->w && kv_seg_w_bytes(c->w) >= KV_LSM_L1_TARGET) {
        rc = kv_seg_w_finish(c->w);
        c->w = NULL;
        if (rc != EFS_OK)
            return rc;
        c->n_out++;
    }
    if (!c->w) {
        if (c->n_out >= KV_LSM_MAX_SEGS)
            return EFS_ERR_BUSY;
        c->seq = c->l->next_seq++;
        c->out[c->n_out].seq = c->seq;
        c->out[c->n_out].level = 1;
        kv_seg_path(c->l, 1, c->seq, path, sizeof(path));
        rc = kv_seg_w_open(path, &c->w);
        if (rc != EFS_OK)
            return rc;
    }
    return kv_seg_w_add(c->w, op, key, klen, val, vlen);
}

/* Merges every L0 plus the L1 segments overlapping their range into fresh
 * L1 runs, so cost tracks overlapping data rather than the whole store. */
int kv_compact_locked(struct kv_lsm *l)
{
    struct msrc src[KV_LSM_MAX_SEGS * 2];
    struct seg_slot keep[KV_LSM_MAX_SEGS];
    struct seg_slot drop[KV_LSM_MAX_SEGS];
    struct compact_ctx c;
    const uint8_t *lo = NULL, *hi = NULL;
    uint32_t lol = 0, hil = 0;
    uint32_t nsrc = 0, n_keep = 0, n_drop = 0, i;
    int rc = EFS_OK;
    int have_range = 0;

    if (l->n_l0 == 0)
        return EFS_OK;
    memset(src, 0, sizeof(src));
    memset(&c, 0, sizeof(c));
    c.l = l;
    for (i = 0; i < l->n_l0; i++) {
        const uint8_t *fk = NULL, *lk = NULL;
        uint32_t fl = 0, ll = 0;

        if (kv_seg_first_key(l->l0[i].seg, &fk, &fl) != EFS_OK)
            continue;
        rc = kv_seg_last_key(l->l0[i].seg, &lk, &ll);
        if (rc != EFS_OK)
            return rc;
        if (!have_range || kv_key_cmp(fk, fl, lo, lol) < 0) {
            lo = fk;
            lol = fl;
        }
        if (!have_range || kv_key_cmp(lk, ll, hi, hil) > 0) {
            hi = lk;
            hil = ll;
        }
        have_range = 1;
    }
    if (!have_range)
        return EFS_OK;
    for (i = 0; i < l->n_l0; i++) {
        rc = kv_seg_iter_open(l->l0[i].seg, &src[nsrc].it);
        if (rc != EFS_OK)
            goto out;
        nsrc++;
        drop[n_drop++] = l->l0[i];
    }
    for (i = 0; i < l->n_l1; i++) {
        if (!overlaps(l->l1[i].seg, lo, lol, hi, hil)) {
            keep[n_keep++] = l->l1[i];
            continue;
        }
        rc = kv_seg_iter_open(l->l1[i].seg, &src[nsrc].it);
        if (rc != EFS_OK)
            goto out;
        nsrc++;
        drop[n_drop++] = l->l1[i];
    }
    for (i = 0; i < nsrc; i++) {
        rc = kv_msrc_advance(l, &src[i], NULL, 0);
        if (rc != EFS_OK)
            goto out;
    }
    for (;;) {
        uint32_t win = nsrc;

        for (i = 0; i < nsrc; i++) {
            if (src[i].done)
                continue;
            if (win == nsrc ||
                kv_key_cmp(src[i].key, src[i].klen, src[win].key,
                           src[win].klen) < 0)
                win = i;
        }
        if (win == nsrc)
            break;
        /* A tombstone is dropped only once nothing older can survive it:
         * every older run is an input here, so L1 output needs no marker. */
        if (src[win].op == KV_OP_PUT) {
            rc = compact_emit(&c, src[win].op, src[win].key, src[win].klen,
                              src[win].val, src[win].vlen);
            if (rc != EFS_OK)
                goto out;
        }
        /* The winner advances last: advancing it first would move the key
         * every other source is being compared against, and the older
         * duplicates would then survive their own tombstone. */
        for (i = 0; i < nsrc; i++) {
            if (src[i].done || i == win)
                continue;
            if (kv_key_cmp(src[i].key, src[i].klen, src[win].key,
                           src[win].klen) != 0)
                continue;
            rc = kv_msrc_advance(l, &src[i], NULL, 0);
            if (rc != EFS_OK)
                goto out;
        }
        rc = kv_msrc_advance(l, &src[win], NULL, 0);
        if (rc != EFS_OK)
            goto out;
    }
    if (c.w) {
        rc = kv_seg_w_finish(c.w);
        c.w = NULL;
        if (rc != EFS_OK)
            goto out;
        c.n_out++;
    }
    for (i = 0; i < nsrc; i++) {
        kv_seg_iter_close(src[i].it);
        src[i].it = NULL;
    }
    for (i = 0; i < c.n_out; i++) {
        char path[KV_LSM_PATH_MAX + 64];
        kv_seg_path(l, 1, c.out[i].seq, path, sizeof(path));
        rc = kv_seg_open(path, &c.out[i].seg);
        if (rc != EFS_OK)
            goto out;
    }
    for (i = 0; i < c.n_out; i++) {
        if (n_keep >= KV_LSM_MAX_SEGS) {
            rc = EFS_ERR_BUSY;
            goto out;
        }
        keep[n_keep++] = c.out[i];
    }
    l->n_l0 = 0;
    l->n_l1 = n_keep;
    for (i = 0; i < n_keep; i++)
        l->l1[i] = keep[i];
    if (l->n_l1 > 1)
        qsort(l->l1, l->n_l1, sizeof(l->l1[0]), kv_l1_cmp);
    rc = kv_manifest_write(l);
    if (rc != EFS_OK) {
        /* The outputs are already owned by l->l1, so they must not be
         * unlinked here. The old MANIFEST still lists the inputs, so a
         * restart GCs them; this engine is done accepting writes. */
        l->io_failed = 1;
        return rc;
    }
    for (i = 0; i < n_drop; i++) {
        char path[KV_LSM_PATH_MAX + 64];
        kv_seg_close(drop[i].seg);
        kv_seg_path(l, drop[i].level, drop[i].seq, path, sizeof(path));
        unlink(path);
    }
    kv_sync_dir(l->dir);
    return EFS_OK;

out:
    for (i = 0; i < nsrc; i++)
        kv_seg_iter_close(src[i].it);
    if (c.w)
        kv_seg_w_abort(c.w);
    for (i = 0; i < c.n_out; i++) {
        char path[KV_LSM_PATH_MAX + 64];
        if (c.out[i].seg)
            kv_seg_close(c.out[i].seg);
        kv_seg_path(l, 1, c.out[i].seq, path, sizeof(path));
        unlink(path);
    }
    return rc;
}

int kv_maybe_flush_locked(struct kv_lsm *l)
{
    uint32_t mmax = l->cfg.memtable_max ? l->cfg.memtable_max : KV_LSM_MEM_DEFAULT;
    uint32_t l0max = l->cfg.l0_max ? l->cfg.l0_max : KV_LSM_L0_DEFAULT;
    int rc;

    if (l->mt.bytes < mmax)
        return EFS_OK;
    /* Compact first when L0 has no room, so the cap is reached only when
     * compaction itself cannot fit the result. */
    if (l->n_l0 >= KV_LSM_MAX_SEGS) {
        rc = kv_compact_locked(l);
        if (rc != EFS_OK)
            return rc;
    }
    rc = kv_flush_locked(l);
    if (rc != EFS_OK)
        return rc;
    if (l->n_l0 >= l0max)
        return kv_compact_locked(l);
    return EFS_OK;
}

