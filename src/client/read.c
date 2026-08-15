#include "client_internal.h"
#include "efs/protocol.h"
#include "efs/network.h"
#include "efs/erasure.h"
#include "efs/checksum.h"
#include "efs/placement.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
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
#define FRAG_POOL_WORKERS (2 * EFS_WRITE_PIPELINE)
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

/* Reuse the fragment scratch across decodes on the same worker thread. */
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
    }
    return buf;
}

/* Reconstruct one logical chunk from any 2 of 3 fragments. The three GETs run
 * concurrently so one slow/dead peer overlaps the others instead of adding a
 * full RTT; decode only needs any two, so we don't serialize on a straggler. */
static int efs_client_decode_placed_chunk_attempts(efs_ino_t ino, uint32_t chunk_index,
                                                   uint8_t *chunk_out, uint32_t chunk_size,
                                                   uint32_t frag_len, int max_attempts)
{
    efs_node_id_t nodes[EFS_NUM_FRAGMENTS];
    efs_place_fragments(g_client.nodes, g_client.node_count, ino, chunk_index,
                        nodes);

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

    if (max_attempts < 1)
        max_attempts = 1;

    /* Heap / TLS: three fragments on a FUSE/main stack overflows easily and
     * SIGSEGV handlers without an alt stack cannot even log. */
    uint8_t *frag_buf = decode_frag_scratch(EFS_NUM_FRAGMENTS * frag_len);
    if (!frag_buf)
        return EFS_ERR_NOMEM;
    uint8_t *frags[EFS_NUM_FRAGMENTS];
    frag_ptrs(frag_buf, frag_len, frags);

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
                                                   data_chunk_size(), data_frag_size(), 2);
}

static int load_page_from_chunk(const struct efs_export_root *root, uint32_t pi,
                                uint32_t chunk_index, uint8_t *page_out)
{
    /* Same rule as server rebuild: only fragments whose hash matches the
     * EFSR page checksums are usable. Dual-slot reuse leaves stale same-ci
     * fragments; decoding any 2 of those yields a page that then fails the
     * re-encode check. */
    efs_node_id_t nodes[EFS_NUM_FRAGMENTS];
    efs_place_fragments(g_client.nodes, g_client.node_count, EFS_META_TABLE_INO,
                        chunk_index, nodes);

    uint8_t *frag_buf = malloc(EFS_NUM_FRAGMENTS * EFS_META_FRAGMENT_SIZE);
    if (!frag_buf)
        return EFS_ERR_NOMEM;
    uint8_t *frags[EFS_NUM_FRAGMENTS];
    frag_ptrs(frag_buf, EFS_META_FRAGMENT_SIZE, frags);

    struct frag_get_job jobs[EFS_NUM_FRAGMENTS];
    pthread_t tids[EFS_NUM_FRAGMENTS];
    int spawned[EFS_NUM_FRAGMENTS] = {0};
    for (int fi = 0; fi < EFS_NUM_FRAGMENTS; fi++) {
        jobs[fi].node = nodes[fi];
        jobs[fi].ino = EFS_META_TABLE_INO;
        jobs[fi].chunk_index = chunk_index;
        jobs[fi].fi = fi;
        jobs[fi].frag_len = EFS_META_FRAGMENT_SIZE;
        jobs[fi].out = frags[fi];
        jobs[fi].len = 0;
        jobs[fi].rc = EFS_ERR_NET;
        if (pthread_create(&tids[fi], NULL, frag_get_thread, &jobs[fi]) == 0)
            spawned[fi] = 1;
        else
            frag_get_thread(&jobs[fi]);
    }

    int have[EFS_NUM_FRAGMENTS] = {0};
    for (int fi = 0; fi < EFS_NUM_FRAGMENTS; fi++) {
        if (spawned[fi])
            pthread_join(tids[fi], NULL);
        if (jobs[fi].rc != 0 || jobs[fi].len != EFS_META_FRAGMENT_SIZE)
            continue;
        uint8_t sum[EFS_HASH_SIZE];
        efs_hash(frags[fi], EFS_META_FRAGMENT_SIZE, sum);
        if (memcmp(sum, efs_export_root_checksum_const(root, pi, fi),
                   EFS_HASH_SIZE) == 0)
            have[fi] = 1;
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
    int rc = EFS_ERR_DECODE;
    if (a >= 0 && b >= 0 && missing >= 0 &&
        efs_decode_chunk(frags, EFS_META_PAGE_SIZE, a, b, missing, page_out,
                         EFS_META_PAGE_SIZE) == 0)
        rc = EFS_OK;
    else if (a >= 0 && b < 0)
        rc = EFS_ERR_CHECKSUM;
    free(frag_buf);
    return rc;
}

/* Recover a torn dual-slot flush: the published EFSR checksums do not match
 * the even/odd pages, but the previous generation's slot is still intact.
 * Accept any two fragments whose self-hash matches the GET digest. */
static int load_page_from_chunk_unverified(uint32_t chunk_index, uint8_t *page_out)
{
    efs_node_id_t nodes[EFS_NUM_FRAGMENTS];
    efs_place_fragments(g_client.nodes, g_client.node_count, EFS_META_TABLE_INO,
                        chunk_index, nodes);

    uint8_t *frag_buf = malloc(EFS_NUM_FRAGMENTS * EFS_META_FRAGMENT_SIZE);
    if (!frag_buf)
        return EFS_ERR_NOMEM;
    uint8_t *frags[EFS_NUM_FRAGMENTS];
    frag_ptrs(frag_buf, EFS_META_FRAGMENT_SIZE, frags);

    struct frag_get_job jobs[EFS_NUM_FRAGMENTS];
    pthread_t tids[EFS_NUM_FRAGMENTS];
    int spawned[EFS_NUM_FRAGMENTS] = {0};
    for (int fi = 0; fi < EFS_NUM_FRAGMENTS; fi++) {
        jobs[fi].node = nodes[fi];
        jobs[fi].ino = EFS_META_TABLE_INO;
        jobs[fi].chunk_index = chunk_index;
        jobs[fi].fi = fi;
        jobs[fi].frag_len = EFS_META_FRAGMENT_SIZE;
        jobs[fi].out = frags[fi];
        jobs[fi].len = 0;
        jobs[fi].rc = EFS_ERR_NET;
        if (pthread_create(&tids[fi], NULL, frag_get_thread, &jobs[fi]) == 0)
            spawned[fi] = 1;
        else
            frag_get_thread(&jobs[fi]);
    }

    int have[EFS_NUM_FRAGMENTS] = {0};
    for (int fi = 0; fi < EFS_NUM_FRAGMENTS; fi++) {
        if (spawned[fi])
            pthread_join(tids[fi], NULL);
        if (jobs[fi].rc != 0 || jobs[fi].len != EFS_META_FRAGMENT_SIZE)
            continue;
        have[fi] = 1;
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
    int rc = EFS_ERR_DECODE;
    if (a >= 0 && b >= 0 && missing >= 0 &&
        efs_decode_chunk(frags, EFS_META_PAGE_SIZE, a, b, missing, page_out,
                         EFS_META_PAGE_SIZE) == 0)
        rc = EFS_OK;
    else if (a >= 0 && b < 0)
        rc = EFS_ERR_CHECKSUM;
    free(frag_buf);
    return rc;
}

static int load_export_from_prev_slot(const struct efs_export_root *root)
{
    if (!root || root->generation == 0 || root->page_count == 0)
        return EFS_ERR_INVAL;

    uint64_t prev = root->generation - 1;
    fprintf(stderr,
            "meta: trying previous dual-slot generation %llu (%u pages)\n",
            (unsigned long long)prev, root->page_count);
    fflush(stderr);

    uint8_t (*pages)[EFS_META_PAGE_SIZE] = calloc(root->page_count,
                                                  EFS_META_PAGE_SIZE);
    if (!pages)
        return EFS_ERR_NOMEM;

    for (uint32_t pi = 0; pi < root->page_count; pi++) {
        uint32_t try_ci[3];
        int ntry = efs_meta_page_ci_candidates(prev, root->version,
                                               root->ino_page_count,
                                               root->chunk_page_count, pi,
                                               try_ci);
        int loaded = 0;
        int rc = EFS_ERR_INVAL;
        for (int ti = 0; ti < ntry && !loaded; ti++) {
            rc = load_page_from_chunk_unverified(try_ci[ti], pages[pi]);
            if (rc == EFS_OK)
                loaded = 1;
        }
        if (!loaded) {
            fprintf(stderr,
                    "meta: prev-slot page %u/%u failed rc=%d (%s)\n",
                    pi, root->page_count, rc, efs_strerror(rc));
            fflush(stderr);
            free(pages);
            return rc;
        }
        if ((pi % 1000u) == 0) {
            fprintf(stderr, "meta: prev-slot loaded %u/%u\n",
                    pi, root->page_count);
            fflush(stderr);
        }
    }

    size_t blob_len = (size_t)root->page_count * EFS_META_PAGE_SIZE;
    char *blob = malloc(blob_len);
    if (!blob) {
        free(pages);
        return EFS_ERR_NOMEM;
    }
    for (uint32_t i = 0; i < root->page_count; i++)
        memcpy(blob + (size_t)i * EFS_META_PAGE_SIZE, pages[i],
               EFS_META_PAGE_SIZE);
    free(pages);

    pthread_mutex_lock(&g_client.lock);
    int rc = efs_export_deserialize(&g_client.export, blob, blob_len);
    if (rc == EFS_OK) {
        g_client.export.meta_fragmented = 1;
        if (efs_export_root_copy(&g_client.export.root, root) != EFS_OK)
            rc = EFS_ERR_NOMEM;
        else {
            /* Keep the published generation so the next flush is strictly
             * newer and replaces the torn root. */
            g_client.export.root.generation = root->generation;
            g_client.export.next_ino = g_client.export.next_ino
                                           ? g_client.export.next_ino
                                           : root->next_ino;
            uint32_t cs = root->chunk_size;
            g_client.export.chunk_size = efs_chunk_size_valid(cs) ? cs
                                                                  : EFS_DEFAULT_CHUNK_SIZE;
            g_client.export.features = root->features;
        }
    }
    pthread_mutex_unlock(&g_client.lock);
    free(blob);
    if (rc == EFS_OK) {
        fprintf(stderr,
                "meta: recovered tables from generation %llu "
                "(inodes=%llu chunks=%llu); will republish on flush\n",
                (unsigned long long)prev,
                (unsigned long long)g_client.export.inode_count,
                (unsigned long long)g_client.export.chunk_count);
        fflush(stderr);
        g_client.meta_dirty = 1;
    }
    return rc;
}

static int load_export_from_root(const struct efs_export_root *root)
{
    if (root->page_count == 0 || root->blob_len == 0)
        return EFS_ERR_PROTO;

    uint8_t (*pages)[EFS_META_PAGE_SIZE] = calloc(root->page_count, EFS_META_PAGE_SIZE);
    if (!pages)
        return EFS_ERR_NOMEM;

    int saw_v4_ci = 0, saw_v5_ci = 0;
    for (uint32_t pi = 0; pi < root->page_count; pi++) {
        uint32_t try_ci[3];
        int ntry = efs_meta_page_ci_candidates(root->generation, root->version,
                                               root->ino_page_count,
                                               root->chunk_page_count, pi,
                                               try_ci);
        int rc = EFS_ERR_INVAL;
        const char *scheme = "v5";
        int loaded = 0;

        for (int ti = 0; ti < ntry && !loaded; ti++) {
            uint32_t ci = try_ci[ti];
            uint32_t layout = efs_meta_page_ci_layout(
                root->generation, root->ino_page_count, root->chunk_page_count,
                pi, ci);
            scheme = layout == 5 ? "v5" : (layout == 4 ? "v4" : "legacy");
            rc = load_page_from_chunk(root, pi, ci, pages[pi]);
            if (rc != EFS_OK)
                continue;
            loaded = 1;
            if (layout == 4)
                saw_v4_ci = 1;
            else if (layout == 5)
                saw_v5_ci = 1;
            if (layout != 0 && layout != (root->version >= 5 ? 5u : 4u)) {
                fprintf(stderr,
                        "meta: page %u/%u loaded via %s chunk_index "
                        "(gen=%llu root v%u)\n",
                        pi, root->page_count, scheme,
                        (unsigned long long)root->generation, root->version);
                fflush(stderr);
            }
        }
        if (loaded)
            continue;
        fprintf(stderr,
                "meta: page reconstruct failed page=%u/%u gen=%llu "
                "blob_len=%u scheme=%s rc=%d (%s)\n",
                pi, root->page_count, (unsigned long long)root->generation,
                root->blob_len, scheme, rc, efs_strerror(rc));
        fflush(stderr);
        free(pages);
        return rc;
    }

    char *blob = NULL;
    size_t blob_len = 0;
    int rc = efs_meta_assemble_blob(root, pages, &blob, &blob_len);
    free(pages);
    if (rc != EFS_OK)
        return rc;

    pthread_mutex_lock(&g_client.lock);
    rc = efs_export_deserialize(&g_client.export, blob, blob_len);
    if (rc == EFS_OK) {
        g_client.export.meta_fragmented = 1;
        if (efs_export_root_copy(&g_client.export.root, root) != EFS_OK)
            rc = EFS_ERR_NOMEM;
        else {
            if (saw_v4_ci && !saw_v5_ci && g_client.export.root.version >= 5)
                g_client.export.root.version = 4;
            else if (saw_v5_ci && !saw_v4_ci &&
                     g_client.export.root.version < 5)
                g_client.export.root.version = 5;
            g_client.export.next_ino = root->next_ino;
            uint32_t cs = root->chunk_size;
            g_client.export.chunk_size = efs_chunk_size_valid(cs) ? cs
                                                                  : EFS_DEFAULT_CHUNK_SIZE;
            g_client.export.features = root->features;
        }
    }
    pthread_mutex_unlock(&g_client.lock);
    free(blob);
    return rc;
}

static int fetch_meta_blob_from(const char *host, uint16_t port,
                                void **payload_out, uint32_t *len_out)
{
    int fd = efs_connect_tcp(host, port);
    if (fd < 0)
        return EFS_ERR_NET;

    efs_set_recv_timeout(fd, EFS_IO_TIMEOUT_MS);
    efs_set_send_timeout(fd, EFS_IO_TIMEOUT_MS);

    uint8_t type;
    void *payload = NULL;
    uint32_t payload_len = 0;
    if (efs_send_msg(fd, EFS_MSG_GET_META, NULL, 0) != 0 ||
        efs_recv_msg(fd, &type, &payload, &payload_len) != 0) {
        free(payload);
        close(fd);
        return EFS_ERR_NET;
    }
    if (type != EFS_MSG_GET_META_REPLY) {
        free(payload);
        close(fd);
        return EFS_ERR_PROTO;
    }
    if (payload_len == 0) {
        free(payload);
        close(fd);
        /* Server has no serializable export root/blob. */
        return EFS_ERR_NOT_FOUND;
    }
    close(fd);
    *payload_out = payload;
    *len_out = payload_len;
    return EFS_OK;
}

int efs_client_fetch_metadata(const char *host, uint16_t port)
{
    /* Prefer the newest EFSR generation across the cluster so a root that
     * missed the last 2-ack quorum cannot shadow fresher peers. */
    struct efs_export_root best_root;
    memset(&best_root, 0, sizeof(best_root));
    int have_root = 0;
    int saw_bootstrap = 0;
    int last_fetch_rc = EFS_ERR_NET;
    int fetch_ok = 0;
    char *best_legacy = NULL;
    size_t best_legacy_len = 0;

    for (int pass = 0; pass < 2; pass++) {
        uint32_t n = (pass == 0) ? 1 : g_client.node_count;
        for (uint32_t i = 0; i < n; i++) {
            const char *h;
            uint16_t p;
            if (pass == 0) {
                h = host;
                p = port;
            } else {
                h = g_client.nodes[i].addr;
                p = g_client.nodes[i].port;
                if (strcmp(h, host) == 0 && p == port)
                    continue;
            }

            void *payload = NULL;
            uint32_t plen = 0;
            int frc = fetch_meta_blob_from(h, p, &payload, &plen);
            if (frc != EFS_OK) {
                last_fetch_rc = frc;
                continue;
            }
            fetch_ok = 1;
            if (efs_meta_blob_is_root(payload, plen)) {
                struct efs_export_root root;
                memset(&root, 0, sizeof(root));
                if (efs_export_root_deserialize(&root, payload, plen) == 0) {
                    /* Ignore bootstrap shells (no pages yet). */
                    if (root.page_count == 0 || root.blob_len == 0) {
                        saw_bootstrap = 1;
                        efs_export_root_free(&root);
                    } else if (!have_root || root.generation > best_root.generation) {
                        efs_export_root_free(&best_root);
                        efs_export_root_move(&best_root, &root);
                        have_root = 1;
                    } else {
                        efs_export_root_free(&root);
                    }
                }
            } else if (efs_meta_blob_is_export(payload, plen) && !have_root) {
                free(best_legacy);
                best_legacy = payload;
                best_legacy_len = plen;
                payload = NULL;
            }
            free(payload);
        }
    }

    if (have_root) {
        fprintf(stderr,
                "meta: got EFSR generation=%llu pages=%u blob_len=%u; "
                "reconstructing table pages...\n",
                (unsigned long long)best_root.generation, best_root.page_count,
                best_root.blob_len);
        int rc = load_export_from_root(&best_root);
        if (rc != EFS_OK) {
            fprintf(stderr,
                    "meta: page reconstruct failed (%s); trying previous dual-slot\n",
                    efs_strerror(rc));
            fflush(stderr);
            rc = load_export_from_prev_slot(&best_root);
        }
        efs_export_root_free(&best_root);
        if (rc == EFS_OK) {
            free(best_legacy);
            return rc;
        }
        fprintf(stderr,
                "meta: page reconstruct failed (%s); trying legacy EFSM fallback\n",
                efs_strerror(rc));
        /* Fall back to legacy EFSM if page reconstruct fails. */
        if (!best_legacy) {
            /* Typical cause: meta table fragments missing on 2+ nodes
             * (2-of-3 decode needs any two fragments). */
            return rc;
        }
    }
    if (best_legacy) {
        pthread_mutex_lock(&g_client.lock);
        int rc = efs_export_deserialize(&g_client.export, best_legacy, best_legacy_len);
        g_client.export.meta_fragmented = 0;
        pthread_mutex_unlock(&g_client.lock);
        free(best_legacy);
        return rc;
    }
    if (!fetch_ok)
        return last_fetch_rc;
    if (saw_bootstrap)
        return EFS_ERR_PROTO; /* export name exists but root has no pages */
    return EFS_ERR_NOT_FOUND;
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
        int fd = efs_client_conn_get(node_id);
        if (fd < 0) {
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
        if (efs_send_msg(fd, EFS_MSG_GET_CHUNK, &req, sizeof(req)) != 0 ||
            efs_recv_msg_into(fd, &reply_type, &status,
                              checksum, EFS_HASH_SIZE,
                              data, expected_frag_len) != 0 ||
            reply_type != EFS_MSG_GET_CHUNK_REPLY) {
            efs_client_conn_drop(node_id, fd);
            efs_client_node_note_fail(node_id);
            if (attempt < 3) {
                usleep(50000u * (unsigned)attempt);
                continue;
            }
            return EFS_ERR_NET;
        }

        if (status != EFS_GET_CHUNK_OK) {
            efs_client_conn_release(node_id, fd);
            return EFS_ERR_NOT_FOUND;
        }
        *data_len = expected_frag_len;

        /* Verify payload. Known-zero digests are trusted (writer/store already
         * short-circuit zeros); avoid a 64KiB memcmp on every GET. */
        uint8_t zero_ck[EFS_HASH_SIZE];
        if (expected_frag_len == EFS_META_FRAGMENT_SIZE)
            efs_hash_zero_fragment(zero_ck);
        else
            efs_hash_zero_fragment_len(expected_frag_len, zero_ck);
        int sum_ok;
        if (memcmp(checksum, zero_ck, EFS_HASH_SIZE) == 0) {
            sum_ok = 1;
        } else {
            uint8_t verify[EFS_HASH_SIZE];
            efs_hash(data, expected_frag_len, verify);
            sum_ok = (memcmp(verify, checksum, EFS_HASH_SIZE) == 0);
        }
        if (!sum_ok) {
            efs_client_conn_release(node_id, fd);
            return EFS_ERR_CHECKSUM;
        }

        efs_client_conn_release(node_id, fd);
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

#define RDCACHE_SLOTS 8192
struct rdcache_ent {
    efs_ino_t ino;
    uint32_t ci;
    uint8_t *data;
    uint32_t len;
};
static struct {
    pthread_mutex_t mu;
    struct rdcache_ent e[RDCACHE_SLOTS];
} g_rdcache = { .mu = PTHREAD_MUTEX_INITIALIZER };

static uint32_t rdcache_slot(efs_ino_t ino, uint32_t ci)
{
    uint64_t h = (uint64_t)ino * 0x9E3779B97F4A7C15ULL;
    h ^= (uint64_t)ci * 0xBF58476D1CE4E5B9ULL;
    return (uint32_t)(h & (RDCACHE_SLOTS - 1));
}

int efs_rdcache_get(efs_ino_t ino, uint32_t ci, uint8_t *dst, uint32_t len)
{
    /* Dirty write-combined chunks are newer than anything on the servers. */
    if (efs_dcache_get(ino, ci, dst, len) == 0)
        return 0;
    uint32_t s = rdcache_slot(ino, ci);
    pthread_mutex_lock(&g_rdcache.mu);
    struct rdcache_ent *e = &g_rdcache.e[s];
    if (e->data && e->ino == ino && e->ci == ci && e->len >= len) {
        memcpy(dst, e->data, len);
        pthread_mutex_unlock(&g_rdcache.mu);
        return 0;
    }
    pthread_mutex_unlock(&g_rdcache.mu);
    return -1;
}

void efs_rdcache_put(efs_ino_t ino, uint32_t ci, const uint8_t *src, uint32_t len)
{
    if (!src || !len)
        return;
    uint32_t s = rdcache_slot(ino, ci);
    pthread_mutex_lock(&g_rdcache.mu);
    struct rdcache_ent *e = &g_rdcache.e[s];
    if (!e->data || e->len < len) {
        uint8_t *nbuf = realloc(e->data, len);
        if (!nbuf) {
            pthread_mutex_unlock(&g_rdcache.mu);
            return;
        }
        e->data = nbuf;
        e->len = len;
    }
    memcpy(e->data, src, len);
    e->ino = ino;
    e->ci = ci;
    pthread_mutex_unlock(&g_rdcache.mu);
}

void efs_rdcache_invalidate(efs_ino_t ino, uint32_t ci)
{
    uint32_t s = rdcache_slot(ino, ci);
    pthread_mutex_lock(&g_rdcache.mu);
    struct rdcache_ent *e = &g_rdcache.e[s];
    if (e->ino == ino && e->ci == ci)
        e->ino = 0;
    pthread_mutex_unlock(&g_rdcache.mu);
}

struct chunk_get_job {
    efs_ino_t ino;
    uint32_t ci;
    int have_ce;
    int cacheable;
    int rc;
    uint8_t *chunk; /* data_chunk_size() bytes, owned by caller */
    struct get_batch *bp;
};

static void *chunk_get_worker(void *arg)
{
    struct chunk_get_job *job = arg;
    uint32_t chunk_size = data_chunk_size();
    if (efs_rdcache_get(job->ino, job->ci, job->chunk, chunk_size) == 0) {
        job->rc = EFS_OK;
        return NULL;
    }
    if (!job->have_ce) {
        memset(job->chunk, 0, chunk_size);
        job->rc = EFS_OK;
        return NULL;
    }
    job->rc = efs_client_decode_placed_chunk(job->ino, job->ci, job->chunk);
    if (job->rc == EFS_OK && job->cacheable)
        efs_rdcache_put(job->ino, job->ci, job->chunk, chunk_size);
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

int efs_client_read(efs_ino_t ino, uint64_t offset, size_t size, char *buf, size_t *out_len)
{
    if (size == 0) {
        *out_len = 0;
        return EFS_OK;
    }

    efs_client_lock_dir(ino);
    struct efs_inode inode;
    if (efs_export_get_inode(&g_client.export, ino, &inode) != 0) {
        efs_client_unlock_dir(ino);
        return EFS_ERR_NOT_FOUND;
    }
    uint64_t file_size = inode.size;
    efs_ino_t pack_ino = inode.pack_ino;
    uint32_t pack_off = inode.pack_off;
    uint32_t pack_len = inode.pack_len;
    efs_client_unlock_dir(ino);

    if (!pack_ino) {
        size_t staged = 0;
        if (efs_client_pack_stage_read(ino, offset, size, buf, &staged) == 0) {
            *out_len = staged;
            return EFS_OK;
        }
    }

    if (pack_ino && pack_len) {
        if (offset >= pack_len) {
            *out_len = 0;
            return EFS_OK;
        }
        if (offset + size > pack_len)
            size = (size_t)(pack_len - offset);
        ino = pack_ino;
        offset = (uint64_t)pack_off + offset;
        file_size = offset + size;
        /* Seal copies into the dir pack but PUTs only when the chunk is
         * full. Serve the in-memory tail so close+read sees the bytes. */
        size_t packed = 0;
        if (efs_client_dir_pack_read(ino, offset, size, buf, &packed) == 0) {
            *out_len = packed;
            return EFS_OK;
        }
    } else if (offset >= file_size) {
        *out_len = 0;
        return EFS_OK;
    } else if (offset + size > file_size) {
        size = (size_t)(file_size - offset);
    }

    size_t total = 0;
    uint64_t end = offset + size;
    uint32_t chunk_size = data_chunk_size();
    uint32_t pipe = EFS_WRITE_PIPELINE;
    if (pipe < 1)
        pipe = 1;

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
            jobs[batch].chunk = malloc(chunk_size);
            if (!jobs[batch].chunk) {
                for (uint32_t j = 0; j < batch; j++)
                    free(jobs[j].chunk);
                return EFS_ERR_NOMEM;
            }
            jobs[batch].rc = EFS_ERR_IO;
            jobs[batch].cacheable = (size < chunk_size);
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

        get_pool_run(jobs, batch);

        for (uint32_t i = 0; i < batch; i++) {
            if (jobs[i].rc != EFS_OK) {
                for (uint32_t j = i; j < batch; j++)
                    free(jobs[j].chunk);
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
            free(jobs[i].chunk);
            jobs[i].chunk = NULL;
        }
    }

    *out_len = total;
    return EFS_OK;
}
