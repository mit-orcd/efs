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
    int done;
    pthread_mutex_t done_mu;
    pthread_cond_t done_cv;
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
        score[i] = (uint64_t)g_pool.inflight[i] * (4ull << 20) +
                   g_pool.assigned_bytes[i];
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
    efs_tls_path_used = efs_tls_write_root;
    efs_tls_write_known_zero = saved_zero;
    efs_tls_write_root = saved_root;
    efs_tls_chunk_gen = saved_gen;
    return rc;
}

static void *writer_thread(void *arg)
{
    (void)arg;
    tls_in_writer = 1;
    for (;;) {
        pthread_mutex_lock(&g_pool.lock);
        while (g_pool.running && g_pool.count == 0)
            pthread_cond_wait(&g_pool.not_empty, &g_pool.lock);
        if (!g_pool.running && g_pool.count == 0) {
            pthread_mutex_unlock(&g_pool.lock);
            break;
        }
        struct writer_job *job = g_pool.queue[g_pool.head];
        efs_tls_write_root = (int)job->path_index;
        g_pool.head = (g_pool.head + 1) % EFS_WRITER_QUEUE_CAP;
        g_pool.count--;
        pthread_cond_signal(&g_pool.not_full);
        pthread_mutex_unlock(&g_pool.lock);

        int rc = run_job(job);
        efs_tls_write_root = -1;

        pthread_mutex_lock(&g_pool.lock);
        if (job->path_index < (uint32_t)g_pool.npaths &&
            g_pool.inflight[job->path_index] > 0)
            g_pool.inflight[job->path_index]--;
        pthread_mutex_unlock(&g_pool.lock);

        pthread_mutex_lock(&job->done_mu);
        job->result = rc;
        job->done = 1;
        pthread_cond_signal(&job->done_cv);
        pthread_mutex_unlock(&job->done_mu);
    }
    return NULL;
}

static int submit_and_wait(struct writer_job *job)
{
    if (g_pool.nwriters <= 0 || tls_in_writer || g_pool.npaths <= 0)
        return run_job(job);

    pthread_mutex_init(&job->done_mu, NULL);
    pthread_cond_init(&job->done_cv, NULL);
    job->done = 0;
    job->result = EFS_ERR_IO;

    int existing = -1;
    if (g_pool.npaths > 1)
        existing = server_find_fragment_root(job->s, job->ex, job->ino,
                                             job->chunk_index,
                                             job->fragment_index);
    pthread_mutex_lock(&g_pool.lock);
    uint32_t pi = pick_write_path(job, existing);
    if (pi >= (uint32_t)g_pool.npaths)
        pi = 0;
    job->path_index = pi;

    /* Bounded wait for queue space: blocking forever parks a conn thread per
     * queued PUT (up to 512) with no client-visible backpressure. After the
     * deadline return EBUSY so the client retries instead of stalling. */
    if (g_pool.running && g_pool.count == EFS_WRITER_QUEUE_CAP) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += 5;
        while (g_pool.running && g_pool.count == EFS_WRITER_QUEUE_CAP) {
            if (pthread_cond_timedwait(&g_pool.not_full, &g_pool.lock, &ts) ==
                ETIMEDOUT) {
                pthread_mutex_unlock(&g_pool.lock);
                pthread_mutex_destroy(&job->done_mu);
                pthread_cond_destroy(&job->done_cv);
                return EFS_ERR_BUSY;
            }
        }
    }
    if (!g_pool.running) {
        pthread_mutex_unlock(&g_pool.lock);
        pthread_mutex_destroy(&job->done_mu);
        pthread_cond_destroy(&job->done_cv);
        efs_tls_write_root = (int)pi;
        int rc = run_job(job);
        efs_tls_write_root = -1;
        return rc;
    }
    g_pool.queue[g_pool.tail] = job;
    g_pool.tail = (g_pool.tail + 1) % EFS_WRITER_QUEUE_CAP;
    g_pool.count++;
    g_pool.inflight[pi]++;
    g_pool.assigned_bytes[pi] += job->data_len ? job->data_len : 64u;
    pthread_cond_signal(&g_pool.not_empty);
    pthread_mutex_unlock(&g_pool.lock);

    pthread_mutex_lock(&job->done_mu);
    while (!job->done)
        pthread_cond_wait(&job->done_cv, &job->done_mu);
    int rc = job->result;
    pthread_mutex_unlock(&job->done_mu);

    pthread_mutex_destroy(&job->done_mu);
    pthread_cond_destroy(&job->done_cv);
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
        if (efsd_pthread_create(&g_pool.threads[i], writer_thread, NULL) != 0) {
            pthread_mutex_lock(&g_pool.lock);
            g_pool.running = 0;
            pthread_cond_broadcast(&g_pool.not_empty);
            pthread_mutex_unlock(&g_pool.lock);
            for (int j = 0; j < i; j++)
                pthread_join(g_pool.threads[j], NULL);
            free(g_pool.threads);
            g_pool.threads = NULL;
            g_pool.nwriters = 0;
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
        for (int i = 0; i < g_pool.nwriters; i++)
            pthread_join(g_pool.threads[i], NULL);
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
