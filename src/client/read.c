#include "client_internal.h"
#include "efs/protocol.h"
#include "efs/network.h"
#include "efs/erasure.h"
#include "efs/checksum.h"
#include "efs/placement.h"
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

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
        close(fd);
        return EFS_ERR_NET;
    }

    pthread_mutex_lock(&g_client.lock);
    efs_export_deserialize(&g_client.export, payload, payload_len);
    pthread_mutex_unlock(&g_client.lock);

    free(payload);
    close(fd);
    return EFS_OK;
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

        efs_node_id_t nodes[EFS_NUM_FRAGMENTS];
        efs_get_placement(g_client.node_count, ino, chunk_index, nodes);

        int order[EFS_NUM_FRAGMENTS] = {0, 1, 2};
        if (g_client.local_node_id != 0) {
            /* prefer local fragment first */
            for (int i = 0; i < EFS_NUM_FRAGMENTS; i++) {
                if (nodes[i] == g_client.local_node_id) {
                    int tmp = order[0];
                    order[0] = order[i];
                    order[i] = tmp;
                    break;
                }
            }
        }

        /* Fetch fragments and decode from any two that are individually
         * self-consistent (each fragment carries its own BLAKE3 checksum, so a
         * torn or corrupt fragment is rejected by efs_client_get_fragment).
         * Retry a few times to ride out a concurrent writer mid-update. */
        uint8_t chunk[EFS_CHUNK_SIZE];
        int decoded = 0;
        for (int attempt = 0; attempt < 5 && !decoded; attempt++) {
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
                } else {
                    if (a < 0)
                        a = i;
                    else if (b < 0)
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

            if (efs_decode_chunk(fragments, a, b, missing, chunk, EFS_CHUNK_SIZE) != 0)
                continue;

            decoded = 1;
        }

        if (!decoded)
            return EFS_ERR_DECODE;

        memcpy(buf + total, chunk + chunk_off, to_copy);
        total += to_copy;
        pos += to_copy;
    }

    *out_len = total;
    return EFS_OK;
}
