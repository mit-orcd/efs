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

/* qsort comparator for readdir candidate rows: ascending ino. */
static int readdir_row_cmp(const void *a, const void *b)
{
    const struct efs_inode *ia = *(const struct efs_inode *const *)a;
    const struct efs_inode *ib = *(const struct efs_inode *const *)b;
    if (ia->ino < ib->ino)
        return -1;
    if (ia->ino > ib->ino)
        return 1;
    return 0;
}

/* Collects a directory's child rows for one readdir page.
 *
 * Rows are gathered as pointers under the shard locks (the table cannot move
 * mid-page), then sorted by ino by the caller. The `after` cursor is an ino
 * rather than a position because remove_inode_slot swap-compacts the table:
 * an unlink in an unrelated directory on the same shard shifts this
 * directory's rows across a page boundary, which used to skip entries. */
struct readdir_collect {
    const struct efs_inode **cand;
    uint64_t n;
    uint64_t cap;
    uint64_t after;
    int rc;
};

static int readdir_collect_cb(struct efs_export *ex, uint64_t slot, void *arg)
{
    struct readdir_collect *c = arg;
    const struct efs_inode *in = &ex->inodes[slot];
    if (in->ino <= c->after)
        return 0;
    if (c->n == c->cap) {
        uint64_t ncap = c->cap ? c->cap * 2 : 64;
        const struct efs_inode **nc = realloc(c->cand, ncap * sizeof(*nc));
        if (!nc) {
            c->rc = EFS_ERR_NOMEM;
            return 1; /* stop the walk */
        }
        c->cand = nc;
        c->cap = ncap;
    }
    c->cand[c->n++] = in;
    return 0;
}

#define EFS_FLOCK_SH 1u
#define EFS_FLOCK_EX 2u
#define EFS_FLOCK_NB 4u
#define EFS_FLOCK_UN 8u

/* Open-addressed buckets: rsync create+HOLD walked a single list of
 * every live hold (~10% of server cycles at 14k files). */
#define EFS_HOLD_BUCK 4096u
static struct efs_ino_hold *hold_buck[EFS_HOLD_BUCK];
/* hold_mu guards hold_buck (the unlink-while-open / flock table). A dedicated
 * lock, separate from g_server->lock, so a partitioned shard op can take it
 * briefly. It is a LEAF lock: no shard/global lock is acquired while holding
 * it (lock order global -> shard -> hold_mu). The hold ops are O(1) (hash +
 * refcount), so the hold time is tiny and won't bottleneck like the global
 * lock did. Blocker 3. */
static pthread_mutex_t hold_mu = PTHREAD_MUTEX_INITIALIZER;

static uint32_t hold_hash(efs_export_id_t eid, efs_ino_t ino)
{
    uint64_t x = ((uint64_t)eid << 1) ^ (uint64_t)ino * 0x9e3779b97f4a7c15ull;
    return (uint32_t)(x ^ (x >> 32));
}

/* Caller holds hold_mu. */
static struct efs_ino_hold *hold_find(efs_export_id_t eid, efs_ino_t ino,
                                      int create)
{
    uint32_t b = hold_hash(eid, ino) & (EFS_HOLD_BUCK - 1u);
    struct efs_ino_hold *h;
    for (h = hold_buck[b]; h; h = h->next) {
        if (h->eid == eid && h->ino == ino)
            return h;
    }
    if (!create)
        return NULL;
    h = calloc(1, sizeof(*h));
    if (!h)
        return NULL;
    h->eid = eid;
    h->ino = ino;
    h->next = hold_buck[b];
    hold_buck[b] = h;
    return h;
}

static uint32_t hold_refs(efs_export_id_t eid, efs_ino_t ino)
{
    pthread_mutex_lock(&hold_mu);
    struct efs_ino_hold *h = hold_find(eid, ino, 0);
    uint32_t r = h ? h->refs : 0;
    pthread_mutex_unlock(&hold_mu);
    return r;
}

static void hold_inc(efs_export_id_t eid, efs_ino_t ino)
{
    pthread_mutex_lock(&hold_mu);
    struct efs_ino_hold *h = hold_find(eid, ino, 1);
    if (h)
        h->refs++;
    pthread_mutex_unlock(&hold_mu);
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
    case EFS_MSG_INODE_RENAME_AT:
        return ((const struct efs_msg_inode_rename_at *)payload)->old_parent;
    case EFS_MSG_INODE_LINK:
        return ((const struct efs_msg_inode_link *)payload)->new_parent;
    case EFS_MSG_INODE_LINK_SHARD:
        return ((const struct efs_msg_inode_link_shard *)payload)->src_ino;
    case EFS_MSG_INODE_UNLINK_SHARD:
        return ((const struct efs_msg_inode_unlink_shard *)payload)->src_ino;
    case EFS_MSG_INODE_LOOKUP_PATH:
        return EFS_ROOT_INO;
    case EFS_MSG_INODE_HOLD:
        return ((const struct efs_msg_inode_hold *)payload)->ino;
    case EFS_MSG_INODE_FLOCK:
        return ((const struct efs_msg_inode_flock *)payload)->ino;
    case EFS_MSG_INODE_DROP_CHUNKS:
        return ((const struct efs_msg_inode_drop_chunks *)payload)->ino;
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

/* Caller holds g_server->lock. May drop and reacquire it while rebuilding
 * a hollow extra. Sets r->status = BUSY and returns 1 when the table is
 * not yet safe to mutate (client retries). */
static int reply_if_shard_busy(struct efs_export *ex, efs_ino_t ino,
                               struct efs_msg_inode_reply *r)
{
    uint32_t sh = 0;
    if (ex && ex->root.shard_bits)
        sh = efs_export_shard_of(ino, ex->root.shard_bits);
    if (server_ensure_shard_ready(g_server, ex, sh) != 0) {
        r->status = EFS_INODE_RPC_BUSY;
        return 1;
    }
    return 0;
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

/* Last-link unlink / truncate: drop mappings on every owned table, then
 * fan to the other shard owners. Caller holds s->lock (may drop it). */
static void fan_drop_chunks(struct efsd_server *s, struct efs_export *ex,
                            efs_export_id_t eid, efs_ino_t ino,
                            uint32_t first)
{
    if (!s || !ex || !ino)
        return;
    uint32_t bits = ex->root.shard_bits;
    uint32_t sc = ex->root.shard_count ? ex->root.shard_count : 1;
    if (bits && sc > 1) {
        efs_node_id_t live[EFS_MAX_NODES];
        uint32_t nlive = server_nlive_locked(s, live);
        for (uint32_t sh = 0; sh < sc; sh++) {
            if (efs_shard_owner_of(sh, sc, live, nlive) == s->id)
                (void)server_ensure_shard_ready(s, ex, sh);
        }
    }
    efs_export_drop_chunks_from(ex, ino, first);
    if (!bits || sc <= 1)
        return;
    efs_node_id_t live[EFS_MAX_NODES];
    uint32_t nlive = server_nlive_locked(s, live);
    efs_node_id_t seen[EFS_MAX_NODES];
    uint32_t nseen = 0;
    seen[nseen++] = s->id;
    struct efs_msg_inode_drop_chunks req;
    memset(&req, 0, sizeof(req));
    req.export_id = eid;
    req.ino = ino;
    req.first_chunk = first;
    for (uint32_t sh = 0; sh < sc; sh++) {
        efs_node_id_t own = efs_shard_owner_of(sh, sc, live, nlive);
        if (!own)
            continue;
        int already = 0;
        for (uint32_t i = 0; i < nseen; i++) {
            if (seen[i] == own) {
                already = 1;
                break;
            }
        }
        if (already)
            continue;
        if (nseen < EFS_MAX_NODES)
            seen[nseen++] = own;
        char host[64];
        uint16_t port = 0;
        if (server_node_addr_locked(s, own, host, sizeof(host), &port) != 0)
            continue;
        pthread_mutex_unlock(&s->lock);
        struct efs_msg_inode_reply ur;
        memset(&ur, 0, sizeof(ur));
        (void)server_peer_inode_rpc(host, port, EFS_MSG_INODE_DROP_CHUNKS,
                                    &req, sizeof(req),
                                    EFS_MSG_INODE_DROP_CHUNKS_REPLY, &ur);
        pthread_mutex_lock(&s->lock);
    }
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
        if (pf[1].revents & POLLIN) {
            /* efd is level-triggered leftovers after a consumed RDMA
             * frame or a send-side wake. Only take RDMA when a recv is
             * actually queued — otherwise recv_wait(-1) blocks forever
             * and treats a later TCP request as peer death. */
            int r2 = efs_rdma_reply_ready_quick(rc);
            if (r2 < 0)
                return -1;
            if (r2 > 0)
                return EFS_CONN_RDMA;
            uint64_t tmp;
            if (read(efs_rdma_reply_fd(rc), &tmp, sizeof(tmp)) < 0 &&
                errno != EAGAIN)
                return -1;
            continue;
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

        if (efs_lock_prof_on && type < 256)
            __atomic_add_fetch(&efs_rpc_count[type], 1, __ATOMIC_RELAXED);
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
                    /* A 1-inode table plus a 71 MB page root is a hollow
                     * load (.efsm adopt / missed rebuild). Serve root-only
                     * so the client assembles pages instead of adopting
                     * the empty RAM table. */
                    int hollow = ex->meta_fragmented &&
                                 ex->root.page_count > 0 &&
                                 (ex->meta_needs_rebuild ||
                                  ex->inode_count <= 1);
                    int do_tab = !hollow &&
                                 ((ex->inode_count > 0 &&
                                   !ex->meta_needs_rebuild) ||
                                  !ex->meta_fragmented);
                    /* Always send the EFSR when the export is sharded:
                     * a fresh bits>0 table can be 1 inode / no pages, and
                     * a bare EFSM left clients at shard_bits=0 — they then
                     * REPORT only to the primary, which drops extra-shard
                     * recs (peer reads size 0). */
                    int do_root = ex->root.shard_bits ||
                                  (ex->meta_fragmented &&
                                   ex->root.page_count > 0);
                    char *rbuf = NULL;
                    size_t rlen = 0;
                    if (do_root)
                        efs_export_root_serialize(&ex->root, &rbuf, &rlen);
                    /* Shard locks for the snapshot, global->shard order: the
                     * per-op CREATE path reallocs ex->inodes[] under a shard
                     * lock alone, so the global lock no longer makes this
                     * memcpy safe. Same fix as the flush snapshot. */
                    int gm_eidx = server_export_index_locked(g_server, ex);
                    uint32_t gm_sc = ex->root.shard_count
                                         ? ex->root.shard_count : 1;
                    if (do_tab && gm_eidx >= 0)
                        server_shard_lock_all(g_server, (uint32_t)gm_eidx,
                                              gm_sc);
                    if (do_tab)
                        efs_export_ensure_rollups(ex);
                    int src = do_tab ? efs_export_table_snapshot(ex, &snap)
                                     : EFS_OK;
                    if (do_tab && gm_eidx >= 0)
                        server_shard_unlock_all(g_server, (uint32_t)gm_eidx,
                                                gm_sc);
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
            if (ex && (ex->meta_fragmented || ex->root.shard_bits))
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
                        } else {
                            /* 2PC phase 1 (prepare): a gen-advancing root is
                             * STASHED as pending — never installed, persisted,
                             * fenced, or GC'd here. The writer may still fail
                             * its prepare quorum and discard this root; a
                             * same-gen retry then rewrites the SAME CoW cis
                             * with different content, so installing an
                             * uncommitted root corrupts the pages it
                             * references (the shard-0 "gen raced with GC"
                             * wedge: every node adopted gen=N+1 and GC'd
                             * gen=N's pages, but gen=N+1's pages were
                             * overwritten by the retry). Promotion happens
                             * only on EFS_MSG_META_COMMIT, after the writer's
                             * quorum — by which point the gen's pages are
                             * 2-of-3 placed. A newer prepare supersedes any
                             * pending root (same-gen retry writes fresh cis;
                             * the old pending's pages are unreferenced and
                             * simply overwritten). */
                            /* Stash the raw prepare bytes so COMMIT can
                             * fingerprint-match (a same-gen retry with new
                             * content at the same cis must never promote a
                             * superseded prepare). No blob, no prepare — the
                             * writer's quorum must know. */
                            uint8_t *pb = ei >= 0 ? malloc(payload_len) : NULL;
                            if (ei >= 0 && pb) {
                                efs_export_root_free(&g_server->pending_root[ei]);
                                memset(&g_server->pending_root[ei], 0,
                                       sizeof(g_server->pending_root[ei]));
                                free(g_server->pending_blob[ei]);
                                memcpy(pb, payload, payload_len);
                                g_server->pending_blob[ei] = pb;
                                g_server->pending_blob_len[ei] =
                                    (uint32_t)payload_len;
                                {
                                    struct sockaddr_in pa;
                                    socklen_t pl = sizeof(pa);
                                    char ip[64] = "?";
                                    if (getpeername(conn->fd,
                                                    (struct sockaddr *)&pa,
                                                    &pl) == 0)
                                        inet_ntop(AF_INET, &pa.sin_addr, ip,
                                                  sizeof(ip));
                                    fprintf(stderr,
                                            "meta-prepare: stashed export=%s "
                                            "gen=%llu page0_ci=%u (was committed "
                                            "gen=%llu) from=%s:%u\n",
                                            ex->name,
                                            (unsigned long long)root.generation,
                                            root.page_cis && root.page_count > 0
                                                ? root.page_cis[0]
                                                : 0,
                                            (unsigned long long)ex->root.generation,
                                            ip, ntohs(pa.sin_port));
                                }
                                g_server->pending_root[ei] = root; /* move */
                                g_server->pending_valid[ei] = 1;
                                reply = EFS_PUT_META_OK;
                                pthread_mutex_unlock(&g_server->lock);
                                /* root ownership moved to pending — not freed */
                            } else {
                                free(pb);
                                pthread_mutex_unlock(&g_server->lock);
                                efs_export_root_free(&root);
                                reply = EFS_PUT_META_ERROR;
                            }
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
        case EFS_MSG_META_COMMIT: {
            /* 2PC phase 2: promote a pending (prepared) root to committed.
             * Only now — with the writer's prepare quorum behind it and the
             * gen's pages 2-of-3 placed — is it safe to install, persist,
             * fence stale tables, and GC the previous gen's cis. */
            uint8_t reply = EFS_PUT_META_STALE;
            if (payload_len >= sizeof(struct efs_msg_meta_commit)) {
                struct efs_msg_meta_commit c;
                memcpy(&c, payload, sizeof(c));
                uint32_t *old_cis = NULL, *new_cis = NULL;
                uint32_t old_cis_count = 0, new_cis_count = 0;
                efs_ino_t gc_table_ino = 0;
                struct efs_export *gex = NULL;
                pthread_mutex_lock(&g_server->lock);
                struct efs_export *ex = server_get_export(g_server,
                                                          c.export_id);
                int ei = ex ? server_export_index_locked(g_server, ex) : -1;
                /* Fingerprint match: the pending root must be exactly the
                 * prepare the writer committed. A same-gen retry rewrites the
                 * same cis with new content; promoting a superseded prepare
                 * would reference overwritten pages (checksum mismatch →
                 * permanent rebuild wedge). Mismatch → keep pending, reply
                 * STALE, converge via catchup. */
                int fp_match = 0;
                if (ex && ei >= 0 && g_server->pending_valid[ei] &&
                    g_server->pending_root[ei].generation == c.gen &&
                    g_server->pending_blob[ei]) {
                    uint8_t sum[EFS_HASH_SIZE];
                    efs_hash(g_server->pending_blob[ei],
                             g_server->pending_blob_len[ei], sum);
                    fp_match = (memcmp(sum, c.root_sum, EFS_HASH_SIZE) == 0);
                }
                if (ex && ei >= 0 && fp_match) {
                    if (ex->shard_dirty) {
                        /* We hold unflushed MAIN-table ops for this export —
                         * we are (or believe we are) the cluster-root writer.
                         * Installing a foreign root now would fence and drop
                         * them. Keep the pending root; catchup converges once
                         * our flush advances past this gen. Dirty SHARD tabs
                         * (rpc_dirty_ops) are not fenced by this install, so
                         * they must not block it. */
                        reply = EFS_PUT_META_BUSY;
                        pthread_mutex_unlock(&g_server->lock);
                    } else {
                        struct efs_export_root *pend =
                            &g_server->pending_root[ei];
                        int same_pages =
                            ex->meta_fragmented &&
                            efs_export_root_same_pages(pend, &ex->root);
                        uint64_t old_gen = ex->root.generation;
                        int had_frag = ex->meta_fragmented;
                        uint32_t prev_features = ex->root.features;
                        /* Snapshot outgoing + incoming page_cis for the
                         * lock-free GC below. */
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
                        if (pend->page_cis && pend->page_count > 0) {
                            new_cis_count = pend->page_count;
                            new_cis = malloc((size_t)new_cis_count *
                                             sizeof(uint32_t));
                            if (new_cis)
                                memcpy(new_cis, pend->page_cis,
                                       (size_t)new_cis_count *
                                           sizeof(uint32_t));
                            else
                                new_cis_count = 0;
                        }
                        ex->meta_fragmented = 1;
                        /* Never let a commit orphan a shard: carry forward
                         * extra-shard descriptors the incoming root lacks
                         * (per-shard higher gen wins). */
                        (void)efs_export_root_maxmerge_extras(pend,
                                                              &ex->root);
                        efs_export_root_move(&ex->root, pend);
                        memset(pend, 0, sizeof(*pend));
                        g_server->pending_valid[ei] = 0;
                        free(g_server->pending_blob[ei]);
                        g_server->pending_blob[ei] = NULL;
                        g_server->pending_blob_len[ei] = 0;
                        ex->id = ex->root.id;
                        strncpy(ex->name, ex->root.name, EFS_MAX_NAME - 1);
                        ex->next_ino = ex->root.next_ino;
                        if (efs_chunk_size_valid(ex->root.chunk_size))
                            ex->chunk_size = ex->root.chunk_size;
                        /* Features are server-owned: ignore the incoming
                         * root.features so a flush can't revert an admin
                         * toggle. A fresh export adopts the incoming value;
                         * thereafter only SET_FEATURES changes it. */
                        if (had_frag)
                            ex->root.features = prev_features;
                        else if (ex->root.features == 0)
                            ex->root.features = EFS_FEATURES_DEFAULT;
                        ex->features = ex->root.features;
                        if (!same_pages && ex->root.page_count > 0) {
                            /* Never rebuild on this handler thread: peer page
                             * fetches would block the pooled client
                             * connection. Fence the now-stale tables so
                             * migrate/stats cannot act on old-gen maps. */
                            ex->meta_needs_rebuild = 1;
                            ex->chunk_count = 0;
                            ex->inode_count = 0;
                        }
                        efs_export_merge_extra_roots(ex, &ex->root);
                        g_server->epoch++;
                        g_server->export_meta_dirty = 1;
                        free(ex->gm_blob);
                        ex->gm_blob = NULL;
                        /* Save under the lock: a concurrent PUT_META can
                         * root_move and free page_checksums under a raced
                         * unlocked save (SIGSEGV). */
                        server_save_export(g_server, ex);
                        g_server->export_meta_dirty = 0;
                        gc_table_ino = EFS_META_TABLE_INO;
                        gex = ex;
                        uint64_t new_gen = ex->root.generation;
                        uint32_t commit_pc0 =
                            (ex->root.page_cis && ex->root.page_count > 0)
                                ? ex->root.page_cis[0]
                                : 0;
                        pthread_mutex_unlock(&g_server->lock);
                        fprintf(stderr,
                                "meta-commit: promoted export=%s old_gen=%llu "
                                "new_gen=%llu page0_ci=%u same_pages=%d\n",
                                ex->name, (unsigned long long)old_gen,
                                (unsigned long long)new_gen, commit_pc0,
                                same_pages);
                        /* Reclaim the retired gen's pages. Safe here: this
                         * gen's pages were 2-of-3 placed before the writer's
                         * prepare, and the committed root now references
                         * exactly new_cis. */
                        if (old_cis && new_cis)
                            server_gc_meta_cow_pages(g_server, gex,
                                                     gc_table_ino,
                                                     old_cis, old_cis_count,
                                                     new_cis, new_cis_count);
                        reply = EFS_PUT_META_OK;
                    }
                } else {
                    pthread_mutex_unlock(&g_server->lock);
                }
                free(old_cis);
                free(new_cis);
            }
            efs_conn_send_msg(conn, EFS_MSG_META_COMMIT_REPLY, &reply, 1);
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
                            int cidx = server_export_index_locked(g_server, ex);
                            server_meta_mark_rpc_dirty_locked(
                                g_server, cidx >= 0 ? (uint32_t)cidx : 0);
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
        case EFS_MSG_INODE_RENAME_AT:
        case EFS_MSG_INODE_SETATTR:
        case EFS_MSG_INODE_APPEND:
        case EFS_MSG_INODE_LINK:
        case EFS_MSG_INODE_LINK_SHARD:
        case EFS_MSG_INODE_UNLINK_SHARD:
        case EFS_MSG_INODE_HOLD:
        case EFS_MSG_INODE_FLOCK:
        case EFS_MSG_INODE_DROP_CHUNKS: {
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
            else if (type == EFS_MSG_INODE_RENAME_AT &&
                     payload_len >= sizeof(struct efs_msg_inode_rename_at))
                eid = ((struct efs_msg_inode_rename_at *)payload)->export_id;
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
            else if (type == EFS_MSG_INODE_HOLD &&
                     payload_len >= sizeof(struct efs_msg_inode_hold))
                eid = ((struct efs_msg_inode_hold *)payload)->export_id;
            else if (type == EFS_MSG_INODE_FLOCK &&
                     payload_len >= sizeof(struct efs_msg_inode_flock))
                eid = ((struct efs_msg_inode_flock *)payload)->export_id;
            else if (type == EFS_MSG_INODE_DROP_CHUNKS &&
                     payload_len >= sizeof(struct efs_msg_inode_drop_chunks))
                eid = ((struct efs_msg_inode_drop_chunks *)payload)->export_id;
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
                       type != EFS_MSG_INODE_DROP_CHUNKS &&
                       !server_owns_req_locked(g_server, ex, type, payload)) {
                r.status = EFS_INODE_RPC_NOT_PRIMARY;
                r.primary_id = server_shard_owner_id_locked(g_server, ex, type,
                                                            payload);
            } else if (type == EFS_MSG_INODE_LOOKUP) {
                struct efs_msg_inode_lookup *req = payload;
                /* Transitional: all shard locks for the table read (global
                 * lock still held -> no parallelism yet). */
                uint32_t sc = ex->root.shard_count ? ex->root.shard_count : 1;
                server_shard_lock_all(g_server, eidx, sc);
                if (efs_export_lookup(ex, req->parent, req->name, &r.inode) == 0)
                    r.status = EFS_INODE_RPC_OK;
                else
                    r.status = EFS_INODE_RPC_NOT_FOUND;
                server_shard_unlock_all(g_server, eidx, sc);
            } else if (type == EFS_MSG_INODE_GETATTR) {
                struct efs_msg_inode_getattr *req = payload;
                if (!server_owns_req_locked(g_server, ex, type, payload)) {
                    r.status = EFS_INODE_RPC_NOT_PRIMARY;
                    r.primary_id = server_shard_owner_id_locked(g_server, ex,
                                                                type, payload);
                } else if (reply_if_shard_busy(ex, req->ino, &r)) {
                    /* hollow extra: client retries */
                } else {
                    /* Transitional: hold the shard lock for the table read.
                     * The ensure above already ran (it may drop the global
                     * lock for a rebuild) BEFORE we take the shard lock, so
                     * we never hold a shard lock across a rebuild's
                     * lock_all. Global lock is still held -> no parallelism
                     * yet; this establishes the discipline. */
                    uint32_t gsh = efs_export_shard_of(req->ino,
                                                       ex->root.shard_bits);
                    server_shard_lock(g_server, eidx, gsh);
                    struct efs_export *tab = table_for_ino(ex, req->ino);
                    if (efs_export_get_inode(tab, req->ino, &r.inode) == 0)
                        r.status = EFS_INODE_RPC_OK;
                    else
                        r.status = EFS_INODE_RPC_NOT_FOUND;
                    server_shard_unlock(g_server, eidx, gsh);
                }
            } else if (type == EFS_MSG_INODE_CREATE) {
                /* Phase 3b Independent: shard-local create. Spread dirs
                 * place the dentry on hash(parent,name); inode still RR. */
                struct efs_msg_inode_create *req = payload;
                uint32_t cflags = 0;
                if (payload_len >= sizeof(*req))
                    cflags = req->flags;
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
                    /* Transitional: all shard locks; released around each
                     * fan-out global-lock drop below. */
                    server_shard_lock_all(g_server, eidx, sc);
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
                        creq.flags = cflags;
                        /* ino=0: the TARGET owner allocates from its own
                         * authoritative shard table. Allocating here from
                         * our local copy of that shard is wrong — a
                         * non-owner's shard table is stale or hollow
                         * (evicted/never-rebuilt), so its next_ino hands
                         * out inos the owner already used → CREATE_SHARD
                         * fails EXIST on the ino collision and parallel
                         * creates spuriously fail with EEXIST. */
                        creq.ino = 0;
                        if (server_node_addr_locked(g_server, owner, host,
                                                    sizeof(host), &port) != 0) {
                            r.status = EFS_INODE_RPC_ERROR;
                        } else {
                            if (!efs_mode_is_dir(req->mode) && sc > 1)
                                (void)__atomic_add_fetch(&ex->create_rr, 1,
                                                         __ATOMIC_RELAXED);
                            server_shard_unlock_all(g_server, eidx, sc);
                            pthread_mutex_unlock(&g_server->lock);
                            int nrc = server_peer_create_shard(host, port,
                                                               &creq, &cr);
                            pthread_mutex_lock(&g_server->lock);
                            server_shard_lock_all(g_server, eidx, sc);
                            if (nrc != 0) {
                                r.status = EFS_INODE_RPC_ERROR;
                            } else if (cr.status != EFS_INODE_RPC_OK) {
                                r.status = cr.status;
                                r.primary_id = cr.primary_id;
                            } else {
                                uint32_t psh = efs_export_shard_of(req->parent,
                                                                   bits);
                                uint32_t dsh = psh;
                                if (efs_export_dir_is_spread(ex, req->parent))
                                    dsh = efs_export_dentry_shard_of(
                                        req->parent, req->name, bits);
                                efs_node_id_t down =
                                    efs_shard_owner_of(dsh, sc, live, nlive);
                                if (dsh == target) {
                                    r.inode = cr.inode;
                                    r.status = EFS_INODE_RPC_OK;
                                    server_meta_mark_rpc_dirty_locked(g_server,
                                                                      eidx);
                                } else if (down == 0 || down == g_server->id) {
                                    struct efs_export *dtab =
                                        efs_export_table(ex, dsh);
                                    if (!dtab || !efs_export_create_with_ino(
                                            dtab, cr.inode.ino, req->parent,
                                            req->mode, (uid_t)req->uid,
                                            (gid_t)req->gid, req->name)) {
                                        int name_dup = dtab &&
                                            efs_export_lookup(dtab, req->parent,
                                                              req->name,
                                                              NULL) == EFS_OK;
                                        struct efs_inode tmpi;
                                        memset(&tmpi, 0, sizeof(tmpi));
                                        int ino_dup = dtab && cr.inode.ino &&
                                            efs_export_get_inode(dtab,
                                                                 cr.inode.ino,
                                                                 &tmpi) == 0;
                                        fprintf(stderr,
                                                "create-exist: dentry-write "
                                                "parent=%llu name=%s dsh=%u "
                                                "ino=%llu name_dup=%d "
                                                "ino_dup=%d dtab_inodes=%llu "
                                                "exist(parent=%llu name=%s "
                                                "nlink=%u mode=%o)\n",
                                                (unsigned long long)req->parent,
                                                req->name, dsh,
                                                (unsigned long long)cr.inode.ino,
                                                name_dup, ino_dup,
                                                dtab ? (unsigned long long)
                                                    dtab->inode_count : 0,
                                                (unsigned long long)tmpi.parent,
                                                tmpi.name, tmpi.nlink,
                                                tmpi.mode);
                                        r.status = EFS_INODE_RPC_EXIST;
                                    } else {
                                        dtab->shard_dirty = 1;
                                        r.inode = cr.inode;
                                        r.status = EFS_INODE_RPC_OK;
                                        server_meta_mark_rpc_dirty_locked(
                                            g_server, eidx);
                                    }
                                } else {
                                    struct efs_msg_inode_create_shard dreq = creq;
                                    struct efs_msg_inode_reply dr;
                                    char dhost[64];
                                    uint16_t dport = 0;
                                    memset(&dr, 0, sizeof(dr));
                                    memset(dhost, 0, sizeof(dhost));
                                    dreq.target_shard = dsh;
                                    dreq.ino = cr.inode.ino;
                                    if (server_node_addr_locked(g_server, down,
                                                                dhost,
                                                                sizeof(dhost),
                                                                &dport) != 0) {
                                        r.status = EFS_INODE_RPC_ERROR;
                                    } else {
                                        server_shard_unlock_all(g_server,
                                                                eidx, sc);
                                        pthread_mutex_unlock(&g_server->lock);
                                        int drc = server_peer_create_shard(
                                            dhost, dport, &dreq, &dr);
                                        pthread_mutex_lock(&g_server->lock);
                                        server_shard_lock_all(g_server, eidx,
                                                              sc);
                                        if (drc != 0 ||
                                            dr.status != EFS_INODE_RPC_OK) {
                                            r.status = EFS_INODE_RPC_ERROR;
                                        } else {
                                            r.inode = cr.inode;
                                            r.status = EFS_INODE_RPC_OK;
                                            server_meta_mark_rpc_dirty_locked(
                                                g_server, eidx);
                                        }
                                    }
                                }
                            }
                        }
                    }
                    server_shard_unlock_all(g_server, eidx, sc);
                } else if (bits && sc > 1 && target != 0 &&
                           server_ensure_shard_ready(g_server, ex, target) != 0) {
                    r.status = EFS_INODE_RPC_BUSY;
                } else {
                    /* Transitional: all shard locks (the ensure in the
                     * else-if above ran before we take them). */
                    server_shard_lock_all(g_server, eidx, sc);
                    efs_ino_t ino = efs_export_create(ex, req->parent, req->mode,
                                                      (uid_t)req->uid,
                                                      (gid_t)req->gid, req->name);
                    if (ino) {
                        efs_export_get_inode(ex, ino, &r.inode);
                        r.status = EFS_INODE_RPC_OK;
                        if (cflags & EFS_CREATE_F_HOLD)
                            hold_inc(ex->id, ino);
                        ex->shard_dirty = 1;
                        server_meta_mark_rpc_dirty_locked(g_server, eidx);
                        efs_export_evict_cold_shards(ex, EFS_SHARD_LRU_KEEP);
                    } else {
                        r.status = EFS_INODE_RPC_EXIST;
                    }
                    server_shard_unlock_all(g_server, eidx, sc);
                }
            } else if (type == EFS_MSG_INODE_CREATE_SHARD) {
                struct efs_msg_inode_create_shard *req = payload;
                uint32_t cflags = 0;
                if (payload_len >= sizeof(*req))
                    cflags = req->flags;
                uint32_t sc = ex->root.shard_count ? ex->root.shard_count : 1;
                if (req->target_shard >= sc) {
                    r.status = EFS_INODE_RPC_INVAL;
                } else if (server_ensure_shard_ready(g_server, ex,
                                                    req->target_shard) != 0) {
                    r.status = EFS_INODE_RPC_BUSY;
                } else {
                    /* Transitional: shard lock for the create. The ensure
                     * above ran before we take the shard lock. */
                    server_shard_lock(g_server, eidx, req->target_shard);
                    struct efs_export *ctab =
                        efs_export_table(ex, req->target_shard);
                    if (!ctab || !efs_export_fits_page_cap(ctab, 1, 0)) {
                        r.status = EFS_INODE_RPC_QUOTA;
                    } else {
                        efs_ino_t given = 0;
                        if (payload_len >= sizeof(*req))
                            given = req->ino;
                        efs_ino_t ino = given ? given
                            : efs_export_alloc_ino(ctab, req->target_shard);
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
                            if (cflags & EFS_CREATE_F_HOLD)
                                hold_inc(ex->id, ino);
                            server_meta_mark_rpc_dirty_locked(g_server, eidx);
                        }
                    }
                    server_shard_unlock(g_server, eidx, req->target_shard);
                }
            } else if (type == EFS_MSG_INODE_UNLINK) {
                struct efs_msg_inode_unlink *req = payload;
                /* Transitional: all shard locks; released around the fan
                 * global-lock drop and fan_drop_chunks below. */
                uint32_t sc = ex->root.shard_count ? ex->root.shard_count : 1;
                server_shard_lock_all(g_server, eidx, sc);
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
                    int keep = 0;
                    if (have_victim && !req->is_dir)
                        keep = hold_refs(ex->id, victim.ino) > 0;
                    int urc = efs_export_unlink_name_ex(ex, req->parent,
                                                        req->name, keep);
                    r.status = (urc == 0) ? EFS_INODE_RPC_OK
                                          : EFS_INODE_RPC_NOT_FOUND;
                    if (urc == 0)
                        server_meta_mark_rpc_dirty_locked(g_server, eidx);
                    /* Child row + chunks live on the child owner. After
                     * the parent dentry is gone, fan nlink-- / last-link
                     * drop there so spread-created files don't leak. */
                    if (urc == 0 && have_victim && !req->is_dir) {
                        /* Link count left on the victim after this unlink. -1
                         * until we learn it; the child owner reports it when
                         * the row lives on another shard. */
                        int nlink_left = -1;
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
                                    (void)efs_export_nlink_dec_ex(
                                        ex, victim.ino, &r.inode, keep);
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
                                    server_shard_unlock_all(g_server, eidx,
                                                            sc);
                                    pthread_mutex_unlock(&g_server->lock);
                                    (void)server_peer_inode_rpc(
                                        host, port,
                                        EFS_MSG_INODE_UNLINK_SHARD, &ureq,
                                        sizeof(ureq),
                                        EFS_MSG_INODE_UNLINK_SHARD_REPLY,
                                        &ur);
                                    pthread_mutex_lock(&g_server->lock);
                                    server_shard_lock_all(g_server, eidx, sc);
                                    if (ur.status == EFS_INODE_RPC_OK)
                                        nlink_left = (int)ur.inode.nlink;
                                }
                            }
                        }
                        /* Free the data only once the LAST link is gone.
                         *
                         * This used to drop on every file unlink, so
                         * `ln a b; rm a` released the chunks while b still
                         * referenced them: b kept its size and read back as
                         * zeros. Nothing in-session noticed, because the
                         * client still had the data cached -- it only
                         * appeared after a remount. The UNLINK_SHARD path
                         * below always gated on nlink reaching 0.
                         *
                         * victim.nlink is the count from BEFORE the unlink,
                         * and on a parent shard it can be a dentry stub whose
                         * nlink is not authoritative, so re-read the row
                         * rather than subtracting from it. */
                        if (nlink_left < 0) {
                            struct efs_inode after;
                            memset(&after, 0, sizeof(after));
                            nlink_left =
                                (efs_export_get_inode(ex, victim.ino,
                                                      &after) == 0)
                                    ? (int)after.nlink : 0;
                        }
                        if (!keep && nlink_left == 0) {
                            /* fan_drop_chunks drops the global lock mid-op. */
                            server_shard_unlock_all(g_server, eidx, sc);
                            fan_drop_chunks(g_server, ex, req->export_id,
                                            victim.ino, 0);
                            server_shard_lock_all(g_server, eidx, sc);
                        }
                    }
                }
                server_shard_unlock_all(g_server, eidx, sc);
            } else if (type == EFS_MSG_INODE_UNLINK_SHARD) {
                struct efs_msg_inode_unlink_shard *req = payload;
                if (reply_if_shard_busy(ex, req->src_ino, &r)) {
                    /* hollow extra */
                } else {
                /* Transitional: all shard locks (taken after the busy-ensure,
                 * which may drop the global lock); released around
                 * fan_drop_chunks. */
                uint32_t sc = ex->root.shard_count ? ex->root.shard_count : 1;
                server_shard_lock_all(g_server, eidx, sc);
                int keep = hold_refs(ex->id, req->src_ino) > 0;
                int urc = efs_export_nlink_dec_ex(ex, req->src_ino, &r.inode,
                                                  keep);
                if (urc == 0) {
                    r.status = EFS_INODE_RPC_OK;
                    server_meta_mark_rpc_dirty_locked(g_server, eidx);
                    if (!keep && r.inode.nlink == 0) {
                        /* fan_drop_chunks drops the global lock mid-op. */
                        server_shard_unlock_all(g_server, eidx, sc);
                        fan_drop_chunks(g_server, ex, req->export_id,
                                        req->src_ino, 0);
                        server_shard_lock_all(g_server, eidx, sc);
                    }
                } else {
                    r.status = (urc == EFS_ERR_NOT_FOUND)
                                   ? EFS_INODE_RPC_NOT_FOUND
                                   : EFS_INODE_RPC_INVAL;
                }
                server_shard_unlock_all(g_server, eidx, sc);
                }
            } else if (type == EFS_MSG_INODE_RENAME_AT) {
                struct efs_msg_inode_rename_at *req = payload;
                /* Transitional: all shard locks for the rename. */
                uint32_t sc = ex->root.shard_count ? ex->root.shard_count : 1;
                server_shard_lock_all(g_server, eidx, sc);
                int rrc = efs_export_rename_at(ex, req->old_parent, req->old_name,
                                               req->new_parent, req->new_name);
                if (rrc == 0) {
                    struct efs_inode row;
                    if (efs_export_lookup(ex, req->new_parent, req->new_name,
                                          &row) == 0)
                        r.inode = row;
                    r.status = EFS_INODE_RPC_OK;
                    ex->shard_dirty = 1;
                    server_meta_mark_rpc_dirty_locked(g_server, eidx);
                } else {
                    r.status = (rrc == EFS_ERR_NOT_FOUND) ? EFS_INODE_RPC_NOT_FOUND
                             : (rrc == EFS_ERR_EXIST) ? EFS_INODE_RPC_EXIST
                             : (rrc == EFS_ERR_NOT_EMPTY) ? EFS_INODE_RPC_NOT_EMPTY
                             : EFS_INODE_RPC_INVAL;
                    uint32_t rsh = efs_export_shard_of(req->old_parent,
                                                       ex->root.shard_bits);
                    struct efs_export *rtab = rsh ? efs_export_table(ex, rsh)
                                                  : ex;
                    struct efs_inode np;
                    int np_ok = (rtab && efs_export_get_inode(rtab,
                                        req->new_parent, &np) == 0);
                    fprintf(stderr,
                            "rename-fail: rrc=%d old_par=%llu old=%s "
                            "new_par=%llu new=%s shard=%u tab=%p np_present=%d "
                            "tab_rebuild=%d tab_inodes=%llu gen=%llu\n",
                            rrc, (unsigned long long)req->old_parent,
                            req->old_name, (unsigned long long)req->new_parent,
                            req->new_name, rsh, (void *)rtab, np_ok,
                            rtab ? rtab->meta_needs_rebuild : -1,
                            rtab ? (unsigned long long)rtab->inode_count : 0,
                            (unsigned long long)ex->root.generation);
                }
                server_shard_unlock_all(g_server, eidx, sc);
            } else if (type == EFS_MSG_INODE_RENAME) {
                /* Phase 3b Conflicting: owner-serialized + dual-apply. */
                struct efs_msg_inode_rename *req = payload;
                /* Transitional: all shard locks for the rename. */
                uint32_t sc = ex->root.shard_count ? ex->root.shard_count : 1;
                server_shard_lock_all(g_server, eidx, sc);
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
                server_shard_unlock_all(g_server, eidx, sc);
            } else if (type == EFS_MSG_INODE_SETATTR) {
                struct efs_msg_inode_setattr *req = payload;
                if (reply_if_shard_busy(ex, req->ino, &r)) {
                    /* hollow extra */
                } else {
                /* Transitional: all shard locks (taken after the
                 * busy-ensure); released around fan_drop_chunks on shrink. */
                uint32_t sc = ex->root.shard_count ? ex->root.shard_count : 1;
                server_shard_lock_all(g_server, eidx, sc);
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
                         * chunks. Extent-sharded groups live on many shards
                         * — fan the drop. */
                        if (req->size < cur.size) {
                            uint32_t cs = server_data_chunk_size(ex);
                            uint32_t first_drop = (req->size == 0) ? 0
                                : (uint32_t)((req->size + cs - 1) / cs);
                            /* fan_drop_chunks drops the global lock mid-op. */
                            server_shard_unlock_all(g_server, eidx, sc);
                            fan_drop_chunks(g_server, ex, req->export_id,
                                            req->ino, first_drop);
                            server_shard_lock_all(g_server, eidx, sc);
                            /* A shrink drops the data a live append rsv
                             * points at. The reserve barrier keys on
                             * rsv_end > size, so a truncated-away rsv would
                             * look permanently outstanding and BUSY the next
                             * append into EIO (peer_append_while_truncate).
                             * Release this ino's stale reservations. */
                            for (uint32_t i = 0; i < EFS_APPEND_RSV_SLOTS;
                                 i++) {
                                if (tab->append_rsv[i].ino == req->ino &&
                                    tab->append_rsv[i].end > req->size) {
                                    tab->append_rsv[i].ino = 0;
                                    tab->append_rsv[i].end = 0;
                                    tab->append_rsv[i].ts_ms = 0;
                                }
                            }
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
                server_shard_unlock_all(g_server, eidx, sc);
                }
            } else if (type == EFS_MSG_INODE_APPEND) {
                struct efs_msg_inode_append *req = payload;
                if (reply_if_shard_busy(ex, req->ino, &r)) {
                    /* hollow extra */
                } else {
                /* Transitional: shard lock for the append reservation (the
                 * ensure above ran before we take the shard lock). */
                uint32_t ash = efs_export_shard_of(req->ino,
                                                   ex->root.shard_bits);
                server_shard_lock(g_server, eidx, ash);
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
                    /* Open-address: a single (ino % 64) slot was overwritten
                     * by any other ino that hashed here. Under 9-way POSIX
                     * that drops the barrier and two appends share an
                     * offset (concurrent_appends lost lines). Probe for
                     * this ino first; never steal another ino's live rsv. */
                    uint32_t start = (uint32_t)(req->ino % EFS_APPEND_RSV_SLOTS);
                    int32_t found = -1, empty = -1;
                    uint64_t rsv_end = 0, rsv_ts = 0;
                    for (uint32_t p = 0; p < EFS_APPEND_RSV_SLOTS; p++) {
                        uint32_t si = (start + p) % EFS_APPEND_RSV_SLOTS;
                        efs_ino_t slo = tab->append_rsv[si].ino;
                        uint64_t ts = tab->append_rsv[si].ts_ms;
                        uint64_t end = tab->append_rsv[si].end;
                        /* Slot is blocking only while the reserved end is
                         * still ahead of that ino's table size. Treating
                         * any 30s-old entry as live filled 64 slots under
                         * 9-way POSIX and forced later appends to fall
                         * back to an unreserved local end (lost lines). */
                        int expired = !ts || now_ms < ts ||
                                      now_ms - ts >= 30000ull;
                        int outstanding = 0;
                        if (slo && end > 0 && !expired) {
                            if (slo == req->ino)
                                outstanding = end > cur.size;
                            else {
                                struct efs_inode oth;
                                if (efs_export_get_inode(tab, slo,
                                                         &oth) != 0)
                                    outstanding = 1;
                                else
                                    outstanding = end > oth.size;
                            }
                        }
                        if (slo == req->ino) {
                            found = (int32_t)si;
                            rsv_end = end;
                            rsv_ts = ts;
                            break;
                        }
                        if (!outstanding && empty < 0)
                            empty = (int32_t)si;
                    }
                    if (found >= 0 && rsv_end > cur.size &&
                        now_ms - rsv_ts < 30000ull) {
                        r.status = EFS_INODE_RPC_BUSY;
                        r.inode = cur;
                    } else if (found < 0 && empty < 0) {
                        r.status = EFS_INODE_RPC_BUSY;
                        r.inode = cur;
                    } else {
                        uint32_t si = (uint32_t)(found >= 0 ? found : empty);
                        uint64_t off = rsv_end > cur.size ? rsv_end : cur.size;
                        tab->append_rsv[si].ino = req->ino;
                        tab->append_rsv[si].end = off + req->len;
                        tab->append_rsv[si].ts_ms = now_ms;
                        r.inode = cur;
                        r.inode.size = off + req->len;
                        r.status = EFS_INODE_RPC_OK;
                    }
                }
                server_shard_unlock(g_server, eidx, ash);
                }
            } else if (type == EFS_MSG_INODE_LINK) {
                struct efs_msg_inode_link *req = payload;
                uint32_t sc = ex->root.shard_count ? ex->root.shard_count : 1;
                uint32_t bits = ex->root.shard_bits;
                /* Transitional: all shard locks; released around the fan
                 * global-lock drop below. */
                server_shard_lock_all(g_server, eidx, sc);
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
                            server_shard_unlock_all(g_server, eidx, sc);
                            pthread_mutex_unlock(&g_server->lock);
                            int nrc = server_peer_inode_rpc(
                                host, port, EFS_MSG_INODE_LINK_SHARD, &lreq,
                                sizeof(lreq), EFS_MSG_INODE_LINK_SHARD_REPLY,
                                &lr);
                            pthread_mutex_lock(&g_server->lock);
                            server_shard_lock_all(g_server, eidx, sc);
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
                server_shard_unlock_all(g_server, eidx, sc);
            } else if (type == EFS_MSG_INODE_LINK_SHARD) {
                struct efs_msg_inode_link_shard *req = payload;
                if (reply_if_shard_busy(ex, req->src_ino, &r)) {
                    /* hollow extra */
                } else {
                /* Transitional: all shard locks (taken after the
                 * busy-ensure). */
                uint32_t sc = ex->root.shard_count ? ex->root.shard_count : 1;
                server_shard_lock_all(g_server, eidx, sc);
                int lrc = efs_export_nlink_inc(ex, req->src_ino, &r.inode);
                if (lrc == 0) {
                    r.status = EFS_INODE_RPC_OK;
                    server_meta_mark_rpc_dirty_locked(g_server, eidx);
                } else {
                    r.status = (lrc == EFS_ERR_NOT_FOUND)
                                   ? EFS_INODE_RPC_NOT_FOUND
                                   : EFS_INODE_RPC_INVAL;
                }
                server_shard_unlock_all(g_server, eidx, sc);
                }
            } else if (type == EFS_MSG_INODE_HOLD) {
                struct efs_msg_inode_hold *req = payload;
                if (payload_len < sizeof(*req) || reply_if_shard_busy(ex, req->ino, &r)) {
                    if (payload_len < sizeof(*req))
                        r.status = EFS_INODE_RPC_INVAL;
                } else {
                    /* HOLD-table mutation under hold_mu (leaf); the refs==0
                     * purge is an export op and runs under the shard/global
                     * lock the handler already holds, NOT under hold_mu. */
                    int do_purge = 0;
                    pthread_mutex_lock(&hold_mu);
                    struct efs_ino_hold *h = hold_find(ex->id, req->ino, 1);
                    if (!h) {
                        pthread_mutex_unlock(&hold_mu);
                        r.status = EFS_INODE_RPC_ERROR;
                    } else if (req->flags) {
                        h->refs++;
                        pthread_mutex_unlock(&hold_mu);
                        r.status = EFS_INODE_RPC_OK;
                    } else {
                        if (h->refs > 0)
                            h->refs--;
                        if (h->refs == 0) {
                            h->flock_n = 0;
                            do_purge = 1;
                        }
                        pthread_mutex_unlock(&hold_mu);
                        if (do_purge) {
                            /* Transitional: shard locks for the export op.
                             * hold_mu is already released; lock order is
                             * global -> shard -> hold_mu. */
                            uint32_t sc = ex->root.shard_count
                                              ? ex->root.shard_count : 1;
                            server_shard_lock_all(g_server, eidx, sc);
                            (void)efs_export_purge_unlinked(ex, req->ino);
                            server_meta_mark_rpc_dirty_locked(g_server, eidx);
                            server_shard_unlock_all(g_server, eidx, sc);
                        }
                        r.status = EFS_INODE_RPC_OK;
                    }
                }
            } else if (type == EFS_MSG_INODE_FLOCK) {
                struct efs_msg_inode_flock *req = payload;
                if (payload_len < sizeof(*req) || reply_if_shard_busy(ex, req->ino, &r)) {
                    if (payload_len < sizeof(*req))
                        r.status = EFS_INODE_RPC_INVAL;
                } else {
                    /* FLOCK state lives entirely in the HOLD table -> the whole
                     * critical section runs under hold_mu (leaf). */
                    uint32_t op = req->op;
                    pthread_mutex_lock(&hold_mu);
                    struct efs_ino_hold *h = hold_find(ex->id, req->ino, 1);
                    if (!h) {
                        r.status = EFS_INODE_RPC_ERROR;
                    } else if (op & EFS_FLOCK_UN) {
                        uint32_t w = 0;
                        for (uint32_t i = 0; i < h->flock_n; i++) {
                            if (h->flock_owner[i] != req->owner) {
                                h->flock_owner[w] = h->flock_owner[i];
                                h->flock_ex[w] = h->flock_ex[i];
                                w++;
                            }
                        }
                        h->flock_n = w;
                        r.status = EFS_INODE_RPC_OK;
                    } else if (op & (EFS_FLOCK_EX | EFS_FLOCK_SH)) {
                        int want_ex = (op & EFS_FLOCK_EX) ? 1 : 0;
                        int conflict = 0, mine = -1;
                        for (uint32_t i = 0; i < h->flock_n; i++) {
                            if (h->flock_owner[i] == req->owner) {
                                mine = (int)i;
                                continue;
                            }
                            if (h->flock_ex[i] || want_ex)
                                conflict = 1;
                        }
                        if (conflict)
                            r.status = EFS_INODE_RPC_BUSY;
                        else if (mine >= 0) {
                            h->flock_ex[mine] = (uint8_t)want_ex;
                            r.status = EFS_INODE_RPC_OK;
                        } else if (h->flock_n >= 8) {
                            r.status = EFS_INODE_RPC_BUSY;
                        } else {
                            h->flock_owner[h->flock_n] = req->owner;
                            h->flock_ex[h->flock_n] = (uint8_t)want_ex;
                            h->flock_n++;
                            r.status = EFS_INODE_RPC_OK;
                        }
                    } else {
                        r.status = EFS_INODE_RPC_INVAL;
                    }
                    pthread_mutex_unlock(&hold_mu);
                }
            } else if (type == EFS_MSG_INODE_DROP_CHUNKS) {
                struct efs_msg_inode_drop_chunks *req = payload;
                if (payload_len < sizeof(*req)) {
                    r.status = EFS_INODE_RPC_INVAL;
                } else {
                    uint32_t bits = ex->root.shard_bits;
                    uint32_t sc = ex->root.shard_count
                                      ? ex->root.shard_count : 1;
                    if (bits && sc > 1) {
                        efs_node_id_t live[EFS_MAX_NODES];
                        uint32_t nlive = server_nlive_locked(g_server, live);
                        for (uint32_t sh = 0; sh < sc; sh++) {
                            if (efs_shard_owner_of(sh, sc, live, nlive) ==
                                g_server->id)
                                (void)server_ensure_shard_ready(g_server, ex,
                                                                sh);
                        }
                    }
                    /* Transitional: all shard locks (the ensure loop above
                     * ran first — it may drop the global lock). */
                    server_shard_lock_all(g_server, eidx, sc);
                    efs_export_drop_chunks_from(ex, req->ino, req->first_chunk);
                    ex->shard_dirty = 1;
                    server_meta_mark_rpc_dirty_locked(g_server, eidx);
                    r.status = EFS_INODE_RPC_OK;
                    server_shard_unlock_all(g_server, eidx, sc);
                }
            }
            pthread_mutex_unlock(&g_server->lock);
            uint8_t rtype = (type == EFS_MSG_INODE_LOOKUP) ? EFS_MSG_INODE_LOOKUP_REPLY
                          : (type == EFS_MSG_INODE_CREATE) ? EFS_MSG_INODE_CREATE_REPLY
                          : (type == EFS_MSG_INODE_CREATE_SHARD) ? EFS_MSG_INODE_CREATE_SHARD_REPLY
                          : (type == EFS_MSG_INODE_GETATTR) ? EFS_MSG_INODE_GETATTR_REPLY
                          : (type == EFS_MSG_INODE_RENAME) ? EFS_MSG_INODE_RENAME_REPLY
                          : (type == EFS_MSG_INODE_RENAME_AT) ? EFS_MSG_INODE_RENAME_AT_REPLY
                          : (type == EFS_MSG_INODE_SETATTR) ? EFS_MSG_INODE_SETATTR_REPLY
                          : (type == EFS_MSG_INODE_APPEND) ? EFS_MSG_INODE_APPEND_REPLY
                          : (type == EFS_MSG_INODE_LINK) ? EFS_MSG_INODE_LINK_REPLY
                          : (type == EFS_MSG_INODE_LINK_SHARD) ? EFS_MSG_INODE_LINK_SHARD_REPLY
                          : (type == EFS_MSG_INODE_UNLINK_SHARD) ? EFS_MSG_INODE_UNLINK_SHARD_REPLY
                          : (type == EFS_MSG_INODE_HOLD) ? EFS_MSG_INODE_HOLD_REPLY
                          : (type == EFS_MSG_INODE_FLOCK) ? EFS_MSG_INODE_FLOCK_REPLY
                          : (type == EFS_MSG_INODE_DROP_CHUNKS) ? EFS_MSG_INODE_DROP_CHUNKS_REPLY
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
            uint32_t eidx = 0;
            efs_export_id_t eid = req->export_id;
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
            } else if (req->path[0] != '/') {
                r.status = EFS_INODE_RPC_INVAL;
            } else if (req->path[1] == '\0') {
                if (efs_export_get_inode(ex, EFS_ROOT_INO, &r.inode) == 0)
                    r.status = EFS_INODE_RPC_OK;
                else
                    r.status = EFS_INODE_RPC_NOT_FOUND;
            } else {
                /* Transitional: all shard locks for the whole walk (no
                 * global-lock drop inside). */
                uint32_t sc = ex->root.shard_count ? ex->root.shard_count : 1;
                server_shard_lock_all(g_server, eidx, sc);
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
                    if (more && (req->flags & EFS_LOOKUP_PATH_F_ANCESTORS) &&
                        r.ancestor_count < EFS_LOOKUP_PATH_MAX_DEPTH) {
                        struct efs_lookup_path_anc *a =
                            &r.ancestors[r.ancestor_count++];
                        a->ino = row.ino;
                        a->mode = row.mode;
                        a->uid = row.uid;
                        a->gid = row.gid;
                    }
                    /* Never abort with DEEP: a 65+ component dest-stat
                     * (rsync of deep_skinny_chain) used to PROTO-fallback
                     * to one LOOKUP RPC per component. Keep walking. */
                    parent = row.ino;
                    r.inode = row;
                    r.status = EFS_INODE_RPC_OK;
                    part = strtok_r(NULL, "/", &save);
                }
                server_shard_unlock_all(g_server, eidx, sc);
            }
            pthread_mutex_unlock(&g_server->lock);
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
                int shard_busy = 0;
                if (bits && sc > 1) {
                    uint8_t seen[EFS_META_MAX_SHARDS];
                    memset(seen, 0, sizeof(seen));
                    for (uint32_t k = 0; k < count && !shard_busy; k++) {
                        uint32_t sh = efs_export_chunk_shard_of(recs[k].ino,
                                                                recs[k].chunk_index,
                                                                bits);
                        if (sh >= EFS_META_MAX_SHARDS || seen[sh])
                            continue;
                        seen[sh] = 1;
                        if (efs_shard_owner_of(sh, sc, live, nlive) !=
                            g_server->id)
                            continue;
                        if (server_ensure_shard_ready(g_server, ex, sh) != 0)
                            shard_busy = 1;
                    }
                    for (uint32_t k = 0; k < ino_count && !shard_busy; k++) {
                        uint32_t sh = efs_export_shard_of(irecs[k].ino, bits);
                        if (sh >= EFS_META_MAX_SHARDS || seen[sh])
                            continue;
                        seen[sh] = 1;
                        if (efs_shard_owner_of(sh, sc, live, nlive) !=
                            g_server->id)
                            continue;
                        if (server_ensure_shard_ready(g_server, ex, sh) != 0)
                            shard_busy = 1;
                    }
                }
                if (shard_busy) {
                    r.status = EFS_INODE_RPC_BUSY;
                } else {
                /* Transitional: all shard locks (the ensure loop above ran
                 * first — it may drop the global lock); released around the
                 * yield below. */
                server_shard_lock_all(g_server, eidx, sc);
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
                        server_shard_unlock_all(g_server, eidx, sc);
                        pthread_mutex_unlock(&g_server->lock);
                        sched_yield();
                        pthread_mutex_lock(&g_server->lock);
                        server_shard_lock_all(g_server, eidx, sc);
                        if (ex->meta_needs_rebuild)
                            break;
                    }
                    if (bits && sc > 1) {
                        uint32_t sh = efs_export_chunk_shard_of(
                            recs[k].ino, recs[k].chunk_index, bits);
                        efs_node_id_t own =
                            efs_shard_owner_of(sh, sc, live, nlive);
                        if (own != g_server->id) {
                            dropped++;
                            if (!drop_owner)
                                drop_owner = own;
                            continue;
                        }
                    }
                    /* Route by chunk group, not inode shard. */
                    if (efs_export_set_chunk(ex, recs[k].ino, recs[k].chunk_index,
                                             recs[k].nodes,
                                             recs[k].checksums) == 0) {
                        applied++;
                        struct efs_export *tab = efs_export_table_for_chunk(
                            ex, recs[k].ino, recs[k].chunk_index);
                        if (tab)
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
                    /* Grow-only: a lagging report must not shrink an
                     * O_APPEND reserve (size advances server-side ahead
                     * of the data report). Shrinks only arrive via
                     * SETATTR. A stale close-REPORT must also not *grow*
                     * over a newer setattr: wr() close kicks REPORT
                     * async, O_TRUNC setattr size=0, then the pre-trunc
                     * report lands and grow-only restores the old size
                     * (posix2 peer_o_trunc_visible: B still saw 10).
                     * SETATTR size stamps mtime=now; reject a grow whose
                     * mtime is older than the row. */
                    int report_stale =
                        irecs[k].mtime < cur.mtime ||
                        (irecs[k].mtime == cur.mtime &&
                         irecs[k].mtime_nsec < cur.mtime_nsec);
                    if (irecs[k].size > cur.size && !report_stale &&
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
                server_shard_unlock_all(g_server, eidx, sc);
                }
            }
            pthread_mutex_unlock(&g_server->lock);
            if (do_flush) {
                if (server_flush_meta_grouped(g_server, ex) != 0) {
                    /* Flush failed (rebuild in flight / no quorum / fenced).
                     * BUSY so the client retries — ERROR used to become
                     * fsync EIO on the first attempt (9-way 007). The
                     * in-memory mutation stays dirty. */
                    if (r.status == EFS_INODE_RPC_OK)
                        r.status = EFS_INODE_RPC_BUSY;
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
                uint32_t eidx = 0;
                for (uint32_t i = 0; i < g_server->export_count; i++) {
                    if (g_server->exports[i].id == req->export_id ||
                        (req->export_id == 0 && i == 0)) {
                        ex = &g_server->exports[i];
                        eidx = i;
                        break;
                    }
                }
                if (!ex) {
                    r.status = EFS_INODE_RPC_NOT_FOUND;
                } else {
                    uint32_t flags = 0, want_shard = 0;
                    if (payload_len >= sizeof(*req)) {
                        flags = req->flags;
                        want_shard = req->shard;
                    }
                    struct efs_export *tab = table_for_ino(ex, req->parent);
                    if (flags & EFS_READDIR_F_LOCAL_ONLY) {
                        if (ex->root.shard_bits)
                            (void)server_ensure_shard_ready(g_server, ex,
                                                            want_shard);
                        tab = efs_export_table(ex, want_shard);
                        if (!tab)
                            tab = ex;
                    }
                    /* Transitional: all shard locks for the read loop (the
                     * ensure above ran first — it may drop the global
                     * lock). */
                    uint32_t sc = ex->root.shard_count
                                      ? ex->root.shard_count : 1;
                    server_shard_lock_all(g_server, eidx, sc);
                    uint32_t max = req->max_ents;
                    if (max == 0 || max > EFS_READDIR_MAX)
                        max = EFS_READDIR_MAX;
                    uint64_t after = 0;
                    if (payload_len >= sizeof(*req))
                        after = req->after_ino;
                    /* Emit this dir's children with ino > after in ascending
                     * ino order. Positional pagination over the live table is
                     * unstable: remove_inode_slot swap-compacts, so a
                     * concurrent unlink of ANOTHER dir on this shard shifts
                     * this dir's rows across a page boundary and a just-
                     * created child is skipped (readdir returned 299/300).
                     * inos survive compaction, so an ino cursor is stable;
                     * the dir's own child set is unchanged during its readdir,
                     * so the sorted order is consistent page to page. Rows are
                     * collected as pointers while holding the lock (the table
                     * cannot move mid-page), then sorted and copied out. */
                    /* Walk the parent's child index, not the table. The scan
                     * this replaces was O(rows in the shard table) per PAGE,
                     * and readdir fans a page to every shard, so listing one
                     * directory cost O(inodes in the export) per page no
                     * matter how few children it had: a 1000-entry directory
                     * took 31 ms at 90k inodes and 1.9 s at 10M.
                     *
                     * efs_export_foreach_child also drops the O(ncand^2)
                     * duplicate-dentry diagnostic that used to run here. It
                     * emits a slot only when the name index maps (parent,
                     * name) back to that same slot, so a duplicate name cannot
                     * reach the reply and the pairwise strncmp had nothing
                     * left to find — it was pure cost, ~500k comparisons per
                     * page on a 1000-entry directory. */
                    struct readdir_collect col = {
                        .after = after, .rc = EFS_OK,
                    };
                    (void)efs_export_foreach_child(tab, req->parent,
                                                   readdir_collect_cb, &col);
                    const struct efs_inode **cand = col.cand;
                    uint64_t ncand = col.n;
                    if (ncand > 1)
                        qsort(cand, ncand, sizeof(*cand), readdir_row_cmp);
                    for (uint64_t k = 0; k < ncand && r.count < max; k++)
                        r.ents[r.count++] = *cand[k];
                    free(cand);
                    r.status = EFS_INODE_RPC_OK;
                    server_shard_unlock_all(g_server, eidx, sc);
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
                uint32_t eidx = 0;
                for (uint32_t i = 0; i < g_server->export_count; i++) {
                    if (g_server->exports[i].id == req->export_id ||
                        (req->export_id == 0 && i == 0)) {
                        ex = &g_server->exports[i];
                        eidx = i;
                        break;
                    }
                }
                if (!ex) {
                    r.status = EFS_INODE_RPC_NOT_FOUND;
                } else {
                    uint32_t bits = ex->root.shard_bits;
                    uint32_t sc = ex->root.shard_count
                                      ? ex->root.shard_count : 1;
                    uint32_t gsh = efs_export_chunk_shard_of(req->ino,
                                                             req->start, bits);
                    if (bits && sc > 1) {
                        efs_node_id_t live[EFS_MAX_NODES];
                        uint32_t nlive = server_nlive_locked(g_server, live);
                        efs_node_id_t own =
                            efs_shard_owner_of(gsh, sc, live, nlive);
                        if (own != g_server->id) {
                            r.status = EFS_INODE_RPC_NOT_PRIMARY;
                            r.primary_id = own;
                        } else if (server_ensure_shard_ready(g_server, ex,
                                                             gsh) != 0) {
                            r.status = EFS_INODE_RPC_BUSY;
                        }
                    }
                    if (r.status != EFS_INODE_RPC_NOT_PRIMARY &&
                        r.status != EFS_INODE_RPC_BUSY) {
                    /* Transitional: shard lock for the chunk-group read (the
                     * ensure above ran before we take the shard lock). */
                    server_shard_lock(g_server, eidx, gsh);
                    /* Serve this group only. The inode row may live on a
                     * different shard — do not require it here. */
                    uint32_t max = req->max;
                    if (max == 0 || max > EFS_GETCHUNKS_MAX)
                        max = EFS_GETCHUNKS_MAX;
                    uint32_t group_end =
                        (req->start | (EFS_CHUNK_GROUP_SIZE - 1u)) + 1u;
                    uint32_t ci = req->start;
                    for (; ci < group_end && r.count < max; ci++) {
                        struct efs_chunk_entry ce;
                        if (efs_export_get_chunk(ex, req->ino, ci, &ce) != 0)
                            continue;
                        r.recs[r.count].ino = req->ino;
                        r.recs[r.count].chunk_index = ci;
                        memcpy(r.recs[r.count].nodes, ce.fragment_nodes,
                               sizeof(r.recs[r.count].nodes));
                        memcpy(r.recs[r.count].checksums, ce.checksums,
                               sizeof(r.recs[r.count].checksums));
                        r.count++;
                    }
                    r.status = EFS_INODE_RPC_OK;
                    server_shard_unlock(g_server, eidx, gsh);
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
