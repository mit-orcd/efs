/* Client staging-table evictor — client-cache design Part A
 * (docs/archive/landed/client-cache-design.md §4).
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
 *   2. an open fd (efs_client_ino_is_open). The design names this for a
 *      ghost; every open fd is pinned so stat/read of that fd still
 *      resolves;
 *   3. an in-flight op pin (efs_client_stage_pin) — create, rename, link,
 *      unlink, and a live append reservation. Dropped when the op ends;
 *   4. a byte-range lock record (efs_client_ino_has_plock).
 * Chunk maps of an unpinned clean file go first. The row follows on a
 * later pass, once the maps are gone.
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
/* While now < this, a kick does not wake a scan that just found nothing
 * evictable. Cleared when a pin drops, so a real opportunity runs at once. */
static int64_t g_evict_idle_until;

static int64_t evict_now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void evict_idle_clear(void)
{
    __atomic_store_n(&g_evict_idle_until, 0, __ATOMIC_RELAXED);
}
/* Oldest tick already proven pinned this drain. The next wake starts
 * here instead of rescanning that prefix. */
static uint64_t g_evict_cursor;
/* Last staged-bytes reading, refreshed by every pass. Read lock-free by
 * the kick path so a create burst only wakes the evictor when the table
 * is actually over the cap. */
static uint64_t g_stage_bytes_seen;

/* In-flight op pins (design rule 3 and the append-reservation half of
 * rule 4). A count, not a bool: create pins the new ino and the parent,
 * and those can be the same ino. pin/unpin take only this leaf lock and
 * drop it before returning, so a caller may already hold a dir stripe.
 * The evictor takes stripes, then idx, then dirty, then this. */
static pthread_mutex_t g_opin_mu = PTHREAD_MUTEX_INITIALIZER;
static uint64_t *g_opin_keys;
static uint32_t *g_opin_counts;
static uint64_t g_opin_mask;
static uint64_t g_opin_count;

static int opin_ensure(uint64_t need)
{
    if (g_opin_keys && need * 2 <= g_opin_mask + 1)
        return 0;
    uint64_t old_mask = g_opin_mask;
    uint64_t *old_keys = g_opin_keys;
    uint32_t *old_counts = g_opin_counts;
    uint64_t cap = g_opin_mask ? g_opin_mask + 1 : 64;
    while (cap < need * 4)
        cap *= 2;
    uint64_t *nk = calloc(cap, sizeof(*nk));
    uint32_t *nc = calloc(cap, sizeof(*nc));
    if (!nk || !nc) {
        free(nk);
        free(nc);
        return -1;
    }
    g_opin_keys = nk;
    g_opin_counts = nc;
    g_opin_mask = cap - 1;
    if (old_keys) {
        for (uint64_t i = 0; i <= old_mask; i++) {
            uint64_t k = old_keys[i];
            if (!k)
                continue;
            uint64_t j = k & g_opin_mask;
            while (g_opin_keys[j])
                j = (j + 1) & g_opin_mask;
            g_opin_keys[j] = k;
            g_opin_counts[j] = old_counts[i];
        }
        free(old_keys);
        free(old_counts);
    }
    return 0;
}

static void opin_del_locked(uint64_t i)
{
    g_opin_keys[i] = 0;
    g_opin_counts[i] = 0;
    g_opin_count--;
    uint64_t j = (i + 1) & g_opin_mask;
    while (g_opin_keys[j]) {
        uint64_t h = g_opin_keys[j] & g_opin_mask;
        int in_gap = (i < j) ? (h > i && h <= j) : (h > i || h <= j);
        if (!in_gap) {
            g_opin_keys[i] = g_opin_keys[j];
            g_opin_counts[i] = g_opin_counts[j];
            g_opin_keys[j] = 0;
            g_opin_counts[j] = 0;
            i = j;
        }
        j = (j + 1) & g_opin_mask;
    }
}

void efs_client_stage_pin(efs_ino_t ino)
{
    if (!ino)
        return;
    pthread_mutex_lock(&g_opin_mu);
    if (opin_ensure(g_opin_count + 1) != 0) {
        pthread_mutex_unlock(&g_opin_mu);
        return;
    }
    uint64_t i = (uint64_t)ino & g_opin_mask;
    for (;;) {
        if (g_opin_keys[i] == (uint64_t)ino) {
            if (g_opin_counts[i] != 0xffffffffu)
                g_opin_counts[i]++;
            break;
        }
        if (!g_opin_keys[i]) {
            g_opin_keys[i] = (uint64_t)ino;
            g_opin_counts[i] = 1;
            g_opin_count++;
            break;
        }
        i = (i + 1) & g_opin_mask;
    }
    pthread_mutex_unlock(&g_opin_mu);
}

void efs_client_stage_unpin(efs_ino_t ino)
{
    if (!ino)
        return;
    pthread_mutex_lock(&g_opin_mu);
    if (g_opin_keys) {
        uint64_t i = (uint64_t)ino & g_opin_mask;
        for (uint64_t n = 0; n <= g_opin_mask; n++) {
            if (!g_opin_keys[i])
                break;
            if (g_opin_keys[i] == (uint64_t)ino) {
                if (g_opin_counts[i] > 1)
                    g_opin_counts[i]--;
                else
                    opin_del_locked(i);
                evict_idle_clear();
                break;
            }
            i = (i + 1) & g_opin_mask;
        }
    }
    pthread_mutex_unlock(&g_opin_mu);
}

static int stage_op_pinned(efs_ino_t ino)
{
    int hit = 0;

    if (!ino)
        return 0;
    pthread_mutex_lock(&g_opin_mu);
    if (g_opin_keys) {
        uint64_t i = (uint64_t)ino & g_opin_mask;
        for (uint64_t n = 0; n <= g_opin_mask; n++) {
            if (!g_opin_keys[i])
                break;
            if (g_opin_keys[i] == (uint64_t)ino) {
                hit = g_opin_counts[i] > 0;
                break;
            }
            i = (i + 1) & g_opin_mask;
        }
    }
    pthread_mutex_unlock(&g_opin_mu);
    return hit;
}

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
    if (stage_op_pinned(ino))
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
        stage_ino_is_open(ino) || stage_op_pinned(ino) ||
        stage_ino_has_plock(ino))
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
    /* Chunk maps first. A later pass drops the row once the maps are
     * gone. efs_export_evict_ino visits the tabs the row names (three
     * walks over every loaded tab per ino were 250 us under the table
     * lock and a 14–27 ms once-a-second stall on every client RPC,
     * Sep 30 2026). */
    if (efs_export_evict_ino(&g_client.export, ino) == 1) {
        pthread_mutex_unlock(&g_lru_mu);
        dropped = 1;
        goto out;
    }
    lru_remove_locked(ino);
    pthread_mutex_unlock(&g_lru_mu);
    dropped = 1;
out:
    pthread_mutex_unlock(&g_client.dirty_mu);
    pthread_mutex_unlock(&g_client.idx_mu);
    efs_client_table_unlock();
    return dropped;
}

/* One pass: if over the cap, evict up to EVICT_BATCH unpinned inos
 * among the SCAN_BATCH oldest whose touch tick is greater than
 * min_tick. A fully pinned oldest window used to end the drain, so a
 * client that still held a few dirty files never reached the clean
 * ones behind them and the table grew without a bound. *band_full is
 * 1 when that window was full (newer entries may exist). *band_max is
 * the newest tick in the window. Returns the staged-bytes reading. */
static uint64_t stage_bytes_now(void)
{
    uint64_t bytes;
    efs_client_table_lock();
    bytes = efs_export_staged_bytes(&g_client.export);
    efs_client_table_unlock();
    __atomic_store_n(&g_stage_bytes_seen, bytes, __ATOMIC_RELAXED);
    return bytes;
}

static uint64_t evict_pass(uint64_t cap, int *evicted, uint64_t min_tick,
                           int *band_full, uint64_t *band_max, uint64_t bytes)
{
    *evicted = 0;
    *band_full = 0;
    *band_max = min_tick;

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
            if (t <= min_tick)
                continue;
            if (nc < SCAN_BATCH) {
                cand[nc].ino = (efs_ino_t)k;
                cand[nc].tick = t;
                if (t > worst)
                    worst = t;
                nc++;
            } else if (t < worst && t > min_tick) {
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
    *band_full = nc == SCAN_BATCH;
    for (uint32_t i = 0; i < nc; i++)
        if (cand[i].tick > *band_max)
            *band_max = cand[i].tick;

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

/* ---------------- D18: whole-tab eviction (Oct 1 2026) ----------------
 * A loaded shard tab costs its slab + index floor (~90–170 KB) however
 * few rows it holds, and ino → shard is uniform, so a client that has
 * touched a few thousand files has ~all 4096 tabs loaded and the floor
 * alone (360–700 MB) is above EFS_CLIENT_META_MB. The row LRU then evicts
 * 64 just-closed rows per pass forever and never reaches the cap: 31.6 %
 * of a 62 min ecopy profile, 47.6 % of its last slice
 * (results/measure/20260930-205300-ecopy-perf-review). After the row
 * bands, if the table is still over the cap, drop the least recently
 * used tabs no pinned ino lives on (same pin rules as evict_one, same
 * lock set, check and drop under one hold) until it is under. The tab
 * is rebuilt empty by efs_export_table() on the next use of its shard;
 * everything it held is re-fetchable. The user chose this over counting
 * the cap above the floor or shrinking the floor (docs/status/decisions.md D18). */
#define TAB_SCAN  256
#define TAB_EVICT 64

static int tab_pin_cb(efs_ino_t ino, void *arg)
{
    (void)arg;
    /* Caller holds table + idx_mu + dirty_mu (evict_one's set). */
    if (efs_client_ino_is_dirty_locked(ino) || efs_dcache_ino_pinned(ino) ||
        stage_ino_is_open(ino) || stage_op_pinned(ino) ||
        stage_ino_has_plock(ino))
        return 1;
    return 0;
}

static int tab_lru_cb(efs_ino_t ino, void *arg)
{
    (void)arg;
    /* Caller holds g_lru_mu. The ino's remaining traces (a dentry stub
     * or chunk recs on another tab) go when that tab goes; a later touch
     * re-enters it. Leaving the entry would send evict_one through the
     * every-tab fan-out for a row that is gone. */
    lru_remove_locked(ino);
    return 0;
}

/* Returns the number of tabs dropped; *bytes_io is the reading after. */
static int evict_cold_tabs(uint64_t cap, uint64_t *bytes_io)
{
    uint32_t shards[TAB_SCAN];
    uint32_t n, i;
    int dropped = 0, skipped = 0;
    uint64_t bytes = *bytes_io;

    efs_client_table_lock();
    pthread_mutex_lock(&g_client.idx_mu);
    n = efs_export_tabs_by_age(&g_client.export, shards, TAB_SCAN);
    pthread_mutex_unlock(&g_client.idx_mu);
    efs_client_table_unlock();
    for (i = 0; i < n && dropped < TAB_EVICT && bytes > cap; i++) {
        efs_client_table_lock();
        pthread_mutex_lock(&g_client.idx_mu);
        pthread_mutex_lock(&g_client.dirty_mu);
        if (efs_export_tab_for_each_ino(&g_client.export, shards[i],
                                        tab_pin_cb, NULL) == 0) {
            pthread_mutex_lock(&g_lru_mu);
            (void)efs_export_tab_for_each_ino(&g_client.export, shards[i],
                                              tab_lru_cb, NULL);
            pthread_mutex_unlock(&g_lru_mu);
            if (efs_export_drop_tab(&g_client.export, shards[i]))
                dropped++;
            bytes = efs_export_staged_bytes(&g_client.export);
        } else {
            skipped++;
        }
        pthread_mutex_unlock(&g_client.dirty_mu);
        pthread_mutex_unlock(&g_client.idx_mu);
        efs_client_table_unlock();
    }
    if (dropped && getenv("EFS_STAGE_DBG"))
        fprintf(stderr,
                "stage-evict: tabs bytes=%lluMB cand=%u dropped=%d "
                "pinned=%d\n",
                (unsigned long long)(bytes >> 20), n, dropped, skipped);
    *bytes_io = bytes;
    return dropped;
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

        /* One staged-bytes reading per wake. A pinned oldest window is
         * skipped by at most a few bands; if those are also pinned, the
         * cursor is kept for the next wake instead of rescanning the
         * LRU hundreds of times in this one. */
        uint64_t bytes = stage_bytes_now();
        int evicted_total = 0;
        if (bytes > cap) {
            for (int bands = 0; bands < 4; bands++) {
                int evicted = 0;
                int band_full = 0;
                uint64_t band_max = 0;
                evict_pass(cap, &evicted, g_evict_cursor, &band_full,
                           &band_max, bytes);
                evicted_total += evicted;
                if (evicted > 0 || !band_full) {
                    if (evicted == 0)
                        g_evict_cursor = 0;
                    break;
                }
                g_evict_cursor = band_max;
            }
        } else {
            g_evict_cursor = 0;
        }
        if (evicted_total > 0)
            g_evict_cursor = 0;
        if (bytes > cap) {
            /* Rows alone cannot reach the cap once the tab floor is
             * above it (D18): drop the coldest unpinned tabs until the
             * reading is under, so the kick path goes quiet instead of
             * re-arming this thread on every staging op. */
            uint64_t after = stage_bytes_now();

            if (after > cap) {
                int tabs = evict_cold_tabs(cap, &after);

                evicted_total += tabs;
                bytes = after;
                __atomic_store_n(&g_stage_bytes_seen, after,
                                 __ATOMIC_RELAXED);
            }
        }
        if (bytes > cap && evicted_total == 0) {
            __atomic_store_n(&g_evict_idle_until, evict_now_ms() + 1000,
                             __ATOMIC_RELAXED);
            /* Every band we looked at is pinned (or the table is
             * already under the next reading). Log once and sleep. */
            if (!logged_grow && g_evict_cursor == 0) {
                logged_grow = 1;
                fprintf(stderr,
                        "efs-fuse: staging table over EFS_CLIENT_META_MB "
                        "(%llu MB) with nothing evictable; growing "
                        "(pinned data is real work, not cache)\n",
                        (unsigned long long)(cap >> 20));
                fflush(stderr);
            }
        }
        if (evicted_total > 0) {
            efs_client_table_lock();
            pthread_mutex_lock(&g_client.idx_mu);
            efs_export_compact(&g_client.export);
            pthread_mutex_unlock(&g_client.idx_mu);
            efs_client_table_unlock();
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
    if (evict_now_ms() <
        __atomic_load_n(&g_evict_idle_until, __ATOMIC_RELAXED))
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
        !efs_dcache_ino_pinned(ino) && !stage_ino_is_open(ino) &&
        !stage_op_pinned(ino) && !stage_ino_has_plock(ino)) {
        efs_export_forget_ino(&g_client.export, ino);
        pthread_mutex_lock(&g_lru_mu);
        lru_remove_locked(ino);
        pthread_mutex_unlock(&g_lru_mu);
    }
    pthread_mutex_unlock(&g_client.idx_mu);
    efs_client_table_unlock();
}
