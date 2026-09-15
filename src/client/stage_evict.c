/* Client staging-table evictor — client-cache design Part A
 * (docs/client-cache-design.md §4).
 *
 * g_client.export is a cache of what this client is DOING, not a replica of
 * what exists. Everything in it is re-fetchable from the servers (rows via
 * GETATTR+adopt, chunk maps via GETCHUNKS, names via LOOKUP), so a row may
 * be evicted whenever no in-flight or owed work can still observe its
 * absence. This module bounds the table to EFS_CLIENT_META_MB (default
 * 256 MB, counted by efs_export_staged_bytes over rows + name arenas +
 * chunk array + indexes) by evicting clean, closed, unpinned rows LRU.
 *
 * Pin rules (a row is unevictable iff any hold):
 *   1. ino is in the dirty set / publishing set (efs_client_ino_is_dirty)
 *      or has unreported dcache data (efs_dcache_ino_pinned) — eviction
 *      would silently drop a REPORT record = data loss;
 *   2. ghost (nlink=0) with an open fd (efs_client_ino_is_open) — open-fd
 *      stat/read must keep working;
 *   3. live byte-range lock record (efs_client_ino_has_plock).
 * The create/rename/unlink dual-apply windows are covered by the locking
 * discipline below, not a per-op pin: every row mutation runs under a
 * dir-stripe lock, and the evictor holds the whole table lock while it
 * evicts, so no op can be mid-mutation on the victim.
 *
 * Check-then-remove atomicity: the final pin re-check and the removal both
 * happen under table_lock + idx_mu + dirty_mu. dirty_mu is what makes
 * efs_client_mark_*_dirty (which takes no stripe) exclude with the check.
 * Lock order here is table -> idx -> dirty -> {pin,open,plock}mu (leaf
 * locks). No path acquires any of these in reverse.
 *
 * A correct client under memory pressure degrades to more RPCs, never to
 * lost writes: if everything is pinned the table simply grows (pinned data
 * is real work, not cache) and the evictor logs once.
 */

#include "efs/common.h"
#include "efs/metadata.h"
#include "client_internal.h"

#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ---------------- LRU side table: ino -> last-touch tick ----------------
 * Open-addressing, power-of-two, 0 = empty, backward-shift delete. Every
 * access under g_lru_mu (a leaf lock). One entry per staged ino; entries
 * for unlinked rows are removed when the row goes (eviction, ghost reclaim)
 * or when a pass finds them unstaged. */

static pthread_mutex_t g_lru_mu = PTHREAD_MUTEX_INITIALIZER;
static uint64_t *g_lru_keys;  /* ino */
static uint64_t *g_lru_ticks; /* touch tick (monotonic counter) */
static uint64_t g_lru_mask;
static uint64_t g_lru_count;
static uint64_t g_lru_clock;

/* Pin providers for rules 2 and 3 live in the FUSE layer (open-fd and
 * byte-range-lock tables are efs_fuse.c state). This module is linked into
 * non-FUSE binaries too (efs-bench, unit tests), so the dependency is
 * INVERTED: the FUSE layer registers its accessors at startup. With no
 * hook registered the rule simply never pins (correct: a binary without
 * FUSE has no open fds or locks). Set once before the evictor thread
 * starts; read without a lock afterwards (never mutated again). */
static int (*g_pin_is_open_fn)(efs_ino_t);
static int (*g_pin_has_plock_fn)(efs_ino_t);

void efs_client_stage_set_pin_hooks(int (*is_open)(efs_ino_t),
                                    int (*has_plock)(efs_ino_t))
{
    g_pin_is_open_fn = is_open;
    g_pin_has_plock_fn = has_plock;
}

static int stage_ino_is_open(efs_ino_t ino)
{
    return g_pin_is_open_fn ? g_pin_is_open_fn(ino) : 0;
}

static int stage_ino_has_plock(efs_ino_t ino)
{
    return g_pin_has_plock_fn ? g_pin_has_plock_fn(ino) : 0;
}

static int lru_ensure(uint64_t need)
{
    if (g_lru_keys && need * 2 <= g_lru_mask + 1)
        return 0;
    uint64_t old_mask = g_lru_mask;
    uint64_t *old_keys = g_lru_keys;
    uint64_t *old_ticks = g_lru_ticks;
    uint64_t cap = g_lru_mask ? g_lru_mask + 1 : 4096;
    while (cap < need * 4)
        cap *= 2;
    uint64_t *nk = calloc(cap, sizeof(*nk));
    uint64_t *nt = calloc(cap, sizeof(*nt));
    if (!nk || !nt) {
        free(nk);
        free(nt);
        return -1;
    }
    g_lru_keys = nk;
    g_lru_ticks = nt;
    g_lru_mask = cap - 1;
    if (old_keys) {
        for (uint64_t i = 0; i <= old_mask; i++) {
            uint64_t k = old_keys[i];
            if (!k)
                continue;
            uint64_t j = k & g_lru_mask;
            while (g_lru_keys[j])
                j = (j + 1) & g_lru_mask;
            g_lru_keys[j] = k;
            g_lru_ticks[j] = old_ticks[i];
        }
        free(old_keys);
        free(old_ticks);
    }
    return 0;
}

/* Record a use of ino. Called from the staging/read/write paths; cheap
 * hash upsert under a leaf lock, safe with any other locks held. */
void efs_client_stage_touch(efs_ino_t ino)
{
    if (!ino)
        return;
    pthread_mutex_lock(&g_lru_mu);
    if (lru_ensure(g_lru_count + 1) == 0) {
        uint64_t tick = ++g_lru_clock;
        uint64_t i = (uint64_t)ino & g_lru_mask;
        for (;;) {
            if (g_lru_keys[i] == (uint64_t)ino) {
                g_lru_ticks[i] = tick;
                break;
            }
            if (!g_lru_keys[i]) {
                g_lru_keys[i] = (uint64_t)ino;
                g_lru_ticks[i] = tick;
                g_lru_count++;
                break;
            }
            i = (i + 1) & g_lru_mask;
        }
    }
    pthread_mutex_unlock(&g_lru_mu);
}

/* Caller holds g_lru_mu. */
static void lru_remove_locked(efs_ino_t ino)
{
    if (!g_lru_keys)
        return;
    uint64_t i = (uint64_t)ino & g_lru_mask;
    for (uint64_t n = 0; n <= g_lru_mask; n++) {
        if (!g_lru_keys[i])
            return;
        if (g_lru_keys[i] != (uint64_t)ino) {
            i = (i + 1) & g_lru_mask;
            continue;
        }
        /* Backward-shift delete (same scheme as the dcache pin table). */
        g_lru_keys[i] = 0;
        g_lru_count--;
        uint64_t j = (i + 1) & g_lru_mask;
        while (g_lru_keys[j]) {
            uint64_t h = g_lru_keys[j] & g_lru_mask;
            int in_gap = (i < j) ? (h > i && h <= j) : (h > i || h <= j);
            if (!in_gap) {
                g_lru_keys[i] = g_lru_keys[j];
                g_lru_ticks[i] = g_lru_ticks[j];
                g_lru_keys[j] = 0;
                i = j;
            }
            j = (j + 1) & g_lru_mask;
        }
        return;
    }
}

/* ------------------------- evictor thread ------------------------- */

static pthread_t g_evict_tid;
static int g_evict_started;
static pthread_mutex_t g_evict_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_evict_cv = PTHREAD_COND_INITIALIZER;
static int g_evict_stop;
static int g_evict_kick;
/* Last staged-bytes reading, refreshed by every pass. Read lock-free by
 * the kick path so a create burst only wakes the evictor when the table
 * is actually over the cap. */
static uint64_t g_stage_bytes_seen;
/* Last staged-bytes reading, refreshed by every pass. Read lock-free by
 * the kick path so a create burst only wakes the evictor when the table
 * is actually over the cap. */
static uint64_t g_stage_bytes_seen;
/* Last staged-bytes reading, refreshed by every pass. Read lock-free by
 * the kick path so a create burst only wakes the evictor when the table
 * is actually over the cap. */
static uint64_t g_stage_bytes_seen;

static uint64_t stage_cap_bytes(void)
{
    const char *env = getenv("EFS_CLIENT_META_MB");
    if (env && *env) {
        unsigned long v = strtoul(env, NULL, 10);
        if (v >= 16 && v <= (1UL << 20))
            return (uint64_t)v << 20;
    }
    return 256ULL << 20;
}

/* Scan collects SCAN_BATCH oldest candidates per pass; up to EVICT_BATCH
 * of them (unpinned) are evicted. The 4x scan ratio keeps a pass
 * productive when part of the oldest cohort is pinned (open files of a
 * long-running writer). Bounded so a pass never holds locks for long;
 * FUSE ops interleave between passes. */
#define EVICT_BATCH 64
#define SCAN_BATCH  256

struct evict_cand {
    efs_ino_t ino;
    uint64_t tick;
};

/* Pin check WITHOUT the table lock (cheap pre-filter). The authoritative
 * re-check happens under the locks in evict_one(). */
static int stage_pinned(efs_ino_t ino)
{
    if (efs_client_ino_is_dirty(ino))
        return 1;
    if (efs_dcache_ino_pinned(ino))
        return 1;
    if (stage_ino_is_open(ino))
        return 1;
    if (stage_ino_has_plock(ino))
        return 1;
    return 0;
}

/* Evict one candidate. Returns 1 if the ino's staged state (or a stale LRU
 * entry) was dropped, 0 if it was pinned or touched since the scan. */
static int evict_one(efs_ino_t ino, uint64_t tick)
{
    int dropped = 0;
    efs_client_table_lock();
    pthread_mutex_lock(&g_client.idx_mu);
    pthread_mutex_lock(&g_client.dirty_mu);
    /* Re-verify under the locks: a touch or a dirty/pin transition since
     * the scan must win. Dirty-set membership is stable here because every
     * mark_*_dirty takes dirty_mu (held) and every row mutation takes a
     * stripe (all held by the table lock). */
    if (efs_client_ino_is_dirty_locked(ino) || efs_dcache_ino_pinned(ino) ||
        stage_ino_is_open(ino) || stage_ino_has_plock(ino))
        goto out;
    pthread_mutex_lock(&g_lru_mu);
    uint64_t cur_tick = 0;
    int listed = 0;
    if (g_lru_keys) {
        uint64_t i = (uint64_t)ino & g_lru_mask;
        for (uint64_t n = 0; n <= g_lru_mask; n++) {
            if (!g_lru_keys[i])
                break;
            if (g_lru_keys[i] == (uint64_t)ino) {
                listed = 1;
                cur_tick = g_lru_ticks[i];
                break;
            }
            i = (i + 1) & g_lru_mask;
        }
    }
    if (listed && cur_tick != tick) {
        /* Touched since the scan — no longer among the oldest. */
        pthread_mutex_unlock(&g_lru_mu);
        goto out;
    }
    /* A ghost with no open fd is evictable (rule 2 pins only OPEN ghosts);
     * forget_ino handles rows and chunk recs across every loaded tab. A
     * not-staged ino is a no-op there — this also GCs its LRU entry. */
    efs_export_forget_ino(&g_client.export, ino);
    lru_remove_locked(ino);
    pthread_mutex_unlock(&g_lru_mu);
    dropped = 1;
out:
    pthread_mutex_unlock(&g_client.dirty_mu);
    pthread_mutex_unlock(&g_client.idx_mu);
    efs_client_table_unlock();
    return dropped;
}

/* One pass: if over the cap, evict up to EVICT_BATCH oldest unpinned inos.
 * Returns the staged-bytes reading that decided the pass. */
static uint64_t evict_pass(uint64_t cap, int *evicted)
{
    uint64_t bytes;
    *evicted = 0;

    efs_client_table_lock();
    bytes = efs_export_staged_bytes(&g_client.export);
    efs_client_table_unlock();
    __atomic_store_n(&g_stage_bytes_seen, bytes, __ATOMIC_RELAXED);
    if (bytes <= cap)
        return bytes;

    /* Collect the SCAN_BATCH oldest entries (unsorted array + running
     * worst; replace-worst only when a strictly older tick appears). */
    static struct evict_cand cand[SCAN_BATCH];
    uint32_t nc = 0;
    uint64_t worst = 0; /* max tick currently in cand[] */
    pthread_mutex_lock(&g_lru_mu);
    if (g_lru_keys) {
        for (uint64_t i = 0; i <= g_lru_mask; i++) {
            uint64_t k = g_lru_keys[i];
            if (!k)
                continue;
            uint64_t t = g_lru_ticks[i];
            if (nc < SCAN_BATCH) {
                cand[nc].ino = (efs_ino_t)k;
                cand[nc].tick = t;
                if (t > worst)
                    worst = t;
                nc++;
            } else if (t < worst) {
                uint32_t w = 0;
                for (uint32_t j = 1; j < nc; j++)
                    if (cand[j].tick > cand[w].tick)
                        w = j;
                cand[w].ino = (efs_ino_t)k;
                cand[w].tick = t;
                worst = cand[0].tick;
                for (uint32_t j = 1; j < nc; j++)
                    if (cand[j].tick > worst)
                        worst = cand[j].tick;
            }
        }
    }
    pthread_mutex_unlock(&g_lru_mu);

    uint32_t evicted_n = 0;
    for (uint32_t i = 0; i < nc && evicted_n < EVICT_BATCH; i++) {
        if (stage_pinned(cand[i].ino))
            continue;
        if (evict_one(cand[i].ino, cand[i].tick)) {
            (*evicted)++;
            evicted_n++;
        }
    }
    if (evicted_n && getenv("EFS_STAGE_DBG"))
        fprintf(stderr,
                "stage-evict: pass bytes=%lluMB cand=%u evicted=%u\n",
                (unsigned long long)(bytes >> 20), nc, evicted_n);
    return bytes;
}

static void *stage_evict_main(void *arg)
{
    (void)arg;
    uint64_t cap = stage_cap_bytes();
    int logged_grow = 0;
    pthread_mutex_lock(&g_evict_mu);
    for (;;) {
        while (!g_evict_stop && !g_evict_kick) {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_sec += 1;
            if (pthread_cond_timedwait(&g_evict_cv, &g_evict_mu, &ts) != 0)
                break; /* 1 s elapsed: run a pass */
        }
        if (g_evict_stop)
            break;
        g_evict_kick = 0;
        pthread_mutex_unlock(&g_evict_mu);

        /* Drain passes until under the cap or nothing is evictable. */
        for (int rounds = 0; rounds < 1024; rounds++) {
            int evicted = 0;
            uint64_t bytes = evict_pass(cap, &evicted);
            if (bytes <= cap)
                break;
            if (evicted == 0) {
                /* Everything found is pinned (or already gone): grow —
                 * pinned data is real work, not cache. Log once. */
                if (!logged_grow) {
                    logged_grow = 1;
                    fprintf(stderr,
                            "efs-fuse: staging table over EFS_CLIENT_META_MB "
                            "(%llu MB) with nothing evictable; growing "
                            "(pinned data is real work, not cache)\n",
                            (unsigned long long)(cap >> 20));
                }
                break;
            }
            /* Reclaim index/array over-capacity left by the removals. */
            efs_client_table_lock();
            pthread_mutex_lock(&g_client.idx_mu);
            efs_export_compact(&g_client.export);
            pthread_mutex_unlock(&g_client.idx_mu);
            efs_client_table_unlock();
            sched_yield();
        }

        pthread_mutex_lock(&g_evict_mu);
    }
    pthread_mutex_unlock(&g_evict_mu);
    return NULL;
}

void efs_client_stage_evict_start(void)
{
    pthread_mutex_lock(&g_evict_mu);
    if (!g_evict_started) {
        g_evict_stop = 0;
        g_evict_kick = 0;
        if (pthread_create(&g_evict_tid, NULL, stage_evict_main, NULL) == 0)
            g_evict_started = 1;
    }
    pthread_mutex_unlock(&g_evict_mu);
}

void efs_client_stage_evict_stop(void)
{
    pthread_mutex_lock(&g_evict_mu);
    if (!g_evict_started) {
        pthread_mutex_unlock(&g_evict_mu);
        return;
    }
    g_evict_stop = 1;
    pthread_cond_signal(&g_evict_cv);
    pthread_mutex_unlock(&g_evict_mu);
    pthread_join(g_evict_tid, NULL);
    pthread_mutex_lock(&g_evict_mu);
    g_evict_started = 0;
    g_evict_stop = 0;
    pthread_mutex_unlock(&g_evict_mu);
}

/* Kick the evictor (called when a staging path notices the cap is hit).
 * Gated on the last pass's occupancy reading so staging under the cap
 * never wakes the thread. */
void efs_client_stage_evict_kick(void)
{
    if (!g_evict_started)
        return;
    if (__atomic_load_n(&g_stage_bytes_seen, __ATOMIC_RELAXED) <=
        stage_cap_bytes())
        return;
    pthread_mutex_lock(&g_evict_mu);
    g_evict_kick = 1;
    pthread_cond_signal(&g_evict_cv);
    pthread_mutex_unlock(&g_evict_mu);
}

/* Targeted drop of one ino — ghost reclaim at last close (design §4 rule 2
 * expiry). The caller has already observed the last close; if the staged
 * row is a ghost (nlink=0) it can never be re-fetched by name, so drop it
 * now rather than waiting for LRU pressure. */
void efs_client_stage_evict_ino(efs_ino_t ino)
{
    if (!ino)
        return;
    efs_client_table_lock();
    pthread_mutex_lock(&g_client.idx_mu);
    struct efs_inode cur;
    if (efs_export_get_inode(&g_client.export, ino, &cur) == EFS_OK &&
        cur.nlink == 0 && !efs_client_ino_is_dirty(ino) &&
        !efs_dcache_ino_pinned(ino)) {
        efs_export_forget_ino(&g_client.export, ino);
        pthread_mutex_lock(&g_lru_mu);
        lru_remove_locked(ino);
        pthread_mutex_unlock(&g_lru_mu);
    }
    pthread_mutex_unlock(&g_client.idx_mu);
    efs_client_table_unlock();
}
