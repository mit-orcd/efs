#include "efs/common.h"
#include "efs/protocol.h"
#include "efs/network.h"
#include "efs/rdma.h"
#include "efs/checksum.h"
#include "server_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <poll.h>
#include <arpa/inet.h>

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

/* Phase 3: name ops use the parent shard; inode ops use the file shard.
 * shard_bits==0 returns `ex` (today's single table). */
static struct efs_export *table_for_ino(struct efs_export *ex, efs_ino_t ino)
{
    struct efs_export *tab = efs_export_table_for_ino(ex, ino);
    return tab ? tab : ex;
}

static uint32_t server_nlive_locked(struct efsd_server *s, efs_node_id_t *live)
{
    uint32_t n = 0;
    if (!s || !live)
        return 0;
    live[n++] = s->id;
    for (uint32_t i = 0; i < s->node_count && n < EFS_MAX_NODES; i++) {
        efs_node_id_t id = s->nodes[i].id;
        if (id == 0 || id == s->id)
            continue;
        if (server_node_is_down_locked(s, id))
            continue;
        live[n++] = id;
    }
    return n;
}

static efs_ino_t inode_rpc_key(uint8_t type, const void *payload)
{
    if (!payload)
        return EFS_ROOT_INO;
    switch (type) {
    case EFS_MSG_INODE_LOOKUP:
        return ((const struct efs_msg_inode_lookup *)payload)->parent;
    case EFS_MSG_INODE_CREATE:
        return ((const struct efs_msg_inode_create *)payload)->parent;
    case EFS_MSG_INODE_CREATE_SHARD:
        /* Ownership is the target shard, not the parent. target_shard < 2^bits
         * so efs_export_shard_of(target_shard) == target_shard. */
        return (efs_ino_t)((const struct efs_msg_inode_create_shard *)payload)
            ->target_shard;
    case EFS_MSG_INODE_UNLINK:
        return ((const struct efs_msg_inode_unlink *)payload)->parent;
    case EFS_MSG_INODE_GETATTR:
        return ((const struct efs_msg_inode_getattr *)payload)->ino;
    case EFS_MSG_INODE_SETATTR:
        return ((const struct efs_msg_inode_setattr *)payload)->ino;
    case EFS_MSG_INODE_APPEND:
        return ((const struct efs_msg_inode_append *)payload)->ino;
    case EFS_MSG_INODE_RENAME:
        return ((const struct efs_msg_inode_rename *)payload)->new_parent;
    case EFS_MSG_INODE_LINK:
        return ((const struct efs_msg_inode_link *)payload)->new_parent;
    case EFS_MSG_INODE_LINK_SHARD:
        return ((const struct efs_msg_inode_link_shard *)payload)->src_ino;
    case EFS_MSG_INODE_UNLINK_SHARD:
        return ((const struct efs_msg_inode_unlink_shard *)payload)->src_ino;
    case EFS_MSG_INODE_LOOKUP_PATH:
        return EFS_ROOT_INO;
    default:
        return EFS_ROOT_INO;
    }
}

static efs_node_id_t server_shard_owner_id_locked(struct efsd_server *s,
                                                 struct efs_export *ex,
                                                 uint8_t type,
                                                 const void *payload)
{
    if (!s)
        return 0;
    if (!ex || ex->root.shard_bits == 0 || ex->root.shard_count <= 1)
        return server_meta_primary_id_locked(s);
    efs_node_id_t live[EFS_MAX_NODES];
    uint32_t nlive = server_nlive_locked(s, live);
    uint32_t shard;
    /* CREATE_SHARD names the shard directly. Do not run it through
     * efs_export_shard_of: target_shard==1 would collide with ROOT_INO. */
    if (type == EFS_MSG_INODE_CREATE_SHARD && payload)
        shard = ((const struct efs_msg_inode_create_shard *)payload)
                    ->target_shard;
    else
        shard = efs_export_shard_of(inode_rpc_key(type, payload),
                                   ex->root.shard_bits);
    return efs_shard_owner_of(shard, ex->root.shard_count, live, nlive);
}

static int server_owns_req_locked(struct efsd_server *s, struct efs_export *ex,
                                  uint8_t type, const void *payload)
{
    efs_node_id_t owner = server_shard_owner_id_locked(s, ex, type, payload);
    return owner != 0 && owner == s->id;
}

static int server_node_addr_locked(struct efsd_server *s, efs_node_id_t id,
                                   char *host, size_t host_sz, uint16_t *port)
{
    if (!s || !host || !port || host_sz == 0)
        return -1;
    if (id == s->id) {
        strncpy(host, s->addr, host_sz - 1);
        host[host_sz - 1] = '\0';
        *port = s->port;
        return 0;
    }
    for (uint32_t i = 0; i < s->node_count; i++) {
        if (s->nodes[i].id == id) {
            strncpy(host, s->nodes[i].addr, host_sz - 1);
            host[host_sz - 1] = '\0';
            *port = s->nodes[i].port;
            return 0;
        }
    }
    return -1;
}

/* Nested inode RPC on an already-resolved peer. Caller must NOT hold
 * s->lock (the peer handler takes its own). */
static int server_peer_inode_rpc(const char *host, uint16_t port,
                                 uint8_t req_type, const void *req,
                                 uint32_t req_len, uint8_t reply_type,
                                 struct efs_msg_inode_reply *out)
{
    if (!host || !port || !req || !out)
        return -1;
    int fd = server_peer_conn_get(host, port);
    if (fd < 0)
        return -1;
    if (efs_send_msg(fd, req_type, req, req_len) != 0) {
        server_peer_conn_drop(host, port, fd);
        return -1;
    }
    uint8_t type = 0;
    void *payload = NULL;
    uint32_t plen = 0;
    if (efs_recv_msg(fd, &type, &payload, &plen) != 0) {
        server_peer_conn_drop(host, port, fd);
        return -1;
    }
    if (type != reply_type || plen < sizeof(*out)) {
        free(payload);
        server_peer_conn_drop(host, port, fd);
        return -1;
    }
    memcpy(out, payload, sizeof(*out));
    free(payload);
    server_peer_conn_release(host, port, fd);
    return 0;
}

/* Nested CREATE_SHARD on an already-resolved peer. Caller must NOT hold
 * s->lock (the peer handler takes its own). */
static int server_peer_create_shard(const char *host, uint16_t port,
                                    const struct efs_msg_inode_create_shard *req,
                                    struct efs_msg_inode_reply *out)
{
    return server_peer_inode_rpc(host, port, EFS_MSG_INODE_CREATE_SHARD, req,
                                 sizeof(*req), EFS_MSG_INODE_CREATE_SHARD_REPLY,
                                 out);
}

/* Wait for the next request on either channel of an RDMA-capable conn.
 * Returns EFS_CONN_TCP / EFS_CONN_RDMA, or -1 on error / peer close.
 * Pure-TCP conns return EFS_CONN_TCP immediately (caller blocks in recv). */
static int conn_wait_request(struct efs_conn *conn)
{
    if (!conn->rc)
        return EFS_CONN_TCP;
    struct efs_rdma_conn *rc = conn->rc;
    for (;;) {
        /* Quick check only: a server conn thread has nothing to gain from
         * spin-polling the ring — with dozens of live conns the aggregate
         * spin was ~13 cores/server under a 9-client write load. It blocks
         * on the eventfd below; the wakeup costs ~2us, nothing next to the
         * per-request work. The client's latency-critical reply path keeps
         * the adaptive spin. */
        int r = efs_rdma_reply_ready_quick(rc);
        if (r < 0)
            return -1;
        if (r > 0)
            return EFS_CONN_RDMA;
        struct pollfd p = { .fd = conn->fd, .events = POLLIN };
        if (poll(&p, 1, 0) < 0)
            return -1;
        if (p.revents & (POLLERR | POLLHUP | POLLNVAL))
            return -1;
        if (p.revents & POLLIN)
            return EFS_CONN_TCP;
        /* Neither channel ready: block on the TCP fd + the CQ channel. */
        struct pollfd pf[2] = {
            { .fd = conn->fd, .events = POLLIN },
            { .fd = efs_rdma_reply_fd(rc), .events = POLLIN },
        };
        int br = poll(pf, 2, -1);
        if (br < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (pf[0].revents & (POLLERR | POLLHUP | POLLNVAL))
            return -1;
        if (pf[0].revents & POLLIN)
            return EFS_CONN_TCP;
        if (pf[1].revents & POLLIN)
            return EFS_CONN_RDMA; /* recv_wait harvests the CQ event */
    }
}

static uint64_t handler_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)ts.tv_nsec / 1000000ull;
}

/* A pending flush election blocks other writers only while unexpired AND
 * not yet committed: once the winner's root lands (committed >= election
 * gen) the slot must not fence out the next flush. Caller holds s->lock. */
static int meta_writer_live(struct efsd_server *s, int ei, uint64_t now_ms)
{
    uint64_t committed = s->exports[ei].meta_fragmented
                             ? s->exports[ei].root.generation : 0;
    return s->meta_writer_id[ei] != 0 &&
           now_ms < s->meta_writer_expiry[ei] &&
           s->meta_writer_gen[ei] > committed;
}

/* Drop queued contenders that stopped re-BEGINing (crashed or gave up).
 * Caller holds s->lock. */
static void meta_writer_q_expire(struct efsd_server *s, int ei, uint64_t now_ms)
{
    uint32_t n = s->meta_writer_q_len[ei];
    uint32_t out = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (now_ms - s->meta_writer_q_ms[ei][i] >= EFS_META_WRITER_Q_EXPIRY_MS)
            continue;
        if (out != i) {
            s->meta_writer_q[ei][out] = s->meta_writer_q[ei][i];
            s->meta_writer_q_ms[ei][out] = s->meta_writer_q_ms[ei][i];
        }
        out++;
    }
    s->meta_writer_q_len[ei] = out;
}

/* Returns 0-based queue position of writer_id, adding/refreshing its entry.
 * Caller holds s->lock. */
static uint32_t meta_writer_q_touch(struct efsd_server *s, int ei,
                                    uint64_t writer_id, uint64_t now_ms)
{
    uint32_t n = s->meta_writer_q_len[ei];
    for (uint32_t i = 0; i < n; i++) {
        if (s->meta_writer_q[ei][i] == writer_id) {
            s->meta_writer_q_ms[ei][i] = now_ms;
            return i;
        }
    }
    if (n < EFS_META_WRITER_QMAX) {
        s->meta_writer_q[ei][n] = writer_id;
        s->meta_writer_q_ms[ei][n] = now_ms;
        s->meta_writer_q_len[ei] = n + 1;
    }
    return n; /* at the end (or unrecorded when full) */
}

/* Remove writer_id from the queue (on grant). Caller holds s->lock. */
static void meta_writer_q_remove(struct efsd_server *s, int ei,
                                 uint64_t writer_id)
{
    uint32_t n = s->meta_writer_q_len[ei];
    for (uint32_t i = 0; i < n; i++) {
        if (s->meta_writer_q[ei][i] == writer_id) {
            for (uint32_t j = i + 1; j < n; j++) {
                s->meta_writer_q[ei][j - 1] = s->meta_writer_q[ei][j];
                s->meta_writer_q_ms[ei][j - 1] = s->meta_writer_q_ms[ei][j];
            }
            s->meta_writer_q_len[ei] = n - 1;
            return;
        }
    }
}

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

        int chan = conn_wait_request(conn);
        if (chan < 0)
            break;
        conn->recv_chan = chan;

        if (chan == EFS_CONN_RDMA) {
            /* The payload aliases a QP recv pool buffer; it is reposted
             * after the switch (every handler consumes it synchronously). */
            if (efs_rdma_recv_wait(conn->rc, -1) != 0)
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
                    rc = server_read_fragment_with_sum(
                        g_server, ex, req->ino, req->chunk_index,
                        req->fragment_index, dptr, &data_len,
                        reply + 1, &sum_ok);
                    if (rc != 0) {
                        reply[0] = EFS_GET_CHUNK_NOT_FOUND;
                    } else {
                        /* Server-side read-verify for data fragments: the
                         * .sum sidecar is already in hand, so one blake3
                         * detects disk rot on the read path without any
                         * client CPU. A mismatch fails this fragment (the
                         * client's 2+1 decode falls back to the other
                         * fragments) and queues a heal. Meta pages skip this:
                         * their integrity is the EFSR root checksums, and
                         * dual-slot churn would obsolete the jobs anyway.
                         * Default on; EFS_SERVER_READ_VERIFY=0 disables. */
                        static int srv_verify = -1;
                        if (srv_verify < 0) {
                            const char *v = getenv("EFS_SERVER_READ_VERIFY");
                            srv_verify = !(v && *v && strcmp(v, "0") == 0);
                        }
                        if (srv_verify && sum_ok &&
                            !efs_ino_is_meta_table(req->ino)) {
                            uint8_t vh[EFS_HASH_SIZE];
                            efs_hash(dptr, data_len, vh);
                            if (memcmp(vh, reply + 1, EFS_HASH_SIZE) != 0) {
                                server_verify_enqueue(g_server, req->export_id,
                                                      req->ino,
                                                      req->chunk_index,
                                                      req->fragment_index,
                                                      data_len, reply + 1);
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
                int put_state = g_server->state;
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
                        /* ACK after length + store. Blake3 is async (verify
                         * thread); a mismatch heals this replica from peers. */
                        uint8_t zero_ck[EFS_HASH_SIZE];
                        efs_hash_zero_fragment_len(expect, zero_ck);
                        int is_zero = (memcmp(req->checksum, zero_ck,
                                              EFS_HASH_SIZE) == 0);
                        rc = server_write_fragment_with_sum(
                            g_server, ex, req->ino, req->chunk_index,
                            req->fragment_index, data, expect, req->checksum);
                        if (rc == 0) {
                            reply = EFS_PUT_CHUNK_OK;
                            if (!is_zero)
                                server_verify_enqueue(
                                    g_server, req->export_id, req->ino,
                                    req->chunk_index, req->fragment_index,
                                    expect, req->checksum);
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
        case EFS_MSG_GET_META: {
            pthread_mutex_lock(&g_server->lock);
            struct efs_export *ex = NULL;
            if (payload_len > 0) {
                char want[EFS_MAX_NAME];
                memset(want, 0, sizeof(want));
                memcpy(want, payload,
                       payload_len < EFS_MAX_NAME ? payload_len : EFS_MAX_NAME - 1);
                /* Read path must not mint exports: exact match only. A
                 * named request that matches nothing gets an empty reply
                 * (NOT_FOUND) — never the primary export's table, so no
                 * client can accidentally attach to the wrong export. */
                ex = server_find_export_no_create(g_server, want);
            } else if (g_server->export_count > 0) {
                /* Unnamed GET_META (server catchup, discovery): primary. */
                ex = &g_server->exports[0];
            }
            char *buf = NULL;
            size_t len = 0;
            if (ex) {
                uint64_t gen = ex->meta_fragmented ? ex->root.generation : 0;
                if (ex->gm_blob && ex->gm_gen == gen &&
                    ex->gm_epoch == g_server->epoch) {
                    /* Serve the cached serialize: a fresh 1+ GiB
                     * efs_export_serialize is seconds under this lock
                     * (strnlen per name), and resync storms pinned the
                     * server at 100% CPU serializing the same generation.
                     * The memcpy is ~0.1 s and 15x cheaper. */
                    buf = malloc(ex->gm_blob_len);
                    if (buf) {
                        memcpy(buf, ex->gm_blob, ex->gm_blob_len);
                        len = ex->gm_blob_len;
                    }
                } else {
                    /* Snapshot + pack unlocked. A live serialize of an
                     * 800k-chunk table is seconds under this lock and
                     * stalls every PUT while peers GET_META after a flush. */
                    struct efs_export snap;
                    memset(&snap, 0, sizeof(snap));
                    int do_tab = (ex->inode_count > 0 && !ex->meta_needs_rebuild) ||
                                 !ex->meta_fragmented;
                    int do_root = ex->meta_fragmented && ex->root.page_count > 0;
                    char *rbuf = NULL;
                    size_t rlen = 0;
                    if (do_root)
                        efs_export_root_serialize(&ex->root, &rbuf, &rlen);
                    if (do_tab)
                        efs_export_ensure_rollups(ex);
                    int src = do_tab ? efs_export_table_snapshot(ex, &snap)
                                     : EFS_OK;
                    pthread_mutex_unlock(&g_server->lock);
                    char *ebuf = NULL;
                    size_t elen = 0;
                    if (src == EFS_OK && do_tab)
                        efs_export_serialize(&snap, &ebuf, &elen);
                    efs_export_table_snapshot_free(&snap);
                    if (rbuf && ebuf) {
                        buf = malloc(rlen + elen);
                        if (buf) {
                            memcpy(buf, rbuf, rlen);
                            memcpy(buf + rlen, ebuf, elen);
                            len = rlen + elen;
                        }
                    } else if (rbuf) {
                        buf = rbuf;
                        rbuf = NULL;
                        len = rlen;
                    } else if (ebuf) {
                        buf = ebuf;
                        ebuf = NULL;
                        len = elen;
                    }
                    free(rbuf);
                    free(ebuf);
                    pthread_mutex_lock(&g_server->lock);
                    /* Cache for this gen if the export is still the same. */
                    if (ex && buf && len) {
                        uint64_t ngen = ex->meta_fragmented ? ex->root.generation
                                                            : 0;
                        if (ngen == gen) {
                            free(ex->gm_blob);
                            ex->gm_blob = malloc(len);
                            if (ex->gm_blob) {
                                memcpy(ex->gm_blob, buf, len);
                                ex->gm_blob_len = len;
                                ex->gm_gen = gen;
                                ex->gm_epoch = g_server->epoch;
                            } else {
                                ex->gm_blob = NULL;
                            }
                        }
                    }
                }
            }
            pthread_mutex_unlock(&g_server->lock);
            if (buf) {
                efs_conn_send_msg(conn, EFS_MSG_GET_META_REPLY, buf, (uint32_t)len);
                free(buf);
            } else {
                efs_conn_send_msg(conn, EFS_MSG_GET_META_REPLY, NULL, 0);
            }
            break;
        }
        case EFS_MSG_GET_META_ROOT: {
            pthread_mutex_lock(&g_server->lock);
            struct efs_export *ex = NULL;
            if (payload_len > 0) {
                char want[EFS_MAX_NAME];
                memset(want, 0, sizeof(want));
                memcpy(want, payload,
                       payload_len < EFS_MAX_NAME ? payload_len : EFS_MAX_NAME - 1);
                ex = server_find_export_no_create(g_server, want);
            } else if (g_server->export_count > 0) {
                ex = &g_server->exports[0];
            }
            char *buf = NULL;
            size_t len = 0;
            if (ex && ex->meta_fragmented)
                efs_export_root_serialize(&ex->root, &buf, &len);
            pthread_mutex_unlock(&g_server->lock);
            if (buf) {
                efs_conn_send_msg(conn, EFS_MSG_GET_META_ROOT_REPLY, buf,
                                  (uint32_t)len);
                free(buf);
            } else {
                efs_conn_send_msg(conn, EFS_MSG_GET_META_ROOT_REPLY, NULL, 0);
            }
            break;
        }
        case EFS_MSG_META_FLUSH_BEGIN: {
            struct efs_msg_meta_flush_begin_reply br;
            memset(&br, 0, sizeof(br));
            br.status = EFS_PUT_META_ERROR;
            if (payload_len >= sizeof(struct efs_msg_meta_flush_begin)) {
                struct efs_msg_meta_flush_begin b;
                memcpy(&b, payload, sizeof(b));
                pthread_mutex_lock(&g_server->lock);
                struct efs_export *ex = b.export_id
                                            ? server_get_export(g_server,
                                                                b.export_id)
                                            : NULL;
                if (!ex) {
                    /* Export not established here yet (fresh server / new
                     * export): nothing committed for it; accept. */
                    br.status = EFS_PUT_META_OK;
                    br.committed_gen = 0;
                } else {
                    int ei = server_export_index_locked(g_server, ex);
                    uint64_t committed = ex->meta_fragmented
                                             ? ex->root.generation : 0;
                    br.committed_gen = committed;
                    if (b.gen <= committed) {
                        br.status = EFS_PUT_META_STALE;
                    } else {
                        uint64_t now = handler_now_ms();
                        /* Expire a dead holder so the queue can advance. */
                        if (g_server->meta_writer_id[ei] != 0 &&
                            !meta_writer_live(g_server, ei, now)) {
                            g_server->meta_writer_id[ei] = 0;
                            g_server->meta_writer_since[ei] = 0;
                        }
                        meta_writer_q_expire(g_server, ei, now);
                        /* Yield an over-held election: a holder re-BEGINing
                         * (flush retry) past EFS_META_WRITER_MAX_HOLD_MS while
                         * contenders queue is moved to the back of the FIFO and
                         * the election freed, so the head-of-queue writer gets
                         * the next grant. Without this a wedged/slow writer
                         * re-grants itself forever and starves the cluster.
                         * Only under contention (q_len > 0): a lone writer is
                         * never yielded. Its partial pages are fenced by the
                         * PUT_META commit gate and overwritten next flush. */
                        if (g_server->meta_writer_id[ei] == b.writer_id &&
                            g_server->meta_writer_since[ei] != 0 &&
                            now - g_server->meta_writer_since[ei] >
                                EFS_META_WRITER_MAX_HOLD_MS &&
                            g_server->meta_writer_q_len[ei] > 0) {
                            uint64_t old = g_server->meta_writer_id[ei];
                            g_server->meta_writer_id[ei] = 0;
                            g_server->meta_writer_since[ei] = 0;
                            meta_writer_q_touch(g_server, ei, old, now);
                        }
                        if (g_server->meta_writer_id[ei] == b.writer_id) {
                            /* Holder re-BEGINing (retry): re-grant. */
                            g_server->meta_writer_gen[ei] = b.gen;
                            g_server->meta_writer_expiry[ei] =
                                now + EFS_META_WRITER_EXPIRY_MS;
                            br.status = EFS_PUT_META_OK;
                        } else if (g_server->meta_writer_id[ei] == 0 &&
                                   (g_server->meta_writer_q_len[ei] == 0 ||
                                    g_server->meta_writer_q[ei][0] ==
                                        b.writer_id)) {
                            /* Election free and we are at the head of the
                             * FIFO (or nobody waits): grant. */
                            meta_writer_q_remove(g_server, ei, b.writer_id);
                            g_server->meta_writer_id[ei] = b.writer_id;
                            g_server->meta_writer_gen[ei] = b.gen;
                            g_server->meta_writer_expiry[ei] =
                                now + EFS_META_WRITER_EXPIRY_MS;
                            g_server->meta_writer_since[ei] = now;
                            br.status = EFS_PUT_META_OK;
                        } else {
                            /* Held by another live writer, or others are
                             * ahead in the FIFO: wait for your turn. */
                            meta_writer_q_touch(g_server, ei, b.writer_id,
                                                now);
                            br.status = EFS_PUT_META_BUSY;
                        }
                    }
                }
                pthread_mutex_unlock(&g_server->lock);
            }
            /* Keep the pooled conn alive for the client's page/root PUTs. */
            efs_conn_send_msg(conn, EFS_MSG_META_FLUSH_BEGIN_REPLY,
                              &br, sizeof(br));
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
                        /* Multi-export: the root lands in ITS OWN export's
                         * slot (find-or-create by id). Keying off exports[0]
                         * morphed slot 0 into whatever root arrived last and
                         * diverged multi-export clusters. */
                        struct efs_export *ex =
                            server_get_export_create(g_server, root.id,
                                                     root.name);
                        if (!ex) {
                            pthread_mutex_unlock(&g_server->lock);
                            efs_export_root_free(&root);
                            reply = EFS_PUT_META_ERROR;
                            efs_conn_send_msg(conn, EFS_MSG_PUT_META_REPLY,
                                              &reply, 1);
                            break;
                        }
                        int ei = server_export_index_locked(g_server, ex);
                        /* Strict monotonic CAS: once we hold an EFSR, reject
                         * any generation we have already seen (<=). Equal gen
                         * from a second writer would collide in the same
                         * dual-slot pages with different content (split-brain);
                         * a lagging writer gets STALE and must re-fetch the max
                         * gen. A forward jump (gap>1) is NOT split-brain: the
                         * single metadata writer advances gen every flush, and
                         * a peer that missed intermediate gens (catch-up lag or
                         * dropped PUT_META) legitimately observes a gap. Accept
                         * any strictly-newer gen; the fence+rebuild below brings
                         * the tables convergent with the newest root. */
                        if (ex->meta_fragmented &&
                            root.generation <= ex->root.generation) {
                            /* Same-gen root with identical shard-0 pages and
                             * extra-shard descriptors: an extra-shard owner's
                             * extras refresh (it must NOT bump the main gen —
                             * the primary is the sole shard-0 writer). Merge
                             * the descriptors; our live tables stay put. */
                            if (root.generation == ex->root.generation &&
                                root.extra_shard_count > 0 &&
                                efs_export_root_same_pages(&root, &ex->root)) {
                                efs_export_merge_extra_roots(ex, &root);
                                g_server->export_meta_dirty = 1;
                                server_save_export(g_server, ex);
                                g_server->export_meta_dirty = 0;
                                reply = EFS_PUT_META_OK;
                            } else {
                                reply = EFS_PUT_META_STALE;
                            }
                            pthread_mutex_unlock(&g_server->lock);
                            efs_export_root_free(&root);
                        } else if (root.write_lease_id &&
                                   (!meta_writer_live(g_server, ei,
                                                      handler_now_ms()) ||
                                    g_server->meta_writer_id[ei] !=
                                        root.write_lease_id)) {
                            /* A stamped writer must CURRENTLY hold the flush
                             * election to commit. "No live holder" used to
                             * pass too: a writer whose lease lapsed mid-flush
                             * committed late, over pages a newer holder was
                             * already PUTting for a same-parity generation —
                             * dual-slot tear. STALE makes the loser resync
                             * and retry. Roots with no lease id (mkfs,
                             * legacy tools) skip the gate entirely. */
                            reply = EFS_PUT_META_STALE;
                            pthread_mutex_unlock(&g_server->lock);
                            efs_export_root_free(&root);
                        } else if (ex->meta_fragmented &&
                                   efs_export_root_same_pages(&root,
                                                              &ex->root)) {
                            /* Newer gen but identical shard-0 pages: the
                             * sender added no shard-0 content (extras-only
                             * refresh that crossed a gen, or a redundant
                             * re-commit of our own root). Adopt the gen so
                             * future commits stay monotonic, merge the extra
                             * descriptors — but keep our live shard-0 table,
                             * placement, next_ino and next_ci. Fencing here
                             * is what wiped unflushed RPC ops on the primary
                             * (mc_stress: appfile/cdir data loss). */
                            ex->root.generation = root.generation;
                            efs_export_merge_extra_roots(ex, &root);
                            g_server->epoch++;
                            g_server->export_meta_dirty = 1;
                            server_save_export(g_server, ex);
                            g_server->export_meta_dirty = 0;
                            reply = EFS_PUT_META_OK;
                            pthread_mutex_unlock(&g_server->lock);
                            efs_export_root_free(&root);
                        } else if (root.extra_shard_count > 0 &&
                                   ex->inode_count > 0) {
                            /* Extra-shard owner sent a root whose shard-0
                             * pages don't match (stale catchup copy, or it
                             * briefly thought it was primary and bumped
                             * gen). Never fence a live table for extras —
                             * that dropped unflushed mkdir/dentry ops and
                             * made sharded dirs vanish mid-create. */
                            efs_export_merge_extra_roots(ex, &root);
                            g_server->export_meta_dirty = 1;
                            server_save_export(g_server, ex);
                            g_server->export_meta_dirty = 0;
                            reply = EFS_PUT_META_OK;
                            pthread_mutex_unlock(&g_server->lock);
                            efs_export_root_free(&root);
                        } else {
                            uint64_t old_gen = ex->root.generation;
                            uint32_t old_ino_pc = ex->root.ino_page_count
                                                      ? ex->root.ino_page_count
                                                      : ex->root.page_count;
                            uint32_t old_ch_pc = ex->root.chunk_page_count;
                            uint32_t new_ino_pc = root.ino_page_count
                                                      ? root.ino_page_count
                                                      : root.page_count;
                            uint32_t new_ch_pc = root.chunk_page_count;
                            int had_frag = ex->meta_fragmented;
                            uint32_t prev_features = ex->root.features;
                            /* CoW (EFSR v7): snapshot the outgoing and incoming
                             * roots' page_cis[] so the GC runs lock-free on
                             * stable copies (a concurrent PUT_META could move
                             * ex->root once we drop the lock). */
                            uint32_t *old_cis = NULL, *new_cis = NULL;
                            uint32_t old_cis_count = 0, new_cis_count = 0;
                            if (ex->root.page_cis && ex->root.page_count > 0) {
                                old_cis_count = ex->root.page_count;
                                old_cis = malloc((size_t)old_cis_count *
                                                 sizeof(uint32_t));
                                if (old_cis)
                                    memcpy(old_cis, ex->root.page_cis,
                                           (size_t)old_cis_count *
                                               sizeof(uint32_t));
                                else
                                    old_cis_count = 0;
                            }
                            if (root.page_cis && root.page_count > 0) {
                                new_cis_count = root.page_count;
                                new_cis = malloc((size_t)new_cis_count *
                                                 sizeof(uint32_t));
                                if (new_cis)
                                    memcpy(new_cis, root.page_cis,
                                           (size_t)new_cis_count *
                                               sizeof(uint32_t));
                                else
                                    new_cis_count = 0;
                            }
                            ex->meta_fragmented = 1;
                            /* Never let an adopt orphan a shard: carry
                             * forward extra-shard descriptors the incoming
                             * root lacks (per-shard higher gen wins). */
                            (void)efs_export_root_maxmerge_extras(&root,
                                                                  &ex->root);
                            efs_export_root_move(&ex->root, &root);
                            ex->id = ex->root.id;
                            strncpy(ex->name, ex->root.name, EFS_MAX_NAME - 1);
                            ex->next_ino = ex->root.next_ino;
                            if (efs_chunk_size_valid(ex->root.chunk_size))
                                ex->chunk_size = ex->root.chunk_size;
                            /* Features are server-owned: ignore the client's
                             * root.features so a flush can't revert an admin
                             * toggle. A fresh export (no prior committed root)
                             * adopts the incoming value; thereafter only
                             * SET_FEATURES changes it. */
                            if (had_frag)
                                ex->root.features = prev_features;
                            else if (ex->root.features == 0)
                                ex->root.features = EFS_FEATURES_DEFAULT;
                            ex->features = ex->root.features;
                            /* Never rebuild on this handler thread: peer page
                             * fetches would block the pooled client connection
                             * and can cascade into multi-node stalls. Fence the
                             * now-stale inode/chunk tables so migrate/stats
                             * cannot act on maps from the old generation. */
                            if (ex->root.page_count > 0) {
                                ex->meta_needs_rebuild = 1;
                                ex->chunk_count = 0;
                                ex->inode_count = 0;
                            }
                            /* Selective: a shard table we still write (dirty,
                             * or same/newer gen) keeps its state; only
                             * genuinely newer foreign descriptors land. */
                            efs_export_merge_extra_roots(ex, &ex->root);
                            g_server->epoch++;
                            g_server->export_meta_dirty = 1;
                            /* New generation: drop the GET_META cache. */
                            free(ex->gm_blob);
                            ex->gm_blob = NULL;
                            /* Commit consumes the flush election. */
                            g_server->meta_writer_id[ei] = 0;
                            g_server->meta_writer_since[ei] = 0;
                            /* Save while holding the lock: concurrent PUT_META
                             * can efs_export_root_move and free page_checksums
                             * under a raced unlocked save (SIGSEGV). */
                            server_save_export(g_server, ex);
                            g_server->export_meta_dirty = 0;
                            uint64_t new_gen = ex->root.generation;
                            pthread_mutex_unlock(&g_server->lock);
                            reply = EFS_PUT_META_OK;
                            /* Client-driven root flip: reclaim the retired
                             * generation's metadata pages (best-effort). */
                            if (old_gen != new_gen) {
                                if (old_cis && new_cis) {
                                    /* Old root was CoW (EFSR v7): reclaim the
                                     * cis it referenced but the new root no
                                     * longer does. */
                                    server_gc_meta_cow_pages(g_server, ex,
                                                             EFS_META_TABLE_INO,
                                                             old_cis,
                                                             old_cis_count,
                                                             new_cis,
                                                             new_cis_count);
                                } else if (had_frag &&
                                           (old_ino_pc > 0 || old_ch_pc > 0)) {
                                    /* Old root was dual-slot (v6 and earlier):
                                     * drop only the retired gen's out-of-range
                                     * pages (in-range fragments stay for reuse
                                     * by dirty-page skip references). */
                                    server_gc_meta_slot_pages(g_server, ex,
                                                              old_gen,
                                                              old_ino_pc,
                                                              old_ch_pc,
                                                              new_ino_pc,
                                                              new_ch_pc);
                                }
                            }
                            free(old_cis);
                            free(new_cis);
                        }
                    }
                } else if (efs_meta_blob_is_export(payload, payload_len)) {
                    /* Legacy full-blob merge (pre-fragmented peers / tests).
                     * An empty blob is a bootstrap shell: it exists only to
                     * plant the export row, so it must NOT flip a fragmented
                     * export back to legacy mode or trigger a save. */
                    struct efs_export inc;
                    efs_export_init(&inc, 1, "pending");
                    if (efs_export_deserialize(&inc, payload, payload_len) == 0) {
                        int empty_shell =
                            (inc.inode_count == 0 && inc.chunk_count == 0);
                        pthread_mutex_lock(&g_server->lock);
                        struct efs_export *ex =
                            server_get_export_create(g_server,
                                                     inc.id ? inc.id : 1,
                                                     inc.name[0] ? inc.name
                                                                 : "pending");
                        if (ex && efs_export_merge(ex, &inc) == 0) {
                            if (!empty_shell) {
                                ex->meta_fragmented = 0;
                                g_server->epoch++;
                                free(ex->gm_blob);
                                ex->gm_blob = NULL;
                                g_server->export_meta_dirty = 1;
                                if (ex->name[0] &&
                                    strcmp(ex->name, "pending") != 0) {
                                    server_save_export(g_server, ex);
                                    g_server->export_meta_dirty = 0;
                                }
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
                efs_conn_send_msg(conn, EFS_MSG_PUT_META_REPLY, &m, sizeof(m));
            }
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
            reply.state = (uint32_t)g_server->state;
            pthread_mutex_unlock(&g_server->lock);
            efs_conn_send_msg(conn, EFS_MSG_STATUS_REPLY, &reply, sizeof(reply));
            break;
        }
        case EFS_MSG_HEAL_STATUS: {
            struct efs_msg_heal_status_reply reply;
            server_fill_heal_status(g_server, &reply);
            efs_conn_send_msg(conn, EFS_MSG_HEAL_STATUS_REPLY, &reply,
                              sizeof(reply));
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
            efs_conn_send_msg(conn, EFS_MSG_DRAIN_NODE_REPLY, &reply, 1);
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
            efs_conn_send_msg(conn, EFS_MSG_UNDRAIN_NODE_REPLY, &reply, 1);
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
                /* Heal orphans first: re-place every fragment that referenced
                 * the departing node and flush the EFSR, so no chunk map points
                 * at a node that is gone before we shrink membership. */
                server_heal_orphan_fragments(g_server, msg->node_id);
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
                efs_conn_send_msg(conn, EFS_MSG_CREATE_EXPORT_REPLY, &reply, 1);
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
            efs_conn_send_msg(conn, EFS_MSG_DESTROY_EXPORT_REPLY, &reply, 1);
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
            efs_conn_send_msg(conn, EFS_MSG_LIST_EXPORTS_REPLY, &reply, sizeof(reply));
            break;
        }
        case EFS_MSG_GET_FEATURES: {
            if (payload_len >= sizeof(struct efs_msg_get_features)) {
                struct efs_msg_get_features *req = payload;
                struct efs_msg_features_reply r;
                memset(&r, 0, sizeof(r));
                r.status = EFS_FEATURES_NOT_FOUND;
                pthread_mutex_lock(&g_server->lock);
                struct efs_export *ex = server_find_export(g_server, req->export_name);
                if (ex) {
                    r.features = ex->features;
                    r.status = EFS_FEATURES_OK;
                }
                pthread_mutex_unlock(&g_server->lock);
                efs_conn_send_msg(conn, EFS_MSG_GET_FEATURES_REPLY, &r, sizeof(r));
            }
            break;
        }
        case EFS_MSG_SET_FEATURES: {
            /* Local-only: efs-mgmt fans out to every cluster node. Features are
             * server-owned; only bits in set_mask change. Persisted via the EFSR
             * root so the setting survives restart. */
            if (payload_len >= sizeof(struct efs_msg_set_features)) {
                struct efs_msg_set_features *req = payload;
                struct efs_msg_features_reply r;
                memset(&r, 0, sizeof(r));
                r.status = EFS_FEATURES_NOT_FOUND;
                pthread_mutex_lock(&g_server->lock);
                struct efs_export *ex = server_find_export(g_server, req->export_name);
                if (ex) {
                    ex->features = (ex->features & ~req->set_mask) |
                                   (req->features & req->set_mask);
                    ex->root.features = ex->features;
                    server_save_export(g_server, ex);
                    r.features = ex->features;
                    r.status = EFS_FEATURES_OK;
                }
                pthread_mutex_unlock(&g_server->lock);
                efs_conn_send_msg(conn, EFS_MSG_SET_FEATURES_REPLY, &r, sizeof(r));
            }
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
                efs_conn_send_msg(conn, EFS_MSG_JOIN_REPLY, &reply, 1);
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
            /* Rebuild dirty/empty tables before counting. The catch-up thread
             * also rebuilds, but a stats query must not report zeros for an
             * export whose pages exist but whose tables are fenced pending
             * rebuild (e.g. right after a PUT_META root flip). Rebuild here
             * synchronously; page fetches are served from local disk when this
             * node holds the fragments, so the common case is fast. */
            for (uint32_t e = 0; e < ec && e < EFS_MAX_EXPORTS; e++) {
                if (need[e])
                    server_rebuild_export_from_pages(g_server,
                                                     &g_server->exports[e]);
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
            efs_conn_send_msg(conn, EFS_MSG_QUERY_STATS_REPLY, &reply, sizeof(reply));
            break;
        }
        case EFS_MSG_INODE_LOOKUP:
        case EFS_MSG_INODE_CREATE:
        case EFS_MSG_INODE_CREATE_SHARD:
        case EFS_MSG_INODE_GETATTR:
        case EFS_MSG_INODE_UNLINK:
        case EFS_MSG_INODE_RENAME:
        case EFS_MSG_INODE_SETATTR:
        case EFS_MSG_INODE_APPEND:
        case EFS_MSG_INODE_LINK:
        case EFS_MSG_INODE_LINK_SHARD:
        case EFS_MSG_INODE_UNLINK_SHARD: {
            struct efs_msg_inode_reply r;
            memset(&r, 0, sizeof(r));
            r.status = EFS_INODE_RPC_ERROR;
            pthread_mutex_lock(&g_server->lock);
            struct efs_export *ex = NULL;
            uint32_t eidx = 0;
            efs_export_id_t eid = 0;
            if (type == EFS_MSG_INODE_LOOKUP &&
                payload_len >= sizeof(struct efs_msg_inode_lookup))
                eid = ((struct efs_msg_inode_lookup *)payload)->export_id;
                else if (type == EFS_MSG_INODE_CREATE &&
                     payload_len >= sizeof(struct efs_msg_inode_create))
                eid = ((struct efs_msg_inode_create *)payload)->export_id;
            else if (type == EFS_MSG_INODE_CREATE_SHARD &&
                     payload_len >= sizeof(struct efs_msg_inode_create_shard))
                eid = ((struct efs_msg_inode_create_shard *)payload)->export_id;
            else if (type == EFS_MSG_INODE_GETATTR &&
                     payload_len >= sizeof(struct efs_msg_inode_getattr))
                eid = ((struct efs_msg_inode_getattr *)payload)->export_id;
            else if (type == EFS_MSG_INODE_UNLINK &&
                     payload_len >= sizeof(struct efs_msg_inode_unlink))
                eid = ((struct efs_msg_inode_unlink *)payload)->export_id;
            else if (type == EFS_MSG_INODE_RENAME &&
                     payload_len >= sizeof(struct efs_msg_inode_rename))
                eid = ((struct efs_msg_inode_rename *)payload)->export_id;
            else if (type == EFS_MSG_INODE_SETATTR &&
                     payload_len >= sizeof(struct efs_msg_inode_setattr))
                eid = ((struct efs_msg_inode_setattr *)payload)->export_id;
            else if (type == EFS_MSG_INODE_APPEND &&
                     payload_len >= sizeof(struct efs_msg_inode_append))
                eid = ((struct efs_msg_inode_append *)payload)->export_id;
            else if (type == EFS_MSG_INODE_LINK &&
                     payload_len >= sizeof(struct efs_msg_inode_link))
                eid = ((struct efs_msg_inode_link *)payload)->export_id;
            else if (type == EFS_MSG_INODE_LINK_SHARD &&
                     payload_len >= sizeof(struct efs_msg_inode_link_shard))
                eid = ((struct efs_msg_inode_link_shard *)payload)->export_id;
            else if (type == EFS_MSG_INODE_UNLINK_SHARD &&
                     payload_len >= sizeof(struct efs_msg_inode_unlink_shard))
                eid = ((struct efs_msg_inode_unlink_shard *)payload)->export_id;
            for (uint32_t i = 0; i < g_server->export_count; i++) {
                if (g_server->exports[i].id == eid ||
                    (eid == 0 && i == 0)) {
                    ex = &g_server->exports[i];
                    eidx = i;
                    break;
                }
            }
            if (!ex) {
                r.status = EFS_INODE_RPC_NOT_FOUND;
            } else if (type != EFS_MSG_INODE_LOOKUP &&
                       type != EFS_MSG_INODE_GETATTR &&
                       !server_owns_req_locked(g_server, ex, type, payload)) {
                r.status = EFS_INODE_RPC_NOT_PRIMARY;
                r.primary_id = server_shard_owner_id_locked(g_server, ex, type,
                                                            payload);
            } else if (type == EFS_MSG_INODE_LOOKUP) {
                struct efs_msg_inode_lookup *req = payload;
                if (efs_export_lookup(ex, req->parent, req->name, &r.inode) == 0)
                    r.status = EFS_INODE_RPC_OK;
                else
                    r.status = EFS_INODE_RPC_NOT_FOUND;
            } else if (type == EFS_MSG_INODE_GETATTR) {
                struct efs_msg_inode_getattr *req = payload;
                if (!server_owns_req_locked(g_server, ex, type, payload)) {
                    r.status = EFS_INODE_RPC_NOT_PRIMARY;
                    r.primary_id = server_shard_owner_id_locked(g_server, ex,
                                                                type, payload);
                } else {
                    struct efs_export *tab = table_for_ino(ex, req->ino);
                    if (efs_export_get_inode(tab, req->ino, &r.inode) == 0)
                        r.status = EFS_INODE_RPC_OK;
                    else
                        r.status = EFS_INODE_RPC_NOT_FOUND;
                }
            } else if (type == EFS_MSG_INODE_CREATE) {
                struct efs_msg_inode_create *req = payload;
                efs_node_id_t live[EFS_MAX_NODES];
                uint32_t nlive = server_nlive_locked(g_server, live);
                uint32_t sc = ex->root.shard_count ? ex->root.shard_count : 1;
                uint32_t bits = ex->root.shard_bits;
                uint32_t target = efs_export_create_target(ex, req->parent,
                                                           req->mode);
                efs_node_id_t owner = efs_shard_owner_of(target, sc, live, nlive);
                struct efs_export *tab = table_for_ino(ex, req->parent);
                if (!efs_export_fits_page_cap(tab, 1, 0)) {
                    r.status = EFS_INODE_RPC_QUOTA;
                } else if (bits && sc > 1 && owner != 0 &&
                           owner != g_server->id) {
                    /* Child row lives on a peer. Create it there first,
                     * then write only the parent dentry locally. */
                    if (efs_export_lookup(ex, req->parent, req->name,
                                          NULL) == EFS_OK) {
                        r.status = EFS_INODE_RPC_EXIST;
                    } else {
                        struct efs_msg_inode_create_shard creq;
                        struct efs_msg_inode_reply cr;
                        char host[64];
                        uint16_t port = 0;
                        memset(&creq, 0, sizeof(creq));
                        memset(&cr, 0, sizeof(cr));
                        memset(host, 0, sizeof(host));
                        creq.export_id = req->export_id;
                        creq.parent = req->parent;
                        memcpy(creq.name, req->name, EFS_MAX_NAME);
                        creq.mode = req->mode;
                        creq.uid = req->uid;
                        creq.gid = req->gid;
                        creq.target_shard = target;
                        if (server_node_addr_locked(g_server, owner, host,
                                                    sizeof(host), &port) != 0) {
                            r.status = EFS_INODE_RPC_ERROR;
                        } else {
                            if (!efs_mode_is_dir(req->mode) && sc > 1)
                                ex->create_rr++;
                            pthread_mutex_unlock(&g_server->lock);
                            int nrc = server_peer_create_shard(host, port,
                                                               &creq, &cr);
                            pthread_mutex_lock(&g_server->lock);
                            if (nrc != 0) {
                                r.status = EFS_INODE_RPC_ERROR;
                            } else if (cr.status != EFS_INODE_RPC_OK) {
                                r.status = cr.status;
                                r.primary_id = cr.primary_id;
                            } else {
                                struct efs_export *ptab =
                                    table_for_ino(ex, req->parent);
                                if (!efs_export_create_with_ino(
                                        ptab, cr.inode.ino, req->parent,
                                        req->mode, (uid_t)req->uid,
                                        (gid_t)req->gid, req->name)) {
                                    r.status = EFS_INODE_RPC_EXIST;
                                } else {
                                    ptab->shard_dirty = 1;
                                    r.inode = cr.inode;
                                    r.status = EFS_INODE_RPC_OK;
                                    server_meta_mark_rpc_dirty_locked(g_server,
                                                                      eidx);
                                }
                            }
                        }
                    }
                } else {
                    efs_ino_t ino = efs_export_create(ex, req->parent, req->mode,
                                                      (uid_t)req->uid,
                                                      (gid_t)req->gid, req->name);
                    if (ino) {
                        efs_export_get_inode(ex, ino, &r.inode);
                        r.status = EFS_INODE_RPC_OK;
                        server_meta_mark_rpc_dirty_locked(g_server, eidx);
                        efs_export_evict_cold_shards(ex, EFS_SHARD_LRU_KEEP);
                    } else {
                        r.status = EFS_INODE_RPC_EXIST;
                    }
                }
            } else if (type == EFS_MSG_INODE_CREATE_SHARD) {
                struct efs_msg_inode_create_shard *req = payload;
                uint32_t sc = ex->root.shard_count ? ex->root.shard_count : 1;
                if (req->target_shard >= sc) {
                    r.status = EFS_INODE_RPC_INVAL;
                } else {
                    struct efs_export *ctab =
                        efs_export_table(ex, req->target_shard);
                    if (!ctab || !efs_export_fits_page_cap(ctab, 1, 0)) {
                        r.status = EFS_INODE_RPC_QUOTA;
                    } else {
                        efs_ino_t ino = efs_export_alloc_ino(ctab,
                                                            req->target_shard);
                        if (!ino ||
                            !efs_export_create_with_ino(ctab, ino, req->parent,
                                                        req->mode,
                                                        (uid_t)req->uid,
                                                        (gid_t)req->gid,
                                                        req->name)) {
                            r.status = EFS_INODE_RPC_EXIST;
                        } else {
                            ctab->shard_dirty = 1;
                            efs_export_get_inode(ctab, ino, &r.inode);
                            r.status = EFS_INODE_RPC_OK;
                            server_meta_mark_rpc_dirty_locked(g_server, eidx);
                        }
                    }
                }
            } else if (type == EFS_MSG_INODE_UNLINK) {
                struct efs_msg_inode_unlink *req = payload;
                /* rmdir: refuse to remove a non-empty directory (ENOTEMPTY). */
                struct efs_inode victim;
                memset(&victim, 0, sizeof(victim));
                int have_victim = (efs_export_lookup(ex, req->parent,
                                                     req->name, &victim) == 0);
                if (req->is_dir && have_victim &&
                    !efs_export_dir_empty(table_for_ino(ex, victim.ino),
                                          victim.ino)) {
                    r.status = EFS_INODE_RPC_NOT_EMPTY;
                } else {
                    int urc = efs_export_unlink_name(ex, req->parent,
                                                     req->name);
                    r.status = (urc == 0) ? EFS_INODE_RPC_OK
                                          : EFS_INODE_RPC_NOT_FOUND;
                    if (urc == 0)
                        server_meta_mark_rpc_dirty_locked(g_server, eidx);
                    /* Child row + chunks live on the child owner. After
                     * the parent dentry is gone, fan nlink-- / last-link
                     * drop there so spread-created files don't leak. */
                    if (urc == 0 && have_victim && !req->is_dir) {
                        uint32_t sc = ex->root.shard_count
                                          ? ex->root.shard_count : 1;
                        uint32_t bits = ex->root.shard_bits;
                        if (bits && sc > 1) {
                            efs_node_id_t live[EFS_MAX_NODES];
                            uint32_t nlive = server_nlive_locked(g_server,
                                                                 live);
                            uint32_t csh = efs_export_shard_of(victim.ino,
                                                               bits);
                            efs_node_id_t owner =
                                efs_shard_owner_of(csh, sc, live, nlive);
                            if (owner == 0 || owner == g_server->id) {
                                /* unlink_name already applied a loaded
                                 * child table. Only fault-in if missing. */
                                if (!efs_export_shard_tab(ex, csh))
                                    (void)efs_export_nlink_dec(ex, victim.ino,
                                                               &r.inode);
                            } else {
                                struct efs_msg_inode_unlink_shard ureq;
                                struct efs_msg_inode_reply ur;
                                char host[64];
                                uint16_t port = 0;
                                memset(&ureq, 0, sizeof(ureq));
                                memset(&ur, 0, sizeof(ur));
                                memset(host, 0, sizeof(host));
                                ureq.export_id = req->export_id;
                                ureq.src_ino = victim.ino;
                                if (server_node_addr_locked(g_server, owner,
                                                            host,
                                                            sizeof(host),
                                                            &port) == 0) {
                                    pthread_mutex_unlock(&g_server->lock);
                                    (void)server_peer_inode_rpc(
                                        host, port,
                                        EFS_MSG_INODE_UNLINK_SHARD, &ureq,
                                        sizeof(ureq),
                                        EFS_MSG_INODE_UNLINK_SHARD_REPLY,
                                        &ur);
                                    pthread_mutex_lock(&g_server->lock);
                                }
                            }
                        }
                    }
                }
            } else if (type == EFS_MSG_INODE_UNLINK_SHARD) {
                struct efs_msg_inode_unlink_shard *req = payload;
                int urc = efs_export_nlink_dec(ex, req->src_ino, &r.inode);
                if (urc == 0) {
                    r.status = EFS_INODE_RPC_OK;
                    server_meta_mark_rpc_dirty_locked(g_server, eidx);
                } else {
                    r.status = (urc == EFS_ERR_NOT_FOUND)
                                   ? EFS_INODE_RPC_NOT_FOUND
                                   : EFS_INODE_RPC_INVAL;
                }
            } else if (type == EFS_MSG_INODE_RENAME) {
                struct efs_msg_inode_rename *req = payload;
                int rrc = efs_export_rename(ex, req->ino, req->new_parent,
                                            req->new_name);
                if (rrc == 0) {
                    efs_export_get_inode(ex, req->ino, &r.inode);
                    r.status = EFS_INODE_RPC_OK;
                    ex->shard_dirty = 1;
                    server_meta_mark_rpc_dirty_locked(g_server, eidx);
                } else {
                    r.status = (rrc == EFS_ERR_NOT_FOUND) ? EFS_INODE_RPC_NOT_FOUND
                             : (rrc == EFS_ERR_EXIST) ? EFS_INODE_RPC_EXIST
                             : (rrc == EFS_ERR_NOT_EMPTY) ? EFS_INODE_RPC_NOT_EMPTY
                             : EFS_INODE_RPC_INVAL;
                }
            } else if (type == EFS_MSG_INODE_SETATTR) {
                struct efs_msg_inode_setattr *req = payload;
                struct efs_export *tab = table_for_ino(ex, req->ino);
                struct efs_inode cur;
                if (efs_export_get_inode(tab, req->ino, &cur) != 0) {
                    r.status = EFS_INODE_RPC_NOT_FOUND;
                } else {
                    if (req->mask & EFS_SETATTR_MODE)
                        efs_export_set_mode(tab, req->ino, req->mode);
                    if (req->mask & (EFS_SETATTR_UID | EFS_SETATTR_GID))
                        efs_export_set_owner(tab, req->ino,
                                (req->mask & EFS_SETATTR_UID) ? (uid_t)req->uid
                                                              : (uid_t)-1,
                                (req->mask & EFS_SETATTR_GID) ? (gid_t)req->gid
                                                              : (gid_t)-1);
                    if (req->mask & EFS_SETATTR_SIZE) {
                        /* Shrink: drop chunks wholly beyond the new size. The
                         * client rewrites the partial last kept chunk (data
                         * path) before calling, so here we only trim whole
                         * chunks. */
                        if (req->size < cur.size) {
                            uint32_t cs = server_data_chunk_size(ex);
                            uint32_t first_drop = (req->size == 0) ? 0
                                : (uint32_t)((req->size + cs - 1) / cs);
                            efs_export_drop_chunks_from(tab, req->ino, first_drop);
                        }
                        efs_export_set_size(tab, req->ino, req->size);
                    }
                    if (req->mask & EFS_SETATTR_MTIME)
                        efs_export_set_mtime_ns(tab, req->ino, req->mtime,
                                                req->mtime_nsec);
                    if (req->mask & EFS_SETATTR_ATIME)
                        efs_export_set_atime(tab, req->ino, req->atime);
                    tab->shard_dirty = 1;
                    server_meta_mark_rpc_dirty_locked(g_server, eidx);
                    efs_export_get_inode(tab, req->ino, &r.inode);
                    r.status = EFS_INODE_RPC_OK;
                }
            } else if (type == EFS_MSG_INODE_APPEND) {
                struct efs_msg_inode_append *req = payload;
                struct efs_export *tab = table_for_ino(ex, req->ino);
                struct efs_inode cur;
                if (efs_export_get_inode(tab, req->ino, &cur) != 0 ||
                    !efs_mode_is_reg(cur.mode)) {
                    r.status = EFS_INODE_RPC_NOT_FOUND;
                } else {
                    /* Append barrier. The table size advances only via
                     * REPORT_CHUNKS (grow-only — i.e. only after the
                     * appender's data is PUT). Handing out a second
                     * reservation while an earlier one is still unflushed
                     * lets the new appender's merge-base read of the shared
                     * tail chunk miss the earlier appender's bytes, and its
                     * whole-chunk PUT then clobbers them (mc_stress appfile
                     * torn lines). So: one outstanding reservation per ino;
                     * the next reserve is BUSY until the table size catches
                     * up. A stale reservation (crashed appender) expires;
                     * its region stays a hole, keeping offsets stable.
                     * Atomic under s->lock; the rsv state is in-memory only
                     * (nothing to flush here). */
                    struct timespec ts;
                    clock_gettime(CLOCK_REALTIME, &ts);
                    uint64_t now_ms = (uint64_t)ts.tv_sec * 1000ull +
                                      (uint64_t)ts.tv_nsec / 1000000ull;
                    uint32_t si = (uint32_t)(req->ino % EFS_APPEND_RSV_SLOTS);
                    uint64_t rsv_end = 0, rsv_ts = 0;
                    if (tab->append_rsv[si].ino == req->ino) {
                        rsv_end = tab->append_rsv[si].end;
                        rsv_ts = tab->append_rsv[si].ts_ms;
                    }
                    if (rsv_end > cur.size && now_ms - rsv_ts < 30000ull) {
                        r.status = EFS_INODE_RPC_BUSY;
                        r.inode = cur;
                    } else {
                        uint64_t off = rsv_end > cur.size ? rsv_end : cur.size;
                        tab->append_rsv[si].ino = req->ino;
                        tab->append_rsv[si].end = off + req->len;
                        tab->append_rsv[si].ts_ms = now_ms;
                        r.inode = cur;
                        r.inode.size = off + req->len;
                        r.status = EFS_INODE_RPC_OK;
                    }
                }
            } else if (type == EFS_MSG_INODE_LINK) {
                struct efs_msg_inode_link *req = payload;
                uint32_t sc = ex->root.shard_count ? ex->root.shard_count : 1;
                uint32_t bits = ex->root.shard_bits;
                efs_node_id_t owner = 0;
                if (bits && sc > 1) {
                    efs_node_id_t live[EFS_MAX_NODES];
                    uint32_t nlive = server_nlive_locked(g_server, live);
                    uint32_t csh = efs_export_shard_of(req->src_ino, bits);
                    owner = efs_shard_owner_of(csh, sc, live, nlive);
                }
                if (bits && sc > 1 && owner != 0 && owner != g_server->id) {
                    if (efs_export_lookup(ex, req->new_parent, req->new_name,
                                          NULL) == EFS_OK) {
                        r.status = EFS_INODE_RPC_EXIST;
                    } else {
                        struct efs_msg_inode_link_shard lreq;
                        struct efs_msg_inode_reply lr;
                        char host[64];
                        uint16_t port = 0;
                        memset(&lreq, 0, sizeof(lreq));
                        memset(&lr, 0, sizeof(lr));
                        memset(host, 0, sizeof(host));
                        lreq.export_id = req->export_id;
                        lreq.src_ino = req->src_ino;
                        if (server_node_addr_locked(g_server, owner, host,
                                                    sizeof(host), &port) != 0) {
                            r.status = EFS_INODE_RPC_ERROR;
                        } else {
                            pthread_mutex_unlock(&g_server->lock);
                            int nrc = server_peer_inode_rpc(
                                host, port, EFS_MSG_INODE_LINK_SHARD, &lreq,
                                sizeof(lreq), EFS_MSG_INODE_LINK_SHARD_REPLY,
                                &lr);
                            pthread_mutex_lock(&g_server->lock);
                            if (nrc != 0) {
                                r.status = EFS_INODE_RPC_ERROR;
                            } else if (lr.status != EFS_INODE_RPC_OK) {
                                r.status = lr.status;
                                r.primary_id = lr.primary_id;
                            } else {
                                int lrc = efs_export_link_dentry(
                                    ex, &lr.inode, req->new_parent,
                                    req->new_name);
                                if (lrc == 0) {
                                    r.inode = lr.inode;
                                    r.status = EFS_INODE_RPC_OK;
                                    server_meta_mark_rpc_dirty_locked(
                                        g_server, eidx);
                                } else {
                                    r.status = (lrc == EFS_ERR_EXIST)
                                                   ? EFS_INODE_RPC_EXIST
                                                   : EFS_INODE_RPC_INVAL;
                                }
                            }
                        }
                    }
                } else {
                    int lrc = efs_export_link(ex, req->src_ino, req->new_parent,
                                              req->new_name);
                    if (lrc == 0) {
                        if (efs_export_get_inode(ex, req->src_ino, &r.inode) != 0)
                            r.inode = (struct efs_inode){0};
                        r.status = EFS_INODE_RPC_OK;
                        server_meta_mark_rpc_dirty_locked(g_server, eidx);
                    } else {
                        r.status = (lrc == EFS_ERR_NOT_FOUND)
                                       ? EFS_INODE_RPC_NOT_FOUND
                                       : (lrc == EFS_ERR_EXIST)
                                             ? EFS_INODE_RPC_EXIST
                                             : EFS_INODE_RPC_INVAL;
                    }
                }
            } else if (type == EFS_MSG_INODE_LINK_SHARD) {
                struct efs_msg_inode_link_shard *req = payload;
                int lrc = efs_export_nlink_inc(ex, req->src_ino, &r.inode);
                if (lrc == 0) {
                    r.status = EFS_INODE_RPC_OK;
                    server_meta_mark_rpc_dirty_locked(g_server, eidx);
                } else {
                    r.status = (lrc == EFS_ERR_NOT_FOUND)
                                   ? EFS_INODE_RPC_NOT_FOUND
                                   : EFS_INODE_RPC_INVAL;
                }
            }
            pthread_mutex_unlock(&g_server->lock);
            uint8_t rtype = (type == EFS_MSG_INODE_LOOKUP) ? EFS_MSG_INODE_LOOKUP_REPLY
                          : (type == EFS_MSG_INODE_CREATE) ? EFS_MSG_INODE_CREATE_REPLY
                          : (type == EFS_MSG_INODE_CREATE_SHARD) ? EFS_MSG_INODE_CREATE_SHARD_REPLY
                          : (type == EFS_MSG_INODE_GETATTR) ? EFS_MSG_INODE_GETATTR_REPLY
                          : (type == EFS_MSG_INODE_RENAME) ? EFS_MSG_INODE_RENAME_REPLY
                          : (type == EFS_MSG_INODE_SETATTR) ? EFS_MSG_INODE_SETATTR_REPLY
                          : (type == EFS_MSG_INODE_APPEND) ? EFS_MSG_INODE_APPEND_REPLY
                          : (type == EFS_MSG_INODE_LINK) ? EFS_MSG_INODE_LINK_REPLY
                          : (type == EFS_MSG_INODE_LINK_SHARD) ? EFS_MSG_INODE_LINK_SHARD_REPLY
                          : (type == EFS_MSG_INODE_UNLINK_SHARD) ? EFS_MSG_INODE_UNLINK_SHARD_REPLY
                          : EFS_MSG_INODE_UNLINK_REPLY;
            efs_conn_send_msg(conn, rtype, &r, sizeof(r));
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
            struct efs_msg_inode_lookup_path *req = payload;
            req->path[sizeof(req->path) - 1] = '\0';
            pthread_mutex_lock(&g_server->lock);
            struct efs_export *ex = NULL;
            efs_export_id_t eid = req->export_id;
            for (uint32_t i = 0; i < g_server->export_count; i++) {
                if (g_server->exports[i].id == eid ||
                    (eid == 0 && i == 0)) {
                    ex = &g_server->exports[i];
                    break;
                }
            }
            if (!ex) {
                r.status = EFS_INODE_RPC_NOT_FOUND;
            } else if (req->path[0] != '/') {
                r.status = EFS_INODE_RPC_INVAL;
            } else if (req->path[1] == '\0') {
                if (efs_export_get_inode(ex, EFS_ROOT_INO, &r.inode) == 0)
                    r.status = EFS_INODE_RPC_OK;
                else
                    r.status = EFS_INODE_RPC_NOT_FOUND;
            } else {
                char pbuf[4096];
                memcpy(pbuf, req->path + 1, sizeof(pbuf) - 1);
                pbuf[sizeof(pbuf) - 1] = '\0';
                char *save = NULL;
                char *part = strtok_r(pbuf, "/", &save);
                efs_ino_t parent = EFS_ROOT_INO;
                r.status = EFS_INODE_RPC_NOT_FOUND;
                while (part) {
                    int more = (save && *save);
                    struct efs_inode row;
                    if (efs_export_lookup(ex, parent, part, &row) != 0) {
                        r.status = EFS_INODE_RPC_NOT_FOUND;
                        break;
                    }
                    if (more && efs_mode_is_lnk(row.mode)) {
                        r.status = EFS_INODE_RPC_SYMLINK;
                        break;
                    }
                    if (more && (req->flags & EFS_LOOKUP_PATH_F_ANCESTORS)) {
                        if (r.ancestor_count >= EFS_LOOKUP_PATH_MAX_DEPTH) {
                            r.status = EFS_INODE_RPC_DEEP;
                            break;
                        }
                        struct efs_lookup_path_anc *a =
                            &r.ancestors[r.ancestor_count++];
                        a->ino = row.ino;
                        a->mode = row.mode;
                        a->uid = row.uid;
                        a->gid = row.gid;
                    }
                    parent = row.ino;
                    r.inode = row;
                    r.status = EFS_INODE_RPC_OK;
                    part = strtok_r(NULL, "/", &save);
                }
            }
            pthread_mutex_unlock(&g_server->lock);
            efs_conn_send_msg(conn, EFS_MSG_INODE_LOOKUP_PATH_REPLY, &r,
                              sizeof(r));
            break;
        }
        case EFS_MSG_REPORT_CHUNKS: {
            /* Phase 2b: batched write-path chunk mappings. Applies each record
             * to the in-memory table and marks the export dirty; the meta-flush
             * thread persists it. Primary-only (it is a mutation). */
            struct efs_msg_inode_reply r;
            memset(&r, 0, sizeof(r));
            r.status = EFS_INODE_RPC_ERROR;
            int bad = 0;
            uint32_t count = 0;
            uint32_t sync = 0;
            uint32_t ino_count = 0;
            const struct efs_chunk_rec *recs = NULL;
            const struct efs_ino_size_rec *irecs = NULL;
            efs_export_id_t eid = 0;
            if (payload_len >= sizeof(struct efs_msg_report_chunks)) {
                struct efs_msg_report_chunks *req = payload;
                eid = req->export_id;
                sync = req->sync;
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
            int do_flush = 0;
            pthread_mutex_lock(&g_server->lock);
            struct efs_export *ex = NULL;
            uint32_t eidx = 0;
            for (uint32_t i = 0; i < g_server->export_count; i++) {
                if (g_server->exports[i].id == eid || (eid == 0 && i == 0)) {
                    ex = &g_server->exports[i];
                    eidx = i;
                    break;
                }
            }
            if (bad) {
                r.status = EFS_INODE_RPC_INVAL;
            } else if (!ex) {
                r.status = EFS_INODE_RPC_NOT_FOUND;
            } else if (!server_is_meta_primary_locked(g_server) &&
                       (ex->root.shard_bits == 0 ||
                        ex->root.shard_count <= 1)) {
                r.status = EFS_INODE_RPC_NOT_PRIMARY;
                r.primary_id = server_meta_primary_id_locked(g_server);
            } else {
                uint32_t applied = 0;
                uint32_t dropped = 0;
                efs_node_id_t drop_owner = 0;
                efs_node_id_t live[EFS_MAX_NODES];
                uint32_t nlive = server_nlive_locked(g_server, live);
                uint32_t sc = ex->root.shard_count ? ex->root.shard_count : 1;
                uint32_t bits = ex->root.shard_bits;
                for (uint32_t k = 0; k < count; k++) {
                    /* Yield the global lock every 1024 recs. A 9-client
                     * close-report applies tens of thousands of chunk recs;
                     * at 8192 the lock was held ~8ms straight, and every
                     * data-path PUT_CHUNK (which takes g_server->lock briefly
                     * to acquire the export) stalled behind it — the multi-
                     * client sw-1m ceiling (12ms avg / 1.5s max write latency).
                     * sched_yield between unlock/lock so a waiting PUT
                     * actually wins the reacquire, not just the reporter. */
                    if ((k & 1023u) == 1023u) {
                        pthread_mutex_unlock(&g_server->lock);
                        sched_yield();
                        pthread_mutex_lock(&g_server->lock);
                        if (ex->meta_needs_rebuild)
                            break;
                    }
                    if (bits && sc > 1) {
                        uint32_t sh = efs_export_shard_of(recs[k].ino, bits);
                        efs_node_id_t own =
                            efs_shard_owner_of(sh, sc, live, nlive);
                        if (own != g_server->id) {
                            dropped++;
                            if (!drop_owner)
                                drop_owner = own;
                            continue;
                        }
                    }
                    struct efs_export *tab = table_for_ino(ex, recs[k].ino);
                    if (efs_export_set_chunk(tab, recs[k].ino, recs[k].chunk_index,
                                             recs[k].nodes,
                                             recs[k].checksums) == 0) {
                        applied++;
                        tab->shard_dirty = 1;
                    }
                }
                /* Write-path size/mtime. Use norollup: the rolling
                 * set_size/set_mtime walk parent rollups on every rec and a
                 * 9-client close-report held s->lock across tens of thousands
                 * of those. Flush calls ensure_rollups once before serialize. */
                for (uint32_t k = 0; k < ino_count; k++) {
                    if (bits && sc > 1) {
                        uint32_t sh = efs_export_shard_of(irecs[k].ino, bits);
                        efs_node_id_t own =
                            efs_shard_owner_of(sh, sc, live, nlive);
                        if (own != g_server->id) {
                            dropped++;
                            if (!drop_owner)
                                drop_owner = own;
                            continue;
                        }
                    }
                    struct efs_export *tab = table_for_ino(ex, irecs[k].ino);
                    struct efs_inode cur;
                    if (efs_export_get_inode(tab, irecs[k].ino, &cur) != 0)
                        continue; /* inode not (yet) on the server; skip */
                    /* Grow-only: a lagging client's report must not shrink a
                     * size another client already advanced (cross-client
                     * O_APPEND reserves size server-side ahead of its data
                     * report). Shrinks only ever arrive via SETATTR. Same for
                     * mtime: apply only when newer. */
                    if (irecs[k].size > cur.size &&
                        efs_export_set_size_norollup(tab, irecs[k].ino,
                                                     irecs[k].size) == 0) {
                        applied++;
                        tab->shard_dirty = 1;
                    }
                    if (irecs[k].mtime > cur.mtime ||
                        (irecs[k].mtime == cur.mtime &&
                         irecs[k].mtime_nsec > cur.mtime_nsec)) {
                        efs_export_set_mtime_ns_norollup(tab, irecs[k].ino,
                                                         irecs[k].mtime,
                                                         irecs[k].mtime_nsec);
                        applied++;
                        tab->shard_dirty = 1;
                    }
                    if (irecs[k].pack_ino || irecs[k].pack_len) {
                        /* Re-fetch: cur above predates the size grow, and
                         * upsert writes the whole row back. */
                        struct efs_inode pc;
                        if (efs_export_get_inode(tab, irecs[k].ino, &pc) == 0) {
                            pc.pack_ino = irecs[k].pack_ino;
                            pc.pack_off = irecs[k].pack_off;
                            pc.pack_len = irecs[k].pack_len;
                            (void)efs_export_upsert_inode(tab, &pc);
                        }
                    }
                }
                if (applied)
                    server_meta_mark_rpc_dirty_locked(g_server, eidx);
                if (dropped) {
                    /* Loud: a silent drop was data loss under a membership
                     * flap. Client retries NOT_PRIMARY by re-resolving. */
                    r.status = EFS_INODE_RPC_NOT_PRIMARY;
                    r.primary_id = drop_owner ? drop_owner
                                              : server_meta_primary_id_locked(g_server);
                    fprintf(stderr,
                            "REPORT_CHUNKS: dropped %u recs not owned here "
                            "(redirect %llu)\n",
                            dropped, (unsigned long long)r.primary_id);
                } else {
                    r.status = EFS_INODE_RPC_OK;
                }
                /* fsync barrier: commit synchronously so the client's fsync is
                 * durable when it returns. Flush whenever sync is set — even
                 * with count==0 there may be earlier async-reported ops still
                 * uncommitted (and the meta-flush thread resets rpc_dirty_ops
                 * before its flush commits, so dirty-count alone can't prove a
                 * barrier). Flush outside the lock (it does network I/O and
                 * re-takes s->lock internally). */
                if (sync)
                    do_flush = 1;
            }
            pthread_mutex_unlock(&g_server->lock);
            if (do_flush) {
                if (server_flush_fragmented_meta(g_server, ex) != 0) {
                    /* Flush failed (no quorum / fenced): the in-memory mutation
                     * is still dirty and the meta-flush thread will retry, but
                     * the client's fsync cannot be told it's durable. */
                    r.status = EFS_INODE_RPC_ERROR;
                }
            }
            efs_conn_send_msg(conn, EFS_MSG_REPORT_CHUNKS_REPLY, &r, sizeof(r));
            break;
        }
        case EFS_MSG_INODE_READDIR: {
            struct efs_msg_inode_readdir_reply r;
            memset(&r, 0, sizeof(r));
            r.status = EFS_INODE_RPC_ERROR;
            if (payload_len >= sizeof(struct efs_msg_inode_readdir)) {
                struct efs_msg_inode_readdir *req = payload;
                pthread_mutex_lock(&g_server->lock);
                struct efs_export *ex = NULL;
                for (uint32_t i = 0; i < g_server->export_count; i++) {
                    if (g_server->exports[i].id == req->export_id ||
                        (req->export_id == 0 && i == 0)) {
                        ex = &g_server->exports[i];
                        break;
                    }
                }
                if (!ex) {
                    r.status = EFS_INODE_RPC_NOT_FOUND;
                } else {
                    struct efs_export *tab = table_for_ino(ex, req->parent);
                    uint32_t max = req->max_ents;
                    if (max == 0 || max > EFS_READDIR_MAX)
                        max = EFS_READDIR_MAX;
                    uint32_t start = 0;
                    if (payload_len >= sizeof(*req))
                        start = req->start;
                    uint32_t seen = 0;
                    for (uint64_t i = 0; i < tab->inode_count && r.count < max; i++) {
                        if (tab->inodes[i].ino == 0)
                            continue;
                        if (tab->inodes[i].name[0] == '\0')
                            continue;
                        if (tab->inodes[i].parent != req->parent)
                            continue;
                        if (tab->inodes[i].ino == req->parent)
                            continue;
                        if (seen++ < start)
                            continue;
                        r.ents[r.count++] = tab->inodes[i];
                    }
                    r.status = EFS_INODE_RPC_OK;
                }
                pthread_mutex_unlock(&g_server->lock);
            }
            efs_conn_send_msg(conn, EFS_MSG_INODE_READDIR_REPLY, &r, sizeof(r));
            break;
        }
        case EFS_MSG_INODE_GETCHUNKS: {
            struct efs_msg_inode_getchunks_reply r;
            memset(&r, 0, sizeof(r));
            r.status = EFS_INODE_RPC_ERROR;
            if (payload_len >= sizeof(struct efs_msg_inode_getchunks)) {
                struct efs_msg_inode_getchunks *req = payload;
                pthread_mutex_lock(&g_server->lock);
                struct efs_export *ex = NULL;
                for (uint32_t i = 0; i < g_server->export_count; i++) {
                    if (g_server->exports[i].id == req->export_id ||
                        (req->export_id == 0 && i == 0)) {
                        ex = &g_server->exports[i];
                        break;
                    }
                }
                if (!ex) {
                    r.status = EFS_INODE_RPC_NOT_FOUND;
                } else {
                    struct efs_export *tab = table_for_ino(ex, req->ino);
                    struct efs_inode ino;
                    if (efs_export_get_inode(tab, req->ino, &ino) != 0) {
                        r.status = EFS_INODE_RPC_NOT_FOUND;
                    } else {
                        uint32_t cs = server_data_chunk_size(ex);
                        uint64_t bytes = ino.size;
                        if (ino.pack_len > bytes)
                            bytes = ino.pack_len;
                        uint32_t nci = 0;
                        if (cs && bytes)
                            nci = (uint32_t)((bytes + cs - 1) / cs);
                        uint32_t max = req->max;
                        if (max == 0 || max > EFS_GETCHUNKS_MAX)
                            max = EFS_GETCHUNKS_MAX;
                        /* Indexed only. A size-0 pack container still has
                         * chunks; the client names the window (pack_off).
                         * Never walk ex->chunks[] — that is O(table) under
                         * g_server->lock and stalled the cluster. */
                        uint32_t ci = req->start;
                        uint32_t limit = (nci > 0) ? nci : (req->start + max);
                        for (; ci < limit && r.count < max; ci++) {
                            struct efs_chunk_entry ce;
                            if (efs_export_get_chunk(tab, req->ino, ci,
                                                     &ce) != 0) {
                                if (nci == 0)
                                    break;
                                continue;
                            }
                            r.recs[r.count].ino = req->ino;
                            r.recs[r.count].chunk_index = ci;
                            memcpy(r.recs[r.count].nodes, ce.fragment_nodes,
                                   sizeof(r.recs[r.count].nodes));
                            memcpy(r.recs[r.count].checksums, ce.checksums,
                                   sizeof(r.recs[r.count].checksums));
                            r.count++;
                        }
                        r.status = EFS_INODE_RPC_OK;
                    }
                }
                pthread_mutex_unlock(&g_server->lock);
            }
            efs_conn_send_msg(conn, EFS_MSG_INODE_GETCHUNKS_REPLY, &r, sizeof(r));
            break;
        }
        case EFS_MSG_UPGRADE_META: {
            struct efs_msg_upgrade_meta_reply r;
            memset(&r, 0, sizeof(r));
            r.status = EFS_UPGRADE_ERROR;
            if (payload_len >= sizeof(struct efs_msg_upgrade_meta)) {
                struct efs_msg_upgrade_meta *req = payload;
                pthread_mutex_lock(&g_server->lock);
                struct efs_export *ex = server_find_export(g_server, req->export_name);
                if (!ex && g_server->export_count)
                    ex = &g_server->exports[0];
                if (!ex) {
                    r.status = EFS_UPGRADE_NOT_FOUND;
                } else {
                    uint32_t bits = req->shard_bits;
                    if (bits > 20)
                        bits = 20;
                    if (efs_export_rehash(ex, bits) == EFS_OK) {
                        r.shard_count = ex->root.shard_count;
                        r.status = EFS_UPGRADE_OK;
                        int uidx = server_export_index_locked(g_server, ex);
                        server_meta_mark_rpc_dirty_locked(g_server,
                                                          uidx >= 0 ? uidx
                                                                    : 0);
                    } else {
                        r.status = EFS_UPGRADE_ERROR;
                    }
                }
                pthread_mutex_unlock(&g_server->lock);
            }
            efs_conn_send_msg(conn, EFS_MSG_UPGRADE_META_REPLY, &r, sizeof(r));
            break;
        }
        case EFS_MSG_RDMA_SETUP: {
            /* Arrived over TCP (the QP does not exist yet); the reply goes
             * back over TCP because recv_chan is TCP for this frame. */
            struct efs_msg_rdma_setup_reply rep;
            uint32_t rlen = sizeof(rep);
            efs_rdma_server_accept(conn, payload, payload_len, &rep, &rlen);
            efs_conn_send_msg(conn, EFS_MSG_RDMA_SETUP_REPLY, &rep, rlen);
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
