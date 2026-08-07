#include "client_internal.h"
#include "efs/protocol.h"
#include "efs/network.h"
#include "efs/numa_locality.h"
#include "efs/erasure.h"
#include "efs/checksum.h"
#include "efs/placement.h"
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include <pthread.h>

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

/* Replicate the tiny export root (EFSR) to all nodes; need ≥2 acks. */
static int send_meta_root(const char *buf, size_t len)
{
    int acks = 0;
    for (uint32_t i = 0; i < g_client.node_count; i++) {
        efs_node_id_t nid = g_client.nodes[i].id;
        int fd = efs_client_conn_get(nid);
        if (fd < 0)
            continue;

        uint8_t type;
        void *reply = NULL;
        uint32_t reply_len = 0;
        if (efs_send_msg(fd, EFS_MSG_PUT_META, buf, (uint32_t)len) == 0 &&
            efs_recv_msg(fd, &type, &reply, &reply_len) == 0 &&
            type == EFS_MSG_PUT_META_REPLY && reply_len >= 1) {
            uint8_t status = ((uint8_t *)reply)[0];
            if (status == EFS_PUT_META_OK)
                acks++;
            free(reply);
            efs_client_conn_release(nid, fd);
        } else {
            free(reply);
            efs_client_conn_drop(nid, fd);
        }
    }
    return (acks >= 2) ? EFS_OK : EFS_ERR_NO_QUORUM;
}

/* Pack the full export into 2+1 pages, then push the EFSR root (≥2 acks). */
int efs_client_replicate_metadata(void)
{
    char *blob = NULL;
    size_t blob_len = 0;
    uint64_t new_gen = 1;

    pthread_mutex_lock(&g_client.lock);
    if (g_client.meta_batch &&
        g_client.dirty_ino_count == 0 && g_client.dirty_chunk_count == 0 &&
        !g_client.meta_dirty) {
        g_client.meta_dirty_ops = 0;
        pthread_mutex_unlock(&g_client.lock);
        return EFS_OK;
    }
    if (efs_export_serialize(&g_client.export, &blob, &blob_len) != EFS_OK) {
        pthread_mutex_unlock(&g_client.lock);
        return EFS_ERR_NOMEM;
    }
    new_gen = g_client.export.root.generation + 1;
    if (new_gen == 0)
        new_gen = 1;
    pthread_mutex_unlock(&g_client.lock);

    if (!blob)
        return EFS_ERR_NOMEM;
    if (blob_len > (size_t)EFS_META_MAX_PAGES * EFS_CHUNK_SIZE) {
        free(blob);
        return EFS_ERR_INVAL;
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
    uint8_t *page = malloc(EFS_CHUNK_SIZE);
    uint8_t (*fragments)[EFS_FRAGMENT_SIZE] =
        malloc(EFS_NUM_FRAGMENTS * EFS_FRAGMENT_SIZE);
    if (!page || !fragments) {
        free(page);
        free(fragments);
        free(blob);
        efs_export_root_free(&root);
        return EFS_ERR_NOMEM;
    }
    for (uint32_t pi = 0; pi < root.page_count; pi++) {
        if (efs_meta_extract_page(blob, (uint32_t)blob_len, pi, page) != EFS_OK) {
            free(page);
            free(fragments);
            free(blob);
            efs_export_root_free(&root);
            return EFS_ERR_INVAL;
        }
        efs_encode_chunk(page, EFS_CHUNK_SIZE, fragments);

        uint8_t checksums[EFS_NUM_FRAGMENTS][EFS_HASH_SIZE];
        for (int fi = 0; fi < EFS_NUM_FRAGMENTS; fi++)
            efs_hash(fragments[fi], EFS_FRAGMENT_SIZE, checksums[fi]);

        efs_node_id_t nodes[EFS_NUM_FRAGMENTS];
        efs_get_placement(g_client.node_count, EFS_META_TABLE_INO, pi, nodes);

        int rc = efs_client_put_fragments_parallel(EFS_META_TABLE_INO, pi, nodes,
                                                   fragments, checksums);
        if (rc != EFS_OK) {
            free(page);
            free(fragments);
            free(blob);
            efs_export_root_free(&root);
            return rc;
        }
        for (int fi = 0; fi < EFS_NUM_FRAGMENTS; fi++)
            memcpy(efs_export_root_checksum(&root, pi, fi), checksums[fi],
                   EFS_HASH_SIZE);
    }
    free(page);
    free(fragments);
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
        dirty_sets_clear();
        g_client.meta_dirty = 0;
        g_client.meta_dirty_ops = 0;
        pthread_mutex_unlock(&g_client.lock);
    } else {
        efs_export_root_free(&root);
    }
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
                            uint32_t fragment_index, const uint8_t *data,
                            const uint8_t checksum[EFS_HASH_SIZE])
{
    if (node_id == 0 || node_id > g_client.node_count)
        return EFS_ERR_INVAL;

    int fd = efs_client_conn_get(node_id);
    if (fd < 0)
        return EFS_ERR_NET;

    /* Zero header (incl. padding) only — avoid sending uninit bytes without
     * memset'ing the 64 KiB fragment body that we overwrite next. */
    struct efs_msg_put_chunk req;
    memset(&req, 0, offsetof(struct efs_msg_put_chunk, data));
    req.export_id = g_client.export_id;
    req.ino = ino;
    req.chunk_index = chunk_index;
    req.fragment_index = fragment_index;
    memcpy(req.checksum, checksum, EFS_HASH_SIZE);
    memcpy(req.data, data, EFS_FRAGMENT_SIZE);

    uint8_t reply_type;
    void *reply = NULL;
    uint32_t reply_len = 0;
    if (efs_send_msg(fd, EFS_MSG_PUT_CHUNK, &req, sizeof(req)) != 0 ||
        efs_recv_msg(fd, &reply_type, &reply, &reply_len) != 0 ||
        reply_type != EFS_MSG_PUT_CHUNK_REPLY || reply_len != 1) {
        free(reply);
        efs_client_conn_drop(node_id, fd);
        return EFS_ERR_NET;
    }

    uint8_t status = ((uint8_t *)reply)[0];
    free(reply);
    efs_client_conn_release(node_id, fd);
    if (status == EFS_PUT_CHUNK_OK)
        return EFS_OK;
    if (status == EFS_PUT_CHUNK_QUOTA_EXCEEDED)
        return EFS_ERR_QUOTA;
    return EFS_ERR_IO;
}

int efs_client_put_fragments_parallel(efs_ino_t ino, uint32_t chunk_index,
                                      const efs_node_id_t nodes[EFS_NUM_FRAGMENTS],
                                      const uint8_t fragments[EFS_NUM_FRAGMENTS][EFS_FRAGMENT_SIZE],
                                      const uint8_t checksums[EFS_NUM_FRAGMENTS][EFS_HASH_SIZE])
{
    int fds[EFS_NUM_FRAGMENTS];
    int acks = 0;
    int quota_errors = 0;

    /* One allocation for three PUT payloads (~192 KiB) instead of three
     * mallocs, and avoid zeroing the fragment bodies. */
    struct efs_msg_put_chunk *reqs = malloc(EFS_NUM_FRAGMENTS * sizeof(*reqs));
    if (!reqs)
        return EFS_ERR_NOMEM;

    for (int i = 0; i < EFS_NUM_FRAGMENTS; i++)
        fds[i] = -1;

    for (int i = 0; i < EFS_NUM_FRAGMENTS; i++) {
        if (nodes[i] == 0 || nodes[i] > g_client.node_count)
            goto fail_net;
        fds[i] = efs_client_conn_get(nodes[i]);
        if (fds[i] < 0)
            goto fail_net;
        memset(&reqs[i], 0, offsetof(struct efs_msg_put_chunk, data));
        reqs[i].export_id = g_client.export_id;
        reqs[i].ino = ino;
        reqs[i].chunk_index = chunk_index;
        reqs[i].fragment_index = (uint32_t)i;
        memcpy(reqs[i].checksum, checksums[i], EFS_HASH_SIZE);
        memcpy(reqs[i].data, fragments[i], EFS_FRAGMENT_SIZE);
    }

    /* Send all three PUTs before waiting for any ACK (overlap RTTs). */
    int send_ok[EFS_NUM_FRAGMENTS];
    for (int i = 0; i < EFS_NUM_FRAGMENTS; i++) {
        send_ok[i] = (efs_send_msg(fds[i], EFS_MSG_PUT_CHUNK, &reqs[i],
                                   sizeof(reqs[i])) == 0);
    }

    for (int i = 0; i < EFS_NUM_FRAGMENTS; i++) {
        if (!send_ok[i]) {
            efs_client_conn_drop(nodes[i], fds[i]);
            fds[i] = -1;
            continue;
        }
        uint8_t reply_type = 0, status = 0;
        if (efs_recv_u8_reply(fds[i], &reply_type, &status) != 0 ||
            reply_type != EFS_MSG_PUT_CHUNK_REPLY) {
            efs_client_conn_drop(nodes[i], fds[i]);
            fds[i] = -1;
            continue;
        }
        if (status == EFS_PUT_CHUNK_OK)
            acks++;
        else if (status == EFS_PUT_CHUNK_QUOTA_EXCEEDED)
            quota_errors++;
        efs_client_conn_release(nodes[i], fds[i]);
        fds[i] = -1;
    }

    free(reqs);

    if (quota_errors >= 2)
        return EFS_ERR_QUOTA;
    if (acks < 2)
        return EFS_ERR_NO_QUORUM;
    return EFS_OK;

fail_net:
    for (int i = 0; i < EFS_NUM_FRAGMENTS; i++) {
        if (fds[i] >= 0)
            efs_client_conn_release(nodes[i], fds[i]);
    }
    free(reqs);
    return EFS_ERR_NET;
}

/* Assemble one chunk buffer for a write spanning [wr_start, wr_end).
 * Returns whether the chunk was built from a zero base (new or full overwrite),
 * which lets callers skip Blake3 of known-zero halves. */
static int assemble_write_chunk(efs_ino_t ino, uint64_t old_size,
                                uint64_t offset, const char *buf,
                                uint64_t chunk_start, uint64_t wr_start,
                                uint64_t wr_end, uint8_t chunk[EFS_CHUNK_SIZE])
{
    int covers_full = (wr_start == chunk_start &&
                       wr_end == chunk_start + EFS_CHUNK_SIZE);
    size_t off_in_chunk = (size_t)(wr_start - chunk_start);
    size_t wr_len = (size_t)(wr_end - wr_start);
    size_t src_off = (size_t)(wr_start - offset);
    int from_zero;

    if (covers_full) {
        /* Full overwrite: copy user bytes only — no memset+memcpy. */
        memcpy(chunk, buf + src_off, EFS_CHUNK_SIZE);
        /* dd if=/dev/zero: mark as from_zero so hash path can use the
         * cached zero-fragment digest (encode of zeros is zeros). */
        const uint64_t *q = (const uint64_t *)(const void *)chunk;
        size_t nq = EFS_CHUNK_SIZE / sizeof(uint64_t);
        for (size_t i = 0; i < nq; i++) {
            if (q[i] != 0)
                return 0;
        }
        return 1;
    }

    if (chunk_start >= old_size) {
        from_zero = 1;
        /* Zero only the unwritten regions. */
        if (off_in_chunk > 0)
            memset(chunk, 0, off_in_chunk);
        if (off_in_chunk + wr_len < EFS_CHUNK_SIZE)
            memset(chunk + off_in_chunk + wr_len, 0,
                   EFS_CHUNK_SIZE - off_in_chunk - wr_len);
    } else {
        from_zero = 0;
        size_t existing = (size_t)(old_size - chunk_start);
        if (existing > EFS_CHUNK_SIZE)
            existing = EFS_CHUNK_SIZE;
        size_t got = 0;
        efs_client_read(ino, chunk_start, existing, (char *)chunk, &got);
        if (got < EFS_CHUNK_SIZE)
            memset(chunk + got, 0, EFS_CHUNK_SIZE - got);
    }

    memcpy(chunk + off_in_chunk, buf + src_off, wr_len);
    return from_zero;
}

/* Hash fragments; when from_zero and a half was not written, reuse the cached
 * zero-fragment digest and skip hashing the identical parity copy. */
static void hash_write_fragments(const uint8_t fragments[EFS_NUM_FRAGMENTS][EFS_FRAGMENT_SIZE],
                                 int from_zero, uint64_t chunk_start,
                                 uint64_t wr_start, uint64_t wr_end,
                                 uint8_t checksums[EFS_NUM_FRAGMENTS][EFS_HASH_SIZE])
{
    int frag0_zero = 0, frag1_zero = 0;
    if (from_zero) {
        frag0_zero = (wr_start >= chunk_start + EFS_FRAGMENT_SIZE);
        frag1_zero = (wr_end <= chunk_start + EFS_FRAGMENT_SIZE);
    }

    /* Full-chunk zero write: both data halves and parity are zero. */
    if (from_zero && wr_start <= chunk_start &&
        wr_end >= chunk_start + EFS_CHUNK_SIZE) {
        efs_hash_zero_fragment(checksums[0]);
        efs_hash_zero_fragment(checksums[1]);
        efs_hash_zero_fragment(checksums[2]);
        return;
    }

    if (frag0_zero)
        efs_hash_zero_fragment(checksums[0]);
    else
        efs_hash(fragments[0], EFS_FRAGMENT_SIZE, checksums[0]);

    if (frag1_zero)
        efs_hash_zero_fragment(checksums[1]);
    else
        efs_hash(fragments[1], EFS_FRAGMENT_SIZE, checksums[1]);

    if (frag0_zero && !frag1_zero)
        memcpy(checksums[2], checksums[1], EFS_HASH_SIZE);
    else if (frag1_zero && !frag0_zero)
        memcpy(checksums[2], checksums[0], EFS_HASH_SIZE);
    else if (frag0_zero && frag1_zero)
        efs_hash_zero_fragment(checksums[2]);
    else
        efs_hash(fragments[2], EFS_FRAGMENT_SIZE, checksums[2]);
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

static void *chunk_put_worker(void *arg)
{
    struct chunk_put_job *job = arg;
    if (g_client.net_affinity_valid)
        efs_numa_apply_affinity(&g_client.net_cpu_set);
    uint64_t chunk_start = (uint64_t)job->ci * EFS_CHUNK_SIZE;
    uint64_t wr_start = (job->offset > chunk_start) ? job->offset : chunk_start;
    uint64_t wr_end = (job->end < chunk_start + EFS_CHUNK_SIZE)
                          ? job->end
                          : chunk_start + EFS_CHUNK_SIZE;

    uint8_t chunk[EFS_CHUNK_SIZE];
    int from_zero = assemble_write_chunk(job->ino, job->old_size, job->offset,
                                         job->buf, chunk_start, wr_start, wr_end,
                                         chunk);

    uint8_t fragments[EFS_NUM_FRAGMENTS][EFS_FRAGMENT_SIZE];
    int full_zero = from_zero && wr_start <= chunk_start &&
                    wr_end >= chunk_start + EFS_CHUNK_SIZE;
    if (full_zero) {
        memset(fragments, 0, sizeof(fragments));
        efs_hash_zero_fragment(job->checksums[0]);
        efs_hash_zero_fragment(job->checksums[1]);
        efs_hash_zero_fragment(job->checksums[2]);
    } else {
        efs_encode_chunk(chunk, EFS_CHUNK_SIZE, fragments);
        hash_write_fragments(fragments, from_zero, chunk_start, wr_start, wr_end,
                             job->checksums);
    }
    efs_get_placement(g_client.node_count, job->ino, job->ci, job->nodes);
    job->rc = efs_client_put_fragments_parallel(job->ino, job->ci, job->nodes,
                                                fragments, job->checksums);
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
    /* Only touch chunks that overlap [offset, end). Rewriting every prior
     * chunk on each append was an O(n^2) amplification of sequential writes. */
    uint32_t first_ci = (uint32_t)(offset / EFS_CHUNK_SIZE);
    uint32_t last_ci = (uint32_t)((end - 1) / EFS_CHUNK_SIZE);

    for (uint32_t ci = first_ci; ci <= last_ci; ci++) {
        uint64_t chunk_start = (uint64_t)ci * EFS_CHUNK_SIZE;
        uint64_t wr_start = (offset > chunk_start) ? offset : chunk_start;
        uint64_t wr_end = (end < chunk_start + EFS_CHUNK_SIZE) ? end
                                                              : chunk_start + EFS_CHUNK_SIZE;

        uint8_t chunk[EFS_CHUNK_SIZE];
        int from_zero = assemble_write_chunk(ino, old_size, offset, buf,
                                             chunk_start, wr_start, wr_end, chunk);

        uint8_t fragments[EFS_NUM_FRAGMENTS][EFS_FRAGMENT_SIZE];
        efs_encode_chunk(chunk, EFS_CHUNK_SIZE, fragments);

        efs_node_id_t nodes[EFS_NUM_FRAGMENTS];
        efs_get_placement(g_client.node_count, ino, ci, nodes);

        uint8_t checksums[EFS_NUM_FRAGMENTS][EFS_HASH_SIZE];
        hash_write_fragments(fragments, from_zero, chunk_start, wr_start, wr_end,
                             checksums);

        int rc = efs_client_put_fragments_parallel(ino, ci, nodes, fragments, checksums);
        if (rc != EFS_OK)
            return rc;

        pthread_mutex_lock(&g_client.lock);
        efs_export_set_chunk(&g_client.export, ino, ci, nodes, checksums);
        efs_client_mark_chunk_dirty(ino, ci);
        pthread_mutex_unlock(&g_client.lock);
    }

    pthread_mutex_lock(&g_client.lock);
    struct efs_inode cur;
    if (efs_export_get_inode(&g_client.export, ino, &cur) == 0 && cur.size < end)
        efs_export_set_size(&g_client.export, ino, end);
    {
        uint64_t sec;
        uint32_t nsec;
        now_ns(&sec, &nsec);
        efs_export_set_mtime_ns(&g_client.export, ino, sec, nsec);
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
    uint32_t first_ci = (uint32_t)(offset / EFS_CHUNK_SIZE);
    uint32_t last_ci = (uint32_t)((end - 1) / EFS_CHUNK_SIZE);
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
        pthread_t tids[EFS_WRITE_PIPELINE];
        int threaded[EFS_WRITE_PIPELINE];

        for (uint32_t i = 0; i < batch; i++) {
            memset(&jobs[i], 0, sizeof(jobs[i]));
            jobs[i].ino = ino;
            jobs[i].ci = base + i;
            jobs[i].old_size = old_size;
            jobs[i].offset = offset;
            jobs[i].buf = buf;
            jobs[i].end = end;
            jobs[i].rc = EFS_ERR_IO;
            threaded[i] = 0;
            if (batch > 1 &&
                pthread_create(&tids[i], NULL, chunk_put_worker, &jobs[i]) == 0) {
                threaded[i] = 1;
            } else {
                chunk_put_worker(&jobs[i]);
            }
        }
        for (uint32_t i = 0; i < batch; i++) {
            if (threaded[i])
                pthread_join(tids[i], NULL);
        }

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
     * different FUSE worker threads cannot shrink or mis-set the size. */
    struct efs_inode cur;
    if (efs_export_get_inode(&g_client.export, ino, &cur) == 0 && cur.size < end)
        efs_export_set_size(&g_client.export, ino, end);
    {
        uint64_t sec;
        uint32_t nsec;
        now_ns(&sec, &nsec);
        efs_export_set_mtime_ns(&g_client.export, ino, sec, nsec);
    }
    efs_client_mark_ino_dirty(ino);
    pthread_mutex_unlock(&g_client.lock);

    return EFS_OK;
}
