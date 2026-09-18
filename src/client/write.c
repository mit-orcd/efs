#include "client_internal.h"
#include "efs/protocol.h"
#include "efs/network.h"
#include "efs/erasure.h"
#include "efs/checksum.h"
#include "efs/placement.h"
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include <poll.h>
#include <pthread.h>
#include <sys/stat.h>

static uint32_t data_chunk_size(void)
{
    uint32_t cs = g_client.export.chunk_size;
    return efs_chunk_size_valid(cs) ? cs : EFS_DEFAULT_CHUNK_SIZE;
}

static uint32_t data_frag_size(void)
{
    return efs_frag_size(data_chunk_size());
}

static void frag_ptrs(uint8_t *buf, uint32_t frag_len, uint8_t *frags[EFS_NUM_FRAGMENTS])
{
    for (int i = 0; i < EFS_NUM_FRAGMENTS; i++)
        frags[i] = buf + (size_t)i * frag_len;
}

/* chunk_keys / chunks realloc under set/reserve. Lookups must hold idx_mu. */
static int export_chunk_exists(efs_ino_t ino, uint32_t ci)
{
    pthread_mutex_lock(&g_client.idx_mu);
    int ok = (efs_export_get_chunk(&g_client.export, ino, ci, NULL) == 0);
    pthread_mutex_unlock(&g_client.idx_mu);
    return ok;
}

static int export_chunk_copy(efs_ino_t ino, uint32_t ci, struct efs_chunk_entry *out)
{
    pthread_mutex_lock(&g_client.idx_mu);
    int rc = efs_export_get_chunk(&g_client.export, ino, ci, out);
    pthread_mutex_unlock(&g_client.idx_mu);
    return rc;
}

static void export_reserve_chunks_locked(uint64_t extra)
{
    efs_client_table_lock();
    pthread_mutex_lock(&g_client.idx_mu);
    (void)efs_export_reserve_chunks(&g_client.export, extra);
    pthread_mutex_unlock(&g_client.idx_mu);
    efs_client_table_unlock();
}

static void now_ns(uint64_t *sec, uint32_t *nsec)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    *sec = (uint64_t)ts.tv_sec;
    *nsec = (uint32_t)ts.tv_nsec;
}

static uint64_t pack_dirty_chunk(efs_ino_t ino, uint32_t chunk_index)
{
    uint64_t k = ino ^ ((uint64_t)chunk_index * 0x9E3779B97F4A7C15ULL);
    return k ? k : 1;
}

static int dirty_set_ensure(uint64_t **keys, uint64_t *mask, uint64_t count)
{
    if (*keys && count * 2 <= *mask + 1)
        return 0;
    uint64_t old_mask = *mask;
    uint64_t *old_keys = *keys;
    uint64_t cap = 16;
    while (cap < (count ? count * 4 : 16))
        cap *= 2;
    uint64_t *nk = calloc(cap, sizeof(uint64_t));
    if (!nk)
        return -1;
    *keys = nk;
    *mask = cap - 1;
    if (old_keys) {
        for (uint64_t i = 0; i <= old_mask; i++) {
            uint64_t k = old_keys[i];
            if (!k)
                continue;
            uint64_t j = k & *mask;
            while ((*keys)[j] != 0)
                j = (j + 1) & *mask;
            (*keys)[j] = k;
        }
        free(old_keys);
    }
    return 0;
}

static void dirty_set_put(uint64_t *keys, uint64_t mask, uint64_t key, uint64_t *count)
{
    if (!keys || mask == 0 || key == 0)
        return;
    uint64_t i = key & mask;
    for (uint64_t n = 0; n <= mask; n++) {
        if (keys[i] == 0) {
            keys[i] = key;
            (*count)++;
            return;
        }
        if (keys[i] == key)
            return;
        i = (i + 1) & mask;
    }
}

static void dirty_sets_clear(void)
{
    if (g_client.dirty_ino_keys && g_client.dirty_ino_mask)
        memset(g_client.dirty_ino_keys, 0,
               (g_client.dirty_ino_mask + 1) * sizeof(uint64_t));
    if (g_client.dirty_chunk_keys && g_client.dirty_chunk_mask)
        memset(g_client.dirty_chunk_keys, 0,
               (g_client.dirty_chunk_mask + 1) * sizeof(uint64_t));
    g_client.dirty_ino_count = 0;
    g_client.dirty_chunk_count = 0;
}

static int dirty_set_has(const uint64_t *keys, uint64_t mask, efs_ino_t ino)
{
    if (!keys || !mask)
        return 0;
    uint64_t i = (uint64_t)ino & mask;
    for (uint64_t n = 0; n <= mask; n++) {
        if (keys[i] == 0)
            return 0;
        if (keys[i] == (uint64_t)ino)
            return 1;
        i = (i + 1) & mask;
    }
    return 0;
}

/* The dirty set a REPORT is currently publishing. dirty_snap_save_locked
 * detaches the live set the moment a report starts, so without this an inode
 * looks clean from snapshot until the owner has actually applied the size --
 * and lookup_walk would then serve the owner's older size while this client
 * still holds the newer one. report_mu serializes reports, so there is at
 * most one of these at a time. Guarded by g_client.dirty_mu. */
static uint64_t *pub_ino_keys;
static uint64_t pub_ino_mask;

/* Drop the alias. Must run before the snapshot array is freed or merged back
 * on every exit path, or ino_is_dirty reads freed memory. */
static void pub_ino_clear(void)
{
    pthread_mutex_lock(&g_client.dirty_mu);
    pub_ino_keys = NULL;
    pub_ino_mask = 0;
    pthread_mutex_unlock(&g_client.dirty_mu);
}

int efs_client_ino_is_dirty(efs_ino_t ino)
{
    if (!ino)
        return 0;
    pthread_mutex_lock(&g_client.dirty_mu);
    int hit = efs_client_ino_is_dirty_locked(ino);
    pthread_mutex_unlock(&g_client.dirty_mu);
    return hit;
}

/* Caller holds g_client.dirty_mu. Used by the staging-table evictor, whose
 * eviction commit already holds dirty_mu (client-cache design Part A). */
int efs_client_ino_is_dirty_locked(efs_ino_t ino)
{
    return dirty_set_has(g_client.dirty_ino_keys, g_client.dirty_ino_mask,
                         ino) ||
           dirty_set_has(pub_ino_keys, pub_ino_mask, ino);
}

#define MTIME_PIN_MAX 256
static efs_ino_t mtime_pin[MTIME_PIN_MAX];
static int mtime_pin_n;

void efs_client_mtime_pin(efs_ino_t ino)
{
    if (!ino)
        return;
    pthread_mutex_lock(&g_client.dirty_mu);
    for (int i = 0; i < mtime_pin_n; i++) {
        if (mtime_pin[i] == ino) {
            pthread_mutex_unlock(&g_client.dirty_mu);
            return;
        }
    }
    if (mtime_pin_n < MTIME_PIN_MAX)
        mtime_pin[mtime_pin_n++] = ino;
    pthread_mutex_unlock(&g_client.dirty_mu);
}

void efs_client_mtime_unpin(efs_ino_t ino)
{
    if (!ino)
        return;
    pthread_mutex_lock(&g_client.dirty_mu);
    for (int i = 0; i < mtime_pin_n; i++) {
        if (mtime_pin[i] == ino) {
            mtime_pin[i] = mtime_pin[mtime_pin_n - 1];
            mtime_pin_n--;
            break;
        }
    }
    pthread_mutex_unlock(&g_client.dirty_mu);
}

int efs_client_mtime_is_pinned(efs_ino_t ino)
{
    if (!ino)
        return 0;
    pthread_mutex_lock(&g_client.dirty_mu);
    int hit = 0;
    for (int i = 0; i < mtime_pin_n; i++) {
        if (mtime_pin[i] == ino) {
            hit = 1;
            break;
        }
    }
    pthread_mutex_unlock(&g_client.dirty_mu);
    return hit;
}

void efs_client_mark_ino_dirty(efs_ino_t ino)
{
    if (!g_client.meta_batch || ino == 0)
        return;
    efs_client_ensure_dir_locks();
    pthread_mutex_lock(&g_client.dirty_mu);
    if (dirty_set_ensure(&g_client.dirty_ino_keys, &g_client.dirty_ino_mask,
                         g_client.dirty_ino_count + 1) != 0) {
        pthread_mutex_unlock(&g_client.dirty_mu);
        return;
    }
    dirty_set_put(g_client.dirty_ino_keys, g_client.dirty_ino_mask, ino,
                  &g_client.dirty_ino_count);
    int stripe = (int)((uint64_t)ino % EFS_DIR_LOCKS);
    g_client.last_dirty_stripe = stripe;
    g_client.dirty_stripe_ops[stripe]++;
    pthread_mutex_unlock(&g_client.dirty_mu);
}

void efs_client_mark_chunk_dirty(efs_ino_t ino, uint32_t chunk_index)
{
    if (!g_client.meta_batch || ino == 0)
        return;
    efs_client_ensure_dir_locks();
    pthread_mutex_lock(&g_client.dirty_mu);
    uint64_t before = 0;
    if (g_client.dirty_chunk_keys && g_client.dirty_chunk_mask) {
        uint64_t key = pack_dirty_chunk(ino, chunk_index);
        uint64_t i = key & g_client.dirty_chunk_mask;
        for (uint64_t n = 0; n <= g_client.dirty_chunk_mask; n++) {
            if (g_client.dirty_chunk_keys[i] == 0)
                break;
            if (g_client.dirty_chunk_keys[i] == key)
                goto out;
            i = (i + 1) & g_client.dirty_chunk_mask;
        }
    }
    before = g_client.dirty_chunk_count;
    if (dirty_set_ensure(&g_client.dirty_chunk_keys, &g_client.dirty_chunk_mask,
                         before + 1) != 0)
        goto out;
    dirty_set_put(g_client.dirty_chunk_keys, g_client.dirty_chunk_mask,
                  pack_dirty_chunk(ino, chunk_index), &g_client.dirty_chunk_count);
    if (g_client.dirty_chunk_count == before)
        goto out;
    if (g_client.dirty_chunk_count > g_client.dirty_chunk_cap) {
        uint64_t ncap = g_client.dirty_chunk_cap ? g_client.dirty_chunk_cap * 2 : 64;
        efs_ino_t *ni = realloc(g_client.dirty_chunk_inos, ncap * sizeof(efs_ino_t));
        uint32_t *nx = realloc(g_client.dirty_chunk_idxs, ncap * sizeof(uint32_t));
        if (!ni || !nx) {
            free(ni);
            free(nx);
            goto out;
        }
        g_client.dirty_chunk_inos = ni;
        g_client.dirty_chunk_idxs = nx;
        g_client.dirty_chunk_cap = ncap;
    }
    uint64_t slot = g_client.dirty_chunk_count - 1;
    g_client.dirty_chunk_inos[slot] = ino;
    g_client.dirty_chunk_idxs[slot] = chunk_index;
out:
    pthread_mutex_unlock(&g_client.dirty_mu);
}

/* The client-driven metadata flush (flush_snapshot / send_meta_root /
 * META_FLUSH_BEGIN election / take_write_lease) was removed: the server is
 * the sole metadata writer (Phase 2b REPORT_CHUNKS + server-side flush with
 * two-phase root commit). A client that builds and PUT_METAs a root is a
 * rogue second writer — concurrent client heal flushes collided on the same
 * gen/next_ci and tore the CoW pages (the shard-0 "gen raced with GC"
 * wedge). */

/* fsync waiters must not serialize on one flush: sync_meta shares one
 * REPORT_CHUNKS barrier across all waiters via g_sync_mu/g_sync_meta_cv. */
static pthread_mutex_t g_sync_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_sync_meta_cv = PTHREAD_COND_INITIALIZER;
static int g_sync_meta_active;
static int g_sync_meta_rc;

/* Dirty state swapped out of g_client at snapshot time. Ops that race the
 * unlocked serialize populate fresh sets and stay dirty for the next flush;
 * on flush failure the saved marks are merged back so nothing is lost. */
struct dirty_snap {
    uint64_t *ino_keys, ino_mask, ino_count;
    uint64_t *chunk_keys, chunk_mask, chunk_count;
    efs_ino_t *chunk_inos;
    uint32_t *chunk_idxs;
    uint64_t chunk_cap;
    uint64_t *ino_slots; /* snapshot-time inode array slots */
    uint64_t *chunk_slots;
    uint64_t layout_epoch;
    uint64_t snap_icount, snap_ccount;
    int meta_dirty;
    uint32_t dirty_ops;
    /* Packed under the table lock from the live export (no full-table copy). */
    char *packed;
    size_t packed_len;
    uint32_t packed_ino_len, packed_ch_len;
    uint8_t *ino_dirty_pg, *ch_dirty_pg;
    int packed_is_cache;
};

static void dirty_snap_save_locked(struct dirty_snap *ds)
{
    ds->ino_keys = g_client.dirty_ino_keys;
    ds->ino_mask = g_client.dirty_ino_mask;
    ds->ino_count = g_client.dirty_ino_count;
    ds->chunk_keys = g_client.dirty_chunk_keys;
    ds->chunk_mask = g_client.dirty_chunk_mask;
    ds->chunk_count = g_client.dirty_chunk_count;
    ds->chunk_inos = g_client.dirty_chunk_inos;
    ds->chunk_idxs = g_client.dirty_chunk_idxs;
    ds->chunk_cap = g_client.dirty_chunk_cap;
    ds->meta_dirty = g_client.meta_dirty;
    ds->dirty_ops = g_client.meta_dirty_ops;
    g_client.dirty_ino_keys = NULL;
    g_client.dirty_ino_mask = 0;
    g_client.dirty_ino_count = 0;
    g_client.dirty_chunk_keys = NULL;
    g_client.dirty_chunk_mask = 0;
    g_client.dirty_chunk_count = 0;
    g_client.dirty_chunk_inos = NULL;
    g_client.dirty_chunk_idxs = NULL;
    g_client.dirty_chunk_cap = 0;
    g_client.meta_dirty = 0;
    g_client.meta_dirty_ops = 0;
    ds->ino_slots = NULL;
    ds->chunk_slots = NULL;
    ds->layout_epoch = g_client.export.layout_epoch;
    ds->snap_icount = g_client.export.inode_count;
    ds->snap_ccount = g_client.export.chunk_count;
    if (ds->ino_count) {
        ds->ino_slots = malloc(ds->ino_count * sizeof(uint64_t));
        uint64_t n = 0;
        if (ds->ino_slots && ds->ino_keys) {
            for (uint64_t i = 0; i <= ds->ino_mask && n < ds->ino_count; i++) {
                if (!ds->ino_keys[i])
                    continue;
                uint64_t slot = UINT64_MAX;
                (void)efs_export_inode_slot(&g_client.export, ds->ino_keys[i],
                                            &slot);
                ds->ino_slots[n++] = slot;
            }
        }
    }
    if (ds->chunk_count) {
        ds->chunk_slots = malloc(ds->chunk_count * sizeof(uint64_t));
        if (ds->chunk_slots) {
            for (uint64_t i = 0; i < ds->chunk_count; i++) {
                uint64_t slot = UINT64_MAX;
                (void)efs_export_chunk_slot(&g_client.export, ds->chunk_inos[i],
                                            ds->chunk_idxs[i], &slot);
                ds->chunk_slots[i] = slot;
            }
        }
    }
}

static void dirty_snap_free(struct dirty_snap *ds)
{
    free(ds->ino_keys);
    free(ds->chunk_keys);
    free(ds->chunk_inos);
    free(ds->chunk_idxs);
    free(ds->ino_slots);
    free(ds->chunk_slots);
    if (!ds->packed_is_cache)
        free(ds->packed);
    free(ds->ino_dirty_pg);
    free(ds->ch_dirty_pg);
    memset(ds, 0, sizeof(*ds));
}

/* Flush failed: re-mark everything the snapshot covered so the next flush
 * retries it. g_client.lock must be held. */
static void dirty_snap_merge_back_locked(struct dirty_snap *ds)
{
    for (uint64_t i = 0; i <= ds->ino_mask; i++) {
        if (ds->ino_keys && ds->ino_keys[i])
            efs_client_mark_ino_dirty(ds->ino_keys[i]);
    }
    for (uint64_t i = 0; i < ds->chunk_count; i++)
        efs_client_mark_chunk_dirty(ds->chunk_inos[i], ds->chunk_idxs[i]);
    g_client.meta_dirty |= ds->meta_dirty;
    g_client.meta_dirty_ops += ds->dirty_ops;
    dirty_snap_free(ds);
}

/* Phase 2b: report the dirty set to the metadata primary (chunk mappings +
 * inode size/mtime) instead of blob-flushing the whole table. The primary
 * applies them to its in-memory table; the server meta-flush thread persists
 * them (or, for sync=1, commits before replying — the fsync durability
 * barrier). On success the dirty set is dropped; on failure it is merged back
 * so the next flush retries. The dirty set / rebase machinery is unchanged —
 * only the flush mechanism (blob PUT -> targeted RPC) differs. */
int efs_client_report_dirty(int sync)
{
    return efs_client_report_dirty_ino(0, sync);
}

/* only_ino=0 reports the whole dirty set. A per-file fsync passes the ino
 * so 9 concurrent 2g jobs do not build one 147k-rec REPORT (that times
 * out as NET). Other inodes stay dirty. */
int efs_client_report_dirty_ino(efs_ino_t only_ino, int sync)
{
    static pthread_mutex_t report_mu = PTHREAD_MUTEX_INITIALIZER;
    struct dirty_snap ds;
    memset(&ds, 0, sizeof(ds));

    /* Close kicks REPORT on the flush thread. setattr (utimens /
     * truncate) drains first so a late rec cannot undo SETATTR. */
    pthread_mutex_lock(&report_mu);

    efs_client_table_lock();
    efs_client_ensure_dir_locks();
    pthread_mutex_lock(&g_client.dirty_mu);
    if (g_client.dirty_ino_count == 0 && g_client.dirty_chunk_count == 0 &&
        !g_client.meta_dirty && !sync) {
        pthread_mutex_unlock(&g_client.dirty_mu);
        efs_client_table_unlock();
        pthread_mutex_unlock(&report_mu);
        return EFS_OK;
    }
    dirty_snap_save_locked(&ds);
    /* Keep these inodes reading as dirty until the owner has the size. */
    pub_ino_keys = ds.ino_keys;
    pub_ino_mask = ds.ino_mask;
    pthread_mutex_unlock(&g_client.dirty_mu);

    /* Build chunk + inode size/mtime recs from the live table (still holding
     * the table lock so the entries are consistent). */
    struct efs_chunk_rec *crecs = NULL;
    struct efs_ino_size_rec *irecs = NULL;
    uint32_t cn = 0, in = 0;
    /* calloc: flags/padding must be zero on the wire. */
    if (ds.chunk_count)
        crecs = calloc(ds.chunk_count, sizeof(*crecs));
    if (ds.ino_count)
        irecs = calloc(ds.ino_count, sizeof(*irecs));
    if ((ds.chunk_count && !crecs) || (ds.ino_count && !irecs)) {
        free(crecs);
        free(irecs);
        pub_ino_clear();
        /* table lock held (not dirty_mu — merge_back takes it internally). */
        dirty_snap_merge_back_locked(&ds);
        efs_client_table_unlock();
        pthread_mutex_unlock(&report_mu);
        return EFS_ERR_NOMEM;
    }
    if (only_ino) {
        for (uint64_t i = 0; i <= ds.ino_mask; i++) {
            if (ds.ino_keys && ds.ino_keys[i] && ds.ino_keys[i] != only_ino) {
                efs_client_mark_ino_dirty(ds.ino_keys[i]);
                ds.ino_keys[i] = 0;
                if (ds.ino_count)
                    ds.ino_count--;
            }
        }
        for (uint64_t i = 0; i < ds.chunk_count; i++) {
            if (ds.chunk_inos[i] && ds.chunk_inos[i] != only_ino) {
                efs_client_mark_chunk_dirty(ds.chunk_inos[i], ds.chunk_idxs[i]);
                ds.chunk_inos[i] = 0;
            }
        }
    }
    for (uint64_t i = 0; i < ds.chunk_count; i++) {
        struct efs_chunk_entry ce;
        if (!ds.chunk_inos[i])
            continue;
        if (only_ino && ds.chunk_inos[i] != only_ino)
            continue;
        if (efs_export_get_chunk(&g_client.export, ds.chunk_inos[i],
                                 ds.chunk_idxs[i], &ce) != 0)
            continue; /* truncated away before the report; skip */
        crecs[cn].ino = ds.chunk_inos[i];
        crecs[cn].chunk_index = ds.chunk_idxs[i];
        memcpy(crecs[cn].nodes, ce.fragment_nodes, sizeof(crecs[cn].nodes));
        memcpy(crecs[cn].checksums, ce.checksums, sizeof(crecs[cn].checksums));
        cn++;
    }
    for (uint64_t i = 0; i <= ds.ino_mask; i++) {
        if (!ds.ino_keys || !ds.ino_keys[i])
            continue;
        if (only_ino && ds.ino_keys[i] != only_ino)
            continue;
        struct efs_inode inode;
        if (efs_export_get_inode(&g_client.export, ds.ino_keys[i], &inode) != 0)
            continue; /* unlinked before the report; skip */
        irecs[in].ino = ds.ino_keys[i];
        irecs[in].size = inode.size;
        irecs[in].mtime = inode.mtime;
        irecs[in].mtime_nsec = inode.mtime_nsec;
        if (efs_client_mtime_is_pinned(ds.ino_keys[i]))
            irecs[in].flags = EFS_INO_REC_F_TIMES;
        irecs[in].atime = inode.atime;
        /* pack fields stay zero: packing is gone with the old engine. */
        in++;
    }
    efs_client_table_unlock();

    /* Send the report as ONE batch to a dual-host voter (a node that is a
     * voter of every metadata group, so it can apply the whole batch
     * locally). NOT_PRIMARY follows the hint; transient NET/NO_QUORUM/BUSY
     * retried a few times. On failure the whole snap is merged back. */
    int rc = EFS_ERR_NET;
    for (int attempt = 0; attempt < 4; attempt++) {
        rc = efs_client_rpc_report_dirty_raft(g_client.export_id, crecs,
                                              cn, irecs, in, sync);
        if (rc == EFS_OK)
            break;
        if (rc != EFS_ERR_NET && rc != EFS_ERR_NO_QUORUM &&
            rc != EFS_ERR_NOT_PRIMARY && rc != EFS_ERR_BUSY)
            break;
        usleep(50000u << attempt);
    }
    free(crecs);
    free(irecs);
    /* The owner now has these sizes (or the snapshot is about to be merged
     * back into the live set), so stop reporting them as in-flight. */
    pub_ino_clear();
    if (rc == EFS_OK) {
        dirty_snap_free(&ds);
    } else {
        efs_client_table_lock();
        dirty_snap_merge_back_locked(&ds);
        efs_client_table_unlock();
        g_client.report_flush_failed = 1;
    }
    pthread_mutex_unlock(&report_mu);
    return rc;
}

/* fsync: one incremental publish shared by all waiters. 8× end_fsync used
 * to each run a full/incremental flush (5–18s) because every caller took
 * g_repl_mu and found new dirty from the others still writing. */
int efs_client_sync_meta(void)
{
    const char *skip = getenv("EFS_SKIP_META_FLUSH");
    if (skip && *skip && strcmp(skip, "0") != 0)
        return EFS_OK;
    pthread_mutex_lock(&g_sync_mu);
    int waited = 0;
    for (;;) {
        if (!g_sync_meta_active) {
            if (waited) {
                pthread_mutex_lock(&g_client.dirty_mu);
                int clean = (g_client.dirty_ino_count == 0 &&
                             g_client.dirty_chunk_count == 0 &&
                             !g_client.meta_dirty);
                pthread_mutex_unlock(&g_client.dirty_mu);
                if (clean) {
                    int rc = g_sync_meta_rc;
                    pthread_mutex_unlock(&g_sync_mu);
                    return rc;
                }
            }
            g_sync_meta_active = 1;
            pthread_mutex_unlock(&g_sync_mu);
            /* Phase 2b fsync barrier: report dirty state and have the primary
             * commit the export before replying. */
            int rc = efs_client_report_dirty(1);
            pthread_mutex_lock(&g_sync_mu);
            g_sync_meta_rc = rc;
            g_sync_meta_active = 0;
            pthread_cond_broadcast(&g_sync_meta_cv);
            pthread_mutex_unlock(&g_sync_mu);
            return rc;
        }
        waited = 1;
        pthread_cond_wait(&g_sync_meta_cv, &g_sync_mu);
    }
}



/* Flush-thread main: waits for threshold hits and runs blocking flushes off
 * the FUSE worker threads. g_repl_mu still serializes against forced flushes
 * (fsync/unmount) from the op path. */
static void *meta_flush_main(void *arg)
{
    (void)arg;
    pthread_mutex_lock(&g_client.meta_flush_mu);
    for (;;) {
        while (!g_client.meta_flush_req && !g_client.meta_flush_stop)
            pthread_cond_wait(&g_client.meta_flush_cv,
                              &g_client.meta_flush_mu);
        if (g_client.meta_flush_stop)
            break;
        g_client.meta_flush_req = 0;
        pthread_mutex_unlock(&g_client.meta_flush_mu);
        /* Phase 2b: background threshold flush reports dirty state via RPC. */
        (void)efs_client_report_dirty(0);
        pthread_mutex_lock(&g_client.meta_flush_mu);
    }
    pthread_mutex_unlock(&g_client.meta_flush_mu);
    return NULL;
}

void efs_client_enable_meta_batch(uint32_t every_n_ops)
{
    g_client.meta_batch = 1;
    g_client.meta_batch_ops = every_n_ops ? every_n_ops : 4096;
    g_client.meta_dirty = 0;
    g_client.meta_dirty_ops = 0;
    dirty_sets_clear();

    if (!g_client.meta_flush_started) {
        pthread_mutex_init(&g_client.meta_flush_mu, NULL);
        pthread_cond_init(&g_client.meta_flush_cv, NULL);
        g_client.meta_flush_req = 0;
        g_client.meta_flush_stop = 0;
        if (pthread_create(&g_client.meta_flush_tid, NULL, meta_flush_main,
                           NULL) == 0)
            g_client.meta_flush_started = 1;
    }

    /* Bound the staging table (client-cache design Part A). */
    efs_client_stage_evict_start();
}



void efs_client_stop_meta_flush(void)
{
    if (!g_client.meta_flush_started)
        return;
    pthread_mutex_lock(&g_client.meta_flush_mu);
    g_client.meta_flush_stop = 1;
    pthread_cond_signal(&g_client.meta_flush_cv);
    pthread_mutex_unlock(&g_client.meta_flush_mu);
    pthread_join(g_client.meta_flush_tid, NULL);
    g_client.meta_flush_started = 0;
}

void efs_client_kick_meta_flush(void)
{
    if (g_client.meta_flush_started) {
        pthread_mutex_lock(&g_client.meta_flush_mu);
        g_client.meta_flush_req = 1;
        pthread_cond_signal(&g_client.meta_flush_cv);
        pthread_mutex_unlock(&g_client.meta_flush_mu);
        return;
    }
    efs_client_ensure_dir_locks();
    pthread_mutex_lock(&g_client.dirty_mu);
    g_client.meta_dirty = 1;
    pthread_mutex_unlock(&g_client.dirty_mu);
}

int efs_client_note_meta_change(int force)
{
    const char *skip = getenv("EFS_SKIP_META_FLUSH");
    if (skip && *skip && strcmp(skip, "0") != 0)
        return EFS_OK;
    if (force)
        g_client.meta_flush_force = 1;
    if (!g_client.meta_batch || force)
        /* Forced flush (fsync/unmount) or non-batched C API: durable report. */
        return efs_client_report_dirty(1);

    efs_client_ensure_dir_locks();
    pthread_mutex_lock(&g_client.dirty_mu);
    g_client.meta_dirty = 1;
    g_client.meta_dirty_ops++;
    uint32_t ops = g_client.meta_dirty_ops;
    uint32_t thresh = g_client.meta_batch_ops ? g_client.meta_batch_ops : 4096;
    int stripe = g_client.last_dirty_stripe;
    if (stripe < 0 || stripe >= EFS_DIR_LOCKS)
        stripe = 0;
    uint32_t stripe_ops = g_client.dirty_stripe_ops[stripe];
    int flush = (ops >= thresh) || (stripe_ops >= thresh);
    pthread_mutex_unlock(&g_client.dirty_mu);

    /* Batched threshold flush: hand off to the dedicated flush thread so the
     * O(table) serialize/encode/PUT never runs on a FUSE worker. The thread
     * coalesces all dirty ops into one flush; forced flushes (fsync/unmount)
     * above stay blocking for durability. */
    if (flush) {
        if (g_client.meta_flush_started) {
            pthread_mutex_lock(&g_client.meta_flush_mu);
            g_client.meta_flush_req = 1;
            pthread_cond_signal(&g_client.meta_flush_cv);
            pthread_mutex_unlock(&g_client.meta_flush_mu);
            return EFS_OK;
        }
        return efs_client_report_dirty(0);
    }
    return EFS_OK;
}

/* Consume one PUT_CHUNK reply from a conn that reply_watch marked ready
 * (or whose poll fd fired). Returns 1 on OK ack, 0 on quota, -1 when the
 * conn was dropped. */
static int put_recv_reply(efs_node_id_t nid, struct efs_conn *conn)
{
    uint8_t reply_type = 0, status = 0;
    if (efs_conn_recv_u8_reply(conn, &reply_type, &status) != 0 ||
        reply_type != EFS_MSG_PUT_CHUNK_REPLY) {
        efs_client_conn_drop(nid, conn);
        efs_client_node_note_fail(nid);
        return -1;
    }
    if (status == EFS_PUT_CHUNK_OK) {
        efs_client_node_note_ok(nid);
        efs_client_conn_release(nid, conn);
        return 1;
    }
    if (status == EFS_PUT_CHUNK_QUOTA_EXCEEDED) {
        efs_client_conn_release(nid, conn);
        return 0;
    }
    efs_client_conn_drop(nid, conn);
    efs_client_node_note_fail(nid);
    return -1;
}

/* failed_out[i]=1 marks placement slots that need invalidate/retry. */
static int put_fragments_parallel_once(efs_ino_t ino, uint32_t chunk_index,
                                      const efs_node_id_t nodes[EFS_NUM_FRAGMENTS],
                                      const uint8_t *fragments[EFS_NUM_FRAGMENTS],
                                      uint32_t frag_len,
                                      const uint8_t checksums[EFS_NUM_FRAGMENTS][EFS_HASH_SIZE],
                                      int failed_out[EFS_NUM_FRAGMENTS])
{
    struct efs_conn *conns[EFS_NUM_FRAGMENTS];
    int pending[EFS_NUM_FRAGMENTS];
    struct efs_msg_put_chunk hdrs[EFS_NUM_FRAGMENTS];
    int acks = 0;
    int quota_errors = 0;
    int reachable = 0;

    if (failed_out) {
        for (int i = 0; i < EFS_NUM_FRAGMENTS; i++)
            failed_out[i] = 0;
    }

    for (int i = 0; i < EFS_NUM_FRAGMENTS; i++) {
        conns[i] = NULL;
        pending[i] = 0;
        memset(&hdrs[i], 0, sizeof(hdrs[i]));
    }

    /* Unreachable peers count as missing acks — do not abort the whole PUT. */
    for (int i = 0; i < EFS_NUM_FRAGMENTS; i++) {
        if (nodes[i] == 0) {
            if (failed_out)
                failed_out[i] = 1;
            continue;
        }
        if (efs_client_node_is_down(nodes[i])) {
            if (failed_out)
                failed_out[i] = 1;
            continue;
        }
        conns[i] = efs_client_conn_get(nodes[i]);
        if (!conns[i]) {
            efs_client_node_note_fail(nodes[i]);
            if (failed_out)
                failed_out[i] = 1;
            continue;
        }
        reachable++;
        hdrs[i].export_id = g_client.export_id;
        hdrs[i].ino = ino;
        hdrs[i].chunk_index = chunk_index;
        hdrs[i].fragment_index = (uint32_t)i;
        hdrs[i].data_len = frag_len;
        memcpy(hdrs[i].checksum, checksums[i], EFS_HASH_SIZE);
    }

    /* Send on every live conn before waiting (overlap RTTs). */
    for (int i = 0; i < EFS_NUM_FRAGMENTS; i++) {
        if (!conns[i])
            continue;
        /* Never enter a blocking writev: a slow peer fills the TCP window
         * and SO_SNDTIMEO stalls the whole chunk. Wait for POLLOUT; a miss
         * is backpressure (failed=2), not a dead peer. 8ms was too short
         * under 8-job 1M (many 64 KiB fragments in flight) and inverted
         * that case vs 1-job / 128k. RDMA sends post to the SQ without
         * touching socket buffers, so the check is TCP-only. */
        if (!conns[i]->rc) {
            struct pollfd p = { .fd = conns[i]->fd, .events = POLLOUT };
            int pr = poll(&p, 1, 100);
            if (pr < 0 || (p.revents & (POLLERR | POLLHUP | POLLNVAL))) {
                efs_client_conn_drop(nodes[i], conns[i]);
                efs_client_node_note_fail(nodes[i]);
                if (failed_out)
                    failed_out[i] = 1;
                conns[i] = NULL;
                continue;
            }
            if (pr == 0 || !(p.revents & POLLOUT)) {
                efs_client_conn_release(nodes[i], conns[i]);
                if (failed_out)
                    failed_out[i] = 2;
                conns[i] = NULL;
                continue;
            }
        }
        if (efs_conn_send_msg_parts(conns[i], EFS_MSG_PUT_CHUNK,
                                    &hdrs[i], (uint32_t)sizeof(hdrs[i]),
                                    fragments[i], frag_len) != 0) {
            efs_client_conn_drop(nodes[i], conns[i]);
            efs_client_node_note_fail(nodes[i]);
            if (failed_out)
                failed_out[i] = 1;
            conns[i] = NULL;
            continue;
        }
        pending[i] = 1;
    }

    /* Poll for replies; return as soon as we have ≥2 acks so one dead peer
     * cannot serialize a full SO_RCVTIMEO wait. */
    struct timespec ts0;
    clock_gettime(CLOCK_MONOTONIC, &ts0);
    int64_t deadline_ms = (int64_t)ts0.tv_sec * 1000 +
                          (int64_t)ts0.tv_nsec / 1000000 + EFS_IO_TIMEOUT_MS;

    while (acks < 2) {
        /* Harvest replies that are already here (RDMA CQE / readable fd).
         * Quick (non-spinning) checks: a full-budget spin per conn per
         * iteration was the single-client CPU sink (up to 6 spins per
         * chunk across the two loops). */
        for (int i = 0; i < EFS_NUM_FRAGMENTS && acks < 2; i++) {
            if (!pending[i] || !conns[i])
                continue;
            int w = efs_conn_reply_watch_quick(conns[i]);
            if (w == EFS_CONN_REPLY_READY) {
                int r = put_recv_reply(nodes[i], conns[i]);
                if (r > 0)
                    acks++;
                else if (r == 0)
                    quota_errors++;
                else if (failed_out)
                    failed_out[i] = 1;
                conns[i] = NULL;
                pending[i] = 0;
            } else if (w < 0) {
                efs_client_conn_drop(nodes[i], conns[i]);
                efs_client_node_note_fail(nodes[i]);
                if (failed_out)
                    failed_out[i] = 1;
                conns[i] = NULL;
                pending[i] = 0;
            }
        }
        if (acks >= 2)
            break;

        /* Nothing ready: ONE short fixed-budget spin on the first pending
         * conn. The legs' replies land within a few us of each other (same
         * server disk latency), so the wave is caught here and reaped by
         * the quick harvest on the next iteration. The budget is capped:
         * the adaptive watch hands out up to 200us whenever fewer than 16
         * of the 32 put workers spin at once, which was ~8 cores of pure
         * spin at 5 GB/s; the poll below catches late replies. */
        for (int i = 0; i < EFS_NUM_FRAGMENTS && acks < 2; i++) {
            if (!pending[i] || !conns[i])
                continue;
            int w = efs_conn_reply_watch_us(conns[i], 24);
            if (w == EFS_CONN_REPLY_READY) {
                int r = put_recv_reply(nodes[i], conns[i]);
                if (r > 0)
                    acks++;
                else if (r == 0)
                    quota_errors++;
                else if (failed_out)
                    failed_out[i] = 1;
                conns[i] = NULL;
                pending[i] = 0;
            } else if (w < 0) {
                efs_client_conn_drop(nodes[i], conns[i]);
                efs_client_node_note_fail(nodes[i]);
                if (failed_out)
                    failed_out[i] = 1;
                conns[i] = NULL;
                pending[i] = 0;
            }
            break; /* spin on one conn per iteration at most */
        }
        if (acks >= 2)
            break;

        struct pollfd pfds[EFS_NUM_FRAGMENTS];
        int map[EFS_NUM_FRAGMENTS];
        int npoll = 0;
        for (int i = 0; i < EFS_NUM_FRAGMENTS; i++) {
            if (!pending[i] || !conns[i])
                continue;
            int w = efs_conn_reply_watch_quick(conns[i]);
            if (w == EFS_CONN_REPLY_READY) {
                int r = put_recv_reply(nodes[i], conns[i]);
                if (r > 0)
                    acks++;
                else if (r == 0)
                    quota_errors++;
                else if (failed_out)
                    failed_out[i] = 1;
                conns[i] = NULL;
                pending[i] = 0;
                continue;
            } else if (w < 0) {
                efs_client_conn_drop(nodes[i], conns[i]);
                efs_client_node_note_fail(nodes[i]);
                if (failed_out)
                    failed_out[i] = 1;
                conns[i] = NULL;
                pending[i] = 0;
                continue;
            }
            pfds[npoll].fd = w;
            pfds[npoll].events = POLLIN;
            pfds[npoll].revents = 0;
            map[npoll] = i;
            npoll++;
        }
        if (acks >= 2)
            break;
        if (npoll == 0)
            break;

        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        int64_t now_ms = (int64_t)ts.tv_sec * 1000 + (int64_t)ts.tv_nsec / 1000000;
        int64_t left = deadline_ms - now_ms;
        if (left <= 0) {
            for (int i = 0; i < EFS_NUM_FRAGMENTS; i++) {
                if (!pending[i] || conns[i] == NULL)
                    continue;
                efs_client_conn_drop(nodes[i], conns[i]);
                efs_client_node_note_fail(nodes[i]);
                if (failed_out)
                    failed_out[i] = 1;
                conns[i] = NULL;
                pending[i] = 0;
            }
            break;
        }
        /* Cap each poll slice so we re-check the deadline promptly. */
        int wait_ms = left > 2000 ? 2000 : (int)left;
        int pr = poll(pfds, (nfds_t)npoll, wait_ms);
        if (pr < 0)
            continue;
        if (pr == 0)
            continue;

        for (int p = 0; p < npoll; p++) {
            if (!(pfds[p].revents & (POLLIN | POLLERR | POLLHUP)))
                continue;
            int i = map[p];
            if (!pending[i] || !conns[i])
                continue;
            int r = put_recv_reply(nodes[i], conns[i]);
            if (r > 0)
                acks++;
            else if (r == 0)
                quota_errors++;
            else if (failed_out)
                failed_out[i] = 1;
            conns[i] = NULL;
            pending[i] = 0;
        }
    }

    /* Quorum met: wait for remaining replies so pooled conns stay reusable.
     * poll(0)+drop used to close the third peer on almost every chunk;
     * 8-job 1M then spent its time in efs_connect_tcp. Cap the drain so a
     * truly stuck peer cannot sit on SO_RCVTIMEO. */
    if (acks >= 2) {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        int64_t now_ms = (int64_t)ts.tv_sec * 1000 +
                         (int64_t)ts.tv_nsec / 1000000;
        int64_t left = deadline_ms - now_ms;
        if (left > 2000)
            left = 2000;
        int64_t drain_end = now_ms + (left > 0 ? left : 0);
        while (now_ms < drain_end) {
            struct pollfd pfds[EFS_NUM_FRAGMENTS];
            int map[EFS_NUM_FRAGMENTS];
            int npoll = 0;
            for (int i = 0; i < EFS_NUM_FRAGMENTS; i++) {
                if (!conns[i])
                    continue;
                int w = efs_conn_reply_watch_quick(conns[i]);
                if (w == EFS_CONN_REPLY_READY) {
                    put_recv_reply(nodes[i], conns[i]);
                    conns[i] = NULL;
                    pending[i] = 0;
                    continue;
                } else if (w < 0) {
                    efs_client_conn_drop(nodes[i], conns[i]);
                    conns[i] = NULL;
                    pending[i] = 0;
                    continue;
                }
                pfds[npoll].fd = w;
                pfds[npoll].events = POLLIN;
                pfds[npoll].revents = 0;
                map[npoll] = i;
                npoll++;
            }
            if (npoll == 0)
                break;
            int wait_ms = (int)(drain_end - now_ms);
            if (wait_ms < 1)
                wait_ms = 1;
            int pr = poll(pfds, (nfds_t)npoll, wait_ms);
            if (pr <= 0)
                break;
            for (int p = 0; p < npoll; p++) {
                if (!(pfds[p].revents & (POLLIN | POLLERR | POLLHUP)))
                    continue;
                int i = map[p];
                if (!conns[i])
                    continue;
                put_recv_reply(nodes[i], conns[i]);
                conns[i] = NULL;
                pending[i] = 0;
            }
            clock_gettime(CLOCK_MONOTONIC, &ts);
            now_ms = (int64_t)ts.tv_sec * 1000 +
                     (int64_t)ts.tv_nsec / 1000000;
        }
    }

    for (int i = 0; i < EFS_NUM_FRAGMENTS; i++) {
        if (!conns[i])
            continue;
        efs_client_conn_drop(nodes[i], conns[i]);
        if (acks < 2)
            efs_client_node_note_fail(nodes[i]);
        if (failed_out)
            failed_out[i] = 1;
        conns[i] = NULL;
        pending[i] = 0;
    }

    if (quota_errors >= 2)
        return EFS_ERR_QUOTA;
    if (acks >= 2)
        return EFS_OK;
    if (reachable == 0)
        return EFS_ERR_NET;
    return EFS_ERR_NO_QUORUM;
}

/* If a stripe member is down (or failed the last attempt), put that
 * fragment on an unused live node. Four-node cluster, one down: the
 * remaining three still take a full 2+1 stripe. */
static void reroute_down_fragments(efs_node_id_t nodes[EFS_NUM_FRAGMENTS],
                                   const int *failed)
{
    for (int i = 0; i < EFS_NUM_FRAGMENTS; i++) {
        int bad = (nodes[i] == 0) || efs_client_node_is_down(nodes[i]) ||
                  (failed && failed[i] == 1);
        if (!bad)
            continue;
        for (uint32_t n = 0; n < g_client.node_count; n++) {
            efs_node_id_t id = g_client.nodes[n].id;
            if (id == 0 || efs_client_node_is_down(id))
                continue;
            int used = 0;
            for (int j = 0; j < EFS_NUM_FRAGMENTS; j++) {
                if (j != i && nodes[j] == id)
                    used = 1;
            }
            if (!used) {
                nodes[i] = id;
                break;
            }
        }
    }
}

int efs_client_put_fragments_parallel(efs_ino_t ino, uint32_t chunk_index,
                                      efs_node_id_t nodes[EFS_NUM_FRAGMENTS],
                                      const uint8_t *fragments[EFS_NUM_FRAGMENTS],
                                      uint32_t frag_len,
                                      const uint8_t checksums[EFS_NUM_FRAGMENTS][EFS_HASH_SIZE])
{
    /* 2+1 EC requires 3 distinct nodes. On a 1-2 node ring placement wraps and
     * two fragments land on one machine, so a "2-ack quorum" can be a single
     * disk — losing that node loses the chunk. Refuse rather than store
     * undurably. */
    if (nodes[0] == nodes[1] || nodes[1] == nodes[2] || nodes[0] == nodes[2]) {
        fprintf(stderr,
                "put_fragments ino=%llu chunk=%u: EC needs 3 distinct nodes "
                "(got %u,%u,%u) — refusing undurable write\n",
                (unsigned long long)ino, chunk_index,
                (unsigned)nodes[0], (unsigned)nodes[1], (unsigned)nodes[2]);
        fflush(stderr);
        return EFS_ERR_NO_QUORUM;
    }

    /* Retry transient blips; only invalidate peers that actually failed. */
    int rc = EFS_ERR_NET;
    int failed[EFS_NUM_FRAGMENTS] = {0, 0, 0};
    for (int attempt = 1; attempt <= 4; attempt++) {
        reroute_down_fragments(nodes, attempt == 1 ? NULL : failed);
        rc = put_fragments_parallel_once(ino, chunk_index, nodes, fragments,
                                         frag_len, checksums, failed);
        if (rc == EFS_OK || rc == EFS_ERR_QUOTA)
            return rc;
        /* Only hard I/O failures invalidate a node's idle pool. failed[] is
         * also set for POLLOUT soft-misses (window backpressure) and down-skip,
         * which must NOT trigger a reconnect storm — the node is not dead. */
        for (int i = 0; i < EFS_NUM_FRAGMENTS; i++) {
            if (failed[i] == 1 && nodes[i] != 0 &&
                !efs_client_node_is_down(nodes[i]))
                efs_client_conn_invalidate_node(nodes[i]);
        }
        /* Fail fast when fewer than 2 peers are even reachable — retrying a
         * quorum we cannot reach only stacks latency. */
        int live = 0;
        for (int i = 0; i < EFS_NUM_FRAGMENTS; i++)
            if (nodes[i] != 0 && !efs_client_node_is_down(nodes[i]))
                live++;
        if (live < 2) {
            if (attempt == 1) {
                fprintf(stderr,
                        "put_fragments ino=%llu chunk=%u: only %d/%u peers live, "
                        "clearing down-marks and re-probing\n",
                        (unsigned long long)ino, chunk_index, live,
                        (unsigned)EFS_NUM_FRAGMENTS);
                fflush(stderr);
                efs_client_nodes_force_reprobe();
                continue;
            }
            fprintf(stderr,
                    "put_fragments ino=%llu chunk=%u: only %d/%u peers live, "
                    "cannot reach EC quorum — failing fast\n",
                    (unsigned long long)ino, chunk_index, live,
                    (unsigned)EFS_NUM_FRAGMENTS);
            fflush(stderr);
            return EFS_ERR_NO_QUORUM;
        }
        fprintf(stderr,
                "put_fragments ino=%llu chunk=%u nodes=%u,%u,%u "
                "attempt %d/4 failed: %s (efs_rc=%d)%s\n",
                (unsigned long long)ino, chunk_index,
                (unsigned)nodes[0], (unsigned)nodes[1], (unsigned)nodes[2],
                attempt, efs_strerror(rc), rc,
                attempt < 4 ? " — retrying" : " — giving up");
        fflush(stderr);
        /* Exponential backoff + jitter instead of a fixed linear sleep. */
        if (attempt < 4) {
            useconds_t base = 100000u << (attempt - 1); /* 100,200,400ms */
            useconds_t jitter = (useconds_t)(rand() % 50000);
            usleep(base + jitter);
        }
    }
    return rc;
}

/* Assemble one chunk buffer for a write spanning [wr_start, wr_end).
 * Sets *from_zero_out when the chunk was built from a zero base (new or full
 * overwrite), which lets callers skip Blake3 of known-zero halves.
 * Returns EFS_OK on success; a read-modify-write fetch failure is propagated
 * (never silently zero over live data). */
static int assemble_write_chunk(efs_ino_t ino, uint64_t old_size,
                                uint64_t offset, const char *buf,
                                uint64_t chunk_start, uint64_t wr_start,
                                uint64_t wr_end, uint32_t chunk_size,
                                uint8_t *chunk, int *from_zero_out)
{
    int covers_full = (wr_start == chunk_start &&
                       wr_end == chunk_start + chunk_size);
    size_t off_in_chunk = (size_t)(wr_start - chunk_start);
    size_t wr_len = (size_t)(wr_end - wr_start);
    size_t src_off = (size_t)(wr_start - offset);

    if (covers_full) {
        /* Full overwrite: copy user bytes only — no memset+memcpy. */
        memcpy(chunk, buf + src_off, chunk_size);
        *from_zero_out = efs_bytes_are_zero(chunk, chunk_size);
        efs_rdcache_invalidate(ino, (uint32_t)(chunk_start / chunk_size));
        return EFS_OK;
    }

    if (chunk_start >= old_size) {
        *from_zero_out = 1;
        /* Zero only the unwritten regions. */
        if (off_in_chunk > 0)
            memset(chunk, 0, off_in_chunk);
        if (off_in_chunk + wr_len < chunk_size)
            memset(chunk + off_in_chunk + wr_len, 0,
                   chunk_size - off_in_chunk - wr_len);
    } else {
        *from_zero_out = 0;
        size_t existing = (size_t)(old_size - chunk_start);
        if (existing > chunk_size)
            existing = chunk_size;
        uint32_t ci = (uint32_t)(chunk_start / chunk_size);
        /* Do not use rdcache as the RMW base. A prior sub-chunk read
         * caches the full 128 KiB (new 4k + zeros). The next 4k write
         * then PUTs that stale chunk and drops every later 4k in it. */
        if (efs_dcache_get(ino, ci, chunk, chunk_size) != 0) {
            size_t got = 0;
            int rrc = efs_client_read(ino, chunk_start, existing, (char *)chunk, &got);
            if (rrc != EFS_OK || got != existing) {
                got = 0;
                rrc = efs_client_read(ino, chunk_start, existing,
                                      (char *)chunk, &got);
            }
            if (rrc != EFS_OK || got != existing) {
                struct efs_chunk_entry ce;
                if (export_chunk_copy(ino, ci, &ce) != 0) {
                    /* Size can be ahead of the store (async WB / dcache
                     * patch). No published chunk yet — base is zeros. */
                    *from_zero_out = 1;
                    if (off_in_chunk > 0)
                        memset(chunk, 0, off_in_chunk);
                    if (off_in_chunk + wr_len < chunk_size)
                        memset(chunk + off_in_chunk + wr_len, 0,
                               chunk_size - off_in_chunk - wr_len);
                } else {
                    /* Chunk is in the table; a failed GET is real IO. */
                    return (rrc != EFS_OK) ? rrc : EFS_ERR_IO;
                }
            }
            if (existing < chunk_size)
                memset(chunk + existing, 0, chunk_size - existing);
        }
    }

    memcpy(chunk + off_in_chunk, buf + src_off, wr_len);
    /* Dirty dcache is the source of truth for partials; skip a second 128 KiB
     * rdcache copy on every RMW. */
    return EFS_OK;
}

/* Hash fragments; when from_zero and a half was not written, reuse the cached
 * zero-fragment digest and skip hashing the identical parity copy. */
static void hash_write_fragments(const uint8_t *fragments[EFS_NUM_FRAGMENTS],
                                 uint32_t frag_len, uint32_t chunk_size,
                                 int from_zero, uint64_t chunk_start,
                                 uint64_t wr_start, uint64_t wr_end,
                                 uint8_t checksums[EFS_NUM_FRAGMENTS][EFS_HASH_SIZE])
{
    uint32_t half = chunk_size / 2;
    int frag0_zero = 0, frag1_zero = 0;
    if (from_zero) {
        frag0_zero = (wr_start >= chunk_start + half);
        frag1_zero = (wr_end <= chunk_start + half);
    }

    /* Full-chunk zero write: both data halves and parity are zero. */
    if (from_zero && wr_start <= chunk_start &&
        wr_end >= chunk_start + chunk_size) {
        efs_hash_zero_fragment_len(frag_len, checksums[0]);
        efs_hash_zero_fragment_len(frag_len, checksums[1]);
        efs_hash_zero_fragment_len(frag_len, checksums[2]);
        return;
    }

    if (frag0_zero)
        efs_hash_zero_fragment_len(frag_len, checksums[0]);
    else
        efs_hash(fragments[0], frag_len, checksums[0]);

    if (frag1_zero)
        efs_hash_zero_fragment_len(frag_len, checksums[1]);
    else
        efs_hash(fragments[1], frag_len, checksums[1]);

    if (frag0_zero && !frag1_zero)
        memcpy(checksums[2], checksums[1], EFS_HASH_SIZE);
    else if (frag1_zero && !frag0_zero)
        memcpy(checksums[2], checksums[0], EFS_HASH_SIZE);
    else if (frag0_zero && frag1_zero)
        efs_hash_zero_fragment_len(frag_len, checksums[2]);
    else
        efs_hash(fragments[2], frag_len, checksums[2]);
}

/* Per-batch completion tracker. Lives on the put_pool_run caller's stack; the
 * caller waits until every job it enqueued has been drained, so the tracker
 * outlives all pool-thread access. This is what makes the pool reentrant —
 * concurrent writers each wait on their own batch instead of racing on one
 * shared batch slot. */
struct put_batch {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    int remaining;
};

struct chunk_put_job {
    efs_ino_t ino;
    uint32_t ci;
    uint64_t old_size;
    uint64_t offset;
    const char *buf;
    uint64_t end;
    int rc;
    int deferred;
    struct put_batch *bp;
    efs_node_id_t nodes[EFS_NUM_FRAGMENTS];
    uint8_t checksums[EFS_NUM_FRAGMENTS][EFS_HASH_SIZE];
};

static void *chunk_put_worker(void *arg);

/* Dirty assembled-chunk cache. Partial writes (4k randwrite) used to GET+encode
 * +PUT a full 128 KiB chunk on every FUSE write. Hold the assembled chunk and
 * PUT once on flush / unmount. 65536 × 128 KiB ≈ 8 GiB if every slot is live;
 * buffers are allocated on first store. Collision does not evict (that
 * serialized PUTs on one mutex and tanked 4k IOPS) — the caller PUTs now. */
#define DCACHE_SLOTS  65536
#define DCACHE_SHARDS 64
#define DCACHE_NR     8
struct dcache_ent {
    efs_ino_t ino;
    uint32_t ci;
    uint8_t *data;
    uint32_t len;
    int dirty;
    int have_base; /* 1 = data[] is a complete chunk; 0 = sparse patches */
    /* 1 = this slot holds a pin on its ino in g_dcache_pins (set at the
     * clean→dirty transition, cleared when the slot's data is published
     * or dropped). Guards against double-count on re-dirty. */
    int pin_held;
    uint8_t nrange;
    uint32_t roff[DCACHE_NR];
    uint32_t rlen[DCACHE_NR];
    struct dcache_ent *next;
};

/* Per-ino count of dcache slots holding unreported data — the "unreported
 * data" eviction pin (client-cache design Part A, pin rule 1's dcache
 * half). A slot pins from its clean→dirty transition until its PUT lands
 * (dcache_put_now then marks the ino dirty, so the dirty-set pin takes
 * over until the REPORT) or the slot is dropped. Without this, a same-size
 * overwrite (dirty dcache data, ino NOT in the dirty set) left the staged
 * row evictable: the row's loss then lost the flush's set_chunk/size
 * update and mis-truncated efs_dcache_flush_ino to ci=0.
 * Open-addressing, power-of-two, 0 = empty, backward-shift delete.
 * ALL table accesses hold g_dcache_pin_mu; add/release are called with
 * the slot's dcache_mu already held (lock order: dcache_mu → pin_mu;
 * nothing takes them in reverse). */
static uint64_t *g_dcache_pin_keys;   /* ino */
static uint32_t *g_dcache_pin_counts; /* live slots */
static uint64_t g_dcache_pin_mask;
static uint64_t g_dcache_pin_count;
static pthread_mutex_t g_dcache_pin_mu = PTHREAD_MUTEX_INITIALIZER;

static int dcache_pin_ensure(uint64_t need)
{
    if (g_dcache_pin_keys && need * 2 <= g_dcache_pin_mask + 1)
        return 0;
    uint64_t old_mask = g_dcache_pin_mask;
    uint64_t *old_keys = g_dcache_pin_keys;
    uint32_t *old_counts = g_dcache_pin_counts;
    uint64_t cap = g_dcache_pin_mask ? g_dcache_pin_mask + 1 : 1024;
    while (cap < need * 4)
        cap *= 2;
    uint64_t *nk = calloc(cap, sizeof(*nk));
    uint32_t *nc = calloc(cap, sizeof(*nc));
    if (!nk || !nc) {
        free(nk);
        free(nc);
        return -1;
    }
    g_dcache_pin_keys = nk;
    g_dcache_pin_counts = nc;
    g_dcache_pin_mask = cap - 1;
    if (old_keys) {
        for (uint64_t i = 0; i <= old_mask; i++) {
            uint64_t k = old_keys[i];
            if (!k)
                continue;
            uint64_t j = k & g_dcache_pin_mask;
            while (g_dcache_pin_keys[j])
                j = (j + 1) & g_dcache_pin_mask;
            g_dcache_pin_keys[j] = k;
            g_dcache_pin_counts[j] = old_counts[i];
        }
        free(old_keys);
        free(old_counts);
    }
    return 0;
}

/* Caller holds the slot's dcache_mu; takes pin_mu internally. Best-effort:
 * a failed grow drops the pin (the row may then be evicted under a dirty
 * dcache slot — a bounded-cache miss, never corruption of data already on
 * the servers). */
static void dcache_pin_add(struct dcache_ent *e)
{
    if (e->pin_held || !e->ino)
        return;
    pthread_mutex_lock(&g_dcache_pin_mu);
    if (dcache_pin_ensure(g_dcache_pin_count + 1) == 0) {
        uint64_t i = (uint64_t)e->ino & g_dcache_pin_mask;
        while (g_dcache_pin_keys[i] && g_dcache_pin_keys[i] != (uint64_t)e->ino)
            i = (i + 1) & g_dcache_pin_mask;
        if (!g_dcache_pin_keys[i]) {
            g_dcache_pin_keys[i] = (uint64_t)e->ino;
            g_dcache_pin_count++;
        }
        g_dcache_pin_counts[i]++;
        e->pin_held = 1;
    }
    pthread_mutex_unlock(&g_dcache_pin_mu);
}

/* Caller holds the slot's dcache_mu; takes pin_mu internally. */
static void dcache_pin_release(struct dcache_ent *e)
{
    if (!e->pin_held)
        return;
    e->pin_held = 0;
    pthread_mutex_lock(&g_dcache_pin_mu);
    if (!g_dcache_pin_keys)
        goto out;
    uint64_t i = (uint64_t)e->ino & g_dcache_pin_mask;
    for (uint64_t n = 0; n <= g_dcache_pin_mask; n++) {
        uint64_t k = g_dcache_pin_keys[i];
        if (!k)
            goto out;
        if (k != (uint64_t)e->ino) {
            i = (i + 1) & g_dcache_pin_mask;
            continue;
        }
        if (--g_dcache_pin_counts[i] == 0) {
            /* Backward-shift delete: clear slot i, then rehome any key in
             * the following run whose ideal slot is not in (i, j]. */
            g_dcache_pin_keys[i] = 0;
            g_dcache_pin_count--;
            uint64_t j = (i + 1) & g_dcache_pin_mask;
            while (g_dcache_pin_keys[j]) {
                uint64_t h = g_dcache_pin_keys[j] & g_dcache_pin_mask;
                /* h is in the cyclic interval (i, j] iff moving j's key
                 * into i would cross its home — then it must stay. */
                int in_gap = (i < j) ? (h > i && h <= j)
                                     : (h > i || h <= j);
                if (!in_gap) {
                    g_dcache_pin_keys[i] = g_dcache_pin_keys[j];
                    g_dcache_pin_counts[i] = g_dcache_pin_counts[j];
                    g_dcache_pin_keys[j] = 0;
                    i = j;
                }
                j = (j + 1) & g_dcache_pin_mask;
            }
        }
        goto out;
    }
out:
    pthread_mutex_unlock(&g_dcache_pin_mu);
}

/* Evictor query: 1 if any dcache slot pins this ino. */
int efs_dcache_ino_pinned(efs_ino_t ino)
{
    if (!ino)
        return 0;
    pthread_mutex_lock(&g_dcache_pin_mu);
    int pinned = 0;
    if (g_dcache_pin_keys) {
        uint64_t i = (uint64_t)ino & g_dcache_pin_mask;
        for (uint64_t n = 0; n <= g_dcache_pin_mask; n++) {
            uint64_t k = g_dcache_pin_keys[i];
            if (!k)
                break;
            if (k == (uint64_t)ino) {
                pinned = g_dcache_pin_counts[i] > 0;
                break;
            }
            i = (i + 1) & g_dcache_pin_mask;
        }
    }
    pthread_mutex_unlock(&g_dcache_pin_mu);
    return pinned;
}
static struct {
    pthread_mutex_t shard[DCACHE_SHARDS];
    /* Serializes the network base-read + PUT phase of a flush per shard.
     * dcache_mu is dropped before that I/O (to avoid holding it over the
     * network), so two concurrent flushes of the same chunk could otherwise
     * both read a stale pre-PUT base and the last PUT would wipe the other's
     * just-written ranges (concurrent-append data loss). */
    pthread_mutex_t shard_io[DCACHE_SHARDS];
    struct dcache_ent e[DCACHE_SLOTS];
    int inited;
    uint64_t dirty_bytes;
} g_dcache;
static pthread_once_t g_dcache_once = PTHREAD_ONCE_INIT;

static void dcache_note_dirty_bytes(int64_t delta)
{
    if (delta > 0)
        __atomic_add_fetch(&g_dcache.dirty_bytes, (uint64_t)delta,
                           __ATOMIC_RELAXED);
    else if (delta < 0) {
        uint64_t sub = (uint64_t)(-delta);
        uint64_t cur = __atomic_load_n(&g_dcache.dirty_bytes, __ATOMIC_RELAXED);
        while (cur) {
            uint64_t next = cur > sub ? cur - sub : 0;
            if (__atomic_compare_exchange_n(&g_dcache.dirty_bytes, &cur, next,
                                            0, __ATOMIC_RELAXED,
                                            __ATOMIC_RELAXED))
                break;
        }
    }
}

static void dcache_init(void)
{
    for (int i = 0; i < DCACHE_SHARDS; i++) {
        pthread_mutex_init(&g_dcache.shard[i], NULL);
        pthread_mutex_init(&g_dcache.shard_io[i], NULL);
    }
    g_dcache.inited = 1;
}

static uint32_t dcache_slot(efs_ino_t ino, uint32_t ci)
{
    uint64_t h = (uint64_t)ino * 0x9E3779B97F4A7C15ULL;
    h ^= (uint64_t)ci * 0xBF58476D1CE4E5B9ULL;
    return (uint32_t)(h & (DCACHE_SLOTS - 1));
}

static pthread_mutex_t *dcache_mu(uint32_t slot)
{
    pthread_once(&g_dcache_once, dcache_init);
    return &g_dcache.shard[slot & (DCACHE_SHARDS - 1)];
}

static pthread_mutex_t *dcache_io_mu(uint32_t slot)
{
    pthread_once(&g_dcache_once, dcache_init);
    return &g_dcache.shard_io[slot & (DCACHE_SHARDS - 1)];
}

static int dcache_put_now(efs_ino_t ino, uint32_t ci, const uint8_t *chunk,
                          uint32_t chunk_size)
{
    uint32_t frag_len = chunk_size / 2;
    uint8_t *parity = efs_buf_alloc(frag_len);
    if (!parity)
        return EFS_ERR_NOMEM;

    uint8_t *frags[EFS_NUM_FRAGMENTS];
    frags[0] = (uint8_t *)(uintptr_t)chunk;
    frags[1] = (uint8_t *)(uintptr_t)(chunk + frag_len);
    frags[2] = parity;
    efs_encode_chunk(chunk, chunk_size, chunk_size, frags);

    const uint8_t *cfrags[EFS_NUM_FRAGMENTS] = {frags[0], frags[1], frags[2]};
    uint8_t checksums[EFS_NUM_FRAGMENTS][EFS_HASH_SIZE];
    int from_zero = efs_bytes_are_zero(chunk, chunk_size);
    hash_write_fragments(cfrags, frag_len, chunk_size, from_zero, 0, 0,
                         chunk_size, checksums);

    efs_node_id_t nodes[EFS_NUM_FRAGMENTS];
    efs_place_fragments(g_client.nodes, g_client.node_count, ino, ci, nodes);
    int rc = efs_client_put_fragments_parallel(ino, ci, nodes, cfrags, frag_len,
                                               checksums);
    efs_buf_free(parity, frag_len);
    if (rc != EFS_OK)
        return rc;

    /* The servers now hold newer data than any cached copy. A have_base=0
     * flush reads the OLD published chunk as its merge base, which populates
     * the rdcache with stale bytes; without invalidating, a later read serves
     * that stale rdcache entry and loses the just-written update. */
    efs_rdcache_invalidate(ino, ci);

    if (!export_chunk_exists(ino, ci)) {
        int room = efs_client_ensure_meta_room(0, 1);
        if (room != EFS_OK)
            return room;
    }
    if (efs_export_needs_chunk_grow(&g_client.export))
        export_reserve_chunks_locked(4096);
    efs_client_lock_dir(ino);
    pthread_mutex_lock(&g_client.idx_mu);
    efs_export_set_chunk(&g_client.export, ino, ci, nodes, checksums);
    pthread_mutex_unlock(&g_client.idx_mu);
    efs_client_unlock_dir(ino);
    efs_client_mark_chunk_dirty(ino, ci);
    /* Mark the ino too so the next report carries its size/mtime irec. The
     * append path reflects the reserved size before the patch, so
     * dcache_note_size sees no growth and skips its mark — and the
     * server-side append barrier releases only when a REPORT grows the
     * table size past the reservation, i.e. only after this PUT landed. */
    efs_client_mark_ino_dirty(ino);
    return EFS_OK;
}

static struct dcache_ent *dcache_find(uint32_t s, efs_ino_t ino, uint32_t ci)
{
    for (struct dcache_ent *e = &g_dcache.e[s]; e; e = e->next) {
        if (e->data && e->ino == ino && e->ci == ci)
            return e;
    }
    return NULL;
}

int efs_dcache_has(efs_ino_t ino, uint32_t ci)
{
    uint32_t s = dcache_slot(ino, ci);
    pthread_mutex_t *mu = dcache_mu(s);
    pthread_mutex_lock(mu);
    int has = dcache_find(s, ino, ci) != NULL;
    pthread_mutex_unlock(mu);
    return has;
}

int efs_dcache_get(efs_ino_t ino, uint32_t ci, uint8_t *dst, uint32_t len)
{
    return efs_dcache_copy(ino, ci, 0, dst, len);
}

static int dcache_range_covered(const struct dcache_ent *e, uint32_t off,
                                uint32_t len)
{
    if (!e || !len)
        return 0;
    uint32_t need = off;
    uint32_t end = off + len;
    while (need < end) {
        int hit = 0;
        for (uint8_t i = 0; i < e->nrange; i++) {
            uint32_t a = e->roff[i], b = a + e->rlen[i];
            if (a <= need && need < b) {
                need = b;
                hit = 1;
                break;
            }
        }
        if (!hit)
            return 0;
    }
    return 1;
}

int efs_dcache_copy(efs_ino_t ino, uint32_t ci, uint32_t off,
                    uint8_t *dst, uint32_t len)
{
    if (!dst || !len)
        return -1;
    uint32_t s = dcache_slot(ino, ci);
    pthread_mutex_t *mu = dcache_mu(s);
    pthread_mutex_lock(mu);
    struct dcache_ent *e = dcache_find(s, ino, ci);
    /* have_base=1: the whole chunk is valid. have_base=0: only dirty
     * ranges are valid (the rest is unfetched). Same-fd read-your-writes
     * after a first 4k on a new file land here — refusing them sent the
     * reader to GET/rdcache zeros (POSIX basic_rdwr_no_reopen). */
    if (e && (uint64_t)off + len <= e->len &&
        (e->have_base || dcache_range_covered(e, off, len))) {
        memcpy(dst, e->data + off, len);
        pthread_mutex_unlock(mu);
        return 0;
    }
    pthread_mutex_unlock(mu);
    return -1;
}

int efs_dcache_copy_unpub(efs_ino_t ino, uint32_t ci, uint32_t off,
                          uint8_t *dst, uint32_t len)
{
    if (!dst || !len)
        return -1;
    uint32_t s = dcache_slot(ino, ci);
    pthread_mutex_t *mu = dcache_mu(s);
    pthread_mutex_lock(mu);
    struct dcache_ent *e = dcache_find(s, ino, ci);
    if (e && e->data && !e->have_base &&
        (uint64_t)off + len <= e->len &&
        dcache_range_covered(e, off, len)) {
        memcpy(dst, e->data + off, len);
        pthread_mutex_unlock(mu);
        return 0;
    }
    pthread_mutex_unlock(mu);
    return -1;
}

void efs_dcache_overlay(efs_ino_t ino, uint32_t ci, uint8_t *dst, uint32_t len)
{
    if (!dst || !len)
        return;
    uint32_t s = dcache_slot(ino, ci);
    pthread_mutex_t *mu = dcache_mu(s);
    pthread_mutex_lock(mu);
    struct dcache_ent *e = dcache_find(s, ino, ci);
    if (!e) {
        pthread_mutex_unlock(mu);
        return;
    }
    if (e->have_base && len <= e->len) {
        memcpy(dst, e->data, len);
        pthread_mutex_unlock(mu);
        return;
    }
    if (!e->have_base) {
        for (uint8_t i = 0; i < e->nrange; i++) {
            uint32_t a = e->roff[i], n = e->rlen[i];
            if (a >= len || !n)
                continue;
            if (a + n > len)
                n = len - a;
            memcpy(dst + a, e->data + a, n);
        }
    }
    pthread_mutex_unlock(mu);
}

static void dcache_drop_locked(efs_ino_t ino, uint32_t ci, int skip_dirty)
{
    uint32_t s = dcache_slot(ino, ci);
    pthread_mutex_t *mu = dcache_mu(s);
    pthread_mutex_lock(mu);
    struct dcache_ent *head = &g_dcache.e[s];
    if (head->ino == ino && head->ci == ci) {
        if (skip_dirty && head->dirty) {
            pthread_mutex_unlock(mu);
            return;
        }
        if (head->dirty && head->len)
            dcache_note_dirty_bytes(-(int64_t)head->len);
        dcache_pin_release(head);
        efs_buf_free(head->data, head->len);
        if (head->next) {
            struct dcache_ent *n = head->next;
            /* n's pin (if any) travels with the copy into the head slot;
             * head's own pin was released above. */
            *head = *n;
            free(n);
        } else {
            memset(head, 0, sizeof(*head));
        }
        pthread_mutex_unlock(mu);
        return;
    }
    struct dcache_ent *prev = head;
    for (struct dcache_ent *e = head->next; e; prev = e, e = e->next) {
        if (e->ino == ino && e->ci == ci) {
            if (skip_dirty && e->dirty)
                break;
            if (e->dirty && e->len)
                dcache_note_dirty_bytes(-(int64_t)e->len);
            dcache_pin_release(e);
            prev->next = e->next;
            efs_buf_free(e->data, e->len);
            free(e);
            break;
        }
    }
    pthread_mutex_unlock(mu);
}

void efs_dcache_drop(efs_ino_t ino, uint32_t ci)
{
    dcache_drop_locked(ino, ci, 0);
}

void efs_dcache_drop_if_clean(efs_ino_t ino, uint32_t ci)
{
    dcache_drop_locked(ino, ci, 1);
}

static int dcache_store_owned(efs_ino_t ino, uint32_t ci, uint8_t *chunk,
                              uint32_t chunk_size);
static int dcache_merge_owned(efs_ino_t ino, uint32_t ci, uint32_t off,
                              const uint8_t *src, uint32_t len,
                              uint8_t *chunk, uint32_t cs);

static void dcache_add_range(struct dcache_ent *e, uint32_t off, uint32_t len)
{
    if (!e || e->have_base || !len)
        return;
    uint32_t end = off + len;
    int merged = 0;
    for (uint8_t i = 0; i < e->nrange; i++) {
        uint32_t a = e->roff[i], b = a + e->rlen[i];
        if (off <= b && a <= end) {
            uint32_t lo = a < off ? a : off;
            uint32_t hi = b > end ? b : end;
            e->roff[i] = lo;
            e->rlen[i] = hi - lo;
            merged = 1;
            break;
        }
    }
    if (!merged && e->nrange < DCACHE_NR) {
        e->roff[e->nrange] = off;
        e->rlen[e->nrange] = len;
        e->nrange++;
    } else if (!merged) {
        /* 9th disjoint range: the flush GET+merge only replays nrange
         * windows onto the published base. Dropping one would zero that
         * write. Collapse to the whole chunk so every dirty byte is kept
         * (peer bytes in true holes are the same loss as have_base=1). */
        e->roff[0] = 0;
        e->rlen[0] = e->len ? e->len : end;
        e->nrange = 1;
        return;
    }
    /* A merge into range i can now touch a neighbour that the first
     * pass did not ( [0,4) + [4,6) after filling the gap ). */
    for (uint8_t i = 0; i < e->nrange; i++) {
        uint32_t a = e->roff[i], b = a + e->rlen[i];
        for (uint8_t j = (uint8_t)(i + 1); j < e->nrange; ) {
            uint32_t c = e->roff[j], d = c + e->rlen[j];
            if (a <= d && c <= b) {
                if (c < a)
                    a = c;
                if (d > b)
                    b = d;
                e->roff[i] = a;
                e->rlen[i] = b - a;
                e->nrange--;
                e->roff[j] = e->roff[e->nrange];
                e->rlen[j] = e->rlen[e->nrange];
                continue;
            }
            j++;
        }
    }
}

static int dcache_patch(efs_ino_t ino, uint32_t ci, uint32_t off, const uint8_t *src,
                        uint32_t len)
{
    if (!src || !len)
        return -1;
    uint32_t s = dcache_slot(ino, ci);
    pthread_mutex_t *mu = dcache_mu(s);
    pthread_mutex_lock(mu);
    struct dcache_ent *e = dcache_find(s, ino, ci);
    if (e && e->data && off + len <= e->len) {
        memcpy(e->data + off, src, len);
        if (!e->dirty) {
            /* The entry is mid-flush: its snapshot was taken and dirty cleared,
             * but e->data is not freed until the flush's post-PUT "!e->dirty"
             * check. Patch it and re-dirty so this write is not lost — that
             * check then leaves the entry in place and a later flush writes it
             * back. Without this, a write landing during another thread's flush
             * falls through to load_and_patch, sees the not-yet-published chunk
             * (the in-flight PUT has not set the mapping), and builds a zeroed
             * have_base=1 entry that overwrites the in-flight data. */
            e->dirty = 1;
            dcache_pin_add(e); /* already held mid-flush; keeps dirty ⟹ pinned */
            dcache_note_dirty_bytes((int64_t)e->len);
        }
        dcache_add_range(e, off, len);
        pthread_mutex_unlock(mu);
        efs_rdcache_invalidate(ino, ci);
        return 0;
    }
    pthread_mutex_unlock(mu);
    return -1;
}

/* First 4k to a chunk: load the published base NOW (one GET), patch the 4k,
 * and cache the full chunk with have_base=1. The old path zero-filled and
 * set have_base=0 for a published chunk, so EVERY flush of that chunk did a
 * 128 KiB GET+merge+PUT (read-modify-write). With a working set over the
 * dcache limit (random 4k on >= 512 MiB) reclaim runs constantly and the
 * writer thread flushes inline (dirty > 2x cap), so pwrite blocked on the
 * GET+PUT — that is what capped fio rw-4k at ~100 MiB/s. Reading the base
 * once here turns later 4k patches into pure memcpy and the flush into a
 * PUT-only. */
static int dcache_load_and_patch(efs_ino_t ino, uint32_t ci, uint32_t off,
                                 const uint8_t *src, uint32_t len, uint32_t cs)
{
    if (dcache_patch(ino, ci, off, src, len) == 0)
        return 0;

    uint8_t *chunk = efs_buf_alloc(cs);
    if (!chunk)
        return -1;

    int have_base = 0;
    if (efs_rdcache_get(ino, ci, chunk, cs) == 0) {
        have_base = 1;
    } else {
        int published = 0;
        pthread_mutex_lock(&g_client.idx_mu);
        published = (efs_export_get_chunk(&g_client.export, ino, ci, NULL) == 0);
        pthread_mutex_unlock(&g_client.idx_mu);
        if (published &&
            efs_client_fetch_published_chunk(ino, ci, chunk, cs) == EFS_OK)
            have_base = 1;
        if (!have_base)
            memset(chunk, 0, cs);
    }
    memcpy(chunk + off, src, len);
    if (dcache_merge_owned(ino, ci, off, src, len, chunk, cs) != 0) {
        efs_buf_free(chunk, cs);
        return -1;
    }
    uint32_t s = dcache_slot(ino, ci);
    pthread_mutex_t *mu = dcache_mu(s);
    pthread_mutex_lock(mu);
    struct dcache_ent *e = dcache_find(s, ino, ci);
    if (e) {
        e->have_base = have_base;
        e->nrange = 0;
        if (!have_base)
            dcache_add_range(e, off, len);
    }
    pthread_mutex_unlock(mu);
    return 0;
}

static void dcache_note_size(efs_ino_t ino, uint64_t end)
{
    efs_client_lock_dir(ino);
    pthread_mutex_lock(&g_client.idx_mu);
    struct efs_inode cur;
    if (efs_export_get_inode(&g_client.export, ino, &cur) == 0) {
        if (cur.size < end) {
            /* Size grow bumps mtime itself. */
            efs_export_set_size_norollup(&g_client.export, ino, end);
            efs_client_mark_ino_dirty(ino);
        } else if (cur.size > 0) {
            /* Overwrite at the same size must still advance the local
             * mtime: reports are newer-only, so a frozen mtime leaves the
             * server at the first writer's mtime and peers never get an
             * adopt trigger to re-pull the layout (mc_stress rwfile
             * identical-content rerun). Field update only — the dirty mark
             * still happens once per flush in dcache_put_now, so rw-4k does
             * not reflood the report path (that flood capped rw-4k before). */
            uint64_t sec;
            uint32_t nsec;
            now_ns(&sec, &nsec);
            if (!efs_client_mtime_is_pinned(ino) &&
                (sec > cur.mtime ||
                 (sec == cur.mtime && nsec > cur.mtime_nsec)))
                efs_export_set_mtime_ns_norollup(&g_client.export, ino,
                                                 sec, nsec);
        }
    }
    pthread_mutex_unlock(&g_client.idx_mu);
    efs_client_unlock_dir(ino);
}

static int dcache_store_full_chunk(efs_ino_t ino, uint32_t ci,
                                   const uint8_t *src, uint32_t cs)
{
    if (dcache_patch(ino, ci, 0, src, cs) == 0)
        return 0;
    uint8_t *chunk = efs_buf_alloc(cs);
    if (!chunk)
        return -1;
    memcpy(chunk, src, cs);
    if (dcache_merge_owned(ino, ci, 0, src, cs, chunk, cs) != 0) {
        efs_buf_free(chunk, cs);
        return -1;
    }
    uint32_t s = dcache_slot(ino, ci);
    pthread_mutex_t *mu = dcache_mu(s);
    pthread_mutex_lock(mu);
    struct dcache_ent *e = dcache_find(s, ino, ci);
    if (e) {
        e->have_base = 1;
        e->nrange = 0;
    }
    pthread_mutex_unlock(mu);
    return 0;
}

int efs_dcache_try_patch(efs_ino_t ino, uint64_t offset, uint32_t len,
                         const uint8_t *src)
{
    if (!src || !len)
        return -1;
    uint32_t cs = data_chunk_size();
    if (cs == 0)
        return -1;
    uint64_t remaining = len;
    uint64_t pos = offset;
    const uint8_t *p = src;
    while (remaining) {
        uint32_t ci = (uint32_t)(pos / cs);
        uint32_t off = (uint32_t)(pos % cs);
        uint32_t n = cs - off;
        if ((uint64_t)n > remaining)
            n = (uint32_t)remaining;
        int rc;
        if (off == 0 && n == cs)
            rc = dcache_store_full_chunk(ino, ci, p, cs);
        else if (dcache_patch(ino, ci, off, p, n) == 0)
            rc = 0;
        else
            rc = dcache_load_and_patch(ino, ci, off, p, n, cs);
        if (rc != 0)
            return -1;
        pos += n;
        p += n;
        remaining -= n;
    }
    dcache_note_size(ino, offset + len);
    return 0;
}

static int dcache_patch_sparse(efs_ino_t ino, uint32_t ci, uint32_t off,
                               const uint8_t *src, uint32_t len)
{
    if (!src || !len)
        return -1;
    uint32_t s = dcache_slot(ino, ci);
    pthread_mutex_t *mu = dcache_mu(s);
    pthread_mutex_lock(mu);
    struct dcache_ent *e = dcache_find(s, ino, ci);
    if (e && e->data && off + len <= e->len) {
        memcpy(e->data + off, src, len);
        if (!e->dirty) {
            e->dirty = 1;
            dcache_pin_add(e);
            dcache_note_dirty_bytes((int64_t)e->len);
        }
        /* A published cached chunk stays have_base=1 so the next close
         * PUTs it without GET. Forcing 0 made every append close RMW a
         * size-lagged zero base and wipe earlier lines. */
        if (!e->have_base)
            dcache_add_range(e, off, len);
        pthread_mutex_unlock(mu);
        efs_rdcache_invalidate(ino, ci);
        return 0;
    }
    pthread_mutex_unlock(mu);
    return -1;
}

static int dcache_load_sparse(efs_ino_t ino, uint32_t ci, uint32_t off,
                              const uint8_t *src, uint32_t len, uint32_t cs)
{
    if (dcache_patch_sparse(ino, ci, off, src, len) == 0)
        return 0;

    uint8_t *chunk = efs_buf_alloc(cs);
    if (!chunk)
        return -1;
    memset(chunk, 0, cs);
    memcpy(chunk + off, src, len);
    if (dcache_merge_owned(ino, ci, off, src, len, chunk, cs) != 0) {
        efs_buf_free(chunk, cs);
        return -1;
    }
    uint32_t s = dcache_slot(ino, ci);
    pthread_mutex_t *mu = dcache_mu(s);
    pthread_mutex_lock(mu);
    struct dcache_ent *e = dcache_find(s, ino, ci);
    if (e) {
        e->have_base = 0;
        e->nrange = 0;
        dcache_add_range(e, off, len);
    }
    pthread_mutex_unlock(mu);
    return 0;
}

int efs_dcache_try_patch_sparse(efs_ino_t ino, uint64_t offset, uint32_t len,
                                const uint8_t *src)
{
    if (!src || !len)
        return -1;
    uint32_t cs = data_chunk_size();
    if (cs == 0)
        return -1;
    uint64_t remaining = len;
    uint64_t pos = offset;
    const uint8_t *p = src;
    while (remaining) {
        uint32_t ci = (uint32_t)(pos / cs);
        uint32_t off = (uint32_t)(pos % cs);
        uint32_t n = cs - off;
        if ((uint64_t)n > remaining)
            n = (uint32_t)remaining;
        if (dcache_load_sparse(ino, ci, off, p, n, cs) != 0)
            return -1;
        pos += n;
        p += n;
        remaining -= n;
    }
    dcache_note_size(ino, offset + len);
    return 0;
}

static int dcache_fill(struct dcache_ent *e, efs_ino_t ino, uint32_t ci,
                       const uint8_t *chunk, uint32_t chunk_size)
{
    int was_dirty = e->dirty && e->data;
    /* Pool buffers are always EFS_CHUNK_SIZE-capacity; only allocate when
     * the slot has none. (The old realloc path also mixed pool/non-pool
     * buffers, which the pool's len-keyed free can't distinguish.) */
    if (!e->data) {
        e->data = efs_buf_alloc(chunk_size);
        if (!e->data)
            return EFS_ERR_NOMEM;
    }
    e->len = chunk_size;
    memcpy(e->data, chunk, chunk_size);
    e->ino = ino;
    e->ci = ci;
    e->dirty = 1;
    e->have_base = 1;
    e->nrange = 0;
    dcache_pin_add(e);
    if (!was_dirty)
        dcache_note_dirty_bytes((int64_t)chunk_size);
    return 0;
}

/* Take ownership of a pool-allocated assemble buffer. Caller must not free
 * `chunk` after success. */
static int dcache_take(struct dcache_ent *e, efs_ino_t ino, uint32_t ci,
                       uint8_t *chunk, uint32_t chunk_size)
{
    int was_dirty = e->dirty && e->data;
    if (e->data && e->data != chunk)
        efs_buf_free(e->data, e->len);
    e->data = chunk;
    e->len = chunk_size;
    e->ino = ino;
    e->ci = ci;
    e->dirty = 1;
    e->have_base = 1;
    e->nrange = 0;
    dcache_pin_add(e);
    if (!was_dirty)
        dcache_note_dirty_bytes((int64_t)chunk_size);
    return 0;
}

/* 0 = cached, 1 = should not happen (chain grows), <0 = error. */
static int dcache_store(efs_ino_t ino, uint32_t ci, const uint8_t *chunk,
                        uint32_t chunk_size)
{
    uint32_t s = dcache_slot(ino, ci);
    pthread_mutex_t *mu = dcache_mu(s);
    pthread_mutex_lock(mu);
    struct dcache_ent *e = dcache_find(s, ino, ci);
    if (e) {
        int rc = dcache_fill(e, ino, ci, chunk, chunk_size);
        pthread_mutex_unlock(mu);
        return rc;
    }
    struct dcache_ent *head = &g_dcache.e[s];
    if (!head->dirty && !head->data) {
        int rc = dcache_fill(head, ino, ci, chunk, chunk_size);
        pthread_mutex_unlock(mu);
        return rc;
    }
    struct dcache_ent *n = calloc(1, sizeof(*n));
    if (!n) {
        pthread_mutex_unlock(mu);
        return EFS_ERR_NOMEM;
    }
    int rc = dcache_fill(n, ino, ci, chunk, chunk_size);
    if (rc != 0) {
        free(n);
        pthread_mutex_unlock(mu);
        return rc;
    }
    n->next = head->next;
    head->next = n;
    pthread_mutex_unlock(mu);
    return 0;
}

/* Like dcache_store, but `chunk` is stolen on success (not copied). */
static int dcache_store_owned(efs_ino_t ino, uint32_t ci, uint8_t *chunk,
                              uint32_t chunk_size)
{
    if (!chunk || !chunk_size)
        return EFS_ERR_INVAL;
    uint32_t s = dcache_slot(ino, ci);
    pthread_mutex_t *mu = dcache_mu(s);
    pthread_mutex_lock(mu);
    struct dcache_ent *e = dcache_find(s, ino, ci);
    if (e) {
        int rc = dcache_take(e, ino, ci, chunk, chunk_size);
        pthread_mutex_unlock(mu);
        return rc;
    }
    struct dcache_ent *head = &g_dcache.e[s];
    if (!head->dirty && !head->data) {
        int rc = dcache_take(head, ino, ci, chunk, chunk_size);
        pthread_mutex_unlock(mu);
        return rc;
    }
    struct dcache_ent *n = calloc(1, sizeof(*n));
    if (!n) {
        pthread_mutex_unlock(mu);
        return EFS_ERR_NOMEM;
    }
    int rc = dcache_take(n, ino, ci, chunk, chunk_size);
    if (rc != 0) {
        free(n);
        pthread_mutex_unlock(mu);
        return rc;
    }
    n->next = head->next;
    head->next = n;
    pthread_mutex_unlock(mu);
    return 0;
}

/* Install a freshly loaded+patched chunk, or fold just [off,len) into a
 * chunk another writer dirtied while we were fetching. Steals `chunk` on
 * success (including the fold path, which frees it). */
static int dcache_merge_owned(efs_ino_t ino, uint32_t ci, uint32_t off,
                              const uint8_t *src, uint32_t len,
                              uint8_t *chunk, uint32_t cs)
{
    uint32_t s = dcache_slot(ino, ci);
    pthread_mutex_t *mu = dcache_mu(s);
    pthread_mutex_lock(mu);
    struct dcache_ent *e = dcache_find(s, ino, ci);
    if (e && e->dirty && e->len >= cs && off + len <= e->len) {
        memcpy(e->data + off, src, len);
        dcache_add_range(e, off, len);
        pthread_mutex_unlock(mu);
        efs_buf_free(chunk, cs);
        efs_rdcache_invalidate(ino, ci);
        return 0;
    }
    pthread_mutex_unlock(mu);
    if (dcache_store_owned(ino, ci, chunk, cs) != 0)
        return -1;
    efs_rdcache_invalidate(ino, ci);
    return 0;
}

static int dcache_flush_slot_inner(uint32_t s, efs_ino_t only_ino, int have_only)
{
    pthread_mutex_t *mu = dcache_mu(s);
    int rc = EFS_OK;
    pthread_mutex_lock(mu);
    struct dcache_ent *e = &g_dcache.e[s];
    while (e) {
        if (!e->dirty || !e->data || (have_only && e->ino != only_ino)) {
            e = e->next;
            continue;
        }
        /* Reclaim (have_only=0) must not GET+PUT an unpublished sparse
         * append chunk. Two such flushes read the same zero base and the
         * last PUT drops the other's ranges (concurrent_appends NULs).
         * Close/fsync pass have_only=1. */
        if (!have_only && !e->have_base) {
            e = e->next;
            continue;
        }
        efs_ino_t ino = e->ino;
        uint32_t ci = e->ci;
        uint32_t len = e->len;
        int have_base = e->have_base;
        uint8_t nrange = e->nrange;
        uint32_t roff[DCACHE_NR], rlen[DCACHE_NR];
        if (!have_base && nrange) {
            memcpy(roff, e->roff, (size_t)nrange * sizeof(uint32_t));
            memcpy(rlen, e->rlen, (size_t)nrange * sizeof(uint32_t));
        }
        uint8_t *copy = efs_buf_alloc(len);
        if (!copy) {
            pthread_mutex_unlock(mu);
            return EFS_ERR_NOMEM;
        }
        memcpy(copy, e->data, len);
        e->dirty = 0;
        /* Un-count now, while the state transition is atomic. The old code
         * decremented only if the entry was still clean after the network
         * PUT; a re-dirty during the PUT re-added a full chunk, so every
         * flush/redirty race leaked 128 KiB into dirty_bytes and pinned it
         * above the reclaim limit (perpetual flush storms). */
        dcache_note_dirty_bytes(-(int64_t)len);
        pthread_mutex_unlock(mu);
        if (!have_base) {
            uint8_t *base = efs_buf_alloc(len);
            if (!base) {
                efs_buf_free(copy, len);
                pthread_mutex_lock(mu);
                if (e->ino == ino && e->ci == ci && e->data && !e->dirty) {
                    e->dirty = 1;
                    dcache_note_dirty_bytes((int64_t)len);
                }
                pthread_mutex_unlock(mu);
                return EFS_ERR_NOMEM;
            }
            int rrc = efs_client_fetch_published_chunk(ino, ci, base, len);
            if (rrc != EFS_OK)
                rrc = efs_client_fetch_published_chunk(ino, ci, base, len);
            if (rrc != EFS_OK) {
                efs_buf_free(base, len);
                efs_buf_free(copy, len);
                pthread_mutex_lock(mu);
                if (e->ino == ino && e->ci == ci && e->data && !e->dirty) {
                    e->dirty = 1;
                    dcache_note_dirty_bytes((int64_t)len);
                }
                if (rc == EFS_OK)
                    rc = rrc;
                e = e->next;
                continue;
            }
            for (uint8_t i = 0; i < nrange; i++) {
                if (roff[i] + rlen[i] <= len)
                    memcpy(base + roff[i], copy + roff[i], rlen[i]);
            }
            memcpy(copy, base, len);
            efs_buf_free(base, len);
        }
        int prc = dcache_put_now(ino, ci, copy, len);
        pthread_mutex_lock(mu);
        if (prc != EFS_OK) {
            efs_buf_free(copy, len);
            if (rc == EFS_OK)
                rc = prc;
            /* PUT failed: this slot is the only copy. Keep it dirty. */
            if (e->ino == ino && e->ci == ci && e->data && !e->dirty) {
                e->dirty = 1;
                dcache_note_dirty_bytes((int64_t)len);
            }
            e = e->next;
            continue;
        }
        /* Keep the published bytes as have_base=1. Freeing the slot made
         * the next close GET a merge base; efs_client_read treats a lagging
         * inode.size as EOF and returns zeros, so that GET+PUT wiped
         * earlier append lines (concurrent_appends NULs). */
        if (e->ino == ino && e->ci == ci && e->data && e->len >= len) {
            if (!e->dirty) {
                memcpy(e->data, copy, len);
                e->nrange = 0;
            }
            e->have_base = 1;
        } else if (!e->dirty && e->ino == ino && e->ci == ci) {
            dcache_pin_release(e);
            efs_buf_free(e->data, e->len);
            e->data = NULL;
            e->len = 0;
            e->ino = 0;
            e->ci = 0;
        }
        efs_buf_free(copy, len);
        e = e->next;
    }
    pthread_mutex_unlock(mu);
    return rc;
}

/* Hold the shard I/O lock across the whole flush so the base-read + PUT of
 * one flush completes before a concurrent flush of the same shard reads its
 * merge base. Lock order is shard_io -> dcache_mu (never the reverse), and
 * the base-read's efs_client_read only takes dcache_mu, so no deadlock. */
static int dcache_flush_slot(uint32_t s, efs_ino_t only_ino, int have_only)
{
    pthread_mutex_t *io = dcache_io_mu(s);
    pthread_mutex_lock(io);
    int rc = dcache_flush_slot_inner(s, only_ino, have_only);
    pthread_mutex_unlock(io);
    return rc;
}

/* Close/truncate used to walk all 65536 slots (and 64 shard locks) per
 * file. ecopy hit that on flush+release+ftruncate (~200k locks/file).
 * Chunks are dense from 0, so only those hashed slots can hold this ino. */
static int dcache_flush_all_slots(efs_ino_t only_ino, int have_only)
{
    int rc = EFS_OK;
    pthread_once(&g_dcache_once, dcache_init);
    for (uint32_t i = 0; i < DCACHE_SLOTS; i++) {
        int prc = dcache_flush_slot(i, only_ino, have_only);
        if (prc != EFS_OK && rc == EFS_OK)
            rc = prc;
    }
    return rc;
}

int efs_dcache_flush_ino(efs_ino_t ino)
{
    uint32_t cs = data_chunk_size();
    uint32_t nci = 1;
    struct efs_inode inode;
    /* idx_mu: create dual-apply reindexes (idx_init frees ino_keys).
     * Close without this lock raced that free → SIGSEGV → ENOTCONN
     * mid-ImageNet copy on node9901. */
    pthread_mutex_lock(&g_client.idx_mu);
    int have = (cs && efs_export_get_inode(&g_client.export, ino, &inode) == 0 &&
                inode.size > 0);
    pthread_mutex_unlock(&g_client.idx_mu);
    if (have) {
        uint64_t n = (inode.size + (uint64_t)cs - 1) / (uint64_t)cs;
        if (n > UINT32_MAX)
            return dcache_flush_all_slots(ino, 1);
        nci = (uint32_t)n;
        if (nci == 0)
            nci = 1;
    }
    /* Hashed slots are exact for dense ci 0..nci-1. Walking all 65536
     * slots was the old fallback once nci > 4096 (a 512 MiB file) and
     * made 1G fsync take tens of seconds. Only scan the table when
     * hashing every ci would touch more slots than exist. */
    if (nci > DCACHE_SLOTS)
        return dcache_flush_all_slots(ino, 1);

    int rc = EFS_OK;
    pthread_once(&g_dcache_once, dcache_init);
    for (uint32_t ci = 0; ci < nci; ci++) {
        int prc = dcache_flush_slot(dcache_slot(ino, ci), ino, 1);
        if (prc != EFS_OK && rc == EFS_OK)
            rc = prc;
    }
    return rc;
}

int efs_dcache_flush_all(void)
{
    return dcache_flush_all_slots(0, 0);
}

/* Background incremental reclaim. The old maybe_reclaim ran a full
 * efs_dcache_flush_all() on the WRITER thread once dirty_bytes crossed the
 * limit: a 65536-slot walk with a synchronous GET+PUT per dirty chunk. With
 * a working set at/over the limit (random 4k on >= 2 GiB files) that fired
 * constantly — flush_all was 41% of efs-fuse CPU and chunks were re-PUT ~4x
 * per test as writers re-dirtied slots mid-walk. Now writers just signal;
 * a small pool sweeps slots round-robin until dirty_bytes is back under the
 * low-water mark, so drain cost is spread and parallel instead of a stall. */
#define DCACHE_RECLAIM_THREADS 16
#define DCACHE_RECLAIM_SCAN    64 /* slots per worker wake */
static struct {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    uint32_t cursor;
    uint64_t lim;
    int shutdown;
    int active;
} g_reclaim = {
    .mu = PTHREAD_MUTEX_INITIALIZER,
    .cv = PTHREAD_COND_INITIALIZER,
};
static pthread_once_t g_reclaim_once = PTHREAD_ONCE_INIT;

static uint64_t dcache_reclaim_limit(void)
{
    /* 2 GiB default. The old 512 MiB ("push data down the pipe sooner") let
     * only ~4096 chunks (512 MiB / 128 KiB) stay cached, so a random-write
     * working set larger than that (fio rw-4k on 2 GiB files = 16384 chunks)
     * churned reclaim: every eviction of a partially-written chunk forced a
     * 128 KiB read-modify-write (GET the published base + PUT the chunk back),
     * ~32-64x amplification that capped rw-4k at ~100 MiB/s. At 2 GiB the
     * working set stays cached, repeat 4k writes to a chunk coalesce in place,
     * and the flush is a single PUT — rw-4k recovers to ~1.5 GB/s. The window
     * still only needs to absorb burst jitter, and the dcache is hard-capped
     * at 8 GiB (65536 slots x 128 KiB) regardless. */
    uint64_t lim = 2ull << 30;
    const char *env = getenv("EFS_DCACHE_BYTES");
    if (env && *env) {
        char *end = NULL;
        unsigned long long v = strtoull(env, &end, 10);
        if (end != env && v >= (1ull << 20) && v <= (4ull << 30))
            lim = (uint64_t)v;
    }
    return lim;
}

static void *dcache_reclaim_main(void *arg)
{
    (void)arg;
    pthread_mutex_lock(&g_reclaim.mu);
    for (;;) {
        while (!g_reclaim.shutdown &&
               __atomic_load_n(&g_dcache.dirty_bytes, __ATOMIC_RELAXED) <=
                   g_reclaim.lim)
            pthread_cond_wait(&g_reclaim.cv, &g_reclaim.mu);
        if (g_reclaim.shutdown)
            break;
        g_reclaim.active++;
        pthread_mutex_unlock(&g_reclaim.mu);

        uint64_t low = g_reclaim.lim - g_reclaim.lim / 8;
        for (int i = 0; i < DCACHE_RECLAIM_SCAN; i++) {
            if (__atomic_load_n(&g_dcache.dirty_bytes, __ATOMIC_RELAXED) <=
                low)
                break;
            uint32_t s = __atomic_fetch_add(&g_reclaim.cursor, 1,
                                            __ATOMIC_RELAXED) &
                         (DCACHE_SLOTS - 1);
            (void)dcache_flush_slot(s, 0, 0);
        }
        pthread_mutex_lock(&g_reclaim.mu);
        g_reclaim.active--;
        pthread_cond_broadcast(&g_reclaim.cv);
    }
    pthread_mutex_unlock(&g_reclaim.mu);
    return NULL;
}

static void dcache_reclaim_start(void)
{
    g_reclaim.lim = dcache_reclaim_limit();
    for (int i = 0; i < DCACHE_RECLAIM_THREADS; i++) {
        pthread_t t;
        if (pthread_create(&t, NULL, dcache_reclaim_main, NULL) != 0) {
            g_reclaim.shutdown = 1;
            pthread_cond_broadcast(&g_reclaim.cv);
            return;
        }
        pthread_detach(t);
    }
}

void efs_dcache_maybe_reclaim(void)
{
    /* getenv per write walks environ on the hot path; the limit is fixed
     * for the process, so read it once (idempotent benign race). */
    static uint64_t lim;
    if (!lim)
        lim = dcache_reclaim_limit();
    uint64_t dirty =
        __atomic_load_n(&g_dcache.dirty_bytes, __ATOMIC_RELAXED);
    if (dirty <= lim)
        return;
    pthread_once(&g_reclaim_once, dcache_reclaim_start);
    pthread_mutex_lock(&g_reclaim.mu);
    pthread_cond_signal(&g_reclaim.cv);
    pthread_mutex_unlock(&g_reclaim.mu);
    /* Hard cap at 2x: the background pool is drain-limited (a sparse-entry
     * flush is a GET+PUT at disk latency), so writers that outpace it must
     * help — otherwise a >>limit working set grows the dcache without bound.
     * A few slots inline is backpressure, not the old full-table stall. */
    if (dirty > lim * 2) {
        for (int i = 0; i < 8; i++) {
            uint32_t s = __atomic_fetch_add(&g_reclaim.cursor, 1,
                                            __ATOMIC_RELAXED) &
                         (DCACHE_SLOTS - 1);
            (void)dcache_flush_slot(s, 0, 0);
            if (__atomic_load_n(&g_dcache.dirty_bytes, __ATOMIC_RELAXED) <=
                lim * 2)
                break;
        }
    }
}

/* Stop the reclaim pool before shutdown-time state (conns, export) goes
 * away; an in-flight flush would otherwise touch freed client state. Waits
 * until no worker is mid-flush. */
void efs_dcache_reclaim_stop(void)
{
    pthread_mutex_lock(&g_reclaim.mu);
    g_reclaim.shutdown = 1;
    pthread_cond_broadcast(&g_reclaim.cv);
    while (g_reclaim.active > 0)
        pthread_cond_wait(&g_reclaim.cv, &g_reclaim.mu);
    pthread_mutex_unlock(&g_reclaim.mu);
}

/* Persistent PUT workers draining a shared FIFO of chunk jobs. Reentrant:
 * each put_pool_run enqueues its jobs tagged with a per-batch tracker and
 * waits on that tracker, so concurrent writers (WB workers) all stay in flight
 * instead of racing on a single batch slot. Queue depth spans several batches
 * so enqueuers rarely block. */
#define PUT_POOL_QDEPTH (8 * EFS_WRITE_PIPELINE)
static struct {
    pthread_mutex_t mu;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
    struct chunk_put_job *q[PUT_POOL_QDEPTH];
    int head, tail, count;
    pthread_t tids[EFS_WRITE_PIPELINE];
    int nworkers;
    int ready;
    int shutdown;
} g_put_pool = {
    .mu = PTHREAD_MUTEX_INITIALIZER,
    .not_empty = PTHREAD_COND_INITIALIZER,
    .not_full = PTHREAD_COND_INITIALIZER,
};

static void *put_pool_thread(void *arg)
{
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&g_put_pool.mu);
        while (g_put_pool.count == 0 && !g_put_pool.shutdown)
            pthread_cond_wait(&g_put_pool.not_empty, &g_put_pool.mu);
        if (g_put_pool.shutdown && g_put_pool.count == 0) {
            pthread_mutex_unlock(&g_put_pool.mu);
            return NULL;
        }
        struct chunk_put_job *job = g_put_pool.q[g_put_pool.head];
        g_put_pool.head = (g_put_pool.head + 1) % PUT_POOL_QDEPTH;
        g_put_pool.count--;
        pthread_cond_signal(&g_put_pool.not_full);
        pthread_mutex_unlock(&g_put_pool.mu);

        chunk_put_worker(job);

        struct put_batch *bp = job->bp;
        pthread_mutex_lock(&bp->mu);
        if (--bp->remaining == 0)
            pthread_cond_signal(&bp->cv);
        pthread_mutex_unlock(&bp->mu);
    }
}

static int put_pool_ensure(void)
{
    if (g_put_pool.ready)
        return 0;
    pthread_mutex_lock(&g_put_pool.mu);
    if (!g_put_pool.ready) {
        uint32_t n = EFS_WRITE_PIPELINE;
        for (uint32_t i = 0; i < n; i++) {
            if (pthread_create(&g_put_pool.tids[i], NULL, put_pool_thread,
                               NULL) != 0) {
                g_put_pool.shutdown = 1;
                pthread_cond_broadcast(&g_put_pool.not_empty);
                pthread_mutex_unlock(&g_put_pool.mu);
                for (uint32_t j = 0; j < i; j++)
                    pthread_join(g_put_pool.tids[j], NULL);
                g_put_pool.shutdown = 0;
                return -1;
            }
        }
        g_put_pool.nworkers = (int)n;
        g_put_pool.ready = 1;
    }
    pthread_mutex_unlock(&g_put_pool.mu);
    return 0;
}

static int put_pool_run(struct chunk_put_job *jobs, uint32_t batch)
{
    if (batch == 0)
        return 0;
    if (batch == 1 || put_pool_ensure() != 0) {
        for (uint32_t i = 0; i < batch; i++)
            chunk_put_worker(&jobs[i]);
        return 0;
    }
    struct put_batch bp;
    pthread_mutex_init(&bp.mu, NULL);
    pthread_cond_init(&bp.cv, NULL);
    bp.remaining = (int)batch;

    pthread_mutex_lock(&g_put_pool.mu);
    for (uint32_t i = 0; i < batch; i++) {
        jobs[i].bp = &bp;
        while (g_put_pool.count == PUT_POOL_QDEPTH && !g_put_pool.shutdown)
            pthread_cond_wait(&g_put_pool.not_full, &g_put_pool.mu);
        g_put_pool.q[g_put_pool.tail] = &jobs[i];
        g_put_pool.tail = (g_put_pool.tail + 1) % PUT_POOL_QDEPTH;
        g_put_pool.count++;
        pthread_cond_signal(&g_put_pool.not_empty);
    }
    pthread_mutex_unlock(&g_put_pool.mu);

    pthread_mutex_lock(&bp.mu);
    while (bp.remaining > 0)
        pthread_cond_wait(&bp.cv, &bp.mu);
    pthread_mutex_unlock(&bp.mu);
    pthread_mutex_destroy(&bp.mu);
    pthread_cond_destroy(&bp.cv);
    return 0;
}

static void *chunk_put_worker(void *arg)
{
    struct chunk_put_job *job = arg;
    uint32_t chunk_size = data_chunk_size();
    uint32_t frag_len = data_frag_size();
    uint64_t chunk_start = (uint64_t)job->ci * chunk_size;
    uint64_t wr_start = (job->offset > chunk_start) ? job->offset : chunk_start;
    uint64_t wr_end = (job->end < chunk_start + chunk_size)
                          ? job->end
                          : chunk_start + chunk_size;
    int covers_full = (wr_start == chunk_start &&
                       wr_end == chunk_start + chunk_size);

    /* dd if=/dev/zero / full-chunk zeros: skip assemble/encode/malloc and PUT
     * shared zero pages with the cached zero digest. */
    if (covers_full) {
        size_t src_off = (size_t)(wr_start - job->offset);
        if (efs_bytes_are_zero(job->buf + src_off, chunk_size)) {
            efs_hash_zero_fragment_len(frag_len, job->checksums[0]);
            efs_hash_zero_fragment_len(frag_len, job->checksums[1]);
            efs_hash_zero_fragment_len(frag_len, job->checksums[2]);
            efs_place_fragments(g_client.nodes, g_client.node_count, job->ino,
                                job->ci, job->nodes);
            /* Do not PUT all-zero fragments. The chunk table stores the
             * well-known zero digest; decode synthesizes zeros on read.
             * dd if=/dev/zero was spending ~1 GB/s on 2-ack PUTs of zeros. */
            efs_dcache_drop(job->ino, job->ci);
            job->rc = EFS_OK;
            return NULL;
        }

        /* Full overwrite: encode in place from the caller's buffer — skip the
         * extra 128 KiB assemble memcpy that showed up as 8–10% of seq write.
         * Parity scratch is a per-thread static (default geometry): one less
         * malloc/free per chunk PUT. */
        static __thread uint8_t parity_tls[EFS_FRAGMENT_SIZE];
        uint8_t *parity = parity_tls;
        if (frag_len > EFS_FRAGMENT_SIZE) {
            parity = malloc(frag_len);
            if (!parity) {
                job->rc = EFS_ERR_NOMEM;
                return NULL;
            }
        }
        uint8_t *frags[EFS_NUM_FRAGMENTS];
        const uint8_t *src = (const uint8_t *)job->buf + src_off;
        frags[0] = (uint8_t *)(uintptr_t)src;
        frags[1] = (uint8_t *)(uintptr_t)(src + frag_len);
        frags[2] = parity;
        efs_encode_chunk(src, chunk_size, chunk_size, frags);
        const uint8_t *cfrags2[EFS_NUM_FRAGMENTS] = {frags[0], frags[1], frags[2]};
        hash_write_fragments(cfrags2, frag_len, chunk_size, 0, chunk_start,
                             wr_start, wr_end, job->checksums);
        efs_place_fragments(g_client.nodes, g_client.node_count, job->ino,
                            job->ci, job->nodes);
        job->rc = efs_client_put_fragments_parallel(
            job->ino, job->ci, job->nodes, cfrags2, frag_len, job->checksums);
        if (frag_len > EFS_FRAGMENT_SIZE)
            free(parity);
        efs_rdcache_invalidate(job->ino, job->ci);
        if (job->rc == EFS_OK)
            efs_dcache_drop(job->ino, job->ci);
        else
            (void)dcache_store(job->ino, job->ci, src, chunk_size);
        return NULL;
    }

    /* Pool-allocate the assemble buffer (malloc churn here was the arena
     * bloat source); frag scratch is a per-thread static for the default
     * geometry. Heap, not stack: large chunks exceed some FUSE/pthread
     * stacks when the worker runs inline on a FUSE thread. */
    static __thread uint8_t frag_tls[EFS_NUM_FRAGMENTS * EFS_FRAGMENT_SIZE];
    uint8_t *chunk = efs_buf_alloc(chunk_size);
    uint8_t *frag_buf = frag_tls;
    uint8_t *frags[EFS_NUM_FRAGMENTS];
    if (frag_len > EFS_FRAGMENT_SIZE)
        frag_buf = malloc(EFS_NUM_FRAGMENTS * frag_len);
    if (!chunk || !frag_buf) {
        efs_buf_free(chunk, chunk_size);
        if (frag_buf != frag_tls)
            free(frag_buf);
        job->rc = EFS_ERR_NOMEM;
        return NULL;
    }
    frag_ptrs(frag_buf, frag_len, frags);

    int from_zero = 0;
    int arc = assemble_write_chunk(job->ino, job->old_size, job->offset,
                                   job->buf, chunk_start, wr_start, wr_end,
                                   chunk_size, chunk, &from_zero);
    if (arc != EFS_OK) {
        efs_buf_free(chunk, chunk_size);
        if (frag_buf != frag_tls)
            free(frag_buf);
        job->rc = arc;
        return NULL;
    }

    const uint8_t *cfrags[EFS_NUM_FRAGMENTS];
    for (int i = 0; i < EFS_NUM_FRAGMENTS; i++)
        cfrags[i] = frags[i];
    efs_encode_chunk(chunk, chunk_size, chunk_size, frags);
    hash_write_fragments(cfrags, frag_len, chunk_size, from_zero, chunk_start,
                         wr_start, wr_end, job->checksums);
    efs_place_fragments(g_client.nodes, g_client.node_count, job->ino, job->ci,
                        job->nodes);
    job->rc = efs_client_put_fragments_parallel(job->ino, job->ci, job->nodes,
                                                cfrags, frag_len, job->checksums);
    efs_rdcache_invalidate(job->ino, job->ci);
    if (job->rc != EFS_OK)
        (void)dcache_store(job->ino, job->ci, chunk, chunk_size);
    else
        efs_dcache_drop(job->ino, job->ci);
    efs_buf_free(chunk, chunk_size);
    if (frag_buf != frag_tls)
        free(frag_buf);
    return NULL;
}

static int write_chunks_no_replicate(efs_ino_t ino, uint64_t offset, size_t size,
                                     const char *buf, uint64_t old_size);


int efs_client_write(efs_ino_t ino, uint64_t offset, size_t size, const char *buf)
{
    efs_client_mtime_unpin(ino);
    if (size == 0)
        return EFS_OK;

    efs_client_lock_dir(ino);
    pthread_mutex_lock(&g_client.idx_mu);
    struct efs_inode inode;
    if (efs_export_get_inode(&g_client.export, ino, &inode) != 0) {
        pthread_mutex_unlock(&g_client.idx_mu);
        efs_client_unlock_dir(ino);
        return EFS_ERR_NOT_FOUND;
    }
    uint64_t old_size = inode.size;
    pthread_mutex_unlock(&g_client.idx_mu);
    efs_client_unlock_dir(ino);

    uint64_t end = offset + size;
    uint32_t chunk_size = data_chunk_size();
    uint32_t frag_len = data_frag_size();
    /* Only touch chunks that overlap [offset, end). Rewriting every prior
     * chunk on each append was an O(n^2) amplification of sequential writes. */
    uint32_t first_ci = (uint32_t)(offset / chunk_size);
    uint32_t last_ci = (uint32_t)((end - 1) / chunk_size);

    for (uint32_t ci = first_ci; ci <= last_ci; ci++) {
        uint64_t chunk_start = (uint64_t)ci * chunk_size;
        uint64_t wr_start = (offset > chunk_start) ? offset : chunk_start;
        uint64_t wr_end = (end < chunk_start + chunk_size) ? end
                                                              : chunk_start + chunk_size;

        static __thread uint8_t frag_tls[EFS_NUM_FRAGMENTS * EFS_FRAGMENT_SIZE];
        uint8_t *chunk = efs_buf_alloc(chunk_size);
        uint8_t *frag_buf = frag_tls;
        uint8_t *frags[EFS_NUM_FRAGMENTS];
        if (frag_len > EFS_FRAGMENT_SIZE)
            frag_buf = malloc(EFS_NUM_FRAGMENTS * frag_len);
        if (!chunk || !frag_buf) {
            efs_buf_free(chunk, chunk_size);
            if (frag_buf != frag_tls)
                free(frag_buf);
            return EFS_ERR_NOMEM;
        }
        frag_ptrs(frag_buf, frag_len, frags);
        int from_zero = 0;
        int arc = assemble_write_chunk(ino, old_size, offset, buf,
                                       chunk_start, wr_start, wr_end,
                                       chunk_size, chunk, &from_zero);
        if (arc != EFS_OK) {
            efs_buf_free(chunk, chunk_size);
            if (frag_buf != frag_tls)
                free(frag_buf);
            return arc;
        }

        efs_encode_chunk(chunk, chunk_size, chunk_size, frags);

        efs_node_id_t nodes[EFS_NUM_FRAGMENTS];
        efs_place_fragments(g_client.nodes, g_client.node_count, ino, ci, nodes);

        uint8_t checksums[EFS_NUM_FRAGMENTS][EFS_HASH_SIZE];
        const uint8_t *cfrags[EFS_NUM_FRAGMENTS];
        for (int i = 0; i < EFS_NUM_FRAGMENTS; i++)
            cfrags[i] = frags[i];
        hash_write_fragments(cfrags, frag_len, chunk_size, from_zero, chunk_start,
                             wr_start, wr_end, checksums);

        int rc = efs_client_put_fragments_parallel(ino, ci, nodes, cfrags, frag_len,
                                                   checksums);
        efs_buf_free(chunk, chunk_size);
        if (frag_buf != frag_tls)
            free(frag_buf);
        if (rc != EFS_OK)
            return rc;

        if (!export_chunk_exists(ino, ci)) {
            int room = efs_client_ensure_meta_room(0, 1);
            if (room != EFS_OK)
                return room;
        }
        if (efs_export_needs_chunk_grow(&g_client.export))
            export_reserve_chunks_locked(4096);
        efs_client_lock_dir(ino);
        pthread_mutex_lock(&g_client.idx_mu);
        efs_export_set_chunk(&g_client.export, ino, ci, nodes, checksums);
        pthread_mutex_unlock(&g_client.idx_mu);
        efs_client_unlock_dir(ino);
        efs_client_mark_chunk_dirty(ino, ci);
    }

    efs_client_lock_dir(ino);
    pthread_mutex_lock(&g_client.idx_mu);
    /* Grow size (sets mtime) OR bump mtime alone — never both (each used to
     * run sync_hardlink_attrs + parent rollups). Skip rollups until flush. */
    struct efs_inode cur;
    if (efs_export_get_inode(&g_client.export, ino, &cur) == 0 && cur.size < end) {
        efs_export_set_size_norollup(&g_client.export, ino, end);
    } else {
        uint64_t sec;
        uint32_t nsec;
        now_ns(&sec, &nsec);
        efs_export_set_mtime_ns_norollup(&g_client.export, ino, sec, nsec);
    }
    pthread_mutex_unlock(&g_client.idx_mu);
    efs_client_mark_ino_dirty(ino);
    efs_client_unlock_dir(ino);
    /* Phase 2b: report dirty chunk/size to the primary instead of blob-flush. */
    efs_client_report_dirty(0);

    return EFS_OK;
}

int efs_client_write_no_replicate(efs_ino_t ino, uint64_t offset, size_t size, const char *buf)
{
    efs_client_mtime_unpin(ino);
    if (size == 0)
        return EFS_OK;

    efs_client_lock_dir(ino);
    pthread_mutex_lock(&g_client.idx_mu);
    struct efs_inode inode;
    if (efs_export_get_inode(&g_client.export, ino, &inode) != 0) {
        pthread_mutex_unlock(&g_client.idx_mu);
        efs_client_unlock_dir(ino);
        return EFS_ERR_NOT_FOUND;
    }
    uint64_t old_size = inode.size;
    pthread_mutex_unlock(&g_client.idx_mu);
    efs_client_unlock_dir(ino);

    /* Pack staging is gone with the old engine (step 11): the KV row has
     * no pack fields and chunks cannot publish under a directory ino, so
     * small writes take the normal chunk path. */
    return write_chunks_no_replicate(ino, offset, size, buf, old_size);
}

static int write_chunks_no_replicate(efs_ino_t ino, uint64_t offset, size_t size,
                                     const char *buf, uint64_t old_size)
{

    uint64_t end = offset + size;
    uint32_t chunk_size = data_chunk_size();
    uint32_t first_ci = (uint32_t)(offset / chunk_size);
    uint32_t last_ci = (uint32_t)((end - 1) / chunk_size);
    uint32_t nchunks = last_ci - first_ci + 1;

    /* Pipeline independent chunk PUTs (each already fans out 3 fragments). */
    uint32_t pipe = EFS_WRITE_PIPELINE;
    if (pipe < 1)
        pipe = 1;
    if (pipe > nchunks)
        pipe = nchunks;

    for (uint32_t base = first_ci; base <= last_ci; ) {
        uint32_t batch = last_ci - base + 1;
        if (batch > pipe)
            batch = pipe;

        struct chunk_put_job jobs[EFS_WRITE_PIPELINE];

        for (uint32_t i = 0; i < batch; i++) {
            memset(&jobs[i], 0, sizeof(jobs[i]));
            jobs[i].ino = ino;
            jobs[i].ci = base + i;
            jobs[i].old_size = old_size;
            jobs[i].offset = offset;
            jobs[i].buf = buf;
            jobs[i].end = end;
            jobs[i].rc = EFS_ERR_IO;
            jobs[i].deferred = 0;
        }
        put_pool_run(jobs, batch);

        for (uint32_t i = 0; i < batch; i++) {
            if (jobs[i].rc != EFS_OK)
                return jobs[i].rc;
            if (jobs[i].deferred)
                continue;
            if (!export_chunk_exists(ino, jobs[i].ci)) {
                int room = efs_client_ensure_meta_room(0, 1);
                if (room != EFS_OK)
                    return room;
            }
            if (efs_export_needs_chunk_grow(&g_client.export))
                export_reserve_chunks_locked(4096);
            efs_client_lock_dir(ino);
            pthread_mutex_lock(&g_client.idx_mu);
            {
                struct efs_chunk_entry prev;
                int have = (efs_export_get_chunk(&g_client.export, ino,
                                                 jobs[i].ci, &prev) == 0);
                int same_nodes = have &&
                    memcmp(prev.fragment_nodes, jobs[i].nodes,
                           sizeof(prev.fragment_nodes)) == 0;
                int same_ck = have &&
                    memcmp(prev.checksums, jobs[i].checksums,
                           sizeof(prev.checksums)) == 0;
                if (!same_nodes || !same_ck)
                    efs_export_set_chunk(&g_client.export, ino, jobs[i].ci,
                                         jobs[i].nodes, jobs[i].checksums);
                pthread_mutex_unlock(&g_client.idx_mu);
                efs_client_unlock_dir(ino);
                /* Placement change must reach the primary. Checksum-only
                 * overwrite of an existing stripe is already on disk; reporting
                 * it dirtied 32k recs per fio file, flushed a 100MB table, and
                 * made every peer rebuild under s->lock (sw-1m → ~140 MiB/s). */
                if (!same_nodes)
                    efs_client_mark_chunk_dirty(ino, jobs[i].ci);
            }
        }
        base += batch;
    }

    efs_client_lock_dir(ino);
    pthread_mutex_lock(&g_client.idx_mu);
    /* Grow the size based on the current value under the lock, not the stale
     * copy read before the (unlocked) write loop, so concurrent writes on
     * different FUSE worker threads cannot shrink or mis-set the size.
     * Size grow already bumps mtime — do not also call set_mtime (that was
     * a double sync_hardlink_attrs + rollup walk per write job). */
    struct efs_inode cur;
    if (efs_export_get_inode(&g_client.export, ino, &cur) == 0 && cur.size < end) {
        efs_export_set_size_norollup(&g_client.export, ino, end);
    } else {
        uint64_t sec;
        uint32_t nsec;
        now_ns(&sec, &nsec);
        efs_export_set_mtime_ns_norollup(&g_client.export, ino, sec, nsec);
    }
    pthread_mutex_unlock(&g_client.idx_mu);
    efs_client_mark_ino_dirty(ino);
    efs_client_unlock_dir(ino);

    return EFS_OK;
}
