/* efs-fuse: append reservation and pressure flush. */
#include "fuse_internal.h"
#include "append.h"
#include "open.h"
#include "writeback.h"

/* Serialize O_APPEND reserve+patch with close/fsync flush. Four threads
 * each write+close: without this, two flushes GET the same unpublished
 * base and the last PUT zeros the other's lines (concurrent_appends
 * 200 reserved bytes, 191 lines, NULs in the middle). */
/* Same-file append writes and close/fsync share one mutex. A single
 * global mutex made every close in the process wait out every other
 * file's REPORT. 64 stripes: one inode always hits the same stripe;
 * unrelated files usually do not. */
#define APPEND_MU_N 64
static pthread_mutex_t g_append_mu[APPEND_MU_N];
static pthread_once_t g_append_mu_once = PTHREAD_ONCE_INIT;

static void append_mu_init(void)
{
    int i;

    for (i = 0; i < APPEND_MU_N; i++)
        pthread_mutex_init(&g_append_mu[i], NULL);
}

pthread_mutex_t *append_mu(efs_ino_t ino)
{
    pthread_once(&g_append_mu_once, append_mu_init);
    return &g_append_mu[efs_ino_lock_slot(ino, APPEND_MU_N)];
}

/* Close: flush and the REPORT that publishes it are one critical
 * section with O_APPEND writes. FUSE_FLUSH and FUSE_FSYNC do not carry
 * the open flags (fi->flags is zero here), so this cannot key off
 * O_APPEND. A write between the PUT and the REPORT merged into the
 * span still in flight; the next image left that offset zero
 * (concurrent_appends: size 1600, 16 NUL bytes). */
/* 1 if this inode can still have bytes this client has not published:
 * a queued/in-flight wb job, a mark in the dirty set, or a dcache slot
 * that pins it (dirty, or clean-before-PUT inside a PUT window — the pin
 * is held until the slot is dropped). These are the three places
 * unpublished data lives (project-state "nothing to flush"). */
static int efs_ino_has_unpublished(efs_ino_t ino)
{
    return efs_wb_pending(ino) ||
           efs_client_ino_has_unpublished_data(ino);
}

static int efs_append_flush_report_run(struct fuse_file_info *fi, efs_ino_t ino)
{
    static int env_rpc_prof = -1; /* W143: read once, per-op path; any thread may be first */
    if (__atomic_load_n(&env_rpc_prof, __ATOMIC_ACQUIRE) < 0)
        __atomic_store_n(&env_rpc_prof, getenv("EFS_RPC_PROF") != NULL, __ATOMIC_RELEASE);
    pthread_mutex_t *amu;
    int rc;
    int profile = __atomic_load_n(&env_rpc_prof, __ATOMIC_RELAXED);
    uint64_t t0=profile?stats_now_ms():0,t1=0,t2=0,t3=0;

    (void)fi;
    if (!ino || virt_kind(ino))
        return EFS_OK;
    /* Every close(2) — read-only opens and dup'd fds included — reached
     * dcache_flush_ino_pass, which probes size/128 KiB dcache slots (two
     * mutex pairs and a chain walk each, all 65536 slots past 8 GiB)
     * under the append stripe. dcache_steal_dirty was 36.9 % of the
     * client's cycles during an ecopy --verify (Sep 30 2026,
     * results/measure/20260930-205300-ecopy-perf-review). A clean inode
     * has nothing to publish; the REPORT for it would carry no record. */
    if (!efs_ino_has_unpublished(ino))
        return EFS_OK;
    if (efs_client_rpc_past_deadline()) {
        fprintf(stderr, "flush-prepare-fail ino=%llu stage=deadline rc=%d\n",
                (unsigned long long)ino, EFS_ERR_BUSY);
        return EFS_ERR_BUSY;
    }
    rc = efs_wb_sync_ino(ino);
    if (rc != EFS_OK) {
        fprintf(stderr, "flush-prepare-fail ino=%llu stage=writeback rc=%d\n",
                (unsigned long long)ino, rc);
        return rc;
    }
    if(profile)t1=stats_now_ms();
    amu = append_mu(ino);
    rc = wb_lock_budget(amu);
    if (rc != EFS_OK) {
        fprintf(stderr, "flush-prepare-fail ino=%llu stage=append-lock rc=%d\n",
                (unsigned long long)ino, rc);
        return rc;
    }
    if(profile)t2=stats_now_ms();
    rc = efs_dcache_flush_ino(ino);
    if(profile)t3=stats_now_ms();
    if (rc == EFS_OK)
        rc = efs_client_report_dirty_ino(ino, 1);
    pthread_mutex_unlock(amu);
    if(profile) {
        uint64_t end=stats_now_ms();
        if(end-t0>=100)
            fprintf(stderr,"CLOSE-PROF ino=%llu mono_ms=%llu total_ms=%llu wb_ms=%llu append_lock_ms=%llu put_ms=%llu report_ms=%llu rc=%d\n",
                (unsigned long long)ino,(unsigned long long)end,
                (unsigned long long)(end-t0),(unsigned long long)(t1-t0),
                (unsigned long long)(t2-t1),(unsigned long long)(t3-t2),
                (unsigned long long)(end-t3),rc);
    }
    return rc;
}

/* One deadline begins before queued writes and append serialization.
 * Worker RPCs inherit it; accepted jobs retain their completion ownership. */
int efs_append_flush_report(struct fuse_file_info *fi, efs_ino_t ino)
{
    uint64_t previous = efs_client_rpc_deadline_ms();
    uint64_t deadline = stats_now_ms() + 30000;
    if (previous && previous < deadline)
        deadline = previous;
    efs_client_rpc_set_deadline_ms(deadline);
    int rc = efs_append_flush_report_run(fi, ino);
    efs_client_rpc_set_deadline_ms(previous);
    return rc;
}

/* Background publication is not fsync: flush a fixed dirty cohort, then
 * publish completed PUTs. Do not wait for subsequently accepted WB jobs or all
 * of the inode's future PUT windows, and never consume a foreground WB error.
 * The regular fsync/close path still drains every prior write and window. */
int fuse_pressure_flush(efs_ino_t ino)
{
    uint64_t previous = efs_client_rpc_deadline_ms();
    uint64_t deadline = stats_now_ms() + 30000;
    if (previous && previous < deadline)
        deadline = previous;
    efs_client_rpc_set_deadline_ms(deadline);
    pthread_mutex_t *amu = append_mu(ino);
    int rc = wb_lock_budget(amu);
    if (rc == EFS_OK) {
        rc = efs_dcache_flush_pressure_ino(ino);
        if (rc == EFS_OK)
            rc = efs_client_report_pressure_ino(ino);
        pthread_mutex_unlock(amu);
    }
    efs_client_rpc_set_deadline_ms(previous);
    return rc;
}

/* One coalesced pressure wakeup, no payload/entry ownership in the queue.
 * The existing snapshot/PUT/REPORT path owns each cohort. Successful admission
 * keeps its reservation; only real budget refusal takes the foreground path.
 * Shutdown joins this worker before writeback or client state is destroyed. */
static struct {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    pthread_t thread;
    int started, pending, stop;
} g_pressure = { .mu = PTHREAD_MUTEX_INITIALIZER,
                 .cv = PTHREAD_COND_INITIALIZER };

static void *fuse_pressure_main(void *unused)
{
    efs_ino_t after = 0;
    (void)unused;
    pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
    pthread_mutex_lock(&g_pressure.mu);
    for (;;) {
        while (!g_pressure.pending && !g_pressure.stop)
            pthread_cond_wait(&g_pressure.cv, &g_pressure.mu);
        if (g_pressure.stop)
            break;
        g_pressure.pending = 0;
        pthread_mutex_unlock(&g_pressure.mu);
        for (int n = 0; n < 16 && efs_buf_should_drain(); ++n) {
            pthread_mutex_lock(&g_pressure.mu);
            int stop = g_pressure.stop;
            pthread_mutex_unlock(&g_pressure.mu);
            if (stop)
                break;
            efs_ino_t ino = efs_dcache_pressure_ino(after);
            if (!ino && after) {
                after = 0;
                ino = efs_dcache_pressure_ino(0);
            }
            if (!ino)
                break;
            after = ino;
            efs_buf_drain_enter();
            int rc = fuse_pressure_flush(ino);
            efs_buf_drain_leave();
            if (rc != EFS_OK) {
                /* No success acknowledgement or retirement on failure. The
                 * existing drain retains data and uncertain REPORT ownership;
                 * fsync/close/next pressure must resolve it in the usual way. */
                fprintf(stderr, "background-pressure ino=%llu rc=%d retained\n",
                        (unsigned long long)ino, rc);
                break;
            }
            efs_rdcache_trim();
            efs_dcache_trim_metadata();
        }
        pthread_mutex_lock(&g_pressure.mu);
    }
    pthread_mutex_unlock(&g_pressure.mu);
    return NULL;
}

int fuse_pressure_kick(void)
{
    pthread_mutex_lock(&g_pressure.mu);
    int ready = !g_pressure.stop;
    if (ready && !g_pressure.started) {
        ready = pthread_create(&g_pressure.thread, NULL, fuse_pressure_main, NULL) == 0;
        g_pressure.started = ready;
    }
    if (ready) {
        g_pressure.pending = 1;
        pthread_cond_signal(&g_pressure.cv);
    }
    pthread_mutex_unlock(&g_pressure.mu);
    return ready;
}

void fuse_pressure_stop(void)
{
    pthread_mutex_lock(&g_pressure.mu);
    g_pressure.stop = 1;
    pthread_cond_broadcast(&g_pressure.cv);
    int started = g_pressure.started;
    pthread_mutex_unlock(&g_pressure.mu);
    if (started)
        pthread_join(g_pressure.thread, NULL);
    pthread_mutex_lock(&g_pressure.mu);
    g_pressure.started = 0;
    pthread_mutex_unlock(&g_pressure.mu);
}

/* utimens(mtime) on an inode with buffered writes: publish them first
 * (the same flush+REPORT close would run) so the SETATTR that follows
 * is the later event on the server. Clean inode: nothing. */
int efs_utimens_flush_dirty(efs_ino_t ino)
{
    if (!ino || virt_kind(ino))
        return EFS_OK;
    /* The dirty SET is not "has unpublished data": a threshold REPORT
     * (only_ino=0) snapshots the ino mark that dcache_note_size set and
     * ships an irec-only record while the bytes are still a dirty dcache
     * entry; the set is then clean, the data is not. The dcache pin is
     * held exactly while such entries exist (the evictor keys on the
     * same predicate). Without it this returned early, the SETATTR went
     * out, and the close published the chunk under the new mtime_gen
     * (1–2 of 5760 files per ecopy-shaped burst, Sep 30 trace: the
     * flush line printed with no report, the steal came after the
     * setattr). */
    if (!efs_ino_has_unpublished(ino))
        return EFS_OK;
    return efs_append_flush_report(NULL, ino);
}

/* O_APPEND writes: the kernel sets the offset from its i_size but does not
 * serialize concurrent appends to a FUSE file (no i_rwsem around the
 * read-i_size + write + update-i_size sequence), so two racing appends can
 * read the same i_size and overwrite each other. Serialize them here and
 * re-read the true end from the local table (updated synchronously by each
 * write via dcache_note_size / the writeback worker). */

off_t append_end_offset(efs_ino_t ino)
{
    struct efs_inode cur;
    efs_client_lock_dir(ino);
    pthread_mutex_lock(&g_client.idx_mu);
    uint64_t size = 0;
    if (efs_export_get_inode(&g_client.export, ino, &cur) == 0)
        size = cur.size;
    pthread_mutex_unlock(&g_client.idx_mu);
    efs_client_unlock_dir(ino);
    return (off_t)size;
}

/* Drop the stripe while backing off and reacquire it within the same
 * inherited call budget. On timeout it stays unowned by this caller. */
static int append_retry_wait(efs_ino_t ino,unsigned delay_us,int *held)
{
    pthread_mutex_unlock(append_mu(ino));*held=0;
    uint64_t deadline=efs_client_rpc_deadline_ms(),now=stats_now_ms();
    if(deadline && now>=deadline)return EFS_ERR_BUSY;
    if(deadline && deadline-now<(delay_us+999u)/1000u)
        delay_us=(unsigned)(deadline-now)*1000u;
    usleep(delay_us);
    int rc=wb_lock_budget(append_mu(ino));
    if(rc==EFS_OK)*held=1;
    return rc;
}

/* All local writers honor a publication handoff. In-flight writers finish
 * before the publisher takes the append stripe; later reservations wait
 * without owning it, allowing queued PUT/REPORT work to drain. */
static int append_publication_wait(efs_ino_t ino,int *held)
{
    for (;;) {
        uint64_t now=stats_now_ms();int publishing=0;
        uint64_t until=efs_open_append_handoff(ino,&publishing);
        if(!publishing && until<=now)return EFS_OK;
        unsigned delay=publishing?2000u:(unsigned)((until-now)*1000u);
        int rc=append_retry_wait(ino,delay,held);
        if(rc!=EFS_OK)return rc;
    }
}

/* Receipt admission can wait behind an older OPEN or reservation. Never
 * hold the append stripe while waiting: that older operation may need it
 * to finish and release the contiguous-ACK window. Saturation is local
 * backpressure, not a failed append; retry before allocating a reservation
 * within the caller's inherited budget. Success returns with the stripe. */
static int append_identity_begin(efs_ino_t ino, struct efs_opid_req *q, int *held)
{
    pthread_mutex_unlock(append_mu(ino));
    *held = 0;
    for (;;) {
        if (efs_client_rpc_past_deadline()) return -1;
        int slot = efs_client_append_begin(q);
        if (slot >= 0) {
            if (wb_lock_budget(append_mu(ino)) == EFS_OK) {
                *held = 1;
                return slot;
            }
            efs_client_append_end(slot);
            return -1;
        }
        if (!efs_client_rpc_deadline_ms() || efs_client_rpc_past_deadline())
            return -1;
        usleep(1000);
    }
}

/* K15/W64 client rule: a reserve whose outcome stayed unknown is either
 * replayed with the same identity (the BUSY loop above) or, once this write
 * gives up, explicitly abandoned under the same live session before the
 * identity is released. The server retires the receipt, so the offset can
 * never be learned later, and a reservation that did apply resolves as a
 * hole so the file's frontier advances. If the abandon cannot be delivered
 * within its own bound, session fencing or the last-close drain remains the
 * backstop; the identity is still released (its later ACK forbids any new
 * allocation under it). */
#ifndef EFS_APPEND_ABANDON_MS
#define EFS_APPEND_ABANDON_MS 10000u
#endif
static void append_abandon_identity(efs_ino_t ino, uint64_t len,
                                    const struct efs_opid_req *q)
{
    uint64_t saved = efs_client_rpc_deadline_ms();
    uint64_t until = stats_now_ms() + EFS_APPEND_ABANDON_MS;
    efs_client_rpc_set_deadline_ms(until);
    unsigned delay_us = 2000;
    int rc = EFS_ERR_BUSY, retired = 0;
    while (stats_now_ms() < until) {
        rc = efs_client_rpc_append_abandon(g_client.export_id, ino, len, q,
                                           &retired);
        if (rc == EFS_OK || rc == EFS_ERR_FENCED || rc == EFS_ERR_INVAL ||
            rc == EFS_ERR_PROTO || efs_client_session_fenced())
            break;
        usleep(delay_us);
        if (delay_us < 100000u) delay_us *= 2;
    }
    efs_client_rpc_set_deadline_ms(saved);
    fprintf(stderr, "append-abandon ino=%llu seq=%llu len=%llu rc=%d retired=%d\n",
            (unsigned long long)ino, (unsigned long long)q->id.seq,
            (unsigned long long)len, rc, retired);
    fflush(stderr);
}

/* Cross-client append: reserve the region at the metadata owner so two
 * clients never write at the same end offset. A second client's uuid is
 * BUSY while the first still has OPEN reservations (in-place fragments
 * cannot merge two GET+PUTs). Their close REPORT resolves nopen. On
 * success, drop cached copies of the tail so a later flush merges the
 * published peer. Does not fall back to an unreserved local end. */
/* Caller holds the append stripe. Success retains it; every failure
 * releases it, including expiry while another writer owns the stripe. */
int append_reserve_offset(efs_ino_t ino, uint64_t len, off_t *off_out)
{
    uint64_t ns = 0, start = 0, seq;
    struct efs_opid_req q;
    int slot;
    int rc = EFS_ERR_BUSY, held = 1, sent = 0;

    slot = append_identity_begin(ino, &q, &held);
    if (slot < 0) return -EAGAIN;
    seq = q.id.seq;
    /* Foreign uuid is BUSY until their close REPORT. 2 ms × 15000 was
     * 500 reserve RPCs/s that stalled the live appender (B's 45 s SSH
     * timeout while A was still writing). Back off; cap ~20 s. */
    {
        int delay_us = 2000;
        for (int attempt = 0; attempt < 400; attempt++) {
            if(efs_client_rpc_past_deadline()){rc=EFS_ERR_BUSY;break;}
            rc=append_publication_wait(ino,&held);
            if(rc!=EFS_OK)break;
            sent = 1;
            rc = efs_client_rpc_append_reserve(g_client.export_id, ino, len, &q,
                                               &ns, &start);
            if (rc == EFS_OK && ns >= len)
                break;
            if (rc != EFS_ERR_BUSY) {
                fprintf(stderr,
                        "append_rsv ino=%llu len=%llu rc=%d ns=%llu start=%llu\n",
                        (unsigned long long)ino, (unsigned long long)len, rc,
                        (unsigned long long)ns, (unsigned long long)start);
                fflush(stderr);
                break;
            }
            int wait_rc=append_retry_wait(ino,(unsigned)delay_us,&held);
            if(wait_rc!=EFS_OK){rc=wait_rc;break;}
            if (delay_us < 50000)
                delay_us *= 2;
        }
    }
    if ((rc != EFS_OK || ns < len) && sent && rc != EFS_ERR_FENCED &&
        !efs_client_session_fenced()) {
        /* Never wait for the abandon while owning the append stripe. */
        if (held) { pthread_mutex_unlock(append_mu(ino)); held = 0; }
        append_abandon_identity(ino, len, &q);
    }
    efs_client_append_end(slot);
    /* Never fall back to an unreserved local end: under 9-way that
     * overlapped another process's reservation and dropped ~40 lines.
     * Do not clamp to local_end either — that overwrites a peer's
     * reserved range with this client's bytes. */
    if (rc != EFS_OK || ns < len) {
        if(held)pthread_mutex_unlock(append_mu(ino));
        return -EIO;
    }
    /* The size below is ahead of the PUT. Pin until the caller has the
     * bytes in dcache or the ino is marked dirty. An early dirty mark
     * would release the server's append barrier before the PUT. */
    uint64_t off = (start <= UINT64_MAX - len && start + len == ns) ? start : (ns - len);
    if (efs_dcache_trace_on())
        fprintf(stderr, "append-reserve ino=%llu seq=%llu start=%llu end=%llu len=%llu\n",
                (unsigned long long)ino, (unsigned long long)seq,
                (unsigned long long)start, (unsigned long long)ns,
                (unsigned long long)len);
    uint32_t reserve_cs = g_client.export.chunk_size ? g_client.export.chunk_size : EFS_CHUNK_SIZE;
    if (efs_write_extent_valid(off, len, reserve_cs) != EFS_OK) {
        pthread_mutex_unlock(append_mu(ino));return -EFBIG;
    }
    efs_client_stage_pin(ino);
    if (len > 0) {
        uint32_t cs = g_client.export.chunk_size ? g_client.export.chunk_size
                                                 : EFS_CHUNK_SIZE;
        for (uint64_t ci = off / cs; ci <= (off + len - 1) / cs; ci++)
            efs_rdcache_invalidate(ino, (uint32_t)ci);
    }
    /* Reflect the reservation locally so same-client readers and the next
     * append see the advanced end before our data flushes. The ino is NOT
     * marked dirty here: the append barrier must release only after the data
     * is PUT, and an early report (periodic reporter) carrying the reflected
     * size would release it prematurely. dcache_put_now marks the ino dirty
     * once the flush lands. */
    efs_client_lock_dir(ino);
    pthread_mutex_lock(&g_client.idx_mu);
    struct efs_inode cur;
    /* The server's reserved EOF can move backwards after peer truncate.
     * Keeping the older cached high-water would re-extend it in REPORT. */
    if (efs_export_get_inode(&g_client.export, ino, &cur) == 0)
        efs_export_set_size(&g_client.export, ino, ns);
    pthread_mutex_unlock(&g_client.idx_mu);
    efs_client_unlock_dir(ino);
    if (off_out)
        *off_out = (off_t)off;
    return 0;
}

/* Bound unpublished append ownership even for an fd kept open indefinitely:
 * every 32nd write, or the first after a second of unpublished ownership,
 * publishes before it is accepted (efs_open_append_burst_note, open.h).
 * Never publish a reflected reservation before its bytes: this runs before
 * accepting the NEXT write, through the same queued-write/stripe/PUT/REPORT
 * barrier as fsync. */
int append_publish_burst(efs_ino_t ino)
{
    if (!efs_open_append_burst_note(ino, stats_now_ms()))
        return EFS_OK;
    int rc = efs_append_flush_report(NULL, ino);
    efs_open_append_burst_published(ino, rc, stats_now_ms());
    return rc;
}
