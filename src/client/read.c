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

struct frag_get_job {
    efs_node_id_t node;
    efs_ino_t ino;
    uint32_t chunk_index;
    int fi;
    uint32_t frag_len;
    uint8_t *out;
    uint32_t len;
    int rc;
};

static void *frag_get_thread(void *arg)
{
    struct frag_get_job *j = arg;
    uint8_t sum[EFS_HASH_SIZE];
    j->rc = efs_client_get_fragment(j->node, j->ino, j->chunk_index, j->fi,
                                    j->frag_len, j->out, &j->len, sum);
    return NULL;
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

    /* Heap-allocate: three fragments on a FUSE/main stack overflows easily and
     * SIGSEGV handlers without an alt stack cannot even log. */
    uint8_t *frag_buf = malloc(EFS_NUM_FRAGMENTS * frag_len);
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
        pthread_t tids[EFS_NUM_FRAGMENTS];
        int spawned[EFS_NUM_FRAGMENTS] = {0, 0, 0};

        /* Stage 1: the two preferred fragments in parallel. */
        for (int i = 0; i < 2; i++) {
            int fi = order[i];
            jobs[i].node = nodes[fi];
            jobs[i].ino = ino;
            jobs[i].chunk_index = chunk_index;
            jobs[i].fi = fi;
            jobs[i].frag_len = frag_len;
            jobs[i].out = frags[fi];
            jobs[i].len = 0;
            jobs[i].rc = EFS_ERR_NET;
            if (pthread_create(&tids[i], NULL, frag_get_thread, &jobs[i]) == 0)
                spawned[i] = 1;
            else
                frag_get_thread(&jobs[i]); /* no thread budget: run inline */
        }
        for (int i = 0; i < 2; i++) {
            if (spawned[i])
                pthread_join(tids[i], NULL);
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
    free(frag_buf);
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
    if (efs_client_decode_placed_chunk_attempts(EFS_META_TABLE_INO, chunk_index,
                                                page_out, EFS_META_PAGE_SIZE,
                                                EFS_META_FRAGMENT_SIZE, 2) != EFS_OK)
        return EFS_ERR_DECODE;

    uint8_t *frag_buf = malloc(EFS_NUM_FRAGMENTS * EFS_META_FRAGMENT_SIZE);
    if (!frag_buf)
        return EFS_ERR_NOMEM;
    uint8_t *frags[EFS_NUM_FRAGMENTS];
    frag_ptrs(frag_buf, EFS_META_FRAGMENT_SIZE, frags);
    efs_encode_chunk(page_out, EFS_META_PAGE_SIZE, EFS_META_PAGE_SIZE, frags);
    for (int fi = 0; fi < EFS_NUM_FRAGMENTS; fi++) {
        uint8_t sum[EFS_HASH_SIZE];
        efs_hash(frags[fi], EFS_META_FRAGMENT_SIZE, sum);
        if (memcmp(sum, efs_export_root_checksum_const(root, pi, fi),
                   EFS_HASH_SIZE) != 0) {
            free(frag_buf);
            return EFS_ERR_CHECKSUM;
        }
    }
    free(frag_buf);
    return EFS_OK;
}

static int load_export_from_root(const struct efs_export_root *root)
{
    if (root->page_count == 0 || root->blob_len == 0)
        return EFS_ERR_PROTO;

    uint8_t (*pages)[EFS_META_PAGE_SIZE] = calloc(root->page_count, EFS_META_PAGE_SIZE);
    if (!pages)
        return EFS_ERR_NOMEM;

    for (uint32_t pi = 0; pi < root->page_count; pi++) {
        uint32_t slotted = efs_meta_page_chunk_index(root->generation, pi);
        int rc = EFS_ERR_INVAL;
        const char *scheme = "slot";

        if (slotted != UINT32_MAX) {
            rc = load_page_from_chunk(root, pi, slotted, pages[pi]);
            if (rc == EFS_OK)
                continue;
        }
        /* Pre-dual-slot exports: chunk_index == page_index. */
        if (slotted != pi) {
            scheme = "legacy";
            rc = load_page_from_chunk(root, pi, pi, pages[pi]);
            if (rc == EFS_OK) {
                fprintf(stderr,
                        "meta: page %u/%u loaded via legacy chunk_index "
                        "(gen=%llu)\n",
                        pi, root->page_count,
                        (unsigned long long)root->generation);
                fflush(stderr);
                continue;
            }
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

    pthread_mutex_lock(&g_client.lock);
    rc = efs_export_deserialize(&g_client.export, blob, blob_len);
    if (rc == EFS_OK) {
        g_client.export.meta_fragmented = 1;
        if (efs_export_root_copy(&g_client.export.root, root) != EFS_OK)
            rc = EFS_ERR_NOMEM;
        else {
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

struct chunk_get_job {
    efs_ino_t ino;
    uint32_t ci;
    int have_ce;
    int rc;
    uint8_t *chunk; /* data_chunk_size() bytes, owned by caller */
};

static void *chunk_get_worker(void *arg)
{
    struct chunk_get_job *job = arg;
    uint32_t chunk_size = data_chunk_size();
    if (!job->have_ce) {
        memset(job->chunk, 0, chunk_size);
        job->rc = EFS_OK;
        return NULL;
    }
    job->rc = efs_client_decode_placed_chunk(job->ino, job->ci, job->chunk);
    return NULL;
}

int efs_client_read(efs_ino_t ino, uint64_t offset, size_t size, char *buf, size_t *out_len)
{
    if (size == 0) {
        *out_len = 0;
        return EFS_OK;
    }

    pthread_mutex_lock(&g_client.lock);
    struct efs_inode inode;
    if (efs_export_get_inode(&g_client.export, ino, &inode) != 0) {
        pthread_mutex_unlock(&g_client.lock);
        return EFS_ERR_NOT_FOUND;
    }
    uint64_t file_size = inode.size;
    pthread_mutex_unlock(&g_client.lock);

    if (offset >= file_size) {
        *out_len = 0;
        return EFS_OK;
    }
    if (offset + size > file_size)
        size = (size_t)(file_size - offset);

    size_t total = 0;
    uint64_t end = offset + size;
    uint32_t chunk_size = data_chunk_size();
    uint32_t pipe = EFS_WRITE_PIPELINE;
    if (pipe < 1)
        pipe = 1;

    for (uint64_t pos = offset; pos < end; ) {
        /* Build a batch of whole chunks covering [pos, end). */
        struct chunk_get_job jobs[EFS_WRITE_PIPELINE];
        pthread_t tids[EFS_WRITE_PIPELINE];
        int threaded[EFS_WRITE_PIPELINE];
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
            struct efs_chunk_entry ce;
            pthread_mutex_lock(&g_client.lock);
            jobs[batch].have_ce =
                (efs_export_get_chunk(&g_client.export, ino, ci, &ce) == EFS_OK);
            pthread_mutex_unlock(&g_client.lock);
            threaded[batch] = 0;
            batch_pos = ((uint64_t)ci + 1) * chunk_size;
            batch++;
        }

        for (uint32_t i = 0; i < batch; i++) {
            if (batch > 1 &&
                pthread_create(&tids[i], NULL, chunk_get_worker, &jobs[i]) == 0) {
                threaded[i] = 1;
            } else {
                chunk_get_worker(&jobs[i]);
            }
        }
        for (uint32_t i = 0; i < batch; i++) {
            if (threaded[i])
                pthread_join(tids[i], NULL);
        }

        for (uint32_t i = 0; i < batch; i++) {
            if (jobs[i].rc != EFS_OK) {
                /* Chunks [0, i) were already freed below; release [i, batch). */
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
