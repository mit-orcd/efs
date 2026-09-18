#include "client_internal.h"
#include "efs/protocol.h"
#include "efs/network.h"
#include "efs/erasure.h"
#include "efs/checksum.h"
#include "efs/placement.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <limits.h>
#include <pthread.h>

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

struct frag_batch {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    int remaining;
};

struct frag_get_job {
    efs_node_id_t node;
    efs_ino_t ino;
    uint32_t chunk_index;
    int fi;
    uint32_t frag_len;
    uint8_t *out;
    uint32_t len;
    int rc;
    struct frag_batch *bp;
};

static void *frag_get_thread(void *arg)
{
    struct frag_get_job *j = arg;
    uint8_t sum[EFS_HASH_SIZE];
    j->rc = efs_client_get_fragment(j->node, j->ino, j->chunk_index, j->fi,
                                    j->frag_len, j->out, &j->len, sum);
    return NULL;
}

/* Persistent fragment GET workers. decode used to pthread_create+join two
 * threads per chunk; at GB/s that is tens of thousands of creates/s. A
 * dedicated pool (separate from the chunk GET pool) keeps the two preferred
 * fragments concurrent without a create/join storm, and cannot deadlock
 * with chunk workers that wait on this pool. */
/* Workers cap outstanding fragment GETs (one in flight per worker); client
 * read throughput ≈ workers × frag_size / RTT. 4× pipeline keeps the conn
 * pool, not the pool, the limiter. */
#define FRAG_POOL_WORKERS (4 * EFS_WRITE_PIPELINE)
#define FRAG_POOL_QDEPTH  (8 * EFS_WRITE_PIPELINE)
static struct {
    pthread_mutex_t mu;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
    struct frag_get_job *q[FRAG_POOL_QDEPTH];
    int head, tail, count;
    pthread_t tids[FRAG_POOL_WORKERS];
    int nworkers;
    int ready;
    int shutdown;
} g_frag_pool = {
    .mu = PTHREAD_MUTEX_INITIALIZER,
    .not_empty = PTHREAD_COND_INITIALIZER,
    .not_full = PTHREAD_COND_INITIALIZER,
};

static void *frag_pool_thread(void *arg)
{
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&g_frag_pool.mu);
        while (g_frag_pool.count == 0 && !g_frag_pool.shutdown)
            pthread_cond_wait(&g_frag_pool.not_empty, &g_frag_pool.mu);
        if (g_frag_pool.shutdown && g_frag_pool.count == 0) {
            pthread_mutex_unlock(&g_frag_pool.mu);
            return NULL;
        }
        struct frag_get_job *job = g_frag_pool.q[g_frag_pool.head];
        g_frag_pool.head = (g_frag_pool.head + 1) % FRAG_POOL_QDEPTH;
        g_frag_pool.count--;
        pthread_cond_signal(&g_frag_pool.not_full);
        pthread_mutex_unlock(&g_frag_pool.mu);

        frag_get_thread(job);

        struct frag_batch *bp = job->bp;
        if (bp) {
            pthread_mutex_lock(&bp->mu);
            if (--bp->remaining == 0)
                pthread_cond_signal(&bp->cv);
            pthread_mutex_unlock(&bp->mu);
        }
    }
}

static int frag_pool_ensure(void)
{
    if (g_frag_pool.ready)
        return 0;
    pthread_mutex_lock(&g_frag_pool.mu);
    if (!g_frag_pool.ready) {
        for (int i = 0; i < FRAG_POOL_WORKERS; i++) {
            if (pthread_create(&g_frag_pool.tids[i], NULL, frag_pool_thread,
                               NULL) != 0) {
                g_frag_pool.shutdown = 1;
                pthread_cond_broadcast(&g_frag_pool.not_empty);
                pthread_mutex_unlock(&g_frag_pool.mu);
                for (int j = 0; j < i; j++)
                    pthread_join(g_frag_pool.tids[j], NULL);
                g_frag_pool.shutdown = 0;
                return -1;
            }
        }
        g_frag_pool.nworkers = FRAG_POOL_WORKERS;
        g_frag_pool.ready = 1;
    }
    pthread_mutex_unlock(&g_frag_pool.mu);
    return 0;
}

static int frag_pool_run(struct frag_get_job *jobs, int n)
{
    if (n <= 0)
        return 0;
    if (n == 1 || frag_pool_ensure() != 0) {
        for (int i = 0; i < n; i++)
            frag_get_thread(&jobs[i]);
        return 0;
    }
    struct frag_batch bp;
    pthread_mutex_init(&bp.mu, NULL);
    pthread_cond_init(&bp.cv, NULL);
    bp.remaining = n;

    pthread_mutex_lock(&g_frag_pool.mu);
    for (int i = 0; i < n; i++) {
        jobs[i].bp = &bp;
        while (g_frag_pool.count == FRAG_POOL_QDEPTH && !g_frag_pool.shutdown)
            pthread_cond_wait(&g_frag_pool.not_full, &g_frag_pool.mu);
        g_frag_pool.q[g_frag_pool.tail] = &jobs[i];
        g_frag_pool.tail = (g_frag_pool.tail + 1) % FRAG_POOL_QDEPTH;
        g_frag_pool.count++;
        pthread_cond_signal(&g_frag_pool.not_empty);
    }
    pthread_mutex_unlock(&g_frag_pool.mu);

    pthread_mutex_lock(&bp.mu);
    while (bp.remaining > 0)
        pthread_cond_wait(&bp.cv, &bp.mu);
    pthread_mutex_unlock(&bp.mu);
    pthread_mutex_destroy(&bp.mu);
    pthread_cond_destroy(&bp.cv);
    return 0;
}

/* Reuse the fragment scratch across decodes on the same worker thread.
 * The buffer is heap + thread-local, so without a destructor it leaks
 * (~3 fragments per decoding thread) when the thread exits. A pthread
 * key frees it at thread exit for pool workers and libfuse threads
 * alike; pool workers only exit via efs_client_read_pools_stop(). */
static pthread_key_t decode_scratch_key;
static pthread_once_t decode_scratch_once = PTHREAD_ONCE_INIT;
static void decode_scratch_free(void *p) { free(p); }
static void decode_scratch_key_make(void)
{
    (void)pthread_key_create(&decode_scratch_key, decode_scratch_free);
}

static uint8_t *decode_frag_scratch(uint32_t need)
{
    static __thread uint8_t *buf;
    static __thread uint32_t cap;
    if (cap < need) {
        uint8_t *nbuf = realloc(buf, need);
        if (!nbuf)
            return NULL;
        buf = nbuf;
        cap = need;
        pthread_once(&decode_scratch_once, decode_scratch_key_make);
        (void)pthread_setspecific(decode_scratch_key, buf);
    }
    return buf;
}

/* Reconstruct one logical chunk from any 2 of 3 fragments. The three GETs run
 * concurrently so one slow/dead peer overlaps the others instead of adding a
 * full RTT; decode only needs any two, so we don't serialize on a straggler. */
static int efs_client_decode_placed_chunk_attempts(efs_ino_t ino, uint32_t chunk_index,
                                                   uint8_t *chunk_out, uint32_t chunk_size,
                                                   uint32_t frag_len, int max_attempts,
                                                   int treat_zero_cksum_as_hole)
{
    efs_node_id_t nodes[EFS_NUM_FRAGMENTS];
    efs_place_fragments(g_client.nodes, g_client.node_count, ino, chunk_index,
                        nodes);

    if (max_attempts < 1)
        max_attempts = 1;

    /* Heap / TLS: three fragments on a FUSE/main stack overflows easily and
     * SIGSEGV handlers without an alt stack cannot even log. */
    uint8_t *frag_buf = decode_frag_scratch(EFS_NUM_FRAGMENTS * frag_len);
    if (!frag_buf)
        return EFS_ERR_NOMEM;
    uint8_t *frags[EFS_NUM_FRAGMENTS];
    frag_ptrs(frag_buf, frag_len, frags);

    {
        struct efs_chunk_entry ce;
        pthread_mutex_lock(&g_client.idx_mu);
        int have_ce = (efs_export_get_chunk(&g_client.export, ino, chunk_index,
                                           &ce) == 0);
        pthread_mutex_unlock(&g_client.idx_mu);
        if (have_ce) {
            uint8_t zck[EFS_HASH_SIZE];
            efs_hash_zero_fragment_len(frag_len, zck);
            if (treat_zero_cksum_as_hole &&
                memcmp(ce.checksums[0], zck, EFS_HASH_SIZE) == 0 &&
                memcmp(ce.checksums[1], zck, EFS_HASH_SIZE) == 0 &&
                memcmp(ce.checksums[2], zck, EFS_HASH_SIZE) == 0) {
                memset(chunk_out, 0, chunk_size);
                return EFS_OK;
            }
            /* Writes may have steered a fragment onto a spare when a stripe
             * member was down. The chunk table is the source of truth. */
            if (ce.fragment_nodes[0] || ce.fragment_nodes[1] ||
                ce.fragment_nodes[2])
                memcpy(nodes, ce.fragment_nodes, sizeof(nodes));
        }
    }

    int order[EFS_NUM_FRAGMENTS] = {0, 1, 2};
    if (g_client.local_node_id != 0) {
        for (int i = 0; i < EFS_NUM_FRAGMENTS; i++) {
            if (nodes[i] == g_client.local_node_id) {
                int tmp = order[0];
                order[0] = order[i];
                order[i] = tmp;
                break;
            }
        }
    }

    int rc = EFS_ERR_DECODE;
    for (int attempt = 0; attempt < max_attempts; attempt++) {
        int have[EFS_NUM_FRAGMENTS] = {0, 0, 0};
        int good = 0;

        /* Fetch the two preferred fragments in parallel; pull the third
         * (parity) only if one of the first two fails. 2+1 reconstruction
         * needs any two fragments, and the all-up common case is served by the
         * two data fragments — saving a third GET, a third verify-hash, and a
         * third thread per chunk. A down node is skipped inside
         * efs_client_get_fragment (down-mark) and fails fast into the parity
         * fallback. */
        struct frag_get_job jobs[EFS_NUM_FRAGMENTS];

        /* Stage 1: the two preferred fragments in parallel. */
        for (int i = 0; i < 2; i++) {
            int fi = order[i];
            memset(&jobs[i], 0, sizeof(jobs[i]));
            jobs[i].node = nodes[fi];
            jobs[i].ino = ino;
            jobs[i].chunk_index = chunk_index;
            jobs[i].fi = fi;
            jobs[i].frag_len = frag_len;
            jobs[i].out = frags[fi];
            jobs[i].len = 0;
            jobs[i].rc = EFS_ERR_NET;
        }
        frag_pool_run(jobs, 2);
        for (int i = 0; i < 2; i++) {
            if (jobs[i].rc == 0 && jobs[i].len == frag_len && !have[jobs[i].fi]) {
                have[jobs[i].fi] = 1;
                good++;
            }
        }

        /* Stage 2: parity fallback only when a preferred fragment failed. */
        if (good < 2) {
            int fi = order[2];
            jobs[2].node = nodes[fi];
            jobs[2].ino = ino;
            jobs[2].chunk_index = chunk_index;
            jobs[2].fi = fi;
            jobs[2].frag_len = frag_len;
            jobs[2].out = frags[fi];
            jobs[2].len = 0;
            jobs[2].rc = EFS_ERR_NET;
            frag_get_thread(&jobs[2]); /* single fetch: run inline */
            if (jobs[2].rc == 0 && jobs[2].len == frag_len && !have[fi]) {
                have[fi] = 1;
                good++;
            }
        }

        int missing = -1, a = -1, b = -1;
        for (int i = 0; i < EFS_NUM_FRAGMENTS; i++) {
            if (!have[i]) {
                if (missing < 0)
                    missing = i;
            } else if (a < 0) {
                a = i;
            } else if (b < 0) {
                b = i;
            }
        }
        if (missing < 0) {
            for (int i = 0; i < EFS_NUM_FRAGMENTS; i++) {
                if (i != a && i != b) {
                    missing = i;
                    break;
                }
            }
        }
        if (a < 0 || b < 0 || missing < 0) {
            /* Fewer than 2 good fragments — brief backoff before refetch. */
            if (attempt + 1 < max_attempts)
                usleep(100000u * (unsigned)(attempt + 1));
            continue;
        }
        if (efs_decode_chunk(frags, chunk_size, a, b, missing, chunk_out,
                             chunk_size) == 0) {
            rc = EFS_OK;
            break;
        }
    }
    return rc;
}

static int efs_client_decode_placed_chunk(efs_ino_t ino, uint32_t chunk_index,
                                          uint8_t *chunk_out)
{
    return efs_client_decode_placed_chunk_attempts(ino, chunk_index, chunk_out,
                                                   data_chunk_size(), data_frag_size(),
                                                   2, 1);
}

int efs_client_fetch_published_chunk(efs_ino_t ino, uint32_t ci,
                                     uint8_t *buf, uint32_t len)
{
    uint32_t cs = data_chunk_size();
    if (!buf || !cs || len != cs)
        return EFS_ERR_INVAL;
    int published = 0;
    pthread_mutex_lock(&g_client.idx_mu);
    published = (efs_export_get_chunk(&g_client.export, ino, ci, NULL) == 0);
    pthread_mutex_unlock(&g_client.idx_mu);
    /* Always GET. A local mapping miss (adopt drop / evict) used to
     * zero-fill and the next PUT wiped sibling append lines. An all-zero
     * checksum stub is a read hole, not a safe merge base. */
    int rc = efs_client_decode_placed_chunk_attempts(ino, ci, buf, cs,
                                                     data_frag_size(), 2, 0);
    if (rc == EFS_OK)
        return EFS_OK;
    if (!published) {
        memset(buf, 0, len);
        return EFS_OK;
    }
    return rc;
}


int efs_client_get_fragment(efs_node_id_t node_id, efs_ino_t ino, uint32_t chunk_index,
                            uint32_t fragment_index, uint32_t expected_frag_len,
                            uint8_t *data, uint32_t *data_len,
                            uint8_t checksum[EFS_HASH_SIZE])
{
    if (node_id == 0 || expected_frag_len == 0)
        return EFS_ERR_INVAL;

    /* Skip nodes already marked down: a dead pooled fd would otherwise cost a
     * full SO_RCVTIMEO (30s) per attempt before we even try the next peer. */
    if (efs_client_node_is_down(node_id))
        return EFS_ERR_NET;

    for (int attempt = 1; attempt <= 3; attempt++) {
        struct efs_conn *conn = efs_client_conn_get(node_id);
        if (!conn) {
            efs_client_node_note_fail(node_id);
            if (attempt < 3) {
                usleep(50000u * (unsigned)attempt);
                continue;
            }
            return EFS_ERR_NET;
        }

        struct efs_msg_get_chunk req;
        memset(&req, 0, sizeof(req));
        req.export_id = g_client.export_id;
        req.ino = ino;
        req.chunk_index = chunk_index;
        req.fragment_index = fragment_index;

        /* Zero-copy receive: status + checksum + fragment land directly in the
         * caller's buffers — no malloc + 64 KiB memcpy per GET. */
        uint8_t reply_type = 0;
        uint8_t status = 0;
        if (efs_conn_send_msg(conn, EFS_MSG_GET_CHUNK, &req, sizeof(req)) != 0 ||
            efs_conn_recv_msg_into(conn, &reply_type, &status,
                                   checksum, EFS_HASH_SIZE,
                                   data, expected_frag_len) != 0 ||
            reply_type != EFS_MSG_GET_CHUNK_REPLY) {
            efs_client_conn_drop(node_id, conn);
            efs_client_node_note_fail(node_id);
            if (attempt < 3) {
                usleep(50000u * (unsigned)attempt);
                continue;
            }
            return EFS_ERR_NET;
        }

        if (status != EFS_GET_CHUNK_OK) {
            efs_client_conn_release(node_id, conn);
            return EFS_ERR_NOT_FOUND;
        }
        *data_len = expected_frag_len;

        /* Verify payload. Known-zero digests are trusted (writer/store already
         * short-circuit zeros); avoid a 64KiB memcmp on every GET.
         * Read-verify is OFF by default: at GB/s per client the inline
         * blake3 re-hash costs several cores and caps the frag pool, and
         * integrity is already covered server-side (write-time verify plus
         * the background scrubber, which also heals). EFS_READ_VERIFY=1
         * re-enables the end-to-end check for RDMA-CRC/ECC paranoia. */
        static int read_verify = -1;
        if (read_verify < 0) {
            const char *v = getenv("EFS_READ_VERIFY");
            read_verify = (v && *v && strcmp(v, "0") != 0) ? 1 : 0;
        }
        int sum_ok;
        if (!read_verify) {
            sum_ok = 1;
        } else {
            uint8_t zero_ck[EFS_HASH_SIZE];
            if (expected_frag_len == EFS_META_FRAGMENT_SIZE)
                efs_hash_zero_fragment(zero_ck);
            else
                efs_hash_zero_fragment_len(expected_frag_len, zero_ck);
            if (memcmp(checksum, zero_ck, EFS_HASH_SIZE) == 0) {
                sum_ok = 1;
            } else {
                uint8_t verify[EFS_HASH_SIZE];
                efs_hash(data, expected_frag_len, verify);
                sum_ok = (memcmp(verify, checksum, EFS_HASH_SIZE) == 0);
            }
        }
        if (!sum_ok) {
            efs_client_conn_release(node_id, conn);
            return EFS_ERR_CHECKSUM;
        }

        efs_client_conn_release(node_id, conn);
        efs_client_node_note_ok(node_id);
        return EFS_OK;
    }
    return EFS_ERR_NET;
}

struct get_batch {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    int remaining;
};

/* 4096 x 2 x 128 KiB = 1 GiB. Part D bypasses the kernel page cache
 * (FOPEN_DIRECT_IO); this is the client read cache for demand + prefetch.
 * Writes invalidate per-ci after PUT. */
#define RDCACHE_SLOTS  4096
#define RDCACHE_WAYS   2
#define RDCACHE_STRIPES 64
struct rdcache_ent {
    efs_ino_t ino;
    uint32_t ci;
    uint8_t *data;
    uint32_t len;
    uint32_t tick;
};
static struct {
    pthread_mutex_t mu[RDCACHE_STRIPES];
    int mu_ready;
    struct rdcache_ent e[RDCACHE_SLOTS][RDCACHE_WAYS];
    uint32_t tick;
} g_rdcache;

static pthread_mutex_t g_rdcache_init_mu = PTHREAD_MUTEX_INITIALIZER;

static void rdcache_ensure(void)
{
    if (__atomic_load_n(&g_rdcache.mu_ready, __ATOMIC_ACQUIRE))
        return;
    pthread_mutex_lock(&g_rdcache_init_mu);
    if (!g_rdcache.mu_ready) {
        for (int i = 0; i < RDCACHE_STRIPES; i++)
            pthread_mutex_init(&g_rdcache.mu[i], NULL);
        __atomic_store_n(&g_rdcache.mu_ready, 1, __ATOMIC_RELEASE);
    }
    pthread_mutex_unlock(&g_rdcache_init_mu);
}

static uint32_t rdcache_slot(efs_ino_t ino, uint32_t ci)
{
    uint64_t h = (uint64_t)ino * 0x9E3779B97F4A7C15ULL;
    h ^= (uint64_t)ci * 0xBF58476D1CE4E5B9ULL;
    return (uint32_t)(h & (RDCACHE_SLOTS - 1));
}

static pthread_mutex_t *rdcache_mu(uint32_t slot)
{
    rdcache_ensure();
    return &g_rdcache.mu[slot & (RDCACHE_STRIPES - 1)];
}

static struct rdcache_ent *rdcache_find(uint32_t s, efs_ino_t ino, uint32_t ci)
{
    for (int w = 0; w < RDCACHE_WAYS; w++) {
        struct rdcache_ent *e = &g_rdcache.e[s][w];
        if (e->data && e->ino == ino && e->ci == ci)
            return e;
    }
    return NULL;
}

int efs_rdcache_get(efs_ino_t ino, uint32_t ci, uint8_t *dst, uint32_t len)
{
    /* Dirty write-combined chunks are newer than anything on the servers. */
    if (efs_dcache_get(ino, ci, dst, len) == 0)
        return 0;
    if (!dst || !len)
        return -1;
    uint32_t s = rdcache_slot(ino, ci);
    pthread_mutex_t *mu = rdcache_mu(s);
    pthread_mutex_lock(mu);
    struct rdcache_ent *e = rdcache_find(s, ino, ci);
    if (e && e->len >= len) {
        memcpy(dst, e->data, len);
        e->tick = ++g_rdcache.tick;
        pthread_mutex_unlock(mu);
        return 0;
    }
    pthread_mutex_unlock(mu);
    return -1;
}

int efs_rdcache_copy(efs_ino_t ino, uint32_t ci, uint32_t off,
                     uint8_t *dst, uint32_t len)
{
    if (!dst || !len)
        return -1;
    uint32_t s = rdcache_slot(ino, ci);
    pthread_mutex_t *mu = rdcache_mu(s);
    pthread_mutex_lock(mu);
    struct rdcache_ent *e = rdcache_find(s, ino, ci);
    if (e && (uint64_t)off + len <= e->len) {
        memcpy(dst, e->data + off, len);
        e->tick = ++g_rdcache.tick;
        pthread_mutex_unlock(mu);
        return 0;
    }
    pthread_mutex_unlock(mu);
    return -1;
}

void efs_rdcache_put(efs_ino_t ino, uint32_t ci, const uint8_t *src, uint32_t len)
{
    if (!src || !len)
        return;
    uint32_t s = rdcache_slot(ino, ci);
    pthread_mutex_t *mu = rdcache_mu(s);
    pthread_mutex_lock(mu);
    struct rdcache_ent *e = rdcache_find(s, ino, ci);
    if (!e) {
        e = &g_rdcache.e[s][0];
        for (int w = 1; w < RDCACHE_WAYS; w++) {
            if (!g_rdcache.e[s][w].data) {
                e = &g_rdcache.e[s][w];
                break;
            }
            if (g_rdcache.e[s][w].tick < e->tick)
                e = &g_rdcache.e[s][w];
        }
    }
    if (e->data && e->len < len) {
        /* Oversized-chunk export: the pooled buffer can't grow. */
        efs_buf_free(e->data, e->len);
        e->data = NULL;
    }
    if (!e->data) {
        /* Pool buffers are EFS_CHUNK_SIZE-capacity; allocate once per way. */
        e->data = efs_buf_alloc(len);
        if (!e->data) {
            pthread_mutex_unlock(mu);
            return;
        }
    }
    e->len = len;
    memcpy(e->data, src, len);
    e->ino = ino;
    e->ci = ci;
    e->tick = ++g_rdcache.tick;
    pthread_mutex_unlock(mu);
}

void efs_rdcache_invalidate(efs_ino_t ino, uint32_t ci)
{
    uint32_t s = rdcache_slot(ino, ci);
    pthread_mutex_t *mu = rdcache_mu(s);
    pthread_mutex_lock(mu);
    struct rdcache_ent *e = rdcache_find(s, ino, ci);
    if (e)
        e->ino = 0;
    pthread_mutex_unlock(mu);
}

struct chunk_get_job {
    efs_ino_t ino;
    uint32_t ci;
    int have_ce;
    int cacheable;
    int owned; /* prefetch: get_pool_thread frees chunk + this struct */
    int rc;
    uint8_t *chunk; /* data_chunk_size() bytes, owned by caller unless owned */
    struct get_batch *bp;
};

static void *chunk_get_worker(void *arg)
{
    struct chunk_get_job *job = arg;
    uint32_t chunk_size = data_chunk_size();
    if (efs_rdcache_get(job->ino, job->ci, job->chunk, chunk_size) == 0) {
        efs_dcache_overlay(job->ino, job->ci, job->chunk, chunk_size);
        job->rc = EFS_OK;
        return NULL;
    }
    if (!job->have_ce) {
        /* have_ce was snapshotted by the reader thread before this worker
         * ran. A concurrent flush commits the chunk (dcache_put_now sets the
         * local chunk mapping) and the reclaimer can then drop the dcache
         * entry, so the overlay below would find nothing and zero-fill a
         * chunk that really has data (basic_chunk_boundary read XY\0). Re-
         * check the local mapping now; if it landed, fall through to fetch. */
        pthread_mutex_lock(&g_client.idx_mu);
        if (efs_export_get_chunk(&g_client.export, job->ino, job->ci,
                                 NULL) == EFS_OK)
            job->have_ce = 1;
        pthread_mutex_unlock(&g_client.idx_mu);
    }
    if (!job->have_ce) {
        memset(job->chunk, 0, chunk_size);
        efs_dcache_overlay(job->ino, job->ci, job->chunk, chunk_size);
        job->rc = EFS_OK;
        return NULL;
    }
    job->rc = efs_client_decode_placed_chunk(job->ino, job->ci, job->chunk);
    if (job->rc == EFS_OK) {
        efs_dcache_overlay(job->ino, job->ci, job->chunk, chunk_size);
        if (job->cacheable)
            efs_rdcache_put(job->ino, job->ci, job->chunk, chunk_size);
    }
    return NULL;
}

#define GET_POOL_QDEPTH (8 * EFS_WRITE_PIPELINE)
static struct {
    pthread_mutex_t mu;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
    struct chunk_get_job *q[GET_POOL_QDEPTH];
    int head, tail, count;
    pthread_t tids[EFS_WRITE_PIPELINE];
    int nworkers;
    int ready;
    int shutdown;
} g_get_pool = {
    .mu = PTHREAD_MUTEX_INITIALIZER,
    .not_empty = PTHREAD_COND_INITIALIZER,
    .not_full = PTHREAD_COND_INITIALIZER,
};

static void *get_pool_thread(void *arg)
{
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&g_get_pool.mu);
        while (g_get_pool.count == 0 && !g_get_pool.shutdown)
            pthread_cond_wait(&g_get_pool.not_empty, &g_get_pool.mu);
        if (g_get_pool.shutdown && g_get_pool.count == 0) {
            pthread_mutex_unlock(&g_get_pool.mu);
            return NULL;
        }
        struct chunk_get_job *job = g_get_pool.q[g_get_pool.head];
        g_get_pool.head = (g_get_pool.head + 1) % GET_POOL_QDEPTH;
        g_get_pool.count--;
        pthread_cond_signal(&g_get_pool.not_full);
        pthread_mutex_unlock(&g_get_pool.mu);

        chunk_get_worker(job);

        if (job->owned) {
            uint32_t cs = data_chunk_size();
            efs_buf_free(job->chunk, cs);
            free(job);
            continue;
        }

        struct get_batch *bp = job->bp;
        if (bp) {
            pthread_mutex_lock(&bp->mu);
            if (--bp->remaining == 0)
                pthread_cond_signal(&bp->cv);
            pthread_mutex_unlock(&bp->mu);
        }
    }
}

static int get_pool_ensure(void)
{
    if (g_get_pool.ready)
        return 0;
    pthread_mutex_lock(&g_get_pool.mu);
    if (!g_get_pool.ready) {
        uint32_t n = EFS_WRITE_PIPELINE;
        for (uint32_t i = 0; i < n; i++) {
            if (pthread_create(&g_get_pool.tids[i], NULL, get_pool_thread,
                               NULL) != 0) {
                g_get_pool.shutdown = 1;
                pthread_cond_broadcast(&g_get_pool.not_empty);
                pthread_mutex_unlock(&g_get_pool.mu);
                for (uint32_t j = 0; j < i; j++)
                    pthread_join(g_get_pool.tids[j], NULL);
                g_get_pool.shutdown = 0;
                return -1;
            }
        }
        g_get_pool.nworkers = (int)n;
        g_get_pool.ready = 1;
    }
    pthread_mutex_unlock(&g_get_pool.mu);
    return 0;
}

/* Stop both persistent read pools and join their workers. Thread exit is
 * what runs the TLS destructor that frees each worker's decode scratch
 * (decode_frag_scratch), so skipping the join leaks ~192 KiB per worker.
 * Call only when no reads can be in flight (client shutdown). */
void efs_client_read_pools_stop(void)
{
    if (g_get_pool.ready) {
        pthread_mutex_lock(&g_get_pool.mu);
        g_get_pool.shutdown = 1;
        pthread_cond_broadcast(&g_get_pool.not_empty);
        pthread_mutex_unlock(&g_get_pool.mu);
        for (int i = 0; i < g_get_pool.nworkers; i++)
            pthread_join(g_get_pool.tids[i], NULL);
        g_get_pool.ready = 0;
        g_get_pool.shutdown = 0;
    }
    if (g_frag_pool.ready) {
        pthread_mutex_lock(&g_frag_pool.mu);
        g_frag_pool.shutdown = 1;
        pthread_cond_broadcast(&g_frag_pool.not_empty);
        pthread_mutex_unlock(&g_frag_pool.mu);
        for (int i = 0; i < g_frag_pool.nworkers; i++)
            pthread_join(g_frag_pool.tids[i], NULL);
        g_frag_pool.ready = 0;
        g_frag_pool.shutdown = 0;
    }
}

static int get_pool_run(struct chunk_get_job *jobs, uint32_t batch)
{
    if (batch == 0)
        return 0;
    if (batch == 1 || get_pool_ensure() != 0) {
        for (uint32_t i = 0; i < batch; i++)
            chunk_get_worker(&jobs[i]);
        return 0;
    }
    struct get_batch bp;
    pthread_mutex_init(&bp.mu, NULL);
    pthread_cond_init(&bp.cv, NULL);
    bp.remaining = (int)batch;

    pthread_mutex_lock(&g_get_pool.mu);
    for (uint32_t i = 0; i < batch; i++) {
        jobs[i].bp = &bp;
        while (g_get_pool.count == GET_POOL_QDEPTH && !g_get_pool.shutdown)
            pthread_cond_wait(&g_get_pool.not_full, &g_get_pool.mu);
        g_get_pool.q[g_get_pool.tail] = &jobs[i];
        g_get_pool.tail = (g_get_pool.tail + 1) % GET_POOL_QDEPTH;
        g_get_pool.count++;
        pthread_cond_signal(&g_get_pool.not_empty);
    }
    pthread_mutex_unlock(&g_get_pool.mu);

    pthread_mutex_lock(&bp.mu);
    while (bp.remaining > 0)
        pthread_cond_wait(&bp.cv, &bp.mu);
    pthread_mutex_unlock(&bp.mu);
    pthread_mutex_destroy(&bp.mu);
    pthread_cond_destroy(&bp.cv);
    return 0;
}

/* Non-blocking submit for prefetch. Skip if the demand queue is already
 * half full so sequential ahead-GET cannot stall a FUSE worker. */
static int get_pool_try_submit(struct chunk_get_job *job)
{
    if (get_pool_ensure() != 0)
        return -1;
    pthread_mutex_lock(&g_get_pool.mu);
    if (g_get_pool.shutdown || g_get_pool.count >= GET_POOL_QDEPTH / 2) {
        pthread_mutex_unlock(&g_get_pool.mu);
        return -1;
    }
    g_get_pool.q[g_get_pool.tail] = job;
    g_get_pool.tail = (g_get_pool.tail + 1) % GET_POOL_QDEPTH;
    g_get_pool.count++;
    pthread_cond_signal(&g_get_pool.not_empty);
    pthread_mutex_unlock(&g_get_pool.mu);
    return 0;
}

static int rdcache_hit(efs_ino_t ino, uint32_t ci)
{
    uint32_t s = rdcache_slot(ino, ci);
    pthread_mutex_t *mu = rdcache_mu(s);
    int hit;
    pthread_mutex_lock(mu);
    hit = rdcache_find(s, ino, ci) != NULL;
    pthread_mutex_unlock(mu);
    return hit;
}

/* Chunks ahead of the demand window. 0 disables. libfuse 3.10.2 does not
 * negotiate FUSE_MAX_PAGES, so DIO sequential reads arrive as 128 KiB
 * FUSE requests; this is what keeps queue depth off the kernel. */
#define EFS_READ_PREFETCH_DEFAULT 16
static uint32_t prefetch_depth(void)
{
    static uint32_t n;
    static int once;
    if (!once) {
        const char *e = getenv("EFS_READ_PREFETCH");
        unsigned long v;
        n = EFS_READ_PREFETCH_DEFAULT;
        if (e && *e) {
            v = strtoul(e, NULL, 10);
            if (v <= (unsigned long)EFS_WRITE_PIPELINE)
                n = (uint32_t)v;
            else
                n = EFS_WRITE_PIPELINE;
        }
        once = 1;
    }
    return n;
}

static void prefetch_ahead(efs_ino_t ino, uint32_t from_ci, uint64_t file_size)
{
    uint32_t cs = data_chunk_size();
    uint32_t depth = prefetch_depth();
    uint32_t max_ci;
    uint32_t i;

    if (!cs || !depth || file_size == 0)
        return;
    max_ci = (uint32_t)((file_size + cs - 1) / cs);
    for (i = 0; i < depth; i++) {
        uint32_t ci = from_ci + i;
        struct chunk_get_job *job;
        int have_ce;

        if (ci >= max_ci)
            break;
        if (rdcache_hit(ino, ci) || efs_dcache_has(ino, ci))
            continue;
        pthread_mutex_lock(&g_client.idx_mu);
        have_ce = (efs_export_get_chunk(&g_client.export, ino, ci,
                                        NULL) == EFS_OK);
        pthread_mutex_unlock(&g_client.idx_mu);
        if (!have_ce)
            continue;
        job = calloc(1, sizeof(*job));
        if (!job)
            return;
        job->chunk = efs_buf_alloc(cs);
        if (!job->chunk) {
            free(job);
            return;
        }
        job->ino = ino;
        job->ci = ci;
        job->have_ce = 1;
        job->cacheable = 1;
        job->owned = 1;
        job->rc = EFS_ERR_IO;
        if (get_pool_try_submit(job) != 0) {
            efs_buf_free(job->chunk, cs);
            free(job);
            return;
        }
    }
}

static void maybe_prefetch(int want, efs_ino_t ino, uint64_t end_off,
                           uint64_t file_size)
{
    uint32_t cs = data_chunk_size();
    uint32_t next_ci;

    if (!want || !cs)
        return;
    next_ci = (uint32_t)((end_off + cs - 1) / cs);
    prefetch_ahead(ino, next_ci, file_size);
}

int efs_client_read(efs_ino_t ino, uint64_t offset, size_t size, char *buf, size_t *out_len)
{
    static __thread efs_ino_t t_seq_ino;
    static __thread uint64_t t_seq_next;
    static __thread int t_seq_run;
    int want_pf;
    uint32_t chunk_size;

    if (size == 0) {
        *out_len = 0;
        return EFS_OK;
    }

    efs_client_lock_dir(ino);
    pthread_mutex_lock(&g_client.idx_mu);
    struct efs_inode inode;
    int have_row = (efs_export_get_inode(&g_client.export, ino, &inode) == 0);
    pthread_mutex_unlock(&g_client.idx_mu);
    efs_client_unlock_dir(ino);
    if (!have_row) {
        /* Row miss on an open fd: the staging-table evictor (client-cache
         * design Part A) may have dropped a clean row — refetch from the
         * owner before concluding anything (stat_ino does the getattr RPC
         * + adopt + copy-out). A genuine unlink-open ghost fails the RPC
         * and falls through: unpublished dirty ranges only. have_base=1
         * past EOF is a shrink leftover; FOPEN_DIRECT_IO ignores i_size. */
        if (efs_client_stat_ino(ino, &inode) != EFS_OK) {
            uint32_t cs = data_chunk_size();
            if (cs && size > 0 &&
                (offset / cs) == ((offset + size - 1) / cs)) {
                uint32_t ci = (uint32_t)(offset / cs);
                uint32_t off = (uint32_t)(offset % cs);
                if (efs_dcache_copy_unpub(ino, ci, off, (uint8_t *)buf,
                                          (uint32_t)size) == 0) {
                    *out_len = size;
                    return EFS_OK;
                }
            }
            *out_len = 0;
            return EFS_OK;
        }
    }
    efs_client_stage_touch(ino);
    uint64_t file_size = inode.size;

    if (offset >= file_size) {
        /* Size not yet reflected (dentry stub / unlink-open ghost): still
         * serve unpublished dirty ranges from this same fd. */
        uint32_t cs = data_chunk_size();
        if (cs && size > 0 &&
            (offset / cs) == ((offset + size - 1) / cs)) {
            uint32_t ci = (uint32_t)(offset / cs);
            uint32_t off = (uint32_t)(offset % cs);
            if (efs_dcache_copy_unpub(ino, ci, off, (uint8_t *)buf,
                                      (uint32_t)size) == 0) {
                *out_len = size;
                return EFS_OK;
            }
        }
        *out_len = 0;
        return EFS_OK;
    } else if (offset + size > file_size) {
        size = (size_t)(file_size - offset);
    }

    if (t_seq_ino == ino && offset == t_seq_next)
        t_seq_run++;
    else
        t_seq_run = 1;
    t_seq_ino = ino;
    t_seq_next = offset + size;
    chunk_size = data_chunk_size();
    want_pf = (t_seq_run >= 2) || (size >= chunk_size);

    size_t total = 0;
    uint64_t end = offset + size;
    /* Sub-chunk read: dirty dcache first (newer than rdcache), then
     * decoded cache — no malloc(128k) on the 4k path. */
    if (chunk_size && size < chunk_size &&
        (offset / chunk_size) == ((end - 1) / chunk_size)) {
        uint32_t ci = (uint32_t)(offset / chunk_size);
        uint32_t off = (uint32_t)(offset % chunk_size);
        if (efs_dcache_copy(ino, ci, off, (uint8_t *)buf, (uint32_t)size) == 0) {
            *out_len = size;
            maybe_prefetch(want_pf, ino, end, file_size);
            return EFS_OK;
        }
        if (efs_rdcache_copy(ino, ci, off, (uint8_t *)buf, (uint32_t)size) == 0) {
            *out_len = size;
            maybe_prefetch(want_pf, ino, end, file_size);
            return EFS_OK;
        }
    }
    uint32_t pipe = EFS_WRITE_PIPELINE;
    if (pipe < 1)
        pipe = 1;
    int layout_pulled = 0;

    for (uint64_t pos = offset; pos < end; ) {
        /* Build a batch of whole chunks covering [pos, end). */
        struct chunk_get_job jobs[EFS_WRITE_PIPELINE];
        uint32_t batch = 0;
        uint64_t batch_pos = pos;

        while (batch < pipe && batch_pos < end) {
            uint32_t ci = (uint32_t)(batch_pos / chunk_size);
            memset(&jobs[batch], 0, sizeof(jobs[batch]));
            jobs[batch].ino = ino;
            jobs[batch].ci = ci;
            jobs[batch].chunk = efs_buf_alloc(chunk_size);
            if (!jobs[batch].chunk) {
                for (uint32_t j = 0; j < batch; j++)
                    efs_buf_free(jobs[j].chunk, chunk_size);
                return EFS_ERR_NOMEM;
            }
            jobs[batch].rc = EFS_ERR_IO;
            jobs[batch].cacheable = 1;
            batch_pos = ((uint64_t)ci + 1) * chunk_size;
            batch++;
        }

        /* Index lookup is idx_mu, not the table lock — g_client.lock is held
         * across serialize/snapshot and would stall every multi-job read. */
        pthread_mutex_lock(&g_client.idx_mu);
        for (uint32_t i = 0; i < batch; i++)
            jobs[i].have_ce =
                (efs_export_get_chunk(&g_client.export, jobs[i].ino,
                                      jobs[i].ci, NULL) == EFS_OK);
        pthread_mutex_unlock(&g_client.idx_mu);

        /* A missing row inside the file size may be a stale local cache,
         * not a real hole — pull the layout once per read and re-check
         * before zero-filling (mc_stress rwfile read zeros forever). */
        if (!layout_pulled) {
            uint32_t miss0 = UINT32_MAX, miss1 = 0;
            for (uint32_t i = 0; i < batch; i++) {
                if (!jobs[i].have_ce) {
                    if (jobs[i].ci < miss0)
                        miss0 = jobs[i].ci;
                    if (jobs[i].ci > miss1)
                        miss1 = jobs[i].ci;
                }
            }
            if (miss0 != UINT32_MAX &&
                efs_client_pull_layout_miss(ino, miss0, miss1 + 1)) {
                layout_pulled = 1;
                pthread_mutex_lock(&g_client.idx_mu);
                for (uint32_t i = 0; i < batch; i++)
                    jobs[i].have_ce =
                        (efs_export_get_chunk(&g_client.export, jobs[i].ino,
                                              jobs[i].ci, NULL) == EFS_OK);
                pthread_mutex_unlock(&g_client.idx_mu);
            }
        }

        get_pool_run(jobs, batch);

        for (uint32_t i = 0; i < batch; i++) {
            if (jobs[i].rc != EFS_OK) {
                for (uint32_t j = i; j < batch; j++)
                    efs_buf_free(jobs[j].chunk, chunk_size);
                return jobs[i].rc;
            }
            uint64_t chunk_start = (uint64_t)jobs[i].ci * chunk_size;
            uint32_t chunk_off = (pos > chunk_start) ? (uint32_t)(pos - chunk_start) : 0;
            size_t to_copy = end - pos;
            if (to_copy > chunk_size - chunk_off)
                to_copy = chunk_size - chunk_off;
            memcpy(buf + total, jobs[i].chunk + chunk_off, to_copy);
            total += to_copy;
            pos += to_copy;
            efs_buf_free(jobs[i].chunk, chunk_size);
            jobs[i].chunk = NULL;
        }
    }

    *out_len = total;
    maybe_prefetch(want_pf, ino, offset + total, file_size);
    return EFS_OK;
}
