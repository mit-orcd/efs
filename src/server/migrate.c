#include "efs/common.h"
#include "efs/protocol.h"
#include "efs/network.h"
#include "efs/erasure.h"
#include "efs/checksum.h"
#include "server_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <sys/socket.h>

static int server_get_fragment_from_peer(const char *host, uint16_t port,
                                         efs_export_id_t export_id, efs_ino_t ino,
                                         uint32_t chunk_index, uint32_t fragment_index,
                                         uint8_t *data, uint8_t *checksum)
{
    int fd = efs_connect_tcp(host, port);
    if (fd < 0)
        return EFS_ERR_NET;

    efs_set_recv_timeout(fd, EFS_IO_TIMEOUT_MS);
    efs_set_send_timeout(fd, EFS_IO_TIMEOUT_MS);

    struct efs_msg_get_chunk req;
    memset(&req, 0, sizeof(req));
    req.export_id = export_id;
    req.ino = ino;
    req.chunk_index = chunk_index;
    req.fragment_index = fragment_index;

    uint8_t type;
    void *reply = NULL;
    uint32_t reply_len = 0;
    int rc = EFS_ERR_NET;

    if (efs_send_msg(fd, EFS_MSG_GET_CHUNK, &req, sizeof(req)) == 0 &&
        efs_recv_msg(fd, &type, &reply, &reply_len) == 0 &&
        type == EFS_MSG_GET_CHUNK_REPLY && reply_len >= 1) {
        uint8_t *r = reply;
        if (r[0] == EFS_GET_CHUNK_OK && reply_len == 1 + EFS_HASH_SIZE + EFS_FRAGMENT_SIZE) {
            memcpy(checksum, r + 1, EFS_HASH_SIZE);
            memcpy(data, r + 1 + EFS_HASH_SIZE, EFS_FRAGMENT_SIZE);
            rc = EFS_OK;
        } else if (r[0] == EFS_GET_CHUNK_NOT_FOUND) {
            rc = EFS_ERR_NOT_FOUND;
        } else {
            rc = EFS_ERR_IO;
        }
    }

    free(reply);
    close(fd);
    return rc;
}

static int server_put_fragment_to_peer(const char *host, uint16_t port,
                                       efs_export_id_t export_id, efs_ino_t ino,
                                       uint32_t chunk_index, uint32_t fragment_index,
                                       const uint8_t *data, const uint8_t *checksum)
{
    int fd = efs_connect_tcp(host, port);
    if (fd < 0)
        return EFS_ERR_NET;

    efs_set_recv_timeout(fd, EFS_IO_TIMEOUT_MS);
    efs_set_send_timeout(fd, EFS_IO_TIMEOUT_MS);

    struct efs_msg_put_chunk req;
    memset(&req, 0, sizeof(req));
    req.export_id = export_id;
    req.ino = ino;
    req.chunk_index = chunk_index;
    req.fragment_index = fragment_index;
    memcpy(req.checksum, checksum, EFS_HASH_SIZE);
    memcpy(req.data, data, EFS_FRAGMENT_SIZE);

    uint8_t type;
    void *reply = NULL;
    uint32_t reply_len = 0;
    int rc = EFS_ERR_NET;

    if (efs_send_msg(fd, EFS_MSG_PUT_CHUNK, &req, sizeof(req)) == 0 &&
        efs_recv_msg(fd, &type, &reply, &reply_len) == 0 &&
        type == EFS_MSG_PUT_CHUNK_REPLY && reply_len == 1) {
        uint8_t *r = reply;
        if (r[0] == EFS_PUT_CHUNK_OK)
            rc = EFS_OK;
        else if (r[0] == EFS_PUT_CHUNK_QUOTA_EXCEEDED)
            rc = EFS_ERR_QUOTA;
        else
            rc = EFS_ERR_IO;
    }

    free(reply);
    close(fd);
    return rc;
}

static int pick_target_node(struct efsd_server *s, struct efs_export *ex,
                            struct efs_chunk_entry *chunk, int fragment_index,
                            efs_node_id_t *target_id)
{
    pthread_mutex_lock(&s->lock);
    uint32_t node_count = s->node_count;
    struct efs_node nodes[EFS_MAX_NODES];
    memcpy(nodes, s->nodes, sizeof(nodes));

    /* Compute fragment storage used by each node from the current metadata. */
    uint64_t node_used[EFS_MAX_NODES] = {0};
    for (uint64_t ci = 0; ci < ex->chunk_count; ci++) {
        for (int j = 0; j < EFS_NUM_FRAGMENTS; j++) {
            efs_node_id_t id = ex->chunks[ci].fragment_nodes[j];
            for (uint32_t k = 0; k < node_count; k++) {
                if (nodes[k].id == id) {
                    node_used[k] += EFS_FRAGMENT_SIZE;
                    break;
                }
            }
        }
    }
    pthread_mutex_unlock(&s->lock);

    efs_node_id_t preferred = 0;
    efs_node_id_t fallback = 0;

    for (uint32_t i = 0; i < node_count; i++) {
        if (nodes[i].id == s->id)
            continue;

        bool has_other_fragment = false;
        for (int j = 0; j < EFS_NUM_FRAGMENTS; j++) {
            if (j != fragment_index && chunk->fragment_nodes[j] == nodes[i].id) {
                has_other_fragment = true;
                break;
            }
        }

        uint64_t free = (nodes[i].quota > 0) ? (nodes[i].quota - node_used[i]) : UINT64_MAX;
        if (free < EFS_FRAGMENT_SIZE)
            continue;

        if (!has_other_fragment && preferred == 0) {
            preferred = nodes[i].id;
        } else if (fallback == 0) {
            fallback = nodes[i].id;
        }
    }

    *target_id = (preferred != 0) ? preferred : fallback;
    return (*target_id != 0) ? EFS_OK : EFS_ERR_NOT_FOUND;
}

static int migrate_one_fragment(struct efsd_server *s, struct efs_export *ex,
                                struct efs_chunk_entry *chunk, int fragment_index)
{
    efs_node_id_t target_id;
    int rc = pick_target_node(s, ex, chunk, fragment_index, &target_id);
    if (rc != 0)
        return rc;

    pthread_mutex_lock(&s->lock);
    uint32_t node_count = s->node_count;
    struct efs_node nodes[EFS_MAX_NODES];
    memcpy(nodes, s->nodes, sizeof(nodes));
    pthread_mutex_unlock(&s->lock);

    /* Find the other two fragment indices. */
    int other[2];
    int idx = 0;
    for (int i = 0; i < EFS_NUM_FRAGMENTS; i++) {
        if (i != fragment_index)
            other[idx++] = i;
    }

    /* Read the other two fragments from their current nodes. */
    uint8_t fragments[EFS_NUM_FRAGMENTS][EFS_FRAGMENT_SIZE];
    uint8_t checksums[EFS_NUM_FRAGMENTS][EFS_HASH_SIZE];

    for (int i = 0; i < 2; i++) {
        int oi = other[i];
        efs_node_id_t node_id = chunk->fragment_nodes[oi];
        struct efs_node *n = NULL;
        for (uint32_t j = 0; j < node_count; j++) {
            if (nodes[j].id == node_id) {
                n = &nodes[j];
                break;
            }
        }
        if (!n)
            return EFS_ERR_NET;
        rc = server_get_fragment_from_peer(n->addr, n->port, ex->id,
                                           chunk->ino, chunk->chunk_index, oi,
                                           fragments[oi], checksums[oi]);
        if (rc != 0)
            return rc;
    }

    /* Reconstruct the full chunk and extract the missing fragment. */
    uint8_t reconstructed_chunk[EFS_CHUNK_SIZE];
    efs_decode_chunk(fragments, other[0], other[1], fragment_index, reconstructed_chunk, EFS_CHUNK_SIZE);
    if (fragment_index == 0) {
        memcpy(fragments[fragment_index], reconstructed_chunk, EFS_FRAGMENT_SIZE);
    } else if (fragment_index == 1) {
        memcpy(fragments[fragment_index], reconstructed_chunk + EFS_FRAGMENT_SIZE, EFS_FRAGMENT_SIZE);
    } else {
        for (size_t i = 0; i < EFS_FRAGMENT_SIZE; i++)
            fragments[fragment_index][i] = reconstructed_chunk[i] ^ reconstructed_chunk[EFS_FRAGMENT_SIZE + i];
    }

    /* Verify against the stored checksum. */
    uint8_t verify_hash[EFS_HASH_SIZE];
    efs_hash(fragments[fragment_index], EFS_FRAGMENT_SIZE, verify_hash);
    if (memcmp(verify_hash, chunk->checksums[fragment_index], EFS_HASH_SIZE) != 0)
        return EFS_ERR_CHECKSUM;

    /* Write the reconstructed fragment to the target node. */
    struct efs_node *target = NULL;
    for (uint32_t i = 0; i < node_count; i++) {
        if (nodes[i].id == target_id) {
            target = &nodes[i];
            break;
        }
    }
    if (!target)
        return EFS_ERR_NET;

    rc = server_put_fragment_to_peer(target->addr, target->port, ex->id,
                                     chunk->ino, chunk->chunk_index, fragment_index,
                                     fragments[fragment_index],
                                     chunk->checksums[fragment_index]);
    if (rc != 0)
        return rc;

    /* Update metadata: this fragment now lives on the target node. */
    efs_node_id_t new_nodes[EFS_NUM_FRAGMENTS];
    uint8_t new_checksums[EFS_NUM_FRAGMENTS][EFS_HASH_SIZE];
    for (int i = 0; i < EFS_NUM_FRAGMENTS; i++) {
        new_nodes[i] = chunk->fragment_nodes[i];
        memcpy(new_checksums[i], chunk->checksums[i], EFS_HASH_SIZE);
    }
    new_nodes[fragment_index] = target_id;

    pthread_mutex_lock(&s->lock);
    efs_export_set_chunk(ex, chunk->ino, chunk->chunk_index, new_nodes, new_checksums);
    s->export_meta_dirty = 1;
    pthread_mutex_unlock(&s->lock);

    /* Delete the local copy of the fragment and its checksum sidecar
     * (sharded path and legacy flat-{ino} path). */
    server_unlink_fragment_files(s, ex, chunk->ino, chunk->chunk_index,
                                 fragment_index);

    pthread_mutex_lock(&s->lock);
    struct efs_node *local = server_local_node(s);
    if (local) {
        if (local->used >= EFS_FRAGMENT_SIZE)
            local->used -= EFS_FRAGMENT_SIZE;
        else
            local->used = 0;
    }
    pthread_mutex_unlock(&s->lock);
    server_usage_save(s);

    return EFS_OK;
}

static bool shrink_target_reached(struct efsd_server *s)
{
    bool reached;
    pthread_mutex_lock(&s->lock);
    struct efs_node *local = server_local_node(s);
    reached = local ? (local->used <= s->shrink_target) : true;
    pthread_mutex_unlock(&s->lock);
    return reached;
}

static void *migration_thread(void *arg)
{
    struct efsd_server *s = arg;

    while (s->running) {
        pthread_mutex_lock(&s->lock);
        int state = s->state;
        struct efs_export *ex = (s->export_count > 0) ? &s->exports[0] : NULL;
        pthread_mutex_unlock(&s->lock);

        if (state == SERVER_STATE_ACTIVE || state == SERVER_STATE_DRAINED ||
            state == SERVER_STATE_LEAVING || ex == NULL) {
            usleep(1000 * 1000);
            continue;
        }

        /* Chunk placement lives in bulk meta pages; rebuild before planning. */
        pthread_mutex_lock(&s->lock);
        int need_rebuild = (ex->meta_fragmented && ex->meta_needs_rebuild);
        pthread_mutex_unlock(&s->lock);
        if (need_rebuild)
            server_rebuild_export_from_pages(s, ex);

        server_update_local_usage(s);

        bool migrated_any = false;
        bool done = true;

        for (uint64_t ci = 0; ci < ex->chunk_count && s->running; ci++) {
            struct efs_chunk_entry *chunk = &ex->chunks[ci];

            int fragment_index = -1;
            for (int i = 0; i < EFS_NUM_FRAGMENTS; i++) {
                if (chunk->fragment_nodes[i] == s->id) {
                    fragment_index = i;
                    break;
                }
            }
            if (fragment_index < 0)
                continue;

            if (state == SERVER_STATE_SHRINKING && shrink_target_reached(s))
                break;

            int rc = migrate_one_fragment(s, ex, chunk, fragment_index);
            if (rc == 0) {
                migrated_any = true;
                done = false;
                usleep(10000); /* brief pause between migrations */
            } else if (rc == EFS_ERR_NOT_FOUND) {
                /* No other node has room for this fragment. */
                continue;
            } else {
                /* Network or checksum error; retry later. */
                usleep(1000 * 1000);
                done = false;
                break;
            }
        }

        /* Flush chunk-map pages once per migration round (not per fragment). */
        if (migrated_any) {
            server_flush_fragmented_meta(s, ex);
            pthread_mutex_lock(&s->lock);
            s->export_meta_dirty = 0;
            pthread_mutex_unlock(&s->lock);
        }

        if (done && state == SERVER_STATE_SHRINKING) {
            if (s->export_meta_dirty)
                server_flush_fragmented_meta(s, ex);
            pthread_mutex_lock(&s->lock);
            s->state = SERVER_STATE_ACTIVE;
            s->export_meta_dirty = 0;
            pthread_mutex_unlock(&s->lock);
            printf("Shrink-quota migration complete, node active\n");
        } else if (done && state == SERVER_STATE_DRAINING) {
            if (s->export_meta_dirty)
                server_flush_fragmented_meta(s, ex);
            server_update_local_usage(s);
            pthread_mutex_lock(&s->lock);
            s->export_meta_dirty = 0;
            s->state = SERVER_STATE_DRAINED;
            pthread_mutex_unlock(&s->lock);
            printf("Drain complete, node drained (empty; rejects new PUTs)\n");
        }

        if (!migrated_any)
            usleep(500 * 1000);
    }

    return NULL;
}

int server_has_local_fragments_locked(struct efsd_server *s)
{
    for (uint32_t e = 0; e < s->export_count; e++) {
        struct efs_export *ex = &s->exports[e];
        for (uint64_t ci = 0; ci < ex->chunk_count; ci++) {
            for (int i = 0; i < EFS_NUM_FRAGMENTS; i++) {
                if (ex->chunks[ci].fragment_nodes[i] == s->id)
                    return 1;
            }
        }
    }
    return 0;
}

int server_node_is_empty(struct efsd_server *s)
{
    /* Fragmented meta may leave chunk maps empty until rebuild. */
    pthread_mutex_lock(&s->lock);
    for (uint32_t e = 0; e < s->export_count; e++) {
        struct efs_export *ex = &s->exports[e];
        int need = (ex->meta_fragmented && ex->meta_needs_rebuild);
        pthread_mutex_unlock(&s->lock);
        if (need)
            server_rebuild_export_from_pages(s, ex);
        pthread_mutex_lock(&s->lock);
    }
    pthread_mutex_unlock(&s->lock);

    server_update_local_usage(s);

    pthread_mutex_lock(&s->lock);
    /* Emptiness is defined by placement metadata, not raw disk used: meta-page
     * sidecars / empty dirs can leave used > 0 after all owned fragments move. */
    int empty = !server_has_local_fragments_locked(s);
    pthread_mutex_unlock(&s->lock);
    return empty;
}

void server_begin_leave_locked(struct efsd_server *s)
{
    /* Caller holds s->lock. Stop accepting work; notify peers after unlock. */
    s->state = SERVER_STATE_LEAVING;
    s->running = 0;
    if (s->listen_fd >= 0)
        shutdown(s->listen_fd, SHUT_RDWR);
    printf("Node leaving cluster (already drained)\n");
}

void server_start_migration(struct efsd_server *s)
{
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);
    pthread_create(&s->migrate_tid, &attr, migration_thread, s);
    pthread_attr_destroy(&attr);
}

void server_remove_node_from_cluster(struct efsd_server *s, efs_node_id_t node_id)
{
    pthread_mutex_lock(&s->lock);
    uint32_t new_count = 0;
    struct efs_node new_nodes[EFS_MAX_NODES];
    memset(new_nodes, 0, sizeof(new_nodes));
    for (uint32_t i = 0; i < s->node_count; i++) {
        if (s->nodes[i].id != node_id)
            new_nodes[new_count++] = s->nodes[i];
    }
    memcpy(s->nodes, new_nodes, sizeof(s->nodes));
    s->node_count = new_count;
    server_save_nodes(s);
    pthread_mutex_unlock(&s->lock);
}

void server_notify_node_left(struct efsd_server *s, efs_node_id_t node_id)
{
    pthread_mutex_lock(&s->lock);
    uint32_t node_count = s->node_count;
    struct efs_node nodes[EFS_MAX_NODES];
    memcpy(nodes, s->nodes, sizeof(nodes));
    pthread_mutex_unlock(&s->lock);

    struct efs_msg_node_left msg;
    memset(&msg, 0, sizeof(msg));
    msg.node_id = node_id;

    for (uint32_t i = 0; i < node_count; i++) {
        if (nodes[i].id == s->id || nodes[i].id == node_id)
            continue;
        int fd = efs_connect_tcp(nodes[i].addr, nodes[i].port);
        if (fd < 0)
            continue;
        efs_set_recv_timeout(fd, EFS_IO_TIMEOUT_MS);
        efs_set_send_timeout(fd, EFS_IO_TIMEOUT_MS);
        efs_send_msg(fd, EFS_MSG_NODE_LEFT, &msg, sizeof(msg));
        close(fd);
    }
}
