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
            if (memcmp(ce.checksums[0], zck, EFS_HASH_SIZE) == 0 &&
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
                                                   data_chunk_size(), data_frag_size(), 2);
}

/* Placement is a hint. After add-storage / 4-node join, meta fragments of
 * one page can sit on nodes the current hash ring does not name. Ask every
 * member for a missing (chunk, fi) before giving up. */
static int get_meta_frag_any_node(uint32_t chunk_index, uint32_t fi,
                                  efs_node_id_t skip, uint8_t *out)
{
    uint8_t ck[EFS_HASH_SIZE];
    for (uint32_t i = 0; i < g_client.node_count; i++) {
        efs_node_id_t id = g_client.nodes[i].id;
        if (id == 0 || id == skip)
            continue;
        uint32_t got = 0;
        if (efs_client_get_fragment(id, EFS_META_TABLE_INO, chunk_index, fi,
                                    EFS_META_FRAGMENT_SIZE, out, &got, ck) == 0 &&
            got == EFS_META_FRAGMENT_SIZE)
            return EFS_OK;
    }
    return EFS_ERR_NOT_FOUND;
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
    int used_scan = 0;
    for (int fi = 0; fi < EFS_NUM_FRAGMENTS; fi++) {
        if (have[fi])
            continue;
        if (get_meta_frag_any_node(chunk_index, (uint32_t)fi, nodes[fi],
                                   frags[fi]) != EFS_OK)
            continue;
        uint8_t sum[EFS_HASH_SIZE];
        efs_hash(frags[fi], EFS_META_FRAGMENT_SIZE, sum);
        if (memcmp(sum, efs_export_root_checksum_const(root, pi, fi),
                   EFS_HASH_SIZE) == 0) {
            have[fi] = 1;
            used_scan = 1;
        }
    }
    if (used_scan) {
        g_client.meta_heal = 1;
        fprintf(stderr,
                "meta: page %u ci=%u recovered fragment(s) via all-node scan\n",
                pi, chunk_index);
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
    for (int fi = 0; fi < EFS_NUM_FRAGMENTS; fi++) {
        if (have[fi])
            continue;
        if (get_meta_frag_any_node(chunk_index, (uint32_t)fi, nodes[fi],
                                   frags[fi]) == EFS_OK)
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

    char *blob = NULL;
    size_t blob_len = 0;
    int arc = efs_meta_assemble_blob(root, pages, &blob, &blob_len);
    free(pages);
    if (arc != EFS_OK)
        return arc;

    /* table_lock (g_client.lock + all dir stripes) + idx_mu: deserialize
     * frees/reallocs the inode/chunk tables that op-path workers walk under
     * lock_dir/idx_mu (chmod and wb_thread raced a resync → SIGSEGV). */
    efs_client_table_lock();
    pthread_mutex_lock(&g_client.idx_mu);
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
    pthread_mutex_unlock(&g_client.idx_mu);
    efs_client_table_unlock();
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

/* Some published EFSRs checksum chunk-region pages that were written at a
 * shifted logical index (this cluster: page 16382 lives at 18017/50785, not
 * v5 16384/49152). Discover that even logical CI by matching EFSR checksums,
 * then reuse (ci - rpi) for the rest of the region. */
static uint32_t g_meta_chunk_ci_even_base = UINT32_MAX;
static uint32_t g_last_chunk_even_ci = UINT32_MAX;
static int g_chunk_skip_streak = 0;

static void add_meta_ci(uint32_t *cis, int *n, int cap, uint32_t ci)
{
    if (ci == UINT32_MAX || *n >= cap)
        return;
    for (int i = 0; i < *n; i++) {
        if (cis[i] == ci)
            return;
    }
    cis[(*n)++] = ci;
}

static int meta_frag_matches(uint32_t ci, uint32_t fi, const uint8_t *want)
{
    uint8_t buf[EFS_META_FRAGMENT_SIZE];
    if (get_meta_frag_any_node(ci, fi, 0, buf) != EFS_OK)
        return 0;
    uint8_t sum[EFS_HASH_SIZE];
    efs_hash(buf, EFS_META_FRAGMENT_SIZE, sum);
    return memcmp(sum, want, EFS_HASH_SIZE) == 0;
}

static int meta_ci_quorum(const struct efs_export_root *root, uint32_t pi,
                          uint32_t ci)
{
    int n = 0;
    for (int fi = 0; fi < EFS_NUM_FRAGMENTS; fi++) {
        if (meta_frag_matches(ci, (uint32_t)fi,
                              efs_export_root_checksum_const(root, pi, fi)))
            n++;
    }
    return n;
}

static uint32_t hunt_chunk_page_even_ci(const struct efs_export_root *root,
                                        uint32_t pi)
{
    uint32_t rpi = pi - root->ino_page_count;
    fprintf(stderr,
            "meta: hunting shifted chunk-page CI for page %u (rpi=%u)\n",
            pi, rpi);
    fflush(stderr);

    /* First hunt: v5 chunk window (16384+). Later hunts: near last hit. */
    uint32_t windows[3][2];
    int nwin;
    if (g_last_chunk_even_ci != UINT32_MAX) {
        uint32_t last = g_last_chunk_even_ci;
        uint32_t lo = (last > 4096) ? last - 4096 : 0;
        uint32_t hi = last + 4096;
        if (hi > EFS_META_MAX_PAGES)
            hi = EFS_META_MAX_PAGES;
        windows[0][0] = lo;
        windows[0][1] = hi;
        windows[1][0] = EFS_META_CHUNK_PAGE_BASE;
        windows[1][1] = EFS_META_CHUNK_PAGE_BASE + 4096;
        windows[2][0] = 0;
        windows[2][1] = 2048;
        nwin = 3;
    } else {
        windows[0][0] = EFS_META_CHUNK_PAGE_BASE;
        windows[0][1] = EFS_META_MAX_PAGES;
        windows[1][0] = 0;
        windows[1][1] = EFS_META_CHUNK_PAGE_BASE;
        nwin = 2;
    }
    int odd_first = (int)(root->generation & 1ULL);
    for (int w = 0; w < nwin; w++) {
        for (uint32_t logical = windows[w][0]; logical < windows[w][1];
             logical++) {
            uint32_t even = logical;
            uint32_t odd = logical + EFS_META_SLOT_STRIDE;
            uint32_t pair[2];
            pair[0] = odd_first ? odd : even;
            pair[1] = odd_first ? even : odd;
            for (int s = 0; s < 2; s++) {
                if (meta_ci_quorum(root, pi, pair[s]) < 2)
                    continue;
                fprintf(stderr,
                        "meta: found page %u fragments at ci=%u "
                        "(even logical=%u base=%u)\n",
                        pi, pair[s], logical, logical - rpi);
                fflush(stderr);
                return logical;
            }
            if (((logical - windows[w][0]) % 1024u) == 0 &&
                logical > windows[w][0]) {
                fprintf(stderr, "meta: CI hunt logical=%u\n", logical);
                fflush(stderr);
            }
        }
    }
    return UINT32_MAX;
}

static int load_export_from_root(const struct efs_export_root *root)
{
    if (root->page_count == 0 || root->blob_len == 0)
        return EFS_ERR_PROTO;

    uint8_t (*pages)[EFS_META_PAGE_SIZE] = calloc(root->page_count, EFS_META_PAGE_SIZE);
    if (!pages)
        return EFS_ERR_NOMEM;

    g_meta_chunk_ci_even_base = UINT32_MAX;
    g_last_chunk_even_ci = UINT32_MAX;
    g_chunk_skip_streak = 0;
    g_client.meta_heal_skipped = 0;
    int saw_v4_ci = 0, saw_v5_ci = 0;
    for (uint32_t pi = 0; pi < root->page_count; pi++) {
        /* Tail pages past ino_blob_len / chunk_blob_len are not in the EFSM. */
        if (root->chunk_page_count > 0) {
            if (pi < root->ino_page_count) {
                if ((uint64_t)pi * EFS_META_PAGE_SIZE >= root->ino_blob_len)
                    continue;
            } else if ((uint64_t)(pi - root->ino_page_count) * EFS_META_PAGE_SIZE >=
                       root->chunk_blob_len) {
                continue;
            }
        } else if ((uint64_t)pi * EFS_META_PAGE_SIZE >= root->blob_len) {
            continue;
        }
        uint32_t try_ci[12];
        int ntry = efs_meta_page_ci_candidates(root->generation, root->version,
                                               root->ino_page_count,
                                               root->chunk_page_count, pi,
                                               try_ci);
        /* Two clients can overwrite the published slot's page while the
         * previous dual-slot still matches this EFSR. Try that CI too. */
        if (root->generation > 0) {
            uint32_t alt[3];
            int na = efs_meta_page_ci_candidates(root->generation - 1,
                                                 root->version,
                                                 root->ino_page_count,
                                                 root->chunk_page_count, pi,
                                                 alt);
            for (int i = 0; i < na; i++)
                add_meta_ci(try_ci, &ntry, 12, alt[i]);
        }
        int is_chunk = root->chunk_page_count > 0 && pi >= root->ino_page_count;
        if (is_chunk && g_meta_chunk_ci_even_base != UINT32_MAX) {
            uint32_t rpi = pi - root->ino_page_count;
            add_meta_ci(try_ci, &ntry, 12, g_meta_chunk_ci_even_base + rpi);
            add_meta_ci(try_ci, &ntry, 12,
                        g_meta_chunk_ci_even_base + rpi + EFS_META_SLOT_STRIDE);
        }
        if (is_chunk && g_last_chunk_even_ci != UINT32_MAX &&
            g_last_chunk_even_ci + 1 < EFS_META_MAX_PAGES) {
            add_meta_ci(try_ci, &ntry, 12, g_last_chunk_even_ci + 1);
            add_meta_ci(try_ci, &ntry, 12,
                        g_last_chunk_even_ci + 1 + EFS_META_SLOT_STRIDE);
        }
        int rc = EFS_ERR_INVAL;
        const char *scheme = "v5";
        int loaded = 0;
        uint32_t used_ci = UINT32_MAX;

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
            used_ci = ci;
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
        if (!loaded && is_chunk && g_last_chunk_even_ci != UINT32_MAX &&
            g_chunk_skip_streak < 3) {
            for (int d = 0; d <= 64 && !loaded; d++) {
                for (int sgn = 0; sgn < 2 && !loaded; sgn++) {
                    if (d == 0 && sgn == 1)
                        continue;
                    int64_t even64 = (int64_t)g_last_chunk_even_ci +
                                     (sgn ? -(int64_t)d : (int64_t)d);
                    if (even64 < 0 || even64 >= (int64_t)EFS_META_MAX_PAGES)
                        continue;
                    uint32_t even = (uint32_t)even64;
                    uint32_t pair[2] = { even, even + EFS_META_SLOT_STRIDE };
                    for (int s = 0; s < 2 && !loaded; s++) {
                        if (meta_ci_quorum(root, pi, pair[s]) < 2)
                            continue;
                        rc = load_page_from_chunk(root, pi, pair[s], pages[pi]);
                        if (rc != EFS_OK)
                            continue;
                        loaded = 1;
                        used_ci = pair[s];
                        if (d > 1) {
                            g_client.meta_heal = 1;
                            fprintf(stderr,
                                    "meta: page %u at ci=%u (delta %d from last)\n",
                                    pi, pair[s], sgn ? -d : d);
                        }
                    }
                }
            }
        }
        if (!loaded && is_chunk && g_last_chunk_even_ci == UINT32_MAX) {
            uint32_t rpi = pi - root->ino_page_count;
            uint32_t even = hunt_chunk_page_even_ci(root, pi);
            if (even != UINT32_MAX) {
                g_client.meta_heal = 1;
                if (even >= rpi)
                    g_meta_chunk_ci_even_base = even - rpi;
                uint32_t found[2] = { even, even + EFS_META_SLOT_STRIDE };
                for (int ti = 0; ti < 2 && !loaded; ti++) {
                    rc = load_page_from_chunk(root, pi, found[ti], pages[pi]);
                    if (rc == EFS_OK) {
                        loaded = 1;
                        used_ci = found[ti];
                    }
                }
            }
        }
        if (loaded) {
            g_chunk_skip_streak = 0;
            if (is_chunk && used_ci != UINT32_MAX)
                g_last_chunk_even_ci = (used_ci >= EFS_META_SLOT_STRIDE)
                                           ? used_ci - EFS_META_SLOT_STRIDE
                                           : used_ci;
            if ((pi % 1000u) == 0) {
                fprintf(stderr, "meta: loaded %u/%u\n", pi, root->page_count);
                fflush(stderr);
            }
            continue;
        }
        if (is_chunk) {
            g_chunk_skip_streak++;
            g_client.meta_heal = 1;
            g_client.meta_heal_skipped++;
            fprintf(stderr,
                    "meta: skipping unrecoverable chunk page %u/%u rc=%d (%s)\n",
                    pi, root->page_count, rc, efs_strerror(rc));
            fflush(stderr);
            continue;
        }
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

    efs_client_table_lock();
    pthread_mutex_lock(&g_client.idx_mu); /* table swap vs op-path workers */
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
        if (rc == EFS_OK && efs_client_meta_cache_adopt(blob, blob_len) == 0)
            blob = NULL;
    }
    pthread_mutex_unlock(&g_client.idx_mu);
    efs_client_table_unlock();
    free(blob);
    return rc;
}

static int fetch_meta_blob_from(const char *host, uint16_t port,
                                void **payload_out, uint32_t *len_out)
{
    int fd = efs_connect_tcp(host, port);
    if (fd < 0)
        return EFS_ERR_NET;

    /* The GET_META reply is the full serialized table — over a GiB at
     * multi-million-inode scale, and the server serializes it on a
     * rebuild-busy handler. The 30 s IO timeout was killing mount fetches
     * and STALE resyncs under load (rc=-6 loops). Default 180 s,
     * EFS_FETCH_TIMEOUT_MS overrides. */
    static int fetch_timeout_ms = -1;
    if (fetch_timeout_ms < 0) {
        const char *e = getenv("EFS_FETCH_TIMEOUT_MS");
        fetch_timeout_ms = (e && *e) ? atoi(e) : 180000;
        if (fetch_timeout_ms < 1000)
            fetch_timeout_ms = 1000;
    }
    efs_set_recv_timeout(fd, fetch_timeout_ms);
    efs_set_send_timeout(fd, fetch_timeout_ms);

    uint8_t type;
    void *payload = NULL;
    uint32_t payload_len = 0;
    char want[EFS_MAX_NAME];
    memset(want, 0, sizeof(want));
    if (g_client.export_name[0])
        strncpy(want, g_client.export_name, EFS_MAX_NAME - 1);
    if (efs_send_msg(fd, EFS_MSG_GET_META, want[0] ? want : NULL,
                     want[0] ? (uint32_t)sizeof(want) : 0) != 0 ||
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

/* Network-only phase of metadata fetch: query the bootstrap node first, then
 * all nodes, keeping the newest EFSR and its assembled EFSM blob when a
 * server has one. No locks taken and no g_client mutation, so callers can
 * run it unlocked and then swap tables under their own lock hold (the STALE
 * resync snapshots dirty state in the same critical section as the swap). */
int efs_client_fetch_meta_best(const char *host, uint16_t port,
                               struct efs_meta_fetch *f)
{
    /* Prefer the newest EFSR generation across the cluster so a root that
     * missed the last 2-ack quorum cannot shadow fresher peers. */
    memset(f, 0, sizeof(*f));
    f->last_rc = EFS_ERR_NET;
    struct efs_export_root best_root;
    memset(&best_root, 0, sizeof(best_root));
    int have_root = 0;
    int saw_bootstrap = 0;
    int last_fetch_rc = EFS_ERR_NET;
    int fetch_ok = 0;
    char *best_efsm = NULL;
    size_t best_efsm_len = 0;

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
                size_t used = 0;
                if (efs_export_root_deserialize_used(&root, payload, plen,
                                                     &used) == 0) {
                    if (g_client.export_name[0] && root.name[0] &&
                        strcmp(root.name, g_client.export_name) != 0) {
                        efs_export_root_free(&root);
                        free(payload);
                        continue;
                    }
                    /* Ignore bootstrap shells (no pages yet) unless this is
                     * the export we asked to mount empty. */
                    if (root.page_count == 0 || root.blob_len == 0) {
                        saw_bootstrap = 1;
                        if (root.id)
                            f->bootstrap_id = root.id;
                        efs_export_root_free(&root);
                    } else if (!have_root || root.generation > best_root.generation) {
                        efs_export_root_free(&best_root);
                        efs_export_root_move(&best_root, &root);
                        have_root = 1;
                        /* The table blob must belong to the SAME generation
                         * as the root we adopt: take this node's blob, or
                         * drop any blob a previous node gave us — keeping an
                         * older blob under a newer root silently regresses
                         * the table (subtrees committed in between vanish). */
                        free(best_efsm);
                        best_efsm = NULL;
                        best_efsm_len = 0;
                        if (used > 0 && used < plen &&
                            efs_meta_blob_is_export((char *)payload + used,
                                                    plen - used)) {
                            best_efsm_len = plen - used;
                            best_efsm = malloc(best_efsm_len);
                            if (best_efsm)
                                memcpy(best_efsm, (char *)payload + used,
                                       best_efsm_len);
                            else
                                best_efsm_len = 0;
                        }
                    } else {
                        /* Same generation as the adopted root: if that node
                         * gave no blob (mid-rebuild) but this one has the
                         * table live, take its blob — same gen means same
                         * content. */
                        if (have_root &&
                            root.generation == best_root.generation &&
                            !best_efsm && used > 0 && used < plen &&
                            efs_meta_blob_is_export((char *)payload + used,
                                                    plen - used)) {
                            best_efsm_len = plen - used;
                            best_efsm = malloc(best_efsm_len);
                            if (best_efsm)
                                memcpy(best_efsm, (char *)payload + used,
                                       best_efsm_len);
                            else
                                best_efsm_len = 0;
                        }
                        efs_export_root_free(&root);
                    }
                }
            } else if (efs_meta_blob_is_export(payload, plen)) {
                /* Bare EFSM (legacy, no root): usable only when no
                 * generation-tracked root is in play. */
                if (!have_root) {
                    free(best_efsm);
                    best_efsm = payload;
                    best_efsm_len = plen;
                    payload = NULL;
                }
            }
            free(payload);
        }
    }

    f->have_root = have_root;
    f->saw_bootstrap = saw_bootstrap;
    f->fetch_ok = fetch_ok;
    f->last_rc = last_fetch_rc;
    f->efsm = best_efsm;
    f->efsm_len = best_efsm_len;
    if (have_root)
        efs_export_root_move(&f->root, &best_root);
    efs_export_root_free(&best_root);
    return fetch_ok ? EFS_OK : last_fetch_rc;
}

int efs_client_fetch_metadata(const char *host, uint16_t port)
{
    struct efs_meta_fetch f;
    (void)efs_client_fetch_meta_best(host, port, &f);
    int have_root = f.have_root;
    int saw_bootstrap = f.saw_bootstrap;
    int fetch_ok = f.fetch_ok;
    int last_fetch_rc = f.last_rc;
    char *best_efsm = f.efsm;
    size_t best_efsm_len = f.efsm_len;
    struct efs_export_root best_root;
    memset(&best_root, 0, sizeof(best_root));
    if (have_root)
        efs_export_root_move(&best_root, &f.root);

    if (best_efsm) {
        efs_client_table_lock();
        pthread_mutex_lock(&g_client.idx_mu); /* table swap vs op-path workers */
        int rc = efs_export_deserialize(&g_client.export, best_efsm, best_efsm_len);
        if (rc == EFS_OK) {
            if (have_root) {
                if (efs_export_root_copy(&g_client.export.root, &best_root) == EFS_OK) {
                    g_client.export.meta_fragmented = 1;
                    g_client.export.next_ino = g_client.export.next_ino
                                                   ? g_client.export.next_ino
                                                   : best_root.next_ino;
                    uint32_t cs = best_root.chunk_size;
                    g_client.export.chunk_size = efs_chunk_size_valid(cs)
                                                     ? cs
                                                     : EFS_DEFAULT_CHUNK_SIZE;
                    g_client.export.features = best_root.features;
                }
                /* Republish a clean generation over the torn EFSR. */
                g_client.meta_dirty = 1;
            } else {
                g_client.export.meta_fragmented = 0;
            }
            if (efs_client_meta_cache_adopt(best_efsm, best_efsm_len) == 0)
                best_efsm = NULL;
            fprintf(stderr,
                    "meta: loaded live tables inodes=%llu chunks=%llu gen=%llu\n",
                    (unsigned long long)g_client.export.inode_count,
                    (unsigned long long)g_client.export.chunk_count,
                    (unsigned long long)g_client.export.root.generation);
            pthread_mutex_unlock(&g_client.idx_mu);
            efs_client_table_unlock();
            free(best_efsm);
            efs_export_root_free(&best_root);
            return rc;
        }
        pthread_mutex_unlock(&g_client.idx_mu);
        efs_client_table_unlock();
        free(best_efsm);
        best_efsm = NULL;
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
        return rc;
    }
    /* Mounting an export that does not exist is a hard error. The ONLY
     * empty-mount case is a bootstrap shell: a root with the requested name
     * but no pages yet, which is exactly what mkfs publishes. Anything else
     * (typo'd name, table mid-rebuild everywhere) must fail, not silently
     * attach the client to a void table it can never flush. */
    if (!have_root && !best_efsm && g_client.export_name[0] && saw_bootstrap) {
        pthread_mutex_lock(&g_client.lock);
        {
            efs_export_id_t eid = f.bootstrap_id ? f.bootstrap_id : 2;
            if (!f.bootstrap_id) {
                if (strcmp(g_client.export_name, "test") == 0)
                    eid = 1;
                else if (strcmp(g_client.export_name, "fiobench") == 0)
                    eid = 2;
            }
            efs_export_init(&g_client.export, eid, g_client.export_name);
        }
        g_client.export.meta_fragmented = 0;
        pthread_mutex_unlock(&g_client.lock);
        fprintf(stderr,
                "meta: mounting empty export '%s' (bootstrap root, no pages yet)\n",
                g_client.export_name);
        return EFS_OK;
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

/* 4096 x 2 x 128 KiB = 1 GiB worst case (was 16 GiB at 65536 slots). The
 * kernel page cache already covers buffered reads; this only serves direct
 * I/O re-reads and dcache RMW bases, which have small working sets. */
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
    /* Sub-chunk read: dirty dcache first (newer than rdcache), then
     * decoded cache — no malloc(128k) on the 4k path. */
    if (chunk_size && size < chunk_size &&
        (offset / chunk_size) == ((end - 1) / chunk_size)) {
        uint32_t ci = (uint32_t)(offset / chunk_size);
        uint32_t off = (uint32_t)(offset % chunk_size);
        if (efs_dcache_copy(ino, ci, off, (uint8_t *)buf, (uint32_t)size) == 0) {
            *out_len = size;
            return EFS_OK;
        }
        if (efs_rdcache_copy(ino, ci, off, (uint8_t *)buf, (uint32_t)size) == 0) {
            *out_len = size;
            return EFS_OK;
        }
    }
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
            jobs[batch].chunk = efs_buf_alloc(chunk_size);
            if (!jobs[batch].chunk) {
                for (uint32_t j = 0; j < batch; j++)
                    efs_buf_free(jobs[j].chunk, chunk_size);
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
    return EFS_OK;
}
