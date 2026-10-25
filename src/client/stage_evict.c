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
#include "../common/stage_limit.h"

#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ---------------- LRU side table: ino -> last-touch tick ----------------
 * A node per staged ino on a doubly linked list in touch order (oldest
 * first), plus an open-addressing index ino -> node (power of two, 0 =
 * empty, backward-shift delete). Every access under g_lru_mu (a leaf lock).
 * Touch, remove and lookup are O(1); a pass reads its candidates from the
 * head. The previous table kept only ticks and every pass scanned all of
 * it under g_lru_mu for 256 candidates: with a crawler staging millions of
 * inos that scan ran back to back, every touch queued behind it and node9900
 * creates went from 11 ms to 207 ms (Oct 16 2026). Entries for unlinked rows
 * are removed when the row goes (eviction, ghost reclaim) or when a pass
 * finds them unstaged. */

struct lru_node {
    uint64_t ino;
    uint64_t tick;  /* touch tick (monotonic counter) */
    uint32_t prev, next; /* node 0 is the list sentinel; free nodes chain on next */
};

static pthread_mutex_t g_lru_mu = PTHREAD_MUTEX_INITIALIZER;
static struct lru_node *g_lru_nodes;
static uint32_t g_lru_ncap;   /* nodes allocated, sentinel included */
static uint32_t g_lru_nused;  /* nodes ever handed out, sentinel included */
static uint32_t g_lru_free;   /* free-list head, 0 = none */
static uint32_t *g_lru_slots; /* node index, 0 = empty */
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

int efs_client_stage_is_open(efs_ino_t ino)
{
    return g_pin_is_open_fn ? g_pin_is_open_fn(ino) : 0;
}

static int stage_ino_has_plock(efs_ino_t ino)
{
    return g_pin_has_plock_fn ? g_pin_has_plock_fn(ino) : 0;
}

static uint64_t lru_hash(uint64_t k)
{
    k ^= k >> 33;
    k *= UINT64_C(0xff51afd7ed558ccd);
    k ^= k >> 33;
    return k & g_lru_mask;
}

/* Caller holds g_lru_mu. Slot holding ino, or -1. */
static int64_t lru_slot_locked(efs_ino_t ino)
{
    if (!g_lru_slots)
        return -1;
    uint64_t i = lru_hash((uint64_t)ino);
    for (uint64_t n = 0; n <= g_lru_mask; n++) {
        uint32_t x = g_lru_slots[i];
        if (!x)
            return -1;
        if (g_lru_nodes[x].ino == (uint64_t)ino)
            return (int64_t)i;
        i = (i + 1) & g_lru_mask;
    }
    return -1;
}

static void lru_unlink(uint32_t x)
{
    struct lru_node *n = &g_lru_nodes[x];
    g_lru_nodes[n->prev].next = n->next;
    g_lru_nodes[n->next].prev = n->prev;
}

static void lru_link_tail(uint32_t x)
{
    struct lru_node *n = &g_lru_nodes[x];
    n->prev = g_lru_nodes[0].prev;
    n->next = 0;
    g_lru_nodes[n->prev].next = x;
    g_lru_nodes[0].prev = x;
}

/* Room for one more node and an index at most half full. */
static int lru_ensure(uint64_t need)
{
    if (!g_lru_nodes) {
        g_lru_nodes = calloc(1024, sizeof(*g_lru_nodes));
        if (!g_lru_nodes)
            return -1;
        g_lru_ncap = 1024;
        g_lru_nused = 1; /* sentinel: empty circular list */
    }
    if (!g_lru_free && g_lru_nused == g_lru_ncap) {
        if (g_lru_ncap > UINT32_MAX / 2)
            return -1;
        struct lru_node *nn =
            realloc(g_lru_nodes, (size_t)g_lru_ncap * 2 * sizeof(*nn));
        if (!nn)
            return -1;
        g_lru_nodes = nn;
        g_lru_ncap *= 2;
    }
    if (g_lru_slots && need * 2 <= g_lru_mask + 1)
        return 0;
    uint64_t cap = g_lru_mask ? g_lru_mask + 1 : 4096;
    while (cap < need * 4)
        cap *= 2;
    uint32_t *ns = calloc(cap, sizeof(*ns));
    if (!ns)
        return -1;
    free(g_lru_slots);
    g_lru_slots = ns;
    g_lru_mask = cap - 1;
    for (uint32_t x = g_lru_nodes[0].next; x; x = g_lru_nodes[x].next) {
        uint64_t j = lru_hash(g_lru_nodes[x].ino);
        while (g_lru_slots[j])
            j = (j + 1) & g_lru_mask;
        g_lru_slots[j] = x;
    }
    return 0;
}

/* Record a use of ino. Called from the staging/read/write paths; O(1)
 * under a leaf lock, safe with any other locks held. */
void efs_client_stage_touch(efs_ino_t ino)
{
    if (!ino)
        return;
    pthread_mutex_lock(&g_lru_mu);
    int64_t i = lru_slot_locked(ino);
    if (i >= 0) {
        uint32_t x = g_lru_slots[i];
        g_lru_nodes[x].tick = ++g_lru_clock;
        lru_unlink(x);
        lru_link_tail(x);
    } else if (lru_ensure(g_lru_count + 1) == 0) {
        uint32_t x = g_lru_free;
        if (x)
            g_lru_free = g_lru_nodes[x].next;
        else
            x = g_lru_nused++;
        g_lru_nodes[x].ino = (uint64_t)ino;
        g_lru_nodes[x].tick = ++g_lru_clock;
        lru_link_tail(x);
        uint64_t j = lru_hash((uint64_t)ino);
        while (g_lru_slots[j])
            j = (j + 1) & g_lru_mask;
        g_lru_slots[j] = x;
        g_lru_count++;
    }
    pthread_mutex_unlock(&g_lru_mu);
}

/* Caller holds g_lru_mu. */
static void lru_remove_locked(efs_ino_t ino)
{
    int64_t s = lru_slot_locked(ino);
    if (s < 0)
        return;
    uint64_t i = (uint64_t)s;
    uint32_t x = g_lru_slots[i];
    lru_unlink(x);
    g_lru_nodes[x].ino = 0;
    g_lru_nodes[x].next = g_lru_free;
    g_lru_free = x;
    g_lru_count--;
    /* Backward-shift delete (same scheme as the dcache pin table). */
    g_lru_slots[i] = 0;
    uint64_t j = (i + 1) & g_lru_mask;
    while (g_lru_slots[j]) {
        uint64_t h = lru_hash(g_lru_nodes[g_lru_slots[j]].ino);
        int in_gap = (i < j) ? (h > i && h <= j) : (h > i || h <= j);
        if (!in_gap) {
            g_lru_slots[i] = g_lru_slots[j];
            g_lru_slots[j] = 0;
            i = j;
        }
        j = (j + 1) & g_lru_mask;
    }
}

/* Caller holds g_lru_mu. Tick of a listed ino; 0 if not listed. */
static uint64_t lru_tick_locked(efs_ino_t ino)
{
    int64_t i = lru_slot_locked(ino);
    return i < 0 ? 0 : g_lru_nodes[g_lru_slots[i]].tick;
}

/* Copy up to max of the oldest entries, oldest first. O(max). */
static uint32_t lru_oldest(struct lru_node *out, uint32_t max)
{
    uint32_t n = 0;
    pthread_mutex_lock(&g_lru_mu);
    if (g_lru_nodes)
        for (uint32_t x = g_lru_nodes[0].next; x && n < max;
             x = g_lru_nodes[x].next)
            out[n++] = g_lru_nodes[x];
    pthread_mutex_unlock(&g_lru_mu);
    return n;
}

/* Second chance for a pinned candidate: move it to the newest end unless
 * a touch already did. Pinned rows are in use; leaving them at the head
 * made every pass re-read the same pinned prefix. */
static void lru_requeue(efs_ino_t ino, uint64_t tick)
{
    pthread_mutex_lock(&g_lru_mu);
    int64_t i = lru_slot_locked(ino);
    if (i >= 0) {
        uint32_t x = g_lru_slots[i];
        if (g_lru_nodes[x].tick == tick) {
            g_lru_nodes[x].tick = ++g_lru_clock;
            lru_unlink(x);
            lru_link_tail(x);
        }
    }
    pthread_mutex_unlock(&g_lru_mu);
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


/* A pass reads the SCAN_BATCH oldest entries off the list head and evicts
 * up to EVICT_BATCH unpinned ones; pinned ones go to the newest end. A
 * wake runs passes until the reading is under the low watermark (7/8 of
 * the cap), nothing is left to evict, or WAKE_BUDGET_MS of work is spent.
 * evict_one takes and drops its locks per ino, so FUSE ops interleave. */
#define EVICT_BATCH 64
#define SCAN_BATCH  256
#define WAKE_BUDGET_MS 20
/* Rows dropped between compactions within one wake: removal leaves slab
 * capacity counted until compaction, so the reading lags without it. */
#define COMPACT_EVERY 4096

/* Pin check WITHOUT the table lock (cheap pre-filter). The authoritative
 * re-check happens under the locks in evict_one(). */
static int stage_pinned(efs_ino_t ino)
{
    if (efs_client_ino_is_dirty(ino))
        return 1;
    if (efs_dcache_ino_pinned(ino))
        return 1;
    if (efs_client_stage_is_open(ino))
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
        efs_client_stage_is_open(ino) || stage_op_pinned(ino) ||
        stage_ino_has_plock(ino))
        goto out;
    pthread_mutex_lock(&g_lru_mu);
    uint64_t cur_tick = lru_tick_locked(ino);
    if (cur_tick && cur_tick != tick) {
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

/* Staged-bytes reading under the table lock, published for the kick path. */
static uint64_t stage_bytes_now(void)
{
    uint64_t bytes;
    efs_client_table_lock();
    bytes = efs_export_staged_bytes(&g_client.export);
    efs_client_table_unlock();
    __atomic_store_n(&g_stage_bytes_seen, bytes, __ATOMIC_RELAXED);
    return bytes;
}

/* One pass over the SCAN_BATCH oldest entries. Returns the number dropped;
 * *requeued counts pinned candidates moved to the newest end. */
static int evict_pass(int *requeued)
{
    /* W143: read once, per-op path. Any thread may be first: the static is
     * read and set atomically and the function uses its local copy (TSan). */
    static int env_stage_dbg_s = -1;
    int env_stage_dbg = __atomic_load_n(&env_stage_dbg_s, __ATOMIC_ACQUIRE);
    if (env_stage_dbg < 0) {
        env_stage_dbg = getenv("EFS_STAGE_DBG") != NULL;
        __atomic_store_n(&env_stage_dbg_s, env_stage_dbg, __ATOMIC_RELEASE);
    }
    static struct lru_node cand[SCAN_BATCH];
    uint32_t nc = lru_oldest(cand, SCAN_BATCH);
    int evicted = 0;

    *requeued = 0;
    for (uint32_t i = 0; i < nc && evicted < EVICT_BATCH; i++) {
        if (stage_pinned((efs_ino_t)cand[i].ino)) {
            lru_requeue((efs_ino_t)cand[i].ino, cand[i].tick);
            (*requeued)++;
            continue;
        }
        if (evict_one((efs_ino_t)cand[i].ino, cand[i].tick))
            evicted++;
    }
    if (evicted && env_stage_dbg)
        fprintf(stderr, "stage-evict: pass cand=%u evicted=%d requeued=%d\n",
                nc, evicted, *requeued);
    return evicted;
}

static void stage_compact(void)
{
    efs_client_table_lock();
    pthread_mutex_lock(&g_client.idx_mu);
    efs_export_compact(&g_client.export);
    pthread_mutex_unlock(&g_client.idx_mu);
    efs_client_table_unlock();
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
        efs_client_stage_is_open(ino) || stage_op_pinned(ino) ||
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
    /* W143: read once, per-op path. Any thread may be first: the static is
     * read and set atomically and the function uses its local copy (TSan). */
    static int env_stage_dbg_s = -1;
    int env_stage_dbg = __atomic_load_n(&env_stage_dbg_s, __ATOMIC_ACQUIRE);
    if (env_stage_dbg < 0) {
        env_stage_dbg = getenv("EFS_STAGE_DBG") != NULL;
        __atomic_store_n(&env_stage_dbg_s, env_stage_dbg, __ATOMIC_RELEASE);
    }
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
    if (dropped && env_stage_dbg)
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
    uint64_t cap = efs_stage_cap_bytes();
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

        /* Over the cap: drain rows oldest first down to the low
         * watermark, within a time budget per wake. */
        uint64_t bytes = stage_bytes_now();
        uint64_t low = cap - cap / 8;
        int evicted_total = 0, since_compact = 0;
        if (bytes > cap) {
            int64_t deadline = evict_now_ms() + WAKE_BUDGET_MS;
            uint64_t visited = 0, listed;
            pthread_mutex_lock(&g_lru_mu);
            listed = g_lru_count;
            pthread_mutex_unlock(&g_lru_mu);
            while (bytes > low && visited < listed &&
                   evict_now_ms() < deadline) {
                int requeued = 0;
                int evicted = evict_pass(&requeued);
                if (!evicted && !requeued)
                    break;
                visited += (uint64_t)evicted + (uint64_t)requeued;
                evicted_total += evicted;
                since_compact += evicted;
                if (since_compact >= COMPACT_EVERY) {
                    stage_compact();
                    since_compact = 0;
                }
                bytes = stage_bytes_now();
            }
            if (since_compact) {
                stage_compact();
                since_compact = 0;
                bytes = stage_bytes_now();
            }
        }
        if (bytes > cap) {
            /* Rows alone cannot reach the cap once the tab floor is
             * above it (D18): drop the coldest unpinned tabs until the
             * reading is under, so the kick path goes quiet instead of
             * re-arming this thread on every staging op. */
            int tabs = evict_cold_tabs(low, &bytes);

            evicted_total += tabs;
            if (tabs) {
                stage_compact();
                bytes = stage_bytes_now();
            }
            __atomic_store_n(&g_stage_bytes_seen, bytes, __ATOMIC_RELAXED);
        }
        if (bytes <= cap)
            logged_grow = 0;
        if (bytes > cap && evicted_total == 0) {
            __atomic_store_n(&g_evict_idle_until, evict_now_ms() + 1000,
                             __ATOMIC_RELAXED);
            /* Everything we looked at is pinned. Log once per episode
             * and sleep. */
            if (!logged_grow) {
                logged_grow = 1;
                fprintf(stderr,
                        "efs-fuse: staging table over EFS_CLIENT_META_MB "
                        "(%llu MB) with nothing evictable; growing "
                        "(pinned data is real work, not cache)\n",
                        (unsigned long long)(cap >> 20));
                fflush(stderr);
            }
        } else if (bytes > cap) {
            /* Progress but still over: run again after a pause as long
             * as the budget, so FUSE ops get the table locks at least
             * half the time. */
            pthread_mutex_lock(&g_evict_mu);
            g_evict_kick = 1;
            pthread_mutex_unlock(&g_evict_mu);
            struct timespec pause = {0, WAKE_BUDGET_MS * 1000000L};
            nanosleep(&pause, NULL);
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
        efs_stage_cap_bytes())
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
        !efs_dcache_ino_pinned(ino) && !efs_client_stage_is_open(ino) &&
        !stage_op_pinned(ino) && !stage_ino_has_plock(ino)) {
        efs_export_forget_ino(&g_client.export, ino);
        pthread_mutex_lock(&g_lru_mu);
        lru_remove_locked(ino);
        pthread_mutex_unlock(&g_lru_mu);
    }
    pthread_mutex_unlock(&g_client.idx_mu);
    efs_client_table_unlock();
}
