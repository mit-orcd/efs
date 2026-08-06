#include "efs/common.h"
#include "efs/protocol.h"
#include "efs/network.h"
#include "efs/checksum.h"
#include "server_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>

static int refresh_peer_usage(const char *host, uint16_t port, uint64_t *quota, uint64_t *used)
{
    int fd = efs_connect_tcp(host, port);
    if (fd < 0)
        return -1;

    efs_set_recv_timeout(fd, EFS_IO_TIMEOUT_MS);
    efs_set_send_timeout(fd, EFS_IO_TIMEOUT_MS);

    uint8_t type;
    void *reply = NULL;
    uint32_t reply_len = 0;
    int rc = -1;
    if (efs_send_msg(fd, EFS_MSG_STATUS, NULL, 0) == 0 &&
        efs_recv_msg(fd, &type, &reply, &reply_len) == 0 &&
        type == EFS_MSG_STATUS_REPLY &&
        reply_len == sizeof(struct efs_msg_status_reply)) {
        struct efs_msg_status_reply *r = reply;
        *quota = r->quota;
        *used = r->used;
        rc = 0;
    }

    free(reply);
    close(fd);
    return rc;
}

void server_handle_conn(int fd)
{
    while (1) {
        uint8_t type;
        void *payload = NULL;
        uint32_t payload_len = 0;

        int rc = efs_recv_msg(fd, &type, &payload, &payload_len);
        if (rc != 0)
            break;

        switch (type) {
        case EFS_MSG_HEARTBEAT: {
            efs_send_msg(fd, EFS_MSG_HEARTBEAT_ACK, NULL, 0);
            break;
        }
        case EFS_MSG_HELLO: {
            if (payload_len >= sizeof(struct efs_msg_hello)) {
                struct efs_msg_hello *h = payload;
                pthread_mutex_lock(&g_server->lock);

                /* Update existing entry or append a new one; never duplicate. */
                uint32_t idx = g_server->node_count;
                int changed = 0;
                for (uint32_t i = 0; i < g_server->node_count; i++) {
                    if (g_server->nodes[i].id == h->node_id) {
                        idx = i;
                        break;
                    }
                }
                if (idx < EFS_MAX_NODES) {
                    struct efs_node *n = &g_server->nodes[idx];
                    if (n->id != h->node_id || n->port != h->port ||
                        strcmp(n->addr, h->addr) != 0 ||
                        n->quota != h->quota || n->used != h->used) {
                        changed = 1;
                    }
                    n->id = h->node_id;
                    strncpy(n->addr, h->addr, sizeof(n->addr) - 1);
                    n->port = h->port;
                    strncpy(n->storage_path, h->storage_path,
                            sizeof(n->storage_path) - 1);
                    n->quota = h->quota;
                    n->used = h->used;
                    if (idx == g_server->node_count && g_server->node_count < EFS_MAX_NODES) {
                        g_server->node_count++;
                        changed = 1;
                    }
                }

                struct efs_msg_hello_ack ack;
                memset(&ack, 0, sizeof(ack));
                ack.epoch = g_server->epoch;
                ack.assigned_id = h->node_id;
                ack.node_count = g_server->node_count;
                memcpy(ack.nodes, g_server->nodes, sizeof(g_server->nodes));
                /* Persist only when membership / addressing actually changed. */
                if (changed)
                    server_save_nodes(g_server);
                pthread_mutex_unlock(&g_server->lock);
                efs_send_msg(fd, EFS_MSG_HELLO_ACK, &ack, sizeof(ack));
            }
            break;
        }
        case EFS_MSG_GET_CHUNK: {
            if (payload_len >= sizeof(struct efs_msg_get_chunk)) {
                struct efs_msg_get_chunk *req = payload;
                pthread_mutex_lock(&g_server->lock);
                struct efs_export *ex = server_get_export(g_server, req->export_id);
                pthread_mutex_unlock(&g_server->lock);

                uint8_t reply[1 + EFS_HASH_SIZE + EFS_FRAGMENT_SIZE];
                if (!ex) {
                    reply[0] = EFS_GET_CHUNK_ERROR;
                    efs_send_msg(fd, EFS_MSG_GET_CHUNK_REPLY, reply, 1);
                } else {
                    uint8_t data[EFS_FRAGMENT_SIZE];
                    uint32_t data_len = 0;
                    uint8_t hash[EFS_HASH_SIZE];
                    rc = server_read_fragment(g_server, ex, req->ino,
                                              req->chunk_index, req->fragment_index,
                                              data, &data_len);
                    if (rc != 0) {
                        reply[0] = EFS_GET_CHUNK_NOT_FOUND;
                        efs_send_msg(fd, EFS_MSG_GET_CHUNK_REPLY, reply, 1);
                    } else {
                        /* Prefer the checksum stored at write time; only
                         * re-hash as a fallback for fragments written by an
                         * older server that did not persist .sum sidecars. */
                        if (server_read_fragment_sum(g_server, ex, req->ino,
                                                     req->chunk_index,
                                                     req->fragment_index,
                                                     hash) != 0)
                            efs_hash(data, data_len, hash);
                        reply[0] = EFS_GET_CHUNK_OK;
                        memcpy(reply + 1, hash, EFS_HASH_SIZE);
                        memcpy(reply + 1 + EFS_HASH_SIZE, data, data_len);
                        efs_send_msg(fd, EFS_MSG_GET_CHUNK_REPLY, reply, 1 + EFS_HASH_SIZE + data_len);
                    }
                }
            }
            break;
        }
        case EFS_MSG_PUT_CHUNK: {
            if (payload_len >= sizeof(struct efs_msg_put_chunk)) {
                struct efs_msg_put_chunk *req = payload;
                pthread_mutex_lock(&g_server->lock);
                struct efs_export *ex = server_get_export(g_server, req->export_id);
                pthread_mutex_unlock(&g_server->lock);

                uint8_t reply = EFS_PUT_CHUNK_ERROR;
                if (ex) {
                    /* Verify the client-supplied checksum once at write time
                     * and persist it so reads do not need to re-hash. */
                    uint8_t verify[EFS_HASH_SIZE];
                    efs_hash(req->data, EFS_FRAGMENT_SIZE, verify);
                    if (memcmp(verify, req->checksum, EFS_HASH_SIZE) != 0) {
                        reply = EFS_PUT_CHUNK_ERROR;
                    } else {
                        rc = server_write_fragment(g_server, ex, req->ino,
                                                   req->chunk_index, req->fragment_index,
                                                   req->data, EFS_FRAGMENT_SIZE);
                        if (rc == 0) {
                            server_write_fragment_sum(g_server, ex, req->ino,
                                                      req->chunk_index,
                                                      req->fragment_index,
                                                      req->checksum);
                            reply = EFS_PUT_CHUNK_OK;
                        } else if (rc == EFS_ERR_QUOTA) {
                            reply = EFS_PUT_CHUNK_QUOTA_EXCEEDED;
                        }
                    }
                }
                efs_send_msg(fd, EFS_MSG_PUT_CHUNK_REPLY, &reply, 1);
            }
            break;
        }
        case EFS_MSG_GET_META: {
            pthread_mutex_lock(&g_server->lock);
            struct efs_export *ex = (g_server->export_count > 0) ? &g_server->exports[0] : NULL;
            char *buf = NULL;
            size_t len = 0;
            if (ex) {
                efs_export_serialize(ex, &buf, &len);
            }
            pthread_mutex_unlock(&g_server->lock);
            if (buf) {
                efs_send_msg(fd, EFS_MSG_GET_META_REPLY, buf, (uint32_t)len);
                free(buf);
            } else {
                efs_send_msg(fd, EFS_MSG_GET_META_REPLY, NULL, 0);
            }
            break;
        }
        case EFS_MSG_PUT_META: {
            if (payload_len > 0) {
                /* Deserialize the client's update into a temporary export,
                 * then merge it into the committed export so concurrent
                 * clients do not overwrite each other's inodes/chunks. */
                struct efs_export inc;
                efs_export_init(&inc, 1, "");
                uint8_t reply = EFS_PUT_META_ERROR;
                if (efs_export_deserialize(&inc, payload, payload_len) == 0) {
                    pthread_mutex_lock(&g_server->lock);
                    if (g_server->export_count == 0) {
                        efs_export_init(&g_server->exports[0], 1, "");
                        g_server->export_count = 1;
                    }
                    struct efs_export *ex = &g_server->exports[0];
                    if (efs_export_merge(ex, &inc) == 0) {
                        g_server->epoch++;
                        /* Defer disk persist: full metadata.bin rewrite+fsync
                         * on every batch dominates bulk-create cost. Flush on
                         * shutdown (and other explicit save paths). */
                        g_server->export_meta_dirty = 1;
                        reply = EFS_PUT_META_OK;
                    }
                    pthread_mutex_unlock(&g_server->lock);
                }
                efs_export_free(&inc);
                struct efs_msg_put_meta_reply m = {reply, g_server->epoch};
                efs_send_msg(fd, EFS_MSG_PUT_META_REPLY, &m, sizeof(m));
            }
            break;
        }
        case EFS_MSG_LIST_NODES: {
            server_update_local_usage(g_server);

            pthread_mutex_lock(&g_server->lock);
            uint32_t node_count = g_server->node_count;
            struct efs_node nodes[EFS_MAX_NODES];
            memcpy(nodes, g_server->nodes, sizeof(nodes));
            pthread_mutex_unlock(&g_server->lock);

            for (uint32_t i = 0; i < node_count; i++) {
                if (nodes[i].id == g_server->id)
                    continue;
                uint64_t q = 0, u = 0;
                if (refresh_peer_usage(nodes[i].addr, nodes[i].port, &q, &u) == 0) {
                    pthread_mutex_lock(&g_server->lock);
                    for (uint32_t j = 0; j < g_server->node_count; j++) {
                        if (g_server->nodes[j].id == nodes[i].id) {
                            g_server->nodes[j].quota = q;
                            g_server->nodes[j].used = u;
                            break;
                        }
                    }
                    pthread_mutex_unlock(&g_server->lock);
                }
            }

            pthread_mutex_lock(&g_server->lock);
            struct efs_msg_list_nodes_reply reply;
            reply.node_count = g_server->node_count;
            memcpy(reply.nodes, g_server->nodes, sizeof(g_server->nodes));
            pthread_mutex_unlock(&g_server->lock);
            efs_send_msg(fd, EFS_MSG_LIST_NODES_REPLY, &reply, sizeof(reply));
            break;
        }
        case EFS_MSG_STATUS: {
            server_update_local_usage(g_server);
            pthread_mutex_lock(&g_server->lock);
            struct efs_msg_status_reply reply;
            reply.quota = g_server->quota;
            reply.used = g_server->nodes[0].used;
            pthread_mutex_unlock(&g_server->lock);
            efs_send_msg(fd, EFS_MSG_STATUS_REPLY, &reply, sizeof(reply));
            break;
        }
        case EFS_MSG_REMOVE_NODE: {
            uint8_t reply = EFS_REMOVE_NODE_ERROR;
            pthread_mutex_lock(&g_server->lock);
            if (g_server->state == SERVER_STATE_ACTIVE) {
                g_server->state = SERVER_STATE_LEAVING;
                printf("Node removal requested, starting background migration\n");
                reply = EFS_REMOVE_NODE_IN_PROGRESS;
            } else if (g_server->state == SERVER_STATE_LEAVING) {
                reply = EFS_REMOVE_NODE_IN_PROGRESS;
            }
            pthread_mutex_unlock(&g_server->lock);
            efs_send_msg(fd, EFS_MSG_REMOVE_NODE_REPLY, &reply, 1);
            break;
        }
        case EFS_MSG_SHRINK_QUOTA: {
            uint8_t reply = EFS_SHRINK_QUOTA_ERROR;
            if (payload_len >= sizeof(struct efs_msg_shrink_quota)) {
                struct efs_msg_shrink_quota *req = payload;
                pthread_mutex_lock(&g_server->lock);
                if (g_server->quota > 0 && g_server->quota > req->amount) {
                    uint64_t new_quota = g_server->quota - req->amount;
                    g_server->quota = new_quota;
                    g_server->nodes[0].quota = new_quota;
                    if (g_server->nodes[0].used > new_quota) {
                        g_server->shrink_target = new_quota;
                        g_server->state = SERVER_STATE_SHRINKING;
                        printf("Shrink-quota requested: new quota %llu, starting migration\n",
                               (unsigned long long)new_quota);
                    } else {
                        printf("Shrink-quota requested: new quota %llu, already within limit\n",
                               (unsigned long long)new_quota);
                    }
                    reply = EFS_SHRINK_QUOTA_IN_PROGRESS;
                }
                pthread_mutex_unlock(&g_server->lock);
            }
            efs_send_msg(fd, EFS_MSG_SHRINK_QUOTA_REPLY, &reply, 1);
            break;
        }
        case EFS_MSG_NODE_LEFT: {
            if (payload_len >= sizeof(struct efs_msg_node_left)) {
                struct efs_msg_node_left *msg = payload;
                server_remove_node_from_cluster(g_server, msg->node_id);
                printf("Node %u left the cluster\n", msg->node_id);
            }
            break;
        }
        case EFS_MSG_CREATE_EXPORT: {
            if (payload_len >= sizeof(struct efs_msg_create_export)) {
                struct efs_msg_create_export *req = payload;
                pthread_mutex_lock(&g_server->lock);
                int exists = 0;
                for (uint32_t i = 0; i < g_server->export_count; i++) {
                    if (strcmp(g_server->exports[i].name, req->name) == 0) {
                        exists = 1;
                        break;
                    }
                }
                struct efs_export *ex = NULL;
                uint8_t reply = EFS_CREATE_EXPORT_ERROR;
                if (!exists) {
                    ex = server_find_export(g_server, req->name);
                    if (ex != NULL)
                        reply = EFS_CREATE_EXPORT_OK;
                } else {
                    reply = EFS_CREATE_EXPORT_EXISTS;
                }
                pthread_mutex_unlock(&g_server->lock);
                if (reply == EFS_CREATE_EXPORT_OK) {
                    server_replicate_metadata(g_server, ex);
                }
                efs_send_msg(fd, EFS_MSG_CREATE_EXPORT_REPLY, &reply, 1);
            }
            break;
        }
        case EFS_MSG_LIST_EXPORTS: {
            struct efs_msg_list_exports_reply reply;
            memset(&reply, 0, sizeof(reply));
            pthread_mutex_lock(&g_server->lock);
            reply.export_count = g_server->export_count;
            for (uint32_t i = 0; i < g_server->export_count && i < EFS_MAX_EXPORTS; i++) {
                reply.exports[i].id = g_server->exports[i].id;
                strncpy(reply.exports[i].name, g_server->exports[i].name,
                        sizeof(reply.exports[i].name) - 1);
            }
            pthread_mutex_unlock(&g_server->lock);
            efs_send_msg(fd, EFS_MSG_LIST_EXPORTS_REPLY, &reply, sizeof(reply));
            break;
        }
        case EFS_MSG_JOIN: {
            if (payload_len >= sizeof(struct efs_msg_join)) {
                struct efs_msg_join *req = payload;
                uint8_t reply = EFS_JOIN_OK;
                if (server_join_cluster(g_server, req->peer_host, req->peer_port) != 0)
                    reply = EFS_JOIN_ERROR;
                if (reply == EFS_JOIN_OK)
                    server_fetch_metadata_from(g_server, req->peer_host, req->peer_port);
                efs_send_msg(fd, EFS_MSG_JOIN_REPLY, &reply, 1);
            }
            break;
        }
        case EFS_MSG_QUERY_STATS: {
            struct efs_msg_query_stats_reply reply;
            memset(&reply, 0, sizeof(reply));
            pthread_mutex_lock(&g_server->lock);
            for (uint32_t e = 0; e < g_server->export_count; e++) {
                struct efs_export *ex = &g_server->exports[e];
                for (uint64_t i = 0; i < ex->inode_count; i++) {
                    struct efs_inode *ino = &ex->inodes[i];
                    if (efs_mode_is_dir(ino->mode))
                        continue;
                    reply.total_files++;
                    reply.total_bytes += ino->size;
                    uint32_t j;
                    for (j = 0; j < reply.user_count; j++) {
                        if (reply.users[j].uid == ino->uid)
                            break;
                    }
                    if (j == reply.user_count && reply.user_count < EFS_MAX_QUERY_USERS) {
                        reply.users[reply.user_count].uid = ino->uid;
                        reply.user_count++;
                    }
                    if (j < EFS_MAX_QUERY_USERS) {
                        reply.users[j].files++;
                        reply.users[j].bytes += ino->size;
                    }
                }
            }
            pthread_mutex_unlock(&g_server->lock);
            efs_send_msg(fd, EFS_MSG_QUERY_STATS_REPLY, &reply, sizeof(reply));
            break;
        }
        default:
            break;
        }

        free(payload);
    }
}
