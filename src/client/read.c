#include "client_internal.h"
#include "efs/protocol.h"
#include "efs/network.h"
#include "efs/erasure.h"
#include "efs/checksum.h"
#include "efs/placement.h"
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>

/* Reconstruct one logical chunk from any 2 of 3 fragments. */
static int efs_client_decode_placed_chunk_attempts(efs_ino_t ino, uint32_t chunk_index,
                                                   uint8_t chunk_out[EFS_CHUNK_SIZE],
                                                   int max_attempts)
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

    /* Heap-allocate: 3 × 64 KiB on a FUSE/main stack overflows easily and
     * SIGSEGV handlers without an alt stack cannot even log. */
    uint8_t (*fragments)[EFS_FRAGMENT_SIZE] =
        malloc(EFS_NUM_FRAGMENTS * EFS_FRAGMENT_SIZE);
    if (!fragments)
        return EFS_ERR_NOMEM;

    int rc = EFS_ERR_DECODE;
    for (int attempt = 0; attempt < max_attempts; attempt++) {
        int have[EFS_NUM_FRAGMENTS] = {0, 0, 0};
        uint8_t fsum[EFS_NUM_FRAGMENTS][EFS_HASH_SIZE];

        for (int i = 0; i < EFS_NUM_FRAGMENTS; i++) {
            int fi = order[i];
            uint32_t len = 0;
            if (efs_client_get_fragment(nodes[fi], ino, chunk_index, fi,
                                        fragments[fi], &len, fsum[fi]) == 0)
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
        if (a < 0 || b < 0 || missing < 0)
            continue;
        if (efs_decode_chunk(fragments, a, b, missing, chunk_out, EFS_CHUNK_SIZE) == 0) {
            rc = EFS_OK;
            break;
        }
    }
    free(fragments);
    return rc;
}

static int efs_client_decode_placed_chunk(efs_ino_t ino, uint32_t chunk_index,
                                          uint8_t chunk_out[EFS_CHUNK_SIZE])
{
    return efs_client_decode_placed_chunk_attempts(ino, chunk_index, chunk_out, 5);
}

static int load_export_from_root(const struct efs_export_root *root)
{
    if (root->page_count == 0 || root->blob_len == 0)
        return EFS_ERR_PROTO;

    uint8_t (*pages)[EFS_CHUNK_SIZE] = calloc(root->page_count, EFS_CHUNK_SIZE);
    if (!pages)
        return EFS_ERR_NOMEM;

    for (uint32_t pi = 0; pi < root->page_count; pi++) {
        /* Meta mount path: fail fast so we can fall back to legacy EFSM. */
        if (efs_client_decode_placed_chunk_attempts(EFS_META_TABLE_INO, pi,
                                                    pages[pi], 2) != EFS_OK) {
            free(pages);
            return EFS_ERR_DECODE;
        }
        /* Verify fragment checksums against the root page map (D1). */
        uint8_t (*fragments)[EFS_FRAGMENT_SIZE] =
            malloc(EFS_NUM_FRAGMENTS * EFS_FRAGMENT_SIZE);
        if (!fragments) {
            free(pages);
            return EFS_ERR_NOMEM;
        }
        efs_encode_chunk(pages[pi], EFS_CHUNK_SIZE, fragments);
        for (int fi = 0; fi < EFS_NUM_FRAGMENTS; fi++) {
            uint8_t sum[EFS_HASH_SIZE];
            efs_hash(fragments[fi], EFS_FRAGMENT_SIZE, sum);
            if (memcmp(sum, efs_export_root_checksum_const(root, pi, fi),
                       EFS_HASH_SIZE) != 0) {
                free(fragments);
                free(pages);
                return EFS_ERR_CHECKSUM;
            }
        }
        free(fragments);
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
        g_client.export.next_ino = root->next_ino;
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
                            uint32_t fragment_index, uint8_t *data, uint32_t *data_len,
                            uint8_t checksum[EFS_HASH_SIZE])
{
    if (node_id == 0)
        return EFS_ERR_INVAL;

    for (int attempt = 1; attempt <= 3; attempt++) {
        int fd = efs_client_conn_get(node_id);
        if (fd < 0) {
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

        uint8_t reply_type;
        void *reply = NULL;
        uint32_t reply_len = 0;
        if (efs_send_msg(fd, EFS_MSG_GET_CHUNK, &req, sizeof(req)) != 0 ||
            efs_recv_msg(fd, &reply_type, &reply, &reply_len) != 0 ||
            reply_type != EFS_MSG_GET_CHUNK_REPLY || reply_len < 1) {
            free(reply);
            efs_client_conn_drop(node_id, fd);
            if (attempt < 3) {
                usleep(50000u * (unsigned)attempt);
                continue;
            }
            return EFS_ERR_NET;
        }

        uint8_t *r = reply;
        int status = r[0];
        if (status != EFS_GET_CHUNK_OK ||
            reply_len != 1 + EFS_HASH_SIZE + EFS_FRAGMENT_SIZE) {
            free(reply);
            efs_client_conn_release(node_id, fd);
            return EFS_ERR_NOT_FOUND;
        }

        memcpy(checksum, r + 1, EFS_HASH_SIZE);
        memcpy(data, r + 1 + EFS_HASH_SIZE, EFS_FRAGMENT_SIZE);
        *data_len = EFS_FRAGMENT_SIZE;

        /* Verify payload. Known-zero digests use a memcmp against zeros
         * (cheap) instead of Blake3 — dominates single-stream dd reads. */
        uint8_t zero_ck[EFS_HASH_SIZE];
        efs_hash_zero_fragment(zero_ck);
        int sum_ok;
        if (memcmp(checksum, zero_ck, EFS_HASH_SIZE) == 0) {
            sum_ok = efs_bytes_are_zero(data, EFS_FRAGMENT_SIZE);
        } else {
            uint8_t verify[EFS_HASH_SIZE];
            efs_hash(data, EFS_FRAGMENT_SIZE, verify);
            sum_ok = (memcmp(verify, checksum, EFS_HASH_SIZE) == 0);
        }
        if (!sum_ok) {
            free(reply);
            efs_client_conn_release(node_id, fd);
            return EFS_ERR_CHECKSUM;
        }

        free(reply);
        efs_client_conn_release(node_id, fd);
        return EFS_OK;
    }
    return EFS_ERR_NET;
}

struct chunk_get_job {
    efs_ino_t ino;
    uint32_t ci;
    int have_ce;
    int rc;
    uint8_t *chunk; /* EFS_CHUNK_SIZE, owned by caller */
};

static void *chunk_get_worker(void *arg)
{
    struct chunk_get_job *job = arg;
    if (!job->have_ce) {
        memset(job->chunk, 0, EFS_CHUNK_SIZE);
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
            uint32_t ci = (uint32_t)(batch_pos / EFS_CHUNK_SIZE);
            memset(&jobs[batch], 0, sizeof(jobs[batch]));
            jobs[batch].ino = ino;
            jobs[batch].ci = ci;
            jobs[batch].chunk = malloc(EFS_CHUNK_SIZE);
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
            batch_pos = ((uint64_t)ci + 1) * EFS_CHUNK_SIZE;
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
                for (uint32_t j = 0; j < batch; j++)
                    free(jobs[j].chunk);
                return jobs[i].rc;
            }
            uint64_t chunk_start = (uint64_t)jobs[i].ci * EFS_CHUNK_SIZE;
            uint32_t chunk_off = (pos > chunk_start) ? (uint32_t)(pos - chunk_start) : 0;
            size_t to_copy = end - pos;
            if (to_copy > EFS_CHUNK_SIZE - chunk_off)
                to_copy = EFS_CHUNK_SIZE - chunk_off;
            memcpy(buf + total, jobs[i].chunk + chunk_off, to_copy);
            total += to_copy;
            pos += to_copy;
            free(jobs[i].chunk);
        }
    }

    *out_len = total;
    return EFS_OK;
}
