#include "server_internal.h"
#include "efs/checksum.h"
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define EFS_WRITER_QUEUE_CAP 256

enum writer_op {
    WRITER_OP_FRAGMENT = 1,
    WRITER_OP_FRAGMENT_SUM = 2,
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

/* One queue + thread set per storage path (stripe lane). */
struct path_writer_pool {
    int nwriters;
    int running;
    pthread_t *threads;
    pthread_mutex_t lock;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
    struct writer_job *queue[EFS_WRITER_QUEUE_CAP];
    int head;
    int tail;
    int count;
};

struct writer_pools {
    int npaths;
    int writers_per_path;
    struct path_writer_pool paths[EFS_MAX_STORAGE_PATHS];
    /* Long-term fairness: bytes successfully queued to each path. */
    uint64_t assigned_bytes[EFS_MAX_STORAGE_PATHS];
};

static struct writer_pools g_pools;
static __thread int tls_in_writer;

/* Pick a local disk: reuse an existing fragment's root on overwrite; else
 * minimize writer-queue depth (latency) with bytes-assigned as tie-break so
 * all paths stay roughly even under sequential EC placement. */
static uint32_t pick_write_path(struct writer_job *job)
{
    uint32_t n = (uint32_t)g_pools.npaths;
    if (n <= 1)
        return 0;

    int existing = server_find_fragment_root(job->s, job->ex, job->ino,
                                             job->chunk_index,
                                             job->fragment_index);
    if (existing >= 0 && (uint32_t)existing < n)
        return (uint32_t)existing;

    uint32_t best = 0;
    uint64_t best_score = UINT64_MAX;
    for (uint32_t i = 0; i < n; i++) {
        struct path_writer_pool *pool = &g_pools.paths[i];
        int q = 0;
        pthread_mutex_lock(&pool->lock);
        q = pool->count;
        pthread_mutex_unlock(&pool->lock);
        /* One queued job ≈ 4 MiB of fairness debt so a backed-up disk sheds
         * new work even if historical bytes are slightly behind. */
        uint64_t score = (uint64_t)q * (4ull << 20) + g_pools.assigned_bytes[i];
        if (score < best_score) {
            best_score = score;
            best = i;
        }
    }
    return best;
}

static int run_job(struct writer_job *job)
{
    int rc = EFS_OK;
    int saved_zero = efs_tls_write_known_zero;
    int saved_root = efs_tls_write_root;
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
        efs_tls_write_root = (int)pick_write_path(job);

    switch (job->op) {
    case WRITER_OP_FRAGMENT:
        rc = server_write_fragment_sync(job->s, job->ex, job->ino,
                                        job->chunk_index, job->fragment_index,
                                        job->data, job->data_len);
        break;
    case WRITER_OP_FRAGMENT_SUM:
        rc = server_write_fragment_sum_sync(job->s, job->ex, job->ino,
                                            job->chunk_index, job->fragment_index,
                                            job->checksum);
        break;
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
    efs_tls_write_known_zero = saved_zero;
    efs_tls_write_root = saved_root;
    return rc;
}

static void *writer_thread(void *arg)
{
    struct path_writer_pool *pool = arg;
    tls_in_writer = 1;
    for (;;) {
        pthread_mutex_lock(&pool->lock);
        while (pool->running && pool->count == 0)
            pthread_cond_wait(&pool->not_empty, &pool->lock);
        if (!pool->running && pool->count == 0) {
            pthread_mutex_unlock(&pool->lock);
            break;
        }
        struct writer_job *job = pool->queue[pool->head];
        efs_tls_write_root = (int)job->path_index;
        pool->head = (pool->head + 1) % EFS_WRITER_QUEUE_CAP;
        pool->count--;
        pthread_cond_signal(&pool->not_full);
        pthread_mutex_unlock(&pool->lock);

        int rc = run_job(job);
        efs_tls_write_root = -1;

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
    if (g_pools.writers_per_path <= 0 || tls_in_writer || g_pools.npaths <= 0)
        return run_job(job);

    uint32_t pi = pick_write_path(job);
    if (pi >= (uint32_t)g_pools.npaths)
        pi = 0;
    job->path_index = pi;
    struct path_writer_pool *pool = &g_pools.paths[pi];
    if (!pool->running || pool->nwriters <= 0) {
        efs_tls_write_root = (int)pi;
        int rc = run_job(job);
        efs_tls_write_root = -1;
        return rc;
    }

    pthread_mutex_init(&job->done_mu, NULL);
    pthread_cond_init(&job->done_cv, NULL);
    job->done = 0;
    job->result = EFS_ERR_IO;

    pthread_mutex_lock(&pool->lock);
    /* Bounded wait for queue space: blocking forever parks a conn thread per
     * queued PUT (up to 512) with no client-visible backpressure. After the
     * deadline return EBUSY so the client retries instead of stalling. */
    if (pool->running && pool->count == EFS_WRITER_QUEUE_CAP) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += 5;
        while (pool->running && pool->count == EFS_WRITER_QUEUE_CAP) {
            if (pthread_cond_timedwait(&pool->not_full, &pool->lock, &ts) == ETIMEDOUT) {
                pthread_mutex_unlock(&pool->lock);
                pthread_mutex_destroy(&job->done_mu);
                pthread_cond_destroy(&job->done_cv);
                return EFS_ERR_BUSY;
            }
        }
    }
    if (!pool->running) {
        pthread_mutex_unlock(&pool->lock);
        pthread_mutex_destroy(&job->done_mu);
        pthread_cond_destroy(&job->done_cv);
        efs_tls_write_root = (int)pi;
        int rc = run_job(job);
        efs_tls_write_root = -1;
        return rc;
    }
    pool->queue[pool->tail] = job;
    pool->tail = (pool->tail + 1) % EFS_WRITER_QUEUE_CAP;
    pool->count++;
    /* Charge fairness when the job is accepted so concurrent pickers see it. */
    g_pools.assigned_bytes[pi] += job->data_len ? job->data_len : 64u;
    pthread_cond_signal(&pool->not_empty);
    pthread_mutex_unlock(&pool->lock);

    pthread_mutex_lock(&job->done_mu);
    while (!job->done)
        pthread_cond_wait(&job->done_cv, &job->done_mu);
    int rc = job->result;
    pthread_mutex_unlock(&job->done_mu);

    pthread_mutex_destroy(&job->done_mu);
    pthread_cond_destroy(&job->done_cv);
    return rc;
}

int server_writer_pool_start(struct efsd_server *s)
{
    memset(&g_pools, 0, sizeof(g_pools));
    int wpp = s->nwriters;
    g_pools.writers_per_path = wpp;
    if (wpp <= 0)
        return 0;

    uint32_t npaths = s->storage_path_count ? s->storage_path_count : 1;
    if (npaths > EFS_MAX_STORAGE_PATHS)
        npaths = EFS_MAX_STORAGE_PATHS;
    g_pools.npaths = (int)npaths;

    for (int p = 0; p < g_pools.npaths; p++) {
        struct path_writer_pool *pool = &g_pools.paths[p];
        pool->nwriters = wpp;
        pthread_mutex_init(&pool->lock, NULL);
        pthread_cond_init(&pool->not_empty, NULL);
        pthread_cond_init(&pool->not_full, NULL);
        pool->threads = calloc((size_t)wpp, sizeof(pthread_t));
        if (!pool->threads) {
            server_writer_pool_stop(s);
            return -1;
        }
        pool->running = 1;
        for (int i = 0; i < wpp; i++) {
            if (efsd_pthread_create(&pool->threads[i], writer_thread, pool) != 0) {
                pool->running = 0;
                pthread_cond_broadcast(&pool->not_empty);
                for (int j = 0; j < i; j++)
                    pthread_join(pool->threads[j], NULL);
                free(pool->threads);
                pool->threads = NULL;
                pool->nwriters = 0;
                server_writer_pool_stop(s);
                return -1;
            }
        }
    }
    return 0;
}

void server_writer_pool_stop(struct efsd_server *s)
{
    (void)s;
    if (g_pools.npaths <= 0)
        return;

    for (int p = 0; p < g_pools.npaths; p++) {
        struct path_writer_pool *pool = &g_pools.paths[p];
        if (!pool->threads)
            continue;
        pthread_mutex_lock(&pool->lock);
        pool->running = 0;
        pthread_cond_broadcast(&pool->not_empty);
        pthread_cond_broadcast(&pool->not_full);
        pthread_mutex_unlock(&pool->lock);
        for (int i = 0; i < pool->nwriters; i++)
            pthread_join(pool->threads[i], NULL);
        free(pool->threads);
        pool->threads = NULL;
        pool->nwriters = 0;
        pthread_mutex_destroy(&pool->lock);
        pthread_cond_destroy(&pool->not_empty);
        pthread_cond_destroy(&pool->not_full);
    }
    g_pools.npaths = 0;
    g_pools.writers_per_path = 0;
}

int server_write_fragment(struct efsd_server *s, struct efs_export *ex,
                          efs_ino_t ino, uint32_t chunk_index, uint32_t fragment_index,
                          const uint8_t *data, uint32_t data_len)
{
    struct writer_job job;
    memset(&job, 0, sizeof(job));
    job.op = WRITER_OP_FRAGMENT;
    job.s = s;
    job.ex = ex;
    job.ino = ino;
    job.chunk_index = chunk_index;
    job.fragment_index = fragment_index;
    job.data = data;
    job.data_len = data_len;
    return submit_and_wait(&job);
}

int server_write_fragment_sum(struct efsd_server *s, struct efs_export *ex,
                              efs_ino_t ino, uint32_t chunk_index, uint32_t fragment_index,
                              const uint8_t checksum[EFS_HASH_SIZE])
{
    struct writer_job job;
    memset(&job, 0, sizeof(job));
    job.op = WRITER_OP_FRAGMENT_SUM;
    job.s = s;
    job.ex = ex;
    job.ino = ino;
    job.chunk_index = chunk_index;
    job.fragment_index = fragment_index;
    job.checksum = checksum;
    return submit_and_wait(&job);
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
    job.data = data;
    job.data_len = data_len;
    job.checksum = checksum;
    return submit_and_wait(&job);
}
