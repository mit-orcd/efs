#include "server_internal.h"
#include "efs/checksum.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>

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

struct writer_pool {
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

static struct writer_pool g_pool;
static __thread int tls_in_writer;

static int run_job(struct writer_job *job)
{
    int rc = EFS_OK;
    int saved_zero = efs_tls_write_known_zero;
    if (job->op == WRITER_OP_FRAGMENT_WITH_SUM && job->checksum &&
        job->data_len == EFS_FRAGMENT_SIZE) {
        uint8_t zero_ck[EFS_HASH_SIZE];
        efs_hash_zero_fragment(zero_ck);
        efs_tls_write_known_zero =
            (memcmp(job->checksum, zero_ck, EFS_HASH_SIZE) == 0);
    } else {
        efs_tls_write_known_zero = 0;
    }
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
        rc = server_write_fragment_sync(job->s, job->ex, job->ino,
                                        job->chunk_index, job->fragment_index,
                                        job->data, job->data_len);
        if (rc == EFS_OK && job->checksum)
            rc = server_write_fragment_sum_sync(job->s, job->ex, job->ino,
                                                job->chunk_index,
                                                job->fragment_index,
                                                job->checksum);
        break;
    default:
        rc = EFS_ERR_INVAL;
        break;
    }
    efs_tls_write_known_zero = saved_zero;
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
        g_pool.head = (g_pool.head + 1) % EFS_WRITER_QUEUE_CAP;
        g_pool.count--;
        pthread_cond_signal(&g_pool.not_full);
        pthread_mutex_unlock(&g_pool.lock);

        int rc = run_job(job);

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
    if (g_pool.nwriters <= 0 || tls_in_writer || !g_pool.running)
        return run_job(job);

    pthread_mutex_init(&job->done_mu, NULL);
    pthread_cond_init(&job->done_cv, NULL);
    job->done = 0;
    job->result = EFS_ERR_IO;

    pthread_mutex_lock(&g_pool.lock);
    while (g_pool.running && g_pool.count == EFS_WRITER_QUEUE_CAP)
        pthread_cond_wait(&g_pool.not_full, &g_pool.lock);
    if (!g_pool.running) {
        pthread_mutex_unlock(&g_pool.lock);
        pthread_mutex_destroy(&job->done_mu);
        pthread_cond_destroy(&job->done_cv);
        return run_job(job);
    }
    g_pool.queue[g_pool.tail] = job;
    g_pool.tail = (g_pool.tail + 1) % EFS_WRITER_QUEUE_CAP;
    g_pool.count++;
    pthread_cond_signal(&g_pool.not_empty);
    pthread_mutex_unlock(&g_pool.lock);

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
    memset(&g_pool, 0, sizeof(g_pool));
    g_pool.nwriters = s->nwriters;
    if (g_pool.nwriters <= 0)
        return 0;

    pthread_mutex_init(&g_pool.lock, NULL);
    pthread_cond_init(&g_pool.not_empty, NULL);
    pthread_cond_init(&g_pool.not_full, NULL);
    g_pool.threads = calloc((size_t)g_pool.nwriters, sizeof(pthread_t));
    if (!g_pool.threads)
        return -1;
    g_pool.running = 1;
    for (int i = 0; i < g_pool.nwriters; i++) {
        if (efsd_pthread_create(&g_pool.threads[i], writer_thread, NULL) != 0) {
            g_pool.running = 0;
            pthread_cond_broadcast(&g_pool.not_empty);
            for (int j = 0; j < i; j++)
                pthread_join(g_pool.threads[j], NULL);
            free(g_pool.threads);
            g_pool.threads = NULL;
            g_pool.nwriters = 0;
            return -1;
        }
    }
    return 0;
}

void server_writer_pool_stop(struct efsd_server *s)
{
    (void)s;
    if (g_pool.nwriters <= 0 || !g_pool.threads)
        return;

    pthread_mutex_lock(&g_pool.lock);
    g_pool.running = 0;
    pthread_cond_broadcast(&g_pool.not_empty);
    pthread_cond_broadcast(&g_pool.not_full);
    pthread_mutex_unlock(&g_pool.lock);

    for (int i = 0; i < g_pool.nwriters; i++)
        pthread_join(g_pool.threads[i], NULL);
    free(g_pool.threads);
    g_pool.threads = NULL;
    g_pool.nwriters = 0;
    pthread_mutex_destroy(&g_pool.lock);
    pthread_cond_destroy(&g_pool.not_empty);
    pthread_cond_destroy(&g_pool.not_full);
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
