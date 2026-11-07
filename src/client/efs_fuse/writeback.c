/* efs-fuse: write-back pool and bounce buffers. */
#include "fuse_internal.h"
#include "writeback.h"
#include "support.h"
#include "getattr.h"
#include "open.h"

/* Pool state: private to this file (contract in writeback.h). */
#define EFS_WB_DEPTH        256

#define EFS_WB_WORKERS_MAX  16

#define EFS_WB_RESERVED     4

struct efs_wb_job {
    efs_ino_t ino;
    uint64_t offset;
    size_t size;
    char *buf;
    /* If set, this is the allocation that contains buf (stolen FUSE
     * receive buffer). Free this instead of buf when the job completes. */
    char *free_base;
    /* Non-zero: buf/free_base came from the bounce pool (see bounce_release). */
    size_t buf_cap;
    uint64_t queued_ms;
    uint64_t deadline;
    /* Caller waits for PUT quorum. Worker writes rc and signals done_cv
     * while holding g_wb.mu. */
    int *done;
    int *done_rc;
    pthread_cond_t *done_cv;
};

/* g_wb.mu covers every field; not_empty / not_full / idle are its
 * condition variables. */
struct efs_wb_state {
    pthread_mutex_t mu;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
    pthread_cond_t idle;
    struct efs_wb_job q[EFS_WB_DEPTH];
    int head, tail, count, inflight;
    uint64_t queued_bytes, inflight_bytes, admission_waits, admission_timeouts;
    int err;
    efs_ino_t err_ino; /* inode that set err; 0 = none / global */
    int ready;
    int shutdown;
    pthread_t workers[EFS_WB_WORKERS_MAX];
    efs_ino_t busy_ino[EFS_WB_WORKERS_MAX];
    uint64_t busy_off[EFS_WB_WORKERS_MAX];
    size_t busy_len[EFS_WB_WORKERS_MAX];
    int nworkers;
};

/* Per-ino write serialization. WB workers run jobs concurrently, but two jobs
 * touching the same inode (overlapping partial chunks) must not RMW the same
 * chunk concurrently or they lose updates. Striped locks keep parallelism
 * across inodes while serializing within one. */
#define EFS_WB_INO_STRIPES 64
static pthread_mutex_t g_wb_ino_mu[EFS_WB_INO_STRIPES];
static pthread_once_t g_wb_ino_once = PTHREAD_ONCE_INIT;
static void wb_ino_init(void)
{
    for (int i = 0; i < EFS_WB_INO_STRIPES; i++)
        pthread_mutex_init(&g_wb_ino_mu[i], NULL);
}
static pthread_mutex_t *efs_wb_ino_lock(efs_ino_t ino)
{
    pthread_once(&g_wb_ino_once, wb_ino_init);
    return &g_wb_ino_mu[efs_ino_lock_slot(ino, EFS_WB_INO_STRIPES)];
}

static struct efs_wb_state g_wb = {
    .mu = PTHREAD_MUTEX_INITIALIZER,
    .not_empty = PTHREAD_COND_INITIALIZER,
    .not_full = PTHREAD_COND_INITIALIZER,
    .idle = PTHREAD_COND_INITIALIZER,
};

/* glibc mmaps allocations >= 128 KiB. Every 128k/1M write_buf was then
 * mmap + 256 page-faults + munmap, so a single psync stream's FUSE ACK
 * took about as long as the PUT and the WB queue never piled up. */
#define EFS_BOUNCE_POOL   256
#define EFS_BOUNCE_SMALL  (128u << 10)
#define EFS_BOUNCE_LARGE  (1u << 20)

static struct {
    pthread_mutex_t mu;
    char *p[EFS_BOUNCE_POOL];
    size_t cap[EFS_BOUNCE_POOL];
    int n;
} g_bounce = { .mu = PTHREAD_MUTEX_INITIALIZER };

static char *bounce_fresh(size_t cap)
{
    char *p = malloc(cap);
    if (!p)
        return NULL;
    /* Fault the pages once so the first write_buf memcpy is not the tax. */
    for (size_t o = 0; o < cap; o += 4096)
        p[o] = 0;
    if (cap)
        p[cap - 1] = 0;
    return p;
}

char *bounce_alloc(size_t size, size_t *cap_out)
{
    if (size < EFS_BOUNCE_SMALL) {
        char *p = malloc(size);
        if (cap_out)
            *cap_out = 0;
        return p;
    }
    size_t want = (size <= EFS_BOUNCE_LARGE) ? EFS_BOUNCE_LARGE : size;
    if (size <= EFS_BOUNCE_SMALL)
        want = EFS_BOUNCE_SMALL;
    pthread_mutex_lock(&g_bounce.mu);
    for (int i = g_bounce.n - 1; i >= 0; i--) {
        if (g_bounce.cap[i] >= size) {
            char *p = g_bounce.p[i];
            size_t cap = g_bounce.cap[i];
            g_bounce.n--;
            g_bounce.p[i] = g_bounce.p[g_bounce.n];
            g_bounce.cap[i] = g_bounce.cap[g_bounce.n];
            pthread_mutex_unlock(&g_bounce.mu);
            if (cap_out)
                *cap_out = cap;
            return p;
        }
    }
    pthread_mutex_unlock(&g_bounce.mu);
    char *p = bounce_fresh(want);
    if (cap_out)
        *cap_out = p ? want : 0;
    return p;
}

void bounce_release(char *p, size_t cap)
{
    if (!p)
        return;
    if (!cap) {
        free(p);
        return;
    }
    pthread_mutex_lock(&g_bounce.mu);
    if (g_bounce.n < EFS_BOUNCE_POOL) {
        g_bounce.p[g_bounce.n] = p;
        g_bounce.cap[g_bounce.n] = cap;
        g_bounce.n++;
        pthread_mutex_unlock(&g_bounce.mu);
        return;
    }
    pthread_mutex_unlock(&g_bounce.mu);
    free(p);
}

static void wb_job_release_buf(char *copy, char *free_base, size_t buf_cap)
{
    char *p = free_base ? free_base : copy;
    bounce_release(p, buf_cap);
}

void efs_fuse_log_err(const char *where, int efs_rc, efs_ino_t ino,
                             uint64_t offset, size_t size, const char *path)
{
    if (path && path[0]) {
        fprintf(stderr,
                "efs-fuse %s: %s (efs_rc=%d) path=%s ino=%llu off=%llu len=%zu\n",
                where, efs_strerror(efs_rc), efs_rc, path,
                (unsigned long long)ino, (unsigned long long)offset, size);
    } else {
        fprintf(stderr,
                "efs-fuse %s: %s (efs_rc=%d) ino=%llu off=%llu len=%zu\n",
                where, efs_strerror(efs_rc), efs_rc,
                (unsigned long long)ino, (unsigned long long)offset, size);
    }
    fflush(stderr);
}

static int wb_ranges_overlap(uint64_t a0, size_t al, uint64_t b0, size_t bl)
{
    if (!al || !bl)
        return 0;
    return a0 < b0 + (uint64_t)bl && b0 < a0 + (uint64_t)al;
}

/* Expand a partial write to whole chunks so two 4k RMWs of the same
 * 128 KiB chunk cannot GET/PUT in parallel (lost update → later EIO). */
static void wb_claim_span(uint64_t off, size_t len, uint32_t cs,
                          uint64_t *out_off, size_t *out_len)
{
    if (!cs || !len ||
        (len >= cs && (off % cs) == 0 && (len % cs) == 0)) {
        *out_off = off;
        *out_len = len;
        return;
    }
    uint64_t a = (off / cs) * cs;
    uint64_t b = (off + (uint64_t)len + cs - 1) / cs * cs;
    *out_off = a;
    *out_len = (size_t)(b - a);
}

/* Caller holds g_wb.mu. True if another worker already claimed an
 * overlapping range of this inode (aligned PUT vs RMW GET race). */
static int wb_overlap_inflight(int self, efs_ino_t ino, uint64_t off, size_t len)
{
    for (int i = 0; i < g_wb.nworkers; i++) {
        if (i == self || g_wb.busy_ino[i] != ino || !g_wb.busy_len[i])
            continue;
        if (wb_ranges_overlap(off, len, g_wb.busy_off[i], g_wb.busy_len[i]))
            return 1;
    }
    return 0;
}

/* Deadline-aware waits preserve accepted-job ownership until completion. */
static int wb_wait_idle_budget(void)
{
    uint64_t deadline = efs_client_rpc_deadline_ms();
    if (!deadline) {
        return pthread_cond_wait(&g_wb.idle, &g_wb.mu) == 0
               ? EFS_OK : EFS_ERR_IO;
    }
    uint64_t now = stats_now_ms();
    if (now >= deadline)
        return EFS_ERR_BUSY;
    uint64_t wait = deadline - now;
    if (wait > 50) wait = 50;
    struct timespec until;
    clock_gettime(CLOCK_REALTIME, &until);
    until.tv_nsec += (long)wait * 1000000;
    until.tv_sec += until.tv_nsec / 1000000000;
    until.tv_nsec %= 1000000000;
    int rc = pthread_cond_timedwait(&g_wb.idle, &g_wb.mu, &until);
    if (rc && rc != ETIMEDOUT)
        return EFS_ERR_IO;
    return efs_client_rpc_past_deadline() ? EFS_ERR_BUSY : EFS_OK;
}



static void *efs_wb_thread(void *arg)
{
    int wid = (int)(intptr_t)arg;
    for (;;) {
        pthread_mutex_lock(&g_wb.mu);
        while (g_wb.count == 0 && !g_wb.shutdown)
            pthread_cond_wait(&g_wb.not_empty, &g_wb.mu);
        if (g_wb.shutdown && g_wb.count == 0) {
            pthread_mutex_unlock(&g_wb.mu);
            return NULL;
        }
        struct efs_wb_job job = g_wb.q[g_wb.head];
        uint64_t previous = efs_client_rpc_deadline_ms();
        efs_client_rpc_set_deadline_ms(job.deadline);
        int rc = EFS_OK;
        g_wb.head = (g_wb.head + 1) % EFS_WB_DEPTH;
        g_wb.count--;
        g_wb.queued_bytes -= job.size;
        g_wb.inflight_bytes += job.size;
        g_wb.inflight++;
        pthread_cond_signal(&g_wb.not_full);
        /* Own the inode from the pop on. A job that was waiting below
         * for an overlapping in-flight job was in neither the queue nor
         * busy_ino[], so efs_wb_ino_pending_locked() said "nothing
         * pending" between the first job's finish and this worker
         * re-taking the mutex: utimens flushed + SETATTR, then this
         * job's PUT re-dirtied the chunk and close published it under
         * the new mtime_gen — the ecopy write(32k) + pwrite(tail) →
         * futimens → close files whose mtime was the close time
         * (Sep 30, ecopy.strace 03:17:43, 29 of ~10k files).
         * busy_len stays 0 until the claim, so wb_overlap_inflight()
         * skips this slot and two waiters cannot deadlock on each
         * other. */
        g_wb.busy_ino[wid] = job.ino;
        g_wb.busy_off[wid] = 0;
        g_wb.busy_len[wid] = 0;
        /* Claim the range only after overlapping in-flight jobs finish.
         * writeback_cache can deliver an unaligned window while a full
         * overwrite of the same chunks is still PUTting; RMW GET then
         * misses and used to return EIO. Partial 4k jobs claim the
         * whole chunk so they do not RMW the same 128 KiB in parallel. */
        uint32_t claim_cs = fuse_chunk_size();
        uint64_t claim_off = job.offset;
        size_t claim_len = job.size;
        wb_claim_span(job.offset, job.size, claim_cs, &claim_off, &claim_len);
        while (wb_overlap_inflight(wid, job.ino, claim_off, claim_len)) {
            rc = wb_wait_idle_budget();
            if (rc != EFS_OK)
                break;
        }
        if (efs_client_rpc_past_deadline())
            rc = EFS_ERR_BUSY;
        if (rc == EFS_OK) {
            g_wb.busy_off[wid] = claim_off;
            g_wb.busy_len[wid] = claim_len;
        }
        pthread_mutex_unlock(&g_wb.mu);

        /* Partial-chunk RMW is not atomic. Full-chunk overwrites of
         * distinct ranges are independent — the per-ino lock used to
         * force 128k×8 and 1M×1 through one PUT at a time. */
        uint32_t cs = fuse_chunk_size();
        int aligned = cs && job.size >= cs &&
                      (job.offset % cs) == 0 && (job.size % cs) == 0;
        pthread_mutex_t *ilock = NULL;
        if (rc == EFS_OK && !aligned) {
            ilock = efs_wb_ino_lock(job.ino);
            rc = wb_lock_budget(ilock);
            if (rc != EFS_OK)
                ilock = NULL;
        }
        if (rc == EFS_OK) {
            lookup_memo_mutation_begin();
            rc = efs_client_write_no_replicate(job.ino, job.offset, job.size,
                                              job.buf);
            lookup_memo_mutation_end();
        }
        if (ilock)
            pthread_mutex_unlock(ilock);
        wb_job_release_buf(job.buf, job.free_base, job.buf_cap);
        efs_client_rpc_set_deadline_ms(previous);

        pthread_mutex_lock(&g_wb.mu);
        if (job.done) {
            *job.done_rc = rc;
            *job.done = 1;
            pthread_cond_signal(job.done_cv);
        }
        if (rc != EFS_OK) {
            if (g_wb.err == EFS_OK) {
                g_wb.err = rc;
                g_wb.err_ino = job.ino;
                /* Log outside? hold lock briefly — fprintf is fine for errors. */
                efs_fuse_log_err("writeback", rc, job.ino, job.offset, job.size,
                                 NULL);
            } else {
                /* Follow-on failures after the first sticky error. */
                fprintf(stderr,
                        "efs-fuse writeback: also failed %s (efs_rc=%d) "
                        "ino=%llu off=%llu len=%zu (sticky_err=%s)\n",
                        efs_strerror(rc), rc,
                        (unsigned long long)job.ino,
                        (unsigned long long)job.offset, job.size,
                        efs_strerror(g_wb.err));
                fflush(stderr);
            }
        }
        g_wb.inflight--;
        g_wb.inflight_bytes -= job.size;
        g_wb.busy_ino[wid] = 0;
        g_wb.busy_off[wid] = 0;
        g_wb.busy_len[wid] = 0;
        pthread_cond_broadcast(&g_wb.idle);
        pthread_mutex_unlock(&g_wb.mu);
    }
}

static int wb_worker_count(void)
{
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    if (n < 1)
        n = 4;
    n -= EFS_WB_RESERVED;
    if (n < 4)
        n = 4;
    if (n > EFS_WB_WORKERS_MAX)
        n = EFS_WB_WORKERS_MAX;
    return (int)n;
}

static int efs_wb_ensure(void)
{
    if (g_wb.ready)
        return 0;
    pthread_mutex_lock(&g_wb.mu);
    if (!g_wb.ready) {
        g_wb.nworkers = wb_worker_count();
        for (int i = 0; i < g_wb.nworkers; i++) {
            if (pthread_create(&g_wb.workers[i], NULL, efs_wb_thread,
                               (void *)(intptr_t)i) != 0) {
                g_wb.shutdown = 1;
                pthread_cond_broadcast(&g_wb.not_empty);
                pthread_mutex_unlock(&g_wb.mu);
                for (int j = 0; j < i; j++)
                    pthread_join(g_wb.workers[j], NULL);
                g_wb.shutdown = 0;
                return -1;
            }
        }
        g_wb.ready = 1;
    }
    pthread_mutex_unlock(&g_wb.mu);
    return 0;
}

/* Enqueue a WB job taking ownership of an already-allocated buffer.
 * free_base, if non-NULL, is the pointer to free when the job finishes
 * (stolen FUSE receive buffer); otherwise `copy` is released.
 * buf_cap != 0 means the buffer came from bounce_alloc and goes back
 * to the pool; 0 means a plain malloc.
 * Blocks until the PUT has 2-of-3 fragment quorum (or fails). */
int efs_wb_enqueue_owned(efs_ino_t ino, uint64_t offset, size_t size,
                                char *copy, char *free_base, size_t buf_cap)
{
    if (efs_wb_ensure() != 0) {
        wb_job_release_buf(copy, free_base, buf_cap);
        return EFS_ERR_NOMEM;
    }
    int done = 0;
    int job_rc = EFS_ERR_IO;
    pthread_cond_t done_cv;
    pthread_cond_init(&done_cv, NULL);
    pthread_mutex_lock(&g_wb.mu);
    /* Do not refuse new files because an earlier inode's PUT failed.
     * That sticky g_wb.err used to make every later write/close EIO
     * (including empty touch) after one quorum blip. */
    uint64_t wait_end = stats_now_ms() + 8000;
    uint64_t outer = efs_client_rpc_deadline_ms();
    if (outer && outer < wait_end)
        wait_end = outer;
    int waited = 0;
    while (g_wb.count == EFS_WB_DEPTH && !g_wb.shutdown) {
        uint64_t now = stats_now_ms();
        if (!waited) {
            waited = 1;
            ++g_wb.admission_waits;
            fprintf(stderr, "write-queue pressure jobs=%d queued_bytes=%llu "
                    "inflight_bytes=%llu oldest_ms=%llu waits=%llu timeouts=%llu\n",
                    g_wb.count, (unsigned long long)g_wb.queued_bytes,
                    (unsigned long long)g_wb.inflight_bytes,
                    (unsigned long long)(now - g_wb.q[g_wb.head].queued_ms),
                    (unsigned long long)g_wb.admission_waits,
                    (unsigned long long)g_wb.admission_timeouts);
        }
        if (now >= wait_end) {
            ++g_wb.admission_timeouts;
            pthread_mutex_unlock(&g_wb.mu);
            pthread_cond_destroy(&done_cv);
            wb_job_release_buf(copy, free_base, buf_cap);
            return EFS_ERR_BUSY; /* not enqueued; no worker owns caller state */
        }
        struct timespec until;
        clock_gettime(CLOCK_REALTIME, &until);
        uint64_t wait = wait_end - now;
        if (wait > 50) wait = 50;
        until.tv_nsec += (long)wait * 1000000;
        until.tv_sec += until.tv_nsec / 1000000000;
        until.tv_nsec %= 1000000000;
        pthread_cond_timedwait(&g_wb.not_full, &g_wb.mu, &until);
    }
    if (g_wb.shutdown) {
        pthread_mutex_unlock(&g_wb.mu);
        pthread_cond_destroy(&done_cv);
        wb_job_release_buf(copy, free_base, buf_cap);
        return EFS_ERR_IO;
    }
    if (stats_now_ms() >= wait_end) {
        ++g_wb.admission_timeouts;
        pthread_mutex_unlock(&g_wb.mu);
        pthread_cond_destroy(&done_cv);
        wb_job_release_buf(copy, free_base, buf_cap);
        return EFS_ERR_BUSY;
    }
    g_wb.q[g_wb.tail].deadline = outer ? outer : stats_now_ms() + 30000;
    g_wb.q[g_wb.tail].ino = ino;
    g_wb.q[g_wb.tail].offset = offset;
    g_wb.q[g_wb.tail].size = size;
    g_wb.q[g_wb.tail].buf = copy;
    g_wb.q[g_wb.tail].free_base = free_base;
    g_wb.q[g_wb.tail].buf_cap = buf_cap;
    g_wb.q[g_wb.tail].queued_ms = stats_now_ms();
    g_wb.queued_bytes += size;
    g_wb.q[g_wb.tail].done = &done;
    g_wb.q[g_wb.tail].done_rc = &job_rc;
    g_wb.q[g_wb.tail].done_cv = &done_cv;
    g_wb.tail = (g_wb.tail + 1) % EFS_WB_DEPTH;
    g_wb.count++;
    pthread_cond_signal(&g_wb.not_empty);
    while (!done)
        pthread_cond_wait(&done_cv, &g_wb.mu);
    pthread_mutex_unlock(&g_wb.mu);
    pthread_cond_destroy(&done_cv);
    return job_rc;
}

int efs_wb_sync(void)
{
    if (!g_wb.ready)
        return EFS_OK;
    int rc = wb_lock_budget(&g_wb.mu);
    if (rc != EFS_OK)
        return rc;
    while (g_wb.count > 0 || g_wb.inflight > 0) {
        rc = wb_wait_idle_budget();
        if (rc != EFS_OK) {
            pthread_mutex_unlock(&g_wb.mu);
            return rc;
        }
    }
    int err = g_wb.err;
    g_wb.err = EFS_OK;
    g_wb.err_ino = 0;
    pthread_mutex_unlock(&g_wb.mu);
    return err;
}

static int efs_wb_ino_pending_locked(efs_ino_t ino)
{
    for (int i = 0; i < g_wb.count; i++) {
        int idx = (g_wb.head + i) % EFS_WB_DEPTH;
        if (g_wb.q[idx].ino == ino)
            return 1;
    }
    for (int i = 0; i < g_wb.nworkers; i++) {
        if (g_wb.busy_ino[i] == ino)
            return 1;
    }
    return 0;
}

int efs_wb_pending(efs_ino_t ino)
{
    int pending = 0;

    if (g_wb.ready) {
        pthread_mutex_lock(&g_wb.mu);
        pending = ino ? efs_wb_ino_pending_locked(ino)
                      : g_wb.count + g_wb.inflight;
        pthread_mutex_unlock(&g_wb.mu);
    }
    return pending;
}

void efs_wb_stop(void)
{
    if (g_wb.ready) {
        pthread_mutex_lock(&g_wb.mu);
        g_wb.shutdown = 1;
        pthread_cond_broadcast(&g_wb.not_empty);
        pthread_mutex_unlock(&g_wb.mu);
        for (int i = 0; i < g_wb.nworkers; i++)
            pthread_join(g_wb.workers[i], NULL);
        g_wb.ready = 0;
        g_wb.shutdown = 0;
    }
}

/* Drain only this inode's writeback jobs so close/ecopy does not wait
 * for every other file in the pool. */
int efs_wb_sync_ino(efs_ino_t ino)
{
    if (!g_wb.ready)
        return EFS_OK;
    int rc = wb_lock_budget(&g_wb.mu);
    if (rc != EFS_OK)
        return rc;
    while (efs_wb_ino_pending_locked(ino)) {
        rc = wb_wait_idle_budget();
        if (rc != EFS_OK) {
            pthread_mutex_unlock(&g_wb.mu);
            return rc;
        }
    }
    int err = EFS_OK;
    if (g_wb.err != EFS_OK && g_wb.err_ino == ino) {
        err = g_wb.err;
        g_wb.err = EFS_OK;
        g_wb.err_ino = 0;
    }
    pthread_mutex_unlock(&g_wb.mu);
    return err;
}
