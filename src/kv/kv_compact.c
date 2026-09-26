/* Turning the memtable into segments, and segments into fewer segments.
 *
 * Flush writes the memtable out as one L0 segment; L0 segments have
 * overlapping key ranges, so a read consults all of them newest-first.
 * Compaction merges every L0 plus the L1 segments whose ranges they touch
 * into fresh disjoint L1 segments, so a read consults at most one of those.
 * Both publish by writing the MANIFEST, which is the only authority on
 * which segment files are live; the old files are unlinked once nothing
 * still pins them.
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
    uint64_t bytes;
    struct seg_slot out[KV_LSM_MAX_SEGS];
    uint32_t n_out;
    int rc;
};

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

    if (c->w && kv_seg_w_bytes(c->w) >= KV_LSM_L1_TARGET) {
        c->bytes += kv_seg_w_bytes(c->w);
        rc = kv_seg_w_finish(c->w);
        c->w = NULL;
        if (rc != EFS_OK)
            return rc;
        compact_maybe_die();
        c->n_out++;
    }
    if (!c->w) {
        if (c->n_out >= KV_LSM_MAX_SEGS)
            return EFS_ERR_BUSY;
        c->seq = take_seq(c->l);
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
 * L1 runs, so cost tracks overlapping data rather than the whole store.
 * Caller holds l->mu. async=1 releases it across the file merge. */
int kv_compact_locked(struct kv_lsm *l, int async)
{
    struct msrc src[KV_LSM_MAX_SEGS * 2];
    struct kv_seg *priv[KV_LSM_MAX_SEGS * 2];
    struct seg_id drop[KV_LSM_MAX_SEGS * 2];
    struct seg_slot keep[KV_LSM_MAX_SEGS];
    struct seg_slot doomed[KV_LSM_MAX_SEGS * 2];
    struct compact_ctx c;
    const uint8_t *lo = NULL, *hi = NULL;
    uint32_t lol = 0, hil = 0;
    uint32_t nsrc = 0, n_drop = 0, n_keep = 0, n_doomed = 0, i, j;
    uint32_t n_l0_in = 0;
    int rc = EFS_OK;
    int have_range = 0;
    int held = 1;
    uint64_t t0;

    if (l->n_l0 == 0)
        return EFS_OK;
    memset(src, 0, sizeof(src));
    memset(priv, 0, sizeof(priv));
    memset(&c, 0, sizeof(c));
    c.l = l;
    n_l0_in = l->n_l0;
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
        if (n_drop >= KV_LSM_MAX_SEGS * 2)
            return EFS_ERR_BUSY;
        drop[n_drop].level = 0;
        drop[n_drop].seq = l->l0[i].seq;
        n_drop++;
    }
    if (!have_range)
        return EFS_OK;
    for (i = 0; i < l->n_l1; i++) {
        if (!overlaps(l->l1[i].seg, lo, lol, hi, hil))
            continue;
        if (n_drop >= KV_LSM_MAX_SEGS * 2)
            return EFS_ERR_BUSY;
        drop[n_drop].level = 1;
        drop[n_drop].seq = l->l1[i].seq;
        n_drop++;
    }
    t0 = mono_ms();
    fprintf(stderr, "kv-compact: start l0=%u inputs=%u\n", n_l0_in, n_drop);
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
        nsrc++;
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

        kv_seg_path(l, 1, c.out[i].seq, path, sizeof(path));
        rc = kv_seg_open(path, &c.out[i].seg);
        if (rc != EFS_OK)
            goto out;
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
    for (i = 0; i < l->n_l0; i++) {
        if (id_has(drop, n_drop, 0, l->l0[i].seq)) {
            if (n_doomed < KV_LSM_MAX_SEGS * 2)
                doomed[n_doomed++] = l->l0[i];
            continue;
        }
        if (n_keep >= KV_LSM_MAX_SEGS) {
            rc = EFS_ERR_BUSY;
            goto discard;
        }
        /* Newer L0 flushed during the merge stays L0. Stash it in
         * keep[0..] temporarily; split below. */
        keep[n_keep++] = l->l0[i];
    }
    {
        struct seg_slot nl0[KV_LSM_MAX_SEGS];
        struct seg_slot nl1[KV_LSM_MAX_SEGS];
        uint32_t nl0n = n_keep, nl1n = 0;

        for (i = 0; i < n_keep; i++)
            nl0[i] = keep[i];
        for (i = 0; i < l->n_l1; i++) {
            if (id_has(drop, n_drop, 1, l->l1[i].seq)) {
                if (n_doomed < KV_LSM_MAX_SEGS * 2)
                    doomed[n_doomed++] = l->l1[i];
                continue;
            }
            if (nl1n >= KV_LSM_MAX_SEGS) {
                rc = EFS_ERR_BUSY;
                goto discard;
            }
            nl1[nl1n++] = l->l1[i];
        }
        for (i = 0; i < c.n_out; i++) {
            if (nl1n >= KV_LSM_MAX_SEGS) {
                rc = EFS_ERR_BUSY;
                goto discard;
            }
            nl1[nl1n++] = c.out[i];
        }
        l->n_l0 = nl0n;
        for (i = 0; i < nl0n; i++)
            l->l0[i] = nl0[i];
        l->n_l1 = nl1n;
        for (i = 0; i < nl1n; i++)
            l->l1[i] = nl1[i];
    }
    if (l->n_l1 > 1)
        qsort(l->l1, l->n_l1, sizeof(l->l1[0]), kv_l1_cmp);
    rc = kv_manifest_write(l);
    if (rc != EFS_OK) {
        /* Outputs are already in l->l1. The old MANIFEST still names the
         * inputs, so a restart GCs the outputs; this engine stops. */
        l->io_failed = 1;
        fprintf(stderr, "kv-compact: end bytes=%llu ms=%llu rc=%d\n",
                (unsigned long long)c.bytes,
                (unsigned long long)(mono_ms() - t0), rc);
        return rc;
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
    return EFS_OK;

discard:
    for (i = 0; i < c.n_out; i++) {
        char path[KV_LSM_PATH_MAX + 64];

        if (c.out[i].seg)
            kv_seg_close(c.out[i].seg);
        kv_seg_path(l, 1, c.out[i].seq, path, sizeof(path));
        unlink(path);
        c.out[i].seg = NULL;
    }
    fprintf(stderr, "kv-compact: end bytes=%llu ms=%llu rc=%d\n",
            (unsigned long long)c.bytes, (unsigned long long)(mono_ms() - t0),
            rc);
    return rc;

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
            kv_seg_path(l, 1, c.out[i].seq, path, sizeof(path));
            unlink(path);
        }
    }
    if (!held)
        pthread_mutex_lock(&l->mu);
    fprintf(stderr, "kv-compact: end bytes=%llu ms=%llu rc=%d\n",
            (unsigned long long)c.bytes, (unsigned long long)(mono_ms() - t0),
            rc);
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
    while (l->n_l0 >= KV_LSM_MAX_SEGS && !l->io_failed) {
        if (!l->bp_logged) {
            fprintf(stderr, "kv-compact: backpressure n_l0=%u\n", l->n_l0);
            l->bp_logged = 1;
        }
        if (!l->compact_started) {
            rc = kv_compact_locked(l, 0);
            if (rc != EFS_OK)
                return rc;
            break;
        }
        compact_kick(l);
        pthread_cond_wait(&l->cv, &l->mu);
    }
    l->bp_logged = 0;
    if (l->io_failed)
        return EFS_ERR_IO;
    rc = kv_flush_locked(l);
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
