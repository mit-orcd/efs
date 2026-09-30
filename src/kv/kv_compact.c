/* Turning the memtable into segments, and segments into fewer segments.
 *
 * Flush writes the memtable as one L0 segment per key[0] range. Those
 * files do not overlap, so a read skips every L0 whose range misses the
 * key, and compaction merges one range plus the L1 segments that overlap
 * it. A segment left by an older flush that itself spans several ranges
 * is still compacted whole, and the output is split on the range boundary
 * so the next cycle is narrow. L1 segments stay disjoint. Both publish
 * by writing the MANIFEST, which is the only authority on which segment
 * files are live; the old files are unlinked once nothing still pins them.
 *
 * kv_flush_locked and kv_maybe_flush_locked are called with l->mu held.
 * kv_compact_locked is too, but async=1 drops the lock for the merge:
 * segment files are immutable, so the compactor reads private opens of
 * them and only the manifest swap needs the lock. The background thread
 * is what production uses; the synchronous call remains for tests and
 * for the case where the thread failed to start. */

#include "kv_lsm_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

/* Range ids: kv_key_range / KV_RANGE_N in kv_lsm_internal.h (the point
 * get filters segments by the same ids). */
#define key_range kv_key_range

static void flush_drop_new(struct kv_lsm *l, struct seg_slot *neu, uint32_t n)
{
    uint32_t i;

    for (i = 0; i < n; i++) {
        char path[KV_LSM_PATH_MAX + 64];

        kv_seg_path(l, 0, neu[i].seq, path, sizeof(path));
        if (neu[i].seg)
            kv_seg_close(neu[i].seg);
        unlink(path);
    }
}

int kv_flush_locked(struct kv_lsm *l)
{
    char path[KV_LSM_PATH_MAX + 64];
    struct seg_slot neu[KV_RANGE_N];
    struct kv_seg_w *w = NULL;
    uint32_t nneu = 0, i, runs = 0;
    int cur = -1, prev = -1;
    int rc = EFS_OK;

    if (l->mt.n == 0)
        return EFS_OK;
    if (kv_l0_bytes(l) >= KV_LSM_L0_BYTES)
        return EFS_ERR_BUSY;
    /* Stop once another run cannot fit in the output array. */
    for (i = 0; i < l->mt.n; i++) {
        int r = key_range(l->mt.e[i]->key, l->mt.e[i]->klen);

        if (r != prev) {
            runs++;
            prev = r;
            if (runs > KV_RANGE_N)
                return EFS_ERR_BUSY;
        }
    }
    memset(neu, 0, sizeof(neu));
    for (i = 0; i < l->mt.n; i++) {
        struct kv_ent *e = l->mt.e[i];
        int r = key_range(e->key, e->klen);

        if (r != cur) {
            if (w) {
                struct kv_seg *s = NULL;
                uint64_t seq = neu[nneu].seq;

                rc = kv_seg_w_finish(w);
                w = NULL;
                if (rc != EFS_OK)
                    goto fail;
                kv_seg_path(l, 0, seq, path, sizeof(path));
                rc = kv_seg_open(path, &s);
                if (rc != EFS_OK) {
                    unlink(path);
                    goto fail;
                }
                seg_slot_set(&neu[nneu], s);
                neu[nneu].level = 0;
                nneu++;
            }
            /* One file per range; range 0 splits into 256, so a
             * memtable can hold up to KV_RANGE_N runs. A cap of 256
             * here made a wide memtable BUSY on every flush, and the
             * apply kept it in RAM forever. */
            if (nneu >= KV_RANGE_N) {
                rc = EFS_ERR_BUSY;
                goto fail;
            }
            neu[nneu].seq = l->next_seq++;
            kv_seg_path(l, 0, neu[nneu].seq, path, sizeof(path));
            rc = kv_seg_w_open(path, &w);
            if (rc != EFS_OK)
                goto fail;
            cur = r;
        }
        rc = kv_seg_w_add(w, e->op, e->key, e->klen, e->val, e->vlen);
        if (rc != EFS_OK) {
            kv_seg_w_abort(w);
            w = NULL;
            goto fail;
        }
    }
    if (w) {
        struct kv_seg *s = NULL;
        uint64_t seq = neu[nneu].seq;

        rc = kv_seg_w_finish(w);
        w = NULL;
        if (rc != EFS_OK)
            goto fail;
        kv_seg_path(l, 0, seq, path, sizeof(path));
        rc = kv_seg_open(path, &s);
        if (rc != EFS_OK) {
            unlink(path);
            goto fail;
        }
        seg_slot_set(&neu[nneu], s);
        neu[nneu].level = 0;
        nneu++;
    }
    rc = kv_l0_reserve(l, l->n_l0 + nneu);
    if (rc != EFS_OK)
        goto fail;
    if (nneu && l->n_l0)
        memmove(&l->l0[nneu], &l->l0[0], l->n_l0 * sizeof(l->l0[0]));
    for (i = 0; i < nneu; i++)
        l->l0[i] = neu[nneu - 1 - i];
    l->n_l0 += nneu;
    rc = kv_manifest_write(l);
    if (rc != EFS_OK)
        return rc;
    kv_mtab_clear(&l->mt);
    return kv_wal_reset(l->wal);

fail:
    flush_drop_new(l, neu, nneu);
    return rc;
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
    uint64_t bytes;
    struct seg_slot *out;
    uint32_t n_out;
    uint32_t out_cap;
    int range;
    int out_level; /* 1 = L1 rewrite; 0 = D13 L0 fold */
    int rc;
};

static int compact_out_slot(struct compact_ctx *c)
{
    struct seg_slot *p;
    uint32_t ncap;

    if (c->n_out < c->out_cap)
        return EFS_OK;
    ncap = c->out_cap ? c->out_cap * 2u : 8u;
    if (ncap < c->n_out + 1)
        ncap = c->n_out + 1;
    p = realloc(c->out, (size_t)ncap * sizeof(*p));
    if (!p)
        return EFS_ERR_NOMEM;
    memset(p + c->out_cap, 0, (size_t)(ncap - c->out_cap) * sizeof(*p));
    c->out = p;
    c->out_cap = ncap;
    return EFS_OK;
}

struct seg_id {
    int level;
    uint64_t seq;
};

static uint64_t take_seq(struct kv_lsm *l)
{
    uint64_t s;

    /* Recursive mutex: a synchronous compact already holds l->mu. */
    pthread_mutex_lock(&l->mu);
    s = l->next_seq++;
    pthread_mutex_unlock(&l->mu);
    return s;
}

/* EFS_KV_COMPACT_DIE=N exits after the Nth finished output segment, before
 * the manifest rename. Recovery must still serve every key. */
static void compact_maybe_die(void)
{
    const char *e = getenv("EFS_KV_COMPACT_DIE");
    static uint32_t n;

    if (!e || !e[0])
        return;
    n++;
    if ((uint32_t)atoi(e) == n)
        _exit(99);
}

static uint64_t mono_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)ts.tv_nsec / 1000000ull;
}

static int id_has(const struct seg_id *d, uint32_t n, int level, uint64_t seq)
{
    uint32_t i;

    for (i = 0; i < n; i++)
        if (d[i].level == level && d[i].seq == seq)
            return 1;
    return 0;
}

static int compact_emit(struct compact_ctx *c, uint8_t op, const uint8_t *key,
                        uint32_t klen, const uint8_t *val, uint32_t vlen)
{
    char path[KV_LSM_PATH_MAX + 64];
    int rc;

    /* An L0 fold is one file. Splitting it could leave the file count
     * unchanged and the compactor would spin. */
    if (c->w && ((c->out_level != 0 &&
                  kv_seg_w_bytes(c->w) >= KV_LSM_L1_TARGET) ||
                 c->range != key_range(key, klen))) {
        c->bytes += kv_seg_w_bytes(c->w);
        rc = kv_seg_w_finish(c->w);
        c->w = NULL;
        if (rc != EFS_OK)
            return rc;
        compact_maybe_die();
        c->n_out++;
    }
    if (!c->w) {
        rc = compact_out_slot(c);
        if (rc != EFS_OK)
            return rc;
        c->range = key_range(key, klen);
        c->seq = take_seq(c->l);
        c->out[c->n_out].seq = c->seq;
        c->out[c->n_out].level = c->out_level;
        kv_seg_path(c->l, c->out_level, c->seq, path, sizeof(path));
        rc = kv_seg_w_open(path, &c->w);
        if (rc != EFS_OK)
            return rc;
    }
    return kv_seg_w_add(c->w, op, key, klen, val, vlen);
}

/* Lowest index wins a tie: L0 is appended newest-first, then L1.
 * The linear scan used strict <, so that source supplies the value. */
static int cm_before(const struct msrc *src, uint32_t a, uint32_t b)
{
    int c = kv_key_cmp(src[a].key, src[a].klen, src[b].key, src[b].klen);

    if (c != 0)
        return c < 0;
    return a < b;
}

static void cm_swap(uint32_t *h, uint32_t i, uint32_t j)
{
    uint32_t t = h[i];

    h[i] = h[j];
    h[j] = t;
}

static void cm_sift_up(uint32_t *h, const struct msrc *src, uint32_t i)
{
    while (i > 0) {
        uint32_t p = (i - 1) / 2;

        if (!cm_before(src, h[i], h[p]))
            break;
        cm_swap(h, i, p);
        i = p;
    }
}

static void cm_sift_down(uint32_t *h, uint32_t n, const struct msrc *src,
                         uint32_t i)
{
    for (;;) {
        uint32_t l = i * 2 + 1;
        uint32_t r = l + 1;
        uint32_t best = i;

        if (l < n && cm_before(src, h[l], h[best]))
            best = l;
        if (r < n && cm_before(src, h[r], h[best]))
            best = r;
        if (best == i)
            break;
        cm_swap(h, i, best);
        i = best;
    }
}

static void cm_push(uint32_t *h, uint32_t *n, const struct msrc *src,
                    uint32_t idx)
{
    h[*n] = idx;
    cm_sift_up(h, src, *n);
    (*n)++;
}

static uint32_t cm_pop(uint32_t *h, uint32_t *n, const struct msrc *src)
{
    uint32_t top = h[0];

    (*n)--;
    if (*n > 0) {
        h[0] = h[*n];
        cm_sift_down(h, *n, src, 0);
    }
    return top;
}

/* Merges one key[0] range of L0, plus the L1 segments overlapping it,
 * into fresh L1 runs. A wide L0 (several ranges in one file) still pulls
 * every L0 so the old shape drains in one pass. Caller holds l->mu.
 * async=1 releases it across the file merge. */
int kv_compact_locked(struct kv_lsm *l, int async)
{
    struct msrc *src = NULL;
    struct kv_seg **priv = NULL;
    struct seg_id *drop = NULL;
    struct seg_slot *keep = NULL;
    struct seg_slot *nl0 = NULL;
    struct seg_slot *doomed = NULL;
    struct seg_slot *nl1 = NULL;
    struct compact_ctx c;
    uint32_t *heap = NULL;
    const uint8_t *lo = NULL, *hi = NULL;
    uint32_t lol = 0, hil = 0;
    uint32_t nsrc = 0, n_drop = 0, n_keep = 0, n_doomed = 0, hn = 0, i, j;
    uint32_t n_l0_in = 0, cap = 0;
    int rc = EFS_OK;
    int have_range = 0;
    int held = 1;
    int wide = 0;
    int chosen = -1;
    uint64_t l0b[KV_RANGE_N], l1b[KV_RANGE_N];
    uint32_t l0n[KV_RANGE_N];
    uint32_t l0max;
    int over_files;
    int l0_only;
    uint64_t t0;

    if (l->n_l0 == 0)
        return EFS_OK;
    memset(&c, 0, sizeof(c));
    memset(l0b, 0, sizeof(l0b));
    memset(l1b, 0, sizeof(l1b));
    memset(l0n, 0, sizeof(l0n));
    l0max = l->cfg.l0_max ? l->cfg.l0_max : KV_LSM_L0_DEFAULT;
    /* D13: past the file cap, fold one range's L0 files into a single
     * L0 file and do not read its L1. The 1/8 rule (and the 1 GiB byte
     * cap) stay the only merges that rewrite L1. */
    over_files = l->n_l0 >= l0max;
    l0_only = 0;
    c.l = l;
    c.range = -1;
    n_l0_in = l->n_l0;
    for (i = 0; i < l->n_l0; i++) {
        const uint8_t *fk = NULL, *lk = NULL;
        uint32_t fl = 0, ll = 0;
        int rlo, rhi;

        if (kv_seg_first_key(l->l0[i].seg, &fk, &fl) != EFS_OK)
            continue;
        rc = kv_seg_last_key(l->l0[i].seg, &lk, &ll);
        if (rc != EFS_OK)
            return rc;
        rlo = key_range(fk, fl);
        rhi = key_range(lk, ll);
        if (rlo != rhi)
            wide = 1;
        else if (rlo >= 0 && rlo < KV_RANGE_N) {
            l0b[rlo] += kv_seg_data_bytes(l->l0[i].seg);
            l0n[rlo]++;
        }
    }
    if (!wide) {
        uint64_t best = 0;
        int over_bytes = kv_l0_bytes(l) >= KV_LSM_L0_BYTES;
        int r;

        for (i = 0; i < l->n_l1; i++) {
            const uint8_t *fk = NULL, *lk = NULL;
            uint32_t fl = 0, ll = 0;
            int rlo, rhi;

            if (kv_seg_first_key(l->l1[i].seg, &fk, &fl) != EFS_OK)
                continue;
            if (kv_seg_last_key(l->l1[i].seg, &lk, &ll) != EFS_OK)
                continue;
            rlo = key_range(fk, fl);
            rhi = key_range(lk, ll);
            if (rlo == rhi && rlo >= 0 && rlo < KV_RANGE_N)
                l1b[rlo] += kv_seg_data_bytes(l->l1[i].seg);
        }
        for (r = 0; r < KV_RANGE_N; r++) {
            int eighth = l0b[r] > 0 &&
                         (l1b[r] == 0 || l0b[r] * 8 >= l1b[r]);
            int bytes = over_bytes && l0b[r] > 0;

            if ((eighth || bytes) && l0b[r] > best) {
                best = l0b[r];
                chosen = r;
            }
        }
        /* The file-cap fold is only used when no range already qualifies
         * for an L1 rewrite. Doing the L1 rewrite first also drops files. */
        if (chosen < 0) {
            uint32_t bn = 0;
            int pick = -1;

            for (r = 0; r < KV_RANGE_N; r++) {
                if (!over_files || l0n[r] < 2)
                    continue;
                if (pick < 0 || l0n[r] > bn ||
                    (l0n[r] == bn && l0b[r] > l0b[pick])) {
                    bn = l0n[r];
                    pick = r;
                }
            }
            if (pick >= 0) {
                chosen = pick;
                l0_only = 1;
            }
        }
        if (chosen < 0)
            return EFS_ERR_BUSY;
    }
    cap = l->n_l0 + l->n_l1;
    src = calloc(cap, sizeof(*src));
    priv = calloc(cap, sizeof(*priv));
    drop = calloc(cap, sizeof(*drop));
    doomed = calloc(cap, sizeof(*doomed));
    heap = calloc(cap, sizeof(*heap));
    if (!src || !priv || !drop || !doomed || !heap) {
        rc = EFS_ERR_NOMEM;
        goto cleanup;
    }
    for (i = 0; i < l->n_l0; i++) {
        const uint8_t *fk = NULL, *lk = NULL;
        uint32_t fl = 0, ll = 0;
        int rlo, rhi;

        if (kv_seg_first_key(l->l0[i].seg, &fk, &fl) != EFS_OK)
            continue;
        rc = kv_seg_last_key(l->l0[i].seg, &lk, &ll);
        if (rc != EFS_OK)
            goto cleanup;
        rlo = key_range(fk, fl);
        rhi = key_range(lk, ll);
        if (!wide && (rlo != chosen || rhi != chosen))
            continue;
        if (!have_range || kv_key_cmp(fk, fl, lo, lol) < 0) {
            lo = fk;
            lol = fl;
        }
        if (!have_range || kv_key_cmp(lk, ll, hi, hil) > 0) {
            hi = lk;
            hil = ll;
        }
        have_range = 1;
        if (n_drop >= cap) {
            rc = EFS_ERR_NOMEM;
            goto cleanup;
        }
        drop[n_drop].level = 0;
        drop[n_drop].seq = l->l0[i].seq;
        n_drop++;
    }
    if (!have_range) {
        rc = EFS_OK;
        goto cleanup;
    }
    /* D13: a file-cap fold never opens L1. A wide L0 still takes the
     * full merge; new flushes are one range per file. */
    if (!l0_only) {
        for (i = 0; i < l->n_l1; i++) {
            if (!overlaps(l->l1[i].seg, lo, lol, hi, hil))
                continue;
            if (n_drop >= cap) {
                rc = EFS_ERR_NOMEM;
                goto cleanup;
            }
            drop[n_drop].level = 1;
            drop[n_drop].seq = l->l1[i].seq;
            n_drop++;
        }
    }
    c.out_level = l0_only ? 0 : 1;
    t0 = mono_ms();
    fprintf(stderr, "kv-compact: start l0=%u inputs=%u range=%d l0only=%d\n",
            n_l0_in, n_drop, wide ? -1 : chosen, l0_only);
    if (async) {
        pthread_mutex_unlock(&l->mu);
        held = 0;
    }
    for (i = 0; i < n_drop; i++) {
        char path[KV_LSM_PATH_MAX + 64];

        kv_seg_path(l, drop[i].level, drop[i].seq, path, sizeof(path));
        rc = kv_seg_open(path, &priv[i]);
        if (rc != EFS_OK)
            goto out;
        rc = kv_seg_iter_open(priv[i], &src[nsrc].it);
        if (rc != EFS_OK)
            goto out;
        kv_seg_iter_set_seq(src[nsrc].it, 1);
        nsrc++;
    }
    for (i = 0; i < nsrc; i++) {
        rc = kv_msrc_advance(l, &src[i], NULL, 0);
        if (rc != EFS_OK)
            goto out;
    }
    for (i = 0; i < nsrc; i++) {
        if (!src[i].done)
            cm_push(heap, &hn, src, i);
    }
    while (hn > 0) {
        uint32_t win = cm_pop(heap, &hn, src);

        /* A tombstone is dropped only once nothing older can survive it:
         * every older run is an input here, so L1 output needs no marker.
         * An L0 fold leaves L1 in place, so its tombstones stay. */
        if (src[win].op == KV_OP_PUT ||
            (l0_only && src[win].op == KV_OP_DEL)) {
            rc = compact_emit(&c, src[win].op, src[win].key, src[win].klen,
                              src[win].val, src[win].vlen);
            if (rc != EFS_OK)
                goto out;
        }
        /* The winner advances last. Advancing it first would change the
         * key the other sources are matched against, and an older copy
         * would survive its tombstone. A scan of every input per key was
         * the compactor's share of efsd cpu-clock during posix. */
        while (hn > 0 &&
               kv_key_cmp(src[heap[0]].key, src[heap[0]].klen,
                          src[win].key, src[win].klen) == 0) {
            uint32_t o = cm_pop(heap, &hn, src);

            rc = kv_msrc_advance(l, &src[o], NULL, 0);
            if (rc != EFS_OK)
                goto out;
            if (!src[o].done)
                cm_push(heap, &hn, src, o);
        }
        rc = kv_msrc_advance(l, &src[win], NULL, 0);
        if (rc != EFS_OK)
            goto out;
        if (!src[win].done)
            cm_push(heap, &hn, src, win);
    }
    if (c.w) {
        c.bytes += kv_seg_w_bytes(c.w);
        rc = kv_seg_w_finish(c.w);
        c.w = NULL;
        if (rc != EFS_OK)
            goto out;
        compact_maybe_die();
        c.n_out++;
    }
    for (i = 0; i < nsrc; i++) {
        kv_seg_iter_close(src[i].it);
        src[i].it = NULL;
    }
    for (i = 0; i < n_drop; i++) {
        kv_seg_close(priv[i]);
        priv[i] = NULL;
    }
    for (i = 0; i < c.n_out; i++) {
        char path[KV_LSM_PATH_MAX + 64];

        kv_seg_path(l, c.out[i].level, c.out[i].seq, path, sizeof(path));
        rc = kv_seg_open(path, &c.out[i].seg);
        if (rc != EFS_OK)
            goto out;
        seg_slot_set(&c.out[i], c.out[i].seg);
    }
    if (!held) {
        pthread_mutex_lock(&l->mu);
        held = 1;
    }
    for (i = 0; i < n_drop; i++) {
        int found = 0;

        if (drop[i].level == 0) {
            for (j = 0; j < l->n_l0; j++)
                if (l->l0[j].seq == drop[i].seq)
                    found = 1;
        } else {
            for (j = 0; j < l->n_l1; j++)
                if (l->l1[j].seq == drop[i].seq)
                    found = 1;
        }
        if (!found) {
            /* A synchronous compact published these inputs already.
             * Discard our outputs; the manifest is the live one. */
            rc = EFS_OK;
            goto discard;
        }
    }
    n_keep = 0;
    keep = calloc(l->n_l0 ? l->n_l0 : 1, sizeof(*keep));
    if (!keep) {
        rc = EFS_ERR_NOMEM;
        goto discard;
    }
    /* keep_at: index in keep[] before which the fold's output goes.
     * L0 is newest first, and a lookup takes the first hit. A file of
     * the same range flushed while the merge ran is newer than every
     * input, sits ahead of them, and must stay ahead of the output;
     * the output goes where the newest input was. */
    {
        uint32_t keep_at = UINT32_MAX;

        for (i = 0; i < l->n_l0; i++) {
            if (id_has(drop, n_drop, 0, l->l0[i].seq)) {
                if (n_doomed >= cap) {
                    rc = EFS_ERR_NOMEM;
                    goto discard;
                }
                doomed[n_doomed++] = l->l0[i];
                if (keep_at == UINT32_MAX)
                    keep_at = n_keep;
                continue;
            }
            keep[n_keep++] = l->l0[i];
        }
        if (keep_at == UINT32_MAX)
            keep_at = 0;
        {
        uint32_t nl0n = 0, nl1n = 0;
        uint32_t nl0_cap = n_keep + (l0_only ? c.n_out : 0);
        uint32_t nl1_cap = l->n_l1 + (l0_only ? 0 : c.n_out);

        nl0 = calloc(nl0_cap ? nl0_cap : 1, sizeof(*nl0));
        nl1 = calloc(nl1_cap ? nl1_cap : 1, sizeof(*nl1));
        if (!nl0 || !nl1) {
            free(nl0);
            rc = EFS_ERR_NOMEM;
            goto discard;
        }
        for (i = 0; i <= n_keep; i++) {
            if (l0_only && i == keep_at) {
                for (j = 0; j < c.n_out; j++) {
                    if (nl0n >= nl0_cap) {
                        rc = EFS_ERR_NOMEM;
                        goto discard;
                    }
                    nl0[nl0n++] = c.out[j];
                }
            }
            if (i == n_keep)
                break;
            if (nl0n >= nl0_cap) {
                rc = EFS_ERR_NOMEM;
                goto discard;
            }
            nl0[nl0n++] = keep[i];
        }
        for (i = 0; i < l->n_l1; i++) {
            if (id_has(drop, n_drop, 1, l->l1[i].seq)) {
                if (n_doomed >= cap) {
                    rc = EFS_ERR_NOMEM;
                    goto discard;
                }
                doomed[n_doomed++] = l->l1[i];
                continue;
            }
            if (nl1n >= nl1_cap) {
                rc = EFS_ERR_NOMEM;
                goto discard;
            }
            nl1[nl1n++] = l->l1[i];
        }
        if (!l0_only) {
            for (i = 0; i < c.n_out; i++) {
                if (nl1n >= nl1_cap) {
                    rc = EFS_ERR_NOMEM;
                    goto discard;
                }
                nl1[nl1n++] = c.out[i];
            }
        }
        rc = kv_l1_reserve(l, nl1n);
        if (rc != EFS_OK)
            goto discard;
        rc = kv_l0_reserve(l, nl0n);
        if (rc != EFS_OK)
            goto discard;
        l->n_l0 = nl0n;
        for (i = 0; i < nl0n; i++)
            l->l0[i] = nl0[i];
        l->n_l1 = nl1n;
        for (i = 0; i < nl1n; i++)
            l->l1[i] = nl1[i];
        }
    }
    if (l->n_l1 > 1)
        qsort(l->l1, l->n_l1, sizeof(l->l1[0]), kv_l1_cmp);
    rc = kv_manifest_write(l);
    if (rc != EFS_OK) {
        /* Outputs are already in the live level. The old MANIFEST still
         * names the inputs, so a restart GCs the outputs; this engine stops. */
        l->io_failed = 1;
        fprintf(stderr, "kv-compact: end bytes=%llu ms=%llu rc=%d\n",
                (unsigned long long)c.bytes,
                (unsigned long long)(mono_ms() - t0), rc);
        goto cleanup;
    }
    for (i = 0; i < n_doomed; i++) {
        char path[KV_LSM_PATH_MAX + 64];

        kv_seg_path(l, doomed[i].level, doomed[i].seq, path, sizeof(path));
        kv_seg_doom(doomed[i].seg, path);
        kv_seg_close(doomed[i].seg);
    }
    kv_sync_dir(l->dir);
    fprintf(stderr, "kv-compact: end bytes=%llu ms=%llu l0=%u l1=%u rc=0\n",
            (unsigned long long)c.bytes, (unsigned long long)(mono_ms() - t0),
            l->n_l0, l->n_l1);
    rc = EFS_OK;
    goto cleanup;

discard:
    for (i = 0; i < c.n_out; i++) {
        char path[KV_LSM_PATH_MAX + 64];

        if (c.out[i].seg)
            kv_seg_close(c.out[i].seg);
        kv_seg_path(l, c.out[i].level, c.out[i].seq, path, sizeof(path));
        unlink(path);
        c.out[i].seg = NULL;
    }
    fprintf(stderr, "kv-compact: end bytes=%llu ms=%llu rc=%d\n",
            (unsigned long long)c.bytes, (unsigned long long)(mono_ms() - t0),
            rc);
    goto cleanup;

out:
    for (i = 0; i < nsrc; i++)
        kv_seg_iter_close(src[i].it);
    for (i = 0; i < n_drop; i++)
        if (priv[i])
            kv_seg_close(priv[i]);
    if (c.w)
        kv_seg_w_abort(c.w);
    for (i = 0; i < c.n_out; i++) {
        char path[KV_LSM_PATH_MAX + 64];

        if (c.out[i].seg)
            kv_seg_close(c.out[i].seg);
        if (c.out[i].seq) {
            kv_seg_path(l, c.out[i].level, c.out[i].seq, path, sizeof(path));
            unlink(path);
        }
    }
    if (!held)
        pthread_mutex_lock(&l->mu);
    fprintf(stderr, "kv-compact: end bytes=%llu ms=%llu rc=%d\n",
            (unsigned long long)c.bytes, (unsigned long long)(mono_ms() - t0),
            rc);
cleanup:
    free(src);
    free(priv);
    free(drop);
    free(doomed);
    free(heap);
    free(keep);
    free(nl0);
    free(nl1);
    free(c.out);
    return rc;
}

static void compact_kick(struct kv_lsm *l)
{
    if (!l->compact_started)
        return;
    l->compact_req = 1;
    pthread_cond_signal(&l->compact_cv);
}

int kv_maybe_flush_locked(struct kv_lsm *l)
{
    uint32_t mmax = l->cfg.memtable_max ? l->cfg.memtable_max : KV_LSM_MEM_DEFAULT;
    uint32_t l0max = l->cfg.l0_max ? l->cfg.l0_max : KV_LSM_L0_DEFAULT;
    int rc;

    if (l->mt.bytes < mmax)
        return EFS_OK;
    /* The only stall left: L0 is at the engine cap, so the writer waits
     * until the compactor publishes. That is 16x behind (l0_max is 4);
     * log it. Inline compact only if the thread never started. */
    /* A flush may add one file per key range, so wait until that many
     * slots are free, not merely until one slot is. */
    /* D9: the apply path is the pump and holds h->mu. Waiting here for a
     * compaction (24 s for range 0) is a 24 s pump hold. Leave the
     * memtable in place and let the compactor catch up; admission of new
     * publish batches returns BUSY instead (efs_kv_lsm_l0_hot). */
    if (kv_l0_bytes(l) >= KV_LSM_L0_BYTES && l->n_l0 > 0) {
        if (!l->bp_logged) {
            fprintf(stderr, "kv-compact: backpressure n_l0=%u\n", l->n_l0);
            l->bp_logged = 1;
        }
        compact_kick(l);
        if (!l->compact_started)
            return kv_compact_locked(l, 0);
        return EFS_OK;
    }
    l->bp_logged = 0;
    if (l->io_failed)
        return EFS_ERR_IO;
    rc = kv_flush_locked(l);
    if (rc == EFS_ERR_BUSY) {
        /* Not enough L0 slots for this memtable's ranges. The keys are
         * in the memtable and the WAL; the apply must not fail. */
        compact_kick(l);
        return EFS_OK;
    }
    if (rc != EFS_OK)
        return rc;
    if (l->n_l0 >= l0max) {
        if (l->compact_started)
            compact_kick(l);
        else
            return kv_compact_locked(l, 0);
    }
    return EFS_OK;
}

static void *compactor_main(void *arg)
{
    struct kv_lsm *l = arg;

    for (;;) {
        uint32_t l0max;
        int rc;

        pthread_mutex_lock(&l->mu);
        l0max = l->cfg.l0_max ? l->cfg.l0_max : KV_LSM_L0_DEFAULT;
        while (!l->compact_stop && !l->compact_req && l->n_l0 < l0max)
            pthread_cond_wait(&l->compact_cv, &l->mu);
        if (l->compact_stop) {
            pthread_mutex_unlock(&l->mu);
            break;
        }
        l->compact_req = 0;
        if (l->n_l0 == 0 || l->io_failed) {
            pthread_cond_broadcast(&l->cv);
            pthread_mutex_unlock(&l->mu);
            continue;
        }
        while (l->compact_busy && !l->compact_stop)
            pthread_cond_wait(&l->cv, &l->mu);
        if (l->compact_stop) {
            pthread_mutex_unlock(&l->mu);
            break;
        }
        l->compact_busy = 1;
        rc = kv_compact_locked(l, 1);
        l->compact_busy = 0;
        if (rc != EFS_OK && rc != EFS_ERR_BUSY)
            l->io_failed = 1;
        pthread_cond_broadcast(&l->cv);
        /* A failed merge must not spin. The next flush or quiesce kicks. */
        if (rc != EFS_OK && !l->compact_stop && !l->compact_req)
            pthread_cond_wait(&l->compact_cv, &l->mu);
        pthread_mutex_unlock(&l->mu);
    }
    return NULL;
}

void kv_compactor_start(struct kv_lsm *l)
{
    if (pthread_create(&l->compact_thr, NULL, compactor_main, l) != 0) {
        fprintf(stderr, "kv-compact: thread start failed\n");
        return;
    }
    l->compact_started = 1;
}

void kv_compactor_stop(struct kv_lsm *l)
{
    if (!l || !l->compact_started)
        return;
    pthread_mutex_lock(&l->mu);
    l->compact_stop = 1;
    pthread_cond_signal(&l->compact_cv);
    pthread_cond_broadcast(&l->cv);
    pthread_mutex_unlock(&l->mu);
    pthread_join(l->compact_thr, NULL);
    l->compact_started = 0;
}
