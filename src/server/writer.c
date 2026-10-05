#include "server_internal.h"
#include "efs/checksum.h"
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define EFS_WRITER_QUEUE_CAP 1024

enum writer_op {
    WRITER_OP_FRAGMENT_WITH_SUM = 3,
};

struct writer_job {
    enum writer_op op;
    struct efsd_server *s;
    struct efs_export *ex;
    efs_ino_t ino;
    uint32_t chunk_index;
    uint32_t fragment_index;
    uint32_t path_index; /* local --storage root chosen for this job */
    uint64_t chunk_generation; /* 0 = legacy `{ci}.{fi}` */
    const uint8_t *data;
    uint32_t data_len;
    const uint8_t *checksum;
    int result;
    int path_hint; /* efs_tls_path_hint of the handler; the write runs here */
};

/* Writer-thread hint: fragment body is known-zero (checksum already verified). */
__thread int efs_tls_write_known_zero;
/* Selected --storage root for the in-flight write (see store path helpers). */
__thread int efs_tls_write_root = -1;
__thread int efs_tls_path_hint = -1;
__thread int efs_tls_path_used = -1;

/* One shared queue + thread set for every --storage path. Path choice is a
 * property of the job (stripe / overwrite-in-place), not of the thread. */
struct writer_pool {
    int nwriters;
    int npaths;
    int running;
    pthread_t *threads;
    pthread_mutex_t lock;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
    struct writer_job *queue[EFS_WRITER_QUEUE_CAP];
    int head;
    int tail;
    int count;
    /* Fairness: bytes accepted + jobs not yet finished, per path. */
    uint64_t assigned_bytes[EFS_MAX_STORAGE_PATHS];
    int inflight[EFS_MAX_STORAGE_PATHS];
};

static struct writer_pool g_pool;
static __thread int tls_in_writer;

/* One job slot per writer. The handler hands a job to an idle slot and
 * waits on that slot; there is no shared queue mutex on the PUT path
 * (Oct 1: ~27 futex per fragment was this hand-off). */
/* W46: one condvar per waiter class, so every transition wakes exactly
 * the thread that waits on it — the writer (cv_work: EMPTY→QUEUED), the
 * owning handler (cv_done: QUEUED→DONE), the handlers in the fallback
 * wait (cv_empty: DONE→EMPTY). Before, one cv carried all three and
 * every transition had to broadcast (a signal could have woken the
 * wrong class and left the owner asleep with no timeout); with 64
 * handlers parked on a slot that was 3 futex wakes per fragment for one
 * useful one. pthread_cond_signal with no waiter is a user-space check,
 * no syscall. The fallback class may hold several waiters, so EMPTY
 * still broadcasts on ITS cv; shutdown broadcasts all three. */
enum { WSLOT_EMPTY = 0, WSLOT_QUEUED = 1, WSLOT_DONE = 2 };
struct writer_slot {
    pthread_mutex_t mu;
    pthread_cond_t cv_work;
    pthread_cond_t cv_done;
    pthread_cond_t cv_empty;
    struct writer_job *job;
    int state;
};
static struct writer_slot g_slots[EFS_MAX_WRITERS];
static int g_slot_rr;
static int g_slots_live;

int server_default_writer_threads(void)
{
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    if (n < 1)
        n = EFS_DEFAULT_WRITERS;
    n -= EFS_WRITERS_RESERVED;
    if (n < 1)
        n = 1;
    if (n > EFS_MAX_WRITERS)
        n = EFS_MAX_WRITERS;
    return (int)n;
}

/* Mix inode/chunk/frag/node so stripe members and successive chunks of
 * one file do not all pick the same newly-empty path index. */
static uint32_t path_spread_hash(const struct writer_job *job)
{
    uint64_t k = (uint64_t)job->ino;
    k ^= (uint64_t)job->chunk_index * 0x9E3779B97F4A7C15ULL;
    k ^= (uint64_t)job->fragment_index * 0xBF58476D1CE4E5B9ULL;
    if (job->s)
        k ^= (uint64_t)job->s->id * 0x94D049BB133111EBULL;
    return (uint32_t)(k ^ (k >> 32));
}

/* Pick a local disk: reuse an existing fragment's root on overwrite; else
 * minimize in-flight jobs (latency) with bytes-assigned as tie-break so
 * all paths stay roughly even. Near-ties (new empty disks) are broken by
 * a per-file hash so one inode does not pile onto one path. */
/* existing is the root already holding this fragment, or -1.
 * The caller probes before taking g_pool.lock: the probe used to
 * run under that lock and every PUT waited on six directory walks. */
static uint32_t pick_write_path(struct writer_job *job, int existing)
{
    uint32_t n = (uint32_t)g_pool.npaths;
    if (n <= 1)
        return 0;

    if (existing >= 0 && (uint32_t)existing < n)
        return (uint32_t)existing;

    uint64_t score[EFS_MAX_STORAGE_PATHS];
    uint64_t best_score = UINT64_MAX;
    for (uint32_t i = 0; i < n; i++) {
        /* One in-flight job ≈ 4 MiB of fairness debt so a backed-up disk
         * sheds new work even if historical bytes are slightly behind. */
        score[i] = (uint64_t)__atomic_load_n(&g_pool.inflight[i],
                                            __ATOMIC_RELAXED) *
                       (4ull << 20) +
                   __atomic_load_n(&g_pool.assigned_bytes[i], __ATOMIC_RELAXED);
        if (score[i] < best_score)
            best_score = score[i];
    }
    /* 64 MiB band: empty add-storage roots all qualify; full 01-04 do not. */
    uint64_t slack = 64ull << 20;
    uint32_t cand[EFS_MAX_STORAGE_PATHS];
    uint32_t nc = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (score[i] <= best_score + slack)
            cand[nc++] = i;
    }
    if (nc <= 1)
        return cand[0];
    return cand[path_spread_hash(job) % nc];
}

static int run_job(struct writer_job *job)
{
    int rc = EFS_OK;
    int saved_zero = efs_tls_write_known_zero;
    int saved_root = efs_tls_write_root;
    uint64_t saved_gen = efs_tls_chunk_gen;
    if (job->op == WRITER_OP_FRAGMENT_WITH_SUM && job->checksum &&
        job->data_len > 0) {
        uint8_t zero_ck[EFS_HASH_SIZE];
        efs_hash_zero_fragment_len(job->data_len, zero_ck);
        efs_tls_write_known_zero =
            (memcmp(job->checksum, zero_ck, EFS_HASH_SIZE) == 0);
    } else {
        efs_tls_write_known_zero = 0;
    }
    if (efs_tls_write_root < 0)
        efs_tls_write_root = (int)pick_write_path(
            job, server_find_fragment_root(job->s, job->ex, job->ino,
                                           job->chunk_index,
                                           job->fragment_index));
    efs_tls_chunk_gen = job->chunk_generation;

    {
        uint64_t io_t0 = efs_iostats_now_us();
        switch (job->op) {
        case WRITER_OP_FRAGMENT_WITH_SUM:
            if (job->checksum)
                rc = server_write_fragment_with_sum_sync(job->s, job->ex, job->ino,
                                                         job->chunk_index,
                                                         job->fragment_index,
                                                         job->data, job->data_len,
                                                         job->checksum);
            else
                rc = server_write_fragment_sync(job->s, job->ex, job->ino,
                                                job->chunk_index, job->fragment_index,
                                                job->data, job->data_len);
            break;
        default:
            rc = EFS_ERR_INVAL;
            break;
        }
        efs_iostats_add(EFS_IOSTAT_DISK_WRITE, job->data_len,
                        efs_iostats_now_us() - io_t0, rc != EFS_OK);
    }
    efs_tls_path_used = efs_tls_write_root;
    efs_tls_write_known_zero = saved_zero;
    efs_tls_write_root = saved_root;
    efs_tls_chunk_gen = saved_gen;
    return rc;
}

static void *writer_thread(void *arg)
{
    int idx = (int)(intptr_t)arg;
    struct writer_slot *sl = &g_slots[idx];

    tls_in_writer = 1;
    for (;;) {
        struct writer_job *job;
        int rc;
        int saved_hint;

        pthread_mutex_lock(&sl->mu);
        while (g_pool.running && sl->state != WSLOT_QUEUED)
            pthread_cond_wait(&sl->cv_work, &sl->mu);
        if (!g_pool.running && sl->state != WSLOT_QUEUED) {
            pthread_mutex_unlock(&sl->mu);
            break;
        }
        job = sl->job;
        pthread_mutex_unlock(&sl->mu);

        efs_tls_write_root = (int)job->path_index;
        saved_hint = efs_tls_path_hint;
        efs_tls_path_hint = job->path_hint;
        rc = run_job(job);
        efs_tls_write_root = -1;
        efs_tls_path_hint = saved_hint;

        if (job->path_index < (uint32_t)g_pool.npaths)
            __atomic_fetch_sub(&g_pool.inflight[job->path_index], 1,
                               __ATOMIC_RELAXED);

        pthread_mutex_lock(&sl->mu);
        job->result = rc;
        sl->state = WSLOT_DONE;
        pthread_cond_signal(&sl->cv_done);
        pthread_mutex_unlock(&sl->mu);
    }
    return NULL;
}

/* Caller holds sl->mu and found the slot EMPTY: queue the job, wake the
 * writer, wait for DONE, free the slot for the fallback waiters. */
static int slot_run_locked(struct writer_slot *sl, struct writer_job *job)
{
    int rc;

    sl->job = job;
    sl->state = WSLOT_QUEUED;
    pthread_cond_signal(&sl->cv_work);
    while (sl->state != WSLOT_DONE)
        pthread_cond_wait(&sl->cv_done, &sl->mu);
    rc = job->result;
    sl->state = WSLOT_EMPTY;
    sl->job = NULL;
    pthread_cond_broadcast(&sl->cv_empty);
    return rc;
}

/* Hand job to an idle writer slot and wait for it. 5 s to find a slot;
 * once queued, wait until that writer finishes (the RPC deadline bounds
 * the write itself). Returns EFS_ERR_BUSY if every slot stays occupied. */
static int slot_handoff(struct writer_job *job, int nw, int *queued)
{
    struct timespec deadline;
    unsigned start = (unsigned)__sync_fetch_and_add(&g_slot_rr, 1);
    int k;

    *queued = 0;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += 5;
    for (k = 0; k < nw; k++) {
        struct writer_slot *sl = &g_slots[(start + k) % nw];
        int rc;
        if (pthread_mutex_trylock(&sl->mu) != 0)
            continue;
        if (sl->state != WSLOT_EMPTY || !g_pool.running) {
            pthread_mutex_unlock(&sl->mu);
            continue;
        }
        *queued = 1;
        rc = slot_run_locked(sl, job);
        pthread_mutex_unlock(&sl->mu);
        return rc;
    }
    {
        struct writer_slot *sl = &g_slots[start % nw];
        int rc;
        pthread_mutex_lock(&sl->mu);
        while (sl->state != WSLOT_EMPTY && g_pool.running) {
            if (pthread_cond_timedwait(&sl->cv_empty, &sl->mu, &deadline) ==
                ETIMEDOUT) {
                pthread_mutex_unlock(&sl->mu);
                return EFS_ERR_BUSY;
            }
        }
        if (!g_pool.running || sl->state != WSLOT_EMPTY) {
            pthread_mutex_unlock(&sl->mu);
            return EFS_ERR_IO;
        }
        *queued = 1;
        rc = slot_run_locked(sl, job);
        pthread_mutex_unlock(&sl->mu);
        return rc;
    }
}

static int submit_and_wait(struct writer_job *job)
{
    int existing = -1;
    uint32_t pi;
    int nw;
    int rc;

    job->path_hint = efs_tls_path_hint;
    job->result = EFS_ERR_IO;
    if (g_pool.nwriters <= 0 || tls_in_writer || g_pool.npaths <= 0)
        return run_job(job);

    if (g_pool.npaths > 1)
        existing = server_find_fragment_root(job->s, job->ex, job->ino,
                                             job->chunk_index,
                                             job->fragment_index);
    pi = pick_write_path(job, existing);
    if (pi >= (uint32_t)g_pool.npaths)
        pi = 0;
    job->path_index = pi;
    __atomic_fetch_add(&g_pool.inflight[pi], 1, __ATOMIC_RELAXED);
    __atomic_fetch_add(&g_pool.assigned_bytes[pi],
                       job->data_len ? (uint64_t)job->data_len : 64ull,
                       __ATOMIC_RELAXED);

    nw = g_pool.nwriters;
    if (!g_pool.running || nw <= 0) {
        __atomic_fetch_sub(&g_pool.inflight[pi], 1, __ATOMIC_RELAXED);
        efs_tls_write_root = (int)pi;
        rc = run_job(job);
        efs_tls_write_root = -1;
        return rc;
    }
    {
        int queued = 0;
        rc = slot_handoff(job, nw, &queued);
        /* The writer decrements inflight only after it runs the job. */
        if (!queued)
            __atomic_fetch_sub(&g_pool.inflight[pi], 1, __ATOMIC_RELAXED);
    }
    efs_tls_path_used = (int)job->path_index;
    return rc;
}

void server_writer_set_npaths(uint32_t n)
{
    if (n < 1)
        n = 1;
    if (n > EFS_MAX_STORAGE_PATHS)
        n = EFS_MAX_STORAGE_PATHS;
    pthread_mutex_lock(&g_pool.lock);
    g_pool.npaths = (int)n;
    pthread_mutex_unlock(&g_pool.lock);
}

int server_writer_pool_start(struct efsd_server *s)
{
    memset(&g_pool, 0, sizeof(g_pool));
    int nw = s->nwriters;
    if (nw < 0)
        nw = server_default_writer_threads();
    s->nwriters = nw;
    g_pool.nwriters = nw;
    if (nw <= 0)
        return 0;

    uint32_t npaths = s->storage_path_count ? s->storage_path_count : 1;
    if (npaths > EFS_MAX_STORAGE_PATHS)
        npaths = EFS_MAX_STORAGE_PATHS;
    g_pool.npaths = (int)npaths;

    pthread_mutex_init(&g_pool.lock, NULL);
    pthread_cond_init(&g_pool.not_empty, NULL);
    pthread_cond_init(&g_pool.not_full, NULL);
    g_pool.threads = calloc((size_t)nw, sizeof(pthread_t));
    if (!g_pool.threads) {
        server_writer_pool_stop(s);
        return -1;
    }
    g_pool.running = 1;
    for (int i = 0; i < nw; i++) {
        pthread_mutex_init(&g_slots[i].mu, NULL);
        pthread_cond_init(&g_slots[i].cv_work, NULL);
        pthread_cond_init(&g_slots[i].cv_done, NULL);
        pthread_cond_init(&g_slots[i].cv_empty, NULL);
        g_slots[i].job = NULL;
        g_slots[i].state = WSLOT_EMPTY;
    }
    g_slots_live = nw;
    for (int i = 0; i < nw; i++) {
        if (efsd_pthread_create(&g_pool.threads[i], writer_thread,
                                (void *)(intptr_t)i) != 0) {
            g_pool.running = 0;
            g_pool.nwriters = i;
            server_writer_pool_stop(s);
            return -1;
        }
    }
    return 0;
}

void server_writer_pool_stop(struct efsd_server *s)
{
    (void)s;
    if (g_pool.npaths <= 0 && !g_pool.threads)
        return;

    if (g_pool.threads) {
        pthread_mutex_lock(&g_pool.lock);
        g_pool.running = 0;
        pthread_cond_broadcast(&g_pool.not_empty);
        pthread_cond_broadcast(&g_pool.not_full);
        pthread_mutex_unlock(&g_pool.lock);
        for (int i = 0; i < g_slots_live; i++) {
            pthread_mutex_lock(&g_slots[i].mu);
            pthread_cond_broadcast(&g_slots[i].cv_work);
            pthread_cond_broadcast(&g_slots[i].cv_done);
            pthread_cond_broadcast(&g_slots[i].cv_empty);
            pthread_mutex_unlock(&g_slots[i].mu);
        }
        for (int i = 0; i < g_pool.nwriters; i++)
            pthread_join(g_pool.threads[i], NULL);
        for (int i = 0; i < g_slots_live; i++) {
            pthread_mutex_destroy(&g_slots[i].mu);
            pthread_cond_destroy(&g_slots[i].cv_work);
            pthread_cond_destroy(&g_slots[i].cv_done);
            pthread_cond_destroy(&g_slots[i].cv_empty);
            g_slots[i].state = WSLOT_EMPTY;
            g_slots[i].job = NULL;
        }
        g_slots_live = 0;
        free(g_pool.threads);
        g_pool.threads = NULL;
    }
    if (g_pool.npaths > 0 || g_pool.nwriters > 0) {
        pthread_mutex_destroy(&g_pool.lock);
        pthread_cond_destroy(&g_pool.not_empty);
        pthread_cond_destroy(&g_pool.not_full);
    }
    g_pool.npaths = 0;
    g_pool.nwriters = 0;
}

int server_write_fragment_with_sum(struct efsd_server *s, struct efs_export *ex,
                                   efs_ino_t ino, uint32_t chunk_index,
                                   uint32_t fragment_index,
                                   const uint8_t *data, uint32_t data_len,
                                   const uint8_t checksum[EFS_HASH_SIZE])
{
    struct writer_job job;
    memset(&job, 0, sizeof(job));
    job.op = WRITER_OP_FRAGMENT_WITH_SUM;
    job.s = s;
    job.ex = ex;
    job.ino = ino;
    job.chunk_index = chunk_index;
    job.fragment_index = fragment_index;
    job.chunk_generation = efs_tls_chunk_gen;
    job.data = data;
    job.data_len = data_len;
    job.checksum = checksum;
    return submit_and_wait(&job);
}
