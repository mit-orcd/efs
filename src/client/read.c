#include "client_internal.h"
#include "efs/protocol.h"
#include "efs/network.h"
#include "efs/erasure.h"
#include "efs/checksum.h"
#include "efs/placement.h"
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

/* Reconstruct one logical chunk from any 2 of 3 fragments. */
static int efs_client_decode_placed_chunk(efs_ino_t ino, uint32_t chunk_index,
                                          uint8_t chunk_out[EFS_CHUNK_SIZE])
{
    efs_node_id_t nodes[EFS_NUM_FRAGMENTS];
    efs_get_placement(g_client.node_count, ino, chunk_index, nodes);

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

    for (int attempt = 0; attempt < 5; attempt++) {
        uint8_t fragments[EFS_NUM_FRAGMENTS][EFS_FRAGMENT_SIZE];
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
        if (efs_decode_chunk(fragments, a, b, missing, chunk_out, EFS_CHUNK_SIZE) == 0)
            return EFS_OK;
    }
    return EFS_ERR_DECODE;
}

static int load_export_from_root(const struct efs_export_root *root)
{
    if (root->page_count == 0 || root->blob_len == 0)
        return EFS_ERR_PROTO;

    uint8_t (*pages)[EFS_CHUNK_SIZE] = calloc(root->page_count, EFS_CHUNK_SIZE);
    if (!pages)
        return EFS_ERR_NOMEM;

    for (uint32_t pi = 0; pi < root->page_count; pi++) {
        if (efs_client_decode_placed_chunk(EFS_META_TABLE_INO, pi, pages[pi]) != EFS_OK) {
            free(pages);
            return EFS_ERR_DECODE;
        }
        /* Verify fragment checksums against the root page map (D1). */
        uint8_t fragments[EFS_NUM_FRAGMENTS][EFS_FRAGMENT_SIZE];
        efs_encode_chunk(pages[pi], EFS_CHUNK_SIZE, fragments);
        for (int fi = 0; fi < EFS_NUM_FRAGMENTS; fi++) {
            uint8_t sum[EFS_HASH_SIZE];
            efs_hash(fragments[fi], EFS_FRAGMENT_SIZE, sum);
            if (memcmp(sum, efs_export_root_checksum_const(root, pi, fi),
                       EFS_HASH_SIZE) != 0) {
                free(pages);
                return EFS_ERR_CHECKSUM;
            }
        }
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

int efs_client_fetch_metadata(const char *host, uint16_t port)
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
        efs_recv_msg(fd, &type, &payload, &payload_len) != 0 ||
        type != EFS_MSG_GET_META_REPLY || payload_len == 0) {
        free(payload);
        close(fd);
        return EFS_ERR_NET;
    }
    close(fd);

    int rc;
    if (efs_meta_blob_is_root(payload, payload_len)) {
        struct efs_export_root root;
        memset(&root, 0, sizeof(root));
        rc = efs_export_root_deserialize(&root, payload, payload_len);
        free(payload);
        if (rc != EFS_OK)
            return rc;
        /* Need cluster membership + conn pool before page fetches. */
        rc = load_export_from_root(&root);
        efs_export_root_free(&root);
        return rc;
    }

    if (!efs_meta_blob_is_export(payload, payload_len)) {
        free(payload);
        return EFS_ERR_PROTO;
    }

    pthread_mutex_lock(&g_client.lock);
    rc = efs_export_deserialize(&g_client.export, payload, payload_len);
    g_client.export.meta_fragmented = 0;
    pthread_mutex_unlock(&g_client.lock);
    free(payload);
    return rc;
}

int efs_client_get_fragment(efs_node_id_t node_id, efs_ino_t ino, uint32_t chunk_index,
                            uint32_t fragment_index, uint8_t *data, uint32_t *data_len,
                            uint8_t checksum[EFS_HASH_SIZE])
{
    if (node_id == 0 || node_id > g_client.node_count)
        return EFS_ERR_INVAL;

    int fd = efs_client_conn_get(node_id);
    if (fd < 0)
        return EFS_ERR_NET;

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
        return EFS_ERR_NET;
    }

    uint8_t *r = reply;
    int status = r[0];
    if (status != EFS_GET_CHUNK_OK || reply_len != 1 + EFS_HASH_SIZE + EFS_FRAGMENT_SIZE) {
        free(reply);
        efs_client_conn_release(node_id, fd);
        return EFS_ERR_NOT_FOUND;
    }

    memcpy(checksum, r + 1, EFS_HASH_SIZE);
    memcpy(data, r + 1 + EFS_HASH_SIZE, EFS_FRAGMENT_SIZE);
    *data_len = EFS_FRAGMENT_SIZE;

    uint8_t verify[EFS_HASH_SIZE];
    efs_hash(data, EFS_FRAGMENT_SIZE, verify);
    if (memcmp(verify, checksum, EFS_HASH_SIZE) != 0) {
        free(reply);
        efs_client_conn_release(node_id, fd);
        return EFS_ERR_CHECKSUM;
    }

    free(reply);
    efs_client_conn_release(node_id, fd);
    return EFS_OK;
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

    for (uint64_t pos = offset; pos < end; ) {
        uint32_t chunk_index = (uint32_t)(pos / EFS_CHUNK_SIZE);
        uint32_t chunk_off = (uint32_t)(pos % EFS_CHUNK_SIZE);
        size_t to_copy = end - pos;
        if (to_copy > EFS_CHUNK_SIZE - chunk_off)
            to_copy = EFS_CHUNK_SIZE - chunk_off;

        /* Fetch fragments and decode from any two that are individually
         * self-consistent (each fragment carries its own BLAKE3 checksum). */
        uint8_t chunk[EFS_CHUNK_SIZE];
        if (efs_client_decode_placed_chunk(ino, chunk_index, chunk) != EFS_OK)
            return EFS_ERR_DECODE;

        memcpy(buf + total, chunk + chunk_off, to_copy);
        total += to_copy;
        pos += to_copy;
    }

    *out_len = total;
    return EFS_OK;
}
