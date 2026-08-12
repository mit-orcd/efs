#include "client_internal.h"
#include "efs/protocol.h"
#include "efs/network.h"
#include "efs/erasure.h"
#include "efs/checksum.h"
#include "efs/placement.h"
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include <poll.h>
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

static uint64_t pack_dirty_chunk(efs_ino_t ino, uint32_t chunk_index)
{
    uint64_t k = ino ^ ((uint64_t)chunk_index * 0x9E3779B97F4A7C15ULL);
    return k ? k : 1;
}

static int dirty_set_ensure(uint64_t **keys, uint64_t *mask, uint64_t count)
{
    if (*keys && count * 2 <= *mask + 1)
        return 0;
    uint64_t old_mask = *mask;
    uint64_t *old_keys = *keys;
    uint64_t cap = 16;
    while (cap < (count ? count * 4 : 16))
        cap *= 2;
    uint64_t *nk = calloc(cap, sizeof(uint64_t));
    if (!nk)
        return -1;
    *keys = nk;
    *mask = cap - 1;
    if (old_keys) {
        for (uint64_t i = 0; i <= old_mask; i++) {
            uint64_t k = old_keys[i];
            if (!k)
                continue;
            uint64_t j = k & *mask;
            while ((*keys)[j] != 0)
                j = (j + 1) & *mask;
            (*keys)[j] = k;
        }
        free(old_keys);
    }
    return 0;
}

static void dirty_set_put(uint64_t *keys, uint64_t mask, uint64_t key, uint64_t *count)
{
    if (!keys || mask == 0 || key == 0)
        return;
    uint64_t i = key & mask;
    for (uint64_t n = 0; n <= mask; n++) {
        if (keys[i] == 0) {
            keys[i] = key;
            (*count)++;
            return;
        }
        if (keys[i] == key)
            return;
        i = (i + 1) & mask;
    }
}

static void dirty_sets_clear(void)
{
    if (g_client.dirty_ino_keys && g_client.dirty_ino_mask)
        memset(g_client.dirty_ino_keys, 0,
               (g_client.dirty_ino_mask + 1) * sizeof(uint64_t));
    if (g_client.dirty_chunk_keys && g_client.dirty_chunk_mask)
        memset(g_client.dirty_chunk_keys, 0,
               (g_client.dirty_chunk_mask + 1) * sizeof(uint64_t));
    g_client.dirty_ino_count = 0;
    g_client.dirty_chunk_count = 0;
}

void efs_client_mark_ino_dirty(efs_ino_t ino)
{
    if (!g_client.meta_batch || ino == 0)
        return;
    if (dirty_set_ensure(&g_client.dirty_ino_keys, &g_client.dirty_ino_mask,
                         g_client.dirty_ino_count + 1) != 0)
        return;
    dirty_set_put(g_client.dirty_ino_keys, g_client.dirty_ino_mask, ino,
                  &g_client.dirty_ino_count);
}

void efs_client_mark_chunk_dirty(efs_ino_t ino, uint32_t chunk_index)
{
    if (!g_client.meta_batch || ino == 0)
        return;
    uint64_t before = 0;
    if (g_client.dirty_chunk_keys && g_client.dirty_chunk_mask) {
        uint64_t key = pack_dirty_chunk(ino, chunk_index);
        uint64_t i = key & g_client.dirty_chunk_mask;
        for (uint64_t n = 0; n <= g_client.dirty_chunk_mask; n++) {
            if (g_client.dirty_chunk_keys[i] == 0)
                break;
            if (g_client.dirty_chunk_keys[i] == key)
                return; /* already dirty */
            i = (i + 1) & g_client.dirty_chunk_mask;
        }
    }
    before = g_client.dirty_chunk_count;
    if (dirty_set_ensure(&g_client.dirty_chunk_keys, &g_client.dirty_chunk_mask,
                         before + 1) != 0)
        return;
    dirty_set_put(g_client.dirty_chunk_keys, g_client.dirty_chunk_mask,
                  pack_dirty_chunk(ino, chunk_index), &g_client.dirty_chunk_count);
    if (g_client.dirty_chunk_count == before)
        return; /* set put was a no-op duplicate */
    if (g_client.dirty_chunk_count > g_client.dirty_chunk_cap) {
        uint64_t ncap = g_client.dirty_chunk_cap ? g_client.dirty_chunk_cap * 2 : 64;
        efs_ino_t *ni = realloc(g_client.dirty_chunk_inos, ncap * sizeof(efs_ino_t));
        uint32_t *nx = realloc(g_client.dirty_chunk_idxs, ncap * sizeof(uint32_t));
        if (!ni || !nx) {
            free(ni);
            free(nx);
            return;
        }
        g_client.dirty_chunk_inos = ni;
        g_client.dirty_chunk_idxs = nx;
        g_client.dirty_chunk_cap = ncap;
    }
    uint64_t slot = g_client.dirty_chunk_count - 1;
    g_client.dirty_chunk_inos[slot] = ino;
    g_client.dirty_chunk_idxs[slot] = chunk_index;
}

/* Replicate the tiny export root (EFSR) to all nodes; need ≥2 acks.
 * Fan-out + poll (like fragment PUT): skip down peers, return as soon as
 * quorum is met so one dead/blackholed node cannot serialize SO_RCVTIMEO. */
static int send_meta_root(const char *buf, size_t len)
{
    uint32_t n = g_client.node_count;
    if (n > EFS_MAX_NODES)
        n = EFS_MAX_NODES;

    int fds[EFS_MAX_NODES];
    efs_node_id_t nids[EFS_MAX_NODES];
    int pending[EFS_MAX_NODES];
    int acks = 0;

    for (uint32_t i = 0; i < n; i++) {
        fds[i] = -1;
        pending[i] = 0;
        nids[i] = g_client.nodes[i].id;
        if (nids[i] == 0 || efs_client_node_is_down(nids[i]))
            continue;
        fds[i] = efs_client_conn_get(nids[i]);
        if (fds[i] < 0) {
            efs_client_node_note_fail(nids[i]);
            continue;
        }
    }

    for (uint32_t i = 0; i < n; i++) {
        if (fds[i] < 0)
            continue;
        if (efs_send_msg(fds[i], EFS_MSG_PUT_META, buf, (uint32_t)len) != 0) {
            efs_client_conn_drop(nids[i], fds[i]);
            efs_client_node_note_fail(nids[i]);
            fds[i] = -1;
            continue;
        }
        pending[i] = 1;
    }

    struct timespec ts0;
    clock_gettime(CLOCK_MONOTONIC, &ts0);
    int64_t deadline_ms = (int64_t)ts0.tv_sec * 1000 +
                          (int64_t)ts0.tv_nsec / 1000000 + EFS_IO_TIMEOUT_MS;

    while (acks < 2) {
        struct pollfd pfds[EFS_MAX_NODES];
        int map[EFS_MAX_NODES];
        int npoll = 0;
        for (uint32_t i = 0; i < n; i++) {
            if (!pending[i] || fds[i] < 0)
                continue;
            pfds[npoll].fd = fds[i];
            pfds[npoll].events = POLLIN;
            pfds[npoll].revents = 0;
            map[npoll] = (int)i;
            npoll++;
        }
        if (npoll == 0)
            break;

        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        int64_t now_ms = (int64_t)ts.tv_sec * 1000 + (int64_t)ts.tv_nsec / 1000000;
        int64_t left = deadline_ms - now_ms;
        if (left <= 0) {
            for (uint32_t i = 0; i < n; i++) {
                if (!pending[i] || fds[i] < 0)
                    continue;
                efs_client_conn_drop(nids[i], fds[i]);
                efs_client_node_note_fail(nids[i]);
                fds[i] = -1;
                pending[i] = 0;
            }
            break;
        }
        int wait_ms = left > 2000 ? 2000 : (int)left;
        int pr = poll(pfds, (nfds_t)npoll, wait_ms);
        if (pr <= 0)
            continue;

        for (int p = 0; p < npoll; p++) {
            if (!(pfds[p].revents & (POLLIN | POLLERR | POLLHUP)))
                continue;
            int i = map[p];
            if (!pending[i] || fds[i] < 0)
                continue;
            uint8_t type = 0;
            void *reply = NULL;
            uint32_t reply_len = 0;
            if ((pfds[p].revents & (POLLERR | POLLHUP)) ||
                efs_recv_msg(fds[i], &type, &reply, &reply_len) != 0 ||
                type != EFS_MSG_PUT_META_REPLY || reply_len < 1) {
                free(reply);
                efs_client_conn_drop(nids[i], fds[i]);
                efs_client_node_note_fail(nids[i]);
                fds[i] = -1;
                pending[i] = 0;
                continue;
            }
            uint8_t status = ((uint8_t *)reply)[0];
            free(reply);
            if (status == EFS_PUT_META_OK) {
                acks++;
                efs_client_node_note_ok(nids[i]);
                efs_client_conn_release(nids[i], fds[i]);
            } else {
                efs_client_conn_drop(nids[i], fds[i]);
                efs_client_node_note_fail(nids[i]);
            }
            fds[i] = -1;
            pending[i] = 0;
        }
    }

    /* Quorum met: drop stragglers without blocking on their RCVTIMEO. */
    for (uint32_t i = 0; i < n; i++) {
        if (fds[i] < 0)
            continue;
        if (acks >= 2) {
            struct pollfd p = { .fd = fds[i], .events = POLLIN };
            int pr = poll(&p, 1, 0);
            if (pr > 0 && (p.revents & POLLIN) &&
                !(p.revents & (POLLERR | POLLHUP | POLLNVAL))) {
                uint8_t type = 0;
                void *reply = NULL;
                uint32_t reply_len = 0;
                if (efs_recv_msg(fds[i], &type, &reply, &reply_len) == 0) {
                    free(reply);
                    efs_client_conn_release(nids[i], fds[i]);
                    fds[i] = -1;
                    pending[i] = 0;
                    continue;
                }
                free(reply);
            }
            efs_client_conn_drop(nids[i], fds[i]);
        } else {
            efs_client_conn_drop(nids[i], fds[i]);
            efs_client_node_note_fail(nids[i]);
        }
        fds[i] = -1;
        pending[i] = 0;
    }

    return (acks >= 2) ? EFS_OK : EFS_ERR_NO_QUORUM;
}

/* Serialize concurrent replicators so two flushes don't interleave page PUTs /
 * gen bumps. This replaces holding g_client.lock across serialize+network. */
static pthread_mutex_t g_repl_mu = PTHREAD_MUTEX_INITIALIZER;

/* Pack the full export into 2+1 pages, then push the EFSR root (≥2 acks). */
static int efs_client_replicate_metadata_once(void)
{
    char *blob = NULL;
    size_t blob_len = 0;
    uint64_t new_gen = 1;

    pthread_mutex_lock(&g_repl_mu);

    pthread_mutex_lock(&g_client.lock);
    if (g_client.meta_batch &&
        g_client.dirty_ino_count == 0 && g_client.dirty_chunk_count == 0 &&
        !g_client.meta_dirty) {
        g_client.meta_dirty_ops = 0;
        pthread_mutex_unlock(&g_client.lock);
        pthread_mutex_unlock(&g_repl_mu);
        return EFS_OK;
    }
    if (efs_export_serialize(&g_client.export, &blob, &blob_len) != EFS_OK) {
        pthread_mutex_unlock(&g_client.lock);
        pthread_mutex_unlock(&g_repl_mu);
        return EFS_ERR_NOMEM;
    }
    new_gen = g_client.export.root.generation + 1;
    if (new_gen == 0)
        new_gen = 1;
    pthread_mutex_unlock(&g_client.lock);

    if (!blob)
        return EFS_ERR_NOMEM;
    if (blob_len > (size_t)EFS_META_MAX_PAGES * EFS_META_PAGE_SIZE) {
        fprintf(stderr,
                "meta replicate: export blob %zu bytes exceeds cap %zu "
                "(%u pages × %u); raise EFS_META_MAX_PAGES\n",
                blob_len,
                (size_t)EFS_META_MAX_PAGES * EFS_META_PAGE_SIZE,
                (unsigned)EFS_META_MAX_PAGES, (unsigned)EFS_META_PAGE_SIZE);
        fflush(stderr);
        free(blob);
        return EFS_ERR_INVAL;
    }

    /* 2+1 EC needs 3 distinct nodes; on a 1-2 node ring placement wraps and
     * two fragments land on one machine, so a "quorum" of 2 acks can be a
     * single disk. Refuse to publish data metadata we cannot durably store. */
    pthread_mutex_lock(&g_client.lock);
    uint32_t live_nodes = g_client.node_count;
    pthread_mutex_unlock(&g_client.lock);
    if (live_nodes < EFS_NUM_FRAGMENTS) {
        fprintf(stderr,
                "meta replicate: only %u live node(s); 2+1 EC needs %u distinct "
                "nodes — refusing to commit gen %llu\n",
                live_nodes, (unsigned)EFS_NUM_FRAGMENTS,
                (unsigned long long)new_gen);
        fflush(stderr);
        free(blob);
        return EFS_ERR_NO_QUORUM;
    }

    struct efs_export_root root;
    memset(&root, 0, sizeof(root));
    pthread_mutex_lock(&g_client.lock);
    int prc = efs_export_root_prepare(&root, &g_client.export, new_gen,
                                      (uint32_t)blob_len);
    pthread_mutex_unlock(&g_client.lock);
    if (prc != EFS_OK) {
        free(blob);
        return prc;
    }

    /* Heap-allocate page/fragments — ~320 KiB is too large for some stacks. */
    uint8_t *page = malloc(EFS_META_PAGE_SIZE);
    uint8_t *frag_buf = malloc(EFS_NUM_FRAGMENTS * EFS_META_FRAGMENT_SIZE);
    uint8_t *frags[EFS_NUM_FRAGMENTS];
    if (!page || !frag_buf) {
        free(page);
        free(frag_buf);
        free(blob);
        efs_export_root_free(&root);
        return EFS_ERR_NOMEM;
    }
    frag_ptrs(frag_buf, EFS_META_FRAGMENT_SIZE, frags);
    /* Test hook: abort after N page PUTs without flipping the root so dual-slot
     * durability can be smoke-tested (live gen remains mountable). */
    uint32_t abort_after = UINT32_MAX;
    {
        const char *env = getenv("EFS_META_FLUSH_ABORT_AFTER_PAGES");
        if (env && *env) {
            char *end = NULL;
            unsigned long v = strtoul(env, &end, 10);
            if (end != env && v < UINT32_MAX)
                abort_after = (uint32_t)v;
        }
    }
    for (uint32_t pi = 0; pi < root.page_count; pi++) {
        if (efs_meta_extract_page(blob, (uint32_t)blob_len, pi, page) != EFS_OK) {
            free(page);
            free(frag_buf);
            free(blob);
            efs_export_root_free(&root);
            return EFS_ERR_INVAL;
        }
        efs_encode_chunk(page, EFS_META_PAGE_SIZE, EFS_META_PAGE_SIZE, frags);

        uint8_t checksums[EFS_NUM_FRAGMENTS][EFS_HASH_SIZE];
        for (int fi = 0; fi < EFS_NUM_FRAGMENTS; fi++)
            efs_hash(frags[fi], EFS_META_FRAGMENT_SIZE, checksums[fi]);

        uint32_t ci = efs_meta_page_chunk_index(new_gen, pi);
        if (ci == UINT32_MAX) {
            free(page);
            free(frag_buf);
            free(blob);
            efs_export_root_free(&root);
            return EFS_ERR_INVAL;
        }

        efs_node_id_t nodes[EFS_NUM_FRAGMENTS];
        efs_place_fragments(g_client.nodes, g_client.node_count,
                            EFS_META_TABLE_INO, ci, nodes);

        const uint8_t *cfrags[EFS_NUM_FRAGMENTS];
        for (int fi = 0; fi < EFS_NUM_FRAGMENTS; fi++)
            cfrags[fi] = frags[fi];
        int rc = efs_client_put_fragments_parallel(EFS_META_TABLE_INO, ci, nodes,
                                                   cfrags, EFS_META_FRAGMENT_SIZE,
                                                   checksums);
        if (rc != EFS_OK) {
            free(page);
            free(frag_buf);
            free(blob);
            efs_export_root_free(&root);
            return rc;
        }
        for (int fi = 0; fi < EFS_NUM_FRAGMENTS; fi++)
            memcpy(efs_export_root_checksum(&root, pi, fi), checksums[fi],
                   EFS_HASH_SIZE);
        /* After N successful page PUTs, skip root flip (dual-slot smoke). */
        if (abort_after != UINT32_MAX && (pi + 1) >= abort_after) {
            fprintf(stderr,
                    "meta replicate: abort after %u page PUT(s) "
                    "(EFS_META_FLUSH_ABORT_AFTER_PAGES); root not flipped\n",
                    abort_after);
            fflush(stderr);
            free(page);
            free(frag_buf);
            free(blob);
            efs_export_root_free(&root);
            return EFS_ERR_IO;
        }
    }
    free(page);
    free(frag_buf);
    free(blob);

    char *root_buf = NULL;
    size_t root_len = 0;
    if (efs_export_root_serialize(&root, &root_buf, &root_len) != EFS_OK) {
        efs_export_root_free(&root);
        return EFS_ERR_NOMEM;
    }

    int rc = send_meta_root(root_buf, root_len);
    free(root_buf);

    if (rc == EFS_OK) {
        pthread_mutex_lock(&g_client.lock);
        g_client.export.meta_fragmented = 1;
        efs_export_root_move(&g_client.export.root, &root);
        {
            uint32_t cs = g_client.export.root.chunk_size;
            g_client.export.chunk_size = efs_chunk_size_valid(cs) ? cs
                                                                  : EFS_DEFAULT_CHUNK_SIZE;
        }
        dirty_sets_clear();
        g_client.meta_dirty = 0;
        g_client.meta_dirty_ops = 0;
        pthread_mutex_unlock(&g_client.lock);
    } else {
        efs_export_root_free(&root);
    }
    pthread_mutex_unlock(&g_repl_mu);
    return rc;
}

int efs_client_replicate_metadata(void)
{
    /* Bulk copies (ecopy/rsync) hit a full meta flush every meta_batch_ops
     * creates/chmods. Transient net/quorum blips show up as fchmod EIO —
     * retry with exponential backoff + jitter before surfacing failure. */
    int rc = EFS_ERR_NET;
    for (int attempt = 1; attempt <= 4; attempt++) {
        rc = efs_client_replicate_metadata_once();
        if (rc == EFS_OK)
            return EFS_OK;
        if (rc != EFS_ERR_NET && rc != EFS_ERR_NO_QUORUM)
            break;
        fprintf(stderr, "meta replicate attempt %d/4 failed: %s\n",
                attempt, efs_strerror(rc));
        fflush(stderr);
        if (attempt < 4) {
            useconds_t base = 100000u << (attempt - 1); /* 100,200,400ms */
            usleep(base + (useconds_t)(rand() % 50000));
        }
    }
    fprintf(stderr, "meta replicate giving up: %s\n", efs_strerror(rc));
    fflush(stderr);
    return rc;
}

void efs_client_enable_meta_batch(uint32_t every_n_ops)
{
    g_client.meta_batch = 1;
    g_client.meta_batch_ops = every_n_ops ? every_n_ops : 4096;
    g_client.meta_dirty = 0;
    g_client.meta_dirty_ops = 0;
    dirty_sets_clear();
}

int efs_client_note_meta_change(int force)
{
    if (!g_client.meta_batch || force)
        return efs_client_replicate_metadata();

    pthread_mutex_lock(&g_client.lock);
    g_client.meta_dirty = 1;
    g_client.meta_dirty_ops++;
    uint32_t ops = g_client.meta_dirty_ops;
    uint32_t thresh = g_client.meta_batch_ops ? g_client.meta_batch_ops : 4096;
    int flush = (ops >= thresh);
    pthread_mutex_unlock(&g_client.lock);

    if (flush)
        return efs_client_replicate_metadata();
    return EFS_OK;
}

int efs_client_put_fragment(efs_node_id_t node_id, efs_ino_t ino, uint32_t chunk_index,
                            uint32_t fragment_index, const uint8_t *data, uint32_t frag_len,
                            const uint8_t checksum[EFS_HASH_SIZE])
{
    if (node_id == 0 || frag_len == 0)
        return EFS_ERR_INVAL;

    size_t msg_size = sizeof(struct efs_msg_put_chunk) + frag_len;

    /* Retry after efsd bounce: pooled fds die; drop invalidates the idle
     * pool and the next attempt reconnects. */
    for (int attempt = 1; attempt <= 3; attempt++) {
        int fd = efs_client_conn_get(node_id);
        if (fd < 0) {
            if (attempt < 3) {
                usleep(100000u * (unsigned)attempt);
                continue;
            }
            return EFS_ERR_NET;
        }

        struct efs_msg_put_chunk *req = malloc(msg_size);
        if (!req) {
            efs_client_conn_release(node_id, fd);
            return EFS_ERR_NOMEM;
        }
        memset(req, 0, sizeof(*req));
        req->export_id = g_client.export_id;
        req->ino = ino;
        req->chunk_index = chunk_index;
        req->fragment_index = fragment_index;
        req->data_len = frag_len;
        memcpy(req->checksum, checksum, EFS_HASH_SIZE);
        memcpy((uint8_t *)req + sizeof(*req), data, frag_len);

        uint8_t reply_type;
        void *reply = NULL;
        uint32_t reply_len = 0;
        if (efs_send_msg(fd, EFS_MSG_PUT_CHUNK, req, (uint32_t)msg_size) != 0 ||
            efs_recv_msg(fd, &reply_type, &reply, &reply_len) != 0 ||
            reply_type != EFS_MSG_PUT_CHUNK_REPLY || reply_len != 1) {
            free(req);
            free(reply);
            efs_client_conn_drop(node_id, fd);
            if (attempt < 3) {
                usleep(100000u * (unsigned)attempt);
                continue;
            }
            return EFS_ERR_NET;
        }

        free(req);
        uint8_t status = ((uint8_t *)reply)[0];
        free(reply);
        efs_client_conn_release(node_id, fd);
        if (status == EFS_PUT_CHUNK_OK)
            return EFS_OK;
        if (status == EFS_PUT_CHUNK_QUOTA_EXCEEDED)
            return EFS_ERR_QUOTA;
        return EFS_ERR_IO;
    }
    return EFS_ERR_NET;
}

/* failed_out[i]=1 marks placement slots that need invalidate/retry. */
static int put_fragments_parallel_once(efs_ino_t ino, uint32_t chunk_index,
                                      const efs_node_id_t nodes[EFS_NUM_FRAGMENTS],
                                      const uint8_t *fragments[EFS_NUM_FRAGMENTS],
                                      uint32_t frag_len,
                                      const uint8_t checksums[EFS_NUM_FRAGMENTS][EFS_HASH_SIZE],
                                      int failed_out[EFS_NUM_FRAGMENTS])
{
    int fds[EFS_NUM_FRAGMENTS];
    int pending[EFS_NUM_FRAGMENTS];
    struct efs_msg_put_chunk hdrs[EFS_NUM_FRAGMENTS];
    int acks = 0;
    int quota_errors = 0;
    int reachable = 0;

    if (failed_out) {
        for (int i = 0; i < EFS_NUM_FRAGMENTS; i++)
            failed_out[i] = 0;
    }

    for (int i = 0; i < EFS_NUM_FRAGMENTS; i++) {
        fds[i] = -1;
        pending[i] = 0;
        memset(&hdrs[i], 0, sizeof(hdrs[i]));
    }

    /* Unreachable peers count as missing acks — do not abort the whole PUT. */
    for (int i = 0; i < EFS_NUM_FRAGMENTS; i++) {
        if (nodes[i] == 0) {
            if (failed_out)
                failed_out[i] = 1;
            continue;
        }
        if (efs_client_node_is_down(nodes[i])) {
            if (failed_out)
                failed_out[i] = 1;
            continue;
        }
        fds[i] = efs_client_conn_get(nodes[i]);
        if (fds[i] < 0) {
            efs_client_node_note_fail(nodes[i]);
            if (failed_out)
                failed_out[i] = 1;
            continue;
        }
        reachable++;
        hdrs[i].export_id = g_client.export_id;
        hdrs[i].ino = ino;
        hdrs[i].chunk_index = chunk_index;
        hdrs[i].fragment_index = (uint32_t)i;
        hdrs[i].data_len = frag_len;
        memcpy(hdrs[i].checksum, checksums[i], EFS_HASH_SIZE);
    }

    /* Send on every live fd before waiting (overlap RTTs). writev header+body
     * so we never bounce-copy the fragment into a contiguous malloc. */
    for (int i = 0; i < EFS_NUM_FRAGMENTS; i++) {
        if (fds[i] < 0)
            continue;
        /* Never enter a blocking writev: a slow peer fills the TCP window
         * and SO_SNDTIMEO stalls the whole chunk. Skip non-writable fds;
         * do not note_fail (window backpressure ≠ dead peer). */
        {
            struct pollfd p = { .fd = fds[i], .events = POLLOUT };
            int pr = poll(&p, 1, 0);
            if (pr < 0 || (p.revents & (POLLERR | POLLHUP | POLLNVAL))) {
                efs_client_conn_drop(nodes[i], fds[i]);
                efs_client_node_note_fail(nodes[i]);
                if (failed_out)
                    failed_out[i] = 1;
                fds[i] = -1;
                continue;
            }
            if (pr == 0 || !(p.revents & POLLOUT)) {
                efs_client_conn_release(nodes[i], fds[i]);
                if (failed_out)
                    failed_out[i] = 1;
                fds[i] = -1;
                continue;
            }
        }
        if (efs_send_msg_parts(fds[i], EFS_MSG_PUT_CHUNK,
                               &hdrs[i], (uint32_t)sizeof(hdrs[i]),
                               fragments[i], frag_len) != 0) {
            efs_client_conn_drop(nodes[i], fds[i]);
            efs_client_node_note_fail(nodes[i]);
            if (failed_out)
                failed_out[i] = 1;
            fds[i] = -1;
            continue;
        }
        pending[i] = 1;
    }

    /* Poll for replies; return as soon as we have ≥2 acks so one dead peer
     * cannot serialize a full SO_RCVTIMEO wait. */
    struct timespec ts0;
    clock_gettime(CLOCK_MONOTONIC, &ts0);
    int64_t deadline_ms = (int64_t)ts0.tv_sec * 1000 +
                          (int64_t)ts0.tv_nsec / 1000000 + EFS_IO_TIMEOUT_MS;

    while (acks < 2) {
        struct pollfd pfds[EFS_NUM_FRAGMENTS];
        int map[EFS_NUM_FRAGMENTS];
        int npoll = 0;
        for (int i = 0; i < EFS_NUM_FRAGMENTS; i++) {
            if (!pending[i] || fds[i] < 0)
                continue;
            pfds[npoll].fd = fds[i];
            pfds[npoll].events = POLLIN;
            pfds[npoll].revents = 0;
            map[npoll] = i;
            npoll++;
        }
        if (npoll == 0)
            break;

        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        int64_t now_ms = (int64_t)ts.tv_sec * 1000 + (int64_t)ts.tv_nsec / 1000000;
        int64_t left = deadline_ms - now_ms;
        if (left <= 0) {
            for (int i = 0; i < EFS_NUM_FRAGMENTS; i++) {
                if (!pending[i] || fds[i] < 0)
                    continue;
                efs_client_conn_drop(nodes[i], fds[i]);
                efs_client_node_note_fail(nodes[i]);
                if (failed_out)
                    failed_out[i] = 1;
                fds[i] = -1;
                pending[i] = 0;
            }
            break;
        }
        /* Cap each poll slice so we re-check the deadline promptly. */
        int wait_ms = left > 2000 ? 2000 : (int)left;
        int pr = poll(pfds, (nfds_t)npoll, wait_ms);
        if (pr < 0)
            continue;
        if (pr == 0)
            continue;

        for (int p = 0; p < npoll; p++) {
            if (!(pfds[p].revents & (POLLIN | POLLERR | POLLHUP)))
                continue;
            int i = map[p];
            if (!pending[i] || fds[i] < 0)
                continue;
            uint8_t reply_type = 0, status = 0;
            if ((pfds[p].revents & (POLLERR | POLLHUP)) ||
                efs_recv_u8_reply(fds[i], &reply_type, &status) != 0 ||
                reply_type != EFS_MSG_PUT_CHUNK_REPLY) {
                efs_client_conn_drop(nodes[i], fds[i]);
                efs_client_node_note_fail(nodes[i]);
                if (failed_out)
                    failed_out[i] = 1;
                fds[i] = -1;
                pending[i] = 0;
                continue;
            }
            if (status == EFS_PUT_CHUNK_OK) {
                acks++;
                efs_client_node_note_ok(nodes[i]);
                efs_client_conn_release(nodes[i], fds[i]);
            } else if (status == EFS_PUT_CHUNK_QUOTA_EXCEEDED) {
                quota_errors++;
                efs_client_conn_release(nodes[i], fds[i]);
            } else {
                efs_client_conn_drop(nodes[i], fds[i]);
                efs_client_node_note_fail(nodes[i]);
                if (failed_out)
                    failed_out[i] = 1;
            }
            fds[i] = -1;
            pending[i] = 0;
        }
    }

    /* Quorum met: drain or drop remaining waiters. Prefer draining a ready
     * reply so pooled fds stay healthy — dropping every third peer was
     * forcing reconnect storms (efs_connect_tcp in the write hot path). */
    for (int i = 0; i < EFS_NUM_FRAGMENTS; i++) {
        if (fds[i] < 0)
            continue;
        if (acks >= 2) {
            struct pollfd p = { .fd = fds[i], .events = POLLIN };
            int pr = poll(&p, 1, 0);
            if (pr > 0 && (p.revents & POLLIN) &&
                !(p.revents & (POLLERR | POLLHUP | POLLNVAL))) {
                uint8_t reply_type = 0, status = 0;
                if (efs_recv_u8_reply(fds[i], &reply_type, &status) == 0 &&
                    reply_type == EFS_MSG_PUT_CHUNK_REPLY) {
                    efs_client_conn_release(nodes[i], fds[i]);
                    fds[i] = -1;
                    pending[i] = 0;
                    continue;
                }
            }
            /* Still in flight / desynced: drop without note_fail. */
            efs_client_conn_drop(nodes[i], fds[i]);
            if (failed_out)
                failed_out[i] = 1;
        } else {
            efs_client_conn_drop(nodes[i], fds[i]);
            efs_client_node_note_fail(nodes[i]);
            if (failed_out)
                failed_out[i] = 1;
        }
        fds[i] = -1;
        pending[i] = 0;
    }

    if (quota_errors >= 2)
        return EFS_ERR_QUOTA;
    if (acks >= 2)
        return EFS_OK;
    if (reachable == 0)
        return EFS_ERR_NET;
    return EFS_ERR_NO_QUORUM;
}

int efs_client_put_fragments_parallel(efs_ino_t ino, uint32_t chunk_index,
                                      const efs_node_id_t nodes[EFS_NUM_FRAGMENTS],
                                      const uint8_t *fragments[EFS_NUM_FRAGMENTS],
                                      uint32_t frag_len,
                                      const uint8_t checksums[EFS_NUM_FRAGMENTS][EFS_HASH_SIZE])
{
    /* 2+1 EC requires 3 distinct nodes. On a 1-2 node ring placement wraps and
     * two fragments land on one machine, so a "2-ack quorum" can be a single
     * disk — losing that node loses the chunk. Refuse rather than store
     * undurably. */
    if (nodes[0] == nodes[1] || nodes[1] == nodes[2] || nodes[0] == nodes[2]) {
        fprintf(stderr,
                "put_fragments ino=%llu chunk=%u: EC needs 3 distinct nodes "
                "(got %u,%u,%u) — refusing undurable write\n",
                (unsigned long long)ino, chunk_index,
                (unsigned)nodes[0], (unsigned)nodes[1], (unsigned)nodes[2]);
        fflush(stderr);
        return EFS_ERR_NO_QUORUM;
    }

    /* Retry transient blips; only invalidate peers that actually failed. */
    int rc = EFS_ERR_NET;
    for (int attempt = 1; attempt <= 4; attempt++) {
        int failed[EFS_NUM_FRAGMENTS];
        rc = put_fragments_parallel_once(ino, chunk_index, nodes, fragments,
                                         frag_len, checksums, failed);
        if (rc == EFS_OK || rc == EFS_ERR_QUOTA)
            return rc;
        /* Only hard I/O failures invalidate a node's idle pool. failed[] is
         * also set for POLLOUT soft-misses (window backpressure) and down-skip,
         * which must NOT trigger a reconnect storm — the node is not dead. */
        for (int i = 0; i < EFS_NUM_FRAGMENTS; i++) {
            if (failed[i] && nodes[i] != 0 &&
                !efs_client_node_is_down(nodes[i]))
                efs_client_conn_invalidate_node(nodes[i]);
        }
        /* Fail fast when fewer than 2 peers are even reachable — retrying a
         * quorum we cannot reach only stacks latency. */
        int live = 0;
        for (int i = 0; i < EFS_NUM_FRAGMENTS; i++)
            if (nodes[i] != 0 && !efs_client_node_is_down(nodes[i]))
                live++;
        if (live < 2) {
            fprintf(stderr,
                    "put_fragments ino=%llu chunk=%u: only %d/%u peers live, "
                    "cannot reach EC quorum — failing fast\n",
                    (unsigned long long)ino, chunk_index, live,
                    (unsigned)EFS_NUM_FRAGMENTS);
            fflush(stderr);
            return EFS_ERR_NO_QUORUM;
        }
        fprintf(stderr,
                "put_fragments ino=%llu chunk=%u nodes=%u,%u,%u "
                "attempt %d/4 failed: %s (efs_rc=%d)%s\n",
                (unsigned long long)ino, chunk_index,
                (unsigned)nodes[0], (unsigned)nodes[1], (unsigned)nodes[2],
                attempt, efs_strerror(rc), rc,
                attempt < 4 ? " — retrying" : " — giving up");
        fflush(stderr);
        /* Exponential backoff + jitter instead of a fixed linear sleep. */
        if (attempt < 4) {
            useconds_t base = 100000u << (attempt - 1); /* 100,200,400ms */
            useconds_t jitter = (useconds_t)(rand() % 50000);
            usleep(base + jitter);
        }
    }
    return rc;
}

/* Assemble one chunk buffer for a write spanning [wr_start, wr_end).
 * Returns whether the chunk was built from a zero base (new or full overwrite),
 * which lets callers skip Blake3 of known-zero halves. */
static int assemble_write_chunk(efs_ino_t ino, uint64_t old_size,
                                uint64_t offset, const char *buf,
                                uint64_t chunk_start, uint64_t wr_start,
                                uint64_t wr_end, uint32_t chunk_size,
                                uint8_t *chunk)
{
    int covers_full = (wr_start == chunk_start &&
                       wr_end == chunk_start + chunk_size);
    size_t off_in_chunk = (size_t)(wr_start - chunk_start);
    size_t wr_len = (size_t)(wr_end - wr_start);
    size_t src_off = (size_t)(wr_start - offset);
    int from_zero;

    if (covers_full) {
        /* Full overwrite: copy user bytes only — no memset+memcpy. */
        memcpy(chunk, buf + src_off, chunk_size);
        return efs_bytes_are_zero(chunk, chunk_size);
    }

    if (chunk_start >= old_size) {
        from_zero = 1;
        /* Zero only the unwritten regions. */
        if (off_in_chunk > 0)
            memset(chunk, 0, off_in_chunk);
        if (off_in_chunk + wr_len < chunk_size)
            memset(chunk + off_in_chunk + wr_len, 0,
                   chunk_size - off_in_chunk - wr_len);
    } else {
        from_zero = 0;
        size_t existing = (size_t)(old_size - chunk_start);
        if (existing > chunk_size)
            existing = chunk_size;
        size_t got = 0;
        efs_client_read(ino, chunk_start, existing, (char *)chunk, &got);
        if (got < chunk_size)
            memset(chunk + got, 0, chunk_size - got);
    }

    memcpy(chunk + off_in_chunk, buf + src_off, wr_len);
    return from_zero;
}

/* Hash fragments; when from_zero and a half was not written, reuse the cached
 * zero-fragment digest and skip hashing the identical parity copy. */
static void hash_write_fragments(const uint8_t *fragments[EFS_NUM_FRAGMENTS],
                                 uint32_t frag_len, uint32_t chunk_size,
                                 int from_zero, uint64_t chunk_start,
                                 uint64_t wr_start, uint64_t wr_end,
                                 uint8_t checksums[EFS_NUM_FRAGMENTS][EFS_HASH_SIZE])
{
    uint32_t half = chunk_size / 2;
    int frag0_zero = 0, frag1_zero = 0;
    if (from_zero) {
        frag0_zero = (wr_start >= chunk_start + half);
        frag1_zero = (wr_end <= chunk_start + half);
    }

    /* Full-chunk zero write: both data halves and parity are zero. */
    if (from_zero && wr_start <= chunk_start &&
        wr_end >= chunk_start + chunk_size) {
        efs_hash_zero_fragment_len(frag_len, checksums[0]);
        efs_hash_zero_fragment_len(frag_len, checksums[1]);
        efs_hash_zero_fragment_len(frag_len, checksums[2]);
        return;
    }

    if (frag0_zero)
        efs_hash_zero_fragment_len(frag_len, checksums[0]);
    else
        efs_hash(fragments[0], frag_len, checksums[0]);

    if (frag1_zero)
        efs_hash_zero_fragment_len(frag_len, checksums[1]);
    else
        efs_hash(fragments[1], frag_len, checksums[1]);

    if (frag0_zero && !frag1_zero)
        memcpy(checksums[2], checksums[1], EFS_HASH_SIZE);
    else if (frag1_zero && !frag0_zero)
        memcpy(checksums[2], checksums[0], EFS_HASH_SIZE);
    else if (frag0_zero && frag1_zero)
        efs_hash_zero_fragment_len(frag_len, checksums[2]);
    else
        efs_hash(fragments[2], frag_len, checksums[2]);
}

struct chunk_put_job {
    efs_ino_t ino;
    uint32_t ci;
    uint64_t old_size;
    uint64_t offset;
    const char *buf;
    uint64_t end;
    int rc;
    efs_node_id_t nodes[EFS_NUM_FRAGMENTS];
    uint8_t checksums[EFS_NUM_FRAGMENTS][EFS_HASH_SIZE];
};

static void *chunk_put_worker(void *arg);

/* Persistent PUT workers — avoids pthread_create/join per FUSE write batch. */
static struct {
    pthread_mutex_t mu;
    pthread_cond_t job_cv;
    pthread_cond_t done_cv;
    pthread_t tids[EFS_WRITE_PIPELINE];
    int nworkers;
    int ready;
    int shutdown;
    struct chunk_put_job *batch;
    uint32_t batch_n;
    uint32_t next_i;
    uint32_t completed;
} g_put_pool = {
    .mu = PTHREAD_MUTEX_INITIALIZER,
    .job_cv = PTHREAD_COND_INITIALIZER,
    .done_cv = PTHREAD_COND_INITIALIZER,
};

static void *put_pool_thread(void *arg)
{
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&g_put_pool.mu);
        while (!g_put_pool.shutdown &&
               (g_put_pool.batch == NULL || g_put_pool.next_i >= g_put_pool.batch_n))
            pthread_cond_wait(&g_put_pool.job_cv, &g_put_pool.mu);
        if (g_put_pool.shutdown) {
            pthread_mutex_unlock(&g_put_pool.mu);
            return NULL;
        }
        uint32_t i = g_put_pool.next_i++;
        struct chunk_put_job *job = &g_put_pool.batch[i];
        pthread_mutex_unlock(&g_put_pool.mu);

        chunk_put_worker(job);

        pthread_mutex_lock(&g_put_pool.mu);
        g_put_pool.completed++;
        if (g_put_pool.completed == g_put_pool.batch_n)
            pthread_cond_signal(&g_put_pool.done_cv);
        pthread_mutex_unlock(&g_put_pool.mu);
    }
}

static int put_pool_ensure(void)
{
    if (g_put_pool.ready)
        return 0;
    pthread_mutex_lock(&g_put_pool.mu);
    if (!g_put_pool.ready) {
        uint32_t n = EFS_WRITE_PIPELINE;
        for (uint32_t i = 0; i < n; i++) {
            if (pthread_create(&g_put_pool.tids[i], NULL, put_pool_thread,
                               NULL) != 0) {
                g_put_pool.shutdown = 1;
                pthread_cond_broadcast(&g_put_pool.job_cv);
                pthread_mutex_unlock(&g_put_pool.mu);
                for (uint32_t j = 0; j < i; j++)
                    pthread_join(g_put_pool.tids[j], NULL);
                g_put_pool.shutdown = 0;
                return -1;
            }
        }
        g_put_pool.nworkers = (int)n;
        g_put_pool.ready = 1;
    }
    pthread_mutex_unlock(&g_put_pool.mu);
    return 0;
}

static int put_pool_run(struct chunk_put_job *jobs, uint32_t batch)
{
    if (batch == 0)
        return 0;
    if (batch == 1 || put_pool_ensure() != 0) {
        for (uint32_t i = 0; i < batch; i++)
            chunk_put_worker(&jobs[i]);
        return 0;
    }
    pthread_mutex_lock(&g_put_pool.mu);
    g_put_pool.batch = jobs;
    g_put_pool.batch_n = batch;
    g_put_pool.next_i = 0;
    g_put_pool.completed = 0;
    pthread_cond_broadcast(&g_put_pool.job_cv);
    while (g_put_pool.completed < batch)
        pthread_cond_wait(&g_put_pool.done_cv, &g_put_pool.mu);
    g_put_pool.batch = NULL;
    g_put_pool.batch_n = 0;
    pthread_mutex_unlock(&g_put_pool.mu);
    return 0;
}

static void *chunk_put_worker(void *arg)
{
    struct chunk_put_job *job = arg;
    uint32_t chunk_size = data_chunk_size();
    uint32_t frag_len = data_frag_size();
    uint64_t chunk_start = (uint64_t)job->ci * chunk_size;
    uint64_t wr_start = (job->offset > chunk_start) ? job->offset : chunk_start;
    uint64_t wr_end = (job->end < chunk_start + chunk_size)
                          ? job->end
                          : chunk_start + chunk_size;
    int covers_full = (wr_start == chunk_start &&
                       wr_end == chunk_start + chunk_size);

    /* dd if=/dev/zero / full-chunk zeros: skip assemble/encode/malloc and PUT
     * shared zero pages with the cached zero digest. */
    if (covers_full) {
        size_t src_off = (size_t)(wr_start - job->offset);
        if (efs_bytes_are_zero(job->buf + src_off, chunk_size)) {
            const uint8_t *z = efs_zero_bytes(frag_len);
            if (!z) {
                job->rc = EFS_ERR_NOMEM;
                return NULL;
            }
            const uint8_t *cfrags[EFS_NUM_FRAGMENTS] = {z, z, z};
            efs_hash_zero_fragment_len(frag_len, job->checksums[0]);
            efs_hash_zero_fragment_len(frag_len, job->checksums[1]);
            efs_hash_zero_fragment_len(frag_len, job->checksums[2]);
            efs_place_fragments(g_client.nodes, g_client.node_count, job->ino,
                                job->ci, job->nodes);
            job->rc = efs_client_put_fragments_parallel(
                job->ino, job->ci, job->nodes, cfrags, frag_len, job->checksums);
            return NULL;
        }
    }

    /* Heap-allocate — large chunks exceed some FUSE/pthread stacks when the
     * worker runs inline on a FUSE thread (pthread_create failed / batch=1). */
    uint8_t *chunk = malloc(chunk_size);
    uint8_t *frag_buf = malloc(EFS_NUM_FRAGMENTS * frag_len);
    uint8_t *frags[EFS_NUM_FRAGMENTS];
    if (!chunk || !frag_buf) {
        free(chunk);
        free(frag_buf);
        job->rc = EFS_ERR_NOMEM;
        return NULL;
    }
    frag_ptrs(frag_buf, frag_len, frags);

    int from_zero = assemble_write_chunk(job->ino, job->old_size, job->offset,
                                         job->buf, chunk_start, wr_start, wr_end,
                                         chunk_size, chunk);

    const uint8_t *cfrags[EFS_NUM_FRAGMENTS];
    for (int i = 0; i < EFS_NUM_FRAGMENTS; i++)
        cfrags[i] = frags[i];
    efs_encode_chunk(chunk, chunk_size, chunk_size, frags);
    hash_write_fragments(cfrags, frag_len, chunk_size, from_zero, chunk_start,
                         wr_start, wr_end, job->checksums);
    efs_place_fragments(g_client.nodes, g_client.node_count, job->ino, job->ci,
                        job->nodes);
    job->rc = efs_client_put_fragments_parallel(job->ino, job->ci, job->nodes,
                                                cfrags, frag_len, job->checksums);
    free(chunk);
    free(frag_buf);
    return NULL;
}

int efs_client_write(efs_ino_t ino, uint64_t offset, size_t size, const char *buf)
{
    if (size == 0)
        return EFS_OK;

    pthread_mutex_lock(&g_client.lock);
    struct efs_inode inode;
    if (efs_export_get_inode(&g_client.export, ino, &inode) != 0) {
        pthread_mutex_unlock(&g_client.lock);
        return EFS_ERR_NOT_FOUND;
    }
    uint64_t old_size = inode.size;
    pthread_mutex_unlock(&g_client.lock);

    uint64_t end = offset + size;
    uint32_t chunk_size = data_chunk_size();
    uint32_t frag_len = data_frag_size();
    /* Only touch chunks that overlap [offset, end). Rewriting every prior
     * chunk on each append was an O(n^2) amplification of sequential writes. */
    uint32_t first_ci = (uint32_t)(offset / chunk_size);
    uint32_t last_ci = (uint32_t)((end - 1) / chunk_size);

    for (uint32_t ci = first_ci; ci <= last_ci; ci++) {
        uint64_t chunk_start = (uint64_t)ci * chunk_size;
        uint64_t wr_start = (offset > chunk_start) ? offset : chunk_start;
        uint64_t wr_end = (end < chunk_start + chunk_size) ? end
                                                              : chunk_start + chunk_size;

        uint8_t *chunk = malloc(chunk_size);
        uint8_t *frag_buf = malloc(EFS_NUM_FRAGMENTS * frag_len);
        uint8_t *frags[EFS_NUM_FRAGMENTS];
        if (!chunk || !frag_buf) {
            free(chunk);
            free(frag_buf);
            return EFS_ERR_NOMEM;
        }
        frag_ptrs(frag_buf, frag_len, frags);
        int from_zero = assemble_write_chunk(ino, old_size, offset, buf,
                                             chunk_start, wr_start, wr_end,
                                             chunk_size, chunk);

        efs_encode_chunk(chunk, chunk_size, chunk_size, frags);

        efs_node_id_t nodes[EFS_NUM_FRAGMENTS];
        efs_place_fragments(g_client.nodes, g_client.node_count, ino, ci, nodes);

        uint8_t checksums[EFS_NUM_FRAGMENTS][EFS_HASH_SIZE];
        const uint8_t *cfrags[EFS_NUM_FRAGMENTS];
        for (int i = 0; i < EFS_NUM_FRAGMENTS; i++)
            cfrags[i] = frags[i];
        hash_write_fragments(cfrags, frag_len, chunk_size, from_zero, chunk_start,
                             wr_start, wr_end, checksums);

        int rc = efs_client_put_fragments_parallel(ino, ci, nodes, cfrags, frag_len,
                                                   checksums);
        free(chunk);
        free(frag_buf);
        if (rc != EFS_OK)
            return rc;

        pthread_mutex_lock(&g_client.lock);
        efs_export_set_chunk(&g_client.export, ino, ci, nodes, checksums);
        efs_client_mark_chunk_dirty(ino, ci);
        pthread_mutex_unlock(&g_client.lock);
    }

    pthread_mutex_lock(&g_client.lock);
    /* Grow size (sets mtime) OR bump mtime alone — never both (each used to
     * run sync_hardlink_attrs + parent rollups). Skip rollups until flush. */
    struct efs_inode cur;
    if (efs_export_get_inode(&g_client.export, ino, &cur) == 0 && cur.size < end) {
        efs_export_set_size_norollup(&g_client.export, ino, end);
    } else {
        uint64_t sec;
        uint32_t nsec;
        now_ns(&sec, &nsec);
        efs_export_set_mtime_ns_norollup(&g_client.export, ino, sec, nsec);
    }
    efs_client_mark_ino_dirty(ino);
    pthread_mutex_unlock(&g_client.lock);
    efs_client_replicate_metadata();

    return EFS_OK;
}

int efs_client_write_no_replicate(efs_ino_t ino, uint64_t offset, size_t size, const char *buf)
{
    if (size == 0)
        return EFS_OK;

    pthread_mutex_lock(&g_client.lock);
    struct efs_inode inode;
    if (efs_export_get_inode(&g_client.export, ino, &inode) != 0) {
        pthread_mutex_unlock(&g_client.lock);
        return EFS_ERR_NOT_FOUND;
    }
    uint64_t old_size = inode.size;
    pthread_mutex_unlock(&g_client.lock);

    uint64_t end = offset + size;
    uint32_t chunk_size = data_chunk_size();
    uint32_t first_ci = (uint32_t)(offset / chunk_size);
    uint32_t last_ci = (uint32_t)((end - 1) / chunk_size);
    uint32_t nchunks = last_ci - first_ci + 1;

    /* Pipeline independent chunk PUTs (each already fans out 3 fragments). */
    uint32_t pipe = EFS_WRITE_PIPELINE;
    if (pipe < 1)
        pipe = 1;
    if (pipe > nchunks)
        pipe = nchunks;

    for (uint32_t base = first_ci; base <= last_ci; ) {
        uint32_t batch = last_ci - base + 1;
        if (batch > pipe)
            batch = pipe;

        struct chunk_put_job jobs[EFS_WRITE_PIPELINE];

        for (uint32_t i = 0; i < batch; i++) {
            memset(&jobs[i], 0, sizeof(jobs[i]));
            jobs[i].ino = ino;
            jobs[i].ci = base + i;
            jobs[i].old_size = old_size;
            jobs[i].offset = offset;
            jobs[i].buf = buf;
            jobs[i].end = end;
            jobs[i].rc = EFS_ERR_IO;
        }
        put_pool_run(jobs, batch);

        for (uint32_t i = 0; i < batch; i++) {
            if (jobs[i].rc != EFS_OK)
                return jobs[i].rc;
            pthread_mutex_lock(&g_client.lock);
            efs_export_set_chunk(&g_client.export, ino, jobs[i].ci, jobs[i].nodes,
                                 jobs[i].checksums);
            efs_client_mark_chunk_dirty(ino, jobs[i].ci);
            pthread_mutex_unlock(&g_client.lock);
        }
        base += batch;
    }

    pthread_mutex_lock(&g_client.lock);
    /* Grow the size based on the current value under the lock, not the stale
     * copy read before the (unlocked) write loop, so concurrent writes on
     * different FUSE worker threads cannot shrink or mis-set the size.
     * Size grow already bumps mtime — do not also call set_mtime (that was
     * a double sync_hardlink_attrs + rollup walk per write job). */
    struct efs_inode cur;
    if (efs_export_get_inode(&g_client.export, ino, &cur) == 0 && cur.size < end) {
        efs_export_set_size_norollup(&g_client.export, ino, end);
    } else {
        uint64_t sec;
        uint32_t nsec;
        now_ns(&sec, &nsec);
        efs_export_set_mtime_ns_norollup(&g_client.export, ino, sec, nsec);
    }
    efs_client_mark_ino_dirty(ino);
    pthread_mutex_unlock(&g_client.lock);

    return EFS_OK;
}
