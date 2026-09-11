#include "efs/common.h"
#include "efs/protocol.h"
#include "efs/network.h"
#include "efs/rdma.h"
#include "efs/checksum.h"
#include "efs/store.h"
#include "server_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <poll.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <time.h>

/* Per-connection-thread arenas. Fragment PUTs/GETs used to each cost a
 * malloc/free pair per message; with ~144 conns at ~100K msg/s that showed
 * up as arena-lock contention. TLS arenas grow once and are reused for the
 * life of the conn thread; freed by server_handler_tls_cleanup on exit.
 * Frames larger than EFS_HANDLER_TLS_MAX (rare meta blobs) still malloc. */
#define EFS_HANDLER_TLS_MAX (1024 * 1024)

static __thread uint8_t *tls_payload;
static __thread uint32_t tls_payload_cap;
static __thread uint8_t *tls_reply;
static __thread uint32_t tls_reply_cap;

void server_handler_tls_cleanup(void)
{
    free(tls_payload);
    tls_payload = NULL;
    tls_payload_cap = 0;
    free(tls_reply);
    tls_reply = NULL;
    tls_reply_cap = 0;
}


/* Wait for the next request on either channel of an RDMA-capable conn.
 * See efs_conn_wait_request (protocol.c) — kept out of this file so the
 * xprt test cannot drift from the server conn thread. */

void server_handle_conn(struct efs_conn *conn)
{
    int fd = conn->fd;
    while (1) {
        uint8_t type = 0;
        void *payload = NULL;
        uint32_t payload_len = 0;
        void *to_free = NULL;
        int rdma_frame = 0;
        int rc = 0;

        int chan = efs_conn_wait_request(conn);
        if (chan < 0)
            break;
        conn->recv_chan = chan;
        if (getenv("EFS_RDMA_FIRST")) {
            static int nwait;
            int n = __sync_fetch_and_add(&nwait, 1);
            if (n < 8)
                fprintf(stderr, "rdma-first: wait_request chan=%s rc=%p\n",
                        chan == EFS_CONN_RDMA ? "RDMA" : "TCP",
                        (void *)conn->rc);
        }

        if (chan == EFS_CONN_RDMA) {
            /* The payload aliases a QP recv pool buffer; it is reposted
             * after the switch (every handler consumes it synchronously). */
            int wr = efs_rdma_recv_wait(conn->rc, -1);
            if (wr == EFS_ERR_AGAIN)
                continue; /* TCP side-channel has a request */
            if (wr != 0)
                break;
            uint32_t flen = 0;
            uint8_t *frame = efs_rdma_recv_frame(conn->rc, &flen);
            if (!frame || flen < 5) {
                efs_rdma_recv_repost(conn->rc);
                break;
            }
            uint32_t nlen;
            memcpy(&nlen, frame, 4);
            nlen = ntohl(nlen);
            if (nlen == 0 || nlen > 16 * 1024 * 1024 || flen != 4 + nlen) {
                efs_rdma_recv_repost(conn->rc);
                break;
            }
            type = frame[4];
            payload = frame + 5;
            payload_len = nlen - 1;
            rdma_frame = 1;
        } else {
            uint32_t len = 0;
            if (efs_recv_all(fd, &len, sizeof(len)) != 0) {
                /* Idle SO_RCVTIMEO must not kill a pooled client fd. */
                if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
                    continue;
                break;
            }
            len = ntohl(len);
            if (len == 0 || len > EFS_MSG_MAX_LEN)
                break;
            if (efs_recv_all(fd, &type, 1) != 0)
                break;
            payload_len = len - 1;
            if (payload_len > 0) {
                if (payload_len <= EFS_HANDLER_TLS_MAX) {
                    if (tls_payload_cap < payload_len) {
                        uint8_t *nb = realloc(tls_payload, payload_len);
                        if (!nb)
                            break;
                        tls_payload = nb;
                        tls_payload_cap = payload_len;
                    }
                    payload = tls_payload;
                } else {
                    payload = malloc(payload_len);
                    if (!payload)
                        break;
                    to_free = payload;
                }
                if (efs_recv_all(fd, payload, payload_len) != 0) {
                    free(to_free);
                    break;
                }
            }
        }

        switch (type) {
        case EFS_MSG_HEARTBEAT: {
            efs_conn_send_msg(conn, EFS_MSG_HEARTBEAT_ACK, NULL, 0);
            break;
        }
        case EFS_MSG_HELLO: {
            /* Legacy (pre-build-id) HELLO is shorter than the current struct;
             * refuse it too — a node we cannot identify by build must not
             * join the placement ring. */
            if (payload_len >= sizeof(uint32_t)) {
                struct efs_msg_hello *h = payload;
                int version_ok =
                    payload_len >= sizeof(struct efs_msg_hello) &&
                    h->version == EFS_VERSION_PACK &&
                    strncmp(h->build_id, EFS_BUILD_ID, EFS_BUILD_ID_LEN) == 0;
                if (!version_ok) {
                    /* Heap: hello_ack embeds nodes[EFS_MAX_NODES] (~16KiB). */
                    struct efs_msg_hello_ack *rej = calloc(1, sizeof(*rej));
                    if (!rej)
                        break;
                    pthread_mutex_lock(&g_server->lock);
                    rej->epoch = g_server->epoch;
                    pthread_mutex_unlock(&g_server->lock);
                    rej->assigned_id = 0;
                    rej->reject_reason = EFS_HELLO_REJECT_VERSION;
                    strncpy(rej->build_id, EFS_BUILD_ID, sizeof(rej->build_id) - 1);
                    fprintf(stderr,
                            "HELLO rejected: node (%s:%u) runs build '%s' "
                            "(version 0x%x); this node is build '%s' "
                            "(version 0x%x) — refusing join\n",
                            payload_len >= offsetof(struct efs_msg_hello, port) +
                                          sizeof(h->port)
                                ? h->addr : "?",
                            payload_len >= offsetof(struct efs_msg_hello, port) +
                                          sizeof(h->port)
                                ? h->port : 0,
                            payload_len >= sizeof(struct efs_msg_hello)
                                ? h->build_id : "<pre-build-id>",
                            payload_len >= sizeof(h->version) ? h->version : 0,
                            EFS_BUILD_ID, EFS_VERSION_PACK);
                    efs_conn_send_msg(conn, EFS_MSG_HELLO_ACK, rej, sizeof(*rej));
                    free(rej);
                    break;
                }
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
                ack->reject_reason =
                    accepted ? EFS_HELLO_REJECT_NONE : EFS_HELLO_REJECT_FULL;
                strncpy(ack->build_id, EFS_BUILD_ID, sizeof(ack->build_id) - 1);
                ack->node_count = g_server->node_count;
                memcpy(ack->nodes, g_server->nodes, sizeof(g_server->nodes));
                /* Persist only when membership / addressing actually changed,
                 * and never under the lock (fsync would stall every handler). */
                if (changed)
                    server_nodes_mark_dirty(g_server);
                pthread_mutex_unlock(&g_server->lock);
                server_nodes_flush_dirty(g_server);
                efs_conn_send_msg(conn, EFS_MSG_HELLO_ACK, ack, sizeof(*ack));
                free(ack);
                /* Converge the placement ring: relay this membership change to
                 * every other peer (HELLO only updates the contacted node). */
                if (changed)
                    server_gossip_membership(g_server, h);
            }
            break;
        }
        case EFS_MSG_GET_CHUNK: {
            if (payload_len >= sizeof(struct efs_msg_get_chunk)) {
                struct efs_msg_get_chunk *req = payload;
                pthread_mutex_lock(&g_server->lock);
                struct efs_export *ex =
                    server_export_acquire_locked(g_server, req->export_id);
                pthread_mutex_unlock(&g_server->lock);

                uint32_t frag_len = server_frag_len(ex, req->ino);
                /* The fragment is read straight into the reply buffer — no
                 * per-GET malloc and no 64 KiB reply-assembly memcpy. Over
                 * RDMA the reply buffer is a registered QP send buffer, so
                 * the disk read is also the DMA source (zero data copies). */
                uint32_t need = 1 + EFS_HASH_SIZE + frag_len;
                uint8_t *reply = NULL;
                int rdma_buf = 0;
                if (chan == EFS_CONN_RDMA) {
                    reply = efs_rdma_send_buf(conn->rc, need);
                    if (reply)
                        rdma_buf = 1;
                }
                if (!reply) {
                    if (tls_reply_cap < need) {
                        uint8_t *nb = realloc(tls_reply, need);
                        if (!nb) {
                            uint8_t err = EFS_GET_CHUNK_ERROR;
                            efs_conn_send_msg(conn, EFS_MSG_GET_CHUNK_REPLY, &err, 1);
                            server_export_put(g_server, ex);
                            break;
                        }
                        tls_reply = nb;
                        tls_reply_cap = need;
                    }
                    reply = tls_reply;
                }
                uint32_t out_len = 1;
                if (!ex) {
                    reply[0] = EFS_GET_CHUNK_ERROR;
                } else {
                    uint32_t data_len = 0;
                    uint8_t *dptr = reply + 1 + EFS_HASH_SIZE;
                    int sum_ok = 0;
                    struct efs_store st;
                    struct efs_nvme_store nctx;
                    struct efs_frag_id fid = {
                        .export_id = ex->id,
                        .ino = req->ino,
                        .chunk_index = req->chunk_index,
                        .fragment_index = req->fragment_index,
                    };
                    data_len = server_frag_len(ex, req->ino);
                    efs_store_nvme_bind(&st, &nctx, g_server, ex);
                    rc = efs_store_get(&st, &fid, dptr, &data_len,
                                       reply + 1, &sum_ok);
                    if (rc != 0) {
                        reply[0] = EFS_GET_CHUNK_NOT_FOUND;
                    } else {
                        /* Server-side read-verify for data fragments: the
                         * .sum sidecar is already in hand, so one blake3
                         * detects disk rot on the read path without any
                         * client CPU. A mismatch fails this fragment (the
                         * client's 2+1 decode falls back to the other
                         * fragments). Meta pages skip this: their integrity
                         * is the EFSR root checksums. */
                        /* I25: a fragment whose payload hash does not match
                         * the recorded checksum is unavailable — never
                         * served. Repair is a control-plane job. */
                        if (sum_ok && !efs_ino_is_meta_table(req->ino)) {
                            uint8_t vh[EFS_HASH_SIZE];
                            efs_hash(dptr, data_len, vh);
                            if (memcmp(vh, reply + 1, EFS_HASH_SIZE) != 0) {
                                reply[0] = EFS_GET_CHUNK_NOT_FOUND;
                                out_len = 1;
                                goto send_reply;
                            }
                        }
                        if (!sum_ok)
                            efs_hash(dptr, data_len, reply + 1);
                        reply[0] = EFS_GET_CHUNK_OK;
                        out_len = 1 + EFS_HASH_SIZE + data_len;
                    }
                }
send_reply:
                if (rdma_buf)
                    efs_rdma_send_commit(conn->rc, reply,
                                         EFS_MSG_GET_CHUNK_REPLY, out_len);
                else
                    efs_conn_send_msg(conn, EFS_MSG_GET_CHUNK_REPLY, reply,
                                      out_len);
                server_export_put(g_server, ex);
            }
            break;
        }
        case EFS_MSG_PUT_CHUNK: {
            if (payload_len >= sizeof(struct efs_msg_put_chunk)) {
                struct efs_msg_put_chunk *req = payload;
                const uint8_t *data =
                    (const uint8_t *)payload + sizeof(struct efs_msg_put_chunk);
                pthread_mutex_lock(&g_server->lock);
                struct efs_export *ex =
                    server_export_acquire_locked(g_server, req->export_id);
                /* Auto-create export shell so meta-page PUTs can land before
                 * the EFSR root arrives (mkfs / first flush race). */
                if (!ex && g_server->export_count < EFS_MAX_EXPORTS) {
                    ex = &g_server->exports[g_server->export_count++];
                    efs_export_init(ex, req->export_id, "pending");
                    ex->id = req->export_id;
                    int nidx = server_export_index_locked(g_server, ex);
                    if (nidx >= 0)
                        g_server->export_inflight[nidx]++;
                }
                /* Learn data chunk_size from first non-meta PUT when still
                 * default (peer may not have applied EFSR yet). */
                if (ex && !efs_ino_is_meta_table(req->ino) &&
                    req->data_len > 0 &&
                    (ex->chunk_size == 0 ||
                     ex->chunk_size == EFS_DEFAULT_CHUNK_SIZE) &&
                    efs_chunk_size_valid(req->data_len * 2u) &&
                    req->data_len != EFS_META_FRAGMENT_SIZE) {
                    ex->chunk_size = req->data_len * 2u;
                }
                pthread_mutex_unlock(&g_server->lock);

                uint8_t reply = EFS_PUT_CHUNK_ERROR;
                if (ex) {
                    uint32_t expect = server_frag_len(ex, req->ino);
                    if (req->data_len != expect ||
                        payload_len < sizeof(*req) + req->data_len) {
                        reply = EFS_PUT_CHUNK_ERROR;
                    } else {
                        /* ACK after length + store. */
                        struct efs_store st;
                        struct efs_nvme_store nctx;
                        struct efs_frag_id fid = {
                            .export_id = ex->id,
                            .ino = req->ino,
                            .chunk_index = req->chunk_index,
                            .fragment_index = req->fragment_index,
                        };
                        efs_store_nvme_bind(&st, &nctx, g_server, ex);
                        rc = efs_store_put(&st, &fid, data, expect,
                                           req->checksum);
                        if (rc == 0) {
                            reply = EFS_PUT_CHUNK_OK;
                        } else if (rc == EFS_ERR_QUOTA) {
                            reply = EFS_PUT_CHUNK_QUOTA_EXCEEDED;
                        }
                    }
                }
                server_export_put(g_server, ex);
                efs_conn_send_msg(conn, EFS_MSG_PUT_CHUNK_REPLY, &reply, 1);
            }
            break;
        }
        case EFS_MSG_GC_FRAGMENT: {
            /* Data-plane GC (spec L7): checksum-conditional fragment
             * delete. Idempotent — absent/deleted/mismatch-gone all reply
             * 0; only a real I/O failure asks the reaper to retry. No
             * export auto-create: no export means no fragments, which is
             * "already gone". */
            if (payload_len >= sizeof(struct efs_msg_gc_fragment)) {
                struct efs_msg_gc_fragment *req = payload;
                struct efs_msg_gc_fragment_reply rep;
                struct efs_store st;
                struct efs_nvme_store nctx;
                struct efs_frag_id fid;
                int grc = EFS_OK;

                memset(&rep, 0, sizeof(rep));
                pthread_mutex_lock(&g_server->lock);
                struct efs_export *ex =
                    server_export_acquire_locked(g_server, req->export_id);
                pthread_mutex_unlock(&g_server->lock);
                if (ex) {
                    fid.export_id = ex->id;
                    fid.ino = req->ino;
                    fid.inode_generation = 0;
                    fid.chunk_generation = 0;
                    fid.chunk_index = req->chunk_index;
                    fid.fragment_index = req->fragment_index;
                    fid.coding_profile_id = 0;
                    efs_store_nvme_bind(&st, &nctx, g_server, ex);
                    grc = efs_store_del_if_sum(&st, &fid, req->checksum);
                    server_export_put(g_server, ex);
                }
                /* EFS_ERR_EXIST = a newer generation occupies the slot:
                 * the dead bytes are already gone, so that is terminal
                 * success for the reaper, not a retry. */
                rep.status = (grc == EFS_OK || grc == EFS_ERR_EXIST) ? 0 : 1;
                efs_conn_send_msg(conn, EFS_MSG_GC_FRAGMENT_REPLY, &rep,
                                  sizeof(rep));
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
            efs_conn_send_msg(conn, EFS_MSG_BENCH_PUT_REPLY, &reply, 1);
            break;
        }
        case EFS_MSG_LIST_NODES: {
            /* Local snapshot only. Do not RPC peers here: after a bounce,
             * meta-rebuild owns the peer pool and a 30s STATUS-per-peer
             * made LIST_NODES miss the client timeout (fuse could not mount).
             * efs-mgmt probes each advertised node itself. */
            struct efs_msg_list_nodes_reply *reply = calloc(1, sizeof(*reply));
            if (!reply)
                break;
            pthread_mutex_lock(&g_server->lock);
            server_dedupe_nodes_locked(g_server);
            uint32_t node_count = g_server->node_count;
            if (node_count > EFS_MAX_NODES)
                node_count = EFS_MAX_NODES;
            reply->node_count = node_count;
            memcpy(reply->nodes, g_server->nodes, sizeof(g_server->nodes));
            pthread_mutex_unlock(&g_server->lock);
            efs_conn_send_msg(conn, EFS_MSG_LIST_NODES_REPLY, reply, sizeof(*reply));
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
            /* The drain/shrink/leave state machine went away with the
             * migration engine (step 11); the wire field stays, always
             * EFS_NODE_STATE_ACTIVE. */
            reply.state = 0;
            pthread_mutex_unlock(&g_server->lock);
            efs_conn_send_msg(conn, EFS_MSG_STATUS_REPLY, &reply, sizeof(reply));
            break;
        }
        case EFS_MSG_HEAL_STATUS: {
            /* Step 11: no table rebuilds — the KV has no heal state. */
            struct efs_msg_heal_status_reply reply;
            memset(&reply, 0, sizeof(reply));
            efs_conn_send_msg(conn, EFS_MSG_HEAL_STATUS_REPLY, &reply,
                              sizeof(reply));
            break;
        }
        /* Drain/undrain/remove drove the old data-migration engine, which
         * is deleted (step 11). Node lifecycle will return as a raft
         * control-plane op; until then these must still REPLY (a silent
         * default: costs the sender a full recv timeout). */
        case EFS_MSG_DRAIN_NODE: {
            uint8_t reply = EFS_DRAIN_NODE_ERROR;
            efs_conn_send_msg(conn, EFS_MSG_DRAIN_NODE_REPLY, &reply, 1);
            break;
        }
        case EFS_MSG_UNDRAIN_NODE: {
            uint8_t reply = EFS_UNDRAIN_NODE_ERROR;
            efs_conn_send_msg(conn, EFS_MSG_UNDRAIN_NODE_REPLY, &reply, 1);
            break;
        }
        case EFS_MSG_REMOVE_NODE: {
            uint8_t reply = EFS_REMOVE_NODE_ERROR;
            efs_conn_send_msg(conn, EFS_MSG_REMOVE_NODE_REPLY, &reply, 1);
            break;
        }
        case EFS_MSG_SHRINK_QUOTA: {
            uint8_t reply = EFS_SHRINK_QUOTA_ERROR;
            if (payload_len >= sizeof(struct efs_msg_shrink_quota)) {
                struct efs_msg_shrink_quota *req = payload;
                pthread_mutex_lock(&g_server->lock);
                if (g_server->quota > 0 && g_server->quota > req->amount) {
                    uint64_t new_quota = g_server->quota - req->amount;
                    struct efs_node *local = server_local_node(g_server);
                    uint64_t used = local ? local->used : 0;
                    /* The data-migration engine that used to drain usage
                     * below the new quota is deleted (step 11). Shrinking
                     * below current usage is refused outright. */
                    if (used > new_quota) {
                        pthread_mutex_unlock(&g_server->lock);
                        efs_conn_send_msg(conn, EFS_MSG_SHRINK_QUOTA_REPLY,
                                          &reply, 1);
                        break;
                    }
                    g_server->quota = new_quota;
                    if (local)
                        local->quota = new_quota;
                    reply = EFS_SHRINK_QUOTA_IN_PROGRESS;
                }
                pthread_mutex_unlock(&g_server->lock);
            }
            efs_conn_send_msg(conn, EFS_MSG_SHRINK_QUOTA_REPLY, &reply, 1);
            break;
        }
        case EFS_MSG_ADD_STORAGE: {
            struct efs_msg_add_storage_reply r;
            memset(&r, 0, sizeof(r));
            r.status = EFS_ADD_STORAGE_INVALID;
            if (payload_len >= sizeof(struct efs_msg_add_storage)) {
                struct efs_msg_add_storage *req = payload;
                req->paths[sizeof(req->paths) - 1] = '\0';
                uint32_t n = 0;
                r.status = (uint8_t)server_add_storage_paths(g_server, req->paths, &n);
                r.path_count = n;
                if (r.status == EFS_ADD_STORAGE_OK && n == 0) {
                    pthread_mutex_lock(&g_server->lock);
                    r.path_count = g_server->storage_path_count;
                    pthread_mutex_unlock(&g_server->lock);
                }
            }
            efs_conn_send_msg(conn, EFS_MSG_ADD_STORAGE_REPLY, &r, sizeof(r));
            break;
        }
        case EFS_MSG_NODE_LEFT: {
            if (payload_len >= sizeof(struct efs_msg_node_left)) {
                struct efs_msg_node_left *msg = payload;
                /* Membership only. Fragment re-placement after a permanent
                 * loss is the control-plane repair protocol (spec §7.8),
                 * not a handler-side scan. */
                server_remove_node_from_cluster(g_server, msg->node_id);
                printf("Node %u left the cluster\n", msg->node_id);
            }
            break;
        }
        case EFS_MSG_JOIN: {
            if (payload_len >= sizeof(struct efs_msg_join)) {
                struct efs_msg_join *req = payload;
                uint8_t reply = EFS_JOIN_OK;
                if (server_join_cluster(g_server, req->peer_host, req->peer_port) != 0)
                    reply = EFS_JOIN_ERROR;
                efs_conn_send_msg(conn, EFS_MSG_JOIN_REPLY, &reply, 1);
            }
            break;
        }
        case EFS_MSG_INODE_LOOKUP:
        case EFS_MSG_INODE_CREATE:
        case EFS_MSG_INODE_CREATE_SHARD:
        case EFS_MSG_INODE_GETATTR:
        case EFS_MSG_INODE_UNLINK:
        case EFS_MSG_INODE_RENAME:
        case EFS_MSG_INODE_RENAME_AT:
        case EFS_MSG_INODE_SETATTR:
        case EFS_MSG_INODE_APPEND:
        case EFS_MSG_INODE_LINK:
        case EFS_MSG_INODE_LINK_SHARD:
        case EFS_MSG_INODE_UNLINK_SHARD:
        case EFS_MSG_INODE_HOLD:
        case EFS_MSG_INODE_FLOCK:
        case EFS_MSG_INODE_DROP_CHUNKS: {
            if ((type == EFS_MSG_INODE_LOOKUP || type == EFS_MSG_INODE_GETATTR ||
                 type == EFS_MSG_INODE_CREATE ||
                 type == EFS_MSG_INODE_CREATE_SHARD ||
                 type == EFS_MSG_INODE_UNLINK ||
                 type == EFS_MSG_INODE_UNLINK_SHARD ||
                 type == EFS_MSG_INODE_SETATTR ||
                 type == EFS_MSG_INODE_APPEND ||
                 type == EFS_MSG_INODE_LINK ||
                 type == EFS_MSG_INODE_LINK_SHARD ||
                 type == EFS_MSG_INODE_RENAME ||
                 type == EFS_MSG_INODE_RENAME_AT ||
                 type == EFS_MSG_INODE_HOLD ||
                 type == EFS_MSG_INODE_FLOCK ||
                 type == EFS_MSG_INODE_DROP_CHUNKS) &&
                server_raft_host_active()) {
                struct efs_msg_inode_reply r;
                uint8_t rtype;
                memset(&r, 0, sizeof(r));
                r.status = EFS_INODE_RPC_ERROR;
                if (type == EFS_MSG_INODE_LOOKUP &&
                    payload_len >= sizeof(struct efs_msg_inode_lookup)) {
                    struct efs_msg_inode_lookup *req = payload;
                    server_raft_host_lookup(req->parent, req->name, &r);
                    rtype = EFS_MSG_INODE_LOOKUP_REPLY;
                } else if (type == EFS_MSG_INODE_GETATTR &&
                           payload_len >= sizeof(struct efs_msg_inode_getattr)) {
                    struct efs_msg_inode_getattr *req = payload;
                    server_raft_host_getattr(req->ino, &r);
                    rtype = EFS_MSG_INODE_GETATTR_REPLY;
                } else if (type == EFS_MSG_INODE_CREATE &&
                           payload_len >= sizeof(struct efs_msg_inode_create)) {
                    struct efs_msg_inode_create *req = payload;
                    server_raft_host_create(req->parent, req->name, req->mode,
                                            req->uid, req->gid, &r);
                    rtype = EFS_MSG_INODE_CREATE_REPLY;
                } else if (type == EFS_MSG_INODE_CREATE_SHARD) {
                    /* Old fan-out. File create is one Raft entry; MKDIR is
                     * a 2-shard txn, not this opcode. */
                    r.status = EFS_INODE_RPC_INVAL;
                    rtype = EFS_MSG_INODE_CREATE_SHARD_REPLY;
                } else if (type == EFS_MSG_INODE_UNLINK &&
                           payload_len >= sizeof(struct efs_msg_inode_unlink)) {
                    struct efs_msg_inode_unlink *req = payload;
                    server_raft_host_unlink(req->parent, req->name, req->is_dir,
                                            &r);
                    rtype = EFS_MSG_INODE_UNLINK_REPLY;
                } else if (type == EFS_MSG_INODE_UNLINK_SHARD) {
                    /* Old fan-out. Last-link file unlink is one Raft entry;
                     * nlink>1 is still a 2-shard txn, not this opcode.
                     * RMDIR is EFS_MSG_INODE_UNLINK with a directory. */
                    r.status = EFS_INODE_RPC_INVAL;
                    rtype = EFS_MSG_INODE_UNLINK_SHARD_REPLY;
                } else if (type == EFS_MSG_INODE_SETATTR &&
                           payload_len >= sizeof(struct efs_msg_inode_setattr)) {
                    struct efs_msg_inode_setattr *req = payload;
                    server_raft_host_setattr(req->ino, req->mask, req->mode,
                                              req->uid, req->gid, req->size,
                                              req->mtime, req->mtime_nsec,
                                              req->atime, &r);
                    rtype = EFS_MSG_INODE_SETATTR_REPLY;
                } else if (type == EFS_MSG_INODE_APPEND &&
                           payload_len >= sizeof(struct efs_msg_inode_append)) {
                    struct efs_msg_inode_append *req = payload;
                    const uint8_t *su = NULL;
                    uint32_t se = 0;

                    rtype = EFS_MSG_INODE_APPEND_REPLY;
                    if (payload_len == sizeof(*req) + EFS_SESS_WIRE_LEN) {
                        su = (const uint8_t *)req + sizeof(*req);
                        memcpy(&se, su + 16, 4);
                        server_raft_host_append(req->ino, req->len, su, se, &r);
                    } else if (payload_len == sizeof(*req)) {
                        server_raft_host_append(req->ino, req->len, NULL, 0, &r);
                    } else {
                        r.status = EFS_INODE_RPC_INVAL;
                    }
                } else if (type == EFS_MSG_INODE_LINK &&
                           payload_len >= sizeof(struct efs_msg_inode_link)) {
                    struct efs_msg_inode_link *req = payload;
                    server_raft_host_link(req->src_ino, req->new_parent,
                                          req->new_name, &r);
                    rtype = EFS_MSG_INODE_LINK_REPLY;
                } else if (type == EFS_MSG_INODE_LINK_SHARD) {
                    /* Old fan-out. LINK is a 2-shard txn, not this opcode. */
                    r.status = EFS_INODE_RPC_INVAL;
                    rtype = EFS_MSG_INODE_LINK_SHARD_REPLY;
                } else if (type == EFS_MSG_INODE_RENAME) {
                    /* Rename-by-ino is not hosted; RENAME_AT is. */
                    r.status = EFS_INODE_RPC_INVAL;
                    rtype = EFS_MSG_INODE_RENAME_REPLY;
                } else if (type == EFS_MSG_INODE_RENAME_AT &&
                           payload_len >= sizeof(struct efs_msg_inode_rename_at)) {
                    struct efs_msg_inode_rename_at *req = payload;
                    server_raft_host_rename_at(req->old_parent, req->old_name,
                                               req->new_parent, req->new_name,
                                               &r);
                    rtype = EFS_MSG_INODE_RENAME_AT_REPLY;
                } else if (type == EFS_MSG_INODE_HOLD &&
                           payload_len >= sizeof(struct efs_msg_inode_hold)) {
                    struct efs_msg_inode_hold *req = payload;
                    const uint8_t *su = NULL;
                    uint32_t se = 0;

                    rtype = EFS_MSG_INODE_HOLD_REPLY;
                    if (payload_len == sizeof(*req) + EFS_SESS_WIRE_LEN) {
                        su = (const uint8_t *)req + sizeof(*req);
                        memcpy(&se, su + 16, 4);
                        server_raft_host_hold(req->ino, req->flags, req->owner,
                                              su, se, &r);
                    } else if (payload_len == sizeof(*req)) {
                        server_raft_host_hold(req->ino, req->flags, req->owner,
                                              NULL, 0, &r);
                    } else {
                        r.status = EFS_INODE_RPC_INVAL;
                    }
                } else if (type == EFS_MSG_INODE_FLOCK &&
                           payload_len >= sizeof(struct efs_msg_inode_flock)) {
                    struct efs_msg_inode_flock *req = payload;
                    uint64_t lstart = 0, lend = ~(uint64_t)0;
                    const uint8_t *su = NULL;
                    uint32_t se = 0;
                    uint32_t extra;

                    rtype = EFS_MSG_INODE_FLOCK_REPLY;
                    extra = payload_len - (uint32_t)sizeof(*req);
                    if (extra == EFS_FLOCK_RANGE_LEN ||
                        extra == EFS_FLOCK_RANGE_LEN + EFS_SESS_WIRE_LEN) {
                        memcpy(&lstart, (uint8_t *)req + sizeof(*req), 8);
                        memcpy(&lend, (uint8_t *)req + sizeof(*req) + 8, 8);
                    }
                    if (extra == EFS_SESS_WIRE_LEN) {
                        su = (const uint8_t *)req + sizeof(*req);
                        memcpy(&se, su + 16, 4);
                    } else if (extra == EFS_FLOCK_RANGE_LEN + EFS_SESS_WIRE_LEN) {
                        su = (const uint8_t *)req + sizeof(*req) +
                             EFS_FLOCK_RANGE_LEN;
                        memcpy(&se, su + 16, 4);
                    }
                    if (extra == 0 || extra == EFS_FLOCK_RANGE_LEN ||
                        extra == EFS_SESS_WIRE_LEN ||
                        extra == EFS_FLOCK_RANGE_LEN + EFS_SESS_WIRE_LEN) {
                        server_raft_host_flock(req->ino, req->op, req->owner,
                                               lstart, lend, su, se, &r);
                    } else {
                        r.status = EFS_INODE_RPC_INVAL;
                    }
                } else if (type == EFS_MSG_INODE_DROP_CHUNKS) {
                    /* Old-engine fragment GC fan-out. Chunk lifetime is
                     * owned by the KV apply layer (spec L7); nothing sends
                     * this in raft mode. */
                    r.status = EFS_INODE_RPC_INVAL;
                    rtype = EFS_MSG_INODE_DROP_CHUNKS_REPLY;
                } else {
                    r.status = EFS_INODE_RPC_INVAL;
                    rtype = (type == EFS_MSG_INODE_LOOKUP)
                                ? EFS_MSG_INODE_LOOKUP_REPLY
                                : (type == EFS_MSG_INODE_GETATTR)
                                      ? EFS_MSG_INODE_GETATTR_REPLY
                                      : (type == EFS_MSG_INODE_UNLINK)
                                            ? EFS_MSG_INODE_UNLINK_REPLY
                                            : (type == EFS_MSG_INODE_SETATTR)
                                                  ? EFS_MSG_INODE_SETATTR_REPLY
                                                  : (type == EFS_MSG_INODE_APPEND)
                                                        ? EFS_MSG_INODE_APPEND_REPLY
                                                        : (type == EFS_MSG_INODE_LINK)
                                                              ? EFS_MSG_INODE_LINK_REPLY
                                                              : EFS_MSG_INODE_CREATE_REPLY;
                }
                efs_conn_send_msg(conn, rtype, &r, sizeof(r));
                break;
            }
            /* Raft host is mandatory: server_raft_host_start fails loud at
             * startup, so a request can only land here if the host died —
             * drop it (the client retries and the operator sees the host
             * down) rather than guess a reply type. */
            break;
        }
        case EFS_MSG_INODE_LOOKUP_PATH: {
            struct efs_msg_inode_lookup_path_reply r;
            memset(&r, 0, sizeof(r));
            r.status = EFS_INODE_RPC_ERROR;
            if (payload_len < sizeof(struct efs_msg_inode_lookup_path)) {
                efs_conn_send_msg(conn, EFS_MSG_INODE_LOOKUP_PATH_REPLY, &r,
                                  sizeof(r));
                break;
            }
            if (server_raft_host_active()) {
                struct efs_msg_inode_lookup_path *req = payload;
                req->path[sizeof(req->path) - 1] = '\0';
                server_raft_host_lookup_path(req->start, req->path, &r);
            }
            efs_conn_send_msg(conn, EFS_MSG_INODE_LOOKUP_PATH_REPLY, &r,
                              sizeof(r));
            break;
        }
        case EFS_MSG_REPORT_CHUNKS: {
            /* Phase 3b Commutative: size grow-only, mtime newer-only.
             * Chunk recs route by chunk_shard_of(ino, index); inode size
             * recs stay on shard_of(ino). */
            struct efs_msg_inode_reply r;
            memset(&r, 0, sizeof(r));
            r.status = EFS_INODE_RPC_ERROR;
            int bad = 0;
            uint32_t count = 0;
            uint32_t ino_count = 0;
            const struct efs_chunk_rec *recs = NULL;
            const struct efs_ino_size_rec *irecs = NULL;
            if (payload_len >= sizeof(struct efs_msg_report_chunks)) {
                struct efs_msg_report_chunks *req = payload;
                ino_count = req->ino_count;
                size_t avail = payload_len - sizeof(*req);
                count = req->count;
                recs = (const struct efs_chunk_rec *)((const uint8_t *)payload +
                                                      sizeof(*req));
                irecs = (const struct efs_ino_size_rec *)(recs + count);
                if (count > avail / sizeof(struct efs_chunk_rec))
                    bad = 1; /* truncated payload */
                else if (ino_count >
                         (avail - (size_t)count * sizeof(struct efs_chunk_rec)) /
                             sizeof(struct efs_ino_size_rec))
                    bad = 1; /* truncated inode recs */
            } else {
                bad = 1;
            }
            if (server_raft_host_active()) {
                if (bad)
                    r.status = EFS_INODE_RPC_INVAL;
                else
                    server_raft_host_report(recs, count, irecs, ino_count, &r);
            }
            efs_conn_send_msg(conn, EFS_MSG_REPORT_CHUNKS_REPLY, &r,
                              sizeof(r));
            break;
        }
        case EFS_MSG_INODE_READDIR: {
            struct efs_msg_inode_readdir_reply r;
            memset(&r, 0, sizeof(r));
            r.status = EFS_INODE_RPC_ERROR;
            if (server_raft_host_active() &&
                payload_len >= sizeof(struct efs_msg_inode_readdir)) {
                struct efs_msg_inode_readdir *req = payload;
                server_raft_host_readdir(req->parent, req->max_ents,
                                         req->after_src, req->after_name,
                                         &r);
            }
            efs_conn_send_msg(conn, EFS_MSG_INODE_READDIR_REPLY, &r,
                              sizeof(r));
            break;
        }
        case EFS_MSG_INODE_GETCHUNKS: {
            struct efs_msg_inode_getchunks_reply r;
            memset(&r, 0, sizeof(r));
            r.status = EFS_INODE_RPC_ERROR;
            if (server_raft_host_active() &&
                payload_len >= sizeof(struct efs_msg_inode_getchunks)) {
                struct efs_msg_inode_getchunks *req = payload;
                server_raft_host_getchunks(req->ino, req->start, req->max, &r);
            }
            efs_conn_send_msg(conn, EFS_MSG_INODE_GETCHUNKS_REPLY, &r,
                              sizeof(r));
            break;
        }
        case EFS_MSG_RDMA_SETUP: {
            /* Arrived over TCP (the QP does not exist yet); the reply goes
             * back over TCP because recv_chan is TCP for this frame. */
            struct efs_msg_rdma_setup_reply rep;
            uint32_t rlen = sizeof(rep);
            efs_rdma_server_accept(conn, payload, payload_len, &rep, &rlen);
            efs_conn_send_msg(conn, EFS_MSG_RDMA_SETUP_REPLY, &rep, rlen);
            if (getenv("EFS_RDMA_FIRST")) {
                /* Tag the peer's port: a QP and the TCP socket that carried
                 * its handshake must belong to the same connection, and that
                 * is exactly what a crossed SETUP reply would break. */
                struct sockaddr_in pa;
                socklen_t pl = sizeof(pa);
                unsigned pport = 0;
                if (getpeername(conn->fd, (struct sockaddr *)&pa, &pl) == 0)
                    pport = ntohs(pa.sin_port);
                fprintf(stderr,
                        "rdma-first: SETUP done qpn=%u status=%u peer_port=%u "
                        "fd=%d, back to wait\n",
                        rep.qpn, rep.status, pport, conn->fd);
            }
            break;
        }
        case EFS_MSG_RAFT: {
            (void)server_raft_host_inbox(payload, payload_len);
            efs_conn_send_msg(conn, EFS_MSG_RAFT_REPLY, NULL, 0);
            break;
        }
        case EFS_MSG_RAFT_MKFS: {
            struct efs_msg_raft_mkfs_reply r;
            if (payload_len >= 1)
                server_raft_host_submit(payload, payload_len, &r);
            else
                server_raft_host_mkfs(&r);
            efs_conn_send_msg(conn, EFS_MSG_RAFT_MKFS_REPLY, &r, sizeof(r));
            break;
        }
        case EFS_MSG_RAFT_STATUS: {
            struct efs_msg_raft_status_reply r;
            server_raft_host_status(&r);
            efs_conn_send_msg(conn, EFS_MSG_RAFT_STATUS_REPLY, &r, sizeof(r));
            break;
        }
        /* --- Old-engine control plane, deleted with the snapshot/2PC
         * metadata engine (roadmap step 11). A deleted handler must still
         * REPLY — silence costs the sender a full recv timeout (the client
         * GET_FEATURES poll is 2 s x node_count and wedged every
         * .stats/.find stat for ~16 s until this was added). */
        case EFS_MSG_GET_FEATURES: {
            /* No per-export feature state in the KV yet; defaults apply. */
            struct efs_msg_features_reply r;
            memset(&r, 0, sizeof(r));
            r.features = EFS_FEATURES_DEFAULT;
            r.status = EFS_FEATURES_OK;
            efs_conn_send_msg(conn, EFS_MSG_GET_FEATURES_REPLY, &r,
                              sizeof(r));
            break;
        }
        case EFS_MSG_SET_FEATURES: {
            /* Feature toggles are not persisted in raft mode. */
            struct efs_msg_features_reply r;
            memset(&r, 0, sizeof(r));
            r.features = EFS_FEATURES_DEFAULT;
            r.status = EFS_FEATURES_NOT_FOUND;
            efs_conn_send_msg(conn, EFS_MSG_SET_FEATURES_REPLY, &r,
                              sizeof(r));
            break;
        }
        case EFS_MSG_QUERY_STATS: {
            /* Whole-table stats query went away with the table. */
            struct efs_msg_query_stats_reply r;
            memset(&r, 0, sizeof(r));
            efs_conn_send_msg(conn, EFS_MSG_QUERY_STATS_REPLY, &r,
                              sizeof(r));
            break;
        }
        case EFS_MSG_LIST_EXPORTS: {
            /* mgmt is raft-only (step 11 inc 5); until then, empty list. */
            struct efs_msg_list_exports_reply r;
            memset(&r, 0, sizeof(r));
            efs_conn_send_msg(conn, EFS_MSG_LIST_EXPORTS_REPLY, &r,
                              sizeof(r));
            break;
        }
        case EFS_MSG_CREATE_EXPORT: {
            uint8_t st = EFS_CREATE_EXPORT_REPLICATE_FAILED;
            efs_conn_send_msg(conn, EFS_MSG_CREATE_EXPORT_REPLY, &st, 1);
            break;
        }
        case EFS_MSG_DESTROY_EXPORT: {
            uint8_t st = 1; /* != EFS_DESTROY_EXPORT_OK */
            efs_conn_send_msg(conn, EFS_MSG_DESTROY_EXPORT_REPLY, &st, 1);
            break;
        }
        case EFS_MSG_UPGRADE_META: {
            struct efs_msg_upgrade_meta_reply r;
            memset(&r, 0, sizeof(r));
            r.status = 1; /* old-engine rehash is gone */
            efs_conn_send_msg(conn, EFS_MSG_UPGRADE_META_REPLY, &r,
                              sizeof(r));
            break;
        }
        case EFS_MSG_GET_META:
        case EFS_MSG_GET_META_ROOT:
            /* Empty reply = "no serializable export" (client maps it to
             * EFS_ERR_NOT_FOUND), never a hang. */
            efs_conn_send_msg(conn,
                              type == EFS_MSG_GET_META
                                  ? EFS_MSG_GET_META_REPLY
                                  : EFS_MSG_GET_META_ROOT_REPLY,
                              NULL, 0);
            break;
        case EFS_MSG_PUT_META: {
            struct efs_msg_put_meta_reply r;
            memset(&r, 0, sizeof(r));
            r.status = 1; /* not EFS_OK */
            efs_conn_send_msg(conn, EFS_MSG_PUT_META_REPLY, &r, sizeof(r));
            break;
        }
        case EFS_MSG_META_COMMIT: {
            uint8_t st = 1; /* never promoted: 2PC is gone */
            efs_conn_send_msg(conn, EFS_MSG_META_COMMIT_REPLY, &st, 1);
            break;
        }
        default:
            break;
        }

        if (rdma_frame)
            efs_rdma_recv_repost(conn->rc);
        free(to_free);
    }
}
