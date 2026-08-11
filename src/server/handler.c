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
                int accepted = 0;
                for (uint32_t i = 0; i < g_server->node_count; i++) {
                    if (g_server->nodes[i].id == h->node_id) {
                        idx = i;
                        break;
                    }
                }
                if (idx < g_server->node_count) {
                    /* Refresh an already-known peer. */
                    struct efs_node *n = &g_server->nodes[idx];
                    if (n->port != h->port ||
                        strcmp(n->addr, h->addr) != 0 ||
                        strcmp(n->storage_path, h->storage_path) != 0 ||
                        n->quota != h->quota || n->used != h->used) {
                        changed = 1;
                    }
                    strncpy(n->addr, h->addr, sizeof(n->addr) - 1);
                    n->port = h->port;
                    strncpy(n->storage_path, h->storage_path,
                            sizeof(n->storage_path) - 1);
                    n->quota = h->quota;
                    n->used = h->used;
                    accepted = 1;
                } else if (g_server->node_count < EFS_MAX_NODES) {
                    struct efs_node *n = &g_server->nodes[g_server->node_count];
                    n->id = h->node_id;
                    strncpy(n->addr, h->addr, sizeof(n->addr) - 1);
                    n->port = h->port;
                    strncpy(n->storage_path, h->storage_path,
                            sizeof(n->storage_path) - 1);
                    n->quota = h->quota;
                    n->used = h->used;
                    g_server->node_count++;
                    changed = 1;
                    accepted = 1;
                }

                server_dedupe_nodes_locked(g_server);
                /* Heap: hello_ack embeds nodes[EFS_MAX_NODES] (~16KiB). */
                struct efs_msg_hello_ack *ack = calloc(1, sizeof(*ack));
                if (!ack) {
                    pthread_mutex_unlock(&g_server->lock);
                    break;
                }
                ack->epoch = g_server->epoch;
                /* assigned_id == 0 means reject (e.g. cluster at capacity). */
                ack->assigned_id = accepted ? h->node_id : 0;
                ack->node_count = g_server->node_count;
                memcpy(ack->nodes, g_server->nodes, sizeof(g_server->nodes));
                /* Persist only when membership / addressing actually changed. */
                if (changed)
                    server_save_nodes(g_server);
                pthread_mutex_unlock(&g_server->lock);
                efs_send_msg(fd, EFS_MSG_HELLO_ACK, ack, sizeof(*ack));
                free(ack);
            }
            break;
        }
        case EFS_MSG_GET_CHUNK: {
            if (payload_len >= sizeof(struct efs_msg_get_chunk)) {
                struct efs_msg_get_chunk *req = payload;
                pthread_mutex_lock(&g_server->lock);
                struct efs_export *ex = server_get_export(g_server, req->export_id);
                pthread_mutex_unlock(&g_server->lock);

                uint32_t frag_len = server_frag_len(ex, req->ino);
                uint8_t *reply = malloc(1 + EFS_HASH_SIZE + frag_len);
                uint8_t *data = malloc(frag_len);
                if (!reply || !data) {
                    free(reply);
                    free(data);
                    uint8_t err = EFS_GET_CHUNK_ERROR;
                    efs_send_msg(fd, EFS_MSG_GET_CHUNK_REPLY, &err, 1);
                    break;
                }
                if (!ex) {
                    reply[0] = EFS_GET_CHUNK_ERROR;
                    efs_send_msg(fd, EFS_MSG_GET_CHUNK_REPLY, reply, 1);
                } else {
                    uint32_t data_len = 0;
                    uint8_t hash[EFS_HASH_SIZE];
                    rc = server_read_fragment(g_server, ex, req->ino,
                                              req->chunk_index, req->fragment_index,
                                              data, &data_len);
                    if (rc != 0) {
                        reply[0] = EFS_GET_CHUNK_NOT_FOUND;
                        efs_send_msg(fd, EFS_MSG_GET_CHUNK_REPLY, reply, 1);
                    } else {
                        if (server_read_fragment_sum(g_server, ex, req->ino,
                                                     req->chunk_index,
                                                     req->fragment_index,
                                                     hash) != 0)
                            efs_hash(data, data_len, hash);
                        reply[0] = EFS_GET_CHUNK_OK;
                        memcpy(reply + 1, hash, EFS_HASH_SIZE);
                        memcpy(reply + 1 + EFS_HASH_SIZE, data, data_len);
                        efs_send_msg(fd, EFS_MSG_GET_CHUNK_REPLY, reply,
                                     1 + EFS_HASH_SIZE + data_len);
                    }
                }
                free(reply);
                free(data);
            }
            break;
        }
        case EFS_MSG_PUT_CHUNK: {
            if (payload_len >= sizeof(struct efs_msg_put_chunk)) {
                struct efs_msg_put_chunk *req = payload;
                const uint8_t *data =
                    (const uint8_t *)payload + sizeof(struct efs_msg_put_chunk);
                pthread_mutex_lock(&g_server->lock);
                int put_state = g_server->state;
                struct efs_export *ex = server_get_export(g_server, req->export_id);
                /* Auto-create export shell so meta-page PUTs can land before
                 * the EFSR root arrives (mkfs / first flush race). */
                if (!ex && g_server->export_count < EFS_MAX_EXPORTS) {
                    ex = &g_server->exports[g_server->export_count++];
                    efs_export_init(ex, req->export_id, "pending");
                    ex->id = req->export_id;
                }
                /* Learn data chunk_size from first non-meta PUT when still
                 * default (peer may not have applied EFSR yet). */
                if (ex && req->ino != EFS_META_TABLE_INO &&
                    req->data_len > 0 &&
                    (ex->chunk_size == 0 ||
                     ex->chunk_size == EFS_DEFAULT_CHUNK_SIZE) &&
                    efs_chunk_size_valid(req->data_len * 2u) &&
                    req->data_len != EFS_META_FRAGMENT_SIZE) {
                    ex->chunk_size = req->data_len * 2u;
                }
                pthread_mutex_unlock(&g_server->lock);

                uint8_t reply = EFS_PUT_CHUNK_ERROR;
                if (put_state == SERVER_STATE_DRAINING ||
                    put_state == SERVER_STATE_DRAINED ||
                    put_state == SERVER_STATE_LEAVING) {
                    reply = EFS_PUT_CHUNK_ERROR;
                } else if (ex) {
                    uint32_t expect = server_frag_len(ex, req->ino);
                    if (req->data_len != expect ||
                        payload_len < sizeof(*req) + req->data_len) {
                        reply = EFS_PUT_CHUNK_ERROR;
                    } else {
                        uint8_t zero_ck[EFS_HASH_SIZE];
                        efs_hash_zero_fragment_len(expect, zero_ck);
                        int sum_ok = 0;
                        if (memcmp(req->checksum, zero_ck, EFS_HASH_SIZE) == 0) {
                            /* Trust the cached zero digest on the PUT hot path.
                             * Writer/store already short-circuit to a shared
                             * zero page; re-scanning 64KiB+ here dominated
                             * single-stream dd if=/dev/zero. */
                            sum_ok = 1;
                        } else {
                            uint8_t verify[EFS_HASH_SIZE];
                            efs_hash(data, expect, verify);
                            sum_ok = (memcmp(verify, req->checksum,
                                             EFS_HASH_SIZE) == 0);
                        }
                        if (!sum_ok) {
                            reply = EFS_PUT_CHUNK_ERROR;
                        } else {
                            rc = server_write_fragment_with_sum(
                                g_server, ex, req->ino, req->chunk_index,
                                req->fragment_index, data, expect, req->checksum);
                            if (rc == 0)
                                reply = EFS_PUT_CHUNK_OK;
                            else if (rc == EFS_ERR_QUOTA)
                                reply = EFS_PUT_CHUNK_QUOTA_EXCEEDED;
                        }
                    }
                }
                efs_send_msg(fd, EFS_MSG_PUT_CHUNK_REPLY, &reply, 1);
            }
            break;
        }
        case EFS_MSG_BENCH_PUT: {
            /* Network bench: accept mount-shaped PUT payload, ACK, discard. */
            uint8_t reply = EFS_BENCH_PUT_ERROR;
            if (payload_len >= sizeof(struct efs_msg_put_chunk)) {
                struct efs_msg_put_chunk *req = payload;
                if (payload_len >= sizeof(*req) + req->data_len)
                    reply = EFS_BENCH_PUT_OK;
            }
            efs_send_msg(fd, EFS_MSG_BENCH_PUT_REPLY, &reply, 1);
            break;
        }
        case EFS_MSG_GET_META: {
            pthread_mutex_lock(&g_server->lock);
            struct efs_export *ex = (g_server->export_count > 0) ? &g_server->exports[0] : NULL;
            char *buf = NULL;
            size_t len = 0;
            if (ex) {
                if (ex->meta_fragmented)
                    efs_export_root_serialize(&ex->root, &buf, &len);
                else
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
                uint8_t reply = EFS_PUT_META_ERROR;
                if (efs_meta_blob_is_root(payload, payload_len)) {
                    struct efs_export_root root;
                    memset(&root, 0, sizeof(root));
                    if (efs_export_root_deserialize(&root, payload, payload_len) == 0) {
                        pthread_mutex_lock(&g_server->lock);
                        if (g_server->export_count == 0) {
                            efs_export_init(&g_server->exports[0], root.id, root.name);
                            g_server->export_count = 1;
                        }
                        struct efs_export *ex = &g_server->exports[0];
                        if (ex->meta_fragmented &&
                            root.generation < ex->root.generation) {
                            reply = EFS_PUT_META_STALE;
                            pthread_mutex_unlock(&g_server->lock);
                            efs_export_root_free(&root);
                        } else {
                            uint64_t old_gen = ex->root.generation;
                            uint32_t old_pages = ex->root.page_count;
                            int had_frag = ex->meta_fragmented;
                            ex->meta_fragmented = 1;
                            efs_export_root_move(&ex->root, &root);
                            ex->id = ex->root.id;
                            strncpy(ex->name, ex->root.name, EFS_MAX_NAME - 1);
                            ex->next_ino = ex->root.next_ino;
                            if (efs_chunk_size_valid(ex->root.chunk_size))
                                ex->chunk_size = ex->root.chunk_size;
                            /* Never rebuild on this handler thread: peer page
                             * fetches would block the pooled client connection
                             * and can cascade into multi-node stalls. */
                            if (ex->root.page_count > 0)
                                ex->meta_needs_rebuild = 1;
                            g_server->epoch++;
                            g_server->export_meta_dirty = 1;
                            /* Save while holding the lock: concurrent PUT_META
                             * can efs_export_root_move and free page_checksums
                             * under a raced unlocked save (SIGSEGV). */
                            server_save_export(g_server, ex);
                            g_server->export_meta_dirty = 0;
                            uint64_t new_gen = ex->root.generation;
                            pthread_mutex_unlock(&g_server->lock);
                            reply = EFS_PUT_META_OK;
                            /* Client-driven root flip: drop local fragments for
                             * the retired dual-slot generation. */
                            if (had_frag && old_pages > 0 && old_gen != new_gen)
                                server_gc_meta_slot_pages(g_server,
                                                          &g_server->exports[0],
                                                          old_gen, old_pages);
                        }
                    }
                } else if (efs_meta_blob_is_export(payload, payload_len)) {
                    /* Legacy full-blob merge (pre-fragmented peers / tests). */
                    struct efs_export inc;
                    efs_export_init(&inc, 1, "pending");
                    if (efs_export_deserialize(&inc, payload, payload_len) == 0) {
                        pthread_mutex_lock(&g_server->lock);
                        if (g_server->export_count == 0) {
                            efs_export_init(&g_server->exports[0],
                                            inc.id ? inc.id : 1,
                                            inc.name[0] ? inc.name : "pending");
                            g_server->export_count = 1;
                        }
                        struct efs_export *ex = &g_server->exports[0];
                        if (efs_export_merge(ex, &inc) == 0) {
                            ex->meta_fragmented = 0;
                            g_server->epoch++;
                            g_server->export_meta_dirty = 1;
                            if (ex->name[0] && strcmp(ex->name, "pending") != 0) {
                                server_save_export(g_server, ex);
                                g_server->export_meta_dirty = 0;
                            }
                            reply = EFS_PUT_META_OK;
                            pthread_mutex_unlock(&g_server->lock);
                        } else {
                            pthread_mutex_unlock(&g_server->lock);
                        }
                    }
                    efs_export_free(&inc);
                }
                struct efs_msg_put_meta_reply m;
                memset(&m, 0, sizeof(m));
                m.status = reply;
                m.new_epoch = g_server->epoch;
                efs_send_msg(fd, EFS_MSG_PUT_META_REPLY, &m, sizeof(m));
            }
            break;
        }
        case EFS_MSG_LIST_NODES: {
            /* used comes from the cached counter (meta/usage.bin); no tree walk. */
            struct efs_node *nodes = malloc(sizeof(struct efs_node) * EFS_MAX_NODES);
            struct efs_msg_list_nodes_reply *reply = calloc(1, sizeof(*reply));
            if (!nodes || !reply) {
                free(nodes);
                free(reply);
                break;
            }
            pthread_mutex_lock(&g_server->lock);
            uint32_t node_count = g_server->node_count;
            if (node_count > EFS_MAX_NODES)
                node_count = EFS_MAX_NODES;
            memcpy(nodes, g_server->nodes, sizeof(struct efs_node) * node_count);
            efs_node_id_t self = g_server->id;
            pthread_mutex_unlock(&g_server->lock);

            for (uint32_t i = 0; i < node_count; i++) {
                if (nodes[i].id == self)
                    continue;
                uint64_t q = 0, u = 0;
                if (refresh_peer_usage(nodes[i].addr, nodes[i].port, &q, &u) == 0) {
                    pthread_mutex_lock(&g_server->lock);
                    /* Match the peer we contacted by addr:port (not only id),
                     * so a corrupt duplicate-id row cannot steal the update. */
                    for (uint32_t j = 0; j < g_server->node_count; j++) {
                        if (g_server->nodes[j].port == nodes[i].port &&
                            strcmp(g_server->nodes[j].addr, nodes[i].addr) == 0) {
                            g_server->nodes[j].quota = q;
                            g_server->nodes[j].used = u;
                            break;
                        }
                    }
                    pthread_mutex_unlock(&g_server->lock);
                }
            }

            pthread_mutex_lock(&g_server->lock);
            server_dedupe_nodes_locked(g_server);
            reply->node_count = g_server->node_count;
            memcpy(reply->nodes, g_server->nodes, sizeof(g_server->nodes));
            pthread_mutex_unlock(&g_server->lock);
            efs_send_msg(fd, EFS_MSG_LIST_NODES_REPLY, reply, sizeof(*reply));
            free(nodes);
            free(reply);
            break;
        }
        case EFS_MSG_STATUS: {
            pthread_mutex_lock(&g_server->lock);
            struct efs_msg_status_reply reply;
            memset(&reply, 0, sizeof(reply));
            reply.quota = g_server->quota;
            struct efs_node *local = server_local_node(g_server);
            reply.used = local ? local->used : 0;
            reply.state = (uint32_t)g_server->state;
            pthread_mutex_unlock(&g_server->lock);
            efs_send_msg(fd, EFS_MSG_STATUS_REPLY, &reply, sizeof(reply));
            break;
        }
        case EFS_MSG_DRAIN_NODE: {
            uint8_t reply = EFS_DRAIN_NODE_ERROR;
            pthread_mutex_lock(&g_server->lock);
            int st = g_server->state;
            pthread_mutex_unlock(&g_server->lock);

            if (st == SERVER_STATE_DRAINED) {
                reply = EFS_DRAIN_NODE_OK;
            } else if (st == SERVER_STATE_DRAINING) {
                reply = EFS_DRAIN_NODE_IN_PROGRESS;
            } else if (st == SERVER_STATE_SHRINKING || st == SERVER_STATE_LEAVING) {
                reply = EFS_DRAIN_NODE_ERROR;
            } else if (st == SERVER_STATE_ACTIVE) {
                int empty = server_node_is_empty(g_server);
                pthread_mutex_lock(&g_server->lock);
                if (g_server->state != SERVER_STATE_ACTIVE) {
                    /* Lost race with another mgmt op. */
                    reply = (g_server->state == SERVER_STATE_DRAINED) ? EFS_DRAIN_NODE_OK
                          : (g_server->state == SERVER_STATE_DRAINING) ? EFS_DRAIN_NODE_IN_PROGRESS
                          : EFS_DRAIN_NODE_ERROR;
                } else if (empty) {
                    g_server->state = SERVER_STATE_DRAINED;
                    printf("Drain requested: already empty, node drained\n");
                    reply = EFS_DRAIN_NODE_OK;
                } else {
                    g_server->state = SERVER_STATE_DRAINING;
                    printf("Drain requested, starting background migration\n");
                    reply = EFS_DRAIN_NODE_IN_PROGRESS;
                }
                pthread_mutex_unlock(&g_server->lock);
            }
            efs_send_msg(fd, EFS_MSG_DRAIN_NODE_REPLY, &reply, 1);
            break;
        }
        case EFS_MSG_UNDRAIN_NODE: {
            uint8_t reply = EFS_UNDRAIN_NODE_ERROR;
            pthread_mutex_lock(&g_server->lock);
            if (g_server->state == SERVER_STATE_DRAINED) {
                g_server->state = SERVER_STATE_ACTIVE;
                printf("Undrain requested, node active\n");
                reply = EFS_UNDRAIN_NODE_OK;
            } else if (g_server->state == SERVER_STATE_ACTIVE) {
                reply = EFS_UNDRAIN_NODE_OK;
            }
            pthread_mutex_unlock(&g_server->lock);
            efs_send_msg(fd, EFS_MSG_UNDRAIN_NODE_REPLY, &reply, 1);
            break;
        }
        case EFS_MSG_REMOVE_NODE: {
            uint8_t reply = EFS_REMOVE_NODE_ERROR;
            int do_notify = 0;
            pthread_mutex_lock(&g_server->lock);
            int st = g_server->state;
            pthread_mutex_unlock(&g_server->lock);

            if (st == SERVER_STATE_LEAVING) {
                reply = EFS_REMOVE_NODE_OK;
            } else if (st == SERVER_STATE_DRAINING || st == SERVER_STATE_SHRINKING) {
                reply = EFS_REMOVE_NODE_NOT_DRAINED;
            } else if (st == SERVER_STATE_DRAINED || st == SERVER_STATE_ACTIVE) {
                int empty = (st == SERVER_STATE_DRAINED) || server_node_is_empty(g_server);
                pthread_mutex_lock(&g_server->lock);
                if (g_server->state == SERVER_STATE_LEAVING) {
                    reply = EFS_REMOVE_NODE_OK;
                } else if (g_server->state == SERVER_STATE_DRAINING ||
                           g_server->state == SERVER_STATE_SHRINKING) {
                    reply = EFS_REMOVE_NODE_NOT_DRAINED;
                } else if (empty || g_server->state == SERVER_STATE_DRAINED) {
                    server_begin_leave_locked(g_server);
                    do_notify = 1;
                    reply = EFS_REMOVE_NODE_OK;
                } else {
                    reply = EFS_REMOVE_NODE_NOT_DRAINED;
                }
                pthread_mutex_unlock(&g_server->lock);
            }
            if (do_notify)
                server_notify_node_left(g_server, g_server->id);
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
                    struct efs_node *local = server_local_node(g_server);
                    if (local)
                        local->quota = new_quota;
                    uint64_t used = local ? local->used : 0;
                    if (used > new_quota) {
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
            /* Accept name-only (legacy) or name+chunk_size. */
            if (payload_len >= EFS_MAX_NAME) {
                struct efs_msg_create_export *req = payload;
                uint32_t chunk_size = EFS_DEFAULT_CHUNK_SIZE;
                if (payload_len >= sizeof(struct efs_msg_create_export) &&
                    req->chunk_size != 0)
                    chunk_size = req->chunk_size;
                uint8_t reply = EFS_CREATE_EXPORT_ERROR;
                struct efs_export *ex = NULL;
                if (!req->name[0] || !efs_chunk_size_valid(chunk_size)) {
                    reply = EFS_CREATE_EXPORT_ERROR;
                } else {
                    pthread_mutex_lock(&g_server->lock);
                    int exists = 0;
                    for (uint32_t i = 0; i < g_server->export_count; i++) {
                        if (strcmp(g_server->exports[i].name, req->name) == 0) {
                            exists = 1;
                            break;
                        }
                    }
                    if (!exists) {
                        ex = server_find_export(g_server, req->name);
                        if (ex != NULL) {
                            ex->chunk_size = chunk_size;
                            server_save_export(g_server, ex);
                            reply = EFS_CREATE_EXPORT_OK;
                        }
                    } else {
                        reply = EFS_CREATE_EXPORT_EXISTS;
                    }
                    pthread_mutex_unlock(&g_server->lock);
                }
                /* Local create already persisted via server_find_export. Do not
                 * report a hard ERROR if only peer replicate fails — that left
                 * mkfs claiming failure while the export already existed. */
                if (reply == EFS_CREATE_EXPORT_OK) {
                    if (server_replicate_metadata(g_server, ex) < 0)
                        reply = EFS_CREATE_EXPORT_REPLICATE_FAILED;
                }
                efs_send_msg(fd, EFS_MSG_CREATE_EXPORT_REPLY, &reply, 1);
            }
            break;
        }
        case EFS_MSG_DESTROY_EXPORT: {
            /* Local wipe only; efs-mgmt fans out to each cluster node. */
            uint8_t reply = EFS_DESTROY_EXPORT_ERROR;
            if (payload_len >= sizeof(struct efs_msg_destroy_export)) {
                struct efs_msg_destroy_export *req = payload;
                int rc = server_destroy_export(g_server, req->name);
                if (rc == EFS_OK)
                    reply = EFS_DESTROY_EXPORT_OK;
                else if (rc == EFS_ERR_NOT_FOUND)
                    reply = EFS_DESTROY_EXPORT_NOT_FOUND;
            }
            efs_send_msg(fd, EFS_MSG_DESTROY_EXPORT_REPLY, &reply, 1);
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
            /* Rebuild bulk tables before counting (EFSR root alone is not enough). */
            pthread_mutex_lock(&g_server->lock);
            uint32_t ec = g_server->export_count;
            int need[EFS_MAX_EXPORTS];
            memset(need, 0, sizeof(need));
            for (uint32_t e = 0; e < ec && e < EFS_MAX_EXPORTS; e++) {
                struct efs_export *ex = &g_server->exports[e];
                /* Rebuild when dirty, or when tables look empty despite pages. */
                need[e] = (ex->meta_fragmented && ex->root.page_count > 0 &&
                           (ex->meta_needs_rebuild || ex->inode_count <= 1));
            }
            pthread_mutex_unlock(&g_server->lock);
            for (uint32_t e = 0; e < ec && e < EFS_MAX_EXPORTS; e++) {
                if (!need[e])
                    continue;
                server_rebuild_export_from_pages(g_server, &g_server->exports[e]);
            }

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
