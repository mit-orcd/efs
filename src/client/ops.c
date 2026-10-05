#include "client_internal.h"
#include "efs/protocol.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <execinfo.h>

static uint32_t data_chunk_size(void)
{
    uint32_t cs = g_client.export.chunk_size;
    return efs_chunk_size_valid(cs) ? cs : EFS_DEFAULT_CHUNK_SIZE;
}

static uint64_t now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec;
}

static void now_ns(uint64_t *sec, uint32_t *nsec)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    *sec = (uint64_t)ts.tv_sec;
    *nsec = (uint32_t)ts.tv_nsec;
}

void efs_client_ensure_dir_locks(void)
{
    if (g_client.dir_locks_ready)
        return;
    static pthread_mutex_t init_mu = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&init_mu);
    if (!g_client.dir_locks_ready) {
        for (int i = 0; i < EFS_DIR_LOCKS; i++)
            pthread_mutex_init(&g_client.dir_lock[i], NULL);
        pthread_mutex_init(&g_client.dirty_mu, NULL);
        pthread_mutex_init(&g_client.idx_mu, NULL);
        g_client.dir_locks_ready = 1;
    }
    pthread_mutex_unlock(&init_mu);
}

static int dir_stripe(efs_ino_t parent)
{
    return (int)((uint64_t)parent % EFS_DIR_LOCKS);
}

/* The dir locks are plain (non-recursive) mutexes and the client flush thread
 * takes every stripe at once, so a stripe taken twice on one thread — or one
 * left held when an op returns — wedges the whole mount. Stripes are shared
 * by ino modulo, so nesting two *different* inodes can collide too. Track what
 * this thread holds and say so loudly instead of deadlocking silently. */
static __thread unsigned char t_dir_held[EFS_DIR_LOCKS];

int efs_client_dir_locks_held(void);

static void dir_lock_report(const char *what, int stripe, efs_ino_t parent)
{
    static int reported;
    fprintf(stderr, "DIRLOCK-%s stripe=%d ino=%llu\n", what, stripe,
            (unsigned long long)parent);
    if (__atomic_fetch_add(&reported, 1, __ATOMIC_RELAXED) < 8) {
        void *frames[32];
        int nf = backtrace(frames, 32);
        backtrace_symbols_fd(frames, nf, STDERR_FILENO);
    }
}

void efs_client_lock_dir(efs_ino_t parent)
{
    int s = dir_stripe(parent);
    efs_client_ensure_dir_locks();
    if (t_dir_held[s])
        dir_lock_report("RECURSE", s, parent);
    else if (efs_client_dir_locks_held())
        dir_lock_report("NEST", s, parent);
    pthread_mutex_lock(&g_client.dir_lock[s]);
    t_dir_held[s]++;
}

void efs_client_unlock_dir(efs_ino_t parent)
{
    int s = dir_stripe(parent);
    if (!t_dir_held[s])
        dir_lock_report("UNDERFLOW", s, parent);
    else
        t_dir_held[s]--;
    pthread_mutex_unlock(&g_client.dir_lock[s]);
}

/* Non-zero when this thread still holds a stripe; a FUSE op that returns
 * with one held has leaked it. */
int efs_client_dir_locks_held(void)
{
    int n = 0;
    for (int i = 0; i < EFS_DIR_LOCKS; i++)
        n += t_dir_held[i];
    return n;
}

void efs_client_lock_dirs2(efs_ino_t a, efs_ino_t b)
{
    efs_client_ensure_dir_locks();
    int ia = dir_stripe(a), ib = dir_stripe(b);
    if (ia == ib) {
        pthread_mutex_lock(&g_client.dir_lock[ia]);
        t_dir_held[ia]++;
        return;
    }
    if (ia < ib) {
        pthread_mutex_lock(&g_client.dir_lock[ia]);
        pthread_mutex_lock(&g_client.dir_lock[ib]);
    } else {
        pthread_mutex_lock(&g_client.dir_lock[ib]);
        pthread_mutex_lock(&g_client.dir_lock[ia]);
    }
    t_dir_held[ia]++;
    t_dir_held[ib]++;
}

void efs_client_unlock_dirs2(efs_ino_t a, efs_ino_t b)
{
    int ia = dir_stripe(a), ib = dir_stripe(b);
    if (ia == ib) {
        t_dir_held[ia]--;
        pthread_mutex_unlock(&g_client.dir_lock[ia]);
        return;
    }
    t_dir_held[ia]--;
    t_dir_held[ib]--;
    pthread_mutex_unlock(&g_client.dir_lock[ia]);
    pthread_mutex_unlock(&g_client.dir_lock[ib]);
}

void efs_client_lock_all_dirs(void)
{
    efs_client_ensure_dir_locks();
    for (int i = 0; i < EFS_DIR_LOCKS; i++) {
        pthread_mutex_lock(&g_client.dir_lock[i]);
        t_dir_held[i]++;
    }
}

void efs_client_unlock_all_dirs(void)
{
    for (int i = EFS_DIR_LOCKS - 1; i >= 0; i--) {
        t_dir_held[i]--;
        pthread_mutex_unlock(&g_client.dir_lock[i]);
    }
}

void efs_client_table_lock(void)
{
    pthread_mutex_lock(&g_client.lock);
    efs_client_lock_all_dirs();
}

void efs_client_table_unlock(void)
{
    efs_client_unlock_all_dirs();
    pthread_mutex_unlock(&g_client.lock);
}

int efs_client_set_chunk(struct efs_export *ex, efs_ino_t ino,
                         uint32_t chunk_index,
                         const efs_node_id_t fragment_nodes[EFS_NUM_FRAGMENTS],
                         const uint8_t checksums[EFS_NUM_FRAGMENTS][EFS_HASH_SIZE])
{
    efs_dcache_yield_extra(ino, chunk_index);
    return efs_export_set_chunk(ex, ino, chunk_index, fragment_nodes,
                                checksums);
}

int efs_client_ensure_meta_room(uint64_t extra_inodes, uint64_t extra_chunks)
{
    (void)extra_inodes;
    (void)extra_chunks;
    if (g_client.write_readonly) {
        g_client.last_err = EFS_ERR_BUSY;
        return EFS_ERR_BUSY;
    }
    /* The staging table is a bounded cache, not the capacity limit: quota
     * is enforced server-side. If the last evictor pass saw the table over
     * EFS_CLIENT_META_MB this nudges it to run now instead of at the next
     * 1 s tick; it is a no-op when the table is under the cap. */
    efs_client_stage_evict_kick();
    return EFS_OK;
}


static void apply_chunk_recs(efs_ino_t lock_ino, const struct efs_chunk_rec *recs,
                             uint32_t n)
{
    efs_client_lock_dir(lock_ino);
    pthread_mutex_lock(&g_client.idx_mu);
    for (uint32_t i = 0; i < n; i++) {
        (void)efs_client_set_chunk(&g_client.export, recs[i].ino,
                                   recs[i].chunk_index, recs[i].nodes,
                                   recs[i].checksums);
        (void)efs_export_set_chunk_gen(&g_client.export, recs[i].ino,
                                       recs[i].chunk_index,
                                       recs[i].chunk_generation
                                           ? recs[i].chunk_generation
                                           : recs[i].base_gen);
        /* set_chunk_gen drops any previous span list. Install the one
         * this reply carries (empty when the image has no spans).
         * Do not keep local spans the reply lacks: their seq is not the
         * lane seq, and a later full-image CAS would name the wrong
         * list and STALE forever. */
        (void)efs_export_set_chunk_deltas(&g_client.export, recs[i].ino,
                                          recs[i].chunk_index,
                                          recs[i].deltas, recs[i].delta_base_n,
                                          recs[i].delta_base_seq);
        efs_client_stage_touch(recs[i].ino);
    }
    pthread_mutex_unlock(&g_client.idx_mu);
    efs_client_unlock_dir(lock_ino);
}

/* Fetch chunk mappings in [start_ci, end_ci). Split at group boundaries so
 * each GETCHUNKS goes to that group's owner. An empty group is a hole, not
 * the end of the file. Returns the first RPC error; the table is then
 * partial for the range and the caller must not treat a missing row as a
 * hole. */
/* Absence bitmap: bit (ci - base) set for every chunk in the pulled range
 * the server returned no row for. The local table keeps whatever it held
 * for those chunks — usually this client's own unreported PUT — so a
 * caller that must tell "the host committed our object" from "we only
 * PUT it" (the STALE classifier) reads this, not the table generation. */
struct pull_absent {
    uint8_t *bits;
    uint32_t base;
    uint32_t end;
};

static void pull_absent_mark(struct pull_absent *ab, uint32_t lo, uint32_t hi)
{
    uint32_t ci;

    if (!ab || !ab->bits)
        return;
    if (lo < ab->base)
        lo = ab->base;
    if (hi > ab->end)
        hi = ab->end;
    /* Atomic: the pull fan marks disjoint groups, but a byte can hold
     * bits of two groups when `base` is not 8-aligned. */
    for (ci = lo; ci < hi; ci++)
        (void)__atomic_fetch_or(&ab->bits[(ci - ab->base) >> 3],
                                (uint8_t)(1u << ((ci - ab->base) & 7)),
                                __ATOMIC_RELAXED);
}

static int pull_chunks_range(efs_ino_t ino, uint32_t start_ci, uint32_t end_ci,
                             struct pull_absent *ab)
{
    if (!ino || start_ci >= end_ci)
        return EFS_OK;
    uint32_t start = start_ci;
    while (start < end_ci) {
        uint32_t group_end = (start | (EFS_CHUNK_GROUP_SIZE - 1u)) + 1u;
        if (group_end > end_ci)
            group_end = end_ci;
        uint32_t cur = start;
        while (cur < group_end) {
            struct efs_chunk_rec recs[EFS_GETCHUNKS_MAX];
            uint32_t n = group_end - cur;
            if (n > EFS_GETCHUNKS_MAX)
                n = EFS_GETCHUNKS_MAX;
            int grc = efs_client_rpc_getchunks(g_client.export_id, ino, cur,
                                               recs, &n);
            if (grc != EFS_OK)
                return grc;
            if (n == 0) {
                pull_absent_mark(ab, cur, group_end);
                break;
            }
            if (ab && ab->bits) {
                uint32_t i, at = cur;

                for (i = 0; i < n; i++) {
                    if (recs[i].chunk_index > at)
                        pull_absent_mark(ab, at, recs[i].chunk_index);
                    at = recs[i].chunk_index + 1;
                }
            }
            apply_chunk_recs(ino, recs, n);
            uint32_t next = recs[n - 1].chunk_index + 1;
            if (next <= cur)
                break;
            cur = next;
            if (n < EFS_GETCHUNKS_MAX) {
                pull_absent_mark(ab, cur, group_end);
                break;
            }
        }
        start = group_end;
    }
    return EFS_OK;
}

/* One GETCHUNKS per chunk group, concurrent. A 1 GiB file is 128 groups;
 * issuing them one after another is the open-time stall D2 removes.
 * Up to PULL_FAN threads take groups from a shared cursor until the
 * range is covered. The STALE repull of a 36-rank shared-file fsync
 * walks the whole file (1600 groups of a 12 GiB IOR hard file); one
 * RPC at a time that was 12 s of the 8 s REPORT budget
 * (`report-stale … pull_ms=12115`, Sep 30 IO-500). */
#define PULL_FAN 16

struct pull_fan {
    efs_ino_t ino;
    uint32_t next;
    uint32_t end;
    int rc;
    pthread_mutex_t mu;
    struct pull_absent *ab;
};

static void *pull_fan_thread(void *arg)
{
    struct pull_fan *f = arg;

    for (;;) {
        uint32_t s, ge;
        int rc;

        pthread_mutex_lock(&f->mu);
        if (f->rc != EFS_OK || f->next >= f->end) {
            pthread_mutex_unlock(&f->mu);
            break;
        }
        s = f->next;
        ge = (s | (EFS_CHUNK_GROUP_SIZE - 1u)) + 1u;
        if (ge > f->end)
            ge = f->end;
        f->next = ge;
        pthread_mutex_unlock(&f->mu);
        rc = pull_chunks_range(f->ino, s, ge, f->ab);
        if (rc != EFS_OK) {
            pthread_mutex_lock(&f->mu);
            if (f->rc == EFS_OK)
                f->rc = rc;
            pthread_mutex_unlock(&f->mu);
        }
    }
    return NULL;
}

static int pull_groups_parallel(efs_ino_t ino, uint32_t start_ci,
                                uint32_t end_ci, struct pull_absent *ab)
{
    struct pull_fan f;
    pthread_t th[PULL_FAN];
    int started[PULL_FAN];
    uint32_t ngroups;
    int nth, i;

    if (start_ci >= end_ci)
        return EFS_OK;
    ngroups = ((end_ci - 1) >> EFS_CHUNK_GROUP_SHIFT) -
              (start_ci >> EFS_CHUNK_GROUP_SHIFT) + 1u;
    if (ngroups <= 1)
        return pull_chunks_range(ino, start_ci, end_ci, ab);
    f.ino = ino;
    f.next = start_ci;
    f.end = end_ci;
    f.rc = EFS_OK;
    f.ab = ab;
    pthread_mutex_init(&f.mu, NULL);
    nth = ngroups < PULL_FAN ? (int)ngroups : PULL_FAN;
    for (i = 1; i < nth; i++)
        started[i] = pthread_create(&th[i], NULL, pull_fan_thread, &f) == 0;
    pull_fan_thread(&f); /* the caller is worker 0 */
    for (i = 1; i < nth; i++)
        if (started[i])
            pthread_join(th[i], NULL);
    pthread_mutex_destroy(&f.mu);
    return f.rc;
}

int efs_client_pull_chunks_range(efs_ino_t ino, uint32_t start_ci,
                                 uint32_t end_ci)
{
    return pull_groups_parallel(ino, start_ci, end_ci, NULL);
}

/* Same pull; `absent` (caller-zeroed, (end_ci - start_ci + 7) / 8 bytes)
 * gets bit (ci - start_ci) for every chunk the host has no row for. On
 * an RPC error the bitmap is partial like the table. */
int efs_client_pull_chunks_range_absent(efs_ino_t ino, uint32_t start_ci,
                                        uint32_t end_ci, uint8_t *absent)
{
    struct pull_absent ab;

    ab.bits = absent;
    ab.base = start_ci;
    ab.end = end_ci;
    return pull_groups_parallel(ino, start_ci, end_ci, absent ? &ab : NULL);
}

static void pull_file_layout(const struct efs_inode *rpc)
{
    if ((rpc->mode & S_IFMT) != S_IFREG)
        return;
    if (rpc->pack_ino && rpc->pack_ino != rpc->ino) {
        uint32_t cs = data_chunk_size();
        uint64_t span = rpc->pack_len ? rpc->pack_len : rpc->size;
        uint32_t c0 = cs ? (uint32_t)(rpc->pack_off / cs) : 0;
        uint32_t c1 = c0 + 1;
        if (cs && span) {
            uint64_t hi = (uint64_t)rpc->pack_off + span;
            c1 = (uint32_t)((hi - 1) / cs) + 1;
        }
        pull_chunks_range(rpc->pack_ino, c0, c1, NULL);
    }
    /* D2: a regular file adopts the inode row only. pull_layout_miss
     * is the chunk-map path. The 64-lane size stat stays at open. */
}

/* Read-miss resolution (Phase 2b follow-on, tightened Sep 19): the local
 * chunk table is only a cache — rows are pulled at adopt, and the row
 * (with its whole chunk array) is EVICTED on the last close
 * (efs_client_stage_evict_ino) or by the Part A evictor. So a missing
 * chunk row inside the file size is, in the common case, not a hole but a
 * cache miss, and the pull that refills it can fail under load (GETCHUNKS
 * NET/BUSY on a busy owner; pull_chunks_range used to swallow that).
 *
 * The old version rate-limited this to one pull per ino per SECOND and
 * let every other miss zero-fill. IO-500 ior-easy-read (rank N reads the
 * 1.2 GiB file rank N+1 just wrote and closed on the same node): the
 * writer's row was evicted at close, the reopen pull was partial under
 * the 9x4 load, and 38054 of 42234 1 MiB reads came back as zeros in
 * 1.36 s — no fetch at all. Earlier the writer's dcache image papered
 * over the missing map; once the overlay demanded a current map entry
 * (dcache_image_current) the zeros showed.
 *
 * Now: always pull the missing range; a miss is a hole only after a
 * SUCCESSFUL pull that covered it. On a pull error the caller gets the
 * error and must not zero-fill. A successful pull of a superset range is
 * reused for 200 ms per ino so a genuinely sparse file read in small
 * pieces costs one GETCHUNKS per range, not one per read. */
int efs_client_pull_layout_miss(efs_ino_t ino, uint32_t ci0, uint32_t ci1)
{
    /* Metadata window: start at the prefetch depth, double while the
     * caller keeps asking at the end of the covered range, cap so this
     * never becomes a whole-map pull. One extra window is pulled ahead
     * of the range the read asked for. Internal; no knob. */
    enum { META_WIN_MIN = 16, META_WIN_MAX = 256 };
    static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
    static struct {
        efs_ino_t ino;
        uint32_t ci0, ci1;
        uint32_t win;
        uint64_t ns;
    } seen[64];
    static uint32_t next;
    if (!ino || ci0 >= ci1 || efs_ino_is_meta_table(ino))
        return EFS_OK;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t now = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
    pthread_mutex_lock(&mu);
    int slot = -1;
    for (uint32_t i = 0; i < 64; i++) {
        if (seen[i].ino == ino) {
            slot = (int)i;
            break;
        }
    }
    int seq = slot >= 0 && seen[slot].ns &&
              ci0 >= seen[slot].ci0 &&
              ci0 <= seen[slot].ci1 + EFS_CHUNK_GROUP_SIZE;
    /* Fresh means the requested range [ci0, ci1) is inside a pull
     * younger than 200 ms AND that pull still has a full window of
     * lookahead past ci1. The old test required seen.ci1 >= ci0+2*win.
     * seen.ci1 was the previous call's ahead, which is one group short
     * of this call's ahead once the window stops growing, so every
     * 1 MiB read issued a GETCHUNKS (Oct 1 dd). Extend only when the
     * covered range ends within `win` of ci1; pull [seen.ci1, ci1+2*win)
     * then, not the range we already have. */
    int covered = slot >= 0 && seen[slot].ns &&
                  now - seen[slot].ns < 200000000ull &&
                  seen[slot].ci0 <= ci0 && seen[slot].ci1 >= ci1;
    uint32_t win = META_WIN_MIN;
    if (seq && seen[slot].win)
        win = seen[slot].win;
    if (covered && seen[slot].ci1 >= ci1 + win) {
        pthread_mutex_unlock(&mu);
        return EFS_OK;
    }
    uint32_t pull_from = ci0;
    uint32_t cov0 = ci0;
    uint32_t ahead;
    if (covered) {
        pull_from = seen[slot].ci1;
        cov0 = seen[slot].ci0;
    } else if (seq && seen[slot].ci1 > ci0) {
        cov0 = seen[slot].ci0;
    }
    if (seq && win < META_WIN_MAX / 2)
        win = win ? win * 2 : META_WIN_MIN;
    else if (seq)
        win = META_WIN_MAX;
    ahead = ci1 + win * 2u;
    if (!covered && seq && seen[slot].ci1 > ci0 && seen[slot].ci1 < ahead)
        pull_from = seen[slot].ci1;
    if (ahead < pull_from)
        ahead = pull_from;
    pthread_mutex_unlock(&mu);
    int rc = EFS_OK;
    if (pull_from < ahead)
        rc = pull_groups_parallel(ino, pull_from, ahead, NULL);
    if (rc != EFS_OK)
        return rc;
    pthread_mutex_lock(&mu);
    if (slot < 0 || seen[slot].ino != ino) {
        slot = -1;
        for (uint32_t i = 0; i < 64; i++)
            if (seen[i].ino == ino) {
                slot = (int)i;
                break;
            }
        if (slot < 0)
            slot = (int)(next++ % 64);
    }
    seen[slot].ino = ino;
    seen[slot].ci0 = cov0;
    seen[slot].ci1 = ahead;
    seen[slot].win = win;
    seen[slot].ns = now;
    pthread_mutex_unlock(&mu);
    return EFS_OK;
}

static void invalidate_file_layout(const struct efs_inode *rpc, int drop_dcache)
{
    uint32_t cs = data_chunk_size();
    if (rpc->pack_ino && rpc->pack_ino != rpc->ino) {
        uint32_t c0 = cs ? (uint32_t)(rpc->pack_off / cs) : 0;
        efs_rdcache_invalidate(rpc->pack_ino, c0);
        if (drop_dcache)
            efs_dcache_drop_if_clean(rpc->pack_ino, c0);
    }
    uint32_t nci = 0;
    if (cs && rpc->size)
        nci = (uint32_t)((rpc->size + cs - 1) / cs);
    for (uint32_t ci = 0; ci < nci; ci++) {
        efs_rdcache_invalidate(rpc->ino, ci);
        /* A size grow (O_APPEND) must not drop the published have_base=1
         * cache. That forced every close to GET+merge and blew the
         * mtime_monotonic 80-stat budget. Shrink or same-size remap
         * (peer hole fill) still drops. */
        if (drop_dcache)
            efs_dcache_drop_if_clean(rpc->ino, ci);
    }
}

/* Adopt an inode the primary just confirmed. A local row that this client
 * dirtied stays (size/pack fresher until REPORT_CHUNKS). A peer's later
 * write is newer on the primary — merge size/pack when remote grew or
 * has a newer mtime (posix2 peer_shared_pwrite). */
static void adopt_rpc_inode(const struct efs_inode *rpc)
{
    if (!rpc || rpc->ino == 0)
        return;
    efs_client_stage_touch(rpc->ino);
    int is_new = 0;
    int take_remote = 0;
    int drop_dcache = 0;
    efs_ino_t lock = rpc->parent ? rpc->parent : rpc->ino;
    efs_client_lock_dir(lock);
    pthread_mutex_lock(&g_client.idx_mu);
    struct efs_inode local;
    if (efs_export_get_inode(&g_client.export, rpc->ino, &local) != 0) {
        (void)efs_export_upsert_inode(&g_client.export, rpc);
        is_new = 1;
    } else if ((rpc->mode & S_IFMT) == S_IFREG) {
        /* Size growth = a peer published more data. mtime-only (chmod)
         * must not clobber the writer's unflushed dcache — that was the
         * POSIX same-fd-read / chmod / unlink-open regression. Shrink
         * only when the primary is newer (peer truncate) AND this client
         * has nothing unflushed: fsync/lookup getattr still sees the
         * post-truncate size 0 until REPORT lands, and the owner's
         * set_size-stamped mtime can look newer than the local write.
         * Taking that shrink is trunc_open_other_fd fstat=0 under 9-way
         * load (and then REPORT publishes the clobbered 0). */
        int newer = rpc->mtime > local.mtime ||
                    (rpc->mtime == local.mtime &&
                     rpc->mtime_nsec > local.mtime_nsec);
        int times_differ = rpc->mtime != local.mtime ||
                           rpc->mtime_nsec != local.mtime_nsec;
        int local_dirty = efs_client_ino_is_dirty(rpc->ino);
        int mtime_pinned = efs_client_mtime_is_pinned(rpc->ino);
        /* Equal-size with a newer mtime also takes the remote: a peer can
         * change chunk mappings without changing size (writing into a
         * pre-sized file's holes). Size-only growth misses that — the
         * reader then serves zeros/stale mappings forever (mc_stress
         * rwfile). Our own reports echo back with the same mtime, so this
         * does not re-pull on our own writes.
         * utimens may move mtime backwards. Once REPORT has cleared dirty
         * the owner is authoritative, including an older setattr. A
         * pinned dirty row must not take a newer REPORT echo ("now"). */
        int take_mtime = 0;
        if (mtime_pinned)
            take_mtime = 0;
        else if (!local_dirty && times_differ)
            take_mtime = 1;
        else if (newer)
            take_mtime = 1;
        /* Shrink only on a newer owner (peer truncate). An older
         * setattr mtime must not pull size 0 from a lagging owner
         * after dirty cleared — that zeroed basic_dd_rw / O_TRUNC. */
        int grow = rpc->size > local.size;
        int same = rpc->size == local.size;
        int shrink = rpc->size < local.size && !local_dirty && newer;
        /* Owner nlink is cluster-shared. Dual-apply of this client's
         * link/unlink only bumps locally; a peer's concurrent link is
         * missing until GETATTR/LOOKUP. Merge it here without treating
         * it as a data-layout change. */
        int nlink_changed = rpc->nlink != local.nlink;
        if (grow || (take_mtime && same) || shrink || nlink_changed) {
            if (grow || shrink) {
                local.size = rpc->size;
                local.pack_ino = rpc->pack_ino;
                local.pack_off = rpc->pack_off;
                local.pack_len = rpc->pack_len;
            } else if (same && take_mtime) {
                local.pack_ino = rpc->pack_ino;
                local.pack_off = rpc->pack_off;
                local.pack_len = rpc->pack_len;
            }
            if (take_mtime) {
                local.mtime = rpc->mtime;
                local.mtime_nsec = rpc->mtime_nsec;
            }
            if (nlink_changed) {
                local.nlink = rpc->nlink;
                local.ctime = rpc->ctime;
            }
            /* The accepted row's present count rides along; stat still
             * takes the max with the local table at read time. */
            local.alloc_chunks = rpc->alloc_chunks;
            (void)efs_export_upsert_inode(&g_client.export, &local);
            if (grow || shrink || (same && take_mtime)) {
                take_remote = 1;
                /* Only a shrink drops clean dcache slots. Same-size +
                 * newer mtime used to drop too (peer hole-fill visibility,
                 * mc_stress rwfile), but that also fired on our OWN report
                 * echo (server-stamped mtime) during O_APPEND getattr and
                 * discarded a clean-but-unreported slot — the only replay
                 * source for its in-flight object. The next append rebuilt
                 * the slot from the committed map gen with just its own
                 * range; when the in-flight report lost its CAS the replay
                 * overlaid only that range and the earlier bytes were gone
                 * (concurrent_appends 190→22/200 lines). Reads no longer
                 * need the drop: efs_dcache_copy serves a whole image only
                 * when the chunk map still names it (dcache_image_current),
                 * and take_remote re-pulls the map. */
                drop_dcache = shrink;
            }
        }
    }
    pthread_mutex_unlock(&g_client.idx_mu);
    efs_client_unlock_dir(lock);
    if (is_new) {
        pull_file_layout(rpc);
        return;
    }
    if (take_remote) {
        invalidate_file_layout(rpc, drop_dcache);
        pull_file_layout(rpc);
    }
}

/* getattr for an open fd (and any ino-keyed stat). Path lookup already
 * prefers the local row after adopt; the FUSE getattr(fi->fh) path used
 * to return the RPC row raw. The owner learns size via async
 * REPORT_CHUNKS, so that row is often still size 0 after a same-fd
 * write. The kernel then sets i_size=0 (attr_timeout=0) and never
 * calls .read — POSIX same-fd / unlink-open / chmod-open all empty.
 *
 * ecopy (and every create/write/close) getattr's the open fd ~2x per
 * file. The RPC was discarded whenever a local row existed — ~2 RTTs
 * of pure wait on the dest owner's shard lock. Local-first: RPC only
 * on a miss (peer-created ino, or a cold table). */
int efs_client_stat_open(efs_ino_t ino, struct efs_inode *out)
{
    struct efs_inode local, rpc;
    int have, rpc_ok;

    if (!out || !ino)
        return EFS_ERR_INVAL;
    have = efs_client_stat_local(ino, &local) == EFS_OK;
    rpc_ok = efs_client_rpc_getattr(g_client.export_id, ino, &rpc);
    if (!have && rpc_ok != EFS_OK)
        return rpc_ok;
    if (!have) {
        adopt_rpc_inode(&rpc);
        if (efs_client_stat_local(ino, out) != EFS_OK)
            *out = rpc;
        return EFS_OK;
    }
    /* An open() of an existing file holds a lease, so the last unlink
     * stores nlink 0 and this getattr sees it. CREATE does not hold
     * (a propose per new file), so a create'd fd can still see
     * NOT_FOUND here and keeps the local ghost. A transport error is
     * not that: keep the local count. */
    if (rpc_ok == EFS_ERR_NOT_FOUND)
        rpc.nlink = 0;
    else if (rpc_ok != EFS_OK) {
        *out = local;
        return EFS_OK;
    }
    if (rpc.nlink != local.nlink) {
        struct efs_inode cur;

        local.nlink = rpc.nlink;
        if (rpc_ok == EFS_OK)
            local.ctime = rpc.ctime;
        efs_client_lock_dir(ino);
        pthread_mutex_lock(&g_client.idx_mu);
        if (efs_export_get_inode(&g_client.export, ino, &cur) == 0) {
            cur.nlink = local.nlink;
            if (rpc_ok == EFS_OK)
                cur.ctime = rpc.ctime;
            (void)efs_export_upsert_inode(&g_client.export, &cur);
            local = cur;
        }
        pthread_mutex_unlock(&g_client.idx_mu);
        efs_client_unlock_dir(ino);
    }
    *out = local;
    return EFS_OK;
}

int efs_client_stat_ino(efs_ino_t ino, struct efs_inode *out)
{
    if (!out || !ino)
        return EFS_ERR_INVAL;
    efs_client_lock_dir(ino);
    pthread_mutex_lock(&g_client.idx_mu);
    if (efs_export_get_inode(&g_client.export, ino, out) == 0) {
        pthread_mutex_unlock(&g_client.idx_mu);
        efs_client_unlock_dir(ino);
        return EFS_OK;
    }
    pthread_mutex_unlock(&g_client.idx_mu);
    efs_client_unlock_dir(ino);

    struct efs_inode rpc;
    int grc = efs_client_rpc_getattr(g_client.export_id, ino, &rpc);
    if (grc != EFS_OK)
        return grc;
    adopt_rpc_inode(&rpc);
    efs_client_lock_dir(ino);
    pthread_mutex_lock(&g_client.idx_mu);
    if (efs_export_get_inode(&g_client.export, ino, out) == 0) {
        pthread_mutex_unlock(&g_client.idx_mu);
        efs_client_unlock_dir(ino);
        return EFS_OK;
    }
    pthread_mutex_unlock(&g_client.idx_mu);
    efs_client_unlock_dir(ino);
    *out = rpc;
    return EFS_OK;
}

int efs_client_stat_refresh(efs_ino_t ino, struct efs_inode *out)
{
    if (!out || !ino)
        return EFS_ERR_INVAL;
    struct efs_inode rpc;
    int grc = efs_client_rpc_getattr(g_client.export_id, ino, &rpc);
    if (grc != EFS_OK)
        return grc;
    efs_client_adopt_lookup(&rpc, out);
    return EFS_OK;
}

/* Is a's mtime strictly older than b's? */
static int inode_mtime_older(const struct efs_inode *a,
                             const struct efs_inode *b)
{
    if (a->mtime != b->mtime)
        return a->mtime < b->mtime;
    return a->mtime_nsec < b->mtime_nsec;
}

int efs_client_stat_local(efs_ino_t ino, struct efs_inode *out)
{
    if (!out || !ino)
        return EFS_ERR_INVAL;
    efs_client_lock_dir(ino);
    pthread_mutex_lock(&g_client.idx_mu);
    int rc = efs_export_get_inode(&g_client.export, ino, out);
    pthread_mutex_unlock(&g_client.idx_mu);
    efs_client_unlock_dir(ino);
    return rc == 0 ? EFS_OK : EFS_ERR_NOT_FOUND;
}

int efs_client_lookup_local(efs_ino_t parent, const char *name,
                            struct efs_inode *out)
{
    if (!out || !parent || !name || !name[0])
        return EFS_ERR_INVAL;
    efs_client_lock_dir(parent);
    pthread_mutex_lock(&g_client.idx_mu);
    int rc = efs_export_lookup(&g_client.export, parent, name, out);
    pthread_mutex_unlock(&g_client.idx_mu);
    efs_client_unlock_dir(parent);
    return rc == 0 ? EFS_OK : EFS_ERR_NOT_FOUND;
}

/* Writer: local size/pack is newer than the owner until REPORT.
 * Peer remount stubs must not clobber GETATTR. more=1 for a walk
 * intermediate (directories; no size overlay). */
static void overlay_local_size(struct efs_inode *child, int more)
{
    int dirty = efs_client_ino_is_dirty(child->ino);
    int want_local = dirty ||
                     (!more && !efs_mode_is_dir(child->mode));
    if (!want_local)
        return;
    efs_ino_t lk = child->parent ? child->parent : child->ino;
    struct efs_inode local;
    int have;
    efs_client_lock_dir(lk);
    pthread_mutex_lock(&g_client.idx_mu);
    have = (efs_export_get_inode(&g_client.export, child->ino, &local) == 0);
    pthread_mutex_unlock(&g_client.idx_mu);
    efs_client_unlock_dir(lk);
    if (have && dirty) {
        *child = local;
    } else if (have && local.size > child->size &&
               !inode_mtime_older(&local, child)) {
        child->size = local.size;
        child->pack_ino = local.pack_ino;
        child->pack_off = local.pack_off;
        child->pack_len = local.pack_len;
    }
}

void efs_client_adopt_lookup(const struct efs_inode *rpc, struct efs_inode *out)
{
    if (!rpc || !out || rpc->ino == 0)
        return;
    *out = *rpc;
    adopt_rpc_inode(rpc);
    overlay_local_size(out, 0);
}

static int lookup_access_ok(const struct efs_inode *ino, uid_t uid, gid_t gid,
                            const gid_t *groups, int ngroups, int mask)
{
    if (uid == 0)
        return 0;
    uint32_t perm;
    if (uid == ino->uid)
        perm = (ino->mode >> 6) & 7;
    else {
        int in_group = (gid == ino->gid);
        for (int i = 0; !in_group && i < ngroups; i++) {
            if (groups[i] == ino->gid)
                in_group = 1;
        }
        perm = in_group ? ((ino->mode >> 3) & 7) : (ino->mode & 7);
    }
    if ((mask & 4) && !(perm & 4))
        return -1;
    if ((mask & 2) && !(perm & 2))
        return -1;
    if ((mask & 1) && !(perm & 1))
        return -1;
    return 0;
}

/* A path can hold at most one component per two bytes ("/x"). */
#define EFS_WALK_MAX_COMPS 2048
/* Below this depth a path costs so few round trips that the extra RPC on a
 * batch miss would not pay for itself. */
#define EFS_WALK_BATCH_MIN 8

/* Resolve comp[0 .. ncomp-2] -- the leaf's ancestor chain -- with batched
 * LOOKUP_PATH calls of up to 64 components each. On EFS_OK *parent is the
 * leaf's parent and *idx is ncomp-1. EFS_ERR_ACCES/EFS_ERR_INVAL fail the
 * whole lookup; any other return means "could not batch" and the caller
 * does the per-component walk, so a batch miss can never change an answer.
 */
static int walk_batch_ancestors(char *const *comp, size_t ncomp, int do_x,
                                uid_t uid, gid_t gid, const gid_t *groups,
                                int ngroups, efs_ino_t *parent, size_t *idx)
{
    efs_ino_t cur = EFS_ROOT_INO;
    size_t done = 0;

    while (done < ncomp - 1) {
        struct efs_msg_inode_lookup_path_reply r;
        char cbuf[4096];
        size_t n = 0, len = 0;

        while (done + n < ncomp - 1 && n < EFS_LOOKUP_PATH_MAX_DEPTH) {
            size_t cl = strlen(comp[done + n]);
            if (len + cl + 2 > sizeof(cbuf))
                break;
            cbuf[len++] = '/';
            memcpy(cbuf + len, comp[done + n], cl);
            len += cl;
            n++;
        }
        if (n == 0)
            return EFS_ERR_NOT_FOUND;
        cbuf[len] = '\0';

        if (efs_client_rpc_lookup_path(g_client.export_id, cur, cbuf,
                                       EFS_LOOKUP_PATH_F_ANCESTORS,
                                       &r) != EFS_OK)
            return EFS_ERR_NOT_FOUND;
        /* The server records an ancestor for every component but the last of
         * the path it was given; anything else means it truncated and the
         * exec chain would have a hole. */
        if (r.ancestor_count != n - 1)
            return EFS_ERR_NOT_FOUND;

        if (do_x) {
            for (uint32_t a = 0; a <= r.ancestor_count; a++) {
                struct efs_inode t;
                memset(&t, 0, sizeof(t));
                if (a < r.ancestor_count) {
                    t.ino = r.ancestors[a].ino;
                    t.mode = r.ancestors[a].mode;
                    t.uid = r.ancestors[a].uid;
                    t.gid = r.ancestors[a].gid;
                } else {
                    t = r.inode; /* this chunk's terminal dir */
                }
                if (!efs_mode_is_dir(t.mode))
                    return EFS_ERR_INVAL;
                if (t.ino != EFS_ROOT_INO &&
                    lookup_access_ok(&t, uid, gid, groups, ngroups, 1) != 0)
                    return EFS_ERR_ACCES;
            }
        }
        cur = r.inode.ino;
        done += n;
    }

    *parent = cur;
    *idx = ncomp - 1;
    return EFS_OK;
}

static int lookup_walk(const char *path, struct efs_inode *out, int do_x,
                       uid_t uid, gid_t gid, const gid_t *groups, int ngroups)
{
    if (!path || path[0] != '/')
        return EFS_ERR_INVAL;

    if (strcmp(path, "/") == 0) {
        efs_client_lock_dir(EFS_ROOT_INO);
        int rc = efs_export_get_inode(&g_client.export, EFS_ROOT_INO, out);
        efs_client_unlock_dir(EFS_ROOT_INO);
        return rc;
    }

    /* Cut 4: names/nlink always from the owner. The local table is not a
     * metadata replica — dir short-circuit, created_recent, and lookup_cache
     * served stale nlink/dentry (posix2 hardlink, rename chase). Dirty
     * size/pack still overlay after RPC so unflushed writes stay visible. */

    /* Copy onto the stack (bounded by PATH_MAX) instead of a per-lookup
     * strdup/free on the getattr/lookup hot path. */
    char pbuf[4096];
    size_t plen = strlen(path + 1);
    if (plen >= sizeof(pbuf))
        return EFS_ERR_INVAL;
    memcpy(pbuf, path + 1, plen + 1);

    char *comp[EFS_WALK_MAX_COMPS];
    size_t ncomp = 0, i;
    {
        char *save = NULL;
        char *part = strtok_r(pbuf, "/", &save);
        while (part) {
            if (ncomp == EFS_WALK_MAX_COMPS)
                return EFS_ERR_INVAL;
            comp[ncomp++] = part;
            part = strtok_r(NULL, "/", &save);
        }
    }
    if (ncomp == 0)
        return EFS_ERR_INVAL;

    efs_client_ensure_dir_locks();

    /* Cut 4: per-component LOOKUP(+GETATTR) for the leaf -- names/nlink
     * always come from the owner. The ancestor chain above it carries no
     * such requirement, so deep paths batch it (see walk_batch_ancestors);
     * LOOKUP_PATH misses nested names on extra shards (hashed ROOT dest),
     * which falls back to walking every component from the root. */

    efs_ino_t parent = EFS_ROOT_INO;
    int rc = EFS_ERR_NOT_FOUND;
    i = 0;
    if (ncomp > EFS_WALK_BATCH_MIN) {
        int brc = walk_batch_ancestors(comp, ncomp, do_x, uid, gid, groups,
                                       ngroups, &parent, &i);
        if (brc == EFS_ERR_ACCES || brc == EFS_ERR_INVAL)
            return brc;
        if (brc != EFS_OK) {
            parent = EFS_ROOT_INO;
            i = 0;
        }
    }
    for (; i < ncomp; i++) {
        int more = (i + 1 < ncomp);
        struct efs_inode child;
        int lrc = efs_client_rpc_lookup(g_client.export_id, parent, comp[i],
                                        &child);
        if (lrc != EFS_OK) {
            rc = (lrc == EFS_ERR_NOT_FOUND) ? EFS_ERR_NOT_FOUND : lrc;
            break;
        }
        if (do_x && more) {
            if (!efs_mode_is_dir(child.mode))
                return EFS_ERR_INVAL;
            if (child.ino != EFS_ROOT_INO &&
                lookup_access_ok(&child, uid, gid, groups, ngroups, 1) != 0)
                return EFS_ERR_ACCES;
        }
        adopt_rpc_inode(&child);
        overlay_local_size(&child, more);
        parent = child.ino;
        *out = child;
        rc = EFS_OK;
    }

    return rc;
}

int efs_client_lookup(const char *path, struct efs_inode *out)
{
    return lookup_walk(path, out, 0, 0, 0, NULL, 0);
}

efs_ino_t efs_client_create(efs_ino_t parent, const char *name, uint32_t mode,
                            uid_t uid, gid_t gid)
{
    return efs_client_create_ex(parent, name, mode, uid, gid, 0);
}

efs_ino_t efs_client_create_ex(efs_ino_t parent, const char *name, uint32_t mode,
                               uid_t uid, gid_t gid, uint32_t flags)
{
    g_client.last_err = EFS_OK;
    /* Phase 2b: the mutation runs on the metadata primary (which allocates the
     * ino and persists via the server flush thread). Dual-apply the returned
     * inode to the local snapshot so the data path sees it immediately. */
    struct efs_inode out;
    int rc = efs_client_rpc_create(g_client.export_id, parent, name, mode,
                                   uid, gid, flags, NULL, &out);
    if (rc != EFS_OK) {
        g_client.last_err = rc;
        return 0;
    }
    /* Pin across the local insert. The dirty mark below is what keeps
     * the row after we unpin; the pin covers the insert itself. */
    efs_client_stage_pin(out.ino);
    efs_client_stage_pin(parent);
    efs_client_lock_dir(parent);
    pthread_mutex_lock(&g_client.idx_mu);
    /* create_with_ino applies the same side effects as the primary
     * (parent nlink++ for a subdirectory, indexes, rollups). Upsert
     * alone would leave the local parent nlink stale.
     * On a sharded export mirror the server's apply: the full row lives
     * on the child shard's table (so the data path's get_inode finds it),
     * plus a dentry row on the parent's shard when the two differ.
     * bits==0: both are the main table — identical to the old path. */
    struct efs_export *ctab = efs_export_table_for_ino(&g_client.export,
                                                       out.ino);
    struct efs_export *ptab = efs_export_table_for_ino(&g_client.export,
                                                       parent);
    if (!ctab)
        ctab = &g_client.export;
    if (efs_export_create_with_ino(ctab, out.ino, parent, mode,
                                   uid, gid, name) == 0)
        efs_export_upsert_inode(ctab, &out);
    if (ptab && ptab != ctab) {
        if (efs_export_create_with_ino(ptab, out.ino, parent, mode,
                                       uid, gid, name) == 0)
            efs_export_upsert_inode(ptab, &out);
    }
    efs_export_set_mtime(&g_client.export, parent, now());
    efs_client_stage_touch(out.ino);
    /* Mark dirty BEFORE dropping the locks: the staging-table evictor runs
     * under the same table lock, so staging + dirty-mark must be one atomic
     * hold — otherwise the row could be evicted in the gap and the later
     * REPORT would skip the missing row (its size/chunk recs never reach
     * the server). Peers never mark this ino dirty. */
    efs_client_mark_ino_dirty(out.ino);
    efs_client_mark_ino_dirty(parent);
    pthread_mutex_unlock(&g_client.idx_mu);
    efs_client_unlock_dir(parent);
    efs_client_stage_unpin(parent);
    efs_client_stage_unpin(out.ino);
    return out.ino;
}

/* Phase 2b: setattr via the primary; dual-apply the returned inode. */
static int setattr_rpc_dual_apply(efs_ino_t ino, uint32_t mask, uint32_t mode,
                                  uid_t uid, gid_t gid, uint64_t size,
                                  uint64_t mtime, uint32_t mtime_nsec,
                                  uint64_t atime, uint32_t atime_nsec)
{
    /* wr() close kicks REPORT async with mtime=now. A later SETATTR
     * (utimens or truncate) must drain that snap first: a late newer-only
     * rec would restore "now" over a backdated utime (posix2
     * peer_utimens_visible: B saw now, want 1500000000). Per-ino so a
     * chmod of one file does not wait out a 9-job write REPORT. */
    if (mask & (EFS_SETATTR_SIZE | EFS_SETATTR_MTIME | EFS_SETATTR_ATIME))
        (void)efs_client_report_dirty_ino(ino, 1);
    struct efs_inode out;
    int rc = efs_client_rpc_setattr(g_client.export_id, ino, mask, mode,
                                    uid, gid, size, mtime, mtime_nsec, atime,
                                    atime_nsec, &out);
    if (rc != EFS_OK) {
        g_client.last_err = rc;
        return rc;
    }
    efs_client_lock_dir(ino);
    pthread_mutex_lock(&g_client.idx_mu);
    /* The primary's returned inode lags the data path: it learns the size only
     * via the async REPORT_CHUNKS flush, so for a non-truncate setattr its
     * size can be stale (0 after a chmod hides the file's data — reads then
     * return empty). Preserve the local data-path size (tracked synchronously
     * by writes) unless this setattr is itself a truncate. */
    if (!(mask & EFS_SETATTR_SIZE)) {
        struct efs_inode cur;
        if (efs_export_get_inode(&g_client.export, ino, &cur) == 0)
            out.size = cur.size;
    }
    /* Owner reply can lag the data path (mtime still "now" from the
     * write, or a hollow extra-shard row). chmod already applied
     * mode/owner locally for that reason — do the same for times,
     * then stamp them on `out` so a later upsert cannot put "now"
     * back. Pin so dcache_note_size / REPORT cannot either. */
    if (mask & EFS_SETATTR_MTIME) {
        out.mtime = mtime;
        out.mtime_nsec = mtime_nsec;
    }
    if (mask & EFS_SETATTR_ATIME) {
        out.atime = atime;
        out.atime_nsec = atime_nsec;
    }
    if (mask & EFS_SETATTR_MODE)
        efs_export_set_mode(&g_client.export, ino, mode);
    if (mask & (EFS_SETATTR_UID | EFS_SETATTR_GID))
        efs_export_set_owner(&g_client.export, ino, uid, gid);
    if (!(mask & (EFS_SETATTR_MODE | EFS_SETATTR_UID | EFS_SETATTR_GID)))
        efs_export_upsert_inode(&g_client.export, &out);
    if (mask & EFS_SETATTR_MTIME) {
        efs_export_set_mtime_ns(&g_client.export, ino, mtime, mtime_nsec);
        efs_client_mtime_pin(ino);
    }
    if (mask & EFS_SETATTR_ATIME)
        efs_export_set_atime(&g_client.export, ino, atime, atime_nsec);
    efs_client_stage_touch(ino);
    pthread_mutex_unlock(&g_client.idx_mu);
    efs_client_unlock_dir(ino);
    return EFS_OK;
}

static int local_inode(efs_ino_t ino, struct efs_inode *out)
{
    int rc;
    efs_client_lock_dir(ino);
    pthread_mutex_lock(&g_client.idx_mu);
    rc = efs_export_get_inode(&g_client.export, ino, out);
    pthread_mutex_unlock(&g_client.idx_mu);
    efs_client_unlock_dir(ino);
    return rc;
}

int efs_client_chmod(efs_ino_t ino, uint32_t mode)
{
    struct efs_inode cur;
    if (local_inode(ino, &cur) == 0 &&
        (cur.mode & 07777) == (mode & 07777))
        return EFS_OK;
    return setattr_rpc_dual_apply(ino, EFS_SETATTR_MODE, mode, 0, 0, 0, 0, 0,
                                  0, 0);
}

int efs_client_chown(efs_ino_t ino, uid_t uid, gid_t gid)
{
    struct efs_inode cur;
    uint32_t mask = 0;
    if (local_inode(ino, &cur) == 0) {
        if (uid != (uid_t)-1 && cur.uid == uid)
            uid = (uid_t)-1;
        if (gid != (gid_t)-1 && cur.gid == gid)
            gid = (gid_t)-1;
    }
    if (uid != (uid_t)-1)
        mask |= EFS_SETATTR_UID;
    if (gid != (gid_t)-1)
        mask |= EFS_SETATTR_GID;
    if (!mask)
        return EFS_OK;
    return setattr_rpc_dual_apply(ino, mask, 0, uid, gid, 0, 0, 0, 0, 0);
}

int efs_client_utimens(efs_ino_t ino, uint64_t mtime, uint32_t mtime_nsec)
{
    return setattr_rpc_dual_apply(ino, EFS_SETATTR_MTIME, 0, 0, 0, 0,
                                  mtime, mtime_nsec, 0, 0);
}

int efs_client_set_atime(efs_ino_t ino, uint64_t atime, uint32_t atime_nsec)
{
    return setattr_rpc_dual_apply(ino, EFS_SETATTR_ATIME, 0, 0, 0, 0, 0, 0,
                                  atime, atime_nsec);
}

int efs_client_utimens_both(efs_ino_t ino, uint64_t mtime, uint32_t mtime_nsec,
                            uint64_t atime, uint32_t atime_nsec)
{
    return setattr_rpc_dual_apply(ino, EFS_SETATTR_ATIME | EFS_SETATTR_MTIME,
                                  0, 0, 0, 0, mtime, mtime_nsec, atime,
                                  atime_nsec);
}

int efs_client_truncate(efs_ino_t ino, uint64_t size)
{
    efs_client_lock_dir(ino);
    struct efs_inode inode;
    if (efs_export_get_inode(&g_client.export, ino, &inode) != 0) {
        efs_client_unlock_dir(ino);
        return EFS_ERR_NOT_FOUND;
    }
    uint64_t old_size = inode.size;
    efs_client_unlock_dir(ino);

    if (size == old_size)
        return EFS_OK;

    if (size < old_size && size % data_chunk_size() != 0) {
        /* Rewrite last kept chunk with a zeroed tail so a later truncate-up
         * does not resurrect discarded bytes (data path; reported via RPC). */
        uint32_t chunk_size = data_chunk_size();
        uint32_t ci = (uint32_t)(size / chunk_size);
        uint32_t keep = (uint32_t)(size % chunk_size);
        uint8_t *chunk = efs_buf_alloc(chunk_size);
        if (!chunk)
            return EFS_ERR_NOMEM;
        memset(chunk, 0, chunk_size);
        uint64_t chunk_start = (uint64_t)ci * chunk_size;
        size_t want = (size_t)(old_size - chunk_start);
        if (want > chunk_size)
            want = chunk_size;
        size_t got = 0;
        (void)efs_client_read(ino, chunk_start, want, (char *)chunk, &got);
        memset(chunk + keep, 0, chunk_size - keep);
        int wrc = efs_client_write(ino, chunk_start, chunk_size,
                                   (const char *)chunk);
        efs_buf_free(chunk, chunk_size);
        if (wrc != 0)
            return wrc;
    }

    /* Phase 2b: the size change (and server-side chunk drop on shrink) runs on
     * the primary. Dual-apply: drop the same chunks locally + upsert the
     * returned inode. Grow is a logical sparse hole (no chunk work).
     * Stamp client-clock mtime with SIZE: REPORT grow is rejected when
     * the report mtime is older than the row. Server-clock "now" from
     * set_size alone is a different host, so a slightly-behind client
     * then REPORTs size 1 with an older mtime and the grow is dropped
     * (peer_o_trunc_visible: B saw 0). Same-client now is strictly
     * after the pre-trunc write, so the stale close-REPORT stays stale
     * and the post-trunc write is newer. */
    /* Same ino as fsync: a process-wide REPORT waits on every other
     * file's recs (and holds report_mu). Under the posix suite that
     * stacked two 15 s truncates + a symlink EIO abort. */
    (void)efs_client_report_dirty_ino(ino, 1);
    uint64_t sec;
    uint32_t nsec;
    now_ns(&sec, &nsec);
    if (sec < inode.mtime ||
        (sec == inode.mtime && nsec <= inode.mtime_nsec)) {
        sec = inode.mtime;
        nsec = inode.mtime_nsec + 1u;
        if (nsec >= 1000000000u) {
            sec++;
            nsec = 0;
        }
    }
    struct efs_inode out;
    int rc = efs_client_rpc_setattr(g_client.export_id, ino,
                                    EFS_SETATTR_SIZE | EFS_SETATTR_MTIME,
                                    0, 0, 0, size, sec, nsec, 0, 0, &out);
    if (rc != EFS_OK) {
        g_client.last_err = rc;
        return rc;
    }
    efs_client_lock_dir(ino);
    pthread_mutex_lock(&g_client.idx_mu);
    uint32_t drop = 0, old_nci = 0;
    if (size < old_size) {
        uint32_t cs = data_chunk_size();
        drop = (size == 0) ? 0 : (uint32_t)((size + cs - 1) / cs);
        old_nci = cs ? (uint32_t)((old_size + cs - 1) / cs) : 0;
        efs_export_drop_chunks_from(&g_client.export, ino, drop);
    }
    efs_export_upsert_inode(&g_client.export, &out);
    efs_client_stage_touch(ino);
    pthread_mutex_unlock(&g_client.idx_mu);
    efs_client_unlock_dir(ino);
    /* The dropped chunks' cached images are the pre-truncate bytes; a
     * later extend must read them as a hole, not resurrect them. Dirty
     * dcache slots stay (a racing write past the new EOF is newer). */
    for (uint32_t ci = drop; ci < old_nci; ci++) {
        efs_rdcache_invalidate(ino, ci);
        efs_dcache_drop_if_clean(ino, ci);
    }
    return EFS_OK;
}

int efs_client_rename_at(efs_ino_t ino, efs_ino_t old_parent, const char *old_name,
                         efs_ino_t new_parent, const char *new_name)
{
    struct efs_inode out;
    int rc = EFS_ERR_BUSY;
    int t;

    /* Held across the RPC and the local apply. mark_ino_dirty runs
     * before the unlock so the dirty set, not this pin, is what the
     * report sees. */
    efs_client_stage_pin(ino);
    efs_client_stage_pin(old_parent);
    efs_client_stage_pin(new_parent);
    for (t = 0; t < 8; t++) {
        rc = efs_client_rpc_rename_at(g_client.export_id, old_parent, old_name,
                                      new_parent, new_name, &out);
        if (rc != EFS_ERR_BUSY && rc != EFS_ERR_NET &&
            rc != EFS_ERR_NO_QUORUM)
            break;
        usleep(2000u << (unsigned)(t < 4 ? t : 4));
    }
    if (rc != EFS_OK) {
        /* Rare and otherwise invisible: FUSE collapses this to EIO. */
        fprintf(stderr, "efs: rename_at %llu/%s -> %llu/%s rc=%d after %d tries\n",
                (unsigned long long)old_parent, old_name,
                (unsigned long long)new_parent, new_name, rc, t + 1);
        g_client.last_err = rc;
        efs_client_stage_unpin(new_parent);
        efs_client_stage_unpin(old_parent);
        efs_client_stage_unpin(ino);
        return rc;
    }
    efs_client_lock_dirs2(ino, new_parent);
    pthread_mutex_lock(&g_client.idx_mu);
    if (efs_export_rename_at(&g_client.export, old_parent, old_name,
                             new_parent, new_name) != EFS_OK &&
        efs_export_rename(&g_client.export, ino, new_parent, new_name) != EFS_OK &&
        out.mode != 0)
        /* mode 0 is a committed rename whose inode was unlinked before
         * the reply (zero stat). Do not plant that over a real row. */
        efs_export_upsert_inode(&g_client.export, &out);
    efs_client_stage_touch(ino);
    /* Before the unlock: the evictor takes every stripe, then the dirty
     * lock. A mark after the unlock let it forget the row, and the
     * report then skipped a name that was no longer staged. */
    efs_client_mark_ino_dirty(ino);
    pthread_mutex_unlock(&g_client.idx_mu);
    efs_client_unlock_dirs2(ino, new_parent);
    efs_client_stage_unpin(new_parent);
    efs_client_stage_unpin(old_parent);
    efs_client_stage_unpin(ino);
    return EFS_OK;
}

static int first_child_name_cb(struct efs_export *ex, uint64_t slot, void *arg)
{
    *(const char **)arg = efs_export_inode_name(ex, slot);
    return 1;
}

int efs_client_unlink(efs_ino_t parent, const char *name, bool is_dir)
{
    /* Validate type locally (and to surface ENOENT/EISDIR before the RPC). */
    efs_client_lock_dir(parent);
    pthread_mutex_lock(&g_client.idx_mu);
    struct efs_inode ino;
    int lrc = efs_export_lookup(&g_client.export, parent, name, &ino);
    if (lrc == 0) {
        /* Local hit: validate type + refuse a non-empty rmdir before the RPC. */
        if (efs_mode_is_dir(ino.mode) != is_dir) {
            pthread_mutex_unlock(&g_client.idx_mu);
            efs_client_unlock_dir(parent);
            return EFS_ERR_INVAL;
        }
        /* rmdir: the local table may still list a child whose removal
         * committed elsewhere (another client, or a retry answered from the
         * op-id window) — refusing here made `rmdir` ENOTEMPTY for minutes
         * on one client while the server's row was empty (posix
         * `dir_deep_nesting`, fcstor012 Sep 30, fcstor009 Oct 1). The
         * server is authoritative (EFS_INODE_RPC_NOT_EMPTY); log the
         * stale view and ask it. */
        if (is_dir && !efs_export_dir_empty(&g_client.export, ino.ino)) {
            const char *child = NULL;
            efs_export_foreach_child(&g_client.export, ino.ino,
                                     first_child_name_cb, &child);
            static unsigned logged;
            if (logged < 16) {
                logged++;
                fprintf(stderr, "efs: rmdir ino=%llu local table lists child '%s' — asking the server\n",
                        (unsigned long long)ino.ino, child ? child : "?");
            }
        }
    }
    /* Local miss (cross-client: this client never looked the name up, so the
     * dentry isn't in its local table). Do NOT fail ENOENT — the server
     * authoritatively decides existence/type. Fall through to the RPC. */
    efs_ino_t victim_ino = (lrc == 0) ? ino.ino : 0;
    pthread_mutex_unlock(&g_client.idx_mu);
    efs_client_unlock_dir(parent);

    efs_client_stage_pin(parent);
    if (victim_ino)
        efs_client_stage_pin(victim_ino);
    /* Phase 2b: unlink on the primary; dual-apply removes the local entry. */
    int rc = efs_client_rpc_unlink(g_client.export_id, parent, name, is_dir);
    if (rc != EFS_OK) {
        if (victim_ino)
            efs_client_stage_unpin(victim_ino);
        efs_client_stage_unpin(parent);
        return rc;
    }
    efs_client_lock_dir(parent);
    pthread_mutex_lock(&g_client.idx_mu);
    /* Keep a nlink=0 ghost so an already-open fd can still get_inode.
     * Sharded unlink_name_ex now honors keep_last when the dentry slot is
     * the canonical row (co-located file create). */ 
    efs_export_unlink_name_ex(&g_client.export, parent, name, is_dir ? 0 : 1);
    /* Remaining hardlink rows keep the old nlink unless we nlink_dec the
     * same way the server does — getattr is local after dual-apply
     * (entry_timeout=0 still hits the snapshot). */
    if (!is_dir && victim_ino)
        (void)efs_export_nlink_dec_ex(&g_client.export, victim_ino, NULL, 1);
    pthread_mutex_unlock(&g_client.idx_mu);
    efs_client_unlock_dir(parent);
    if (victim_ino)
        efs_client_stage_unpin(victim_ino);
    efs_client_stage_unpin(parent);
    return EFS_OK;
}

int efs_client_link(efs_ino_t src_ino, efs_ino_t new_parent, const char *new_name)
{
    /* Phase 2b: link on the primary. */
    struct efs_inode out;
    efs_client_stage_pin(src_ino);
    efs_client_stage_pin(new_parent);
    int rc = efs_client_rpc_link(g_client.export_id, src_ino, new_parent,
                                 new_name, &out);
    if (rc != EFS_OK) {
        g_client.last_err = rc;
        efs_client_stage_unpin(new_parent);
        efs_client_stage_unpin(src_ino);
        return rc;
    }
    efs_client_lock_dirs2(src_ino, new_parent);
    pthread_mutex_lock(&g_client.idx_mu);
    /* Dual-apply by linking the local table too: efs_export_link only bumps
     * nlink + binds the new name, preserving the local data-path state (size,
     * pack fields, chunk mappings) which the server may not have yet (batched
     * flush). Upserting the returned inode would wipe that fresher local state.
     * Fall back to upsert only if the local table lacks the inode. */
    if (efs_export_link(&g_client.export, src_ino, new_parent, new_name) != EFS_OK)
        efs_export_upsert_inode(&g_client.export, &out);
    efs_client_stage_touch(src_ino);
    pthread_mutex_unlock(&g_client.idx_mu);
    efs_client_unlock_dirs2(src_ino, new_parent);
    efs_client_stage_unpin(new_parent);
    efs_client_stage_unpin(src_ino);
    return EFS_OK;
}

void efs_client_setup_ino_namespace(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    srandom((unsigned)(ts.tv_nsec ^ ts.tv_sec ^ (getpid() << 8)));
    uint64_t tag = ((uint64_t)getpid() << 32) ^
                   ((uint64_t)ts.tv_sec << 12) ^
                   (uint64_t)ts.tv_nsec ^
                   ((uint64_t)random() << 20);
    tag &= 0x7FFFFF; /* 23 bits, keeps the ino positive */
    if (tag == 0)
        tag = 1;
    g_client.ino_namespace = tag << 40;
    g_client.ino_counter = 1;
    if (!g_client.flock_token) {
        g_client.flock_token = tag ^ ((uint64_t)getpid() << 1);
        if (!g_client.flock_token)
            g_client.flock_token = 1;
    }
}
